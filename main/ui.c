#include "ui.h"

#include <stdint.h>

#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui";

/* Rows held in each draw buffer. LVGL renders a slice at a time and flushes it,
 * so this trades RAM against the number of SPI transactions per frame; it does
 * not have to cover the screen.
 *
 * 20 rows double-buffered is 9.6 kB each, 19.2 kB total, of DMA-capable
 * internal DRAM. Doubling to 40 rows halves the flush count for a full-screen
 * refresh (12 -> 6) but each extra flush costs only tens of microseconds
 * against ~23 ms of SPI wire time, so it buys under 1% for another 19 kB —
 * a bad trade on a board that still has to fit Wi-Fi.
 *
 * These stay in internal DRAM even once PSRAM is enabled: LVGL's software
 * renderer does per-pixel read-modify-write here, and WROVER PSRAM is roughly
 * an order of magnitude slower, which would push render time past flush time.
 *
 * The row count and the resulting pixel count both come from display.h, so the
 * SPI bus transfer limit is sized from the same numbers rather than from a
 * second expression that has to agree with them by eye. */

/* Screen palette. Kept together so a retheme is one edit rather than a hunt
 * through widget setup. */
#define COLOR_ACCENT     0x00E676  /* arc indicator */
#define COLOR_TRACK      0x1A2E22  /* arc groove, a dark cast of the accent */
#define COLOR_SUBTITLE   0x7E9488

/* Arc sweep. The value range is also what the centre label counts through, so
 * the two cannot drift apart. ARC_STEP_MS is how long one integer value is held
 * — 600 ms, a hundredth of a minute. */
#define ARC_VALUE_MIN 0
#define ARC_VALUE_MAX 100
#define ARC_STEP_MS   600

static lv_obj_t *s_arc;
static lv_obj_t *s_value_label;

/* Sweeps the arc back and forth. The value is also written into the centre
 * label, so the two update together and a stalled UI is obvious. */
static void arc_anim_cb(void *obj, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)obj, value);

    /* The exec callback fires every refresh, but the animation only crosses an
     * integer boundary a fraction of those times. Reformatting an unchanged
     * string would reallocate the label text out of the 16 kB LVGL pool and
     * invalidate the centre of the screen for a repaint of identical pixels. */
    static int32_t last_shown = INT32_MIN;
    if (value == last_shown) {
        return;
    }
    last_shown = value;
    lv_label_set_text_fmt(s_value_label, "%d", (int)value);
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Ring around the rim. Inset far enough that the round bezel does not clip
     * it, and knob removed since nothing is interactive yet. */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 220, 220);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, ARC_VALUE_MIN, ARC_VALUE_MAX);
    lv_arc_set_value(s_arc, ARC_VALUE_MIN);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);

    s_value_label = lv_label_create(scr);
    lv_label_set_text(s_value_label, "0");
    lv_obj_set_style_text_color(s_value_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PandaDeath");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_SUBTITLE), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 25);

    /* One step per 600 ms — a hundredth of a minute — so the centre value is
     * readable as it changes rather than blurring past. The animation is
     * specified by total duration, so that is the step time times the number of
     * steps in the range, kept as that arithmetic instead of a bare 60000 to
     * survive a change to either end of the range. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_arc);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_duration(&a, ARC_STEP_MS * (ARC_VALUE_MAX - ARC_VALUE_MIN));
    lv_anim_set_playback_duration(&a,
                                  ARC_STEP_MS * (ARC_VALUE_MAX - ARC_VALUE_MIN));
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&a, ARC_VALUE_MIN, ARC_VALUE_MAX);
    lv_anim_start(&a);
}

esp_err_t ui_init(void)
{
    ESP_RETURN_ON_FALSE(display_panel_handle() != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "display not initialised");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* The default 5 ms tick wakes the LVGL task 200x/s, but LV_DEF_REFR_PERIOD
     * caps output at ~30 fps, so most of those wakeups walk the timer list and
     * go straight back to blocked. Animations interpolate on elapsed
     * milliseconds, not tick count, so a coarser tick costs no smoothness. */
    port_cfg.timer_period_ms = 10;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = display_io_handle(),
        .panel_handle = display_panel_handle(),
        .buffer_size  = DISPLAY_DRAW_BUF_PX,
        .double_buffer = true,
        .hres         = DISPLAY_WIDTH,
        .vres         = DISPLAY_HEIGHT,
        .monochrome   = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* Same constants display_init() applies. esp_lvgl_port mirrors the
         * panel itself when it attaches, so leaving these zeroed would silently
         * undo the driver's orientation rather than inherit it. */
        .rotation = {
            .swap_xy  = false,
            .mirror_x = PANEL_MIRROR_X,
            .mirror_y = PANEL_MIRROR_Y,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = PANEL_SWAP_BYTES,
        },
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");

    /* LVGL is not thread safe and now has its own task. Every call into it from
     * elsewhere, including this one, has to hold the port lock. */
    /* A 0 timeout means wait forever in esp_lvgl_port, which would make the
     * failure branch unreachable and hang boot on a stuck lock. Bounded
     * instead: nothing else holds this yet, so a second is already generous. */
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "could not take lvgl lock");
        return ESP_ERR_TIMEOUT;
    }
    build_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "lvgl running (%dx%d, %d-row buffers)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_DRAW_ROWS);
    return ESP_OK;
}
