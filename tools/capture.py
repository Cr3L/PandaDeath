"""Serial capture that survives a USB replug.

Unplugging the board destroys /dev/ttyUSB0, which kills any handle held across
it. This reopens the port as it reappears, so a capture can be started before
the replug and still record the boot log that follows it.
"""

import os
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0

deadline = time.time() + DURATION
out = []
ser = None

while time.time() < deadline:
    if ser is None:
        if not os.path.exists(PORT):
            time.sleep(0.2)
            continue
        try:
            ser = serial.Serial()
            ser.port = PORT
            ser.baudrate = BAUD
            ser.timeout = 0.3
            # Leave the handshake lines alone: this board does not wire them to
            # EN/GPIO0, and driving them has no useful effect here.
            ser.dtr = False
            ser.rts = False
            ser.open()
            out.append(b"\n--- port opened ---\n")
        except Exception:
            ser = None
            time.sleep(0.3)
            continue

    try:
        chunk = ser.read(512)
        if chunk:
            out.append(chunk)
    except Exception as exc:
        out.append(f"\n--- port lost: {exc} ---\n".encode())
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(0.3)

if ser is not None:
    try:
        ser.close()
    except Exception:
        pass

data = b"".join(out)
sys.stdout.write(f"bytes captured: {len(data)}\n")
sys.stdout.write(data.decode(errors="replace"))
