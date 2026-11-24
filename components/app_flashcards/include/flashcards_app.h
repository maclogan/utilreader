/*
 * Flashcards: spaced-repetition review of the words saved in the Story app. Part of the
 * Story app ("Review cards" on its menu), not a menu tile of its own.
 * See components/app_flashcards/README.md.
 *
 *   Front: the word.            SELECT shows the answer.
 *   Back:  pinyin and meaning.  LEFT = again (seen again this session), RIGHT = got it.
 *   BACK leaves the review; progress is saved after every answer.
 */
#pragma once

#include <stdbool.h>
#include "buttons.h"                    // button_event_t

// Starts today's review session and shows its first screen (full refresh)
void flashcards_start(void);

// A button while the review is on screen. False on BACK: the caller shows its own screen
// again (progress is already saved).
bool flashcards_button(button_event_t event);

// Cards due today, for a menu label. -1 while the date isn't known (no internet time yet).
int flashcards_due_count(void);
