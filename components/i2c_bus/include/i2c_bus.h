/*
 * The board's I2C bus (SDA GPIO13, SCL GPIO14, 4.7k pull-ups on the board), shared by the
 * AXP2602 fuel gauge (0x62) and the PCF85063A clock chip (0x51). Also brought out on the
 * 14-pin header.
 */
#pragma once

#include "driver/i2c_master.h"

// The bus, started on the first call. NULL if it couldn't be started.
i2c_master_bus_handle_t i2c_bus_get(void);
