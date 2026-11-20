#include "sdcard.h"

#include "board_power.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define PIN_CMD 10
#define PIN_D0  11
#define PIN_CLK 12

static sdmmc_card_t *card;              // NULL = not mounted
static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;

static void take(void)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutexStatic(&lock_buf);      // first call: main task at start-up
    }
    xSemaphoreTake(lock, portMAX_DELAY);
}

bool sdcard_mount(void)
{
    take();
    if (card == NULL) {
        rail_acquire();
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;           // 40 MHz; plenty for audio
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = PIN_CLK;
        slot.cmd = PIN_CMD;
        slot.d0 = PIN_D0;
        slot.flags = 0;                                     // the board has 10k pull-ups
        esp_vfs_fat_sdmmc_mount_config_t cfg = {
            .format_if_mount_failed = false,                // never wipe someone's card
            .max_files = 4,
            .allocation_unit_size = 0,
        };
        esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_ROOT, &host, &slot, &cfg, &card);
        if (err != ESP_OK && host.max_freq_khz != SDMMC_FREQ_DEFAULT) {
            // Some cards or long traces don't manage high speed: try 20 MHz
            host.max_freq_khz = SDMMC_FREQ_DEFAULT;
            err = esp_vfs_fat_sdmmc_mount(SDCARD_ROOT, &host, &slot, &cfg, &card);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no card, or not FAT32 (%s)", esp_err_to_name(err));
            card = NULL;
            rail_release();
        } else {
            ESP_LOGI(TAG, "mounted: %s, %llu MB", card->cid.name,
                     ((unsigned long long)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        }
    }
    bool ok = card != NULL;
    xSemaphoreGive(lock);
    return ok;
}

void sdcard_unmount(void)
{
    take();
    if (card != NULL) {
        esp_vfs_fat_sdcard_unmount(SDCARD_ROOT, card);
        card = NULL;
        gpio_config_t off = {                               // nothing may feed the card once the rail drops
            .pin_bit_mask = (1ULL << PIN_CMD) | (1ULL << PIN_D0) | (1ULL << PIN_CLK),
            .mode = GPIO_MODE_DISABLE,
        };
        gpio_config(&off);
        rail_release();
        ESP_LOGI(TAG, "unmounted");
    }
    xSemaphoreGive(lock);
}

bool sdcard_mounted(void)
{
    return card != NULL;
}
