# app_music: Music app

Plays audio files from a folder on the microSD card (default `/Music`, set in **Settings**)
through the TLV320DAC3100 headphone DAC. Playback runs in the background: leave the app and
the music carries on while you use the others.

| File | What it does |
|---|---|
| `music_app.c`, `include/music_app.h` | The `app_t`: library and Now playing screens, saving the place |
| `player.c/.h` | The player task: file -> decoder -> `components/audio`, commands, seeking |
| `library.c/.h` | Finds the tracks (folder + 4 levels of subfolders), sorted by path |
| `media_info.c/.h` | Tags, duration and the audio data's position, read from the headers (plain C) |
| `idf_component.yml` | Pulls in Espressif's `esp_audio_codec` decoders |

## Using it

| Screen | Left / right | Hold left / right | Press | Hold press | BACK |
|---|---|---|---|---|---|
| **Library** | Move | A page (6 tracks) | Play | Now playing (if something plays) | Main menu, music keeps playing |
| **Now playing**, VOL mode | Volume down / up | Previous / next track | Play / pause | Switch to SEEK | Library |
| **Now playing**, SEEK mode | Back / ahead 10 s | Previous / next track | Play / pause | Switch to VOL | Library |

- Opening the app while music plays goes straight to Now playing.
- Previous restarts the track if it's more than 3 s in.
- At the end of a track the next one starts; at the end of the list playback stops.
- Now playing shows the title, artist and album (from the tags, or the file name), a
  progress bar, the elapsed and total time, the format and sample rate, and a VOL or SEEK
  badge with the volume. The time updates every 10 s (e-paper); presses update it at once.
- Volume: 30 steps of 2 dB (DAC digital volume), remembered.
- The last track and position are saved on pause, on track changes, every 2 minutes, when
  leaving the app and at power-off; choosing that track again resumes there.
- After 5 minutes paused the DAC is switched off; pressing play turns it back on.
- Files that can't be played (damaged, or above 96 kHz) are skipped with a note at the
  bottom of Now playing. No DAC found: playback stops with a message.

## Formats

| Extension | Decoder | Tags | Duration | Seeking |
|---|---|---|---|---|
| `.mp3` | MP3 | ID3v2.2-2.4, ID3v1 | Xing/Info/VBRI frame count, else bitrate | Jumps to the estimated byte |
| `.flac` | FLAC (16/24-bit) | Vorbis comments | STREAMINFO (exact) | Re-sends the header, then jumps |
| `.wav` | PCM | LIST/INFO | data size (exact) | Jumps (exact) |
| `.m4a`, `.mp4` | AAC (incl. HE-AAC) | iTunes `ilst` | `mvhd` | Decodes from the start up to the target |
| `.aac` | AAC (ADTS) | ID3v2 | unknown | Jumps |
| `.ogg`, `.oga`, `.opus` | Vorbis / Opus | Vorbis comments | last page's granule | Decodes from the start up to the target |

Sample rates 8-96 kHz. The decoder library's simple API can only start a stream from the
beginning, so "jumping" means: reset the decoder, (for FLAC and WAV) feed it the stream
header again, then continue from a byte offset proportional to the time; MP3, AAC and FLAC
frames carry sync codes the decoder finds again. If the decoder can't find its feet after a
jump, the player falls back to decoding from the start. M4A and OGG need their index tables,
so they always decode-and-discard, which takes a moment for seeks far into a long track.

## How playback is put together

```
  main task (UI)                player task (prio 8)              audio_out task (prio 12)
  music_app.c ---commands--->  player.c                           components/audio
                               fread 16 KB -> esp_audio_simple_dec
                               -> PCM -> audio_write() --> PSRAM ring (384 KB, ~1 s) --> I2S DMA --> TLV320
             <--BUTTON_WAKE--  (new track, pause, error)
```

E-paper refreshes and SD-card pauses don't interrupt the sound: the output task has the
highest priority and about a second of audio buffered. More detail: `docs/audio.md`.

## Changing it

- **Another format:** add the extension in `media_format_from_name()` and `dec_type()`
  (any type `esp_audio_simple_dec` supports), and tag/duration parsing in `media_info.c`.
- **Shuffle / repeat:** `play_track()` returns the next index; that's the place.
- **Lists by album or artist:** `library.c` keeps only paths; reading tags for every file at
  scan time would be the start (slow on big cards: cache them).
