# power: turning the device off and on

The board has no power switch: whenever a battery is connected, it feeds the ESP32-S3's
regulator directly. So "off" is **deep sleep** with everything else switched off:

| Part | While off |
|---|---|
| ESP32-S3 | Deep sleep; only the wake-up logic and its pull-up on GPIO2 run |
| Display, LoRa radio, microSD, RGB LED | Their shared rail (GPIO38) switched off, all their pins floated |
| AXP2602 fuel gauge | Its sleep mode |
| E-paper | Keeps showing the "off" screen with no power |

Waveshare measures about 36 µW in this state, so a battery lasts years.

## Off

The **Power** tile in the main menu (`power_app`) calls `power_off()`, which:

1. Waits up to 15 s for background jobs to finish and save their results.
2. Draws the "off" screen (power symbol, "Hold BACK to turn on") with a full refresh.
3. Puts the panel to sleep and switches the peripheral rail off (`epd_power_down()`).
4. Stops Wi-Fi and puts the fuel gauge to sleep.
5. Waits until no power button is held, arms GPIO2 (header BACK / HOME) and GPIO1 (board
   KEY) to wake the chip when pressed, and enters deep sleep.

`power_app` is marked `no_resume`, so the next start shows the menu, not the Power tile.

## On

Any press of a power button wakes the chip, which boots from scratch. `power_boot_check()`
runs first in `app_main`:

- Woken by something other than a button (RESET, a battery just plugged in): carry on.
- Lock switch on, or the button let go before `POWER_ON_HOLD_MS` (1.5 s, counted from the
  start of boot): straight back to sleep. The screen is never touched, so it still says off.
- Held long enough: shows "Starting..." with a quick partial refresh (the panel forgot the
  "off" image when its power was cut, so it's redrawn and loaded into the panel first),
  then start-up continues as normal.

## Notes

- The USB serial port disappears while the device is off. Turn it on or press RESET
  before flashing.
- An empty battery isn't detected: the board runs until the cell's own protection cuts
  out. Plug in USB and press RESET to start again.
