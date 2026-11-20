# Hardware

What the firmware expects to be connected, and every pin it uses. Pin facts come from
Waveshare's [Rev1.1 schematic](https://github.com/waveshareteam/ESP32-S3-LR1121-ePaper-2.9/blob/main/hardware/schematics/ESP32-S3-LR1121-ePaper-2.9-XF-Schematics.pdf)
and [hardware notes](https://github.com/waveshareteam/ESP32-S3-LR1121-ePaper-2.9/blob/main/HARDWARE_REFERENCE.md);
see also the [wiki](https://docs.waveshare.com/ESP32-S3-LR1121-ePaper-2.9).

## Parts

| Part | Required | Notes |
|---|---|---|
| [Waveshare ESP32-S3-LR1121-ePaper-2.9](https://www.waveshare.com/esp32-s3-lr1121-epaper-2.9.htm) | Yes | HF (850-930 MHz) or LF version; the radio isn't used, so either works. The 2.9" panel comes attached |
| 5-way tact switch | Yes | Only **left**, **right**, **center** and **common** are wired; up and down stay unconnected. Connects through the I/O expander |
| I2C I/O expander module | Yes | **MCP23017** (easiest to find on Amazon) or **PCF8574 / PCF8574A**; the firmware detects which. Reads the 5-way switch and drives the DAC's reset. See [Audio and the I/O expander](#audio-and-the-io-expander) |
| Momentary push button | Yes | BACK / HOME, and turns the device on. Normally open |
| Slide or toggle switch | Yes | Lock. SPST is enough: closed = locked |
| 3.7 V single-cell LiPo | For battery use | MX1.25 2-pin plug. At least 200 mAh (see Charging) |
| SH1.0 14-pin cable | Yes | To reach header P2. See [Connecting to the header](#connecting-to-the-header): Waveshare ships one, otherwise build it from a housing and pre-crimped leads |
| Small perfboard + female pin headers | Recommended | The hub the cable and the modules plug into |
| Adafruit TLV320DAC3100 | For music | I2S DAC with headphone amp and 3.5 mm jack, I2C 0x18 |
| INMP441 I2S microphone | Not yet used | Wired now for a future app |
| microSD card | For music | FAT32, music in a folder (default `/Music`, set in **Settings**). Goes in the board's own slot |
| Headphones | For music | 16-32 Ω. The TLV320's speaker output needs 5 V and isn't used |
| USB-C cable | Yes | Power, charging, flashing, logs |

No resistors are needed: every control switches its pin to GND, and the firmware enables
the internal pull-ups (the ESP32-S3's, or the expander's).

> **Upgrading from older firmware:** the 5-way switch used to go straight to GPIO3-5. From
> this firmware on it **only works through the I/O expander**; GPIO3-5 now carry audio. BACK
> / HOME, the lock switch and the onboard KEY are unchanged, so the device still turns on
> and can be used to reach the menu without the expander.

## Header P2 (SH1.0, 14-pin)

Pin order from the schematic; confirm it against the silkscreen before wiring.

| Pin | Signal | Used for |
|---|---|---|
| 1 | VBUS (5 V from USB) | **Don't connect controls here** |
| 2 | GND | Controls' common / ground side |
| 3 | TX (GPIO43) | **I2S data to the DAC** (TLV320 DIN). Boot messages appear here at start-up; harmless, the DAC ignores them without a bit clock |
| 4 | RX (GPIO44) | **Lock switch** |
| 5 | 3V3 | Power for the expander, DAC and microphone |
| 6 | GND | Controls' common / ground side, modules' GND |
| 7 | GPIO2 | **BACK / HOME button** |
| 8 | GPIO3 | **Microphone data** (INMP441 SD), for a future app |
| 9 | GPIO4 | **I2S bit clock** (TLV320 BCK + INMP441 SCK) |
| 10 | GPIO5 | **I2S word select** (TLV320 WSEL + INMP441 WS) |
| 11 | SDA (GPIO13) | I2C bus: fuel gauge, clock chip (onboard), expander, DAC. 4.7k pull-ups on the board |
| 12 | SCL (GPIO14) | |
| 13 | BAT_STAT (GPIO47) | Charger status (read by the firmware) |
| 14 | VBAT | Battery voltage, **don't connect controls here** |

Pins 1, 13 and 14 stay unconnected: 10 of the 14 are used.

### Connecting to the header

The header is a JST **SH** 1.0 mm socket. Don't solder to it, and don't push loose wires or
Dupont pins into it: the contacts bend easily and the socket is hard to replace. Use a
plug with the wires already crimped on and break it out onto a small perfboard.

**The cable:**

1. **The one in the box.** Waveshare ships an SH1.0 14-pin cable with the board. If it has a
   plug on both ends, cut it in half: two pigtails.
2. **Build one.** 14-pin SH cables are rare on Amazon, and a smaller plug won't fit. Get an
   empty 14-way housing, **JST SHR-14V-S-B** (Digi-Key, Mouser, LCSC), and **pre-crimped JST
   SH leads** (28 AWG; Digi-Key / Mouser "JST SH pre-crimped lead", or Amazon "JST SH 1.0mm
   pre-crimped wire": check it says **SH**, not ZH or GH). Push each contact in until it
   clicks. Only fill the positions you use (2-12) and leave 1, 13 and 14 empty, which also
   keeps 5 V and the battery off the perfboard. Order spare housings.
3. **Reuse Qwiic / STEMMA QT cables.** They're the same SH system: lift a contact's latch
   with a needle, slide it out of its 4-way housing and into the 14-way one.

**The hub:** solder the bare ends to a perfboard with a 3V3 rail, a GND rail, an SDA/SCL
pair shared by the expander and the DAC, and a point for each other signal. Plug the modules
in through female pin headers so any one can be swapped. Heat-shrink wire-to-wire joints.

**Keeping it in:** SH plugs hold by friction only.

- **Strain relief first:** fix the cable 1-2 cm from the plug (zip tie, or hot glue on the
  cable) so a pull on the wires never reaches the socket.
- A strip of Kapton or electrical tape over the plug, or a dab of hot glue where plug meets
  socket, stops it backing out. Hot glue peels off later; never superglue or epoxy.
- Leave some slack between that point and the perfboard.

Before soldering, check the pin order against the board's silkscreen (the numbers above are
from the schematic, and cables can be mirrored). A continuity beep from pin 2 (GND) to the
USB-C shell confirms which end is which.

### Wiring the controls

```
 BACK / HOME button        Lock switch                5-way switch: on the expander
   one side ── GPIO2         one side ── RX / GPIO44     (see below)
   other    ── GND           other    ── GND
                             (closed = locked)
```

Why these pins: the power button must wake the chip from deep sleep, which only GPIO0-21
can do, so it's on GPIO2. The lock switch never needs to wake anything, so it takes the RX
pin; logs go out over USB, so the UART isn't needed. The 5-way switch moved to the I/O
expander to free GPIO3-5 for audio.

### Electrical expectations

- **Logic:** 3.3 V. Contacts only ever connect a pin to GND.
- **Pull-ups:** internal, about 45 kΩ on the ESP32-S3 (100 kΩ on an MCP23017, a weak
  current source on a PCF8574). While the device is on, a closed contact draws about 70 µA
  (the lock switch while locked, for example). While it's off, the lock pin is floated so
  it draws nothing; only GPIO2 keeps its pull-up, to detect the power button.
- **Debounce:** in firmware (30 ms). No capacitors needed. The expander is read every 10 ms.
- **Hold times:** 500 ms = hold gesture (left/right/center hold, BACK held = HOME); 1.5 s
  on BACK / HOME = turn on.
- **Long wires:** under 20 cm is fine as is. For longer runs, or if presses register on
  their own, add a 10 kΩ pull-up to 3V3 on the affected pin.

## Audio and the I/O expander

Three modules hang off the header: the **I/O expander** (5-way switch, DAC reset), the
**TLV320DAC3100** (music out) and the **INMP441** (microphone, for later). Music comes from
the board's own microSD slot. Everything runs on 3.3 V from header pin 5.

```
 Header P2                 Expander         TLV320 DAC       INMP441 mic
 ─────────────             ────────         ──────────       ───────────
 5   3V3   ───────────────  VCC ──────────── VIN ──────────── VDD
 2/6 GND   ───────────────  GND ──────────── GND ──────────── GND, L/R
 11  SDA   ───────────────  SDA ──────────── SDA
 12  SCL   ───────────────  SCL ──────────── SCL
 9   GPIO4 ─────────────────────────────────  BCK ──────────── SCK
 10  GPIO5 ─────────────────────────────────  WSEL ─────────── WS
 3   TX    ─────────────────────────────────  DIN
 8   GPIO3 ──────────────────────────────────────────────────── SD
                            pin 3 ─────────── RST
                            pin 0 ── 5-way left
                            pin 1 ── 5-way right
                            pin 2 ── 5-way center
                            5-way common ── GND
```

### I/O expander

| Expander pin (MCP23017 / PCF8574) | Connects to |
|---|---|
| VCC / VDD | 3V3 (header 5) |
| GND | GND |
| SDA, SCL | SDA (11), SCL (12) |
| GPA0 / P0 | 5-way **left** |
| GPA1 / P1 | 5-way **right** |
| GPA2 / P2 | 5-way **center** |
| GPA3 / P3 | TLV320 **RST** |
| 5-way common | GND |
| GPA4-7, port B / P4-7 | Spare |

- **MCP23017:** tie **RESET to VCC** (some modules leave it floating, and the chip then
  resets at random) and **A0, A1, A2 to GND** (address 0x20). Only the I2C pins and VCC/GND
  go back to the header.
- **PCF8574 / PCF8574A:** any address works (0x20-0x27, A version 0x38-0x3F).
- Avoid the blue I2C **LCD backpack** boards: they're PCF8574s, but P3 is wired to the
  backlight transistor.
- Module pull-ups on SDA/SCL can stay; they're in parallel with the board's 4.7k.
- No expander, or a wiring fault: the log says so at start-up and the 5-way does nothing.
  Everything else works.

### TLV320DAC3100 (Adafruit breakout)

| Breakout pin | Connects to |
|---|---|
| VIN | 3V3 (header 5). Fine for headphones; the speaker output needs 5 V and isn't used |
| GND | GND |
| SDA, SCL | SDA (11), SCL (12). Address 0x18 |
| RST | Expander GPA3 / P3 |
| BCK | GPIO4 (header 9) |
| WSEL | GPIO5 (header 10) |
| DIN | TX / GPIO43 (header 3) |
| MCLK | Unconnected: the DAC's PLL runs from BCK |
| Headphone jack | Your headphones (16-32 Ω) |

### INMP441 microphone (for later)

| Module pin | Connects to |
|---|---|
| VDD | 3V3 |
| GND | GND |
| L/R | GND (left channel) |
| SCK | GPIO4 (shared with the DAC's BCK) |
| WS | GPIO5 (shared with the DAC's WSEL) |
| SD | GPIO3 (header 8) |

The DAC and the microphone share one I2S clock pair: the ESP32-S3 sends audio out on TX and
would read the microphone on GPIO3 at the same sample rate. No firmware uses the microphone
yet.

### microSD card

The board's own slot (1-bit SDMMC on GPIO10-12, powered by the peripheral rail). Format the
card **FAT32** (cards over 32 GB come as exFAT: reformat them as FAT32) and put music in a
folder, `/Music` by default; subfolders are fine (an album per folder plays in order).
Choose another folder in **Settings → Music folder**.

### I2C addresses

| Address | Device |
|---|---|
| 0x18 | TLV320DAC3100 |
| 0x20-0x27 | MCP23017 or PCF8574 |
| 0x38-0x3F | PCF8574A |
| 0x51 | Clock chip (onboard) |
| 0x62 | Fuel gauge (onboard) |

### While off

The modules are on the always-on 3.3 V, so they stay powered in deep sleep. Before turning
off, the firmware powers the DAC down over I2C (rather than holding it in reset, which could
draw current through a pull-up on RST); the microphone sleeps by itself once its clock
stops, and the expander idles. Each draws microamps, but **measure the off-current with
your modules fitted**: a module's power LED alone can draw a milliamp or more (remove it, or
cut its trace, for long battery life).

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
| Off | ESP32-S3 deep-sleep logic + GPIO2 pull-up; fuel gauge (asleep); clock chip; 3.3 V regulator; header modules (DAC powered down, expander, microphone) |

The on-state current hasn't been measured on this board yet; it depends mostly on Wi-Fi.

An empty battery isn't detected: the board runs until the cell's protection circuit cuts
out. Plug in USB and press RESET to start again.

## Full GPIO map

| GPIO | Use | Notes |
|---|---|---|
| 0 | BOOT button | Strapping pin |
| 1 | KEY button | = BACK / HOME; 10k pull-up on the board; wakes from off |
| 2 | **BACK / HOME** (header) | Wakes from off |
| 3 | **Microphone data** (header) | INMP441 SD; I2S input, not used yet |
| 4 | **I2S bit clock** (header) | TLV320 BCK + INMP441 SCK |
| 5 | **I2S word select** (header) | TLV320 WSEL + INMP441 WS |
| 6 | LR1121 DIO1 | Radio idle; floated when off |
| 7 | Clock chip INT | Not used yet (alarm could wake the device from off) |
| 8 | E-paper BUSY | High while busy |
| 9 | E-paper RST | |
| 10, 11, 12 | microSD CMD, D0, CLK | 1-bit SDMMC; 10k pull-ups on the board; on the peripheral rail |
| 13, 14 | I2C SDA, SCL | Fuel gauge 0x62, clock chip 0x51, DAC 0x18, expander (`components/i2c_bus`) |
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
| 43 | **I2S data out** (header TX) | TLV320 DIN. Boot messages go out here before the firmware takes it over; the console is on USB |
| 44 | **Lock switch** (header RX) | |
| 45 | Shared SPI MOSI | Strapping pin (flash voltage); fine as-is |
| 46 | Shared SPI MISO | Radio only; strapping pin |
| 47 | Charger status | High while charging |
| 48 | Fuel gauge IRQ | Not used |

**Free for more add-ons:** the header has no spare GPIOs left. More buttons or outputs go
on the expander's spare pins (GPA4-7 and port B on an MCP23017, P4-7 on a PCF8574), and
more I2C devices can share SDA/SCL at other addresses.

## Peripheral rail (GPIO38)

One switched 3.3 V regulator (U6) powers the e-paper, the LR1121 radio, the microSD slot
and the RGB LED. The display and the SD card each take a reference on it
(`components/board_power`): it switches on with the first user and off with the last.
Before it goes off, every pin that leads to those chips is floated (so none is powered
through its I/O pins) and the 1 MΩ pull-down switches the rail off. The
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
| PCF85063A clock chip's alarm | Waking from off at set times (the clock itself is used: `components/clock_chip`) |
| WS2812 RGB LED | Status light (charging, power-on feedback) |
| 4 grey levels on the panel | Smoother fonts and icons |
