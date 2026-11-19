/*
 * Minimal driver for the 2.9" 296x128 black/white e-paper on the Waveshare
 * ESP32-S3-LR1121-ePaper-2.9 (SSD16xx-family controller; see README.md).
 *
 * Drawing uses landscape coordinates: x = 0..295, y = 0..127.
 * Draw into the frame buffer, then call epd_display() to push it to the panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EPD_WIDTH   296     /* landscape */
#define EPD_HEIGHT  128

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
void epd_display_base(void);       /* no refresh: tells the panel the frame buffer is what's already
                                      on screen (it forgets when the rail is off), so a following
                                      epd_display_partial() only changes the differences */
void epd_sleep(void);
bool epd_is_awake(void);           /* false after epd_sleep(): partial refresh isn't possible then */

/* Before deep sleep: puts the panel to sleep, switches the shared peripheral rail (display,
 * LoRa radio, microSD, RGB LED) off and floats every pin that leads to it, so nothing is
 * powered through the I/O pins. The image stays on the panel. epd_init() undoes it. */
void epd_power_down(void);

/* Turns the picture upside down (applies to what's drawn from now on). The default comes
 * from menuconfig; the Settings app changes it at run time. */
void epd_set_rotated(bool on);
bool epd_rotated(void);

/* Vertical drawing: until epd_vertical_end(), every drawing call is turned a quarter turn
 * counter-clockwise, with drawing (0, 0) at screen (x, y). Drawing x then runs up the
 * screen and drawing y runs to the right, so text drawn at (u, 0) reads bottom to top,
 * starting u pixels above y, with the tops of the letters facing left. */
void epd_vertical_begin(int x, int y);
void epd_vertical_end(void);

void epd_draw_pixel(int x, int y, uint8_t color);
void epd_draw_rect(int x, int y, int w, int h, uint8_t color, bool fill);
void epd_draw_hline(int x, int y, int w, uint8_t color);
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
void epd_fill_circle(int cx, int cy, int r, uint8_t color);

/* 5x7 font, scaled by an integer factor. Returns x position after the text. */
int epd_draw_text(int x, int y, const char *text, int scale, uint8_t color);
int epd_text_width(const char *text, int scale);
