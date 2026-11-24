# Reader app: e-books uploaded through the web interface, paginated on the device
idf_component_register(SRCS "reader_app.c" "reader_layout.c" "reader_ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core books
                       PRIV_REQUIRES display esp_psram image)
