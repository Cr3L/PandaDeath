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

## 2. "Don't mix drawing paths" is prose, not code

Nothing stops a caller using `display_fill*` after LVGL owns the panel.
`fill_buf_wait_idle()` does not help: it protects the fill buffer from the
previous *fill*, not from LVGL.

**Why it waits:** no caller does this today, and the enforcement worth having
depends on what the second drawing path turns out to be.

**Trigger:** the first time something draws a splash before LVGL starts.

## 3. PSRAM disabled

8 MB unused.

**Why it waits:** nothing cold needs to live there yet, and enabling it costs
DRAM for the mapping.

**Trigger:** something large and rarely touched — a font set, a bitmap cache.

**Do not** move LVGL draw buffers into it: the software renderer does per-pixel
read-modify-write and WROVER PSRAM is roughly 10x slower, which would push
render time past flush time.

## 4. Stack smashing protection off

**Why it waits:** it costs a little size and speed, and catches a bug class the
current code cannot produce — there is no parser here.

**Trigger:** the day an HTTP or JSON parser lands.

## 5. The initial black clear paints an invisible frame

`display_init()` clears to black while the backlight is still at zero, costing
~23 ms and 115 kB of SPI at boot. It is also the only reason the 9.6 kB DMA
fill buffer is allocated during init at all.

**Why it waits:** kept deliberately as defence for a future splash path, and as
insurance against showing power-up noise if the fade ever moves earlier.

**Trigger:** if boot latency ever matters, this is 23 ms sitting in plain sight.

## 6. `CONFIG_FREERTOS_HZ=1000`

Buys 1 ms scheduling granularity that nothing currently needs — the LVGL tick is
10 ms and the shortest delay in the tree is 30 ms. Costs ~900 timer interrupts a
second.

**Why it waits:** harmless today, and 100 Hz would be a regression if something
later does want fine delays.

**Trigger:** if the tick ISR shows up competing with Wi-Fi.

## 7. Only one of `display_init()`'s six failure exits has executed

The unwind was verified by injecting a failure at the last and most
resource-heavy point, confirming on hardware that a second `display_init()`
then succeeds. Earlier failures take shorter paths through the same label and
remain reasoned-about rather than run.

**Why it waits:** each additional exit costs a flash cycle for progressively
less information, and the whole path is unreachable while `app_main` wraps init
in `ESP_ERROR_CHECK`.

**Trigger:** the first caller that wants to survive a failed `display_init()`.
