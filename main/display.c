#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "display";

/* ---------------------------------------------------------------------------
 * Board wiring
 *
 * Taken from BigTreeTech's own KNOMI V1 firmware (src/pinout_knomi_v1.h), not
 * measured or guessed. The V1 board is the ESP32-WROVER-E variant; V2 is an
 * ESP32-S3 with a different map, so these numbers are V1-only.
 *
 * These happen to be the ESP32's native VSPI (SPI3) IOMUX pins for SCLK/MOSI/CS,
 * which is why SPI3_HOST is used below: routing through the GPIO matrix instead
 * would cap the usable clock well below what the panel can take.
 * ------------------------------------------------------------------------ */
#define PIN_MOSI 23
#define PIN_SCLK 18
#define PIN_CS    5
#define PIN_DC   19
#define PIN_RST   4
#define PIN_BL    2  /* also a boot strapping pin; only driven after startup */

#define LCD_HOST SPI3_HOST

/* BTT clocks this panel at 80 MHz. Bring-up starts at 40 MHz because a marginal
 * signal shows up as sparkle or a dead panel rather than a clean error, and
 * that is a miserable thing to debug while everything else is also unproven.
 * Raise once the display is known good. */
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

/* --- Panel quirks, confirmed on hardware ----------------------------------
 *   PANEL_BGR_ORDER  : red and blue swapped   -> flip this
 *   PANEL_INVERT     : photo-negative colours -> flip this
 * Inversion is on. BTT's TFT_eSPI setup drives this same panel with inversion
 * off, but esp_lcd's GC9A01 driver does not share that baseline: starting from
 * BTT's value produced an exact colour complement on screen (black rendered
 * white, green rendered magenta). */
#define PANEL_BGR_ORDER true
#define PANEL_INVERT    true

/* Rows painted per SPI transaction. 240 px * 20 rows * 2 B = 9.6 kB, which is a
 * comfortable DRAM allocation; a whole 240x240 frame would be 115 kB and is not
 * worth reserving for solid fills. */
#define FILL_CHUNK_ROWS 20

#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ  5000
#define BL_DUTY_MAX      ((1 << 10) - 1)

static esp_lcd_panel_handle_t s_panel;

/* Scratch buffer for fills, allocated once. Sized for the widest possible
 * chunk, so any rectangle up to full width reuses it. Allocating per call
 * instead would put a malloc/free pair inside every frame of an animation.
 * Not re-entrant: fills are expected from a single task. */
static uint16_t *s_fill_buf;
#define FILL_BUF_PX (DISPLAY_WIDTH * FILL_CHUNK_ROWS)

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");

    const ledc_channel_config_t channel = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .gpio_num   = PIN_BL,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");

    return ESP_OK;
}

esp_err_t display_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    const uint32_t duty = (uint32_t)percent * BL_DUTY_MAX / 100;

    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty),
                        TAG, "set duty");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_FALSE(s_panel == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    /* DMA-capable memory is required: the SPI driver cannot transfer from the
     * stack, and on PSRAM-equipped modules the general heap may not be DMA
     * reachable. */
    s_fill_buf = heap_caps_malloc(FILL_BUF_PX * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_fill_buf != NULL, ESP_ERR_NO_MEM, TAG,
                        "fill buffer (%d px)", FILL_BUF_PX);

    ESP_LOGI(TAG, "SPI bus: mosi=%d sclk=%d @ %d MHz",
             PIN_MOSI, PIN_SCLK, LCD_PIXEL_CLOCK_HZ / 1000000);

    const spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .sclk_io_num     = PIN_SCLK,
        .miso_io_num     = -1,  /* panel is write-only in this design */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_WIDTH * FILL_CHUNK_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize");

    ESP_LOGI(TAG, "panel IO: cs=%d dc=%d", PIN_CS, PIN_DC);

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = PIN_CS,
        .dc_gpio_num       = PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io),
        TAG, "new_panel_io_spi");

    ESP_LOGI(TAG, "GC9A01 panel: rst=%d bgr=%d invert=%d",
             PIN_RST, PANEL_BGR_ORDER, PANEL_INVERT);

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = PANEL_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR
                                          : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel),
                        TAG, "new_panel_gc9a01");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, PANEL_INVERT),
                        TAG, "panel_invert");

    /* Clear before switching on, so the first thing shown is black rather than
     * whatever noise the panel RAM powered up holding. */
    ESP_RETURN_ON_ERROR(display_fill(COLOR_BLACK), TAG, "initial clear");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight_init");

    ESP_LOGI(TAG, "display ready (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

esp_err_t display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");

    /* Clip to the panel. Callers doing animation are allowed to walk a shape
     * off the edge; that should dim the shape, not fail the call. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  { w = DISPLAY_WIDTH - x; }
    if (y + h > DISPLAY_HEIGHT) { h = DISPLAY_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    const int chunk_rows = (h < FILL_CHUNK_ROWS) ? h : FILL_CHUNK_ROWS;
    const size_t buf_px = (size_t)w * chunk_rows;

    const uint16_t swapped = RGB565_SWAP(color);
    for (size_t i = 0; i < buf_px; i++) {
        s_fill_buf[i] = swapped;
    }

    for (int row = y; row < y + h; row += chunk_rows) {
        const int rows = ((y + h) - row < chunk_rows) ? (y + h) - row : chunk_rows;
        const esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, row,
                                                       x + w, row + rows,
                                                       s_fill_buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap at row %d: %s", row, esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t display_fill(uint16_t color)
{
    return display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}
