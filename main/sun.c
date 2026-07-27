#include "sun.h"

#include <math.h>

/* Julian day number of the Unix epoch. Unix time is seconds since
 * 1970-01-01T00:00:00Z; Julian days count from noon, which is where the half
 * comes from. */
#define JULIAN_UNIX_EPOCH 2440587.5

/* Julian day of J2000.0 — 2000-01-01T12:00:00 TT, the epoch every constant
 * below is referred to. */
#define JULIAN_J2000 2451545.0

/* Earth's axial tilt, degrees. */
#define OBLIQUITY 23.4397

/* How far below the horizon the sun's centre is at the moment it appears to
 * rise: its own apparent radius plus refraction. See sun.h. */
#define HORIZON_ANGLE -0.833

#define DEG (M_PI / 180.0)

static double sin_deg(double d) { return sin(d * DEG); }
static double cos_deg(double d) { return cos(d * DEG); }

static time_t julian_to_unix(double julian)
{
    return (time_t)llround((julian - JULIAN_UNIX_EPOCH) * 86400.0);
}

esp_err_t sun_times(double latitude, double longitude, time_t when,
                    time_t *sunrise, time_t *sunset)
{
    const double julian_now = (double)when / 86400.0 + JULIAN_UNIX_EPOCH;

    /* Whole days since J2000, for the day containing `when`. The 0.0008 is a
     * leap-second offset folded in by the usual formulation of this algorithm;
     * it is under a minute and is kept only so the constants below match their
     * published values. */
    const double day = floor(julian_now - JULIAN_J2000 + 0.0008);

    /* Mean solar noon at this longitude, in days since J2000. East is positive,
     * so the longitude is subtracted: a place east of Greenwich reaches noon
     * earlier, meaning at a smaller day fraction. */
    const double mean_noon = day - longitude / 360.0;

    /* Where the sun would be in a circular orbit. */
    const double anomaly = fmod(357.5291 + 0.98560028 * mean_noon, 360.0);

    /* And the correction for the orbit not being circular — the equation of
     * centre. Three terms is well past what a minute of accuracy needs. */
    const double centre = 1.9148 * sin_deg(anomaly)
                        + 0.0200 * sin_deg(2.0 * anomaly)
                        + 0.0003 * sin_deg(3.0 * anomaly);

    /* Ecliptic longitude. 102.9372 is the longitude of perihelion, and the 180
     * turns the sun's position into the earth's as seen from it. */
    const double ecliptic = fmod(anomaly + centre + 180.0 + 102.9372, 360.0);

    /* Solar noon — when the sun actually crosses the meridian, which is not
     * clock noon and moves by a quarter of an hour across the year. */
    const double transit = JULIAN_J2000 + mean_noon
                         + 0.0053 * sin_deg(anomaly)
                         - 0.0069 * sin_deg(2.0 * ecliptic);

    const double declination_sin = sin_deg(ecliptic) * sin_deg(OBLIQUITY);
    const double declination_cos = cos(asin(declination_sin));

    /* The hour angle at which the sun sits at HORIZON_ANGLE. Out of range means
     * the sun never reaches that altitude on this day in either direction —
     * midnight sun or polar night, depending on the sign. */
    const double numerator = sin_deg(HORIZON_ANGLE) - sin_deg(latitude) * declination_sin;
    const double denominator = cos_deg(latitude) * declination_cos;
    if (denominator == 0.0) {
        return ESP_ERR_INVALID_STATE;
    }
    const double cos_hour_angle = numerator / denominator;
    if (cos_hour_angle < -1.0 || cos_hour_angle > 1.0) {
        return ESP_ERR_INVALID_STATE;
    }

    const double hour_angle = acos(cos_hour_angle) / DEG;

    *sunset = julian_to_unix(transit + hour_angle / 360.0);
    *sunrise = julian_to_unix(transit - hour_angle / 360.0);
    return ESP_OK;
}
