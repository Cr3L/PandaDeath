#pragma once

/* What the station is doing, as seen from outside the Wi-Fi module.
 *
 * In its own header, included by both wifi_sta.c and ui_status.c, so that
 * neither has to include the other. The alternative — putting this enum in
 * wifi_sta.h — would make the UI layer include esp_wifi.h transitively just to
 * name a colour, and would let a later edit reach for the radio from a draw
 * callback because the declaration was already in scope. The seam is narrow on
 * purpose: an enum and a name, no handles.
 *
 * Ordered roughly by how far the station has got, but nothing depends on the
 * ordering — treat it as a set of names, not a scale. */
typedef enum {
    WIFI_STATUS_NO_CREDENTIALS,  /* nothing in NVS; the console is the fix */
    WIFI_STATUS_DISCONNECTED,    /* has credentials, not currently associated */
    WIFI_STATUS_CONNECTING,      /* associating, or waiting to retry */
    WIFI_STATUS_CONNECTED,       /* associated and holding an IP address */
} wifi_status_t;

/* Called on status change. Runs on the default event loop's task, not the
 * caller's: an implementation that touches LVGL takes the port lock, and one
 * that blocks for long stalls every other event handler. */
typedef void (*wifi_status_observer_t)(wifi_status_t status);

const char *wifi_status_name(wifi_status_t status);
