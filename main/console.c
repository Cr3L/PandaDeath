#include "console.h"

#include "esp_check.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"

static const char *TAG = "console";

/* Deliberately short. linenoise keeps every accepted line in DRAM until the
 * REPL stops, and this one never stops, so the default 32 is 32 lines of
 * indefinitely retained typing on a target whose LVGL heap is 16 kB. Four is
 * enough to arrow back through a mistyped command, which is all history is for
 * here.
 *
 * Note this is a cap, not a scrubber: anything typed still reaches the history
 * until it ages out. Commands that carry a secret must clear it themselves —
 * see console_forget_history(). */
#define CONSOLE_HISTORY_LINES 4

esp_err_t console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "panda>";
    repl_config.max_history_len = CONSOLE_HISTORY_LINES;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    /* Registers the help command itself, so this must not do so again:
     * esp_console_common_init() overwrites the file-static argtable on a second
     * call and orphans the first, which is a small permanent leak for no gain
     * (esp_console_common.c:96). */
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_config, &repl_config, &repl),
                        TAG, "repl init failed");

    return esp_console_start_repl(repl);
}

void console_forget_history(void)
{
    linenoiseHistoryFree();
}
