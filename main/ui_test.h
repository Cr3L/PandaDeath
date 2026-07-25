#pragma once

/* The sweeping arc and counter that were the first thing this panel ever drew.
 * Kept as a known-good reference rather than deleted: it exercises LVGL's
 * animation timer, the draw path and the flush end to end, and it is the only
 * screen whose correct appearance is already established on hardware. When
 * something looks wrong, this answers "is the display broken, or is it my new
 * screen?" — which is a question the zoo screen cannot answer about itself.
 *
 * Caller must hold the LVGL port lock. */
void ui_test_screen_build(void);
