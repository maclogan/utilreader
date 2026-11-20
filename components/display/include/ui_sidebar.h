/*
 * The title bar, standing on the left edge of the screen so the screen keeps its full
 * height for content. The title reads bottom to top; the battery sits at the top end with a
 * small counter (e.g. "3/12") just below it; a vertical rule separates the bar from the
 * content.
 *
 *   +--+------------------------------+
 *   |3 |                              |
 *   |/ |                              |
 *   |12|        content from          |
 *   |  |        x = UI_CONTENT_X      |
 *   |s |                              |
 *   |k |                              |
 *   |o |                              |
 *   |o |                              |
 *   |B |                              |
 *   +--+------------------------------+
 */
#pragma once

#include <stdbool.h>
#include "epd.h"                        // EPD_WIDTH

#define UI_SIDEBAR_W   18               // the bar, including its rule (at x = UI_SIDEBAR_W - 1)
#define UI_CONTENT_X   UI_SIDEBAR_W     // first column to the right of the bar
#define UI_CONTENT_W   (EPD_WIDTH - UI_CONTENT_X)
#define UI_CONTENT_CX  (UI_CONTENT_X + UI_CONTENT_W / 2)   // horizontal center of the content
#define UI_SIDEBAR_LEN (EPD_HEIGHT - 8) // usable length along the bar (4 px clear at each end)

// Draws the bar: `title` (UTF-8, 16 px font, cut off if too long), `counter` (ASCII, small
// font, or NULL), the battery, and the rule. Draw the rest of the screen as usual.
void ui_sidebar(const char *title, const char *counter);

// The battery the bars show. main.c sets it from the fuel gauge at start-up and after each
// reading (the display driver doesn't know about the gauge). percent < 0 or NONE: no battery.
typedef enum { UI_BATTERY_NONE, UI_BATTERY_DISCHARGING, UI_BATTERY_CHARGING, UI_BATTERY_FULL } ui_battery_t;
void ui_sidebar_set_battery(int percent, ui_battery_t state);

// Inside ui_sidebar_begin() / epd_vertical_end(): draws the battery with its far end at `end`
// along the bar: a small battery whose fill shows the level, then "87%" (or "Full") if
// `with_text`, and a lightning bolt while charging. Returns where it starts (`end` if there's
// no battery), so other things can go below it.
int ui_sidebar_battery(int end, bool with_text);

// For custom bars (the main menu): starts vertical drawing in the bar, where drawing
// x = 0..UI_SIDEBAR_LEN runs bottom to top and drawing y = 0..15 left to right.
// Finish with epd_vertical_end(), then draw the rule with ui_sidebar_rule().
void ui_sidebar_begin(void);
void ui_sidebar_rule(void);
