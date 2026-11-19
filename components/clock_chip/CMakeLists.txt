# PCF85063A clock chip: keeps the time across resets and while off
idf_component_register(SRCS "clock_chip.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES i2c_bus esp_driver_i2c)
