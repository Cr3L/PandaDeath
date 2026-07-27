#include "ui_test.h"

#include <stdint.h>

#include "lvgl.h"
#include "ui_palette.h"

/* Screen palette. Kept together so a retheme is one edit rather than a hunt
 * through widget setup. The two the storm screen also uses live in
 * ui_palette.h, which is what keeps them equal. */
#define COLOR_ACCENT     UI_COLOR_ACCENT
#define COLOR_TRACK      UI_COLOR_TRACK
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

void ui_test_screen_build(void)
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
