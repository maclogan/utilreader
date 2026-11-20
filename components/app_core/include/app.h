/*
 * The interface every app implements, native (C, compiled in) or script (Lua, installed
 * through the web portal; see components/app_script). Apps are listed in the app registry
 * (app_registry.h); main.c draws them as menu tiles and forwards events to whichever one
 * is on screen. See components/app_core/README.md.
 *
 * Every callback gets the app itself (`self`), so one implementation can serve many apps:
 * the script host uses self->ctx to find which Lua app it's running.
 *
 * All callbacks run in the main task, so apps never need locks for their own state.
 * Slow work (network) goes to the worker (worker.h), whose results also arrive in the main task.
 */
#pragma once

#include <stdbool.h>
#include "buttons.h"                    // button_event_t
#include "freertos/FreeRTOS.h"          // TickType_t

typedef struct app app_t;

struct app {
    const char *id;                     // stable identifier ("story", or a script app's id);
                                        // remembered as the app to reopen at power-on
    const char *name;                   // label on the menu tile

    // Draws the menu icon centered on (cx, cy) into the frame buffer (about 32x32 px)
    void (*draw_icon)(const app_t *self, int cx, int cy);

    // Optional. Called once at power-on (or when installed), before Wi-Fi: load files,
    // allocate buffers.
    void (*init)(const app_t *self);

    // The app was opened from the menu: draw its first screen with a full refresh
    void (*open)(const app_t *self);

    // A button event while the app is on screen. HOME never arrives here (main handles it).
    // Return false to leave the app, which main does by showing the menu. Apps return
    // false for BACK on their top screen and true for everything else. BUTTON_WAKE (another
    // task asked for attention, e.g. the web server) also arrives here; ignore it if unused.
    bool (*button)(const app_t *self, button_event_t event);

    // Optional. Ticks until the app wants tick() called while on screen, e.g. the clock's
    // next minute. Return portMAX_DELAY for "only when a button is pressed".
    TickType_t (*next_wakeup)(const app_t *self);

    // Optional. Called when next_wakeup() expires. May also be called early; check the time.
    void (*tick)(const app_t *self);

    // Optional. Called about every 30 s whether or not the app is on screen, for
    // background work such as starting a nightly download. Keep it quick.
    void (*background)(const app_t *self);

    // Optional. The app is leaving the screen (BACK out of it, or HOME). Stop anything
    // that should only run while it's open, e.g. the Transfer app's web server.
    void (*close)(const app_t *self);

    // true: never reopened by itself at power-on (power-on shows the menu instead), e.g.
    // Transfer, whose web server should only run when opened on purpose
    bool no_resume;

    // true: the app gets BUTTON_SELECT_HOLD (center held). Otherwise main.c delivers a held
    // center as an ordinary BUTTON_SELECT.
    bool select_hold;

    void *ctx;                          // for the implementation (the script host's per-app state)
};

// The app on screen, or NULL while the menu is showing (set by main.c)
const app_t *app_showing(void);
void app_set_showing(const app_t *app);

// True if `app` is on screen. Worker callbacks use this to decide whether to redraw.
bool app_is_showing(const app_t *app);

// True once `deadline` (a tick count) has passed, even across tick counter wrap-around
bool app_due(TickType_t deadline);

// Ticks from now until `deadline`, 0 if it has passed. Handy for next_wakeup().
TickType_t app_ticks_until(TickType_t deadline);
