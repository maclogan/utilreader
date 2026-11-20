#include "playlists.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "musicdb.h"
#include "sdcard.h"

static const char *TAG = "playlists";

#define DIR_PATH    SDCARD_ROOT "/Playlists"
#define LINE_MAX_   400

bool playlist_file_ok(const char *file)
{
    size_t n = strlen(file);
    if (n == 0 || n >= sizeof(((playlist_info_t *)0)->file) || strchr(file, '/') || strchr(file, '\\') ||
        file[0] == '.') {
        return false;
    }
    const char *dot = strrchr(file, '.');
    return dot && (!strcasecmp(dot, ".m3u8") || !strcasecmp(dot, ".m3u"));
}

static void trim_line(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

// Reads the display name and entry count without resolving paths
static void read_header(const char *file, playlist_info_t *info)
{
    char path[96], line[LINE_MAX_];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, file);
    snprintf(info->file, sizeof(info->file), "%s", file);
    const char *dot = strrchr(file, '.');
    snprintf(info->name, sizeof(info->name), "%.*s", (int)(dot - file), file);
    info->tracks = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        if (first && (uint8_t)l[0] == 0xEF && (uint8_t)l[1] == 0xBB && (uint8_t)l[2] == 0xBF) {
            l += 3;                                         // UTF-8 byte order mark
        }
        first = false;
        trim_line(l);
        if (!strncmp(l, "#PLAYLIST:", 10) && l[10]) {
            snprintf(info->name, sizeof(info->name), "%s", l + 10);
        } else if (l[0] && l[0] != '#') {
            info->tracks++;
        }
    }
    fclose(f);
}

static int cmp_info(const void *a, const void *b)
{
    return strcasecmp(((const playlist_info_t *)a)->name, ((const playlist_info_t *)b)->name);
}

int playlists_list(playlist_info_t *out, int max)
{
    if (!sdcard_mount()) {
        return 0;
    }
    DIR *d = opendir(DIR_PATH);
    if (d == NULL) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_type != DT_DIR && playlist_file_ok(e->d_name)) {
            read_header(e->d_name, &out[n++]);
        }
    }
    closedir(d);
    qsort(out, n, sizeof(playlist_info_t), cmp_info);
    return n;
}

// Turns an entry into a full path under /sdcard: "\" -> "/", drive letters dropped,
// relative paths resolved against /Playlists, "." and ".." segments removed
static void resolve(const char *entry, char *out, size_t size)
{
    char tmp[LINE_MAX_ + 32];
    const char *e = entry;
    if (!strncasecmp(e, "file://", 7)) {
        e += 7;
    }
    if (isalpha((unsigned char)e[0]) && e[1] == ':') {
        e += 2;                                             // "E:\Music\..." -> "\Music\..."
    }
    if (e[0] == '/' || e[0] == '\\') {
        snprintf(tmp, sizeof(tmp), "%s", e);
    } else {
        snprintf(tmp, sizeof(tmp), "/Playlists/%s", e);
    }
    for (char *p = tmp; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    // Normalise segments
    char norm[LINE_MAX_ + 32] = "";
    size_t len = 0;
    char *save = NULL;
    for (char *seg = strtok_r(tmp, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".")) {
            continue;
        }
        if (!strcmp(seg, "..")) {
            char *slash = strrchr(norm, '/');
            len = slash ? (size_t)(slash - norm) : 0;
            norm[len] = '\0';
            continue;
        }
        int w = snprintf(norm + len, sizeof(norm) - len, "/%s", seg);
        if (w < 0 || (size_t)w >= sizeof(norm) - len) {
            break;
        }
        len += w;
    }
    snprintf(out, size, "%s%s", SDCARD_ROOT, norm);
}

int playlist_load(const char *file, int *out, int max, int *missing)
{
    if (missing) {
        *missing = 0;
    }
    if (!playlist_file_ok(file) || !sdcard_mount()) {
        return -1;
    }
    char path[96], line[LINE_MAX_], full[LINE_MAX_ + 48];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, file);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    int n = 0;
    bool first = true;
    while (fgets(line, sizeof(line), f) && n < max) {
        char *l = line;
        if (first && (uint8_t)l[0] == 0xEF && (uint8_t)l[1] == 0xBB && (uint8_t)l[2] == 0xBF) {
            l += 3;
        }
        first = false;
        trim_line(l);
        while (*l == ' ' || *l == '\t') {
            l++;
        }
        if (!l[0] || l[0] == '#') {
            continue;
        }
        resolve(l, full, sizeof(full));
        int i = musicdb_find_path(full);
        if (i >= 0) {
            out[n++] = i;
        } else if (missing) {
            (*missing)++;
        }
    }
    fclose(f);
    return n;
}

// "Road trip!" -> "Road trip_.m3u8": ASCII letters, digits, space and -_(),.&' kept
static void file_name_for(const char *name, char *out, size_t size)
{
    char base[40];
    size_t n = 0;
    for (const char *p = name; *p && n < sizeof(base) - 1; p++) {
        unsigned char c = *p;
        bool ok = isalnum(c) || strchr(" -_(),.&'", c);
        if (c >= 0x80) {
            ok = false;
        }
        base[n++] = ok ? (char)c : '_';
    }
    base[n] = '\0';
    while (n > 0 && (base[n - 1] == ' ' || base[n - 1] == '.')) {
        base[--n] = '\0';
    }
    char *b = base;
    while (*b == ' ' || *b == '.') {
        b++;
    }
    snprintf(out, size, "%s.m3u8", *b ? b : "Playlist");
}

bool playlist_save(const char *name, const char *old_file, const int *tracks, int n, char *file_out)
{
    if (!sdcard_mount()) {
        return false;
    }
    mkdir(DIR_PATH, 0777);
    char file[48];
    file_name_for(name, file, sizeof(file));
    // A different playlist already using that file name: number this one
    char path[96];
    struct stat st;
    for (int k = 2; k < 100; k++) {
        snprintf(path, sizeof(path), "%s/%s", DIR_PATH, file);
        if ((old_file && !strcasecmp(file, old_file)) || stat(path, &st) != 0) {
            break;
        }
        char base[40];
        file_name_for(name, base, sizeof(base));
        base[strlen(base) - 5] = '\0';                      // drop ".m3u8"
        snprintf(file, sizeof(file), "%.36s %d.m3u8", base, k);
    }

    char tmp_path[100];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, file);
    snprintf(tmp_path, sizeof(tmp_path), "%s/.saving.tmp", DIR_PATH);
    FILE *f = fopen(tmp_path, "w");
    if (f == NULL) {
        return false;
    }
    fprintf(f, "#EXTM3U\n#PLAYLIST:%s\n", name);
    char card_path[400];
    for (int i = 0; i < n; i++) {
        const musicdb_track_t *t = musicdb_track(tracks[i]);
        musicdb_card_path(tracks[i], card_path, sizeof(card_path));
        if (!card_path[0]) {
            continue;
        }
        fprintf(f, "#EXTINF:%lu,%s - %s\n%s\n", (unsigned long)(t->duration_ms / 1000),
                musicdb_artist_of(tracks[i]), t->title, card_path);
    }
    if (fclose(f) != 0) {
        remove(tmp_path);
        return false;
    }
    if (old_file && old_file[0] && playlist_file_ok(old_file)) {
        char old_path[96];
        snprintf(old_path, sizeof(old_path), "%s/%s", DIR_PATH, old_file);
        remove(old_path);
    }
    remove(path);
    if (rename(tmp_path, path) != 0) {
        return false;
    }
    if (file_out) {
        snprintf(file_out, 48, "%s", file);
    }
    ESP_LOGI(TAG, "saved %s (%d tracks)", file, n);
    return true;
}

bool playlist_delete(const char *file)
{
    if (!playlist_file_ok(file) || !sdcard_mount()) {
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, file);
    return remove(path) == 0;
}
