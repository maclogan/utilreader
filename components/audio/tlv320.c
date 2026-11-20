#include "tlv320.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "expander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "tlv320";

#define ADDR        0x18
#define I2C_HZ      400000
#define TIMEOUT_MS  20

// Registers (page, register). From the TLV320DAC3100 datasheet, section 7.
#define PAGE_SELECT     0x00
// page 0
#define P0_RESET        0x01            // write 1: soft reset
#define P0_CLOCK_MUX1   0x04            // [3:2] PLL_CLKIN (01 = BCLK), [1:0] CODEC_CLKIN (11 = PLL)
#define P0_PLL_PR       0x05            // [7] PLL power, [6:4] P, [3:0] R
#define P0_PLL_J        0x06
#define P0_PLL_D_MSB    0x07
#define P0_PLL_D_LSB    0x08
#define P0_NDAC         0x0B            // [7] power, [6:0] value (0 = 128)
#define P0_MDAC         0x0C
#define P0_DOSR_MSB     0x0D            // 10 bits, 0 = 1024
#define P0_DOSR_LSB     0x0E
#define P0_IF_CTRL1     0x1B            // [7:6] format (00 = I2S), [5:4] word length (11 = 32 bit)
#define P0_DAC_PATH     0x3F            // [7:6] L/R DAC power, [5:4] L data, [3:2] R data, [1:0] soft step
#define P0_DAC_VOL_CTRL 0x40            // [3] L mute, [2] R mute
#define P0_DAC_LVOL     0x41            // signed, 0.5 dB steps, -127 (-63.5 dB) .. +48 (+24 dB)
#define P0_DAC_RVOL     0x42
// page 1
#define P1_HP_DRIVERS   0x1F            // [7:6] HPL/HPR power, [4:3] common mode, [2] write 1
#define P1_HP_POP       0x21            // [6:3] driver power-on time, [2:1] ramp step time
#define P1_OUT_ROUTING  0x23            // [7:6] left DAC route (01 = mixer), [3:2] right DAC route
#define P1_HPL_VOL      0x24            // [7] route on, [6:0] attenuation (0 = 0 dB)
#define P1_HPR_VOL      0x25
#define P1_HPL_DRIVER   0x28            // [6:3] gain dB, [2] unmuted
#define P1_HPR_DRIVER   0x29

#define HP_CM_1V65      (2 << 3)        // headphone common mode 1.65 V: centred on a 3.3 V supply
#define HP_DRIVERS_OFF  (0x04 | HP_CM_1V65)
#define HP_DRIVERS_ON   (0xC0 | HP_DRIVERS_OFF)
#define HP_POP_SETTING  ((6 << 3) | (3 << 1))   // 153 ms power-on, 3.9 ms ramp steps: no pop

static i2c_master_dev_handle_t dev;
static int page = -1;                   // page currently selected on the chip

static bool write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, 2, TIMEOUT_MS) == ESP_OK;
}

static bool set_page(int p)
{
    if (page == p) {
        return true;
    }
    if (!write_reg(PAGE_SELECT, (uint8_t)p)) {
        page = -1;
        return false;
    }
    page = p;
    return true;
}

// Writes one register on a page; logs and returns false on an I2C error
static bool w(int p, uint8_t reg, uint8_t value)
{
    if (set_page(p) && write_reg(reg, value)) {
        return true;
    }
    ESP_LOGW(TAG, "write %d/0x%02x failed", p, reg);
    return false;
}

// ---------- clocks ----------

static bool dividers(uint32_t codec_clkin, uint32_t rate, tlv320_clocks_t *c)
{
    for (int dosr = 1016; dosr >= 8; dosr -= 8) {           // highest oversampling first
        uint32_t mod = rate * (uint32_t)dosr;               // DAC_MOD_CLK
        if (mod < 2800000 || mod > 6200000 || codec_clkin % mod) {
            continue;
        }
        uint32_t nm = codec_clkin / mod;                    // NDAC * MDAC
        for (uint32_t mdac = 1; mdac <= 128; mdac++) {
            if (nm % mdac) {
                continue;
            }
            uint32_t ndac = nm / mdac;
            if (ndac < 1 || ndac > 128 || mdac * dosr < 256 ||   // 256: processing block PRB_P1's needs
                codec_clkin / ndac > 49152000) {
                continue;
            }
            c->ndac = (uint8_t)ndac;
            c->mdac = (uint8_t)mdac;
            c->dosr = (uint16_t)dosr;
            return true;
        }
    }
    return false;
}

bool tlv320_solve(uint32_t rate, tlv320_clocks_t *out)
{
    uint64_t bclk = 64ULL * rate;
    for (int p = 1; p <= 8; p++) {
        if (bclk / p < 512000 || bclk / p > 20000000) {
            continue;
        }
        for (int r = 1; r <= 4; r++) {
            for (int j = 4; j <= 63; j++) {
                uint64_t x = bclk * r * j;
                if (x % p) {
                    continue;
                }
                uint64_t pll = x / p;
                if (pll < 80000000 || pll > 110000000) {
                    continue;
                }
                if (dividers((uint32_t)pll, rate, out)) {
                    out->p = (uint8_t)p;
                    out->r = (uint8_t)r;
                    out->j = (uint8_t)j;
                    return true;
                }
            }
        }
    }
    return false;
}

// Datasheet order: dividers and DAC off, PLL off; program the PLL; power it and let it lock;
// switch CODEC_CLKIN to it; then the dividers
static bool program_clocks(const tlv320_clocks_t *c)
{
    return w(0, P0_DAC_PATH, 0x00) &&
           w(0, P0_NDAC, 0x00) && w(0, P0_MDAC, 0x00) &&
           w(0, P0_PLL_PR, ((c->p & 7) << 4) | (c->r & 0x0F)) &&     // PLL off while programming
           w(0, P0_PLL_J, c->j) &&
           w(0, P0_PLL_D_MSB, 0) && w(0, P0_PLL_D_LSB, 0) &&
           w(0, P0_CLOCK_MUX1, (1 << 2) | 0) &&                      // PLL_CLKIN = BCLK
           w(0, P0_PLL_PR, 0x80 | ((c->p & 7) << 4) | (c->r & 0x0F)) &&
           (vTaskDelay(pdMS_TO_TICKS(10)), true) &&                  // PLL lock time
           w(0, P0_CLOCK_MUX1, (1 << 2) | 3) &&                      // CODEC_CLKIN = PLL_CLK
           w(0, P0_DOSR_MSB, (c->dosr >> 8) & 0x03) &&
           w(0, P0_DOSR_LSB, c->dosr & 0xFF) &&
           w(0, P0_NDAC, 0x80 | (c->ndac & 0x7F)) &&
           w(0, P0_MDAC, 0x80 | (c->mdac & 0x7F));
}

static bool solve_logged(uint32_t rate, tlv320_clocks_t *c)
{
    if (!tlv320_solve(rate, c)) {
        ESP_LOGW(TAG, "no clock settings for %lu Hz", (unsigned long)rate);
        return false;
    }
    ESP_LOGI(TAG, "%lu Hz: P=%d R=%d J=%d NDAC=%d MDAC=%d DOSR=%d", (unsigned long)rate,
             c->p, c->r, c->j, c->ndac, c->mdac, c->dosr);
    return true;
}

// ---------- start / stop ----------

bool tlv320_start(uint32_t rate)
{
    tlv320_clocks_t c;
    if (!solve_logged(rate, &c)) {
        return false;
    }
    if (dev == NULL) {
        i2c_master_bus_handle_t bus = i2c_bus_get();
        if (bus == NULL) {
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
    }

    // Hardware reset through the expander (if there is one; the line idles high), then a
    // soft reset, which on its own is enough when RST is tied high instead
    expander_set_output(EXPANDER_DAC_RESET, false);
    vTaskDelay(pdMS_TO_TICKS(2));
    expander_set_output(EXPANDER_DAC_RESET, true);
    vTaskDelay(pdMS_TO_TICKS(2));
    page = -1;
    if (!w(0, P0_RESET, 0x01)) {
        ESP_LOGW(TAG, "no TLV320DAC3100 at 0x%02x (check SDA/SCL/VIN, and RST high)", ADDR);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    page = -1;

    bool ok = program_clocks(&c) &&
              w(0, P0_IF_CTRL1, (0 << 6) | (3 << 4)) &&            // I2S, 32-bit words, clocks in
              w(0, P0_DAC_VOL_CTRL, 0x0C) &&                         // muted for now
              // Headphone path: DAC -> mixer -> HP volume 0 dB -> driver 0 dB
              w(1, P1_HP_POP, HP_POP_SETTING) &&
              w(1, P1_OUT_ROUTING, (1 << 6) | (1 << 2)) &&
              w(1, P1_HPL_VOL, 0x80) && w(1, P1_HPR_VOL, 0x80) &&
              w(1, P1_HPL_DRIVER, 0x04) && w(1, P1_HPR_DRIVER, 0x04) &&
              w(1, P1_HP_DRIVERS, HP_DRIVERS_ON) &&
              w(0, P0_DAC_PATH, 0xD4);                               // DACs on, L->L, R->R, soft step
    if (!ok) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(160));                                  // headphone driver power-on time
    return true;
}

bool tlv320_set_rate(uint32_t rate)
{
    tlv320_clocks_t c;
    if (dev == NULL || !solve_logged(rate, &c)) {
        return false;
    }
    bool ok = w(0, P0_DAC_VOL_CTRL, 0x0C) &&
              (vTaskDelay(pdMS_TO_TICKS(20)), true) &&               // let the soft mute finish
              program_clocks(&c) &&
              w(0, P0_DAC_PATH, 0xD4);
    return ok;
}

void tlv320_mute(bool mute)
{
    if (dev) {
        w(0, P0_DAC_VOL_CTRL, mute ? 0x0C : 0x00);
    }
}

void tlv320_set_volume_db(float db)
{
    if (dev == NULL) {
        return;
    }
    if (db > 0) {
        db = 0;
    }
    if (db < -63.5f) {
        db = -63.5f;
    }
    int8_t v = (int8_t)(db * 2.0f + (db < 0 ? -0.5f : 0.5f));
    w(0, P0_DAC_LVOL, (uint8_t)v);
    w(0, P0_DAC_RVOL, (uint8_t)v);
}

void tlv320_stop(void)
{
    if (dev == NULL) {
        return;
    }
    w(0, P0_DAC_VOL_CTRL, 0x0C);
    vTaskDelay(pdMS_TO_TICKS(20));
    w(1, P1_HP_DRIVERS, HP_DRIVERS_OFF);                             // driver ramps down first
    vTaskDelay(pdMS_TO_TICKS(50));
    w(0, P0_DAC_PATH, 0x00);
    w(0, P0_NDAC, 0x00);
    w(0, P0_MDAC, 0x00);
    w(0, P0_PLL_PR, 0x11);                                           // PLL off (P=1, R=1)
}
