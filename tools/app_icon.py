"""
Converts an app icon between the two forms an app package can carry:

  icon.png  any size, for people and the catalog
  icon.bin  32x32, 1 bit per pixel (128 bytes): rows top to bottom, 4 bytes each,
            most significant bit = leftmost pixel, 1 = black. This is what the device draws.

The web portal does png -> bin itself when installing; this script is for bundled apps and
for checking an icon. It has no dependencies (PNG is read and written with zlib).

  python tools/app_icon.py to-bin apps/clock/icon.png apps/clock/icon.bin
  python tools/app_icon.py to-png apps/clock/icon.bin apps/clock/icon.png
"""
import struct
import sys
import zlib


def read_png_gray(path):
    """PNG -> (width, height, rows of 0-255 luminance). 8-bit gray/RGB/RGBA, no interlace."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, width = 8, b'', 0
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and interlace == 0 and ctype in (0, 2, 4, 6), 'use an 8-bit, non-interlaced PNG'
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw, stride = zlib.decompress(idat), width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prev[i], prev[i - channels] if i >= channels else 0
            pred = [0, a, b, (a + b) // 2, a + b - c][f] if f < 4 else 0
            if f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
            line[i] = (line[i] + pred) & 255
        gray = []
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            alpha = px[-1] if ctype in (4, 6) else 255
            lum = px[0] if ctype in (0, 4) else (px[0] * 30 + px[1] * 59 + px[2] * 11) // 100
            gray.append(255 - (255 - lum) * alpha // 255)   # transparent = white
        rows.append(gray)
        prev = line
    return width, height, rows


def to_bin(png, out):
    w, h, rows = read_png_gray(png)
    bits = bytearray(128)
    for y in range(32):
        for x in range(32):
            if rows[y * h // 32][x * w // 32] < 128:      # nearest-neighbour scale to 32x32
                bits[y * 4 + x // 8] |= 0x80 >> (x % 8)
    open(out, 'wb').write(bits)


def to_png(binfile, out, scale=1):
    bits = open(binfile, 'rb').read()
    assert len(bits) == 128, 'icon.bin must be 128 bytes'
    raw = b''
    for y in range(32 * scale):
        raw += b'\x00' + bytes(0 if bits[(y // scale) * 4 + (x // scale) // 8] & (0x80 >> ((x // scale) % 8)) else 255
                               for x in range(32 * scale))
    chunk = lambda kind, body: struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32 * scale, 32 * scale, 8, 0, 0, 0, 0)) \
        + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] not in ('to-bin', 'to-png'):
        sys.exit(__doc__)
    (to_bin if sys.argv[1] == 'to-bin' else to_png)(sys.argv[2], sys.argv[3])
