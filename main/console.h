#pragma once

#include "esp_err.h"

/* Starts the serial console on the same UART the log already uses.
 *
 * Sharing the port is a deliberate trade. The alternative is a second
 * USB-serial adapter on spare GPIOs, which buys clean separation at the cost of
 * hardware nobody wants to keep plugged in to type a password twice a year. The
 * cost of sharing is that anything logging on a timer walks over what is being
 * typed — see the rule in CLAUDE.md's Conventions.
 *
 * Owns the REPL and nothing else. Commands belong to the modules whose state
 * they touch and register themselves before this is called; a console that
 * #included every feature it can drive would depend on all of them to start
 * one.
 *
 * Runs in its own task from here on and returns immediately. Commands therefore
 * execute on that task, not app_main's. */
esp_err_t console_init(void);

/* Drops the line-editor history, so a line that has just been typed cannot be
 * recovered from DRAM or with the up arrow.
 *
 * For commands that take a secret as an argument. The REPL adds every accepted
 * line to the history unconditionally (esp_console_common.c:213), so a command
 * that scrubs its own buffers has still left the whole line, secret included,
 * in the editor's copy. Call this after handling one. */
void console_forget_history(void);
