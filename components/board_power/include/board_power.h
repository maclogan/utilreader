/*
 * The board's switched peripheral rail (GPIO38, high = on, 1 MOhm pull-down). One regulator
 * powers the e-paper, the LR1121 radio, the microSD slot and the RGB LED, so the display
 * and the SD card share it: each takes a reference while it needs power, and the rail
 * switches off with the last one. See README.md.
 */
#pragma once

// Switches the rail on if this is the first user (parking the radio first so it ignores
// the shared SPI bus, then waiting for the rail to settle). Thread-safe.
void rail_acquire(void);

// Drops a reference. With the last one, every pin leading to the rail's chips is floated
// (so none is fed through its I/O pins) and the rail switches off. The caller floats or
// releases its own pins first.
void rail_release(void);
