#include "weather_api.h"

#include <math.h>                       // lround() for rounding temperatures
#include <stdio.h>                      // snprintf(), sscanf()
#include <string.h>                     // memset(), strchr()
#include "cJSON.h"                      // JSON parser bundled with ESP-IDF
#include "esp_crt_bundle.h"             // built-in root certificates so HTTPS can be verified
#include "esp_http_client.h"            // HTTP(S) client
#include "esp_log.h"                    // ESP_LOGI / ESP_LOGE logging macros
#include "sdkconfig.h"                  // CONFIG_WEATHER_* values from menuconfig

static const char *TAG = "weather";     // tag shown in front of this file's log lines

#define RESPONSE_MAX 16384              // the reply is ~6.5 KB (mostly hourly data); 16 KB leaves headroom

// Pick the unit query parameters based on the menuconfig "Units" choice
#if CONFIG_WEATHER_UNITS_METRIC
#define UNIT_PARAMS "&temperature_unit=celsius&wind_speed_unit=kmh&precipitation_unit=mm"
#else
#define UNIT_PARAMS "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch"
#endif

// Full request URL. Each "&name=..." asks Open-Meteo for one extra piece of data.
#define WEATHER_URL "https://api.open-meteo.com/v1/forecast"                          \
    "?latitude=" CONFIG_WEATHER_LATITUDE                /* location from menuconfig */ \
    "&longitude=" CONFIG_WEATHER_LONGITUDE                                             \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,"                       \
    "precipitation_probability_max,wind_speed_10m_max,"                                \
    "wind_direction_10m_dominant,"                      /* per-day forecast fields */  \
    "apparent_temperature_max,apparent_temperature_min,precipitation_sum,"             \
    "wind_gusts_10m_max,uv_index_max,sunrise,sunset"    /* extra fields for details */ \
    "&hourly=temperature_2m,precipitation_probability"  /* for the hourly graph */     \
    "&current=temperature_2m,weather_code,wind_speed_10m,wind_direction_10m" /* now */ \
    UNIT_PARAMS                                                                        \
    "&timezone=auto"                    /* use the location's local time zone */       \
    "&forecast_days=" "7"               /* must match WEATHER_DAYS */

// Reads number i from a JSON array like [73.0, 59.7, ...], rounded to an int
static int array_int(const cJSON *array, int i)
{
    const cJSON *item = cJSON_GetArrayItem(array, i);      // NULL if the index is missing
    return cJSON_IsNumber(item) ? (int)lround(item->valuedouble) : 0;  // missing/null -> 0
}

// Reads number i from a JSON array as a decimal number (for rain amounts like 0.12)
static float array_float(const cJSON *array, int i)
{
    const cJSON *item = cJSON_GetArrayItem(array, i);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
}

// Copies the "HH:MM" part of an ISO time like "2026-10-27T07:23" from array entry i into out[6]
static void array_hhmm(const cJSON *array, int i, char out[6])
{
    const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(array, i));
    const char *t = s ? strchr(s, 'T') : NULL;              // the time starts after the 'T'
    snprintf(out, 6, "%s", t ? t + 1 : "--:--");            // snprintf stops at 5 chars + '\0'
}

// Reads number field "name" from a JSON object, rounded to an int
static int object_int(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(object, name);  // NULL if the field is missing
    return cJSON_IsNumber(item) ? (int)lround(item->valuedouble) : 0;  // missing/null -> 0
}

// Turns the raw JSON text into a weather_t. Returns ESP_FAIL if fields are missing.
static esp_err_t parse_response(const char *json, weather_t *out)
{
    cJSON *root = cJSON_Parse(json);                        // build a tree from the text
    if (root == NULL) {                                     // not valid JSON at all
        ESP_LOGE(TAG, "response is not valid JSON");
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;                            // assume failure until proven otherwise
    const cJSON *current = cJSON_GetObjectItem(root, "current");   // "current": {...}
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");       // "daily": {...}
    if (current == NULL || daily == NULL) {                 // e.g. an error reply from the API
        ESP_LOGE(TAG, "response missing 'current' or 'daily'");
        goto done;
    }

    // Local time zone offset (e.g. -21600 for UTC-6), from "timezone=auto"
    out->utc_offset = object_int(root, "utc_offset_seconds");

    // Current conditions: single numbers
    out->current_temp = object_int(current, "temperature_2m");
    out->current_code = object_int(current, "weather_code");
    out->current_wind = object_int(current, "wind_speed_10m");
    out->current_wind_dir = object_int(current, "wind_direction_10m");

    // Daily forecast: each field is an array with one entry per day
    const cJSON *dates = cJSON_GetObjectItem(daily, "time");
    const cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
    const cJSON *highs = cJSON_GetObjectItem(daily, "temperature_2m_max");
    const cJSON *lows = cJSON_GetObjectItem(daily, "temperature_2m_min");
    const cJSON *rain = cJSON_GetObjectItem(daily, "precipitation_probability_max");
    const cJSON *wind = cJSON_GetObjectItem(daily, "wind_speed_10m_max");
    const cJSON *wind_dir = cJSON_GetObjectItem(daily, "wind_direction_10m_dominant");
    const cJSON *feels_hi = cJSON_GetObjectItem(daily, "apparent_temperature_max");
    const cJSON *feels_lo = cJSON_GetObjectItem(daily, "apparent_temperature_min");
    const cJSON *precip = cJSON_GetObjectItem(daily, "precipitation_sum");
    const cJSON *gusts = cJSON_GetObjectItem(daily, "wind_gusts_10m_max");
    const cJSON *uv = cJSON_GetObjectItem(daily, "uv_index_max");
    const cJSON *sunrise = cJSON_GetObjectItem(daily, "sunrise");
    const cJSON *sunset = cJSON_GetObjectItem(daily, "sunset");
    if (cJSON_GetArraySize(dates) < WEATHER_DAYS) {         // fewer days than we can draw
        ESP_LOGE(TAG, "expected %d days of data", WEATHER_DAYS);
        goto done;
    }

    // Hourly data: one long array per field, 24 entries per day starting at local midnight today
    const cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    const cJSON *h_temp = cJSON_GetObjectItem(hourly, "temperature_2m");
    const cJSON *h_rain = cJSON_GetObjectItem(hourly, "precipitation_probability");

    for (int i = 0; i < WEATHER_DAYS; i++) {                // copy each day into our struct
        weather_day_t *d = &out->days[i];
        const char *date = cJSON_GetStringValue(cJSON_GetArrayItem(dates, i)); // "2026-10-26"
        if (date == NULL || sscanf(date, "%d-%d-%d", &d->year, &d->month, &d->day) != 3) {
            goto done;                                      // date in an unexpected format
        }
        d->weather_code = array_int(codes, i);
        d->temp_high = array_int(highs, i);
        d->temp_low = array_int(lows, i);
        d->rain_chance = array_int(rain, i);
        d->wind_max = array_int(wind, i);
        d->wind_dir = array_int(wind_dir, i);
        d->feels_high = array_int(feels_hi, i);
        d->feels_low = array_int(feels_lo, i);
        d->precip_sum = array_float(precip, i);
        d->gust_max = array_int(gusts, i);
        d->uv_max = array_int(uv, i);
        array_hhmm(sunrise, i, d->sunrise);
        array_hhmm(sunset, i, d->sunset);
        for (int h = 0; h < 24; h++) {                      // this day's slice of the hourly arrays
            d->hourly_temp[h] = array_int(h_temp, i * 24 + h);
            d->hourly_rain[h] = array_int(h_rain, i * 24 + h);
        }
    }
    result = ESP_OK;                                        // everything parsed

done:
    cJSON_Delete(root);                                     // free the parsed tree
    return result;
}

esp_err_t weather_fetch(weather_t *out)
{
    static char response[RESPONSE_MAX];                     // static: keeps 16 KB off the task stack
    memset(out, 0, sizeof(*out));                           // start from a clean struct

    esp_http_client_config_t config = {
        .url = WEATHER_URL,                                 // where to send the request
        .crt_bundle_attach = esp_crt_bundle_attach,         // verify the server's HTTPS certificate
        .timeout_ms = 10000,                                // give up after 10 s without data
        .buffer_size_tx = 1024,                             // the request URL is ~600 chars; default buffer is 512
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);  // allocate the client
    if (client == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GET %s", WEATHER_URL);
    esp_err_t err = esp_http_client_open(client, 0);        // connect + send request (0 = no body)
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);                  // read the reply headers

    int status = esp_http_client_get_status_code(client);   // 200 means success
    int total = 0;                                          // bytes received so far
    while (total < RESPONSE_MAX - 1) {                      // leave room for the '\0'
        int n = esp_http_client_read(client, response + total, RESPONSE_MAX - 1 - total);
        if (n <= 0) {                                       // 0 = finished, <0 = error
            break;
        }
        total += n;
    }
    response[total] = '\0';                                 // terminate so it's a valid C string

    esp_http_client_close(client);                          // close the connection
    esp_http_client_cleanup(client);                        // free the client

    if (status != 200) {                                    // server rejected the request
        ESP_LOGE(TAG, "HTTP %d: %s", status, response);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "received %d bytes", total);
    return parse_response(response, out);                   // turn the text into numbers
}
