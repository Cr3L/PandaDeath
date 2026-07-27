#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* The shared network layer everything else sits on: the esp_netif stack and the
 * default event loop.
 *
 * These are not any one feature's to create. They were inside wifi_sta_start()
 * while Wi-Fi was their only user, and the moment SNTP needed them too,
 * whichever module created them would have become the one the other had to be
 * started after — an ordering constraint with nothing in the source to state
 * it. */

/* Brings up esp_netif and the default event loop. Call once, before any module
 * that registers an event handler or creates an interface. */
esp_err_t net_init(void);

/* True when the station holds a non-zero IPv4 address.
 *
 * Exists so that a module which acts on IP_EVENT_STA_GOT_IP can also handle the
 * case where that event has already happened — it fires once per address, and a
 * handler registered afterwards waits forever for a repeat that only a
 * reconnection will bring. Checking here is what lets those modules be started
 * in any order relative to the radio.
 *
 * Looks the interface up by key rather than asking wifi_sta, so a caller
 * depends on having an address and not on which module supplied it. Before the
 * station exists the answer is simply false, which is the normal boot case. */
bool net_have_address(void);
