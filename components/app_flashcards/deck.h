/*
 * The flashcard deck: every saved word (components/review) plus its spaced-repetition
 * progress, kept in /store/flashcards.txt:
 *
 *   word <TAB> box <TAB> due day
 *
 * Leitner boxes: a new or forgotten word is in box 0 (due now). Each correct answer moves
 * it up a box and schedules it further out (see INTERVALS in deck.c); a wrong answer puts
 * it back in box 0. Days are counted in local time (days since 1970).
 */
#pragma once

#include <stdbool.h>

#define DECK_MAX   500                  // most cards the deck holds (matches the saved list)
#define DECK_BOXES 6                    // boxes 0..5

typedef struct {
    char w[24];                         // the word
    char p[48];                         // pinyin
    char d[64];                         // English meaning
    int box;                            // 0 = new / forgotten ... DECK_BOXES-1 = well known
    int due;                            // day it's next due (deck_today() numbering)
} card_t;

// Today's day number in local time, or -1 if the clock isn't set yet
int deck_today(void);

// Fills cards[] with every saved word and its progress. Returns how many.
// Words never reviewed are in box 0, due today.
int deck_load(card_t *cards, int max, int today);

// Writes the progress of all cards to flash (words no longer saved are dropped)
void deck_save(const card_t *cards, int count);

// True if the card should be reviewed today
bool deck_is_due(const card_t *c, int today);

// Records an answer: correct moves the card up a box and schedules it; wrong resets it
// to box 0 (still due today, so the session shows it again).
void deck_grade(card_t *c, bool correct, int today);
