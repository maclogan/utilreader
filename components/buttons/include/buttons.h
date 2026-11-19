/*
 * The CrowPanel's controls: a 3-way thumbwheel (up / down / press) plus EXIT and MENU
 * buttons. A background task watches them and turns presses into events. The event names
 * describe what they mean to apps (previous / next / select / back / home):
 *
 *   wheel up   tap -> BUTTON_LEFT     hold -> BUTTON_LEFT_HOLD
 *   wheel down tap -> BUTTON_RIGHT    hold -> BUTTON_RIGHT_HOLD
 *   wheel press     -> BUTTON_SELECT
 *   EXIT        press -> BUTTON_BACK
 *   MENU        press -> BUTTON_HOME
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"          // TickType_t

typedef enum {
    BUTTON_LEFT,                        // previous: wheel up, tapped
    BUTTON_RIGHT,                       // next: wheel down, tapped
    BUTTON_SELECT,                      // wheel pressed and released
    BUTTON_LEFT_HOLD,                   // wheel held up for LONG_PRESS_MS (fires while still held)
    BUTTON_RIGHT_HOLD,                  // wheel held down for LONG_PRESS_MS
    BUTTON_BACK,                        // EXIT pressed
    BUTTON_HOME,                        // MENU pressed
    BUTTON_WAKE,                        // not a button: another task wants the main loop to check for
                                        // work (e.g. the web interface asking to show a pairing code)
} button_event_t;

// Sets up the GPIOs and starts the task that watches the controls
void buttons_init(void);

// Puts an event on the queue from another task (used to send BUTTON_WAKE)
void buttons_post(button_event_t event);

// Waits up to `timeout` ticks for a button event. Returns true and fills *event if one arrived.
bool buttons_wait(button_event_t *event, TickType_t timeout);
