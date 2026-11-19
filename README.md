# E-paper app board

Apps on an [Elecrow CrowPanel ESP32 E-Paper HMI 2.13"](https://elecrow.com/wiki/CrowPanel_ESP32_E-Paper_HMI_2.13-inch_Display.html):
an ESP32-S3 with a built-in 250×122 black/white e-paper screen, a 3-way thumbwheel, EXIT
and MENU buttons, and a LiPo battery connector with charger. No wiring needed; an optional
fuel gauge on the GPIO_D port adds a battery level to the menu.

| App | What it does |
|---|---|
| **Weather** | 7-day forecast from [Open-Meteo](https://open-meteo.com) (free, no key), with a details screen per day |
| **Clock** | Local time in big digits, redrawn every minute |
| **Story** | A short Chinese graded-reader story written by Claude each day, with per-word pinyin and meaning lookup and a saved-words list |
| **Cards** | Spaced-repetition flashcards of the words saved in the story |
| **Books** | E-book reader: chapters, reading position and progress kept per book |
| **Transfer** | While open, a web page on your Wi-Fi to add/delete books and see saved words |

At power-on the board returns to where it was: the last app, or the main menu if that's
where you left it (Transfer is never reopened automatically).

## Hardware

- **Module:** ESP32-S3-WROOM-1-N8R8: dual-core 240 MHz, 8 MB flash, 8 MB octal PSRAM,
  2.4 GHz Wi-Fi (no 5 GHz). Flashed over the USB-C port, which shows up as a COM port.
- **Display:** 2.13" 250×122 black/white e-paper. Depending on the production batch the
  controller is an **SSD1680** or a **JD79661**; the driver detects which at start-up.
- **Controls:** thumbwheel (up / down / press in), EXIT, MENU; plus RESET and BOOT.
- **Battery:** 3.7 V LiPo on the 2-pin SH1.0 `BAT` connector, charged over USB by a
  4054-type charger. The ESP32 can't read the battery level or charging state (see below).

## Pinout

Everything is wired on the board; these are the GPIOs the firmware uses (from Elecrow's
schematic, net names in `code` font):

| Function | GPIO | Net | In the firmware |
|---|---|---|---|
| Display SPI clock | 12 | `IO12_SPI_CLK` | `components/display/epd_pins.h` |
| Display SPI data (MOSI) | 11 | `IO11_SPI_MOSI` | |
| Display chip select | 14 | `IO14_CS` | |
| Display data/command | 13 | `IO13_D/C` | |
| Display reset | 10 | `IO10_RES` | |
| Display busy | 9 | `IO9_BUSY` | |
| Display power switch | 7 | `IO7_LCD_3.3_CTL` | High = display powered |
| Thumbwheel up | 6 | `IO6_UP` | `components/buttons/buttons.c` |
| Thumbwheel down | 4 | `IO4_DOWN` | |
| Thumbwheel press | 5 | `IO5_CONF` | |
| EXIT button | 1 | `IO1_EXIT` | |
| MENU button | 2 | `IO2_MENU` | |
| LED | 19 | `IO19_LED` | `components/board_led` (switched off; high = on) |
| GPIO_D port | 40, 41 | `IO40`, `IO41` | `components/battery` (I2C to the fuel gauge) |
| BOOT button | 0 | | Hold while resetting for download mode |

All five controls pull their pin to GND when pressed and have pull-up resistors on the board.

**Free for add-ons:** GPIO40 and GPIO41 on the 4-pin **GPIO_D** port (`P1`, small HY2.0
socket: GND, 3V3, GPIO41, GPIO40; used by the battery fuel gauge), plus the 4-pin UART0
port (`P2`, larger XH2.54 socket: GND, **5V**, TX, RX). Elsewhere the module also has GPIO15-18, 20, 21 and 45-48 unconnected, but they
aren't brought out to a header.

### Battery

The `BAT` connector feeds a 4054 linear charger (`U6`, charges from USB at about 330 mA,
set by `R12`) and a P-MOSFET switch (`U9`) that runs the board from the battery when USB
is unplugged. Neither the battery voltage nor the charger's status pin reaches the ESP32.

**Battery level add-on:** a **MAX17043 or MAX17048 fuel gauge** board on the
**GPIO_D** port gives the main menu a battery percentage and charging indicator, with no
soldering on the board:

| GPIO_D (P1) pin | Signal | Fuel gauge |
|---|---|---|
| 1 | GND | GND |
| 2 | 3V3 | VCC / VIN |
| 3 | GPIO41 | SDA or SCL (either way round) |
| 4 | GPIO40 | SCL or SDA |

The gauge also goes in line with the battery (battery → gauge → CrowPanel `BAT`). Details
in [components/battery/README.md](components/battery/README.md). Without a gauge the
firmware simply shows no battery level.

## Using it

| Control | Does |
|---|---|
| **MENU** | Home: back to the main menu, from anywhere |
| **EXIT** | Back: up one level (from an app's first screen, to the menu) |
| Thumbwheel **up / down** | Previous / next (moves the highlight, cursor, card...) |
| Hold the wheel **up / down** | App-specific, e.g. previous / next page in the story |
| **Press** the wheel | Select / open / act |

Downloads run in the background, so the controls keep working while something loads;
going home mid-download is instant and the download finishes in the background.

## Adding books

1. Open **Transfer** on the board; it shows an address like `http://192.168.1.174`.
2. Open that in a browser on the same Wi-Fi. The first time, click **Show code on the
   board** and type the 6-digit code from the board's screen.
3. Choose a `.txt`, `.epub` (2 or 3) or `.html` file. The page converts it to plain text the
   board can show, marks the chapters, shows a preview, and uploads it in pieces.
4. EXIT on the board stops the web page. Read the book in **Books**.

The same page lists and deletes books and shows the saved Chinese words. Details:
`components/webui/README.md` and `components/app_reader/README.md`.

## Building and flashing

Requires ESP-IDF v5.5 (installed at `C:\Users\p13ml\esp\v5.5.1\esp-idf` here). The
project is set to the `esp32s3` target; changing target (`idf.py set-target ...`)
regenerates `sdkconfig`, so note your Wi-Fi/API settings first.

1. Open an **ESP-IDF terminal** (VS Code: *ESP-IDF: Open ESP-IDF Terminal*). A plain
   terminal doesn't have `idf.py` on its PATH.
2. Configure: `idf.py menuconfig`, or VS Code *ESP-IDF: SDK Configuration Editor*, or edit
   `sdkconfig` directly. Settings live under two menus:
   - **Weather Display**: Wi-Fi SSID/password (2.4 GHz), latitude/longitude, units, refresh timings
   - **Chinese Story**: Claude API key, model, HSK level, the hour a new story starts
3. Build and flash: `idf.py -p COMx build flash` (the board's COM number: Device Manager →
   Ports, "USB Serial Device"). Watch the log with `idf.py -p COMx monitor` (close the
   monitor before flashing again, it holds the port). If the board won't connect, hold
   **BOOT**, tap **RESET**, release BOOT, then flash.

`sdkconfig` holds your Wi-Fi password and API key in plain text, so don't share it.
`sdkconfig.defaults` holds the non-secret defaults (target, flash size, PSRAM, partition
table).

### Flash layout (`partitions.csv`)

| Partition | Size | Holds |
|---|---|---|
| nvs | 24 KB | Settings: last app, time zone, HSK level, Wi-Fi calibration |
| phy_init | 4 KB | Radio calibration |
| factory | 4 MB | The firmware (about 1.8 MB, mostly the 700 KB Chinese font) |
| storage | 1 MB | SPIFFS files at `/store`: today's story, known words, saved words, flashcard progress |
| books | 2.9 MB | FAT at `/books`: e-books, their page indexes and chapter lists |

## How the code is organised

```
main/                     the launcher: start-up, main menu, event loop      (main/README.md)
components/
  app_core/               what apps are built on: app_t interface, worker,
                          settings, file storage, local time               (README inside)
  display/                e-paper driver (SSD1680 + JD79661), 16 px Unicode font,
                          loading box, wiring diagnostic
  buttons/                thumbwheel + EXIT/MENU -> tap / hold events
  board_led/              the board's LED (switched off)
  battery/                MAX17043/48 fuel gauge on GPIO_D (battery % on the main menu)
  books/                  e-book files (shared by the web interface and the reader)
  webui/                  web server + page: pairing, book upload/convert, saved words
  net/                    Wi-Fi connection
  review/                 saved review words (shared by the story and flashcard apps)
  app_weather/            Weather app
  app_clock/              Clock app
  app_story/              Story app
  app_flashcards/         Flashcards (Cards) app
  app_reader/             Books (e-reader) app
  app_transfer/           Transfer app (runs the web interface while open)
tools/make_font.py        builds the embedded font from GNU Unifont
```

Every folder has a README describing how it works and how to extend it. Layering, from
bottom to top: **drivers** (`display`, `buttons`, `net`) → **services** (`app_core`,
`review`) → **apps** (`app_*`) → **launcher** (`main`). Lower layers never include
headers from higher ones, and each component's `CMakeLists.txt` lists exactly what it
depends on, so the build enforces this.

```
                       main  (launcher, menu, event loop)
                                       |
       +---------------+---------------+---------------+
       |               |               |               |
  app_weather      app_clock       app_story     app_flashcards
       |               |               |   \           /   |
       |               |               |    +- review -+   |
       +---------------+---------------+-------+-----------+
                                       |
          app_core  (app_t, worker, settings, storage, local_time)
                                       |
                   display          buttons          net
```

### The three ideas that hold it together

1. **Apps implement `app_t`** (`components/app_core/include/app.h`): `open`, `button`,
   optional `tick`/`next_wakeup` for timed redraws, `background` for work that runs even
   when the app isn't showing. `main.c` never knows what's inside an app.
2. **Everything runs in the main task**, except slow network calls, which go to the
   **worker** task. The worker only downloads; its results come back to the main task
   (`worker_dispatch()`), so app state never needs locks.
3. **The screen is drawn into a frame buffer, then pushed** with either a full refresh
   (~2 s, flashes, clears ghosting) or a partial refresh (~0.3 s, no flash). Apps use
   partials for small changes and a full refresh when the whole screen changes.

### Adding an app

See [main/README.md](main/README.md#adding-an-app). In short: create
`components/app_<name>/` exporting an `app_t`, add it to `PRIV_REQUIRES` in
`main/CMakeLists.txt`, and add it to the `apps[]` list in `main/main.c`. The menu scrolls
when there are more than three apps.

## Troubleshooting

- **"Could not open COMx, the port is busy"**: close the serial monitor, or unplug/replug.
  If it still won't connect, hold BOOT, tap RESET, release BOOT (download mode).
- **Screen never changes**: the log says `full refresh: panel didn't respond` (panel not
  powered or not receiving commands) or `BUSY pin stuck`. Turn on *menuconfig →
  Hardware → Run the display wiring diagnostic* for a pin-by-pin report (see
  `components/display/README.md`). Buttons also stop responding while the firmware waits
  on a stuck display, because each wait times out after 10 s.
- **Everything is upside down**: turn on *menuconfig → Hardware → Rotate the display
  180 degrees*.
- **Partial refreshes look wrong or leave ghosts** on a JD79661 board (the log says
  `controller: JD79661` at start-up): see "JD79661" in `components/display/README.md`.
- **Wi-Fi won't connect**: the ESP32-S3 only does 2.4 GHz; a 5 GHz-only network won't work.
- **Story says "Set the API key in menuconfig"**: add a key under *Chinese Story*. API use
  is billed separately from Claude subscriptions (pay-as-you-go credit at platform.claude.com).
