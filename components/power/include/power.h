/*
 * Turning the device off and on.
 *
 * The board has no power switch: the battery always feeds the ESP32-S3. "Off" is deep sleep
 * with the display, radio, SD card and LED rail switched off and the fuel gauge asleep, which
 * Waveshare measures at about 36 uW (years on a small LiPo). The e-paper keeps showing the
 * "off" screen.
 *
 * Off:  the "Power" tile in the menu (power_app).
 * On:   hold the BACK / HOME button for POWER_ON_HOLD_MS. A shorter press, or any press
 *       while the lock switch is on, wakes the chip only long enough to check, then it goes
 *       straight back to sleep without touching the screen.
 * The RESET button and plugging in a battery also start it.
 */
#pragma once

#include "app.h"                        // app_t

#define POWER_ON_HOLD_MS 1500           // how long BACK / HOME must be held to turn on

// A picture for the "off" screen (296x128 baseline JPEG, uploaded on the web page), shown in
// 4 greys with the status line on top. Without it: the power symbol.
#define POWER_OFF_PICTURE "/store/offpic.jpg"

// Call first thing in app_main. After a wake-up from "off" it returns only if the power-on
// gesture was made (and shows "Starting..." right away); otherwise the chip goes back to
// sleep and this never returns. Does nothing after a reset or first power-up.
void power_boot_check(void);

// Finishes background jobs, runs the off hooks, shows the "off" screen and enters deep
// sleep. Never returns.
void power_off(void);

// Registers fn to run at the start of power_off(), in the main task (e.g. the music app
// stops playback and saves its place). Up to 4 hooks.
void power_add_off_hook(void (*fn)(void));

// Menu tile that turns the device off when opened
extern const app_t power_app;
