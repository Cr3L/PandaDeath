#include "time_cmd.h"

#include <stdio.h>
#include <time.h>

#include "console.h"
#include "esp_console.h"
#include "time_sync.h"

/* Replies go through printf, not ESP_LOGI — same reasoning as wifi_cmd.c: a
 * console reply is addressed to the person who just typed, wants no log
 * furniture, and should not vanish when the log level is raised. */

static void print_zone(void)
{
    char tz[TIME_ZONE_BUF];
    if (time_zone_get(tz, sizeof(tz)) == ESP_OK) {
        printf("zone:   %s\n", tz);
    } else {
        printf("zone:   %s (default; none stored)\n", TIME_ZONE_DEFAULT);
    }
}

static int cmd_time(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* The unsynced case is reported first and on its own, rather than printing
     * a 1970 timestamp with a footnote. A clock that has never been set does
     * not have a slightly wrong time; it has no time, and showing one invites
     * reading it as a small error. */
    if (!time_sync_synced()) {
        printf("synced: no — the clock has never been set\n");
        print_zone();
        printf("        needs a connection; check wifi_status\n");
        return 0;
    }

    time_t now = time(NULL);
    char stamp[TIME_STAMP_BUF];

    time_format(now, true, stamp, sizeof(stamp));
    printf("utc:    %s\n", stamp);

    time_format(now, false, stamp, sizeof(stamp));
    printf("local:  %s\n", stamp);

    print_zone();

    time_t last = time_sync_last();
    time_format(last, false, stamp, sizeof(stamp));
    printf("synced: %s (%lld s ago)\n", stamp, (long long)(now - last));
    return 0;
}

static int cmd_tz(int argc, char **argv)
{
    if (argc > 2) {
        printf("usage: tz [<posix tz string>]\n");
        printf("       quote it if it contains spaces; with no argument, shows the current zone\n");
        return 1;
    }

    /* No argument shows rather than errors: it is the same question the command
     * answers after setting, and a read-only form means checking the zone costs
     * nothing and risks nothing. */
    if (argc == 1) {
        print_zone();
        return 0;
    }

    esp_err_t err = time_zone_set(argv[1]);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("rejected: zone must be 1-%d characters\n", TIME_ZONE_MAX);
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed to store: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* Printing the resulting local time is not a courtesy, it is the only
     * validation there is: tzset accepts a malformed zone silently and falls
     * back to UTC, so a human recognising the wall clock is the check. */
    print_zone();
    if (time_sync_synced()) {
        char stamp[TIME_STAMP_BUF];
        time_format(time(NULL), false, stamp, sizeof(stamp));
        printf("local:  %s\n", stamp);
        printf("        if that is not your wall clock, the zone string is wrong\n");
    } else {
        printf("        stored; the clock is not set yet, so there is nothing to show\n");
    }
    return 0;
}

static const esp_console_cmd_t COMMANDS[] = {
    {
        .command = "time",
        .help = "Show the current time, the zone, and when the clock was last set",
        .func = cmd_time,
    },
    {
        .command = "tz",
        .help = "Show or set the POSIX timezone: tz [EST5EDT,M3.2.0,M11.1.0]",
        .func = cmd_tz,
    },
};

esp_err_t time_cmd_register(void)
{
    return console_register(COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0]));
}
