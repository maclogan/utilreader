/*
 * Flashcards logic (part of the Story app): build today's session from the deck, show cards,
 * record answers.
 *
 * A session is a queue of the cards due today, shuffled. "Got it" removes the card from
 * the queue; "again" moves it to the back, so it comes round once more before the end.
 */
#include "flashcards_app.h"

#include <stdio.h>                      // snprintf()
#include "deck.h"                     // cards and scheduling
#include "esp_heap_caps.h"              // heap_caps_malloc(): the deck lives in PSRAM
#include "esp_log.h"
#include "esp_random.h"                 // esp_random(): shuffling
#include "flashcards_ui.h"              // drawing

static const char *TAG = "flashcards";

#define FULL_EVERY 10                   // full refresh every this many cards, to clear ghosting

static card_t *cards;                   // the whole deck (PSRAM, DECK_MAX entries)
static int card_count = 0;
static int queue[DECK_MAX];             // indexes into cards[] still to review this session
static int queue_len = 0;
static bool showing_back = false;       // answer visible
static int today = -1;
static int shown = 0;                   // cards shown since the last full refresh

// The card at the front of the queue
static card_t *current(void)
{
    return &cards[queue[0]];
}

static void show_current(bool full)
{
    if (!full && !showing_back && ++shown >= FULL_EVERY) {
        full = true;                                        // new card: time to clear ghosting
    }
    if (full) {
        shown = 0;
    }
    flashcards_ui_show_card(current(), showing_back, queue_len, full);
}

// What to show when there's nothing (left) to review
static void show_done(bool full)
{
    if (card_count == 0) {
        flashcards_ui_show_message("No cards yet", "Press on a word in a story to save it", full);
    } else {
        // When is the next card due?
        int next = -1;
        for (int i = 0; i < card_count; i++) {
            if (next < 0 || cards[i].due < next) {
                next = cards[i].due;
            }
        }
        char detail[48];
        int days = next - today;
        if (days <= 1) {
            snprintf(detail, sizeof(detail), "%d cards. Next review tomorrow", card_count);
        } else {
            snprintf(detail, sizeof(detail), "%d cards. Next review in %d days", card_count, days);
        }
        flashcards_ui_show_message("All done!", detail, full);
    }
}

// Loads the deck and queues every card due today, in random order
static void start_session(void)
{
    if (cards == NULL) {
        cards = heap_caps_malloc(DECK_MAX * sizeof(card_t), MALLOC_CAP_SPIRAM);
    }
    card_count = cards ? deck_load(cards, DECK_MAX, today) : 0;

    queue_len = 0;
    for (int i = 0; i < card_count; i++) {
        if (deck_is_due(&cards[i], today)) {
            queue[queue_len++] = i;
        }
    }
    for (int i = queue_len - 1; i > 0; i--) {               // Fisher-Yates shuffle
        int j = esp_random() % (i + 1);
        int t = queue[i];
        queue[i] = queue[j];
        queue[j] = t;
    }
    showing_back = false;
    ESP_LOGI(TAG, "%d of %d cards due", queue_len, card_count);
}

// Records the answer to the current card and moves on
static void answer(bool correct)
{
    card_t *c = current();
    deck_grade(c, correct, today);
    deck_save(cards, card_count);                           // progress survives power-off

    int index = queue[0];
    for (int i = 1; i < queue_len; i++) {                   // take it off the front...
        queue[i - 1] = queue[i];
    }
    queue_len--;
    if (!correct) {
        queue[queue_len++] = index;                         // ...and, if missed, put it at the back
    }

    showing_back = false;
    if (queue_len > 0) {
        show_current(false);
    } else {
        show_done(false);
    }
}

// ---------- entry points (the Story app's "Review cards") ----------

void flashcards_start(void)
{
    today = deck_today();
    if (today < 0) {
        flashcards_ui_show_message("Flashcards", "Waiting for internet time", true);
        return;
    }
    start_session();
    shown = 0;
    if (queue_len > 0) {
        show_current(true);
    } else {
        show_done(true);
    }
}

bool flashcards_button(button_event_t event)
{
    if (event == BUTTON_BACK) {
        return false;                                       // leave (progress is already saved)
    }
    if (today < 0 || queue_len == 0) {
        return true;                                        // a message is showing
    }

    if (!showing_back) {
        if (event == BUTTON_SELECT) {                       // flip
            showing_back = true;
            show_current(false);
        }
    } else if (event == BUTTON_LEFT) {
        answer(false);                                      // again
    } else if (event == BUTTON_RIGHT) {
        answer(true);                                       // got it
    }
    return true;
}

int flashcards_due_count(void)
{
    int day = deck_today();
    if (day < 0) {
        return -1;
    }
    if (cards == NULL) {
        cards = heap_caps_malloc(DECK_MAX * sizeof(card_t), MALLOC_CAP_SPIRAM);
    }
    int n = cards ? deck_load(cards, DECK_MAX, day) : 0, due = 0;
    for (int i = 0; i < n; i++) {
        due += deck_is_due(&cards[i], day);
    }
    return due;
}
