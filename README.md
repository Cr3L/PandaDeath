# PandaDeath

Custom firmware for the **BigTreeTech Knomi V1** — an ESP32-WROVER-E board with
a 240×240 round GC9A01 display, originally a Klipper printer monitor.

The stock Knomi firmware has been replaced and is not being kept. What the board
eventually *does* is still undecided; what exists is a working display stack, a
serial console, and the storage half of Wi-Fi.

Built on ESP-IDF's native `esp_lcd` stack rather than Arduino/TFT_eSPI, so the
concepts carry over to other ESP32 boards. Only the pin numbers came from BTT.

## Status

Verified on hardware:

- **Display** — colour, geometry, orientation and refresh all confirmed.
- **LVGL 9** on top of it via `esp_lvgl_port`.
- **The zoo** — four pixel-art animals walking in turn, from PNGs in `assets/`
  compiled to C sprite tables. This is what the board shows today.
- **A serial console** on the log UART, and **Wi-Fi credentials in NVS**
  entered through it. Confirmed surviving a power cycle.
- **Wi-Fi** — the station reads those credentials at boot, associates, and
  reconnects with exponential backoff. Failure paths confirmed too: wrong
  password, absent network, and credentials changed on a live station.
- **The clock** — SNTP from `pool.ntp.org` a few seconds after the address
  arrives, with the timezone stored in NVS through a `tz` command.
- **A partition table sized for OTA**, laid out before Wi-Fi on purpose — and
  now used: the board installs firmware over the network into whichever slot it
  is not running from. A new image is on probation until it reaches the network,
  and reverts on the next reset if it never does, so a bad update costs a reboot
  rather than a cable. Verified by installing an image built deliberately not to
  confirm itself and watching it roll back.

Not done: anything that uses the network for a purpose. The board knows who it
is, where it is on the network and what time it is, and shows none of it — the
glass still shows only the zoo. Clock and weather, a Klipper/Moonraker monitor,
an MQTT display are all unblocked and none has been chosen. Each needs a screen
design, which is the first real UI decision this project has had to make.

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

Both `tools/capture.py` and `tools/console.py` import pyserial, which exists only
in the IDF virtualenv. Source `export.sh` first, or call that interpreter
directly.

## The console

The firmware runs an `esp_console` REPL on the same UART as the log, prompting
`panda>`. Open it with `idf.py monitor` and type `help`.

Sharing the UART with the log is a real constraint, not an accident: anything
that prints on a timer walks over whatever is being typed. Hence the rule that
**nothing periodic logs above `DEBUG`**, which is compiled out at the current
log level.

`console.c` owns the REPL and **no commands at all**. Commands live with the
module whose state they touch — the `wifi_*` commands are in `wifi_cmd.c`, next
to the credential store they drive. A console that `#include`d every feature it
can drive would depend on all of them just to start. `main.c` registers commands
before starting the REPL, so the prompt never offers one that does not exist yet.

For scripted use without a terminal:

```sh
python3 tools/console.py wifi_show "wifi_set TestNetwork hunter2" wifi_show
```

Each argument is one command line. **Never pass a real credential this way** —
arguments land in shell history and in the process list. Real ones get typed by
a human at `idf.py monitor`.

## Wi-Fi credentials

| Command | |
|---|---|
| `wifi_set <ssid> [password]` | store; quote an SSID containing spaces |
| `wifi_show` | print the SSID, and the password's *length* only |
| `wifi_clear` | erase both |

They go to NVS under the `pandadeath` namespace and survive a power cycle. This
repo is public, so **there is deliberately no API anywhere in the tree that takes
a credential from a compiled-in constant** — the console is the only way one gets
in.

The password is never printed, logged or echoed. `wifi_set` also calls
`console_forget_history()`, because clearing the local buffer is not enough: the
REPL's line editor has already retained the whole line, so a later up-arrow would
hand the password back. That is the layer the secret actually lives in.

## Flash layout

[`partitions.csv`](partitions.csv) is sized for OTA — two 4 MB app slots against
a ~0.6 MB image. That is not speculation about growth; it is because this board
is *awkward to flash by wire*, so an over-the-air path removes the manual gesture
from the normal loop once Wi-Fi runs.

It was laid out **before** Wi-Fi on purpose. Changing the map once `nvs` and
`otadata` hold real data means erasing the chip, which takes the credentials with
it.

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

`BOOT_MODE` in [`main/boot_mode.h`](main/boot_mode.h) picks what the build brings
up. The three values are rungs of one ladder, not independent options — each adds
a layer to the one below, so stepping *down* it is how you find which layer owns
a fault:

| Value | Brings up | Useful when |
|---|---|---|
| `BOOT_MODE_SELFTEST` | the panel alone, no LVGL | a wiring, SPI or panel-config fault |
| `BOOT_MODE_TEST_SCREEN` | LVGL, no assets | LVGL port or rendering fault |
| `BOOT_MODE_ZOO` | LVGL plus the sprite tables | the normal build |

The self-test is a colour walk, a **static** geometry frame held 20 s, then a
motion loop, talking straight to the panel with no LVGL involved. It is how a
second board should be brought up.

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
  display.c/h    panel hardware: pins, SPI, backlight, orientation, rect fills
  ui.c/h         LVGL port bring-up; picks which screen gets built
  ui_zoo.c/h     the zoo screen: four animals, cycling
  ui_test.c/h    arc + counter, kept as a known-good LVGL reference
  zoo_sprites.*  generated by tools/gen_sprites.py — never edit by hand
  selftest.c/h   raw panel exercise, bypasses LVGL entirely
  boot_mode.h    which of the three the build brings up
  console.c/h    the REPL on the log UART; owns no commands
  wifi_cmd.c/h   the wifi_* console commands
  wifi_creds.c/h Wi-Fi credentials in NVS — the only way one gets in
  main.c         boot path: NVS -> commands -> console -> display -> UI -> fade up
assets/          sprite PNGs; the source of truth for the art
tools/
  capture.py     serial capture that survives a USB replug
  console.py     drives the console without a terminal
  gen_sprites.py assets/*.png -> zoo_sprites.c (needs Pillow, system python)
docs/
  open-items.md  work deferred deliberately, with the reason and the trigger
```

The console comes up **before** the display. It owns no hardware the display
wants, and it is the only way to repair a board whose credentials are wrong — a
UI failure should not take the repair path down with it.

`ui.c` touches no pin, clock or panel command — it depends on `display.h` only
for the panel dimensions and handles. That seam is what should carry to a second
board; the screen *layouts* will not, since they are tuned for a 240×240 round
panel.

Anything that is a property of the physical glass — inversion, BGR order,
mirroring — belongs in the display layer, so the LVGL-free self-test sees the
same panel the UI does.

The generated sprite C is committed so that a firmware build needs nothing beyond
the IDF. The PNGs remain the source of truth; regenerate rather than editing the
output.

Working on this? [CLAUDE.md](CLAUDE.md) covers *how* — the build model, the
verification rules, the conventions. This file covers what the thing is.

## What is deliberately unfinished

[`docs/open-items.md`](docs/open-items.md) carries each deferred item with the
reason it waits and the condition that should bring it back. **Read it before
planning work** — it is where "why hasn't this been done yet" is answered.
