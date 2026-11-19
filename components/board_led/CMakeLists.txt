# The board's user LED (switched off at start-up)
idf_component_register(SRCS "board_led.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio)
