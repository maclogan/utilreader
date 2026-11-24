#include "media_ui.h"

#include <ctype.h>                      // tolower(): cover file names
#include <dirent.h>                     // looking for a cover in the folder
#include <stdlib.h>
#include <strings.h>

#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "epd.h"
#include "esp_log.h"
#include "font16.h"
#include "image.h"
#include "media_info.h"
#include "ui_sidebar.h"
#include "app.h"                        // app_due(), app_ticks_until()

#define MARGIN      4
#define LIST_X      (UI_CONTENT_X + MARGIN)
#define ROW_H       21                  // 6 x 21 = 126 of 128 px
#define SCROLL_W    4
#define ELLIPSIS    0x2026
#define ART         96                  // album art box, px: the left third of the screen

static const char *TAG = "media_ui";


// ---------- Now Playing's text ----------
//
// Nothing scrolls: on this panel every partial refresh nudges the grey cover's pixels too
// (it visibly pulses), so Now Playing changes the screen as rarely as it can. Long titles wrap
// onto a second line; artist and album use the small font when they can.

TickType_t media_ui_marquee_wait(void)
{
    return portMAX_DELAY;
}

bool media_ui_marquee_step(void)
{
    return false;
}

static bool is_ascii(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) {
            return false;
        }
    }
    return true;
}

// A line in the small 5x7 font (ASCII), cut with "..." to fit [x, right)
static void small_cut(int x, int y, const char *text, int right)
{
    int fits = (right - x + 1) / 6;
    if ((int)strlen(text) <= fits) {
        epd_draw_text(x, y, text, 1, EPD_BLACK);
        return;
    }
    char cut[64];
    snprintf(cut, sizeof(cut), "%.*s...", fits > 3 ? (fits - 3 < 60 ? fits - 3 : 60) : 0, text);
    epd_draw_text(x, y, cut, 1, EPD_BLACK);
}

// Artist / album: the small font for plain ASCII, the 16 px one (accents, Chinese) otherwise.
// Returns the y below it.
static int detail_line(int x, int y, const char *text, int right)
{
    if (!text[0]) {
        return y;
    }
    if (is_ascii(text)) {
        small_cut(x, y, text, right);
        return y + 11;
    }
    media_ui_text_cut(x, y, text, right, EPD_BLACK);
    return y + FONT16_HEIGHT + 1;
}

// The title in the 16 px font on up to two lines, broken between words when it can be, the
// second line cut with "..." if it's still too long. Returns the y below it.
static int title_lines(int x, int y, const char *text, int right)
{
    if (x + font16_text_width(text) <= right) {
        font16_draw_text(x, y, text, EPD_BLACK);
        return y + FONT16_HEIGHT + 1;
    }
    const char *p = text, *last_space = NULL, *fit_end = text;
    int w = 0;
    while (*p) {                                            // how much fits on line one
        const char *before = p;
        uint32_t cp = font16_next_char(&p);
        if (cp == 0 || x + w + font16_char_width(cp) > right) {
            p = before;
            break;
        }
        w += font16_char_width(cp);
        if (cp == ' ') {
            last_space = before;
        }
        fit_end = p;
    }
    const char *brk = last_space && last_space > text ? last_space : fit_end;
    char first[160];
    snprintf(first, sizeof(first), "%.*s", (int)(brk - text), text);
    font16_draw_text(x, y, first, EPD_BLACK);
    while (*brk == ' ') {
        brk++;
    }
    media_ui_text_cut(x, y + FONT16_HEIGHT + 1, brk, right, EPD_BLACK);
    return y + 2 * (FONT16_HEIGHT + 1);
}

// Which screen is up (a list's title, Now Playing...). A different one means the whole screen
// changes: a full refresh then, which also clears the ghosting the quick ones leave. (The display
// driver adds one after every so many quick refreshes too: Settings > Cleanup.)
static char shown_screen[100];
static bool screen_changes(const char *key);

bool media_ui_screen_changes(const char *key)
{
    return screen_changes(key);
}

static bool screen_changes(const char *key)
{
    if (strcmp(key, shown_screen) == 0) {
        return false;
    }
    ESP_LOGI(TAG, "screen \"%s\" -> \"%s\": full refresh", shown_screen, key);
    snprintf(shown_screen, sizeof(shown_screen), "%s", key);
    return true;
}

bool media_ui_begin(bool full)
{
    if (!full && !epd_is_awake()) {
        full = true;
    }
    if (full) {
        epd_init();
    }
    epd_clear(EPD_WHITE);
    return full;
}

void media_ui_finish(bool full)
{
    if (full) {
        epd_display();
    } else {
        epd_display_partial();
    }
}

void media_ui_text_cut(int x, int y, const char *text, int right_edge, uint8_t color)
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

static void media_ui_small_cut(int x, int y, const char *text, int right_edge)
{
    char cut[64];
    int fits = (right_edge - x) / 6;
    snprintf(cut, sizeof(cut), "%.*s", fits < 0 ? 0 : fits > 63 ? 63 : fits, text);
    epd_draw_text(x, y, cut, 1, EPD_BLACK);
}

void media_ui_small_centered(int y, const char *text)
{
    epd_draw_text(UI_CONTENT_CX - epd_text_width(text, 1) / 2, y, text, 1, EPD_BLACK);
}

void media_ui_format_time(char *out, size_t size, uint32_t ms)
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

static uint32_t art_shown_sum;          // checksum of the cover on screen (0 = none)

void media_ui_list(const char *title, int n, int sel, media_row_fn row, const char *hint, bool full)
{
    char key[100];
    snprintf(key, sizeof(key), "list:%.90s", title);
    bool changed = screen_changes(key);                     // noted even when `full` already (no || short cut)
    full = media_ui_begin(full || changed);
    art_shown_sum = 0;                  // the cover is off screen
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", sel + 1, n);
    ui_sidebar(title, n > 1 ? counter : NULL);

    player_status_t ps;
    player_status(&ps);
    bool scroll = n > MEDIA_UI_ROWS;
    int right = EPD_WIDTH - MARGIN - (scroll ? SCROLL_W + 3 : 0);
    int first = sel / MEDIA_UI_ROWS * MEDIA_UI_ROWS;
    for (int i = first; i < n && i < first + MEDIA_UI_ROWS; i++) {
        int y = 1 + (i - first) * ROW_H;
        bool is_sel = i == sel;
        uint8_t color = is_sel ? EPD_WHITE : EPD_BLACK;
        if (is_sel) {
            epd_draw_rect(UI_CONTENT_X, y, right - UI_CONTENT_X + 2, ROW_H - 1, EPD_BLACK, true);
        }
        media_row_t r = {.label = ""};
        row(i, &r);
        int text_right = right - (r.opens ? 10 : 0);
        if (r.tag[0]) {
            int w = epd_text_width(r.tag, 1);
            epd_draw_text(text_right - w, y + 7, r.tag, 1, color);
            text_right -= w + 6;
        }
        if (r.playing) {
            const char *m = ps.paused ? "||" : ">";
            epd_draw_text(text_right - epd_text_width(m, 1), y + 7, m, 1, color);
            text_right -= 14;
        }
        media_ui_text_cut(LIST_X, y + 2, r.label, text_right, color);
        if (r.opens) {
            chevron(right - 6, y + 6, color);
        }
    }
    if (scroll) {                                           // scroll bar: track and thumb
        int x = EPD_WIDTH - MARGIN - SCROLL_W, h = EPD_HEIGHT - 4;
        epd_draw_rect(x, 2, SCROLL_W, h, EPD_BLACK, false);
        int th = h * MEDIA_UI_ROWS / n;
        th = th < 6 ? 6 : th;
        int last_first = (n - 1) / MEDIA_UI_ROWS * MEDIA_UI_ROWS;
        int ty = 2 + (h - th) * first / (last_first > 0 ? last_first : 1);
        epd_draw_rect(x, ty, SCROLL_W, th, EPD_BLACK, true);
    }
    if (n == 0 && hint) {
        media_ui_small_centered(60, hint);
    }
    media_ui_finish(full);
}

// ---------- album art ----------

// The cover of the item playing, decoded once per item and kept in greyscale
static struct {
    char path[320];                     // the item it belongs to
    image_grey_t img;                   // pix NULL: no cover
    uint32_t sum;                       // checksum of the picture: same cover = same sum
} art;

// A picture file next to the track, as an album cover: higher = more likely the front cover.
// 0 = not a picture. Covers "cover.jpg" / "folder.png" / Windows' hidden "AlbumArt_{...}_Large.jpg"
// and "AlbumArtSmall.jpg", and any other picture as a last resort (not the back / disc scans).
static int cover_score(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png"))) {
        return 0;
    }
    char base[64];
    int n = (int)(dot - name) < (int)sizeof(base) - 1 ? (int)(dot - name) : (int)sizeof(base) - 1;
    for (int i = 0; i < n; i++) {
        base[i] = (char)tolower((unsigned char)name[i]);
    }
    base[n] = '\0';
    if (!strcmp(base, "cover") || !strcmp(base, "folder") || !strcmp(base, "front")) return 100;
    if (!strcmp(base, "album")) return 90;
    if (!strncmp(base, "albumart", 8) && strstr(base, "large")) return 80;
    if (!strcmp(base, "albumartsmall")) return 60;
    if (!strncmp(base, "albumart", 8)) return 50;
    if (strstr(base, "cover") || strstr(base, "front")) return 40;
    if (strstr(base, "back") || strstr(base, "disc") || strstr(base, "cd") || strstr(base, "inlay") ||
        strstr(base, "booklet") || strstr(base, "tray")) return 5;
    return 20;
}

// Main task only: the big buffers are static rather than on its stack (Now Playing draws from
// inside button handlers, where the stack is already deep)
static bool load_cover(const char *path, image_grey_t *img)
{
    // The file's own cover: straight from the file, or (unsynchronised ID3, Ogg / Opus comments)
    // through a copy in memory
    static media_info_t m;
    static const char *const kinds[] = {"none", "in the file", "unsynchronised ID3", "Ogg comment"};
    bool have_info = media_info_read(path, &m);
    if (have_info && m.art_size) {
        ESP_LOGI(TAG, "cover tag: %s, %lu bytes", kinds[m.art_kind & 3], (unsigned long)m.art_size);
    } else {
        ESP_LOGI(TAG, "cover tag: none in the file (%s)", have_info ? media_format_name(m.format) : "tags unreadable");
    }
    if (have_info && m.art_size) {
        bool ok = false;
        if (m.art_kind == MEDIA_ART_FILE) {
            FILE *f = fopen(path, "rb");
            if (f) {
                ok = image_jpeg_grey(f, m.art_offset, m.art_size, ART, ART, img);
                fclose(f);
            }
        }
        if (!ok) {
            uint8_t *data;
            size_t len;
            if (media_info_art_bytes(path, &m, &data, &len)) {
                ok = image_grey_mem(data, len, ART, ART, img);
                free(data);
            }
        }
        if (ok) {
            return true;
        }
        ESP_LOGI(TAG, "cover tag: couldn't decode it (see the image: line above)");
    }
    // Otherwise a picture in the track's folder: the best few candidates, best first
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }
    static char dir[340];
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    enum { TRIES = 3 };
    static char best[TRIES][128];
    int score[TRIES] = {0};
    memset(best, 0, sizeof(best));
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        int sc = e->d_type == DT_DIR ? 0 : cover_score(e->d_name);
        if (sc == 0 || strlen(e->d_name) >= sizeof(best[0])) {
            continue;
        }
        for (int k = 0; k < TRIES; k++) {                   // keep the top few, in order
            if (sc > score[k]) {
                for (int m2 = TRIES - 1; m2 > k; m2--) {
                    score[m2] = score[m2 - 1];
                    memcpy(best[m2], best[m2 - 1], sizeof(best[0]));
                }
                score[k] = sc;
                memcpy(best[k], e->d_name, strlen(e->d_name) + 1);   // fits: checked above
                break;
            }
        }
    }
    if (d) {
        closedir(d);
    }
    static char file[480];
    if (score[0] == 0) {
        ESP_LOGI(TAG, "cover: no picture file in %s", dir);
    }
    for (int k = 0; k < TRIES && score[k]; k++) {
        snprintf(file, sizeof(file), "%s/%s", dir, best[k]);
        if (image_jpeg_grey_file(file, ART, ART, img)) {
            ESP_LOGI(TAG, "cover from %s", best[k]);
            return true;
        }
        ESP_LOGI(TAG, "cover: couldn't decode %s", best[k]);
    }
    return false;
}

// Makes `art` match the item at `path` ("" = nothing playing)
static void update_art(const char *path)
{
    if (strcmp(path, art.path) == 0) {
        return;
    }
    snprintf(art.path, sizeof(art.path), "%s", path);
    image_free(&art.img);
    art.sum = 0;
    if (path[0] && load_cover(path, &art.img)) {
        image_enlarge_to_fit(&art.img, ART, ART);           // small covers (e.g. progressive JPEGs' 1/8 size)
        image_auto_levels(&art.img);
        uint32_t h = 2166136261u;                           // FNV-1a
        for (int i = 0; i < art.img.w * art.img.h; i++) {
            h = (h ^ art.img.pix[i]) * 16777619u;
        }
        art.sum = h ? h : 1;
        ESP_LOGI(TAG, "cover %dx%d", art.img.w, art.img.h);
    }
}

// No cover: a framed note stands in for it
static void draw_note(int x, int y, int s)
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

// The cover in 4 greys, centred in the box, or the note
static void draw_art(int x, int y, int s)
{
    if (art.img.pix == NULL) {
        draw_note(x, y, s);
        return;
    }
    epd_draw_grey8(x + (s - art.img.w) / 2, y + (s - art.img.h) / 2, art.img.w, art.img.h,
                   art.img.pix, art.img.w, true);
}

void media_ui_playing(bool seek_mode, const char *seek_hint, const char *idle, bool full)
{
    player_status_t ps;
    player_status(&ps);
    // The last item that played. When playback stops because of a problem (no DAC, a file that
    // won't decode), keep showing it, cover and all, with the problem, rather than "Not playing"
    static struct { char path[320], title[128], artist[96], album[96]; } last;
    if (ps.index >= 0) {
        snprintf(last.path, sizeof(last.path), "%s", ps.path);
        snprintf(last.title, sizeof(last.title), "%s", ps.title);
        snprintf(last.artist, sizeof(last.artist), "%s", ps.artist);
        snprintf(last.album, sizeof(last.album), "%s", ps.album);
    } else if (ps.message[0] && last.path[0]) {
        snprintf(ps.path, sizeof(ps.path), "%s", last.path);
        snprintf(ps.title, sizeof(ps.title), "%s", last.title);
        snprintf(ps.artist, sizeof(ps.artist), "%s", last.artist);
        snprintf(ps.album, sizeof(ps.album), "%s", last.album);
        ps.index = 0;                                       // drawn as that item, stopped
        ps.queue_pos = 0;
        ps.queue_len = 1;
        ps.elapsed_ms = 0;
        ps.paused = true;
    }
    update_art(ps.index >= 0 ? ps.path : "");
    if (art.sum != art_shown_sum) {
        full = true;                    // a new cover needs the grey (full) refresh
    }
    bool changed = screen_changes("playing");
    full = media_ui_begin(full || changed);
    art_shown_sum = art.sum;
    char a[16], b[24], line[64];

    // The cover in the left third, centred top to bottom (no title bar here: "3/12" is on the
    // info line); everything else in the column to its right
    const int ax = 2, ay = (EPD_HEIGHT - ART) / 2;
    const int tx = ax + ART + 6, right = EPD_WIDTH - MARGIN;
    draw_art(ax, ay, ART);
    int text_end;                                           // the y below the title / artist / album
    if (ps.index < 0) {
        font16_draw_text(tx, 4, "Not playing", EPD_BLACK);
        small_cut(tx, 26, idle, right);
        text_end = 36;
    } else {
        int y = title_lines(tx, 2, ps.title, right) + 2;
        y = detail_line(tx, y, ps.artist, right);
        text_end = detail_line(tx, y, ps.album, right);
    }
    const int info_y = text_end + 4 > 62 ? text_end + 4 : 62;  // the rest follows the text
    if (ps.message[0]) {
        snprintf(line, sizeof(line), "%s", ps.message);
    } else if (ps.index >= 0) {
        snprintf(line, sizeof(line), "%d/%d  %s%s", ps.queue_pos + 1, ps.queue_len,
                 ps.paused ? "Paused  " : "", ps.rate ? media_format_name(ps.format) : "");
        if (ps.rate) {
            size_t n = strlen(line);
            snprintf(line + n, sizeof(line) - n, " %lu.%lu kHz", (unsigned long)(ps.rate / 1000),
                     (unsigned long)(ps.rate % 1000 / 100));
        }
    } else {
        line[0] = '\0';
    }
    small_cut(tx, info_y, line, right);

    // Progress: elapsed on the left, time left on the right
    const int bar_y = info_y + 12, bar_w = right - tx;
    epd_draw_rect(tx, bar_y, bar_w, 7, EPD_BLACK, false);
    if (ps.duration_ms) {
        epd_draw_rect(tx, bar_y, (int)((uint64_t)bar_w * ps.elapsed_ms / ps.duration_ms), 7, EPD_BLACK, true);
    }
    media_ui_format_time(a, sizeof(a), ps.elapsed_ms);
    epd_draw_text(tx, bar_y + 11, a, 1, EPD_BLACK);
    if (ps.duration_ms) {
        b[0] = '-';
        media_ui_format_time(b + 1, sizeof(b) - 1, ps.duration_ms - ps.elapsed_ms);
        epd_draw_text(right - epd_text_width(b, 1), bar_y + 11, b, 1, EPD_BLACK);
    }

    // Mode: VOL with the volume bar, or SEEK with its hint
    const int mode_y = 106;
    const char *badge = seek_mode ? "SEEK" : "VOL";
    int bw = epd_text_width(badge, 1) + 8;
    epd_draw_rect(tx, mode_y, bw, 13, EPD_BLACK, true);
    epd_draw_text(tx + 4, mode_y + 3, badge, 1, EPD_WHITE);
    int vx = tx + bw + 6;
    if (seek_mode) {
        media_ui_small_cut(vx, mode_y + 3, seek_hint, right);
    } else {
        int vol = audio_volume();
        snprintf(line, sizeof(line), "%d", vol);
        int nx = right - epd_text_width(line, 1);
        int vw = nx - 6 - vx;                               // a bar filled to the volume
        epd_draw_rect(vx, mode_y + 2, vw, 9, EPD_BLACK, false);
        epd_draw_rect(vx + 2, mode_y + 4, (vw - 4) * vol / AUDIO_VOLUME_MAX, 5, EPD_BLACK, true);
        epd_draw_text(nx, mode_y + 3, line, 1, EPD_BLACK);
    }
    media_ui_finish(full);
}

void media_ui_message(const char *bar_title, const char *title, const char *detail, const char *hint)
{
    media_ui_begin(true);
    screen_changes("message");
    art_shown_sum = 0;
    ui_sidebar(bar_title, NULL);
    font16_draw_text(UI_CONTENT_CX - font16_text_width(title) / 2, 34, title, EPD_BLACK);
    media_ui_small_centered(62, detail);
    media_ui_small_centered(EPD_HEIGHT - 9, hint);
    media_ui_finish(true);
}
