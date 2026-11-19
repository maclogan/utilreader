/*
 * Clock app: the current time in big digits, with the date underneath.
 *
 *  +-----------------------------------------+
 *  |                                         |
 *  |      ##  ##### ##  #####   PM           |  <- "12:05", 7x size, AM/PM beside it
 *  |                                         |
 *  |            Tuesday Oct 27               |  <- date
 *  +-----------------------------------------+
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"          // TickType_t

// Draws the clock. utc_offset = seconds to add to UTC for local time.
// full = true: full refresh (switching from another screen, or hourly to clear ghosting).
// full = false: quick partial refresh (the minute changed).
void clock_ui_show(int utc_offset, bool full);

// Ticks until the next minute starts, i.e. when the clock should redraw next
TickType_t clock_ui_ticks_to_next_minute(void);

// True if it's the first minute of an hour (time for a full refresh)
bool clock_ui_top_of_hour(int utc_offset);

// Draws the clock app's icon centered on (cx, cy) into the frame buffer, for the menu
void clock_ui_draw_app_icon(int cx, int cy);
