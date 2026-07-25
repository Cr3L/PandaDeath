"""Serial capture that survives a USB replug.

Unplugging the board destroys /dev/ttyUSB0, which kills any handle held across
it. This reopens the port as it reappears, so a capture can be started before
the replug and still record the boot log that follows it.
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
out = []
ser = None

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
        out.append(b"\n--- port opened ---\n")

    try:
        chunk = ser.read(512)
        if chunk:
            out.append(chunk)
    except Exception as exc:
        out.append(f"\n--- port lost: {exc} ---\n".encode())
        with suppress(Exception):
            ser.close()
        ser = None
        time.sleep(RETRY_DELAY)

if ser is not None:
    with suppress(Exception):
        ser.close()

data = b"".join(out)
sys.stdout.write(f"bytes captured: {len(data)}\n")
sys.stdout.write(data.decode(errors="replace"))
