"""
Builds main/unifont16.bin, the 16 px bitmap font the story app draws Chinese with.

Source: GNU Unifont (https://unifoundry.com/unifont/, GPLv2+ with font embedding
exception / SIL OFL 1.1). Download unifont-<version>.hex.gz, unzip it, then run:

    python tools/make_font.py path/to/unifont.hex main/unifont16.bin

Output layout (all integers little-endian), read by main/font16.c:
    "UF16"                      magic
    u32 range_count
    range_count x {u32 first, u32 last, u32 index}   code point ranges; index = glyph number of `first`
    u8  widths[glyph_count]     8 or 16 px, 0 = missing (drawn as a box); padded to a multiple of 4
    glyph_count x 32 bytes      16 rows x 2 bytes, MSB = leftmost pixel (8 px glyphs use the first byte)
"""
import struct
import sys

# Code point ranges to include. Keep in sync with what the story prompt allows.
RANGES = [
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x024F),   # Latin-1 + Latin Extended-A/B: pinyin tone vowels (ā á ǎ à, ü ǖ ǘ ǚ ǜ)
    (0x2010, 0x2027),   # general punctuation: dashes, curly quotes, ellipsis
    (0x3000, 0x303F),   # CJK punctuation: 。、《》「」
    (0x4E00, 0x9FFF),   # CJK Unified Ideographs (all common Chinese characters)
    (0xFF00, 0xFFEF),   # fullwidth forms: ，！？：；（）
]


def main(hex_path, out_path):
    glyphs = {}
    with open(hex_path, encoding="ascii") as f:
        for line in f:
            cp, _, bits = line.strip().partition(":")
            if bits:
                glyphs[int(cp, 16)] = bytes.fromhex(bits)

    header = [b"UF16", struct.pack("<I", len(RANGES))]
    widths = bytearray()
    data = bytearray()
    for first, last in RANGES:
        header.append(struct.pack("<III", first, last, len(widths)))
        for cp in range(first, last + 1):
            g = glyphs.get(cp)
            if g is None:
                widths.append(0)
                data += bytes(32)
            elif len(g) == 16:                      # 8 px wide: one byte per row
                widths.append(8)
                data += b"".join(bytes([b, 0]) for b in g)
            else:                                   # 16 px wide: two bytes per row
                widths.append(16)
                data += g
    widths += bytes(-len(widths) % 4)

    with open(out_path, "wb") as f:
        f.write(b"".join(header) + widths + data)
    print(f"{out_path}: {len(widths)} glyphs, {len(data) // 1024} KB of bitmaps")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
