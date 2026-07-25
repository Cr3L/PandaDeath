#include "display.h"
#include "selftest.h"
#include "ui.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* Set to 1 to run the raw panel self-test instead of the LVGL UI. See
 * selftest.h for why it is kept. */
#define RUN_HARDWARE_SELFTEST 0

#define BACKLIGHT_FADE_MS 400

void app_main(void)
{
    ESP_LOGI(TAG, "PandaDeath starting on Knomi V1");

    /* Init failure stays fatal: there is no useful fallback for a device whose
     * only output is the screen. display_init() leaves the backlight at 0, so
     * nothing is shown until there is something worth showing. */
    ESP_ERROR_CHECK(display_init());

#if RUN_HARDWARE_SELFTEST
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
