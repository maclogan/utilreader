# player: the playback engine and shared screens

| File | What it does |
|---|---|
| `player.c`, `include/player.h` | The player task: plays a queue of items from the card through `components/audio` |
| `media_ui.c`, `include/media_ui.h` | The list, Now Playing and message screens Music and Podcasts share |
| `idf_component.yml` | Pulls in Espressif's `esp_audio_codec` decoders |

## Sources

The engine doesn't know what it plays. An app hands it a queue of ids and a **source**:

```c
static const player_source_t music_source = {.get = music_get, .advance = true};
player_play_queue(&music_source, ids, count, pos, start_ms);
```

- `get(id, &item)` fills the file path, the title / artist / album lines and where to start
  (`start_ms`, used when the queue moves on: podcasts resume each episode). Returning false
  skips the item (podcasts skip played episodes this way).
- `finished(id)` (optional) is called when an item plays to its end; Podcasts marks it played.
- `advance`: go on to the next item, or stop after this one.

`player_status()` says which source is playing, so each app shows only its own Now Playing.
There's one engine and one DAC: starting a queue replaces whatever another app was playing.

Seeking, decoding, formats and the task layout: `components/app_music/README.md` and
`docs/audio.md`.
