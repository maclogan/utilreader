# Hardware

What the firmware expects to be connected, and every pin it uses. Pin facts come from
Waveshare's [Rev1.1 schematic](https://github.com/waveshareteam/ESP32-S3-LR1121-ePaper-2.9/blob/main/hardware/schematics/ESP32-S3-LR1121-ePaper-2.9-XF-Schematics.pdf)
and [hardware notes](https://github.com/waveshareteam/ESP32-S3-LR1121-ePaper-2.9/blob/main/HARDWARE_REFERENCE.md);
see also the [wiki](https://docs.waveshare.com/ESP32-S3-LR1121-ePaper-2.9).

## Parts

| Part | Required | Notes |
|---|---|---|
| [Waveshare ESP32-S3-LR1121-ePaper-2.9](https://www.waveshare.com/esp32-s3-lr1121-epaper-2.9.htm) | Yes | HF (850-930 MHz) or LF version; the radio isn't used, so either works. The 2.9" panel comes attached |
| 5-way tact switch | Yes | Only **left**, **right**, **center** and **common** are wired; up and down stay unconnected |
| Momentary push button | Yes | BACK / HOME, and turns the device on. Normally open |
| Slide or toggle switch | Yes | Lock. SPST is enough: closed = locked |
| 3.7 V single-cell LiPo | For battery use | MX1.25 2-pin plug. At least 200 mAh (see Charging) |
| SH1.0 14-pin cable | Yes | To reach header P2 (Waveshare ships one with the board) |
| USB-C cable | Yes | Power, charging, flashing, logs |

No resistors are needed: every control switches its pin to GND, and the firmware enables
the ESP32-S3's internal pull-ups.

## Header P2 (SH1.0, 14-pin)

Pin order from the schematic; confirm it against the silkscreen before wiring.

| Pin | Signal | Used for |
|---|---|---|
| 1 | VBUS (5 V from USB) | **Don't connect controls here** |
| 2 | GND | Controls' common / ground side |
| 3 | TX (GPIO43) | Not used. Don't put an input here: the chip prints boot messages on it |
| 4 | RX (GPIO44) | **Lock switch** |
| 5 | 3V3 | Not needed |
| 6 | GND | Controls' common / ground side |
| 7 | GPIO2 | **BACK / HOME button** |
| 8 | GPIO3 | **5-way center** |
| 9 | GPIO4 | **5-way left** |
| 10 | GPIO5 | **5-way right** |
| 11 | SDA (GPIO13) | Onboard I2C bus (fuel gauge, clock chip), 4.7k pull-ups on the board |
| 12 | SCL (GPIO14) | |
| 13 | BAT_STAT (GPIO47) | Charger status (read by the firmware) |
| 14 | VBAT | Battery voltage, **don't connect controls here** |

### Wiring the controls

```
 5-way switch                       BACK / HOME button      Lock switch
   left   ── GPIO4 (pin 9)           one side ── GPIO2       one side ── RX / GPIO44
   right  ── GPIO5 (pin 10)          other    ── GND         other    ── GND
   center ── GPIO3 (pin 8)                                   (closed = locked)
   common ── GND  (pin 2 or 6)
   up / down: leave unconnected
```

Why these pins: GPIO2-5 are the header's only general-purpose pins. The power button
must wake the chip from deep sleep, which only GPIO0-21 can do, so it's on GPIO2. The lock
switch never needs to wake anything, so it takes the RX pin; logs go out over USB, so the
UART isn't needed. GPIO3 is a strapping pin only if a particular eFuse has been burned
(it hasn't on a stock board), so a switch on it is safe.

### Electrical expectations

- **Logic:** 3.3 V. Contacts only ever connect a pin to GND.
- **Pull-ups:** internal, about 45 kΩ. While the device is on, a closed contact draws
  about 70 µA (the lock switch while locked, for example). While it's off, the 5-way and
  lock pins are floated so they draw nothing; only GPIO2 keeps its pull-up, to detect the
  power button.
- **Debounce:** in firmware (30 ms). No capacitors needed.
- **Hold times:** 500 ms = hold gesture (left/right hold, BACK held = HOME); 1.5 s on
  BACK / HOME = turn on.
- **Long wires:** under 20 cm is fine as is. For longer runs, or if presses register on
  their own, add a 10 kΩ pull-up to 3V3 on the affected pin.

## Onboard buttons and lights

| Button | GPIO | Does |
|---|---|---|
| BOOT | 0 | Hold while pressing RESET to enter download mode |
| RESET | EN | Restarts (also starts the device if it's off) |
| KEY | 1 | Same as BACK / HOME, including turning on. Useful before anything is wired |

| Light | Meaning |
|---|---|
| CHG (red) | Charging; off when full |
| Warn | Battery plugged in backwards |
| RGB (WS2812, GPIO21) | Not used; stays dark |

## Power

### Supply and charging

- **USB-C:** 5 V. Runs the board and charges the battery at the same time.
- **Charger:** ETA6098 switching charger. Charge current is set by R31 = 820 kΩ, about
  **200 mA** per the schematic's table. Pick a cell of at least 200 mAh so that's no more
  than 1C.
- **Battery connector:** MX1.25 2-pin. **Check polarity against the board's +/-
  markings**: battery leads with this plug are not standardised, and the Warn LED lights if
  it's reversed. Unplug USB before plugging or unplugging the battery (Waveshare's advice).
- **Fuel gauge:** AXP2602 at I2C 0x62, measuring through a 10 mΩ resistor. It only
  measures; it doesn't control charging.

### Off and on

The board has **no power switch**: whenever a battery is connected, it feeds the 3.3 V
regulator. "Off" is the ESP32-S3's deep sleep with the peripheral rail (GPIO38) switched
off and the fuel gauge asleep. Waveshare measures about **36 µW** in that state (about
11 µA at 3.3 V), so the battery's own self-discharge matters more than the board.

| State | What's powered |
|---|---|
| On | Everything; Wi-Fi as the apps need it |
| Off | ESP32-S3 deep-sleep logic + GPIO2 pull-up; fuel gauge (asleep); clock chip; 3.3 V regulator |

The on-state current hasn't been measured on this board yet; it depends mostly on Wi-Fi.

An empty battery isn't detected: the board runs until the cell's protection circuit cuts
out. Plug in USB and press RESET to start again.

## Full GPIO map

| GPIO | Use | Notes |
|---|---|---|
| 0 | BOOT button | Strapping pin |
| 1 | KEY button | = BACK / HOME; 10k pull-up on the board; wakes from off |
| 2 | **BACK / HOME** (header) | Wakes from off |
| 3 | **5-way center** (header) | |
| 4 | **5-way left** (header) | |
| 5 | **5-way right** (header) | |
| 6 | LR1121 DIO1 | Radio idle; floated when off |
| 7 | Clock chip INT | Not used yet (alarm could wake the device from off) |
| 8 | E-paper BUSY | High while busy |
| 9 | E-paper RST | |
| 10, 11, 12 | microSD CMD, D0, CLK | Not used (1-bit SDMMC) |
| 13, 14 | I2C SDA, SCL | Fuel gauge 0x62, clock chip 0x51 (`components/i2c_bus`) |
| 15, 16 | 32.768 kHz crystal for the ESP32-S3 | Through 0 Ω resistors R27/R28; the chip's slow clock (`CONFIG_RTC_CLK_SRC_EXT_CRYS`) |
| 17 | E-paper DC | |
| 18 | E-paper CS | |
| 19, 20 | USB D-, D+ | Flashing and logs. **Never drive these** |
| 21 | WS2812 RGB LED | Not used |
| 26-32 | Flash | Inside the module |
| 33-37 | Octal PSRAM | Inside the module |
| 38 | Peripheral rail enable | High = on; 1 MΩ pull-down |
| 39 | LR1121 RESET | Held high while on |
| 40 | Shared SPI clock | E-paper + radio |
| 41 | LR1121 BUSY | |
| 42 | LR1121 NSS | Held high so the radio ignores display traffic |
| 43 | UART0 TX (header) | Boot messages; leave free |
| 44 | **Lock switch** (header RX) | |
| 45 | Shared SPI MOSI | Strapping pin (flash voltage); fine as-is |
| 46 | Shared SPI MISO | Radio only; strapping pin |
| 47 | Charger status | High while charging |
| 48 | Fuel gauge IRQ | Not used |

**Free for more add-ons:** the header has no spare GPIOs left. TX can drive an output that
tolerates boot messages, and more I2C devices can share SDA/SCL at other addresses; for
more buttons, an I2C GPIO expander is the way.

## Peripheral rail (GPIO38)

One switched 3.3 V regulator (U6) powers the e-paper, the LR1121 radio, the microSD slot
and the RGB LED. The display driver switches it on at the first `epd_init()`; before the
device turns off, `epd_power_down()` floats every pin that leads to those chips (so none
is powered through its I/O pins) and lets the 1 MΩ pull-down switch the rail off. The
ESP32-S3, fuel gauge and clock chip are on the always-on 3.3 V regulator (U1).

## Timekeeping

| Part | Use |
|---|---|
| PCF85063A clock chip (I2C 0x51, own crystal) | Holds UTC through resets and while off; set after every internet sync and manual change, read at start-up if the ESP32-S3's clock isn't running. About 0.25 µA, on the always-on supply whether used or not |
| 32.768 kHz crystal on GPIO15/16 | The ESP32-S3's slow clock, which keeps time in deep sleep. Without it the internal RC oscillator drifts by minutes a day. If it fails to start, ESP-IDF falls back to the RC oscillator and logs a warning |

## Not used (yet)

| Part | Could be used for |
|---|---|
| LR1121 LoRa radio | Messaging between boards, LoRaWAN. **Fit an antenna before transmitting** |
| microSD slot | Larger book library |
| PCF85063A clock chip's alarm | Waking from off at set times (the clock itself is used: `components/clock_chip`) |
| WS2812 RGB LED | Status light (charging, power-on feedback) |
| 4 grey levels on the panel | Smoother fonts and icons |
