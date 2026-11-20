# Audio

How music gets from the microSD card to your headphones, and what's planned for the
microphone. Wiring: [HARDWARE.md](../HARDWARE.md#audio-and-the-io-expander).

## Pieces

```
 microSD (FAT32)       components/sdcard        mount at /sdcard, on the shared rail
      |
 app_music/library.c   track list: the music folder + subfolders, sorted by path
 app_music/media_info  tags, duration, where the audio starts (seeking)
      |
 app_music/player.c    player task: read 16 KB -> esp_audio_codec -> PCM
      |
 components/audio      ring buffer (PSRAM, ~1 s) -> audio_out task -> I2S (32-bit stereo)
      |                 GPIO4 BCLK, GPIO5 WS, GPIO43 data
 TLV320DAC3100         PLL from BCLK; DAC -> mixer -> headphone amp -> 3.5 mm jack
      ^ I2C 0x18 (setup, volume), reset from the I/O expander's pin 3
```

## Decoders

[esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec)
2.6.2, from the ESP-IDF component manager (`components/app_music/idf_component.yml`;
`dependencies.lock` pins it, `managed_components/` is downloaded at build time). Espressif
ships it prebuilt and optimised for the ESP32-S3, under its Modified MIT licence, which
allows use on Espressif chips only (fine here). The "simple decoder" layer parses the
containers: MP3, AAC (ADTS), M4A/MP4, FLAC, WAV, OGG (Vorbis, Opus). It adds about 1 MB to
the firmware.

Supported: 8-96 kHz, 16- or 24-bit, mono or stereo. Higher rates (176.4 / 192 kHz) are
skipped with a message: the DAC's clock tree tops out around 96 kHz from BCLK.

## Timing and priorities

| Task | Priority | Job |
|---|---|---|
| `audio_out` | 12 | Ring buffer -> I2S DMA (65 ms of DMA buffers) |
| `player` | 8 | Read, decode, convert, fill the ring |
| main | 1 | Screen, buttons, apps |

An e-paper full refresh takes about 2-3 s of the main task, and SD reads occasionally stall
for tens of ms; neither reaches the output, which has about a second of audio queued.

## Power

The DAC is powered down when playback stops, after 5 minutes paused, and at power-off (the
music app's power-off hook stops the player and saves the place). While playing, expect
roughly 15-25 mA for the DAC and headphone amp on top of the board (to be measured).

## Microphone (planned)

The INMP441 shares BCLK and WS with the DAC and sends its data on GPIO3, so recording uses
the same I2S controller in full duplex at the playback rate. Not set up yet: the plan is a
language-learning feature that plays a word's pronunciation, records yours and plays both
back to compare by ear. The voice source (bundled recordings or generated speech) is still
to be decided.
