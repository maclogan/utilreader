#include "podcasts.h"
#include "podcasts_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>                     // rmdir()
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdcard.h"
#include "settings.h"

static const char *TAG = "podcasts";

#define FEEDS_FILE      PODCASTS_ROOT "/feeds.tsv"
#define FEEDS_MAGIC     "PODFEEDS 1"
#define EPS_MAGIC       "PODEP 1"
#define LINE_MAX_       1400

static SemaphoreHandle_t mutex;
static char playing_feed[9], playing_ep[17];

static void lock(void)
{
    if (mutex == NULL) {
        mutex = xSemaphoreCreateRecursiveMutex();           // first call: main task at start-up
    }
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(mutex);
}

void *pod_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

// Copies src into dst[size] with tabs and line breaks turned into spaces (the files are TSV)
static void clean_copy(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < size; i++) {
        char c = src[i];
        dst[i] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[i] = '\0';
}

// Splits a line at tabs in place: fields[max]. Returns the count.
static int split(char *line, char **fields, int max)
{
    int n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    while (n < max) {
        fields[n++] = line;
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        line = tab + 1;
    }
    return n;
}

static bool ready(void)
{
    if (!sdcard_mount()) {
        return false;
    }
    mkdir(PODCASTS_ROOT, 0777);
    return true;
}

// ---------- defaults ----------

void podcasts_defaults(pod_options_t *opt, int *seek_back_s, int *seek_fwd_s)
{
    opt->keep_mode = settings_get_i32("pod_keepmode", POD_KEEP_NEWEST);
    opt->keep = settings_get_i32("pod_keep", 3);
    opt->delete_played = settings_get_i32("pod_delete", 1);
    opt->after_next = settings_get_i32("pod_after", 1);
    opt->oldest_first = settings_get_i32("pod_order", 0);
    if (seek_back_s) {
        *seek_back_s = settings_get_i32("pod_seekback", 10);
    }
    if (seek_fwd_s) {
        *seek_fwd_s = settings_get_i32("pod_seekfwd", 30);
    }
}

void podcasts_set_defaults(const pod_options_t *opt, int seek_back_s, int seek_fwd_s)
{
    settings_set_i32("pod_keepmode", opt->keep_mode == POD_KEEP_CUSTOM ? POD_KEEP_CUSTOM : POD_KEEP_NEWEST);
    settings_set_i32("pod_keep", opt->keep < 1 ? 1 : opt->keep > 50 ? 50 : opt->keep);
    settings_set_i32("pod_delete", opt->delete_played ? 1 : 0);
    settings_set_i32("pod_after", opt->after_next ? 1 : 0);
    settings_set_i32("pod_order", opt->oldest_first ? 1 : 0);
    settings_set_i32("pod_seekback", seek_back_s < 1 ? 1 : seek_back_s > 600 ? 600 : seek_back_s);
    settings_set_i32("pod_seekfwd", seek_fwd_s < 1 ? 1 : seek_fwd_s > 600 ? 600 : seek_fwd_s);
}

pod_options_t podcasts_effective(const pod_options_t *f)
{
    pod_options_t d;
    podcasts_defaults(&d, NULL, NULL);
    if (f) {
        if (f->keep_mode >= 0) d.keep_mode = f->keep_mode;
        if (f->keep >= 1) d.keep = f->keep;
        if (f->delete_played >= 0) d.delete_played = f->delete_played;
        if (f->after_next >= 0) d.after_next = f->after_next;
        if (f->oldest_first >= 0) d.oldest_first = f->oldest_first;
    }
    return d;
}

// ---------- feeds ----------

// FNV-1a: stable short ids from URLs and guids
static uint64_t fnv64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
    }
    return h;
}

void pod_episode_id(const char *guid, char out[17])
{
    snprintf(out, 17, "%016llx", (unsigned long long)fnv64(guid));
}

static int load_feeds(pod_feed_t *out, int max)
{
    FILE *f = fopen(FEEDS_FILE, "r");
    if (f == NULL) {
        return 0;
    }
    char *line = pod_alloc(LINE_MAX_);
    int n = 0;
    if (line && fgets(line, LINE_MAX_, f) && !strncmp(line, FEEDS_MAGIC, strlen(FEEDS_MAGIC))) {
        while (n < max && fgets(line, LINE_MAX_, f)) {
            char *v[12];
            if (split(line, v, 12) < 12) {
                continue;
            }
            pod_feed_t *d = &out[n++];
            memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "%s", v[0]);
            d->opt = (pod_options_t){atoi(v[1]), atoi(v[2]), atoi(v[3]), atoi(v[4]), atoi(v[5])};
            d->last_update = atoll(v[6]);
            clean_copy(d->url, sizeof(d->url), v[7]);
            clean_copy(d->title, sizeof(d->title), v[8]);
            clean_copy(d->etag, sizeof(d->etag), v[9]);
            clean_copy(d->last_modified, sizeof(d->last_modified), v[10]);
            clean_copy(d->error, sizeof(d->error), v[11]);
        }
    }
    free(line);
    fclose(f);
    return n;
}

static bool save_feeds(const pod_feed_t *feeds, int n)
{
    FILE *f = fopen(FEEDS_FILE ".tmp", "w");
    if (f == NULL) {
        return false;
    }
    fprintf(f, "%s\n", FEEDS_MAGIC);
    for (int i = 0; i < n; i++) {
        const pod_feed_t *d = &feeds[i];
        fprintf(f, "%s\t%d\t%d\t%d\t%d\t%d\t%lld\t%s\t%s\t%s\t%s\t%s\n", d->id, d->opt.keep_mode, d->opt.keep,
                d->opt.delete_played, d->opt.after_next, d->opt.oldest_first, (long long)d->last_update,
                d->url, d->title, d->etag, d->last_modified, d->error);
    }
    if (fclose(f) != 0) {
        return false;
    }
    remove(FEEDS_FILE);
    return rename(FEEDS_FILE ".tmp", FEEDS_FILE) == 0;
}

static int cmp_feed(const void *a, const void *b)
{
    const pod_feed_t *x = a, *y = b;
    return strcasecmp(x->title[0] ? x->title : x->url, y->title[0] ? y->title : y->url);
}

int podcasts_feeds(pod_feed_t *out, int max)
{
    lock();
    int n = ready() ? load_feeds(out, max) : 0;
    unlock();
    qsort(out, n, sizeof(pod_feed_t), cmp_feed);
    return n;
}

bool podcasts_feed(const char *id, pod_feed_t *out)
{
    lock();
    pod_feed_t *all = pod_alloc(PODCASTS_MAX_FEEDS * sizeof(pod_feed_t));
    bool found = false;
    int n = all && ready() ? load_feeds(all, PODCASTS_MAX_FEEDS) : 0;
    for (int i = 0; i < n && !found; i++) {
        if (!strcmp(all[i].id, id)) {
            *out = all[i];
            found = true;
        }
    }
    free(all);
    unlock();
    return found;
}

bool podcasts_add_feed(const char *url, char *id_out)
{
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) {
        return false;
    }
    lock();
    pod_feed_t *all = pod_alloc((PODCASTS_MAX_FEEDS + 1) * sizeof(pod_feed_t));
    bool ok = false;
    if (all && ready()) {
        int n = load_feeds(all, PODCASTS_MAX_FEEDS);
        char id[9];
        snprintf(id, sizeof(id), "%08lx", (unsigned long)(fnv64(url) & 0xffffffff));
        bool dup = false;
        for (int i = 0; i < n; i++) {
            dup |= !strcmp(all[i].id, id);
        }
        if (!dup && n < PODCASTS_MAX_FEEDS) {
            pod_feed_t *d = &all[n++];
            memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "%s", id);
            clean_copy(d->url, sizeof(d->url), url);
            d->opt = (pod_options_t){-1, -1, -1, -1, -1};
            ok = save_feeds(all, n);
            char dir[64];
            snprintf(dir, sizeof(dir), "%s/%s", PODCASTS_ROOT, id);
            mkdir(dir, 0777);
        }
        if (id_out) {
            snprintf(id_out, 9, "%s", id);
        }
    }
    free(all);
    unlock();
    return ok;
}

bool podcasts_update_feed(const pod_feed_t *feed)
{
    lock();
    pod_feed_t *all = pod_alloc(PODCASTS_MAX_FEEDS * sizeof(pod_feed_t));
    bool ok = false;
    int n = all && ready() ? load_feeds(all, PODCASTS_MAX_FEEDS) : 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(all[i].id, feed->id)) {
            all[i] = *feed;
            ok = save_feeds(all, n);
        }
    }
    free(all);
    unlock();
    return ok;
}

bool podcasts_remove_feed(const char *id)
{
    lock();
    pod_feed_t *all = pod_alloc(PODCASTS_MAX_FEEDS * sizeof(pod_feed_t));
    bool ok = false;
    int n = all && ready() ? load_feeds(all, PODCASTS_MAX_FEEDS) : 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].id, id)) {
            continue;
        }
        memmove(&all[i], &all[i + 1], (n - i - 1) * sizeof(pod_feed_t));
        ok = save_feeds(all, n - 1);
        // Its folder: every file, then the folder
        char dir[64], path[128];
        snprintf(dir, sizeof(dir), "%s/%s", PODCASTS_ROOT, id);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            snprintf(path, sizeof(path), "%s/%.40s", dir, e->d_name);
            remove(path);
        }
        if (d) {
            closedir(d);
        }
        rmdir(dir);
        break;
    }
    free(all);
    unlock();
    return ok;
}

// ---------- episodes ----------

static void eps_path(const char *feed_id, char *out, size_t size)
{
    snprintf(out, size, "%s/%s/episodes.tsv", PODCASTS_ROOT, feed_id);
}

static int cmp_ep_date(const void *a, const void *b)
{
    int64_t x = ((const pod_episode_t *)a)->date, y = ((const pod_episode_t *)b)->date;
    return x < y ? 1 : x > y ? -1 : 0;                      // newest first
}

static int load_eps(const char *feed_id, pod_episode_t *out, int max)
{
    char path[64];
    eps_path(feed_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    char *line = pod_alloc(LINE_MAX_);
    int n = 0;
    if (line && fgets(line, LINE_MAX_, f) && !strncmp(line, EPS_MAGIC, strlen(EPS_MAGIC))) {
        while (n < max && fgets(line, LINE_MAX_, f)) {
            char *v[9];
            if (split(line, v, 9) < 9) {
                continue;
            }
            pod_episode_t *e = &out[n++];
            memset(e, 0, sizeof(*e));
            snprintf(e->id, sizeof(e->id), "%s", v[0]);
            e->date = atoll(v[1]);
            e->length = strtoul(v[2], NULL, 10);
            e->duration_s = strtoul(v[3], NULL, 10);
            e->position_ms = strtoul(v[4], NULL, 10);
            e->flags = (uint8_t)atoi(v[5]);
            clean_copy(e->ext, sizeof(e->ext), v[6]);
            clean_copy(e->title, sizeof(e->title), v[7]);
            clean_copy(e->url, sizeof(e->url), v[8]);
        }
    }
    free(line);
    fclose(f);
    return n;
}

static bool save_eps(const char *feed_id, const pod_episode_t *eps, int n)
{
    char path[64], tmp[72], dir[48];
    snprintf(dir, sizeof(dir), "%s/%s", PODCASTS_ROOT, feed_id);
    mkdir(dir, 0777);
    eps_path(feed_id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return false;
    }
    fprintf(f, "%s\n", EPS_MAGIC);
    char title[128], url[512];
    for (int i = 0; i < n; i++) {
        const pod_episode_t *e = &eps[i];
        clean_copy(title, sizeof(title), e->title);
        clean_copy(url, sizeof(url), e->url);
        fprintf(f, "%s\t%lld\t%lu\t%lu\t%lu\t%u\t%s\t%s\t%s\n", e->id, (long long)e->date,
                (unsigned long)e->length, (unsigned long)e->duration_s, (unsigned long)e->position_ms,
                e->flags, e->ext, title, url);
    }
    if (fclose(f) != 0) {
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

int podcasts_episodes(const char *feed_id, pod_episode_t *out, int max)
{
    lock();
    int n = ready() ? load_eps(feed_id, out, max) : 0;
    unlock();
    qsort(out, n, sizeof(pod_episode_t), cmp_ep_date);
    return n;
}

bool podcasts_save_episodes(const char *feed_id, const pod_episode_t *eps, int n)
{
    lock();
    bool ok = ready() && save_eps(feed_id, eps, n);
    unlock();
    return ok;
}

bool podcasts_episode(const char *feed_id, const char *ep_id, pod_episode_t *out)
{
    lock();
    pod_episode_t *eps = pod_alloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t));
    bool found = false;
    int n = eps && ready() ? load_eps(feed_id, eps, PODCASTS_MAX_EPISODES) : 0;
    for (int i = 0; i < n && !found; i++) {
        if (!strcmp(eps[i].id, ep_id)) {
            *out = eps[i];
            found = true;
        }
    }
    free(eps);
    unlock();
    return found;
}

// Loads a feed's episodes, lets fn change episode ep_id (or all, if ep_id is NULL), saves
static void modify(const char *feed_id, const char *ep_id, void (*fn)(pod_episode_t *e, void *arg), void *arg)
{
    lock();
    pod_episode_t *eps = pod_alloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t));
    int n = eps && ready() ? load_eps(feed_id, eps, PODCASTS_MAX_EPISODES) : 0;
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (ep_id == NULL || !strcmp(eps[i].id, ep_id)) {
            fn(&eps[i], arg);
            changed = true;
        }
    }
    if (changed) {
        save_eps(feed_id, eps, n);
    }
    free(eps);
    unlock();
}

typedef struct {
    uint32_t pos;
    bool played;
} progress_t;

static void set_progress(pod_episode_t *e, void *arg)
{
    const progress_t *p = arg;
    e->position_ms = p->played ? 0 : p->pos;
    if (p->played) {
        e->flags |= POD_PLAYED;
    }
}

void podcasts_set_progress(const char *feed_id, const char *ep_id, uint32_t position_ms, bool played)
{
    progress_t p = {position_ms, played};
    modify(feed_id, ep_id, set_progress, &p);
}

typedef struct {
    uint8_t set, clear;
} flags_t;

static void set_flags(pod_episode_t *e, void *arg)
{
    const flags_t *f = arg;
    e->flags = (e->flags | f->set) & ~f->clear;
    if (f->clear & POD_PLAYED) {
        e->position_ms = 0;                                 // marked unplayed: from the start
    }
}

void podcasts_set_flags(const char *feed_id, const char *ep_id, uint8_t set, uint8_t clear)
{
    flags_t f = {set, clear};
    modify(feed_id, ep_id, set_flags, &f);
}

typedef struct {
    const char *const *ids;
    int n;
} wanted_t;

static void set_wanted(pod_episode_t *e, void *arg)
{
    const wanted_t *w = arg;
    bool want = false;
    for (int i = 0; i < w->n && !want; i++) {
        want = !strcmp(w->ids[i], e->id);
    }
    e->flags = want ? (e->flags | POD_WANTED) : (e->flags & ~POD_WANTED);
}

void podcasts_set_wanted(const char *feed_id, const char *const *ids, int n)
{
    wanted_t w = {ids, n};
    modify(feed_id, NULL, set_wanted, &w);
}

static void mark_downloaded(pod_episode_t *e, void *arg)
{
    snprintf(e->ext, sizeof(e->ext), "%s", (const char *)arg);
    e->flags |= POD_DOWNLOADED;
}

void pod_mark_downloaded(const char *feed_id, const char *ep_id, const char *ext)
{
    modify(feed_id, ep_id, mark_downloaded, (void *)ext);
}

void pod_lock(void) { lock(); }
void pod_unlock(void) { unlock(); }

void podcasts_episode_path(const char *feed_id, const pod_episode_t *ep, bool part, char *out, size_t size)
{
    snprintf(out, size, "%s/%s/%s.%s%s", PODCASTS_ROOT, feed_id, ep->id, ep->ext[0] ? ep->ext : "mp3",
             part ? ".part" : "");
}

void podcasts_set_playing(const char *feed_id, const char *ep_id)
{
    lock();
    snprintf(playing_feed, sizeof(playing_feed), "%s", feed_id ? feed_id : "");
    snprintf(playing_ep, sizeof(playing_ep), "%s", ep_id ? ep_id : "");
    unlock();
}

// ---------- the keep rules ----------

bool pod_should_keep(const pod_options_t *o, int i, const pod_episode_t *e)
{
    bool keep = o->keep_mode == POD_KEEP_CUSTOM ? (e->flags & POD_WANTED) != 0 : i < o->keep;
    return keep && !((e->flags & POD_PLAYED) && o->delete_played);
}

int podcasts_apply_rules(const char *feed_id)
{
    lock();
    pod_feed_t feed;
    pod_episode_t *eps = pod_alloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t));
    if (eps == NULL || !podcasts_feed(feed_id, &feed)) {
        free(eps);
        unlock();
        return 0;
    }
    pod_options_t o = podcasts_effective(&feed.opt);
    int n = load_eps(feed_id, eps, PODCASTS_MAX_EPISODES);
    qsort(eps, n, sizeof(pod_episode_t), cmp_ep_date);
    int missing = 0;
    bool changed = false;
    for (int i = 0; i < n; i++) {
        pod_episode_t *e = &eps[i];
        bool keep = pod_should_keep(&o, i, e);
        bool playing = !strcmp(playing_feed, feed_id) && !strcmp(playing_ep, e->id);
        if (!keep && (e->flags & POD_DOWNLOADED) && !playing) {
            char path[128];
            podcasts_episode_path(feed_id, e, false, path, sizeof(path));
            remove(path);
            podcasts_episode_path(feed_id, e, true, path, sizeof(path));
            remove(path);
            e->flags &= ~POD_DOWNLOADED;
            changed = true;
            ESP_LOGI(TAG, "%s: removed %s", feed_id, e->title);
        }
        if (keep && !(e->flags & POD_DOWNLOADED) && e->url[0]) {
            missing++;
        }
    }
    if (changed) {
        save_eps(feed_id, eps, n);
    }
    free(eps);
    unlock();
    return missing;
}
