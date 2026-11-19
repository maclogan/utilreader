#include "flashcards_ui.h"

#include <stdio.h>                      // snprintf()
#include "epd_2in13.h"                  // drawing primitives + display refresh
#include "font16.h"                     // Chinese / pinyin text

// Layout (pixels). The screen is 250 wide x 122 tall.
#define MARGIN      4
#define HEADER_LINE 17                  // rule under the title
#define AREA_TOP    18                  // card area between the header and the hint
#define AREA_BOTTOM 104
#define HINT_Y      110                 // hint line (5x7 font)

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

static void draw_header(int left)
{
    font16_draw_text(MARGIN, 0, "字卡  Flashcards", EPD_BLACK);
    if (left > 0) {
        char counter[24];
        snprintf(counter, sizeof(counter), "%d left", left);
        epd_draw_text(EPD_WIDTH - MARGIN - epd_text_width(counter, 1), 5, counter, 1, EPD_BLACK);
    }
    epd_draw_hline(0, HEADER_LINE, EPD_WIDTH, EPD_BLACK);
}

static void hint(const char *text)
{
    epd_draw_text((EPD_WIDTH - epd_text_width(text, 1)) / 2, HINT_Y, text, 1, EPD_BLACK);
}

// Draws UTF-8 text centered on the screen at `scale` (1-3), top at y. Returns its height.
static int text_centered(int y, const char *text, int scale)
{
    int x = (EPD_WIDTH - font16_text_width(text) * scale) / 2;
    const char *p = text;
    uint32_t cp;
    while ((cp = font16_next_char(&p)) != 0) {
        x += font16_draw_char(x, y, cp, scale, EPD_BLACK);
    }
    return FONT16_HEIGHT * scale;
}

// Biggest scale (up to max) at which `text` fits across the screen
static int fit_scale(const char *text, int max)
{
    int w = font16_text_width(text);
    for (int s = max; s > 1; s--) {
        if (w * s <= EPD_WIDTH - 2 * MARGIN) {
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
        hint("SELECT: show answer");
    } else {
        int y = AREA_TOP + 6;
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
    epd_draw_text((EPD_WIDTH - epd_text_width(title, 2)) / 2, 45, title, 2, EPD_BLACK);
    epd_draw_text((EPD_WIDTH - epd_text_width(detail, 1)) / 2, 72, detail, 1, EPD_BLACK);
    finish(full);
}

void flashcards_ui_draw_app_icon(int cx, int cy)
{
    epd_draw_rect(cx - 18, cy - 14, 36, 28, EPD_BLACK, false);  // back card...
    epd_draw_rect(cx - 15, cy - 17, 36, 28, EPD_WHITE, true);   // ...peeking out behind
    epd_draw_rect(cx - 15, cy - 17, 36, 28, EPD_BLACK, false);  // front card
    font16_draw_char(cx - 5, cy - 11, 0x5B57, 1, EPD_BLACK);    // 字 ("character")
}
