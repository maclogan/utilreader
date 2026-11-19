#include "settings.h"

#include <stdio.h>                      // snprintf()
#include <string.h>                     // strcmp()
#include "nvs.h"                        // nvs_open(), nvs_get_i32(), nvs_set_i32()

#define NAMESPACE "app"

int32_t settings_get_i32(const char *key, int32_t fallback)
{
    nvs_handle_t h;
    int32_t value = fallback;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, key, &value) != ESP_OK) {
            value = fallback;
        }
        nvs_close(h);
    }
    return value;
}

void settings_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    int32_t old;
    if (nvs_get_i32(h, key, &old) != ESP_OK || old != value) {  // flash wears out: only write changes
        nvs_set_i32(h, key, value);
        nvs_commit(h);
    }
    nvs_close(h);
}

void settings_get_str(const char *key, char *out, size_t size, const char *fallback)
{
    nvs_handle_t h;
    size_t len = size;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_str(h, key, out, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            return;
        }
    }
    snprintf(out, size, "%s", fallback);
}

void settings_set_str(const char *key, const char *value)
{
    char old[64];
    settings_get_str(key, old, sizeof(old), "\x01");      // a value nothing stores
    if (strcmp(old, value) == 0) {
        return;                                             // flash wears out: only write changes
    }
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}
