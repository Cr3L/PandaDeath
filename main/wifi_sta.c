#include "wifi_sta.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_sta";

/* Exponential backoff between association attempts, doubling from 1 s to 30 s.
 *
 * A fixed short retry is the tempting version and is wrong in both directions:
 * against a router that is rebooting it hammers the air for minutes, and
 * against a wrong password it never stops. Backoff makes the common case
 * (router back in ten seconds) fast and the hopeless case cheap. The 30 s
 * ceiling is short enough that a network returning after an hour is rejoined
 * within half a minute, without a human present. */
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000

static wifi_status_observer_t s_observer;
static wifi_status_t s_status = WIFI_STATUS_NO_CREDENTIALS;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS;

/* Last disconnect reason logged at WARN, so an unchanging one is not repeated
 * onto a UART someone may be typing at. -1 is no real reason code, so the first
 * disconnect always reports. */
static int s_last_reason = -1;

const char *wifi_status_name(wifi_status_t status)
{
    switch (status) {
    case WIFI_STATUS_NO_CREDENTIALS: return "no credentials";
    case WIFI_STATUS_DISCONNECTED:   return "disconnected";
    case WIFI_STATUS_CONNECTING:     return "connecting";
    case WIFI_STATUS_CONNECTED:      return "connected";
    }
    return "unknown";
}

/* Notifies only on an actual change, so that a network flapping through the
 * same state repeatedly does not repaint the screen or fill the log. */
static void set_status(wifi_status_t status)
{
    if (s_status == status) {
        return;
    }
    s_status = status;
    ESP_LOGI(TAG, "%s", wifi_status_name(status));
    if (s_observer) {
        s_observer(status);
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    /* The return is deliberately unchecked: a failure here produces a
     * DISCONNECTED event, which schedules the next retry through the same path
     * that scheduled this one. Handling it separately would be a second retry
     * mechanism racing the first. */
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    /* Disarm first. esp_timer_start_once returns ESP_ERR_INVALID_STATE on an
     * already-armed timer, and under ESP_ERROR_CHECK that is a reboot — a
     * crash in the error path of the error path.
     *
     * Two disconnects can land inside one retry window: wifi_sta_reconnect
     * calls esp_wifi_stop(), which raises DISCONNECTED and arms the timer, and
     * the association that follows can fail before it fires. A wrong password
     * against a nearby AP is rejected fast enough to do exactly that.
     *
     * Stopping an idle timer is defined and harmless, so this needs no test
     * for which case it is in. */
    esp_timer_stop(s_retry_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000));
    ESP_LOGD(TAG, "retrying in %" PRIu32 " ms", s_retry_ms);

    s_retry_ms *= 2;
    if (s_retry_ms > RETRY_MAX_MS) {
        s_retry_ms = RETRY_MAX_MS;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        set_status(WIFI_STATUS_CONNECTING);
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;

        /* The reason code is the single most useful line in this whole module
         * when a network will not come up: it separates "wrong password"
         * (WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT / MIC_FAILURE) from "wrong SSID"
         * (NO_AP_FOUND) from "the router pushed us off" (ASSOC_LEAVE). Without
         * it every failure looks identical from the outside.
         *
         * But it is logged loudly only when it says something new. A saturated
         * backoff retries every 30 s forever, and at WARN that is a periodic
         * line on the UART the console shares — it lands in the middle of
         * whatever is being typed and corrupts the command. That is not
         * hypothetical: it ate a wifi_set mid-word, split the line in two, and
         * the console rejected the wreckage.
         *
         * An earlier comment here argued the rule in CLAUDE.md did not apply
         * because backoff "goes quiet on its own". It does not; it slows to a
         * fixed 30 s and stays there. The rule meant what it said. */
        if (e->reason != s_last_reason) {
            s_last_reason = e->reason;
            ESP_LOGW(TAG, "disconnected, reason %d", e->reason);
        } else {
            ESP_LOGD(TAG, "disconnected, reason %d", e->reason);
        }

        /* CONNECTING while there is still reason to hope, DISCONNECTED once the
         * backoff has saturated — roughly a minute of unbroken failure. The
         * distinction is the whole value of the indicator: amber means "wait",
         * red means "this is not going to fix itself, go look at it".
         *
         * Without this the station is only ever amber or green, because it
         * never stops retrying, and an indicator that cannot show failure is
         * decoration. Found by pointing it at a network that does not exist —
         * it reads as correct in the source. */
        set_status(s_retry_ms < RETRY_MAX_MS ? WIFI_STATUS_CONNECTING
                                             : WIFI_STATUS_DISCONNECTED);
        schedule_retry();
        break;
    }

    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));

    /* Reset here rather than on association, because an AP that accepts the
     * association and then drops us before DHCP completes has not actually
     * worked — resetting there would turn that loop into a fixed 1 s retry. */
    s_retry_ms = RETRY_MIN_MS;
    /* Re-arms the disconnect log: a link that worked and later fails is news
     * even if it fails for the same reason as an hour ago. */
    s_last_reason = -1;
    set_status(WIFI_STATUS_CONNECTED);
}

/* Loads NVS credentials into the driver. Split out because both start and
 * reconnect need exactly this, and because it is the only place that touches
 * the password — keeping it in one short function makes the scrub obvious. */
static esp_err_t apply_credentials(void)
{
    char ssid[WIFI_SSID_BUF];
    char pass[WIFI_PASS_BUF];

    esp_err_t err = wifi_creds_get(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t cfg = { 0 };
    /* strlcpy, not strncpy: the driver's fields are fixed arrays that need no
     * terminator, but a silently truncated SSID would associate with the wrong
     * network name. The lengths were already validated on the way into NVS, so
     * truncation here would mean the two limits had drifted apart. */
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);

    /* The driver has its own copy now; this one has no reason to outlive the
     * function. Scrubbing both is cheap and keeps the password out of whatever
     * later reuses this stack memory. */
    memset(&cfg, 0, sizeof(cfg));
    memset(pass, 0, sizeof(pass));
    return err;
}

esp_err_t wifi_sta_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "netif create");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, NULL, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL),
        TAG, "ip handler");

    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .name = "wifi_retry",
        /* The timer task, not a dedicated one: the callback is a single
         * non-blocking call, so it has no business owning a stack. */
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");

    /* Credentials are loaded before the driver starts, so STA_START can connect
     * immediately rather than firing against an empty config. */
    esp_err_t err = apply_credentials();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Nothing stored. The driver is fully initialised and idle, which is
         * what makes wifi_sta_credentials_changed() work the moment someone
         * types wifi_set — no reboot, and no second bring-up path to keep
         * correct. */
        ESP_LOGI(TAG, "no credentials stored; idle until wifi_set");
        set_status(WIFI_STATUS_NO_CREDENTIALS);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "credentials");

    return esp_wifi_start();
}

esp_err_t wifi_sta_credentials_changed(void)
{
    /* The timer exists from the end of wifi_sta_start() and is never destroyed,
     * so its handle is the same "are we up?" answer a separate flag would give,
     * without a second thing to keep in step. */
    ESP_RETURN_ON_FALSE(s_retry_timer != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");

    /* Stop the pending retry first. Without this, a reconnect landing during a
     * backoff wait leaves the old timer armed, and it fires a second
     * esp_wifi_connect() into the attempt this one just began. */
    esp_timer_stop(s_retry_timer);
    s_retry_ms = RETRY_MIN_MS;
    /* New credentials are a new question; whatever the old ones failed with
     * should not silence the answer. */
    s_last_reason = -1;

    /* Stop, reconfigure, start — in that order, and deliberately not the
     * gentler disconnect/set_config/connect.
     *
     * The driver rejects esp_wifi_set_config outright while the station is
     * connecting ("sta is connecting, cannot set config"), which is the normal
     * state here: this is called from wifi_set, and the station is usually
     * mid-retry against whatever credentials are being replaced. A disconnect
     * first is not enough either, because it is asynchronous — the state has
     * not necessarily left "connecting" by the time the next line runs.
     *
     * esp_wifi_stop() is synchronous and leaves the driver in a state that
     * accepts configuration. The restart then raises STA_START, whose handler
     * connects, so there is exactly one path that begins an association
     * instead of two racing to. */
    ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "wifi stop");

    /* Credentials gone is a normal outcome here, not an error: wifi_clear calls
     * this too. Leaving the station associated to credentials the device has
     * been told to forget is the one state the indicator exists to make honest
     * — it would sit green while wifi_show reports nothing stored.
     *
     * Handling set and clear in one function is the point of the name. Hanging
     * "re-apply the stored credentials" off whichever command happened to need
     * it first is what let wifi_clear forget to. */
    esp_err_t err = apply_credentials();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "credentials cleared; station stopped");
        set_status(WIFI_STATUS_NO_CREDENTIALS);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "credentials");

    /* No set_status(CONNECTING) here: esp_wifi_start() raises STA_START, whose
     * handler owns that transition. Two authors of one state is what the stop/
     * start sequence above exists to avoid. */
    return esp_wifi_start();
}

void wifi_sta_set_observer(wifi_status_observer_t observer)
{
    s_observer = observer;
}

wifi_status_t wifi_sta_status(void)
{
    return s_status;
}

void wifi_sta_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t info = { 0 };
    if (s_netif != NULL && s_status == WIFI_STATUS_CONNECTED) {
        esp_netif_get_ip_info(s_netif, &info);
    }
    esp_ip4addr_ntoa(&info.ip, buf, (int)len);
}

/* Meaningless unless connected, so the caller is not asked to know that: a
 * stale RSSI from the last association is worse than no number, because it
 * looks like a measurement. Keeps esp_wifi to this file — every other module
 * asks wifi_sta rather than the driver. */
bool wifi_sta_rssi(int *rssi)
{
    wifi_ap_record_t ap;
    if (s_status != WIFI_STATUS_CONNECTED || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi = ap.rssi;
    return true;
}
