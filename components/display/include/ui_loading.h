/*
 * "Loading..." box drawn over whatever is on screen, using a quick partial refresh,
 * so a button press gets instant feedback before a slow full refresh.
 */
#pragma once

// Draws the loading box on top of the current frame and partial-refreshes it (~0.3 s).
// Does nothing if the panel is asleep (e.g. right after an error screen).
void ui_show_loading(void);
