/*
 * Shared between the script host (script_host.c) and the Lua API (script_api.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app.h"
#include "cJSON.h"
#include "lua.h"

#define SCRIPT_ICON_BYTES   128         // 32x32, 1 bit per pixel

// One installed script app
typedef struct script {
    app_t app;                          // what the registry and main.c see; app.ctx points back here
    char id[25];
    char name[32];
    char version[16];
    char dir[40];                       // "/apps/<id>"
    bool background;                    // manifest: stay loaded, on_background() every 30 s
    bool broken;                        // the manifest itself is unusable (e.g. API too new): never load
    bool has_icon;
    uint8_t icon[SCRIPT_ICON_BYTES];    // rows top to bottom, 4 bytes each, MSB = leftmost, 1 = black
    cJSON *settings_schema;             // manifest "settings" array (NULL if none)
    cJSON *settings_values;             // settings.json, read when the app loads (NULL if none)

    lua_State *L;                       // NULL while not loaded
    size_t mem_used;                    // bytes the Lua state has allocated
    int hook_ticks;                     // instruction-count hook calls during the current callback
    bool failed;                        // the last load or callback raised an error...
    char error[200];                    // ...this one
    bool error_on_screen;               // the error screen is showing: any press leaves
    bool close_requested;               // app.close() was called
    bool wake_set;                      // app.wake_in() asked for on_tick() at wake_at
    TickType_t wake_at;
    bool frame_full;                    // what screen.begin() chose, for screen.finish()
} script_t;

// The app a Lua state belongs to (stored in the state's extra space)
static inline script_t *script_from_lua(lua_State *L)
{
    return *(script_t **)lua_getextraspace(L);
}

// Registers the API modules (screen, ui, app, time, battery, storage, settings, log, print)
void script_api_open(lua_State *L);

// Adds a line to the app's log (serial log + the ring buffer the portal shows)
void script_log(const script_t *s, const char *text);

// True for a plain file name an app may use: [A-Za-z0-9._-], 1-32 characters, no leading dot
bool script_valid_file(const char *name);
