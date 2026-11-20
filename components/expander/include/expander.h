/*
 * I2C I/O expander on the header's SDA/SCL (components/i2c_bus): a PCF8574 / PCF8574A or an
 * MCP23017, found and told apart automatically. It reads the 5-way switch and drives the
 * TLV320DAC3100's reset line. See README.md.
 *
 *   PCF8574 pin  MCP23017 pin
 *   P0           GPA0          5-way left    (input: pressed = low)
 *   P1           GPA1          5-way right
 *   P2           GPA2          5-way center
 *   P3           GPA3          DAC reset     (output: low = held in reset)
 *   P4-P7        GPA4-GPA7     spare (inputs with pull-ups)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EXPANDER_LEFT       0
#define EXPANDER_RIGHT      1
#define EXPANDER_CENTER     2
#define EXPANDER_DAC_RESET  3

// Finds the expander (0x20-0x27: PCF8574 or MCP23017; 0x38-0x3F: PCF8574A), sets the inputs'
// pull-ups and the outputs high. Returns false if there's none. Safe to call again.
bool expander_init(void);

bool expander_present(void);

// Reads the 8 pins (bit n = pin n; inputs read 0 while pressed). False on an I2C error.
bool expander_read(uint8_t *pins);

// Drives an output pin (only EXPANDER_DAC_RESET is an output): high = released, low = held low
void expander_set_output(int bit, bool high);
