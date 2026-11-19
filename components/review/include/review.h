/*
 * Words saved for review (SELECT in the story app), reviewed by the flashcards app.
 *
 * Stored in /store/review.txt, one word per line, tab-separated:
 *   word <TAB> pinyin <TAB> English meaning <TAB> day saved (YYYY-MM-DD)
 * The storage partition must already be mounted (storage_init(), called by main.c).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "storage.h"                    // STORAGE_PATH

#define REVIEW_FILE STORAGE_PATH "/review.txt"

// One saved word, as listed in the Saved words screen
typedef struct {
    char w[24];                         // the characters
    char p[48];                         // pinyin
    char d[64];                         // English meaning
    char day[11];                       // story day it was saved from
    bool saved;                         // false once un-saved from the list (kept on screen until it closes)
} review_entry_t;

// Reads up to `max` saved words, oldest first. Returns how many.
int review_load(review_entry_t *out, int max);

// Number of saved words
int review_count(void);

// True if `word` is already saved
bool review_contains(const char *word);

// Saves a word (does nothing if it's already saved)
esp_err_t review_add(const char *word, const char *pinyin, const char *meaning, const char *day);

// Un-saves a word (does nothing if it isn't saved)
esp_err_t review_remove(const char *word);
