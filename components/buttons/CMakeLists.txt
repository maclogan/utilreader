# Three push buttons turned into tap / hold / combo events
idf_component_register(SRCS "buttons.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio)
