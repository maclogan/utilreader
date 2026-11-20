/*
 * What's in an audio file, read from its headers without decoding: format, title / artist /
 * album, duration, and where the audio data starts (for seeking). Plain C and stdio only,
 * so it also builds on a PC.
 *
 *   MP3   ID3v2 (2.2-2.4) and ID3v1 tags; duration from the Xing/Info/VBRI header or the bitrate
 *   FLAC  Vorbis comments; duration from STREAMINFO
 *   WAV   LIST/INFO tags; duration from the data size
 *   M4A   iTunes ilst tags; duration from mvhd
 *   OGG   Vorbis / Opus comments; duration from the last page's granule position
 *   AAC   (ADTS) ID3v2 tags only; duration unknown
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MEDIA_UNKNOWN,
    MEDIA_MP3,
    MEDIA_FLAC,
    MEDIA_WAV,
    MEDIA_M4A,
    MEDIA_AAC,
    MEDIA_OGG,
} media_format_t;

typedef struct {
    media_format_t format;
    char title[128];                    // UTF-8; empty if the file has no tag
    char artist[96];
    char album[96];
    uint32_t duration_ms;               // 0 = unknown
    uint32_t file_size;
    uint32_t header_start;              // FLAC / WAV: the stream header to re-send after a jump
    uint32_t audio_offset;              // first byte of audio data (frames / samples)
    uint32_t audio_end;                 // end of audio data (before an ID3v1 tag, WAV chunk end)
    uint32_t sample_rate;               // from the headers, 0 if not read
} media_info_t;

// The format from the file name's extension (.mp3 .flac .wav .m4a .mp4 .aac .ogg .opus)
media_format_t media_format_from_name(const char *path);

// Reads `path`'s headers into *out. False if the file can't be opened or the format is
// unknown; a damaged tag just leaves fields empty.
bool media_info_read(const char *path, media_info_t *out);

// The extension as a short label ("FLAC", "MP3"...)
const char *media_format_name(media_format_t f);
