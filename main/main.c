#include "boot_mode.h"
#include "console.h"
#include "display.h"
#include "selftest.h"
#include "ui.h"
#include "wifi_cmd.h"

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
#endif

    /* LVGL runs in its own task; nothing further is needed here. */
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
