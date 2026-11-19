# battery: fuel gauge on the GPIO_D port

The CrowPanel charges a LiPo but can't measure it: neither the battery voltage nor the
charger's status pin reaches the ESP32. This component reads an add-on **MAX17043 or
MAX17048 fuel gauge** instead, plugged into the board's **GPIO_D** port, and the main menu
shows the result in its top-right corner:

| Menu shows | When |
|---|---|
| `87%` + battery icon | Running on the battery |
| ⚡ `87%` + battery icon | Charging |
| `Full` + battery icon | 98% or more and no longer draining |
| nothing | No gauge plugged in |

## Hardware

Any MAX17043/44 or MAX17048/49 board works; the driver tells them apart by their version
register.

### MAX17043 board (JST, +, -, SDA, VCC, SCL, GND, QST, ALT)

```
 CrowPanel GPIO_D (HY2.0, P1)       MAX17043 board
   pin 1  GND  --------------------  GND
   pin 2  3V3  --------------------  VCC    (powers the board's I2C pull-up resistors)
   pin 3  GPIO41 ------------------  SDA    (SDA/SCL either way round:
   pin 4  GPIO40 ------------------  SCL     the firmware finds out)

 LiPo ---> board JST
 board + / - ---> CrowPanel BAT socket (SH1.0, 2-pin): + to BAT+, - to BAT-
```

- **QST** (quick-start input) must not float: if it doesn't measure a resistance to GND
  (a pull-down on the board), bridge it to the board's GND pin.
- **ALT** (low-battery alert output): leave unconnected.
- The chip runs from the battery itself, so it only answers with a battery connected.

### MAX17048 board (e.g. Adafruit #5580)

```
 GPIO_D pin 1 GND -> GND, pin 2 3V3 -> VIN, pins 3/4 GPIO41/40 -> SDA/SCL (either way)
 LiPo ---> [gauge BAT in] == [gauge BAT out] ---> CrowPanel BAT socket
```

### Both

- Use the small HY2.0 **GPIO_D** socket, not the larger UART socket, whose power pin is 5 V.
- The CrowPanel's battery socket is SH1.0, so you'll need a JST-to-SH1.0 adapter or a
  splice. **Check polarity**: reversed LiPo leads can damage the gauge and the charger.
- No calibration: the ModelGauge algorithm estimates charge from the cell's voltage
  behaviour, without a current-sense resistor or the battery's capacity. It assumes a
  standard 3.7 V / 4.2 V LiPo (not a 4.35 V "LiHV" cell).

## How it works

- `battery_init()` (called by `main.c` at start-up) opens I2C at 100 kHz on GPIO40/41 and
  probes for the gauge at address **0x36**. If it doesn't answer, it swaps SDA and SCL and
  tries again. It reads the VERSION register (0x08): `0x001x` is a MAX17048/49, anything
  else a MAX17043/44. The log says `fuel gauge found (SDA GPIOx, SCL GPIOy)`, the chip,
  and the first reading, or `no fuel gauge on the GPIO_D port`.
- `battery_sample()` (every 30 s, from `main.c`) reads three 16-bit registers:

  | Register | Meaning | Scale |
  |---|---|---|
  | 0x02 VCELL | Cell voltage | 78.125 µV per bit |
  | 0x04 SOC | State of charge | 1/256 % per bit (high byte = whole %) |
  | 0x16 CRATE | Charge rate (MAX17048/49 only) | 0.208 %/hour per bit, signed (+ = charging) |

- **State on a MAX17048/49**, from CRATE: rate above +0.5 %/h and below 98% = charging;
  98% or more and not draining faster than 0.5 %/h = full; otherwise discharging.
- **State on a MAX17043/44**, which has no CRATE, from the voltage over the last 5 minutes
  (10 samples): plugging in USB lifts the cell voltage at once (charge current through
  the cell's resistance), and it keeps rising while charging.
  - at least 4.15 V, 98% or more, and no more than 10 mV below the window's peak = full
  - at least 25 mV above the window's lowest reading and still at its peak = charging
  - otherwise discharging (unplugging drops the voltage straight away)
  - The rate is estimated from the SOC change across the window. Right after power-on
    the window is empty, so charging shows up within a sample or two of plugging in.
- `battery_sample()` returns true when the percentage or state changed; main then redraws
  the menu with a partial refresh if the menu is on screen. Other screens don't show the
  battery.
- If the gauge stops answering (unplugged), the indicator disappears. Plugging it back in
  needs a restart to be found again.

## API

```c
battery_init();                         // once at start-up
if (battery_sample()) { ... }           // true if the shown % or state changed
int pct = battery_percent();            // 0-100, or -1 = no gauge
battery_state_t s = battery_state();    // NONE / DISCHARGING / CHARGING / FULL
int mv = battery_millivolts();          // cell voltage
int r = battery_rate_tenths();          // charge rate in 0.1 %/h, e.g. -45 = -4.5 %/h
```

## Ideas

- **Time remaining:** `percent * 10 / -rate_tenths` hours while discharging (e.g. 50% at
  -10.0 %/h = 5 h). No capacity needed.
- **Low-battery warning:** the gauge can raise an alert below a set percentage (CONFIG
  register 0x0C, ATHD bits); poll `battery_percent()` in an app's `background()` instead
  for a simple on-screen warning.
- **Power saving:** the gauge hibernates on its own when the current is steady.
