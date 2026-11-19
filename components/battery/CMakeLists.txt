# Battery level + charging state from the onboard AXP2602 fuel gauge (I2C) and charger status pin
idf_component_register(SRCS "battery.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES i2c_bus esp_driver_i2c esp_driver_gpio)
