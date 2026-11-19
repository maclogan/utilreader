#include "story.h"

#include <stdio.h>                      // fopen(), snprintf()
#include <stdlib.h>                     // free()
#include <string.h>                     // strcmp(), strchr(), memcpy()
#include <time.h>                       // time(), gmtime_r()
#include "cJSON.h"                      // JSON parser bundled with ESP-IDF
#include "esp_crt_bundle.h"             // built-in root certificates so HTTPS can be verified
#include "esp_heap_caps.h"              // heap_caps_malloc(): put big buffers in PSRAM
#include "esp_http_client.h"            // HTTP(S) client
#include "esp_log.h"
#include "local_time.h"                 // local_time_synced()
#include "review.h"                     // REVIEW_FILE
#include "storage.h"                    // STORAGE_PATH
#include "sdkconfig.h"                  // CONFIG_STORY_* from menuconfig
#include "settings.h"                   // the HSK level chosen in the story menu
#include "story_ui.h"                   // story_ui_paginate(): how many sentences fit on a page

static const char *TAG = "story";

#define STORY_FILE      STORAGE_PATH "/story.json"
#define KNOWN_FILE      STORAGE_PATH "/known.txt"
#define API_URL         "https://api.anthropic.com/v1/messages"
#define RESPONSE_MAX    (128 * 1024)    // Claude's reply: the story JSON (~15 KB) plus thinking metadata
#define KNOWN_MAX       (16 * 1024)     // send at most this much of known.txt (the newest words)

static story_t *story;                  // today's story, in PSRAM (it's ~85 KB)
static story_t *incoming;               // a new story is parsed here first, then swapped with `story`
static bool have_story = false;

// The shape Claude's answer must have (structured outputs guarantee it matches)
static const char *SCHEMA =
    "{\"type\":\"object\",\"additionalProperties\":false,"
    "\"required\":[\"title\",\"sentences\",\"new_words\"],\"properties\":{"
      "\"title\":{\"type\":\"string\"},"
      "\"new_words\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"sentences\":{\"type\":\"array\",\"items\":{"
        "\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"tokens\"],\"properties\":{"
          "\"tokens\":{\"type\":\"array\",\"items\":{"
            "\"type\":\"object\",\"additionalProperties\":false,"
            "\"required\":[\"w\",\"p\",\"d\",\"new\"],\"properties\":{"
              "\"w\":{\"type\":\"string\"},\"p\":{\"type\":\"string\"},"
              "\"d\":{\"type\":\"string\"},\"new\":{\"type\":\"boolean\"}}}}}}}}}";

static const char *SYSTEM_PROMPT =
    "You write daily graded-reader stories for an adult learner of Mandarin Chinese. "
    "The story is read one sentence at a time on a small e-paper screen, and the reader "
    "can look up the pinyin and meaning of each word, so accuracy of pinyin and meanings matters.";

// Template for the request; filled in with the date, level and word lists
static const char *USER_PROMPT =
    "Write today's short story (story date %s).\n\n"
    "Reader level: comfortable with HSK %d vocabulary.\n"
    "Words introduced in earlier stories (treat them as known; reuse some so they stick): %s\n"
    "Words the reader saved for review (work a few of them in naturally): %s\n\n"
    "Rules:\n"
    "- Simplified Chinese. 8 to 14 sentences forming a complete little story with an ending. "
    "Pick a fresh topic, setting and characters; don't repeat earlier stories.\n"
    "- Each sentence at most 30 characters including punctuation. Use fullwidth Chinese "
    "punctuation (，。！？：、“”).\n"
    "- Apart from known words, introduce between 5 and 10 new characters, inside new words. "
    "Use each new word at least twice.\n"
    "- Split every sentence into tokens in reading order. A word token is one word, usually "
    "1 or 2 characters (longer only for names and fixed expressions). Each punctuation mark "
    "is its own token.\n"
    "- Word tokens: w = the characters; p = pinyin with tone marks, no spaces inside the word "
    "(e.g. \"péngyou\"); d = a short English meaning that fits this context, ASCII only, "
    "at most 26 characters; new = true if it contains one of today's new characters.\n"
    "- Punctuation tokens: p = \"\", d = \"\", new = false.\n"
    "- title: a short Chinese title, at most 10 characters.\n"
    "- new_words: the new words introduced today.";

// cJSON allocates lots of small nodes for a story; keep them in PSRAM, not internal RAM
static void *psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Copies a UTF-8 string, cutting it at a character boundary if it doesn't fit
static void copy_utf8(char *dst, size_t size, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;                                            // don't split a multi-byte character
        }
    }
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

// Reads a whole file into a new PSRAM buffer ('\0'-terminated). Returns NULL if it doesn't exist.
static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = psram_malloc(len + 1);
    if (buf) {
        len = fread(buf, 1, len, f);
        buf[len] = '\0';
        if (len_out) {
            *len_out = len;
        }
    }
    fclose(f);
    return buf;
}

// Story day for the current time: the local date, shifted so it changes at the reset hour
static bool story_day(int utc_offset, char out[11])
{
    if (!local_time_synced()) {
        return false;
    }
    time_t now = time(NULL) + utc_offset - CONFIG_STORY_RESET_HOUR * 3600;
    struct tm t;
    gmtime_r(&now, &t);
    char buf[32];                                           // roomy, so the compiler can't see truncation
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    copy_utf8(out, 11, buf);
    return true;
}

// Fills *dst from the JSON Claude returned. Returns false if it's unusable.
static bool parse_story(story_t *dst, const char *json, const char *day)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "story is not valid JSON");
        return false;
    }

    memset(dst, 0, sizeof(*dst));
    copy_utf8(dst->day, sizeof(dst->day), day);
    copy_utf8(dst->title, sizeof(dst->title), cJSON_GetStringValue(cJSON_GetObjectItem(root, "title")));

    const cJSON *sentence;
    cJSON_ArrayForEach(sentence, cJSON_GetObjectItem(root, "sentences")) {
        if (dst->sentence_count == STORY_MAX_SENTENCES) {
            break;
        }
        story_span_t *s = &dst->sentences[dst->sentence_count];
        s->first = dst->token_count;

        const cJSON *token;
        cJSON_ArrayForEach(token, cJSON_GetObjectItem(sentence, "tokens")) {
            if (dst->token_count == STORY_MAX_TOKENS) {
                break;
            }
            story_token_t *t = &dst->tokens[dst->token_count++];
            copy_utf8(t->w, sizeof(t->w), cJSON_GetStringValue(cJSON_GetObjectItem(token, "w")));
            copy_utf8(t->p, sizeof(t->p), cJSON_GetStringValue(cJSON_GetObjectItem(token, "p")));
            copy_utf8(t->d, sizeof(t->d), cJSON_GetStringValue(cJSON_GetObjectItem(token, "d")));
            t->is_new = cJSON_IsTrue(cJSON_GetObjectItem(token, "new"));
        }
        s->count = dst->token_count - s->first;
        if (s->count > 0) {
            dst->sentence_count++;                        // skip empty sentences
        }
    }
    cJSON_Delete(root);

    if (dst->sentence_count == 0) {
        ESP_LOGE(TAG, "story has no sentences");
        return false;
    }
    story_ui_paginate(dst);                               // group sentences into screen pages
    return true;
}

void story_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    story =psram_malloc(sizeof(*story));
    incoming = psram_malloc(sizeof(*incoming));
    if (story == NULL || incoming == NULL) {
        ESP_LOGE(TAG, "out of memory");
        story = NULL;
        return;
    }

    // Load the cached story: "YYYY-MM-DD\n{json}"
    char *file = read_file(STORY_FILE, NULL);
    if (file) {
        char *json = strchr(file, '\n');
        if (json) {
            *json++ = '\0';
            have_story = parse_story(story, json, file);
            if (have_story) {
                ESP_LOGI(TAG, "loaded story for %s: %d sentences", story->day, story->sentence_count);
            }
        }
        free(file);
    }
}

const story_t *story_get(void)
{
    return have_story ? story : NULL;
}

bool story_is_current(int utc_offset)
{
    char today[11];
    return have_story && story_day(utc_offset, today) && strcmp(today, story->day) == 0;
}

// Turns a word list file into one line for the prompt ("词、词、词"). Only the first column
// of each line is used, and only the last `max_bytes` of the file. Returns a PSRAM string.
static char *word_list(const char *path, size_t max_bytes)
{
    size_t len = 0;
    char *text = read_file(path, &len);
    if (text == NULL || len == 0) {
        free(text);
        char *none = psram_malloc(16);
        if (none) {
            strcpy(none, "(none yet)");
        }
        return none;
    }

    char *start = text;
    if (len > max_bytes) {                                  // keep only the newest words
        start = strchr(text + len - max_bytes, '\n');
        start = start ? start + 1 : text + len;
    }

    // "word\t...\n" lines -> "word、word". Each "、" is 3 bytes where "\n" was 1, so build a new string.
    char *list = psram_malloc(3 * len + 1);
    if (list == NULL) {
        free(text);
        return NULL;
    }
    char *out = list;
    bool first = true;
    for (char *line = start; *line; ) {
        char *end = strchr(line, '\n');
        size_t word_len = strcspn(line, "\t\n");
        if (word_len > 0) {
            if (!first) {
                memcpy(out, "\xE3\x80\x81", 3);             // "、" separator
                out += 3;
            }
            memcpy(out, line, word_len);
            out += word_len;
            first = false;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    *out = '\0';
    free(text);
    return list;
}

// Builds the JSON body for the Messages API request. Returns a PSRAM string or NULL.
static char *build_request(const char *day)
{
    char *known = word_list(KNOWN_FILE, KNOWN_MAX);
    char *review = word_list(REVIEW_FILE, 4096);
    size_t prompt_size = strlen(USER_PROMPT) + strlen(known) + strlen(review) + 64;
    char *prompt = psram_malloc(prompt_size);
    char *body = NULL;

    if (known && review && prompt) {
        snprintf(prompt, prompt_size, USER_PROMPT, day, story_hsk_level(), known, review);

        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", CONFIG_STORY_MODEL);
        cJSON_AddNumberToObject(req, "max_tokens", 16000);
        cJSON_AddStringToObject(req, "fallbacks", "default");   // if the model declines, Anthropic retries on another
        cJSON_AddStringToObject(req, "system", SYSTEM_PROMPT);

        cJSON *output = cJSON_AddObjectToObject(req, "output_config");
        cJSON_AddStringToObject(output, "effort", "medium");
        cJSON *format = cJSON_AddObjectToObject(output, "format");
        cJSON_AddStringToObject(format, "type", "json_schema");
        cJSON_AddItemToObject(format, "schema", cJSON_Parse(SCHEMA));

        cJSON *message = cJSON_CreateObject();
        cJSON_AddStringToObject(message, "role", "user");
        cJSON_AddStringToObject(message, "content", prompt);
        cJSON *messages = cJSON_AddArrayToObject(req, "messages");
        cJSON_AddItemToArray(messages, message);

        body = cJSON_PrintUnformatted(req);                 // escapes the prompt text properly
        cJSON_Delete(req);
    }
    free(known);
    free(review);
    free(prompt);
    return body;
}

// Sends the request and returns Claude's reply text (the story JSON), or NULL on failure
static char *call_claude(const char *body)
{
    char *response = psram_malloc(RESPONSE_MAX);
    if (response == NULL) {
        return NULL;
    }

    esp_http_client_config_t config = {
        .url = API_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,         // verify the server's HTTPS certificate
        .timeout_ms = 300000,                               // writing the story can take a few minutes
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "content-type", "application/json");
    esp_http_client_set_header(client, "x-api-key", CONFIG_STORY_API_KEY);
    esp_http_client_set_header(client, "anthropic-version", "2026-10-21");
    esp_http_client_set_header(client, "anthropic-beta", "server-side-fallback-2026-07-01");

    int body_len = strlen(body);
    int total = 0;
    int status = 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK && esp_http_client_write(client, body, body_len) == body_len) {
        esp_http_client_fetch_headers(client);              // blocks while Claude writes
        status = esp_http_client_get_status_code(client);
        while (total < RESPONSE_MAX - 1) {
            int n = esp_http_client_read(client, response + total, RESPONSE_MAX - 1 - total);
            if (n <= 0) {
                break;
            }
            total += n;
        }
    } else {
        ESP_LOGE(TAG, "request failed: %s", esp_err_to_name(err));
    }
    response[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.300s", status, response);
        free(response);
        return NULL;
    }

    // Reply: {"stop_reason": ..., "content": [{"type": "thinking", ...}, {"type": "text", "text": "{story}"}]}
    char *text = NULL;
    cJSON *root = cJSON_Parse(response);
    const char *stop = cJSON_GetStringValue(cJSON_GetObjectItem(root, "stop_reason"));
    if (stop && strcmp(stop, "end_turn") != 0) {
        ESP_LOGE(TAG, "story not finished (stop_reason %s)", stop);   // e.g. "refusal" or "max_tokens"
    } else {
        const cJSON *block;
        cJSON_ArrayForEach(block, cJSON_GetObjectItem(root, "content")) {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
            if (type && strcmp(type, "text") == 0 && s) {
                text = psram_malloc(strlen(s) + 1);
                if (text) {
                    strcpy(text, s);
                }
                break;
            }
        }
        if (text == NULL) {
            ESP_LOGE(TAG, "reply has no text block");
        }
    }
    cJSON_Delete(root);
    free(response);
    return text;
}

// Appends today's new words to known.txt (skipping ones already there)
static void remember_new_words(const char *story_json)
{
    cJSON *root = cJSON_Parse(story_json);
    char *known = read_file(KNOWN_FILE, NULL);
    FILE *f = fopen(KNOWN_FILE, "a");
    if (root && f) {
        const cJSON *word;
        cJSON_ArrayForEach(word, cJSON_GetObjectItem(root, "new_words")) {
            const char *w = cJSON_GetStringValue(word);
            char needle[64];
            snprintf(needle, sizeof(needle), "\n%s\n", w ? w : "");
            bool already = known && (strstr(known, needle) || strncmp(known, needle + 1, strlen(needle + 1)) == 0);
            if (w && *w && !already) {
                fprintf(f, "%s\n", w);
            }
        }
    }
    if (f) {
        fclose(f);
    }
    free(known);
    cJSON_Delete(root);
}

char *story_request(int utc_offset, char day[11])
{
    if (story == NULL || !story_day(utc_offset, day)) {
        return NULL;                                        // storage failed, or no time yet
    }
    if (!story_has_api_key()) {
        ESP_LOGE(TAG, "no API key: set it in menuconfig -> Chinese Story");
        return NULL;
    }

    ESP_LOGI(TAG, "writing the story for %s", day);
    char *body = build_request(day);
    char *json = body ? call_claude(body) : NULL;
    free(body);
    return json;
}

esp_err_t story_accept(const char *json, const char *day)
{
    if (incoming == NULL || !parse_story(incoming, json, day)) {
        return ESP_FAIL;                                    // the story on screen stays as it was
    }

    story_t *old = story;                                   // swap the new story in
    story = incoming;
    incoming = old;
    have_story = true;

    FILE *f = fopen(STORY_FILE, "w");                       // cache it so a reboot doesn't pay for another
    if (f) {
        fprintf(f, "%s\n%s", day, json);
        fclose(f);
    }
    remember_new_words(json);
    ESP_LOGI(TAG, "story \"%s\": %d sentences, %d pages", story->title, story->sentence_count, story->page_count);
    return ESP_OK;
}

bool story_has_api_key(void)
{
    return strlen(CONFIG_STORY_API_KEY) > 0;
}

bool story_is_word(const story_token_t *t)
{
    return t->p[0] != '\0';
}

int story_page_word(const story_t *s, int page, bool last)
{
    const story_span_t *span = &s->pages[page];
    for (int i = 0; i < span->count; i++) {
        int t = last ? span->first + span->count - 1 - i : span->first + i;
        if (story_is_word(&s->tokens[t])) {
            return t;
        }
    }
    return -1;
}

bool story_step_word(const story_t *s, int *page, int *token, int dir)
{
    int p = *page;
    int t = *token;
    while (true) {
        const story_span_t *span = &s->pages[p];
        t += dir;
        if (t < span->first || t >= span->first + span->count) {   // ran off this page
            p += dir;
            if (p < 0 || p >= s->page_count) {
                return false;                                       // start/end of the story
            }
            t = dir > 0 ? s->pages[p].first - 1 : s->pages[p].first + s->pages[p].count;
            continue;
        }
        if (story_is_word(&s->tokens[t])) {
            *page = p;
            *token = t;
            return true;
        }
    }
}

int story_hsk_level(void)
{
    int level = settings_get_i32("hsk_level", CONFIG_STORY_HSK_LEVEL);  // menuconfig value until changed
    return (level >= 1 && level <= 6) ? level : CONFIG_STORY_HSK_LEVEL;
}

void story_set_hsk_level(int level)
{
    settings_set_i32("hsk_level", level);
}
