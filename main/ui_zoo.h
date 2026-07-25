#pragma once

/* The zoo: each animal walks its loop for a few seconds, then the next one
 * takes over, round and round.
 *
 * Caller must hold the LVGL port lock. */
void ui_zoo_screen_build(void);
