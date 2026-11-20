/*
 * Playlists: standard .m3u8 / .m3u files in /Playlists on the microSD card. The web
 * interface makes them; music software on a computer can too. Entries are paths on the card
 * ("/Music/Album/01 Song.flac"), relative to the playlist's folder, or with a drive letter
 * ("E:\Music\..."): all are matched against the library (musicdb). The display name comes
 * from a "#PLAYLIST:" line, else the file name.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PLAYLISTS_MAX       64
#define PLAYLIST_MAX_TRACKS 1000

typedef struct {
    char file[48];                      // file name in /Playlists ("Road trip.m3u8")
    char name[64];                      // display name (UTF-8)
    int tracks;                         // entries in the file (found in the library or not)
} playlist_info_t;

// The playlists, sorted by name. Returns the count.
int playlists_list(playlist_info_t *out, int max);

// The playlist's tracks as library indices, in order, into out[max]. *missing (may be NULL)
// counts entries not in the library. -1 if the file can't be read.
int playlist_load(const char *file, int *out, int max, int *missing);

// Writes a playlist with these library tracks. `old_file` ("" for a new one) is replaced
// (also when the name changes). The file name made from `name` goes to file_out[48].
bool playlist_save(const char *name, const char *old_file, const int *tracks, int n, char *file_out);

bool playlist_delete(const char *file);

// True for a plain file name in /Playlists ending in .m3u or .m3u8 (no slashes)
bool playlist_file_ok(const char *file);
