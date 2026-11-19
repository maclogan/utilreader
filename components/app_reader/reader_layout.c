#include "reader_layout.h"

#include "books.h"                      // BOOK_CHAPTER_MARK
#include "epd_2in13.h"                  // EPD_WIDTH, drawing
#include "font16.h"                     // glyphs and widths

#define X0          4                   // left margin
#define Y0          2                   // top of the first line
#define LINE_H      18                  // 16 px glyphs + 2 px gap
#define LINES       6                   // lines per page (the footer sits below)
#define TEXT_W      (EPD_WIDTH - 2 * X0)
#define SPACE_W     8                   // width of a space between Latin words
#define CJK_FROM    0x2E80              // from here on characters wrap one at a time

// Chinese punctuation may hang into the right margin instead of starting a line
static bool hangs(uint32_t cp)
{
    return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

int reader_layout_page(const char *text, int len, bool at_end, bool draw)
{
    int limit = at_end ? len : len - 4;                     // keep clear of a cut-off character
    int pos = 0, x = 0, line = 0;
    bool just_wrapped = false;                              // an automatic line break, nothing drawn since

#define NEW_LINE()  do { line++; x = 0; just_wrapped = true; } while (0)
#define DRAW(cp)    do { if (draw) font16_draw_char(X0 + x, Y0 + line * LINE_H, (cp), 1, EPD_BLACK); } while (0)

    while (pos < limit && line < LINES) {
        unsigned char b = text[pos];

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
