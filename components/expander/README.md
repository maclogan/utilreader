# expander: I2C I/O expander

Reads the 5-way switch and drives the TLV320DAC3100's reset line, so the header's GPIO3-5
are free for audio. Works with an **MCP23017** or a **PCF8574 / PCF8574A** on the board's
I2C bus (`components/i2c_bus`); `expander_init()` finds it and tells them apart.

| Pin (MCP23017 / PCF8574) | Use |
|---|---|
| GPA0 / P0 | 5-way left (input, pressed = low) |
| GPA1 / P1 | 5-way right |
| GPA2 / P2 | 5-way center |
| GPA3 / P3 | DAC reset (output, low = in reset) |
| the rest | spare inputs with pull-ups |

- **Finding it:** probes 0x20-0x27, then 0x38-0x3F (PCF8574A only). At 0x20-0x27 both chips
  answer, so it reads register 0x0A: an MCP23017 returns its IOCON register, 0x00 after
  power-up; a PCF8574 echoes the byte back as pin levels (0x0A).
- **MCP23017:** port A pins 0-2 and 4-7 are inputs with the 100k pull-ups on, pin 3 an
  output; port B is all inputs with pull-ups (nothing floats).
- **PCF8574:** has no direction register: a pin written 1 is an input with a weak pull-up,
  a pin written 0 pulls low. Every pin is kept at 1 except an output set low.
- `buttons.c` calls `expander_read()` every 10 ms; `components/audio` calls
  `expander_set_output(EXPANDER_DAC_RESET, ...)`.

Wiring, including the MCP23017's RESET and address pins: HARDWARE.md.
