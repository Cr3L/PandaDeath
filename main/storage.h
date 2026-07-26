#pragma once

/* The NVS namespace every module on this board stores settings under.
 *
 * Shared because it is a property of the partition, not of any one feature —
 * wifi_creds.c and time_sync.c both write here, and a typo in one copy would
 * not fail. NVS creates the misspelled namespace on first write, both modules
 * keep working in isolation, and the divergence only surfaces later as an erase
 * that clears one module's keys and not the other's.
 *
 * Deliberately not the driver's. esp_wifi keeps its calibration and its own
 * copy of the last-used credentials under "nvs.net80211" in this same partition
 * (CONFIG_ESP_WIFI_NVS_ENABLED=y); writing there would mean two owners for one
 * set of keys. Ours is the source of truth, the driver's copy is its business.
 *
 * Keys stay with the module that owns them. A central list of every key would
 * put unrelated features in one file and make each addition a shared edit. */
#define STORAGE_NAMESPACE "pandadeath"
