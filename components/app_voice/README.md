# app_voice: the Voice Notes app

Records voice notes from the INMP441 microphone onto the microSD card and plays them back.
The menu tile is called **Voice**.

| File | What it does |
|---|---|
| `voice_app.c`, `include/voice_app.h` | The `app_t`: the list, recording, playback, deleting, the menu icon |

## Using it

| Screen | Left / right | Hold left / right | Press | Hold press | BACK |
|---|---|---|---|---|---|
| **List** | Move | A page | **+ New note**: record. A note: play it | **+ New note**: record. A note: delete it (asks first) | Main menu |
| **Recording** | | | Stop and save | Stop and save | Stop and save |
| **Playing** | Back / ahead 5 s | Volume down / up | Pause / resume | Pause / resume | Stop, back to the list |
| **Delete?** | | | | Delete | Keep it |

Recording is a toggle: one press starts it, the next stops it; nothing has to be held. The list
always starts with **+ New note**, then the notes **newest first**, each named by
when it was recorded ("Mon 23 Nov  2:41 PM") with its length on the right. The note playing
is marked `>` (`||` when paused). The recording screen shows the elapsed time and a level meter,
updated every second. A recording under half a second (a stray press) isn't kept; one stops by
itself after an hour or when the card is full. Turning the device off, leaving the app or
pressing HOME while recording saves what was recorded.

## Files

Notes are 16 kHz, 16-bit mono WAV files (32 KB a second, about 1.9 MB a minute) in the
folder chosen in **Settings > Voice** (default `/Voice Notes` on the card; changing it moves
the notes), named `YYYYMMDD-HHMMSS.wav` so the names sort by date and time.
Notes made before the clock was ever set are `note-0001.wav` and so on (they're listed above
the dated ones). The files are ordinary WAVs: copy them off the card and play them anywhere.
Deleting a note deletes its file.

## How it works

- **Recording:** the microphone shares the DAC's I2S clock pins (BCLK GPIO4, WS GPIO5), so it
  runs on I2S0 as a receive-only channel, and only while playback is stopped: starting a
  recording stops the player. `audio_rec_start()` / `audio_rec_read()` / `audio_rec_stop()` in
  `components/audio` read the INMP441's 24-bit left-channel samples, remove the DC offset
  (high-pass at ~10 Hz) and add +24 dB of gain. A recorder task writes them to the card through
  a 16 KB buffer and fills in the WAV header's sizes at the end.
- **Playback:** the shared player engine (`components/player`) with a one-item queue, and the
  shared Now Playing screen in SEEK mode.

## Changing it

- Too quiet or too loud: `mic_gain` in `components/audio/audio.c` (16 = +24 dB).
- Better quality: `RATE` in `voice_app.c` (e.g. 22050 or 32000), at more card space per minute.
