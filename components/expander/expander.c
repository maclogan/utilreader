#include "expander.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"                    // the board's I2C bus

static const char *TAG = "expander";

#define I2C_HZ      100000
#define TIMEOUT_MS  20

// MCP23017 registers (IOCON.BANK = 0, the power-up default)
#define MCP_IODIRA  0x00                // 1 = input
#define MCP_IODIRB  0x01
#define MCP_IOCON   0x0A                // reads 0x00 after power-up
#define MCP_GPPUA   0x0C                // 1 = 100k pull-up on
#define MCP_GPPUB   0x0D
#define MCP_GPIOA   0x12                // pin levels
#define MCP_OLATA   0x14                // output latches

#define OUTPUT_PINS (1 << EXPANDER_DAC_RESET)

typedef enum { NONE, PCF8574, MCP23017 } chip_t;

static chip_t chip = NONE;
static i2c_master_dev_handle_t dev;
static uint8_t outputs = 0xFF;          // output levels (PCF8574: inputs must stay 1 too)
static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;

static bool mcp_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, 2, TIMEOUT_MS) == ESP_OK;
}

static bool write_outputs(void)
{
    if (chip == MCP23017) {
        return mcp_write(MCP_OLATA, outputs);
    }
    return i2c_master_transmit(dev, &outputs, 1, TIMEOUT_MS) == ESP_OK;
}

// At 0x20-0x27 both chips answer. Asking for the MCP23017's IOCON register (0x0A) returns
// 0x00 on an MCP23017. A PCF8574 takes the same byte as new pin levels instead (0x0A: P1 and
// P3 high, the rest low; harmless with switches to GND) and reads them back as 0x0A.
static chip_t identify(void)
{
    uint8_t reg = MCP_IOCON, value = 0xFF;
    if (i2c_master_transmit_receive(dev, &reg, 1, &value, 1, TIMEOUT_MS) != ESP_OK) {
        return NONE;
    }
    return value == 0x00 ? MCP23017 : PCF8574;
}

static bool setup(chip_t c)
{
    chip = c;
    if (c == MCP23017) {
        return mcp_write(MCP_OLATA, outputs) &&
               mcp_write(MCP_IODIRA, (uint8_t)~OUTPUT_PINS) &&      // only the DAC reset drives
               mcp_write(MCP_GPPUA, (uint8_t)~OUTPUT_PINS) &&       // pull-ups on the inputs
               mcp_write(MCP_IODIRB, 0xFF) &&
               mcp_write(MCP_GPPUB, 0xFF);                         // unused port: no floating pins
    }
    return write_outputs();                                         // PCF8574: all pins high
}

bool expander_init(void)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);
    }
    if (dev != NULL) {
        return true;
    }
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL) {
        return false;
    }
    static const uint8_t ranges[][2] = {{0x20, 0x27}, {0x38, 0x3F}};
    for (int r = 0; r < 2; r++) {
        for (uint8_t addr = ranges[r][0]; addr <= ranges[r][1]; addr++) {
            if (i2c_master_probe(bus, addr, TIMEOUT_MS) != ESP_OK) {
                continue;
            }
            i2c_device_config_t cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = I2C_HZ,
            };
            if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
                dev = NULL;
                return false;
            }
            chip_t c = r == 1 ? PCF8574 : identify();               // 0x38-0x3F is only ever a PCF8574A
            if (c == NONE || !setup(c)) {
                ESP_LOGW(TAG, "device at 0x%02x didn't respond like an expander", addr);
                i2c_master_bus_rm_device(dev);
                dev = NULL;
                chip = NONE;
                continue;
            }
            ESP_LOGI(TAG, "%s at 0x%02x", c == MCP23017 ? "MCP23017" : (r ? "PCF8574A" : "PCF8574"), addr);
            return true;
        }
    }
    ESP_LOGW(TAG, "no I/O expander found: the 5-way switch won't work (see HARDWARE.md)");
    return false;
}

bool expander_present(void)
{
    return dev != NULL;
}

bool expander_read(uint8_t *pins)
{
    if (dev == NULL) {
        return false;
    }
    if (chip == MCP23017) {
        uint8_t reg = MCP_GPIOA;
        return i2c_master_transmit_receive(dev, &reg, 1, pins, 1, TIMEOUT_MS) == ESP_OK;
    }
    return i2c_master_receive(dev, pins, 1, TIMEOUT_MS) == ESP_OK;
}

void expander_set_output(int bit, bool high)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (high) {
        outputs |= 1 << bit;
    } else {
        outputs &= ~(1 << bit);
    }
    if (dev != NULL) {
        write_outputs();
    }
    xSemaphoreGive(lock);
}
