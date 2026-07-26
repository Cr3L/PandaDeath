#pragma once

#include "esp_err.h"

/* Registers the wifi_set / wifi_show / wifi_clear console commands.
 *
 * Must be called before console_init(), so the prompt never accepts a command
 * that is not yet registered. Keeping the ordering in the boot path rather than
 * inside console_init() is what lets the console stay ignorant of Wi-Fi. */
esp_err_t wifi_cmd_register(void);
