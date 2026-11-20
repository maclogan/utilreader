# audio: headphone output (TLV320DAC3100 over I2S)

| File | What it does |
|---|---|
| `audio.c`, `include/audio.h` | The API, the PSRAM ring buffer, the output task, I2S |
| `tlv320.c/.h` | The DAC over I2C: reset, clocks (PLL from BCLK), headphone path, volume, power down |

## API

`audio_start(rate)` -> `audio_write(pcm, frames, bits, channels)` (any number of times) ->
`audio_drain()` / `audio_flush()` -> `audio_stop()`. Plus `audio_pause()`,
`audio_set_volume()` (0-30, 2 dB steps, remembered) and `audio_frames_played()` for the
elapsed time. One producer task at a time.

## How it works

- **I2S:** controller 0, master, standard (Philips) mode, always 32-bit stereo slots, so
  BCLK = 64 x sample rate. Pins: BCLK GPIO4, WS GPIO5, data out GPIO43 (header TX), no MCLK.
  `auto_clear` makes an underrun play silence. GPIO3 is reserved for the microphone's data
  (not set up yet; a full-duplex channel would add it as `din`).
- **Ring buffer:** a FreeRTOS stream buffer with its 384 KB in PSRAM (about 1.1 s at
  44.1 kHz). `audio_write()` converts 16/24/32-bit, mono or stereo input to 32-bit stereo
  and blocks while it's full. The `audio_out` task (priority 12, above everything else)
  moves it to I2S.
- **DAC start-up** (`tlv320_start`, datasheet order): BCLK running first; reset pulse on
  the expander's pin 3, then a soft reset; PLL programmed from BCLK, powered, 10 ms to lock;
  CODEC_CLKIN = PLL; NDAC / MDAC / DOSR; I2S 32-bit; DAC muted; headphone path DAC ->
  mixer -> HP volume 0 dB -> driver 0 dB, common mode 1.65 V, pop-free ramp (153 ms power-on,
  3.9 ms steps); DACs on; unmute at the chosen volume.
- **Clocks:** `tlv320_solve()` searches P, R, J (D = 0), NDAC, MDAC and DOSR for an exact
  match within the datasheet limits (PLL 80-110 MHz, DAC_MOD_CLK 2.8-6.2 MHz, DOSR a multiple
  of 8, MDAC x DOSR >= 256), preferring the highest oversampling. `tools/tlv320_clocks.py`
  runs the same search and prints the table; 8-96 kHz all have solutions, e.g. 44.1 kHz:
  P=1 R=1 J=30 NDAC=5 MDAC=3 DOSR=128; 48 kHz: J=27 NDAC=6 MDAC=3 DOSR=96.
- **Rate change:** buffered audio plays out, then mute, I2S re-clocked, DAC PLL
  re-programmed, unmute.
- **Stop:** soft mute, headphone driver off (ramps), DAC and PLL off; I2S deleted and its pins
  floated. The DAC's reset line is left high (released): holding it low would draw current
  through a pull-up while the device is off.

## Notes

- The ESP32-S3 makes BCLK with a fractional divider, so it has some jitter; the DAC's PLL
  filters most of it. If it ever sounds rough, a cleaner clock needs MCLK, which needs a
  free GPIO (none on the header).
- The DAC is shared with nothing else; every I2C access goes through one mutex because the
  chip's registers are paged.
