#include "books.h"

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
#include "settings.h"                   // reading positions

static const char *TAG = "books";

#define BASE        "/books"
#define PARTITION   "books"             // see partitions.csv
#define MAX_SIZE    (3 * 1024 * 1024)   // biggest book accepted

static SemaphoreHandle_t lock;
static wl_handle_t wl = WL_INVALID_HANDLE;
static bool mounted = false;
static uint32_t generation = 1;

static void path(char *out, size_t size, const char *id, const char *ext)
{
    snprintf(out, size, BASE "/%s.%s", id, ext);
}

bool books_init(void)
{
    lock = xSemaphoreCreateMutex();
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,                     // first boot: blank partition
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE, PARTITION, &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }
    mounted = true;
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
    char p[32];
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
    if (!mounted) {
        return 0;
    }
    int count = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    DIR *dir = opendir(BASE);
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
    if (!mounted || !books_valid_id(id)) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = get_locked(id, out);
    xSemaphoreGive(lock);
    return ok;
}

int books_read(const char *id, uint32_t offset, void *buf, int len)
{
    char p[32];
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
    char p[32];
    xSemaphoreTake(lock, portMAX_DELAY);
    path(p, sizeof(p), id, "TXT");
    int r = remove(p);
    path(p, sizeof(p), id, "IDX");
    remove(p);
    path(p, sizeof(p), id, "CHP");
    remove(p);
    path(p, sizeof(p), id, "TMP");
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
    if (!mounted || !books_valid_id(id) || total == 0 || total > MAX_SIZE
        || offset + (uint32_t)len > total) {
        return ESP_ERR_INVALID_ARG;
    }

    char tmp[32];
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
        char txt[32], idx[32];
        path(txt, sizeof(txt), id, "TXT");
        path(idx, sizeof(idx), id, "IDX");
        remove(txt);
        remove(idx);                                        // old page index no longer matches
        path(idx, sizeof(idx), id, "CHP");
        remove(idx);                                        // nor the chapter list
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
    if (mounted) {
        esp_vfs_fat_info(BASE, total, free_bytes);
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
    char p[32];
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
    char p[32], txt[32];
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
    char p[32];
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
    char p[32], txt[32];
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
