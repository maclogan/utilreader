#include "books.h"
#include "books_internal.h"

#include <ctype.h>                      // isxdigit()
#include <dirent.h>                     // opendir(), readdir()
#include <stdio.h>
#include <stdlib.h>                     // qsort()
#include <string.h>
#include <sys/stat.h>                   // stat()
#include "esp_log.h"
#include "esp_vfs_fat.h"                // FAT on flash with wear levelling
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"            // the lock
#include "sdcard.h"                     // books kept on the card
#include "settings.h"                   // reading positions, the location

static const char *TAG = "books";

#define INTERNAL    "/books"            // the flash partition's mount point
#define PARTITION   "books"             // see partitions.csv
#define MAX_SIZE    (3 * 1024 * 1024)   // biggest book accepted

static SemaphoreHandle_t lock;
static wl_handle_t wl = WL_INVALID_HANDLE;
static bool mounted = false;            // the internal partition
static char base[160] = INTERNAL;       // where the books are: INTERNAL or "/sdcard/<folder>"
static uint32_t generation = 1;

static void path(char *out, size_t size, const char *id, const char *ext)
{
    snprintf(out, size, "%s/%s.%s", base, id, ext);
}

// Removes a book's pictures, <ID>.I00 .. <ID>.I99 (lock held)
static void remove_pictures(const char *id)
{
    char p[192], ext[8];
    for (int i = 0; i < BOOK_PICTURES_MAX; i++) {
        snprintf(ext, sizeof(ext), "I%02d", i);
        path(p, sizeof(p), id, ext);
        remove(p);
    }
}

// A book's file: <8 hex digits>.<3 characters> (TXT, IDX, CHP, I07, TMP ...)
static bool is_book_file(const char *name)
{
    const char *dot = strchr(name, '.');
    if (dot == NULL || dot - name != BOOK_ID_LEN || strlen(dot + 1) != 3) {
        return false;
    }
    char id[BOOK_ID_LEN + 1];
    memcpy(id, name, BOOK_ID_LEN);
    id[BOOK_ID_LEN] = '\0';
    return books_valid_id(id);
}

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    FILE *out = in ? fopen(to, "wb") : NULL;
    char *buf = out ? malloc(8192) : NULL;
    bool ok = buf != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, 8192, in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    free(buf);
    if (out && fclose(out) != 0) {
        ok = false;
    }
    if (in) {
        fclose(in);
    }
    if (!ok) {
        remove(to);
    }
    return ok;
}

// Moves every book file from folder `from` to folder `to` (renamed on the same file system,
// copied then deleted between flash and card). Names are gathered a batch at a time, so the
// folder isn't changed while it's being read. True if everything moved.
static bool move_books(const char *from, const char *to)
{
    enum { BATCH = 64 };
    char (*names)[16] = malloc(BATCH * sizeof(*names));
    if (names == NULL) {
        return false;
    }
    mkdir(to, 0777);
    bool ok = true;
    int moved = 0, failed = 0;
    while (1) {
        int n = 0;
        DIR *d = opendir(from);
        struct dirent *e;
        while (d && n < BATCH && (e = readdir(d)) != NULL) {
            if (is_book_file(e->d_name) && strlen(e->d_name) < sizeof(names[0])) {
                memcpy(names[n++], e->d_name, strlen(e->d_name) + 1);
            }
        }
        if (d) {
            closedir(d);
        }
        if (n == 0) {
            break;
        }
        int progress = 0;
        for (int i = 0; i < n; i++) {
            char a[200], b[200];
            snprintf(a, sizeof(a), "%s/%s", from, names[i]);
            snprintf(b, sizeof(b), "%s/%s", to, names[i]);
            remove(b);                                      // a copy already there: replaced
            if (rename(a, b) == 0 || (copy_file(a, b) && remove(a) == 0)) {
                moved++;
                progress++;
            } else {
                failed++;
                ok = false;
            }
        }
        if (progress == 0) {
            break;                                          // nothing more can be moved (card full?)
        }
    }
    free(names);
    ESP_LOGI(TAG, "moved %d book files from %s to %s%s", moved, from, to, failed ? " (some failed)" : "");
    return ok;
}

static bool on_card(void)
{
    return strncmp(base, SDCARD_ROOT "/", strlen(SDCARD_ROOT) + 1) == 0 || !strcmp(base, SDCARD_ROOT);
}

// The current location can be used: the partition is mounted, or the card is (mounted on
// demand; the folder is made if missing)
static bool ready(void)
{
    if (!on_card()) {
        return mounted;
    }
    if (!sdcard_mount()) {
        return false;
    }
    mkdir(base, 0777);
    static bool migrated;                                   // once per start
    if (!migrated && mounted) {
        migrated = true;
        move_books(INTERNAL, base);                         // books still in flash join the card's
        generation++;
    }
    return true;
}

static void set_base(const char *card_dir)
{
    if (card_dir[0] == '\0') {
        snprintf(base, sizeof(base), INTERNAL);
    } else {
        snprintf(base, sizeof(base), "%s%s", SDCARD_ROOT, strcmp(card_dir, "/") ? card_dir : "");
    }
}

bool books_set_location(const char *card_dir)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    char old[160];
    snprintf(old, sizeof(old), "%s", base);
    bool old_card = on_card();
    set_base(card_dir);
    bool ok = true;
    if (strcmp(old, base) != 0) {
        if ((old_card || on_card()) && !sdcard_mount()) {
            snprintf(base, sizeof(base), "%s", old);        // no card: stay where they are
            ok = false;
        } else {
            ok = move_books(old, base);
        }
    }
    if (ok || strcmp(old, base) != 0) {
        settings_set_str(SETTING_BOOKS_DIR, card_dir);
    }
    generation++;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "books in %s", base);
    return ok;
}

void books_location(char *out, size_t size)
{
    settings_get_str(SETTING_BOOKS_DIR, out, size, SETTING_BOOKS_DIR_DEFAULT);
}

bool books_init(void)
{
    lock = xSemaphoreCreateMutex();
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,                     // first boot: blank partition
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(INTERNAL, PARTITION, &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }
    mounted = true;
    char dir[128];
    books_location(dir, sizeof(dir));
    set_base(dir);
    uint64_t total, free_bytes;
    books_space(&total, &free_bytes);
    ESP_LOGI(TAG, "%llu KB free of %llu KB", free_bytes / 1024, total / 1024);
    return true;
}

bool books_valid_id(const char *id)
{
    if (id == NULL || strlen(id) != BOOK_ID_LEN) {
        return false;
    }
    for (int i = 0; i < BOOK_ID_LEN; i++) {
        if (!isxdigit((unsigned char)id[i])) {
            return false;
        }
    }
    return true;
}

// Reads the title line and size of a book (lock held)
static bool get_locked(const char *id, book_info_t *out)
{
    char p[192];
    path(p, sizeof(p), id, "TXT");
    FILE *f = fopen(p, "r");
    if (f == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        out->text_start = strlen(line);
        if (line[strlen(line) - 1] != '\n') {               // title longer than the buffer
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                out->text_start++;
            }
            out->text_start++;
        }
        line[strcspn(line, "\r\n")] = '\0';
        // Keep whole UTF-8 characters only
        size_t n = strlen(line);
        if (n >= sizeof(out->title)) {
            n = sizeof(out->title) - 1;
            while (n > 0 && ((unsigned char)line[n] & 0xC0) == 0x80) {
                n--;
            }
        }
        memcpy(out->title, line, n);
        out->title[n] = '\0';
    }
    fseek(f, 0, SEEK_END);
    out->size = ftell(f);
    fclose(f);
    if (out->title[0] == '\0') {
        snprintf(out->title, sizeof(out->title), "Book %s", id);
    }
    return true;
}

static int by_title(const void *a, const void *b)
{
    return strcmp(((const book_info_t *)a)->title, ((const book_info_t *)b)->title);
}

int books_list(book_info_t *out, int max)
{
    int count = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    DIR *dir = ready() ? opendir(base) : NULL;
    struct dirent *e;
    while (dir && count < max && (e = readdir(dir)) != NULL) {
        // "1A2B3C4D.TXT": take the 8-digit name of each .TXT file
        const char *dot = strchr(e->d_name, '.');
        if (dot == NULL || dot - e->d_name != BOOK_ID_LEN || strcasecmp(dot, ".TXT") != 0) {
            continue;
        }
        char id[BOOK_ID_LEN + 1];
        memcpy(id, e->d_name, BOOK_ID_LEN);
        id[BOOK_ID_LEN] = '\0';
        for (int i = 0; i < BOOK_ID_LEN; i++) {
            id[i] = toupper((unsigned char)id[i]);
        }
        if (books_valid_id(id) && get_locked(id, &out[count])) {
            count++;
        }
    }
    if (dir) {
        closedir(dir);
    }
    xSemaphoreGive(lock);
    qsort(out, count, sizeof(out[0]), by_title);
    return count;
}

bool books_get(const char *id, book_info_t *out)
{
    if (!books_valid_id(id) || !ready()) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = get_locked(id, out);
    xSemaphoreGive(lock);
    return ok;
}

int books_read(const char *id, uint32_t offset, void *buf, int len)
{
    char p[192];
    path(p, sizeof(p), id, "TXT");
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = -1;
    FILE *f = fopen(p, "r");
    if (f) {
        n = (fseek(f, offset, SEEK_SET) == 0) ? (int)fread(buf, 1, len, f) : 0;
        fclose(f);
    }
    xSemaphoreGive(lock);
    return n;
}

// Settings key for a book's reading position: "bk" + ID (10 characters, under NVS's 15)
static void position_key(char *out, const char *id)
{
    snprintf(out, 16, "bk%s", id);
}

esp_err_t books_delete(const char *id)
{
    if (!books_valid_id(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    char p[192];
    xSemaphoreTake(lock, portMAX_DELAY);
    path(p, sizeof(p), id, "TXT");
    int r = remove(p);
    path(p, sizeof(p), id, "IDX");
    remove(p);
    path(p, sizeof(p), id, "CHP");
    remove(p);
    path(p, sizeof(p), id, "TMP");
    remove(p);
    remove_pictures(id);
    path(p, sizeof(p), id, "SRC");                          // converted from a file in the folder:
    FILE *src = fopen(p, "r");                              // that goes too, or it would come back
    if (src) {
        char name[200];
        if (fgets(name, sizeof(name), src)) {
            name[strcspn(name, "\r\n")] = '\0';
            if (name[0] && !strchr(name, '/')) {
                char orig[400];
                snprintf(orig, sizeof(orig), "%s/%s", base, name);
                remove(orig);
            }
        }
        fclose(src);
    }
    remove(p);
    generation++;
    xSemaphoreGive(lock);

    char key[16];
    position_key(key, id);
    settings_set_i32(key, 0);
    snprintf(key, sizeof(key), "bo%s", id);
    settings_set_i32(key, 0);                               // forget when it was opened
    ESP_LOGI(TAG, "deleted %s", id);
    return r == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static uint32_t file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (uint32_t)st.st_size : 0;
}

esp_err_t books_upload_chunk(const char *id, uint32_t offset, uint32_t total,
                             const void *data, int len, uint32_t *have, bool *done)
{
    *done = false;
    *have = 0;
    if (!books_valid_id(id) || total == 0 || total > MAX_SIZE
        || offset + (uint32_t)len > total || !ready()) {
        return ESP_ERR_INVALID_ARG;
    }

    char tmp[192];
    path(tmp, sizeof(tmp), id, "TMP");
    esp_err_t result = ESP_OK;
    xSemaphoreTake(lock, portMAX_DELAY);

    uint32_t current = (offset == 0) ? 0 : file_size(tmp);
    if (offset != current) {
        *have = current;                                    // out of step: tell the sender where we are
        result = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (offset == 0) {
        uint64_t total_bytes, free_bytes;
        books_space(&total_bytes, &free_bytes);
        if (total > free_bytes) {
            result = ESP_ERR_NO_MEM;
            goto out;
        }
    }

    FILE *f = fopen(tmp, offset == 0 ? "wb" : "ab");
    if (f == NULL) {
        result = ESP_FAIL;
        goto out;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != (size_t)len) {
        result = ESP_ERR_NO_MEM;                            // partition full
        goto out;
    }
    *have = offset + len;

    if (*have == total) {                                   // last chunk: becomes the book
        char txt[192], idx[192];
        path(txt, sizeof(txt), id, "TXT");
        path(idx, sizeof(idx), id, "IDX");
        remove(txt);
        remove(idx);                                        // old page index no longer matches
        path(idx, sizeof(idx), id, "CHP");
        remove(idx);                                        // nor the chapter list
        remove_pictures(id);                                // the new book's pictures follow
        if (rename(tmp, txt) != 0) {
            result = ESP_FAIL;
            goto out;
        }
        generation++;
        *done = true;
        ESP_LOGI(TAG, "received %s (%lu bytes)", id, (unsigned long)total);
    }
out:
    xSemaphoreGive(lock);
    return result;
}

void books_space(uint64_t *total, uint64_t *free_bytes)
{
    *total = *free_bytes = 0;
    if (on_card()) {
        if (sdcard_mount()) {
            esp_vfs_fat_info(SDCARD_ROOT, total, free_bytes);
        }
    } else if (mounted) {
        esp_vfs_fat_info(INTERNAL, total, free_bytes);
    }
}

uint32_t books_generation(void)
{
    return generation;
}

uint32_t books_get_position(const char *id)
{
    char key[16];
    position_key(key, id);
    return (uint32_t)settings_get_i32(key, 0);
}

void books_set_position(const char *id, uint32_t offset)
{
    char key[16];
    position_key(key, id);
    settings_set_i32(key, (int32_t)offset);
}

// Last-opened stamps: settings "bo<ID>" = the value of the "book_seq" counter when it was
// opened. A counter rather than a time, so it works before the clock is set.
uint32_t books_last_opened(const char *id)
{
    char key[16];
    snprintf(key, sizeof(key), "bo%s", id);
    return (uint32_t)settings_get_i32(key, 0);
}

void books_mark_opened(const char *id)
{
    int32_t seq = settings_get_i32("book_seq", 0) + 1;
    settings_set_i32("book_seq", seq);
    char key[16];
    snprintf(key, sizeof(key), "bo%s", id);
    settings_set_i32(key, seq);
}

int books_read_index(const char *id, uint32_t *out, int max)
{
    char p[192];
    path(p, sizeof(p), id, "IDX");
    xSemaphoreTake(lock, portMAX_DELAY);
    int count = -1;
    FILE *f = fopen(p, "rb");
    if (f) {
        count = (int)fread(out, sizeof(uint32_t), max, f);
        fclose(f);
    }
    xSemaphoreGive(lock);
    return count > 0 ? count : -1;
}

esp_err_t books_write_index(const char *id, const uint32_t *pages, int count)
{
    char p[192], txt[192];
    path(p, sizeof(p), id, "IDX");
    path(txt, sizeof(txt), id, "TXT");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t result = ESP_FAIL;
    if (file_size(txt) > 0) {                               // book still there
        FILE *f = fopen(p, "wb");
        if (f) {
            if (fwrite(pages, sizeof(uint32_t), count, f) == (size_t)count) {
                result = ESP_OK;
            }
            fclose(f);
        }
    }
    xSemaphoreGive(lock);
    return result;
}

int books_read_extra(const char *id, const char *ext, void *buf, int max)
{
    char p[192];
    path(p, sizeof(p), id, ext);
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = -1;
    FILE *f = fopen(p, "rb");
    if (f) {
        n = (int)fread(buf, 1, max, f);
        fclose(f);
    }
    xSemaphoreGive(lock);
    return n;
}

esp_err_t books_write_extra(const char *id, const char *ext, const void *data, int len)
{
    char p[192], txt[192];
    path(p, sizeof(p), id, ext);
    path(txt, sizeof(txt), id, "TXT");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t result = ESP_FAIL;
    if (file_size(txt) > 0) {                               // book still there
        FILE *f = fopen(p, "wb");
        if (f) {
            if (len == 0 || fwrite(data, 1, len, f) == (size_t)len) {
                result = ESP_OK;
            }
            fclose(f);
        }
    }
    xSemaphoreGive(lock);
    return result;
}

void books_extra_path(const char *id, const char *ext, char *out, size_t size)
{
    path(out, size, id, ext);
}

bool books_base_ready(char *out, size_t size)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = ready();
    snprintf(out, size, "%s", base);
    xSemaphoreGive(lock);
    return ok;
}

void books_changed(void)
{
    generation++;
}
