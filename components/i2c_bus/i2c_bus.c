#include "i2c_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"            // first-call guard

#define PIN_SDA 13
#define PIN_SCL 14

static const char *TAG = "i2c_bus";

i2c_master_bus_handle_t i2c_bus_get(void)
{
    static i2c_master_bus_handle_t bus;
    static StaticSemaphore_t lock_buf;
    static SemaphoreHandle_t lock;

    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);      // first caller is the main task at start-up
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (bus == NULL) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = -1,                                 // any free I2C controller
            .sda_io_num = PIN_SDA,
            .scl_io_num = PIN_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
        };
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
            ESP_LOGE(TAG, "failed to start");
            bus = NULL;
        }
    }
    xSemaphoreGive(lock);
    return bus;
}
