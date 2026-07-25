#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

/* Panel mirroring. Public because two independent consumers apply it and they
 * must agree: display_init() sets it on the panel so the LVGL-free self-test
 * path is oriented correctly, and esp_lvgl_port re-applies it from its own
 * rotation config when LVGL attaches. Passing zeroes to the latter does not
 * mean "leave alone" — it actively mirrors nothing, undoing display_init().
 *
 * This panel needs a reflection, not a rotation, so an odd number of axis
 * flips. Mirroring both axes is the tempting fix and is wrong: it composes to a
 * 180 degree rotation, leaving text upright but reading backwards, as if seen
 * from behind the glass. */
#define PANEL_MIRROR_X false
#define PANEL_MIRROR_Y true

/* Rows per draw buffer, shared by this module's fill chunks and by LVGL's draw
 * buffers so that the SPI bus transfer limit below covers both. Keeping them on
 * one constant is deliberate: sizing the bus to the smaller of two independent
 * numbers truncates the larger transfer, and does it silently. */
#define DISPLAY_DRAW_ROWS 20
#define DISPLAY_MAX_TRANSFER_SZ \
    (DISPLAY_WIDTH * DISPLAY_DRAW_ROWS * (int)sizeof(uint16_t))

/* Colours are plain RGB565 in host byte order. The byte swap the panel needs is
 * an internal detail of the fill path, so callers never see it. */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BLACK   RGB565(0x00, 0x00, 0x00)
#define COLOR_WHITE   RGB565(0xFF, 0xFF, 0xFF)
#define COLOR_RED     RGB565(0xFF, 0x00, 0x00)
#define COLOR_GREEN   RGB565(0x00, 0xFF, 0x00)
#define COLOR_BLUE    RGB565(0x00, 0x00, 0xFF)

/* Brings up the SPI bus, the GC9A01 panel and the backlight PWM channel.
 * On success the panel is initialised, cleared to black and switched on, and
 * the backlight sits at 0% so the caller decides when the screen lights up. */
esp_err_t display_init(void);

/* Backlight duty, 0 (off) to 100 (full). Takes effect immediately. */
esp_err_t display_set_backlight(uint8_t percent);

/* Ramps the backlight to `percent` over `ms`, driven by the LEDC hardware fader.
 * Returns as soon as the fade is armed, so the caller is free to get on with
 * something useful while the panel lights up. */
esp_err_t display_fade_backlight(uint8_t percent, uint32_t ms);

/* Panel handles, for handing the display to a graphics library. NULL before
 * display_init() succeeds. Anything drawing through these owns the panel; the
 * display_fill* helpers below must not be used at the same time. */
esp_lcd_panel_handle_t display_panel_handle(void);
esp_lcd_panel_io_handle_t display_io_handle(void);

/* Paints the whole panel a single RGB565 colour (unswapped, e.g. COLOR_RED). */
esp_err_t display_fill(uint16_t color);

/* Fills an arbitrary rectangle. Coordinates are clipped to the panel bounds. */
esp_err_t display_fill_rect(int x, int y, int w, int h, uint16_t color);
