#include "deck.h"

#include <stdio.h>                      // fopen(), fgets(), fprintf()
#include <stdlib.h>                     // free(), strtol()
#include <string.h>                     // strcmp(), strcspn()
#include <time.h>                       // time()
#include "esp_heap_caps.h"              // heap_caps_malloc(): temporary list in PSRAM
#include "esp_log.h"
#include "local_time.h"                 // local_time_synced(), local_time_offset()
#include "review.h"                     // the saved words
#include "storage.h"                    // STORAGE_PATH

static const char *TAG = "deck";

#define PROGRESS_FILE STORAGE_PATH "/flashcards.txt"

// Days until a card is due again after a correct answer, by the box it moves into
static const int INTERVALS[DECK_BOXES] = {0, 1, 3, 7, 14, 30};

int deck_today(void)
{
    if (!local_time_synced()) {
        return -1;
    }
    return (int)((time(NULL) + local_time_offset()) / 86400);
}

int deck_load(card_t *cards, int max, int today)
{
    // 1. The saved words
    review_entry_t *saved = heap_caps_malloc(max * sizeof(review_entry_t), MALLOC_CAP_SPIRAM);
    if (saved == NULL) {
        return 0;
    }
    int count = review_load(saved, max);
    for (int i = 0; i < count; i++) {
        card_t *c = &cards[i];
        snprintf(c->w, sizeof(c->w), "%s", saved[i].w);
        snprintf(c->p, sizeof(c->p), "%s", saved[i].p);
        snprintf(c->d, sizeof(c->d), "%s", saved[i].d);
        c->box = 0;                                         // never reviewed: new, due today
        c->due = today;
    }
    free(saved);

    // 2. Their progress, matched by word (lines for words no longer saved are ignored)
    FILE *f = fopen(PROGRESS_FILE, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char *tab = strchr(line, '\t');
            if (tab == NULL) {
                continue;
            }
            *tab = '\0';
            char *end;
            int box = (int)strtol(tab + 1, &end, 10);
            int due = (int)strtol(end, NULL, 10);
            for (int i = 0; i < count; i++) {
                if (strcmp(cards[i].w, line) == 0) {
                    cards[i].box = (box >= 0 && box < DECK_BOXES) ? box : 0;
                    cards[i].due = due;
                    break;
                }
            }
        }
        fclose(f);
    }
    ESP_LOGI(TAG, "%d cards", count);
    return count;
}

void deck_save(const card_t *cards, int count)
{
    FILE *f = fopen(PROGRESS_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "can't write %s", PROGRESS_FILE);
        return;
    }
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s\t%d\t%d\n", cards[i].w, cards[i].box, cards[i].due);
    }
    fclose(f);
}

bool deck_is_due(const card_t *c, int today)
{
    return c->due <= today;
}

void deck_grade(card_t *c, bool correct, int today)
{
    if (correct) {
        if (c->box < DECK_BOXES - 1) {
            c->box++;
        }
        c->due = today + INTERVALS[c->box];
    } else {
        c->box = 0;
        c->due = today;                                     // again this session
    }
}
