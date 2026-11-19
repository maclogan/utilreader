/*
 * Story app screen: as many whole sentences as fit in 3 rows, with the highlighted word
 * looked up underneath.
 *
 *  +-----------------------------------------+
 *  | 小猫的早上                           3/6 |  <- title, page number
 *  |-----------------------------------------|
 *  | 今天早上，[小猫]在窗户边上看鸟。它很    |  <- running text, 16 px characters, wrapped;
 *  | 想出去玩，可是外面在下雨。              |     cursor word inverted, new words underlined
 *  |                                         |
 *  |-----------------------------------------|
 *  | xiǎomāo                      NEW  SAVED |  <- pinyin, tags
 *  | kitten                                  |  <- meaning
 *  +-----------------------------------------+
 */
#pragma once

#include <stdbool.h>
#include "review.h"                     // review_entry_t
#include "story.h"

// Groups the story's sentences into pages (fills s->pages / s->page_count). A sentence
// never splits across pages; one too long for a page gets a page to itself, cut off at the bottom.
void story_ui_paginate(story_t *s);

// Draws page `page` of the story with word token `cursor` highlighted (-1 = none).
// full = true: full refresh (opening the app, or now and then to clear ghosting).
// full = false: quick partial refresh (cursor moved, page turned, word saved).
void story_ui_show(const story_t *s, int page, int cursor, bool full);

// The story app's own menu: a vertical list of ASCII labels with `selected` highlighted
void story_ui_show_menu(const char *const items[], int count, int selected, bool full);

// Saved words, STORY_UI_SAVED_ROWS per screen starting at entry `top`, `cursor` highlighted:
//   可是  kěshì
//   but, however
#define STORY_UI_SAVED_ROWS 3
void story_ui_show_saved(const review_entry_t *entries, int count, int top, int cursor, bool full);

// Full-screen message, e.g. while the story is being written, with an "EXIT to go back"
// hint. title is UTF-8 (Chinese OK), detail is plain ASCII.
void story_ui_show_message(const char *title, const char *detail);

// Draws the story app's icon (文) centered on (cx, cy) into the frame buffer, for the menu
void story_ui_draw_app_icon(int cx, int cy);
