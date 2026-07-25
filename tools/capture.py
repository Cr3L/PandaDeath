"""Serial capture that survives a USB replug.

Unplugging the board destroys /dev/ttyUSB0, which kills any handle held across
it. This reopens the port as it reappears, so a capture can be started before
the replug and still record the boot log that follows it.

Output streams as it arrives rather than accumulating. That matters more than it
sounds: a capture that buffers to the end is indistinguishable from one that
died on the first read, and both look like a board that printed nothing — which
is the same symptom as the crash such a capture is usually chasing. Piping this
through `tail` reintroduces the problem at the shell instead, so don't.
"""

import sys
import time
from contextlib import suppress

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200
RETRY_DELAY = 0.3
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

deadline = time.time() + DURATION
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

while time.time() < deadline:
    if ser is None:
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
        except Exception:
            # Covers the device being absent as well as failing to open, so the
            # missing-path case needs no separate check.
            time.sleep(RETRY_DELAY)
            continue
        # Published only once fully open, so "ser is not None" always means a
        # readable port rather than a half-built one.
        ser = port
        emit(b"\n--- port opened ---\n")

    try:
        chunk = ser.read(512)
        if chunk:
            emit(chunk)
    except Exception as exc:
        emit(f"\n--- port lost: {exc} ---\n".encode())
        with suppress(Exception):
            ser.close()
        ser = None
        time.sleep(RETRY_DELAY)

if ser is not None:
    with suppress(Exception):
        ser.close()

# Trails the log rather than leading it, since the log is now already out.
sys.stdout.write(f"\n--- bytes captured: {total} ---\n")
