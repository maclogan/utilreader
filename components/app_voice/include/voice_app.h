/*
 * Voice Notes app: records notes from the INMP441 microphone to the microSD card and plays
 * them back. See components/app_voice/README.md.
 *
 *   List:       "New note" at the top, then the notes, newest first.
 *               left / right move (hold: a page); press: record (on "New note", a press of
 *               any length) or play; hold press on a note: delete it (asks first); BACK leaves
 *   Recording:  press (any length) or BACK: stop and save. A toggle: nothing needs holding
 *   Playing:    left / right: back / ahead 5 s; hold left / right: volume; press: pause;
 *               BACK: stop, back to the list
 */
#pragma once

#include "app.h"

// Notes are 16 kHz mono WAV files in the folder chosen in Settings (SETTING_VOICE_DIR,
// default /Voice Notes on the card)

extern const app_t voice_app;
