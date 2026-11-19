/*
 * Flashcards app: spaced-repetition review of the words saved in the story app.
 * See components/app_flashcards/README.md.
 *
 *   Front: the word.            SELECT shows the answer.
 *   Back:  pinyin and meaning.  LEFT = again (seen again this session), RIGHT = got it.
 *   BACK leaves the app; progress is saved after every answer.
 */
#pragma once

#include "app.h"

extern const app_t flashcards_app;
