# Audio out: TLV320DAC3100 headphone DAC over I2C + I2S, with a PSRAM ring buffer
idf_component_register(SRCS "audio.c" "tlv320.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_gpio i2c_bus expander)
