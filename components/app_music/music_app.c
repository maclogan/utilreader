#include "music_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "buttons.h"                    // buttons_post(BUTTON_WAKE)
#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "font16.h"
#include "musicdb.h"
#include "player.h"
#include "playlists.h"
#include "power.h"                      // power_add_off_hook()
#include "sdcard.h"
#include "settings.h"
#include "ui_sidebar.h"

static const char *TAG = "music";

#define KEY_VOLUME      "music_vol"
#define KEY_QPOS        "music_qpos"    // position in the saved queue
#define KEY_POS         "music_pos"     // ms into that track
#define QUEUE_FILE      SDCARD_ROOT "/.musicq"   // the queue, as card paths, for resuming

#define MARGIN          4
#define LIST_X          (UI_CONTENT_X + MARGIN)
#define ROW_H           21
#define ROWS            6               // 6 x 21 = 126 of 128 px
#define SCROLL_W        4               // scroll bar at the right edge
#define SEEK_STEP_MS    10000
#define TICK_MS         10000           // Now playing: time updated this often
#define SAVE_EVERY_MS   (2 * 60 * 1000) // position saved this often while playing
#define FULL_EVERY      30              // partial refreshes between full ones (ghosting)
#define ELLIPSIS        0x2026

// ---------- the menu tree ----------
//   Music: Playlists > playlist > songs | Artists > artist > All Songs / Shuffle All Songs /
//          album > songs | Albums > album > songs | Songs | Shuffle Songs | Now Playing |
//          Rescan Library
// Every song list starts with a "Shuffle Songs" row (TRACK_ROW0 = the first song's row).

typedef enum {
    V_ROOT, V_PLAYLISTS, V_PLAYLIST, V_ARTISTS, V_ARTIST, V_ARTIST_SONGS,
    V_ALBUMS, V_ALBUM, V_SONGS, V_PLAYING, V_MESSAGE,
} view_t;

typedef struct {
    view_t view;
    int key;                            // playlist / artist / album the view shows
    int sel;
} level_t;

typedef enum { R_PLAYLISTS, R_ARTISTS, R_ALBUMS, R_SONGS, R_SHUFFLE, R_PLAYING, R_RESCAN } root_item_t;

#define MAX_DEPTH 6
static level_t stack[MAX_DEPTH];
static int depth;                       // stack[depth] is on screen
static int *rows;                       // the current list's tracks / ids (PSRAM)
static int *shuffled;                   // a shuffled copy for playing (PSRAM)
static int row_count;
static int root_items[8], root_count;
static playlist_info_t *pls;            // playlists (PSRAM)
static int pl_count;

static bool seek_mode;                  // Now playing: false = VOL mode
static int partials;
static TickType_t next_tick, last_save, last_progress;
static bool resume_available;
static char message_title[48], message_detail[128];

#define CUR (stack[depth])

// ---------- drawing helpers ----------

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

// A ">" for rows that open another list
static void chevron(int x, int y, uint8_t color)
{
    for (int i = 0; i < 4; i++) {
        epd_draw_line(x + i, y + i, x + i, y + 8 - i, color);
    }
}

// ---------- lists ----------

static bool is_track_list(view_t v)
{
    return v == V_PLAYLIST || v == V_ARTIST_SONGS || v == V_ALBUM || v == V_SONGS;
}

#define TRACK_ROW0  1                   // song lists: row 0 is "Shuffle Songs"
#define ARTIST_ROW0 2                   // an artist: All Songs, Shuffle All Songs, then albums

static const char *root_label(int item)
{
    static const char *labels[] = {"Playlists", "Artists", "Albums", "Songs", "Shuffle Songs", "Now Playing", "Rescan Library"};
    return labels[item];
}

static const char *row_label(int i, bool *opens)
{
    *opens = true;
    switch (CUR.view) {
    case V_ROOT:
        *opens = root_items[i] <= R_SONGS || root_items[i] == R_PLAYING;
        return root_label(root_items[i]);
    case V_PLAYLISTS:
        return pl_count ? pls[i].name : "No playlists yet";
    case V_ARTISTS:
        return musicdb_artist_name(i);
    case V_ARTIST:
        if (i == 1) {
            *opens = false;
            return "Shuffle All Songs";
        }
        return i == 0 ? "All Songs" : musicdb_album_name(rows[i - ARTIST_ROW0]);
    case V_ALBUMS:
        return musicdb_album_name(i);
    default:                                                // track lists
        *opens = false;
        return i == 0 ? "Shuffle Songs" : musicdb_track(rows[i - TRACK_ROW0])->title;
    }
}

static int list_count(void)
{
    switch (CUR.view) {
    case V_ROOT: return root_count;
    case V_PLAYLISTS: return pl_count ? pl_count : 1;
    case V_ARTISTS: return musicdb_artist_count();
    case V_ARTIST: return row_count + ARTIST_ROW0;
    case V_ALBUMS: return musicdb_album_count();
    default: return row_count ? row_count + TRACK_ROW0 : 0;
    }
}

static const char *view_title(void)
{
    switch (CUR.view) {
    case V_PLAYLISTS: return "Playlists";
    case V_PLAYLIST: return pls[CUR.key].name;
    case V_ARTISTS: return "Artists";
    case V_ARTIST: return musicdb_artist_name(CUR.key);
    case V_ARTIST_SONGS: return musicdb_artist_name(CUR.key);
    case V_ALBUMS: return "Albums";
    case V_ALBUM: return musicdb_album_name(CUR.key);
    case V_SONGS: return "Songs";
    default: return "Music";
    }
}

static void draw_list(bool full)
{
    full = begin(full);
    int n = list_count();
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", CUR.sel + 1, n);
    ui_sidebar(view_title(), n > 1 ? counter : NULL);

    player_status_t ps;
    player_status(&ps);
    bool scroll = n > ROWS;
    int right = EPD_WIDTH - MARGIN - (scroll ? SCROLL_W + 3 : 0);
    int first = CUR.sel / ROWS * ROWS;
    for (int i = first; i < n && i < first + ROWS; i++) {
        int y = 1 + (i - first) * ROW_H;
        bool sel = i == CUR.sel;
        uint8_t color = sel ? EPD_WHITE : EPD_BLACK;
        if (sel) {
            epd_draw_rect(UI_CONTENT_X, y, right - UI_CONTENT_X + 2, ROW_H - 1, EPD_BLACK, true);
        }
        bool opens;
        const char *label = row_label(i, &opens);
        int text_right = right - (opens ? 10 : 0);
        // The playing track gets a small speaker mark
        if (is_track_list(CUR.view) && i >= TRACK_ROW0 && rows[i - TRACK_ROW0] == ps.index) {
            epd_draw_text(text_right - 10, y + 7, ps.paused ? "||" : ">", 1, color);
            text_right -= 14;
        }
        text_cut(LIST_X, y + 2, label, text_right, color);
        if (opens) {
            chevron(right - 6, y + 6, color);
        }
    }
    if (scroll) {                                           // scroll bar: track and thumb
        int x = EPD_WIDTH - MARGIN - SCROLL_W, h = EPD_HEIGHT - 4;
        epd_draw_rect(x, 2, SCROLL_W, h, EPD_BLACK, false);
        int th = h * ROWS / n;
        th = th < 6 ? 6 : th;
        int ty = 2 + (h - th) * first / (n - ROWS > 0 ? (n - 1) / ROWS * ROWS : 1);
        epd_draw_rect(x, ty > 2 + h - th ? 2 + h - th : ty, SCROLL_W, th, EPD_BLACK, true);
    }
    if (CUR.view == V_PLAYLISTS && pl_count == 0) {
        small_centered(60, "Make playlists on the web page:");
        small_centered(72, "Transfer app > Music");
    }
    finish(full);
}

static void draw_message(void)
{
    begin(true);
    ui_sidebar("Music", NULL);
    font16_draw_text(UI_CONTENT_CX - font16_text_width(message_title) / 2, 34, message_title, EPD_BLACK);
    small_centered(62, message_detail);
    small_centered(EPD_HEIGHT - 9, "Press: try again   BACK: leave");
    finish(true);
}

// Album art isn't decoded: a framed note stands in for it
static void draw_art(int x, int y, int s)
{
    epd_draw_rect(x, y, s, s, EPD_BLACK, false);
    epd_draw_rect(x + 1, y + 1, s - 2, s - 2, EPD_BLACK, false);
    int cx = x + s / 2, cy = y + s / 2;
    epd_fill_circle(cx - 6, cy + 9, 5, EPD_BLACK);
    for (int t = 0; t < 2; t++) {
        epd_draw_line(cx - 2 + t, cy + 9, cx - 2 + t, cy - 13, EPD_BLACK);
    }
    for (int t = 0; t < 3; t++) {
        epd_draw_line(cx - 1, cy - 13 + t, cx + 8, cy - 7 + t, EPD_BLACK);
    }
}

static void draw_playing(bool full)
{
    player_status_t ps;
    player_status(&ps);
    full = begin(full);
    char counter[16], a[16], b[24], line[64];
    snprintf(counter, sizeof(counter), "%d/%d", ps.queue_pos + 1, ps.queue_len);
    ui_sidebar("Now Playing", ps.index >= 0 ? counter : NULL);

    const int art = 52, ax = UI_CONTENT_X + 5, tx = ax + art + 8, right = EPD_WIDTH - MARGIN;
    draw_art(ax, 4, art);
    if (ps.index < 0) {
        font16_draw_text(tx, 4, "Not playing", EPD_BLACK);
        epd_draw_text(tx, 26, "Choose something in the library", 1, EPD_BLACK);
    } else {
        text_cut(tx, 3, ps.title, right, EPD_BLACK);
        text_cut(tx, 21, ps.artist, right, EPD_BLACK);
        text_cut(tx, 39, ps.album[0] ? ps.album : "", right, EPD_BLACK);
    }
    if (ps.message[0]) {
        snprintf(line, sizeof(line), "%s", ps.message);
    } else if (ps.index >= 0) {
        snprintf(line, sizeof(line), "%d of %d   %s%s", ps.queue_pos + 1, ps.queue_len,
                 ps.paused ? "Paused   " : "", ps.rate ? media_format_name(ps.format) : "");
        if (ps.rate) {
            size_t n = strlen(line);
            snprintf(line + n, sizeof(line) - n, " %lu.%lu kHz", (unsigned long)(ps.rate / 1000),
                     (unsigned long)(ps.rate % 1000 / 100));
        }
    } else {
        line[0] = '\0';
    }
    epd_draw_text(tx, 61, line, 1, EPD_BLACK);

    // Progress: elapsed on the left, time left on the right
    const int bar_y = 76, bar_w = right - ax;
    epd_draw_rect(ax, bar_y, bar_w, 7, EPD_BLACK, false);
    if (ps.duration_ms) {
        epd_draw_rect(ax, bar_y, (int)((uint64_t)bar_w * ps.elapsed_ms / ps.duration_ms), 7, EPD_BLACK, true);
    }
    format_time(a, sizeof(a), ps.elapsed_ms);
    epd_draw_text(ax, bar_y + 11, a, 1, EPD_BLACK);
    if (ps.duration_ms) {
        b[0] = '-';
        format_time(b + 1, sizeof(b) - 1, ps.duration_ms - ps.elapsed_ms);
        epd_draw_text(right - epd_text_width(b, 1), bar_y + 11, b, 1, EPD_BLACK);
    }

    // Mode: VOL with the volume bar, or SEEK
    const int mode_y = 106;
    const char *badge = seek_mode ? "SEEK" : "VOL";
    int bw = epd_text_width(badge, 1) + 8;
    epd_draw_rect(ax, mode_y, bw, 13, EPD_BLACK, true);
    epd_draw_text(ax + 4, mode_y + 3, badge, 1, EPD_WHITE);
    int vx = ax + bw + 8;
    if (seek_mode) {
        epd_draw_text(vx, mode_y + 3, "< >  back / ahead 10 s", 1, EPD_BLACK);
    } else {
        int vol = audio_volume();
        int seg = (right - vx - 20) / AUDIO_VOLUME_MAX;
        for (int i = 0; i < AUDIO_VOLUME_MAX; i++) {
            int x = vx + i * seg;
            if (i < vol) {
                epd_draw_rect(x, mode_y + 2, seg - 2, 9, EPD_BLACK, true);
            } else {
                epd_draw_hline(x, mode_y + 10, seg - 2, EPD_BLACK);
            }
        }
        snprintf(line, sizeof(line), "%d", vol);
        epd_draw_text(right - epd_text_width(line, 1), mode_y + 3, line, 1, EPD_BLACK);
    }
    finish(full);
    next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_MS);
}

static void redraw(bool full)
{
    if (CUR.view == V_PLAYING) {
        draw_playing(full);
    } else if (CUR.view == V_MESSAGE) {
        draw_message();
    } else {
        draw_list(full);
    }
}

// ---------- saving and resuming ----------

static void save_queue(const int *tracks, int n)
{
    FILE *f = fopen(QUEUE_FILE, "w");
    if (f == NULL) {
        return;
    }
    char path[400];
    for (int i = 0; i < n; i++) {
        musicdb_card_path(tracks[i], path, sizeof(path));
        fprintf(f, "%s\n", path);
    }
    fclose(f);
    resume_available = true;
}

static void save_place(void)
{
    player_status_t ps;
    player_status(&ps);
    if (ps.index < 0) {
        return;
    }
    settings_set_i32(KEY_QPOS, ps.queue_pos);
    settings_set_i32(KEY_POS, (int32_t)ps.elapsed_ms);
    last_save = xTaskGetTickCount();
}

// Reads the saved queue back into rows (paths no longer in the library are dropped)
static int load_queue(void)
{
    FILE *f = fopen(QUEUE_FILE, "r");
    if (f == NULL) {
        return 0;
    }
    char line[400];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < MUSICDB_MAX_TRACKS) {
        line[strcspn(line, "\r\n")] = '\0';
        int i = musicdb_find_path(line);
        if (i >= 0) {
            rows[n++] = i;
        }
    }
    fclose(f);
    return n;
}

static void play(const int *tracks, int n, int pos, uint32_t start_ms);

// Plays a shuffled copy of tracks[n]
static void play_shuffled(const int *tracks, int n)
{
    if (n <= 0) {
        return;
    }
    memmove(shuffled, tracks, n * sizeof(int));
    for (int i = n - 1; i > 0; i--) {                       // Fisher-Yates
        int j = esp_random() % (i + 1), t = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = t;
    }
    play(shuffled, n, 0, 0);
}

static void play(const int *tracks, int n, int pos, uint32_t start_ms)
{
    save_queue(tracks, n);
    player_play_queue(tracks, n, pos, start_ms);
    settings_set_i32(KEY_QPOS, pos);
    settings_set_i32(KEY_POS, (int32_t)start_ms);
}

// ---------- building the views ----------

static void build_root(void)
{
    root_count = 0;
    for (int i = R_PLAYLISTS; i <= R_SHUFFLE; i++) {
        root_items[root_count++] = i;
    }
    if (player_active() || resume_available) {
        root_items[root_count++] = R_PLAYING;
    }
    root_items[root_count++] = R_RESCAN;
}

// Fills rows for the view on screen
static void load_view(void)
{
    row_count = 0;
    switch (CUR.view) {
    case V_ROOT:
        build_root();
        break;
    case V_PLAYLISTS:
        pl_count = playlists_list(pls, PLAYLISTS_MAX);
        break;
    case V_PLAYLIST:
        row_count = playlist_load(pls[CUR.key].file, rows, PLAYLIST_MAX_TRACKS, NULL);
        row_count = row_count < 0 ? 0 : row_count;
        break;
    case V_ARTIST:
        row_count = musicdb_artist_albums(CUR.key, rows, MUSICDB_MAX_TRACKS);
        break;
    case V_ARTIST_SONGS:
        row_count = musicdb_artist_songs(CUR.key, rows, MUSICDB_MAX_TRACKS);
        break;
    case V_ALBUM:
        row_count = musicdb_album_songs(CUR.key, rows, MUSICDB_MAX_TRACKS);
        break;
    case V_SONGS: {
        int n;
        const int *s = musicdb_songs(&n);
        memcpy(rows, s, n * sizeof(int));
        row_count = n;
        break;
    }
    default:
        break;
    }
    int n = list_count();
    if (CUR.sel >= n) {
        CUR.sel = n > 0 ? n - 1 : 0;
    }
}

static void push(view_t v, int key, int sel)
{
    if (depth < MAX_DEPTH - 1) {
        depth++;
    }
    CUR = (level_t){.view = v, .key = key, .sel = sel};
    load_view();
}

static void pop(void)
{
    if (depth > 0) {
        depth--;
    }
    load_view();
}

// ---------- the library ----------

static void scan_progress(int done, int total)
{
    if (pdTICKS_TO_MS(xTaskGetTickCount() - last_progress) < 1500 && done < total) {
        return;
    }
    last_progress = xTaskGetTickCount();
    bool full = begin(done <= 10);
    ui_sidebar("Music", NULL);
    font16_draw_text(UI_CONTENT_CX - font16_text_width("Reading the library") / 2, 36, "Reading the library", EPD_BLACK);
    char line[48];
    snprintf(line, sizeof(line), "%d of %d new files", done, total);
    small_centered(62, line);
    small_centered(76, "Only new files are read next time");
    finish(full);
}

static void show_status_message(musicdb_status_t s)
{
    char dir[64];
    settings_get_str(MUSIC_DIR_KEY, dir, sizeof(dir), MUSIC_DIR_DEFAULT);
    depth = 0;
    CUR = (level_t){.view = V_MESSAGE};
    switch (s) {
    case MUSICDB_NO_CARD:
        snprintf(message_title, sizeof(message_title), "No microSD card");
        snprintf(message_detail, sizeof(message_detail), "Insert a FAT32 card with music");
        break;
    case MUSICDB_NO_FOLDER:
        snprintf(message_title, sizeof(message_title), "Music folder missing");
        snprintf(message_detail, sizeof(message_detail), "No %s on the card (Settings > Music)", dir);
        break;
    default:
        snprintf(message_title, sizeof(message_title), "No music yet");
        snprintf(message_detail, sizeof(message_detail), "Copy MP3/FLAC/M4A/OGG/WAV to %s", dir);
        break;
    }
    draw_message();
}

// Scans if the folder is new or `force`. Returns true when there's music.
static bool ensure_library(bool force)
{
    char dir[64];
    settings_get_str(MUSIC_DIR_KEY, dir, sizeof(dir), MUSIC_DIR_DEFAULT);
    if (!force && musicdb_status() == MUSICDB_OK && !strcmp(dir, musicdb_dir())) {
        return true;
    }
    if (player_active()) {
        save_place();
        player_stop();                                      // track indices are about to change
    }
    last_progress = 0;
    musicdb_status_t s = musicdb_scan(dir, scan_progress);
    FILE *f = fopen(QUEUE_FILE, "r");
    resume_available = s == MUSICDB_OK && f != NULL;
    if (f) {
        fclose(f);
    }
    return s == MUSICDB_OK;
}

static void open_root(bool full)
{
    depth = 0;
    CUR = (level_t){.view = V_ROOT};
    load_view();
    draw_list(full);
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
    rows = heap_caps_malloc(MUSICDB_MAX_TRACKS * sizeof(int), MALLOC_CAP_SPIRAM);
    shuffled = heap_caps_malloc(MUSICDB_MAX_TRACKS * sizeof(int), MALLOC_CAP_SPIRAM);
    pls = heap_caps_malloc(PLAYLISTS_MAX * sizeof(playlist_info_t), MALLOC_CAP_SPIRAM);
    audio_set_volume(settings_get_i32(KEY_VOLUME, 20));
    player_init(notify_from_player);
    power_add_off_hook(power_off_hook);
}

static void music_open(const app_t *self)
{
    if (rows == NULL || pls == NULL || shuffled == NULL) {
        return;
    }
    if (player_active()) {                                  // back to what's playing
        open_root(true);
        push(V_PLAYING, 0, 0);
        draw_playing(true);
        return;
    }
    if (!ensure_library(false)) {
        show_status_message(musicdb_status());
        return;
    }
    open_root(true);
}

static void choose(void)
{
    int sel = CUR.sel;
    switch (CUR.view) {
    case V_ROOT:
        switch (root_items[sel]) {
        case R_PLAYLISTS: push(V_PLAYLISTS, 0, 0); break;
        case R_ARTISTS: push(V_ARTISTS, 0, 0); break;
        case R_ALBUMS: push(V_ALBUMS, 0, 0); break;
        case R_SONGS: push(V_SONGS, 0, 0); break;
        case R_SHUFFLE: {
            int n;
            const int *all = musicdb_songs(&n);
            play_shuffled(all, n);
            push(V_PLAYING, 0, 0);
            break;
        }
        case R_PLAYING:
            if (!player_active()) {                         // resume the saved queue
                int n = load_queue();
                int pos = settings_get_i32(KEY_QPOS, 0);
                if (n > 0) {
                    player_play_queue(rows, n, pos < n ? pos : 0, pos < n ? settings_get_i32(KEY_POS, 0) : 0);
                }
            }
            push(V_PLAYING, 0, 0);
            break;
        case R_RESCAN:
            if (ensure_library(true)) {
                open_root(true);
            } else {
                show_status_message(musicdb_status());
            }
            return;
        }
        break;
    case V_PLAYLISTS:
        if (pl_count) {
            push(V_PLAYLIST, sel, 0);
        } else {
            return;
        }
        break;
    case V_ARTISTS:
        push(V_ARTIST, sel, 0);
        break;
    case V_ARTIST:
        if (sel == 0) {
            push(V_ARTIST_SONGS, CUR.key, 0);
        } else if (sel == 1) {
            int n = musicdb_artist_songs(CUR.key, shuffled, MUSICDB_MAX_TRACKS);
            play_shuffled(shuffled, n);
            push(V_PLAYING, 0, 0);
        } else {
            push(V_ALBUM, rows[sel - ARTIST_ROW0], 0);
        }
        break;
    case V_ALBUMS:
        push(V_ALBUM, sel, 0);
        break;
    default:                                                // a song list
        if (row_count == 0) {
            return;
        }
        if (sel == 0) {
            play_shuffled(rows, row_count);                 // "Shuffle Songs"
        } else {
            play(rows, row_count, sel - TRACK_ROW0, 0);     // the list from this song
        }
        push(V_PLAYING, 0, 0);
        break;
    }
    redraw(false);
}

static bool list_button(button_event_t event)
{
    int n = list_count();
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_RIGHT:
        if (n > 0) {
            CUR.sel = (CUR.sel + (event == BUTTON_LEFT ? n - 1 : 1)) % n;
            draw_list(false);
        }
        return true;
    case BUTTON_LEFT_HOLD:                                  // a page at a time
        CUR.sel = CUR.sel >= ROWS ? (CUR.sel / ROWS - 1) * ROWS : 0;
        draw_list(false);
        return true;
    case BUTTON_RIGHT_HOLD:
        CUR.sel = (CUR.sel / ROWS + 1) * ROWS < n ? (CUR.sel / ROWS + 1) * ROWS : n - 1;
        draw_list(false);
        return true;
    case BUTTON_SELECT:
        choose();
        return true;
    case BUTTON_SELECT_HOLD:                                // straight to Now Playing
        if (player_active()) {
            push(V_PLAYING, 0, 0);
            draw_playing(false);
        }
        return true;
    case BUTTON_WAKE:
        if (CUR.view == V_ROOT && (player_active() || resume_available) != (root_count == 7)) {
            load_view();                                    // Now Playing appeared or went
        }
        draw_list(false);                                   // the playing mark moved
        return true;
    case BUTTON_BACK:
        if (depth == 0) {
            return false;                                   // leave; playback carries on
        }
        pop();
        draw_list(false);
        return true;
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
        }
        return true;
    case BUTTON_SELECT_HOLD:
        seek_mode = !seek_mode;
        draw_playing(false);
        return true;
    case BUTTON_WAKE:
        if (player_active() && pdTICKS_TO_MS(xTaskGetTickCount() - last_save) > 5000) {
            save_place();                                   // new track or pause: remember it
        }
        draw_playing(false);
        return true;
    case BUTTON_BACK:
        pop();
        draw_list(false);
        return true;
    default:
        return true;
    }
}

static bool music_button(const app_t *self, button_event_t event)
{
    switch (CUR.view) {
    case V_PLAYING:
        return playing_button(event);
    case V_MESSAGE:
        if (event == BUTTON_BACK) {
            return false;
        }
        if (event == BUTTON_SELECT) {
            if (ensure_library(true)) {
                open_root(true);
            } else {
                show_status_message(musicdb_status());
            }
        }
        return true;
    default:
        return list_button(event);
    }
}

static TickType_t music_next_wakeup(const app_t *self)
{
    if (CUR.view != V_PLAYING || !player_active()) {
        return portMAX_DELAY;
    }
    player_status_t ps;
    player_status(&ps);
    return ps.paused ? portMAX_DELAY : app_ticks_until(next_tick);
}

static void music_tick(const app_t *self)
{
    if (CUR.view == V_PLAYING && app_due(next_tick)) {
        draw_playing(false);
    }
}

// Runs every 30 s whether or not the app is on screen (music plays on in other apps)
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
