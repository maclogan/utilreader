/*
 * The list of apps, native and script, in the order the user chose, and which are hidden.
 *
 * Order: regular apps (native ones in the order main.c adds them, then script apps), then
 * "system" apps (Transfer, Settings, Power), which always stay at the end. The user can
 * reorder the regular apps and hide apps (web portal); both are kept in flash. Transfer
 * (the portal itself) and Power can't be hidden.
 *
 * Adding, removing and reloading are main-task only; reading the menu as JSON and saving
 * preferences are safe from any task (the web portal).
 */
#pragma once

#include <stdbool.h>
#include "app.h"
#include "cJSON.h"

#define APP_REGISTRY_MAX 32

// Adds an app. system = true keeps it in the group at the end of the menu.
void app_registry_add(const app_t *app, bool system);

// Removes an app (e.g. a script app that was uninstalled)
void app_registry_remove(const app_t *app);

// The apps on the menu (in order, hidden ones left out), and how many. The array stays
// valid until the next add / remove / reload.
const app_t *const *app_registry_list(int *count);

// Every app, hidden ones included (for init() and background())
const app_t *const *app_registry_all(int *count);

// Index on the menu of the app with this id, or -1 (missing or hidden)
int app_registry_find(const char *id);

// Re-reads the saved order and hidden set (after the portal changed them)
void app_registry_reload_prefs(void);

// ---- any task ----

// Saves the user's choices: ids comma-separated (order of the regular apps; hidden apps).
// Takes effect at the next app_registry_reload_prefs().
void app_registry_save_prefs(const char *order_csv, const char *hidden_csv);

// Every app in menu order: [{id, name, hidden, can_hide, system, native}]
cJSON *app_registry_menu_json(void);
