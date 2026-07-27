#include "net.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "net";

esp_err_t net_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    return ESP_OK;
}

bool net_have_address(void)
{
    /* WIFI_STA_DEF is the key esp_netif_create_default_wifi_sta() registers
     * under. Until that runs the handle does not exist, which is not an error
     * here — it is the state of a board whose radio has not started yet. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}
