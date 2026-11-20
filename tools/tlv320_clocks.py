"""TLV320DAC3100 clock solver, the same search the firmware runs (components/audio/tlv320.c).
Run: python tools/tlv320_clocks.py

BCLK mode: PLL_CLKIN = BCLK = 64 * fs (32-bit stereo slots), CODEC_CLKIN = PLL_CLK.
MCLK mode: CODEC_CLKIN = MCLK = 256 * fs directly (no PLL).
Constraints (datasheet, D = 0):
  512 kHz <= PLL_CLKIN / P <= 20 MHz ; 80 MHz <= PLL_CLK = PLL_CLKIN * R * J / P <= 110 MHz
  P 1-8, R 1-4, J 4-63 ; NDAC, MDAC 1-128 ; DAC_CLK = CODEC_CLKIN / NDAC <= 49.152 MHz
  DAC_MOD_CLK = DAC_CLK / MDAC <= 6.758 MHz, and 2.8-6.2 MHz recommended
  DOSR multiple of 8 (filter A), DAC_MOD_CLK = fs * DOSR exactly
  resource count: MDAC * DOSR / 32 >= 8 (processing block PRB_P1)
"""


def dividers(codec_clkin, fs):
    best = None
    for dosr in range(1016, 7, -8):                     # prefer high oversampling
        mod = fs * dosr
        if not (2.8e6 <= mod <= 6.2e6):
            continue
        if codec_clkin % mod:
            continue
        nm = codec_clkin // mod                         # NDAC * MDAC
        for mdac in range(1, 129):
            if nm % mdac:
                continue
            ndac = nm // mdac
            if not (1 <= ndac <= 128) or mdac * dosr < 256:
                continue
            if codec_clkin / ndac > 49.152e6 or codec_clkin / ndac / mdac > 6.758e6:
                continue
            return ndac, mdac, dosr
    return best


def solve_bclk(fs):
    bclk = 64 * fs
    for p in range(1, 9):
        if not (512e3 <= bclk / p <= 20e6):
            continue
        for r in range(1, 5):
            for j in range(4, 64):
                if (bclk * r * j) % p:
                    continue
                pll = bclk * r * j // p
                if not (80e6 <= pll <= 110e6):
                    continue
                d = dividers(pll, fs)
                if d:
                    return (p, r, j) + d + (pll,)
    return None


for fs in (8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000):
    s = solve_bclk(fs)
    m = dividers(256 * fs, fs)
    print(f"{fs:6d} Hz  BCLK mode: " + (f"P={s[0]} R={s[1]} J={s[2]} NDAC={s[3]} MDAC={s[4]} DOSR={s[5]} PLL={s[6]/1e6:.3f} MHz" if s else "no solution") +
          f"   MCLK mode: " + (f"NDAC={m[0]} MDAC={m[1]} DOSR={m[2]}" if m else "no solution"))
