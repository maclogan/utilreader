/*
 * Main menu: a row of app tiles. Left/right move the highlight, select opens the app.
 * The tiles come from the app list in main.c (each app's name and draw_icon).
 *
 *  +-----------------------------------------+
 *  |    +--------+  +--------+  +--------+   |
 *  |    | [icon] |  | [icon] |  | [icon] |   |  <- highlighted tile has a thick border
 *  |    |Weather |  | Clock  |  | Story  |   |     and an inverted label
 *  |    +--------+  +--------+  +--------+   |
 *  |        < > choose   SELECT open         |
 *  +-----------------------------------------+
 */
#pragma once

#include <stdbool.h>
#include "app.h"                        // app_t

// Draws the menu with apps[selected] highlighted. Fits up to 3 tiles.
// full = true: full refresh (use when switching to the menu from another screen).
// full = false: quick partial refresh (use when only the highlight moved).
void menu_ui_show(const app_t *const apps[], int count, int selected, bool full);
