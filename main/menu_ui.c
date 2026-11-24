#include "menu_ui.h"

#include <stdio.h>                      // snprintf()
#include <string.h>                     // memmove()
#include <time.h>                       // time()
#include "buttons.h"                    // buttons_locked()
#include "epd.h"                        // drawing primitives + display refresh
#include "font16.h"                     // the time in the status bar
#include "local_time.h"                 // local_time_format_hm()
#include "settings.h"                   // the layout (rows x columns), set on the web page
#include "ui_sidebar.h"                 // status bar on the left

// Tile layout (pixels). The screen is 296 wide x 128 tall; the status bar takes the left
// UI_SIDEBAR_W columns. One row: 58 x 64 tiles with a hint line below. Two rows: smaller tiles
// filling the height, no hint.
#define LABEL_H     13                  // height of the label strip at the bottom of a tile
#define HINT_Y      (EPD_HEIGHT - 14)   // hint line at the bottom (one row only)
#define ARROW_W     7                   // room at each side for the page arrows

typedef struct {
    int rows, cols;                     // grid
    int w, h, gap;                      // tile size and spacing
    int y;                              // top of the first row
} layout_t;

static layout_t tile;                   // the layout being drawn (draw_tile uses its size)

// The layout chosen on the web page (MENU_ROWS / MENU_COLS settings), clamped to what fits
static layout_t get_layout(void)
{
    layout_t l;
    l.rows = settings_get_i32(SETTING_MENU_ROWS, 2) >= 2 ? 2 : 1;
    l.cols = settings_get_i32(SETTING_MENU_COLS, 4);
    l.cols = l.cols < 3 ? 3 : l.cols > 5 ? 5 : l.cols;
    l.gap = l.rows == 1 ? 6 : 4;
    int room = UI_CONTENT_W - 2 * ARROW_W;
    l.w = (room - (l.cols - 1) * l.gap) / l.cols;
    if (l.w > 64) {
        l.w = 64;
    }
    if (l.rows == 1) {
        l.h = 64;
        l.y = 22;
    } else {
        l.h = (EPD_HEIGHT - 4 - l.gap) / 2;                 // 60 px each, 2 px margins
        l.y = 2;
    }
    return l;
}

static long drawn_minute = -1;          // minute (since 1970) the status bar shows; -1 = no time

// Draws text horizontally centered on cx
static void text_centered(int cx, int y, const char *text, int scale, uint8_t color)
{
    epd_draw_text(cx - epd_text_width(text, scale) / 2, y, text, scale, color);
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
    ui_sidebar_battery(UI_SIDEBAR_LEN, true);
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
    const int TILE_W = tile.w, TILE_H = tile.h;
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

    // A page of rows x cols tiles, filled row by row. Left / right walk the highlight across
    // the page; going past its last (or first) tile turns to the next (or previous) page.
    tile = get_layout();
    int per_page = tile.rows * tile.cols;
    int pages = (count + per_page - 1) / per_page;
    int page = selected / per_page, first = page * per_page;
    int on_page = count - first < per_page ? count - first : per_page;
    int cols_used = on_page < tile.cols ? on_page : tile.cols;   // a short single row is centred
    int grid_w = (pages > 1 ? tile.cols : cols_used) * tile.w + ((pages > 1 ? tile.cols : cols_used) - 1) * tile.gap;
    int x0 = UI_CONTENT_X + (UI_CONTENT_W - grid_w) / 2;
    int rows_used = (on_page + tile.cols - 1) / tile.cols;
    int y0 = tile.y + (tile.rows == 2 && rows_used == 1 && pages == 1 ? (tile.h + tile.gap) / 2 : 0);
    for (int i = 0; i < on_page; i++) {
        int r = i / tile.cols, c = i % tile.cols;
        draw_tile(x0 + c * (tile.w + tile.gap), y0 + r * (tile.h + tile.gap), apps[first + i], first + i == selected);
    }

    // Arrows when there are pages to either side
    if (pages > 1) {
        int mid_y = tile.y + (tile.rows * tile.h + (tile.rows - 1) * tile.gap) / 2 - 4;
        if (page > 0) {
            epd_draw_text(UI_CONTENT_X + 1, mid_y, "<", 1, EPD_BLACK);
        }
        if (page < pages - 1) {
            epd_draw_text(EPD_WIDTH - 6, mid_y, ">", 1, EPD_BLACK);
        }
    }

    if (tile.rows == 1) {
        char hint[64];
        if (pages > 1) {
            snprintf(hint, sizeof(hint), "< > choose   press: open   %d/%d", page + 1, pages);
        } else {
            snprintf(hint, sizeof(hint), "< > choose   press: open");
        }
        text_centered(UI_CONTENT_CX, HINT_Y, hint, 1, EPD_BLACK);
    }
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
