# PandaDeath

Custom firmware for the **BigTreeTech Knomi V1** — an ESP32-WROVER-E board with
a 240×240 round GC9A01 display, originally a Klipper printer monitor.

The stock Knomi firmware has been replaced and is not being kept. What the board
eventually *does* is undecided; right now this is a working display driver and a
foundation to build on.

## Status

Display bring-up is complete and verified on hardware: colour, geometry,
orientation and refresh all confirmed. LVGL 9 runs on top of it via
`esp_lvgl_port`, currently showing a placeholder arc and labels.

## Hardware

| | |
|---|---|
| MCU | ESP32-WROVER-E (ESP32-D0WD-V3, dual core, 8 MB PSRAM) |
| Flash | 16 MB |
| Display | GC9A01, 240×240 round, 4-wire SPI |
| USB serial | CH340 (`/dev/ttyUSB0` on Linux) |
| Buttons | BOOT only (the gold button next to USB-C is RESET) |

### Pinout

Taken from BigTreeTech's own V1 firmware
([`src/pinout_knomi_v1.h`](https://github.com/bigtreetech/KNOMI/blob/firmware/src/pinout_knomi_v1.h)),
not measured or guessed. These are V1-only — the V2 is an ESP32-S3 with a
different map.

| Signal | GPIO | Note |
|---|---|---|
| MOSI | 23 | native VSPI IOMUX |
| SCLK | 18 | native VSPI IOMUX |
| CS | 5 | native VSPI IOMUX |
| DC | 19 | |
| RST | 4 | |
| Backlight | 2 | PWM via LEDC; also a boot strapping pin |
| BOOT | 0 | |

SPI3 (VSPI) is used because those three pins are its IOMUX defaults; routing
through the GPIO matrix instead would cap the usable clock.

## Building

Requires ESP-IDF 5.5+. Source the environment in every new shell:

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32     # first time only
idf.py build
```

## Flashing — read this first

**This board has no working auto-reset.** DTR/RTS are not wired to EN/GPIO0, so
esptool cannot enter download mode on its own. Every flash fails with
`Failed to connect to ESP32: No serial data received` unless the board is put
into the bootloader by hand:

> **unplug USB → hold BOOT → replug while still holding → hold 2–3 s → release**

Then:

```sh
idf.py -p /dev/ttyUSB0 flash
```

Holding BOOT on a board that is already running does nothing useful — it has to
be held across an actual power-on. After flashing, **physically replug again** to
run the new app.

### Two different replugs

These are easy to conflate and the symptoms are confusing when you do:

| Goal | Gesture |
|---|---|
| **Flash** new firmware | unplug → **hold BOOT** → replug → hold 2–3 s → release |
| **Run** what is flashed | unplug → replug, **touching nothing** |

Holding BOOT during what was meant to be a plain replug leaves the board in
download mode: the app never starts, so the screen stays completely dark and
nothing is printed on serial. A powered board that is silent *and* dark is
almost always this, not a crash. Confirm with:

```sh
python -m esptool --chip esp32 -p /dev/ttyUSB0 --before no_reset chip_id
```

If that connects without the BOOT sequence, the board was already sitting in the
bootloader.

### Watching the serial log

`idf.py monitor` needs a TTY. For scripted capture use:

```sh
python3 tools/capture.py 60     # seconds
```

It reopens the port when the device reappears, because unplugging destroys the
tty and kills any handle held across it — a capture started before the replug
would otherwise just die.

## Panel quirks

Two settings could not be resolved by reading code and are isolated at the top of
[`main/display.c`](main/display.c) as one-line changes:

- **`PANEL_INVERT`** — on here. BTT's TFT_eSPI setup drives the same panel with
  inversion *off*; starting from their value rendered an exact colour complement
  (black as white, green as magenta). The two stacks don't share a baseline.
- **`PANEL_BGR_ORDER`** — on. Flip if red and blue swap.

`LCD_PIXEL_CLOCK_HZ` is 40 MHz. BTT runs this panel at 80 MHz, so there is
headroom, but a marginal signal shows up as sparkle or a dead panel rather than a
clean error.

**Orientation** is `PANEL_MIRROR_X` / `PANEL_MIRROR_Y` in
[`main/display.h`](main/display.h). The panel's native scan order is a
*reflection*, not a rotation — so it needs an odd number of axis flips to
correct. Mirroring both axes looks like an obvious fix and is wrong: that is a
180° rotation, which leaves text upright but reading backwards, as if seen from
behind the screen.

These constants are public rather than private to `display.c` because two things
apply them and they must agree. `display_init()` mirrors the panel so the
LVGL-free self-test is oriented correctly, and `esp_lvgl_port` mirrors it again
from its own rotation config when LVGL attaches. Zeroing the latter does *not*
mean "inherit" — it actively mirrors nothing and silently undoes the driver.

## Verifying changes on hardware

Set `RUN_HARDWARE_SELFTEST` to `1` in [`main/main.c`](main/main.c) to run the raw
panel exercise instead of the UI: a colour walk, a **static** geometry frame held
20 s, then a motion loop. It talks straight to the panel with no LVGL involved,
which is the fastest way to tell a wiring or panel-config fault from a graphics
fault — and it is how a second board should be brought up.

The static frame exists because photographing a *moving* pattern is misleading —
phone rolling shutter smears a travelling shape along its axis of motion, which
looks exactly like the shape being the wrong size. Judge geometry only from the
static frame. Note it is symmetric about both axes, so it cannot catch a
mirroring fault; add an asymmetric element if that is what you are chasing.

Cameras also blow out bright colours against a dark background (green reads
white, red reads yellow) and LCD black leaks blue at full backlight, so **colour
correctness is an eyeball judgement, not a photo judgement.**

## Layout

```
main/
  display.c/h    GC9A01 driver: SPI bus, panel, backlight PWM/fade, rect fills
  ui.c/h         LVGL setup and screen construction
  selftest.c/h   raw panel exercise, bypasses LVGL entirely
  main.c         boot path: display -> UI -> fade up
tools/
  capture.py     serial capture that survives a USB replug
```

`ui.c` touches no pin, clock or panel command — it depends on `display.h` only
for the panel dimensions and handles. That seam is what should carry to a second
board; the screen *layout* in `ui.c` will not, since the arc and offsets are
tuned for a 240×240 round panel.

Built on ESP-IDF's native `esp_lcd` stack rather than Arduino/TFT_eSPI, so the
concepts carry over to other ESP32 boards. Only the pin numbers came from BTT.
