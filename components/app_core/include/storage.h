/*
 * The file system on the "storage" flash partition (SPIFFS, 1 MB), mounted at /store.
 * Apps keep their files here with plain fopen()/fprintf(). SPIFFS has no real folders,
 * so name files by app, e.g. /store/story.json.
 */
#pragma once

#include <stdbool.h>

#define STORAGE_PATH "/store"

// Mounts the partition, formatting it on first use. Call once at startup.
// Returns false if it can't be mounted (apps then find no files).
bool storage_init(void);
