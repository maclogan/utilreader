#include "local_time.h"

#include <stdio.h>                      // snprintf()
#include <sys/time.h>                   // settimeofday()
#include "clock_chip.h"                 // the board's real-time clock chip
#include "esp_log.h"
#include "settings.h"                   // remembering the time zone

static const char *TAG = "local_time";

#define CLOCK_SET_AFTER 1700000000      // time() is below this until the clock is set (Nov 2023)
#define KEY             "utc_off"

static int offset = 0;
static bool offset_known = false;
static volatile time_t last_sync = 0;   // written by the SNTP task, read by the main task

void local_time_init(void)
{
    const int32_t unset = INT32_MIN;
    int32_t saved = settings_get_i32(KEY, unset);
    if (saved != unset) {
        offset = saved;
        offset_known = true;
    }

    // After deep sleep the ESP32-S3 has kept counting; after a reset it starts from 1970
    if (!local_time_synced()) {
        time_t chip;
        if (clock_chip_read(&chip) && chip >= CLOCK_SET_AFTER) {
            struct timeval tv = {.tv_sec = chip};
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "clock set from the clock chip");
        } else {
            ESP_LOGI(TAG, "clock not set: waiting for internet time");
        }
    }
}

bool local_time_synced(void)
{
    return time(NULL) >= CLOCK_SET_AFTER;
}

int local_time_offset(void)
{
    return offset;
}

bool local_time_offset_known(void)
{
    return offset_known;
}

void local_time_set_offset(int utc_offset)
{
    if (offset_known && offset == utc_offset) {
        return;
    }
    offset = utc_offset;
    offset_known = true;
    settings_set_i32(KEY, utc_offset);
}

bool local_time_now(struct tm *out)
{
    if (!local_time_synced()) {
        return false;
    }
    time_t now = time(NULL) + offset;                       // shift to local time...
    gmtime_r(&now, out);                                    // ...then split it like UTC
    return true;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian)
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void local_time_set_local(const struct tm *t)
{
    time_t local = (time_t)days_from_civil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday) * 86400
                   + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
    struct timeval tv = {.tv_sec = local - offset};
    settimeofday(&tv, NULL);
    clock_chip_write(tv.tv_sec);
    ESP_LOGI(TAG, "clock set by hand");
}

void local_time_internet_synced(void)
{
    time_t now = time(NULL);
    last_sync = now;
    clock_chip_write(now);
    ESP_LOGI(TAG, "clock set from the internet, copied to the clock chip");
}

time_t local_time_last_internet_sync(void)
{
    return last_sync;
}

bool local_time_format_hm(time_t utc, char *out, size_t size)
{
    if (utc < CLOCK_SET_AFTER) {
        if (size > 0) {
            out[0] = '\0';
        }
        return false;
    }
    time_t local = utc + offset;
    struct tm t;
    gmtime_r(&local, &t);
#if LOCAL_TIME_24H
    snprintf(out, size, "%d:%02d", t.tm_hour, t.tm_min);
#else
    int hour12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
    snprintf(out, size, "%d:%02d %s", hour12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
#endif
    return true;
}
