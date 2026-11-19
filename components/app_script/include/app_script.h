/*
 * Script apps: Lua programs installed through the web portal into /apps/<id>/, run by one
 * host that gives each of them an app_t in the app registry. See README.md and
 * docs/api.md (the API apps program against).
 *
 * An app is a folder:
 *   app.json       manifest: id, name, version, api, background, settings
 *   main.lua       entry point (other .lua files load with require)
 *   icon.bin       optional 32x32 1-bit icon (the portal makes it from icon.png)
 *   settings.json  values of the manifest's settings (written by the portal)
 *   data/          the app's own files (storage module)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define APP_SCRIPT_API_VERSION 1        // highest "api" in app.json this firmware runs
#define APP_SCRIPT_FILE_MAX    (256 * 1024)  // largest single file in an app package

// ---- main task ----

// Mounts /apps, installs the bundled apps on first start (and updates them when the
// firmware brings newer versions), and adds every installed app to the app registry
// (without calling their init: main.c does that for every registered app).
// Call after nvs_flash_init(), before the registry is used.
void app_script_init(void);

// Re-reads /apps after an install or removal: removes the script apps from the registry
// (closing them) and adds them again. Call only while no script app is on screen.
void app_script_rescan(void);

// ---- any task (the web portal) ----

// True for an app id: [a-z0-9_], 1-24 characters
bool app_script_valid_id(const char *id);

// Receives part of one file of a package being uploaded, into a staging folder.
// `fresh` on the first chunk of a package empties the staging folder first. Chunks must
// arrive in order: if `offset` isn't where the file ends, nothing is written and
// ESP_ERR_INVALID_STATE comes back with *have = the file's current size (resume from there).
esp_err_t app_script_stage(const char *id, const char *file, uint32_t offset, uint32_t total,
                           const void *data, size_t len, bool fresh, uint32_t *have);

// Installs the staged package as /apps/<id>, replacing an older version's files but
// keeping its settings.json and data/. On failure, err says why.
esp_err_t app_script_install(const char *id, char *err, size_t err_size);

// Deletes /apps/<id> with its settings and data
esp_err_t app_script_uninstall(const char *id);

// Installed apps as a JSON array: [{id, name, version, author, description, api}]
cJSON *app_script_list_json(void);

// Copies an app's recent log lines into out[size] ("" if none)
void app_script_log_read(const char *id, char *out, size_t size);
