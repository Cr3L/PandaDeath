#include "ota.h"

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"  /* IP_EVENT, IP_EVENT_STA_GOT_IP */
#include "esp_ota_ops.h"
#include "net.h"

static const char *TAG = "ota";

/* How long to wait on a stalled server before giving up. Long enough that a
 * slow LAN transfer is not mistaken for a hang, short enough that a wrong URL
 * answers while someone is still watching the console. */
#define HTTP_TIMEOUT_MS 10000

/* Confirms the running image, cancelling the pending rollback.
 *
 * Wired to IP_EVENT_STA_GOT_IP rather than to the end of boot, and that choice
 * is the whole design. The state this feature must never leave the board in is
 * "new image runs but cannot reach the network", because that is the one no
 * further update can fix — it costs a USB gesture, which is what OTA exists to
 * remove. Confirming only once an address is in hand makes that state
 * self-healing: the image never confirms, the next reset reverts, and the board
 * comes back on the firmware that worked.
 *
 * The cost is real and was accepted deliberately: if the router happens to be
 * down while a good new image is on probation, that image is rolled back
 * despite being fine. The board then runs the previous firmware, which cannot
 * reach the network either — so nothing is lost but the update, and it can be
 * done again. The asymmetry is the point. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    if (!ota_pending_verify()) {
        return;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "image confirmed; rollback cancelled");
    } else {
        /* Not fatal, and deliberately not a reboot: the image stays on
         * probation and the next reset reverts it, which is the safe direction
         * to fail in. */
        ESP_LOGE(TAG, "could not confirm image: %s", esp_err_to_name(err));
    }
}

esp_err_t ota_start(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "running %s from %s, built %s %s",
             app->version, ota_running_slot(), app->date, app->time);

    if (ota_pending_verify()) {
        ESP_LOGW(TAG, "image is on probation; will confirm once an address is up");
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL),
        TAG, "ip handler");

    /* An address that already exists means the event above has already fired,
     * and it will not fire again until the link drops — which for a probationary
     * image would mean never confirming. Same pattern as time_sync.c. */
    if (net_have_address()) {
        on_got_ip(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    }
    return ESP_OK;
}

esp_err_t ota_update(const char *url, ota_progress_cb_t progress, void *ctx)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Checked here rather than left to esp_ota_begin, which reports the same
     * condition as a generic failure several seconds and one whole download
     * later. Nothing has been fetched yet at this point. */
    if (ota_pending_verify()) {
        return ESP_ERR_OTA_ROLLBACK_INVALID_STATE;
    }

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };

    esp_https_ota_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(esp_https_ota_begin(&cfg, &handle), TAG, "begin");

    /* Read once, before the loop: this is the length from the HTTP response,
     * and it does not change as the body arrives. A server that sends no
     * content-length reports a non-positive size, which becomes zero here so
     * the rule lives where the value is derived rather than in the callback. */
    const int size = esp_https_ota_get_image_size(handle);
    const size_t total = size > 0 ? (size_t)size : 0;

    esp_err_t err;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (progress != NULL) {
            progress((size_t)esp_https_ota_get_image_len_read(handle), total, ctx);
        }
    }

    if (err != ESP_OK) {
        /* abort, not finish. finish() on an incomplete transfer would set the
         * boot partition to a slot holding a partial image. */
        esp_https_ota_abort(handle);
        return err;
    }

    /* A connection that closes early ends the loop with ESP_OK and a short
     * image, which is the failure most likely to be mistaken for success —
     * esp_https_ota_finish() also checks the image header, but a truncated
     * body can still carry a valid one. */
    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        ESP_LOGE(TAG, "connection closed before the image finished");
        return ESP_ERR_INVALID_SIZE;
    }

    return esp_https_ota_finish(handle);
}

const char *ota_running_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running != NULL ? running->label : "unknown";
}

const char *ota_next_slot(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    return boot != NULL ? boot->label : "unknown";
}

bool ota_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return false;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}
