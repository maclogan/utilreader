/*
 * Screens shared by the Music and Podcasts apps, on the board's usual left title bar (with
 * the battery): a list with ">" for rows that open more, a mark on the item playing, a tag on
 * the right and a scroll bar; Now Playing; and a message screen.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "player.h"
#include "freertos/FreeRTOS.h"              // TickType_t

#define MEDIA_UI_ROWS 6                 // rows on a screen

typedef struct {
    const char *label;                  // UTF-8
    char tag[20];                       // small text on the right ("3", "25m left"), or ""
    bool opens;                         // draws ">"
    bool playing;                       // the item playing: ">" or "||"
} media_row_t;

// Fills *out for row i
typedef void (*media_row_fn)(int i, media_row_t *out);

// Starts a frame (full refresh when asked, when the panel slept, or every so often against
// ghosting); returns whether it's a full one. media_ui_finish() shows it.
bool media_ui_begin(bool full);
void media_ui_finish(bool full);

// The list screen. `hint` (may be NULL) is shown in the middle when count is 0.
void media_ui_list(const char *title, int count, int sel, media_row_fn row, const char *hint, bool full);

// Now Playing for the player's current item. `seek_hint` is shown in SEEK mode
// ("< >  back 10 s / ahead 30 s"); `idle` is the line when nothing plays.
void media_ui_playing(bool seek_mode, const char *seek_hint, const char *idle, bool full);

// Now Playing's long title / artist / album lines scroll. An app showing Now Playing adds
// media_ui_marquee_wait() to its next_wakeup, and in tick redraws Now Playing (partial) when
// media_ui_marquee_step() returns true.
TickType_t media_ui_marquee_wait(void);
bool media_ui_marquee_step(void);

// A message: title, a detail line, and a hint at the bottom
void media_ui_message(const char *bar_title, const char *title, const char *detail, const char *hint);

// Helpers for screens of your own
void media_ui_text_cut(int x, int y, const char *text, int right_edge, uint8_t color);
void media_ui_small_centered(int y, const char *text);
void media_ui_format_time(char *out, size_t size, uint32_t ms);
