#include "app_registry.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"            // the web portal reads the list from its own task
#include "settings.h"                   // order and hidden set, kept in flash

static const char *TAG = "apps";

#define ORDER_KEY   "menu_order"        // comma-separated ids of the regular apps, in order
#define HIDDEN_KEY  "menu_hidden"       // comma-separated ids of hidden apps
#define PREFS_MAX   (APP_REGISTRY_MAX * 26)

typedef struct {
    const app_t *app;
    bool system;
} entry_t;

static entry_t added[APP_REGISTRY_MAX]; // in the order added: regular apps, then system apps
static int count = 0;
static int regular = 0;                 // how many entries come before the system group

static const app_t *ordered[APP_REGISTRY_MAX];  // every app in menu order
static bool ordered_hidden[APP_REGISTRY_MAX];
static bool ordered_system[APP_REGISTRY_MAX];
static const app_t *visible[APP_REGISTRY_MAX];  // the menu: ordered minus hidden
static int visible_count = 0;
static const app_t *all[APP_REGISTRY_MAX];      // every app, in the order added

static char order_prefs[PREFS_MAX];
static char hidden_prefs[PREFS_MAX];
static bool prefs_loaded = false;

static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;

static void take(void)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);      // first call: main task at start-up
    }
    xSemaphoreTake(lock, portMAX_DELAY);
}

static void give(void)
{
    xSemaphoreGive(lock);
}

// Position of `id` in a comma-separated list, or -1
static int position_in(const char *csv, const char *id)
{
    size_t n = strlen(id);
    int pos = 0;
    for (const char *p = csv; *p; pos++) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, id, n) == 0) {
            return pos;
        }
        p += len + (end != NULL);
    }
    return -1;
}

static bool can_hide(const app_t *app)
{
    return !(app->id && (strcmp(app->id, "transfer") == 0 || strcmp(app->id, "power") == 0));
}

// Rebuilds `ordered`, `visible` and `all` from `added` and the preferences (lock held)
static void rebuild(void)
{
    if (!prefs_loaded) {
        settings_get_str(ORDER_KEY, order_prefs, sizeof(order_prefs), "");
        settings_get_str(HIDDEN_KEY, hidden_prefs, sizeof(hidden_prefs), "");
        prefs_loaded = true;
    }
    int n = 0;
    // Regular apps: those named in the saved order first (in that order), then the rest
    // in the order they were added
    bool used[APP_REGISTRY_MAX] = {0};
    for (int want = 0; want < APP_REGISTRY_MAX; want++) {
        for (int i = 0; i < regular; i++) {
            if (!used[i] && added[i].app->id && position_in(order_prefs, added[i].app->id) == want) {
                ordered[n] = added[i].app;
                ordered_system[n++] = false;
                used[i] = true;
            }
        }
    }
    for (int i = 0; i < regular; i++) {
        if (!used[i]) {
            ordered[n] = added[i].app;
            ordered_system[n++] = false;
        }
    }
    for (int i = regular; i < count; i++) {
        ordered[n] = added[i].app;
        ordered_system[n++] = true;
    }
    visible_count = 0;
    for (int i = 0; i < n; i++) {
        ordered_hidden[i] = can_hide(ordered[i]) && ordered[i]->id && position_in(hidden_prefs, ordered[i]->id) >= 0;
        if (!ordered_hidden[i]) {
            visible[visible_count++] = ordered[i];
        }
        all[i] = added[i].app;
    }
}

void app_registry_add(const app_t *app, bool system)
{
    take();
    if (count >= APP_REGISTRY_MAX) {
        give();
        ESP_LOGE(TAG, "too many apps, %s not added", app->name);
        return;
    }
    if (system) {
        added[count++] = (entry_t){app, true};
    } else {
        memmove(&added[regular + 1], &added[regular], (count - regular) * sizeof(added[0]));
        added[regular++] = (entry_t){app, false};
        count++;
    }
    rebuild();
    give();
}

void app_registry_remove(const app_t *app)
{
    take();
    for (int i = 0; i < count; i++) {
        if (added[i].app == app) {
            memmove(&added[i], &added[i + 1], (count - i - 1) * sizeof(added[0]));
            count--;
            if (i < regular) {
                regular--;
            }
            break;
        }
    }
    rebuild();
    give();
}

const app_t *const *app_registry_list(int *out_count)
{
    *out_count = visible_count;
    return visible;
}

const app_t *const *app_registry_all(int *out_count)
{
    *out_count = count;
    return all;
}

int app_registry_find(const char *id)
{
    for (int i = 0; id && i < visible_count; i++) {
        if (visible[i]->id && strcmp(visible[i]->id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void app_registry_reload_prefs(void)
{
    take();
    prefs_loaded = false;
    rebuild();
    give();
}

void app_registry_save_prefs(const char *order_csv, const char *hidden_csv)
{
    settings_set_str(ORDER_KEY, order_csv);
    settings_set_str(HIDDEN_KEY, hidden_csv);
}

cJSON *app_registry_menu_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    take();
    for (int i = 0; i < count; i++) {
        const app_t *a = ordered[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", a->id ? a->id : "");
        cJSON_AddStringToObject(o, "name", a->name ? a->name : "");
        cJSON_AddBoolToObject(o, "hidden", ordered_hidden[i]);
        cJSON_AddBoolToObject(o, "can_hide", can_hide(a));
        cJSON_AddBoolToObject(o, "system", ordered_system[i]);
        cJSON_AddBoolToObject(o, "native", a->ctx == NULL);  // script apps carry their state in ctx
        cJSON_AddItemToArray(arr, o);
    }
    give();
    return arr;
}
