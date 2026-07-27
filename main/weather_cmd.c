#include "weather_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "console.h"
#include "esp_console.h"
#include "sun.h"
#include "time_sync.h"
#include "weather.h"

/* Replies go through printf, not ESP_LOGI — same reasoning as wifi_cmd.c. */

/* One table, rather than a command per source.
 *
 * The forecast, the instruments and the sun were three commands answering one
 * question in three shapes, and reading them meant holding three layouts in
 * your head to compare numbers that belong side by side. They are folded here.
 *
 * Column widths live in these two constants and nowhere else. A table whose
 * widths are spelled out in a dozen format strings stays aligned exactly until
 * someone adds a row, and the misalignment then looks like corrupted output
 * rather than a missed edit. */
#define WX_LABEL 12
#define WX_VALUE 13

/* Cell text. The widest thing any cell actually holds is "falling fast -12.3",
 * around twenty characters; the rest is headroom the compiler insists on,
 * because it cannot bound a unit string passed in as a pointer and treats a
 * tight buffer as a truncation it must warn about. */
typedef char wx_cell_t[40];

static void wx_row(const char *label_a, const char *value_a,
                   const char *label_b, const char *value_b)
{
    printf("  %-*s %-*s %-*s %s\n", WX_LABEL, label_a, WX_VALUE, value_a,
           WX_LABEL, label_b, value_b);
}

/* A number with its unit, or a dash where the station reported nothing. Every
 * numeric field needs this: on a clear day the nearest test station sent four
 * nulls alongside everything else, and a zero in their place would be a reading
 * no instrument ever made. */
static void wx_number(wx_cell_t out, int value, const char *unit)
{
    if (value == WEATHER_UNKNOWN) {
        snprintf(out, sizeof(wx_cell_t), "--");
    } else {
        snprintf(out, sizeof(wx_cell_t), "%d %s", value, unit);
    }
}

/* Degrees the wind blows from, as the compass point anyone would say out loud.
 *
 * Sixteen points of 22.5 degrees, each name covering the arc *centred* on its
 * bearing rather than starting at it — so north runs from 348.75 round to
 * 11.25, which is why the half-point offset is there. In integers that is
 * (deg + 11.25) / 22.5 scaled by four to clear the fractions; the obvious form
 * without the scaling puts 0 degrees in NNE. */
static const char *compass(int degrees)
{
    static const char *const POINTS[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    const int index = ((degrees * 4 + 45) / 90) % 16;
    return POINTS[index < 0 ? 0 : index];
}

/* The trend in the words a barometer face has carried for two centuries, with
 * the figure behind them. Thresholds are the marine convention: half a
 * millibar over three hours is instrument noise, three is the fall that
 * precedes weather. */
static void wx_trend(wx_cell_t out, int trend)
{
    if (trend == WEATHER_UNKNOWN) {
        snprintf(out, sizeof(wx_cell_t), "(collecting)");
        return;
    }
    const int magnitude = trend < 0 ? -trend : trend;
    const char *word = magnitude < 5   ? "steady"
                     : magnitude >= 30 ? (trend < 0 ? "falling fast" : "rising fast")
                     :                   (trend < 0 ? "falling" : "rising");
    /* The sign is printed rather than left to %+d: a fall of three tenths has
     * an integer part of zero, which %+d renders as "+0.3" for a barometer
     * that is going down. */
    snprintf(out, sizeof(wx_cell_t), "%s %c%d.%d", word,
             trend < 0 ? '-' : '+', magnitude / 10, magnitude % 10);
}

/* Local wall-clock time of a UTC instant, rounded to the nearest minute.
 * strftime truncates, so a sunrise at 06:31:39 would print as 06:31 against
 * every published table saying 06:32 — a minute of error introduced at the last
 * step of arithmetic accurate to well under one. */
static void wx_clock(wx_cell_t out, time_t utc)
{
    const time_t rounded = utc + 30;
    struct tm local;
    localtime_r(&rounded, &local);
    strftime(out, sizeof(wx_cell_t), "%H:%M", &local);
}

static void wx_header(void)
{
    if (time_sync_synced()) {
        const time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M %Z", &local);
        printf("%-*s %s\n", WX_LABEL, "clock", stamp);
    } else {
        printf("%-*s not set\n", WX_LABEL, "clock");
    }

    double lat, lon;
    if (weather_location_get(&lat, &lon) == ESP_OK) {
        printf("%-*s %.4f, %.4f\n", WX_LABEL, "location", lat, lon);
    } else {
        printf("%-*s not set\n", WX_LABEL, "location");
    }
}

static int cmd_wx(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    weather_report_t report;
    weather_obs_t obs;
    weather_alert_t alert;
    weather_report_copy(&report);
    weather_obs_copy(&obs);
    weather_alert_copy(&alert);

    const time_t now = time(NULL);

    /* The alert first, above everything, because it is the one line here that
     * is urgent. It was once printed only once a forecast existed, which hid a
     * live warning behind the absence of the less important half. */
    if (alert.level != WEATHER_ALERT_NONE) {
        printf("** %s (%s%s)\n\n", alert.event,
               weather_alert_level_name(alert.level),
               alert.severe ? ", severe" : "");
    }

    wx_header();
    printf("\n");

    if (obs.observed == 0) {
        printf("  no reading yet");
        if (obs.last_error != ESP_OK) {
            printf(" (%s)", esp_err_to_name(obs.last_error));
        }
        printf("\n");
    } else {
        wx_cell_t temperature, feels, dewpoint, humidity;
        wx_cell_t pressure, trend, wind, gust, visibility;

        wx_number(temperature, obs.temperature, "F");
        wx_number(feels, obs.feels_like, "F");
        wx_number(dewpoint, obs.dewpoint, "F");
        wx_number(humidity, obs.humidity, "%");
        wx_number(pressure, obs.pressure, "mb");
        wx_trend(trend, obs.pressure_trend);
        wx_number(gust, obs.gust, "mph");
        wx_number(visibility, obs.visibility, "mi");

        if (obs.wind == WEATHER_UNKNOWN) {
            snprintf(wind, sizeof(wind), "--");
        } else if (obs.wind_direction == WEATHER_UNKNOWN) {
            snprintf(wind, sizeof(wind), "%d mph", obs.wind);
        } else {
            snprintf(wind, sizeof(wind), "%s %d mph",
                     compass(obs.wind_direction), obs.wind);
        }

        /* Whose reading this is, and how old — together, because they are one
         * fact. A station reporting hourly is up to an hour stale the moment it
         * is read, which is normal; without the age beside it a number that has
         * not moved looks like a fault. */
        printf("  %-*s %s, %lld min ago\n", WX_LABEL, "station", obs.station,
               (long long)((now - obs.observed) / 60));

        wx_row("temperature", temperature, "dewpoint", dewpoint);
        wx_row("feels like", feels, "humidity", humidity);
        wx_row("wind", wind, "gusts", gust);
        wx_row("pressure", pressure, "trend", trend);
        wx_row("visibility", visibility, "conditions",
               obs.text[0] != '\0' ? obs.text : "--");
    }

    /* Needs no network, only the date — so it is printed whenever the clock is
     * set, including when every fetch above has failed. */
    double lat, lon;
    if (time_sync_synced() && weather_location_get(&lat, &lon) == ESP_OK) {
        time_t rise, set;
        if (sun_times(lat, lon, now, &rise, &set) == ESP_OK) {
            wx_cell_t rise_text, set_text, daylight;
            wx_clock(rise_text, rise);
            wx_clock(set_text, set);
            const long span = (long)(set - rise);
            snprintf(daylight, sizeof(daylight), "%ldh %02ldm",
                     span / 3600, (span % 3600) / 60);
            printf("\n");
            wx_row("sunrise", rise_text, "sunset", set_text);
            printf("  %-*s %s\n", WX_LABEL, "daylight", daylight);
        }
    }

    printf("\n");
    if (report.fetched == 0) {
        printf("%-*s none yet\n", WX_LABEL, "forecast");
        /* The three things that stop one arriving, in the order worth checking.
         * Without this the empty answer looks like a broken feature rather than
         * a board that has not been told where it is. */
        if (weather_location_get(&lat, &lon) != ESP_OK) {
            printf("%-*s no location — try: loc 39.64 -84.28\n", WX_LABEL, "");
        } else if (!time_sync_synced()) {
            printf("%-*s the clock is not set; TLS needs it\n", WX_LABEL, "");
        } else if (report.last_error != ESP_OK) {
            printf("%-*s last attempt: %s\n", WX_LABEL, "",
                   esp_err_to_name(report.last_error));
            /* The one failure a US-only service produces that looks like a
             * network fault. Worth naming, because the coordinates are the
             * thing to change and nothing else hints at that. */
            if (report.last_error == ESP_ERR_NOT_FOUND ||
                report.last_error == ESP_ERR_INVALID_RESPONSE) {
                printf("%-*s are those coordinates inside the United States?\n",
                       WX_LABEL, "");
            }
        } else {
            printf("%-*s check wifi_status, then weather_refresh\n", WX_LABEL, "");
        }
        return 0;
    }

    if (report.storm == WEATHER_STORM_NONE) {
        printf("%-*s none in the forecast\n", WX_LABEL, "storm");
    } else {
        printf("%-*s %s", WX_LABEL, "storm", weather_storm_name(report.storm));
        if (report.pop >= 0) {
            printf(", %d%% precipitation", report.pop);
        }
        printf(" — %s\n", report.when);
        printf("%-*s %s\n", WX_LABEL, "outlook", report.summary);
    }
    printf("%-*s %lld min ago\n", WX_LABEL, "forecast",
           (long long)((now - report.fetched) / 60));
    return 0;
}

static int cmd_weather_refresh(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = weather_refresh();
    if (err != ESP_OK) {
        printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* Asked for, not performed. The fetch runs on the weather task because a
     * TLS handshake needs more stack than this one has — so this cannot report
     * the result, and says so rather than implying the data is ready. */
    printf("requested; run wx in a few seconds\n");
    return 0;
}

static int cmd_loc(int argc, char **argv)
{
    if (argc == 1) {
        double lat, lon;
        if (weather_location_get(&lat, &lon) != ESP_OK) {
            printf("no location stored\n");
            return 0;
        }
        printf("location: %.4f, %.4f\n", lat, lon);

        /* The stations are derived from the coordinates and are the thing that
         * says the derivation actually happened — an empty list here means the
         * grid lookup has not run or could not read the list, which is
         * otherwise only visible in the log. */
        char stations[WEATHER_STATION_COUNT][WEATHER_STATION_ID_MAX];
        weather_stations_copy(stations);
        if (stations[0][0] == '\0') {
            printf("stations: not resolved yet\n");
        } else {
            printf("stations:");
            for (int i = 0; i < WEATHER_STATION_COUNT && stations[i][0] != '\0'; i++) {
                printf(" %s", stations[i]);
            }
            printf("\n");
        }
        return 0;
    }

    if (argc != 3) {
        printf("usage: loc <latitude> <longitude>\n");
        printf("       decimal degrees, north and east positive: loc 39.64 -84.28\n");
        return 1;
    }

    /* strtod, not atof: atof cannot distinguish a bad value from zero, and zero
     * is a real coordinate in the Gulf of Guinea. */
    char *end_lat = NULL;
    char *end_lon = NULL;
    const double lat = strtod(argv[1], &end_lat);
    const double lon = strtod(argv[2], &end_lon);
    if (end_lat == argv[1] || *end_lat != '\0' || end_lon == argv[2] || *end_lon != '\0') {
        printf("rejected: both values must be numbers\n");
        return 1;
    }

    esp_err_t err = weather_location_set(lat, lon);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("rejected: latitude is -90..90, longitude is -180..180\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed to store: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("stored %.4f, %.4f\n", lat, lon);
    printf("        US only — api.weather.gov covers no other country\n");
    weather_refresh();
    return 0;
}

static const esp_console_cmd_t COMMANDS[] = {
    {
        .command = "wx",
        .help = "Everything: readings, sun, forecast, alerts",
        .func = cmd_wx,
    },
    {
        .command = "weather_refresh",
        .help = "Ask the weather task to fetch now",
        .func = cmd_weather_refresh,
    },
    {
        .command = "loc",
        .help = "Show or set the forecast location: loc <lat> <lon>",
        .func = cmd_loc,
    },
};

esp_err_t weather_cmd_register(void)
{
    return console_register(COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0]));
}
