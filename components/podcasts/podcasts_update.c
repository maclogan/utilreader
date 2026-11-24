// The updater: refreshes every feed, applies the keep rules, downloads episodes. Runs on its
// own task (downloads take minutes; the shared worker would be blocked).
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "podcasts.h"
#include "podcasts_internal.h"
#include "rss.h"
#include "sdcard.h"
#include "wifi.h"

static const char *TAG = "pod_update";

#define MIN_INTERVAL_MS     (5 * 60 * 1000)     // opening the app again soon doesn't refetch
#define MIN_FREE_BYTES      (200ULL * 1024 * 1024)
#define DL_BUF              (16 * 1024)
#define NEWEST_STOP_AFTER   50                  // newest-N feeds: items read when newest-first
#define MAX_TODO            100
#define MAX_REDIRECTS       8

static SemaphoreHandle_t status_lock;
static pod_update_status_t st;
static TaskHandle_t task;
static volatile bool stop_requested;
static volatile bool rerun;              // asked for again while running: go round once more
static TickType_t last_start;
static bool started_once;
static void (*notify_cb)(void);

// ---------- status ----------

static void st_lock(void)
{
    if (status_lock == NULL) {
        status_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(status_lock, portMAX_DELAY);
}

static void st_unlock(void)
{
    xSemaphoreGive(status_lock);
}

static void notify(void)
{
    if (notify_cb) {
        notify_cb();
    }
}

void podcasts_update_status(pod_update_status_t *out)
{
    st_lock();
    *out = st;
    st_unlock();
}

static void set_message(const char *m)
{
    st_lock();
    snprintf(st.message, sizeof(st.message), "%s", m);
    st_unlock();
    ESP_LOGW(TAG, "%s", m);
}

// ---------- HTTP ----------

typedef struct {
    char etag[100];
    char last_modified[40];
} hdrs_t;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    hdrs_t *h = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h) {
        if (!strcasecmp(e->header_key, "ETag")) {
            snprintf(h->etag, sizeof(h->etag), "%s", e->header_value);
        } else if (!strcasecmp(e->header_key, "Last-Modified")) {
            snprintf(h->last_modified, sizeof(h->last_modified), "%s", e->header_value);
        }
    }
    return ESP_OK;
}

// Opens url, following redirects (enclosures usually go through a tracker or two). Returns a
// client ready to read, with *status and *length (-1 unknown) set; NULL on a network error.
static esp_http_client_handle_t http_open(const char *url, const char *range, const char *etag,
                                          const char *last_modified, hdrs_t *h, int *status, int64_t *length)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_event,
        .user_data = h,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = true,
        .user_agent = "utilreader-podcasts/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return NULL;
    }
    for (int hop = 0; hop <= MAX_REDIRECTS && !stop_requested; hop++) {
        if (range) {
            esp_http_client_set_header(c, "Range", range);
        }
        if (etag && etag[0]) {
            esp_http_client_set_header(c, "If-None-Match", etag);
        }
        if (last_modified && last_modified[0]) {
            esp_http_client_set_header(c, "If-Modified-Since", last_modified);
        }
        if (esp_http_client_open(c, 0) != ESP_OK) {
            break;
        }
        *length = esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (*status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308) {
            esp_http_client_close(c);
            if (esp_http_client_set_redirection(c) != ESP_OK) {
                break;
            }
            continue;
        }
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

static void http_close(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

// ---------- feeds ----------

typedef struct {
    rss_item_t *items;
    int n, max;
    char title[96];
    char image[400];                    // the show's artwork, "" if none
    bool stop_early;                    // newest-N feed: enough items once newest-first is clear
    bool ascending;                     // an item newer than the one before: oldest-first feed
} parse_ctx_t;

static void on_channel_title(void *arg, const char *title)
{
    parse_ctx_t *p = arg;
    snprintf(p->title, sizeof(p->title), "%s", title);
}

static void on_channel_image(void *arg, const char *url)
{
    parse_ctx_t *p = arg;
    snprintf(p->image, sizeof(p->image), "%s", url);
}

// The show's artwork as <feed>/cover.jpg, which Now Playing shows for its episodes (as it
// does a music folder's cover.jpg). Fetched once per artwork URL (remembered in cover.url).
// Only JPEGs are kept: the device can't decode PNGs.
#define COVER_MAX (4 * 1024 * 1024)

static void fetch_cover(const char *feed_id, const char *url)
{
    char mark[224], tmp[224], dest[224], old[400] = "";
    snprintf(mark, sizeof(mark), "%s/%s/cover.url", podcasts_root(), feed_id);
    snprintf(tmp, sizeof(tmp), "%s/%s/cover.tmp", podcasts_root(), feed_id);
    snprintf(dest, sizeof(dest), "%s/%s/cover.jpg", podcasts_root(), feed_id);
    FILE *f = fopen(mark, "r");
    if (f) {
        if (fgets(old, sizeof(old), f) == NULL) {
            old[0] = '\0';
        }
        fclose(f);
    }
    if (!strcmp(old, url)) {
        return;                                             // already have it (or know it's no use)
    }
    if (!strcmp(url, "-")) {                                // the feed has no artwork: remember that
        f = fopen(mark, "w");
        if (f) {
            fputs(url, f);
            fclose(f);
        }
        return;
    }
    hdrs_t h = {0};
    int status = 0;
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(url, NULL, NULL, NULL, &h, &status, &len);
    if (c == NULL) {
        return;                                             // network: try again next update
    }
    bool jpeg = false;
    if (status == 200 && len <= COVER_MAX) {
        char *buf = malloc(4096);
        FILE *out = buf ? fopen(tmp, "wb") : NULL;
        int r, total = 0;
        while (out && !stop_requested && total < COVER_MAX && (r = esp_http_client_read(c, buf, 4096)) > 0) {
            if (total == 0) {
                jpeg = r >= 3 && (uint8_t)buf[0] == 0xFF && (uint8_t)buf[1] == 0xD8 && (uint8_t)buf[2] == 0xFF;
            }
            if (fwrite(buf, 1, r, out) != (size_t)r) {
                jpeg = false;                               // card full
                break;
            }
            total += r;
        }
        if (out) {
            fclose(out);
        }
        free(buf);
        if (stop_requested) {
            jpeg = false;
            status = 0;                                     // incomplete: try again next time
        }
    }
    http_close(c);
    if (jpeg) {
        remove(dest);
        jpeg = rename(tmp, dest) == 0;
    }
    remove(tmp);
    if (status == 200) {
        f = fopen(mark, "w");
        if (f) {
            fputs(url, f);
            fclose(f);
        }
    }
    ESP_LOGI(TAG, "%s: artwork %s", feed_id, jpeg ? "saved" : "not usable (not a JPEG?)");
}

static bool on_item(void *arg, const rss_item_t *it)
{
    parse_ctx_t *p = arg;
    if (p->n > 0 && it->date > p->items[p->n - 1].date) {
        p->ascending = true;
    }
    if (p->n < p->max) {
        p->items[p->n++] = *it;
    } else {
        // Full: keep the newest. Replace the oldest held if this one is newer.
        int oldest = 0;
        for (int i = 1; i < p->n; i++) {
            if (p->items[i].date < p->items[oldest].date) {
                oldest = i;
            }
        }
        if (it->date > p->items[oldest].date) {
            p->items[oldest] = *it;
        }
    }
    return !(p->stop_early && !p->ascending && p->n >= NEWEST_STOP_AFTER) && !stop_requested;
}

static void ext_for(const char *type, const char *url, char out[6])
{
    const char *e = "mp3";
    if (strstr(type, "mp4") || strstr(type, "m4a") || strstr(type, "aac")) e = "m4a";
    else if (strstr(type, "ogg") || strstr(type, "opus")) e = "ogg";
    else if (strstr(type, "wav")) e = "wav";
    else if (strstr(type, "flac")) e = "flac";
    if (!type[0]) {                                         // no type: the URL's extension
        const char *q = strchr(url, '?');
        size_t n = q ? (size_t)(q - url) : strlen(url);
        if (n > 4 && !strncasecmp(url + n - 4, ".m4a", 4)) e = "m4a";
        else if (n > 4 && !strncasecmp(url + n - 4, ".ogg", 4)) e = "ogg";
        else if (n > 5 && !strncasecmp(url + n - 5, ".opus", 5)) e = "ogg";
    }
    snprintf(out, 6, "%s", e);
}

static int cmp_ep_date(const void *a, const void *b)
{
    int64_t x = ((const pod_episode_t *)a)->date, y = ((const pod_episode_t *)b)->date;
    return x < y ? 1 : x > y ? -1 : 0;
}

// Merges freshly parsed items into the feed's episode list (under the store lock)
static int merge(const char *feed_id, const rss_item_t *items, int count)
{
    pod_episode_t *eps = pod_alloc((PODCASTS_MAX_EPISODES + 64) * sizeof(pod_episode_t));
    if (eps == NULL) {
        return 0;
    }
    pod_lock();
    int n = podcasts_episodes(feed_id, eps, PODCASTS_MAX_EPISODES);
    int added = 0;
    for (int i = 0; i < count; i++) {
        const rss_item_t *it = &items[i];
        char id[17];
        pod_episode_id(it->guid[0] ? it->guid : it->url, id);
        pod_episode_t *e = NULL;
        for (int k = 0; k < n && !e; k++) {
            if (!strcmp(eps[k].id, id)) {
                e = &eps[k];
            }
        }
        if (e == NULL) {
            if (n >= PODCASTS_MAX_EPISODES + 64) {
                continue;
            }
            e = &eps[n++];
            memset(e, 0, sizeof(*e));
            snprintf(e->id, sizeof(e->id), "%s", id);
            ext_for(it->type, it->url, e->ext);
            added++;
        }
        snprintf(e->title, sizeof(e->title), "%s", it->title[0] ? it->title : "(untitled)");
        e->date = it->date;
        e->length = it->length;
        e->duration_s = it->duration_s;
        if (!(e->flags & POD_DOWNLOADED)) {
            snprintf(e->url, sizeof(e->url), "%s", it->url);   // trackers change; keep the latest
        }
    }
    qsort(eps, n, sizeof(pod_episode_t), cmp_ep_date);
    // Over the limit: drop the oldest that aren't on the card or picked
    while (n > PODCASTS_MAX_EPISODES) {
        int drop = -1;
        for (int k = n - 1; k >= 0 && drop < 0; k--) {
            if (!(eps[k].flags & (POD_DOWNLOADED | POD_WANTED))) {
                drop = k;
            }
        }
        if (drop < 0) {
            drop = n - 1;
        }
        memmove(&eps[drop], &eps[drop + 1], (n - drop - 1) * sizeof(pod_episode_t));
        n--;
    }
    podcasts_save_episodes(feed_id, eps, n);
    pod_unlock();
    free(eps);
    return added;
}

static void refresh_feed(const pod_feed_t *feed)
{
    pod_options_t o = podcasts_effective(&feed->opt);
    parse_ctx_t ctx = {.max = PODCASTS_MAX_EPISODES, .stop_early = o.keep_mode == POD_KEEP_NEWEST};
    ctx.items = pod_alloc(ctx.max * sizeof(rss_item_t));
    char *buf = malloc(4096);
    hdrs_t h = {0};
    char error[64] = "";
    bool changed = false;
    int status = 0;
    int64_t len = 0;
    if (ctx.items == NULL || buf == NULL) {
        snprintf(error, sizeof(error), "Out of memory");
    } else {
        // No artwork looked for yet (feeds added before artwork support): fetch in full once
        char mark[224];
        struct stat sb;
        snprintf(mark, sizeof(mark), "%s/%s/cover.url", podcasts_root(), feed->id);
        bool fresh = stat(mark, &sb) != 0;
        esp_http_client_handle_t c = http_open(feed->url, NULL, fresh ? NULL : feed->etag,
                                               fresh ? NULL : feed->last_modified, &h, &status, &len);
        if (c == NULL) {
            snprintf(error, sizeof(error), "Can't reach the feed");
        } else if (status == 304) {
            http_close(c);                                  // unchanged since last time
        } else if (status != 200) {
            snprintf(error, sizeof(error), "Feed answered HTTP %d", status);
            http_close(c);
        } else {
            rss_callbacks_t cb = {.channel_title = on_channel_title, .channel_image = on_channel_image,
                                 .item = on_item, .ctx = &ctx};
            rss_parser_t *p = rss_new(&cb);
            int r;
            while (p && (r = esp_http_client_read(c, buf, 4096)) > 0 && rss_feed(p, buf, r)) {
            }
            rss_free(p);
            http_close(c);
            changed = true;
            if (ctx.n == 0) {
                snprintf(error, sizeof(error), "No episodes found (is it a podcast feed?)");
            }
        }
    }
    int added = changed && ctx.n ? merge(feed->id, ctx.items, ctx.n) : 0;
    if (changed && ctx.n) {
        fetch_cover(feed->id, ctx.image[0] ? ctx.image : "-");  // merge() made the feed's folder
    }
    ESP_LOGI(TAG, "%s: HTTP %d, %d items read, %d new", feed->title[0] ? feed->title : feed->url, status, ctx.n, added);

    // Write back what was learned, onto the latest copy (the web page may have edited it)
    pod_feed_t cur;
    if (podcasts_feed(feed->id, &cur)) {
        if (ctx.title[0]) {
            snprintf(cur.title, sizeof(cur.title), "%s", ctx.title);
        }
        if (changed) {
            snprintf(cur.etag, sizeof(cur.etag), "%s", h.etag);
            snprintf(cur.last_modified, sizeof(cur.last_modified), "%s", h.last_modified);
        }
        if (!error[0]) {
            cur.last_update = time(NULL);
        }
        snprintf(cur.error, sizeof(cur.error), "%s", error);
        podcasts_update_feed(&cur);
    }
    free(ctx.items);
    free(buf);
}

// ---------- downloads ----------

typedef struct {
    char feed[9];
    char ep[17];
} todo_t;

static bool card_has_room(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SDCARD_ROOT, &total, &free_bytes) != ESP_OK) {
        return true;                                        // can't tell: try
    }
    return free_bytes >= MIN_FREE_BYTES;
}

// Downloads one episode, resuming a .part file if the server allows. True when complete.
static bool download(const char *feed_id, pod_episode_t *e)
{
    char part[224], done[224], range[40];
    podcasts_episode_path(feed_id, e, true, part, sizeof(part));
    podcasts_episode_path(feed_id, e, false, done, sizeof(done));
    struct stat sb;
    uint32_t have = stat(part, &sb) == 0 ? (uint32_t)sb.st_size : 0;
    snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)have);

    hdrs_t h = {0};
    int status = 0;
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(e->url, have ? range : NULL, NULL, NULL, &h, &status, &len);
    if (c == NULL) {
        set_message("Can't reach the episode");
        return false;
    }
    if (status == 416 && have > 0) {                        // nothing left to fetch: it's complete
        http_close(c);
        remove(done);
        return rename(part, done) == 0;
    }
    if (status != 200 && status != 206) {
        char m[48];
        snprintf(m, sizeof(m), "Episode answered HTTP %d", status);
        set_message(m);
        http_close(c);
        return false;
    }
    if (status == 200) {
        have = 0;                                           // no resume: from the start
    }
    // The buffer: internal DMA memory makes card writes fastest, but Wi-Fi and TLS use most of
    // it, so fall back to PSRAM (the SD driver copies through a small bounce buffer then)
    uint8_t *buf = heap_caps_malloc(DL_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        buf = heap_caps_malloc(DL_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        http_close(c);
        ESP_LOGW(TAG, "no memory for the download buffer (%d bytes)", DL_BUF);
        set_message("Out of memory for the download");
        return false;
    }
    FILE *f = fopen(part, have ? "ab" : "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "can't open %s: %s", part, strerror(errno));
        if (!sdcard_mount()) {
            set_message("No card (or it can't be read)");
        } else {
            char m[64];
            snprintf(m, sizeof(m), "Can't write to the card: %s", strerror(errno));
            set_message(m);
        }
        free(buf);
        http_close(c);
        return false;
    }
    uint64_t total = len > 0 ? have + (uint64_t)len : e->length;
    uint64_t got = have;
    bool ok = true;
    TickType_t last_note = 0;
    int r;
    while ((r = esp_http_client_read(c, (char *)buf, DL_BUF)) > 0) {
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            set_message("Card full or write error");
            ok = false;
            break;
        }
        got += r;
        if (stop_requested) {
            ok = false;
            break;
        }
        if (xTaskGetTickCount() - last_note > pdMS_TO_TICKS(2000)) {
            last_note = xTaskGetTickCount();
            st_lock();
            st.percent = total ? (int)(got * 100 / total) : -1;
            st_unlock();
            notify();
        }
    }
    if (r < 0) {
        ok = false;
        set_message("Download interrupted (will resume)");
    }
    bool complete = ok && (len <= 0 || esp_http_client_is_complete_data_received(c));
    fclose(f);
    free(buf);
    http_close(c);
    if (!complete) {
        return false;                                       // the .part stays for next time
    }
    remove(done);
    return rename(part, done) == 0;
}

// ---------- the task ----------

static void update_once(void);

static void update_task(void *arg)
{
    do {
        rerun = false;
        update_once();
    } while (rerun && !stop_requested);
    st_lock();
    st.running = false;
    st.downloading = 0;
    st.percent = -1;
    st.current[0] = '\0';
    st_unlock();
    ESP_LOGI(TAG, "update finished");
    notify();
    task = NULL;
    vTaskDelete(NULL);
}

static void update_once(void)
{
    pod_feed_t *feeds = pod_alloc(PODCASTS_MAX_FEEDS * sizeof(pod_feed_t));
    pod_episode_t *eps = pod_alloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t));
    todo_t *todo = pod_alloc(MAX_TODO * sizeof(todo_t));
    char ip[20];
    int nf = 0;

    if (!feeds || !eps || !todo) {
        set_message("Out of memory");
    } else if (!wifi_ip_string(ip, sizeof(ip))) {
        st_lock();
        st.offline = true;
        st_unlock();
    } else if (!sdcard_mount()) {
        set_message("No microSD card");
    } else {
        // 1. Feeds
        nf = podcasts_feeds(feeds, PODCASTS_MAX_FEEDS);
        for (int i = 0; i < nf && !stop_requested; i++) {
            st_lock();
            st.feed = i + 1;
            st.feeds = nf;
            snprintf(st.current, sizeof(st.current), "%.120s", feeds[i].title[0] ? feeds[i].title : feeds[i].url);
            st_unlock();
            notify();
            refresh_feed(&feeds[i]);
        }

        // 2. Rules: delete what's no longer wanted, list what's missing
        int nt = 0;
        nf = podcasts_feeds(feeds, PODCASTS_MAX_FEEDS);
        for (int i = 0; i < nf && !stop_requested; i++) {
            if (podcasts_apply_rules(feeds[i].id) == 0) {
                continue;
            }
            pod_options_t o = podcasts_effective(&feeds[i].opt);
            int n = podcasts_episodes(feeds[i].id, eps, PODCASTS_MAX_EPISODES);
            for (int k = 0; k < n && nt < MAX_TODO; k++) {
                if (pod_should_keep(&o, k, &eps[k]) && !(eps[k].flags & POD_DOWNLOADED) && eps[k].url[0]) {
                    snprintf(todo[nt].feed, sizeof(todo[nt].feed), "%s", feeds[i].id);
                    snprintf(todo[nt].ep, sizeof(todo[nt].ep), "%s", eps[k].id);
                    nt++;
                }
            }
        }

        // 3. Downloads
        st_lock();
        st.feed = st.feeds = 0;
        st_unlock();
        for (int i = 0; i < nt && !stop_requested; i++) {
            pod_episode_t e;
            if (!podcasts_episode(todo[i].feed, todo[i].ep, &e)) {
                continue;
            }
            if (!card_has_room()) {
                set_message("Card full: downloads stopped");
                break;
            }
            st_lock();
            st.downloading = nt - i;
            st.percent = 0;
            snprintf(st.current, sizeof(st.current), "%.120s", e.title);
            st_unlock();
            notify();
            ESP_LOGI(TAG, "downloading %s", e.title);
            if (download(todo[i].feed, &e)) {
                pod_mark_downloaded(todo[i].feed, e.id, e.ext);
                notify();
            }
        }
    }

    free(feeds);
    free(eps);
    free(todo);
}

void podcasts_update_start(bool force, void (*notify_fn)(void))
{
    st_lock();
    bool busy = st.running;
    bool recent = started_once && xTaskGetTickCount() - last_start < pdMS_TO_TICKS(MIN_INTERVAL_MS);
    if (busy) {
        rerun = rerun || force;                             // feeds changed meanwhile: check again after
        st_unlock();
        return;
    }
    if (recent && !force) {
        st_unlock();
        return;
    }
    if (notify_fn) {
        notify_cb = notify_fn;
    }
    memset(&st, 0, sizeof(st));
    st.running = true;
    st.percent = -1;
    last_start = xTaskGetTickCount();
    started_once = true;
    stop_requested = false;
    st_unlock();
    if (xTaskCreate(update_task, "pod_update", 8192, NULL, 4, &task) != pdPASS) {
        st_lock();
        st.running = false;
        st_unlock();
    }
}

void podcasts_update_stop(void)
{
    stop_requested = true;
    for (int i = 0; i < 100 && task != NULL; i++) {         // up to 5 s for the current read
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
