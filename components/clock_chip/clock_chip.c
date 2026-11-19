#include "clock_chip.h"

#include "esp_log.h"
#include "i2c_bus.h"                    // the shared I2C bus

static const char *TAG = "clock_chip";

#define ADDR        0x51
#define I2C_HZ      100000
#define TIMEOUT_MS  50

// PCF85063A registers
#define REG_CONTROL_2   0x01            // bits 0-2: CLKOUT frequency, 111 = CLKOUT off
#define REG_SECONDS     0x04            // seconds..years follow (7 registers, BCD)
#define OS_FLAG         0x80            // in the seconds register: oscillator stopped since last set
#define COF_OFF         0x07

static i2c_master_dev_handle_t dev;     // NULL until the first successful start()

static uint8_t from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0F);
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

// Adds the chip to the bus and switches its CLKOUT pin off (it runs at 32 kHz from
// power-up, which costs a little current for nothing)
static bool start(void)
{
    if (dev != NULL) {
        return true;
    }
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL || i2c_master_probe(bus, ADDR, TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A not answering");
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        dev = NULL;
        return false;
    }
    uint8_t reg = REG_CONTROL_2, ctrl2;
    if (i2c_master_transmit_receive(dev, &reg, 1, &ctrl2, 1, TIMEOUT_MS) == ESP_OK &&
        (ctrl2 & COF_OFF) != COF_OFF) {
        uint8_t buf[2] = {REG_CONTROL_2, ctrl2 | COF_OFF};
        i2c_master_transmit(dev, buf, sizeof(buf), TIMEOUT_MS);
    }
    return true;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian)
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool clock_chip_read(time_t *out)
{
    if (!start()) {
        return false;
    }
    uint8_t reg = REG_SECONDS, r[7];
    if (i2c_master_transmit_receive(dev, &reg, 1, r, sizeof(r), TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (r[0] & OS_FLAG) {
        ESP_LOGW(TAG, "oscillator stopped since it was set: time not valid");
        return false;
    }
    int sec = from_bcd(r[0] & 0x7F), min = from_bcd(r[1] & 0x7F), hour = from_bcd(r[2] & 0x3F);
    int day = from_bcd(r[3] & 0x3F), month = from_bcd(r[5] & 0x1F), year = 2000 + from_bcd(r[6]);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        return false;
    }
    *out = (time_t)days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
    return true;
}

bool clock_chip_write(time_t t)
{
    if (!start()) {
        return false;
    }
    struct tm u;
    gmtime_r(&t, &u);
    if (u.tm_year < 100 || u.tm_year > 199) {
        return false;                                       // the chip counts years 2000-2099
    }
    uint8_t buf[8] = {
        REG_SECONDS,
        to_bcd(u.tm_sec),                                   // writing seconds clears OS_FLAG
        to_bcd(u.tm_min),
        to_bcd(u.tm_hour),
        to_bcd(u.tm_mday),
        (uint8_t)u.tm_wday,
        to_bcd(u.tm_mon + 1),
        to_bcd(u.tm_year - 100),
    };
    if (i2c_master_transmit(dev, buf, sizeof(buf), TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "write failed");
        return false;
    }
    return true;
}
