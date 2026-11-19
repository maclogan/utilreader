/*
 * Local time for every app.
 *
 * Where the time comes from, best first:
 *   - the internet (SNTP, started in main.c), re-checked every hour while online;
 *   - the board's clock chip (components/clock_chip), which keeps time through resets and
 *     while off. It's set after every internet sync and every manual change;
 *   - by hand, in the Settings app.
 * The ESP32-S3 also keeps counting through deep sleep on its own 32.768 kHz crystal.
 *
 * The time zone (UTC offset, including daylight saving) comes from the internet with each
 * weather forecast (Open-Meteo's "timezone=auto" for the configured location), which the
 * Weather app (Lua, apps/weather) downloads in the background and passes on with
 * time.set_zone(). It's remembered in
 * flash so it's right straight after power-on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>                       // struct tm, time_t

// Loads the remembered time zone, and sets the clock from the clock chip if it isn't
// already running (after a reset or a battery change). Call once after nvs_flash_init().
void local_time_init(void);

// True once the clock is set (from the internet, the clock chip or by hand)
bool local_time_synced(void);

// Seconds to add to UTC for local time (0 if no time zone has ever been known)
int local_time_offset(void);

// True if local_time_offset() is a real time zone rather than the 0 fallback
bool local_time_offset_known(void);

// Sets the time zone (the weather app calls this with each forecast). Remembered in flash.
void local_time_set_offset(int utc_offset);

// Current local time split into fields. Returns false if the clock isn't set yet.
bool local_time_now(struct tm *out);

// Sets the clock by hand from local date and time fields (tm_year, tm_mon, tm_mday,
// tm_hour, tm_min, tm_sec), using the current time zone. Also sets the clock chip.
void local_time_set_local(const struct tm *local);

// Call when the clock has just been set from the internet: copies it to the clock chip
// and records when (main.c passes this to SNTP as its sync callback). Safe from any task.
void local_time_internet_synced(void);

// When the clock was last set from the internet (UTC), 0 if not since power-on
time_t local_time_last_internet_sync(void);

// Local time as "5:05 PM" (or "17:05" with LOCAL_TIME_24H), for status bars.
// Returns false (and an empty string) while the clock isn't set.
#define LOCAL_TIME_24H 0
bool local_time_format_hm(time_t utc, char *out, size_t size);
