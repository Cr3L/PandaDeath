#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"
#include "nvs.h"  /* ESP_ERR_NVS_NOT_FOUND, returned below. */

/* Wall-clock time over SNTP, plus the timezone it is displayed in.
 *
 * The ESP32 has no battery-backed clock. At power-on the system time is the
 * epoch, and it stays there until something authoritative sets it — so every
 * timestamp before the first sync is 1970, not a wrong guess but no guess at
 * all. Callers that care must ask time_sync_synced() rather than sniffing the
 * year, which is the same test written less honestly.
 *
 * Deliberately separate from wifi_sta. Association and time sync fail for
 * unrelated reasons — a wrong password, versus a router that blocks outbound
 * UDP 123 — and one module reporting both would collapse two diagnoses into
 * one "nothing works". */

/* POSIX TZ string, excluding the terminator; _BUF adds it. 63 is comfortably
 * past the longest real zone rule ("EST5EDT,M3.2.0/2,M11.1.0/2" is 26) while
 * staying a sane bound on something typed at a console. */
#define TIME_ZONE_MAX 63
#define TIME_ZONE_BUF (TIME_ZONE_MAX + 1)

/* The zone a board that has never been told one runs in. UTC is the honest
 * default: it is right somewhere, it never shifts under a DST rule nobody
 * configured, and a clock an hour out is worse than one plainly in UTC. */
#define TIME_ZONE_DEFAULT "UTC0"

/* Buffer for time_format(). "2026-07-26 18:34:12 EDT" is 23; 48 leaves room for
 * a zone abbreviation longer than any real one. */
#define TIME_STAMP_BUF 48

/* Applies the stored zone and arms SNTP. Returns once armed, not once synced —
 * the first sync needs a network and a round trip, and a call that waited would
 * block boot on a router that never answers.
 *
 * SNTP starts on IP_EVENT_STA_GOT_IP rather than here, because before there is
 * an address there is nobody to ask. Requires esp_netif_init() and the default
 * event loop; order against wifi_sta_start() does not matter, because an
 * address that already exists is detected rather than waited for. */
esp_err_t time_sync_start(void);

/* Formats a time as "2026-07-26 18:34:12 EDT", or without the zone label when
 * `utc` is true. Wants a TIME_STAMP_BUF buffer.
 *
 * Here rather than in time_cmd.c because the sync log needs the same string,
 * and two copies of one strftime format is how the format and the buffer size
 * drift apart — which they had, at 32 bytes against 48, before this was one
 * function. */
void time_format(time_t t, bool utc, char *buf, size_t len);

/* Formats a time as a wall clock, rounded to the nearest minute rather than
 * truncated to it, with any leading space removed.
 *
 * Both halves of that are corrections to strftime rather than preferences.
 * strftime discards the seconds, so 06:31:39 prints as 06:31 against every
 * published table saying 06:32 — a minute of error added at the last step of
 * arithmetic accurate to well under one. And %l pads a single-digit hour with a
 * space, which centres a label half a character off from everything beneath it.
 *
 * `format` must not ask for seconds; rounding the minute makes them a lie.
 *
 * Here for the same reason time_format is: this was independently rediscovered
 * and written out four times across two files — the +30, the localtime_r, the
 * strftime, the trim — each with its own copy of the comment explaining why.
 * This module owns the timezone, so it owns rendering a local wall clock. */
void time_clock(time_t t, const char *format, char *buf, size_t len);

/* True once the clock has been set from the network at least once. False means
 * the time is the epoch and should not be shown as a time. */
bool time_sync_synced(void);

/* When the clock was last set, as a wall-clock time, or 0 if never.
 *
 * Wall-clock rather than uptime so that "synced at 16:04" survives being
 * printed; the age in seconds is then the caller's subtraction. */
time_t time_sync_last(void);

/* Stores a POSIX TZ string in NVS and applies it immediately — no reboot, for
 * the same reason wifi_set applies credentials immediately.
 *
 * Note that a malformed zone is not an error anyone can report: newlib's tzset
 * silently falls back to UTC on anything it cannot parse, so there is no return
 * code to check. This validates only length and emptiness; the real check is
 * the caller printing the resulting local time and a human recognising it.
 *
 * Beware the sign convention, which catches everyone once: in a POSIX TZ string
 * the offset is west-positive, so US Eastern is "EST5EDT", not "EST-5EDT". */
esp_err_t time_zone_set(const char *tz);

/* Reads the stored zone into a TIME_ZONE_BUF buffer. Returns
 * ESP_ERR_NVS_NOT_FOUND if none was ever set, in which case the board is
 * running TIME_ZONE_DEFAULT. */
esp_err_t time_zone_get(char *buf, size_t len);
