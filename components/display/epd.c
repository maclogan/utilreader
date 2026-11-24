#include "epd.h"

#include <stdlib.h>
#include <math.h>                       // powf(): the picture curve
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"                  // esp_timer_get_time(): partial refresh timing
#include "sdkconfig.h"                  // CONFIG_DISPLAY_ROTATE_180
#include "epd_pins.h"                   // PIN_* wiring
#include "board_power.h"                // rail_acquire(), rail_release()

#define EPD_SPI_HOST    SPI2_HOST
#define EPD_SPI_HZ      (4 * 1000 * 1000)
#define BUSY_TIMEOUT_MS 10000
#define THOROUGH_EVERY  10              /* fast full refreshes: every 10th is the normal, slower one */

/* Native panel memory is portrait: 128 px wide, 296 rows */
#define NATIVE_W        128
#define NATIVE_H        296
#define LINE_BYTES      ((NATIVE_W + 7) / 8)
#define BUF_SIZE        (LINE_BYTES * NATIVE_H)

static const char *TAG = "epd";
static spi_device_handle_t spi;
static uint8_t frame[BUF_SIZE];   /* 1 = white, 0 = black, MSB = leftmost */
/* Grey: each pixel is a level 0..3 (black, dark grey, light grey, white) split over two
 * planes: `frame` holds the high bit (so a black/white partial refresh shows a grey frame
 * thresholded at the middle) and `grey_lo` the low bit. Black/white drawing sets both. */
static uint8_t grey_lo[BUF_SIZE];
static bool has_grey;             /* a grey pixel was drawn since the last epd_clear() */
static int fulls;                 /* full refreshes since boot (every so often a thorough one) */

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

/* Waits while the controller is working (BUSY high). Returns roughly how long, in ms. */
static int epd_wait_busy(void)
{
    int waited = 0;
    while (gpio_get_level(PIN_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (waited >= BUSY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "BUSY pin stuck, check the panel's ribbon cable");
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
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* A real full refresh keeps the panel busy for ~2-3 s. Back instantly = the panel never
 * started: it isn't receiving commands (power, ribbon cable) or BUSY isn't connected. */
static void check_full_refresh(int ms)
{
    if (ms < 100) {
        ESP_LOGW(TAG, "full refresh: panel didn't respond (never busy), check power/cable");
    } else {
        ESP_LOGI(TAG, "full refresh took %d ms", ms);
    }
}

/* Takes a reference on the shared rail (board_power.h, which also parks the LoRa radio) and
 * sets up GPIO + SPI. Runs on the first epd_init() after boot and again after
 * epd_power_down(). */
static bool bus_ready = false;

static void epd_bus_init(void)
{
    if (bus_ready) {
        return;
    }
    bus_ready = true;

    rail_acquire();                                     /* the peripheral rail on (if not already) */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,                              /* only the radio uses MISO */
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

/* ---------- SSD16xx controller ---------- */

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

/* Gate count, scan direction and full window: needed after reset and before each partial */
static void ssd_set_geometry(void)
{
    epd_cmd(0x01);                  /* driver output control: 296 gate lines */
    epd_data((NATIVE_H - 1) & 0xFF);
    epd_data((NATIVE_H - 1) >> 8);
    epd_data(0x00);
    epd_cmd(0x11);                  /* data entry: X+, Y+ */
    epd_data(0x03);
    ssd_set_full_window();
}

static void ssd_init(void)
{
    epd_wait_busy();
    epd_cmd(0x12);                  /* software reset */
    epd_wait_busy();

    ssd_set_geometry();

    epd_cmd(0x3C);                  /* border waveform */
    epd_data(0x05);
    epd_cmd(0x21);                  /* display update control */
    epd_data(0x00);
    epd_data(0x80);
    epd_cmd(0x18);                  /* use internal temperature sensor */
    epd_data(0x80);
    epd_wait_busy();
}

/* Waveshare's 4-grey waveform for this panel (EPD_2in9_V2.c, "Gray4"): 153 bytes of LUT,
 * then EOPT, VGH, VSH1, VSH2, VSL, VCOM. Pixel waveform = (RAM 0x26 bit, RAM 0x24 bit):
 * 00 white, 01 light grey, 10 dark grey, 11 black. */
static const uint8_t lut_grey4[159] = {
    0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x60, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x28, 0x60, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2A, 0x60, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x05, 0x14, 0x00, 0x00,
    0x1E, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x02, 0x00, 0x05, 0x14, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x24, 0x22, 0x22, 0x22, 0x23, 0x32, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0xAE, 0x32, 0x28,
};

/* ---------- partial refresh modes (Settings > Refresh) ----------
 * STOCK     the chip's own partial waveform: ~890 ms busy, nudges every pixel (the rest dims)
 * CLEAN     Waveshare's partial waveform for this panel, changed: every pixel is driven towards its
 *           colour (3 x 10 frames): changed ones flip, unchanged ones are reinforced (undriven they
 *           fade towards white; the original's one-frame "keep" let blacks grey): ~830 ms
 * QUICK     the same with its drive phase run twice instead of three times (~2/3 the time)
 * QUICKEST  run once (~1/3 the time): fastest, but the change may come out a little faint
 * (A forced-100 C variant of STOCK was measured too: 880 ms, no faster.) */
static int partial_mode = EPD_PARTIAL_STOCK;
static bool registers_reset_by_partial;    /* CLEAN reset the controller's registers */
static int partials_logged;

/* Rows: (previous, new) pixel = black->black, black->white, white->black, white->white. Every
 * pixel is driven towards the colour it should be, all through the refresh: changed ones flip,
 * and unchanged ones are pushed the way they already are (no visible change, no flicker). Left
 * undriven, pixels on this panel fade towards white during a refresh (Waveshare's original
 * gives them only a one-frame "keep", so blacks greyed over a few refreshes; none at all was
 * worse). */
static const uint8_t lut_clean[153] = {
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

void epd_set_partial_mode(int mode)
{
    partial_mode = mode < 0 || mode >= EPD_PARTIAL_MODES ? EPD_PARTIAL_STOCK : mode;
    partials_logged = 0;                    /* log the new mode's timings */
}

int epd_partial_mode(void)
{
    return partial_mode;
}

static void ssd_load_lut(const uint8_t *lut)
{
    epd_cmd(0x32);                  /* waveform LUT */
    epd_data_buf(lut, 153);
    epd_wait_busy();
    epd_cmd(0x3F);                  /* end option */
    epd_data(lut[153]);
    epd_cmd(0x03);                  /* gate voltage */
    epd_data(lut[154]);
    epd_cmd(0x04);                  /* source voltages VSH1, VSH2, VSL */
    epd_data_buf(lut + 155, 3);
    epd_cmd(0x2C);                  /* VCOM */
    epd_data(lut[158]);
}

/* Sends `buf` with every bit flipped (the grey planes are stored the other way up) */
static void epd_data_inverted(const uint8_t *buf, size_t len)
{
    uint8_t chunk[256];
    while (len > 0) {
        size_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
        for (size_t i = 0; i < n; i++) {
            chunk[i] = ~buf[i];
        }
        epd_data_buf(chunk, n);
        buf += n;
        len -= n;
    }
}

/* ---------- panel control ---------- */

/* ---------- the display task ----------
 *
 * A refresh keeps the panel busy for 0.3-3 s. Rather than make the caller wait, every
 * epd_display*() call takes a snapshot of the frame buffer and hands it to this task, then
 * returns at once, so the app can deal with the next button press straight away. Requests
 * that pile up while the panel is busy collapse into one: when it's free, the task shows
 * the latest snapshot (a full refresh if any of them asked for one). Five quick presses
 * therefore cost at most two refreshes, and the screen always ends up on the newest state.
 *
 * Only this task talks to the panel, except epd_sleep() / epd_power_down(), which wait for
 * it to finish first. */

typedef enum { OP_NONE, OP_PARTIAL, OP_FULL, OP_BASE } op_t;

static SemaphoreHandle_t snap_lock;     /* guards everything below */
static uint8_t *snap, *snap_lo;         /* the frame (both planes) the next refresh shows */
static bool snap_grey;
static op_t pending = OP_NONE;
static bool pending_init;               /* reset + set up the controller before the next op */
static EventGroupHandle_t state;        /* IDLE bit: nothing pending or running */
#define IDLE_BIT BIT0

static TaskHandle_t task;
static bool screen_grey;                /* main side: the last full refresh showed grey */
static uint8_t *shown;                  /* task only: the black/white image being shown */

static bool asleep = true;              /* panel needs epd_init() before it can show anything */

bool epd_is_awake(void)
{
    return !asleep;
}

/* Black/white image into both RAMs: 0x24 is the new image, 0x26 the "previous" one a later
 * partial refresh compares against */
static void write_base(const uint8_t *img)
{
    ssd_set_full_window();
    epd_cmd(0x24);
    epd_data_buf(img, BUF_SIZE);
    ssd_set_full_window();
    epd_cmd(0x26);
    epd_data_buf(img, BUF_SIZE);
}

/* Full refresh in 4 greys with the host-loaded waveform (~2.3 s), then back to the chip's
 * own waveforms with both RAMs holding the black/white version of the frame, so a
 * following partial refresh leaves the grey pixels alone unless they change. Takes the
 * snapshot (lock held on entry, released inside). */
static void display_grey(void)
{
    xSemaphoreGive(snap_lock);
    registers_reset_by_partial = false;     /* the resets below set everything up again */
    epd_reset();
    epd_wait_busy();
    epd_cmd(0x12);                  /* software reset */
    epd_wait_busy();
    epd_cmd(0x74);                  /* analog block control */
    epd_data(0x54);
    epd_cmd(0x7E);                  /* digital block control */
    epd_data(0x3B);
    ssd_set_geometry();
    epd_cmd(0x3C);                  /* border waveform */
    epd_data(0x00);
    epd_cmd(0x21);                  /* display update control */
    epd_data(0x00);
    epd_data(0x80);
    epd_wait_busy();
    ssd_load_lut(lut_grey4);

    /* (0x26, 0x24) = (not high bit, not low bit): 00 white .. 11 black */
    xSemaphoreTake(snap_lock, portMAX_DELAY);
    memcpy(shown, snap, BUF_SIZE);
    ssd_set_full_window();
    epd_cmd(0x24);
    epd_data_inverted(snap_lo, BUF_SIZE);
    xSemaphoreGive(snap_lock);
    ssd_set_full_window();
    epd_cmd(0x26);
    epd_data_inverted(shown, BUF_SIZE);
    epd_cmd(0x22);                  /* display with the loaded LUT (no OTP LUT, no temperature) */
    epd_data(0xC7);
    epd_cmd(0x20);
    int ms = epd_wait_busy();
    if (ms < 100) {
        check_full_refresh(ms);
    } else {
        ESP_LOGI(TAG, "grey refresh took %d ms", ms);
    }

    epd_reset();                    /* registers back to their defaults; the image stays */
    ssd_init();
    write_base(shown);
}

/* The chip's black/white full refresh: the thorough one (~3 s, flashes several times) or,
 * with CONFIG_DISPLAY_FAST_REFRESH, its shorter variant (~1.5 s), chosen by telling the
 * chip it is hot. The fast one clears ghosting less well, so every THOROUGH_EVERY-th
 * refresh is the thorough one. */
static void display_bw(void)
{
    if (registers_reset_by_partial) {   /* set the controller up again after a CLEAN partial */
        epd_reset();
        ssd_init();
        registers_reset_by_partial = false;
    }
    write_base(shown);
#if CONFIG_DISPLAY_FAST_REFRESH
    bool fast = fulls % THOROUGH_EVERY != 0;
#else
    bool fast = false;
#endif
    if (fast) {
        epd_cmd(0x18);              /* internal temperature sensor */
        epd_data(0x80);
        epd_cmd(0x22);              /* load the temperature */
        epd_data(0xB1);
        epd_cmd(0x20);
        epd_wait_busy();
        epd_cmd(0x1A);              /* then override it: 100 C selects the short waveform */
        epd_data(0x64);
        epd_data(0x00);
        epd_cmd(0x22);              /* load the waveform for that temperature */
        epd_data(0x91);
        epd_cmd(0x20);
        epd_wait_busy();
        epd_cmd(0x22);              /* full refresh with it */
        epd_data(0xC7);
    } else {
        epd_cmd(0x22);              /* full refresh (reads the real temperature) */
        epd_data(0xF7);
    }
    epd_cmd(0x20);
    int ms = epd_wait_busy();
    if (ms >= 100) {
        ESP_LOGI(TAG, "%s refresh took %d ms", fast ? "fast" : "full", ms);
    } else {
        check_full_refresh(ms);
    }
}

/* Partial refresh: only pixels that differ from the previous image change. Three ways
 * (Settings > Refresh, see partial_mode); each logs how long the panel was busy. */
static void display_partial(void)
{
    int64_t t0 = esp_timer_get_time();
    int busy;
    if (partial_mode != EPD_PARTIAL_STOCK) {
        /* As Waveshare's driver: a reset pulse, their LUT (voltages stay at the reset defaults),
         * RAM ping-pong for display mode 2 (the chip keeps the previous image), clock on,
         * the new image, a mode-2 update with the loaded LUT */
        gpio_set_level(PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        registers_reset_by_partial = true;
        uint8_t lut[sizeof(lut_clean)];
        memcpy(lut, lut_clean, sizeof(lut));
        lut[66] = partial_mode == EPD_PARTIAL_QUICKEST ? 0 : partial_mode == EPD_PARTIAL_QUICK ? 1 : 2;   /* group 0 repeats */
        epd_cmd(0x32);
        epd_data_buf(lut, sizeof(lut));
        epd_wait_busy();
        static const uint8_t options[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00};
        epd_cmd(0x37);
        epd_data_buf(options, sizeof(options));
        epd_cmd(0x3C);
        epd_data(0x80);
        epd_cmd(0x22);
        epd_data(0xC0);
        epd_cmd(0x20);
        epd_wait_busy();
        ssd_set_geometry();
        epd_cmd(0x24);
        epd_data_buf(shown, BUF_SIZE);
        epd_cmd(0x22);
        epd_data(0x0F);
        epd_cmd(0x20);
        busy = epd_wait_busy();
    } else {
        if (registers_reset_by_partial) {   /* coming from CLEAN: set the controller up again */
            epd_reset();
            ssd_init();
            registers_reset_by_partial = false;
        }
        epd_cmd(0x3C);                      /* border waveform: keep the border as-is */
        epd_data(0x80);
        ssd_set_geometry();
        epd_cmd(0x24);                      /* new image; only pixels that differ from 0x26 change */
        epd_data_buf(shown, BUF_SIZE);
        epd_cmd(0x22);                      /* partial refresh with the chip's own waveform */
        epd_data(0xFF);
        epd_cmd(0x20);
        busy = epd_wait_busy();
        ssd_set_full_window();              /* what's on screen now becomes the next "previous" image */
        epd_cmd(0x26);
        epd_data_buf(shown, BUF_SIZE);
    }
    if (partials_logged < 6 || partials_logged % 25 == 0) {
        static const char *const names[] = {"stock", "clean", "quick", "quickest"};
        ESP_LOGI(TAG, "partial refresh (%s): panel busy %d ms, %d ms in all", names[partial_mode], busy,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    partials_logged++;
}

static void display_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (1) {
            xSemaphoreTake(snap_lock, portMAX_DELAY);
            op_t op = pending;
            bool init = pending_init;
            pending = OP_NONE;
            pending_init = false;
            if (op == OP_NONE && !init) {
                xEventGroupSetBits(state, IDLE_BIT);
                xSemaphoreGive(snap_lock);
                break;
            }
            if (op == OP_FULL && snap_grey) {
                display_grey();             /* takes its own snapshot, releases the lock */
                fulls++;
                continue;
            }
            if (op == OP_PARTIAL && !init && memcmp(shown, snap, BUF_SIZE) == 0) {
                xSemaphoreGive(snap_lock);              /* same picture as on screen: nothing to do */
                continue;
            }
            memcpy(shown, snap, BUF_SIZE);
            xSemaphoreGive(snap_lock);

            if (init) {
                epd_reset();
                ssd_init();
                registers_reset_by_partial = false;
            }
            switch (op) {
            case OP_FULL:
                display_bw();
                fulls++;
                break;
            case OP_PARTIAL:
                display_partial();
                break;
            case OP_BASE:
                write_base(shown);
                break;
            default:
                break;
            }
        }
    }
}

static void *big_alloc(void)
{
    void *p = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(BUF_SIZE);
}

static void task_start(void)
{
    if (task) {
        return;
    }
    snap = big_alloc();
    snap_lo = big_alloc();
    shown = big_alloc();
    assert(snap && snap_lo && shown);
    snap_lock = xSemaphoreCreateMutex();
    state = xEventGroupCreate();
    xEventGroupSetBits(state, IDLE_BIT);
    xTaskCreate(display_task, "epd", 4096, NULL, 6, &task);
}

/* Queues `op` with a snapshot of the frame buffer. A full refresh outranks a partial one;
 * either way the newest snapshot is what gets shown. */
static void request(op_t op)
{
    /* Leaving a grey screen: a partial refresh can't clear grey pixels whose black/white
     * value doesn't change (light grey -> white), so it would leave a faint ghost of the
     * picture. Use a full refresh then. */
    if (op == OP_PARTIAL && screen_grey && !has_grey) {
        op = OP_FULL;
    }
    if (op == OP_FULL) {
        screen_grey = has_grey;
    }
    xSemaphoreTake(snap_lock, portMAX_DELAY);
    memcpy(snap, frame, BUF_SIZE);
    memcpy(snap_lo, grey_lo, BUF_SIZE);
    snap_grey = has_grey;
    if (op != OP_PARTIAL || pending == OP_NONE) {
        pending = op;
    }
    xEventGroupClearBits(state, IDLE_BIT);
    xSemaphoreGive(snap_lock);
    xTaskNotifyGive(task);
}

void epd_wait_idle(void)
{
    if (task) {
        xEventGroupWaitBits(state, IDLE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    }
}

void epd_init(void)
{
    epd_bus_init();
    task_start();
    xSemaphoreTake(snap_lock, portMAX_DELAY);
    pending_init = true;            /* done by the task, before the refresh that follows */
    xSemaphoreGive(snap_lock);
    asleep = false;
    epd_clear(EPD_WHITE);
}

void epd_display_base(void)
{
    request(OP_BASE);
    epd_wait_idle();                /* callers rely on it being in place for the next partial */
}

void epd_display(void)
{
    request(OP_FULL);
}

bool epd_frame_has_grey(void)
{
    return has_grey;
}

void epd_display_partial(void)
{
    /* The panel must still be awake from the last epd_display() / partial:
     * don't call epd_sleep() in between. */
    if (asleep) {
        ESP_LOGW(TAG, "partial refresh skipped: panel is asleep");
        return;
    }
    request(OP_PARTIAL);
}

void epd_sleep(void)
{
    if (!bus_ready) {
        return;
    }
    epd_wait_idle();
    epd_cmd(0x10);                  /* deep sleep mode 1: RAM is kept, wake with a reset */
    epd_data(0x01);
    asleep = true;
}

void epd_power_down(void)
{
    bool had_rail = bus_ready;
    if (bus_ready) {
        epd_wait_idle();
        epd_wait_busy();
        epd_sleep();
        spi_bus_remove_device(spi);
        spi_bus_free(EPD_SPI_HOST);
        bus_ready = false;
    }
    asleep = true;

    // Float the display's pins (an output left high would feed the unpowered panel through
    // its protection diodes), then drop the display's reference on the rail: it switches off
    // with the last user, which floats the radio, SD and LED pins too
    static const int pins[] = {PIN_MOSI, PIN_SCLK, PIN_CS, PIN_DC, PIN_RST, PIN_BUSY};
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        mask |= 1ULL << pins[i];
    }
    gpio_config_t off = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&off);
    if (had_rail) {
        rail_release();
    }
}

/* ---------- drawing ---------- */

void epd_clear(uint8_t color)
{
    memset(frame, color ? 0xFF : 0x00, sizeof(frame));
    memset(grey_lo, color ? 0xFF : 0x00, sizeof(grey_lo));
    has_grey = false;
}

#ifdef CONFIG_DISPLAY_ROTATE_180
static bool rotated = true;                         /* upside down (menuconfig default; Settings app) */
#else
static bool rotated = false;
#endif
static bool vertical = false;                       /* inside epd_vertical_begin() / _end() */
static int vertical_x, vertical_y;                  /* screen position of the vertical origin */

void epd_set_rotated(bool on)
{
    rotated = on;
}

bool epd_rotated(void)
{
    return rotated;
}

void epd_vertical_begin(int x, int y)
{
    vertical = true;
    vertical_x = x;
    vertical_y = y;
}

void epd_vertical_end(void)
{
    vertical = false;
}

static bool clip_on;                                /* epd_set_clip(): draw only inside this box */
static int clip_x0, clip_y0, clip_x1, clip_y1;

void epd_set_clip(int x, int y, int w, int h)
{
    clip_on = true;
    clip_x0 = x;
    clip_y0 = y;
    clip_x1 = x + w;
    clip_y1 = y + h;
}

void epd_clear_clip(void)
{
    clip_on = false;
}

/* Where landscape (x, y) lives in the frame buffers; false if off screen (or outside the clip) */
static bool locate(int x, int y, int *index, uint8_t *mask)
{
    if (vertical) {                 /* quarter turn counter-clockwise: drawing x runs up the screen */
        int sx = vertical_x + y;
        y = vertical_y - x;
        x = sx;
    }
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return false;
    }
    if (clip_on && (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)) {
        return false;
    }
    /* Rotate landscape coordinates onto the portrait panel memory. The default is the
     * orientation Waveshare's examples use on this board. */
    int nx, ny;
    if (rotated) {
        nx = NATIVE_W - 1 - y;
        ny = x;
    } else {
        nx = y;
        ny = NATIVE_H - 1 - x;
    }
    *mask = 0x80 >> (nx % 8);
    *index = ny * LINE_BYTES + nx / 8;
    return true;
}

static inline void set_bit(uint8_t *byte, uint8_t mask, bool on)
{
    if (on) {
        *byte |= mask;
    } else {
        *byte &= ~mask;
    }
}

void epd_draw_pixel(int x, int y, uint8_t color)
{
    int i;
    uint8_t mask;
    if (locate(x, y, &i, &mask)) {
        set_bit(&frame[i], mask, color);
        set_bit(&grey_lo[i], mask, color);
    }
}

void epd_draw_pixel_grey(int x, int y, uint8_t level)
{
    int i;
    uint8_t mask;
    if (level > EPD_GREY_WHITE) {
        level = EPD_GREY_WHITE;
    }
    if (locate(x, y, &i, &mask)) {
        set_bit(&frame[i], mask, level & 2);
        set_bit(&grey_lo[i], mask, level & 1);
        if (level == EPD_GREY_DARK || level == EPD_GREY_LIGHT) {
            has_grey = true;
        }
    }
}

/* Error diffusion (Floyd-Steinberg): each pixel goes to the nearest available level and
 * the rounding error is spread over the neighbours still to come, so on average the
 * area keeps its brightness. Two rows of error are kept (this one and the next). */
// Lightening curve applied to pictures before dithering: the panel's greys look darker than
// their nominal levels. Gamma x 100 (lower = lighter): Settings > Darkness, default menuconfig.
static int picture_gamma = CONFIG_DISPLAY_PICTURE_GAMMA;
static int curve_gamma = -1;                        /* the gamma lut[] was made for */

void epd_set_picture_gamma(int percent)
{
    picture_gamma = percent < 30 ? 30 : percent > 100 ? 100 : percent;
}

int epd_picture_gamma(void)
{
    return picture_gamma;
}

static const uint8_t *picture_curve(void)
{
    static uint8_t lut[256];
    if (curve_gamma != picture_gamma) {
        float g = picture_gamma / 100.0f;
        for (int i = 0; i < 256; i++) {
            lut[i] = (uint8_t)(255.0f * powf(i / 255.0f, g) + 0.5f);
        }
        curve_gamma = picture_gamma;
    }
    return lut;
}

void epd_draw_grey8(int x, int y, int w, int h, const uint8_t *pix, int stride, bool grey)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    const uint8_t *curve = picture_curve();
    int16_t *err = calloc(2 * (w + 2), sizeof(int16_t));
    if (err == NULL) {
        return;
    }
    int16_t *cur = err + 1, *next = err + (w + 2) + 1;      /* index -1 and w are spare */
    const int levels = grey ? 4 : 2;
    const int step = 255 / (levels - 1);
    for (int j = 0; j < h; j++) {
        const uint8_t *row = pix + j * stride;
        memset(next - 1, 0, (w + 2) * sizeof(int16_t));
        for (int i = 0; i < w; i++) {
            int v = curve[row[i]] + cur[i] / 16;
            int q = (v * (levels - 1) + 127) / 255;          /* nearest level */
            q = q < 0 ? 0 : q >= levels ? levels - 1 : q;
            int e = v - q * step;
            cur[i + 1] += e * 7;
            next[i - 1] += e * 3;
            next[i] += e * 5;
            next[i + 1] += e;
            if (grey) {
                epd_draw_pixel_grey(x + i, y + j, (uint8_t)q);
            } else {
                epd_draw_pixel(x + i, y + j, q ? EPD_WHITE : EPD_BLACK);
            }
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
    free(err);
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
