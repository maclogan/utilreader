#include "review.h"

#include <stdio.h>                      // fopen(), fgets(), fprintf(), rename()
#include <string.h>                     // strcspn(), strcmp(), strncmp()
#include "esp_log.h"

static const char *TAG = "review";

// Copies the next tab-separated field of *line into out[size] and advances *line past it
static void next_field(char **line, char *out, size_t size)
{
    size_t n = strcspn(*line, "\t\n");
    snprintf(out, size, "%.*s", (int)n, *line);
    *line += n;
    if (**line == '\t') {
        (*line)++;
    }
}

int review_load(review_entry_t *out, int max)
{
    FILE *f = fopen(REVIEW_FILE, "r");
    if (f == NULL) {
        return 0;
    }
    char line[256];
    int count = 0;
    while (count < max && fgets(line, sizeof(line), f)) {
        review_entry_t *e = &out[count];
        char *p = line;
        next_field(&p, e->w, sizeof(e->w));
        next_field(&p, e->p, sizeof(e->p));
        next_field(&p, e->d, sizeof(e->d));
        next_field(&p, e->day, sizeof(e->day));
        e->saved = true;
        if (e->w[0]) {
            count++;                                        // skip blank lines
        }
    }
    fclose(f);
    return count;
}

int review_count(void)
{
    FILE *f = fopen(REVIEW_FILE, "r");
    if (f == NULL) {
        return 0;
    }
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '\n' && line[0] != '\0') {
            count++;
        }
    }
    fclose(f);
    return count;
}

bool review_contains(const char *word)
{
    FILE *f = fopen(REVIEW_FILE, "r");
    if (f == NULL) {                                        // nothing saved yet
        return false;
    }
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\t\n")] = '\0';                 // keep just the first column (the word)
        found = strcmp(line, word) == 0;
    }
    fclose(f);
    return found;
}

esp_err_t review_add(const char *word, const char *pinyin, const char *meaning, const char *day)
{
    if (review_contains(word)) {
        return ESP_OK;
    }
    FILE *f = fopen(REVIEW_FILE, "a");                      // append, creating the file if needed
    if (f == NULL) {
        ESP_LOGE(TAG, "can't open %s", REVIEW_FILE);
        return ESP_FAIL;
    }
    fprintf(f, "%s\t%s\t%s\t%s\n", word, pinyin, meaning, day);
    fclose(f);
    ESP_LOGI(TAG, "saved %s (%s)", word, pinyin);
    return ESP_OK;
}

esp_err_t review_remove(const char *word)
{
    // Copy every other line to a new file, then swap it in
    const char *tmp_path = REVIEW_FILE ".tmp";
    FILE *in = fopen(REVIEW_FILE, "r");
    if (in == NULL) {
        return ESP_OK;                                      // nothing saved at all
    }
    FILE *out = fopen(tmp_path, "w");
    if (out == NULL) {
        fclose(in);
        ESP_LOGE(TAG, "can't write %s", tmp_path);
        return ESP_FAIL;
    }

    char line[256];
    while (fgets(line, sizeof(line), in)) {
        size_t word_len = strcspn(line, "\t\n");
        bool match = word_len == strlen(word) && strncmp(line, word, word_len) == 0;
        if (!match) {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);

    remove(REVIEW_FILE);
    if (rename(tmp_path, REVIEW_FILE) != 0) {
        ESP_LOGE(TAG, "can't replace %s", REVIEW_FILE);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "removed %s", word);
    return ESP_OK;
}
