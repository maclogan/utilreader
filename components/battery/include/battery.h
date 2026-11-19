/*
 * Battery level and charging state on the Waveshare ESP32-S3-LR1121-ePaper-2.9: the onboard
 * AXP2602 fuel gauge (I2C) plus the charger's status line (GPIO47). Without a battery
 * connected everything reports "no battery". See README.md.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    BATTERY_NONE,                       // gauge not answering, or no battery connected
    BATTERY_DISCHARGING,                // running on the battery
    BATTERY_CHARGING,                   // the charger is charging (USB connected)
    BATTERY_FULL,                       // at the top and no current flowing (USB, charge done)
} battery_state_t;

// Wakes the gauge and takes a first reading. Call once at start-up.
void battery_init(void);

// Takes a new reading. Returns true if what the menu shows (percentage or state) changed.
bool battery_sample(void);

// Battery level to show, 0-100, or -1 if there's no battery
int battery_percent(void);

battery_state_t battery_state(void);

// Last cell voltage in millivolts and battery current in milliamps (as the gauge reports
// it), for logs and debugging. 0 when there's no battery.
int battery_millivolts(void);
int battery_milliamps(void);

// Before deep sleep: puts the gauge into its low-power mode. battery_init() wakes it.
void battery_sleep(void);
