/*
 * Settings app: set the date and time by hand, and turn the screen upside down.
 *
 *  +--+--------------------------------------+
 *  |  |##Date######2026-11-19################|  <- highlighted row
 *  |s |  Time       5:05 PM                  |
 *  |g |  Rotate     Normal                   |
 *  |n |                                      |
 *  |i |  Internet time: synced 4:58 PM       |
 *  |t |  Time zone: UTC-6 (from forecast)    |
 *  +--+--------------------------------------+
 *
 * LIST     left/right choose a row; press: edit the date or time, or flip the screen
 * EDITING  left/right change the highlighted field (hold: by 10), press: next field,
 *          and on the last field saves; BACK cancels
 *
 * A manual time holds until the next internet sync (every hour while online), which
 * replaces it. Both go to the clock chip, so the time survives resets.
 */
#pragma once

#include "app.h"

extern const app_t settings_app;
