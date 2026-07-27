#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

/* Storm watch from the US National Weather Service.
 *
 * api.weather.gov is free, needs no key, and is the authoritative source for US
 * forecasts. It is a two-request API and there is no way around that: the
 * /points/{lat},{lon} endpoint returns *metadata* — which forecast office and
 * grid square the coordinates fall in — and the forecast lives at a URL it
 * hands back. The grid square never moves, so that first request is made once
 * and its answer kept.
 *
 * Uses the twelve-hourly forecast, not the hourly one. The hourly endpoint
 * would give "storm in three hours" instead of "storm tonight", and it is 92 kB
 * of JSON against 14 kB — enough to need PSRAM, which is a separate piece of
 * work with its own verification. Recorded in docs/open-items.md rather than
 * folded in here.
 *
 * Depends on the clock. TLS rejects a certificate whose validity window does
 * not contain the current time, and an unsynced board thinks it is 1970 — so
 * this waits for time_sync before its first request rather than failing in a
 * way that looks like a network fault. */

/* How close a thunderstorm is. Ordered by severity, and nothing depends on the
 * numbering. */
typedef enum {
    WEATHER_STORM_NONE,      /* nothing in the forecast window */
    WEATHER_STORM_POSSIBLE,  /* forecast mentions storms, low probability */
    WEATHER_STORM_LIKELY,    /* forecast mentions storms, high probability */
    WEATHER_STORM_NOW,       /* the current period is the stormy one */
} weather_storm_t;

const char *weather_storm_name(weather_storm_t storm);

typedef struct {
    /* Wall clock of the last successful fetch, 0 for never — which is also the
     * "is there a report" test. A separate valid flag was a second copy of the
     * same fact, and the invariant had already broken: changing the location
     * cleared the flag and left the timestamp, so the two disagreed by
     * construction. Same reasoning as time_sync.c's s_last. */
    time_t fetched;

    /* Why the last attempt failed, ESP_OK if it did not. Kept because the
     * reasons are not guessable from outside: coordinates outside the United
     * States fail exactly like a dead network, and telling someone to check
     * their Wi-Fi when the real answer is "that place is not in the service"
     * sends them the wrong way. */
    esp_err_t last_error;

    weather_storm_t storm;
    int periods_ahead;          /* 12-hour periods until the storm; 0 = now */

    /* When the stormy period begins, 0 if no storm is forecast. Parsed from the
     * period's ISO 8601 startTime rather than inferred from periods_ahead,
     * because the periods are not a uniform twelve hours — the first one runs
     * from now until the next boundary, so "two periods ahead" is anywhere from
     * twelve to thirty-six hours away. */
    time_t starts_at;
    int pop;                    /* probability of precipitation, percent, -1 unknown */
    char when[24];              /* the period's own name: "Monday Night" */
    char summary[64];           /* that period's shortForecast */

    int temperature;            /* current period, in temperature_unit */
    char temperature_unit[2];   /* "F" or "C", as the service reports it */
    char now[64];               /* current period's shortForecast */
} weather_report_t;

/* Starts the poll task. Returns immediately; the first report arrives once
 * there is an address and a synced clock. */
esp_err_t weather_start(void);

/* Asks the poll task to fetch now, and returns without waiting.
 *
 * Deliberately does not fetch on the caller's task — not because the stack
 * could not take it (ota_cmd.c runs a TLS handshake on the console task and
 * that works), but because this one can block for the length of two HTTPS
 * round trips with the prompt held, and because the 24 kB buffer belongs to the
 * module that owns the poll interval rather than to whoever asked. */
esp_err_t weather_refresh(void);

/* A consistent snapshot, taken under the lock the poll task writes behind.
 * `fetched` is 0 until a report has arrived.
 *
 * The only accessor. There was briefly a second returning the live struct,
 * with a comment conceding it was safe for the console and unsafe for the
 * screen — a rule enforced by prose, on a codebase that otherwise deletes the
 * shape which makes the wrong thing representable. Copying ~200 bytes costs
 * well under a microsecond, so there was nothing to trade for the risk. */
void weather_report_copy(weather_report_t *out);

/* Coordinates, stored in NVS.
 *
 * Not compiled in. A latitude and longitude to two decimal places is someone's
 * neighbourhood, this repo is public, and git keeps a value after the file
 * holding it is edited — the same reasoning that keeps Wi-Fi credentials out of
 * the tree. Changing them clears the cached grid square, so the next poll
 * re-derives it. */
esp_err_t weather_location_set(double lat, double lon);
esp_err_t weather_location_get(double *lat, double *lon);
