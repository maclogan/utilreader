# E-paper driver, 16 px Unicode font, and the shared "Loading..." box
idf_component_register(SRCS "epd_2in13.c" "epd_diag.c" "font16.c" "ui_loading.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio
                       EMBED_FILES "unifont16.bin")    # built by tools/make_font.py
