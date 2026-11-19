/*
 * The list of apps on the main menu, native and script. Main-task only.
 *
 * Order: regular apps (native ones in the order main.c adds them, then script apps),
 * then "system" apps (Transfer, Settings, Power), which always stay at the end.
 */
#pragma once

#include <stdbool.h>
#include "app.h"

#define APP_REGISTRY_MAX 32

// Adds an app. system = true keeps it in the group at the end of the menu.
void app_registry_add(const app_t *app, bool system);

// Removes an app (e.g. a script app that was uninstalled)
void app_registry_remove(const app_t *app);

// The apps in menu order, and how many (the array stays valid until the next add/remove)
const app_t *const *app_registry_list(int *count);

// Index of the app with this id, or -1
int app_registry_find(const char *id);
