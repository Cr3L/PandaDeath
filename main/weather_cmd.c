#include "weather_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "console.h"
#include "esp_console.h"
#include "time_sync.h"
#include "weather.h"

/* Replies go through printf, not ESP_LOGI — same reasoning as wifi_cmd.c. */

static int cmd_weather(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    weather_report_t report;
    weather_report_copy(&report);
    const weather_report_t *r = &report;

    /* Before the no-forecast branch, not after it. An alert is the one urgent
     * thing this command can say, and it was briefly printed only once a
     * forecast existed — so a live warning was hidden by the absence of the
     * less important half. Found by pointing the board at a real warning. */
    weather_alert_t alert;
    weather_alert_copy(&alert);
    if (alert.level != WEATHER_ALERT_NONE) {
        printf("ALERT:   %s (%s%s)\n", alert.event,
               weather_alert_level_name(alert.level),
               alert.severe ? ", severe" : "");
    }

    if (r->fetched == 0) {
        printf("no forecast yet\n");
        /* The three things that stop one arriving, in the order worth checking.
         * Without this the empty answer looks like a broken feature rather than
         * a board that has not been told where it is. */
        double lat, lon;
        if (weather_location_get(&lat, &lon) != ESP_OK) {
            printf("        no location set — try: loc 39.64 -84.28\n");
        } else if (!time_sync_synced()) {
            printf("        the clock is not set yet; TLS needs it — check time\n");
        } else if (r->last_error != ESP_OK) {
            printf("        last attempt: %s\n", esp_err_to_name(r->last_error));
            /* The one failure a US-only service produces that looks like a
             * network fault. Worth naming, because the coordinates are the
             * thing to change and nothing else hints at that. */
            if (r->last_error == ESP_ERR_NOT_FOUND ||
                r->last_error == ESP_ERR_INVALID_RESPONSE) {
                printf("        are those coordinates inside the United States?\n");
            }
        } else {
            printf("        check wifi_status, then weather_refresh\n");
        }
        return 0;
    }

    printf("now:     %d%s, %s\n", r->temperature, r->temperature_unit, r->now);

    if (r->storm == WEATHER_STORM_NONE) {
        printf("storm:   none in the forecast\n");
    } else {
        printf("storm:   %s", weather_storm_name(r->storm));
        if (r->pop >= 0) {
            printf(" (%d%% precipitation)", r->pop);
        }
        printf("\n");
        printf("when:    %s\n", r->when);
        printf("outlook: %s\n", r->summary);
    }

    /* Age, not just the timestamp. A forecast is a claim about the future made
     * at a point in the past, and "fetched 4 hours ago" is the part that says
     * whether to believe it. */
    const time_t now = time(NULL);
    printf("fetched: %lld min ago\n", (long long)((now - r->fetched) / 60));
    return 0;
}

/* Prints an integer reading, or a dash where the station reported nothing.
 * Every numeric field needs this: on a clear day the nearest test station sent
 * four nulls alongside everything else, and a zero in their place would be a
 * reading no instrument made. */
static void print_reading(const char *label, int value, const char *unit)
{
    if (value == WEATHER_UNKNOWN) {
        printf("  %-12s --\n", label);
    } else {
        printf("  %-12s %d %s\n", label, value, unit);
    }
}

/* Degrees the wind blows from, as the compass point anyone would say out loud.
 *
 * Sixteen points of 22.5°, each name covering the arc *centred* on its bearing
 * rather than starting at it — so north runs from 348.75° round to 11.25°,
 * which is why the half-point offset is there. In integers that is
 * (deg + 11.25) / 22.5 scaled by four to clear the fractions. */
static const char *compass(int degrees)
{
    static const char *const POINTS[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    const int index = ((degrees * 4 + 45) / 90) % 16;
    return POINTS[index < 0 ? 0 : index];
}

static int cmd_obs(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    weather_obs_t obs;
    weather_obs_copy(&obs);

    if (obs.observed == 0) {
        printf("no observation yet\n");
        if (obs.last_error != ESP_OK) {
            printf("        last attempt: %s\n", esp_err_to_name(obs.last_error));
        }
        return 0;
    }

    /* The station's own reading time, and its age. A station reporting hourly
     * is up to an hour stale the moment it is read, which is normal — without
     * the age beside it, a number that has not moved looks like a bug. */
    const time_t now = time(NULL);
    printf("station: %s, %lld min ago\n", obs.station,
           (long long)((now - obs.observed) / 60));
    if (obs.text[0] != '\0') {
        printf("  %-12s %s\n", "conditions", obs.text);
    }

    print_reading("temperature", obs.temperature, "F");
    print_reading("feels like", obs.feels_like, "F");
    print_reading("dewpoint", obs.dewpoint, "F");
    print_reading("humidity", obs.humidity, "%");
    print_reading("pressure", obs.pressure, "mb");

    if (obs.wind == WEATHER_UNKNOWN) {
        printf("  %-12s --\n", "wind");
    } else if (obs.wind_direction == WEATHER_UNKNOWN) {
        printf("  %-12s %d mph\n", "wind", obs.wind);
    } else {
        printf("  %-12s %s %d mph\n", "wind", compass(obs.wind_direction), obs.wind);
    }
    print_reading("gusts", obs.gust, "mph");
    print_reading("visibility", obs.visibility, "mi");
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
    printf("requested; run weather in a few seconds\n");
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
        .command = "weather",
        .help = "Show the last forecast and how close a thunderstorm is",
        .func = cmd_weather,
    },
    {
        .command = "obs",
        .help = "Show the latest reading from the nearest station",
        .func = cmd_obs,
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
