/*
 * The interface every app implements. main.c keeps a list of apps, draws them as menu
 * tiles, and forwards events to whichever one is on screen. See components/app_core/README.md.
 *
 * All callbacks run in the main task, so apps never need locks for their own state.
 * Slow work (network) goes to the worker (worker.h), whose results also arrive in the main task.
 */
#pragma once

#include <stdbool.h>
#include "buttons.h"                    // button_event_t
#include "freertos/FreeRTOS.h"          // TickType_t

typedef struct app {
    const char *name;                   // label on the menu tile

    // Draws the menu icon centered on (cx, cy) into the frame buffer (about 32x32 px)
    void (*draw_icon)(int cx, int cy);

    // Optional. Called once at power-on, before Wi-Fi: load files, allocate buffers.
    void (*init)(void);

    // The app was opened from the menu: draw its first screen with a full refresh
    void (*open)(void);

    // A button event while the app is on screen. HOME never arrives here (main handles it).
    // Return false to leave the app, which main does by showing the menu. Apps return
    // false for BACK on their top screen and true for everything else. BUTTON_WAKE (another
    // task asked for attention, e.g. the web server) also arrives here; ignore it if unused.
    bool (*button)(button_event_t event);

    // Optional. Ticks until the app wants tick() called while on screen, e.g. the clock's
    // next minute. Return portMAX_DELAY for "only when a button is pressed".
    TickType_t (*next_wakeup)(void);

    // Optional. Called when next_wakeup() expires. May also be called early; check the time.
    void (*tick)(void);

    // Optional. Called about every 30 s whether or not the app is on screen, for
    // background work such as starting a nightly download. Keep it quick.
    void (*background)(void);

    // Optional. The app is leaving the screen (BACK out of it, or MENU). Stop anything
    // that should only run while it's open, e.g. the Transfer app's web server.
    void (*close)(void);
} app_t;

// The app on screen, or NULL while the menu is showing (set by main.c)
const app_t *app_showing(void);
void app_set_showing(const app_t *app);

// True if `app` is on screen. Worker callbacks use this to decide whether to redraw.
bool app_is_showing(const app_t *app);

// True once `deadline` (a tick count) has passed, even across tick counter wrap-around
bool app_due(TickType_t deadline);

// Ticks from now until `deadline`, 0 if it has passed. Handy for next_wakeup().
TickType_t app_ticks_until(TickType_t deadline);
