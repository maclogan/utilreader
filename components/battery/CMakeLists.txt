# Battery level + charging state from a MAX17043/MAX17048 fuel gauge on the GPIO_D port (I2C)
idf_component_register(SRCS "battery.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c)
