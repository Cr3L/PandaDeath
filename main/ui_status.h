#pragma once

#include "wifi_status.h"

/* The connection indicator: one small glyph at the top of whatever screen is
 * showing.
 *
 * Deliberately not part of ui_zoo.c or ui_test.c. It belongs to no screen —
 * it reports something true of the whole device — and duplicating it into each
 * screen would mean every future screen has to remember to add it. */

/* Builds the indicator on the active screen. Call with the LVGL port lock held;
 * ui_init() already holds it while building screens. */
void ui_status_build(void);

/* Sets what the indicator shows. Takes the LVGL lock itself, because this is
 * what the Wi-Fi module's observer calls and that runs on the event task.
 * Safe to call before ui_status_build(), and before LVGL exists at all — it
 * does nothing rather than requiring the caller to sequence the two. */
void ui_status_set(wifi_status_t status);
