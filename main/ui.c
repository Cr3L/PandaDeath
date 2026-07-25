#include "ui.h"

#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui";

/* Rows held in each draw buffer. LVGL renders a slice at a time and flushes it,
 * so this trades RAM against the number of SPI transactions per frame; it does
 * not have to cover the screen. A full 240x240 frame would be 115 kB, which is
 * a lot to reserve on a board whose DRAM is already shared with Wi-Fi later. */
#define LVGL_BUFFER_ROWS 40

static lv_obj_t *s_arc;
static lv_obj_t *s_value_label;

/* Sweeps the arc back and forth. The value is also written into the centre
 * label, so the two update together and a stalled UI is obvious. */
static void arc_anim_cb(void *obj, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)obj, value);
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
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x203040), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x00C8FF), LV_PART_INDICATOR);

    s_value_label = lv_label_create(scr);
    lv_label_set_text(s_value_label, "0");
    lv_obj_set_style_text_color(s_value_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PandaDeath");
    lv_obj_set_style_text_color(title, lv_color_hex(0x8090A0), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 25);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_arc);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_duration(&a, 2500);
    lv_anim_set_playback_duration(&a, 2500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_start(&a);
}

esp_err_t ui_init(void)
{
    ESP_RETURN_ON_FALSE(display_panel_handle() != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "display not initialised");

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = display_io_handle(),
        .panel_handle = display_panel_handle(),
        .buffer_size  = DISPLAY_WIDTH * LVGL_BUFFER_ROWS,
        .double_buffer = true,
        .hres         = DISPLAY_WIDTH,
        .vres         = DISPLAY_HEIGHT,
        .monochrome   = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* The panel's native scan order is a reflection, not a rotation, so a
         * single axis is mirrored here. Mirroring both instead yields a 180
         * degree rotation, which leaves text upright but reversed. */
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma   = true,
            /* The panel takes pixels big-endian, matching the byte swap the
             * display driver does for its own fills. */
            .swap_bytes = true,
        },
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");

    /* LVGL is not thread safe and now has its own task. Every call into it from
     * elsewhere, including this one, has to hold the port lock. */
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "could not take lvgl lock");
        return ESP_ERR_TIMEOUT;
    }
    build_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "lvgl running (%dx%d, %d-row buffers)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, LVGL_BUFFER_ROWS);
    return ESP_OK;
}
