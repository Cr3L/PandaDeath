#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Firmware updates over the network.
 *
 * This board has no working auto-reset, so every wired update costs a manual
 * unplug / hold-BOOT / replug. partitions.csv was laid out with two 4 MB app
 * slots two sessions before this was possible, for exactly this reason. The
 * point of the feature is not convenience — it is that the gesture is the
 * step of the loop most likely to go wrong in a way nobody can see.
 *
 * The update is written to whichever slot is not running, so a failure part way
 * through costs nothing: the running image is never touched. */

/* Called with bytes received so far and the total, as the download proceeds.
 * Runs on whatever task called ota_update(). */
typedef void (*ota_progress_cb_t)(size_t received, size_t total, void *ctx);

/* Arms rollback confirmation and reports what the running image is.
 *
 * Must be called every boot, not only after an update. With rollback enabled a
 * newly installed image boots in ESP_OTA_IMG_PENDING_VERIFY, and if nothing
 * confirms it the next reset reverts to the slot it replaced — so an image that
 * never calls this is an image that cannot survive a power cycle. */
esp_err_t ota_start(void);

/* Downloads the image at `url` into the inactive slot and marks it for the next
 * boot. Blocking, and slow — the caller's task carries an HTTP client and a
 * flash writer, so it wants a stack to match (see CONSOLE_TASK_STACK).
 *
 * Does not reboot. Deciding when to restart belongs to whoever asked for the
 * update, and a function that reboots as a side effect of returning cannot
 * report what it did.
 *
 * `progress` may be NULL. */
esp_err_t ota_update(const char *url, ota_progress_cb_t progress, void *ctx);

/* Label of the partition the running image was booted from — "ota_0" or
 * "ota_1". Alternation across updates is the only real proof that an update
 * took: a board that reboots into a fresh-looking log has not necessarily
 * changed slots. */
const char *ota_running_slot(void);

/* Label of the partition the *next* boot will use, which differs from
 * ota_running_slot() exactly when an installed update is waiting for a restart.
 *
 * Added because without it a successful update is invisible until you reboot
 * into it: ota_status reported the running image, which is by definition the
 * old one, so "installed and waiting" and "nothing happened" printed the same
 * four lines. Found by testing rather than by reading. */
const char *ota_next_slot(void);

/* True while the running image is on probation and has not yet been confirmed.
 * A second update cannot start in this state — esp_ota_begin() refuses with
 * ESP_ERR_OTA_ROLLBACK_INVALID_STATE — so it is worth being able to see. */
bool ota_pending_verify(void);
