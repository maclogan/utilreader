#include "power.h"

#include "battery.h"                    // battery_sleep()
#include "buttons.h"                    // buttons_power_on_held(), buttons_prepare_wake()
#include "sdcard.h"                     // sdcard_unmount()
#include "epd.h"                        // the "off" screen, epd_power_down()
#include "esp_log.h"
#include "esp_sleep.h"                  // deep sleep and the wake-up cause
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"              // vTaskDelay()
#include "wifi.h"                       // wifi_disconnect()
#include "worker.h"                     // let downloads finish writing their files

static const char *TAG = "power";

#define JOB_WAIT_MS 15000               // at most this long for background jobs before turning off

// Power symbol (a ring open at the top with a bar through the gap), centered on (cx, cy)
static void draw_power_symbol(int cx, int cy, int r, int thickness)
{
    epd_fill_circle(cx, cy, r, EPD_BLACK);
    epd_fill_circle(cx, cy, r - thickness, EPD_WHITE);
    int gap = r / 2 + thickness;                            // opening at the top of the ring
    epd_draw_rect(cx - gap / 2 - thickness / 2, cy - r - 1, gap + thickness, r, EPD_WHITE, true);
    epd_draw_rect(cx - thickness / 2, cy - r - 2, thickness, r + 2, EPD_BLACK, true);
}

static void power_icon(const app_t *self, int cx, int cy)
{
    draw_power_symbol(cx, cy, 13, 3);
}

// The screen left showing while off. `status` is the line under the symbol.
static void draw_off_screen(const char *status)
{
    epd_clear(EPD_WHITE);
    draw_power_symbol(EPD_WIDTH / 2, 48, 26, 5);
    epd_draw_text((EPD_WIDTH - epd_text_width(status, 1)) / 2, 100, status, 1, EPD_BLACK);
}

void power_boot_check(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return;                                             // reset or first power-up: just start
    }
    if (!buttons_power_on_held(POWER_ON_HOLD_MS)) {
        buttons_prepare_wake();                             // not a real "on": straight back to sleep
        esp_deep_sleep_start();
    }
    ESP_LOGI(TAG, "turning on");

    // Feedback within a second rather than after Wi-Fi: the panel lost its memory while the
    // rail was off, but the "off" screen is known, so tell the panel that's what is showing
    // and change only the status line with a quick partial refresh
    epd_init();
    draw_off_screen("Hold BACK to turn on");
    epd_display_base();
    draw_off_screen("Starting...");
    epd_display_partial();
}

#define MAX_OFF_HOOKS 4
static void (*off_hooks[MAX_OFF_HOOKS])(void);

void power_add_off_hook(void (*fn)(void))
{
    for (int i = 0; i < MAX_OFF_HOOKS; i++) {
        if (off_hooks[i] == NULL || off_hooks[i] == fn) {
            off_hooks[i] = fn;
            return;
        }
    }
}

void power_off(void)
{
    ESP_LOGI(TAG, "turning off");
    for (int i = 0; i < MAX_OFF_HOOKS && off_hooks[i]; i++) {
        off_hooks[i]();                                     // e.g. music: stop, DAC off, save place
    }
    for (int waited = 0; worker_any_pending() && waited < JOB_WAIT_MS; waited += 100) {
        worker_dispatch();                                  // apply results so files get saved
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    epd_init();
    draw_off_screen("Hold BACK to turn on");
    epd_display();                                          // full refresh: a clean image to keep
    sdcard_unmount();                                       // before the rail goes off
    epd_power_down();                                       // panel asleep, peripheral rail off

    wifi_disconnect();
    battery_sleep();
    buttons_prepare_wake();                                 // waits until no power button is held
    esp_deep_sleep_start();
}

static void power_open(const app_t *self)
{
    power_off();
}

static bool power_button(const app_t *self, button_event_t event)
{
    return false;                                           // never reached: power_off() doesn't return
}

const app_t power_app = {
    .id = "power",
    .name = "Power",
    .draw_icon = power_icon,
    .open = power_open,
    .button = power_button,
    .no_resume = true,
};
