#include "settings_app.h"

#include <dirent.h>                     // the music folder picker
#include <stdio.h>                      // snprintf()
#include <stdlib.h>                     // qsort()
#include <string.h>                     // strncmp()
#include <strings.h>                    // strcasecmp()
#include <time.h>
#include "font16.h"                     // folder names
#include "sdcard.h"                     // the microSD card
#include "epd.h"                        // drawing, epd_set_rotated()
#include "esp_log.h"
#include "local_time.h"                 // reading and setting the clock
#include "settings.h"                   // SETTING_ROTATE_180
#include "ui_sidebar.h"                 // title bar on the left

static const char *TAG = "settings_app";

// Layout (pixels). The title bar takes the left UI_SIDEBAR_W columns.
#define LEFT        (UI_CONTENT_X + 4)
#define ROW_Y       6                   // top of the first row
#define ROW_H       22                  // row spacing
#define BOX_H       20                  // highlight bar height
#define VALUE_X     (UI_CONTENT_X + 100)    // left edge of the values
#define CHAR_W      12                  // 5x7 font at scale 2: 6 px per character x 2
#define INFO_Y      94                  // two lines of small text at the bottom
#define INFO_H      13

typedef enum { ROW_DATE, ROW_TIME, ROW_ROTATE, ROW_MUSIC, ROW_COUNT } row_t;
static const char *const labels[ROW_COUNT] = {"Date", "Time", "Rotate", "Music"};

static int row = ROW_DATE;              // highlighted row
static int editing = -1;                // field being edited (-1 = choosing a row)
static struct tm edit;                  // the date/time being edited (local)
static long drawn_minute = -1;          // minute (since 1970) the screen shows

// ---------- music folder picker ----------
// Lists the folders inside `pick_dir` on the card. Entry 0 is "Use this folder"; the rest
// open a folder. BACK goes up a level, and out of the picker from the top.

#define PICK_MAX    64
#define PICK_ROWS   5
#define PICK_ROW_H  18

static void info_line(int line, const char *text);

static bool picking;
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
    ui_sidebar("Music folder", NULL);
    char line[160];
    if (pick_no_card) {
        font16_draw_text(LEFT + 4, 30, "No microSD card", EPD_BLACK);
        info_line(1, "BACK: cancel");
    } else {
        snprintf(line, sizeof(line), "In: %s", pick_dir[0] ? pick_dir : "/ (top of the card)");
        epd_draw_text(LEFT, 3, line, 1, EPD_BLACK);
        int n = pick_count + 1;
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
            } else {
                snprintf(line, sizeof(line), "%s/", pick_names[i - 1]);
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

static void pick_start(void)
{
    // Start in the folder above the current choice, with it highlighted
    char cur[128];
    settings_get_str(SETTING_MUSIC_DIR, cur, sizeof(cur), SETTING_MUSIC_DIR_DEFAULT);
    char *slash = strrchr(cur, '/');
    const char *leaf = slash ? slash + 1 : cur;
    snprintf(pick_dir, sizeof(pick_dir), "%.*s", slash ? (int)(slash - cur) : 0, cur);
    picking = true;
    pick_load();
    for (int i = 0; i < pick_count; i++) {
        if (!strcasecmp(pick_names[i], leaf)) {
            pick_sel = i + 1;
        }
    }
    pick_draw(false);
}

// Returns false when the picker closes (the settings list is redrawn by the caller)
static bool pick_button(button_event_t event)
{
    int n = pick_count + 1;
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
            settings_set_str(SETTING_MUSIC_DIR, pick_dir[0] ? pick_dir : "/");
            ESP_LOGI(TAG, "music folder: %s", pick_dir[0] ? pick_dir : "/");
            picking = false;
            return false;
        } else {
            size_t len = strlen(pick_dir);
            snprintf(pick_dir + len, sizeof(pick_dir) - len, "/%s", pick_names[pick_sel - 1]);
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
                pick_sel = i + 1;                           // back on the folder we came from
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

static void info_line(int line, const char *text)
{
    epd_draw_text(LEFT, INFO_Y + line * INFO_H, text, 1, EPD_BLACK);
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
    for (int r = 0; r < ROW_COUNT; r++) {
        int y = ROW_Y + r * ROW_H;
        const struct tm *t = (r == row && editing >= 0) ? &edit : &now;
        char value[24];
        if (r == ROW_ROTATE) {
            snprintf(value, sizeof(value), "%s", epd_rotated() ? "Flipped" : "Normal");
        } else if (r == ROW_MUSIC) {
            char dir[64];
            settings_get_str(SETTING_MUSIC_DIR, dir, sizeof(dir), SETTING_MUSIC_DIR_DEFAULT);
            int fits = (EPD_WIDTH - VALUE_X) / CHAR_W;      // keep the end: the folder's own name
            int len = strlen(dir);
            snprintf(value, sizeof(value), "%s%.*s", len > fits ? ".." : "", (int)sizeof(value) - 3,
                     len > fits ? dir + len - fits + 2 : dir);
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
        int ty = y + (BOX_H - 14) / 2;
        epd_draw_text(LEFT + 4, ty, labels[r], 2, color);
        epd_draw_text(VALUE_X, ty, value, 2, color);

        if (r == row && editing >= 0) {                     // field being edited: boxed, inverted
            field_t f;
            field_value(r, editing, &f);
            int fx = VALUE_X + f.first_char * CHAR_W;
            int fw = f.chars * CHAR_W;
            epd_draw_rect(fx - 2, y, fw + 2, BOX_H, EPD_BLACK, true);
            char part[8];
            snprintf(part, sizeof(part), "%.*s", f.chars, value + f.first_char);
            epd_draw_text(fx, ty, part, 2, EPD_WHITE);
        }
    }

    char line[48];
    if (editing >= 0) {
        int last = (row == ROW_DATE ? DATE_FIELDS : TIME_FIELDS) - 1;
        info_line(0, "< > change      hold < >: by more");
        info_line(1, editing == last ? "press: save     BACK: cancel" : "press: next     BACK: cancel");
    } else {
        char hm[16];
        if (local_time_format_hm(local_time_last_internet_sync(), hm, sizeof(hm))) {
            snprintf(line, sizeof(line), "Internet time: synced %s", hm);
        } else {
            snprintf(line, sizeof(line), "Internet time: not synced yet");
        }
        info_line(0, line);
        if (local_time_offset_known()) {
            int off = local_time_offset();
            int a = off < 0 ? -off : off;
            if (a % 3600) {
                snprintf(line, sizeof(line), "Time zone: UTC%c%d:%02d (from forecast)",
                         off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
            } else {
                snprintf(line, sizeof(line), "Time zone: UTC%c%d (from forecast)", off < 0 ? '-' : '+', a / 3600);
            }
        } else {
            snprintf(line, sizeof(line), "Time zone: not known yet (UTC)");
        }
        info_line(1, line);
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
    editing = -1;
    picking = false;
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
        if (row == ROW_MUSIC) {
            pick_start();
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
    if (editing >= 0 || picking || !local_time_synced()) {
        return portMAX_DELAY;
    }
    return pdMS_TO_TICKS((60 - time(NULL) % 60) * 1000 + 200);
}

static void settings_tick(const app_t *self)
{
    if (editing < 0 && !picking && (long)(time(NULL) / 60) != drawn_minute) {  // also called early: check
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
};
