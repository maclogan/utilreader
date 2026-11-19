# clock_chip: PCF85063A real-time clock

The board has a PCF85063A clock chip (I2C address 0x51, its own 32.768 kHz crystal) on
the always-on 3.3 V supply. It draws about 0.25 µA whether used or not, and keeps counting
through resets and while the device is off, as long as the battery has charge.

| Function | Does |
|---|---|
| `clock_chip_read(&t)` | Reads the time (UTC). False if the chip doesn't answer or its "oscillator stopped" flag is set (it lost power since it was last set) |
| `clock_chip_write(t)` | Sets it (UTC); this also clears the flag |

The first access switches the chip's CLKOUT pin off (it outputs 32 kHz from power-up,
for nothing).

## Who uses it

`components/app_core/local_time.c`:

- **At start-up:** if the ESP32-S3's own clock isn't set (after a reset; after deep sleep
  it has kept counting), it's set from the chip. So the clock and every date-based app are
  right straight away, without waiting for Wi-Fi.
- **After every internet sync** (SNTP, hourly while online) and **every manual change** (the
  Settings app): the chip is set to match.

## Registers used

| Register | Use |
|---|---|
| 0x01 Control_2 | Bits 0-2 = 111: CLKOUT off |
| 0x04-0x0A | Seconds (bit 7 = oscillator stopped), minutes, hours (24 h), day, weekday, month, year (00-99 = 2000-2099), all BCD |

Not used yet: the alarm and timer, whose interrupt line reaches GPIO7 and could wake the
device from "off" at a set time (e.g. to fetch the morning's forecast).
