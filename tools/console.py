"""Drive the board's serial console non-interactively.

    python3 tools/console.py wifi_show
    python3 tools/console.py "wifi_set TestNetwork hunter2" wifi_show

Each argument is one command line, sent in order; everything the board prints
back is echoed here. Exists because `idf.py monitor` needs a real terminal, so
an agent cannot use it — but the console is just line-oriented text over the
same UART that capture.py already reads, and that needs no terminal at all.
Before this, every console check cost a human typing at a prompt.

Uses pyserial, which lives only in the IDF virtualenv. Either source export.sh
first or call that interpreter directly:

    ~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/console.py wifi_show

Does not replace capture.py: this assumes the board is already up and the port
already exists, whereas capture.py exists to survive a replug. Use capture.py
for boot logs, this for asking the running firmware questions.

**Never pass a real credential.** Arguments land in shell history, in the
process list while running, and in whatever transcript is watching. Test with
throwaway values; real ones get typed by a human at `idf.py monitor`.
"""

import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200

# How long to keep reading after a command before deciding the reply is over.
# The console answers in microseconds; this is slack for a busy LVGL task
# holding the CPU, not a guess at how long a command takes.
QUIET_TIME = 0.4

# Ceiling on any one command, measured from when the read started rather than
# from the last byte. A board logging continuously — which is what Wi-Fi
# reconnect attempts look like — refreshes the quiet timer forever and would
# otherwise never let go.
REPLY_TIMEOUT = 5.0


def drain(port: serial.Serial, settle: float) -> bytes:
    """Read until the board has been quiet for `settle` seconds, or the ceiling."""
    out = bytearray()
    started = time.time()
    last = started
    while time.time() - last < settle:
        chunk = port.read(256)
        if chunk:
            out += chunk
            last = time.time()
        if time.time() - started > REPLY_TIMEOUT:
            # Deliberately returns what it has rather than raising: a truncated
            # reply still tells the caller what the board said, and a chatty
            # board is not an error.
            break
    return bytes(out)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    with serial.Serial(PORT, BAUD, timeout=0.1) as port:
        # Not asserted: this board wires neither line to EN/GPIO0, and driving
        # them would do nothing useful. Left explicit so nobody adds a reset
        # here expecting it to work — see CLAUDE.md on the two USB gestures.
        port.dtr = False
        port.rts = False

        # A newline first, to flush any half-typed line and force a fresh
        # prompt. Whatever comes back is boot log or prompt noise, not a reply
        # to anything asked, so it is discarded.
        port.write(b"\n")
        drain(port, QUIET_TIME)

        for command in sys.argv[1:]:
            print(f"$ {command}")
            port.write(command.encode() + b"\r\n")
            reply = drain(port, QUIET_TIME).decode(errors="replace")
            # The console echoes what was typed and re-prints the prompt around
            # the answer. Both are noise once the command is known, so only the
            # lines between them are shown.
            for line in reply.splitlines():
                stripped = line.strip()
                if not stripped or stripped == command:
                    continue
                print(f"  {stripped.removeprefix('panda>').strip()}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
