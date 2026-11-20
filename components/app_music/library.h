/*
 * The track list: every supported audio file in the music folder on the SD card and its
 * subfolders (4 levels), sorted by path so albums in folders play in order. Kept in PSRAM.
 * Main task only (the player gets paths as copies).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LIBRARY_OK,
    LIBRARY_NO_CARD,                    // no card, or not FAT32
    LIBRARY_NO_FOLDER,                  // the music folder doesn't exist
    LIBRARY_EMPTY,                      // the folder has no supported files
} library_status_t;

#define LIBRARY_MAX_TRACKS 4000

// Scans `dir` (a path on the card such as "/Music"). Replaces the previous list.
library_status_t library_scan(const char *dir);

int library_count(void);

// Full path ("/sdcard/Music/Album/01 Song.flac") of track i into out[size]
void library_path(int i, char *out, size_t size);

// The file name without folder or extension, for lists and as a fallback title
const char *library_name(int i);

// The folder the track is in, relative to the music folder ("" at the top)
void library_folder(int i, char *out, size_t size);

// Index of the track with this full path, or -1
int library_find(const char *path);
