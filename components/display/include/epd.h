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
void epd_sleep(void);               /* waits for any refresh in progress first */

/* Refreshes run in the background: epd_display() / epd_display_partial() copy the frame
 * buffer and return at once (the frame buffer is free to draw the next screen straight
 * away). Requests made while the panel is still busy merge into one refresh showing the
 * latest frame. This waits until the panel has caught up. */
void epd_wait_idle(void);
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

/* Until epd_clear_clip(), drawing only lands inside this box (screen coordinates): e.g.
 * text sliding inside a column. epd_clear() doesn't reset it. */
void epd_set_clip(int x, int y, int w, int h);
void epd_clear_clip(void);

/* Grey: the panel can show 4 levels. A frame with any grey pixel in it is shown by
 * epd_display() with the grey waveform (~2.3 s, one flash); black/white frames keep the
 * faster black/white refresh. A partial refresh shows grey pixels as black (dark grey) or
 * white (light grey), but leaves grey already on screen alone where it hasn't changed, so
 * a grey picture survives partial updates to the rest of the screen. */
#define EPD_GREY_BLACK  0
#define EPD_GREY_DARK   1
#define EPD_GREY_LIGHT  2
#define EPD_GREY_WHITE  3
void epd_draw_pixel_grey(int x, int y, uint8_t level);
bool epd_frame_has_grey(void);     /* something grey was drawn since epd_clear() */

/* Draws an 8-bit greyscale picture (0 = black, 255 = white; `stride` bytes per row) with
 * error-diffusion dithering, into the 4 grey levels, or into pure black and white when
 * `grey` is false (for screens that must stay black/white, e.g. ones refreshed often). */
void epd_draw_grey8(int x, int y, int w, int h, const uint8_t *pix, int stride, bool grey);

/* How much pictures are lightened before dithering: gamma x 100, 30-100 (100 = unchanged,
 * lower = lighter). Settings > Darkness; main.c applies the saved value at start-up. */
void epd_set_picture_gamma(int percent);

/* How partial refreshes are done (Settings > Refresh; main.c applies the saved choice):
 * STOCK the chip's own waveform; CLEAN Waveshare's for this panel; QUICK and QUICKEST the same
 * with a shorter drive phase (faster, possibly fainter). Each logs how long the panel was busy. */
/* After this many quick (partial) refreshes in a row, the next is a full one, whatever asked
 * for it (ghosting builds up). Settings > Cleanup; main.c applies the saved value. */
void epd_set_full_every(int n);
int epd_full_every(void);

enum { EPD_PARTIAL_STOCK, EPD_PARTIAL_CLEAN, EPD_PARTIAL_QUICK, EPD_PARTIAL_QUICKEST, EPD_PARTIAL_MODES };
void epd_set_partial_mode(int mode);
int epd_partial_mode(void);
int epd_picture_gamma(void);
void epd_draw_rect(int x, int y, int w, int h, uint8_t color, bool fill);
void epd_draw_hline(int x, int y, int w, uint8_t color);
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
void epd_fill_circle(int cx, int cy, int r, uint8_t color);

/* 5x7 font, scaled by an integer factor. Returns x position after the text. */
int epd_draw_text(int x, int y, const char *text, int scale, uint8_t color);
int epd_text_width(const char *text, int scale);
