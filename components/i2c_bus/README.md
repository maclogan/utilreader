# i2c_bus: the board's I2C bus

`i2c_bus_get()` returns the I2C master bus on SDA GPIO13 / SCL GPIO14 (4.7k pull-ups on
the board), starting it on the first call. Every component that talks I2C gets its bus
here, so they can share it:

| Address | Chip | Component |
|---|---|---|
| 0x62 | AXP2602 fuel gauge | `components/battery` |
| 0x51 | PCF85063A clock chip | `components/clock_chip` |

The same two lines are on the 14-pin header (SDA, SCL), so an add-on I2C device can join
the bus at another address. ESP-IDF's I2C master driver locks the bus per transaction, so
different tasks can use it.
