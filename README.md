# E-paper app board

Apps on a [Waveshare ESP32-S3-LR1121-ePaper-2.9](https://www.waveshare.com/esp32-s3-lr1121-epaper-2.9.htm)
([wiki](https://docs.waveshare.com/ESP32-S3-LR1121-ePaper-2.9)): an ESP32-S3 with a built-in
296×128 black/white e-paper screen, a LiPo charger and fuel gauge. The controls (a 5-way
switch, a BACK / HOME button and a lock switch) are wired to its 14-pin header.

| App | What it does |
|---|---|
| **Weather** | 7-day forecast from [Open-Meteo](https://open-meteo.com) (free, no key), with a details screen per day; also sets the time zone (a Lua app) |
| **Clock** | Local time in big digits, redrawn every minute (a Lua app: the example for writing your own) |
| **Story** | A short Chinese graded-reader story written by Claude each day, with per-word pinyin and meaning lookup and a saved-words list |
| **Cards** | Spaced-repetition flashcards of the words saved in the story |
| **Books** | E-book reader: chapters, reading position and progress kept per book |
| **Transfer** | While open, a web page on your Wi-Fi to add/delete books and see saved words |
| **Settings** | Set the date and time by hand; turn the screen upside down |
| **Power** | Turns the device off (hold BACK / HOME to turn it on again) |

**Your own apps.** Apps can also be small Lua programs installed from the web page
(**Transfer** → **Apps**), with no rebuilding: see [docs/api.md](docs/api.md). Clock is one,
built in; it can be removed and reinstalled like any other.

At power-on the board returns to where it was: the last app, or the main menu if that's
where you left it (Transfer and Power are never reopened automatically).

## Hardware

- **Board:** ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM, 2.4 GHz Wi-Fi), 2.9"
  296×128 e-paper, LiPo charger and AXP2602 fuel gauge. Flashed over its USB-C port.
- **You add:** a 5-way tact switch (left/right/center), a push button (BACK / HOME) and a
  lock switch on the 14-pin header, and optionally a 3.7 V LiPo. Every control just
  connects a pin to GND.

| Control | Header pin | GPIO |
|---|---|---|
| 5-way left / right / center | GP4 / GP5 / GP3 | 4 / 5 / 3 |
| BACK / HOME (also turns on) | GP2 | 2 |
| Lock (closed = locked) | RX | 44 |

**[HARDWARE.md](HARDWARE.md)** has the parts list, header pinout, wiring, power and
charging details, and the full GPIO map.

## Using it

| Control | Does |
|---|---|
| **Hold BACK / HOME** | Home: back to the main menu, from anywhere |
| **Tap BACK / HOME** | Back: up one level (from an app's first screen, to the menu) |
| 5-way **left / right** | Previous / next (moves the highlight, cursor, card...) |
| Hold **left / right** | App-specific, e.g. previous / next page in the story |
| **Press** the 5-way | Select / open / act |
| **Lock** switch on | All controls ignored (a padlock shows on the menu) |

**Off and on.** Open the **Power** tile to turn off: the screen shows a power symbol and
"Hold BACK to turn on". To turn on, hold BACK / HOME for about 1.5 s until "Starting..."
appears. A shorter press does nothing, and with the lock switch on the device stays off.
"Off" is the chip's deep sleep with the display, radio, SD card and LED switched off (the
board has no real power switch): Waveshare measures about 36 µW, so a battery lasts years.
RESET also starts it.

**Apps and the menu.** On the board's web page (**Transfer** app):
- **Apps** reorders the menu and hides apps (they keep working in the background), and
  has each Lua app's **Settings** (Weather's location and units, for example) and **Log**.
- **Install an app** takes a `.zip` or a folder.
- **App catalog** lists shared apps, installs them and offers updates
  ([publishing yours](docs/api.md#sharing-your-app)).

**Screens.** Titles stand in a narrow bar on the left edge, reading bottom to top, so
the screen keeps its full height for content. On the main menu that bar shows the time,
a padlock while locked, and the battery.

**Time.** The clock is set from the internet (checked hourly while online) and copied to
the board's clock chip, so it's right straight after a reset or a long time off, even
without Wi-Fi. The time zone comes with each weather forecast. Without internet, set the
date and time in **Settings**.

Downloads run in the background, so the controls keep working while something loads;
going home mid-download is instant and the download finishes in the background.

## Adding books

1. Open **Transfer** on the board; it shows an address like `http://192.168.1.174`.
2. Open that in a browser on the same Wi-Fi. The first time, click **Show code on the
   board** and type the 6-digit code from the board's screen.
3. Choose a `.txt`, `.epub` (2 or 3) or `.html` file. The page converts it to plain text the
   board can show, marks the chapters, shows a preview, and uploads it in pieces.
4. BACK on the board stops the web page. Read the book in **Books**.

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
   - **Wi-Fi**: network name and password (2.4 GHz)
   - **Chinese Story**: Claude API key, model, HSK level, the hour a new story starts
3. Build and flash: `idf.py -p COMx build flash` (the board's COM number: Device Manager →
   Ports, "USB Serial Device"). Watch the log with `idf.py -p COMx monitor` (close the
   monitor before flashing again, it holds the port). If the board won't connect, hold
   **BOOT**, tap **RESET**, release BOOT, then flash. While the device is off (deep sleep)
   the USB port disappears: turn it on or press RESET first.

`sdkconfig` holds your Wi-Fi password and API key in plain text, so don't share it.
`sdkconfig.defaults` holds the non-secret defaults (target, flash size, PSRAM, partition
table).

### Flash layout (`partitions.csv`)

| Partition | Size | Holds |
|---|---|---|
| nvs | 24 KB | Settings: last app, time zone, screen rotation, HSK level, Wi-Fi calibration |
| phy_init | 4 KB | Radio calibration |
| factory | 4 MB | The firmware (about 1.8 MB, mostly the 700 KB Chinese font) |
| storage | 1 MB | SPIFFS files at `/store`: today's story, known words, saved words, flashcard progress |
| books | 8.9 MB | FAT at `/books`: e-books, their page indexes and chapter lists |
| apps | 2 MB | FAT at `/apps`: installed Lua apps, their settings and data |

## How the code is organised

```
HARDWARE.md               parts, wiring, pinout, power
docs/api.md               writing Lua apps: quick start, then the reference
apps/                     Lua apps: the catalog, and the ones bundled with the firmware
                          (Weather, Clock)
.github/workflows/        catalog.yml: checks apps/ and publishes the catalog to GitHub Pages
tools/catalog.py          checks app packages; builds the catalog
main/                     the launcher: start-up, main menu, event loop      (main/README.md)
components/
  app_core/               what apps are built on: app_t interface, app registry,
                          worker, settings, file storage, local time       (README inside)
  app_script/             Lua apps: host, API, installing, bundled apps
  lua/                    Lua 5.4.7 interpreter (unmodified)
  display/                e-paper driver (SSD16xx), 16 px Unicode font, loading box,
                          left title bar, wiring diagnostic; owns the peripheral rail
  buttons/                5-way switch + BACK/HOME + lock switch -> tap / hold events
  battery/                AXP2602 fuel gauge + charge status (battery % on the main menu)
  clock_chip/             PCF85063A real-time clock: keeps the time across resets
  i2c_bus/                the board's I2C bus, shared by the gauge and the clock chip
  power/                  off (deep sleep, the Power tile) and on (hold BACK/HOME)
  books/                  e-book files (shared by the web interface and the reader)
  webui/                  web server + page: pairing, book upload/convert, saved words
  net/                    Wi-Fi connection
  review/                 saved review words (shared by the story and flashcard apps)
  app_story/              Story app
  app_flashcards/         Flashcards (Cards) app
  app_reader/             Books (e-reader) app
  app_transfer/           Transfer app (runs the web interface while open)
  app_settings/           Settings app (date, time, screen rotation)
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
  app_reader       app_script      app_story     app_flashcards
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
`main/CMakeLists.txt`, and add it in `register_apps()` in `main/main.c`. Or write it in
Lua and install it from the web page: [docs/api.md](docs/api.md). The menu scrolls
when there are more than four apps.

## Troubleshooting

- **"Could not open COMx, the port is busy"**: close the serial monitor, or unplug/replug.
  If it still won't connect, hold BOOT, tap RESET, release BOOT (download mode).
- **Screen never changes**: the log says `full refresh: panel didn't respond` (panel not
  powered or not receiving commands) or `BUSY pin stuck`: check the panel's ribbon cable
  in its FPC connector. *menuconfig → Hardware → Run the display wiring diagnostic* gives a
  pin-by-pin report (see `components/display/README.md`). Buttons also stop responding while the firmware waits
  on a stuck display, because each wait times out after 10 s.
- **Everything is upside down**: *Settings → Rotate*. (*menuconfig → Hardware → Rotate
  the display 180 degrees* sets the default for a board that has never saved the setting.)
- **Clock wrong or blank after a reset**: the clock chip lost its time (the battery ran
  flat or was unplugged, log: `oscillator stopped`). It's set again at the next internet
  sync, or set it in *Settings*.
- **Controls do nothing**: check the lock switch (the menu shows a padlock while locked;
  the log says `controls locked`). The board's KEY button works as BACK / HOME without any
  wiring.
- **Won't turn on**: hold BACK / HOME for a full 1.5 s, with the lock switch off. If the
  battery ran flat, plug in USB and press RESET.
- **Wi-Fi won't connect**: the ESP32-S3 only does 2.4 GHz; a 5 GHz-only network won't work.
- **Story says "Set the API key in menuconfig"**: add a key under *Chinese Story*. API use
  is billed separately from Claude subscriptions (pay-as-you-go credit at platform.claude.com).
