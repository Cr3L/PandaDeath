#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"  /* IP4ADDR_STRLEN_MAX, the size wifi_sta_ip wants */

/* Wi-Fi station: reads the credentials wifi_creds.c stored and keeps the board
 * associated.
 *
 * There is no wifi_sta_stop(). Nothing needs one yet, and an untested teardown
 * path is worse than an absent one — it reads as available.
 *
 * Requires esp_netif_init() and esp_event_loop_create_default() to have run.
 * Those are the boot path's, not this module's: SNTP needs the same two, and a
 * module that initialises shared infrastructure as a side effect makes the
 * second user's correctness depend on call order. */

/* What the station is doing, as seen from outside.
 *
 * This lived in its own wifi_status.h while the on-glass indicator existed, so
 * that the UI could name a state without including esp_wifi.h transitively. The
 * indicator is gone and the only readers now are this module and the console
 * commands, which already include this header — so the seam has become a file
 * that exists to separate two things that are no longer apart.
 *
 * Ordered roughly by how far the station has got, but nothing depends on the
 * ordering — treat it as a set of names, not a scale. */
typedef enum {
    WIFI_STATUS_NO_CREDENTIALS,  /* nothing in NVS; the console is the fix */
    WIFI_STATUS_DISCONNECTED,    /* has credentials, not currently associated */
    WIFI_STATUS_CONNECTING,      /* associating, or waiting to retry */
    WIFI_STATUS_CONNECTED,       /* associated and holding an IP address */
} wifi_status_t;

const char *wifi_status_name(wifi_status_t status);

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

/* Tells the station the stored credentials changed: re-reads NVS and applies
 * whatever is there now, so nothing needs a reboot to take effect.
 *
 * Covers clearing as well as setting. Credentials gone stops the station and
 * reports NO_CREDENTIALS rather than failing — otherwise the board stays
 * associated to credentials it has been told to forget, with the indicator
 * green and wifi_show reporting nothing stored.
 *
 * Every command that mutates the credential store should call this. Naming it
 * for the event rather than for one caller's intent ("reconnect") is what stops
 * the next such command forgetting to. */
esp_err_t wifi_sta_credentials_changed(void);

wifi_status_t wifi_sta_status(void);

/* Fills `buf` with the current IPv4 address, or "0.0.0.0" when not connected.
 * Wants at least IP4ADDR_STRLEN_MAX bytes. */
void wifi_sta_ip(char *buf, size_t len);

/* Signal strength in dBm, false when not connected. Reported through here
 * rather than by letting callers reach for esp_wifi_sta_get_ap_info: this file
 * is the only one that talks to the driver, and the "connected or it is stale"
 * rule lives with the status it depends on. */
bool wifi_sta_rssi(int *rssi);
