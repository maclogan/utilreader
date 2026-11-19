/*
 * The PCF85063A real-time clock chip on the board (I2C 0x51). It runs from the always-on
 * 3.3 V supply on its own 32.768 kHz crystal (about 0.25 uA), so it keeps time through
 * resets and while the device is off, as long as the battery has charge. It holds UTC.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

// Reads the time (UTC). False if the chip doesn't answer, or reports that its oscillator
// stopped since it was last set (e.g. the battery ran flat), so its time can't be trusted.
bool clock_chip_read(time_t *out);

// Sets the chip to `t` (UTC) and clears its "oscillator stopped" flag
bool clock_chip_write(time_t t);
