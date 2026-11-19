/*
 * Weather app logic: when to download, what to draw, button handling.
 *
 * Screens:
 *   FORECAST - 7 days. Updates on its own while showing:
 *       - every CONFIG_WEATHER_REFRESH_MINUTES: new forecast + FULL refresh (flashes, clears ghosting)
 *       - every CONFIG_WEATHER_PARTIAL_SECONDS: new current conditions + PARTIAL refresh (no flashing)
 *     LEFT/RIGHT highlight a day, SELECT opens its details.
 *   DAY - full-screen details for one day. LEFT/RIGHT step between days, BACK returns.
 *
 * Downloads run on the worker; download_done() applies them in the main task.
 */
#include "weather_app.h"

#include "esp_log.h"
#include "local_time.h"                 // local_time_set_offset()
#include "sdkconfig.h"                  // CONFIG_WEATHER_* from menuconfig
#include "ui_loading.h"                 // ui_show_loading()
#include "weather_api.h"                // weather_fetch()
#include "weather_ui.h"                 // drawing
#include "worker.h"                     // background downloads

static const char *TAG = "weather";

// Timings converted to FreeRTOS ticks
#define FULL_TICKS        pdMS_TO_TICKS(CONFIG_WEATHER_REFRESH_MINUTES * 60 * 1000)
#define PARTIAL_TICKS     pdMS_TO_TICKS(CONFIG_WEATHER_PARTIAL_SECONDS * 1000)
#define FETCH_TICKS       pdMS_TO_TICKS(60 * 1000)      // re-download current conditions at most once a minute
#define RETRY_TICKS       pdMS_TO_TICKS(5 * 60 * 1000)  // after a failed full update, try again this soon
#define FIRST_RETRY_TICKS pdMS_TO_TICKS(30 * 1000)      // no forecast at all yet: retry this soon

static weather_t weather;               // latest good data (what's on screen); static keeps it off the stack
static weather_t download;              // the worker downloads into this, so a failure can't wipe `weather`
static bool have_forecast = false;      // true once a forecast has been downloaded
static bool full_pending = false;       // the download in progress is a full update
static bool requested = false;          // a download has been started at least once
static TickType_t last_full = 0;        // when the last full forecast download happened
static TickType_t last_fetch = 0;       // when we last started a download
static TickType_t next_redraw = 0;      // when the forecast screen next updates on its own
static int hover = -1;                  // highlighted day (0-6), -1 = none yet
static bool in_day = false;             // showing a day's details instead of the forecast

// ---------- downloads ----------

// Runs in the worker task: just the network part
static void *download_job(void *arg)
{
    return weather_fetch(&download) == ESP_OK ? &download : NULL;
}

// Runs in the main task when download_job() finishes. result = &download, or NULL on failure.
static void download_done(void *result, void *arg)
{
    bool full = full_pending || !have_forecast;             // the first forecast always needs a full draw
    bool on_forecast = app_is_showing(&weather_app) && !in_day;
    full_pending = false;

    if (result == NULL) {
        if (have_forecast && full) {
            ESP_LOGW(TAG, "full update failed, keeping old forecast");
            last_full = xTaskGetTickCount() - FULL_TICKS + RETRY_TICKS;  // try again in 5 minutes
        } else if (!have_forecast && on_forecast) {
            weather_ui_show_error("No forecast", "Weather service error, retrying");
        }
        return;
    }

    weather = download;                                     // accept the new data
    have_forecast = true;
    if (full) {
        last_full = xTaskGetTickCount();
    }
    local_time_set_offset(weather.utc_offset);              // every app's time zone

    if (on_forecast) {
        if (full) {
            weather_ui_show(&weather, hover);               // full refresh
        } else {
            weather_ui_show_partial(&weather, hover);       // only the changed values update
        }
    }
}

// Starts a download. full = also do a full refresh when it lands.
static void request(bool full)
{
    requested = true;
    if (worker_submit(download_job, download_done, NULL)) {
        full_pending = full;
        last_fetch = xTaskGetTickCount();
    } else if (full) {
        full_pending = true;                                // one is already running: make it a full update
    }
}

// ---------- screens ----------

// Shows the forecast right away from what we have, and downloads fresh data in the background
static void show_forecast(void)
{
    in_day = false;
    next_redraw = xTaskGetTickCount() + PARTIAL_TICKS;
    if (!have_forecast) {
        weather_ui_show_error("Weather", "Downloading the forecast...");
        request(true);                                      // drawn when it arrives
        return;
    }
    weather_ui_show(&weather, hover);                       // cached data right away (full refresh)

    TickType_t now = xTaskGetTickCount();
    if (now - last_full >= FULL_TICKS) {
        request(true);                                      // forecast is due anyway
    } else if (now - last_fetch >= FETCH_TICKS) {
        request(false);                                     // current conditions are stale
    }
}

// ---------- app callbacks ----------

static void weather_open(void)
{
    hover = -1;                                             // no day highlighted when the app opens
    show_forecast();
}

static bool weather_button(button_event_t event)
{
    if (event == BUTTON_BACK) {
        if (!in_day) {
            return false;                                   // leave the app
        }
        ESP_LOGI(TAG, "back -> forecast");
        ui_show_loading();
        show_forecast();                                    // the day we came from stays highlighted
        return true;
    }
    if (!have_forecast) {
        return true;                                        // nothing to highlight or open yet
    }

    if (in_day) {
        // LEFT/RIGHT step through the days without going back to the forecast
        if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {
            hover = (event == BUTTON_LEFT) ? (hover + WEATHER_DAYS - 1) % WEATHER_DAYS
                                           : (hover + 1) % WEATHER_DAYS;
            weather_ui_show_day(&weather, hover, false);    // quick partial refresh
        }
    } else if (event == BUTTON_LEFT || event == BUTTON_RIGHT) {
        if (hover < 0) {
            hover = 0;                                      // first press: start on today
        } else if (event == BUTTON_LEFT) {
            hover = (hover + WEATHER_DAYS - 1) % WEATHER_DAYS;  // previous day (wraps around)
        } else {
            hover = (hover + 1) % WEATHER_DAYS;             // next day (wraps around)
        }
        weather_ui_show_partial(&weather, hover);           // quick refresh: only the highlight moved
    } else if (event == BUTTON_SELECT) {                    // open the highlighted day
        if (hover < 0) {
            hover = 0;                                      // nothing highlighted yet: open today
        }
        ESP_LOGI(TAG, "day %d", hover);
        ui_show_loading();                                  // instant feedback during the full refresh
        in_day = true;
        weather_ui_show_day(&weather, hover, true);         // full refresh: whole screen changes
    }
    return true;
}

static TickType_t weather_next_wakeup(void)
{
    return in_day ? portMAX_DELAY : app_ticks_until(next_redraw);
}

// Scheduled update while the forecast is on screen
static void weather_tick(void)
{
    if (in_day || !app_due(next_redraw)) {
        return;
    }
    next_redraw = xTaskGetTickCount() + PARTIAL_TICKS;

    TickType_t now = xTaskGetTickCount();
    if (!have_forecast || now - last_full >= FULL_TICKS) {
        request(true);                                      // new forecast + full refresh when it lands
    } else if (now - last_fetch >= FETCH_TICKS) {
        request(false);                                     // new current conditions + partial refresh
    } else {
        weather_ui_show_partial(&weather, hover);           // just redraw (updates the "as of" line)
    }
}

// Even when not on screen: keep trying until there's a forecast (other apps need the time zone)
static void weather_background(void)
{
    if (!have_forecast && !worker_pending(download_job)
        && (!requested || app_due(last_fetch + FIRST_RETRY_TICKS))) {
        request(true);
    }
}

const app_t weather_app = {
    .name = "Weather",
    .draw_icon = weather_ui_draw_app_icon,
    .open = weather_open,
    .button = weather_button,
    .next_wakeup = weather_next_wakeup,
    .tick = weather_tick,
    .background = weather_background,
};
