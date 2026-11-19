#include "storage.h"

#include "esp_log.h"
#include "esp_spiffs.h"                 // flash file system

static const char *TAG = "storage";

bool storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_PATH,
        .partition_label = "storage",                       // see partitions.csv
        .max_files = 4,                                     // files open at the same time
        .format_if_mount_failed = true,                     // first boot: the partition is blank
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
