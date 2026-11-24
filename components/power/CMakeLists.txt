# Turning the device off (deep sleep, menu tile) and on (holding BACK / HOME)
idf_component_register(SRCS "power.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES battery buttons display net sdcard esp_hw_support image)
