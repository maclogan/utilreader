#include "podcasts_app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "audio.h"
#include "buttons.h"                    // buttons_post(BUTTON_WAKE)
#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_ui.h"                   // the list / Now Playing screens shared with Music
#include "player.h"
#include "podcasts.h"
#include "power.h"                      // power_add_off_hook()
#include "sdcard.h"
#include "settings.h"

static const char *TAG = "podcasts_app";

#define TICK_MS         30000           // Now Playing: time updated this often (each update nudges the grey cover)
#define SAVE_EVERY_MS   (2 * 60 * 1000)
#define MAX_QUEUE       PODCASTS_MAX_EPISODES

typedef enum { V_ROOT, V_FEED, V_PLAYING, V_MESSAGE } view_t;

typedef struct {
    view_t view;
    int sel;
} level_t;

static level_t stack[3];
static int depth;
#define CUR (stack[depth])

static pod_feed_t *feeds;               // PSRAM
static int feed_count;
static int *unplayed;                   // per feed, counted when the top list loads
static pod_episode_t *eps;              // the open feed's episodes on the card, in its order
static int ep_count;
static int open_feed;                   // index into feeds
static int root_rows[PODCASTS_MAX_FEEDS + 2], root_count;
enum { ROOT_PLAYING = -1, ROOT_UPDATE = -2 };          // root_rows: >= 0 a feed index

static bool seek_mode;
static int seek_back_ms = 10000, seek_fwd_ms = 30000;
static char seek_hint[40];
static TickType_t next_tick, last_save;

// ---------- the player source: queue ids index qmap ----------

typedef struct {
    char feed[9];
    char ep[17];
} qitem_t;

static qitem_t *qmap;                   // PSRAM
static int qstart = -1;                 // the episode chosen (plays even if already played)
static char playing_feed_title[96];

static bool pod_get(int id, player_item_t *out)
{
    pod_episode_t e;
    if (id < 0 || id >= MAX_QUEUE || !podcasts_episode(qmap[id].feed, qmap[id].ep, &e) ||
        !(e.flags & POD_DOWNLOADED)) {
        return false;
    }
    if ((e.flags & POD_PLAYED) && id != qstart) {
        return false;                                       // "next unplayed": skip played ones
    }
    podcasts_episode_path(qmap[id].feed, &e, false, out->path, sizeof(out->path));
    snprintf(out->title, sizeof(out->title), "%s", e.title);
    snprintf(out->artist, sizeof(out->artist), "%s", playing_feed_title);
    struct tm tm;
    time_t t = (time_t)e.date;
    gmtime_r(&t, &tm);
    strftime(out->album, sizeof(out->album), "%d %b %Y", &tm);
    out->start_ms = e.position_ms > 5000 ? e.position_ms - 3000 : 0;   // a little before where it stopped
    podcasts_set_playing(qmap[id].feed, qmap[id].ep);
    return true;
}

static void pod_finished(int id)
{
    if (id >= 0 && id < MAX_QUEUE) {
        podcasts_set_progress(qmap[id].feed, qmap[id].ep, 0, true);
        podcasts_set_playing(NULL, NULL);                   // the rules may now tidy it away
    }
}

static const player_source_t src_next = {.get = pod_get, .finished = pod_finished, .advance = true};
static const player_source_t src_stop = {.get = pod_get, .finished = pod_finished, .advance = false};

// An episode was queued since the app opened: Now Playing stays on the list even if it
// stopped because of a problem (no DAC yet, a bad file), as in the Music app
static bool pod_queued;

static bool pod_active(void)
{
    player_status_t ps;
    player_status(&ps);
    return ps.index >= 0 && (ps.source == &src_next || ps.source == &src_stop);
}

static void save_place(void)
{
    player_status_t ps;
    player_status(&ps);
    if (ps.index < 0 || (ps.source != &src_next && ps.source != &src_stop) || ps.index >= MAX_QUEUE) {
        return;
    }
    podcasts_set_progress(qmap[ps.index].feed, qmap[ps.index].ep, ps.elapsed_ms, false);
    last_save = xTaskGetTickCount();
}

// ---------- loading ----------

static void load_root(void)
{
    feed_count = podcasts_feeds(feeds, PODCASTS_MAX_FEEDS);
    for (int i = 0; i < feed_count; i++) {
        int n = podcasts_episodes(feeds[i].id, eps, PODCASTS_MAX_EPISODES);
        unplayed[i] = 0;
        for (int k = 0; k < n; k++) {
            unplayed[i] += (eps[k].flags & POD_DOWNLOADED) && !(eps[k].flags & POD_PLAYED);
        }
    }
    ep_count = 0;                                           // eps was borrowed for counting
    root_count = 0;
    if (pod_active() || pod_queued) {
        root_rows[root_count++] = ROOT_PLAYING;
    }
    for (int i = 0; i < feed_count; i++) {
        root_rows[root_count++] = i;
    }
    root_rows[root_count++] = ROOT_UPDATE;
}

// The open feed's episodes that are on the card, in the feed's order
static void load_feed(void)
{
    pod_options_t o = podcasts_effective(&feeds[open_feed].opt);
    int n = podcasts_episodes(feeds[open_feed].id, eps, PODCASTS_MAX_EPISODES);
    ep_count = 0;
    for (int k = 0; k < n; k++) {
        if (eps[k].flags & POD_DOWNLOADED) {
            eps[ep_count++] = eps[k];
        }
    }
    if (o.oldest_first) {
        for (int a = 0, b = ep_count - 1; a < b; a++, b--) {
            pod_episode_t t = eps[a];
            eps[a] = eps[b];
            eps[b] = t;
        }
    }
}

static void load_view(void)
{
    if (CUR.view == V_ROOT) {
        load_root();
    } else if (CUR.view == V_FEED) {
        load_feed();
    }
    int n = CUR.view == V_ROOT ? root_count : CUR.view == V_FEED ? ep_count : 0;
    if (CUR.sel >= n) {
        CUR.sel = n > 0 ? n - 1 : 0;
    }
}

// ---------- drawing ----------

// Updates happen only while the Transfer app is open: the last row says so
static void update_label(char *out, size_t size)
{
    pod_update_status_t u;
    podcasts_update_status(&u);
    if (u.message[0]) {
        snprintf(out, size, "Last update: %.50s", u.message);
    } else {
        snprintf(out, size, "New episodes: open Transfer");
    }
}

static char row_text[64];

static void root_row(int i, media_row_t *r)
{
    int k = root_rows[i];
    if (k == ROOT_PLAYING) {
        r->label = "Now Playing";
        r->opens = true;
    } else if (k == ROOT_UPDATE) {
        update_label(row_text, sizeof(row_text));
        r->label = row_text;
    } else {
        r->label = feeds[k].title[0] ? feeds[k].title : feeds[k].url;
        r->opens = true;
        if (unplayed[k]) {
            snprintf(r->tag, sizeof(r->tag), "%d", unplayed[k]);
        }
    }
}

static void episode_row(int i, media_row_t *r)
{
    const pod_episode_t *e = &eps[i];
    r->label = e->title;
    player_status_t ps;
    player_status(&ps);
    r->playing = pod_active() && ps.index >= 0 && ps.index < MAX_QUEUE &&
                 !strcmp(qmap[ps.index].ep, e->id) && !strcmp(qmap[ps.index].feed, feeds[open_feed].id);
    if (e->flags & POD_PLAYED) {
        snprintf(r->tag, sizeof(r->tag), "played");
    } else if (e->position_ms > 0 && e->duration_s) {
        uint32_t left = e->duration_s > e->position_ms / 1000 ? e->duration_s - e->position_ms / 1000 : 0;
        snprintf(r->tag, sizeof(r->tag), "%lum left", (unsigned long)((left + 59) / 60));
    } else if (e->position_ms > 0) {
        snprintf(r->tag, sizeof(r->tag), "started");
    } else {
        snprintf(r->tag, sizeof(r->tag), "new");
    }
}

static void draw(bool full)
{
    switch (CUR.view) {
    case V_ROOT:
        media_ui_list("Podcasts", root_count, CUR.sel, root_row, NULL, full);
        break;
    case V_FEED:
        media_ui_list(feeds[open_feed].title[0] ? feeds[open_feed].title : "Podcast", ep_count, CUR.sel,
                      episode_row, "Nothing downloaded yet", full);
        break;
    case V_PLAYING:
        media_ui_playing(seek_mode, seek_hint, "Choose an episode", full);
        next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_MS);
        break;
    default:
        media_ui_message("Podcasts", "No microSD card", "Podcasts are kept on the card",
                         "Press: try again   BACK: leave");
        break;
    }
}

static void push(view_t v, int sel)
{
    if (depth < 2) {
        depth++;
    }
    CUR = (level_t){v, sel};
    load_view();
}

static void pop(void)
{
    if (depth > 0) {
        depth--;
    }
    load_view();
}

// ---------- actions ----------

static void notify_from_tasks(void)
{
    buttons_post(BUTTON_WAKE);                              // player or updater: the main loop redraws
}

// Plays eps[pos] of the open feed, then (if the feed says so) the next unplayed ones
static void play_episode(int pos)
{
    save_place();
    pod_options_t o = podcasts_effective(&feeds[open_feed].opt);
    static int ids[MAX_QUEUE];
    for (int i = 0; i < ep_count && i < MAX_QUEUE; i++) {
        snprintf(qmap[i].feed, sizeof(qmap[i].feed), "%s", feeds[open_feed].id);
        snprintf(qmap[i].ep, sizeof(qmap[i].ep), "%s", eps[i].id);
        ids[i] = i;
    }
    qstart = pos;
    snprintf(playing_feed_title, sizeof(playing_feed_title), "%s", feeds[open_feed].title);
    player_play_queue(o.after_next ? &src_next : &src_stop, ids, ep_count, pos, PLAYER_ITEM_START);
    pod_queued = true;
}

static void open_root(bool full)
{
    depth = 0;
    CUR = (level_t){V_ROOT, 0};
    load_view();
    draw(full);
}

// ---------- app callbacks ----------

static void power_off_hook(void)
{
    podcasts_update_stop();                                 // a partial download resumes next time
    if (pod_active()) {
        save_place();
        player_stop();
    }
    podcasts_set_playing(NULL, NULL);
}

static void podcasts_init(const app_t *self)
{
    feeds = heap_caps_malloc(PODCASTS_MAX_FEEDS * sizeof(pod_feed_t), MALLOC_CAP_SPIRAM);
    unplayed = heap_caps_malloc(PODCASTS_MAX_FEEDS * sizeof(int), MALLOC_CAP_SPIRAM);
    eps = heap_caps_malloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t), MALLOC_CAP_SPIRAM);
    qmap = heap_caps_malloc(MAX_QUEUE * sizeof(qitem_t), MALLOC_CAP_SPIRAM);
    player_init(notify_from_tasks);                         // already started by Music: no-op
    power_add_off_hook(power_off_hook);
}

static void podcasts_open(const app_t *self)
{
    if (!feeds || !unplayed || !eps || !qmap) {
        return;
    }
    pod_options_t o;
    int back, fwd;
    podcasts_defaults(&o, &back, &fwd);
    seek_back_ms = back * 1000;
    seek_fwd_ms = fwd * 1000;
    snprintf(seek_hint, sizeof(seek_hint), "< >  back %d s / ahead %d s", back, fwd);

    if (!sdcard_mount()) {
        depth = 0;
        CUR = (level_t){V_MESSAGE, 0};
        draw(true);
        return;
    }
    // Tidy up by the keep rules (deletes played episodes). Fetching and downloading happen
    // only while the Transfer app is open, so they never compete with playback.
    feed_count = podcasts_feeds(feeds, PODCASTS_MAX_FEEDS);
    for (int i = 0; i < feed_count; i++) {
        podcasts_apply_rules(feeds[i].id);
    }
    if (pod_active()) {
        open_root(true);
        push(V_PLAYING, 0);
        draw(true);
    } else {
        open_root(true);
    }
}

static bool list_button(button_event_t event)
{
    int n = CUR.view == V_ROOT ? root_count : ep_count;
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_RIGHT:
        if (n > 0) {
            CUR.sel = (CUR.sel + (event == BUTTON_LEFT ? n - 1 : 1)) % n;
            draw(false);
        }
        return true;
    case BUTTON_LEFT_HOLD:
        CUR.sel = CUR.sel >= MEDIA_UI_ROWS ? (CUR.sel / MEDIA_UI_ROWS - 1) * MEDIA_UI_ROWS : 0;
        draw(false);
        return true;
    case BUTTON_RIGHT_HOLD:
        CUR.sel = (CUR.sel / MEDIA_UI_ROWS + 1) * MEDIA_UI_ROWS < n ? (CUR.sel / MEDIA_UI_ROWS + 1) * MEDIA_UI_ROWS : n - 1;
        draw(false);
        return true;
    case BUTTON_SELECT:
        if (n == 0) {
            return true;
        }
        if (CUR.view == V_ROOT) {
            int k = root_rows[CUR.sel];
            if (k == ROOT_PLAYING) {
                push(V_PLAYING, 0);
            } else if (k == ROOT_UPDATE) {
                return true;                                // just information
            } else {
                open_feed = k;
                push(V_FEED, 0);
            }
        } else {
            play_episode(CUR.sel);
            push(V_PLAYING, 0);
        }
        draw(false);
        return true;
    case BUTTON_SELECT_HOLD:
        if (CUR.view == V_FEED && n > 0) {                  // mark played / unplayed
            pod_episode_t *e = &eps[CUR.sel];
            if (e->flags & POD_PLAYED) {
                podcasts_set_flags(feeds[open_feed].id, e->id, 0, POD_PLAYED);
            } else {
                podcasts_set_flags(feeds[open_feed].id, e->id, POD_PLAYED, 0);
            }
            load_view();
            draw(false);
        } else if (pod_active()) {
            push(V_PLAYING, 0);
            draw(false);
        }
        return true;
    case BUTTON_WAKE:
        if (CUR.view == V_ROOT || CUR.view == V_FEED) {
            load_view();                                    // a download finished, an episode ended...
        }
        draw(false);
        return true;
    case BUTTON_BACK:
        if (depth == 0) {
            return false;                                   // leave; playback and downloads carry on
        }
        pop();
        draw(false);
        return true;
    default:
        return true;
    }
}

static bool playing_button(button_event_t event)
{
    switch (event) {
    case BUTTON_LEFT:
    case BUTTON_RIGHT:
        if (seek_mode) {
            player_seek(event == BUTTON_LEFT ? -seek_back_ms : seek_fwd_ms);
        } else {
            audio_set_volume(audio_volume() + (event == BUTTON_LEFT ? -1 : 1));
            settings_set_i32("music_vol", audio_volume());  // one volume for Music and Podcasts
        }
        draw(false);
        return true;
    case BUTTON_LEFT_HOLD:
        save_place();
        player_prev();
        return true;
    case BUTTON_RIGHT_HOLD:
        save_place();
        player_next();
        return true;
    case BUTTON_SELECT:
        if (pod_active()) {
            player_toggle_pause();
            save_place();
        }
        return true;
    case BUTTON_SELECT_HOLD:
        seek_mode = !seek_mode;
        draw(false);
        return true;
    case BUTTON_WAKE:
        if (pod_active() && pdTICKS_TO_MS(xTaskGetTickCount() - last_save) > 5000) {
            save_place();
        }
        draw(false);
        return true;
    case BUTTON_BACK:
        pop();
        draw(false);
        return true;
    default:
        return true;
    }
}

static bool podcasts_button(const app_t *self, button_event_t event)
{
    switch (CUR.view) {
    case V_PLAYING:
        return playing_button(event);
    case V_MESSAGE:
        if (event == BUTTON_BACK) {
            return false;
        }
        if (event == BUTTON_SELECT) {
            podcasts_open(self);
        }
        return true;
    default:
        return list_button(event);
    }
}

static TickType_t podcasts_next_wakeup(const app_t *self)
{
    if (CUR.view != V_PLAYING) {
        return portMAX_DELAY;
    }
    TickType_t wait = media_ui_marquee_wait();              // long lines scroll, even when paused
    if (pod_active()) {
        player_status_t ps;
        player_status(&ps);
        if (!ps.paused) {
            TickType_t t = app_ticks_until(next_tick);
            wait = t < wait ? t : wait;
        }
    }
    return wait;
}

static void podcasts_tick(const app_t *self)
{
    if (CUR.view != V_PLAYING) {
        return;
    }
    bool scroll = media_ui_marquee_step();
    if (scroll || (pod_active() && app_due(next_tick))) {
        draw(false);
    }
}

// Every 30 s, on screen or not: keep the listening position safe
static void podcasts_background(const app_t *self)
{
    if (pod_active() && pdTICKS_TO_MS(xTaskGetTickCount() - last_save) >= SAVE_EVERY_MS) {
        save_place();
    }
}

static void podcasts_close(const app_t *self)
{
    save_place();
    ESP_LOGI(TAG, "left the app; playback %s", pod_active() ? "continues" : "stopped");
}

// Menu icon: a microphone with sound waves
// Thick arcs (3 px) of radius r around (cx, cy), on the left and right only: the parts within
// about 50 degrees of horizontal, like radio waves going out both ways
static void wave_arcs(int cx, int cy, int r)
{
    for (int dy = -r - 3; dy <= r + 3; dy++) {
        for (int dx = -r - 3; dx <= r + 3; dx++) {
            int d2 = dx * dx + dy * dy;
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (d2 >= r * r && d2 <= (r + 3) * (r + 3) && ay * 5 <= ax * 6) {   // |dy| <= 1.2 |dx|
                epd_draw_pixel(cx + dx, cy + dy, EPD_BLACK);
            }
        }
    }
}

// Menu icon: a broadcast mark (a dot sending waves both ways, on a small stand): unlike
// the Voice app's microphone
static void podcasts_icon(const app_t *self, int cx, int cy)
{
    int my = cy - 4;                                        // centre of the waves
    epd_fill_circle(cx, my, 4, EPD_BLACK);
    wave_arcs(cx, my, 8);
    wave_arcs(cx, my, 15);
    for (int t = -1; t <= 1; t++) {                         // stand: a stem widening to a base
        epd_draw_line(cx + t, my + 4, cx + t, cy + 13, EPD_BLACK);
    }
    epd_draw_line(cx - 2, cy + 9, cx - 6, cy + 14, EPD_BLACK);
    epd_draw_line(cx + 2, cy + 9, cx + 6, cy + 14, EPD_BLACK);
    epd_draw_rect(cx - 7, cy + 14, 15, 3, EPD_BLACK, true);
}

const app_t podcasts_app = {
    .id = "podcasts",
    .name = "Podcasts",
    .draw_icon = podcasts_icon,
    .init = podcasts_init,
    .open = podcasts_open,
    .button = podcasts_button,
    .next_wakeup = podcasts_next_wakeup,
    .tick = podcasts_tick,
    .background = podcasts_background,
    .close = podcasts_close,
    .select_hold = true,
};
