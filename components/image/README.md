# image: JPEG to greyscale

`image_jpeg_grey()` / `image_jpeg_grey_file()` decode a baseline JPEG (from any offset in a
file, e.g. a cover embedded in an MP3) into a small 8-bit greyscale picture that fits a box,
for `epd_draw_grey8()` to dither onto the panel. `image_auto_levels()` stretches the contrast
first, which helps a lot with only 4 levels.

The decoder is TJpgDec from the ESP32-S3 ROM (no flash cost). It picks the largest of its
1/2, 1/4, 1/8 scales that still leaves enough pixels, then box-filters down to the box, so
even big pictures decode quickly and with little RAM. Progressive JPEGs and PNGs are refused.
