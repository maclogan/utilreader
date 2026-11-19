#include "flashcards_ui.h"

#include <stdio.h>                      // snprintf()
#include "epd.h"                  // drawing primitives + display refresh
#include "font16.h"                     // Chinese / pinyin text
#include "ui_sidebar.h"                 // title bar on the left

// Layout (pixels). The screen is 296 wide x 128 tall; the title bar takes the left
// UI_SIDEBAR_W columns.
#define MARGIN      4
#define AREA_TOP    0                   // card area above the hint
#define AREA_BOTTOM 110
#define HINT_Y      116                 // hint line (5x7 font)

// Wakes the panel for a full refresh, or falls back to full if a partial isn't possible
static bool begin(bool full)
{
    if (!full && !epd_is_awake()) {
        full = true;
    }
    if (full) {
        epd_init();
    }
    epd_clear(EPD_WHITE);
    return full;
}

static void finish(bool full)
{
    if (full) {
        epd_display();                                      // full refresh (~2 s, flashes)
    } else {
        epd_display_partial();                              // quick refresh, only changes update
    }
}

// Title bar on the left, with the number of cards still due at its top end
static void draw_header(int left)
{
    char counter[24];
    snprintf(counter, sizeof(counter), "%d left", left);
    ui_sidebar("字卡  Cards", left > 0 ? counter : NULL);
}

static void hint(const char *text)
{
    epd_draw_text(UI_CONTENT_CX - epd_text_width(text, 1) / 2, HINT_Y, text, 1, EPD_BLACK);
}

// Draws UTF-8 text centered in the content area at `scale` (1-3), top at y. Returns its height.
static int text_centered(int y, const char *text, int scale)
{
    int x = UI_CONTENT_CX - font16_text_width(text) * scale / 2;
    const char *p = text;
    uint32_t cp;
    while ((cp = font16_next_char(&p)) != 0) {
        x += font16_draw_char(x, y, cp, scale, EPD_BLACK);
    }
    return FONT16_HEIGHT * scale;
}

// Biggest scale (up to max) at which `text` fits across the content area
static int fit_scale(const char *text, int max)
{
    int w = font16_text_width(text);
    for (int s = max; s > 1; s--) {
        if (w * s <= UI_CONTENT_W - 2 * MARGIN) {
            return s;
        }
    }
    return 1;
}

void flashcards_ui_show_card(const card_t *c, bool back, int left, bool full)
{
    full = begin(full);
    draw_header(left);

    if (!back) {
        int scale = fit_scale(c->w, 3);                     // 48 px characters when they fit
        int h = FONT16_HEIGHT * scale;
        text_centered(AREA_TOP + (AREA_BOTTOM - AREA_TOP - h) / 2, c->w, scale);
        hint("Press: show answer");
    } else {
        int y = AREA_TOP + 12;
        y += text_centered(y, c->w, fit_scale(c->w, 2)) + 6;   // word at 2x
        y += text_centered(y, c->p, 1) + 4;                 // pinyin
        text_centered(y, c->d, 1);                          // meaning
        hint("< again                 got it >");
    }

    finish(full);
}

void flashcards_ui_show_message(const char *title, const char *detail, bool full)
{
    full = begin(full);
    draw_header(0);
    epd_draw_text(UI_CONTENT_CX - epd_text_width(title, 2) / 2, 42, title, 2, EPD_BLACK);
    epd_draw_text(UI_CONTENT_CX - epd_text_width(detail, 1) / 2, 69, detail, 1, EPD_BLACK);
    finish(full);
}

void flashcards_ui_draw_app_icon(int cx, int cy)
{
    epd_draw_rect(cx - 18, cy - 14, 36, 28, EPD_BLACK, false);  // back card...
    epd_draw_rect(cx - 15, cy - 17, 36, 28, EPD_WHITE, true);   // ...peeking out behind
    epd_draw_rect(cx - 15, cy - 17, 36, 28, EPD_BLACK, false);  // front card
    font16_draw_char(cx - 5, cy - 11, 0x5B57, 1, EPD_BLACK);    // 字 ("character")
}
