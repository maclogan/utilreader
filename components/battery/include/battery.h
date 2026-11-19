/*
 * Battery level and charging state from a MAX17043 or MAX17048 fuel gauge board on the
 * CrowPanel's GPIO_D port (I2C: GPIO40/41, 3V3, GND), with the gauge in line with the
 * LiPo. Works without the gauge too: everything then reports "no battery". See README.md.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    BATTERY_NONE,                       // no fuel gauge found (or it stopped answering)
    BATTERY_DISCHARGING,                // running on the battery
    BATTERY_CHARGING,                   // charge rising (USB connected)
    BATTERY_FULL,                       // at the top and no longer draining
} battery_state_t;

// Looks for the gauge (trying both SDA/SCL assignments) and takes a first reading.
// Call once at start-up.
void battery_init(void);

// Takes a new reading. Returns true if what the menu shows (percentage or state) changed.
bool battery_sample(void);

// Battery level to show, 0-100, or -1 if there's no gauge
int battery_percent(void);

battery_state_t battery_state(void);

// Last cell voltage in millivolts, and charge rate in tenths of a percent per hour
// (positive = charging), for logs and debugging. 0 when there's no gauge.
int battery_millivolts(void);
int battery_rate_tenths(void);
