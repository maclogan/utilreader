/*
 * E-paper app launcher: start-up, the main menu, and the event loop that drives the apps.
 *
 * Every app implements app_t (components/app_core/include/app.h) and is listed in the app
 * registry (app_registry.h): native apps are added in register_apps() below, script apps
 * (Lua, installed through the web portal) by components/app_script. This file:
 *   - starts everything (storage, Wi-Fi, internet time, the worker, each app's init)
 *   - shows the menu of apps and opens the chosen one (and, at power-on, the last one used)
 *   - waits for buttons, scheduled wake-ups and finished downloads, and passes them on
 *
 * Controls anywhere: holding BACK = home (this menu); everything else goes to the app.
 * The lock switch silences every control; the Power tile turns the device off (power.h).
 * See main/README.md.
 */
#include <stdbool.h>
#include <stdio.h>                      // snprintf()
#include <string.h>                     // strlen(), strcmp()
#include <sys/time.h>                   // struct timeval (SNTP callback)
#include "app.h"                        // app_t
#include "app_registry.h"               // the apps on the menu
#include "app_script.h"                 // script apps
#include "battery.h"                    // onboard fuel gauge
#include "books.h"                      // books_init()
#include "buttons.h"                    // buttons_init(), buttons_wait()
#include "ui_sidebar.h"                // ui_sidebar_set_battery()
#include "epd.h"                  // halt() screen
#include "esp_log.h"
#include "music_app.h"
#include "podcasts_app.h"
#include "reader_app.h"
#include "settings_app.h"
#include "transfer_app.h"
#include "voice_app.h"
#include "esp_netif_sntp.h"             // internet time (SNTP)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"              // vTaskDelay(), xTaskGetTickCount()
#include "local_time.h"                 // local_time_init()
#include "menu_ui.h"                    // menu_ui_show()
#include "power.h"                      // power_boot_check(), power_app
#include "nvs_flash.h"                  // non-volatile storage (settings, Wi-Fi calibration)
#include "sdkconfig.h"                  // CONFIG_WEATHER_WIFI_SSID
#include "settings.h"                   // remembering the last app
#include "storage.h"                    // storage_init()
#include "story_app.h"
#include "ui_loading.h"                 // ui_show_loading()
#include "wifi.h"                       // wifi_connect()
#include "worker.h"                     // background jobs

static const char *TAG = "main";

// Native apps, in menu order. Script apps follow the regular ones, alphabetically; the
// system apps (Transfer, Settings, Power) always come last.
static void register_apps(void)
{
    app_registry_add(&story_app, false);
    app_registry_add(&reader_app, false);
    app_registry_add(&music_app, false);
    app_registry_add(&podcasts_app, false);
    app_registry_add(&voice_app, false);
    app_script_init();                                      // Weather, Clock and any installed apps
    app_registry_add(&transfer_app, true);
    app_registry_add(&settings_app, true);
    app_registry_add(&power_app, true);
}

// Hands the fuel gauge's reading to the shared title bar (display can't see the gauge)
static void update_bar_battery(void)
{
    static const ui_battery_t states[] = {
        [BATTERY_NONE] = UI_BATTERY_NONE, [BATTERY_DISCHARGING] = UI_BATTERY_DISCHARGING,
        [BATTERY_CHARGING] = UI_BATTERY_CHARGING, [BATTERY_FULL] = UI_BATTERY_FULL,
    };
    ui_sidebar_set_battery(battery_percent(), states[battery_state()]);
}

#define BACKGROUND_TICKS pdMS_TO_TICKS(30 * 1000)  // how often every app's background() runs
#define POLL_TICKS       pdMS_TO_TICKS(100)        // while a job runs, check for its result this often

static int selected = 0;                // highlighted tile in the menu (also the app on screen)

static int app_count(void)
{
    int n;
    app_registry_list(&n);
    return n;
}

static const app_t *app_at(int index)
{
    int n;
    return app_registry_list(&n)[index];
}

static void show_menu(bool full)
{
    int n;
    const app_t *const *list = app_registry_list(&n);
    menu_ui_show(list, n, selected, full);
}

// Shows an error and stops (nothing works without Wi-Fi settings)
static void halt(const char *title, const char *detail)
{
    ESP_LOGE(TAG, "%s: %s", title, detail);
    epd_init();
    epd_clear(EPD_WHITE);
    epd_draw_text(10, 40, title, 2, EPD_BLACK);
    epd_draw_text(10, 70, detail, 1, EPD_BLACK);
    epd_display();
    vTaskDelay(portMAX_DELAY);                              // wait forever
}

// Opens the app at `index` and remembers it as the one to open at the next power-on
// (unless it's one that shouldn't reopen by itself)
static void open_app(int index)
{
    const app_t *app = app_at(index);
    selected = index;
    if (!app->no_resume) {
        settings_set_str("last_app_id", app->id);
        settings_set_i32("on_menu", 0);
    }
    ESP_LOGI(TAG, "open %s", app->name);
    ui_show_loading();                                      // instant feedback during the full refresh
    app_set_showing(app);
    app->open(app);
}

static void go_home(const char *why)
{
    ESP_LOGI(TAG, "%s -> menu", why);
    const app_t *leaving = app_showing();
    if (leaving && leaving->close) {
        leaving->close(leaving);                            // e.g. the Transfer app stops its server
    }
    settings_set_i32("on_menu", 1);                         // power-on returns to the menu
    ui_show_loading();                                      // instant feedback (quick partial refresh)
    app_set_showing(NULL);                                  // the app we left stays highlighted
    show_menu(true);                                        // full refresh: whole screen changes
}

// Calls every app's background() (they rate-limit themselves)
static void run_background(void)
{
    int n;
    const app_t *const *all = app_registry_all(&n);         // hidden apps keep working in the background
    for (int i = 0; i < n; i++) {
        if (all[i]->background) {
            all[i]->background(all[i]);
        }
    }
}

// SNTP set the clock (runs in the SNTP task)
static void on_internet_time(struct timeval *tv)
{
    local_time_internet_synced();
}

void app_main(void)
{
    // ---------- start-up ----------
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());                 // NVS partition is full/old: wipe it...
        err = nvs_flash_init();                             // ...and try again
    }
    ESP_ERROR_CHECK(err);
    // Screen orientation before anything is drawn (power_boot_check() redraws the "off"
    // screen exactly as it was left)
    epd_set_rotated(settings_get_i32(SETTING_ROTATE_180, epd_rotated()));  // default: menuconfig
    epd_set_picture_gamma(settings_get_i32(SETTING_PICTURE_GAMMA, epd_picture_gamma()));
    epd_set_partial_mode(settings_get_i32(SETTING_PARTIAL_MODE, EPD_PARTIAL_STOCK));
    epd_set_full_every(settings_get_i32(SETTING_FULL_EVERY, epd_full_every()));
    power_boot_check();                                     // woken from "off": on only if BACK is held
#if CONFIG_DISPLAY_DIAGNOSTIC
    vTaskDelay(pdMS_TO_TICKS(4000));                        // time to open the serial log after a reset
    epd_diagnose();                                         // display wiring report (tag "epd_diag")
#endif

    if (strlen(CONFIG_WEATHER_WIFI_SSID) == 0) {
        halt("Setup needed", "Set Wi-Fi in menuconfig");
    }

    local_time_init();                                      // remembered time zone; clock chip -> clock
    storage_init();                                         // /store file system
    books_init();                                           // /books (e-books)
    worker_init();                                          // background jobs
    register_apps();
    int all_count;
    const app_t *const *all = app_registry_all(&all_count);
    for (int i = 0; i < all_count; i++) {
        if (all[i]->init) {
            all[i]->init(all[i]);
        }
    }
    buttons_init();
    battery_init();                                         // wakes the fuel gauge
    update_bar_battery();

    // Get online. If it fails, the Wi-Fi driver keeps retrying in the background.
    if (wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi yet, still trying");
    }

    // Set the clock from the internet, now and every hour; each sync also sets the clock chip
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = on_internet_time;
    esp_netif_sntp_init(&sntp);                             // starts syncing in the background
    if (!local_time_synced() &&                             // the clock chip usually has the time already
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {  // if not, wait up to 10 s
        ESP_LOGW(TAG, "time not synced yet");
    }

    run_background();                                       // e.g. the weather app starts its first download
    TickType_t next_background = xTaskGetTickCount() + BACKGROUND_TICKS;

    // Power-on: back where we were, the last app or the menu (a new board starts on the menu).
    // Apps marked no_resume (Transfer, Power) are never reopened by themselves. Remembered
    // by id: installing or removing apps moves the others along the menu.
    char last[32];
    settings_get_str("last_app_id", last, sizeof(last), "");
    int index = app_registry_find(last);
    selected = index >= 0 ? index : 0;
    if (index < 0 || settings_get_i32("on_menu", 1) || app_at(selected)->no_resume) {
        show_menu(true);
    } else {
        open_app(selected);
    }

    // ---------- event loop ----------
    while (1) {
        worker_dispatch();                                  // apply finished downloads
        const app_t *app = app_showing();

        // Sleep until a button press, the app's next scheduled wake-up, the next background
        // run, or (while a job runs) a short poll interval
        TickType_t wait = app_ticks_until(next_background);
        if (app && app->next_wakeup) {
            TickType_t w = app->next_wakeup(app);
            wait = w < wait ? w : wait;
        } else if (app == NULL) {
            TickType_t w = menu_ui_next_wakeup();           // the menu's clock
            wait = w < wait ? w : wait;
        }
        if (worker_any_pending() && wait > POLL_TICKS) {
            wait = POLL_TICKS;
        }

        button_event_t event;
        if (!buttons_wait(&event, wait)) {
            // Timeout: background work, then the app's scheduled update
            if (app_due(next_background)) {
                bool battery_changed = battery_sample();
                update_bar_battery();                       // apps' title bars pick it up on their next redraw
                if (battery_changed && app == NULL) {
                    show_menu(false);                       // the menu shows the battery
                }
                run_background();
                next_background = xTaskGetTickCount() + BACKGROUND_TICKS;
            }
            if (app && app->tick) {
                app->tick(app);
            } else if (app == NULL) {
                int n;
                const app_t *const *list = app_registry_list(&n);
                menu_ui_tick(list, n, selected);            // redraws when the minute changed
            }
            continue;
        }

        // Apps were installed, removed or reconfigured, or the menu was reordered (web portal,
        // so Transfer is on screen): reload the script apps and the menu preferences, keeping
        // the same tile highlighted. Apps never see this event.
        if (event == BUTTON_APPS_CHANGED) {
            char keep[32];
            snprintf(keep, sizeof(keep), "%s", app_at(selected)->id);
            if (app && app->ctx) {
                go_home("apps changed");                    // a script app is showing (ctx set): leave it first
                app = NULL;
            }
            app_script_rescan();
            app_registry_reload_prefs();
            int index = app_registry_find(keep);
            selected = index >= 0 ? index : 0;
            if (app == NULL) {
                show_menu(false);
            }
            continue;
        }

        if (event == BUTTON_WAKE && app == NULL) {
            continue;                                       // a nudge for an app, but the menu is showing
        }

        // The lock switch moved: the menu shows a padlock. Apps never see this event.
        if (event == BUTTON_LOCK_CHANGED) {
            if (app == NULL) {
                show_menu(false);
            }
            continue;
        }

        // Center held: only apps that ask for it see the hold; for everyone else it's a press
        if (event == BUTTON_SELECT_HOLD && (app == NULL || !app->select_hold)) {
            event = BUTTON_SELECT;
        }

        // HOME (BACK held) from anywhere: the menu. Jobs carry on in the background.
        if (event == BUTTON_HOME) {
            if (app) {
                go_home("home");
            }
            continue;
        }

        if (app) {
            if (!app->button(app, event)) {                 // the app asked to be closed (BACK)
                go_home("back");
            }
            continue;
        }

        // The menu
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {  // move the highlight (wraps around)
            int n = app_count();
            selected = (selected + (event == BUTTON_LEFT ? n - 1 : 1)) % n;
            ESP_LOGI(TAG, "highlight %s", app_at(selected)->name);
            show_menu(false);                               // partial: only the highlight moved
        } else if (event == BUTTON_SELECT) {
            open_app(selected);
        }
    }
}
