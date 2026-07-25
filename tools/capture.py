"""Serial capture that survives a USB replug.

Unplugging the board destroys /dev/ttyUSB0, which kills any handle held across
it. This reopens the port as it reappears, so a capture can be started before
the replug and still record the boot log that follows it.

    python3 tools/capture.py [seconds-after-replug]

The argument times the capture *from the moment the board comes back*, not from
launch. It used to be a wall-clock budget for the whole run, which quietly made
the operator's thinking time compete with the capture: pause to read the
instruction and the window closes before you have touched the board. Twice that
produced an empty capture that looked exactly like a dead one. The clock now
starts when there is something to capture, so there is nothing to be late for.

Because the wait is open-ended, the script tells you out loud when it wants the
gesture and when it is finished — see ding(). A capture that needs a human and
waits silently for one is a capture that waits forever.

Output streams as it arrives rather than accumulating. That matters more than it
sounds: a capture that buffers to the end is indistinguishable from one that
died on the first read, and both look like a board that printed nothing — which
is the same symptom as the crash such a capture is usually chasing. Piping this
through `tail` reintroduces the problem at the shell instead, so don't.
"""

import subprocess
import sys
import time
from contextlib import suppress

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200
RETRY_DELAY = 0.3

# Seconds of capture once the board is back. The app logs only during its first
# seconds, so this only has to outlast boot.
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

# How long to wait for the human before giving up. Generous rather than
# unbounded: a script left running forever on a port nobody is going to touch
# blocks the next flash, which needs the port to itself.
WAIT_FOR_GESTURE = 600.0

SOUNDS = "/usr/share/sounds/freedesktop/stereo"


def ding(sound: str) -> None:
    """Audible cue that the script wants attention, or is done.

    Best-effort and deliberately never fatal: on a machine with no audio, a
    capture that still captures is worth more than one that dies over a missing
    sound file. The terminal bell goes out regardless, since it costs nothing
    and works over ssh where PulseAudio does not.
    """
    sys.stderr.write("\a")
    sys.stderr.flush()
    with suppress(Exception):
        subprocess.Popen(
            ["paplay", f"{SOUNDS}/{sound}.oga"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


total = 0
ser = None


def emit(data: bytes) -> None:
    """Write straight through to stdout, unbuffered.

    Bytes rather than text: a read can split a UTF-8 sequence across chunks, and
    decoding each chunk on its own would turn a clean log line into replacement
    characters at the boundary. The terminal reassembles them.
    """
    global total
    total += len(data)
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def open_port():
    try:
        port = serial.Serial()
        port.port = PORT
        port.baudrate = BAUD
        port.timeout = RETRY_DELAY
        # Leave the handshake lines alone: this board does not wire them to
        # EN/GPIO0, and driving them has no useful effect here.
        port.dtr = False
        port.rts = False
        port.open()
        return port
    except Exception:
        # Covers the device being absent as well as failing to open, so the
        # missing-path case needs no separate check.
        return None


# Phase 1: wait for the unplug. The port may be present or already gone; either
# is a fine starting state, so this does not insist on opening it first.
emit(b"\n--- waiting for the replug (unplug the board when ready) ---\n")
ding("message")

gesture_deadline = time.time() + WAIT_FOR_GESTURE
ser = open_port()
while time.time() < gesture_deadline:
    if ser is None:
        break  # Already unplugged.
    try:
        chunk = ser.read(512)
        if chunk:
            emit(chunk)
    except Exception:
        with suppress(Exception):
            ser.close()
        ser = None
        break
else:
    emit(b"\n--- gave up waiting for the unplug ---\n")
    ding("dialog-warning")
    sys.exit(1)

emit(b"\n--- board unplugged, waiting for it to come back ---\n")

# Phase 2: wait for the board to reappear, then capture for DURATION from that
# moment. This is the whole point of the rewrite — the clock starts here.
while time.time() < gesture_deadline:
    ser = open_port()
    if ser is not None:
        break
    time.sleep(RETRY_DELAY)

if ser is None:
    emit(b"\n--- board never came back ---\n")
    ding("dialog-warning")
    sys.exit(1)

emit(b"\n--- port opened, capturing ---\n")
deadline = time.time() + DURATION

while time.time() < deadline:
    if ser is None:
        ser = open_port()
        if ser is None:
            time.sleep(RETRY_DELAY)
            continue
        emit(b"\n--- port reopened ---\n")
    try:
        chunk = ser.read(512)
        if chunk:
            emit(chunk)
    except Exception as exc:
        emit(f"\n--- port lost: {exc} ---\n".encode())
        with suppress(Exception):
            ser.close()
        ser = None

if ser is not None:
    with suppress(Exception):
        ser.close()

# Trails the log rather than leading it, since the log is now already out.
sys.stdout.write(f"\n--- bytes captured: {total} ---\n")
ding("complete")
