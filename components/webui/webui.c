#include "webui.h"

#include <stdio.h>
#include <stdlib.h>                     // strtoul(), free()
#include <string.h>
#include "app_registry.h"               // menu order and hidden apps
#include "app_script.h"                 // installing script apps
#include "books.h"                      // book files
#include "buttons.h"                    // buttons_post(BUTTON_WAKE): tell the main loop to redraw
#include "cJSON.h"                      // JSON responses
#include "esp_heap_caps.h"              // PSRAM buffers
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"                 // esp_random(): pairing codes and tokens
#include "esp_timer.h"                  // esp_timer_get_time(): code expiry
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "musicdb.h"                    // the music library
#include "playlists.h"
#include "podcasts.h"                   // podcast feeds and episodes
#include "sdcard.h"
#include <stdarg.h>
#include "review.h"                     // saved words
#include "settings.h"                   // the music folder
#include "wifi.h"                       // wifi_ip_string()

static const char *TAG = "webui";

#define CHUNK_MAX   (64 * 1024)         // biggest upload chunk accepted (the page sends 32 KB)
#define WORDS_MAX   500

#define CODE_TTL_US     (180LL * 1000 * 1000)   // a pairing code is valid for 3 minutes
#define CODE_TRIES      5                       // wrong guesses before the code is withdrawn
#define TOKENS          4                       // browsers paired at once (oldest replaced)
#define TOKEN_LEN       32                      // hex characters (128 bits)

// index.html, embedded by CMakeLists.txt (EMBED_TXTFILES adds a terminating '\0')
extern const char index_html[] asm("_binary_index_html_start");

// ---------- pairing ----------
// State shared between the web server's task and the main loop, guarded by `pair_lock`.

static SemaphoreHandle_t pair_lock;
static char code[7];                    // "" = no code on offer
static int64_t code_expires;            // esp_timer_get_time() deadline
static int code_tries;
static char tokens[TOKENS][TOKEN_LEN + 1];
static int next_token = 0;

// Server and status for the Transfer app (also guarded by pair_lock)
static httpd_handle_t server;
static int upload_percent = -1;
static char activity[128];             // fits "Received: " + the longest title

// Records what just happened and wakes the main loop so the Transfer app can show it
static void set_activity(const char *text, int percent)
{
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    snprintf(activity, sizeof(activity), "%s", text);
    upload_percent = percent;
    xSemaphoreGive(pair_lock);
    buttons_post(BUTTON_WAKE);
}

static void random_hex(char *out, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < digits; i++) {
        out[i] = hex[esp_random() & 15];
    }
    out[digits] = '\0';
}

// Clears an expired code (lock held). Returns true if a code is on offer.
static bool code_live_locked(void)
{
    if (code[0] && esp_timer_get_time() > code_expires) {
        code[0] = '\0';
    }
    return code[0] != '\0';
}

void webui_status(webui_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (pair_lock == NULL) {
        out->upload_percent = -1;
        return;                                             // never started
    }
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    out->running = server != NULL;
    out->code_live = code_live_locked();
    if (out->code_live) {
        memcpy(out->code, code, sizeof(out->code));
    }
    out->upload_percent = upload_percent;
    memcpy(out->activity, activity, sizeof(out->activity));
    xSemaphoreGive(pair_lock);
}

void webui_pairing_cancel(void)
{
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    code[0] = '\0';
    xSemaphoreGive(pair_lock);
}

// True if the request carries a token handed out by /api/pair/verify
static bool authorized(httpd_req_t *req)
{
    char t[TOKEN_LEN + 1];
    if (httpd_req_get_hdr_value_str(req, "X-Token", t, sizeof(t)) != ESP_OK || strlen(t) != TOKEN_LEN) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    for (int i = 0; i < TOKENS && !ok; i++) {
        ok = tokens[i][0] && strcmp(tokens[i], t) == 0;
    }
    xSemaphoreGive(pair_lock);
    return ok;
}

// ---------- helpers ----------

static esp_err_t send_json(httpd_req_t *req, cJSON *json, const char *status)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", message);
    return send_json(req, j, status);
}

// Value of ?key=... in the request URL, into out[size]. Returns false if missing.
static bool query(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char q[160];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, key, out, size) == ESP_OK;
}

// Reads the whole request body (at most CHUNK_MAX bytes) into a new PSRAM buffer.
// Returns ESP_OK and fills *out/*len, or sends an error response and returns its result
// (ESP_FAIL if the connection dropped: nothing more can be sent then).
static esp_err_t recv_body(httpd_req_t *req, char **out, int *len_out)
{
    int len = req->content_len;
    if (len <= 0 || len > CHUNK_MAX) {
        send_error(req, "400 Bad Request", "chunk too big");
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        send_error(req, "500 Internal Server Error", "out of memory");
        return ESP_ERR_NO_MEM;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;                                       // slow network: keep waiting
        }
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;                                // connection dropped; the page retries
        }
        got += r;
    }
    *out = buf;
    *len_out = len;
    return ESP_OK;
}

// ---------- handlers ----------

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, index_html);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    uint64_t total, free_bytes;
    books_space(&total, &free_bytes);
    char ip[20] = "";
    wifi_ip_string(ip, sizeof(ip));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddNumberToObject(j, "total", (double)total);
    cJSON_AddNumberToObject(j, "free", (double)free_bytes);
    cJSON_AddNumberToObject(j, "app_api", APP_SCRIPT_API_VERSION);   // the catalog checks apps against it
    return send_json(req, j, NULL);
}

static esp_err_t books_get_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    book_info_t *list = heap_caps_malloc(BOOKS_MAX * sizeof(book_info_t), MALLOC_CAP_SPIRAM);
    if (list == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = books_list(list, BOOKS_MAX);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const book_info_t *b = &list[i];
        uint32_t pos = books_get_position(b->id);
        uint32_t text_len = b->size > b->text_start ? b->size - b->text_start : 1;
        int progress = pos > b->text_start ? (int)((uint64_t)(pos - b->text_start) * 100 / text_len) : 0;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", b->id);
        cJSON_AddStringToObject(o, "title", b->title);
        cJSON_AddNumberToObject(o, "size", b->size);
        cJSON_AddNumberToObject(o, "progress", progress > 100 ? 100 : progress);
        cJSON_AddItemToArray(arr, o);
    }
    free(list);
    return send_json(req, arr, NULL);
}

static esp_err_t books_delete_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[16];
    if (!query(req, "id", id, sizeof(id)) || !books_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad id");
    }
    book_info_t info;
    bool known = books_get(id, &info);
    esp_err_t err = books_delete(id);
    if (err == ESP_OK) {
        char text[sizeof(activity)];
        snprintf(text, sizeof(text), "Deleted: %s", known ? info.title : id);
        set_activity(text, -1);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "deleted", err == ESP_OK);
    return send_json(req, j, err == ESP_OK ? NULL : "404 Not Found");
}

// POST /api/upload?id=1A2B3C4D&offset=N&total=T, body = the next chunk of the book
static esp_err_t upload_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[16], num[16];
    uint32_t offset, total;
    if (!query(req, "id", id, sizeof(id)) || !books_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad id");
    }
    if (!query(req, "offset", num, sizeof(num))) {
        return send_error(req, "400 Bad Request", "missing offset");
    }
    offset = strtoul(num, NULL, 10);
    if (!query(req, "total", num, sizeof(num))) {
        return send_error(req, "400 Bad Request", "missing total");
    }
    total = strtoul(num, NULL, 10);
    char *buf;
    int len;
    esp_err_t got = recv_body(req, &buf, &len);
    if (got != ESP_OK) {
        return got == ESP_FAIL ? ESP_FAIL : ESP_OK;         // error already sent (or connection gone)
    }

    uint32_t have = 0;
    bool done = false;
    esp_err_t err = books_upload_chunk(id, offset, total, buf, len, &have, &done);
    free(buf);

    if (err == ESP_OK && done) {
        book_info_t info;
        char text[sizeof(activity)];
        snprintf(text, sizeof(text), "Received: %s", books_get(id, &info) ? info.title : id);
        set_activity(text, -1);
    } else if (err == ESP_OK && have * 10 / total != offset * 10 / total) {
        set_activity("Receiving a book", (int)((uint64_t)have * 100 / total));  // every 10%
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "have", have);
    cJSON_AddBoolToObject(j, "done", done);
    switch (err) {
    case ESP_OK:
        return send_json(req, j, NULL);
    case ESP_ERR_INVALID_STATE:
        return send_json(req, j, "409 Conflict");          // the page resumes from "have"
    case ESP_ERR_NO_MEM:
        cJSON_AddStringToObject(j, "error", "not enough space");
        return send_json(req, j, "507 Insufficient Storage");
    default:
        cJSON_AddStringToObject(j, "error", esp_err_to_name(err));
        return send_json(req, j, "400 Bad Request");
    }
}

static esp_err_t words_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    review_entry_t *words = heap_caps_malloc(WORDS_MAX * sizeof(review_entry_t), MALLOC_CAP_SPIRAM);
    if (words == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = review_load(words, WORDS_MAX);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "w", words[i].w);
        cJSON_AddStringToObject(o, "p", words[i].p);
        cJSON_AddStringToObject(o, "d", words[i].d);
        cJSON_AddStringToObject(o, "day", words[i].day);
        cJSON_AddItemToArray(arr, o);
    }
    free(words);
    return send_json(req, arr, NULL);
}

// ---------- script apps ----------

// GET /api/apps: installed script apps
static esp_err_t apps_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    return send_json(req, app_script_list_json(), NULL);
}

// POST /api/apps/upload?id=clock&file=main.lua&offset=N&total=T[&new=1], body = the next
// chunk of that file. new=1 on the first chunk of a package starts a fresh upload.
static esp_err_t apps_upload_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[32], file[40], num[16], fresh[4] = "";
    if (!query(req, "id", id, sizeof(id)) || !app_script_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad app id");
    }
    if (!query(req, "file", file, sizeof(file))) {
        return send_error(req, "400 Bad Request", "missing file");
    }
    if (!query(req, "offset", num, sizeof(num))) {
        return send_error(req, "400 Bad Request", "missing offset");
    }
    uint32_t offset = strtoul(num, NULL, 10);
    if (!query(req, "total", num, sizeof(num))) {
        return send_error(req, "400 Bad Request", "missing total");
    }
    uint32_t total = strtoul(num, NULL, 10);
    query(req, "new", fresh, sizeof(fresh));

    char *buf;
    int len;
    esp_err_t got = recv_body(req, &buf, &len);
    if (got != ESP_OK) {
        return got == ESP_FAIL ? ESP_FAIL : ESP_OK;
    }
    uint32_t have = 0;
    esp_err_t err = app_script_stage(id, file, offset, total, buf, len, strcmp(fresh, "1") == 0, &have);
    free(buf);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "have", have);
    switch (err) {
    case ESP_OK:
        return send_json(req, j, NULL);
    case ESP_ERR_INVALID_STATE:
        return send_json(req, j, "409 Conflict");          // the page resumes from "have"
    case ESP_ERR_NO_MEM:
        cJSON_AddStringToObject(j, "error", "not enough space for apps");
        return send_json(req, j, "507 Insufficient Storage");
    default:
        cJSON_AddStringToObject(j, "error", "bad file name, size or id");
        return send_json(req, j, "400 Bad Request");
    }
}

// POST /api/apps/install?id=clock: install the uploaded package
static esp_err_t apps_install_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[32], err[96];
    if (!query(req, "id", id, sizeof(id)) || !app_script_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad app id");
    }
    if (app_script_install(id, err, sizeof(err)) != ESP_OK) {
        return send_error(req, "400 Bad Request", err);
    }
    char text[sizeof(activity)];
    snprintf(text, sizeof(text), "Installed app: %s", id);
    set_activity(text, -1);
    buttons_post(BUTTON_APPS_CHANGED);                      // the main loop reloads the app list
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "installed", true);
    return send_json(req, j, NULL);
}

// DELETE /api/apps?id=clock: remove an app with its settings and data
static esp_err_t apps_delete(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[32];
    if (!query(req, "id", id, sizeof(id)) || !app_script_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad app id");
    }
    esp_err_t err = app_script_uninstall(id);
    if (err == ESP_OK) {
        char text[sizeof(activity)];
        snprintf(text, sizeof(text), "Removed app: %s", id);
        set_activity(text, -1);
        buttons_post(BUTTON_APPS_CHANGED);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "deleted", err == ESP_OK);
    return send_json(req, j, err == ESP_OK ? NULL : "404 Not Found");
}

// GET /api/apps/log?id=clock: the app's recent log lines (print(), errors)
static esp_err_t apps_log_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[32];
    if (!query(req, "id", id, sizeof(id)) || !app_script_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad app id");
    }
    char *text = heap_caps_malloc(2100, MALLOC_CAP_SPIRAM);
    if (text == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    app_script_log_read(id, text, 2100);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, text);
    free(text);
    return e;
}

// Reads a small JSON request body. Returns the parsed value, or NULL after sending an error.
static cJSON *recv_json(httpd_req_t *req)
{
    char *buf;
    int len;
    if (req->content_len > 8192) {
        send_error(req, "400 Bad Request", "body too big");
        return NULL;
    }
    if (recv_body(req, &buf, &len) != ESP_OK) {
        return NULL;
    }
    cJSON *j = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (j == NULL) {
        send_error(req, "400 Bad Request", "body is not valid JSON");
    }
    return j;
}

// GET /api/apps/settings?id=clock: the app's settings and current values (no secrets)
static esp_err_t apps_settings_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[32];
    if (!query(req, "id", id, sizeof(id)) || !app_script_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad app id");
    }
    cJSON *j = app_script_settings_json(id);
    return j ? send_json(req, j, NULL) : send_error(req, "404 Not Found", "no such app");
}

// POST /api/apps/settings?id=clock, body {"key": value, ...}: save settings; the app reloads
static esp_err_t apps_settings_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[32], err[96];
    if (!query(req, "id", id, sizeof(id)) || !app_script_valid_id(id)) {
        return send_error(req, "400 Bad Request", "missing or bad app id");
    }
    cJSON *values = recv_json(req);
    if (values == NULL) {
        return ESP_OK;                                      // error already sent
    }
    esp_err_t e = app_script_save_settings(id, values, err, sizeof(err));
    cJSON_Delete(values);
    if (e != ESP_OK) {
        return send_error(req, e == ESP_ERR_NOT_FOUND ? "404 Not Found" : "400 Bad Request", err);
    }
    buttons_post(BUTTON_APPS_CHANGED);                      // apps reload with the new values
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "saved", true);
    return send_json(req, j, NULL);
}

// GET /api/menu: every app in menu order, with hidden flags
static esp_err_t menu_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    return send_json(req, app_registry_menu_json(), NULL);
}

// Joins an array of id strings into "a,b,c" (out[size]); false if it isn't one
static bool join_ids(const cJSON *arr, char *out, size_t size)
{
    out[0] = '\0';
    if (arr == NULL) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        return false;
    }
    size_t at = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > 24 || strchr(v->valuestring, ',')) {
            return false;
        }
        int n = snprintf(out + at, size - at, "%s%s", at ? "," : "", v->valuestring);
        if (n < 0 || (size_t)n >= size - at) {
            return false;
        }
        at += n;
    }
    return true;
}

// POST /api/menu, body {"order": [ids], "hidden": [ids]}: the menu's order and hidden apps
static esp_err_t menu_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return ESP_OK;
    }
    char order[APP_REGISTRY_MAX * 26], hidden[APP_REGISTRY_MAX * 26];
    bool ok = join_ids(cJSON_GetObjectItem(j, "order"), order, sizeof(order)) &&
              join_ids(cJSON_GetObjectItem(j, "hidden"), hidden, sizeof(hidden));
    cJSON_Delete(j);
    if (!ok) {
        return send_error(req, "400 Bad Request", "order and hidden must be arrays of app ids");
    }
    app_registry_save_prefs(order, hidden);
    buttons_post(BUTTON_APPS_CHANGED);                      // the main loop rebuilds the menu
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "saved", true);
    return send_json(req, r, NULL);
}

// POST /api/pair: show a code on the board's screen (the same one again if still valid)
static esp_err_t pair_post(httpd_req_t *req)
{
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    if (!code_live_locked()) {
        snprintf(code, sizeof(code), "%06lu", (unsigned long)(esp_random() % 1000000));
        code_tries = 0;
    }
    code_expires = esp_timer_get_time() + CODE_TTL_US;
    xSemaphoreGive(pair_lock);
    buttons_post(BUTTON_WAKE);                              // main loop draws the code
    ESP_LOGI(TAG, "pairing code requested");

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "seconds", CODE_TTL_US / 1000000);
    return send_json(req, j, NULL);
}

// POST /api/pair/verify?code=123456: exchange the code on screen for a token
static esp_err_t pair_verify_post(httpd_req_t *req)
{
    char guess[8] = "";
    query(req, "code", guess, sizeof(guess));

    char token[TOKEN_LEN + 1] = "";
    int tries_left = 0;
    bool live, match = false;
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    live = code_live_locked();
    if (live) {
        match = strcmp(guess, code) == 0;
        if (match) {
            random_hex(token, TOKEN_LEN);
            memcpy(tokens[next_token], token, sizeof(token));
            next_token = (next_token + 1) % TOKENS;
            code[0] = '\0';                                 // used up
        } else if (++code_tries >= CODE_TRIES) {
            code[0] = '\0';                                 // too many guesses: withdraw it
        }
        tries_left = CODE_TRIES - code_tries;
    }
    xSemaphoreGive(pair_lock);

    cJSON *j = cJSON_CreateObject();
    if (!live) {
        cJSON_AddStringToObject(j, "error", "no code on screen: ask for a new one");
        return send_json(req, j, "410 Gone");
    }
    if (!match) {
        if (tries_left <= 0) {
            buttons_post(BUTTON_WAKE);                      // take the withdrawn code off the screen
        }
        cJSON_AddStringToObject(j, "error", tries_left > 0 ? "wrong code" : "too many tries: ask for a new code");
        cJSON_AddNumberToObject(j, "triesLeft", tries_left > 0 ? tries_left : 0);
        return send_json(req, j, "403 Forbidden");
    }
    set_activity("A browser paired", -1);                  // also takes the code off the screen
    ESP_LOGI(TAG, "browser paired");
    cJSON_AddStringToObject(j, "token", token);
    return send_json(req, j, NULL);
}

// ---------- music: the library and playlists (musicdb) ----------

// %XX and + decoded in place (query values arrive URL-encoded)
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p == '+' ? ' ' : *p;
        }
    }
    *o = '\0';
}

// Scans the library if nothing has been scanned yet (no track can be playing then)
static musicdb_status_t music_ready(void)
{
    if (musicdb_status() == MUSICDB_NOT_SCANNED) {
        char dir[64];
        settings_get_str(SETTING_MUSIC_DIR, dir, sizeof(dir), SETTING_MUSIC_DIR_DEFAULT);
        musicdb_scan(dir, NULL);
    }
    return musicdb_status();
}

// Appends s as a JSON string to buf (len, cap); flushes a chunk when it fills
static void json_str(httpd_req_t *req, char *buf, size_t *len, size_t cap, const char *s)
{
    if (*len + 8 + strlen(s) * 6 > cap) {
        httpd_resp_send_chunk(req, buf, *len);
        *len = 0;
    }
    buf[(*len)++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            buf[(*len)++] = '\\';
            buf[(*len)++] = *p;
        } else if (*p < 0x20) {
            *len += sprintf(buf + *len, "\\u%04x", *p);
        } else {
            buf[(*len)++] = *p;
        }
    }
    buf[(*len)++] = '"';
}

// GET /api/music: {"status","dir","gen","songs":[[title, artist, album, seconds], ...]}
// The array position is the track's id for the playlist calls. Streamed: big libraries
// don't fit cJSON in RAM.
static esp_err_t music_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    musicdb_lock();
    static const char *names[] = {"ok", "no_card", "no_folder", "empty", "not_scanned"};
    musicdb_status_t s = music_ready();
    const size_t cap = 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        musicdb_unlock();
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = sprintf(buf, "{\"status\":\"%s\",\"gen\":%lu,\"dir\":", names[s], (unsigned long)musicdb_generation());
    json_str(req, buf, &len, cap, musicdb_dir());
    len += sprintf(buf + len, ",\"songs\":[");
    for (int i = 0; i < musicdb_count(); i++) {
        const musicdb_track_t *t = musicdb_track(i);
        if (len + 64 > cap) {
            httpd_resp_send_chunk(req, buf, len);
            len = 0;
        }
        len += sprintf(buf + len, "%s[", i ? "," : "");
        json_str(req, buf, &len, cap, t->title);
        buf[len++] = ',';
        json_str(req, buf, &len, cap, musicdb_artist_of(i));
        buf[len++] = ',';
        json_str(req, buf, &len, cap, t->album);
        len += sprintf(buf + len, ",%lu]", (unsigned long)(t->duration_ms / 1000));
    }
    len += sprintf(buf + len, "]}");
    httpd_resp_send_chunk(req, buf, len);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    musicdb_unlock();
    return ESP_OK;
}

// GET /api/music/playlists: [{"file","name","tracks"}]
static esp_err_t playlists_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    playlist_info_t *list = heap_caps_malloc(PLAYLISTS_MAX * sizeof(playlist_info_t), MALLOC_CAP_SPIRAM);
    if (list == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = playlists_list(list, PLAYLISTS_MAX);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "file", list[i].file);
        cJSON_AddStringToObject(o, "name", list[i].name);
        cJSON_AddNumberToObject(o, "tracks", list[i].tracks);
        cJSON_AddItemToArray(arr, o);
    }
    free(list);
    return send_json(req, arr, NULL);
}

// GET /api/music/playlist?file=X: {"file","name","items":[track ids],"missing":n}
static esp_err_t playlist_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char file[160];
    if (!query(req, "file", file, sizeof(file))) {
        return send_error(req, "400 Bad Request", "missing file");
    }
    url_decode(file);
    int *items = heap_caps_malloc(PLAYLIST_MAX_TRACKS * sizeof(int), MALLOC_CAP_SPIRAM);
    if (items == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    musicdb_lock();
    music_ready();
    int missing = 0;
    int n = playlist_load(file, items, PLAYLIST_MAX_TRACKS, &missing);
    musicdb_unlock();
    if (n < 0) {
        free(items);
        return send_error(req, "404 Not Found", "no such playlist");
    }
    playlist_info_t info[1] = {0};                          // the display name, from the list
    snprintf(info[0].name, sizeof(info[0].name), "%.*s", (int)sizeof(info[0].name) - 1, file);
    playlist_info_t *all = heap_caps_malloc(PLAYLISTS_MAX * sizeof(playlist_info_t), MALLOC_CAP_SPIRAM);
    if (all) {
        int c = playlists_list(all, PLAYLISTS_MAX);
        for (int i = 0; i < c; i++) {
            if (!strcmp(all[i].file, file)) {
                info[0] = all[i];
            }
        }
        free(all);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "file", file);
    cJSON_AddStringToObject(j, "name", info[0].name);
    cJSON_AddItemToObject(j, "items", cJSON_CreateIntArray(items, n));
    cJSON_AddNumberToObject(j, "missing", missing);
    cJSON_AddNumberToObject(j, "gen", musicdb_generation());
    free(items);
    return send_json(req, j, NULL);
}

// POST /api/music/playlist, body {"name","file" (the one being replaced, or ""),"gen","items":[ids]}
static esp_err_t playlist_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return ESP_OK;
    }
    const cJSON *name = cJSON_GetObjectItem(j, "name"), *old = cJSON_GetObjectItem(j, "file");
    const cJSON *gen = cJSON_GetObjectItem(j, "gen"), *items = cJSON_GetObjectItem(j, "items");
    if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) > 60 ||
        !cJSON_IsArray(items) || !cJSON_IsNumber(gen)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "need name, gen and items");
    }
    musicdb_lock();
    if ((uint32_t)gen->valuedouble != musicdb_generation()) {
        musicdb_unlock();
        cJSON_Delete(j);
        return send_error(req, "409 Conflict", "the library was rescanned: reload the page");
    }
    int n = 0, *ids = heap_caps_malloc(PLAYLIST_MAX_TRACKS * sizeof(int), MALLOC_CAP_SPIRAM);
    const cJSON *v;
    cJSON_ArrayForEach(v, items) {
        if (ids && n < PLAYLIST_MAX_TRACKS && cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint < musicdb_count()) {
            ids[n++] = v->valueint;
        }
    }
    char file[48] = "";
    const char *old_file = cJSON_IsString(old) && playlist_file_ok(old->valuestring) ? old->valuestring : "";
    bool ok = ids && playlist_save(name->valuestring, old_file, ids, n, file);
    musicdb_unlock();
    free(ids);
    cJSON_Delete(j);
    if (!ok) {
        return send_error(req, "500 Internal Server Error", "couldn't write the playlist (card missing?)");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "file", file);
    return send_json(req, r, NULL);
}

// DELETE /api/music/playlist?file=X
static esp_err_t playlist_delete_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char file[160];
    if (!query(req, "file", file, sizeof(file))) {
        return send_error(req, "400 Bad Request", "missing file");
    }
    url_decode(file);
    if (!playlist_delete(file)) {
        return send_error(req, "404 Not Found", "no such playlist");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "deleted", true);
    return send_json(req, r, NULL);
}

// ---------- podcasts ----------

// Streams JSON: a small buffer flushed as chunks
typedef struct {
    httpd_req_t *req;
    char *buf;
    size_t len, cap;
} jstream_t;

static void js_raw(jstream_t *j, const char *fmt, ...)
{
    if (j->len + 256 > j->cap) {
        httpd_resp_send_chunk(j->req, j->buf, j->len);
        j->len = 0;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        j->len += (size_t)n < j->cap - j->len ? (size_t)n : j->cap - j->len - 1;
    }
}

static void js_str(jstream_t *j, const char *s)
{
    json_str(j->req, j->buf, &j->len, j->cap, s);
}

static void js_options(jstream_t *j, const pod_options_t *o)
{
    js_raw(j, "{\"keep_mode\":%d,\"keep\":%d,\"delete_played\":%d,\"after_next\":%d,\"oldest_first\":%d}",
           o->keep_mode, o->keep, o->delete_played, o->after_next, o->oldest_first);
}

static int opt_int(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

// GET /api/podcasts: defaults, the updater's status, every feed with what's on the card
static esp_err_t podcasts_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    pod_feed_t *feeds = heap_caps_malloc(PODCASTS_MAX_FEEDS * sizeof(pod_feed_t), MALLOC_CAP_SPIRAM);
    pod_episode_t *eps = heap_caps_malloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t), MALLOC_CAP_SPIRAM);
    jstream_t j = {.req = req, .cap = 4096};
    j.buf = heap_caps_malloc(j.cap, MALLOC_CAP_SPIRAM);
    if (!feeds || !eps || !j.buf) {
        free(feeds);
        free(eps);
        free(j.buf);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    pod_options_t d;
    int back, fwd;
    podcasts_defaults(&d, &back, &fwd);
    js_raw(&j, "{\"card\":%s,\"defaults\":", sdcard_mount() ? "true" : "false");
    js_options(&j, &d);
    js_raw(&j, ",\"seek_back\":%d,\"seek_fwd\":%d,\"status\":", back, fwd);
    pod_update_status_t u;
    podcasts_update_status(&u);
    js_raw(&j, "{\"running\":%s,\"offline\":%s,\"feed\":%d,\"feeds\":%d,\"downloading\":%d,\"percent\":%d,\"current\":",
           u.running ? "true" : "false", u.offline ? "true" : "false", u.feed, u.feeds, u.downloading, u.percent);
    js_str(&j, u.current);
    js_raw(&j, ",\"message\":");
    js_str(&j, u.message);
    js_raw(&j, "},\"feeds\":[");
    int nf = podcasts_feeds(feeds, PODCASTS_MAX_FEEDS);
    for (int i = 0; i < nf; i++) {
        const pod_feed_t *f = &feeds[i];
        int n = podcasts_episodes(f->id, eps, PODCASTS_MAX_EPISODES), on_card = 0, unplayed = 0;
        uint64_t bytes = 0;
        for (int k = 0; k < n; k++) {
            if (eps[k].flags & POD_DOWNLOADED) {
                on_card++;
                unplayed += !(eps[k].flags & POD_PLAYED);
                bytes += eps[k].length;
            }
        }
        js_raw(&j, "%s{\"id\":\"%s\",\"url\":", i ? "," : "", f->id);
        js_str(&j, f->url);
        js_raw(&j, ",\"title\":");
        js_str(&j, f->title);
        js_raw(&j, ",\"error\":");
        js_str(&j, f->error);
        js_raw(&j, ",\"last_update\":%lld,\"episodes\":%d,\"on_card\":%d,\"unplayed\":%d,\"bytes\":%llu,\"opt\":",
               (long long)f->last_update, n, on_card, unplayed, (unsigned long long)bytes);
        js_options(&j, &f->opt);
        js_raw(&j, "}");
    }
    js_raw(&j, "]}");
    httpd_resp_send_chunk(req, j.buf, j.len);
    httpd_resp_send_chunk(req, NULL, 0);
    free(feeds);
    free(eps);
    free(j.buf);
    return ESP_OK;
}

// POST /api/podcasts/defaults, body {"keep_mode","keep","delete_played","after_next","oldest_first","seek_back","seek_fwd"}
static esp_err_t podcasts_defaults_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return ESP_OK;
    }
    pod_options_t d;
    int back, fwd;
    podcasts_defaults(&d, &back, &fwd);
    pod_options_t o = {
        opt_int(j, "keep_mode", d.keep_mode), opt_int(j, "keep", d.keep), opt_int(j, "delete_played", d.delete_played),
        opt_int(j, "after_next", d.after_next), opt_int(j, "oldest_first", d.oldest_first),
    };
    podcasts_set_defaults(&o, opt_int(j, "seek_back", back), opt_int(j, "seek_fwd", fwd));
    cJSON_Delete(j);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "saved", true);
    return send_json(req, r, NULL);
}

// POST /api/podcasts/feed: {"url"} adds a feed; {"id","opt":{...}} changes its options
// (-1 = use the default)
static esp_err_t podcasts_feed_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return ESP_OK;
    }
    const cJSON *url = cJSON_GetObjectItem(j, "url"), *id = cJSON_GetObjectItem(j, "id");
    const cJSON *opt = cJSON_GetObjectItem(j, "opt");
    char new_id[9] = "";
    const char *err = NULL;
    if (cJSON_IsString(url)) {
        if (strlen(url->valuestring) >= sizeof(((pod_feed_t *)0)->url) || !podcasts_add_feed(url->valuestring, new_id)) {
            err = "Couldn't add it: not an http(s) address, already added, too many feeds, or no card";
        }
    } else if (cJSON_IsString(id) && cJSON_IsObject(opt)) {
        pod_feed_t f;
        if (!podcasts_feed(id->valuestring, &f)) {
            err = "No such feed";
        } else {
            f.opt = (pod_options_t){
                opt_int(opt, "keep_mode", -1), opt_int(opt, "keep", -1), opt_int(opt, "delete_played", -1),
                opt_int(opt, "after_next", -1), opt_int(opt, "oldest_first", -1),
            };
            podcasts_update_feed(&f);
            snprintf(new_id, sizeof(new_id), "%s", f.id);
        }
    } else {
        err = "Send {url} or {id, opt}";
    }
    cJSON_Delete(j);
    if (err) {
        return send_error(req, "400 Bad Request", err);
    }
    podcasts_update_start(true, NULL);                      // fetch it now (Transfer is open)
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "id", new_id);
    return send_json(req, r, NULL);
}

// DELETE /api/podcasts/feed?id=X: the feed, its episodes and their files
static esp_err_t podcasts_feed_delete(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[16];
    if (!query(req, "id", id, sizeof(id)) || !podcasts_remove_feed(id)) {
        return send_error(req, "404 Not Found", "no such feed");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "deleted", true);
    return send_json(req, r, NULL);
}

// GET /api/podcasts/episodes?id=X: [{"id","t","d","dur","f"}], newest first (f = flags:
// 1 picked, 2 played, 4 on the card)
static esp_err_t podcasts_episodes_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[16];
    if (!query(req, "id", id, sizeof(id))) {
        return send_error(req, "400 Bad Request", "missing id");
    }
    pod_episode_t *eps = heap_caps_malloc(PODCASTS_MAX_EPISODES * sizeof(pod_episode_t), MALLOC_CAP_SPIRAM);
    jstream_t j = {.req = req, .cap = 4096};
    j.buf = heap_caps_malloc(j.cap, MALLOC_CAP_SPIRAM);
    if (!eps || !j.buf) {
        free(eps);
        free(j.buf);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = podcasts_episodes(id, eps, PODCASTS_MAX_EPISODES);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    js_raw(&j, "[");
    for (int i = 0; i < n; i++) {
        js_raw(&j, "%s{\"id\":\"%s\",\"t\":", i ? "," : "", eps[i].id);
        js_str(&j, eps[i].title);
        js_raw(&j, ",\"d\":%lld,\"dur\":%lu,\"f\":%u}", (long long)eps[i].date,
               (unsigned long)eps[i].duration_s, eps[i].flags);
    }
    js_raw(&j, "]");
    httpd_resp_send_chunk(req, j.buf, j.len);
    httpd_resp_send_chunk(req, NULL, 0);
    free(eps);
    free(j.buf);
    return ESP_OK;
}

// POST /api/podcasts/wanted?id=X, body {"ids":[episode ids]}: the custom picks
static esp_err_t podcasts_wanted_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "not paired");
    }
    char id[16];
    if (!query(req, "id", id, sizeof(id))) {
        return send_error(req, "400 Bad Request", "missing id");
    }
    cJSON *j = recv_json(req);
    if (j == NULL) {
        return ESP_OK;
    }
    const cJSON *arr = cJSON_GetObjectItem(j, "ids");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    const char **ids = malloc((n ? n : 1) * sizeof(char *));
    int k = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) {
        if (ids && cJSON_IsString(v)) {
            ids[k++] = v->valuestring;
        }
    }
    if (ids) {
        podcasts_set_wanted(id, ids, k);
        podcasts_update_start(true, NULL);                  // download the picks now
    }
    free(ids);
    cJSON_Delete(j);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "picked", k);
    return send_json(req, r, NULL);
}

// ---------- start ----------

esp_err_t webui_start(void)
{
    if (pair_lock == NULL) {
        pair_lock = xSemaphoreCreateMutex();
    }
    if (server != NULL) {
        return ESP_OK;                                      // already running
    }
    activity[0] = '\0';
    upload_percent = -1;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 36;
    cfg.lru_purge_enable = true;                            // drop idle connections when busy
    cfg.recv_wait_timeout = 15;                             // seconds per upload read

    httpd_handle_t handle;
    esp_err_t err = httpd_start(&handle, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/",            .method = HTTP_GET,    .handler = page_get},
        {.uri = "/api/info",    .method = HTTP_GET,    .handler = info_get},
        {.uri = "/api/books",   .method = HTTP_GET,    .handler = books_get_handler},
        {.uri = "/api/books",   .method = HTTP_DELETE, .handler = books_delete_handler},
        {.uri = "/api/upload",  .method = HTTP_POST,   .handler = upload_post},
        {.uri = "/api/words",   .method = HTTP_GET,    .handler = words_get},
        {.uri = "/api/apps",    .method = HTTP_GET,    .handler = apps_get},
        {.uri = "/api/apps",    .method = HTTP_DELETE, .handler = apps_delete},
        {.uri = "/api/apps/upload",  .method = HTTP_POST, .handler = apps_upload_post},
        {.uri = "/api/apps/install", .method = HTTP_POST, .handler = apps_install_post},
        {.uri = "/api/apps/log",     .method = HTTP_GET,  .handler = apps_log_get},
        {.uri = "/api/apps/settings", .method = HTTP_GET,  .handler = apps_settings_get},
        {.uri = "/api/apps/settings", .method = HTTP_POST, .handler = apps_settings_post},
        {.uri = "/api/menu",    .method = HTTP_GET,    .handler = menu_get},
        {.uri = "/api/menu",    .method = HTTP_POST,   .handler = menu_post},
        {.uri = "/api/pair",    .method = HTTP_POST,   .handler = pair_post},
        {.uri = "/api/pair/verify", .method = HTTP_POST, .handler = pair_verify_post},
        {.uri = "/api/music",   .method = HTTP_GET,    .handler = music_get},
        {.uri = "/api/music/playlists", .method = HTTP_GET, .handler = playlists_get},
        {.uri = "/api/music/playlist",  .method = HTTP_GET, .handler = playlist_get},
        {.uri = "/api/music/playlist",  .method = HTTP_POST, .handler = playlist_post},
        {.uri = "/api/music/playlist",  .method = HTTP_DELETE, .handler = playlist_delete_handler},
        {.uri = "/api/podcasts",          .method = HTTP_GET,    .handler = podcasts_get},
        {.uri = "/api/podcasts/defaults", .method = HTTP_POST,   .handler = podcasts_defaults_post},
        {.uri = "/api/podcasts/feed",     .method = HTTP_POST,   .handler = podcasts_feed_post},
        {.uri = "/api/podcasts/feed",     .method = HTTP_DELETE, .handler = podcasts_feed_delete},
        {.uri = "/api/podcasts/episodes", .method = HTTP_GET,    .handler = podcasts_episodes_get},
        {.uri = "/api/podcasts/wanted",   .method = HTTP_POST,   .handler = podcasts_wanted_post},
    };
    for (int i = 0; i < (int)(sizeof(routes) / sizeof(routes[0])); i++) {
        httpd_register_uri_handler(handle, &routes[i]);
    }
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    server = handle;
    xSemaphoreGive(pair_lock);
    ESP_LOGI(TAG, "web interface running on port %d", cfg.server_port);
    return ESP_OK;
}

void webui_stop(void)
{
    if (pair_lock == NULL || server == NULL) {
        return;
    }
    httpd_handle_t handle = server;
    xSemaphoreTake(pair_lock, portMAX_DELAY);
    server = NULL;
    code[0] = '\0';                                         // no pairing without the server
    xSemaphoreGive(pair_lock);
    httpd_stop(handle);                                     // waits for requests in progress
    ESP_LOGI(TAG, "web interface stopped");
}
