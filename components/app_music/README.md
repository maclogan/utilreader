# app_music: Music app

Plays music from the microSD card through the TLV320DAC3100 headphone DAC, organised the
way an iPod does it: playlists, artists, albums, songs. The screens use the board's usual
left title bar. Playback runs on its own task, so it **keeps going when you leave the app**
(BACK from the top menu, or HOME) and use any other app; the next track starts on its own.
Turning the device off stops it and remembers the place.

| File | What it does |
|---|---|
| `music_app.c`, `include/music_app.h` | The `app_t`: the menu tree, Now Playing, saving and resuming |

Playback is the shared engine in `components/player` (also used by Podcasts): Music hands it a
queue of library tracks through its `music_source`. The list and Now Playing screens are the
shared `media_ui` too.

The library itself (scanning, tags, artists, albums, playlists) is `components/musicdb`, shared
with the web page.

## Menus

```
Music
  Playlists      > a playlist          > Shuffle Songs, then its songs in order
  Artists        > an artist           > All Songs (by album, then track)
                                        > Shuffle All Songs
                                        > an album      > Shuffle Songs, then its songs (disc, track)
  Albums         > an album            > Shuffle Songs, then its songs
  Songs          (Shuffle Songs, then every song A-Z by title)
  Shuffle Songs  (every song, shuffled; starts playing)
  Now Playing    (while something plays, or to resume after a restart)
  Rescan Library (after copying music to the card)
```

Choosing a song plays **that list** from there (the album, the artist's songs, the playlist or
all songs), like an iPod. **Shuffle Songs** at the top of any song list plays that list
shuffled; **Shuffle All Songs** on an artist shuffles everything by them; **Shuffle Songs** on
the top menu shuffles the whole library. Rows that open another list end in `>`; long lists show a scroll bar
and `3/120` in the title bar; the song playing is marked `>` (`||` when paused). Artists and
albums use the album-artist tag when there is one (so compilations stay together), and "The"
is ignored when sorting. Untagged files show their file name, "Unknown Artist" and "Unknown
Album".

| Where | Left / right | Hold left / right | Press | Hold press | BACK |
|---|---|---|---|---|---|
| Lists | Move | A page | Open / play | Now Playing | Up a level (from the top: leave, music carries on) |
| Now Playing, VOL | Volume | Previous / next track | Play / pause | Switch to SEEK | Back to the list |
| Now Playing, SEEK | Back / ahead 10 s | Previous / next track | Play / pause | Switch to VOL | Back to the list |

**Now Playing** shows the album art in 4 greys (dithered, 68x68), the title, artist and
album, "3 of 12", the format and sample rate, a progress bar with the elapsed time and the
time left, and the VOL / SEEK badge with the volume bar. The time updates every 10 s; presses
update it at once.

**Album art** fills the left third of Now Playing (96 x 96, 4 greys, dithered); the title,
artist, album, progress and volume are on the right. It comes from the file's own tag (MP3/AAC
ID3 `APIC`, including "unsynchronised" tags; FLAC `PICTURE`; M4A `covr`; Ogg/Opus
`METADATA_BLOCK_PICTURE`), otherwise from a picture in the song's folder: `cover`, `folder` or
`front` (.jpg/.jpeg/.png) first, then `album`, Windows' hidden `AlbumArt_{...}_Large.jpg` /
`AlbumArtSmall.jpg`, anything named like a cover, and as a last resort any other picture there
(back covers and disc scans come last). Baseline JPEGs, progressive JPEGs (their first scan: a
1/8-size picture, enlarged up to 2x) and PNGs (not interlaced) all work. A new cover redraws
the screen with the grey refresh; the time updates and button presses stay quick partial
refreshes and leave the cover alone. Decoding: `components/image`.

## Headphones

The TLV320 watches the headphone jack (its fourth contact, through the breakout's mic
detect jumper: leave it uncut). This works in every app, for music, podcasts and voice notes:

- **Unplugging pauses** playback; plugging back in leaves it paused.
- **The inline button** (on most headsets with a microphone): one press = play / pause, two =
  next track, three = previous.
- **Inline volume buttons don't work:** telling them apart needs an analog input the TLV320
  doesn't have (Android remotes signal by resistance; Apple's use their own protocol).

`components/player/player.c` (`headset_task`) polls the chip every 50 ms; the chip side is
`audio_headset_*()` / `tlv320_headset_*()`. Until the DAC answers, it looks every 10 s.

## Remembering

- Volume: in flash.
- The queue: written to `/.musicq` on the card when a list starts playing; the position in it
  and the time into the track go to flash on pause, on track changes, every 2 minutes, when
  leaving the app and at power-off. After a restart **Now Playing** on the top menu resumes it.

## Playlists

Made on the board's web page (**Transfer** app, then **Music** on the page): search the
library, add songs, reorder, save. They're standard `.m3u8` files in `/Playlists` on the card,
so playlists exported from music software on a computer work too (paths on the card, relative
paths, or with a drive letter). See `components/musicdb/README.md`.

## Formats and seeking

| Extension | Tags | Duration | Seeking |
|---|---|---|---|
| `.mp3` | ID3v2.2-2.4, ID3v1 | Xing/Info/VBRI frame count, else bitrate | Jumps to the estimated byte |
| `.flac` | Vorbis comments | STREAMINFO (exact) | Re-sends the header, then jumps |
| `.wav` | LIST/INFO | data size (exact) | Jumps (exact) |
| `.m4a`, `.mp4` | iTunes `ilst` | `mvhd` | Decodes from the start up to the target |
| `.aac` | ID3v2 | unknown | Jumps |
| `.ogg`, `.oga`, `.opus` | Vorbis comments | last page's granule | Decodes from the start up to the target |

8-96 kHz, 16/24-bit. Files that can't be played are skipped with a note on Now Playing. The
decoder library can only start a stream from the beginning, so a "jump" resets it and feeds it
from a byte offset proportional to the time (after the stream header for FLAC and WAV); if it
doesn't find its feet, the player falls back to decoding from the start.

## How playback is put together

```
  main task (UI)               player task (prio 8)               audio_out task (prio 12)
  music_app.c --play queue-->  player.c                           components/audio
                               fread 16 KB -> esp_audio_simple_dec
                               -> PCM -> audio_write() --> PSRAM ring (~1 s) --> I2S --> TLV320
             <--BUTTON_WAKE--  (new track, pause, error)
```

The main loop forwards BUTTON_WAKE only to the app on screen, so while another app is open the
player just carries on; the Music app redraws when it's next opened. More: `docs/audio.md`.
