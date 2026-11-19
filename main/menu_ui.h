/*
 * Main menu: a row of app tiles. Left/right move the highlight, select opens the app.
 * The tiles come from the app list in main.c (each app's name and draw_icon). The status
 * bar on the left (reading bottom to top) shows the time, a padlock while the controls are
 * locked, and the battery.
 *
 *  +--+--------------------------------------+
 *  |% |  +------+ +------+ +------+ +------+ |
 *  |87|  |[icon]| |[icon]| |[icon]| |[icon]| |  <- highlighted tile has a thick border
 *  |  |  |Weathr| |Clock | |Story | |Cards | |     and an inverted label
 *  |MP|  +------+ +------+ +------+ +------+ |
 *  |50|                                      |
 *  |:5|       < > choose   press: open       |
 *  +--+--------------------------------------+
 */
#pragma once

#include <stdbool.h>
#include "app.h"                        // app_t
#include "freertos/FreeRTOS.h"          // TickType_t

// Draws the menu with apps[selected] highlighted. Shows up to 4 tiles; more scroll.
// full = true: full refresh (use when switching to the menu from another screen).
// full = false: quick partial refresh (use when only the highlight moved).
void menu_ui_show(const app_t *const apps[], int count, int selected, bool full);

// Ticks until the status bar's clock next changes (portMAX_DELAY while the time isn't set)
TickType_t menu_ui_next_wakeup(void);

// Redraws the menu if the minute changed since it was drawn (full refresh on the hour)
void menu_ui_tick(const app_t *const apps[], int count, int selected);
