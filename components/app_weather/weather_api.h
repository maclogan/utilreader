/*
 * Fetches current conditions and a 7-day forecast from Open-Meteo
 * (https://open-meteo.com, free, no API key needed).
 */
#pragma once

#include "esp_err.h"

#define WEATHER_DAYS 7                  // number of forecast days we request and display

// One day of forecast data
typedef struct {
    int year, month, day;               // calendar date of this forecast day (local time)
    int weather_code;                   // WMO weather code (0 = clear, 61 = rain, ...)
    int temp_high;                      // daily maximum temperature, rounded
    int temp_low;                       // daily minimum temperature, rounded
    int rain_chance;                    // max chance of precipitation that day, 0-100 %
    int wind_max;                       // strongest sustained wind expected that day, rounded
    int wind_dir;                       // dominant direction the wind blows FROM, 0-359 deg (0 = north)
    // Extra detail for the day screen
    int feels_high, feels_low;          // "feels like" (apparent) max/min temperature
    float precip_sum;                   // total expected rain/snow (inches or mm)
    int gust_max;                       // strongest gust expected
    int uv_max;                         // peak UV index
    char sunrise[6], sunset[6];         // local times, "HH:MM"
    int hourly_temp[24];                // temperature for each hour, 0:00 to 23:00
    int hourly_rain[24];                // chance of precipitation for each hour, 0-100 %
} weather_day_t;

// Everything the display needs
typedef struct {
    int utc_offset;                     // seconds to add to UTC to get the location's local time
    int current_temp;                   // temperature right now, rounded
    int current_code;                   // WMO weather code right now
    int current_wind;                   // wind speed right now, rounded
    int current_wind_dir;               // direction the wind blows FROM right now, 0-359 deg
    weather_day_t days[WEATHER_DAYS];   // days[0] is today
} weather_t;

// Downloads and parses the forecast into *out. Wi-Fi must already be connected.
esp_err_t weather_fetch(weather_t *out);
