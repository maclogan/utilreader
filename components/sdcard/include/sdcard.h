/*
 * The board's microSD slot, mounted at /sdcard (FAT32). 1-bit SDMMC on GPIO10 (CMD),
 * 11 (D0), 12 (CLK), 10k pull-ups on the board, powered by the shared peripheral rail
 * (board_power.h). Mounted on demand; thread-safe. See README.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SDCARD_ROOT "/sdcard"

// Mounts the card if it isn't already. Returns true when it's mounted. A missing or
// unreadable card returns false (logged); call again to retry. If the card is mounted but
// has stopped answering (taken out, or taken out and put back), the stale mount is dropped
// and it's mounted afresh, so every app that calls this before using the card recovers
// from a card swap by itself. The card is asked at most once a second.
bool sdcard_mount(void);

// Unmounts and mounts again (Settings > Card). True when a card is mounted afterwards.
bool sdcard_remount(void);

// Goes up with every successful mount: a change means the card may be a different one
// (the Music app rescans then)
uint32_t sdcard_generation(void);

// Size of the mounted card in MB, 0 if none
uint32_t sdcard_size_mb(void);

// Unmounts and releases the rail (before turning off, or after a card error). Files left
// open are invalid afterwards.
void sdcard_unmount(void);

bool sdcard_mounted(void);

// Erases the card: a fresh FAT32 file system, whatever was on it before (FAT, exFAT,
// nothing). Everything on it is lost. Takes seconds to a minute depending on the card's size. Close files on the card first (stop the player). True on success; the card
// stays mounted.
bool sdcard_format(void);

// While sdcard_format() runs (from another task): roughly how far it is, 0-99. -1 otherwise.
int sdcard_format_progress(void);
