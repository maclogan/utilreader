/*
 * Reader app: books uploaded with the Transfer app (components/webui), read a page at
 * a time. See components/app_reader/README.md.
 *
 *   LIBRARY   wheel up/down choose a book, press opens it, EXIT leaves the app
 *   READING   wheel down/up next/previous page, hold the wheel down/up = 10 pages,
 *             press the wheel for the chapter list,
 *             EXIT back to the library. The position is remembered per book.
 *   CHAPTERS  wheel up/down choose (hold = a screen), press jumps there, EXIT back to reading.
 */
#pragma once

#include "app.h"

extern const app_t reader_app;
