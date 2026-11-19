#include "font16.h"

#include <stdbool.h>
#include <string.h>                     // memcmp(), memcpy()
#include "epd.h"                  // epd_draw_pixel(), epd_draw_rect()

// unifont16.bin, built by tools/make_font.py and linked into the firmware (EMBED_FILES)
extern const uint8_t font_start[] asm("_binary_unifont16_bin_start");

#define GLYPH_BYTES 32                  // 16 rows x 2 bytes

typedef struct {
    uint32_t first, last, index;        // code points first..last are glyphs index.. onward
} range_t;

static uint32_t range_count;
static const uint8_t *widths;           // one byte per glyph: 8, 16, or 0 = missing
static const uint8_t *glyphs;           // GLYPH_BYTES per glyph

// Reads the file header once
static bool font_ready(void)
{
    if (glyphs) {
        return true;
    }
    if (memcmp(font_start, "UF16", 4) != 0) {
        return false;                                       // wrong file embedded
    }
    memcpy(&range_count, font_start + 4, 4);
    const uint8_t *ranges = font_start + 8;
    range_t last;
    memcpy(&last, ranges + (range_count - 1) * sizeof(range_t), sizeof(range_t));
    uint32_t glyph_count = last.index + (last.last - last.first + 1);
    widths = ranges + range_count * sizeof(range_t);
    glyphs = widths + ((glyph_count + 3) & ~3u);            // widths are padded to 4 bytes
    return true;
}

// Glyph number for a code point, or -1 if the font doesn't have it
static int glyph_index(uint32_t cp)
{
    if (!font_ready()) {
        return -1;
    }
    for (uint32_t i = 0; i < range_count; i++) {
        range_t r;
        memcpy(&r, font_start + 8 + i * sizeof(range_t), sizeof(range_t));
        if (cp >= r.first && cp <= r.last) {
            int index = r.index + (cp - r.first);
            return widths[index] ? index : -1;
        }
    }
    return -1;
}

uint32_t font16_next_char(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp;
    int extra;                                              // continuation bytes that follow

    if (*p == 0) {
        return 0;
    } else if (*p < 0x80) {
        cp = *p; extra = 0;                                 // 0xxxxxxx: ASCII
    } else if ((*p & 0xE0) == 0xC0) {
        cp = *p & 0x1F; extra = 1;                          // 110xxxxx: 2 bytes
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p & 0x0F; extra = 2;                          // 1110xxxx: 3 bytes (all Chinese)
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p & 0x07; extra = 3;                          // 11110xxx: 4 bytes
    } else {
        *s += 1;                                            // stray continuation byte: skip it
        return '?';
    }
    p++;
    for (int i = 0; i < extra; i++, p++) {
        if ((*p & 0xC0) != 0x80) {                          // truncated sequence
            *s = (const char *)p;
            return '?';
        }
        cp = (cp << 6) | (*p & 0x3F);
    }
    *s = (const char *)p;
    return cp;
}

int font16_char_width(uint32_t cp)
{
    int index = glyph_index(cp);
    return index < 0 ? 16 : widths[index];
}

int font16_draw_char(int x, int y, uint32_t cp, int scale, uint8_t color)
{
    int index = glyph_index(cp);
    if (index < 0) {                                        // missing: hollow box so it's obvious
        epd_draw_rect(x + 2 * scale, y + 2 * scale, 12 * scale, 12 * scale, color, false);
        return 16 * scale;
    }

    const uint8_t *g = glyphs + index * GLYPH_BYTES;
    int w = widths[index];
    for (int row = 0; row < 16; row++) {
        uint16_t bits = (g[row * 2] << 8) | g[row * 2 + 1];  // MSB = leftmost pixel
        for (int col = 0; col < w; col++) {
            if (bits & (0x8000 >> col)) {
                if (scale == 1) {
                    epd_draw_pixel(x + col, y + row, color);
                } else {
                    epd_draw_rect(x + col * scale, y + row * scale, scale, scale, color, true);
                }
            }
        }
    }
    return w * scale;
}

int font16_draw_text(int x, int y, const char *text, uint8_t color)
{
    uint32_t cp;
    while ((cp = font16_next_char(&text)) != 0) {
        x += font16_draw_char(x, y, cp, 1, color);
    }
    return x;
}

int font16_text_width(const char *text)
{
    int w = 0;
    uint32_t cp;
    while ((cp = font16_next_char(&text)) != 0) {
        w += font16_char_width(cp);
    }
    return w;
}
