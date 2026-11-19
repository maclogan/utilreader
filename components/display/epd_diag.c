/*
 * Display wiring diagnostic. Drives the display pins as plain GPIOs (no SPI driver) and
 * logs what it finds. Must run before epd_init() sets up SPI on the same pins.
 *
 *   0. Switches the display's power on (PIN_PWR).
 *   1. Pin survey: each pin read with the pull-up, then the pull-down.
 *        follows the pull -> nothing drives it (fine for ESP->display lines; for BUSY it
 *                            means the wire or the display's power is missing)
 *        always 0 / 1     -> something holds it low / high (shorted, or BUSY driven)
 *   2. Shorts: drive each pin high and low; any other pin that follows is connected to it.
 *   3. Reset: pulse RST, watch BUSY.
 *   4. Commands: bit-bang "software reset" and "refresh" (refresh holds BUSY high ~2 s on a
 *      working panel), with the normal wiring, then DIN/CLK swapped, then CS/DC swapped.
 */
#include "epd_2in13.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"                // esp_rom_delay_us()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "epd_pins.h"

static const char *TAG = "epd_diag";

typedef struct {
    int pin;
    const char *name;
} named_pin_t;

static const named_pin_t PINS[] = {
    {PIN_MOSI, "DIN"}, {PIN_SCLK, "CLK"}, {PIN_CS, "CS"},
    {PIN_DC, "DC"}, {PIN_RST, "RST"}, {PIN_BUSY, "BUSY"},
};
#define PIN_COUNT ((int)(sizeof(PINS) / sizeof(PINS[0])))

// Reads `pin` as an input with the given pull (true = up, false = down)
static int read_with_pull(int pin, bool up)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, up ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(500);                                  // let the pull settle
    return gpio_get_level(pin);
}

static void survey(void)
{
    ESP_LOGI(TAG, "1. pin survey (level with pull-up / pull-down):");
    for (int i = 0; i < PIN_COUNT; i++) {
        int up = read_with_pull(PINS[i].pin, true);
        int down = read_with_pull(PINS[i].pin, false);
        const char *meaning = (up && !down) ? "floating (not driven)"
                            : (!up && !down) ? "held LOW"
                            : (up && down)   ? "held HIGH"
                                             : "?";
        ESP_LOGI(TAG, "   %-4s GPIO%-2d  %d / %d  %s", PINS[i].name, PINS[i].pin, up, down, meaning);
    }
}

static void shorts(void)
{
    ESP_LOGI(TAG, "2. shorts between display pins:");
    int found = 0;
    for (int a = 0; a < PIN_COUNT; a++) {
        for (int b = 0; b < PIN_COUNT; b++) {
            if (a == b || PINS[a].pin == PIN_BUSY) {
                continue;                                   // never drive BUSY: the panel drives it
            }
            gpio_reset_pin(PINS[a].pin);
            gpio_set_direction(PINS[a].pin, GPIO_MODE_OUTPUT);
            gpio_set_level(PINS[a].pin, 1);
            int high = read_with_pull(PINS[b].pin, false);  // pulled down: reads 1 only if driven by a
            gpio_set_level(PINS[a].pin, 0);
            int low = read_with_pull(PINS[b].pin, true);    // pulled up: reads 0 only if driven by a
            if (high == 1 && low == 0) {
                ESP_LOGW(TAG, "   %s (GPIO%d) is connected to %s (GPIO%d)!",
                         PINS[a].name, PINS[a].pin, PINS[b].name, PINS[b].pin);
                found++;
            }
            gpio_reset_pin(PINS[a].pin);
        }
    }
    if (!found) {
        ESP_LOGI(TAG, "   none");
    }
}

// ---------- bit-banged SPI ----------

typedef struct {
    int din, clk, cs, dc;
} wiring_t;

static void bb_setup(const wiring_t *w)
{
    int outs[] = {w->din, w->clk, w->cs, w->dc, PIN_RST};
    for (int i = 0; i < 5; i++) {
        gpio_reset_pin(outs[i]);
        gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
    }
    gpio_set_level(w->cs, 1);
    gpio_set_level(w->clk, 0);
    gpio_set_level(PIN_RST, 1);
    gpio_reset_pin(PIN_BUSY);
    gpio_set_direction(PIN_BUSY, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BUSY, GPIO_PULLDOWN_ONLY);       // reads low if nothing drives it
}

// One byte, SPI mode 0, MSB first, slowly. is_data = DC level (0 = command, 1 = data).
static void bb_write(const wiring_t *w, uint8_t byte, int is_data)
{
    gpio_set_level(w->dc, is_data);
    gpio_set_level(w->cs, 0);
    esp_rom_delay_us(5);
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(w->din, (byte >> bit) & 1);
        esp_rom_delay_us(5);
        gpio_set_level(w->clk, 1);
        esp_rom_delay_us(5);
        gpio_set_level(w->clk, 0);
    }
    esp_rom_delay_us(5);
    gpio_set_level(w->cs, 1);
    esp_rom_delay_us(5);
}

// Milliseconds BUSY is away from `idle`, watching for up to window_ms.
// (SSD1680 panels idle low and go high while busy; JD79661 panels the other way round.)
static int busy_ms(int idle, int window_ms)
{
    int high_ms = 0;
    for (int t = 0; t < window_ms; t++) {
        if (gpio_get_level(PIN_BUSY) != idle) {
            high_ms++;
        }
        esp_rom_delay_us(1000);
        if ((t & 63) == 63) {
            vTaskDelay(1);                                  // let other tasks run during long waits
        }
    }
    return high_ms;
}

// Hardware reset, software reset, then a full refresh. Returns ms BUSY was high during the refresh.
static int try_wiring(const char *label, const wiring_t *w)
{
    bb_setup(w);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    int idle = gpio_get_level(PIN_BUSY);                    // idle level tells the controllers apart

    // SSD1680 commands. A JD79661 reads 0x12 as "refresh", so it also shows up as busy here.
    bb_write(w, 0x12, 0);                                   // software reset
    int after_swreset = busy_ms(idle, 100);

    bb_write(w, 0x22, 0);                                   // display update control 2...
    bb_write(w, 0xF7, 1);                                   // ...full update sequence
    bb_write(w, 0x20, 0);                                   // master activation: refresh now
    int refresh = busy_ms(idle, 4000);

    ESP_LOGI(TAG, "   %-22s BUSY idle %s (%s), busy: after cmd 0x12 %3d ms, "
             "during refresh %4d ms%s",
             label, idle ? "HIGH" : "LOW", idle ? "JD79661?" : "SSD1680?",
             after_swreset, refresh, (after_swreset + refresh) > 300 ? "   <-- panel responded" : "");
    return refresh;
}

void epd_diagnose(void)
{
    ESP_LOGI(TAG, "===== display wiring diagnostic =====");
    gpio_reset_pin(PIN_PWR);                                // the display's power switch: on
    gpio_set_direction(PIN_PWR, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    survey();
    shorts();

    ESP_LOGI(TAG, "3/4. reset and commands (a working panel is busy ~1000-3000 ms in total):");
    wiring_t normal = {PIN_MOSI, PIN_SCLK, PIN_CS, PIN_DC};
    wiring_t din_clk = {PIN_SCLK, PIN_MOSI, PIN_CS, PIN_DC};
    wiring_t cs_dc = {PIN_MOSI, PIN_SCLK, PIN_DC, PIN_CS};
    try_wiring("as wired:", &normal);
    try_wiring("DIN/CLK swapped:", &din_clk);
    try_wiring("CS/DC swapped:", &cs_dc);

    for (int i = 0; i < PIN_COUNT; i++) {
        gpio_reset_pin(PINS[i].pin);                        // hand the pins back to the driver
    }
    ESP_LOGI(TAG, "===== end of diagnostic =====");
}
