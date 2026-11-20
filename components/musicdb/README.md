# musicdb: the music library

Everything the Music app and the web page know about the music on the microSD card.

| File | What it does |
|---|---|
| `musicdb.c`, `include/musicdb.h` | Scan, tag cache, artist / album grouping, sorted lists |
| `playlists.c`, `include/playlists.h` | `.m3u8` playlists in `/Playlists` |
| `media_info.c`, `include/media_info.h` | Tags, duration and audio position from file headers (plain C) |

## Scanning

`musicdb_scan("/Music", progress)` lists the folder and 4 levels of subfolders (hidden files
skipped), up to 4000 tracks. Reading tags needs a file open each, so the results are cached in
`/.musicdb` on the card (tab-separated text: size, duration, track, disc, format, path, title,
artist, album artist, album). On the next scan only files that are new or changed size are
read; with nothing new, a big library takes seconds. The Music app scans on first open (per
folder) and from **Rescan Library**; the web page scans if nothing has been scanned yet.

Strings live in 64 KB PSRAM blocks freed together on the next scan. Track indices are valid
until the next scan; `musicdb_generation()` changes with each, so the web page can tell its
list is stale.

## Grouping (like an iPod)

- **Artist** of a group = album artist if tagged, else artist, else "Unknown Artist".
- **Album** = (album name, group artist): two artists' "Greatest Hits" stay apart.
- Sorting ignores case and a leading "The ". Albums list songs by disc, track, then file name;
  an artist's All Songs go album by album.

## Playlists

Standard M3U with `#PLAYLIST:` for the display name:

```
#EXTM3U
#PLAYLIST:Road trip
#EXTINF:215,Artist - Title
/Music/Album/01 Song.flac
```

The web page writes them (file name from the playlist name, ASCII only; the display name can be
any language). When reading, entries can be card paths, relative to `/Playlists`, or with a
drive letter and backslashes (from a computer); entries not in the library are skipped and
counted. Up to 64 playlists of 1000 songs.

## Threads

`musicdb_scan()` must not run while track indices are in use: the Music app stops the player
first. The web server holds `musicdb_lock()` while reading. Nothing else changes the data.
