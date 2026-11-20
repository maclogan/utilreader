#include "app_script.h"

#include <ctype.h>                      // toupper()
#include <dirent.h>                     // opendir(): the installed apps
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>                    // strcasecmp()
#include <sys/stat.h>                   // stat(), mkdir()
#include <unistd.h>                     // unlink(), rmdir()
#include "app_registry.h"               // adding the apps to the menu
#include "buttons.h"                    // buttons_post()
#include "epd.h"                        // icons, the error screen
#include "esp_heap_caps.h"              // Lua heaps in PSRAM
#include "esp_log.h"
#include "esp_vfs_fat.h"                // the /apps partition
#include "font16.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"            // the log ring buffers are read by the web portal
#include "lauxlib.h"
#include "lualib.h"
#include "script_internal.h"
#include "settings.h"                   // which bundled apps were installed already
#include "ui_sidebar.h"

static const char *TAG = "app_script";

#define BASE            "/apps"
#define PARTITION       "apps"
#define STAGING         BASE "/.staging"
#define MANIFEST_MAX    (8 * 1024)      // largest app.json / settings.json read
#define MEM_LIMIT       (512 * 1024)    // Lua heap per app
#define HOOK_EVERY      1000            // instructions between hook calls
#define HOOK_LIMIT      10000           // hook calls per callback: 10 M instructions, then it's stuck
#define LOG_SLOTS       8               // apps with a log at once (oldest reused)
#define LOG_SIZE        2048            // bytes of log kept per app

static script_t *scripts[APP_REGISTRY_MAX];
static int script_count = 0;
static bool mounted = false;
static uint32_t next_generation = 1;    // unique across every load, so a stale answer never matches

// ---------- files ----------

bool app_script_valid_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > 24) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')) {
            return false;
        }
    }
    return true;
}

bool script_valid_file(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > 32 || name[0] == '.') {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

// Whole file into a malloc'd, '\0'-terminated buffer; NULL if missing or bigger than max
static char *read_file(const char *path, size_t max, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = NULL;
    if (len >= 0 && (size_t)len <= max && (buf = malloc(len + 1)) != NULL) {
        if (fread(buf, 1, len, f) != (size_t)len) {
            free(buf);
            buf = NULL;
        } else {
            buf[len] = '\0';
            if (len_out) {
                *len_out = len;
            }
        }
    }
    fclose(f);
    return buf;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static cJSON *read_json(const char *path)
{
    char *text = read_file(path, MANIFEST_MAX, NULL);
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    return j;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Deletes the plain files in `dir`, except `keep` (may be NULL). Returns false on a failure.
static bool delete_files(const char *dir, const char *keep)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return true;
    }
    bool ok = true;
    struct dirent *e;
    char path[320];                                         // folder + a long file name
    while ((e = readdir(d)) != NULL) {
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR || (keep && strcmp(e->d_name, keep) == 0)) {
            continue;
        }
        ok &= unlink(path) == 0;
    }
    closedir(d);
    return ok;
}

// "1.10.2" > "1.9" etc. Missing parts count as 0.
static int compare_versions(const char *a, const char *b)
{
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        a += *a == '.';
        b += *b == '.';
        if ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
            break;                                          // suffixes like "-beta": stop comparing
        }
    }
    return 0;
}

// ---------- logs ----------
// Kept per app id, apart from the apps themselves, so the web portal can read them from its
// own task while the main task reloads apps.

static struct {
    char id[25];
    char text[LOG_SIZE];
    int len;
} logs[LOG_SLOTS];
static int log_next = 0;
static SemaphoreHandle_t log_lock;

void script_log(const script_t *s, const char *text)
{
    ESP_LOGI(TAG, "[%s] %s", s->id, text);
    xSemaphoreTake(log_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < LOG_SLOTS && slot < 0; i++) {
        if (strcmp(logs[i].id, s->id) == 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = log_next;
        log_next = (log_next + 1) % LOG_SLOTS;
        snprintf(logs[slot].id, sizeof(logs[slot].id), "%s", s->id);
        logs[slot].len = 0;
    }
    int add = strlen(text) + 1;                             // the line and its '\n'
    if (add > LOG_SIZE - 1) {
        text += add - (LOG_SIZE - 1);
        add = LOG_SIZE - 1;
    }
    if (logs[slot].len + add > LOG_SIZE - 1) {              // full: drop the oldest lines
        int drop = logs[slot].len + add - (LOG_SIZE - 1);
        char *nl = memchr(logs[slot].text + drop, '\n', logs[slot].len - drop);
        drop = nl ? nl - logs[slot].text + 1 : logs[slot].len;
        memmove(logs[slot].text, logs[slot].text + drop, logs[slot].len - drop);
        logs[slot].len -= drop;
    }
    memcpy(logs[slot].text + logs[slot].len, text, add - 1);
    logs[slot].len += add;
    logs[slot].text[logs[slot].len - 1] = '\n';
    logs[slot].text[logs[slot].len] = '\0';
    xSemaphoreGive(log_lock);
}

void app_script_log_read(const char *id, char *out, size_t size)
{
    out[0] = '\0';
    xSemaphoreTake(log_lock, portMAX_DELAY);
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (strcmp(logs[i].id, id) == 0) {
            snprintf(out, size, "%s", logs[i].text);
        }
    }
    xSemaphoreGive(log_lock);
}

// ---------- Lua state ----------

// Lua's allocator: PSRAM, with a cap per app (returning NULL makes Lua raise "not enough memory")
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    script_t *s = ud;
    if (ptr == NULL) {
        osize = 0;                                          // osize is a type tag for new blocks
    }
    if (nsize == 0) {
        heap_caps_free(ptr);
        s->mem_used -= osize;
        return NULL;
    }
    if (s->mem_used - osize + nsize > MEM_LIMIT) {
        return NULL;
    }
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        s->mem_used = s->mem_used - osize + nsize;
    }
    return p;
}

// Instruction-count hook: stops a callback that runs too long (an endless loop)
static void hook(lua_State *L, lua_Debug *ar)
{
    script_t *s = script_from_lua(L);
    if (++s->hook_ticks > HOOK_LIMIT) {
        luaL_error(L, "took too long (an endless loop?)");
    }
}

// Error handler for lua_pcall: the message with a stack traceback
static int on_error(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

static void unload(script_t *s)
{
    if (s->L) {
        lua_close(s->L);
        s->L = NULL;
    }
    s->generation = 0;                                      // answers for the old state are now stale
    s->net_pending = 0;
    cJSON_Delete(s->settings_values);
    s->settings_values = NULL;
    s->mem_used = 0;
}

// Records an error (full text to the log, the first lines for the screen) and unloads
static void fail(script_t *s, const char *msg)
{
    s->failed = true;
    snprintf(s->error, sizeof(s->error), "%s", msg ? msg : "unknown error");
    char *tb = strstr(s->error, "\nstack traceback:");      // keep the screen message short
    if (tb) {
        *tb = '\0';
    }
    char line[64];
    snprintf(line, sizeof(line), "error (Lua heap %u KB):", (unsigned)(s->mem_used / 1024));
    script_log(s, line);
    script_log(s, msg ? msg : "unknown error");
    unload(s);
}

// Sets a field of the table on top of the stack to nil
static void clear_field(lua_State *L, const char *name)
{
    lua_pushnil(L);
    lua_setfield(L, -2, name);
}

static bool load(script_t *s)
{
    unload(s);
    s->failed = false;
    s->error[0] = '\0';
    lua_State *L = lua_newstate(l_alloc, s);
    if (L == NULL) {
        fail(s, "not enough memory to start");
        return false;
    }
    *(script_t **)lua_getextraspace(L) = s;
    s->L = L;
    s->generation = next_generation++;

    // The standard libraries apps may use (no io, os or debug: files go through `storage`)
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_LOADLIBNAME, luaopen_package},
        {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
    lua_pushglobaltable(L);
    clear_field(L, "dofile");                               // file access only through storage/require
    clear_field(L, "loadfile");
    lua_pop(L, 1);
    lua_getglobal(L, LUA_LOADLIBNAME);                      // require() finds the app's own modules
    lua_pushfstring(L, "%s/?.lua", s->dir);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    clear_field(L, "loadlib");
    lua_pop(L, 1);

    char path[64];
    snprintf(path, sizeof(path), "%s/settings.json", s->dir);
    s->settings_values = read_json(path);
    script_api_open(L);
    script_json_open(L);
    script_net_open(L);
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);

    snprintf(path, sizeof(path), "%s/main.lua", s->dir);
    lua_pushcfunction(L, on_error);
    s->hook_ticks = 0;
    int status = luaL_loadfilex(L, path, "t");              // text only: no precompiled chunks
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, -2);
    }
    if (status != LUA_OK) {
        fail(s, lua_tostring(L, -1));
        return false;
    }
    lua_pop(L, 1);
    ESP_LOGI(TAG, "%s loaded (%u KB)", s->id, (unsigned)(s->mem_used / 1024));
    return true;
}

// Calls the function below the top `nargs` values, under the error handler and the
// instruction limit. If it returns a boolean, *result gets it. On an error, records it,
// unloads the app and returns false.
static bool pcall_top(script_t *s, int nargs, bool *result)
{
    lua_State *L = s->L;
    int fn = lua_gettop(L) - nargs;
    lua_pushcfunction(L, on_error);
    lua_insert(L, fn);                                      // handler just below the function
    s->hook_ticks = 0;
    if (lua_pcall(L, nargs, 1, fn) != LUA_OK) {
        fail(s, lua_tostring(L, -1));                       // unloads: no settop on a closed state
        return false;
    }
    if (result && lua_isboolean(L, -1)) {
        *result = lua_toboolean(L, -1);
    }
    lua_settop(L, fn - 1);
    return true;
}

// Calls the app's global function `fn` with an optional string argument. Missing functions
// are fine (returns true, *result untouched).
static bool call(script_t *s, const char *fn, const char *arg, bool *result)
{
    if (s->L == NULL) {
        return false;
    }
    if (lua_getglobal(s->L, fn) != LUA_TFUNCTION) {
        lua_pop(s->L, 1);
        return true;
    }
    if (arg) {
        lua_pushstring(s->L, arg);
    }
    return pcall_top(s, arg ? 1 : 0, result);
}

// ---------- error screen ----------

static void show_error(script_t *s)
{
    s->error_on_screen = true;
    epd_init();
    epd_clear(EPD_WHITE);
    ui_sidebar(s->name, NULL);
    const int x = UI_CONTENT_X + 6;
    font16_draw_text(x, 4, s->broken ? "This app can't run" : "This app stopped", EPD_BLACK);

    // The message in the small font, wrapped to the content width, at most 8 lines
    const int per_line = (EPD_WIDTH - x - 4) / 6;
    const char *p = s->error;
    char line[64];
    for (int row = 0; row < 8 && *p; row++) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < per_line && n < (int)sizeof(line) - 1) {
            n++;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        epd_draw_text(x, 26 + row * 10, line, 1, EPD_BLACK);
        p += n;
        p += *p == '\n';
    }
    epd_draw_text(x, EPD_HEIGHT - 11, "Any button: back to the menu", 1, EPD_BLACK);
    epd_display();
}

script_t *script_live(const char *id, uint32_t generation)
{
    for (int i = 0; i < script_count; i++) {
        if (scripts[i]->L && scripts[i]->generation == generation && strcmp(scripts[i]->id, id) == 0) {
            return scripts[i];
        }
    }
    return NULL;
}

void script_deliver(script_t *s, int nargs)
{
    if (!pcall_top(s, nargs, NULL) && app_is_showing(&s->app) && !s->error_on_screen) {
        show_error(s);
    }
    if (s->close_requested && app_is_showing(&s->app)) {
        buttons_post(BUTTON_WAKE);                          // app.close() from a callback: leave
    }
}

// ---------- the app_t callbacks ----------

static script_t *S(const app_t *self)
{
    return self->ctx;
}

static void script_draw_icon(const app_t *self, int cx, int cy)
{
    script_t *s = S(self);
    int x0 = cx - 16, y0 = cy - 16;
    if (s->has_icon) {
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                if (s->icon[y * 4 + x / 8] & (0x80 >> (x % 8))) {
                    epd_draw_pixel(x0 + x, y0 + y, EPD_BLACK);
                }
            }
        }
    } else {                                                // no icon: the name's first letter in a box
        epd_draw_rect(x0 + 2, y0 + 2, 28, 28, EPD_BLACK, false);
        char letter[2] = {(char)toupper((unsigned char)s->name[0]), '\0'};
        epd_draw_text(cx - epd_text_width(letter, 3) / 2, cy - 10, letter, 3, EPD_BLACK);
    }
    if (s->failed || s->broken) {                           // "!" badge: open it to see the error
        epd_draw_rect(x0 + 24, y0, 10, 11, EPD_BLACK, true);
        epd_draw_text(x0 + 27, y0 + 2, "!", 1, EPD_WHITE);
    }
}

static void script_init(const app_t *self)
{
    script_t *s = S(self);
    if (s->background && !s->broken) {
        load(s);                                            // stays loaded for on_background()
    }
}

static void script_open(const app_t *self)
{
    script_t *s = S(self);
    s->error_on_screen = false;
    s->close_requested = false;
    s->wake_set = false;
    if (s->broken || (s->L == NULL && !load(s)) || !call(s, "on_open", NULL, NULL)) {
        show_error(s);
    }
}

static const char *button_name(button_event_t event)
{
    switch (event) {
    case BUTTON_LEFT:       return "left";
    case BUTTON_RIGHT:      return "right";
    case BUTTON_SELECT:     return "select";
    case BUTTON_BACK:       return "back";
    case BUTTON_LEFT_HOLD:  return "left_hold";
    case BUTTON_RIGHT_HOLD: return "right_hold";
    case BUTTON_SELECT_HOLD: return "select_hold";
    default:                return NULL;
    }
}

static bool script_button(const app_t *self, button_event_t event)
{
    script_t *s = S(self);
    if (s->error_on_screen || s->close_requested) {
        return false;                                       // leave (app.close() posts a WAKE to get here)
    }
    const char *name = button_name(event);
    if (name == NULL) {
        return true;
    }
    if (event == BUTTON_SELECT_HOLD) {
        // An app that handles the hold returns true; anything else, and it's a plain press
        bool handled = false;
        if (!call(s, "on_button", name, &handled)) {
            show_error(s);
            return true;
        }
        if (handled || s->close_requested) {
            return !s->close_requested;
        }
        name = "select";
        event = BUTTON_SELECT;
    }
    bool keep = event != BUTTON_BACK;                       // without on_button, BACK leaves
    if (!call(s, "on_button", name, &keep)) {
        show_error(s);
        return true;                                        // the next press leaves
    }
    return keep && !s->close_requested;
}

static TickType_t script_next_wakeup(const app_t *self)
{
    script_t *s = S(self);
    return s->wake_set && !s->error_on_screen ? app_ticks_until(s->wake_at) : portMAX_DELAY;
}

static void script_tick(const app_t *self)
{
    script_t *s = S(self);
    if (!s->wake_set || !app_due(s->wake_at) || s->error_on_screen) {
        return;                                             // called early: not time yet
    }
    s->wake_set = false;
    if (!call(s, "on_tick", NULL, NULL)) {
        show_error(s);
    }
}

static void script_background(const app_t *self)
{
    script_t *s = S(self);
    if (!s->background || s->broken || s->failed) {
        return;                                             // a failed app reloads when opened
    }
    if (s->L == NULL && !load(s)) {
        return;
    }
    call(s, "on_background", NULL, NULL);                   // errors go to the log
}

static void script_close(const app_t *self)
{
    script_t *s = S(self);
    if (s->L) {
        call(s, "on_close", NULL, NULL);
    }
    if (!s->background) {
        unload(s);                                          // free the memory until it's opened again
    }
    s->error_on_screen = false;
    s->close_requested = false;
    s->wake_set = false;
}

// ---------- scanning ----------

static void free_script(script_t *s)
{
    unload(s);
    cJSON_Delete(s->settings_schema);
    heap_caps_free(s);
}

// Reads /apps/<id>/app.json into a new script_t. NULL if there's no usable manifest at all.
static script_t *read_app(const char *id)
{
    char path[64];
    snprintf(path, sizeof(path), BASE "/%.24s/app.json", id);
    cJSON *m = read_json(path);
    if (m == NULL) {
        ESP_LOGW(TAG, "%s: no readable app.json, skipped", id);
        return NULL;
    }
    script_t *s = heap_caps_calloc(1, sizeof(script_t), MALLOC_CAP_SPIRAM);
    if (s == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    snprintf(s->id, sizeof(s->id), "%s", id);
    snprintf(s->dir, sizeof(s->dir), BASE "/%s", id);
    const cJSON *name = cJSON_GetObjectItem(m, "name");
    const cJSON *version = cJSON_GetObjectItem(m, "version");
    const cJSON *api = cJSON_GetObjectItem(m, "api");
    snprintf(s->name, sizeof(s->name), "%s", cJSON_IsString(name) ? name->valuestring : id);
    snprintf(s->version, sizeof(s->version), "%s", cJSON_IsString(version) ? version->valuestring : "");
    s->background = cJSON_IsTrue(cJSON_GetObjectItem(m, "background"));
    s->settings_schema = cJSON_DetachItemFromObject(m, "settings");
    if (!cJSON_IsNumber(api) || api->valueint > APP_SCRIPT_API_VERSION) {
        s->broken = true;
        snprintf(s->error, sizeof(s->error), "Needs a newer firmware (app API %d, this firmware %d).",
                 cJSON_IsNumber(api) ? api->valueint : 0, APP_SCRIPT_API_VERSION);
    }
    cJSON_Delete(m);

    snprintf(path, sizeof(path), BASE "/%.24s/icon.bin", id);
    size_t len = 0;
    char *icon = read_file(path, SCRIPT_ICON_BYTES, &len);
    if (icon && len == SCRIPT_ICON_BYTES) {
        memcpy(s->icon, icon, SCRIPT_ICON_BYTES);
        s->has_icon = true;
    }
    free(icon);

    s->app = (app_t){
        .id = s->id,
        .name = s->name,
        .draw_icon = script_draw_icon,
        .init = script_init,
        .open = script_open,
        .button = script_button,
        .select_hold = true,                                // script_button falls back to "select"
        .next_wakeup = script_next_wakeup,
        .tick = script_tick,
        .background = script_background,
        .close = script_close,
        .ctx = s,
    };
    return s;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp((*(script_t *const *)a)->name, (*(script_t *const *)b)->name);
}

// Adds every installed app to the registry. `init`: also call their init (after start-up,
// when main.c has already initialized the apps it knows about).
static void scan(bool init)
{
    DIR *d = opendir(BASE);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && script_count < APP_REGISTRY_MAX - 8) {
        if (e->d_type != DT_DIR || !app_script_valid_id(e->d_name)) {
            continue;                                       // e.g. .staging
        }
        script_t *s = read_app(e->d_name);
        if (s) {
            scripts[script_count++] = s;
        }
    }
    closedir(d);
    qsort(scripts, script_count, sizeof(scripts[0]), by_name);
    for (int i = 0; i < script_count; i++) {
        app_registry_add(&scripts[i]->app, false);
        if (init) {
            script_init(&scripts[i]->app);
        }
        ESP_LOGI(TAG, "%s %s (%s)%s", scripts[i]->id, scripts[i]->version, scripts[i]->name,
                 scripts[i]->broken ? ": needs a newer firmware" : "");
    }
}

void app_script_rescan(void)
{
    for (int i = 0; i < script_count; i++) {
        app_registry_remove(&scripts[i]->app);
        free_script(scripts[i]);
    }
    script_count = 0;
    scan(true);
}

// ---------- bundled apps ----------
// Apps built into the firmware (from the repository's apps/ folder, see CMakeLists.txt).
// Each is installed once; uninstalling it keeps it gone. A firmware with a newer version
// updates an installed copy (program files only: settings and data stay).

extern const uint8_t clock_app_json[] asm("_binary_clock_app_json_start");
extern const uint8_t clock_app_json_end[] asm("_binary_clock_app_json_end");
extern const uint8_t clock_main_lua[] asm("_binary_clock_main_lua_start");
extern const uint8_t clock_main_lua_end[] asm("_binary_clock_main_lua_end");
extern const uint8_t clock_icon_bin[] asm("_binary_clock_icon_bin_start");
extern const uint8_t clock_icon_bin_end[] asm("_binary_clock_icon_bin_end");
extern const uint8_t weather_app_json[] asm("_binary_weather_app_json_start");
extern const uint8_t weather_app_json_end[] asm("_binary_weather_app_json_end");
extern const uint8_t weather_main_lua[] asm("_binary_weather_main_lua_start");
extern const uint8_t weather_main_lua_end[] asm("_binary_weather_main_lua_end");
extern const uint8_t weather_icon_bin[] asm("_binary_weather_icon_bin_start");
extern const uint8_t weather_icon_bin_end[] asm("_binary_weather_icon_bin_end");

typedef struct {
    const char *name;
    const uint8_t *start, *end;
} bundled_file_t;

typedef struct {
    const char *id;
    bundled_file_t files[3];            // app.json first
} bundled_app_t;

static const bundled_app_t bundled[] = {
    {"clock", {{"app.json", clock_app_json, clock_app_json_end},
               {"main.lua", clock_main_lua, clock_main_lua_end},
               {"icon.bin", clock_icon_bin, clock_icon_bin_end}}},
    {"weather", {{"app.json", weather_app_json, weather_app_json_end},
                 {"main.lua", weather_main_lua, weather_main_lua_end},
                 {"icon.bin", weather_icon_bin, weather_icon_bin_end}}},
};
#define SEEDED_KEY "apps_seeded"         // bit i set = bundled[i] was installed once

// Version in an app.json held in memory
static void manifest_version(const uint8_t *start, const uint8_t *end, char *out, size_t size)
{
    cJSON *m = cJSON_ParseWithLength((const char *)start, end - start);
    const cJSON *v = cJSON_GetObjectItem(m, "version");
    snprintf(out, size, "%s", cJSON_IsString(v) ? v->valuestring : "0");
    cJSON_Delete(m);
}

static void install_bundled(void)
{
    int32_t seeded = settings_get_i32(SEEDED_KEY, 0);
    for (int i = 0; i < (int)(sizeof(bundled) / sizeof(bundled[0])); i++) {
        const bundled_app_t *b = &bundled[i];
        char dir[40], path[72], have[16] = "", ships[16];
        snprintf(dir, sizeof(dir), BASE "/%s", b->id);
        bool installed = is_dir(dir);
        manifest_version(b->files[0].start, b->files[0].end, ships, sizeof(ships));
        if (installed) {
            snprintf(path, sizeof(path), "%s/app.json", dir);
            cJSON *m = read_json(path);
            const cJSON *v = cJSON_GetObjectItem(m, "version");
            snprintf(have, sizeof(have), "%s", cJSON_IsString(v) ? v->valuestring : "0");
            cJSON_Delete(m);
        }
        bool first_time = !(seeded & (1 << i)) && !installed;
        bool newer = installed && compare_versions(ships, have) > 0;
        if (first_time || newer) {
            mkdir(dir, 0775);
            for (int f = 0; f < 3; f++) {
                snprintf(path, sizeof(path), "%s/%s", dir, b->files[f].name);
                write_file(path, b->files[f].start, b->files[f].end - b->files[f].start);
            }
            ESP_LOGI(TAG, "%s %s %s", first_time ? "installed bundled app" : "updated bundled app",
                     b->id, ships);
        }
        seeded |= 1 << i;
    }
    settings_set_i32(SEEDED_KEY, seeded);
}

void app_script_init(void)
{
    log_lock = xSemaphoreCreateMutex();
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,                     // first boot: blank partition
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    wl_handle_t wl;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE, PARTITION, &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return;
    }
    mounted = true;
    install_bundled();
    scan(false);                                            // main.c calls each app's init
}

// ---------- installing (web portal task) ----------

esp_err_t app_script_stage(const char *id, const char *file, uint32_t offset, uint32_t total,
                           const void *data, size_t len, bool fresh, uint32_t *have)
{
    *have = 0;
    if (!mounted || !app_script_valid_id(id) || !script_valid_file(file) ||
        total == 0 || total > APP_SCRIPT_FILE_MAX || offset + len > total) {
        return ESP_ERR_INVALID_ARG;
    }
    char dir[48], path[96];
    snprintf(dir, sizeof(dir), STAGING "/%s", id);
    if (fresh && offset == 0) {
        delete_files(dir, NULL);                            // a new package: forget earlier attempts
    }
    mkdir(STAGING, 0775);
    mkdir(dir, 0775);
    snprintf(path, sizeof(path), "%s/%s", dir, file);

    struct stat st;
    uint32_t size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    if (offset != 0 && offset != size) {
        *have = size;
        return ESP_ERR_INVALID_STATE;                       // out of step: resume from `have`
    }
    FILE *f = fopen(path, offset == 0 ? "wb" : "ab");
    if (f == NULL) {
        return ESP_FAIL;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    if (!ok) {
        return ESP_ERR_NO_MEM;                              // partition full
    }
    *have = offset + len;
    return ESP_OK;
}

esp_err_t app_script_install(const char *id, char *err, size_t err_size)
{
    char staged[48], dest[40], path[320], from[320];       // folders + long file names
    snprintf(staged, sizeof(staged), STAGING "/%s", id);
    snprintf(dest, sizeof(dest), BASE "/%s", id);
    snprintf(err, err_size, "%s", "");
    if (!mounted || !app_script_valid_id(id)) {
        snprintf(err, err_size, "bad app id");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(path, sizeof(path), "%s/app.json", staged);
    cJSON *m = read_json(path);
    const cJSON *mid = cJSON_GetObjectItem(m, "id"), *api = cJSON_GetObjectItem(m, "api");
    const cJSON *name = cJSON_GetObjectItem(m, "name");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    struct stat st;
    snprintf(from, sizeof(from), "%s/main.lua", staged);
    if (m == NULL) {
        snprintf(err, err_size, "app.json missing or not valid JSON");
    } else if (!cJSON_IsString(mid) || strcmp(mid->valuestring, id) != 0) {
        snprintf(err, err_size, "app.json id doesn't match \"%s\"", id);
    } else if (!cJSON_IsString(name) || !name->valuestring[0]) {
        snprintf(err, err_size, "app.json has no name");
    } else if (!cJSON_IsNumber(api) || api->valueint < 1) {
        snprintf(err, err_size, "app.json has no api version");
    } else if (api->valueint > APP_SCRIPT_API_VERSION) {
        snprintf(err, err_size, "needs API %d; this firmware has API %d", api->valueint, APP_SCRIPT_API_VERSION);
    } else if (stat(from, &st) != 0) {
        snprintf(err, err_size, "main.lua missing");
    } else {
        result = ESP_OK;
    }
    cJSON_Delete(m);
    if (result != ESP_OK) {
        return result;
    }

    // Replace the program files; settings.json and data/ stay
    mkdir(dest, 0775);
    delete_files(dest, "settings.json");
    DIR *d = opendir(staged);
    if (d == NULL) {
        snprintf(err, err_size, "nothing staged");
        return ESP_FAIL;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) {
            continue;
        }
        snprintf(from, sizeof(from), "%s/%s", staged, e->d_name);
        snprintf(path, sizeof(path), "%s/%s", dest, e->d_name);
        unlink(path);
        if (rename(from, path) != 0) {
            snprintf(err, err_size, "couldn't move %s", e->d_name);
            result = ESP_FAIL;
        }
    }
    closedir(d);
    rmdir(staged);
    ESP_LOGI(TAG, "installed %s", id);
    return result;
}

esp_err_t app_script_uninstall(const char *id)
{
    char dir[48];
    if (!mounted || !app_script_valid_id(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(dir, sizeof(dir), BASE "/%s", id);
    if (!is_dir(dir)) {
        return ESP_ERR_NOT_FOUND;
    }
    char data[56];
    snprintf(data, sizeof(data), "%s/data", dir);
    delete_files(data, NULL);
    rmdir(data);
    delete_files(dir, NULL);
    ESP_LOGI(TAG, "removed %s", id);
    return rmdir(dir) == 0 ? ESP_OK : ESP_FAIL;
}

// ---------- settings (web portal task) ----------

static cJSON *manifest_settings(const char *id)
{
    char path[64];
    snprintf(path, sizeof(path), BASE "/%.24s/app.json", id);
    cJSON *m = read_json(path);
    cJSON *schema = cJSON_DetachItemFromObject(m, "settings");
    cJSON_Delete(m);
    return cJSON_IsArray(schema) ? schema : (cJSON_Delete(schema), cJSON_CreateArray());
}

static const char *setting_type(const cJSON *item)
{
    const cJSON *t = cJSON_GetObjectItem(item, "type");
    return cJSON_IsString(t) ? t->valuestring : "string";
}

cJSON *app_script_settings_json(const char *id)
{
    char path[64];
    snprintf(path, sizeof(path), BASE "/%.24s", id);
    if (!mounted || !app_script_valid_id(id) || !is_dir(path)) {
        return NULL;
    }
    cJSON *schema = manifest_settings(id);
    snprintf(path, sizeof(path), BASE "/%.24s/settings.json", id);
    cJSON *saved = read_json(path);
    cJSON *values = cJSON_CreateObject(), *secrets = cJSON_CreateArray();
    const cJSON *item;
    cJSON_ArrayForEach(item, schema) {
        const cJSON *key = cJSON_GetObjectItem(item, "key");
        const cJSON *v = cJSON_IsString(key) ? cJSON_GetObjectItem(saved, key->valuestring) : NULL;
        if (v == NULL) {
            continue;
        }
        if (strcmp(setting_type(item), "secret") == 0) {
            if (cJSON_IsString(v) && v->valuestring[0]) {
                cJSON_AddItemToArray(secrets, cJSON_CreateString(key->valuestring));   // set, but never sent back
            }
        } else {
            cJSON_AddItemToObject(values, key->valuestring, cJSON_Duplicate(v, true));
        }
    }
    cJSON_Delete(saved);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "schema", schema);
    cJSON_AddItemToObject(out, "values", values);
    cJSON_AddItemToObject(out, "secrets_set", secrets);
    return out;
}

// True if `v` is one of a choice setting's options (strings, or {"value": ..., "label": ...})
static bool is_option(const cJSON *item, const char *v)
{
    const cJSON *opt;
    cJSON_ArrayForEach(opt, cJSON_GetObjectItem(item, "options")) {
        const cJSON *value = cJSON_IsObject(opt) ? cJSON_GetObjectItem(opt, "value") : opt;
        if (cJSON_IsString(value) && strcmp(value->valuestring, v) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t app_script_save_settings(const char *id, const cJSON *values, char *err, size_t err_size)
{
    char path[64];
    snprintf(err, err_size, "%s", "");
    snprintf(path, sizeof(path), BASE "/%.24s", id);
    if (!mounted || !app_script_valid_id(id) || !is_dir(path)) {
        snprintf(err, err_size, "no such app");
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_IsObject(values)) {
        snprintf(err, err_size, "expected an object of settings");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *schema = manifest_settings(id);
    snprintf(path, sizeof(path), BASE "/%.24s/settings.json", id);
    cJSON *saved = read_json(path);
    if (!cJSON_IsObject(saved)) {
        cJSON_Delete(saved);
        saved = cJSON_CreateObject();
    }
    esp_err_t result = ESP_OK;
    const cJSON *item;
    cJSON_ArrayForEach(item, schema) {
        const cJSON *key = cJSON_GetObjectItem(item, "key");
        if (!cJSON_IsString(key)) {
            continue;
        }
        const cJSON *v = cJSON_GetObjectItem(values, key->valuestring);
        if (v == NULL) {
            continue;                                       // not changed
        }
        const char *type = setting_type(item);
        cJSON *keep = NULL;
        if (strcmp(type, "bool") == 0 && cJSON_IsBool(v)) {
            keep = cJSON_CreateBool(cJSON_IsTrue(v));
        } else if (strcmp(type, "int") == 0 && cJSON_IsNumber(v) && v->valuedouble == (double)(long long)v->valuedouble) {
            const cJSON *min = cJSON_GetObjectItem(item, "min"), *max = cJSON_GetObjectItem(item, "max");
            if ((cJSON_IsNumber(min) && v->valuedouble < min->valuedouble) ||
                (cJSON_IsNumber(max) && v->valuedouble > max->valuedouble)) {
                snprintf(err, err_size, "%s is out of range", key->valuestring);
                result = ESP_ERR_INVALID_ARG;
                break;
            }
            keep = cJSON_CreateNumber(v->valuedouble);
        } else if ((strcmp(type, "string") == 0 || strcmp(type, "secret") == 0) && cJSON_IsString(v) &&
                   strlen(v->valuestring) <= 512) {
            if (strcmp(type, "secret") == 0 && !v->valuestring[0]) {
                continue;                                   // empty: keep the secret already saved
            }
            keep = cJSON_CreateString(v->valuestring);
        } else if (strcmp(type, "choice") == 0 && cJSON_IsString(v) && is_option(item, v->valuestring)) {
            keep = cJSON_CreateString(v->valuestring);
        } else {
            snprintf(err, err_size, "%s: not a valid %s", key->valuestring, type);
            result = ESP_ERR_INVALID_ARG;
            break;
        }
        cJSON_DeleteItemFromObject(saved, key->valuestring);
        cJSON_AddItemToObject(saved, key->valuestring, keep);
    }
    if (result == ESP_OK) {
        char *text = cJSON_PrintUnformatted(saved);
        if (text == NULL || !write_file(path, text, strlen(text))) {
            snprintf(err, err_size, "couldn't save (partition full?)");
            result = ESP_FAIL;
        }
        free(text);
    }
    cJSON_Delete(saved);
    cJSON_Delete(schema);
    return result;
}

cJSON *app_script_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    DIR *d = mounted ? opendir(BASE) : NULL;
    if (d == NULL) {
        return arr;
    }
    struct dirent *e;
    char path[64];
    static const char *const fields[] = {"name", "version", "author", "description", "api"};
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR || !app_script_valid_id(e->d_name)) {
            continue;
        }
        snprintf(path, sizeof(path), BASE "/%.24s/app.json", e->d_name);   // a valid id: <= 24 chars
        cJSON *m = read_json(path);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", e->d_name);
        for (size_t i = 0; m && i < sizeof(fields) / sizeof(fields[0]); i++) {
            const cJSON *v = cJSON_GetObjectItem(m, fields[i]);
            if (v) {
                cJSON_AddItemToObject(o, fields[i], cJSON_Duplicate(v, true));
            }
        }
        cJSON_Delete(m);
        cJSON_AddItemToArray(arr, o);
    }
    closedir(d);
    return arr;
}
