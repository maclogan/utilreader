/*
 * The board's microSD slot, mounted at /sdcard (FAT32). 1-bit SDMMC on GPIO10 (CMD),
 * 11 (D0), 12 (CLK), 10k pull-ups on the board, powered by the shared peripheral rail
 * (board_power.h). Mounted on demand; thread-safe. See README.md.
 */
#pragma once

#include <stdbool.h>

#define SDCARD_ROOT "/sdcard"

// Mounts the card if it isn't already. Returns true when it's mounted. A missing or
// unreadable card returns false (logged); call again to retry, e.g. after a card swap.
bool sdcard_mount(void);

// Unmounts and releases the rail (before turning off, or after a card error). Files left
// open are invalid afterwards.
void sdcard_unmount(void);

bool sdcard_mounted(void);
