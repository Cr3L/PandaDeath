#include "display.h"

#include <endian.h>   /* htobe16 */
#include <string.h>
#include <sys/param.h>  /* MIN */

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
 *
 * Inversion is on. BTT's TFT_eSPI setup drives this same panel with inversion
 * off, but esp_lcd's GC9A01 driver does not share that baseline: starting from
 * BTT's value produced an exact colour complement on screen (black rendered
 * white, green rendered magenta).
 *
 * Mirroring lives in display.h, because LVGL needs to read it too. */
#define PANEL_BGR_ORDER true
#define PANEL_INVERT    true

#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ  5000
#define BL_DUTY_MAX      ((1 << 10) - 1)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

/* Scratch buffer for fills, one chunk of DISPLAY_DRAW_ROWS rows: 240 px * 20
 * rows * 2 B = 9.6 kB, a comfortable DRAM allocation where a whole 240x240
 * frame would be 115 kB and is not worth reserving for solid fills.
 *
 * Allocated on first use and reused, so an animation does not put a malloc/free
 * pair in every frame, but not held when nothing is filling: once LVGL owns the
 * panel these helpers go idle, and DMA-capable internal DRAM is the same scarce
 * pool Wi-Fi draws from. Not re-entrant: fills are expected from a single task. */
static uint16_t *s_fill_buf;

/* Blocks until nothing the panel IO has queued is still reading s_fill_buf.
 *
 * esp_lcd_panel_draw_bitmap does not wait for the wire: it hands the pixels to
 * spi_device_queue_trans and returns with the last chunk still in flight. The
 * driver only reclaims those descriptors when the next *command* goes out, so
 * within one fill each chunk's CASET drains the chunk before it and reuse is
 * safe. Between fills it is not — display_fill_rect rewrites the buffer before
 * issuing any command, so without this the new colour can overwrite bytes the
 * previous fill's tail is still transmitting, painting a band of the wrong
 * colour at the end of a shape.
 *
 * A -1 command is the documented "no command needed" form. It is used here for
 * its side effect only: tx_param drains the queue before it inspects lcd_cmd,
 * so this reclaims every descriptor and puts nothing on the wire. */
static esp_err_t fill_buf_wait_idle(void)
{
    return esp_lcd_panel_io_tx_param(s_io, -1, NULL, 0);
}

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

    /* Enables the hardware fader, so ramps cost one call and no CPU rather than
     * a loop of duty writes with delays between them. */
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "backlight fader");

    return ESP_OK;
}

static uint32_t backlight_duty(uint8_t percent)
{
    return (uint32_t)MIN(percent, 100) * BL_DUTY_MAX / 100;
}

esp_err_t display_set_backlight(uint8_t percent)
{
    /* The driver's combined form, not set_duty + update_duty: ledc.h documents
     * that pair as not thread safe and points here instead. */
    return ledc_set_duty_and_update(BL_LEDC_MODE, BL_LEDC_CHANNEL,
                                    backlight_duty(percent), 0);
}

esp_err_t display_fade_backlight(uint8_t percent, uint32_t ms)
{
    ESP_RETURN_ON_ERROR(
        ledc_set_fade_with_time(BL_LEDC_MODE, BL_LEDC_CHANNEL,
                                backlight_duty(percent), (int)ms),
        TAG, "set fade");
    return ledc_fade_start(BL_LEDC_MODE, BL_LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_FALSE(s_panel == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already initialised");

    ESP_LOGI(TAG, "SPI bus: mosi=%d sclk=%d @ %d MHz",
             PIN_MOSI, PIN_SCLK, LCD_PIXEL_CLOCK_HZ / 1000000);

    /* Must cover the largest single transfer any consumer will push, which is
     * LVGL's draw buffer, not this module's fill chunks — LVGL's is the larger
     * of the two and sizing to the fill buffer would silently truncate it. */
    const spi_bus_config_t bus =
        GC9A01_PANEL_BUS_SPI_CONFIG(PIN_SCLK, PIN_MOSI, DISPLAY_MAX_TRANSFER_SZ);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize");

    ESP_LOGI(TAG, "panel IO: cs=%d dc=%d", PIN_CS, PIN_DC);

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
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
        TAG, "new_panel_io_spi");

    ESP_LOGI(TAG, "GC9A01 panel: rst=%d bgr=%d invert=%d",
             PIN_RST, PANEL_BGR_ORDER, PANEL_INVERT);

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = PANEL_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR
                                          : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(s_io, &panel_cfg, &s_panel),
                        TAG, "new_panel_gc9a01");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, PANEL_INVERT),
                        TAG, "panel_invert");

    /* Orientation belongs here with the other panel facts, not in the graphics
     * layer. Setting it via LVGL's port config would work, but LVGL applies it
     * by mirroring this same panel behind our back, which leaves the self-test
     * path running an orientation the product never ships with, and silently
     * changes what display_fill_rect's y axis means partway through boot. */
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_mirror(s_panel, PANEL_MIRROR_X, PANEL_MIRROR_Y),
        TAG, "panel_mirror");

    /* Clear before switching on, so the first thing shown is black rather than
     * whatever noise the panel RAM powered up holding. */
    ESP_RETURN_ON_ERROR(display_fill(COLOR_BLACK), TAG, "initial clear");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");

    /* In the normal build that clear is the only fill there will ever be —
     * LVGL takes the panel from here and brings its own buffers. Hand the DMA
     * memory back rather than holding 9.6 kB for the life of the process; the
     * self-test path reallocates it on its next fill.
     *
     * Drain first: freeing a buffer the SPI driver is still transmitting from
     * is a use-after-free that the disp_on_off above happens to prevent today,
     * purely because sending a command reclaims queued descriptors. Relying on
     * that means these two lines cannot be reordered without silent corruption,
     * so state the dependency instead of resting on it. */
    ESP_RETURN_ON_ERROR(fill_buf_wait_idle(), TAG, "drain before free");
    heap_caps_free(s_fill_buf);
    s_fill_buf = NULL;

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight_init");

    ESP_LOGI(TAG, "display ready (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_panel_handle(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t display_io_handle(void)
{
    return s_io;
}

esp_err_t display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");

    /* Clip to the panel. Callers doing animation are allowed to walk a shape
     * off the edge; that should dim the shape, not fail the call.
     *
     * Written to never form `x + w` or `y + h`. Those overflow for large
     * inputs, and the wrapped result compares as negative, so the clamp does
     * not fire and an unbounded w reaches the buffer fill below as an
     * arbitrary-length write into DMA memory. Not reachable from today's
     * callers, which pass only constants, but the class disappears entirely by
     * subtracting from the bound instead of adding to the origin.
     *
     * `w += x` below cannot overflow: w is already known positive and x
     * negative, so the sum lies between them. */
    if (w <= 0 || h <= 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
        return ESP_OK;
    }
    if (x < 0) {
        w += x;
        if (w <= 0) {
            return ESP_OK;
        }
        x = 0;
    }
    if (y < 0) {
        h += y;
        if (h <= 0) {
            return ESP_OK;
        }
        y = 0;
    }
    if (w > DISPLAY_WIDTH - x)  { w = DISPLAY_WIDTH - x; }
    if (h > DISPLAY_HEIGHT - y) { h = DISPLAY_HEIGHT - y; }

    if (s_fill_buf == NULL) {
        /* DMA-capable memory is required: the SPI driver cannot transfer from
         * the stack, and on PSRAM-equipped modules the general heap may not be
         * DMA reachable. */
        s_fill_buf = heap_caps_malloc(DISPLAY_DRAW_BUF_PX * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_fill_buf != NULL, ESP_ERR_NO_MEM, TAG,
                            "fill buffer (%d px)", DISPLAY_DRAW_BUF_PX);
    } else {
        /* Buffer survives from an earlier fill, so an earlier fill's tail may
         * still be reading it. Nothing to wait for on the freshly allocated
         * path. */
        ESP_RETURN_ON_ERROR(fill_buf_wait_idle(), TAG, "drain before refill");
    }

    /* Only the first chunk's worth needs filling; every band re-sends it. */
    const size_t buf_px = (size_t)w * MIN(h, DISPLAY_DRAW_ROWS);

    /* The clipping above already guarantees this. Checked anyway because the
     * cost is one comparison and the failure mode it guards is a silent write
     * past the end of a DMA buffer. */
    ESP_RETURN_ON_FALSE(buf_px <= DISPLAY_DRAW_BUF_PX, ESP_ERR_INVALID_SIZE, TAG,
                        "fill %dx%d exceeds buffer", w, h);
    const uint16_t swapped = htobe16(color);
    for (size_t i = 0; i < buf_px; i++) {
        s_fill_buf[i] = swapped;
    }

    for (int row = y; row < y + h; row += DISPLAY_DRAW_ROWS) {
        const int rows = MIN(DISPLAY_DRAW_ROWS, y + h - row);
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
