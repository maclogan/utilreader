#include "webui.h"

#include <stdio.h>
#include <stdlib.h>                     // strtoul(), free()
#include <string.h>
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
#include "review.h"                     // saved words
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
    cfg.max_uri_handlers = 16;
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
        {.uri = "/api/pair",    .method = HTTP_POST,   .handler = pair_post},
        {.uri = "/api/pair/verify", .method = HTTP_POST, .handler = pair_verify_post},
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
