/*
 * Lays out the weather forecast on the 250x122 e-paper display.
 *
 *  +-----------------------------------------+
 *  |Today| Tue | Wed | Thu | Fri | Sat | Sun |   <- 7 columns, one per day
 *  |10/26|10/27|10/28|10/29|10/30|10/31|11/01|   <- date (mm/dd)
 *  | icon| icon| icon| ... |     |     |     |
 *  |  73 |  60 |  59 |     |     |     |     |   <- high
 *  |  43 |  44 |  39 |     |     |     |     |   <- low
 *  | o1% | o2% | o1% |     |     |     |     |   <- chance of rain
 *  |12mph| 8mph|15mph|     |     |     |     |   <- max wind speed
 *  |  ^  |  /  |  >  |     |     |     |     |   <- wind direction (arrow shows where it blows, up = north)
 *  +-----------------------------------------+
 */
#pragma once

#include "weather_api.h"

#include <stdbool.h>

// Draws the full forecast screen with a full refresh (flashes, removes ghosting).
// hover = day whose label is highlighted (0 = today, 1-6 = grid cells), or -1 for none.
void weather_ui_show(const weather_t *weather, int hover);

// Redraws the forecast screen with a partial refresh: only changed pixels update, no flashing.
// Must follow at least one full refresh.
void weather_ui_show_partial(const weather_t *weather, int hover);

// Full-screen details for one day (0 = today ... 6): stats on the left, hourly graph on the right.
// full = true for a full refresh (coming from another screen), false for a quick partial
// refresh (stepping between days).
void weather_ui_show_day(const weather_t *weather, int day, bool full);

// Draws the weather app's icon (sun behind a cloud) centered on (cx, cy) into the
// frame buffer, for use in the menu. Doesn't refresh the display.
void weather_ui_draw_app_icon(int cx, int cy);

// Draws a simple error screen (title + one line of detail) and refreshes the display
void weather_ui_show_error(const char *title, const char *detail);
