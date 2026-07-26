#include "wifi_creds.h"

#include <string.h>

#include "storage.h"

#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_pass"

/* Nothing here logs. This is a library: it reports by returning esp_err_t, and
 * its only caller is a console command that already prints the outcome to the
 * person who asked. A second message would arrive with log furniture, on the
 * same UART, asynchronously against the prompt — and for wifi_set it would put
 * the SSID on screen twice. */

static esp_err_t erase_if_present(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);
    /* Erasing an absent key is the end state the caller asked for, so it is
     * success rather than a failure to remove something that was not there. */
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t wifi_creds_set(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || ssid[0] == '\0' ||
        strlen(ssid) > WIFI_SSID_MAX || strlen(pass) > WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    /* Both sets land in the same commit, so a power loss between them leaves
     * the old pair intact rather than a new SSID against an old password —
     * a state nothing could recover from without knowing which half was stale.
     * That is why the handle is held across both. */
    err = nvs_set_str(handle, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t wifi_creds_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    /* An unconfigured board fails here, at the open, rather than at the get
     * below: the namespace does not exist until something is written to it.
     * Both report ESP_ERR_NVS_NOT_FOUND, which is what the caller acts on. */
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    /* nvs_get_str takes the buffer size in and writes the used size back, so
     * the two calls cannot share one length variable. */
    size_t len = ssid_len;
    err = nvs_get_str(handle, KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(handle, KEY_PASS, pass, &len);
    }

    nvs_close(handle);
    return err;
}

esp_err_t wifi_creds_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  /* Never configured; the caller's wish already holds. */
    }
    if (err != ESP_OK) {
        return err;
    }

    err = erase_if_present(handle, KEY_SSID);
    if (err == ESP_OK) {
        err = erase_if_present(handle, KEY_PASS);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}
