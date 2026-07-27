#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "net.h"
#include "storage.h"

static const char *TAG = "time_sync";

/* The namespace is shared and comes from storage.h. The key is stored here
 * rather than behind a time_creds.c because there is exactly one of it — the
 * separate module exists for the credentials only because SSID and password
 * have to land in a single commit, and a lone string has no such constraint. */
#define KEY_ZONE "tz"

/* pool.ntp.org resolves to a rotating set of volunteer servers and is what the
 * vendor default and every example use. One server, because
 * CONFIG_LWIP_SNTP_MAX_SERVERS is 1: a second entry would be silently dropped,
 * which is worse than plainly having one. */
#define NTP_SERVER "pool.ntp.org"

/* When the clock was last set, or 0 for never. This is also the "have we ever
 * synced" flag: a separate bool would be a second copy of the same fact, with
 * an invariant that the two are updated together — the kind that holds until
 * someone adds a third setter. The epoch is not a time this can legitimately
 * hold, because the clock is only ever set from a server. */
static time_t s_last;

/* Applies a zone to the C library. setenv+tzset is the whole mechanism —
 * localtime() and strftime() read the TZ environment variable, and nothing
 * caches it past the tzset. */
static void apply_zone(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

void time_format(time_t t, bool utc, char *buf, size_t len)
{
    struct tm tm;
    if (utc) {
        gmtime_r(&t, &tm);
    } else {
        localtime_r(&t, &tm);
    }
    /* %Z prints nothing at all under a zone string tzset could not parse, which
     * is exactly the case worth being able to see — an unlabelled time next to
     * a labelled one is the symptom of a bad TZ. */
    strftime(buf, len, utc ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M:%S %Z", &tm);
}

/* Runs on the lwIP tcpip task, not a task of ours: the path is sntp_recv ->
 * sntp_process -> sntp_sync_time -> here, and CONFIG_LWIP_TCPIP_CORE_LOCKING is
 * off, so the stack is CONFIG_LWIP_TCPIP_TASK_STACK_SIZE — 3072 bytes.
 *
 * That is why this does as little as possible, and why the formatting below
 * happens only on the first sync. Measured depth of the logging branch is about
 * 1.65 kB, over half the stack, almost all of it newlib's ~800-byte vfprintf
 * frame (CONFIG_LIBC_NEWLIB_NANO_FORMAT is off). It fits with ~1.4 kB spare and
 * has run on hardware, but it is the deepest thing this firmware puts on that
 * stack, so anything added here should be weighed against that number rather
 * than against the source looking short. */
static void on_sync(struct timeval *tv)
{
    const bool first = (s_last == 0);
    s_last = tv->tv_sec;

    /* The first sync is news; the hourly ones that follow are not, and this
     * shares UART0 with the console. A line arriving at INFO every
     * CONFIG_LWIP_SNTP_UPDATE_DELAY would land in the middle of whatever is
     * being typed — the exact failure the disconnect log in wifi_sta.c already
     * had to be talked down from.
     *
     * The formatting is inside the branch, not above it: at
     * CONFIG_LOG_MAXIMUM_LEVEL=3 an ESP_LOGD here compiles out entirely, so
     * formatting for one would be building a string for a call that no longer
     * exists — and would put the strftime and timezone frames on the tcpip
     * stack every hour instead of once. */
    if (first) {
        char stamp[TIME_STAMP_BUF];
        time_format(s_last, false, stamp, sizeof(stamp));
        ESP_LOGI(TAG, "clock set from %s: %s", NTP_SERVER, stamp);
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    /* Safe on every reconnection, not just the first: esp_netif_sntp_start()
     * stops the client before starting it (esp_netif_sntp.c, sntp_start_api),
     * so this restarts the query cycle rather than stacking a second one.
     *
     * Restarting on each address is also what makes a resume-from-outage
     * correct — the client's next scheduled poll could otherwise be up to an
     * hour away, having been armed before the link went down. */
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not start sntp: %s", esp_err_to_name(err));
    }
}

esp_err_t time_sync_start(void)
{
    char tz[TIME_ZONE_BUF];
    if (time_zone_get(tz, sizeof(tz)) == ESP_OK) {
        apply_zone(tz);
        ESP_LOGI(TAG, "timezone %s", tz);
    } else {
        apply_zone(TIME_ZONE_DEFAULT);
        ESP_LOGI(TAG, "no timezone stored; running %s", TIME_ZONE_DEFAULT);
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    /* Armed, not started: see on_got_ip. Starting here would send queries into
     * an interface with no address, and the client's retry schedule would then
     * be running against failures that had nothing to do with the server. */
    cfg.start = false;
    /* No semaphore. It exists only for esp_netif_sntp_sync_wait(), and nothing
     * here blocks on the clock — the callback is how this module finds out. */
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sync;

    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&cfg), TAG, "sntp init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL),
        TAG, "ip handler");

    /* If an address already exists, the event registered above has already
     * fired and will not fire again until the link drops. Catching that is what
     * lets this be called in any order relative to wifi_sta_start() — see
     * net_have_address(), which exists for exactly this pattern. */
    if (net_have_address()) {
        ESP_LOGI(TAG, "address already up; starting sntp now");
        return esp_netif_sntp_start();
    }
    return ESP_OK;
}

bool time_sync_synced(void)
{
    return s_last != 0;
}

time_t time_sync_last(void)
{
    return s_last;
}

esp_err_t time_zone_set(const char *tz)
{
    if (tz == NULL || tz[0] == '\0' || strlen(tz) > TIME_ZONE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_ZONE, tz);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    /* Applied only if it was stored, so that what the next boot does and what
     * this boot does cannot disagree. A zone that took effect but was not
     * written would come back wrong after a power cycle, silently. */
    if (err == ESP_OK) {
        apply_zone(tz);
    }
    return err;
}

esp_err_t time_zone_get(char *buf, size_t len)
{
    nvs_handle_t handle;
    /* A board that has never been given a zone fails at the open rather than
     * the get, because the namespace does not exist until something is written
     * to it. Both report ESP_ERR_NVS_NOT_FOUND, which is what callers act on. */
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = len;
    err = nvs_get_str(handle, KEY_ZONE, buf, &size);
    nvs_close(handle);
    return err;
}
