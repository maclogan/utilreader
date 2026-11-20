#include "media_ui.h"

#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "epd.h"
#include "font16.h"
#include "ui_sidebar.h"

#define MARGIN      4
#define LIST_X      (UI_CONTENT_X + MARGIN)
#define ROW_H       21                  // 6 x 21 = 126 of 128 px
#define SCROLL_W    4
#define FULL_EVERY  30                  // partial refreshes between full ones (ghosting)
#define ELLIPSIS    0x2026

static int partials;

bool media_ui_begin(bool full)
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

void media_ui_list(const char *title, int n, int sel, media_row_fn row, const char *hint, bool full)
{
    full = media_ui_begin(full);
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

void media_ui_playing(bool seek_mode, const char *seek_hint, const char *idle, bool full)
{
    player_status_t ps;
    player_status(&ps);
    full = media_ui_begin(full);
    char counter[16], a[16], b[24], line[64];
    snprintf(counter, sizeof(counter), "%d/%d", ps.queue_pos + 1, ps.queue_len);
    ui_sidebar("Now Playing", ps.index >= 0 ? counter : NULL);

    const int art = 52, ax = UI_CONTENT_X + 5, tx = ax + art + 8, right = EPD_WIDTH - MARGIN;
    draw_art(ax, 4, art);
    if (ps.index < 0) {
        font16_draw_text(tx, 4, "Not playing", EPD_BLACK);
        epd_draw_text(tx, 26, idle, 1, EPD_BLACK);
    } else {
        media_ui_text_cut(tx, 3, ps.title, right, EPD_BLACK);
        media_ui_text_cut(tx, 21, ps.artist, right, EPD_BLACK);
        media_ui_text_cut(tx, 39, ps.album, right, EPD_BLACK);
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
    media_ui_format_time(a, sizeof(a), ps.elapsed_ms);
    epd_draw_text(ax, bar_y + 11, a, 1, EPD_BLACK);
    if (ps.duration_ms) {
        b[0] = '-';
        media_ui_format_time(b + 1, sizeof(b) - 1, ps.duration_ms - ps.elapsed_ms);
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
        epd_draw_text(vx, mode_y + 3, seek_hint, 1, EPD_BLACK);
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
    media_ui_finish(full);
}

void media_ui_message(const char *bar_title, const char *title, const char *detail, const char *hint)
{
    media_ui_begin(true);
    ui_sidebar(bar_title, NULL);
    font16_draw_text(UI_CONTENT_CX - font16_text_width(title) / 2, 34, title, EPD_BLACK);
    media_ui_small_centered(62, detail);
    media_ui_small_centered(EPD_HEIGHT - 9, hint);
    media_ui_finish(true);
}
