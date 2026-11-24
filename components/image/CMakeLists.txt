# JPEG -> small greyscale picture (TJpgDec from the ESP32-S3 ROM), for drawing on the e-paper
idf_component_register(SRCS "image.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_rom log)
