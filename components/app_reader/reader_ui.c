#include "reader_ui.h"

#include <stdio.h>                      // snprintf()
#include <string.h>                     // strlen()
#include "epd.h"                  // drawing primitives + display refresh
#include "font16.h"                     // titles and text
#include "reader_layout.h"
#include "ui_sidebar.h"                 // title bar on the left (lists)

#define MARGIN      4
#define FOOTER_Y    (EPD_HEIGHT - 9)    // 5x7 footer line
#define LEFT        (UI_CONTENT_X + MARGIN)  // left edge of list text (right of the title bar)

static bool begin(bool full)
{
    if (!full && !epd_is_awake()) {
        full = true;                                        // partial refresh needs an awake panel
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
        epd_display();
    } else {
        epd_display_partial();
    }
}

static void text_centered(int y, const char *text)
{
    epd_draw_text((EPD_WIDTH - epd_text_width(text, 1)) / 2, y, text, 1, EPD_BLACK);
}

// Centered in the content area right of the title bar
static void content_centered(int y, const char *text)
{
    epd_draw_text(UI_CONTENT_CX - epd_text_width(text, 1) / 2, y, text, 1, EPD_BLACK);
}

// Title bar on the left: title from the bottom, "n/total" at the top
static void draw_header(const char *title, int selected, int count)
{
    char counter[24];
    snprintf(counter, sizeof(counter), "%d/%d", selected + 1, count);
    ui_sidebar(title, count > 0 ? counter : NULL);
}

// ---------- lists ----------
// Each row shows a title and a short tag on the right. A title that doesn't fit on one line
// wraps onto a second; one that doesn't fit on two stays on one line, ending in "…", and
// when it's the highlighted row it scrolls instead (`marquee` = characters scrolled off).
// Rows are 1 or 2 lines tall, so how many fit on a screen varies.

#define LIST_TOP    2                   // top of the first row
#define LIST_BOTTOM (FOOTER_Y - 3)      // rows end above the footer
#define ROW_H1      23                  // one-line row
#define ROW_H2      41                  // two-line row
#define ELLIPSIS    0x2026              // "…"

// Gets entry i's title and tag (tag[16], ASCII)
typedef void (*list_item_fn)(const void *ctx, int i, const char **title, char *tag);

typedef enum { FIT_ONE, FIT_TWO, FIT_LONG } fit_t;

static int text_width(const char *t)
{
    return font16_text_width(t);
}

// Byte offset where a line of at most `width` px should end: after the last space that
// fits, or after the last character that fits if there's no space
static int wrap_point(const char *text, int width)
{
    const char *p = text;
    int x = 0, last_space = -1, last_fit = 0;
    uint32_t cp;
    while ((cp = font16_next_char(&p)) != 0) {
        int w = font16_char_width(cp);
        if (x + w > width) {
            break;
        }
        x += w;
        last_fit = p - text;
        if (cp == ' ') {
            last_space = last_fit;
        }
    }
    return last_space > 0 ? last_space : last_fit;
}

static const char *skip_spaces(const char *t)
{
    while (*t == ' ') {
        t++;
    }
    return t;
}

static fit_t fit_title(const char *title, int width, int *split)
{
    if (text_width(title) <= width) {
        return FIT_ONE;
    }
    *split = wrap_point(title, width);
    return text_width(skip_spaces(title + *split)) <= width ? FIT_TWO : FIT_LONG;
}

static int title_width(const char *tag)
{
    return EPD_WIDTH - LEFT - MARGIN - epd_text_width(tag, 1) - 6;
}

static int row_height(const void *ctx, list_item_fn item, int i)
{
    const char *title;
    char tag[16];
    int split;
    item(ctx, i, &title, tag);
    return fit_title(title, title_width(tag), &split) == FIT_TWO ? ROW_H2 : ROW_H1;
}

// First entry on the screen holding `selected`: screens fill top to bottom in order
static int screen_start(const void *ctx, list_item_fn item, int selected)
{
    int start = 0, y = 0;
    for (int i = 0; i <= selected; i++) {
        int h = row_height(ctx, item, i);
        if (y > 0 && y + h > LIST_BOTTOM - LIST_TOP) {
            start = i;                                      // doesn't fit: next screen
            y = 0;
        }
        y += h;
    }
    return start;
}

// Draws text[0..bytes) from (x, y), stopping before right_edge. With `ellipsis`, text that
// doesn't fit is cut one character earlier and ends in "…".
static void draw_line(int x, int y, const char *text, int bytes, int right_edge, bool ellipsis, uint8_t color)
{
    const char *p = text, *end = text + bytes;
    bool cut = false;
    if (ellipsis) {
        int w = 0;                                          // does it all fit?
        for (const char *q = text; q < end; ) {
            w += font16_char_width(font16_next_char(&q));
        }
        if (x + w > right_edge) {
            cut = true;
            right_edge -= font16_char_width(ELLIPSIS);      // leave room for the "…"
        }
    }
    while (p < end) {
        const char *before = p;
        uint32_t cp = font16_next_char(&p);
        if (cp == 0 || x + font16_char_width(cp) > right_edge) {
            p = before;
            break;
        }
        x += font16_draw_char(x, y, cp, 1, color);
    }
    if (cut) {
        font16_draw_char(x, y, ELLIPSIS, 1, color);
    }
}

// Characters a too-long title can scroll before its end is in view
static int marquee_steps(const char *title, int width)
{
    int steps = 0;
    const char *p = title;
    while (*p && text_width(p) > width) {
        font16_next_char(&p);
        steps++;
    }
    return steps;
}

// Draws the screen of rows holding `selected`. Returns how many characters the selected
// row can scroll (0 = it fits, no marquee).
static int draw_list(const void *ctx, list_item_fn item, int count, int selected, int marquee)
{
    int steps = 0;
    int y = LIST_TOP;
    for (int i = screen_start(ctx, item, selected); i < count; i++) {
        const char *title;
        char tag[16];
        item(ctx, i, &title, tag);
        int width = title_width(tag), split = 0;
        fit_t fit = fit_title(title, width, &split);
        int h = fit == FIT_TWO ? ROW_H2 : ROW_H1;
        if (y + h > LIST_BOTTOM + 1) {
            break;                                          // screen full
        }
        bool sel = i == selected;
        uint8_t color = sel ? EPD_WHITE : EPD_BLACK;
        if (sel) {
            epd_draw_rect(UI_CONTENT_X, y, UI_CONTENT_W, h - 1, EPD_BLACK, true);   // highlight: white on black
        }
        epd_draw_text(EPD_WIDTH - MARGIN - epd_text_width(tag, 1), y + 8, tag, 1, color);

        int right = LEFT + width;
        if (fit == FIT_ONE) {
            draw_line(LEFT, y + 3, title, strlen(title), right, false, color);
        } else if (fit == FIT_TWO) {
            draw_line(LEFT, y + 3, title, split, right, false, color);
            const char *rest = skip_spaces(title + split);
            draw_line(LEFT, y + 3 + 18, rest, strlen(rest), right, false, color);
        } else if (sel) {                                   // too long and highlighted: scroll
            steps = marquee_steps(title, width);
            const char *p = title;
            for (int k = 0; k < marquee && *p; k++) {
                font16_next_char(&p);
            }
            draw_line(LEFT, y + 3, p, strlen(p), right, false, color);
        } else {                                            // too long: cut with "…"
            draw_line(LEFT, y + 3, title, strlen(title), right, true, color);
        }
        y += h;
    }
    return steps;
}

static void library_item(const void *ctx, int i, const char **title, char *tag)
{
    const book_info_t *list = ((const void **)ctx)[0];
    const int *percents = ((const void **)ctx)[1];
    *title = list[i].title;
    snprintf(tag, 16, "%d%%", percents[i]);
}

int reader_ui_library(const book_info_t *list, const int *percents, int count, int selected,
                      int marquee, bool full)
{
    full = begin(full);
    draw_header("Books", selected, count);
    if (count == 0) {
        content_centered(34, "No books yet");
        content_centered(52, "Best way to add them: open Transfer,");
        content_centered(64, "then \"Add a book\" on its web page");
        content_centered(76, "(keeps covers, pictures and chapters).");
        content_centered(94, ".epub/.txt files put in the Books folder");
        content_centered(106, "also work, but text only.");
    }
    const void *ctx[2] = {list, percents};
    int steps = draw_list(ctx, library_item, count, selected, marquee);
    if (count > 0) {
        content_centered(FOOTER_Y, "Add books: Transfer > web page > Add a book");
    }
    finish(full);
    return steps;
}

void reader_ui_page(const char *text, int len, bool at_end, const char *left, const char *right, bool full)
{
    if (reader_layout_is_picture(text, len)) {
        full = true;                    // grey needs a full refresh (and the page after clears it)
    }
    full = begin(full);
    reader_layout_page(text, len, at_end, true);
    epd_draw_text(MARGIN, FOOTER_Y, left, 1, EPD_BLACK);
    epd_draw_text(EPD_WIDTH - MARGIN - epd_text_width(right, 1), FOOTER_Y, right, 1, EPD_BLACK);
    finish(full);
}

typedef struct {
    const reader_chapter_t *list;
    int current;
} chapters_ctx_t;

static void chapter_item(const void *ctx, int i, const char **title, char *tag)
{
    const chapters_ctx_t *c = ctx;
    *title = c->list[i].title[0] ? c->list[i].title : "(untitled)";
    snprintf(tag, 16, "%sp%lu", i == c->current ? "* " : "",          // "* " = the chapter being read
             (unsigned long)c->list[i].page + 1);
}

int reader_ui_chapters(const reader_chapter_t *list, int count, int selected, int current,
                       int marquee, bool full)
{
    full = begin(full);
    draw_header("Chapters", selected, count);
    chapters_ctx_t ctx = {list, current};
    int steps = draw_list(&ctx, chapter_item, count, selected, marquee);
    content_centered(FOOTER_Y, "Press: go to chapter   BACK: back");
    finish(full);
    return steps;
}

void reader_ui_message(const char *title, const char *detail)
{
    epd_init();
    epd_clear(EPD_WHITE);
    font16_draw_text((EPD_WIDTH - font16_text_width(title)) / 2, 40, title, EPD_BLACK);
    text_centered(66, detail);
    text_centered(100, "BACK to go back");
    epd_display();
}

void reader_ui_draw_app_icon(int cx, int cy)
{
    // An open book in solid black (readable from across the room): two pages rising from the
    // spine to their outer corners, a white gutter between them, white lines of "text"
    const int half = 17, top_out = -12, top_in = -8, bot_out = 10, bot_in = 14;
    for (int i = 1; i <= half; i++) {                       // i = distance from the spine
        int top = cy + top_in + (top_out - top_in) * i / half;
        int bot = cy + bot_in + (bot_out - bot_in) * i / half;
        epd_draw_line(cx - i, top, cx - i, bot, EPD_BLACK); // left page
        epd_draw_line(cx + i, top, cx + i, bot, EPD_BLACK); // right page
    }
    for (int k = 0; k < 3; k++) {                           // text lines, 2 px thick, following the page tilt
        int y = cy - 5 + k * 6;
        for (int t = 0; t < 2; t++) {
            epd_draw_line(cx - 4, y + t, cx - 14, y - 3 + t, EPD_WHITE);
            epd_draw_line(cx + 4, y + t, cx + 14, y - 3 + t, EPD_WHITE);
        }
    }
}
