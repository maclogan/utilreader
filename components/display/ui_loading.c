#include "ui_loading.h"

#include "epd.h"                  // drawing primitives + display refresh

#define BOX_W   100                     // loading box size (pixels)
#define BOX_H   30

// Small 11x15 hourglass with its top-left corner at (x, y)
static void draw_hourglass(int x, int y)
{
    epd_draw_rect(x, y, 11, 2, EPD_BLACK, true);            // top cap
    epd_draw_rect(x, y + 13, 11, 2, EPD_BLACK, true);       // bottom cap
    epd_draw_line(x + 1, y + 2, x + 5, y + 7, EPD_BLACK);   // left glass, top half
    epd_draw_line(x + 9, y + 2, x + 5, y + 7, EPD_BLACK);   // right glass, top half
    epd_draw_line(x + 5, y + 7, x + 1, y + 12, EPD_BLACK);  // left glass, bottom half
    epd_draw_line(x + 5, y + 7, x + 9, y + 12, EPD_BLACK);  // right glass, bottom half
    epd_draw_rect(x + 3, y + 10, 5, 2, EPD_BLACK, true);    // sand piled in the bottom
    epd_draw_pixel(x + 5, y + 8, EPD_BLACK);                // sand falling through the neck
    epd_draw_pixel(x + 5, y + 9, EPD_BLACK);
}

void ui_show_loading(void)
{
    if (!epd_is_awake()) {                                  // partial refresh not possible right now
        return;
    }
    int x = (EPD_WIDTH - BOX_W) / 2;                        // centered on screen
    int y = (EPD_HEIGHT - BOX_H) / 2;

    // Draw over the existing frame (don't clear it): white box with a 2 px black border
    epd_draw_rect(x, y, BOX_W, BOX_H, EPD_WHITE, true);
    epd_draw_rect(x, y, BOX_W, BOX_H, EPD_BLACK, false);
    epd_draw_rect(x + 1, y + 1, BOX_W - 2, BOX_H - 2, EPD_BLACK, false);

    draw_hourglass(x + 12, y + 8);                          // icon on the left
    epd_draw_text(x + 32, y + 11, "Loading...", 1, EPD_BLACK);  // text on the right

    epd_display_partial();                                  // quick refresh: only the box changes
}
