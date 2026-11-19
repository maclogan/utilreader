#include "board_led.h"

#include "driver/gpio.h"

#define PIN_POWER_LED 19                // IO19_LED on the CrowPanel 2.13": on when the pin is HIGH
#define LED_OFF       0

void board_led_off(void)
{
    // Set the level before switching to output, so the LED never flashes on
    gpio_set_level(PIN_POWER_LED, LED_OFF);
    gpio_set_direction(PIN_POWER_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_POWER_LED, LED_OFF);
}
