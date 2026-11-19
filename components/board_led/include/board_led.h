/*
 * The CrowPanel 2.13"'s "POWER" LED (GPIO19, on when the pin is HIGH). Nothing in the
 * firmware uses it, so it's switched off at start-up to save battery. See README.md.
 */
#pragma once

// Turns the LED off. Call once at start-up.
void board_led_off(void);
