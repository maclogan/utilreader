/*
 * Minimal driver for the 2.13" 250x122 black/white e-paper on the Elecrow CrowPanel ESP32
 * E-Paper 2.13" (SSD1680 or JD79661 controller, detected at start-up; see README.md).
 *
 * Drawing uses landscape coordinates: x = 0..249, y = 0..121.
 * Draw into the frame buffer, then call epd_display() to push it to the panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EPD_WIDTH   250     /* landscape */
#define EPD_HEIGHT  122

#define EPD_BLACK   0
#define EPD_WHITE   1

// Wiring diagnostic: tests the display pins and logs a report (tag "epd_diag").
// Only works before epd_init(), which takes the pins over for SPI.
void epd_diagnose(void);

void epd_init(void);
void epd_clear(uint8_t color);
void epd_display(void);            /* full refresh (flashes), also saves the image for partials */
void epd_display_partial(void);    /* quick refresh of changed pixels only, after an epd_display();
                                      the panel must not be put to sleep in between */
void epd_sleep(void);
bool epd_is_awake(void);           /* false after epd_sleep(): partial refresh isn't possible then */

void epd_draw_pixel(int x, int y, uint8_t color);
void epd_draw_rect(int x, int y, int w, int h, uint8_t color, bool fill);
void epd_draw_hline(int x, int y, int w, uint8_t color);
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
void epd_fill_circle(int cx, int cy, int r, uint8_t color);

/* 5x7 font, scaled by an integer factor. Returns x position after the text. */
int epd_draw_text(int x, int y, const char *text, int scale, uint8_t color);
int epd_text_width(const char *text, int scale);
