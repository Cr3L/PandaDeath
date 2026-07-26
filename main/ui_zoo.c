#include "ui_zoo.h"

#include "esp_log.h"
#include "lvgl.h"
#include "zoo_sprites.h"

static const char *TAG = "zoo";

/* 4x, so a 48 px sprite lands as 192 px on a 240 px panel: big enough to be the
 * subject of the screen, with the remaining 24 px a side keeping it clear of
 * the round bezel. Integer scale matters more than the exact number — at 4x
 * every source pixel becomes an exact 4x4 block, so the art stays pixel art
 * instead of acquiring uneven edges where a fractional scale rounds one row
 * differently from the next. */
#define ZOO_SCALE (4 * LV_SCALE_NONE)

/* One walk cycle over 12 frames. 100 ms a frame is brisk enough to read as
 * walking rather than as a slideshow, and slow enough that the panel's ~30 fps
 * refresh has three refreshes to land each frame. */
#define ZOO_LOOP_MS (ZOO_FRAMES * 100)

/* Three loops each, so an animal reads as walking rather than as flashing past,
 * and a full tour of four animals takes about fifteen seconds. */
#define ZOO_ANIMAL_MS (ZOO_LOOP_MS * 3)

static lv_obj_t *s_animimg;

static void show_animal(uint32_t index)
{
    const zoo_animal_t *animal = &zoo_animals[index];

    /* The cast drops the const on the array *elements*: the table is
     * `const void *const *`, and lv_animimg_set_src wants `const void **`.
     * Sound because the function only reads — lv_animimage.c stores the
     * pointer and indexes it, and index_change() passes each element straight
     * to lv_image_set_src. That element const is not decoration: without it
     * the array is one of mutable pointers and the linker must put all 192
     * bytes in DRAM rather than flash. */
    lv_animimg_set_src(s_animimg, (const void **)animal->frames, ZOO_FRAMES);
    lv_animimg_set_duration(s_animimg, ZOO_LOOP_MS);
    lv_animimg_set_repeat_count(s_animimg, LV_ANIM_REPEAT_INFINITE);
    /* Restarts the animation from frame 0 rather than resuming wherever the
     * previous animal happened to be, so every animal enters on the same foot.
     * lv_anim_start replaces the running animation for this object, so this
     * does not stack a second one. */
    lv_animimg_start(s_animimg);

    /* DEBUG because it fires on a timer and the console shares this UART — see
     * the rule in CLAUDE.md's Conventions. Kept rather than deleted because it
     * is how the cycle timing was verified. Recovering it needs
     * CONFIG_LOG_MAXIMUM_LEVEL raised and a rebuild, not a runtime
     * esp_log_level_set: at the current level 3 this call compiles out. */
    ESP_LOGD(TAG, "%s", animal->name);
}

static void next_animal_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    /* Local rather than file-scope: nothing outside this callback advances the
     * tour, so nothing outside it needs to see where the tour has got to. */
    static uint32_t current;
    current = (current + 1) % ZOO_ANIMAL_COUNT;
    show_animal(current);
}

void ui_zoo_screen_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_animimg = lv_animimg_create(scr);
    lv_obj_center(s_animimg);

    /* Nearest-neighbour, not smoothed. Antialiasing a 4x upscale is exactly
     * wrong for pixel art: it blurs the hard edges the art is made of, and on
     * a four-to-nine colour sprite it invents intermediate colours that are not
     * in the palette. */
    lv_image_set_antialias(s_animimg, false);
    /* Scale about the sprite's own centre; the default pivot is the top-left,
     * which would throw a 4x sprite three-quarters of its size off-centre. */
    lv_image_set_pivot(s_animimg, ZOO_FRAME_W / 2, ZOO_FRAME_H / 2);
    lv_image_set_scale(s_animimg, ZOO_SCALE);

    show_animal(0);

    /* A timer rather than lv_animimg's completed callback, which would fire
     * inside the animation it then has to replace. The cost is that the
     * handover lands on a wall-clock boundary instead of exactly on a loop
     * boundary — harmless here because the period is a whole number of loops,
     * and the two clocks are the same LVGL tick. */
    lv_timer_create(next_animal_cb, ZOO_ANIMAL_MS, NULL);
}
