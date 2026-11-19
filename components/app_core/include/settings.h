/*
 * Small values remembered across power-offs (NVS flash, namespace "app"), e.g. the last
 * opened app. nvs_flash_init() must have run first (main.c does it).
 */
#pragma once

#include <stdint.h>

// Keys shared between components (each component's own keys stay private to it)
#define SETTING_ROTATE_180 "rotate180"  // 1 = display upside down (Settings app; main.c applies it at start-up)

// The stored value for `key`, or `fallback` if it was never set. Keys are at most 15 characters.
int32_t settings_get_i32(const char *key, int32_t fallback);

// Stores a value. Skips the flash write if it's unchanged.
void settings_set_i32(const char *key, int32_t value);
