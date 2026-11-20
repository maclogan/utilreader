# 5-way switch (left/right/center, via the I/O expander), BACK/HOME button and lock switch turned into events;
# also the power button's part in turning the device on and off
idf_component_register(SRCS "buttons.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_hw_support esp_timer expander)
