#include "app_registry.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "apps";

static const app_t *list[APP_REGISTRY_MAX];
static int count = 0;                   // all apps
static int regular = 0;                 // how many of them come before the system group

void app_registry_add(const app_t *app, bool system)
{
    if (count >= APP_REGISTRY_MAX) {
        ESP_LOGE(TAG, "too many apps, %s not added", app->name);
        return;
    }
    if (system) {
        list[count++] = app;
        return;
    }
    memmove(&list[regular + 1], &list[regular], (count - regular) * sizeof(list[0]));
    list[regular++] = app;
    count++;
}

void app_registry_remove(const app_t *app)
{
    for (int i = 0; i < count; i++) {
        if (list[i] == app) {
            memmove(&list[i], &list[i + 1], (count - i - 1) * sizeof(list[0]));
            count--;
            if (i < regular) {
                regular--;
            }
            return;
        }
    }
}

const app_t *const *app_registry_list(int *out_count)
{
    *out_count = count;
    return list;
}

int app_registry_find(const char *id)
{
    for (int i = 0; id && i < count; i++) {
        if (list[i]->id && strcmp(list[i]->id, id) == 0) {
            return i;
        }
    }
    return -1;
}
