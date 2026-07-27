#include "ota_cmd.h"

#include <stdio.h>
#include <string.h>

#include "console.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "ota.h"

/* Replies go through printf, not ESP_LOGI — same reasoning as wifi_cmd.c. */

/* Progress is printed every this many percent rather than on every callback.
 * esp_https_ota_perform() returns per HTTP read, which for a 1.4 MB image over
 * a LAN is hundreds of calls a second; printing each would flood the UART and
 * slow the very transfer it is reporting on. */
#define PROGRESS_STEP_PCT 10

static void print_progress(size_t received, size_t total, void *ctx)
{
    int *last = ctx;

    /* A server that sends no content-length gives a zero total. Rather than
     * divide by it, fall back to reporting kilobytes — the download still works
     * and the operator still sees it moving. */
    if (total == 0) {
        printf("  %u kB\n", (unsigned)(received / 1024));
        return;
    }

    const int pct = (int)((received * 100) / total);
    if (pct >= *last + PROGRESS_STEP_PCT) {
        *last = pct - (pct % PROGRESS_STEP_PCT);
        printf("  %d%%\n", *last);
    }
}

static int cmd_ota(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: ota <url>\n");
        printf("       e.g. ota http://192.168.1.50:8000/pandadeath.bin\n");
        return 1;
    }

    printf("fetching %s\n", argv[1]);

    int last_pct = 0;
    esp_err_t err = ota_update(argv[1], print_progress, &last_pct);

    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        printf("refused: this image is still on probation and cannot be replaced\n");
        printf("         it confirms itself once the board has an address — check wifi_status\n");
        return 1;
    }
    if (err != ESP_OK) {
        /* The running image is untouched whatever went wrong: the download
         * writes only to the inactive slot, and nothing has been pointed at it.
         * Saying so is the difference between a failed update and a frightening
         * one. */
        printf("failed: %s\n", esp_err_to_name(err));
        printf("        still running %s, unchanged\n", ota_running_slot());
        return 1;
    }

    printf("installed; reboot to run it\n");
    printf("        the new image reverts on its next reset unless it reaches the network\n");
    return 0;
}

static int cmd_ota_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_app_desc_t *app = esp_app_get_description();
    const char *slot = ota_running_slot();

    printf("slot:    %s\n", slot);
    printf("version: %s\n", app->version);
    printf("built:   %s %s\n", app->date, app->time);
    printf("state:   %s\n", ota_pending_verify() ? "on probation, not yet confirmed"
                                                 : "confirmed");

    /* Only when they differ, which is the only time it is news. Printing
     * "next: ota_1" under "slot: ota_1" every time would train the eye past the
     * one case that matters. */
    const char *next = ota_next_slot();
    if (strcmp(next, slot) != 0) {
        printf("next:    %s (installed, waiting for a reboot)\n", next);
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Here rather than in a module of its own: the only reason this board has
     * ever needed a software reset is to run an image that was just installed,
     * and a reset is the second half of that operation. It is also the one
     * console command that cannot report its own outcome. */
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static const esp_console_cmd_t COMMANDS[] = {
    {
        .command = "ota",
        .help = "Install firmware from a URL into the inactive slot: ota <url>",
        .func = cmd_ota,
    },
    {
        .command = "ota_status",
        .help = "Show the running slot, version, and whether it is confirmed",
        .func = cmd_ota_status,
    },
    {
        .command = "reboot",
        .help = "Restart the board",
        .func = cmd_reboot,
    },
};

esp_err_t ota_cmd_register(void)
{
    return console_register(COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0]));
}
