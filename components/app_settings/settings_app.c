#include "settings_app.h"

#include <dirent.h>                     // the folder picker
#include <unistd.h>                     // rmdir()
#include <sys/stat.h>                   // mkdir()
#include <stdio.h>                      // snprintf()
#include <stdlib.h>                     // qsort()
#include <string.h>                     // strncmp()
#include <strings.h>                    // strcasecmp()
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"            // the format task
#include "books.h"                      // where the e-books live
#include "font16.h"                     // folder names
#include "musicdb.h"                    // rescanned after formatting the card
#include "player.h"                     // stopped before moving / erasing files it may have open
#include "podcasts.h"                   // moving the podcasts folder
#include "sdcard.h"                     // the microSD card
#include "epd.h"                        // drawing, epd_set_rotated()
#include "esp_log.h"
#include "local_time.h"                 // reading and setting the clock
#include "settings.h"                   // SETTING_ROTATE_180
#include "ui_sidebar.h"                 // title bar on the left

static const char *TAG = "settings_app";

// Layout (pixels). The title bar takes the left UI_SIDEBAR_W columns.
#define LEFT        (UI_CONTENT_X + 4)
#define ROW_Y       1                   // top of the first row
#define ROW_H       18                  // row spacing
#define BOX_H       17                  // highlight bar height
#define VALUE_X     (UI_CONTENT_X + 84)     // left edge of the values
#define CHAR_W      8                   // 16 px font: Latin characters are 8 px wide
#define INFO_RULE_Y 111                 // a rule, then one line of small text at the bottom
#define INFO_Y      116

typedef enum { ROW_DATE, ROW_TIME, ROW_ROTATE, ROW_REFRESH, ROW_CLEANUP, ROW_PICTURES, ROW_DARKNESS, ROW_MUSIC, ROW_PODCASTS, ROW_BOOKS, ROW_VOICE, ROW_CARD, ROW_FORMAT, ROW_COUNT } row_t;
static const char *const labels[ROW_COUNT] = {"Date", "Time", "Rotate", "Refresh", "Cleanup", "Pictures", "Darkness", "Music", "Podcasts", "Books", "Voice", "Card", "Format"};
// Settings > Darkness: how much pictures are lightened before dithering (epd_set_picture_gamma)
#define DARKNESS_LEVELS 5
static const int darkness_gamma[DARKNESS_LEVELS] = {50, 62, 75, 87, 100};   // 1 lightest .. 5 untouched
static bool dark_editing;               // the Darkness slider is being moved
static bool cleanup_editing;            // Cleanup (full refresh after N quick ones) is being changed
#define CLEANUP_STEP 5
#define CLEANUP_MIN  5
#define CLEANUP_MAX  60

// The level for the current gamma: the nearest one
static int darkness_level(void)
{
    int g = epd_picture_gamma(), best = 0;
    for (int i = 1; i < DARKNESS_LEVELS; i++) {
        if (abs(darkness_gamma[i] - g) < abs(darkness_gamma[best] - g)) {
            best = i;
        }
    }
    return best + 1;
}

#define IS_FOLDER_ROW(r) ((r) >= ROW_MUSIC && (r) <= ROW_VOICE)
#define CONFIRM_GUARD_MS 1000           // a hold this soon after the format screen opened is ignored
#define ROWS_SHOWN  6                   // rows on screen; the list scrolls

static int row = ROW_DATE;              // highlighted row
static int top = 0;                     // first row on screen
static char notice[64];                 // result of the last action ("Card formatted"), shown once
static bool confirm_format;             // the "erase the card?" screen is showing
static TickType_t confirm_since;        // when it opened (a key held from before doesn't count)
static int editing = -1;                // field being edited (-1 = choosing a row)
static struct tm edit;                  // the date/time being edited (local)
static long drawn_minute = -1;          // minute (since 1970) the screen shows

// ---------- folder picker (music, podcasts, e-books) ----------
// Lists the folders inside `pick_dir` on the card. Entry 0 is "Use this folder" (for
// e-books, entry 1 is "Internal memory"); the rest open a folder. BACK goes up a level, and
// out of the picker from the top.

#define PICK_MAX    64
#define PICK_ROWS   5
#define PICK_ROW_H  18

static void info_line(int line, const char *text);
static void draw(bool full);

static bool picking;
static row_t pick_row;                  // ROW_MUSIC, ROW_PODCASTS, ROW_BOOKS or ROW_VOICE
static int pick_extra;                  // entries before the folders (1, or 2 for e-books)
static char pick_dir[128];              // "" = the card's top level, else "/Music/Rock"
static char *pick_names[PICK_MAX];
static int pick_count, pick_sel;
static bool pick_no_card;

static int compare_names(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void pick_load(void)
{
    for (int i = 0; i < pick_count; i++) {
        free(pick_names[i]);
    }
    pick_count = 0;
    pick_sel = 0;
    pick_no_card = !sdcard_mount();
    if (pick_no_card) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s%s", SDCARD_ROOT, pick_dir);
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && pick_count < PICK_MAX) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.' && strcasecmp(e->d_name, "System Volume Information")) {
            pick_names[pick_count] = strdup(e->d_name);
            if (pick_names[pick_count]) {
                pick_count++;
            }
        }
    }
    if (d) {
        closedir(d);
    }
    qsort(pick_names, pick_count, sizeof(char *), compare_names);
}

static void pick_draw(bool full)
{
    if (!full && !epd_is_awake()) {
        full = true;
    }
    if (full) {
        epd_init();
    }
    epd_clear(EPD_WHITE);
    ui_sidebar(pick_row == ROW_MUSIC ? "Music folder" : pick_row == ROW_PODCASTS ? "Podcasts folder" :
               pick_row == ROW_VOICE ? "Voice notes folder" : "E-books", NULL);
    char line[160];
    if (pick_no_card) {
        font16_draw_text(LEFT + 4, 30, "No microSD card", EPD_BLACK);
        info_line(0, "BACK: cancel");
    } else {
        snprintf(line, sizeof(line), "In: %s", pick_dir[0] ? pick_dir : "/ (top of the card)");
        epd_draw_text(LEFT, 3, line, 1, EPD_BLACK);
        int n = pick_count + pick_extra;
        int first = pick_sel / PICK_ROWS * PICK_ROWS;
        for (int i = first; i < n && i < first + PICK_ROWS; i++) {
            int y = 14 + (i - first) * PICK_ROW_H;
            uint8_t color = EPD_BLACK;
            if (i == pick_sel) {
                epd_draw_rect(UI_CONTENT_X, y, UI_CONTENT_W, PICK_ROW_H - 1, EPD_BLACK, true);
                color = EPD_WHITE;
            }
            if (i == 0) {
                font16_draw_text(LEFT + 4, y + 1, "[ Use this folder ]", color);
            } else if (i < pick_extra) {
                font16_draw_text(LEFT + 4, y + 1, "[ Internal memory ]", color);
            } else {
                snprintf(line, sizeof(line), "%s/", pick_names[i - pick_extra]);
                font16_draw_text(LEFT + 4, y + 1, line, color);
            }
        }
        epd_draw_text(LEFT, EPD_HEIGHT - 9, "press: open / choose   BACK: up", 1, EPD_BLACK);
    }
    if (full) {
        epd_display();
    } else {
        epd_display_partial();
    }
}

// The folder a row currently uses, as a card path ("/Music"); "" for internal e-books
static void row_dir(row_t r, char *out, size_t size)
{
    if (r == ROW_MUSIC) {
        settings_get_str(SETTING_MUSIC_DIR, out, size, SETTING_MUSIC_DIR_DEFAULT);
    } else if (r == ROW_PODCASTS) {
        settings_get_str(SETTING_PODCASTS_DIR, out, size, SETTING_PODCASTS_DIR_DEFAULT);
    } else if (r == ROW_VOICE) {
        settings_get_str(SETTING_VOICE_DIR, out, size, SETTING_VOICE_DIR_DEFAULT);
    } else {
        books_location(out, size);
    }
}

// "/Music/Rock" -> /sdcard/Music and /sdcard/Music/Rock (mkdir makes one level at a time)
static void make_card_dirs(const char *card_dir)
{
    char path[192];
    int n = snprintf(path, sizeof(path), "%s%s", SDCARD_ROOT, card_dir);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return;
    }
    for (char *p = path + strlen(SDCARD_ROOT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0777);
            *p = '/';
        }
    }
    mkdir(path, 0777);
}

// Moves the voice notes (*.wav) from one card folder to another (same card: just renames).
// Names are gathered a batch at a time, so the folder isn't changed while it's being read.
static bool move_voice_notes(const char *from_dir, const char *to_dir)
{
    char from[192], to[192];
    snprintf(from, sizeof(from), "%s%s", SDCARD_ROOT, strcmp(from_dir, "/") ? from_dir : "");
    snprintf(to, sizeof(to), "%s%s", SDCARD_ROOT, strcmp(to_dir, "/") ? to_dir : "");
    if (!strcasecmp(from, to)) {
        return true;
    }
    make_card_dirs(to_dir);
    enum { BATCH = 32 };
    static char names[BATCH][64];
    bool ok = true;
    while (1) {
        int n = 0;
        DIR *d = opendir(from);
        struct dirent *e;
        while (d && n < BATCH && (e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len > 4 && len < sizeof(names[0]) && !strcasecmp(e->d_name + len - 4, ".wav")) {
                memcpy(names[n++], e->d_name, len + 1);
            }
        }
        if (d) {
            closedir(d);
        }
        int moved = 0;
        for (int i = 0; i < n; i++) {
            char a[260], b[260];
            snprintf(a, sizeof(a), "%s/%s", from, names[i]);
            snprintf(b, sizeof(b), "%s/%s", to, names[i]);
            if (rename(a, b) == 0) {
                moved++;
            } else {
                ok = false;
            }
        }
        if (n == 0 || moved == 0) {
            break;
        }
    }
    if (ok && strcmp(from, SDCARD_ROOT)) {
        rmdir(from);                                        // only goes if nothing else is in it
    }
    return ok;
}

static void pick_start(row_t r)
{
    pick_row = r;
    pick_extra = r == ROW_BOOKS ? 2 : 1;
    // Start in the folder above the current choice, with it highlighted
    char cur[128];
    row_dir(r, cur, sizeof(cur));
    char *slash = strrchr(cur, '/');
    const char *leaf = slash ? slash + 1 : cur;
    snprintf(pick_dir, sizeof(pick_dir), "%.*s", slash ? (int)(slash - cur) : 0, cur);
    picking = true;
    pick_load();
    if (r == ROW_BOOKS && cur[0] == '\0') {
        pick_sel = 1;                                       // on "Internal memory"
    }
    for (int i = 0; i < pick_count; i++) {
        if (leaf[0] && !strcasecmp(pick_names[i], leaf)) {
            pick_sel = i + pick_extra;
        }
    }
    pick_draw(false);
}

// Stops the player if it has a file open under `dir` (a full path), before files move
static void stop_player_in(const char *dir)
{
    player_status_t ps;
    player_status(&ps);
    size_t n = strlen(dir);
    if (ps.index >= 0 && !strncasecmp(ps.path, dir, n) && ps.path[n] == '/') {
        player_stop();
    }
}

// Applies the picker's choice. `dir`: card folder, or "" for internal e-books.
static void pick_apply(const char *dir)
{
    notice[0] = '\0';
    if (pick_row == ROW_MUSIC) {
        settings_set_str(SETTING_MUSIC_DIR, dir);
        ESP_LOGI(TAG, "music folder: %s", dir);
    } else if (pick_row == ROW_PODCASTS) {
        stop_player_in(podcasts_root());
        if (!podcasts_set_root(dir)) {
            snprintf(notice, sizeof(notice), "Some podcast files didn't move");
        } else {
            snprintf(notice, sizeof(notice), "Podcasts moved to %.24s", dir);
        }
    } else if (pick_row == ROW_VOICE) {
        char old[128];
        row_dir(ROW_VOICE, old, sizeof(old));
        char full[192];
        snprintf(full, sizeof(full), "%s%s", SDCARD_ROOT, strcmp(old, "/") ? old : "");
        stop_player_in(full);
        bool ok = move_voice_notes(old, dir);
        settings_set_str(SETTING_VOICE_DIR, dir);
        snprintf(notice, sizeof(notice), ok ? "Notes moved to %.27s" : "Some voice notes couldn't be moved", dir);
    } else {
        if (books_set_location(dir)) {
            snprintf(notice, sizeof(notice), "E-books moved to %.24s", dir[0] ? dir : "internal memory");
        } else {
            snprintf(notice, sizeof(notice), "Some e-books couldn't be moved");
        }
    }
}

// Returns false when the picker closes (the settings list is redrawn by the caller)
static bool pick_button(button_event_t event)
{
    int n = pick_count + pick_extra;
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_LEFT_HOLD:
        pick_sel = (pick_sel + n - 1) % n;
        break;
    case BUTTON_RIGHT:
    case BUTTON_RIGHT_HOLD:
        pick_sel = (pick_sel + 1) % n;
        break;
    case BUTTON_SELECT:
        if (pick_no_card) {
            pick_load();                                    // card inserted since? try again
        } else if (pick_sel == 0) {
            picking = false;
            pick_apply(pick_dir[0] ? pick_dir : "/");
            return false;
        } else if (pick_sel < pick_extra) {
            picking = false;
            pick_apply("");                                 // e-books: internal memory
            return false;
        } else {
            size_t len = strlen(pick_dir);
            snprintf(pick_dir + len, sizeof(pick_dir) - len, "/%s", pick_names[pick_sel - pick_extra]);
            pick_load();
        }
        break;
    case BUTTON_BACK: {
        char *slash = strrchr(pick_dir, '/');
        if (pick_no_card || slash == NULL) {
            picking = false;                                // top of the card: cancel
            return false;
        }
        char leaf[64];
        snprintf(leaf, sizeof(leaf), "%s", slash + 1);
        *slash = '\0';
        pick_load();
        for (int i = 0; i < pick_count; i++) {
            if (!strcmp(pick_names[i], leaf)) {
                pick_sel = i + pick_extra;                  // back on the folder we came from
            }
        }
        break;
    }
    default:
        return true;
    }
    pick_draw(false);
    return true;
}

// ---------- format card ----------

// Formatting runs in a task of its own, so the screen can show its progress
static volatile bool formatting;        // the format task is running
static volatile bool format_ok;         // its result, once it's done
static TickType_t format_started;
#define FORMAT_TICK_MS 1000             // progress bar updated this often

// status NULL: the question. Otherwise the status line, with a progress bar when percent >= 0.
static void draw_format_screen(const char *status, int percent)
{
    if (!epd_is_awake()) {
        epd_init();
    }
    epd_clear(EPD_WHITE);
    ui_sidebar("Format card", NULL);
    font16_draw_text(LEFT + 4, 8, "Erase the microSD card?", EPD_BLACK);
    epd_draw_text(LEFT + 4, 34, "Everything on it is deleted: music,", 1, EPD_BLACK);
    char line[64];
    char books[64];
    books_location(books, sizeof(books));
    snprintf(line, sizeof(line), "podcasts,%s voice notes, playlists.", books[0] ? " e-books," : "");
    epd_draw_text(LEFT + 4, 46, line, 1, EPD_BLACK);
    epd_draw_text(LEFT + 4, 58, "The apps' folders are made again, empty.", 1, EPD_BLACK);
    if (status) {
        font16_draw_text(LEFT + 4, 72, status, EPD_BLACK);
        if (percent >= 0) {
            const int bx = LEFT + 4, by = 94, bw = EPD_WIDTH - bx - 10, bh = 12;
            epd_draw_rect(bx, by, bw, bh, EPD_BLACK, false);
            epd_draw_rect(bx + 2, by + 2, (bw - 4) * percent / 100, bh - 4, EPD_BLACK, true);
            char line2[48];
            int secs = (int)(pdTICKS_TO_MS(xTaskGetTickCount() - format_started) / 1000);
            snprintf(line2, sizeof(line2), "%d%%   %d:%02d", percent, secs / 60, secs % 60);
            epd_draw_text(bx, by + bh + 4, line2, 1, EPD_BLACK);
        }
    } else {
        epd_draw_rect(LEFT + 2, 72, 230, 22, EPD_BLACK, true);
        font16_draw_text(LEFT + 8, 75, "Hold center to format", EPD_WHITE);
        epd_draw_text(LEFT + 4, EPD_HEIGHT - 12, "BACK: cancel", 1, EPD_BLACK);
    }
    epd_display_partial();
}

static void format_task(void *arg)
{
    format_ok = sdcard_format();
    formatting = false;
    buttons_post(BUTTON_WAKE);                              // the main loop finishes up (finish_format)
    vTaskDelete(NULL);
}

// Starts formatting in the background; finish_format() completes it in the main task
static void start_format(void)
{
    player_stop();                                          // nothing may have a file open
    podcasts_update_stop();
    format_started = xTaskGetTickCount();
    formatting = true;
    draw_format_screen("Formatting...", 0);
    if (xTaskCreate(format_task, "format", 6144, NULL, 4, NULL) != pdPASS) {
        formatting = false;
        format_ok = false;
        buttons_post(BUTTON_WAKE);
    }
}

static void finish_format(void)
{
    bool ok = format_ok;
    if (ok) {
        // Every app's folder, as Settings has them, empty: music, podcasts, e-books (when
        // they're on the card), voice notes, and the music playlists
        char dir[128];
        for (row_t r = ROW_MUSIC; r <= ROW_VOICE; r++) {
            row_dir(r, dir, sizeof(dir));
            if (dir[0] && strcmp(dir, "/")) {
                make_card_dirs(dir);
                ESP_LOGI(TAG, "made %s", dir);
            }
        }
        make_card_dirs("/Playlists");
        if (musicdb_status() != MUSICDB_NOT_SCANNED) {
            settings_get_str(SETTING_MUSIC_DIR, dir, sizeof(dir), SETTING_MUSIC_DIR_DEFAULT);
            musicdb_scan(dir, NULL);                        // forget the old library
        }
    }
    snprintf(notice, sizeof(notice), ok ? "Card formatted" : "Format failed (the log has details)");
    confirm_format = false;
    draw(false);
}

// Fields of each editable row, as character positions in its value text
typedef struct { int min, max, step_hold, first_char, chars; } field_t;
#define DATE_FIELDS 3                   // "2026-11-19": year, month, day
#define TIME_FIELDS 2                   // " 5:05 PM": hour, minute (AM/PM follows the hour)

static int days_in_month(int year, int month)  // month 1-12
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

// Pointer to the struct tm member a field edits, and its range
static int *field_value(int r, int f, field_t *out)
{
    if (r == ROW_DATE) {
        static const field_t date[DATE_FIELDS] = {{2024, 2099, 10, 0, 4}, {1, 12, 3, 5, 2}, {1, 31, 10, 8, 2}};
        *out = date[f];
        if (f == 2) {
            out->max = days_in_month(edit.tm_year + 1900, edit.tm_mon + 1);
        }
        static int year, month;                             // tm stores these offset
        year = edit.tm_year + 1900;
        month = edit.tm_mon + 1;
        return f == 0 ? &year : f == 1 ? &month : &edit.tm_mday;
    }
    static const field_t time_f[TIME_FIELDS] = {{0, 23, 6, 0, 2}, {0, 59, 10, 3, 2}};
    *out = time_f[f];
    return f == 0 ? &edit.tm_hour : &edit.tm_min;
}

// Adds `delta` to the field being edited, wrapping around its range
static void change_field(int delta)
{
    field_t f;
    int *v = field_value(row, editing, &f);
    int span = f.max - f.min + 1;
    *v = f.min + ((*v - f.min + delta) % span + span) % span;
    if (row == ROW_DATE) {
        if (editing == 0) {
            edit.tm_year = *v - 1900;
        } else if (editing == 1) {
            edit.tm_mon = *v - 1;
        }
        int dim = days_in_month(edit.tm_year + 1900, edit.tm_mon + 1);
        if (edit.tm_mday > dim) {
            edit.tm_mday = dim;                             // e.g. Jan 31 -> Feb 28
        }
    }
}

static void format_date(const struct tm *t, char *out, size_t size)
{
    snprintf(out, size, "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
}

static void format_time(const struct tm *t, char *out, size_t size)
{
#if LOCAL_TIME_24H
    snprintf(out, size, "%02d:%02d", t->tm_hour, t->tm_min);
#else
    int hour12 = t->tm_hour % 12 == 0 ? 12 : t->tm_hour % 12;
    snprintf(out, size, "%2d:%02d %s", hour12, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
#endif
}

// The current local time, or the day this firmware was built (noon) if the clock isn't set
static void current_or_build_time(struct tm *t)
{
    if (local_time_now(t)) {
        return;
    }
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;                               // "Nov 19 2026"
    *t = (struct tm){.tm_hour = 12};
    for (int m = 0; m < 12; m++) {
        if (strncmp(d, months + m * 3, 3) == 0) {
            t->tm_mon = m;
        }
    }
    sscanf(d + 4, "%d %d", &t->tm_mday, &t->tm_year);
    t->tm_year -= 1900;
}

// The hint at the bottom: one line of small text under a rule. (`line` 1 is from the old
// two-line layout and is ignored.)
static void info_line(int line, const char *text)
{
    if (line != 0) {
        return;
    }
    epd_draw_hline(UI_CONTENT_X, INFO_RULE_Y, UI_CONTENT_W, EPD_BLACK);
    epd_draw_text(LEFT, INFO_Y, text, 1, EPD_BLACK);
}

// Settings > Darkness as a slider: a track with a tick for each level and the chosen one in a
// box with its number. While it's being moved, arrows either side say left / right move it.
static void draw_darkness_slider(int y, uint8_t color, bool moving)
{
    const int x0 = VALUE_X + (moving ? 14 : 6), x1 = EPD_WIDTH - (moving ? 22 : 14);
    const int mid = y + BOX_H / 2;
    int level = darkness_level();
    if (moving) {                                           // a box around it while moving
        epd_draw_rect(VALUE_X - 2, y, EPD_WIDTH - VALUE_X - 4, BOX_H, EPD_BLACK, false);
        epd_draw_text(VALUE_X + 3, mid - 3, "<", 1, EPD_BLACK);
        epd_draw_text(EPD_WIDTH - 14, mid - 3, ">", 1, EPD_BLACK);
    }
    epd_draw_hline(x0, mid, x1 - x0 + 1, color);
    for (int i = 0; i < DARKNESS_LEVELS; i++) {
        int x = x0 + (x1 - x0) * i / (DARKNESS_LEVELS - 1);
        epd_draw_line(x, mid - 3, x, mid + 3, color);
        if (i + 1 == level) {
            uint8_t ink = moving ? EPD_BLACK : color;       // the knob: filled, its number inverted
            epd_draw_rect(x - 6, y + 1, 13, BOX_H - 2, ink, true);
            char num[2] = {(char)('0' + level), 0};
            epd_draw_text(x - 2, mid - 3, num, 1, !ink);
        }
    }
}

static void draw(bool full)
{
    if (!full && !epd_is_awake()) {
        full = true;
    }
    if (full) {
        epd_init();
    }
    epd_clear(EPD_WHITE);
    ui_sidebar("Settings", NULL);
    drawn_minute = (long)(time(NULL) / 60);

    struct tm now;
    bool have_time = local_time_now(&now);
    if (row < top) {
        top = row;
    } else if (row >= top + ROWS_SHOWN) {
        top = row - ROWS_SHOWN + 1;
    }
    for (int r = top; r < ROW_COUNT && r < top + ROWS_SHOWN; r++) {
        int y = ROW_Y + (r - top) * ROW_H;
        const struct tm *t = (r == row && editing >= 0) ? &edit : &now;
        char value[24];
        if (r == ROW_ROTATE) {
            snprintf(value, sizeof(value), "%s", epd_rotated() ? "Flipped" : "Normal");
        } else if (r == ROW_REFRESH) {
            static const char *const modes[] = {"Stock", "Clean", "Quick", "Quickest"};
            snprintf(value, sizeof(value), "%s", modes[epd_partial_mode()]);
        } else if (r == ROW_CLEANUP) {
            snprintf(value, sizeof(value), cleanup_editing && r == row ? "< every %d >" : "every %d",
                     epd_full_every());
        } else if (r == ROW_PICTURES) {
            snprintf(value, sizeof(value), "%s", settings_get_i32(SETTING_ROTATE_TALL, 1) ? "Turn tall ones" : "Always upright");
        } else if (r == ROW_DARKNESS) {
            value[0] = '\0';                               // drawn as a slider below
        } else if (IS_FOLDER_ROW(r)) {
            char dir[128];
            row_dir(r, dir, sizeof(dir));
            if (!dir[0]) {
                snprintf(dir, sizeof(dir), "Internal");
            }
            int fits = (EPD_WIDTH - 6 - VALUE_X) / CHAR_W;  // keep the end: the folder's own name
            int len = strlen(dir);
            snprintf(value, sizeof(value), "%s%.*s", len > fits ? ".." : "", (int)sizeof(value) - 3,
                     len > fits ? dir + len - fits + 2 : dir);
        } else if (r == ROW_CARD) {
            uint32_t mb = sdcard_size_mb();
            if (mb >= 1024) {                               // its size, then what pressing does
                snprintf(value, sizeof(value), "%lu GB · remount", (unsigned long)((mb + 512) / 1024));
            } else if (mb) {
                snprintf(value, sizeof(value), "%lu MB · remount", (unsigned long)mb);
            } else {
                snprintf(value, sizeof(value), "none · mount");
            }
        } else if (r == ROW_FORMAT) {
            snprintf(value, sizeof(value), "SD card");
        } else if (!have_time && t == &now) {
            snprintf(value, sizeof(value), "not set");
        } else if (r == ROW_DATE) {
            format_date(t, value, sizeof(value));
        } else {
            format_time(t, value, sizeof(value));
        }

        uint8_t color = EPD_BLACK;
        if (r == row && editing < 0) {                      // chosen row: white on black
            epd_draw_rect(UI_CONTENT_X, y, UI_CONTENT_W, BOX_H, EPD_BLACK, true);
            color = EPD_WHITE;
        }
        int ty = y + (BOX_H - FONT16_HEIGHT) / 2;
        font16_draw_text(LEFT + 4, ty, labels[r], color);
        font16_draw_text(VALUE_X, ty, value, color);
        if (r == ROW_DARKNESS) {
            draw_darkness_slider(y, color, r == row && dark_editing);
        }

        if (r == row && editing >= 0) {                     // field being edited: boxed, inverted
            field_t f;
            field_value(r, editing, &f);
            int fx = VALUE_X + f.first_char * CHAR_W;
            int fw = f.chars * CHAR_W;
            epd_draw_rect(fx - 2, y, fw + 2, BOX_H, EPD_BLACK, true);
            char part[8];
            snprintf(part, sizeof(part), "%.*s", f.chars, value + f.first_char);
            font16_draw_text(fx, ty, part, EPD_WHITE);
        }
    }

    // Scroll bar on the right edge: where the shown rows are in the list
    int bar_h = ROWS_SHOWN * ROW_H - 1;
    epd_draw_rect(EPD_WIDTH - 3, ROW_Y + top * bar_h / ROW_COUNT, 2, bar_h * ROWS_SHOWN / ROW_COUNT, EPD_BLACK, true);

    char line[160];
    // One short line of help or status at the bottom
    if (notice[0] && editing < 0) {
        info_line(0, notice);
        notice[0] = '\0';                                  // shown once
    } else if (editing >= 0) {
        int last = (row == ROW_DATE ? DATE_FIELDS : TIME_FIELDS) - 1;
        info_line(0, editing == last ? "< > change   press: save   BACK: cancel" : "< > change   press: next   BACK: cancel");
    } else if (row == ROW_MUSIC) {
        info_line(0, "press: choose the music folder");
    } else if (IS_FOLDER_ROW(row)) {
        info_line(0, "press: choose a folder (files move along)");
    } else if (row == ROW_CARD) {
        info_line(0, "press: mount the card again (after a swap)");
    } else if (row == ROW_FORMAT) {
        info_line(0, "press: erase the card...");
    } else if (row == ROW_ROTATE) {
        info_line(0, "press: turn the screen upside down");
    } else if (row == ROW_REFRESH) {
        info_line(0, "press: next way to do quick refreshes");
    } else if (row == ROW_CLEANUP && cleanup_editing) {
        info_line(0, "< > fewer / more quick refreshes  press: done");
    } else if (row == ROW_CLEANUP) {
        info_line(0, "Full refresh after this many quick ones");
    } else if (row == ROW_PICTURES) {
        info_line(0, "Tall pictures sideways: twice as big");
    } else if (row == ROW_DARKNESS && dark_editing) {
        info_line(0, "< > lighter / darker      press: done");
    } else if (row == ROW_DARKNESS) {
        info_line(0, "press: adjust picture darkness");
    } else {
        char hm[16], zone[32];
        if (local_time_offset_known()) {
            int off = local_time_offset(), a = off < 0 ? -off : off;
            if (a % 3600) {
                snprintf(zone, sizeof(zone), "UTC%c%d:%02d", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
            } else {
                snprintf(zone, sizeof(zone), "UTC%c%d", off < 0 ? '-' : '+', a / 3600);
            }
        } else {
            snprintf(zone, sizeof(zone), "UTC (zone not known yet)");
        }
        if (local_time_format_hm(local_time_last_internet_sync(), hm, sizeof(hm))) {
            snprintf(line, sizeof(line), "Synced %s, %s", hm, zone);
        } else {
            snprintf(line, sizeof(line), "Not synced yet, %s", zone);
        }
        info_line(0, line);
    }

    if (full) {
        epd_display();
    } else {
        epd_display_partial();
    }
}

static void settings_open(const app_t *self)
{
    row = ROW_DATE;
    top = 0;
    editing = -1;
    picking = false;
    confirm_format = false;
    dark_editing = false;
    cleanup_editing = false;
    notice[0] = '\0';
    draw(true);
}

static bool button_choosing(button_event_t event)
{
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_LEFT_HOLD:
        row = (row + ROW_COUNT - 1) % ROW_COUNT;
        draw(false);
        return true;
    case BUTTON_RIGHT:
    case BUTTON_RIGHT_HOLD:
        row = (row + 1) % ROW_COUNT;
        draw(false);
        return true;
    case BUTTON_SELECT:
    case BUTTON_SELECT_HOLD:
        if (IS_FOLDER_ROW(row)) {
            pick_start(row);
        } else if (row == ROW_CARD) {
            stop_player_in(SDCARD_ROOT);                    // its open file goes with the old mount
            if (sdcard_remount()) {
                uint32_t mb = sdcard_size_mb();
                snprintf(notice, sizeof(notice), "Card mounted: %lu.%lu GB", (unsigned long)(mb / 1024),
                         (unsigned long)(mb % 1024 * 10 / 1024));
            } else {
                snprintf(notice, sizeof(notice), "No card found (or not FAT32: try Format)");
            }
            draw(false);
        } else if (row == ROW_FORMAT) {
            confirm_format = true;
            confirm_since = xTaskGetTickCount();
            draw_format_screen(NULL, -1);
        } else if (row == ROW_REFRESH) {
            int mode = (epd_partial_mode() + 1) % EPD_PARTIAL_MODES;   // Stock -> Clean -> Quick -> Quickest
            epd_set_partial_mode(mode);
            settings_set_i32(SETTING_PARTIAL_MODE, mode);
            draw(true);                                     // a clean start: the ways keep the previous image differently
        } else if (row == ROW_CLEANUP) {
            cleanup_editing = true;                         // left / right change it
            draw(false);
        } else if (row == ROW_PICTURES) {
            settings_set_i32(SETTING_ROTATE_TALL, !settings_get_i32(SETTING_ROTATE_TALL, 1));
            draw(false);
        } else if (row == ROW_DARKNESS) {
            dark_editing = true;                            // left / right move the slider
            draw(false);
        } else if (row == ROW_ROTATE) {
            bool on = !epd_rotated();
            epd_set_rotated(on);
            settings_set_i32(SETTING_ROTATE_180, on);
            ESP_LOGI(TAG, "screen %s", on ? "flipped" : "normal");
            draw(true);                                     // everything moved: full refresh
        } else {
            current_or_build_time(&edit);
            edit.tm_sec = 0;
            editing = 0;
            draw(false);
        }
        return true;
    case BUTTON_BACK:
        return false;                                       // leave the app
    default:
        return true;
    }
}

static bool button_editing(button_event_t event)
{
    field_t f;
    field_value(row, editing, &f);
    switch (event) {
    case BUTTON_LEFT:       change_field(-1); break;
    case BUTTON_RIGHT:      change_field(1); break;
    case BUTTON_LEFT_HOLD:  change_field(-f.step_hold); break;
    case BUTTON_RIGHT_HOLD: change_field(f.step_hold); break;
    case BUTTON_SELECT: {
        int fields = row == ROW_DATE ? DATE_FIELDS : TIME_FIELDS;
        if (++editing < fields) {
            break;
        }
        // Last field done: save. Editing the date keeps the current time of day and vice
        // versa, read fresh so a slow edit doesn't move the other part back.
        struct tm now;
        current_or_build_time(&now);
        if (row == ROW_DATE) {
            now.tm_year = edit.tm_year;
            now.tm_mon = edit.tm_mon;
            now.tm_mday = edit.tm_mday;
        } else {
            now.tm_hour = edit.tm_hour;
            now.tm_min = edit.tm_min;
            now.tm_sec = 0;
        }
        local_time_set_local(&now);
        editing = -1;
        break;
    }
    case BUTTON_BACK:
        editing = -1;                                       // cancel
        break;
    default:
        return true;
    }
    draw(false);
    return true;
}

static bool settings_button(const app_t *self, button_event_t event)
{
    if (confirm_format) {
        if (formatting) {
            return true;                                    // nothing until it's done
        }
        if (event == BUTTON_WAKE && format_started) {       // the format task finished
            format_started = 0;
            finish_format();
        } else if (event == BUTTON_SELECT_HOLD &&
            xTaskGetTickCount() - confirm_since >= pdMS_TO_TICKS(CONFIRM_GUARD_MS)) {
            start_format();
        } else if (event == BUTTON_BACK) {
            confirm_format = false;
            draw(false);
        }
        return true;                                        // a short press does nothing here
    }
    if (event == BUTTON_SELECT_HOLD) {
        event = BUTTON_SELECT;                              // only the format screen tells them apart
    }
    if (cleanup_editing) {                                  // Cleanup: full refresh every N
        int n = epd_full_every();
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT || event == BUTTON_LEFT_HOLD || event == BUTTON_RIGHT_HOLD) {
            n += (event == BUTTON_LEFT || event == BUTTON_LEFT_HOLD) ? -CLEANUP_STEP : CLEANUP_STEP;
            n = n < CLEANUP_MIN ? CLEANUP_MIN : n > CLEANUP_MAX ? CLEANUP_MAX : n;
            epd_set_full_every(n);
            settings_set_i32(SETTING_FULL_EVERY, n);
        } else if (event == BUTTON_SELECT || event == BUTTON_BACK) {
            cleanup_editing = false;
        } else {
            return true;
        }
        draw(false);
        return true;
    }
    if (dark_editing) {                                     // the Darkness slider
        int level = darkness_level();
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT || event == BUTTON_LEFT_HOLD || event == BUTTON_RIGHT_HOLD) {
            int step = (event == BUTTON_LEFT || event == BUTTON_LEFT_HOLD) ? -1 : 1;
            level = level + step < 1 ? 1 : level + step > DARKNESS_LEVELS ? DARKNESS_LEVELS : level + step;
            epd_set_picture_gamma(darkness_gamma[level - 1]);
            settings_set_i32(SETTING_PICTURE_GAMMA, darkness_gamma[level - 1]);
        } else if (event == BUTTON_SELECT || event == BUTTON_BACK) {
            dark_editing = false;
        } else {
            return true;
        }
        draw(false);
        return true;
    }
    if (picking) {
        if (!pick_button(event)) {
            draw(false);                                    // back to the list
        }
        return true;
    }
    return editing >= 0 ? button_editing(event) : button_choosing(event);
}

// The time on screen changes every minute while choosing a row
static TickType_t settings_next_wakeup(const app_t *self)
{
    if (formatting) {
        return pdMS_TO_TICKS(FORMAT_TICK_MS);
    }
    if (editing >= 0 || picking || confirm_format || !local_time_synced()) {
        return portMAX_DELAY;
    }
    return pdMS_TO_TICKS((60 - time(NULL) % 60) * 1000 + 200);
}

static void settings_tick(const app_t *self)
{
    if (formatting) {
        int pct = sdcard_format_progress();
        draw_format_screen("Formatting...", pct < 0 ? 0 : pct);
        return;
    }
    if (editing < 0 && !picking && !confirm_format && (long)(time(NULL) / 60) != drawn_minute) {  // also called early: check
        draw(false);
    }
}

// Menu icon: a gear (a ring with eight teeth)
static void settings_icon(const app_t *self, int cx, int cy)
{
    static const int teeth[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
    for (int i = 0; i < 8; i++) {
        int dx = teeth[i][0], dy = teeth[i][1];
        int d = (dx && dy) ? 11 : 15;                       // diagonal teeth sit a little closer in
        epd_fill_circle(cx + dx * d, cy + dy * d, 4, EPD_BLACK);
    }
    epd_fill_circle(cx, cy, 13, EPD_BLACK);
    epd_fill_circle(cx, cy, 5, EPD_WHITE);
}

const app_t settings_app = {
    .id = "settings",
    .name = "Settings",
    .draw_icon = settings_icon,
    .open = settings_open,
    .button = settings_button,
    .next_wakeup = settings_next_wakeup,
    .tick = settings_tick,
    .select_hold = true,                // hold center confirms formatting the card
};
