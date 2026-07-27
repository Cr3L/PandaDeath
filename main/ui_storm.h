#pragma once

/* The storm screen: a proximity dial around the rim, conditions in the middle,
 * and a rotating thunderstorm fact underneath.
 *
 * Builds on lv_screen_active(). Assumes the LVGL port lock is held, like every
 * other screen builder here. */
void ui_storm_screen_build(void);
