/*
 * Story app: a short Chinese graded-reader story written by Claude each day, with word
 * lookup and a saved-words list. See components/app_story/README.md.
 *
 *   MENU    - Today's story / Saved words / New story / Level. LEFT/RIGHT choose, SELECT opens
 *             (on Level, SELECT cycles HSK 1-6 for the next story).
 *   READER  - the story, up to 3 rows per page. Each page starts with no word selected;
 *             LEFT/RIGHT select and move between words, hold LEFT/RIGHT turns the page,
 *             SELECT saves / un-saves the selected word.
 *   SAVED   - saved words, 3 per screen. LEFT/RIGHT move, hold LEFT/RIGHT jump a screen,
 *             SELECT un-saves / re-saves the highlighted word.
 *   WRITING - "writing the story" message; the reader opens by itself when it's done.
 *
 * BACK goes back to the story menu; from the story menu, back to the main menu.
 */
#pragma once

#include "app.h"

extern const app_t story_app;
