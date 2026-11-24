/*
 * Pictures for the e-paper: decodes a JPEG or PNG into a small 8-bit greyscale image, ready
 * for epd_draw_grey8() (which dithers it to the panel's 4 greys or to black and white).
 *
 *   Baseline JPEG     TJpgDec from the ESP32-S3's ROM; big pictures are decoded at 1/2, 1/4
 *                     or 1/8 scale on the way in, so a 1000x1000 cover takes about as long as
 *                     a 125x125 one
 *   Progressive JPEG  only its first scan (each 8x8 block's average): a 1/8-size picture,
 *                     fine for a cover thumbnail
 *   PNG               any colour type and bit depth, not interlaced; streamed a row at a time
 *                     (inflate from the ROM), transparency on white
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int w, h;
    uint8_t *pix;                       // w * h bytes, row by row, 0 = black, 255 = white
} image_grey_t;

// Decodes the picture (JPEG or PNG, told apart by its first bytes) at `offset` in `f`
// (`size` bytes; 0 = up to the end of the file), shrunk to fit within max_w x max_h (aspect
// ratio kept, never enlarged). On success fills *out (free with image_free()).
bool image_jpeg_grey(FILE *f, uint32_t offset, uint32_t size, int max_w, int max_h, image_grey_t *out);

// Same, from a file path, and from memory
bool image_jpeg_grey_file(const char *path, int max_w, int max_h, image_grey_t *out);
bool image_grey_mem(const void *data, size_t len, int max_w, int max_h, image_grey_t *out);

// Stretches the contrast so the darkest ~1% becomes black and the lightest ~1% white:
// worth doing before dithering to so few levels
void image_auto_levels(image_grey_t *img);

void image_free(image_grey_t *img);

// The size of the picture (JPEG or PNG) at `offset` in `f` (`size` bytes, 0 = to the end),
// from its headers, without decoding it
bool image_jpeg_size(FILE *f, uint32_t offset, uint32_t size, int *w, int *h);
bool image_jpeg_size_file(const char *path, int *w, int *h);

// Turns the picture a quarter turn counter-clockwise (its top ends up on the left: the same
// way as the title bar's text), in place. False if there's no memory (the picture is unchanged).
bool image_rotate_ccw(image_grey_t *img);

#define IMAGE_MAX_ENLARGE 2.0f        // small pictures are shown at most this much bigger

// Whether a w x h picture is shown bigger turned a quarter turn in a box_w x box_h space:
// true when that makes it at least 1.5 x the area (pictures clearly taller than wide, in a wide box).
// Counts small pictures as growing up to IMAGE_MAX_ENLARGE (see image_enlarge_to_fit).
bool image_should_rotate(int w, int h, int box_w, int box_h);

// Scales a small picture up (bilinear, at most IMAGE_MAX_ENLARGE) to fit box_w x box_h.
// Does nothing to one that already fills the box. True if it changed.
bool image_enlarge_to_fit(image_grey_t *img, int box_w, int box_h);
