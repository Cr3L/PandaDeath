#pragma once

#include "esp_err.h"

/* Registers the weather, weather_refresh and loc console commands. Call before
 * console_init(), so the prompt never accepts a command that does not exist
 * yet. */
esp_err_t weather_cmd_register(void);
