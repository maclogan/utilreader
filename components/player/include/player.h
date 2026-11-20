/*
 * The playback engine, shared by the Music and Podcasts apps: a background task that plays a
 * queue of items from the SD card. It decodes each (esp_audio_codec), feeds components/audio
 * and moves on to the next. It keeps going while other apps are on screen. Commands come
 * from the main task and return at once; the task posts a notification when something the
 * screen shows changes. See README.md.
 *
 * What an item is belongs to its source: a library track for Music, an episode for Podcasts.
 * The source turns item ids into a file and display text. One engine, one DAC: starting a
 * queue from one source replaces whatever another source was playing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "media_info.h"

#define PLAYER_ITEM_START   UINT32_MAX  // start_ms: use the item's own start (player_item_t)

typedef struct {
    char path[320];                     // full path of the file
    char title[128];
    char artist[96];                    // podcasts: the show
    char album[96];
    uint32_t start_ms;                  // where to start when the queue reaches it (podcasts: resume)
} player_item_t;

typedef struct {
    // Fills *out for item `id`. Called from the player's task. False: skip the item.
    bool (*get)(int id, player_item_t *out);
    // Optional: item `id` played to its end (podcasts mark it played). Player's task.
    void (*finished)(int id);
    // At the end of an item: go on to the next in the queue (else stop)
    bool advance;
} player_source_t;

typedef struct {
    int index;                          // item id (from the source), -1 = nothing loaded
    const player_source_t *source;      // who's playing (NULL = nothing)
    int queue_pos, queue_len;           // "3 of 12"
    bool paused;
    uint32_t elapsed_ms;                // seek target while a seek is in progress
    uint32_t duration_ms;               // 0 = unknown
    char title[128];                    // the tag, or the file name
    char artist[96];
    char album[96];
    media_format_t format;
    uint32_t rate;                      // 0 until the first audio is decoded
    uint8_t bits, channels;
    char message[64];                   // last problem ("Can't play 192 kHz"), "" if none
} player_status_t;

// Starts the task. `notify` is called (from the player's task) when the status changes in a
// way the screen should show: new track, pause, end of the list, an error.
void player_init(void (*notify)(void));

// Plays items[pos] (ids for `source`; copied) from start_ms (or PLAYER_ITEM_START), then the
// rest of the list if the source advances. `source` must stay valid (a static).
void player_play_queue(const player_source_t *source, const int *items, int count, int pos, uint32_t start_ms);

// The queue being played (valid until the next player_play_queue); *count set
const int *player_queue(int *count);
void player_toggle_pause(void);
void player_seek(int delta_ms);         // relative to the current position
void player_next(void);
void player_prev(void);                 // restarts the track if more than 3 s in
void player_stop(void);                 // waits until playback has stopped and the DAC is off

void player_status(player_status_t *out);
bool player_active(void);               // a track is loaded (playing or paused)
