#include "wifi_cmd.h"

#include <stdio.h>
#include <string.h>

#include "console.h"
#include "esp_check.h"
#include "esp_console.h"
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
     * wrong password indistinguishable from a bad flash.
     *
     * One outcome line, printed after both steps. Announcing the store and
     * then the connection separately reads as two stores when the second
     * fails, and the store is not interesting on its own. */
    err = wifi_sta_credentials_changed();
    if (err != ESP_OK) {
        printf("stored, but the station would not take them: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("stored; connecting, check wifi_status in a few seconds\n");
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

    /* Same call wifi_set makes, for the same reason: the stored credentials
     * changed. Without it the board stays on the network it was just told to
     * forget — green glyph, live IP, and wifi_show reporting nothing stored. */
    err = wifi_sta_credentials_changed();
    if (err != ESP_OK) {
        printf("cleared, but the station would not stop: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("cleared; station stopped\n");
    return 0;
}

static int cmd_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    wifi_status_t status = wifi_sta_status();
    printf("state: %s\n", wifi_status_name(status));

    char ip[IP4ADDR_STRLEN_MAX];
    wifi_sta_ip(ip, sizeof(ip));

    int rssi;
    /* wifi_sta owns the "only meaningful when connected" rule for both of
     * these, so this prints what it is given rather than re-deriving when to
     * ask. RSSI is the first thing to check when a link associates and then
     * drops: below about -75 dBm the answer is usually distance. */
    if (wifi_sta_rssi(&rssi)) {
        printf("ip: %s\n", ip);
        printf("rssi: %d dBm\n", rssi);
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
