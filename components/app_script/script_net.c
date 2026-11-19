/*
 * The `net` module for script apps: HTTP(S) requests that run on the worker task, so the
 * screen and buttons keep working, with the answer delivered to a Lua callback later in
 * the main task.
 *
 *   net.get(url, headers, callback)          -> true, or false + reason if it can't start
 *   net.post(url, headers, body, callback)
 *   callback(status, body): status = HTTP status (200, 404, ...), or nil if the request
 *   failed (body then says why)
 *
 * A callback whose app was closed (and unloaded) or reinstalled in the meantime is
 * dropped: its Lua state no longer exists.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"             // root certificates, so HTTPS servers are verified
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "script_internal.h"
#include "worker.h"

static const char *TAG = "app_script";

#define RESPONSE_MAX    (256 * 1024)    // longest answer accepted
#define URL_MAX         2048
#define BODY_MAX        (64 * 1024)
#define TIMEOUT_MS      15000
#define PER_APP         2               // requests one app may have in flight
#define KEEP_FREE_SLOTS 1               // worker slots left for the native apps

typedef struct {
    char id[25];                        // the app it belongs to...
    uint32_t generation;                // ...in this loaded instance
    int callback;                       // registry reference in that app's Lua state
    bool post;
    char *url;
    char *headers;                      // "name\0value\0name\0value\0...\0"
    char *body;
    size_t body_len;
    int status;                         // result: HTTP status, -1 = failed
    char *response;
    size_t response_len;
    char error[96];
} request_t;

static char *psram_dup(const char *s, size_t len)
{
    char *p = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

static void free_request(request_t *r)
{
    heap_caps_free(r->url);
    heap_caps_free(r->headers);
    heap_caps_free(r->body);
    heap_caps_free(r->response);
    heap_caps_free(r);
}

// ---------- worker task ----------

static void *net_job(void *arg)
{
    request_t *r = arg;
    r->status = -1;
    esp_http_client_config_t cfg = {
        .url = r->url,
        .method = r->post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,                             // long query strings
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        snprintf(r->error, sizeof(r->error), "couldn't start the request (bad URL or out of memory)");
        return r;
    }
    for (const char *h = r->headers; h && *h; ) {
        const char *value = h + strlen(h) + 1;
        esp_http_client_set_header(client, h, value);
        h = value + strlen(value) + 1;
    }
    esp_err_t err = esp_http_client_open(client, r->post ? (int)r->body_len : 0);
    if (err != ESP_OK) {
        snprintf(r->error, sizeof(r->error), "couldn't connect: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return r;
    }
    for (size_t sent = 0; r->post && sent < r->body_len; ) {
        int n = esp_http_client_write(client, r->body + sent, r->body_len - sent);
        if (n <= 0) {
            snprintf(r->error, sizeof(r->error), "sending the body failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return r;
        }
        sent += n;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    size_t cap = 4096;
    r->response = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    bool too_big = false;
    while (r->response) {
        if (r->response_len == cap) {
            if (cap >= RESPONSE_MAX) {
                too_big = true;
                break;
            }
            cap *= 2;
            char *bigger = heap_caps_realloc(r->response, cap, MALLOC_CAP_SPIRAM);
            if (bigger == NULL) {
                too_big = true;
                break;
            }
            r->response = bigger;
        }
        int n = esp_http_client_read(client, r->response + r->response_len, cap - r->response_len);
        if (n < 0) {
            snprintf(r->error, sizeof(r->error), "the connection broke while reading");
            break;
        }
        if (n == 0) {
            r->status = status;                             // complete
            break;
        }
        r->response_len += n;
    }
    if (r->response == NULL) {
        snprintf(r->error, sizeof(r->error), "out of memory");
    } else if (too_big) {
        r->status = -1;
        snprintf(r->error, sizeof(r->error), "the answer is bigger than %d KB", RESPONSE_MAX / 1024);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return r;
}

// ---------- main task ----------

static void net_done(void *result, void *arg)
{
    request_t *r = arg;
    script_t *s = script_live(r->id, r->generation);
    if (s == NULL) {
        free_request(r);                                    // the app was closed or reinstalled
        return;
    }
    s->net_pending--;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r->callback);
    luaL_unref(L, LUA_REGISTRYINDEX, r->callback);
    if (r->status >= 0) {
        lua_pushinteger(L, r->status);
        lua_pushlstring(L, r->response ? r->response : "", r->response_len);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, r->error[0] ? r->error : "request failed");
    }
    free_request(r);
    script_deliver(s, 2);                                   // callback(status, body)
}

// net.get(url, headers, callback) / net.post(url, headers, body, callback)
static int request(lua_State *L, bool post)
{
    script_t *s = script_from_lua(L);
    size_t url_len, body_len = 0;
    const char *url = luaL_checklstring(L, 1, &url_len);
    luaL_argcheck(L, url_len < URL_MAX && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0),
                  1, "an http:// or https:// URL, under 2 KB");
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
    }
    const char *body = NULL;
    int cb = 3;
    if (post) {
        body = luaL_checklstring(L, 3, &body_len);
        luaL_argcheck(L, body_len <= BODY_MAX, 3, "body over 64 KB");
        cb = 4;
    }
    luaL_checktype(L, cb, LUA_TFUNCTION);

    if (s->net_pending >= PER_APP || worker_free_slots() <= KEEP_FREE_SLOTS) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "busy: too many requests at once");
        return 2;
    }

    // Headers as "name\0value\0...\0"
    size_t hlen = 1;
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
                return luaL_error(L, "bad argument #2 (headers: a table of name = \"value\" strings)");
            }
            hlen += lua_rawlen(L, -2) + lua_rawlen(L, -1) + 2;
            lua_pop(L, 1);
        }
    }
    request_t *r = heap_caps_calloc(1, sizeof(request_t), MALLOC_CAP_SPIRAM);
    char *headers = heap_caps_malloc(hlen, MALLOC_CAP_SPIRAM);
    if (r == NULL || headers == NULL) {
        heap_caps_free(r);
        heap_caps_free(headers);
        return luaL_error(L, "not enough memory");
    }
    size_t at = 0;
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            size_t n;
            const char *k = lua_tolstring(L, -2, &n);
            memcpy(headers + at, k, n + 1);
            at += n + 1;
            const char *v = lua_tolstring(L, -1, &n);
            memcpy(headers + at, v, n + 1);
            at += n + 1;
            lua_pop(L, 1);
        }
    }
    headers[at] = '\0';

    snprintf(r->id, sizeof(r->id), "%s", s->id);
    r->generation = s->generation;
    r->post = post;
    r->headers = headers;
    r->url = psram_dup(url, url_len);
    r->body = post ? psram_dup(body, body_len) : NULL;
    r->body_len = body_len;
    if (r->url == NULL || (post && r->body == NULL)) {
        free_request(r);
        return luaL_error(L, "not enough memory");
    }
    lua_pushvalue(L, cb);
    r->callback = luaL_ref(L, LUA_REGISTRYINDEX);
    if (!worker_submit_always(net_job, net_done, r)) {
        luaL_unref(L, LUA_REGISTRYINDEX, r->callback);
        free_request(r);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "busy: too many requests at once");
        return 2;
    }
    s->net_pending++;
    ESP_LOGI(TAG, "[%s] %s %s", s->id, post ? "POST" : "GET", url);
    lua_pushboolean(L, 1);
    return 1;
}

static int net_get(lua_State *L)
{
    return request(L, false);
}

static int net_post(lua_State *L)
{
    return request(L, true);
}

void script_net_open(lua_State *L)
{
    static const luaL_Reg funcs[] = {{"get", net_get}, {"post", net_post}, {NULL, NULL}};
    luaL_newlib(L, funcs);
    lua_setglobal(L, "net");
}
