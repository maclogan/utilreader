# E-paper driver, 16 px Unicode font, the shared "Loading..." box and the left title bar
idf_component_register(SRCS "epd.c" "epd_diag.c" "font16.c" "ui_loading.c" "ui_sidebar.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio board_power esp_timer
                       EMBED_FILES "unifont16.bin")    # built by tools/make_font.py
