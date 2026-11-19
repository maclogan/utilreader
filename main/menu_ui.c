#include "menu_ui.h"

#include <stdio.h>                      // snprintf()
#include <string.h>                     // memmove()
#include <time.h>                       // time()
#include "battery.h"                    // battery_percent(), battery_state()
#include "buttons.h"                    // buttons_locked()
#include "epd.h"                        // drawing primitives + display refresh
#include "font16.h"                     // the time in the status bar
#include "local_time.h"                 // local_time_format_hm()
#include "ui_sidebar.h"                 // status bar on the left

// Tile layout (pixels). The screen is 296 wide x 128 tall; the status bar takes the left
// UI_SIDEBAR_W columns.
#define TILE_W      58                  // tile width
#define TILE_H      64                  // tile height
#define TILE_GAP    6                   // space between tiles
#define TILE_Y      22                  // top edge of the tiles
#define LABEL_H     13                  // height of the label strip at the bottom of a tile
#define MAX_TILES   4                   // tiles that fit across; with more apps the row scrolls
#define HINT_Y      (EPD_HEIGHT - 14)   // hint line at the bottom

static long drawn_minute = -1;          // minute (since 1970) the status bar shows; -1 = no time

// Draws text horizontally centered on cx
static void text_centered(int cx, int y, const char *text, int scale, uint8_t color)
{
    epd_draw_text(cx - epd_text_width(text, scale) / 2, y, text, scale, color);
}

// Battery at drawing position u (along the bar), its far end at `end`: "87%" (or "Full"),
// then a small battery whose fill shows the level, pointing up the bar, and a lightning
// bolt before the text while charging. Nothing without a battery.
static void draw_battery(int end)
{
    int pct = battery_percent();
    battery_state_t state = battery_state();
    if (pct < 0 || state == BATTERY_NONE) {
        return;
    }
    const int w = 14, h = 8, v = 4;                         // battery body, 4 px in from the edge
    int u = end - w - 2;                                    // leave room for the terminal nub
    epd_draw_rect(u, v, w, h, EPD_BLACK, false);            // outline
    epd_draw_rect(u + w, v + 2, 2, h - 4, EPD_BLACK, true); // terminal nub, at the top of the screen
    int fill = (w - 4) * pct / 100;
    if (fill > 0) {
        epd_draw_rect(u + 2, v + 2, fill, h - 4, EPD_BLACK, true);
    }

    char text[16];
    snprintf(text, sizeof(text), state == BATTERY_FULL ? "Full" : "%d%%", pct);
    int text_u = u - 3 - epd_text_width(text, 1);
    epd_draw_text(text_u, v, text, 1, EPD_BLACK);

    if (state == BATTERY_CHARGING) {
        int b = text_u - 8;
        epd_draw_line(b + 4, v - 1, b + 1, v + 4, EPD_BLACK);
        epd_draw_line(b + 1, v + 4, b + 5, v + 4, EPD_BLACK);
        epd_draw_line(b + 5, v + 4, b + 2, v + 9, EPD_BLACK);
    }
}

// Upright padlock with its top-left corner at screen (x, y)
static void draw_padlock(int x, int y)
{
    epd_draw_rect(x + 2, y, 5, 6, EPD_BLACK, false);        // shackle
    epd_draw_rect(x, y + 4, 9, 6, EPD_BLACK, true);         // body
    epd_draw_pixel(x + 4, y + 6, EPD_WHITE);                // keyhole
    epd_draw_pixel(x + 4, y + 7, EPD_WHITE);
}

// The status bar on the left, reading bottom to top: time, padlock (while locked), battery
// at the top end
static void draw_status(void)
{
    time_t now = time(NULL);
    char hm[16];
    bool have_time = local_time_format_hm(now, hm, sizeof(hm));
    char *space = strchr(hm, ' ');
    if (space) {
        memmove(space, space + 1, strlen(space));           // "5:05PM": the bar is short
    }
    drawn_minute = have_time ? (long)(now / 60) : -1;

    ui_sidebar_begin();
    int u = have_time ? font16_draw_text(0, 0, hm, EPD_BLACK) + 5 : 0;
    draw_battery(UI_SIDEBAR_LEN);
    epd_vertical_end();

    if (buttons_locked()) {                                 // drawn upright, just above the time
        int bottom = EPD_HEIGHT - 1 - 4 - u;                // screen y of drawing position u
        draw_padlock(4, bottom - 10);
    }
    ui_sidebar_rule();
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

    item->draw_icon(item, cx, y + (TILE_H - LABEL_H) / 2 + 1);    // icon centered in the space above the label
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

    // Center the row of tiles in the space right of the status bar
    int row_w = visible * TILE_W + (visible - 1) * TILE_GAP;
    int x = UI_CONTENT_X + (UI_CONTENT_W - row_w) / 2;
    for (int i = first; i < first + visible; i++) {
        draw_tile(x, TILE_Y, apps[i], i == selected);
        x += TILE_W + TILE_GAP;
    }

    // Arrows when more apps are off-screen to either side
    int mid_y = TILE_Y + TILE_H / 2 - 7;
    if (first > 0) {
        epd_draw_text(UI_CONTENT_X + 2, mid_y, "<", 2, EPD_BLACK);
    }
    if (first + visible < count) {
        epd_draw_text(EPD_WIDTH - 12, mid_y, ">", 2, EPD_BLACK);
    }

    text_centered(UI_CONTENT_CX, HINT_Y, "< > choose   press: open", 1, EPD_BLACK);  // hint
    draw_status();

    if (full) {
        epd_display();                                      // full refresh (~3 s, flashes)
    } else {
        epd_display_partial();                              // quick refresh, only changes update
    }
}

TickType_t menu_ui_next_wakeup(void)
{
    if (!local_time_synced()) {
        return portMAX_DELAY;                               // no time to show
    }
    int sec = (int)(time(NULL) % 60);
    return pdMS_TO_TICKS((60 - sec) * 1000 + 200);          // just after the minute changes
}

void menu_ui_tick(const app_t *const apps[], int count, int selected)
{
    time_t now = time(NULL);
    if (!local_time_synced() || (long)(now / 60) == drawn_minute) {
        return;
    }
    struct tm t;
    local_time_now(&t);
    menu_ui_show(apps, count, selected, t.tm_min == 0);     // full refresh on the hour clears ghosting
}
