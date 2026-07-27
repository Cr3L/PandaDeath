#include "ui_storm.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "sun.h"
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
/* Pushed further from yellow after judging both on the glass: at 0xFF8A00 the
 * two were tellable apart but not at a glance, which is the only way this gets
 * read. All three storm colours hold red at full and separate on green —
 * 196, 109, 23 — so the progression is one channel walking down, and each step
 * is large enough to survive the panel and the room. */
#define COLOR_LIKELY   0xFF6D00  /* deep orange: storms mentioned, high probability */
#define COLOR_NOW      0xFF1744  /* crimson: it is happening in this period */
#define COLOR_TRACK    UI_COLOR_TRACK
/* Secondary text: the clock above the headline and the readings below it. One
 * name, because they are one role — a second constant holding the same value
 * was the start of exactly the drift ui_palette.h exists to prevent. */
#define COLOR_DIM      0x7E9488

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

/* Text widths. The panel is round, so a label must fit the chord at the height
 * of its *lowest line*, not at its own origin — the first version sized the
 * fact from the chord at its centre and the third line, 96 px down where the
 * circle is only 144 px across, ran off the glass. Caught in a photograph,
 * which is the one thing a still frame is reliable for. */
#define TEXT_WIDTH 170

/* The readings sit lower, where the circle has narrowed, and are two lines
 * rather than three. The fact that used to live here was sized from the chord
 * at its centre and ran off the glass on its third line, 96 px down where the
 * panel is only 144 px across. The lower of these two lines ends around 59 px,
 * where the chord is still 209 px — comfortable, and confirmed on the glass. */
#define READING_WIDTH 150

/* Vertical layout, as offsets from the centre.
 *
 * Gathered here rather than spread through the build function because they are
 * one decision — the spacing between them is the design, and a value that only
 * makes sense next to its neighbours should be readable next to its neighbours.
 *
 * The watch line is the reason these are not simply constants applied once. It
 * is blank on an ordinary day, and holding a line open for it left a visible
 * dead band under the temperature that never filled: the screen looked like it
 * had lost something. It now takes its space only when it has something to say,
 * and everything below it moves down by ALERT_SHIFT when it does. */
#define Y_CLOCK   -70
#define Y_TEMP    -34
#define Y_ALERT     4
#define Y_ETA       4
#define Y_READ_TOP 32
#define Y_READ_BOT 50
#define ALERT_SHIFT 22

static lv_obj_t *s_arc;
static lv_obj_t *s_clock;
static lv_obj_t *s_temp;
static lv_obj_t *s_alert;   /* the watch line, in the middle; usually hidden */
static lv_obj_t *s_eta;

/* Live readings from the nearest station, two lines under the headline. These
 * replaced eight rotating thunderstorm facts, which were charming for about a
 * week and then were furniture — a fixed list on a device that is on all day
 * gets memorised, and memorised text stops being read. These change on their
 * own, which is the only way a glanceable screen stays worth glancing at. */
static lv_obj_t *s_reading_top;
static lv_obj_t *s_reading_bottom;

/* Shown alone when a warning is active: everything else is hidden behind it.
 * A warning means the weather is happening and near, and a screen that puts
 * that beside a fact about lightning temperature has misjudged the moment. */
static lv_obj_t *s_takeover;

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

/* Warnings take the screen; watches and advisories take a line.
 *
 * The split is the issuing forecaster's, not one invented here: a Warning means
 * it is happening, a Watch means conditions are favourable hours out. Taking
 * over for a watch would make takeover common, and a takeover that happens
 * often is one that gets ignored. */
static bool alert_takes_over(const weather_alert_t *a)
{
    return a->level == WEATHER_ALERT_WARNING && a->severe;
}

/* Writes a label only when its text has actually changed.
 *
 * lv_label_set_text frees and reallocates the string and invalidates the
 * label's area, so rewriting identical text drags a redraw over SPI for
 * nothing. The existing clock and temperature guard this way with their own
 * static; the readings are two more of the same, so it is a function now
 * rather than a third and fourth copy of the pattern. */
static void set_text_if_changed(lv_obj_t *label, char *last, size_t last_len,
                                const char *text)
{
    if (strcmp(last, text) != 0) {
        strlcpy(last, text, last_len);
        lv_label_set_text(label, text);
    }
}

/* The live readings, in two lines under the storm line.
 *
 * What earns a place is what changes and what a storm changes first. Dewpoint
 * is the storm fuel and the honest humidity figure — 58% means nothing without
 * a temperature beside it, a 68° dewpoint means "muggy" to anyone. Pressure
 * with its trend is the classic, and the only number here the board works out
 * for itself. Gusts displace the steady wind whenever the station reports them,
 * because a gust front arriving ahead of a storm is exactly the moment this
 * screen is worth looking at, and on an ordinary day there is no gust to show.
 *
 * Anything the station did not report is left out rather than shown as a dash.
 * The console table has a fixed shape so a gap is visibly a gap; the glass has
 * eleven characters a line and should spend them on what is known. */
static void update_readings(const weather_obs_t *obs, time_t now)
{
    /* Generously sized for about twenty characters of content. The compiler
     * cannot bound what snprintf writes through these and treats a tight buffer
     * as a truncation it must warn about, which -Werror turns into a build
     * failure; headroom is cheaper than suppressing the diagnostic. */
    char top[64] = "";
    char bottom[64] = "";

    if (obs->observed != 0) {
        char dew[24] = "";
        char pressure[32] = "";

        if (obs->dewpoint != WEATHER_UNKNOWN) {
            snprintf(dew, sizeof(dew), "dew %d°", obs->dewpoint);
        }
        if (obs->pressure != WEATHER_UNKNOWN) {
            /* An arrow, not a number. The figure is three characters this
             * screen does not have, and the direction is the whole of what a
             * falling barometer means. LVGL's symbol font carries these; a
             * Unicode arrow would render as an empty box, the same way a middle
             * dot did on the storm line. */
            const char *arrow = "";
            if (obs->pressure_trend != WEATHER_UNKNOWN) {
                if (obs->pressure_trend <= -5) {
                    arrow = " " LV_SYMBOL_DOWN;
                } else if (obs->pressure_trend >= 5) {
                    arrow = " " LV_SYMBOL_UP;
                }
            }
            snprintf(pressure, sizeof(pressure), "%d mb%s", obs->pressure, arrow);
        }
        snprintf(top, sizeof(top), "%s%s%s", dew,
                 (dew[0] != '\0' && pressure[0] != '\0') ? "   " : "", pressure);

        if (obs->gust != WEATHER_UNKNOWN) {
            snprintf(bottom, sizeof(bottom), "gusts %d", obs->gust);
        } else if (obs->wind != WEATHER_UNKNOWN) {
            snprintf(bottom, sizeof(bottom), "wind %d", obs->wind);
        }
    }

    /* The next sun event, whichever it is. Showing both would spend a line on
     * the one that has already happened. Needs no network, so it is the last
     * thing left on the screen when everything else has failed. */
    double lat, lon;
    if (time_sync_synced() && weather_location_get(&lat, &lon) == ESP_OK) {
        time_t rise, set;
        if (sun_times(lat, lon, now, &rise, &set) == ESP_OK) {
            const bool before_rise = now < rise;
            const time_t next = before_rise ? rise : set;
            struct tm local;
            const time_t rounded = next + 30;   /* strftime truncates */
            localtime_r(&rounded, &local);
            char when[12];
            strftime(when, sizeof(when), "%l:%M", &local);
            const char *trimmed = when;
            while (*trimmed == ' ') {
                trimmed++;
            }
            char sun[32];
            snprintf(sun, sizeof(sun), "%s %s", before_rise ? "rise" : "set", trimmed);
            if (bottom[0] != '\0') {
                strlcat(bottom, "   ", sizeof(bottom));
            }
            strlcat(bottom, sun, sizeof(bottom));
        }
    }

    static char last_top[64];
    static char last_bottom[64];
    set_text_if_changed(s_reading_top, last_top, sizeof(last_top), top);
    set_text_if_changed(s_reading_bottom, last_bottom, sizeof(last_bottom), bottom);
}

/* Moves everything below the watch line down when it appears, and back when it
 * goes. Only on a change: lv_obj_align invalidates both the old area and the
 * new one, and an alert lasts hours while this runs every ten seconds. */
static void set_alert_room(bool alert_visible)
{
    static int applied = -1;   /* neither state, so the first call always runs */
    const int wanted = alert_visible ? 1 : 0;
    if (applied == wanted) {
        return;
    }
    applied = wanted;

    const int shift = alert_visible ? ALERT_SHIFT : 0;
    lv_obj_align(s_eta, LV_ALIGN_CENTER, 0, Y_ETA + shift);
    lv_obj_align(s_reading_top, LV_ALIGN_CENTER, 0, Y_READ_TOP + shift);
    lv_obj_align(s_reading_bottom, LV_ALIGN_CENTER, 0, Y_READ_BOT + shift);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    weather_alert_t alert;
    weather_alert_copy(&alert);

    const bool takeover = alert_takes_over(&alert);
    lv_obj_t *const hidden[] = { s_arc, s_clock, s_temp, s_alert, s_eta,
                                 s_reading_top, s_reading_bottom };
    for (size_t i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++) {
        if (takeover) {
            lv_obj_add_flag(hidden[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(hidden[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (takeover) {
        lv_obj_remove_flag(s_takeover, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_takeover, alert.event);
        return;  /* nothing below is visible; do not spend the redraw */
    }
    lv_obj_add_flag(s_takeover, LV_OBJ_FLAG_HIDDEN);

    /* Watches and advisories sit where the conditions line used to, which is
     * blank on an ordinary day — so anything appearing there means something. */
    set_alert_room(alert.level != WEATHER_ALERT_NONE);
    if (alert.level == WEATHER_ALERT_NONE) {
        lv_label_set_text(s_alert, "");
    } else {
        lv_label_set_text(s_alert, alert.event);
        lv_obj_set_style_text_color(s_alert,
                                    lv_color_hex(alert.level == WEATHER_ALERT_WATCH
                                                 ? COLOR_LIKELY : COLOR_DIM),
                                    LV_PART_MAIN);
    }

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

    weather_obs_t obs;
    weather_obs_copy(&obs);

    /* Above the no-forecast return, because they do not depend on one. The
     * instruments, the clock and the sun can all be right while the forecast
     * fetch is still failing, and a screen that blanks them because of that is
     * throwing away what it knows. */
    update_readings(&obs, now);

    /* The measured temperature, not the forecast one.
     *
     * These are different numbers and routinely disagree by several degrees:
     * weather_report_t.temperature is what the forecast office expects for this
     * half of the day, while the observation is what an instrument recorded a
     * few miles away twenty minutes ago. The headline figure on a device
     * sitting in the room should be the second. The forecast temperature stays
     * in the report for the storm line to reason about. */
    static int last_temp = INT_MIN;
    const int shown = (obs.observed != 0 && obs.temperature != WEATHER_UNKNOWN)
                      ? obs.temperature : INT_MIN;
    if (shown != last_temp) {
        last_temp = shown;
        if (shown == INT_MIN) {
            lv_label_set_text(s_temp, "--");
        } else {
            /* Always Fahrenheit: the observation is converted at parse time, so
             * unlike the forecast there is no unit travelling with it. */
            lv_label_set_text_fmt(s_temp, "%d°F", shown);
        }
    }

    if (r.fetched == 0) {
        lv_label_set_text(s_eta, "waiting for a forecast");
        lv_arc_set_value(s_arc, 0);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(COLOR_TRACK), LV_PART_INDICATOR);
        return;
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
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, Y_CLOCK);

    s_temp = lv_label_create(scr);
    lv_label_set_text(s_temp, "--");
    lv_obj_set_style_text_color(s_temp, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_temp, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_temp, LV_ALIGN_CENTER, 0, Y_TEMP);

    s_alert = lv_label_create(scr);
    lv_label_set_long_mode(s_alert, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_alert, TEXT_WIDTH);
    lv_obj_set_style_text_align(s_alert, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_alert, "");
    lv_obj_set_style_text_color(s_alert, lv_color_hex(COLOR_LIKELY), LV_PART_MAIN);
    lv_obj_align(s_alert, LV_ALIGN_CENTER, 0, Y_ALERT);

    s_takeover = lv_label_create(scr);
    lv_label_set_long_mode(s_takeover, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_takeover, TEXT_WIDTH);
    lv_obj_set_style_text_align(s_takeover, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_takeover, "");
    lv_obj_set_style_text_color(s_takeover, lv_color_hex(COLOR_NOW), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_takeover, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(s_takeover);
    lv_obj_add_flag(s_takeover, LV_OBJ_FLAG_HIDDEN);

    s_eta = lv_label_create(scr);
    lv_label_set_long_mode(s_eta, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_eta, TEXT_WIDTH);
    lv_obj_set_style_text_align(s_eta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_eta, "");
    lv_obj_set_style_text_color(s_eta, lv_color_hex(COLOR_CALM), LV_PART_MAIN);
    lv_obj_align(s_eta, LV_ALIGN_CENTER, 0, Y_ETA);

    s_reading_top = lv_label_create(scr);
    lv_obj_set_width(s_reading_top, READING_WIDTH);
    lv_obj_set_style_text_align(s_reading_top, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_reading_top, "");
    lv_obj_set_style_text_color(s_reading_top, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
    lv_obj_align(s_reading_top, LV_ALIGN_CENTER, 0, Y_READ_TOP);

    s_reading_bottom = lv_label_create(scr);
    lv_obj_set_width(s_reading_bottom, READING_WIDTH);
    lv_obj_set_style_text_align(s_reading_bottom, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_reading_bottom, "");
    lv_obj_set_style_text_color(s_reading_bottom, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
    lv_obj_align(s_reading_bottom, LV_ALIGN_CENTER, 0, Y_READ_BOT);

    /* Fires immediately as well as on its period, so the screen is populated on
     * the first frame rather than showing placeholders for ten seconds. One
     * timer now: the readings used to have their own because they rotated on a
     * schedule of their own, and there is nothing left to rotate. */
    refresh_cb(NULL);
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}
