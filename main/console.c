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

/* Commands run on the REPL's own task, so this is the stack every command gets.
 * The default 4 kB was fine while the deepest command formatted a timestamp,
 * and is not fine now that `ota` runs an HTTP client and a flash writer on it:
 * IDF's own OTA examples give that work a dedicated 8 kB task.
 *
 * Raising the shared stack rather than spawning a task for the update is the
 * cheaper of the two. A task would need a completion handshake back to the
 * command that started it, so the console can block and report — machinery
 * whose only purpose would be to keep a number in this file small. The cost is
 * 4 kB of DRAM on one task that already exists. */
#define CONSOLE_TASK_STACK 8192

esp_err_t console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "panda>";
    repl_config.max_history_len = CONSOLE_HISTORY_LINES;
    repl_config.task_stack_size = CONSOLE_TASK_STACK;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    /* Registers the help command itself, so this must not do so again:
     * esp_console_common_init() overwrites the file-static argtable on a second
     * call and orphans the first, which is a small permanent leak for no gain
     * (esp_console_common.c:96). */
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_config, &repl_config, &repl),
                        TAG, "repl init failed");

    return esp_console_start_repl(repl);
}

esp_err_t console_register(const esp_console_cmd_t *cmds, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]),
                            TAG, "failed to register %s", cmds[i].command);
    }
    return ESP_OK;
}

void console_forget_history(void)
{
    linenoiseHistoryFree();
}
