#include "voice_app.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "audio.h"                      // the microphone
#include "buttons.h"
#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "local_time.h"                 // notes are named by the date and time
#include "media_ui.h"                   // list / Now Playing screens shared with Music and Podcasts
#include "player.h"
#include "power.h"                      // power_add_off_hook(): save a recording when turning off
#include "sdcard.h"
#include "settings.h"                   // SETTING_VOICE_DIR: the folder, chosen in Settings
#include "ui_sidebar.h"

static const char *TAG = "voice";

#define RATE            16000           // Hz, mono, 16-bit: 32 KB per second, ~1.9 MB a minute
#define MAX_NOTES       500
#define MAX_SECONDS     (60 * 60)       // a recording stops by itself after an hour
#define READ_FRAMES     1024
#define SEEK_MS         5000
#define VOLUME_STEP     2
#define TICK_PLAYING_MS 5000            // Now Playing time updated this often
#define TICK_REC_MS     1000            // recording screen: elapsed time and level

typedef enum { V_LIST, V_RECORDING, V_PLAYING, V_DELETE, V_MESSAGE } view_t;

static view_t view = V_LIST;
static int sel;                         // list row: 0 = "New note", i + 1 = notes[i]
static char (*notes)[32];               // file names, newest first (PSRAM)
static int note_count;
static TickType_t next_tick;
static char message[64];

// ---------- notes on the card ----------

// The notes folder (full path), e.g. "/sdcard/Voice Notes", from Settings
static void voice_dir(char *out, size_t size)
{
    char dir[128];
    settings_get_str(SETTING_VOICE_DIR, dir, sizeof(dir), SETTING_VOICE_DIR_DEFAULT);
    snprintf(out, size, "%s%s", SDCARD_ROOT, strcmp(dir, "/") ? dir : "");
}

static int newest_first(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);
}

// "20261123-144105.wav" sorts by date and time; notes made before the clock was set are
// "note-0007.wav" and sort above them
static void load_notes(void)
{
    note_count = 0;
    if (!sdcard_mount()) {
        return;
    }
    char dir[160];
    voice_dir(dir, sizeof(dir));
    mkdir(dir, 0777);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && note_count < MAX_NOTES) {
        size_t n = strlen(e->d_name);
        if (n > 4 && n < sizeof(notes[0]) && !strcasecmp(e->d_name + n - 4, ".wav")) {
            memcpy(notes[note_count++], e->d_name, n + 1);  // n < 32: fits with its '\0'
        }
    }
    if (d) {
        closedir(d);
    }
    qsort(notes, note_count, sizeof(notes[0]), newest_first);
}

static void note_path(const char *name, char *out, size_t size)
{
    char dir[160];
    voice_dir(dir, sizeof(dir));
    snprintf(out, size, "%s/%s", dir, name);
}

// Display name: "Mon 23 Nov  2:41 PM" from the file name
static void note_label(const char *name, char *out, size_t size)
{
    struct tm t = {0};
    if (sscanf(name, "%4d%2d%2d-%2d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        mktime(&t);                                         // fills in the weekday
        static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
#if LOCAL_TIME_24H
        snprintf(out, size, "%s %d %s  %02d:%02d", days[t.tm_wday % 7], t.tm_mday, months[t.tm_mon % 12],
                 t.tm_hour, t.tm_min);
#else
        int h = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
        snprintf(out, size, "%s %d %s  %d:%02d %s", days[t.tm_wday % 7], t.tm_mday, months[t.tm_mon % 12],
                 h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
#endif
        return;
    }
    int n;
    if (sscanf(name, "note-%d", &n) == 1) {
        snprintf(out, size, "Note %d (clock not set)", n);
        return;
    }
    snprintf(out, size, "%.*s", (int)strcspn(name, "."), name);
}

// Length from the file size (44-byte header, then 2 bytes per sample)
static uint32_t note_ms(const char *name)
{
    char path[200];
    struct stat st;
    note_path(name, path, sizeof(path));
    if (stat(path, &st) != 0 || st.st_size <= 44) {
        return 0;
    }
    return (uint32_t)((uint64_t)(st.st_size - 44) * 1000 / (RATE * 2));
}

// ---------- recording ----------

static TaskHandle_t rec_task;
static SemaphoreHandle_t rec_done;
static volatile bool rec_stop;
static volatile uint32_t rec_frames;    // recorded so far
static volatile int rec_level;          // 0-100: loudness of the last half second, for the meter
static volatile bool rec_failed;        // card full / write error
static char rec_name[32];

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

// The 44-byte WAV header for `data_bytes` of 16-bit mono PCM
static void wav_header(uint8_t h[44], uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);                                    // PCM
    put_le16(h + 22, 1);                                    // mono
    put_le32(h + 24, RATE);
    put_le32(h + 28, RATE * 2);
    put_le16(h + 32, 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

static void recorder(void *arg)
{
    FILE *f = arg;
    int16_t *buf = heap_caps_malloc(READ_FRAMES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int peak = 0, peak_frames = 0;
    while (buf && !rec_stop && rec_frames < (uint32_t)MAX_SECONDS * RATE) {
        size_t n = audio_rec_read(buf, READ_FRAMES);
        if (n == 0) {
            continue;
        }
        if (fwrite(buf, sizeof(int16_t), n, f) != n) {
            rec_failed = true;
            break;
        }
        rec_frames += n;
        for (size_t i = 0; i < n; i++) {
            int a = buf[i] < 0 ? -buf[i] : buf[i];
            peak = a > peak ? a : peak;
        }
        if ((peak_frames += n) >= RATE / 2) {
            rec_level = peak * 100 / 32768;
            peak = peak_frames = 0;
        }
    }
    free(buf);
    uint8_t h[44];
    wav_header(h, rec_frames * 2);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fclose(f);
    rec_task = NULL;
    xSemaphoreGive(rec_done);
    vTaskDelete(NULL);
}

static bool start_recording(void)
{
    player_stop();                                          // the microphone shares the I2S clocks
    if (!sdcard_mount()) {
        snprintf(message, sizeof(message), "No microSD card");
        return false;
    }
    char dir[160];
    voice_dir(dir, sizeof(dir));
    mkdir(dir, 0777);
    struct tm t;
    if (local_time_now(&t)) {
        strftime(rec_name, sizeof(rec_name), "%Y%m%d-%H%M%S.wav", &t);
    } else {
        int max = 0, n;
        for (int i = 0; i < note_count; i++) {
            if (sscanf(notes[i], "note-%d", &n) == 1 && n > max) {
                max = n;
            }
        }
        snprintf(rec_name, sizeof(rec_name), "note-%04d.wav", max + 1);
    }
    char path[200];
    note_path(rec_name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        snprintf(message, sizeof(message), "Can't write to the card");
        return false;
    }
    setvbuf(f, NULL, _IOFBF, 16 * 1024);                    // fewer, bigger card writes
    uint8_t h[44];
    wav_header(h, 0);
    fwrite(h, 1, sizeof(h), f);
    if (!audio_rec_start(RATE)) {
        fclose(f);
        remove(path);
        snprintf(message, sizeof(message), "Microphone didn't start");
        return false;
    }
    rec_stop = false;
    rec_failed = false;
    rec_frames = 0;
    rec_level = 0;
    if (rec_done == NULL) {
        rec_done = xSemaphoreCreateBinary();
    }
    xTaskCreate(recorder, "voice_rec", 4096, f, 10, &rec_task);
    ESP_LOGI(TAG, "recording %s", rec_name);
    return true;
}

// Stops and saves. A recording under half a second is dropped (a stray press).
static void stop_recording(void)
{
    if (rec_task == NULL) {
        return;
    }
    rec_stop = true;
    xSemaphoreTake(rec_done, portMAX_DELAY);
    audio_rec_stop();
    if (rec_frames < RATE / 2) {
        char path[200];
        note_path(rec_name, path, sizeof(path));
        remove(path);
        ESP_LOGI(TAG, "too short, not kept");
    } else {
        ESP_LOGI(TAG, "saved %s (%lu s)", rec_name, (unsigned long)(rec_frames / RATE));
    }
}

// ---------- playback (the shared player) ----------

static char play_path[200], play_label[48];

static bool voice_get(int id, player_item_t *out)
{
    snprintf(out->path, sizeof(out->path), "%s", play_path);
    snprintf(out->title, sizeof(out->title), "%s", play_label);
    snprintf(out->artist, sizeof(out->artist), "Voice note");
    out->album[0] = '\0';
    out->start_ms = 0;
    return true;
}

static const player_source_t voice_source = {.get = voice_get, .advance = false};

static bool voice_playing(void)
{
    player_status_t ps;
    player_status(&ps);
    return ps.index >= 0 && ps.source == &voice_source;
}

// ---------- screens ----------

static void row(int i, media_row_t *out)
{
    static char label[48];
    if (i == 0) {
        out->label = "+ New note";
        return;
    }
    const char *name = notes[i - 1];
    note_label(name, label, sizeof(label));
    out->label = label;
    media_ui_format_time(out->tag, sizeof(out->tag), note_ms(name));
    char path[200];
    note_path(name, path, sizeof(path));
    out->playing = voice_playing() && !strcmp(path, play_path);
}

static void draw_list(bool full)
{
    media_ui_list("Voice Notes", note_count + 1, sel, row, NULL, full);
}

static void draw_recording(bool full)
{
    bool changed = media_ui_screen_changes("recording");
    full = media_ui_begin(full || changed);
    ui_sidebar("Recording", NULL);
    char t[16];
    media_ui_format_time(t, sizeof(t), (uint32_t)((uint64_t)rec_frames * 1000 / RATE));
    const int x = UI_CONTENT_X + 12;
    epd_fill_circle(x + 8, 30, 8, EPD_BLACK);               // the "recording" dot
    epd_draw_text(x + 26, 16, t, 4, EPD_BLACK);
    // Level meter: how loud the last half second was (a square-root scale shows quiet speech)
    const int mw = EPD_WIDTH - x - 12, my = 62;
    epd_draw_rect(x, my, mw, 12, EPD_BLACK, false);
    int level = rec_level;
    int fill = 0;
    for (int v = 0; v * v < level * 100 && fill < mw - 4; v++) {
        fill = (v + 1) * (mw - 4) / 10;
    }
    epd_draw_rect(x + 2, my + 2, fill < mw - 4 ? fill : mw - 4, 8, EPD_BLACK, true);
    media_ui_small_centered(88, rec_failed ? "The card is full: recording stopped" : "Speak a hand's width from the microphone");
    media_ui_small_centered(EPD_HEIGHT - 12, "press or BACK: stop and save");
    media_ui_finish(full);
}

static void draw_playing(bool full)
{
    media_ui_playing(true, "< > 5 s   hold < >: volume", "", full);
    next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_PLAYING_MS);
}

static void draw_delete(void)
{
    char label[48];
    note_label(notes[sel - 1], label, sizeof(label));
    media_ui_message("Voice Notes", "Delete this note?", label, "hold center: delete     BACK: keep");
}

static void redraw(bool full)
{
    switch (view) {
    case V_RECORDING: draw_recording(full); break;
    case V_PLAYING: draw_playing(full); break;
    case V_DELETE: draw_delete(); break;
    case V_MESSAGE: media_ui_message("Voice Notes", message, "", "BACK: the list"); break;
    default: draw_list(full); break;
    }
}

static void show_list(bool full)
{
    view = V_LIST;
    load_notes();
    if (sel > note_count) {
        sel = note_count;
    }
    draw_list(full);
}

// ---------- app callbacks ----------

static void notify_from_player(void)
{
    buttons_post(BUTTON_WAKE);                              // the main loop calls voice_button()
}

static void power_off_hook(void)
{
    stop_recording();                                       // keep what was recorded
}

static void voice_init(const app_t *self)
{
    notes = heap_caps_malloc(MAX_NOTES * sizeof(notes[0]), MALLOC_CAP_SPIRAM);
    player_init(notify_from_player);                        // already started by Music: no-op
    power_add_off_hook(power_off_hook);
}

static void voice_open(const app_t *self)
{
    sel = 0;
    if (notes == NULL) {
        snprintf(message, sizeof(message), "Out of memory");
        view = V_MESSAGE;
        redraw(true);
        return;
    }
    if (!sdcard_mount()) {
        snprintf(message, sizeof(message), "No microSD card");
        view = V_MESSAGE;
        redraw(true);
        return;
    }
    show_list(true);
}

static bool list_button(button_event_t event)
{
    int n = note_count + 1;
    switch (event) {
    case BUTTON_LEFT:       sel = (sel + n - 1) % n; break;
    case BUTTON_RIGHT:      sel = (sel + 1) % n; break;
    case BUTTON_LEFT_HOLD:  sel = sel >= MEDIA_UI_ROWS ? sel - MEDIA_UI_ROWS : 0; break;
    case BUTTON_RIGHT_HOLD: sel = sel + MEDIA_UI_ROWS < n ? sel + MEDIA_UI_ROWS : n - 1; break;
    case BUTTON_SELECT:
        if (sel == 0) {
            if (start_recording()) {
                view = V_RECORDING;
                next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_REC_MS);
            } else {
                view = V_MESSAGE;
            }
            redraw(false);
        } else {
            note_path(notes[sel - 1], play_path, sizeof(play_path));
            note_label(notes[sel - 1], play_label, sizeof(play_label));
            int id = 0;
            player_play_queue(&voice_source, &id, 1, 0, 0);
            view = V_PLAYING;
            draw_playing(false);
        }
        return true;
    case BUTTON_SELECT_HOLD:
        if (sel == 0) {
            return list_button(BUTTON_SELECT);              // "New note": a long press records too
        }
        view = V_DELETE;
        draw_delete();
        return true;
    case BUTTON_WAKE:
        break;                                              // playback ended: the mark goes
    case BUTTON_BACK:
        return false;
    default:
        return true;
    }
    draw_list(false);
    return true;
}

static bool voice_button(const app_t *self, button_event_t event)
{
    switch (view) {
    case V_RECORDING:
        if (event == BUTTON_SELECT || event == BUTTON_SELECT_HOLD || event == BUTTON_BACK) {
            stop_recording();
            sel = 0;
            show_list(false);
        }
        return true;

    case V_PLAYING:
        switch (event) {
        case BUTTON_LEFT:  player_seek(-SEEK_MS); break;
        case BUTTON_RIGHT: player_seek(SEEK_MS); break;
        case BUTTON_LEFT_HOLD:
        case BUTTON_RIGHT_HOLD: {
            int v = audio_volume() + (event == BUTTON_RIGHT_HOLD ? VOLUME_STEP : -VOLUME_STEP);
            audio_set_volume(v < 0 ? 0 : v > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : v);
            break;
        }
        case BUTTON_SELECT:
        case BUTTON_SELECT_HOLD:
            player_toggle_pause();
            break;
        case BUTTON_WAKE:
            if (!voice_playing()) {                         // reached the end
                show_list(false);
                return true;
            }
            break;
        case BUTTON_BACK:
            if (voice_playing()) {
                player_stop();
            }
            show_list(false);
            return true;
        default:
            return true;
        }
        draw_playing(false);
        return true;

    case V_DELETE:
        if (event == BUTTON_SELECT_HOLD) {
            char path[200];
            note_path(notes[sel - 1], path, sizeof(path));
            if (voice_playing() && !strcmp(path, play_path)) {
                player_stop();
            }
            remove(path);
            ESP_LOGI(TAG, "deleted %s", notes[sel - 1]);
            show_list(false);
        } else if (event == BUTTON_BACK) {
            show_list(false);
        }
        return true;

    case V_MESSAGE:
        if (event == BUTTON_BACK || event == BUTTON_SELECT) {
            if (!sdcard_mounted() && !sdcard_mount()) {
                return false;                               // still no card: leave
            }
            show_list(true);
        }
        return true;

    default:
        return list_button(event);
    }
}

static TickType_t voice_next_wakeup(const app_t *self)
{
    if (view == V_RECORDING) {
        return app_ticks_until(next_tick);
    }
    if (view == V_PLAYING) {
        TickType_t wait = media_ui_marquee_wait();
        if (voice_playing()) {
            TickType_t t = app_ticks_until(next_tick);
            wait = t < wait ? t : wait;
        }
        return wait;
    }
    return portMAX_DELAY;
}

static void voice_tick(const app_t *self)
{
    if (view == V_PLAYING && media_ui_marquee_step()) {
        draw_playing(false);                                // long lines scroll
        return;
    }
    if (!app_due(next_tick)) {
        return;
    }
    if (view == V_RECORDING) {
        if (rec_task == NULL) {                             // stopped by itself (an hour, card full)
            stop_recording();
            show_list(false);
            return;
        }
        next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_REC_MS);
        draw_recording(false);
    } else if (view == V_PLAYING) {
        draw_playing(false);
    }
}

static void voice_close(const app_t *self)
{
    stop_recording();
    if (voice_playing()) {
        player_stop();
    }
    view = V_LIST;
}

// Menu icon: a microphone on a stand
static void voice_icon(const app_t *self, int cx, int cy)
{
    epd_fill_circle(cx, cy - 8, 7, EPD_BLACK);              // the capsule: rounded top and bottom
    epd_draw_rect(cx - 7, cy - 8, 15, 8, EPD_BLACK, true);
    epd_fill_circle(cx, cy, 7, EPD_BLACK);
    for (int t = 0; t < 2; t++) {                           // the cradle around it
        epd_draw_line(cx - 11 - t, cy - 4, cx - 11 - t, cy + 1, EPD_BLACK);
        epd_draw_line(cx + 11 + t, cy - 4, cx + 11 + t, cy + 1, EPD_BLACK);
        epd_draw_line(cx - 11, cy + 2 + t, cx - 5, cy + 9 + t, EPD_BLACK);
        epd_draw_line(cx + 11, cy + 2 + t, cx + 5, cy + 9 + t, EPD_BLACK);
        epd_draw_line(cx - 5, cy + 9 + t, cx + 5, cy + 9 + t, EPD_BLACK);
    }
    epd_draw_rect(cx - 1, cy + 10, 3, 6, EPD_BLACK, true);  // stand
    epd_draw_rect(cx - 7, cy + 15, 15, 3, EPD_BLACK, true); // base
}

const app_t voice_app = {
    .id = "voice",
    .name = "Voice",
    .draw_icon = voice_icon,
    .init = voice_init,
    .open = voice_open,
    .button = voice_button,
    .next_wakeup = voice_next_wakeup,
    .tick = voice_tick,
    .close = voice_close,
    .select_hold = true,                // hold center on a note: delete
};
