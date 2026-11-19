# board_led: the board's LED

The Elecrow CrowPanel ESP32 E-Paper 2.13" has an LED driven by **GPIO19** through a
resistor (`IO19_LED` in Elecrow's schematic; their examples call it the "POWER light"),
**on when the pin is HIGH**.

Nothing in the firmware uses it, so `board_led_off()` (called once by `main.c` at start-up)
drives it low. On battery, every milliamp counts.

## Using the LED for something

E.g. blink it while a download runs: `gpio_set_level(19, 1)` for on, `0` for off. Keep the
pin number in one place (`PIN_POWER_LED` in `board_led.c`) and add functions here rather
than driving GPIO19 from apps directly.

(This component has followed the hardware: on the ESP32-S3-DevKitC-1 it switched off a WS2812
RGB LED, on the XIAO ESP32-C5 a yellow user LED on GPIO27.)
