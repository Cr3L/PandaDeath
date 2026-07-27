#include "boot_mode.h"
#include "console.h"
#include "display.h"
#include "net.h"
#include "ota.h"
#include "ota_cmd.h"
#include "selftest.h"
#include "time_cmd.h"
#include "time_sync.h"
#include "ui.h"
#include "wifi_cmd.h"
#include "wifi_sta.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#define BACKLIGHT_FADE_MS 400

/* Brings up the NVS partition, recovering from the two states in which a
 * partition that exists is nonetheless unusable.
 *
 * NO_FREE_PAGES means the partition filled; NEW_VERSION_FOUND means it was
 * written by a newer NVS format than this build understands. Both are fixed
 * only by erasing, and both are survivable here because everything we keep in
 * NVS is re-enterable at the console — losing it costs one wifi_set, not a
 * brick. The alternative, refusing to boot, would leave a device whose only
 * repair path is the console it just declined to start. */
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "PandaDeath starting on Knomi V1");

    /* Before the display, because it owns no hardware the display wants and
     * because the console it feeds is the only way to repair a board whose
     * credentials are wrong. A UI failure should not take the repair path with
     * it. */
    ESP_ERROR_CHECK(nvs_init());
    /* Commands are registered before the REPL starts, so the prompt never
     * accepts one that does not exist yet. */
    ESP_ERROR_CHECK(wifi_cmd_register());
    ESP_ERROR_CHECK(time_cmd_register());
    ESP_ERROR_CHECK(ota_cmd_register());
    ESP_ERROR_CHECK(console_init());

    /* Init failure stays fatal: there is no useful fallback for a device whose
     * only output is the screen. display_init() leaves the backlight at 0, so
     * nothing is shown until there is something worth showing. */
    ESP_ERROR_CHECK(display_init());

#if BOOT_MODE == BOOT_MODE_SELFTEST
    ESP_ERROR_CHECK(display_fade_backlight(100, BACKLIGHT_FADE_MS));
    run_selftest();
#else
    ESP_ERROR_CHECK(ui_init());

    /* Fade up only once LVGL has rendered, so the ramp reveals the UI instead
     * of a black screen. The LEDC fader runs in hardware and returns
     * immediately, costing no CPU and no boot latency. */
    ESP_ERROR_CHECK(display_fade_backlight(100, BACKLIGHT_FADE_MS));

    /* Wi-Fi last, and only in the LVGL modes: the radio is the slowest thing
     * here and the only one that can fail for reasons outside the board, so
     * everything that can be shown is on the glass before it starts. The
     * self-test path skips it entirely — it exists to exercise the panel with
     * as little else running as possible.
     *
     * net_init() first: everything below registers an event handler or creates
     * an interface, and both need the netif layer and the default event loop to
     * exist. It is nobody's module in particular, which is why it is not
     * inside one.
     *
     * The three after it are order-independent by construction — each handles
     * an address that already exists rather than only the event announcing one
     * — so this reads top to bottom without a rule about which comes first. */
    ESP_ERROR_CHECK(net_init());

    /* Before wifi_sta_start(), and this one is not merely tidy: a newly
     * installed image is on probation until something confirms it, and this is
     * what arms the confirmation. */
    ESP_ERROR_CHECK(ota_start());
    ESP_ERROR_CHECK(time_sync_start());
    ESP_ERROR_CHECK(wifi_sta_start());
#endif

    /* LVGL runs in its own task; nothing further is needed here. */
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
