/*
 * Reader app: books uploaded with the Transfer app (components/webui), read a page at
 * a time. See components/app_reader/README.md.
 *
 *   LIBRARY   left/right choose a book, press opens it, BACK leaves the app
 *   READING   right/left next/previous page, hold right/left = 10 pages,
 *             press for the chapter list,
 *             BACK back to the library. The position is remembered per book.
 *   CHAPTERS  left/right choose (hold = a screen), press jumps there, BACK back to reading.
 */
#pragma once

#include "app.h"

extern const app_t reader_app;
