#include "musicdb.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdcard.h"

static const char *TAG = "musicdb";

#define MAX_DEPTH       4
#define PATH_LEN        320
#define CACHE_FILE      SDCARD_ROOT "/.musicdb"
#define CACHE_MAGIC     "MUSICDB 2"
#define ARENA_BLOCK     (64 * 1024)

static const char UNKNOWN_ARTIST[] = "Unknown Artist";
static const char UNKNOWN_ALBUM[] = "Unknown Album";

static musicdb_track_t *tracks;
static int count;
static int *by_title;                   // track indices sorted by title
static int *artist_first;               // per artist: a track that names it
static int artist_count;
static int *album_first;                // per album: a track that names it
static int album_count;
static musicdb_status_t status = MUSICDB_NOT_SCANNED;
static char dir_scanned[96];
static char root[128];                  // "/sdcard/Music"
static uint32_t generation;
static SemaphoreHandle_t mutex;

// ---------- memory: strings live in PSRAM blocks freed together ----------

typedef struct block {
    struct block *next;
    size_t used;
    char data[];
} block_t;

static block_t *arena;

static void *ps_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static const char *arena_str(const char *s, size_t n)
{
    if (n == 0) {
        return "";
    }
    if (n + 1 > ARENA_BLOCK - sizeof(block_t)) {
        n = ARENA_BLOCK - sizeof(block_t) - 1;
    }
    if (arena == NULL || arena->used + n + 1 > ARENA_BLOCK - sizeof(block_t)) {
        block_t *b = ps_malloc(ARENA_BLOCK);
        if (b == NULL) {
            return "";
        }
        b->next = arena;
        b->used = 0;
        arena = b;
    }
    char *d = arena->data + arena->used;
    memcpy(d, s, n);
    d[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (d[i] == '\t' || d[i] == '\n' || d[i] == '\r') {
            d[i] = ' ';                                     // the cache is tab-separated
        }
    }
    arena->used += n + 1;
    return d;
}

static const char *astr(const char *s)
{
    return arena_str(s, strlen(s));
}

static void arena_free(void)
{
    while (arena) {
        block_t *n = arena->next;
        free(arena);
        arena = n;
    }
}

// ---------- lock ----------

void musicdb_lock(void)
{
    if (mutex == NULL) {
        mutex = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
}

void musicdb_unlock(void)
{
    xSemaphoreGiveRecursive(mutex);
}

// ---------- sorting ----------

// Sort key: "The Beatles" sorts as "Beatles"
static const char *sort_key(const char *s)
{
    return strncasecmp(s, "the ", 4) == 0 ? s + 4 : s;
}

static int cmp_text(const char *a, const char *b)
{
    int c = strcasecmp(sort_key(a), sort_key(b));
    return c ? c : strcmp(a, b);
}

static const char *group_artist(const musicdb_track_t *t)
{
    return t->album_artist[0] ? t->album_artist : t->artist[0] ? t->artist : UNKNOWN_ARTIST;
}

static const char *album_name(const musicdb_track_t *t)
{
    return t->album[0] ? t->album : UNKNOWN_ALBUM;
}

static int cmp_rel(const void *a, const void *b)
{
    return strcasecmp(((const musicdb_track_t *)a)->rel, ((const musicdb_track_t *)b)->rel);
}

static int cmp_title_idx(const void *a, const void *b)
{
    const musicdb_track_t *x = &tracks[*(const int *)a], *y = &tracks[*(const int *)b];
    int c = cmp_text(x->title, y->title);
    return c ? c : cmp_text(musicdb_artist_of(x - tracks), musicdb_artist_of(y - tracks));
}

static int cmp_artist_idx(const void *a, const void *b)
{
    return cmp_text(group_artist(&tracks[*(const int *)a]), group_artist(&tracks[*(const int *)b]));
}

static int cmp_album_idx(const void *a, const void *b)
{
    const musicdb_track_t *x = &tracks[*(const int *)a], *y = &tracks[*(const int *)b];
    int c = cmp_text(album_name(x), album_name(y));
    return c ? c : cmp_text(group_artist(x), group_artist(y));
}

// Within an album: disc, track, then file name
static int cmp_in_album(const musicdb_track_t *x, const musicdb_track_t *y)
{
    if (x->disc != y->disc) return (int)x->disc - (int)y->disc;
    if (x->track != y->track) return (int)x->track - (int)y->track;
    return strcasecmp(x->rel, y->rel);
}

static int cmp_album_track_idx(const void *a, const void *b)
{
    return cmp_in_album(&tracks[*(const int *)a], &tracks[*(const int *)b]);
}

static int cmp_artist_song_idx(const void *a, const void *b)
{
    const musicdb_track_t *x = &tracks[*(const int *)a], *y = &tracks[*(const int *)b];
    int c = cmp_text(album_name(x), album_name(y));
    return c ? c : cmp_in_album(x, y);
}

// ---------- the tag cache ----------
// Text, one track per line, tab-separated:
//   size  duration_ms  track  disc  format  rel  title  artist  album_artist  album

typedef struct {
    char *rel;
    char *fields;                       // the whole line (rel points into it)
} cache_entry_t;

static char *cache_buf;
static cache_entry_t *cache;
static int cache_count;

static int cmp_cache(const void *a, const void *b)
{
    return strcasecmp(((const cache_entry_t *)a)->rel, ((const cache_entry_t *)b)->rel);
}

static void cache_load(void)
{
    cache_count = 0;
    FILE *f = fopen(CACHE_FILE, "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024 || (cache_buf = ps_malloc(size + 1)) == NULL) {
        fclose(f);
        return;
    }
    size_t got = fread(cache_buf, 1, size, f);
    fclose(f);
    cache_buf[got] = '\0';

    char *line = cache_buf, *nl = strchr(line, '\n');
    char header[160];
    snprintf(header, sizeof(header), "%s\t%s", CACHE_MAGIC, dir_scanned);
    if (nl == NULL || (size_t)(nl - line) != strlen(header) || strncmp(line, header, nl - line) != 0) {
        return;                                             // another folder or format: ignore it
    }
    cache = ps_malloc(MUSICDB_MAX_TRACKS * sizeof(cache_entry_t));
    if (cache == NULL) {
        return;
    }
    for (line = nl + 1; *line && cache_count < MUSICDB_MAX_TRACKS; line = nl + 1) {
        nl = strchr(line, '\n');
        if (nl == NULL) {
            break;
        }
        *nl = '\0';
        // rel is the 6th field
        char *p = line;
        for (int i = 0; i < 5 && p; i++) {
            p = strchr(p, '\t');
            p = p ? p + 1 : NULL;
        }
        char *end = p ? strchr(p, '\t') : NULL;
        if (end == NULL) {
            continue;
        }
        *end = '\0';                                        // rel terminated; restored when parsed
        cache[cache_count++] = (cache_entry_t){.rel = p, .fields = line};
    }
    qsort(cache, cache_count, sizeof(cache_entry_t), cmp_cache);
}

static cache_entry_t *cache_find(const char *rel, uint32_t size)
{
    if (cache_count == 0) {
        return NULL;
    }
    cache_entry_t key = {.rel = (char *)rel};
    cache_entry_t *e = bsearch(&key, cache, cache_count, sizeof(cache_entry_t), cmp_cache);
    unsigned long sz;
    return e && sscanf(e->fields, "%lu", &sz) == 1 && sz == size ? e : NULL;
}

static bool cache_has(const char *rel, uint32_t size)
{
    return cache_find(rel, size) != NULL;
}

// Fills t from the cache if the file is there with the same size
static bool cache_lookup(const char *rel, uint32_t size, musicdb_track_t *t)
{
    if (cache_count == 0) {
        return false;
    }
    cache_entry_t key = {.rel = (char *)rel};
    cache_entry_t *e = bsearch(&key, cache, cache_count, sizeof(cache_entry_t), cmp_cache);
    if (e == NULL) {
        return false;
    }
    unsigned long sz, dur;
    unsigned tr, di, fmt;
    if (sscanf(e->fields, "%lu\t%lu\t%u\t%u\t%u", &sz, &dur, &tr, &di, &fmt) != 5 || sz != size) {
        return false;
    }
    char *f[4];                                             // after rel: title artist album_artist album
    char *p = e->rel + strlen(e->rel) + 1;
    for (int i = 0; i < 4; i++) {
        f[i] = p;
        char *tab = strchr(p, '\t');
        if (tab == NULL && i < 3) {
            return false;
        }
        p = tab ? tab + 1 : p + strlen(p);
    }
    #define FIELD_LEN(i) ((size_t)((i) < 3 ? strchr(f[i], '\t') - f[i] : strlen(f[i])))
    t->title = arena_str(f[0], FIELD_LEN(0));
    t->artist = arena_str(f[1], FIELD_LEN(1));
    t->album_artist = arena_str(f[2], FIELD_LEN(2));
    t->album = arena_str(f[3], FIELD_LEN(3));
    #undef FIELD_LEN
    t->duration_ms = dur;
    t->track = tr;
    t->disc = di;
    t->format = (media_format_t)fmt;
    return true;
}

static void cache_free(void)
{
    free(cache_buf);
    free(cache);
    cache_buf = NULL;
    cache = NULL;
    cache_count = 0;
}

static void cache_save(void)
{
    FILE *f = fopen(CACHE_FILE ".tmp", "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%s\t%s\n", CACHE_MAGIC, dir_scanned);
    for (int i = 0; i < count; i++) {
        const musicdb_track_t *t = &tracks[i];
        fprintf(f, "%lu\t%lu\t%u\t%u\t%u\t%s\t%s\t%s\t%s\t%s\n", (unsigned long)t->size,
                (unsigned long)t->duration_ms, t->track, t->disc, (unsigned)t->format,
                t->rel, t->title, t->artist, t->album_artist, t->album);
    }
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(CACHE_FILE);
        rename(CACHE_FILE ".tmp", CACHE_FILE);
    }
}

// ---------- scanning ----------

typedef struct {
    char *rel;
    uint32_t size;
} found_t;

static found_t *found;
static int found_count;

static void list_files(const char *rel, int depth)
{
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    DIR *d = opendir(path);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && found_count < MUSICDB_MAX_TRACKS) {
        if (e->d_name[0] == '.') {
            continue;                                       // hidden, macOS "._" files
        }
        char child[PATH_LEN];
        int n = snprintf(child, sizeof(child), "%s%s", rel, e->d_name);
        if (n <= 0 || n >= (int)sizeof(child) - 2) {
            continue;
        }
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH) {
                strcat(child, "/");
                list_files(child, depth + 1);
            }
        } else if (media_format_from_name(e->d_name) != MEDIA_UNKNOWN) {
            char full[PATH_LEN + 140];
            snprintf(full, sizeof(full), "%s/%s", root, child);
            struct stat st;
            found[found_count].size = stat(full, &st) == 0 ? (uint32_t)st.st_size : 0;
            found[found_count].rel = (char *)astr(child);
            found_count++;
        }
    }
    closedir(d);
}

static void build_indexes(void)
{
    int *tmp = ps_malloc((count ? count : 1) * sizeof(int));
    if (tmp == NULL) {
        return;
    }
    // Songs by title
    for (int i = 0; i < count; i++) {
        by_title[i] = i;
    }
    qsort(by_title, count, sizeof(int), cmp_title_idx);

    // Artists
    for (int i = 0; i < count; i++) {
        tmp[i] = i;
    }
    qsort(tmp, count, sizeof(int), cmp_artist_idx);
    artist_count = 0;
    for (int k = 0; k < count; k++) {
        if (k == 0 || strcasecmp(group_artist(&tracks[tmp[k]]), group_artist(&tracks[tmp[k - 1]])) != 0) {
            artist_first[artist_count++] = tmp[k];
        }
        tracks[tmp[k]].artist_id = artist_count - 1;
    }

    // Albums (same name by two artists = two albums)
    for (int i = 0; i < count; i++) {
        tmp[i] = i;
    }
    qsort(tmp, count, sizeof(int), cmp_album_idx);
    album_count = 0;
    for (int k = 0; k < count; k++) {
        const musicdb_track_t *a = &tracks[tmp[k]], *b = k ? &tracks[tmp[k - 1]] : NULL;
        if (b == NULL || strcasecmp(album_name(a), album_name(b)) != 0 ||
            strcasecmp(group_artist(a), group_artist(b)) != 0) {
            album_first[album_count++] = tmp[k];
        }
        tracks[tmp[k]].album_id = album_count - 1;
    }
    free(tmp);
}

musicdb_status_t musicdb_scan(const char *dir, void (*progress)(int done, int total))
{
    musicdb_lock();
    arena_free();
    count = artist_count = album_count = 0;
    generation++;
    snprintf(dir_scanned, sizeof(dir_scanned), "%s", dir);
    if (tracks == NULL) {
        tracks = ps_malloc(MUSICDB_MAX_TRACKS * sizeof(musicdb_track_t));
        by_title = ps_malloc(MUSICDB_MAX_TRACKS * sizeof(int));
        artist_first = ps_malloc(MUSICDB_MAX_TRACKS * sizeof(int));
        album_first = ps_malloc(MUSICDB_MAX_TRACKS * sizeof(int));
        found = ps_malloc(MUSICDB_MAX_TRACKS * sizeof(found_t));
    }
    if (!tracks || !by_title || !artist_first || !album_first || !found) {
        musicdb_unlock();
        return status = MUSICDB_EMPTY;
    }
    if (!sdcard_mount()) {
        musicdb_unlock();
        return status = MUSICDB_NO_CARD;
    }
    while (*dir == '/') {
        dir++;
    }
    snprintf(root, sizeof(root), "%s/%s", SDCARD_ROOT, dir);
    size_t rn = strlen(root);
    while (rn > 0 && root[rn - 1] == '/') {
        root[--rn] = '\0';
    }
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "%s: no such folder", root);
        musicdb_unlock();
        return status = MUSICDB_NO_FOLDER;
    }

    found_count = 0;
    list_files("", 0);
    cache_load();
    int read = 0, need = 0;
    for (int i = 0; i < found_count; i++) {
        need += !cache_has(found[i].rel, found[i].size);
    }

    bool changed = need > 0 || cache_count != found_count;
    for (int i = 0; i < found_count; i++) {
        musicdb_track_t *t = &tracks[count];
        memset(t, 0, sizeof(*t));
        t->rel = found[i].rel;
        t->size = found[i].size;
        if (!cache_lookup(t->rel, t->size, t)) {
            char full[PATH_LEN + 140];
            snprintf(full, sizeof(full), "%s/%s", root, t->rel);
            media_info_t m;
            if (!media_info_read(full, &m)) {
                continue;
            }
            t->title = astr(m.title);
            t->artist = astr(m.artist);
            t->album = astr(m.album);
            t->album_artist = astr(m.album_artist);
            t->track = m.track;
            t->disc = m.disc;
            t->duration_ms = m.duration_ms;
            t->format = m.format;
            if (progress && (++read % 10 == 0 || read == need)) {
                progress(read, need);
            }
        }
        if (!t->title[0]) {                                 // no title tag: the file name
            const char *base = strrchr(t->rel, '/');
            base = base ? base + 1 : t->rel;
            const char *dot = strrchr(base, '.');
            t->title = arena_str(base, dot ? (size_t)(dot - base) : strlen(base));
        }
        count++;
    }
    cache_free();
    qsort(tracks, count, sizeof(musicdb_track_t), cmp_rel);
    build_indexes();
    if (changed) {
        cache_save();
    }
    ESP_LOGI(TAG, "%s: %d tracks (%d read, the rest cached), %d artists, %d albums",
             root, count, read, artist_count, album_count);
    status = count ? MUSICDB_OK : MUSICDB_EMPTY;
    musicdb_unlock();
    return status;
}

// ---------- queries ----------

musicdb_status_t musicdb_status(void) { return status; }
const char *musicdb_dir(void) { return dir_scanned; }
uint32_t musicdb_generation(void) { return generation; }
int musicdb_count(void) { return count; }

const musicdb_track_t *musicdb_track(int i)
{
    static const musicdb_track_t none = {.rel = "", .title = "", .artist = "", .album = "", .album_artist = ""};
    return i >= 0 && i < count ? &tracks[i] : &none;
}

void musicdb_path(int i, char *out, size_t size)
{
    if (i < 0 || i >= count) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%s/%s", root, tracks[i].rel);
}

void musicdb_card_path(int i, char *out, size_t size)
{
    if (i < 0 || i >= count) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%s/%s", root + strlen(SDCARD_ROOT), tracks[i].rel);
}

int musicdb_find_path(const char *path)
{
    size_t sl = strlen(SDCARD_ROOT);
    char full[PATH_LEN + 140];
    if (strncmp(path, SDCARD_ROOT "/", sl + 1) != 0) {
        snprintf(full, sizeof(full), "%s%s%s", SDCARD_ROOT, path[0] == '/' ? "" : "/", path);
        path = full;
    }
    size_t rn = strlen(root);
    if (count == 0 || strncasecmp(path, root, rn) != 0 || path[rn] != '/') {
        return -1;
    }
    musicdb_track_t key = {.rel = path + rn + 1};
    musicdb_track_t *t = bsearch(&key, tracks, count, sizeof(musicdb_track_t), cmp_rel);
    return t ? (int)(t - tracks) : -1;
}

const char *musicdb_artist_of(int i)
{
    const musicdb_track_t *t = musicdb_track(i);
    return t->artist[0] ? t->artist : t->album_artist[0] ? t->album_artist : UNKNOWN_ARTIST;
}

const char *musicdb_album_of(int i)
{
    return album_name(musicdb_track(i));
}

const int *musicdb_songs(int *n)
{
    *n = count;
    return by_title;
}

int musicdb_artist_count(void) { return artist_count; }
int musicdb_album_count(void) { return album_count; }

const char *musicdb_artist_name(int a)
{
    return a >= 0 && a < artist_count ? group_artist(&tracks[artist_first[a]]) : "";
}

const char *musicdb_album_name(int al)
{
    return al >= 0 && al < album_count ? album_name(&tracks[album_first[al]]) : "";
}

const char *musicdb_album_artist(int al)
{
    return al >= 0 && al < album_count ? group_artist(&tracks[album_first[al]]) : "";
}

static int cmp_album_id(const void *a, const void *b)
{
    return cmp_text(musicdb_album_name(*(const int *)a), musicdb_album_name(*(const int *)b));
}

int musicdb_artist_albums(int a, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (tracks[i].artist_id != a) {
            continue;
        }
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) {
            seen = out[k] == tracks[i].album_id;
        }
        if (!seen) {
            out[n++] = tracks[i].album_id;
        }
    }
    qsort(out, n, sizeof(int), cmp_album_id);
    return n;
}

int musicdb_artist_songs(int a, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (tracks[i].artist_id == a) {
            out[n++] = i;
        }
    }
    qsort(out, n, sizeof(int), cmp_artist_song_idx);
    return n;
}

int musicdb_album_songs(int al, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (tracks[i].album_id == al) {
            out[n++] = i;
        }
    }
    qsort(out, n, sizeof(int), cmp_album_track_idx);
    return n;
}
