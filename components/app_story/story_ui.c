#include "story_ui.h"

#include <stdio.h>                      // snprintf()
#include "epd.h"                  // drawing primitives + display refresh
#include "font16.h"                     // Chinese / pinyin text
#include "review.h"                     // review_contains()
#include "ui_sidebar.h"                 // title bar on the left

// Layout (pixels). The screen is 296 wide x 128 tall; the title bar takes the left
// UI_SIDEBAR_W columns, the content has the full height.
#define MARGIN      4                   // space between the content and its edges
#define LEFT        (UI_CONTENT_X + MARGIN)  // left edge of text
#define TEXT_Y      3                   // top of the first text row
#define ROW_H       19                  // 16 px characters + room for the new-word underline
#define TEXT_ROWS   6                   // rows of text: the whole height (the lookup box floats over it)
#define BOX_H       44                  // the lookup box: pinyin and meaning, over the text
#define BOX_PAD     4

// True for Chinese punctuation, which may hang past the right margin rather than start a row
static bool is_punctuation(uint32_t cp)
{
    return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

// Story menu layout
#define ITEM_Y      4                   // top of the first item
#define ITEM_H      24                  // item spacing (5 items fit)
#define ITEM_BOX_H  22                  // highlight bar height

// Saved words layout: each entry is word + pinyin on one row, meaning below
#define SAVED_Y     13                  // top of the first entry
#define SAVED_H     34                  // entry height

// Draws text clipped at the right margin (meanings are short, but don't trust that)
static void draw_clipped(int x, int y, const char *text, uint8_t color)
{
    uint32_t cp;
    while ((cp = font16_next_char(&text)) != 0) {
        if (x + font16_char_width(cp) > EPD_WIDTH - MARGIN) {
            break;
        }
        x += font16_draw_char(x, y, cp, 1, color);
    }
}

// Small tag like "SAVED" in the opposite color of `color`, right edge at x. Returns its left edge.
static int draw_tag(int right, int y, const char *text, uint8_t color)
{
    int w = epd_text_width(text, 1) + 6;
    epd_draw_rect(right - w, y, w, 11, color, true);
    epd_draw_text(right - w + 3, y + 2, text, 1, !color);
    return right - w;
}

// Title bar on the left: title (UTF-8) from the bottom, small counter like "3/12" at the top
static void draw_header(const char *title, const char *counter)
{
    ui_sidebar(title, counter);
}

// Wakes the panel for a full refresh, or falls back to full if a partial isn't possible
static bool begin(bool full)
{
    if (!full && !epd_is_awake()) {
        full = true;                                        // partial refresh needs an awake panel
    }
    if (full) {
        epd_init();                                         // wake + fully reset the panel
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

// Lays out tokens first..first+count-1 as running text, wrapping by character.
// draw = true draws them (rows past TEXT_ROWS are cut off); draw = false only measures.
// Returns the number of rows the text needs. *cursor_row (may be NULL) gets the row the cursor
// word starts on (-1 if it isn't on this page).
static int layout(const story_t *s, int first, int count, int cursor, bool draw, int *cursor_row)
{
    int x = LEFT, row = 0;
    if (cursor_row) {
        *cursor_row = -1;
    }

    for (int i = first; i < first + count; i++) {
        const story_token_t *t = &s->tokens[i];
        bool highlighted = (i == cursor);
        const char *p = t->w;
        uint32_t cp;
        while ((cp = font16_next_char(&p)) != 0) {
            int w = font16_char_width(cp);
            if (x + w > EPD_WIDTH - MARGIN && !(is_punctuation(cp) && x + w <= EPD_WIDTH)) {
                x = LEFT;                                   // next row
                row++;
            }
            int y = TEXT_Y + row * ROW_H;
            if (highlighted && cursor_row && *cursor_row < 0) {
                *cursor_row = row;
            }
            if (!draw || row >= TEXT_ROWS) {
                x += w;                                     // measuring, or past the bottom
                continue;
            }
            if (highlighted) {                              // cursor: white on black
                epd_draw_rect(x, y - 1, w, FONT16_HEIGHT + 2, EPD_BLACK, true);
                font16_draw_char(x, y, cp, 1, EPD_WHITE);
            } else {
                font16_draw_char(x, y, cp, 1, EPD_BLACK);
                if (t->is_new) {
                    epd_draw_hline(x + 1, y + FONT16_HEIGHT + 1, w - 2, EPD_BLACK);  // underline new words
                }
            }
            x += w;
        }
    }
    return count > 0 ? row + 1 : 0;
}

void story_ui_paginate(story_t *s)
{
    s->page_count = 0;
    for (int i = 0; i < s->sentence_count; ) {
        // Start a page with sentence i, then add following sentences while they still fit
        int first = s->sentences[i].first;
        int count = s->sentences[i].count;
        for (i++; i < s->sentence_count; i++) {
            int with_next = s->sentences[i].first + s->sentences[i].count - first;
            if (layout(s, first, with_next, -1, false, NULL) > TEXT_ROWS) {
                break;
            }
            count = with_next;
        }
        s->pages[s->page_count++] = (story_span_t){first, count};
    }
}

void story_ui_show(const story_t *s, int page, int cursor, bool full)
{
    full = begin(full);

    char page_text[24];
    snprintf(page_text, sizeof(page_text), "%d/%d", page + 1, s->page_count);
    draw_header(s->title, page_text);

    int cursor_row;
    layout(s, s->pages[page].first, s->pages[page].count, cursor, true, &cursor_row);

    // The lookup box, only while a word is selected: pinyin and meaning in a bordered box over
    // the text, at the bottom, or at the top when the selected word would be under it
    if (cursor >= 0) {
        const story_token_t *t = &s->tokens[cursor];
        int word_top = TEXT_Y + (cursor_row < 0 ? 0 : cursor_row) * ROW_H - 1;
        int word_bottom = word_top + ROW_H + 1;
        int box_y = word_bottom > EPD_HEIGHT - BOX_H ? 0 : EPD_HEIGHT - BOX_H;
        (void)word_top;
        epd_draw_rect(UI_CONTENT_X, box_y, UI_CONTENT_W, BOX_H, EPD_WHITE, true);
        epd_draw_rect(UI_CONTENT_X, box_y, UI_CONTENT_W, BOX_H, EPD_BLACK, false);
        epd_draw_rect(UI_CONTENT_X + 1, box_y + 1, UI_CONTENT_W - 2, BOX_H - 2, EPD_BLACK, false);
        int pinyin_y = box_y + BOX_PAD, meaning_y = box_y + BOX_PAD + 19;
        int tag_x = EPD_WIDTH - MARGIN - 2;
        if (review_contains(t->w)) {
            tag_x = draw_tag(tag_x, pinyin_y + 3, "SAVED", EPD_BLACK) - 4;
        }
        if (t->is_new) {
            draw_tag(tag_x, pinyin_y + 3, "NEW", EPD_BLACK);
        }
        font16_draw_text(LEFT + 2, pinyin_y, t->p, EPD_BLACK);
        draw_clipped(LEFT + 2, meaning_y, t->d, EPD_BLACK);
    }

    finish(full);
}

void story_ui_show_menu(const char *const items[], int count, int selected, bool full)
{
    full = begin(full);
    draw_header("故事  Story", NULL);

    for (int i = 0; i < count; i++) {
        int y = ITEM_Y + i * ITEM_H;
        uint8_t color = EPD_BLACK;
        if (i == selected) {                                // highlight: white text on a black bar
            epd_draw_rect(UI_CONTENT_X, y, UI_CONTENT_W, ITEM_BOX_H, EPD_BLACK, true);
            color = EPD_WHITE;
        }
        epd_draw_text(LEFT + 6, y + (ITEM_BOX_H - 14) / 2, items[i], 2, color);
    }

    finish(full);
}

void story_ui_show_saved(const review_entry_t *entries, int count, int top, int cursor, bool full)
{
    full = begin(full);

    char counter[24];
    snprintf(counter, sizeof(counter), "%d/%d", count ? cursor + 1 : 0, count);
    draw_header("生词  Saved", counter);

    if (count == 0) {
        const char *line1 = "No saved words yet";
        const char *line2 = "Press on a word in a story to save it";
        epd_draw_text(UI_CONTENT_CX - epd_text_width(line1, 1) / 2, 50, line1, 1, EPD_BLACK);
        epd_draw_text(UI_CONTENT_CX - epd_text_width(line2, 1) / 2, 66, line2, 1, EPD_BLACK);
    }

    for (int i = 0; i < STORY_UI_SAVED_ROWS && top + i < count; i++) {
        const review_entry_t *e = &entries[top + i];
        int y = SAVED_Y + i * SAVED_H;
        uint8_t color = EPD_BLACK;
        if (top + i == cursor) {                            // highlight: white on black
            epd_draw_rect(UI_CONTENT_X, y - 1, UI_CONTENT_W, SAVED_H, EPD_BLACK, true);
            color = EPD_WHITE;
        }
        int x = font16_draw_text(LEFT, y, e->w, color);   // 可是  kěshì            REMOVED
        font16_draw_text(x + 8, y, e->p, color);            // but, however
        if (!e->saved) {
            draw_tag(EPD_WIDTH - MARGIN, y + 3, "REMOVED", color);
        }
        draw_clipped(LEFT, y + FONT16_HEIGHT, e->d, color);
    }

    finish(full);
}

void story_ui_show_message(const char *title, const char *detail)
{
    const char *hint = "BACK to go back";
    epd_init();
    epd_clear(EPD_WHITE);
    font16_draw_text((EPD_WIDTH - font16_text_width(title)) / 2, 34, title, EPD_BLACK);
    epd_draw_text((EPD_WIDTH - epd_text_width(detail, 1)) / 2, 64, detail, 1, EPD_BLACK);
    epd_draw_text((EPD_WIDTH - epd_text_width(hint, 1)) / 2, 100, hint, 1, EPD_BLACK);
    epd_display();
}

void story_ui_draw_app_icon(int cx, int cy)
{
    // 文 ("writing"), a little heavier than the plain glyph, in a rounded 2 px frame
    const int half = 21, r = 7;
    for (int t = 0; t < 2; t++) {
        int h = half - t;
        epd_draw_hline(cx - h + r, cy - h, 2 * (h - r) + 1, EPD_BLACK);   // straight edges
        epd_draw_hline(cx - h + r, cy + h, 2 * (h - r) + 1, EPD_BLACK);
        epd_draw_line(cx - h, cy - h + r, cx - h, cy + h - r, EPD_BLACK);
        epd_draw_line(cx + h, cy - h + r, cx + h, cy + h - r, EPD_BLACK);
        for (int sx = -1; sx <= 1; sx += 2) {               // rounded corners
            for (int sy = -1; sy <= 1; sy += 2) {
                int ccx = cx + sx * (half - r), ccy = cy + sy * (half - r);
                for (int dy = -r; dy <= r; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 >= (r - 1 - t) * (r - 1 - t) && d2 <= (r - t) * (r - t) + (r - t) &&
                            dx * sx >= 0 && dy * sy >= 0) {
                            epd_draw_pixel(ccx + dx, ccy + dy, EPD_BLACK);
                        }
                    }
                }
            }
        }
    }
    font16_draw_char(cx - 16, cy - 15, 0x6587, 2, EPD_BLACK);
    font16_draw_char(cx - 15, cy - 15, 0x6587, 2, EPD_BLACK);   // a pixel over: heavier strokes
}
