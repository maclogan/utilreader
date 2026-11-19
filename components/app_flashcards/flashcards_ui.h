/*
 * Flashcard screens.
 *
 *  FRONT                                     BACK
 *  +----------------------------------+      +----------------------------------+
 *  | 字卡  Flashcards          5 left |      | 字卡  Flashcards          5 left |
 *  |----------------------------------|      |----------------------------------|
 *  |                                  |      |              院子                |  <- 2x
 *  |            院  子                |      |             yuànzi               |
 *  |                                  |      |          courtyard, yard         |
 *  |       SELECT: show answer        |      |  < again               got it >  |
 *  +----------------------------------+      +----------------------------------+
 *        word as big as fits (3x)
 */
#pragma once

#include <stdbool.h>
#include "deck.h"                       // card_t

// One card. back = false shows just the word; true adds the pinyin, meaning and grading hint.
// left = cards still to go in this session (including this one).
// full = true for a full refresh (opening the app, or now and then against ghosting).
void flashcards_ui_show_card(const card_t *c, bool back, int left, bool full);

// Full-screen message with a title and one line of detail (ASCII), e.g. "All done".
void flashcards_ui_show_message(const char *title, const char *detail, bool full);

// Menu icon: 字 on a card outline, centered on (cx, cy)
void flashcards_ui_draw_app_icon(int cx, int cy);
