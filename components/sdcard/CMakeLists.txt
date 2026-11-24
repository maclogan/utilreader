# The board's microSD slot, mounted at /sdcard on demand
idf_component_register(SRCS "sdcard.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_power fatfs sdmmc esp_driver_sdmmc esp_driver_gpio heap esp_timer)
