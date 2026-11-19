# display: e-paper driver, fonts, loading box

| File | What it does |
|---|---|
| `epd.c/.h` | Driver for the 2.9" 296×128 panel (SSD16xx): power, frame buffer, drawing, refresh |
| `epd_pins.h`, `epd_diag.c` | Pin numbers; the optional wiring diagnostic |
| `font16.c/.h` | 16 px Unicode font (Chinese, pinyin, Latin, punctuation) from GNU Unifont |
| `unifont16.bin` | The font bitmaps, embedded in the firmware (built by `tools/make_font.py`) |
| `ui_loading.c/.h` | The small "Loading..." box apps show before a slow full refresh |
| `ui_sidebar.c/.h` | The title bar on the left edge that apps draw their title into |

## How drawing works

All drawing goes into a **frame buffer** in RAM (296×128, 1 bit per pixel). Nothing
appears until you push the buffer to the panel:

```c
epd_init();                         // wake + reset the panel (only before a full refresh)
epd_clear(EPD_WHITE);               // start from a blank buffer
epd_draw_text(10, 10, "Hi", 2, EPD_BLACK);
epd_display();                      // full refresh
```

Coordinates are **landscape**: x = 0..295 left to right, y = 0..127 top to bottom.
The panel is physically portrait (128×296); `epd_draw_pixel()` rotates for you.

### Full vs partial refresh

| | `epd_display()` (full) | `epd_display_partial()` |
|---|---|---|
| Time | ~3 s | ~0.6 s |
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

`epd_display_base()` writes the buffer to both RAMs without refreshing: it tells the
panel what is already on screen. `components/power` uses it after the panel lost power:
it redraws the known "off" screen, loads it as the base, then changes one line with a
partial refresh.

### Sleep and power

`epd_sleep()` puts the controller into deep sleep, and only a pulse on **RST** can wake it.
While the device is on, the panel just idles awake between refreshes (it draws almost
nothing when not refreshing, and the image stays without power anyway).

One switched 3.3 V rail (GPIO38, high = on) powers the display, the LoRa radio, the
microSD slot and the RGB LED. `epd_init()` switches it on the first time it runs, and also
holds the radio's chip select high: the radio shares the display's SPI bus, and would
otherwise take display traffic as its own. `epd_power_down()`, used before turning the
device off, puts the panel to sleep, floats every pin leading to the rail (an output left
high would power the chips through their protection diodes) and lets the rail's pull-down
switch it off. The next `epd_init()` sets it all up again.

### BUSY and timeouts

During a refresh the controller holds BUSY high; `epd_wait_busy()` polls it every 10 ms.
If it stays busy for 10 s it logs `BUSY pin stuck, check the panel's ribbon cable` and
carries on. On this board that points to the rail (GPIO38) or the panel's FPC cable.

After every full refresh `epd_display()` also logs how long the panel was busy. Under 100 ms
means the panel never started (`full refresh: panel didn't respond`): it isn't powered,
isn't receiving commands, or BUSY isn't connected.

## Drawing functions (`epd.h`)

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

`epd_pins.h` (shared by the driver and the diagnostic), on the Waveshare
ESP32-S3-LR1121-ePaper-2.9:

| Display | GPIO |
|---|---|
| MOSI (DIN) | 45 (shared with the radio) |
| CLK | 40 (shared with the radio) |
| CS | 18 |
| DC | 17 |
| RST | 9 |
| BUSY | 8 |
| Peripheral rail | 38 (high = on) |
| LoRa radio NSS / RST | 42 / 39 (held high) |

SPI2 at 4 MHz, write-only.

## Controller

The panel is Waveshare's 2.9" V2 module with an SSD16xx-family controller (the same
command set as the SSD1680), 128 sources × 296 gates. The driver uses the chip's built-in
waveforms: full refresh with 0x22 0xF7, partial with 0x22 0xFF after writing the new image
to RAM 0x24 while RAM 0x26 holds the previous one. Waveshare's own driver has a third,
"fast" full refresh (temperature register forced to 100 °C, 0x22 0xC7) that this driver
doesn't use. The panel can also show 4 grey levels, which this driver doesn't use either.

## Orientation

Pixels are stored as row = 295 - x, column = y, which is the orientation Waveshare's
examples use. `epd_set_rotated(true)` turns the picture upside down for everything drawn
afterwards; the Settings app changes it and `main.c` applies the saved choice at start-up.
*menuconfig → Hardware → Rotate the display 180 degrees* is only the default.

## Vertical drawing and the title bar

`epd_vertical_begin(x, y)` turns every drawing call a quarter turn counter-clockwise until
`epd_vertical_end()`: drawing (0, 0) lands on screen (x, y), drawing x runs up the screen
and drawing y to the right. Anything (5×7 text, the 16 px font, rectangles) then comes out
reading bottom to top, as on a book spine.

`ui_sidebar(title, counter)` uses it for the title bar: an 18 px strip on the left edge
with the title (UTF-8, cut off if too long) from the bottom, a small counter such as
"3/12" at the top, and a rule. Content starts at `UI_CONTENT_X`; `UI_CONTENT_W` and
`UI_CONTENT_CX` help with centering. The Story, Cards, Books and Settings screens use it,
and the main menu draws its status bar with `ui_sidebar_begin()` / `ui_sidebar_rule()`.
Titles get about 120 px: 7 Chinese characters, or 15 Latin letters.

## Wiring diagnostic (`epd_diag.c`)

Turn on *menuconfig → Hardware → Run the display wiring diagnostic at every boot* (or call
`epd_diagnose()` before `epd_init()`). After a 4 s pause (time to open the serial log), it
drives the display pins as plain GPIOs and logs, under the tag `epd_diag`:

0. Switches the peripheral rail on (GPIO38).
1. **Pin survey:** each pin with the pull-up, then the pull-down. `floating` is normal for
   the ESP-to-display lines; **BUSY should be held low**, because a powered panel drives
   it. BUSY `floating` means the display has no power or its cable is loose.
2. **Shorts:** each pin driven high and low; any other pin that follows is reported.
   Caution: an *unpowered* panel can also show up here, because driving DIN high leaks
   power into it through its input protection diodes, which can drag other lines along.
3. **Reset and commands:** pulses RST, reports BUSY's idle level, then bit-bangs
   "software reset" and "refresh". A working panel is busy ~1-3 s in total
   (`<-- panel responded`). It also tries DIN/CLK swapped and CS/DC swapped (from the days
   of a hand-wired panel; on this board the wiring is fixed).

Turn the option off again afterwards: it adds ~20 s to every start-up.
