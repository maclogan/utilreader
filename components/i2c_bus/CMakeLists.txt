# The board's I2C bus, shared by the fuel gauge and the clock chip
idf_component_register(SRCS "i2c_bus.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c)
