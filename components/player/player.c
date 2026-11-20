#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "decoder/impl/esp_aac_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impl/esp_m4a_dec.h"

static const char *TAG = "player";

#define IN_SIZE             (16 * 1024)             // file read size
#define OUT_START_SIZE      (16 * 1024)             // decoder output, grown on demand
#define PAUSE_POWER_DOWN_MS (5 * 60 * 1000)         // paused this long: DAC off until resumed
#define PREV_RESTART_MS     3000

typedef enum { CMD_PLAY, CMD_PAUSE, CMD_SEEK, CMD_NEXT, CMD_PREV, CMD_STOP } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    int index;                                      // PLAY: queue position
    uint32_t ms;                                    // PLAY: start; SEEK: target
    int *tracks;                                    // PLAY: the new queue (the player frees it)
    int count;
    const player_source_t *source;                  // PLAY: whose items they are
} cmd_t;

// What to do after a track: play queue position `index` from `ms`, or stop (index -1)
typedef struct {
    int index;
    uint32_t ms;
} next_t;

static QueueHandle_t queue;
static SemaphoreHandle_t status_lock;
static SemaphoreHandle_t stopped_sem;               // given when a STOP has been carried out
static player_status_t st = {.index = -1};
static uint32_t base_ms;                            // position when audio_frames_played() was 0
static bool seeking;
static void (*notify_cb)(void);

static uint8_t *in_buf, *out_buf;
static uint32_t out_size;

static int *q;                                      // the queue: item ids for `src`
static volatile int qlen;
static const player_source_t *src;

// Takes over a PLAY command's queue
static void install_queue(cmd_t *c)
{
    int *old = q;
    q = c->tracks;
    qlen = c->count;
    src = c->source;
    free(old);
    c->tracks = NULL;
}

const int *player_queue(int *count)
{
    *count = qlen;
    return q;
}

// ---------- status ----------

static void lock(void) { xSemaphoreTake(status_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(status_lock); }

static void notify(void)
{
    if (notify_cb) {
        notify_cb();
    }
}

static uint32_t elapsed_locked(void)
{
    if (seeking || st.rate == 0) {
        return base_ms;
    }
    return base_ms + (uint32_t)(audio_frames_played() * 1000 / st.rate);
}

void player_status(player_status_t *out)
{
    lock();
    *out = st;
    out->elapsed_ms = elapsed_locked();
    if (out->duration_ms && out->elapsed_ms > out->duration_ms) {
        out->elapsed_ms = out->duration_ms;
    }
    unlock();
}

bool player_active(void)
{
    return st.index >= 0;
}

static void set_message(const char *msg)
{
    lock();
    snprintf(st.message, sizeof(st.message), "%s", msg);
    unlock();
    ESP_LOGW(TAG, "%s", msg);
}

// ---------- commands (main task) ----------

static void send(cmd_kind_t kind, int index, uint32_t ms)
{
    cmd_t c = {.kind = kind, .index = index, .ms = ms};
    xQueueSend(queue, &c, portMAX_DELAY);
}

void player_play_queue(const player_source_t *source, const int *tracks, int count, int pos, uint32_t start_ms)
{
    if (count <= 0 || source == NULL) {
        return;
    }
    int *copy = heap_caps_malloc(count * sizeof(int), MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, tracks, count * sizeof(int));
    cmd_t c = {.kind = CMD_PLAY, .index = pos, .ms = start_ms, .tracks = copy, .count = count, .source = source};
    xQueueSend(queue, &c, portMAX_DELAY);
}
void player_toggle_pause(void) { send(CMD_PAUSE, 0, 0); }
void player_next(void) { send(CMD_NEXT, 0, 0); }
void player_prev(void) { send(CMD_PREV, 0, 0); }

void player_seek(int delta_ms)
{
    lock();
    if (st.index < 0) {
        unlock();
        return;
    }
    int64_t target = (int64_t)elapsed_locked() + delta_ms;
    if (target < 0) {
        target = 0;
    }
    base_ms = (uint32_t)target;                     // shown straight away
    seeking = true;
    unlock();
    send(CMD_SEEK, 0, (uint32_t)target);
}

void player_stop(void)
{
    xSemaphoreTake(stopped_sem, 0);                 // clear a stale one
    send(CMD_STOP, 0, 0);
    xSemaphoreTake(stopped_sem, pdMS_TO_TICKS(3000));
}

// ---------- decoding ----------

static esp_audio_simple_dec_type_t dec_type(media_format_t f)
{
    switch (f) {
    case MEDIA_MP3:  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case MEDIA_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case MEDIA_WAV:  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case MEDIA_M4A:  return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case MEDIA_AAC:  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case MEDIA_OGG:  return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    default:         return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static esp_audio_simple_dec_handle_t open_decoder(media_format_t f)
{
    esp_aac_dec_cfg_t aac = {.aac_plus_enable = true};
    esp_m4a_dec_cfg_t m4a = {.aac_plus_enable = true};
    esp_audio_simple_dec_cfg_t cfg = {.dec_type = dec_type(f)};
    if (f == MEDIA_AAC) {
        cfg.dec_cfg = &aac;
        cfg.cfg_size = sizeof(aac);
    } else if (f == MEDIA_M4A) {
        cfg.dec_cfg = &m4a;
        cfg.cfg_size = sizeof(m4a);
    }
    esp_audio_simple_dec_handle_t h = NULL;
    if (esp_audio_simple_dec_open(&cfg, &h) != ESP_AUDIO_ERR_OK) {
        return NULL;
    }
    return h;
}

// Jumping into the middle works for formats whose decoder finds its place again from any
// byte: MP3 and AAC frames carry sync words, and FLAC frames too once the stream header
// has been seen; WAV is plain samples. M4A and OGG need their index, so they're decoded
// from the start and thrown away up to the target instead.
static bool can_jump(const media_info_t *m)
{
    return m->duration_ms > 0 && m->audio_end > m->audio_offset &&
           (m->format == MEDIA_MP3 || m->format == MEDIA_AAC || m->format == MEDIA_FLAC || m->format == MEDIA_WAV);
}

typedef struct {
    FILE *f;
    media_info_t info;
    esp_audio_simple_dec_handle_t dec;
    uint32_t discard;                               // frames still to throw away (seek by decoding)
    uint64_t decoded;                               // frames decoded since the stream start (no jump)
    bool jumped;                                    // decoded counts from a jump, not the start
    bool header_pending;                            // send the stream header before the next read
    uint32_t jump_to;                               // file offset after the header
    bool fresh;                                     // nothing decoded since the last jump / start
    uint32_t in_len, in_pos;
    bool eof;
} stream_t;

// Rewinds the stream so the next audio heard is at target_ms
static void seek_stream(stream_t *s, uint32_t target_ms)
{
    const media_info_t *m = &s->info;
    s->in_len = s->in_pos = 0;
    s->eof = false;
    s->fresh = true;
    esp_audio_simple_dec_reset(s->dec);
    if (target_ms > 0 && can_jump(m)) {
        uint64_t span = m->audio_end - m->audio_offset;
        uint64_t off = span * target_ms / m->duration_ms;
        if (m->format == MEDIA_WAV && st.channels && st.bits) {
            uint32_t align = st.channels * st.bits / 8;
            off -= off % align;
        }
        s->jump_to = m->audio_offset + (uint32_t)off;
        s->header_pending = m->format == MEDIA_FLAC || m->format == MEDIA_WAV;
        if (!s->header_pending) {
            fseek(s->f, (long)s->jump_to, SEEK_SET);
        }
        s->jumped = true;
        s->discard = 0;
    } else {
        fseek(s->f, 0, SEEK_SET);
        s->header_pending = false;
        s->jumped = false;
        s->decoded = 0;
        s->discard = st.rate ? (uint32_t)((uint64_t)target_ms * st.rate / 1000) : 0;
        if (st.rate == 0 && target_ms) {
            // Rate not known yet (seek before the first frame): estimate it from the header
            s->discard = (uint32_t)((uint64_t)target_ms * (m->sample_rate ? m->sample_rate : 44100) / 1000);
        }
    }
}

// Reads the next input block. For a FLAC/WAV jump, the stream header comes first.
static bool fill(stream_t *s)
{
    if (s->header_pending) {
        s->header_pending = false;
        uint32_t n = s->info.audio_offset - s->info.header_start;
        if (n > IN_SIZE) {
            n = IN_SIZE;                                    // huge metadata (cover art): parser copes
        }
        fseek(s->f, (long)s->info.header_start, SEEK_SET);
        s->in_len = fread(in_buf, 1, n, s->f);
        s->in_pos = 0;
        fseek(s->f, (long)s->jump_to, SEEK_SET);
        return s->in_len > 0;
    }
    s->in_len = fread(in_buf, 1, IN_SIZE, s->f);
    s->in_pos = 0;
    s->eof = s->in_len < IN_SIZE;
    return s->in_len > 0 || s->eof;
}

// ---------- one track ----------

static bool start_output(const esp_audio_simple_dec_info_t *info)
{
    if (info->sample_rate > 96000) {
        char m[64];
        snprintf(m, sizeof(m), "Can't play %lu kHz files", (unsigned long)(info->sample_rate / 1000));
        set_message(m);
        return false;
    }
    if (!audio_start(info->sample_rate)) {
        set_message("No DAC: check the TLV320 wiring");
        return false;
    }
    return true;
}

// Waits for a command while paused. After PAUSE_POWER_DOWN_MS the DAC is switched off
// (the position is kept in base_ms) and *was_off is set: the caller restarts the output.
static void wait_paused(cmd_t *c, bool *was_off)
{
    *was_off = false;
    if (xQueueReceive(queue, c, pdMS_TO_TICKS(PAUSE_POWER_DOWN_MS)) == pdTRUE) {
        return;
    }
    ESP_LOGI(TAG, "paused for long: DAC off");
    lock();
    base_ms = elapsed_locked();
    unlock();
    audio_stop();
    *was_off = true;
    xQueueReceive(queue, c, portMAX_DELAY);
}

static next_t play_track(int index, uint32_t start_ms)
{
    next_t next = {.index = -1};
    int track = q[index];
    static player_item_t item;                      // big: not on the stack
    memset(&item, 0, sizeof(item));
    if (!src->get(track, &item)) {
        return (next_t){.index = index + 1, .ms = PLAYER_ITEM_START};
    }
    if (start_ms == PLAYER_ITEM_START) {
        start_ms = item.start_ms == PLAYER_ITEM_START ? 0 : item.start_ms;
    }
    const char *path = item.path;
    stream_t s = {0};

    lock();
    memset(&st, 0, sizeof(st));
    st.index = track;
    st.source = src;
    st.queue_pos = index;
    st.queue_len = qlen;
    snprintf(st.title, sizeof(st.title), "%s", item.title);
    snprintf(st.artist, sizeof(st.artist), "%s", item.artist);
    snprintf(st.album, sizeof(st.album), "%s", item.album);
    base_ms = start_ms;
    seeking = false;
    unlock();

    if (!media_info_read(path, &s.info) || (s.f = fopen(path, "rb")) == NULL) {
        set_message("Can't open the file");
        notify();
        return (next_t){.index = index + 1, .ms = PLAYER_ITEM_START};
    }
    lock();
    st.duration_ms = s.info.duration_ms;
    st.format = s.info.format;
    unlock();
    notify();
    ESP_LOGI(TAG, "%d: %s (%s, %lu s)", index, path, media_format_name(s.info.format),
             (unsigned long)(s.info.duration_ms / 1000));

    s.dec = open_decoder(s.info.format);
    if (s.dec == NULL) {
        fclose(s.f);
        set_message("Can't decode this file");
        notify();
        return (next_t){.index = index + 1, .ms = PLAYER_ITEM_START};
    }
    audio_flush();
    seek_stream(&s, start_ms);
    bool output_ok = audio_running();
    bool failed = false;
    int stuck = 0;                                          // decoder calls in a row that did nothing

    while (1) {
        // Commands: waited for while paused, otherwise checked between blocks
        cmd_t c;
        bool have;
        if (st.paused) {
            bool was_off;
            wait_paused(&c, &was_off);
            have = true;
            if (was_off && c.kind != CMD_STOP && c.kind != CMD_PLAY) {
                audio_start(st.rate);                       // back on, still paused
                audio_pause(true);
                if (c.kind != CMD_SEEK) {
                    lock();
                    uint32_t pos = base_ms;
                    unlock();
                    seek_stream(&s, pos);                   // the buffered audio went with the DAC
                }
            }
        } else {
            have = xQueueReceive(queue, &c, 0) == pdTRUE;
        }
        if (have) {
            if (c.kind == CMD_PAUSE) {
                lock();
                st.paused = !st.paused;
                bool p = st.paused;
                unlock();
                audio_pause(p);
                notify();
                continue;                                   // paused: wait at the top of the loop
            }
            if (c.kind == CMD_SEEK) {
                cmd_t more;                                 // presses queued meanwhile: only the last counts
                while (xQueuePeek(queue, &more, 0) == pdTRUE && more.kind == CMD_SEEK) {
                    xQueueReceive(queue, &c, 0);
                }
                if (st.duration_ms && c.ms + 1000 >= st.duration_ms) {
                    audio_flush();
                    if (src->finished) {
                        src->finished(track);               // skipped past the end counts as played
                    }
                    next = (next_t){.index = src->advance ? index + 1 : -1, .ms = PLAYER_ITEM_START};
                    goto done;
                }
                audio_flush();
                seek_stream(&s, c.ms);
                lock();
                base_ms = c.ms;
                seeking = s.discard > 0;
                unlock();
                notify();
                continue;
            }
            switch (c.kind) {
            case CMD_PLAY: install_queue(&c); next = (next_t){c.index, c.ms}; break;
            case CMD_NEXT: next = (next_t){index + 1, PLAYER_ITEM_START}; break;
            case CMD_PREV: {
                player_status_t now;
                player_status(&now);
                next = now.elapsed_ms > PREV_RESTART_MS || index == 0 ? (next_t){index, 0}
                                                                       : (next_t){index - 1, PLAYER_ITEM_START};
                break;
            }
            default: next = (next_t){-1, 0}; break;          // STOP
            }
            audio_flush();
            goto done;
        }

        // Next block of input
        if (s.in_pos >= s.in_len) {
            if (s.eof) {
                break;
            }
            if (!fill(&s)) {
                set_message("Card read error");
                failed = true;
                break;
            }
        }
        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buf + s.in_pos,
            .len = s.in_len - s.in_pos,
            .eos = s.eof,
        };
        esp_audio_simple_dec_out_t out = {.buffer = out_buf, .len = out_size};
        esp_audio_err_t ret = esp_audio_simple_dec_process(s.dec, &raw, &out);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t *bigger = heap_caps_realloc(out_buf, out.needed_size, MALLOC_CAP_8BIT);
            if (bigger == NULL) {
                set_message("Out of memory");
                failed = true;
                break;
            }
            out_buf = bigger;
            out_size = out.needed_size;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            if (s.jumped && s.fresh) {
                // The decoder didn't find its feet after a jump: decode from the start instead
                ESP_LOGW(TAG, "jump failed (%d), seeking by decoding", ret);
                lock();
                uint32_t target = base_ms;
                unlock();
                s.info.duration_ms = 0;                     // can_jump() false for this stream from now on
                seek_stream(&s, target);
                lock();
                seeking = s.discard > 0;
                unlock();
                continue;
            }
            ESP_LOGW(TAG, "decode error %d", ret);
            set_message("This file is damaged or unsupported");
            failed = true;
            break;
        }
        s.in_pos += raw.consumed;
        if (raw.consumed == 0 && out.decoded_size == 0) {
            if (s.eof || ++stuck > 100) {
                break;                                      // nothing more to come
            }
        } else {
            stuck = 0;
        }
        if (out.decoded_size == 0) {
            continue;
        }

        // Output
        if (s.fresh || st.rate == 0) {
            esp_audio_simple_dec_info_t info = {0};
            esp_audio_simple_dec_get_info(s.dec, &info);
            if (info.sample_rate != st.rate || !output_ok) {
                if (!start_output(&info)) {
                    failed = true;
                    if (!audio_running() && info.sample_rate <= 96000) {
                        next = (next_t){-1, 0};             // no DAC: stop rather than skip everything
                        goto done;
                    }
                    break;
                }
                output_ok = true;
                audio_flush();
            }
            lock();
            st.rate = info.sample_rate;
            st.bits = info.bits_per_sample;
            st.channels = info.channel;
            unlock();
            if (s.fresh && st.paused) {
                audio_pause(true);
            }
            s.fresh = false;
        }
        uint32_t frame_bytes = st.channels * (st.bits / 8);
        if (frame_bytes == 0) {
            continue;
        }
        uint32_t frames = out.decoded_size / frame_bytes;
        uint32_t skip = s.discard < frames ? s.discard : frames;
        s.discard -= skip;
        s.decoded += frames;
        if (skip < frames) {
            if (seeking) {
                lock();
                seeking = false;
                unlock();
                notify();
            }
            audio_write(out_buf + skip * frame_bytes, frames - skip, st.bits, st.channels);
        }
    }

    // End of the track (or a problem)
    audio_drain();
    next = (next_t){.index = index + 1, .ms = PLAYER_ITEM_START};
    if (failed) {
        notify();
    } else {
        if (src->finished) {
            src->finished(track);
        }
        if (!src->advance) {
            next.index = -1;                                // one item, then stop
        }
    }
done:
    esp_audio_simple_dec_close(s.dec);
    fclose(s.f);
    return next;
}

// ---------- task ----------

static void player_task(void *arg)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    next_t next = {.index = -1};
    while (1) {
        if (next.index < 0) {
            cmd_t c;
            xQueueReceive(queue, &c, portMAX_DELAY);
            if (c.kind == CMD_PLAY) {
                install_queue(&c);
                next = (next_t){c.index, c.ms};
            } else if (c.kind == CMD_STOP) {
                audio_stop();
                xSemaphoreGive(stopped_sem);
            }
            continue;                                       // nothing loaded: other commands are moot
        }
        if (next.index >= qlen || next.index < 0 || q == NULL) {
            // End of the list
            audio_stop();
            lock();
            st.index = -1;
            st.paused = false;
            unlock();
            notify();
            next.index = -1;
            continue;
        }
        lock();
        st.message[0] = '\0';
        unlock();
        next = play_track(next.index, next.ms);
        if (next.index < 0) {
            audio_stop();
            lock();
            st.index = -1;
            st.paused = false;
            unlock();
            notify();
            xSemaphoreGive(stopped_sem);
        }
    }
}

void player_init(void (*notify_fn)(void))
{
    if (queue) {
        return;
    }
    notify_cb = notify_fn;
    queue = xQueueCreate(16, sizeof(cmd_t));
    status_lock = xSemaphoreCreateMutex();
    stopped_sem = xSemaphoreCreateBinary();
    in_buf = heap_caps_malloc(IN_SIZE, MALLOC_CAP_8BIT);
    out_size = OUT_START_SIZE;
    out_buf = heap_caps_malloc(out_size, MALLOC_CAP_8BIT);
    xTaskCreate(player_task, "player", 12 * 1024, NULL, 8, NULL);
}
