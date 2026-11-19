#include "battery.h"

#include "driver/i2c_master.h"          // I2C bus to the fuel gauge
#include "esp_log.h"

static const char *TAG = "battery";

// GPIO_D port (connector P1: GND, 3V3, IO41, IO40). Which one is SDA depends on the
// cable, so battery_init() tries both.
#define PIN_A           40
#define PIN_B           41
#define GAUGE_ADDR      0x36            // fixed I2C address of the MAX1704x family
#define I2C_HZ          100000
#define TIMEOUT_MS      50

// MAX1704x registers (16 bit, big-endian). VCELL and SOC read the same on both chips
// (the MAX17043's 12-bit VCELL sits in the top bits, so the 78.125 uV scale still holds).
#define REG_VCELL       0x02            // cell voltage, 78.125 uV per bit
#define REG_SOC         0x04            // state of charge, 1/256 % per bit
#define REG_VERSION     0x08            // MAX17048/49: 0x001x; MAX17043/44: lower
#define REG_CRATE       0x16            // MAX17048/49 only: charge rate, 0.208 %/hour per bit

#define CHARGING_RATE   5               // tenths of %/h: above this counts as charging
#define FULL_PERCENT    98              // at or above this, and not draining = full
#define FULL_MV         4150            // MAX17043: at or above this, and not falling = full

// MAX17043 has no charge-rate register, so charging is read from the voltage trend over
// the last HISTORY samples (5 minutes at one sample per 30 s).
#define HISTORY         10
#define RISE_MV         25              // this far above the window's lowest reading = charging
#define FALL_MV         10              // this far below the window's highest = not full any more

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t gauge;   // NULL = no gauge found
static bool has_crate = false;          // MAX17048/49 (true) or MAX17043/44 (false)
static int percent = -1;
static battery_state_t state = BATTERY_NONE;
static int millivolts = 0;
static int rate_tenths = 0;

static int mv_history[HISTORY];         // MAX17043: recent voltages, oldest overwritten first
static int soc_history[HISTORY];        // and SOC in 1/256 %, for an estimated rate
static int history_len = 0;
static int history_next = 0;

static esp_err_t read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    esp_err_t err = i2c_master_transmit_receive(gauge, &reg, 1, buf, 2, TIMEOUT_MS);
    if (err == ESP_OK) {
        *out = (buf[0] << 8) | buf[1];
    }
    return err;
}

// Opens the bus with the given SDA/SCL and checks for the gauge. Leaves it open if found.
static bool try_pins(int sda, int scl)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                                     // any free I2C controller
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,               // breakout boards have pull-ups too
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return false;
    }
    if (i2c_master_probe(bus, GAUGE_ADDR, TIMEOUT_MS) == ESP_OK) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = GAUGE_ADDR,
            .scl_speed_hz = I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &gauge) == ESP_OK) {
            ESP_LOGI(TAG, "fuel gauge found (SDA GPIO%d, SCL GPIO%d)", sda, scl);
            return true;
        }
    }
    i2c_del_master_bus(bus);
    bus = NULL;
    return false;
}

void battery_init(void)
{
    if (!try_pins(PIN_A, PIN_B) && !try_pins(PIN_B, PIN_A)) {
        ESP_LOGI(TAG, "no fuel gauge on the GPIO_D port");
        return;
    }
    uint16_t version = 0;
    read_reg(REG_VERSION, &version);
    has_crate = (version & 0xFFF0) == 0x0010;
    ESP_LOGI(TAG, "%s (version 0x%04x)", has_crate ? "MAX17048/49" : "MAX17043/44", version);

    battery_sample();
    ESP_LOGI(TAG, "%d mV, %d%%", millivolts, percent);
}

// MAX17043: charging state from the voltage trend; also estimates the rate from SOC
static battery_state_t state_from_history(int mv, int soc)
{
    mv_history[history_next] = mv;
    soc_history[history_next] = soc;
    int oldest = history_len < HISTORY ? 0 : (history_next + 1) % HISTORY;
    history_next = (history_next + 1) % HISTORY;
    if (history_len < HISTORY) {
        history_len++;
    }

    int lowest = mv, highest = mv;
    for (int i = 0; i < history_len; i++) {
        lowest = mv_history[i] < lowest ? mv_history[i] : lowest;
        highest = mv_history[i] > highest ? mv_history[i] : highest;
    }

    // Rate: SOC change across the window, in tenths of % per hour (30 s per sample)
    int span = history_len - 1;
    rate_tenths = span > 0 ? (soc - soc_history[oldest]) * 10 * 120 / (256 * span) : 0;

    if (mv >= FULL_MV && percent >= FULL_PERCENT && highest - mv < FALL_MV) {
        return BATTERY_FULL;                                // at the top and holding
    }
    if (mv - lowest >= RISE_MV && highest - mv < FALL_MV) {
        return BATTERY_CHARGING;                            // rose recently and still at its peak
    }
    return BATTERY_DISCHARGING;
}

bool battery_sample(void)
{
    if (gauge == NULL) {
        return false;
    }
    int old_percent = percent;
    battery_state_t old_state = state;

    uint16_t vcell, soc;
    if (read_reg(REG_VCELL, &vcell) != ESP_OK || read_reg(REG_SOC, &soc) != ESP_OK) {
        percent = -1;                                       // gauge unplugged or not answering
        state = BATTERY_NONE;
        return old_percent != percent || old_state != state;
    }

    millivolts = (int)(vcell * 78125LL / 1000000);          // 78.125 uV per bit
    int pct = (soc + 128) >> 8;                             // round 1/256 % to whole %
    percent = pct > 100 ? 100 : pct;                        // the gauge can report a bit over 100

    uint16_t crate;
    if (has_crate && read_reg(REG_CRATE, &crate) == ESP_OK) {
        rate_tenths = (int)((int16_t)crate * 208 / 100);    // 0.208 %/h per bit, in tenths
        if (rate_tenths > CHARGING_RATE && percent < FULL_PERCENT) {
            state = BATTERY_CHARGING;
        } else if (percent >= FULL_PERCENT && rate_tenths >= -CHARGING_RATE) {
            state = BATTERY_FULL;
        } else {
            state = BATTERY_DISCHARGING;
        }
    } else {
        state = state_from_history(millivolts, soc);
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

int battery_rate_tenths(void)
{
    return rate_tenths;
}
