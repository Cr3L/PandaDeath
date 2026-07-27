#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "storage.h"
#include "time_sync.h"

static const char *TAG = "weather";

#define KEY_LOCATION "wx_loc"

/* api.weather.gov asks callers to identify themselves and to contact them if a
 * client misbehaves. A repo URL is the honest answer and is public already —
 * unlike an email address, which this file would then be leaking. */
#define USER_AGENT "PandaDeath/1.0 (github.com/Cr3L/PandaDeath)"

/* Twelve-hourly forecasts are reissued a few times a day, so polling faster
 * than this buys nothing and spends someone else's bandwidth. The service is
 * free, unauthenticated and run by a public agency; 48 requests a day is a
 * guest's share. */
#define POLL_INTERVAL_MS (30 * 60 * 1000)

/* After a failure, rather than the full interval — a router that came back
 * should not cost half an hour of staleness. Not a backoff ladder: unlike
 * association, a failed fetch has a working fallback (the previous report) and
 * the retry costs one request. */
#define RETRY_INTERVAL_MS (2 * 60 * 1000)

/* While waiting for the address and the clock — a different question from a
 * failed fetch, and it was briefly given the same two-minute answer. Both
 * arrive within seconds of boot, so a board that was ready at five seconds sat
 * idle until two minutes with everything working and nothing to show. Found on
 * hardware; it reads as correct in the source, because the mistake is that one
 * constant answered two questions. */
#define READY_POLL_MS 2000

/* Not-ready wakeups before dropping to the retry pace — a minute at
 * READY_POLL_MS, which is far longer than a working boot takes. */
#define READY_POLL_LIMIT 30

/* The twelve-hourly document measured 14 kB. 24 kB is room for a wordier
 * forecast office without being a gamble; a response that exceeds it is
 * rejected rather than parsed from a truncated buffer, because half a JSON
 * document is not a smaller forecast, it is a wrong one. */
#define RESPONSE_MAX 24576

/* The /points metadata is about 2 kB — an order of magnitude under the
 * forecast, and it does not deserve the forecast's buffer. It is also
 * requested at the worst heap moment the firmware has: first poll after boot,
 * with Wi-Fi, an open TLS session and LVGL all resident. */
#define POINTS_MAX 4096

/* TLS against the full certificate bundle and a cJSON parse of 14 kB, on one
 * task. The console's 8 kB is not enough for this, which is why weather_refresh
 * signals this task instead of doing the work itself. */
#define TASK_STACK 12288
#define TASK_PRIORITY 4

/* Enough for "https://api.weather.gov/gridpoints/ABC/123,456/forecast" with
 * room to spare. */
#define URL_MAX 160

/* Written by the weather task, read by whoever asks. Unsynchronised, which is
 * fine for a console command reading it a few times a day — a torn read costs
 * one wrong line — but is worth revisiting before a screen reads it every
 * frame. Noted here rather than solved, because the right shape depends on what
 * the UI ends up wanting. */
static weather_report_t s_report;
static TaskHandle_t s_task;

/* Consecutive not-ready wakeups, so a board that will never be ready stops
 * asking every two seconds forever. */
static int s_not_ready_polls;

/* The grid square the coordinates fall in never moves, so the /points lookup
 * that derives it is made once and its answer kept. Cleared when the location
 * changes, which is what makes weather_location_set() take effect. */
static char s_forecast_url[URL_MAX];

const char *weather_storm_name(weather_storm_t storm)
{
    switch (storm) {
    case WEATHER_STORM_NONE:     return "none";
    case WEATHER_STORM_POSSIBLE: return "possible";
    case WEATHER_STORM_LIKELY:   return "likely";
    case WEATHER_STORM_NOW:      return "now";
    }
    return "unknown";
}

/* Fetches `url` into `buf`. Streams rather than using esp_http_client_perform,
 * because perform gives no way to refuse a response that is too large — it is
 * already in memory by the time anyone could ask. */
static esp_err_t fetch(const char *url, char *buf, size_t len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .user_agent = USER_AGENT,
        /* Certificate validation against IDF's bundled roots. Without this the
         * handshake fails; with it, a wrong clock does too, which is why the
         * caller waits for time_sync. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "client init");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "http %d from %s", status, url);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    /* A chunked response reports -1, which is not an error and not a length —
     * the read loop below bounds itself either way. */
    if (content_length > 0 && (size_t)content_length >= len) {
        ESP_LOGE(TAG, "response is %lld bytes, buffer is %u",
                 (long long)content_length, (unsigned)len);
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t total = 0;
    while (total < len - 1) {
        int got = esp_http_client_read(client, buf + total, (int)(len - 1 - total));
        if (got < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (got == 0) {
            break;  /* complete */
        }
        total += (size_t)got;
    }
    buf[total] = '\0';

    /* Ran to the end of the buffer with the connection still open, so the
     * document is longer than the space for it and what was read is a prefix. */
    if (total >= len - 1) {
        ESP_LOGE(TAG, "response exceeds %u bytes", (unsigned)len);
        err = ESP_ERR_NO_MEM;
        goto done;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

/* Resolves coordinates to the forecast URL for their grid square. */
static esp_err_t resolve_forecast_url(char *buf, size_t len)
{
    double lat, lon;
    ESP_RETURN_ON_ERROR(weather_location_get(&lat, &lon), TAG, "no location");

    char url[URL_MAX];
    snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);

    char *body = malloc(POINTS_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = fetch(url, body, POINTS_MAX);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "points json");

    err = ESP_ERR_NOT_FOUND;
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    const cJSON *forecast = cJSON_GetObjectItemCaseSensitive(props, "forecast");
    if (cJSON_IsString(forecast) && forecast->valuestring != NULL) {
        if (strlen(forecast->valuestring) < len) {
            strlcpy(buf, forecast->valuestring, len);
            err = ESP_OK;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(root);
    return err;
}

/* Reads probabilityOfPrecipitation, which is an object holding a value that is
 * null as often as it is a number. Returns -1 for "not stated", which is not
 * the same as zero and should not be shown as one. */
static int period_pop(const cJSON *period)
{
    const cJSON *pop = cJSON_GetObjectItemCaseSensitive(period, "probabilityOfPrecipitation");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(pop, "value");
    return cJSON_IsNumber(value) ? value->valueint : -1;
}

/* NWS writes shortForecast from a controlled vocabulary, so matching on it is
 * reading a field rather than guessing at prose — "Chance Showers And
 * Thunderstorms", "Slight Chance Showers And Thunderstorms". Matching the same
 * way against the whole JSON document would also hit the office name, the icon
 * URL and the detailed text, which is why this takes one field. */
static bool mentions_thunder(const char *short_forecast)
{
    return short_forecast != NULL && strcasestr(short_forecast, "thunder") != NULL;
}

/* The threshold between "possible" and "likely". NWS phrasing puts "Slight
 * Chance" at 20% or below and "Chance" from 30 to 50, so this splits the two
 * bands the service itself uses rather than inventing one. */
#define POP_LIKELY 30

static esp_err_t parse_forecast(const char *body, weather_report_t *out)
{
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "forecast json");

    esp_err_t err = ESP_ERR_NOT_FOUND;

    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    const cJSON *periods = cJSON_GetObjectItemCaseSensitive(props, "periods");
    if (!cJSON_IsArray(periods) || cJSON_GetArraySize(periods) == 0) {
        goto done;
    }

    weather_report_t r = { 0 };
    r.pop = -1;
    r.periods_ahead = -1;
    r.storm = WEATHER_STORM_NONE;

    /* The current conditions come from period 0 regardless of storms — the
     * screen wants a temperature whether or not anything is brewing. */
    const cJSON *first = cJSON_GetArrayItem(periods, 0);
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(first, "temperature");
    const cJSON *unit = cJSON_GetObjectItemCaseSensitive(first, "temperatureUnit");
    const cJSON *now_short = cJSON_GetObjectItemCaseSensitive(first, "shortForecast");
    if (cJSON_IsNumber(temp)) {
        r.temperature = temp->valueint;
    }
    if (cJSON_IsString(unit) && unit->valuestring != NULL) {
        strlcpy(r.temperature_unit, unit->valuestring, sizeof(r.temperature_unit));
    }
    if (cJSON_IsString(now_short) && now_short->valuestring != NULL) {
        strlcpy(r.now, now_short->valuestring, sizeof(r.now));
    }

    /* First storm wins: the question this answers is "when is the next one",
     * so scanning past it to a worse one later would report the wrong period. */
    int index = 0;
    const cJSON *period = NULL;
    cJSON_ArrayForEach(period, periods) {
        const cJSON *short_forecast =
            cJSON_GetObjectItemCaseSensitive(period, "shortForecast");
        const char *text = cJSON_IsString(short_forecast) ? short_forecast->valuestring : NULL;

        if (mentions_thunder(text)) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(period, "name");
            const int pop = period_pop(period);

            r.periods_ahead = index;
            r.pop = pop;
            if (cJSON_IsString(name) && name->valuestring != NULL) {
                strlcpy(r.when, name->valuestring, sizeof(r.when));
            }
            strlcpy(r.summary, text, sizeof(r.summary));

            if (index == 0) {
                r.storm = WEATHER_STORM_NOW;
            } else {
                r.storm = (pop >= POP_LIKELY) ? WEATHER_STORM_LIKELY
                                              : WEATHER_STORM_POSSIBLE;
            }
            break;
        }
        index++;
    }

    r.fetched = time(NULL);
    *out = r;
    err = ESP_OK;

done:
    cJSON_Delete(root);
    return err;
}

static esp_err_t poll_once(void)
{
    if (s_forecast_url[0] == '\0') {
        ESP_RETURN_ON_ERROR(resolve_forecast_url(s_forecast_url, sizeof(s_forecast_url)),
                            TAG, "resolve");
        ESP_LOGI(TAG, "grid resolved: %s", s_forecast_url);
    }

    char *body = malloc(RESPONSE_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = fetch(s_forecast_url, body, RESPONSE_MAX);
    if (err == ESP_OK) {
        err = parse_forecast(body, &s_report);
    }
    free(body);
    return err;
}

static void weather_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t wait_ms = POLL_INTERVAL_MS;

        /* Both conditions, not just the address: a certificate is rejected when
         * the clock says 1970, and that failure reads like a network fault
         * while being a boot-ordering one. */
        if (!net_have_address() || !time_sync_synced()) {
            /* Fast while there is reason to expect it soon — both arrive within
             * seconds of a normal boot — then at the ordinary retry pace. A
             * board with no credentials is never ready, and without this it
             * would wake 43,000 times a day to ask. */
            wait_ms = (s_not_ready_polls < READY_POLL_LIMIT) ? READY_POLL_MS
                                                             : RETRY_INTERVAL_MS;
            s_not_ready_polls++;
        } else {
            s_not_ready_polls = 0;
            esp_err_t err = poll_once();
            s_report.last_error = err;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "poll failed: %s", esp_err_to_name(err));
                /* The previous report is left standing. Stale weather with a
                 * timestamp beside it is more use than no weather, and the
                 * console prints how old it is. */
                wait_ms = RETRY_INTERVAL_MS;
            } else {
                ESP_LOGI(TAG, "%d%s %s; storm %s",
                         s_report.temperature, s_report.temperature_unit,
                         s_report.now, weather_storm_name(s_report.storm));
            }
        }

        /* Waits, but wakes early when weather_refresh() notifies. The
         * notification is the whole mechanism: it keeps every TLS handshake on
         * this task and its stack, no matter who asked for one. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t weather_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    /* Not pinned. The task is asleep almost always and its wakeups are neither
     * latency-critical nor cache-sensitive, so choosing a core for it would be
     * a decision with no reason behind it. */
    BaseType_t ok = xTaskCreate(weather_task, "weather", TASK_STACK, NULL,
                                TASK_PRIORITY, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t weather_refresh(void)
{
    ESP_RETURN_ON_FALSE(s_task != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

const weather_report_t *weather_report(void)
{
    return &s_report;
}

esp_err_t weather_location_set(double lat, double lon)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    char value[48];
    snprintf(value, sizeof(value), "%.4f,%.4f", lat, lon);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, KEY_LOCATION, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        /* The cached grid square belongs to the old coordinates. Clearing it
         * here rather than recomputing keeps this function free of network
         * work — the next poll resolves it, and fails visibly if it cannot. */
        s_forecast_url[0] = '\0';
        s_report.fetched = 0;
    }
    return err;
}

esp_err_t weather_location_get(double *lat, double *lon)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char value[48];
    size_t size = sizeof(value);
    err = nvs_get_str(handle, KEY_LOCATION, value, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    /* Written by this module in one format, so a value that will not parse means
     * the key was written by something else — worth an error rather than a
     * silently wrong location. */
    if (sscanf(value, "%lf,%lf", lat, lon) != 2) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
