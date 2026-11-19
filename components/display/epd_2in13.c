#include "epd_2in13.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "sdkconfig.h"                  // CONFIG_DISPLAY_ROTATE_180
#include "epd_pins.h"                   // PIN_* wiring
#include "jd79661_lut.h"                // waveforms for the JD79661 controller

#define EPD_SPI_HOST    SPI2_HOST
#define EPD_SPI_HZ      (4 * 1000 * 1000)
#define BUSY_TIMEOUT_MS 10000

/* Native panel memory is portrait: 122 px wide (padded to 128), 250 rows */
#define NATIVE_W        122
#define NATIVE_H        250
#define LINE_BYTES      ((NATIVE_W + 7) / 8)
#define BUF_SIZE        (LINE_BYTES * NATIVE_H)

static const char *TAG = "epd";
static spi_device_handle_t spi;
static uint8_t frame[BUF_SIZE];   /* 1 = white, 0 = black, MSB = leftmost */

/* Elecrow ships this board with one of two controllers; epd_init() finds out which */
typedef enum { CTRL_UNKNOWN, CTRL_SSD1680, CTRL_JD79661 } controller_t;
static controller_t ctrl = CTRL_UNKNOWN;

/* Classic 5x7 font, ASCII 0x20-0x7E, column-major, LSB = top row */
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01}, {0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63}, {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x02,0x01,0x02,0x04,0x02},
};

/* ---------- low-level SPI / GPIO ---------- */

static void epd_write(bool is_data, const uint8_t *buf, size_t len)
{
    gpio_set_level(PIN_DC, is_data);
    while (len > 0) {
        size_t chunk = len > 2048 ? 2048 : len;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = buf,
        };
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
        buf += chunk;
        len -= chunk;
    }
}

static void epd_cmd(uint8_t cmd)
{
    epd_write(false, &cmd, 1);
}

static void epd_data(uint8_t data)
{
    epd_write(true, &data, 1);
}

static void epd_data_buf(const uint8_t *buf, size_t len)
{
    epd_write(true, buf, len);
}

/* Waits while the controller is working. Returns roughly how long, in ms.
 * BUSY is HIGH while busy on the SSD1680, LOW while busy on the JD79661. */
static int epd_wait_busy(void)
{
    int busy_level = (ctrl == CTRL_JD79661) ? 0 : 1;
    int waited = 0;
    while (gpio_get_level(PIN_BUSY) == busy_level) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (waited >= BUSY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "BUSY pin stuck, check wiring");
            break;
        }
    }
    return waited;
}

static void epd_reset(void)
{
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));                     /* JD79661 wants >= 100 ms after reset */
}

/* A real full refresh keeps the panel busy for ~1-3 s. Back instantly = the panel never
 * started: it isn't receiving commands (power, wiring) or BUSY isn't connected. */
static void check_full_refresh(int ms)
{
    if (ms < 100) {
        ESP_LOGW(TAG, "full refresh: panel didn't respond (never busy), check power/wiring");
    } else {
        ESP_LOGI(TAG, "full refresh took %d ms", ms);
    }
}

/* GPIO + SPI setup, display power on; only needs to happen once per boot */
static void epd_bus_init(void)
{
    static bool ready = false;
    if (ready) {
        return;
    }
    ready = true;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST) | (1ULL << PIN_PWR),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_PWR, 1);                         /* switch the display's 3.3 V on */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &spi));
}

/* After a hardware reset the controller sits idle, and the two report idle differently:
 * SSD1680 BUSY low, JD79661 BUSY high. */
static controller_t detect_controller(void)
{
    return gpio_get_level(PIN_BUSY) ? CTRL_JD79661 : CTRL_SSD1680;
}

/* ---------- SSD1680 (earlier boards) ---------- */

/* RAM window = whole panel, address counters back at the start */
static void ssd_set_full_window(void)
{
    epd_cmd(0x44);                  /* RAM X window, in bytes */
    epd_data(0x00);
    epd_data(LINE_BYTES - 1);
    epd_cmd(0x45);                  /* RAM Y window */
    epd_data(0x00);
    epd_data(0x00);
    epd_data((NATIVE_H - 1) & 0xFF);
    epd_data((NATIVE_H - 1) >> 8);

    epd_cmd(0x4E);                  /* RAM X address counter */
    epd_data(0x00);
    epd_cmd(0x4F);                  /* RAM Y address counter */
    epd_data(0x00);
    epd_data(0x00);
}

static void ssd_init(void)
{
    epd_wait_busy();
    epd_cmd(0x12);                  /* software reset */
    epd_wait_busy();

    epd_cmd(0x01);                  /* driver output control: 250 gate lines */
    epd_data((NATIVE_H - 1) & 0xFF);
    epd_data((NATIVE_H - 1) >> 8);
    epd_data(0x00);

    epd_cmd(0x11);                  /* data entry: X+, Y+ */
    epd_data(0x03);

    ssd_set_full_window();

    epd_cmd(0x3C);                  /* border waveform */
    epd_data(0x05);
    epd_cmd(0x21);                  /* display update control */
    epd_data(0x00);
    epd_data(0x80);
    epd_cmd(0x18);                  /* use internal temperature sensor */
    epd_data(0x80);
    epd_wait_busy();
}

static void ssd_display(void)
{
    /* Write the frame to both RAMs: 0x24 is the new image, 0x26 is the
     * "previous" image that a later partial refresh compares against. */
    ssd_set_full_window();
    epd_cmd(0x24);
    epd_data_buf(frame, BUF_SIZE);
    ssd_set_full_window();
    epd_cmd(0x26);
    epd_data_buf(frame, BUF_SIZE);

    epd_cmd(0x22);                  /* full refresh */
    epd_data(0xF7);
    epd_cmd(0x20);
    check_full_refresh(epd_wait_busy());
}

static void ssd_display_partial(void)
{
    epd_cmd(0x3C);                  /* border waveform: keep the border as-is */
    epd_data(0x80);
    epd_cmd(0x01);                  /* driver output control: 250 gate lines */
    epd_data((NATIVE_H - 1) & 0xFF);
    epd_data((NATIVE_H - 1) >> 8);
    epd_data(0x00);
    epd_cmd(0x11);                  /* data entry: X+, Y+ */
    epd_data(0x03);

    ssd_set_full_window();
    epd_cmd(0x24);                  /* new image; the panel only flips pixels that differ from 0x26 */
    epd_data_buf(frame, BUF_SIZE);

    epd_cmd(0x22);                  /* partial refresh (no full-screen flashing) */
    epd_data(0xFF);
    epd_cmd(0x20);
    epd_wait_busy();

    ssd_set_full_window();          /* what's on screen now becomes the next "previous" image */
    epd_cmd(0x26);
    epd_data_buf(frame, BUF_SIZE);
}

/* ---------- JD79661 (newer boards) ----------
 * Follows the sequence in Elecrow's v1.2 example, which is what the chip expects:
 *   - once: reset, configure, both image buffers white (0x10, 0x13), one refresh
 *   - every update: only the NEW image to 0x13 (1 = white), one waveform upload, refresh
 * The chip keeps the "old" image itself; the host never writes 0x10 again. Every waveform
 * upload swaps the 0x22/0x23 tables (see jd_upload_lut()), so exactly one upload must
 * happen per refresh, and the chip isn't reset again after the first init.
 * Rows go out last to first (otherwise the picture is mirrored left to right). */

static bool jd_ready = false;       /* configured and cleared since power-on / deep sleep */

static bool lut_swap = false;       /* Elecrow alternates the R22/R23 tables every refresh */

static void jd_upload_lut(const uint8_t *r20, const uint8_t *r21, const uint8_t *r22,
                          const uint8_t *r23, const uint8_t *r24)
{
    epd_cmd(0x20);
    epd_data_buf(r20, 56);
    epd_cmd(0x21);
    epd_data_buf(r21, 56);
    epd_cmd(0x24);
    epd_data_buf(r24, 56);
    epd_cmd(lut_swap ? 0x23 : 0x22);
    epd_data_buf(r22, 56);
    epd_cmd(lut_swap ? 0x22 : 0x23);
    epd_data_buf(r23, 56);
    lut_swap = !lut_swap;
}

/* Sends our frame as the new image (0x13), rows last to first. Bits go as they are: with this
 * sequence the chip's data is 1 = white, like our frame (checked on hardware; Elecrow's
 * drawing code suggests the opposite, but its 0xFF "clear" and the panel agree on white). */
static void jd_send_frame(void)
{
    static uint8_t chunk[16 * LINE_BYTES];                  /* 16 rows per SPI transfer */
    epd_cmd(0x13);
    for (int row = 0; row < NATIVE_H; ) {
        int n = 0;
        for (; n < 16 && row < NATIVE_H; n++, row++) {
            const uint8_t *src = &frame[(NATIVE_H - 1 - row) * LINE_BYTES];
            memcpy(&chunk[n * LINE_BYTES], src, LINE_BYTES);
        }
        epd_data_buf(chunk, n * LINE_BYTES);
    }
}

/* Fills an image buffer (0x10 or 0x13) with one byte value */
static void jd_fill(uint8_t cmd, uint8_t value)
{
    static uint8_t chunk[256];
    memset(chunk, value, sizeof(chunk));
    epd_cmd(cmd);
    for (int i = 0; i < BUF_SIZE; i += sizeof(chunk)) {
        int n = (BUF_SIZE - i < (int)sizeof(chunk)) ? BUF_SIZE - i : (int)sizeof(chunk);
        epd_data_buf(chunk, n);
    }
}

static int jd_refresh(void);

static void jd_init(void)
{
    epd_cmd(0x00);                  /* panel setting: scan directions */
    epd_data(0xF7);
    epd_data(0x8A);
    epd_cmd(0x01);                  /* power setting */
    epd_data(0x03);
    epd_data(0x00);
    epd_data(0x3F);
    epd_data(0x3F);
    epd_data(0x03);
    epd_cmd(0x03);                  /* power off sequence */
    epd_data(0x00);
    epd_cmd(0x06);                  /* booster soft start */
    epd_data(0x27);
    epd_data(0x27);
    epd_data(0x2F);
    epd_cmd(0x30);                  /* PLL / frame rate */
    epd_data(0x0D);
    epd_cmd(0x60);                  /* TCON */
    epd_data(0x22);
    epd_cmd(0x82);                  /* VCOM DC */
    epd_data(0x07);
    epd_cmd(0xE3);                  /* power saving */
    epd_data(0x88);
    epd_cmd(0x41);                  /* temperature sensor: internal */
    epd_data(0x00);
    epd_cmd(0x61);                  /* resolution: 128 x 250 */
    epd_data(0x80);
    epd_data(0x00);
    epd_data(0xFA);
    epd_cmd(0x65);                  /* gate/source start: 0 */
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_cmd(0x50);                  /* VCOM + data interval */
    epd_data(0xB7);

    // Clear: both image buffers white, then one full refresh (Elecrow's EPD_Clear + EPD_Update)
    jd_fill(0x10, 0xFF);
    jd_fill(0x13, 0xFF);
    jd_upload_lut(lut_R20_GC, lut_R21_GC, lut_R22_GC, lut_R23_GC, lut_R24_GC);
    jd_refresh();
    jd_ready = true;
}

static int jd_refresh(void)
{
    epd_cmd(0x17);                  /* auto sequence: power on, refresh, power off */
    epd_data(0xA5);
    return epd_wait_busy();
}

static void jd_display(void)
{
    epd_cmd(0x50);                  /* VCOM + data interval, as Elecrow sets it before each image */
    epd_data(0xD7);
    jd_send_frame();
    jd_upload_lut(lut_R20_GC, lut_R21_GC, lut_R22_GC, lut_R23_GC, lut_R24_GC);  /* full ("GC") */
    check_full_refresh(jd_refresh());
}

static void jd_display_partial(void)
{
    epd_cmd(0x50);
    epd_data(0xD7);
    jd_send_frame();
    jd_upload_lut(lut_R20_DU, lut_R21_DU, lut_R22_DU, lut_R23_DU, lut_R24_DU);  /* fast ("DU"): changed pixels only */
    jd_refresh();
}

/* ---------- panel control ---------- */

static bool asleep = true;          /* panel needs epd_init() before it can show anything */

bool epd_is_awake(void)
{
    return !asleep;
}

void epd_init(void)
{
    epd_bus_init();
    asleep = false;

    if (ctrl == CTRL_JD79661 && jd_ready) {
        epd_clear(EPD_WHITE);       /* JD79661: configured once; resetting it again would lose
                                       its internal old/new image state */
        return;
    }

    epd_reset();
    if (ctrl == CTRL_UNKNOWN) {
        ctrl = detect_controller();
        ESP_LOGI(TAG, "controller: %s", ctrl == CTRL_JD79661 ? "JD79661" : "SSD1680");
    }
    if (ctrl == CTRL_JD79661) {
        jd_init();
    } else {
        ssd_init();
    }

    epd_clear(EPD_WHITE);
    ESP_LOGI(TAG, "initialized");
}

void epd_display(void)
{
    if (ctrl == CTRL_JD79661) {
        jd_display();
    } else {
        ssd_display();
    }
}

void epd_display_partial(void)
{
    /* The panel must still be awake from the last epd_display() / partial:
     * don't call epd_sleep() in between. */
    if (asleep) {
        ESP_LOGW(TAG, "partial refresh skipped: panel is asleep");
        return;
    }
    if (ctrl == CTRL_JD79661) {
        jd_display_partial();
    } else {
        ssd_display_partial();
    }
}

void epd_sleep(void)
{
    if (ctrl == CTRL_JD79661) {
        epd_cmd(0x07);              /* deep sleep */
        epd_data(0xA5);
        jd_ready = false;           /* waking needs a reset and a fresh init */
    } else {
        epd_cmd(0x10);              /* deep sleep mode 1: RAM is kept, wake with a reset */
        epd_data(0x01);
    }
    asleep = true;
}

/* ---------- drawing ---------- */

void epd_clear(uint8_t color)
{
    memset(frame, color ? 0xFF : 0x00, sizeof(frame));
}

void epd_draw_pixel(int x, int y, uint8_t color)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }
    /* Rotate landscape coordinates onto the portrait panel memory. The default is the
     * orientation Elecrow's examples use on the CrowPanel 2.13"; menuconfig can flip it. */
#if CONFIG_DISPLAY_ROTATE_180
    int nx = NATIVE_W - 1 - y;
    int ny = x;
#else
    int nx = y;
    int ny = NATIVE_H - 1 - x;
#endif
    uint8_t mask = 0x80 >> (nx % 8);
    uint8_t *byte = &frame[ny * LINE_BYTES + nx / 8];
    if (color) {
        *byte |= mask;
    } else {
        *byte &= ~mask;
    }
}

void epd_draw_hline(int x, int y, int w, uint8_t color)
{
    for (int i = 0; i < w; i++) {
        epd_draw_pixel(x + i, y, color);
    }
}

void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color)
{
    /* Bresenham's line algorithm */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        epd_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void epd_fill_circle(int cx, int cy, int r, uint8_t color)
{
    /* "+ r" rounds the edge so small circles look less blocky */
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r + r) {
                epd_draw_pixel(cx + dx, cy + dy, color);
            }
        }
    }
}

void epd_draw_rect(int x, int y, int w, int h, uint8_t color, bool fill)
{
    for (int j = 0; j < h; j++) {
        if (fill || j == 0 || j == h - 1) {
            epd_draw_hline(x, y + j, w, color);
        } else {
            epd_draw_pixel(x, y + j, color);
            epd_draw_pixel(x + w - 1, y + j, color);
        }
    }
}

static void epd_draw_char(int x, int y, char c, int scale, uint8_t color)
{
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (glyph[col] & (1 << row)) {
                for (int dx = 0; dx < scale; dx++) {
                    for (int dy = 0; dy < scale; dy++) {
                        epd_draw_pixel(x + col * scale + dx, y + row * scale + dy, color);
                    }
                }
            }
        }
    }
}

int epd_text_width(const char *text, int scale)
{
    int n = strlen(text);
    return n > 0 ? n * 6 * scale - scale : 0;
}

int epd_draw_text(int x, int y, const char *text, int scale, uint8_t color)
{
    for (; *text; text++) {
        epd_draw_char(x, y, *text, scale, color);
        x += 6 * scale;             /* 5 px glyph + 1 px gap */
    }
    return x;
}
