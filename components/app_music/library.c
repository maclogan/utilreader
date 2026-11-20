#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_info.h"
#include "sdcard.h"

static const char *TAG = "library";

#define MAX_DEPTH   4
#define PATH_MAX_   256

typedef struct {
    char *rel;                          // path relative to the music folder ("Album/01 Song.flac")
    char *name;                         // display name (own copy: no extension)
} track_t;

static track_t *tracks;
static int count;
static char root[PATH_MAX_];            // "/sdcard/Music"

static void *ps_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static char *ps_strdup(const char *s, size_t n)
{
    char *d = ps_alloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static void clear(void)
{
    for (int i = 0; i < count; i++) {
        free(tracks[i].rel);
        free(tracks[i].name);
    }
    count = 0;
}

static void add(const char *rel)
{
    if (count >= LIBRARY_MAX_TRACKS) {
        return;
    }
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    const char *dot = strrchr(base, '.');
    track_t t = {
        .rel = ps_strdup(rel, strlen(rel)),
        .name = ps_strdup(base, dot ? (size_t)(dot - base) : strlen(base)),
    };
    if (t.rel == NULL || t.name == NULL) {
        free(t.rel);
        free(t.name);
        return;
    }
    tracks[count++] = t;
}

// rel = "" or "Album/" (with the slash)
static void scan(const char *rel, int depth)
{
    char path[2 * PATH_MAX_ + 2];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    DIR *d = opendir(path);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < LIBRARY_MAX_TRACKS) {
        if (e->d_name[0] == '.') {
            continue;                                       // hidden files, macOS "._" leftovers
        }
        char child[PATH_MAX_];
        int n = snprintf(child, sizeof(child), "%s%s", rel, e->d_name);
        if (n <= 0 || n >= (int)sizeof(child) - 2) {
            continue;
        }
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH) {
                strcat(child, "/");
                scan(child, depth + 1);
            }
        } else if (media_format_from_name(e->d_name) != MEDIA_UNKNOWN) {
            add(child);
        }
    }
    closedir(d);
}

static int compare(const void *a, const void *b)
{
    return strcasecmp(((const track_t *)a)->rel, ((const track_t *)b)->rel);
}

library_status_t library_scan(const char *dir)
{
    clear();
    if (tracks == NULL) {
        tracks = ps_alloc(LIBRARY_MAX_TRACKS * sizeof(track_t));
        if (tracks == NULL) {
            return LIBRARY_EMPTY;
        }
    }
    if (!sdcard_mount()) {
        return LIBRARY_NO_CARD;
    }
    while (*dir == '/') {
        dir++;
    }
    snprintf(root, sizeof(root), "%s/%s", SDCARD_ROOT, dir);
    size_t n = strlen(root);
    while (n > 0 && root[n - 1] == '/') {
        root[--n] = '\0';
    }
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "%s: no such folder", root);
        return LIBRARY_NO_FOLDER;
    }
    scan("", 0);
    qsort(tracks, count, sizeof(track_t), compare);
    ESP_LOGI(TAG, "%s: %d tracks", root, count);
    return count ? LIBRARY_OK : LIBRARY_EMPTY;
}

int library_count(void)
{
    return count;
}

void library_path(int i, char *out, size_t size)
{
    if (i < 0 || i >= count) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%s/%s", root, tracks[i].rel);
}

const char *library_name(int i)
{
    return i >= 0 && i < count ? tracks[i].name : "";
}

void library_folder(int i, char *out, size_t size)
{
    out[0] = '\0';
    if (i < 0 || i >= count) {
        return;
    }
    const char *slash = strrchr(tracks[i].rel, '/');
    if (slash) {
        size_t n = slash - tracks[i].rel;
        snprintf(out, size, "%.*s", (int)n, tracks[i].rel);
    }
}

int library_find(const char *path)
{
    size_t rn = strlen(root);
    if (strncmp(path, root, rn) != 0 || path[rn] != '/') {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (!strcmp(tracks[i].rel, path + rn + 1)) {
            return i;
        }
    }
    return -1;
}
