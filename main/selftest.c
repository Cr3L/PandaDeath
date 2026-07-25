#include "selftest.h"

#include <stddef.h>

#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "selftest";

/* Abandon the current test on a draw failure, but leave the device running.
 * ESP_ERROR_CHECK would panic and reboot, which on a board that needs a manual
 * BOOT sequence to reflash turns a one-off glitch into a power-cycle. The error
 * still reaches the log either way. */
#define TRY(expr)                                                        \
    do {                                                                 \
        const esp_err_t _err = (expr);                                   \
        if (_err != ESP_OK) {                                            \
            ESP_LOGE(TAG, "%s: %s", #expr, esp_err_to_name(_err));       \
            return;                                                      \
        }                                                                \
    } while (0)

/* Each colour is announced over serial before it is drawn, so the log and the
 * panel can be compared frame by frame. If the log says RED and the screen is
 * blue, the fault is colour order; if the screen stays dark throughout, the
 * fault is upstream of colour entirely. */
static void run_color_test(void)
{
    static const struct {
        const char *name;
        uint16_t    color;
    } steps[] = {
        { "RED",   COLOR_RED   },
        { "GREEN", COLOR_GREEN },
        { "BLUE",  COLOR_BLUE  },
        { "WHITE", COLOR_WHITE },
    };

    const size_t n = sizeof(steps) / sizeof(steps[0]);

    for (size_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "colour test %u/%u: %s",
                 (unsigned)(i + 1), (unsigned)n, steps[i].name);
        TRY(display_fill(steps[i].color));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* A still frame with known dimensions, held long enough to photograph.
 *
 * Anything moving is unreliable to verify from a phone camera: rolling shutter
 * smears a travelling shape along its axis of motion, which is indistinguishable
 * by eye from the shape being the wrong size. This frame holds still so that
 * what is measured is what was drawn.
 *
 * Correct output, on black:
 *   - a white crosshair meeting at the exact centre of the circle
 *   - a green square, visibly square, one fifth of the panel width
 *   - four red edge markers, equidistant from the centre
 * A square that reads as a rectangle here is a real geometry fault.
 *
 * Note the frame is symmetric about both axes, so it cannot catch a mirroring
 * fault. Add an asymmetric element if that is what is being chased. */
static void run_geometry_test(void)
{
    const int cx = DISPLAY_WIDTH / 2;
    const int cy = DISPLAY_HEIGHT / 2;
    const int square = 48;
    const int marker = 16;
    const int inset = 10;

    ESP_LOGI(TAG, "geometry test: crosshair + %dx%d square, holding 20 s",
             square, square);

    TRY(display_fill(COLOR_BLACK));

    /* Crosshair: spans the full panel, so the arms are clipped by the round
     * bezel symmetrically if and only if the centre is correct. */
    TRY(display_fill_rect(0, cy - 1, DISPLAY_WIDTH, 2, COLOR_WHITE));
    TRY(display_fill_rect(cx - 1, 0, 2, DISPLAY_HEIGHT, COLOR_WHITE));

    TRY(display_fill_rect(cx - square / 2, cy - square / 2,
                          square, square, COLOR_GREEN));

    /* Cardinal markers. Equal distances confirm no axis is being scaled, so the
     * four positions are listed together where that symmetry is checkable by
     * eye rather than spread over four near-identical calls. */
    const struct { int x, y; } markers[] = {
        { cx - marker / 2, inset                          },  /* north */
        { cx - marker / 2, DISPLAY_HEIGHT - inset - marker },  /* south */
        { inset,           cy - marker / 2                },  /* west  */
        { DISPLAY_WIDTH - inset - marker, cy - marker / 2 },  /* east  */
    };

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        TRY(display_fill_rect(markers[i].x, markers[i].y,
                              marker, marker, COLOR_RED));
    }

    vTaskDelay(pdMS_TO_TICKS(20000));
}

/* A square sweeping top to bottom. Solid fills alone cannot distinguish a live
 * panel from one holding a stale frame, so this proves the pixels are actually
 * being rewritten. */
static void run_motion_test(void)
{
    const int size = 48;
    const int x    = (DISPLAY_WIDTH - size) / 2;
    int       y    = 0;
    int       step = 4;

    ESP_LOGI(TAG, "motion test: square sweeping vertically");
    TRY(display_fill(COLOR_BLACK));

    while (true) {
        TRY(display_fill_rect(x, y, size, size, COLOR_GREEN));
        vTaskDelay(pdMS_TO_TICKS(30));
        TRY(display_fill_rect(x, y, size, size, COLOR_BLACK));

        /* Turn around before moving rather than after, so the square never
         * takes a step it has to undo. */
        if (y + step < 0 || y + step + size > DISPLAY_HEIGHT) {
            step = -step;
        }
        y += step;
    }
}

void run_selftest(void)
{
    run_color_test();
    run_geometry_test();
    run_motion_test();
}
