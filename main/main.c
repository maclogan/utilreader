/*
 * E-paper app launcher: start-up, the main menu, and the event loop that drives the apps.
 *
 * Every app implements app_t (components/app_core/include/app.h). This file:
 *   - starts everything (storage, Wi-Fi, internet time, the worker, each app's init)
 *   - shows the menu of apps and opens the chosen one (and, at power-on, the last one used)
 *   - waits for buttons, scheduled wake-ups and finished downloads, and passes them on
 *
 * Controls anywhere: holding BACK = home (this menu); everything else goes to the app.
 * The lock switch silences every control; the Power tile turns the device off (power.h).
 * To add an app, write a component that exports an app_t and list it in `apps` below.
 * See main/README.md.
 */
#include <stdbool.h>
#include <stdio.h>                      // snprintf()
#include <string.h>                     // strlen(), strcmp()
#include <sys/time.h>                   // struct timeval (SNTP callback)
#include "app.h"                        // app_t
#include "battery.h"                    // onboard fuel gauge
#include "books.h"                      // books_init()
#include "buttons.h"                    // buttons_init(), buttons_wait()
#include "clock_app.h"
#include "epd.h"                  // halt() screen
#include "esp_log.h"
#include "flashcards_app.h"
#include "reader_app.h"
#include "settings_app.h"
#include "transfer_app.h"
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
#include "weather_app.h"
#include "wifi.h"                       // wifi_connect()
#include "worker.h"                     // background jobs

static const char *TAG = "main";

// The apps, in menu order
static const app_t *const apps[] = {
    &weather_app,
    &clock_app,
    &story_app,
    &flashcards_app,
    &reader_app,
    &transfer_app,
    &settings_app,
    &power_app,
};
#define APP_COUNT ((int)(sizeof(apps) / sizeof(apps[0])))

#define BACKGROUND_TICKS pdMS_TO_TICKS(30 * 1000)  // how often every app's background() runs
#define POLL_TICKS       pdMS_TO_TICKS(100)        // while a job runs, check for its result this often

static int selected = 0;                // highlighted tile in the menu (also the app on screen)

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

// Opens apps[index] and remembers it as the one to open at the next power-on
// (unless it's one that shouldn't reopen by itself)
static void open_app(int index)
{
    selected = index;
    if (!apps[index]->no_resume) {
        settings_set_i32("last_app", index);
        settings_set_i32("on_menu", 0);
    }
    ESP_LOGI(TAG, "open %s", apps[index]->name);
    ui_show_loading();                                      // instant feedback during the full refresh
    app_set_showing(apps[index]);
    apps[index]->open();
}

static void go_home(const char *why)
{
    ESP_LOGI(TAG, "%s -> menu", why);
    const app_t *leaving = app_showing();
    if (leaving && leaving->close) {
        leaving->close();                                   // e.g. the Transfer app stops its server
    }
    settings_set_i32("on_menu", 1);                         // power-on returns to the menu
    ui_show_loading();                                      // instant feedback (quick partial refresh)
    app_set_showing(NULL);                                  // the app we left stays highlighted
    menu_ui_show(apps, APP_COUNT, selected, true);          // full refresh: whole screen changes
}

// Calls every app's background() (they rate-limit themselves)
static void run_background(void)
{
    for (int i = 0; i < APP_COUNT; i++) {
        if (apps[i]->background) {
            apps[i]->background();
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
    for (int i = 0; i < APP_COUNT; i++) {
        if (apps[i]->init) {
            apps[i]->init();
        }
    }
    buttons_init();
    battery_init();                                         // wakes the fuel gauge

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
    // Apps marked no_resume (Transfer, Power) are never reopened by themselves.
    int last = settings_get_i32("last_app", 0);
    selected = (last >= 0 && last < APP_COUNT) ? last : 0;
    if (settings_get_i32("on_menu", 1) || apps[selected]->no_resume) {
        menu_ui_show(apps, APP_COUNT, selected, true);
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
            TickType_t w = app->next_wakeup();
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
                if (battery_sample() && app == NULL) {
                    menu_ui_show(apps, APP_COUNT, selected, false);  // the menu shows the battery
                }
                run_background();
                next_background = xTaskGetTickCount() + BACKGROUND_TICKS;
            }
            if (app && app->tick) {
                app->tick();
            } else if (app == NULL) {
                menu_ui_tick(apps, APP_COUNT, selected);   // redraws when the minute changed
            }
            continue;
        }

        if (event == BUTTON_WAKE && app == NULL) {
            continue;                                       // a nudge for an app, but the menu is showing
        }

        // The lock switch moved: the menu shows a padlock. Apps never see this event.
        if (event == BUTTON_LOCK_CHANGED) {
            if (app == NULL) {
                menu_ui_show(apps, APP_COUNT, selected, false);
            }
            continue;
        }

        // HOME (BACK held) from anywhere: the menu. Jobs carry on in the background.
        if (event == BUTTON_HOME) {
            if (app) {
                go_home("home");
            }
            continue;
        }

        if (app) {
            if (!app->button(event)) {                      // the app asked to be closed (BACK)
                go_home("back");
            }
            continue;
        }

        // The menu
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {  // move the highlight (wraps around)
            selected = (selected + (event == BUTTON_LEFT ? APP_COUNT - 1 : 1)) % APP_COUNT;
            menu_ui_show(apps, APP_COUNT, selected, false);   // partial: only the highlight moved
        } else if (event == BUTTON_SELECT) {
            open_app(selected);
        }
    }
}
