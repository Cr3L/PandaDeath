#include "wifi_cmd.h"

#include <stdio.h>
#include <string.h>

#include "console.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_wifi.h"
#include "wifi_creds.h"
#include "wifi_sta.h"

static const char *TAG = "wifi_cmd";

/* Replies go through printf, not ESP_LOGI. A console reply is addressed to the
 * person who just typed: it wants no timestamp, no level letter and no tag, and
 * it should not vanish when the log level is raised. wifi_creds.c is silent for
 * the same reason from the other side — it is a library, it reports by
 * returning esp_err_t, and a second message about the same event would arrive
 * asynchronously against the prompt. */

static int cmd_wifi_set(int argc, char **argv)
{
    /* Two forms: an open network takes the SSID alone, a protected one takes
     * SSID and passphrase. Anything else is a typo, most likely an unquoted
     * SSID containing a space, so the error says so. */
    if (argc < 2 || argc > 3) {
        printf("usage: wifi_set <ssid> [password]\n");
        printf("       quote values containing spaces: wifi_set \"My Network\" secret\n");
        return 1;
    }

    esp_err_t err = wifi_creds_set(argv[1], (argc == 3) ? argv[2] : "");

    /* Before any early return, and unconditionally: the password was typed
     * whether or not storing it worked, so the history holds it either way. */
    console_forget_history();

    if (err == ESP_ERR_INVALID_ARG) {
        printf("rejected: ssid must be 1-%d characters, password at most %d\n",
               WIFI_SSID_MAX, WIFI_PASS_MAX);
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed to store: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* Applying them immediately is the whole reason this is worth doing at a
     * console: the alternative is store-then-reboot, and a reboot makes a
     * wrong password indistinguishable from a bad flash. Reported separately
     * from the store because they fail for unrelated reasons — NVS is full
     * versus the radio is not up. */
    printf("stored.\n");

    err = wifi_sta_reconnect();
    if (err != ESP_OK) {
        printf("stored, but could not reconnect: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("connecting; check wifi_status in a few seconds\n");
    return 0;
}

static int cmd_wifi_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char ssid[WIFI_SSID_BUF];
    char pass[WIFI_PASS_BUF];

    esp_err_t err = wifi_creds_get(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("no credentials stored\n");
        return 0;
    }
    if (err != ESP_OK) {
        printf("failed to read: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* The password is described, never printed. Its length catches the mistakes
     * this command exists to catch — a trailing space, a shell that ate a
     * character, an empty value where one was meant — without putting the
     * secret into a scrollback buffer that outlives the session. */
    printf("ssid: %s\n", ssid);
    if (pass[0] == '\0') {
        printf("password: none (open network)\n");
    } else {
        printf("password: set, %u characters\n", (unsigned)strlen(pass));
    }

    memset(pass, 0, sizeof(pass));
    return 0;
}

static int cmd_wifi_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = wifi_creds_clear();
    if (err != ESP_OK) {
        printf("failed to clear: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("cleared.\n");
    return 0;
}

static int cmd_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    wifi_status_t status = wifi_sta_status();
    printf("state: %s\n", wifi_status_name(status));

    if (status == WIFI_STATUS_CONNECTED) {
        char ip[16];
        wifi_sta_ip(ip, sizeof(ip));
        printf("ip: %s\n", ip);

        /* RSSI only when connected, because ap_info is stale otherwise and a
         * plausible-looking number from the last association is worse than no
         * number. It is the first thing to check when a link associates and
         * then drops: below about -75 dBm, the answer is usually distance. */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            printf("rssi: %d dBm\n", ap.rssi);
        }
    }
    return 0;
}

static const esp_console_cmd_t COMMANDS[] = {
    {
        .command = "wifi_set",
        .help = "Store Wi-Fi credentials in NVS: wifi_set <ssid> [password]",
        .func = cmd_wifi_set,
    },
    {
        .command = "wifi_show",
        .help = "Show the stored SSID (the password is never printed)",
        .func = cmd_wifi_show,
    },
    {
        .command = "wifi_clear",
        .help = "Forget the stored credentials",
        .func = cmd_wifi_clear,
    },
    {
        .command = "wifi_status",
        .help = "Show the connection state, and the IP and signal when connected",
        .func = cmd_wifi_status,
    },
};

esp_err_t wifi_cmd_register(void)
{
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&COMMANDS[i]),
                            TAG, "failed to register %s", COMMANDS[i].command);
    }
    return ESP_OK;
}
