/*
 * Lays out book text into screen pages: 6 lines of 16 px text, Chinese wrapped by
 * character, Latin text by word. The same function measures pages (for the page index)
 * and draws them, so the two can never disagree. A chapter marker line (BOOK_CHAPTER_MARK)
 * ends the page before it, so every chapter starts on a new page; the marker line itself is
 * never drawn.
 */
#pragma once

#include <stdbool.h>

#define READER_WINDOW 2048              // bytes of text handed to the layout; a page is far less

// Lays out one page from the start of text[0..len). text[len] must be '\0'. at_end = the
// text runs to the end of the book (otherwise the last few bytes are held back, in case
// they are part of a character cut off by the window). draw = also draw into the frame
// buffer. Returns the number of bytes the page uses (at least 1 if len > 0).
int reader_layout_page(const char *text, int len, bool at_end, bool draw);
