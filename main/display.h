#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

/* RGB565 helpers. The panel is fed big-endian over SPI, so colours are byte
 * swapped once here rather than at every draw call. */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define RGB565_SWAP(c) ((uint16_t)((((c) & 0xFF) << 8) | (((c) >> 8) & 0xFF)))

#define COLOR_BLACK   RGB565(0x00, 0x00, 0x00)
#define COLOR_WHITE   RGB565(0xFF, 0xFF, 0xFF)
#define COLOR_RED     RGB565(0xFF, 0x00, 0x00)
#define COLOR_GREEN   RGB565(0x00, 0xFF, 0x00)
#define COLOR_BLUE    RGB565(0x00, 0x00, 0xFF)

/* Brings up the SPI bus, the GC9A01 panel and the backlight PWM channel.
 * On success the panel is initialised, cleared to black and switched on, and
 * the backlight sits at 0% so the caller decides when the screen lights up. */
esp_err_t display_init(void);

/* Backlight duty, 0 (off) to 100 (full). */
esp_err_t display_set_backlight(uint8_t percent);

/* Paints the whole panel a single RGB565 colour (unswapped, e.g. COLOR_RED). */
esp_err_t display_fill(uint16_t color);

/* Fills an arbitrary rectangle. Coordinates are clipped to the panel bounds. */
esp_err_t display_fill_rect(int x, int y, int w, int h, uint16_t color);
