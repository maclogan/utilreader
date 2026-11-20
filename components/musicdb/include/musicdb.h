/*
 * The music library on the microSD card: every audio file in the music folder (and 4 levels
 * of subfolders) with its tags, grouped into artists and albums the way an iPod does, plus
 * playlists (playlists.h). Shared by the Music app and the web interface. See README.md.
 *
 * Tags are slow to read (a file open each), so they're cached on the card in /.musicdb and
 * only files that are new or changed size are read again on a rescan.
 *
 * Threads: musicdb_scan() replaces everything, so it must not run while anyone uses track
 * indices (the Music app stops the player first). The web server holds musicdb_lock() while
 * it reads. Everything else is read-only and safe between scans.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "media_info.h"

#define MUSICDB_MAX_TRACKS  4000

typedef enum {
    MUSICDB_OK,
    MUSICDB_NO_CARD,                    // no card, or not FAT32
    MUSICDB_NO_FOLDER,                  // the music folder doesn't exist
    MUSICDB_EMPTY,                      // no supported files
    MUSICDB_NOT_SCANNED,
} musicdb_status_t;

typedef struct {
    const char *rel;                    // path relative to the music folder
    const char *title;                  // tag, or the file name without extension
    const char *artist;                 // "" if untagged
    const char *album;                  // ""
    const char *album_artist;           // ""
    uint16_t track, disc;
    uint32_t duration_ms;
    uint32_t size;                      // file size (cache check)
    media_format_t format;
    int artist_id, album_id;            // index into the artist / album lists
} musicdb_track_t;

// Scans `dir` (on the card, e.g. "/Music"). `progress` (may be NULL) is called now and then
// with files done / total while tags are being read.
musicdb_status_t musicdb_scan(const char *dir, void (*progress)(int done, int total));

musicdb_status_t musicdb_status(void);
const char *musicdb_dir(void);          // the folder last scanned ("/Music"), "" before
uint32_t musicdb_generation(void);      // changes with every scan

int musicdb_count(void);
const musicdb_track_t *musicdb_track(int i);
void musicdb_path(int i, char *out, size_t size);       // "/sdcard/Music/Album/01.flac"
void musicdb_card_path(int i, char *out, size_t size);  // "/Music/Album/01.flac" (playlists)
int musicdb_find_path(const char *full_path);           // full or card path -> index, -1

// Display helpers: never empty
const char *musicdb_artist_of(int i);   // track artist, else album artist, else "Unknown Artist"
const char *musicdb_album_of(int i);    // album, else "Unknown Album"

// All songs sorted by title (n set to the count)
const int *musicdb_songs(int *n);

// Artists (album artist if tagged, else artist), sorted ("The" ignored)
int musicdb_artist_count(void);
const char *musicdb_artist_name(int a);
// Albums: one per (album name, artist), sorted by name
int musicdb_album_count(void);
const char *musicdb_album_name(int al);
const char *musicdb_album_artist(int al);

// Fill out[max]; return the count
int musicdb_artist_albums(int a, int *out, int max);    // album ids, by name
int musicdb_artist_songs(int a, int *out, int max);     // by album, disc, track
int musicdb_album_songs(int al, int *out, int max);     // by disc, track, file name

void musicdb_lock(void);
void musicdb_unlock(void);
