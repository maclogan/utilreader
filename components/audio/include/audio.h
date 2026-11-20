/*
 * Audio out: I2S to the TLV320DAC3100 headphone DAC. One producer (the music player's task)
 * writes PCM; a high-priority output task moves it from a PSRAM ring buffer (about 1 s) to
 * I2S, so slow SD reads or e-paper refreshes don't cause dropouts. See README.md.
 *
 * Pins: BCLK GPIO4, WS GPIO5, data out GPIO43 (header TX). GPIO3 is kept for the
 * microphone's data. Output is always 32-bit stereo (BCLK = 64 x rate, which the DAC's PLL
 * multiplies up to its own clocks).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_VOLUME_MAX    30          // volume steps; 2 dB each, AUDIO_VOLUME_MAX = 0 dB

// Starts output at `rate` Hz, or changes the rate (waits for buffered audio to finish
// first). False if the DAC doesn't answer or the rate is unsupported (8-96 kHz work).
bool audio_start(uint32_t rate);

// Queues `frames` frames of interleaved PCM: `bits` 16, 24 (packed 3 bytes) or 32, any
// channel count (mono is doubled; beyond 2 channels only the first two are used). Blocks
// while the buffer is full. Returns the frames queued (fewer if audio_flush() or
// audio_stop() cut in from another task).
size_t audio_write(const void *pcm, size_t frames, int bits, int channels);

// Waits until everything queued has been played
void audio_drain(void);

// Drops everything queued (seek, skip). The played-frames counter restarts at 0.
void audio_flush(void);

// Pause keeps the output running (DAC softly muted) so resuming is instant
void audio_pause(bool paused);

// Frames actually sent to the DAC since audio_start() or the last audio_flush()
uint64_t audio_frames_played(void);

// 0 (silent) to AUDIO_VOLUME_MAX. Remembered across audio_start() / audio_stop().
void audio_set_volume(int level);
int audio_volume(void);

// Mutes, powers the DAC down and releases the I2S pins
void audio_stop(void);

bool audio_running(void);
