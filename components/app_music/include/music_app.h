/*
 * Music app: plays audio files from a folder on the microSD card through the TLV320DAC3100
 * headphone DAC. See components/app_music/README.md.
 *
 *   Library:      left / right move, hold = a page; press plays; hold press = Now playing;
 *                 BACK leaves (playback carries on in the background)
 *   Now playing:  left / right = volume (VOL mode) or -10 s / +10 s (SEEK mode);
 *                 hold left / right = previous / next track; press = play / pause;
 *                 hold press = switch VOL / SEEK; BACK = the library
 */
#pragma once

#include "app.h"
#include "settings.h"

// The music folder on the card: SETTING_MUSIC_DIR in settings.h, set in the Settings app
#define MUSIC_DIR_KEY     SETTING_MUSIC_DIR
#define MUSIC_DIR_DEFAULT SETTING_MUSIC_DIR_DEFAULT

extern const app_t music_app;
