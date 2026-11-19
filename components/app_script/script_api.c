/*
 * The API script apps program against (API version 1; documented in docs/api.md).
 * Changes within a version may only add things: an app written for API 1 must keep working.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "app_script.h"                 // APP_SCRIPT_API_VERSION
#include "battery.h"
#include "buttons.h"                    // buttons_post(): app.close() wakes the main loop
#include "epd.h"
#include "font16.h"
#include "lauxlib.h"
#include "local_time.h"
#include "script_internal.h"
#include "ui_loading.h"
#include "ui_sidebar.h"

#define FILE_READ_MAX (256 * 1024)      // largest file storage.read() returns

// Optional color argument: 0 = black (the default), 1 = white
static uint8_t opt_color(lua_State *L, int idx, int fallback)
{
    return luaL_optinteger(L, idx, fallback) ? EPD_WHITE : EPD_BLACK;
}

static int int_arg(lua_State *L, int idx)
{
    return (int)luaL_checknumber(L, idx);                   // accept 3.0 as well as 3
}

// ---------- screen ----------

static int screen_begin(lua_State *L)
{
    script_t *s = script_from_lua(L);
    bool full = lua_toboolean(L, 1);
    if (!full && !epd_is_awake()) {
        full = true;                                        // a partial refresh needs an awake panel
    }
    if (full) {
        epd_init();
    }
    epd_clear(EPD_WHITE);
    s->frame_full = full;
    lua_pushboolean(L, full);
    return 1;
}

static int screen_finish(lua_State *L)
{
    if (script_from_lua(L)->frame_full) {
        epd_display();
    } else {
        epd_display_partial();
    }
    return 0;
}

static int screen_clear(lua_State *L)
{
    epd_clear(opt_color(L, 1, EPD_WHITE));
    return 0;
}

static int screen_pixel(lua_State *L)
{
    epd_draw_pixel(int_arg(L, 1), int_arg(L, 2), opt_color(L, 3, EPD_BLACK));
    return 0;
}

static int screen_line(lua_State *L)
{
    epd_draw_line(int_arg(L, 1), int_arg(L, 2), int_arg(L, 3), int_arg(L, 4), opt_color(L, 5, EPD_BLACK));
    return 0;
}

static int screen_rect(lua_State *L)
{
    epd_draw_rect(int_arg(L, 1), int_arg(L, 2), int_arg(L, 3), int_arg(L, 4),
                  opt_color(L, 6, EPD_BLACK), lua_toboolean(L, 5));
    return 0;
}

static int screen_circle(lua_State *L)
{
    epd_fill_circle(int_arg(L, 1), int_arg(L, 2), int_arg(L, 3), opt_color(L, 4, EPD_BLACK));
    return 0;
}

static int screen_text(lua_State *L)
{
    int scale = (int)luaL_optinteger(L, 4, 1);
    luaL_argcheck(L, scale >= 1 && scale <= 16, 4, "scale must be 1-16");
    lua_pushinteger(L, epd_draw_text(int_arg(L, 1), int_arg(L, 2), luaL_checkstring(L, 3), scale,
                                     opt_color(L, 5, EPD_BLACK)));
    return 1;
}

static int screen_text_width(lua_State *L)
{
    lua_pushinteger(L, epd_text_width(luaL_checkstring(L, 1), (int)luaL_optinteger(L, 2, 1)));
    return 1;
}

static int screen_text16(lua_State *L)
{
    lua_pushinteger(L, font16_draw_text(int_arg(L, 1), int_arg(L, 2), luaL_checkstring(L, 3),
                                        opt_color(L, 4, EPD_BLACK)));
    return 1;
}

static int screen_text16_width(lua_State *L)
{
    lua_pushinteger(L, font16_text_width(luaL_checkstring(L, 1)));
    return 1;
}

// bitmap(x, y, w, h, data[, color]): data has ceil(w/8) bytes per row, MSB = leftmost, 1 = ink
static int screen_bitmap(lua_State *L)
{
    int x0 = int_arg(L, 1), y0 = int_arg(L, 2), w = int_arg(L, 3), h = int_arg(L, 4);
    size_t len;
    const uint8_t *bits = (const uint8_t *)luaL_checklstring(L, 5, &len);
    uint8_t color = opt_color(L, 6, EPD_BLACK);
    int row_bytes = (w + 7) / 8;
    luaL_argcheck(L, w > 0 && h > 0 && len >= (size_t)row_bytes * h, 5, "data too short for w x h");
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (bits[y * row_bytes + x / 8] & (0x80 >> (x % 8))) {
                epd_draw_pixel(x0 + x, y0 + y, color);
            }
        }
    }
    return 0;
}

static const luaL_Reg screen_funcs[] = {
    {"begin", screen_begin}, {"finish", screen_finish}, {"clear", screen_clear},
    {"pixel", screen_pixel}, {"line", screen_line}, {"rect", screen_rect},
    {"circle", screen_circle}, {"text", screen_text}, {"text_width", screen_text_width},
    {"text16", screen_text16}, {"text16_width", screen_text16_width}, {"bitmap", screen_bitmap},
    {NULL, NULL},
};

// ---------- ui ----------

static int ui_sidebar_fn(lua_State *L)
{
    ui_sidebar(luaL_checkstring(L, 1), luaL_optstring(L, 2, NULL));
    return 0;
}

static int ui_loading_fn(lua_State *L)
{
    ui_show_loading();
    return 0;
}

static const luaL_Reg ui_funcs[] = {
    {"sidebar", ui_sidebar_fn}, {"loading", ui_loading_fn}, {NULL, NULL},
};

// ---------- app ----------

static int app_wake_in(lua_State *L)
{
    script_t *s = script_from_lua(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    s->wake_set = ms >= 0;
    if (s->wake_set) {
        s->wake_at = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    }
    return 0;
}

static int app_close(lua_State *L)
{
    script_t *s = script_from_lua(L);
    s->close_requested = true;
    buttons_post(BUTTON_WAKE);                              // the host leaves on the next event
    return 0;
}

static const luaL_Reg app_funcs[] = {
    {"wake_in", app_wake_in}, {"close", app_close}, {NULL, NULL},
};

// ---------- time ----------

static int time_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)time(NULL));
    return 1;
}

// date([utc]) -> local {year, month, day, hour, min, sec, wday (1 = Sunday)}, or nil while the
// clock isn't set. (Not "local": that's a Lua keyword.)
static int time_date(lua_State *L)
{
    time_t t = (time_t)luaL_optinteger(L, 1, (lua_Integer)time(NULL));
    if (!local_time_synced()) {
        lua_pushnil(L);
        return 1;
    }
    t += local_time_offset();
    struct tm tm;
    gmtime_r(&t, &tm);
    lua_createtable(L, 0, 7);
    static const char *const keys[] = {"year", "month", "day", "hour", "min", "sec", "wday"};
    int values[] = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday + 1};
    for (int i = 0; i < 7; i++) {
        lua_pushinteger(L, values[i]);
        lua_setfield(L, -2, keys[i]);
    }
    return 1;
}

static int time_synced(lua_State *L)
{
    lua_pushboolean(L, local_time_synced());
    return 1;
}

static int time_format_hm(lua_State *L)
{
    char hm[16];
    if (local_time_format_hm((time_t)luaL_optinteger(L, 1, (lua_Integer)time(NULL)), hm, sizeof(hm))) {
        lua_pushstring(L, hm);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int time_zone(lua_State *L)
{
    lua_pushinteger(L, local_time_offset());
    return 1;
}

static int time_set_zone(lua_State *L)
{
    local_time_set_offset((int)luaL_checkinteger(L, 1));
    return 0;
}

static const luaL_Reg time_funcs[] = {
    {"now", time_now}, {"date", time_date}, {"synced", time_synced},
    {"format_hm", time_format_hm}, {"zone", time_zone}, {"set_zone", time_set_zone},
    {NULL, NULL},
};

// ---------- battery ----------

static int battery_percent_fn(lua_State *L)
{
    int p = battery_percent();
    if (p < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, p);
    }
    return 1;
}

static int battery_state_fn(lua_State *L)
{
    static const char *const names[] = {"none", "discharging", "charging", "full"};
    lua_pushstring(L, names[battery_state()]);
    return 1;
}

static int battery_millivolts_fn(lua_State *L)
{
    lua_pushinteger(L, battery_millivolts());
    return 1;
}

static const luaL_Reg battery_funcs[] = {
    {"percent", battery_percent_fn}, {"state", battery_state_fn},
    {"millivolts", battery_millivolts_fn}, {NULL, NULL},
};

// ---------- storage: the app's own files in /apps/<id>/data ----------

static void data_path(lua_State *L, int idx, char *out, size_t size)
{
    const char *name = luaL_checkstring(L, idx);
    luaL_argcheck(L, script_valid_file(name), idx, "file names: letters, digits, . _ - (up to 32)");
    snprintf(out, size, "%s/data/%s", script_from_lua(L)->dir, name);
}

static int storage_read(lua_State *L)
{
    char path[96];
    data_path(L, 1, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        lua_pushnil(L);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > FILE_READ_MAX) {
        fclose(f);
        return luaL_error(L, "file too big to read");
    }
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, len);
    size_t got = fread(p, 1, len, f);
    fclose(f);
    luaL_pushresultsize(&b, got);
    return 1;
}

static int storage_write(lua_State *L)
{
    char path[96], dir[56];
    data_path(L, 1, path, sizeof(path));
    size_t len;
    const char *text = luaL_checklstring(L, 2, &len);
    snprintf(dir, sizeof(dir), "%s/data", script_from_lua(L)->dir);
    mkdir(dir, 0775);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f) {
        fclose(f);
    }
    lua_pushboolean(L, ok);
    return 1;
}

static int storage_remove(lua_State *L)
{
    char path[96];
    data_path(L, 1, path, sizeof(path));
    lua_pushboolean(L, remove(path) == 0);
    return 1;
}

static int storage_list(lua_State *L)
{
    char dir[56];
    snprintf(dir, sizeof(dir), "%s/data", script_from_lua(L)->dir);
    lua_newtable(L);
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 1;
    }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR) {
            lua_pushstring(L, e->d_name);
            lua_rawseti(L, -2, ++n);
        }
    }
    closedir(d);
    return 1;
}

static const luaL_Reg storage_funcs[] = {
    {"read", storage_read}, {"write", storage_write}, {"remove", storage_remove},
    {"list", storage_list}, {NULL, NULL},
};

// ---------- settings: values the user set in the portal, else the manifest's defaults ----------

static void push_json_value(lua_State *L, const cJSON *v)
{
    if (cJSON_IsBool(v)) {
        lua_pushboolean(L, cJSON_IsTrue(v));
    } else if (cJSON_IsNumber(v)) {
        if (v->valuedouble == (double)(lua_Integer)v->valuedouble) {
            lua_pushinteger(L, (lua_Integer)v->valuedouble);
        } else {
            lua_pushnumber(L, v->valuedouble);
        }
    } else if (cJSON_IsString(v)) {
        lua_pushstring(L, v->valuestring);
    } else {
        lua_pushnil(L);
    }
}

static int settings_get(lua_State *L)
{
    script_t *s = script_from_lua(L);
    const char *key = luaL_checkstring(L, 1);
    const cJSON *v = s->settings_values ? cJSON_GetObjectItem(s->settings_values, key) : NULL;
    if (v == NULL) {
        const cJSON *item;
        cJSON_ArrayForEach(item, s->settings_schema) {
            const cJSON *k = cJSON_GetObjectItem(item, "key");
            if (cJSON_IsString(k) && strcmp(k->valuestring, key) == 0) {
                v = cJSON_GetObjectItem(item, "default");
                break;
            }
        }
    }
    push_json_value(L, v);
    return 1;
}

static const luaL_Reg settings_funcs[] = {
    {"get", settings_get}, {NULL, NULL},
};

// ---------- log / print ----------

static int log_fn(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);                         // pushes the text...
        luaL_addvalue(&b);                                  // ...and moves it into the buffer
    }
    luaL_pushresult(&b);
    script_log(script_from_lua(L), lua_tostring(L, -1));
    return 0;
}

// ---------- registration ----------

static void add_module(lua_State *L, const char *name, const luaL_Reg *funcs)
{
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_setglobal(L, name);
}

void script_api_open(lua_State *L)
{
    script_t *s = script_from_lua(L);

    add_module(L, "screen", screen_funcs);
    lua_getglobal(L, "screen");
    lua_pushinteger(L, EPD_WIDTH);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, EPD_HEIGHT);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "BLACK");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "WHITE");
    lua_pop(L, 1);

    add_module(L, "ui", ui_funcs);
    lua_getglobal(L, "ui");
    lua_pushinteger(L, UI_CONTENT_X);
    lua_setfield(L, -2, "content_x");
    lua_pushinteger(L, UI_CONTENT_W);
    lua_setfield(L, -2, "content_w");
    lua_pop(L, 1);

    add_module(L, "app", app_funcs);
    lua_getglobal(L, "app");
    lua_pushstring(L, s->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, s->name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, s->version);
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, APP_SCRIPT_API_VERSION);
    lua_setfield(L, -2, "api");
    lua_pop(L, 1);

    add_module(L, "time", time_funcs);
    add_module(L, "battery", battery_funcs);
    add_module(L, "storage", storage_funcs);
    add_module(L, "settings", settings_funcs);
    lua_register(L, "print", log_fn);
    lua_register(L, "log", log_fn);
}
