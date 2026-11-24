// Shared between books.c and books_import.c (not part of the public API)
#pragma once

#include <stdbool.h>
#include <stddef.h>

// The books folder as a full path, with the card mounted if that's where it is.
// False if it can't be used right now.
bool books_base_ready(char *out, size_t size);

// The list of books changed (lists reload)
void books_changed(void);
