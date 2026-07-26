#include "ui_status.h"

#include <stdint.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"

/* Indicator palette. The accent matches ui_test.c's, which is the device's
 * green; the rest are chosen to read at a glance on a 240 px round panel from
 * across a room, where a shape this small is a colour before it is a symbol.
 *
 * Distinct hues rather than shades of one: at this size, on a panel whose black
 * leaks blue at full backlight, a brightness difference is not reliably visible
 * but a hue difference is.
 *
 * The first attempt at this got it wrong in a way worth recording, because the
 * values looked obviously fine written down. Trying was 0xE6A100 and fault was
 * 0xE64A19 — an amber and a red-orange sharing a red channel exactly, differing
 * only in green. That is a brightness difference wearing two names, and on a
 * ~20 px glyph it read as amber in both states. The fix is not a darker red but
 * a red that is not also orange, which is why fault is crimson and trying moved
 * toward yellow: the gap has to be opened from both ends. Judged by eye on the
 * hardware, which is the only instrument that settles this — a photo would have
 * blown both out to white. */
#define COLOR_CONNECTED  0x00E676  /* the device green */
#define COLOR_CONNECTING 0xFFC400  /* yellow: trying */
#define COLOR_IDLE       0x4A5A50  /* grey-green: nothing wrong, nothing to say */
#define COLOR_FAULT      0xFF1744  /* crimson: has credentials, cannot use them */

/* Inset from the top of the glass. The panel is round, so the top-centre pixel
 * is on the bezel; 16 px down is inside the visible circle with room for the
 * glyph's own height, and clear of a centred 192 px sprite. */
#define STATUS_TOP_PAD 16

static lv_obj_t *s_label;

/* The last status set, replayed by ui_status_build(). Wi-Fi may reach a state
 * before the screen exists — it does not, in the current boot order, but that
 * ordering is main.c's to change and this should not quietly break when it
 * does. */
static wifi_status_t s_status = WIFI_STATUS_NO_CREDENTIALS;

static uint32_t status_color(wifi_status_t status)
{
    switch (status) {
    case WIFI_STATUS_CONNECTED:    return COLOR_CONNECTED;
    case WIFI_STATUS_CONNECTING:   return COLOR_CONNECTING;
    case WIFI_STATUS_DISCONNECTED: return COLOR_FAULT;
    case WIFI_STATUS_NO_CREDENTIALS:
    default:                       return COLOR_IDLE;
    }
}

/* Assumes the LVGL lock is held. */
static void apply(void)
{
    if (s_label == NULL) {
        return;
    }
    lv_obj_set_style_text_color(s_label, lv_color_hex(status_color(s_status)),
                                LV_PART_MAIN);
    /* No credentials is not a Wi-Fi *fault*, so it does not get the Wi-Fi
     * glyph lit in a warning colour — it gets no glyph at all. An indicator
     * that is always showing something trains the eye to ignore it. */
    lv_obj_set_style_opa(s_label,
                         s_status == WIFI_STATUS_NO_CREDENTIALS ? LV_OPA_30 : LV_OPA_COVER,
                         LV_PART_MAIN);
}

void ui_status_build(void)
{
    s_label = lv_label_create(lv_screen_active());
    lv_label_set_text(s_label, LV_SYMBOL_WIFI);
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, STATUS_TOP_PAD);
    /* The zoo's sprite is centred and the indicator is not, so they do not
     * overlap today. Kept above anyway: a later screen that fills the glass
     * should hide the indicator deliberately, not by drawing over it. */
    lv_obj_move_foreground(s_label);
    apply();
}

void ui_status_set(wifi_status_t status)
{
    s_status = status;

    if (s_label == NULL) {
        return;  /* replayed by ui_status_build() */
    }

    /* Bounded, and a failure is dropped rather than retried: this is a status
     * glyph, and the next transition will paint it correctly anyway. Blocking
     * the event task on a stuck LVGL lock would stall every other handler,
     * including the ones that keep the connection alive. */
    if (!lvgl_port_lock(100)) {
        return;
    }
    apply();
    lvgl_port_unlock();
}
