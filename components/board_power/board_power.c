#include "board_power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "rail";

// Waveshare ESP32-S3-LR1121-ePaper-2.9 Rev1.1 schematic
#define PIN_PWR         38              // rail enable
#define PIN_LORA_NSS    42              // radio chip select: held high so it ignores display traffic
#define PIN_LORA_RST    39              // radio reset: released so it idles in standby

// Everything else that leads to a chip on the rail, floated while it's off. The display's own
// pins are handled by epd.c, the SD pins also by the SD driver when it unmounts.
static const int rail_pins[] = {
    PIN_LORA_NSS, PIN_LORA_RST,
    41,                                 // radio BUSY
    6,                                  // radio DIO1
    46,                                 // SPI MISO (only the radio drives it)
    10, 11, 12,                         // microSD CMD, D0, CLK
    21,                                 // RGB LED
    PIN_PWR,
};

static int users;
static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;

static void take(void)
{
    // The first call comes from the main task at start-up, before any other task uses it
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
}

void rail_acquire(void)
{
    take();
    if (users++ == 0) {
        // Radio chip select high before the rail comes up, so the radio never takes display
        // traffic as its own; its reset released so it idles in standby
        gpio_set_level(PIN_LORA_NSS, 1);
        gpio_set_level(PIN_LORA_RST, 1);
        gpio_set_level(PIN_PWR, 0);
        gpio_config_t out = {
            .pin_bit_mask = (1ULL << PIN_PWR) | (1ULL << PIN_LORA_NSS) | (1ULL << PIN_LORA_RST),
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&out));
        gpio_set_level(PIN_LORA_NSS, 1);
        gpio_set_level(PIN_LORA_RST, 1);
        gpio_set_level(PIN_PWR, 1);
        vTaskDelay(pdMS_TO_TICKS(50));                      // let the rail settle
        ESP_LOGI(TAG, "on");
    }
    xSemaphoreGive(lock);
}

void rail_release(void)
{
    take();
    if (users > 0 && --users == 0) {
        uint64_t mask = 0;
        for (size_t i = 0; i < sizeof(rail_pins) / sizeof(rail_pins[0]); i++) {
            mask |= 1ULL << rail_pins[i];
        }
        gpio_set_level(PIN_PWR, 0);
        gpio_config_t off = {
            .pin_bit_mask = mask,
            .mode = GPIO_MODE_DISABLE,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&off);                                  // the 1 MOhm pull-down switches it off
        ESP_LOGI(TAG, "off");
    }
    xSemaphoreGive(lock);
}
