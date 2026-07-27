#pragma once

#include <time.h>

#include "esp_err.h"

/* Sunrise and sunset from coordinates and a date.
 *
 * The only thing this device knows that needs no network at all. Everything
 * else here — forecast, observations, alerts, even the clock — is a request
 * that can fail; this is arithmetic, correct offline and correct forever.
 *
 * Deliberately holds no state and reads no NVS. It is given the coordinates
 * rather than fetching them, which keeps a module of pure astronomy from
 * depending on the weather service that happens to store where the board is.
 *
 * The algorithm is NOAA's, in the form usually written up as the "sunrise
 * equation": mean solar noon from the longitude, the sun's mean anomaly and
 * equation of centre to get ecliptic longitude, declination from that, and
 * finally the hour angle at which the sun's centre sits 0.833° below the
 * horizon. That last figure is not a fudge — it is the sum of the sun's
 * apparent radius (about 0.27°) and average atmospheric refraction at the
 * horizon (about 0.57°), which together mean the sun is seen to rise while it
 * is geometrically still below the horizon.
 *
 * Measured against the US Naval Observatory: within 42 seconds everywhere in
 * the continental US across a solstice, an equinox and midsummer, and within
 * 105 seconds at 64°N. The error grows with latitude because the sun crosses
 * the horizon at a shallower angle there, so the same small error in
 * declination moves the crossing time further. It does not model the observer's
 * elevation, and it uses a circular orbit corrected by the equation of centre
 * rather than solving Kepler's equation.
 *
 * Worth knowing when checking this: api.sunrise-sunset.org, the first result
 * anyone reaches for, disagrees with USNO by up to four and a half minutes and
 * is the less accurate of the two. Testing against it made correct code look
 * broken and nearly bought a fix for a bug that was not there.
 */

/* Sunrise and sunset for the day containing `when`, as UTC timestamps.
 *
 * `when` selects the day in UTC, not in local time. For the continental US that
 * differs from the local day only for a query made between local midnight and
 * the UTC date rollover a few hours later, which is the middle of the night;
 * the answer then is the previous day's, off by a few minutes.
 *
 * Returns ESP_ERR_INVALID_STATE where the sun does not rise or set at all —
 * above the Arctic circle in midsummer or midwinter. That is a real answer
 * rather than a failure, and the caller must say so rather than print a
 * meaningless time.
 */
esp_err_t sun_times(double latitude, double longitude, time_t when,
                    time_t *sunrise, time_t *sunset);
