/*
 * The controls, wired to the 14-pin header (P2) of the Waveshare ESP32-S3-LR1121-ePaper-2.9:
 * a 5-way tact switch with only left / right / center connected (through the I2C I/O
 * expander, expander.h), one push button (BACK / HOME), and a lock slide switch. A background task watches them and turns
 * presses into events. The event names describe what they mean to apps
 * (previous / next / select / back / home):
 *
 *   5-way left   tap -> BUTTON_LEFT     hold -> BUTTON_LEFT_HOLD
 *   5-way right  tap -> BUTTON_RIGHT    hold -> BUTTON_RIGHT_HOLD
 *   5-way center tap -> BUTTON_SELECT   hold -> BUTTON_SELECT_HOLD
 *   button       tap -> BUTTON_BACK     hold -> BUTTON_HOME
 *
 * The board's own KEY button does the same as the BACK / HOME button (handy before anything
 * is wired). Taps on controls with a hold gesture arrive when they're released. While the
 * lock switch is on, presses are ignored. The BACK / HOME button also
 * turns the device on (see power.h). Wiring is in README.md.
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"          // TickType_t

typedef enum {
    BUTTON_LEFT,                        // previous: 5-way left, tapped
    BUTTON_RIGHT,                       // next: 5-way right, tapped
    BUTTON_SELECT,                      // 5-way center pressed
    BUTTON_LEFT_HOLD,                   // left held for LONG_PRESS_MS (fires while still held)
    BUTTON_RIGHT_HOLD,                  // right held for LONG_PRESS_MS
    BUTTON_BACK,                        // BACK / HOME button tapped
    BUTTON_HOME,                        // BACK / HOME button held
    BUTTON_WAKE,                        // not a button: another task wants the main loop to check for
                                        // work (e.g. the web interface asking to show a pairing code)
    BUTTON_LOCK_CHANGED,                // not a press: the lock switch moved (see buttons_locked()).
                                        // Handled by main.c; never reaches apps.
    BUTTON_APPS_CHANGED,                // not a press: apps were installed or removed (web portal).
                                        // Handled by main.c; never reaches apps.
    BUTTON_SELECT_HOLD,                 // center held for LONG_PRESS_MS. Only apps with
                                        // app_t.select_hold get it; for the rest main.c turns
                                        // it into BUTTON_SELECT
} button_event_t;

// Sets up the GPIOs and starts the task that watches the controls. A button that is already
// held at this point (e.g. the power button, still down from turning the device on) is
// ignored until it's released.
void buttons_init(void);

// Puts an event on the queue from another task (used to send BUTTON_WAKE)
void buttons_post(button_event_t event);

// Waits up to `timeout` ticks for a button event. Returns true and fills *event if one arrived.
bool buttons_wait(button_event_t *event, TickType_t timeout);

// True while the lock switch is on (controls ignored)
bool buttons_locked(void);

// ---- power on / off (used by power.c) ----

// Right after waking from "off": true if the controls are unlocked and the BACK / HOME button
// stays held until `hold_ms` after the wake-up (counted from the start of boot). Returns
// false as soon as it's released early.
bool buttons_power_on_held(int hold_ms);

// Before deep sleep: waits for the BACK / HOME button to be released (so it doesn't wake the
// chip straight away), floats the other inputs, and arms the BACK / HOME button to wake the
// chip when pressed.
void buttons_prepare_wake(void);
