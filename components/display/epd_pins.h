/* Display wiring on the Elecrow CrowPanel ESP32 E-Paper 2.13" (built in; from Elecrow's
 * schematic). Shared by the driver and the wiring diagnostic. */
#pragma once

#define PIN_MOSI 11   // IO11_SPI_MOSI
#define PIN_SCLK 12   // IO12_SPI_CLK
#define PIN_CS   14   // IO14_CS
#define PIN_DC   13   // IO13_D/C
#define PIN_RST  10   // IO10_RES
#define PIN_BUSY  9   // IO9_BUSY
#define PIN_PWR   7   // IO7_LCD_3.3_CTL: high switches the display's 3.3 V supply on
