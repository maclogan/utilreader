# The switched peripheral rail (GPIO38) shared by the display and the microSD card
idf_component_register(SRCS "board_power.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio)
