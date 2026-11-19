#include "battery.h"

#include <stdlib.h>                     // abs()
#include "driver/gpio.h"                // charger status line
#include "driver/i2c_master.h"          // I2C bus to the fuel gauge
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"              // vTaskDelay()
#include "i2c_bus.h"                    // the board's I2C bus

static const char *TAG = "battery";

#define PIN_CHARGING    47              // high while the charger is charging (BAT_STAT through Q7)
#define GAUGE_ADDR      0x62            // AXP2602
#define I2C_HZ          100000
#define TIMEOUT_MS      50

// AXP2602 registers (values from Waveshare's driver for this board)
#define REG_CHIP_ID     0x00            // low 6 bits read 0x1C
#define REG_MODE        0x02            // bit 0: sleep
#define REG_VBAT        0x04            // 16 bit big-endian, low 14 bits = millivolts
#define REG_SOC         0x08            // state of charge, %
#define REG_ADC_CONFIG  0x11            // bits 4-6 switch the measurements on
#define REG_IBAT        0x14            // 16 bit big-endian signed, 0.25 mA per bit

#define CHIP_ID         0x1C
#define FULL_PERCENT    95              // at or above this...
#define IDLE_MA         5               // ...with less current than this either way = full

static i2c_master_dev_handle_t gauge;   // NULL = gauge not found
static int percent = -1;
static battery_state_t state = BATTERY_NONE;
static int millivolts = 0;
static int milliamps = 0;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(gauge, &reg, 1, buf, len, TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(gauge, buf, sizeof(buf), TIMEOUT_MS);
}

// Read-modify-write of one register
static esp_err_t update_reg(uint8_t reg, uint8_t clear, uint8_t set)
{
    uint8_t value;
    esp_err_t err = read_regs(reg, &value, 1);
    if (err == ESP_OK) {
        err = write_reg(reg, (value & ~clear) | set);
    }
    return err;
}

void battery_init(void)
{
    gpio_config_t chg = {
        .pin_bit_mask = 1ULL << PIN_CHARGING,
        .mode = GPIO_MODE_INPUT,                            // driven by Q7 on the board
    };
    gpio_config(&chg);

    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL) {
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GAUGE_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &gauge) != ESP_OK) {
        gauge = NULL;
        return;
    }

    uint8_t id = 0;
    if (read_regs(REG_CHIP_ID, &id, 1) != ESP_OK || (id & 0x3F) != CHIP_ID) {
        ESP_LOGW(TAG, "AXP2602 not found (id 0x%02x)", id);
        i2c_master_bus_rm_device(gauge);
        gauge = NULL;
        return;
    }
    update_reg(REG_MODE, 0x01, 0);                          // out of sleep
    update_reg(REG_ADC_CONFIG, 0, 0x70);                    // measurements on
    vTaskDelay(pdMS_TO_TICKS(350));                         // first conversion (as Waveshare waits)

    battery_sample();
    ESP_LOGI(TAG, "%d mV, %d mA, %d%%", millivolts, milliamps, percent);
}

bool battery_sample(void)
{
    if (gauge == NULL) {
        return false;
    }
    int old_percent = percent;
    battery_state_t old_state = state;

    uint8_t v[2], soc, i[2];
    if (read_regs(REG_VBAT, v, 2) != ESP_OK || read_regs(REG_SOC, &soc, 1) != ESP_OK ||
        read_regs(REG_IBAT, i, 2) != ESP_OK) {
        percent = -1;                                       // gauge not answering
        state = BATTERY_NONE;
        return old_percent != percent || old_state != state;
    }
    millivolts = ((v[0] << 8) | v[1]) & 0x3FFF;
    milliamps = (int16_t)((i[0] << 8) | i[1]) / 4;          // 0.25 mA per bit

    if (millivolts < 2500 || millivolts > 4500 || soc > 100) {
        percent = -1;                                       // no cell on the connector
        state = BATTERY_NONE;
    } else {
        percent = soc;
        if (gpio_get_level(PIN_CHARGING)) {
            state = BATTERY_CHARGING;
        } else if (percent >= FULL_PERCENT && abs(milliamps) < IDLE_MA) {
            state = BATTERY_FULL;                           // on USB, charger finished
        } else {
            state = BATTERY_DISCHARGING;
        }
    }
    return old_percent != percent || old_state != state;
}

int battery_percent(void)
{
    return percent;
}

battery_state_t battery_state(void)
{
    return state;
}

int battery_millivolts(void)
{
    return millivolts;
}

int battery_milliamps(void)
{
    return milliamps;
}

void battery_sleep(void)
{
    if (gauge != NULL) {
        update_reg(REG_MODE, 0, 0x01);
    }
}
