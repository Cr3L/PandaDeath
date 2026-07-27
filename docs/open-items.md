# Open items

Work deferred deliberately, each with the reason and the condition that should
bring it back. Nothing here is forgotten; if an item no longer has a reason to
wait, it has become work rather than an open item.

**Read this before planning work.** [CLAUDE.md](../CLAUDE.md) covers how to
build, flash and verify; this file covers what is knowingly unfinished.

When an item closes, delete it and say so in the commit that closed it. Do not
leave a struck-through record — git already has that, and a list of dead entries
is what stops people reading the live ones.

---

## 1. The whole-tree correctness pass was a hand read, not a tool

Every source file, plus the build config and `tools/capture.py`, has now been
read end to end against the esp_lcd and LEDC sources. That produced eight
findings, all fixed. So the gap this item described is largely closed — by one
reading, which is as repeatable as the reader.

`/code-review ultra` cannot substitute, and it is worth recording why so the
next person does not spend a run learning it: the command is diff-scoped
against the branch upstream. Passing "review the whole project" as an argument
is recorded as a note and does not change the scope — two runs have now
returned zero findings on narrow diffs, which reads as a clean bill of health
for the project and is not one. Forcing whole-tree scope means constructing a
PR against an empty base, which is a contrivance rather than a workflow.

**Why it waits:** the cheap version is done. What remains is a second
*independent* read, and a second pass by the same reader is worth much less
than the first.

**Trigger:** a reviewer who is not me — a human, or ultra pointed at a diff
that genuinely contains the files. Otherwise this closes on its own as each
future change gets reviewed on the way in.

## 2. Sprite pixels are committed as 2 MB of C text

`main/zoo_sprites.c` is 2 MB of `0x00, ` ASCII holding 324 kB of actual pixels —
a 6x expansion of the bytes and a 100x expansion of the 20 kB of PNGs they come
from. Every regeneration rewrites all of it, so a one-pixel art tweak produces a
multi-megabyte diff no review can read.

The fix is not build-time generation: that would put Pillow in the path of every
clean build, on a project whose CLAUDE.md already documents two Python
environments as a live source of confusion. Committing the artifact is right.
Committing it *as C text* is not — the generator should emit a packed binary
blob plus a small descriptor table, pulled in with IDF's `EMBED_FILES`. Git then
stores the pixels as pixels, and the reviewable part shrinks to a few dozen
lines.

**Why it waits:** the art is settled and the current form works, verified on
hardware. Doing it now would mean rewriting the asset pipeline in the same
breath as landing the feature, and the feature is the thing that was tested.

**Trigger:** the first art change. That is when the unreadable diff stops being
theoretical. Fold in the removal of `zoo_animal_t` at the same time — the struct
exists to serve one log line and the blob rework replaces its layout anyway.

## 3. Nothing can tear a screen down

`ui_zoo_screen_build()` creates an `lv_timer` and builds objects on
`lv_screen_active()`; neither can be released. With compile-time `BOOT_MODE`
selection exactly one screen is ever built and never destroyed, so this costs
nothing today.

**Why it waits:** a `screen_t { build, destroy }` interface with two screens,
one of which is never dispatched on, is indirection that nothing uses. Screen
count is not the trigger — three more `void f(void)` builders cost nothing.

**Trigger:** the first time two screens must coexist or swap at runtime. That is
when `destroy` acquires a job, and the interface can be extracted then from two
concrete examples rather than guessed at now from none.

## 4. "Don't mix drawing paths" is prose, not code

Nothing stops a caller using `display_fill*` after LVGL owns the panel.
`fill_buf_wait_idle()` does not help: it protects the fill buffer from the
previous *fill*, not from LVGL.

**Why it waits:** no caller does this today, and the enforcement worth having
depends on what the second drawing path turns out to be.

**Trigger:** the first time something draws a splash before LVGL starts.

## 5. PSRAM disabled

8 MB unused.

**Why it waits:** nothing cold needs to live there yet, and enabling it costs
DRAM for the mapping.

**Trigger:** something large and rarely touched — a font set, a bitmap cache.

**Do not** move LVGL draw buffers into it: the software renderer does per-pixel
read-modify-write and WROVER PSRAM is roughly 10x slower, which would push
render time past flush time.

## 6. Stack smashing protection off — **the trigger has fired**

**Why it waited:** it costs a little size and speed, and caught a bug class the
code could not produce, because there was no parser.

**Trigger, as written:** "the day an HTTP or JSON parser lands." That day was
the OTA session — `esp_http_client` now parses headers and a chunked body from
the network, on a task that also holds a flash writer.

This is left as an open item rather than done silently only because turning it
on changes every stack frame in the image and wants its own verification pass,
not because the reason to wait still holds. It does not. This should be the
first thing picked up next session: `CONFIG_COMPILER_STACK_CHECK_MODE_NORM`,
then re-run the OTA matrix, which is now cheap to re-run.

## 7. The initial black clear paints an invisible frame

`display_init()` clears to black while the backlight is still at zero, costing
~23 ms and 115 kB of SPI at boot. It is also the only reason the 9.6 kB DMA
fill buffer is allocated during init at all.

**Why it waits:** kept deliberately as defence for a future splash path, and as
insurance against showing power-up noise if the fade ever moves earlier.

**Trigger:** if boot latency ever matters, this is 23 ms sitting in plain sight.

## 8. `CONFIG_FREERTOS_HZ=1000`

Buys 1 ms scheduling granularity that nothing currently needs — the LVGL tick is
10 ms and the shortest delay in the tree is 30 ms. Costs ~900 timer interrupts a
second.

**Why it waits:** harmless today, and 100 Hz would be a regression if something
later does want fine delays.

**Trigger:** if the tick ISR shows up competing with Wi-Fi.

## 9. Only one of `display_init()`'s six failure exits has executed

The unwind was verified by injecting a failure at the last and most
resource-heavy point, confirming on hardware that a second `display_init()`
then succeeds. Earlier failures take shorter paths through the same label and
remain reasoned-about rather than run.

**Why it waits:** each additional exit costs a flash cycle for progressively
less information, and the whole path is unreachable while `app_main` wraps init
in `ESP_ERROR_CHECK`.

**Trigger:** the first caller that wants to survive a failed `display_init()`.

## 10. OTA is plain HTTP

`ota <url>` fetches over unencrypted HTTP, enabled deliberately with
`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`. On a LAN, initiated by a human typing a URL
at a console, the exposure is that someone already inside the network could
answer at that address with their own image.

**Why it waits:** the certificate bundle is ~50 kB and TLS would have landed in
the same change as the feature being tested, so a failure could have been either.
It is genuinely unblocked now — certificate validation needs a correct clock, and
SNTP supplies one.

**Trigger:** the first update that comes from anywhere but this LAN. Also worth
doing sooner if the board is ever exposed to a network with guests on it.

## 11. `built:` in `ota_status` does not track source changes

`esp_app_desc.c` is compiled once and not rebuilt when other sources change, so
two images with different code can report the same build timestamp — observed
directly during OTA testing, where two functionally different images both said
`20:43:47`. The version string is `git describe`, so it only moves on a commit
and reads `-dirty` in between.

**Why it waits:** slot alternation and observed behaviour distinguished the
images perfectly well during testing, so nothing was blocked.

**Trigger:** the first time an update has to be identified after the fact rather
than during a session that just built it — which is the first update that
matters. A `version.txt` read by IDF as `PROJECT_VER` is the small fix.
