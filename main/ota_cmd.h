#pragma once

#include "esp_err.h"

/* Registers the ota, ota_status and reboot console commands. Call before
 * console_init(), so the prompt never accepts a command that does not exist
 * yet. */
esp_err_t ota_cmd_register(void);
