/*
 * The music player engine: a background task that reads a track from the SD card, decodes it
 * (esp_audio_codec) and feeds components/audio, then moves on to the next track in the
 * library. Commands come from the main task and return at once; the task posts a
 * notification when something the screen shows changes.
 *
 * The library (library.h) must not be rescanned while a track is playing: stop first.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "media_info.h"

typedef struct {
    int index;                          // track in the library, -1 = nothing loaded
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

void player_play(int index, uint32_t start_ms);
void player_toggle_pause(void);
void player_seek(int delta_ms);         // relative to the current position
void player_next(void);
void player_prev(void);                 // restarts the track if more than 3 s in
void player_stop(void);                 // waits until playback has stopped and the DAC is off

void player_status(player_status_t *out);
bool player_active(void);               // a track is loaded (playing or paused)
