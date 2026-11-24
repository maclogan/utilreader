#include "sdcard.h"

#include <stdlib.h>
#include "board_power.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"                    // esp_timer_get_time(): how often the card is checked
#include "esp_vfs_fat.h"
#include "ff.h"                          // f_mkfs()
#include "diskio_impl.h"                 // ff_diskio_register(): the format wrapper
#include "diskio_sdmmc.h"                // ff_diskio_get_pdrv_card(): the card's FatFs drive (needs ff.h first)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define PIN_CMD 10
#define PIN_D0  11
#define PIN_CLK 12

#define FORMAT_AU   (32 * 1024)          // cluster size when formatting: quick for big files

static sdmmc_card_t *card;              // NULL = not mounted
static uint32_t generation;             // goes up with every successful mount
static int64_t last_check_us;           // when the mounted card last answered a status check
#define CHECK_EVERY_US  (1000 * 1000)   // sdcard_mount() asks the card at most this often
static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;

static void take(void)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);      // first call: main task at start-up
    }
    xSemaphoreTake(lock, portMAX_DELAY);
}

static void give(void)
{
    xSemaphoreGive(lock);
}

// Mounts the card (lock held). `format`: if there's no FAT file system on it (blank, or
// exFAT, which this firmware can't read), make one. Returns the error, ESP_OK when mounted.
static esp_err_t mount_locked(bool format)
{
    if (card != NULL) {
        return ESP_OK;
    }
    rail_acquire();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;               // 40 MHz; plenty for audio
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_CLK;
    slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0;
    slot.flags = 0;                                         // the board has 10k pull-ups
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = format,                   // otherwise never wipe someone's card
        .max_files = 4,
        .allocation_unit_size = format ? FORMAT_AU : 0,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_ROOT, &host, &slot, &cfg, &card);
    if (err != ESP_OK && host.max_freq_khz != SDMMC_FREQ_DEFAULT) {
        // Some cards or long traces don't manage high speed: try 20 MHz
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        err = esp_vfs_fat_sdmmc_mount(SDCARD_ROOT, &host, &slot, &cfg, &card);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s (%s)", err == ESP_FAIL ? "card found, but not FAT32 (exFAT? Settings > Format fixes it)"
                                                 : "no card, or it didn't answer", esp_err_to_name(err));
        card = NULL;
        rail_release();
    } else {
        generation++;
        last_check_us = esp_timer_get_time();
        ESP_LOGI(TAG, "mounted: %s, %llu MB", card->cid.name,
                 ((unsigned long long)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    }
    return err;
}

static void unmount_locked(void)
{
    if (card != NULL) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_ROOT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
        }
        card = NULL;
        gpio_config_t off = {                               // nothing may feed the card once the rail drops
            .pin_bit_mask = (1ULL << PIN_CMD) | (1ULL << PIN_D0) | (1ULL << PIN_CLK),
            .mode = GPIO_MODE_DISABLE,
        };
        gpio_config(&off);
        rail_release();
        ESP_LOGI(TAG, "unmounted");
    }
}

// A mounted card that no longer answers was pulled out (and maybe put back: a card that's
// been out has reset and forgotten its address, so it doesn't answer either). Drop the stale
// mount so the next mount starts afresh. Lock held.
static void check_locked(void)
{
    int64_t now = esp_timer_get_time();
    if (card == NULL || now - last_check_us < CHECK_EVERY_US) {
        return;
    }
    if (sdmmc_get_status(card) == ESP_OK) {
        last_check_us = now;
        return;
    }
    ESP_LOGW(TAG, "the card stopped answering (taken out?): mounting again");
    unmount_locked();
}

bool sdcard_mount(void)
{
    take();
    check_locked();
    bool ok = mount_locked(false) == ESP_OK;
    give();
    return ok;
}

bool sdcard_remount(void)
{
    take();
    unmount_locked();
    bool ok = mount_locked(false) == ESP_OK;
    give();
    return ok;
}

uint32_t sdcard_generation(void)
{
    return generation;
}

uint32_t sdcard_size_mb(void)
{
    sdmmc_card_t *c = card;
    return c ? (uint32_t)(((uint64_t)c->csd.capacity) * c->csd.sector_size / (1024 * 1024)) : 0;
}

void sdcard_unmount(void)
{
    take();
    unmount_locked();
    give();
}

bool sdcard_mounted(void)
{
    return card != NULL;
}

// ---------- formatting, with progress ----------
//
// While f_mkfs runs, the card's FatFs drive goes through this thin wrapper instead of
// ESP-IDF's: it counts the sectors written (for a progress bar) and skips TRIM. FatFs is
// built with FF_USE_TRIM, so f_mkfs would first ask the card to erase all of itself, which on
// a big card takes minutes with nothing to show for it; a format doesn't need it.

static sdmmc_card_t *fmt_card;
static volatile uint32_t fmt_written, fmt_expected;     // sectors
static volatile bool fmt_running;

static DSTATUS fmt_init(BYTE pdrv) { return 0; }
static DSTATUS fmt_status(BYTE pdrv) { return 0; }

static DRESULT fmt_read(BYTE pdrv, BYTE *buf, uint32_t sector, unsigned count)
{
    return sdmmc_read_sectors(fmt_card, buf, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT fmt_write(BYTE pdrv, const BYTE *buf, uint32_t sector, unsigned count)
{
    esp_err_t err = sdmmc_write_sectors(fmt_card, buf, sector, count);
    fmt_written += count;
    return err == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT fmt_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buf = fmt_card->csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = fmt_card->csd.sector_size;
        return RES_OK;
    case CTRL_TRIM:
        return RES_OK;                                      // skipped (see above)
    default:
        return RES_ERROR;
    }
}

static const ff_diskio_impl_t fmt_impl = {
    .init = fmt_init, .status = fmt_status, .read = fmt_read, .write = fmt_write, .ioctl = fmt_ioctl,
};

// Roughly how many sectors f_mkfs writes: both FATs (zeroed in full), the root directory's
// cluster and a few boot sectors. Good enough for a progress bar.
static uint32_t expected_sectors(const sdmmc_card_t *c, uint32_t au)
{
    uint64_t sectors = c->csd.capacity;
    uint32_t ss = c->csd.sector_size ? c->csd.sector_size : 512;
    uint64_t clusters = sectors * ss / au;
    uint64_t fat_sectors = (clusters * 4 + ss - 1) / ss;
    return (uint32_t)(2 * fat_sectors + au / ss + 64);
}

int sdcard_format_progress(void)
{
    if (!fmt_running) {
        return -1;
    }
    uint32_t e = fmt_expected;
    if (e == 0) {
        return 0;
    }
    uint32_t pct = (uint32_t)((uint64_t)fmt_written * 100 / e);
    return pct > 99 ? 99 : (int)pct;
}

bool sdcard_format(void)
{
    fmt_written = 0;
    fmt_expected = 0;
    fmt_running = true;
    take();
    // The card on its own, without a file system mounted: this works whatever is on it
    // (FAT, exFAT, nothing), which is the point of formatting
    unmount_locked();
    rail_acquire();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_CLK;
    slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0;
    slot.flags = 0;
    sdmmc_card_t *c = calloc(1, sizeof(sdmmc_card_t));
    bool host_up = false, ok = false;
    FRESULT res = FR_NOT_READY;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (c && (err = host.init()) == ESP_OK) {
        host_up = true;
        err = sdmmc_host_init_slot(host.slot, &slot);
        if (err == ESP_OK && (err = sdmmc_card_init(&host, c)) != ESP_OK) {
            host.max_freq_khz = SDMMC_FREQ_DEFAULT;         // as when mounting: try 20 MHz
            err = sdmmc_card_init(&host, c);
        }
    }
    BYTE pdrv = 0xFF;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format: no card, or it didn't answer (%s)", esp_err_to_name(err));
    } else if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        ESP_LOGE(TAG, "format: no free FatFs drive");
    } else {
        fmt_card = c;
        ff_diskio_register(pdrv, &fmt_impl);
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        size_t work_size = 32 * 1024;                       // bigger writes: much faster
        void *work = heap_caps_malloc(work_size, MALLOC_CAP_DMA);
        if (work == NULL) {
            work_size = 4096;
            work = heap_caps_malloc(work_size, MALLOC_CAP_DMA);
        }
        if (work) {
            MKFS_PARM opt = {FM_ANY, 2, 0, 0, FORMAT_AU};
            fmt_expected = expected_sectors(c, FORMAT_AU);
            ESP_LOGW(TAG, "formatting the card: %s, %llu MB", c->cid.name,
                     ((unsigned long long)c->csd.capacity) * c->csd.sector_size / (1024 * 1024));
            res = f_mkfs(drv, &opt, work, work_size);
            if (res != FR_OK) {
                ESP_LOGW(TAG, "format with 32 KB clusters failed (FatFs error %d): letting FatFs choose", res);
                opt.au_size = 0;
                fmt_written = 0;
                res = f_mkfs(drv, &opt, work, work_size);
            }
            free(work);
        }
        ff_diskio_unregister(pdrv);
        fmt_card = NULL;
        if (res != FR_OK) {
            ESP_LOGE(TAG, "format failed: FatFs error %d", res);
        }
    }
    if (host_up) {
        host.deinit();
    }
    free(c);
    rail_release();
    ok = res == FR_OK && mount_locked(false) == ESP_OK;
    give();
    fmt_running = false;
    if (ok) {
        ESP_LOGI(TAG, "formatted");
    }
    return ok;
}
