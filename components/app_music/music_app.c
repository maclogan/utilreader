#include "music_app.h"

#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "buttons.h"                    // buttons_post(BUTTON_WAKE)
#include "epd.h"
#include "esp_log.h"
#include "font16.h"
#include "library.h"
#include "player.h"
#include "power.h"                      // power_add_off_hook()
#include "settings.h"
#include "ui_sidebar.h"

static const char *TAG = "music";

#define KEY_VOLUME      "music_vol"
#define KEY_LAST        "music_last"    // path of the last track played
#define KEY_POS         "music_pos"     // its position, ms

#define MARGIN          4
#define X0              (UI_CONTENT_X + 6)
#define RIGHT           (EPD_WIDTH - MARGIN)
#define FOOTER_Y        (EPD_HEIGHT - 9)
#define ROW_H           18
#define ROWS            6
#define SEEK_STEP_MS    10000
#define TICK_MS         10000           // Now playing: time updated this often
#define SAVE_EVERY_MS   (2 * 60 * 1000) // position saved this often while playing
#define FULL_EVERY      30              // partial refreshes between full ones (ghosting)
#define ELLIPSIS        0x2026

typedef enum { SCREEN_LIBRARY, SCREEN_PLAYING, SCREEN_MESSAGE } screen_t;

static screen_t screen;
static library_status_t lib_status = LIBRARY_NO_CARD;
static bool scanned;
static char scanned_dir[64];
static int selected;
static bool seek_mode;                  // false = VOL mode
static int partials;
static TickType_t next_tick, last_save;

// ---------- small drawing helpers ----------

static bool begin(bool full)
{
    if (!full && (!epd_is_awake() || ++partials >= FULL_EVERY)) {
        full = true;
    }
    if (full) {
        partials = 0;
        epd_init();
    }
    epd_clear(EPD_WHITE);
    return full;
}

static void finish(bool full)
{
    if (full) {
        epd_display();
    } else {
        epd_display_partial();
    }
}

// 16 px text from x, cut with "…" before right_edge
static void text_cut(int x, int y, const char *text, int right_edge, uint8_t color)
{
    if (x + font16_text_width(text) <= right_edge) {
        font16_draw_text(x, y, text, color);
        return;
    }
    right_edge -= font16_char_width(ELLIPSIS);
    const char *p = text;
    while (*p) {
        const char *before = p;
        uint32_t cp = font16_next_char(&p);
        if (cp == 0 || x + font16_char_width(cp) > right_edge) {
            p = before;
            break;
        }
        x += font16_draw_char(x, y, cp, 1, color);
    }
    font16_draw_char(x, y, ELLIPSIS, 1, color);
}

static void small_centered(int y, const char *text)
{
    epd_draw_text(UI_CONTENT_CX - epd_text_width(text, 1) / 2, y, text, 1, EPD_BLACK);
}

static void format_time(char *out, size_t size, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    } else {
        snprintf(out, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
}

// ---------- screens ----------

static void draw_message(const char *title, const char *detail, const char *hint)
{
    begin(true);
    ui_sidebar("Music", NULL);
    font16_draw_text(UI_CONTENT_CX - font16_text_width(title) / 2, 30, title, EPD_BLACK);
    small_centered(58, detail);
    small_centered(FOOTER_Y, hint);
    finish(true);
}

static void draw_library(bool full)
{
    full = begin(full);
    int count = library_count();
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", selected + 1, count);
    ui_sidebar("Music", counter);

    player_status_t ps;
    player_status(&ps);
    int first = selected / ROWS * ROWS;
    for (int i = first; i < count && i < first + ROWS; i++) {
        int y = 2 + (i - first) * ROW_H;
        bool sel = i == selected;
        uint8_t color = sel ? EPD_WHITE : EPD_BLACK;
        if (sel) {
            epd_draw_rect(UI_CONTENT_X, y, UI_CONTENT_W, ROW_H - 1, EPD_BLACK, true);
        }
        char tag[8];
        snprintf(tag, sizeof(tag), "%s", i == ps.index ? (ps.paused ? "||" : ">") : "");
        int tag_w = tag[0] ? epd_text_width(tag, 1) + 6 : 0;
        if (tag[0]) {
            epd_draw_text(RIGHT - epd_text_width(tag, 1), y + 5, tag, 1, color);
        }
        text_cut(UI_CONTENT_X + MARGIN, y + 1, library_name(i), RIGHT - tag_w, color);
    }
    small_centered(FOOTER_Y, ps.index >= 0 ? "Press: play   Hold press: now playing"
                                           : "Press: play   Hold </>: page");
    finish(full);
}

static void draw_playing(bool full)
{
    player_status_t ps;
    player_status(&ps);
    full = begin(full);
    char counter[16];
    int shown = ps.index >= 0 ? ps.index : selected;
    snprintf(counter, sizeof(counter), "%d/%d", shown + 1, library_count());
    ui_sidebar("Now playing", counter);

    if (ps.index < 0) {
        font16_draw_text(X0, 4, "Stopped", EPD_BLACK);
        text_cut(X0, 24, library_name(selected), RIGHT, EPD_BLACK);
    } else {
        text_cut(X0, 3, ps.title, RIGHT, EPD_BLACK);
        text_cut(X0, 22, ps.artist[0] ? ps.artist : "", RIGHT, EPD_BLACK);
        text_cut(X0, 41, ps.album, RIGHT, EPD_BLACK);
    }

    // Progress bar and times
    const int bar_y = 62, bar_w = RIGHT - X0;
    epd_draw_rect(X0, bar_y, bar_w, 6, EPD_BLACK, false);
    if (ps.duration_ms) {
        int fill = (int)((uint64_t)bar_w * ps.elapsed_ms / ps.duration_ms);
        epd_draw_rect(X0, bar_y, fill, 6, EPD_BLACK, true);
    }
    char a[16], b[16], line[48];
    format_time(a, sizeof(a), ps.elapsed_ms);
    epd_draw_text(X0, bar_y + 10, a, 1, EPD_BLACK);
    if (ps.duration_ms) {
        format_time(b, sizeof(b), ps.duration_ms);
        epd_draw_text(RIGHT - epd_text_width(b, 1), bar_y + 10, b, 1, EPD_BLACK);
    }
    const char *state = ps.index < 0 ? "STOPPED" : ps.paused ? "PAUSED" : "PLAYING";
    if (ps.rate) {
        snprintf(line, sizeof(line), "%s  %s %lu.%lu kHz", state, media_format_name(ps.format),
                 (unsigned long)(ps.rate / 1000), (unsigned long)(ps.rate % 1000 / 100));
    } else {
        snprintf(line, sizeof(line), "%s", state);
    }
    small_centered(bar_y + 10, line);

    // Mode badge, then the volume bar or the seek step
    const int mode_y = 88;
    const char *badge = seek_mode ? "SEEK" : "VOL";
    int bw = epd_text_width(badge, 1) + 8;
    epd_draw_rect(X0, mode_y, bw, 13, EPD_BLACK, true);
    epd_draw_text(X0 + 4, mode_y + 3, badge, 1, EPD_WHITE);
    int vx = X0 + bw + 8;
    if (seek_mode) {
        epd_draw_text(vx, mode_y + 3, "</>: back / ahead 10 s", 1, EPD_BLACK);
    } else {
        int vol = audio_volume();
        for (int i = 0; i < AUDIO_VOLUME_MAX; i++) {        // one tick per step, filled up to the level
            int x = vx + i * 6;
            if (i < vol) {
                epd_draw_rect(x, mode_y + 2, 4, 9, EPD_BLACK, true);
            } else {
                epd_draw_hline(x, mode_y + 10, 4, EPD_BLACK);
            }
        }
        snprintf(line, sizeof(line), "%d", vol);
        epd_draw_text(vx + AUDIO_VOLUME_MAX * 6 + 4, mode_y + 3, line, 1, EPD_BLACK);
    }

    // Footer: a problem if there is one, else the controls
    if (ps.message[0]) {
        small_centered(FOOTER_Y, ps.message);
    } else {
        small_centered(FOOTER_Y, seek_mode ? "Hold </>: track   Hold press: volume mode"
                                           : "Hold </>: track   Hold press: seek mode");
    }
    finish(full);
    next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_MS);
}

// ---------- saving ----------

static void save_place(void)
{
    player_status_t ps;
    player_status(&ps);
    if (ps.index < 0) {
        return;
    }
    char path[300];
    library_path(ps.index, path, sizeof(path));
    settings_set_str(KEY_LAST, path);
    settings_set_i32(KEY_POS, (int32_t)ps.elapsed_ms);
    last_save = xTaskGetTickCount();
}

// ---------- library ----------

static void show_library_status(void)
{
    char dir[64], detail[96];
    settings_get_str(MUSIC_DIR_KEY, dir, sizeof(dir), MUSIC_DIR_DEFAULT);
    screen = SCREEN_MESSAGE;
    switch (lib_status) {
    case LIBRARY_NO_CARD:
        draw_message("No microSD card", "Insert a FAT32 card with music", "Press: try again   BACK: leave");
        break;
    case LIBRARY_NO_FOLDER:
        snprintf(detail, sizeof(detail), "No folder %s on the card", dir);
        draw_message("Music folder missing", detail, "Choose one in Settings   Press: retry");
        break;
    default:
        snprintf(detail, sizeof(detail), "Copy MP3/FLAC/WAV/M4A/OGG to %s", dir);
        draw_message("No music yet", detail, "Press: try again   BACK: leave");
        break;
    }
}

// Scans the folder if it's new or hasn't been scanned. Returns true if there are tracks.
static bool ensure_library(bool force)
{
    char dir[64];
    settings_get_str(MUSIC_DIR_KEY, dir, sizeof(dir), MUSIC_DIR_DEFAULT);
    if (!force && scanned && lib_status == LIBRARY_OK && !strcmp(dir, scanned_dir)) {
        return true;
    }
    if (player_active()) {
        player_stop();                                      // the list is about to change under it
    }
    lib_status = library_scan(dir);
    scanned = true;
    snprintf(scanned_dir, sizeof(scanned_dir), "%s", dir);

    // Start on the last track played
    char last[300];
    settings_get_str(KEY_LAST, last, sizeof(last), "");
    int i = library_find(last);
    selected = i >= 0 ? i : 0;
    return lib_status == LIBRARY_OK;
}

static void play_selected(void)
{
    char path[300], last[300];
    library_path(selected, path, sizeof(path));
    settings_get_str(KEY_LAST, last, sizeof(last), "");
    uint32_t start = !strcmp(path, last) ? (uint32_t)settings_get_i32(KEY_POS, 0) : 0;
    player_play(selected, start);                           // resumes where it was left
    settings_set_str(KEY_LAST, path);
    if (start == 0) {
        settings_set_i32(KEY_POS, 0);
    }
}

// ---------- app callbacks ----------

static void notify_from_player(void)
{
    buttons_post(BUTTON_WAKE);                              // the main loop calls music_button()
}

static void power_off_hook(void)
{
    if (player_active()) {
        save_place();
        player_stop();                                      // DAC off, nothing reads the card
    }
}

static void music_init(const app_t *self)
{
    audio_set_volume(settings_get_i32(KEY_VOLUME, 20));
    player_init(notify_from_player);
    power_add_off_hook(power_off_hook);
}

static void music_open(const app_t *self)
{
    if (player_active()) {
        screen = SCREEN_PLAYING;                            // back to what's playing
        draw_playing(true);
        return;
    }
    if (!ensure_library(false)) {
        show_library_status();
        return;
    }
    screen = SCREEN_LIBRARY;
    draw_library(true);
}

static bool library_button(button_event_t event)
{
    int count = library_count();
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_RIGHT:
        selected = (selected + (event == BUTTON_LEFT ? count - 1 : 1)) % count;
        draw_library(false);
        return true;
    case BUTTON_LEFT_HOLD:
        selected = selected >= ROWS ? selected - ROWS : 0;
        draw_library(false);
        return true;
    case BUTTON_RIGHT_HOLD:
        selected = selected + ROWS < count ? selected + ROWS : count - 1;
        draw_library(false);
        return true;
    case BUTTON_SELECT:
        play_selected();
        screen = SCREEN_PLAYING;
        draw_playing(false);
        return true;
    case BUTTON_SELECT_HOLD:
        if (player_active()) {
            screen = SCREEN_PLAYING;
            draw_playing(false);
        }
        return true;
    case BUTTON_WAKE:
        draw_library(false);                                // the playing marker moved
        return true;
    case BUTTON_BACK:
        return false;                                       // leave; playback carries on
    default:
        return true;
    }
}

static bool playing_button(button_event_t event)
{
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_RIGHT: {
        int dir = event == BUTTON_LEFT ? -1 : 1;
        if (seek_mode) {
            player_seek(dir * SEEK_STEP_MS);
        } else {
            audio_set_volume(audio_volume() + dir);
            settings_set_i32(KEY_VOLUME, audio_volume());
        }
        draw_playing(false);
        return true;
    }
    case BUTTON_LEFT_HOLD:
        player_prev();
        return true;                                        // redrawn when the player reports
    case BUTTON_RIGHT_HOLD:
        player_next();
        return true;
    case BUTTON_SELECT:
        if (player_active()) {
            player_toggle_pause();
            save_place();
        } else {
            play_selected();
        }
        return true;
    case BUTTON_SELECT_HOLD:
        seek_mode = !seek_mode;
        draw_playing(false);
        return true;
    case BUTTON_WAKE: {
        player_status_t ps;
        player_status(&ps);
        if (ps.index >= 0) {
            selected = ps.index;
            if (pdTICKS_TO_MS(xTaskGetTickCount() - last_save) > 5000) {
                save_place();                               // new track or pause: remember it
            }
        }
        draw_playing(false);
        return true;
    }
    case BUTTON_BACK:
        if (lib_status != LIBRARY_OK) {
            return false;
        }
        screen = SCREEN_LIBRARY;
        draw_library(false);
        return true;
    default:
        return true;
    }
}

static bool music_button(const app_t *self, button_event_t event)
{
    switch (screen) {
    case SCREEN_LIBRARY:
        return library_button(event);
    case SCREEN_PLAYING:
        return playing_button(event);
    default:                                                // a message
        if (event == BUTTON_BACK) {
            return false;
        }
        if (event == BUTTON_SELECT) {
            if (ensure_library(true)) {
                screen = SCREEN_LIBRARY;
                draw_library(true);
            } else {
                show_library_status();
            }
        }
        return true;
    }
}

static TickType_t music_next_wakeup(const app_t *self)
{
    if (screen != SCREEN_PLAYING || !player_active()) {
        return portMAX_DELAY;
    }
    player_status_t ps;
    player_status(&ps);
    return ps.paused ? portMAX_DELAY : app_ticks_until(next_tick);
}

static void music_tick(const app_t *self)
{
    if (screen == SCREEN_PLAYING && app_due(next_tick)) {
        draw_playing(false);
    }
}

static void music_background(const app_t *self)
{
    if (player_active() && pdTICKS_TO_MS(xTaskGetTickCount() - last_save) >= SAVE_EVERY_MS) {
        save_place();
    }
}

static void music_close(const app_t *self)
{
    save_place();
    ESP_LOGI(TAG, "left the app; playback %s", player_active() ? "continues" : "stopped");
}

// Menu icon: a pair of beamed eighth notes
static void music_icon(const app_t *self, int cx, int cy)
{
    epd_fill_circle(cx - 9, cy + 9, 5, EPD_BLACK);
    epd_fill_circle(cx + 9, cy + 6, 5, EPD_BLACK);
    for (int t = 0; t < 2; t++) {
        epd_draw_line(cx - 5 + t, cy + 9, cx - 5 + t, cy - 12, EPD_BLACK);   // stems
        epd_draw_line(cx + 13 + t, cy + 6, cx + 13 + t, cy - 15, EPD_BLACK);
    }
    for (int t = 0; t < 4; t++) {
        epd_draw_line(cx - 5, cy - 12 + t, cx + 14, cy - 15 + t, EPD_BLACK); // beam
    }
}

const app_t music_app = {
    .id = "music",
    .name = "Music",
    .draw_icon = music_icon,
    .init = music_init,
    .open = music_open,
    .button = music_button,
    .next_wakeup = music_next_wakeup,
    .tick = music_tick,
    .background = music_background,
    .close = music_close,
    .select_hold = true,
};
