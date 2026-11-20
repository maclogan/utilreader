# PCF8574 I/O expander: the 5-way switch and the DAC's reset line
idf_component_register(SRCS "expander.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES i2c_bus esp_driver_i2c)
