/* Display wiring on the Waveshare ESP32-S3-LR1121-ePaper-2.9 (built in; from Waveshare's Rev1.1
 * schematic). Shared by the driver and the wiring diagnostic.
 *
 * The display shares its SPI bus with the LR1121 LoRa radio (which the firmware doesn't use),
 * and one switched 3.3 V rail powers the display, the radio, the microSD slot and the RGB LED. */
#pragma once

#define PIN_MOSI 45   // shared SPI MOSI (EPD_DIN / LoRa_MOSI)
#define PIN_SCLK 40   // shared SPI clock (EPD_SCK / LoRa_SCK)
#define PIN_CS   18   // EPD_CS
#define PIN_DC   17   // EPD_DC
#define PIN_RST   9   // EPD_RST
#define PIN_BUSY  8   // EPD_BUSY: high while the controller works
#define PIN_PWR  38   // EPD_PWR_IO: high switches the shared peripheral rail on (1 MOhm pull-down)

/* LR1121 radio on the same bus: its chip select is held high so it ignores display traffic */
#define PIN_LORA_NSS   42
#define PIN_LORA_RST   39
#define PIN_LORA_BUSY  41
#define PIN_LORA_DIO1   6
#define PIN_SPI_MISO   46   // only the radio drives it

/* Other pins on the switched rail, left floating while it's off */
#define PIN_SD_CMD     10
#define PIN_SD_D0      11
#define PIN_SD_CLK     12
#define PIN_RGB_LED    21
