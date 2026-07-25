#pragma once

#include "esp_err.h"

/* Starts LVGL on top of the initialised display and builds the first screen.
 * display_init() must have succeeded first.
 *
 * LVGL runs in its own task from here on. Any code touching LVGL objects from
 * another task must hold the port lock (see ui.c for why). */
esp_err_t ui_init(void);
