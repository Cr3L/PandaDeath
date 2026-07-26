#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "wifi_status.h"

/* Wi-Fi station: reads the credentials wifi_creds.c stored and keeps the board
 * associated.
 *
 * There is no wifi_sta_stop(). Nothing needs one yet, and an untested teardown
 * path is worse than an absent one — it reads as available. */

/* Brings the station up and returns as soon as association has been *started*,
 * not when it succeeds. Connecting takes seconds and may never succeed; a call
 * that blocked on it would hold up boot on a bad password.
 *
 * With no credentials stored this still returns ESP_OK, having set the status
 * to NO_CREDENTIALS. That is not a failure of this function: a board that has
 * never been told a network is in a normal state, and the console that fixes it
 * is already running. Returning an error would push ESP_ERROR_CHECK into
 * rebooting the one path out of the problem. */
esp_err_t wifi_sta_start(void);

/* Re-reads NVS and re-associates. For use after wifi_set, so that new
 * credentials do not need a reboot to take effect. Safe before wifi_sta_start()
 * only in the sense that it will fail cleanly. */
esp_err_t wifi_sta_reconnect(void);

/* Registers the single status observer. Called before wifi_sta_start() so no
 * transition is missed; a second call replaces the first. One observer rather
 * than a list because there is exactly one consumer and a list would be
 * speculative machinery with a locking question attached. */
void wifi_sta_set_observer(wifi_status_observer_t observer);

wifi_status_t wifi_sta_status(void);

/* Fills `buf` with the current IPv4 address, or "0.0.0.0" when not connected.
 * Wants at least 16 bytes. */
void wifi_sta_ip(char *buf, size_t len);
