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
