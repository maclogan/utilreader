/*
 * Reader screens.
 *
 *  Lists have their title ("Books", "Chapters") and "n/total" in the title bar on the left
 *  (ui_sidebar.h), reading bottom to top; drawn here as a top line for brevity.
 *
 *  LIBRARY                                     PAGE
 *  +--------------------------------------+    +--------------------------------------+
 *  | Books                            2/5 |    | 第一章                                |
 *  |--------------------------------------|    | 　　天色渐渐暗了下来，街上的人越来越 |
 *  | 三体                             12% |    | 少。她站在窗前，看着远处的灯光一盏一 |
 *  |######################################|    | 盏亮起来……                           |
 *  |#Pride and Prejudice              4%#|    |                                      |
 *  |######################################|    |                                      |
 *  | 活着                              0% |    |                                      |
 *  | Add books with the Transfer app      |    | Ch 3/24  45%              12/340 12% |
 *  +--------------------------------------+    +--------------------------------------+
 *
 *  CHAPTERS (press while reading)
 *  +--------------------------------------+
 *  | Chapters                        3/24 |
 *  |--------------------------------------|
 *  | 第二章 雨夜                      p31 |
 *  |######################################|
 *  |#第三章 来信                   * p58#|   <- * = the chapter you're in
 *  ...
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "books.h"                      // book_info_t

#define READER_UI_ROWS 5                // one-line rows per list screen (holding left / right jumps this far)

// One chapter: where it starts and its title (stored as-is in <ID>.CHP)
typedef struct {
    uint32_t page;                      // index of the page it starts on
    char title[60];                     // UTF-8
} reader_chapter_t;

// Lists: a title that doesn't fit wraps onto a second line; one that doesn't fit on two
// ends in "…", except on the highlighted row, which is drawn scrolled left by `marquee`
// characters. Both return how many characters the highlighted row can scroll (0 = it fits).

// The book list with `selected` highlighted. percents[i] = reading progress of list[i].
int reader_ui_library(const book_info_t *list, const int *percents, int count, int selected,
                      int marquee, bool full);

// One page of text (see reader_layout.h for text/len/at_end), with footer text at the
// bottom left and right (ASCII)
void reader_ui_page(const char *text, int len, bool at_end, const char *left, const char *right, bool full);

// The chapter list with `selected` highlighted and `current` (the chapter being read) marked
int reader_ui_chapters(const reader_chapter_t *list, int count, int selected, int current,
                       int marquee, bool full);

// Full-screen message: a UTF-8 title and a line of ASCII detail
void reader_ui_message(const char *title, const char *detail);

// Menu icon: an open book
void reader_ui_draw_app_icon(int cx, int cy);
