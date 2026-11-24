/*
 * TLV320DAC3100 over I2C (address 0x18), headphone output only. Internal to the audio
 * component. Its PLL runs from the I2S bit clock (BCLK = 64 x sample rate: 32-bit stereo
 * slots), so BCLK must already be running when tlv320_start() is called.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t p, r, j;                    // PLL: PLL_CLK = BCLK * R * J / P (D = 0)
    uint8_t ndac, mdac;                 // DAC_CLK = PLL_CLK / NDAC, DAC_MOD_CLK = DAC_CLK / MDAC
    uint16_t dosr;                      // sample rate = DAC_MOD_CLK / DOSR
} tlv320_clocks_t;

// Finds PLL and divider settings for `rate` within the datasheet's limits. False if none
// (below about 8 kHz or above 96 kHz). Pure arithmetic; tools/tlv320_clocks.py does the same.
bool tlv320_solve(uint32_t rate, tlv320_clocks_t *out);

// Releases the reset line, checks the chip answers, and sets everything up for `rate`:
// clocks, I2S 32-bit, DAC on, headphone driver on (muted until tlv320_mute(false))
bool tlv320_start(uint32_t rate);

// Changes the sample rate while running (DAC muted and powered down during the change)
bool tlv320_set_rate(uint32_t rate);

// Soft mute (the DAC ramps the volume, no click)
void tlv320_mute(bool mute);

// Digital volume, -63.5 to 0 dB (0.5 dB steps)
void tlv320_set_volume_db(float db);

// Powers everything down (headphone driver first, then DAC and PLL). The chip keeps its
// I2C interface and draws microamps. Headset detection keeps working.
void tlv320_stop(void);

// Headset detection on the jack's fourth contact (MICDET), also while nothing plays. False if
// the chip doesn't answer. tlv320_start() sets it up again after its reset.
bool tlv320_headset_init(void);

// The headset state now (in, button held) and what happened since the last call (a button
// press, a plug in or out: the chip's sticky flags, cleared by reading). False on an I2C error.
bool tlv320_headset_poll(bool *in, bool *pressed_now, bool *button_event, bool *plug_event);
