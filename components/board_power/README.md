# board_power: the switched peripheral rail

GPIO38 switches a 3.3 V regulator (U6) that powers the e-paper, the LR1121 radio, the
microSD slot and the RGB LED (1 MOhm pull-down: off unless driven high). The display and the
SD card both need it, so it's reference-counted:

- `rail_acquire()`: the first user parks the radio (chip select and reset high, so it
  ignores the shared SPI bus), switches the rail on and waits 50 ms.
- `rail_release()`: the last user floats every pin leading to the rail's chips (radio, SD,
  LED) and lets the pull-down switch it off. Each user floats its own pins first (`epd.c`
  the display's, `sdcard.c` the card's).

`epd_init()` takes the display's reference; `epd_power_down()` gives it back. At power-off
`power_off()` unmounts the card and powers the display down, so the rail goes off.
