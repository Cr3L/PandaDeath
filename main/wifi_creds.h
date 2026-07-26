#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "nvs.h"  /* ESP_ERR_NVS_NOT_FOUND, returned below. */

/* Wi-Fi credentials in NVS.
 *
 * There is no API here that takes a credential from a compiled-in constant:
 * the only way one gets in is wifi_creds_set() from the serial console. The
 * repo is public, and git keeps a password after the file holding it is edited,
 * so the unsafe option is absent rather than discouraged.
 *
 * The password is never printed, logged, or echoed anywhere in the tree. That
 * is an interface promise, not an implementation detail — callers displaying
 * credentials should show the SSID and describe the password.
 *
 * Lengths are the 802.11 maxima and exclude the terminator; the _BUF forms add
 * it, so callers declaring `char ssid[WIFI_SSID_BUF]` cannot get that ±1 wrong. */
#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 63
#define WIFI_SSID_BUF (WIFI_SSID_MAX + 1)
#define WIFI_PASS_BUF (WIFI_PASS_MAX + 1)

/* Stores both values or neither — see the single commit in wifi_creds.c.
 *
 * An empty pass means an open network, which is legal; an empty ssid is not.
 * Returns ESP_ERR_INVALID_ARG for an empty ssid or a value too long for the
 * standard. */
esp_err_t wifi_creds_set(const char *ssid, const char *pass);

/* Reads the stored pair into buffers of WIFI_SSID_BUF / WIFI_PASS_BUF.
 *
 * Returns ESP_ERR_NVS_NOT_FOUND if nothing has been stored, which is the
 * expected state of a freshly flashed board rather than an error. */
esp_err_t wifi_creds_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* Forgets both values. Clearing what was never set is success. */
esp_err_t wifi_creds_clear(void);
