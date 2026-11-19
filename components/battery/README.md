# battery: AXP2602 fuel gauge and charge status

The board measures its own LiPo: an **AXP2602** fuel gauge sits in the battery's ground
path (10 mΩ sense resistor) and reports voltage, current and state of charge over I2C,
and the **ETA6098** charger's status line reaches GPIO47. The main menu shows the level
in the top-right corner with a lightning bolt while charging.

## What it reads

| | From | Notes |
|---|---|---|
| Percentage | AXP2602 register 0x08 | The gauge's own estimate, 0-100 |
| Voltage | 0x04-0x05 | Low 14 bits, millivolts |
| Current | 0x14-0x15 | Signed, 0.25 mA per bit |
| Charging | GPIO47 | High while the charger is charging |

States (`battery_state()`):

- `BATTERY_CHARGING`: GPIO47 high.
- `BATTERY_FULL`: not charging, at least 95 %, and under 5 mA either way. On USB with
  the charge finished, the board runs from USB and the battery current drops to about zero.
- `BATTERY_DISCHARGING`: anything else.
- `BATTERY_NONE`: the gauge doesn't answer, or the voltage is outside 2.5-4.5 V (no cell
  on the connector). The menu then shows no battery.

`main.c` calls `battery_sample()` every 30 s and redraws the menu when the percentage or
state changes. `battery_sleep()` puts the gauge into its low-power mode before the device
turns off; `battery_init()` wakes it at the next start.

## Wiring

Nothing to wire: plug a 3.7 V single-cell LiPo into the MX1.25 `BAT` connector. Check the
polarity against the board's markings, not the wire colours. The red **CHG** LED lights
while charging; the **Warn** LED lights if the battery is plugged in backwards.

## Pins and addresses

| Signal | GPIO |
|---|---|
| I2C SDA | 13 |
| I2C SCL | 14 |
| Charging status | 47 |

The AXP2602 is at I2C address 0x62, on the bus from `components/i2c_bus`, shared with
the PCF85063A clock chip at 0x51 and brought out on the 14-pin header (SDA, SCL).

## Log

At start-up: `battery: 3912 mV, -38 mA, 74%`, or `AXP2602 not found` if the gauge
doesn't answer. The current's sign follows the gauge; this code only uses its size.
