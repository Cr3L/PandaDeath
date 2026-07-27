#include "ui_storm.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "time_sync.h"
#include "ui_palette.h"
#include "weather.h"

/* Screen palette. Distinct hues rather than shades of one: on a ~20 px arc
 * viewed across a room, on a panel whose black leaks blue at full backlight, a
 * brightness difference is not reliably visible but a hue difference is. The
 * project learned that the hard way on the old Wi-Fi glyph, where an amber and
 * a red-orange shared a red channel exactly and read as one colour. */
#define COLOR_CALM     UI_COLOR_ACCENT  /* the device green: nothing coming */
#define COLOR_POSSIBLE 0xFFC400  /* yellow: storms mentioned, low probability */
#define COLOR_LIKELY   0xFF8A00  /* orange: storms mentioned, high probability */
#define COLOR_NOW      0xFF1744  /* crimson: it is happening in this period */
#define COLOR_TRACK    UI_COLOR_TRACK
#define COLOR_DIM      0x7E9488  /* secondary text */
#define COLOR_FACT     0x5A6B60  /* quieter still: the fact is never the point */

/* The arc reads as "how close", so it needs a horizon to be a fraction of.
 * Two days: the forecast runs a week, but a storm on Thursday is not something
 * to look at a dial about on Monday, and compressing the whole week would leave
 * everything actionable bunched at one end. */
#define HORIZON_HOURS 48

/* How often the screen re-reads the report and the clock.
 *
 * The forecast would be happy with minutes — a new one lands every half hour,
 * and the ETA is printed in whole hours. The clock sets this instead: at 60 s
 * the displayed minute could be a full minute stale, which is visibly wrong on
 * a clock in a way a stale forecast is not.
 *
 * Ten seconds is affordable only because every label below is guarded against
 * rewriting identical text. Without those guards this would be the 5 s version
 * the review objected to, which freed and reallocated four label strings and
 * invalidated their areas to redraw the same pixels. */
#define REFRESH_MS 10000

/* Long enough to read one at a glance and not so long it becomes furniture. */
#define FACT_ROTATE_MS 12000

/* Thunderstorm facts, cycled underneath. This is a desk ornament, and a screen
 * that says "no storms" for three weeks has nothing to look at; the facts are
 * what make the quiet state worth glancing at.
 *
 * Kept under about 60 characters so they wrap to three lines at FACT_WIDTH.
 * The longest one here sets that limit, and the limit is the glass: a fourth
 * line reaches where the panel has already curved away. */
static const char *const FACTS[] = {
    "Lightning is five times hotter than the sun's surface.",
    "Thunder is air exploding away from the lightning.",
    "Seconds from flash to bang, divided by five, is miles.",
    "2,000 thunderstorms are running on Earth right now.",
    "One bolt can carry 30,000 amperes.",
    "Most lightning never reaches the ground.",
    "If you can hear thunder, you are close enough to be hit.",
    "A storm needs moisture, unstable air, and a lift.",
};
#define FACT_COUNT (sizeof(FACTS) / sizeof(FACTS[0]))

/* Text widths. The panel is round, so a label must fit the chord at the height
 * of its *lowest line*, not at its own origin — the first version sized the
 * fact from the chord at its centre and the third line, 96 px down where the
 * circle is only 144 px across, ran off the glass. Caught in a photograph,
 * which is the one thing a still frame is reliable for. */
#define TEXT_WIDTH 170
#define FACT_WIDTH 140

static lv_obj_t *s_arc;
static lv_obj_t *s_clock;
static lv_obj_t *s_temp;
static lv_obj_t *s_eta;
static lv_obj_t *s_fact;

static uint32_t storm_color(weather_storm_t storm)
{
    /* No default: an unhandled state should be a -Wswitch warning at build time,
     * not a silently calm-looking dial. */
    switch (storm) {
    case WEATHER_STORM_NONE:     return COLOR_CALM;
    case WEATHER_STORM_POSSIBLE: return COLOR_POSSIBLE;
    case WEATHER_STORM_LIKELY:   return COLOR_LIKELY;
    case WEATHER_STORM_NOW:      return COLOR_NOW;
    }
    return COLOR_CALM;
}

/* How much of the ring to fill: nearness scaled by probability.
 *
 * Nearness alone was the first version and it was wrong in the way that
 * matters. A 19% chance five hours out filled almost the whole ring, which from
 * across a room says "storm imminent" about something that will probably not
 * happen — and it looked identical to a certainty two days away. The question
 * a dial answers from the doorway is *should I care*, and time alone answers a
 * different one.
 *
 * Multiplying the two means the ring is dramatic only when something is both
 * close and likely. Colour still carries probability on its own, so a distant
 * near-certainty is a small amber mark rather than nothing at all. */
static int32_t arc_value(const weather_report_t *r, time_t now)
{
    if (r->storm == WEATHER_STORM_NONE || r->starts_at == 0) {
        return 0;
    }

    double nearness;
    if (r->storm == WEATHER_STORM_NOW) {
        nearness = 1.0;
    } else {
        const double hours = difftime(r->starts_at, now) / 3600.0;
        if (hours <= 0) {
            nearness = 1.0;  /* the period has begun since the last fetch */
        } else if (hours >= HORIZON_HOURS) {
            return 0;
        } else {
            nearness = 1.0 - (hours / HORIZON_HOURS);
        }
    }

    /* An unstated probability is treated as certain rather than as zero: the
     * service omits it, it does not deny it, and a forecast that says
     * "thunderstorms" without a number should not read as calm. */
    const double likelihood = (r->pop >= 0) ? (r->pop / 100.0) : 1.0;

    /* A floor, so that a real forecast is never an invisible ring. Below a few
     * percent of the arc the indicator is a rendering artefact rather than a
     * reading. */
    const int32_t value = (int32_t)(nearness * likelihood * 100.0);
    return value < 4 ? 4 : value;
}

/* "in 3 h" / "in 2 d 4 h". Hours alone would read "in 39 h" two days out, which
 * is arithmetic the reader should not have to do. */
static void format_eta(char *buf, size_t len, time_t starts_at, time_t now)
{
    const double seconds = difftime(starts_at, now);
    if (seconds <= 0) {
        snprintf(buf, len, "now");
        return;
    }

    const int hours = (int)(seconds / 3600.0);
    if (hours < 24) {
        snprintf(buf, len, "in %d h", hours);
    } else {
        snprintf(buf, len, "in %d d %d h", hours / 24, hours % 24);
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* A snapshot, not the live struct: the poll task can rewrite it at any
     * moment, and half of one report beside half of another is a forecast that
     * was never issued. */
    weather_report_t r;
    weather_report_copy(&r);

    const time_t now = time(NULL);

    /* The clock runs off the same tick as the forecast, and off the same guard.
     * It is shown small and without a date on purpose: this is a storm watch
     * that happens to know the time, not a clock. */
    char clock[16] = "";
    if (time_sync_synced()) {
        struct tm local;
        localtime_r(&now, &local);
        strftime(clock, sizeof(clock), "%l:%M %p", &local);
    }
    /* %l space-pads a single-digit hour, which centres the label off by half a
     * character against everything below it. */
    const char *trimmed = clock;
    while (*trimmed == ' ') {
        trimmed++;
    }
    static char last_clock[16];
    if (strcmp(last_clock, trimmed) != 0) {
        strlcpy(last_clock, trimmed, sizeof(last_clock));
        lv_label_set_text(s_clock, trimmed);
    }

    if (r.fetched == 0) {
        lv_label_set_text(s_temp, "--");
        lv_label_set_text(s_eta, "waiting for a forecast");
        lv_arc_set_value(s_arc, 0);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(COLOR_TRACK), LV_PART_INDICATOR);
        return;
    }

    /* Only when they change. lv_label_set_text frees and reallocates the label's
     * string every call and invalidates its area, so rewriting an identical
     * temperature drags a redraw over SPI for nothing. These move once per
     * poll at most, against a refresh that runs far more often than that.
     * ui_test.c's arc_anim_cb guards the same way for the same reason. */
    static int last_temp = INT_MIN;
    if (r.temperature != last_temp) {
        last_temp = r.temperature;
        lv_label_set_text_fmt(s_temp, "%d°%s", r.temperature, r.temperature_unit);
    }

    const uint32_t color = storm_color(r.storm);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_arc_set_value(s_arc, arc_value(&r, now));
    lv_obj_set_style_text_color(s_eta, lv_color_hex(color), LV_PART_MAIN);

    if (r.storm == WEATHER_STORM_NONE) {
        /* Said plainly rather than left blank. A blank line where a warning
         * would go is indistinguishable from a screen that has stopped
         * updating. */
        lv_label_set_text(s_eta, "no storms in 7 days");
    } else {
        char eta[24];
        format_eta(eta, sizeof(eta), r.starts_at, now);
        if (r.pop >= 0) {
            /* Plain ASCII. A middle dot was prettier and rendered as an empty
             * box: LVGL's Montserrat 14 carries the ASCII range and its own
             * symbol set, not Latin-1 punctuation. Nothing in the source says
             * so — it took a photograph of the glass. */
            lv_label_set_text_fmt(s_eta, "storm %s, %d%%", eta, r.pop);
        } else {
            lv_label_set_text_fmt(s_eta, "storm %s", eta);
        }
    }
}

static void fact_cb(lv_timer_t *timer)
{
    (void)timer;

    static size_t index;
    lv_label_set_text(s_fact, FACTS[index]);
    index = (index + 1) % FACT_COUNT;
}

void ui_storm_screen_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Ring around the rim, inset far enough that the round bezel does not clip
     * it. Knob removed and clicks disabled: it reports, it is not a control. */
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
    /* Indicator colour is not set here: refresh_cb below assigns it on every
     * path before the first frame is drawn, so a value here would be written
     * and overwritten without ever being seen. */
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);

    s_clock = lv_label_create(scr);
    lv_label_set_text(s_clock, "");
    lv_obj_set_style_text_color(s_clock, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -66);

    s_temp = lv_label_create(scr);
    lv_label_set_text(s_temp, "--");
    lv_obj_set_style_text_color(s_temp, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_temp, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_temp, LV_ALIGN_CENTER, 0, -32);

    s_eta = lv_label_create(scr);
    lv_label_set_long_mode(s_eta, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_eta, TEXT_WIDTH);
    lv_obj_set_style_text_align(s_eta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_eta, "");
    lv_obj_set_style_text_color(s_eta, lv_color_hex(COLOR_CALM), LV_PART_MAIN);
    lv_obj_align(s_eta, LV_ALIGN_CENTER, 0, 6);

    s_fact = lv_label_create(scr);
    lv_label_set_long_mode(s_fact, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_fact, FACT_WIDTH);
    lv_obj_set_style_text_align(s_fact, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_fact, lv_color_hex(COLOR_FACT), LV_PART_MAIN);
    lv_obj_align(s_fact, LV_ALIGN_CENTER, 0, 46);

    /* Both fire immediately as well as on their period, so the screen is
     * populated on the first frame rather than showing placeholders for the
     * length of one interval. */
    fact_cb(NULL);
    refresh_cb(NULL);
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    lv_timer_create(fact_cb, FACT_ROTATE_MS, NULL);
}
