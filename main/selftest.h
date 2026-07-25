#pragma once

/* Raw panel exercise that bypasses LVGL entirely: colour walk, a static
 * geometry frame, then a motion loop. Never returns.
 *
 * Kept because it is the fastest way to tell a wiring or panel-config fault
 * from a graphics fault, and it is what a second board should be brought up
 * with before LVGL is in the picture. Requires display_init() to have run. */
void run_selftest(void);
