#pragma once

#include "esp_err.h"

/* Registers the time and tz console commands. Call before console_init(), so
 * the prompt never accepts a command that does not exist yet.
 *
 * Lives with time_sync rather than in console.c for the reason CLAUDE.md gives:
 * a console that #included every feature it can drive would depend on all of
 * them to start one. */
esp_err_t time_cmd_register(void);
