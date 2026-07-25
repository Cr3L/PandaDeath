# Working on PandaDeath

Firmware for a BTT Knomi V1 (ESP32-WROVER-E, 240×240 round GC9A01 panel).
See [README.md](README.md) for hardware facts, pinout and panel quirks — this
file is about *how to work on it*, not what it is.

Michael is new to embedded development and is building fundamentals
deliberately, smallest verifiable step first. Explain what a thing *is*
alongside the command that does it — the build model, what a log line means,
why a peripheral needs a given setup. Do not just hand over commands.

## The two USB gestures — get this right

This board's CH340 has no working auto-reset, so **both** of these are manual
and they are different:

| Goal | Gesture |
|---|---|
| **Flash** firmware | unplug → **hold BOOT** → replug → hold 2–3 s → release |
| **Run** what's flashed | unplug → replug, **touching nothing** |

Always ask Michael to perform the gesture and wait for confirmation before
running `idf.py flash`. Never assume auto-reset works — it does not.

**`erase-flash` needs its own gesture.** It is tempting to reason that since the
closing "hard reset via RTS" does nothing on this board, the chip stays in
download mode and an erase can be chained straight into a flash on one gesture.
It cannot: the erase leaves the chip out of download mode, and the flash that
follows fails to connect. Budget two gestures, and note the window between them
is the one moment the board has no firmware at all — dark screen and silent
serial there is the erase having worked, not a brick.

Holding BOOT during what was meant to be a plain replug leaves the board in
download mode: **screen completely dark and serial completely silent.** That
combination reads like a crash but is not. Confirm with:

```sh
python -m esptool --chip esp32 -p /dev/ttyUSB0 --before no_reset chip_id
```

If it connects *without* the BOOT gesture, the board was parked in the
bootloader. This cost half an hour once; don't rediscover it.

## Build

```sh
. ~/esp/esp-idf/export.sh      # required in every new shell
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

`/dev/ttyUSB0` is the board. `/dev/ttyACM0` on this machine is the laptop's
fingerprint reader — not the board.

Changing `sdkconfig.defaults` does nothing on its own; `sdkconfig` already
exists and wins. Delete `sdkconfig` to pick up new defaults.

## Verifying changes

**Every behavioural change gets flashed and checked on hardware before it is
committed.** Two of today's bugs — a mirrored display and a silently overridden
panel setting — compiled cleanly, reviewed cleanly, and were only caught on the
device.

Serial: `idf.py monitor` needs a TTY, so it must be run by Michael, not by an
agent. For scripted capture use `tools/capture.py 60`, which reopens the port
across a replug.

It imports pyserial, which exists **only in the IDF virtualenv** — a bare
`python3 tools/capture.py` dies on `ModuleNotFoundError: No module named
'serial'`. Either source `export.sh` first, or call the interpreter directly:

```sh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/capture.py 60
```

Start it *before* asking for the replug. It streams as it reads, so a live
capture is visibly alive; do not pipe it through `tail`, which buffers until the
process exits and makes a working capture look identical to a dead one. Note
also the app logs only during its first seconds, so a capture started later
records nothing — that is not a fault.

**Never diagnose geometry from a photo or video of a moving pattern.** Phone
rolling shutter smears a travelling shape along its axis of motion, which is
indistinguishable from the shape being the wrong size, and the apparent size
varies frame to frame so it reads as an intermittent bug. Use the static frame
in `selftest.c` (`RUN_HARDWARE_SELFTEST 1`) and judge only that.

Cameras also blow out bright colours on a dark background (green photographs as
white, red as yellow) and LCD black leaks blue at full backlight. **Colour
judgements belong to Michael's eye, not to a photo.**

## Layering

```
display.c/h   panel hardware: pins, SPI, backlight, orientation, rect fills
ui.c/h        LVGL setup and screens — touches no pin or panel command
selftest.c/h  raw panel exercise, bypasses LVGL entirely
main.c        boot path: display → UI → fade up
```

Anything that is a property of the physical glass — inversion, BGR order,
mirroring — belongs in the display layer, so that the LVGL-free self-test path
sees the same panel the UI does.

**But note:** `esp_lvgl_port` re-applies mirroring from its own rotation config
when it attaches, and zeroes there mean "mirror nothing", not "inherit". That is
why `PANEL_MIRROR_*` is public in `display.h` and read by both.

## Conventions

- Comments explain *why*, especially where a value was settled empirically or a
  tempting alternative is wrong. Do not narrate what the code plainly does.
- Commit messages explain the reasoning and what was rejected, not just the
  change. Existing history is the reference for depth.
- Prefer fixing the layer the problem lives in over adding a special case above
  it.

## Open items

Tracked in [docs/open-items.md](docs/open-items.md), one entry each with the
reason it waits and the condition that should bring it back. **Read that file
before planning work** — it is where "why hasn't this been done" is answered,
and it is deliberately not duplicated here.

The one that has no reason to wait, repeated here so it cannot be missed: **no
correctness pass has ever examined `display.c` and `ui.c` as a whole.**
`/code-review ultra` has run only against a cleanup diff, and the single real
bug found so far came from reading esp_lcd's source by hand.

## Not yet decided

What the device actually does. Wi-Fi is the prerequisite for every candidate
(clock + weather, Klipper/Moonraker monitor, MQTT display). When Wi-Fi lands,
credentials should go in NVS via a serial console — never in the source tree,
since this repo is already on GitHub.
