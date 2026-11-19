/*
 * Local time for every app. The clock is set from the internet (SNTP, started in main.c);
 * the time zone comes from the weather forecast (Open-Meteo's "timezone=auto"), and is
 * remembered in flash so it's right straight after power-on, before a forecast arrives.
 */
#pragma once

#include <stdbool.h>
#include <time.h>                       // struct tm

// Loads the remembered time zone. Call once after nvs_flash_init().
void local_time_init(void);

// True once the clock has been set from the internet
bool local_time_synced(void);

// Seconds to add to UTC for local time (0 if no time zone has ever been known)
int local_time_offset(void);

// True if local_time_offset() is a real time zone rather than the 0 fallback
bool local_time_offset_known(void);

// Sets the time zone (the weather app calls this with each forecast). Remembered in flash.
void local_time_set_offset(int utc_offset);

// Current local time split into fields. Returns false if the clock isn't set yet.
bool local_time_now(struct tm *out);
