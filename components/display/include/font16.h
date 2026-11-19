/*
 * 16 px tall Unicode font (GNU Unifont): Chinese characters, pinyin with tone marks,
 * ASCII and punctuation. Glyphs are 16 px wide (Chinese, fullwidth punctuation) or
 * 8 px wide (Latin). Text is UTF-8.
 */
#pragma once

#include <stdint.h>

#define FONT16_HEIGHT 16

// Decodes one UTF-8 character from *s, advances *s past it, and returns its code point
// (0 at the end of the string)
uint32_t font16_next_char(const char **s);

// Width in pixels of one character (8 or 16; characters the font lacks count as 16)
int font16_char_width(uint32_t cp);

// Draws one character with its top-left corner at (x, y). Returns its width.
// scale > 1 draws each pixel as a scale x scale block (for icons).
int font16_draw_char(int x, int y, uint32_t cp, int scale, uint8_t color);

// Draws a UTF-8 string. Returns the x position after the text.
int font16_draw_text(int x, int y, const char *text, uint8_t color);

// Width in pixels of a UTF-8 string
int font16_text_width(const char *text);
