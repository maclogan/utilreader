#include "reader_layout.h"

#include "books.h"                      // BOOK_CHAPTER_MARK
#include "epd.h"                  // EPD_WIDTH, drawing
#include "font16.h"                     // glyphs and widths
#include "image.h"                      // picture pages
#include "esp_log.h"
#include "settings.h"                   // SETTING_ROTATE_TALL
#include <stdio.h>                      // snprintf()
#include <stdlib.h>                     // atoi()

#define X0          4                   // left margin
#define Y0          2                   // top of the first line
#define LINE_H      18                  // 16 px glyphs + 2 px gap
#define LINES       6                   // lines per page (the footer sits below)
#define TEXT_W      (EPD_WIDTH - 2 * X0)
#define SPACE_W     8                   // width of a space between Latin words
#define CJK_FROM    0x2E80              // from here on characters wrap one at a time
#define PICTURE_H   (LINES * LINE_H)    // picture pages: the text area, above the footer

// Chinese punctuation may hang into the right margin instead of starting a line
static bool hangs(uint32_t cp)
{
    return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

// ---------- picture pages ----------

static char book_id[BOOK_ID_LEN + 1];

void reader_layout_set_book(const char *id)
{
    snprintf(book_id, sizeof(book_id), "%s", id);
}

// Skips a chapter-title line at the start, if any; returns where the content starts
static int content_start(const char *text, int len)
{
    int pos = 0;
    if (len > 0 && text[0] == BOOK_CHAPTER_MARK) {
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        if (pos < len) {
            pos++;
        }
    }
    return pos;
}

bool reader_layout_is_picture(const char *text, int len)
{
    int pos = content_start(text, len);
    return pos < len && text[pos] == BOOK_PICTURE_MARK;
}

// Draws picture `n` of the book, centred above the footer, in 4 greys
static void draw_picture(int n)
{
    char ext[8], path[200];
    snprintf(ext, sizeof(ext), "I%02u", (unsigned)n % BOOK_PICTURES_MAX);
    books_extra_path(book_id, ext, path, sizeof(path));
    // Tall pictures (covers) turned a quarter turn when that at least doubles their size, the
    // same way as the title bar (Settings > Pictures)
    const int box_w = EPD_WIDTH - 2 * X0;
    int w, h;
    bool setting = settings_get_i32(SETTING_ROTATE_TALL, 1);
    bool sized = image_jpeg_size_file(path, &w, &h);
    bool turn = setting && sized && image_should_rotate(w, h, box_w, PICTURE_H);
    ESP_LOGI("reader", "picture %d: %dx%d%s, %s", n, w, h, sized ? "" : " (size unreadable)",
             turn ? "turned" : setting ? "upright (turning wouldn't make it much bigger)" : "upright (Settings: always upright)");
    image_grey_t img;
    if (!image_jpeg_grey_file(path, turn ? PICTURE_H : box_w, turn ? box_w : PICTURE_H, &img)) {
        font16_draw_text(X0, Y0 + 2 * LINE_H, "[picture]", EPD_BLACK);
        return;
    }
    if (turn) {
        image_rotate_ccw(&img);
    }
    image_enlarge_to_fit(&img, box_w, PICTURE_H);           // pictures stored small (older uploads)
    image_auto_levels(&img);
    epd_draw_grey8((EPD_WIDTH - img.w) / 2, (PICTURE_H - img.h) / 2 + 1, img.w, img.h, img.pix, img.w, true);
    image_free(&img);
}

int reader_layout_page(const char *text, int len, bool at_end, bool draw)
{
    int limit = at_end ? len : len - 4;                     // keep clear of a cut-off character
    int pos = 0, x = 0, line = 0;
    bool just_wrapped = false;                              // an automatic line break, nothing drawn since
    int first = content_start(text, limit);                 // after a chapter-title line

    if (first < limit && text[first] == BOOK_PICTURE_MARK) { // a picture: the page is just that line
        pos = first + 1;
        int n = atoi(text + pos);
        while (pos < limit && text[pos] != '\n') {
            pos++;
        }
        if (pos < limit) {
            pos++;
        }
        if (draw) {
            draw_picture(n);
        }
        return pos;
    }

#define NEW_LINE()  do { line++; x = 0; just_wrapped = true; } while (0)
#define DRAW(cp)    do { if (draw) font16_draw_char(X0 + x, Y0 + line * LINE_H, (cp), 1, EPD_BLACK); } while (0)

    while (pos < limit && line < LINES) {
        unsigned char b = text[pos];

        if (b == BOOK_PICTURE_MARK && pos > first) {        // a picture: on a page of its own
            break;
        }
        if (b == BOOK_CHAPTER_MARK) {                       // chapter-title line (not drawn)
            if (pos > 0) {
                break;                                      // a chapter always starts a new page
            }
            while (pos < limit && text[pos] != '\n') {      // at the top of a page: skip the line
                pos++;
            }
            if (pos < limit) {
                pos++;
            }
            continue;
        }
        if (b == '\n') {                                    // end of paragraph
            if (!(just_wrapped && x == 0)) {                // (not if the line just wrapped anyway)
                line++;
            }
            x = 0;
            just_wrapped = false;
            pos++;
            continue;
        }
        if (b == ' ') {
            pos++;
            if (x == 0) {
                continue;                                   // no spaces at the start of a line
            }
            if (x + SPACE_W > TEXT_W) {
                NEW_LINE();
            } else {
                x += SPACE_W;
            }
            continue;
        }

        const char *p = text + pos;
        uint32_t cp = font16_next_char(&p);
        if (cp >= CJK_FROM) {                               // Chinese and fullwidth: one character at a time
            int w = font16_char_width(cp);
            if (x + w > TEXT_W && !(hangs(cp) && X0 + x + w <= EPD_WIDTH)) {
                NEW_LINE();
                if (line >= LINES) {
                    break;
                }
            }
            DRAW(cp);
            x += w;
            just_wrapped = false;
            pos = p - text;
            continue;
        }

        // Latin word: everything up to the next space, newline or Chinese character
        int end = pos, word_w = 0;
        while (end < limit && text[end] != ' ' && text[end] != '\n') {
            const char *q = text + end;
            uint32_t c = font16_next_char(&q);
            if (c >= CJK_FROM) {
                break;
            }
            word_w += font16_char_width(c);
            end = q - text;
        }
        if (x > 0 && x + word_w > TEXT_W) {                 // doesn't fit: start it on the next line
            NEW_LINE();
            if (line >= LINES) {
                break;
            }
        }
        while (pos < end) {                                 // draw it, breaking words longer than a line
            const char *q = text + pos;
            uint32_t c = font16_next_char(&q);
            int w = font16_char_width(c);
            if (x + w > TEXT_W) {
                NEW_LINE();
                if (line >= LINES) {
                    break;
                }
            }
            DRAW(c);
            x += w;
            just_wrapped = false;
            pos = q - text;
        }
    }
#undef NEW_LINE
#undef DRAW

    return (pos == 0 && len > 0) ? 1 : pos;                 // always make progress
}
