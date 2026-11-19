# display: e-paper driver, fonts, loading box

| File | What it does |
|---|---|
| `epd_2in13.c/.h` | Driver for the 2.13" 250×122 panel (SSD1680 or JD79661): power, frame buffer, drawing, refresh |
| `jd79661_lut.h` | Waveform tables for the JD79661, generated from Elecrow's example driver |
| `epd_pins.h`, `epd_diag.c` | Pin numbers; the optional wiring diagnostic |
| `font16.c/.h` | 16 px Unicode font (Chinese, pinyin, Latin, punctuation) from GNU Unifont |
| `unifont16.bin` | The font bitmaps, embedded in the firmware (built by `tools/make_font.py`) |
| `ui_loading.c/.h` | The small "Loading..." box apps show before a slow full refresh |

## How drawing works

All drawing goes into a **frame buffer** in RAM (250×122, 1 bit per pixel). Nothing
appears until you push the buffer to the panel:

```c
epd_init();                         // wake + reset the panel (only before a full refresh)
epd_clear(EPD_WHITE);               // start from a blank buffer
epd_draw_text(10, 10, "Hi", 2, EPD_BLACK);
epd_display();                      // full refresh
```

Coordinates are **landscape**: x = 0..249 left to right, y = 0..121 top to bottom.
The panel is physically portrait (122×250); `epd_draw_pixel()` rotates for you.

### Full vs partial refresh

| | `epd_display()` (full) | `epd_display_partial()` |
|---|---|---|
| Time | ~2 s | ~0.3-0.5 s |
| Looks | Flashes black/white | Only changed pixels change |
| Ghosting | Clears it | Builds up a little each time |
| Needs | `epd_init()` first | A previous `epd_display()` and an awake panel |

The usual pattern, used by every app:

- **Whole screen changes** (opening an app, a new screen): `epd_init()` + full refresh.
- **Small change** (cursor moved, a number updated): redraw the whole buffer the same way,
  then `epd_display_partial()`. The controller compares with the previous image and only
  drives the differences.
- **Now and then**, do a full refresh anyway to clear ghosting (the clock does on the hour,
  the story reader every 8 page turns, the forecast every hour).

`epd_display()` writes the image to both of the controller's RAMs, so the next partial has
an accurate "previous" picture. A partial refresh also updates the "previous" RAM afterwards.

### Sleep

`epd_sleep()` puts the controller into deep sleep, and only a pulse on **RST** can wake it.
The firmware doesn't use it: the panel just idles awake between refreshes (it draws almost
nothing when not refreshing, and the image stays without power anyway). If you add sleep
for battery use, make sure RST is wired solidly.

### BUSY and timeouts

During a refresh the controller signals busy on BUSY (high on the SSD1680, low on the
JD79661); `epd_wait_busy()` polls it every 10 ms.
If it stays busy for 10 s it logs `BUSY pin stuck, check wiring` and carries on.
On this board that points to the display power switch (GPIO7) or a damaged panel cable.

After every full refresh `epd_display()` also logs how long the panel was busy. Under 100 ms
means the panel never started (`full refresh: panel didn't respond`): it isn't powered,
isn't receiving commands, or BUSY isn't connected.

## Drawing functions (`epd_2in13.h`)

| Function | Notes |
|---|---|
| `epd_clear(color)` | Fill the buffer |
| `epd_draw_pixel(x, y, color)` | Clipped to the screen |
| `epd_draw_hline(x, y, w, color)` | |
| `epd_draw_line(x0, y0, x1, y1, color)` | Bresenham |
| `epd_draw_rect(x, y, w, h, color, fill)` | Outline or filled |
| `epd_fill_circle(cx, cy, r, color)` | |
| `epd_draw_text(x, y, text, scale, color)` | 5×7 ASCII font, 6 px per character × scale. Returns the x after the text |
| `epd_text_width(text, scale)` | For centering |

`EPD_BLACK` is 0 and `EPD_WHITE` is 1, so `!color` gives the opposite color (handy for
inverted highlights: fill a black box, draw white text).

## font16: Chinese, pinyin and Unicode text

```c
int x = font16_draw_text(4, 20, "今天 jīntiān", EPD_BLACK);   // UTF-8 in, returns end x
int w = font16_text_width("你好");                          // 32
```

- Every glyph is **16 px tall**; Chinese and fullwidth punctuation are 16 px wide, Latin
  letters (including pinyin tone marks ā á ǎ à ü ǖ) are 8 px wide.
- Characters the font doesn't have are drawn as a hollow box.
- `font16_draw_char(x, y, codepoint, scale, color)` draws one character, optionally
  scaled up (the story app's 文 icon is scale 2). `font16_next_char(&p)` steps through a
  UTF-8 string one code point at a time, for custom layout (see `app_story/story_ui.c`).

### Included characters

| Range | What |
|---|---|
| U+0020-007E | ASCII |
| U+00A0-024F | Latin-1 and Latin Extended A/B (pinyin vowels) |
| U+2010-2027 | Dashes, curly quotes, ellipsis |
| U+3000-303F | CJK punctuation 。、《》「」 |
| U+4E00-9FFF | CJK Unified Ideographs (all common Chinese characters) |
| U+FF00-FFEF | Fullwidth forms ，！？：（） |

### Adding characters (e.g. Japanese kana, symbols)

1. Download GNU Unifont's `.hex` file (unifoundry.com, `unifont-<version>.hex.gz`) and unzip it.
2. Add the range to `RANGES` in `tools/make_font.py`.
3. `python tools/make_font.py unifont.hex components/display/unifont16.bin`
4. Rebuild. Each 16 px glyph costs 32 bytes of flash; the app partition has ~2 MB free.

The `.bin` format (header, ranges, widths, bitmaps) is described at the top of
`tools/make_font.py`; `font16.c` reads it directly from flash, with no RAM copy.

Unifont is free (GPL 2+ with a font embedding exception / SIL OFL 1.1).

## ui_loading

`ui_show_loading()` draws a small "Loading..." box with an hourglass over whatever is on
screen and partial-refreshes it (~0.3 s). Call it right after a button press that leads to
a slow full refresh, so the press gets instant feedback. It does nothing if the panel is
asleep or hasn't had a full refresh yet.

## Pins

`epd_pins.h` (shared by the driver and the diagnostic), on the CrowPanel 2.13":

| Display | GPIO |
|---|---|
| MOSI (DIN) | 11 |
| CLK | 12 |
| CS | 14 |
| DC | 13 |
| RST | 10 |
| BUSY | 9 |
| Power switch | 7 (high = on) |

SPI2 at 4 MHz, write-only. The display's 3.3 V supply is switched by GPIO7; `epd_init()`
turns it on the first time it runs.

## Two controllers: SSD1680 and JD79661

Elecrow has shipped this board with either of two controller chips. They do the same job
with different command sets, so the driver has a code path for each and picks one at the
first `epd_init()`:

| | SSD1680 (earlier boards) | JD79661 (newer boards) |
|---|---|---|
| BUSY | **high** while busy, low when idle | **low** while busy, high when idle |
| Pixel bit | 1 = white | 1 = white |
| Row order | As stored | Reversed: the driver sends the last row first |
| Image RAM | Driver writes new (0x24) and previous (0x26) | Driver writes only new (0x13); the chip keeps the previous one itself |
| Waveform | Built into the chip | Uploaded by the driver before every refresh (`jd79661_lut.h`) |
| Refresh | 0x22 + 0x20 | 0x17 0xA5 |
| Partial refresh | Built-in mode (0x22 0xFF) | "DU" waveform (only changed pixels are driven) |
| Reset | On every `epd_init()` | Only the first time (and after `epd_sleep()`) |

**Detection:** after a hardware reset both chips sit idle, and the idle level of BUSY
differs (SSD1680 low, JD79661 high). `detect_controller()` reads it once and the result is
logged: `epd: controller: SSD1680` or `JD79661`.

**The JD79661 sequence** follows Elecrow's v1.2 example, with init values and waveform
tables copied from it, and the order of operations matters:

1. Once: reset, configure, fill both image buffers white (0x10, 0x13), upload the full
   ("GC") waveform, refresh.
2. Every update: write only the new image to 0x13, upload one waveform set (GC for full
   refreshes, DU for partial ones), refresh with 0x17 0xA5.

Every waveform upload swaps the contents of registers 0x22 and 0x23 (the white→black and
black→white tables), as Elecrow's code does. The chip apparently alternates which of its
two buffers counts as "old", and the swap keeps the two in step. So: **exactly one
upload per refresh, never write 0x10 after the first clear, and don't reset the chip
between screens.** Breaking any of these gives inverted colours, an all-white screen, or
partial updates that never erase.

Found on a real board (2026-10-30), and different from what Elecrow's drawing code
suggests: the data is 1 = white, and rows must go out last to first (otherwise the
picture is mirrored left to right). If partial updates misbehave, a quick workaround is to
make `jd_display_partial()` call `jd_display()` (every update becomes a full, flashing
refresh).

## Orientation

Pixels are stored as row = 249 - x, column = y, which is the orientation Elecrow's
examples use, so text reads correctly with the board as it sits in its case. If yours is
upside down, turn on *menuconfig → Hardware → Rotate the display 180 degrees*.

## Wiring diagnostic (`epd_diag.c`)

Turn on *menuconfig → Hardware → Run the display wiring diagnostic at every boot* (or call
`epd_diagnose()` before `epd_init()`). After a 4 s pause (time to open the serial log), it
drives the display pins as plain GPIOs and logs, under the tag `epd_diag`:

0. Switches the display's power on (GPIO7).
1. **Pin survey:** each pin with the pull-up, then the pull-down. `floating` is normal for
   the ESP-to-display lines; **BUSY should be held** (low on an SSD1680, high on a
   JD79661), because a powered panel drives it. BUSY `floating` means the display has no
   power or BUSY is disconnected.
2. **Shorts:** each pin driven high and low; any other pin that follows is reported.
   Caution: an *unpowered* panel can also show up here, because driving DIN high leaks
   power into it through its input protection diodes, which can drag other lines along.
3. **Reset and commands:** pulses RST, reports BUSY's idle level (which also hints at the
   controller), then bit-bangs SSD1680 "software reset" and "refresh". A working panel is
   busy ~1-3 s in total (`<-- panel responded`); a JD79661 reacts to the first command,
   which it reads as "refresh".
   It also tries DIN/CLK swapped and CS/DC swapped, the common wiring mix-ups.

Turn the option off again afterwards: it adds ~20 s to every start-up.
