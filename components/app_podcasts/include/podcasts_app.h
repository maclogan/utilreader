/*
 * Podcasts app: episodes downloaded from podcast feeds (managed on the web page) played
 * from the microSD card, like the Music app. Feeds are checked and episodes downloaded while
 * the Transfer app is open. See components/app_podcasts/README.md.
 *
 *   Lists:        left / right move, hold = a page; press opens / plays;
 *                 hold press = mark an episode played / unplayed (elsewhere: Now Playing)
 *   Now Playing:  left / right = volume (VOL) or back 10 s / ahead 30 s (SEEK);
 *                 hold left / right = previous / next episode; press = play / pause;
 *                 hold press = switch VOL / SEEK
 */
#pragma once

#include "app.h"

extern const app_t podcasts_app;
