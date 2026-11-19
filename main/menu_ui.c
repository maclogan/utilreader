#include "menu_ui.h"

#include <stdio.h>                      // snprintf()
#include "battery.h"                    // battery_percent(), battery_state()
#include "epd_2in13.h"                  // drawing primitives + display refresh

// Tile layout (pixels). The screen is 250 wide x 122 tall.
#define TILE_W      64                  // tile width
#define TILE_H      64                  // tile height
#define TILE_GAP    12                  // space between tiles
#define TILE_Y      20                  // top edge of the tiles (no title, so the row sits higher)
#define LABEL_H     13                  // height of the label strip at the bottom of a tile
#define MAX_TILES   3                   // tiles that fit across; with more apps the row scrolls

// Draws text horizontally centered on cx
static void text_centered(int cx, int y, const char *text, int scale, uint8_t color)
{
    epd_draw_text(cx - epd_text_width(text, scale) / 2, y, text, scale, color);
}

// Battery in the top-right corner: a small battery whose fill shows the level, "87%" (or
// "Full") to its left, and a lightning bolt while charging. Nothing without a fuel gauge.
static void draw_battery(void)
{
    int pct = battery_percent();
    battery_state_t state = battery_state();
    if (pct < 0 || state == BATTERY_NONE) {
        return;
    }
    const int w = 14, h = 8;                                // battery body
    int x = EPD_WIDTH - 4 - w - 2;                          // leave room for the terminal nub
    int y = 4;
    epd_draw_rect(x, y, w, h, EPD_BLACK, false);            // outline
    epd_draw_rect(x + w, y + 2, 2, h - 4, EPD_BLACK, true); // terminal nub on the right
    int fill = (w - 4) * pct / 100;                         // inside is w - 4 px wide
    if (fill > 0) {
        epd_draw_rect(x + 2, y + 2, fill, h - 4, EPD_BLACK, true);
    }

    char text[16];
    if (state == BATTERY_FULL) {
        snprintf(text, sizeof(text), "Full");
    } else {
        snprintf(text, sizeof(text), "%d%%", pct);
    }
    int text_x = x - 3 - epd_text_width(text, 1);
    epd_draw_text(text_x, y, text, 1, EPD_BLACK);

    if (state == BATTERY_CHARGING) {                        // lightning bolt left of the text
        int bx = text_x - 8;
        epd_draw_line(bx + 4, y - 1, bx + 1, y + 4, EPD_BLACK);
        epd_draw_line(bx + 1, y + 4, bx + 5, y + 4, EPD_BLACK);
        epd_draw_line(bx + 5, y + 4, bx + 2, y + 9, EPD_BLACK);
    }
}

// Draws one tile with its top-left corner at (x, y)
static void draw_tile(int x, int y, const app_t *item, bool highlighted)
{
    int cx = x + TILE_W / 2;                                // horizontal center of the tile
    int label_y = y + TILE_H - LABEL_H;                     // top of the label strip

    if (highlighted) {
        // Thick 3 px border, then a black label strip with white text
        for (int i = 0; i < 3; i++) {
            epd_draw_rect(x + i, y + i, TILE_W - 2 * i, TILE_H - 2 * i, EPD_BLACK, false);
        }
        epd_draw_rect(x, label_y, TILE_W, LABEL_H, EPD_BLACK, true);
        text_centered(cx, label_y + 3, item->name, 1, EPD_WHITE);
    } else {
        // Thin 1 px border, black text on white
        epd_draw_rect(x, y, TILE_W, TILE_H, EPD_BLACK, false);
        text_centered(cx, label_y + 3, item->name, 1, EPD_BLACK);
    }

    item->draw_icon(cx, y + (TILE_H - LABEL_H) / 2 + 1);    // icon centered in the space above the label
}

void menu_ui_show(const app_t *const apps[], int count, int selected, bool full)
{
    if (full) {
        epd_init();                                         // wake + fully reset the panel
    }
    epd_clear(EPD_WHITE);

    // Show up to MAX_TILES tiles, scrolled so the selected one is visible
    int visible = count < MAX_TILES ? count : MAX_TILES;
    int first = selected < visible ? 0 : selected - visible + 1;

    // Center the row of tiles horizontally
    int row_w = visible * TILE_W + (visible - 1) * TILE_GAP;
    int x = (EPD_WIDTH - row_w) / 2;
    for (int i = first; i < first + visible; i++) {
        draw_tile(x, TILE_Y, apps[i], i == selected);
        x += TILE_W + TILE_GAP;
    }

    // Arrows when more apps are off-screen to either side
    int mid_y = TILE_Y + TILE_H / 2 - 7;
    if (first > 0) {
        epd_draw_text(2, mid_y, "<", 2, EPD_BLACK);
    }
    if (first + visible < count) {
        epd_draw_text(EPD_WIDTH - 12, mid_y, ">", 2, EPD_BLACK);
    }

    text_centered(EPD_WIDTH / 2, 106, "< > choose   SELECT open", 1, EPD_BLACK);  // hint
    draw_battery();                                         // top-right corner, above the tiles

    if (full) {
        epd_display();                                      // full refresh (~2 s, flashes)
    } else {
        epd_display_partial();                              // quick refresh, only the highlight changes
    }
}
