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

Start it *before* asking for the replug, and do not pipe it through `tail` —
that buffers until the process exits, so a capture that is working looks
identical to one that is dead. Note also the app logs only during its first
seconds, so a capture started later records nothing — that is not a fault.

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

Deferred deliberately, with reasons — not forgotten:

1. **No correctness pass over the firmware as a whole.** `/code-review ultra`
   has now run once, but scoped to a two-commit cleanup diff, and returned
   nothing — which says the cleanup was clean, not that `display.c` and `ui.c`
   are. The bug it would have been aimed at (see the DMA drain in
   `display_fill_rect`) was found by reading, and sat outside that diff. A run
   scoped to the whole tree is still the highest-value gap.
2. **Partition table** — 1 MB app partition on a 16 MB chip, ~45% free. Worth
   fixing *before* Wi-Fi + TLS, since changing it later means a full erase.
   Note `sdkconfig` also sets `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=
   "partitions.csv"` while `SINGLE_APP` is selected, so that setting is inert
   and no such file exists — it reads as though a custom table is already in
   play. Clear that up in the same pass.
3. **No unwind on `display_init()` failure** — `s_panel` stays non-NULL, the
   module wedges, SPI bus and panel objects leak. Unreachable today because
   `ESP_ERROR_CHECK` aborts first. Wants a `fail:` label and a separate
   `s_ready` flag as the guard. The drain added before the fill-buffer free is
   one more early return on that path, and leaves `s_fill_buf` allocated as
   well, so the label now has more to clean up than when this was written.
4. **"Don't mix drawing paths" is prose, not code.** Nothing stops a caller
   using `display_fill*` after LVGL owns the panel. Becomes real the first time
   something draws a splash before LVGL starts. `fill_buf_wait_idle()` does not
   help here: it protects the fill buffer from the previous *fill*, not from
   LVGL.
5. **PSRAM disabled** — 8 MB unused. Enable when there is something cold to put
   there. Do *not* move LVGL draw buffers into it; the software renderer does
   per-pixel read-modify-write and PSRAM is ~10× slower.
6. **Stack smashing protection off** — turn on the day an HTTP or JSON parser
   lands. That is the bug class it catches.
7. **Author email is a personal gmail** in all commit history. Irrelevant while
   the repo is private, permanent if it ever goes public. Rewritable only while
   commits are unpushed, so this decides itself by default if left alone.
8. **Build inherits `-Og`** (IDF's default), never `-Os`. Likely the largest
   single win available on both image size and LVGL render time, since the
   software renderer's per-pixel work is what sets frame time. Needs deleting
   `sdkconfig` so new defaults take, so it batches with item 2.
9. **The initial black clear in `display_init()`** costs ~23 ms and 115 kB of
   SPI at boot, painting a frame nobody sees under a backlight still at zero.
   It is also the only reason the 9.6 kB DMA buffer is allocated during init at
   all. Kept deliberately as defence for a future splash path; recorded here so
   it is a decision rather than an oversight.
10. **`CONFIG_FREERTOS_HZ=1000`** buys 1 ms granularity that nothing currently
   needs — the LVGL tick is 10 ms and the shortest delay in the tree is 30 ms.
   The comment claiming the display wants it has been corrected; the value
   itself is still an open question, worth ~900 timer interrupts/s.

## Not yet decided

What the device actually does. Wi-Fi is the prerequisite for every candidate
(clock + weather, Klipper/Moonraker monitor, MQTT display). When Wi-Fi lands,
credentials should go in NVS via a serial console — never in the source tree,
since this repo is already on GitHub.
