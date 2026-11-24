/*
 * Small values remembered across power-offs (NVS flash, namespace "app"), e.g. the last
 * opened app. nvs_flash_init() must have run first (main.c does it).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

// Keys shared between components (each component's own keys stay private to it)
#define SETTING_ROTATE_180 "rotate180"  // 1 = display upside down (Settings app; main.c applies it at start-up)
#define SETTING_MENU_ROWS  "menu_rows"  // home screen: 1 or 2 rows of tiles (web page; default 2)
#define SETTING_MENU_COLS  "menu_cols"  // home screen: 3-5 tiles per row (web page; default 4)
#define SETTING_ROTATE_TALL "rot_tall" // 1 (default) = tall pictures (book pictures, the off screen photo) turned sideways when that doubles their size
#define SETTING_FULL_EVERY "full_every" // full refresh after this many quick ones (Settings > Cleanup; epd.h)
#define SETTING_PARTIAL_MODE "partial"  // how partial refreshes are done (Settings > Refresh; epd.h)
#define SETTING_PICTURE_GAMMA "pic_gamma" // pictures' gamma x 100 (Settings > Darkness; main.c applies it)
#define SETTING_MUSIC_DIR  "music_dir"  // music folder on the microSD card (Settings app; the music app reads it)
#define SETTING_MUSIC_DIR_DEFAULT "/Music"
#define SETTING_PODCASTS_DIR "pod_dir"  // podcasts folder on the card (Settings app; components/podcasts)
#define SETTING_PODCASTS_DIR_DEFAULT "/Podcasts"
#define SETTING_BOOKS_DIR  "books_dir"  // e-books: a folder on the card, or "" = internal flash
#define SETTING_BOOKS_DIR_DEFAULT "/Books"
#define SETTING_VOICE_DIR  "voice_dir"  // voice notes folder on the card (Settings app; the Voice app)
#define SETTING_VOICE_DIR_DEFAULT "/Voice Notes"

// The stored value for `key`, or `fallback` if it was never set. Keys are at most 15 characters.
int32_t settings_get_i32(const char *key, int32_t fallback);

// Stores a value. Skips the flash write if it's unchanged.
void settings_set_i32(const char *key, int32_t value);

// The stored string for `key` into out[size]; `fallback` if it was never set
void settings_get_str(const char *key, char *out, size_t size, const char *fallback);

// Stores a string. Skips the flash write if it's unchanged.
void settings_set_str(const char *key, const char *value);
