/*
 * Daily Chinese story: written by Claude once a day, cached in flash.
 *
 * Each day's story uses mostly words the reader already knows (their HSK level plus
 * every word earlier stories introduced) and introduces 5-10 new characters.
 * The "day" rolls over at CONFIG_STORY_RESET_HOUR local time, not at midnight.
 *
 * Files on the "storage" partition (mounted at /store):
 *   story.json  - today's story: first line is its day ("2026-10-27"), then the JSON from Claude
 *   known.txt   - every word an earlier story introduced, one per line
 *   review.txt  - words saved for review (see review.h)
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define STORY_MAX_SENTENCES 40
#define STORY_MAX_TOKENS    600

// One word or punctuation mark
typedef struct {
    char w[24];                         // the characters (UTF-8)
    char p[48];                         // pinyin with tone marks; empty for punctuation
    char d[64];                         // short English meaning; empty for punctuation
    bool is_new;                        // contains one of today's new characters
} story_token_t;

// A run of consecutive tokens: one sentence, or one screen page
typedef struct {
    int first;                          // index of the first token in tokens[]
    int count;                          // number of tokens
} story_span_t;

typedef struct {
    char day[11];                       // story day, "YYYY-MM-DD"
    char title[64];
    int sentence_count;
    story_span_t sentences[STORY_MAX_SENTENCES];
    int page_count;                     // pages = whole sentences grouped to fill the screen
    story_span_t pages[STORY_MAX_SENTENCES];
    int token_count;
    story_token_t tokens[STORY_MAX_TOKENS];
} story_t;

// Loads the cached story. Call once at startup, after storage_init() (the app's init hook).
void story_init(void);

// The cached story, or NULL if there isn't one yet. Only valid until the next story_accept().
const story_t *story_get(void);

// True if the cached story is today's. utc_offset = seconds to add to UTC for local time.
bool story_is_current(int utc_offset);

// True if an API key is set in menuconfig
bool story_has_api_key(void);

// Reader level (HSK 1-6) the stories are written for. Starts as CONFIG_STORY_HSK_LEVEL;
// story_set_hsk_level() (the story menu) overrides it and is remembered in flash.
int story_hsk_level(void);
void story_set_hsk_level(int level);

// Asks Claude for a story for the current story day (takes a minute or two). Network only,
// so it can run in a background task. Returns the story JSON (free() it) and fills
// day[11], or returns NULL on failure. Wi-Fi must be connected.
char *story_request(int utc_offset, char day[11]);

// Makes a story from story_request() the current one: caches it in flash and adds its new
// words to known.txt. Call from the main task (the one that draws the story).
esp_err_t story_accept(const char *json, const char *day);

// True for word tokens (the cursor skips punctuation)
bool story_is_word(const story_token_t *t);

// First (last = false) or last (last = true) word token on a page, or -1 if it has none
int story_page_word(const story_t *s, int page, bool last);

// Moves the cursor one word forward (dir = +1) or back (dir = -1), continuing onto the
// next/previous page at the ends. Updates *page and *token; returns false at the story's ends.
bool story_step_word(const story_t *s, int *page, int *token, int dir);
