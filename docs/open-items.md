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

## 1. No correctness pass over the firmware as a whole

`/code-review ultra` has run once, scoped to a two-commit cleanup diff, and
returned nothing — which says the cleanup was clean, not that `display.c` and
`ui.c` are. The one real bug found so far (the DMA drain in
`display_fill_rect`) came from reading the esp_lcd source, which is not a
repeatable process.

**Why it waits:** nothing blocks it. This is the largest gap on the list.

**Trigger:** run it scoped to the whole tree, not a branch diff.

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
