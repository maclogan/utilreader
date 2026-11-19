/*
 * Story app logic: its menu, the reader, the saved-words list, and background writing.
 * Stories are written on the worker (story_job), then applied in the main task (story_job_done).
 */
#include "story_app.h"

#include <stdint.h>                     // intptr_t
#include <stdio.h>                      // snprintf()
#include <stdlib.h>                     // calloc(), free()
#include "esp_heap_caps.h"              // heap_caps_malloc(): the saved-words list lives in PSRAM
#include "esp_log.h"
#include "local_time.h"                 // local_time_offset(), local_time_synced()
#include "review.h"                     // saved words
#include "story.h"                      // the story itself
#include "story_ui.h"                   // drawing
#include "worker.h"                     // background story writing

static const char *TAG = "story_app";

#define RETRY_TICKS pdMS_TO_TICKS(15 * 60 * 1000)  // after a failed story request, try again this soon
#define FULL_EVERY  8                   // full refresh every this many page turns, to clear ghosting
#define SAVED_MAX   500                 // most saved words the list can show

typedef enum {
    VIEW_MENU,                          // Today's story / Saved words / New story / Level
    VIEW_READER,                        // the story
    VIEW_SAVED,                         // saved words list
    VIEW_WRITING,                       // waiting for a story to be written
} view_t;

enum { ITEM_TODAY, ITEM_SAVED, ITEM_NEW, ITEM_LEVEL, ITEM_COUNT };  // story menu entries

static view_t view = VIEW_MENU;
static int menu_sel = ITEM_TODAY;
static TickType_t retry_at = 0;         // don't start an automatic story request before this tick

// Reader state
static int page = 0;                    // page on screen (index into pages[])
static int cursor = -1;                 // selected word (index into tokens[]), -1 = none
static int turns = 0;                   // page turns since the last full refresh

// Saved-words state (the list is read from flash when the screen opens)
static review_entry_t *saved;           // PSRAM array of SAVED_MAX entries
static int saved_count = 0;
static int saved_top = 0;               // first entry on screen
static int saved_cursor = 0;            // highlighted entry

// ---------- background writing ----------

// What story_job() hands back to the main task
typedef struct {
    char *json;                         // story JSON, NULL if the request failed
    char day[11];                       // story day it was written for
} download_t;

static void story_job_done(void *result, void *arg);

// Runs in the worker task: just the network part. arg = UTC offset for the story date.
static void *story_job(void *arg)
{
    download_t *d = calloc(1, sizeof(*d));
    if (d) {
        d->json = story_request((int)(intptr_t)arg, d->day);
    }
    return d;
}

static bool can_write(void)
{
    return story_has_api_key() && local_time_synced();
}

// Starts writing a story in the background (does nothing if one is already being written)
static void start_writing(void)
{
    worker_submit(story_job, story_job_done, (void *)(intptr_t)local_time_offset());
}

// ---------- drawing ----------

static void draw_menu(bool full)
{
    char saved_label[32], level_label[32];
    snprintf(saved_label, sizeof(saved_label), "Saved words (%d)", review_count());
    snprintf(level_label, sizeof(level_label), "Level: HSK %d", story_hsk_level());
    const char *items[ITEM_COUNT] = {
        [ITEM_TODAY] = "Today's story",
        [ITEM_SAVED] = saved_label,
        [ITEM_NEW]   = worker_pending(story_job) ? "Writing..." : "New story",
        [ITEM_LEVEL] = level_label,
    };
    story_ui_show_menu(items, ITEM_COUNT, menu_sel, full);
}

// page_turned = true counts towards the periodic full refresh
static void draw_reader(bool full, bool page_turned)
{
    if (page_turned && ++turns >= FULL_EVERY) {
        full = true;
    }
    if (full) {
        turns = 0;
    }
    story_ui_show(story_get(), page, cursor, full);
}

static void draw_saved(bool full)
{
    story_ui_show_saved(saved, saved_count, saved_top, saved_cursor, full);
}

// Back to the first page with nothing selected (for a new story)
static void rewind_reader(void)
{
    page = 0;
    cursor = -1;
    turns = 0;
}

// The "writing" screen, or why a story can't be written right now
static void show_writing(void)
{
    view = VIEW_WRITING;
    if (!story_has_api_key()) {
        story_ui_show_message("故事", "Set the API key in menuconfig");
    } else if (!local_time_synced()) {
        story_ui_show_message("故事", "Waiting for internet time");
    } else {
        story_ui_show_message("正在写故事…", "Writing a story, about a minute");
    }
}

// ---------- opening each view ----------

static void open_reader(void)
{
    view = VIEW_READER;
    if (page >= story_get()->page_count) {
        rewind_reader();
    }
    draw_reader(true, false);                               // full refresh: whole screen changes
}

static void open_today(void)
{
    if (story_is_current(local_time_offset())) {
        open_reader();
        return;
    }
    // No story for today yet: write one (or keep waiting for the one being written)
    if (can_write()) {
        start_writing();
    }
    show_writing();
}

static void open_saved(void)
{
    if (saved == NULL) {
        saved = heap_caps_malloc(SAVED_MAX * sizeof(review_entry_t), MALLOC_CAP_SPIRAM);
    }
    saved_count = saved ? review_load(saved, SAVED_MAX) : 0;
    saved_top = saved_cursor = 0;
    view = VIEW_SAVED;
    draw_saved(true);
}

static void open_new(void)
{
    if (can_write()) {
        start_writing();                                    // no-op if one is already being written
    }
    show_writing();
}

// ---------- buttons ----------

static void reader_button(button_event_t event)
{
    const story_t *s = story_get();
    if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {
        int old_page = page;
        if (cursor < 0) {                                   // nothing selected yet: select the first word
            cursor = story_page_word(s, page, false);
            draw_reader(false, false);
        } else if (story_step_word(s, &page, &cursor, event == BUTTON_LEFT ? -1 : 1)) {
            draw_reader(false, page != old_page);           // may have continued onto another page
        }
    } else if (event == BUTTON_LEFT_HOLD || event == BUTTON_RIGHT_HOLD) {  // previous/next page
        int p = page + (event == BUTTON_LEFT_HOLD ? -1 : 1);
        if (p >= 0 && p < s->page_count) {
            page = p;
            cursor = -1;                                    // each page starts with nothing selected
            draw_reader(false, true);
        }
    } else if (event == BUTTON_SELECT && cursor >= 0) {     // save / un-save the word
        const story_token_t *t = &s->tokens[cursor];
        esp_err_t err = review_contains(t->w) ? review_remove(t->w)
                                              : review_add(t->w, t->p, t->d, s->day);
        if (err == ESP_OK) {
            draw_reader(false, false);                      // shows / hides the SAVED tag
        }
    }
}

static void saved_button(button_event_t event)
{
    if (saved_count == 0) {
        return;
    }
    if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {    // previous/next word
        int c = saved_cursor + (event == BUTTON_LEFT ? -1 : 1);
        if (c < 0 || c >= saved_count) {
            return;
        }
        saved_cursor = c;
    } else if (event == BUTTON_LEFT_HOLD || event == BUTTON_RIGHT_HOLD) {  // previous/next screen
        int t = saved_top + (event == BUTTON_LEFT_HOLD ? -STORY_UI_SAVED_ROWS : STORY_UI_SAVED_ROWS);
        if (t < 0 || t >= saved_count) {
            return;
        }
        saved_cursor = t;
    } else if (event == BUTTON_SELECT) {                    // un-save / re-save
        review_entry_t *e = &saved[saved_cursor];
        esp_err_t err = e->saved ? review_remove(e->w) : review_add(e->w, e->p, e->d, e->day);
        if (err == ESP_OK) {
            e->saved = !e->saved;
        }
    } else {
        return;
    }
    // Keep the cursor on screen, scrolling a whole screen at a time
    saved_top = saved_cursor / STORY_UI_SAVED_ROWS * STORY_UI_SAVED_ROWS;
    draw_saved(false);
}

// ---------- app callbacks ----------

static void story_open(const app_t *self)
{
    view = VIEW_MENU;
    draw_menu(true);
}

static bool story_button(const app_t *self, button_event_t event)
{
    if (event == BUTTON_BACK) {
        if (view == VIEW_MENU) {
            return false;                                   // leave the app
        }
        view = VIEW_MENU;
        draw_menu(true);
        return true;
    }

    switch (view) {
    case VIEW_MENU:
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {
            menu_sel = (menu_sel + (event == BUTTON_LEFT ? ITEM_COUNT - 1 : 1)) % ITEM_COUNT;
            draw_menu(false);
        } else if (event == BUTTON_SELECT) {
            ESP_LOGI(TAG, "menu item %d", menu_sel);
            if (menu_sel == ITEM_TODAY) {
                open_today();
            } else if (menu_sel == ITEM_SAVED) {
                open_saved();
            } else if (menu_sel == ITEM_NEW) {
                open_new();
            } else {                                        // ITEM_LEVEL: cycle HSK 1 -> 6 -> 1
                story_set_hsk_level(story_hsk_level() % 6 + 1);
                draw_menu(false);                           // applies to the next story written
            }
        }
        break;
    case VIEW_READER:
        reader_button(event);
        break;
    case VIEW_SAVED:
        saved_button(event);
        break;
    case VIEW_WRITING:
        break;                                              // only BACK/HOME do anything here
    }
    return true;
}

// Runs in the main task when story_job() finishes
static void story_job_done(void *result, void *arg)
{
    download_t *d = result;
    esp_err_t err = (d && d->json) ? story_accept(d->json, d->day) : ESP_FAIL;
    if (d) {
        free(d->json);
        free(d);
    }

    if (err == ESP_OK) {
        rewind_reader();
    } else {
        retry_at = xTaskGetTickCount() + RETRY_TICKS;
    }
    if (!app_is_showing(&story_app)) {
        return;                                             // it'll show next time the app opens
    }

    if (view == VIEW_WRITING || (view == VIEW_READER && err == ESP_OK)) {
        if (story_get()) {
            open_reader();                                  // the new story (or, on failure, the last one)
        } else {
            story_ui_show_message("故事", "Couldn't get a story, try again later");
        }
    } else if (view == VIEW_MENU) {
        draw_menu(false);                                   // "Writing..." -> "New story"
    }
}

// Even when not on screen: write the next story once the story day rolls over
static void story_background(const app_t *self)
{
    if (local_time_offset_known()                           // need the time zone to know the date
        && can_write()
        && !story_is_current(local_time_offset())
        && !worker_pending(story_job)
        && app_due(retry_at)) {
        ESP_LOGI(TAG, "new story day: writing in the background");
        start_writing();
    }
}

// Menu icon (the drawing lives with the app's other screens)
static void app_icon(const app_t *self, int cx, int cy)
{
    story_ui_draw_app_icon(cx, cy);
}

static void app_init(const app_t *self)
{
    story_init();
}

const app_t story_app = {
    .id = "story",
    .name = "Story",
    .draw_icon = app_icon,
    .init = app_init,
    .open = story_open,
    .button = story_button,
    .background = story_background,
};
