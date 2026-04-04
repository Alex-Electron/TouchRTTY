#!/usr/bin/env python3
"""Convert BDF bitmap font to Adafruit GFX font header (.h) for LovyanGFX."""

import sys
import re
from pathlib import Path

def parse_bdf(filepath):
    """Parse a BDF font file, return list of glyphs and font properties."""
    glyphs = {}
    font_props = {}

    with open(filepath, 'r') as f:
        lines = f.readlines()

    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if line.startswith("PIXEL_SIZE"):
            font_props['pixel_size'] = int(line.split()[-1])
        elif line.startswith("FONT_ASCENT"):
            font_props['ascent'] = int(line.split()[-1])
        elif line.startswith("FONT_DESCENT"):
            font_props['descent'] = int(line.split()[-1])

        if line == "STARTCHAR":
            pass

        if line.startswith("STARTCHAR"):
            char_name = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
            encoding = -1
            bbx_w, bbx_h, bbx_xoff, bbx_yoff = 0, 0, 0, 0
            dwidth = 0
            bitmap_data = []

            i += 1
            while i < len(lines) and not lines[i].strip().startswith("ENDCHAR"):
                cl = lines[i].strip()
                if cl.startswith("ENCODING"):
                    encoding = int(cl.split()[-1])
                elif cl.startswith("DWIDTH"):
                    dwidth = int(cl.split()[1])
                elif cl.startswith("BBX"):
                    parts = cl.split()
                    bbx_w = int(parts[1])
                    bbx_h = int(parts[2])
                    bbx_xoff = int(parts[3])
                    bbx_yoff = int(parts[4])
                elif cl == "BITMAP":
                    i += 1
                    while i < len(lines) and not lines[i].strip().startswith("ENDCHAR"):
                        hex_row = lines[i].strip()
                        if hex_row:
                            bitmap_data.append(int(hex_row, 16))
                        i += 1
                    break
                i += 1

            if 32 <= encoding <= 126:  # ASCII printable range
                glyphs[encoding] = {
                    'name': char_name,
                    'width': bbx_w,
                    'height': bbx_h,
                    'xoff': bbx_xoff,
                    'yoff': bbx_yoff,
                    'xadvance': dwidth if dwidth > 0 else bbx_w,
                    'bitmap': bitmap_data,
                    'bbx_w': bbx_w,
                }
        i += 1

    return glyphs, font_props


def glyph_to_bits(glyph):
    """Convert glyph bitmap to packed bit array (Adafruit GFX format)."""
    bits = []
    w = glyph['width']
    for row_val in glyph['bitmap']:
        # BDF stores bits left-aligned in bytes
        # We need the leftmost 'w' bits
        byte_width = (w + 7) // 8
        total_bits = byte_width * 8
        for bit_pos in range(w):
            shift = total_bits - 1 - bit_pos
            bits.append(1 if (row_val >> shift) & 1 else 0)
    return bits


def pack_bits(bits):
    """Pack bit array into bytes."""
    result = []
    for i in range(0, len(bits), 8):
        byte = 0
        for j in range(8):
            if i + j < len(bits):
                byte |= (bits[i + j] << (7 - j))
        result.append(byte)
    return result


def generate_header(glyphs, font_props, font_name):
    """Generate C header file content."""
    first_char = 32
    last_char = 126

    ascent = font_props.get('ascent', 0)
    descent = font_props.get('descent', 0)
    y_advance = ascent + descent

    # Pack all bitmaps
    all_bitmap_bytes = []
    glyph_data = []

    for code in range(first_char, last_char + 1):
        offset = len(all_bitmap_bytes)

        if code in glyphs:
            g = glyphs[code]
            bits = glyph_to_bits(g)
            packed = pack_bits(bits)
            all_bitmap_bytes.extend(packed)

            # yOffset: in Adafruit GFX, yOffset is relative to cursor baseline
            # BDF: yoff is from baseline going up
            y_offset = -(g['yoff'] + g['height'])  # convert to GFX convention

            glyph_data.append({
                'offset': offset,
                'width': g['width'],
                'height': g['height'],
                'xadvance': g['xadvance'],
                'xoffset': g['xoff'],
                'yoffset': y_offset,
            })
        else:
            # Missing glyph — use space
            glyph_data.append({
                'offset': offset,
                'width': 0,
                'height': 0,
                'xadvance': glyphs.get(32, {}).get('xadvance', 4),
                'xoffset': 0,
                'yoffset': 0,
            })

    # Generate C code
    lines = []
    lines.append(f"// Auto-generated from BDF by bdf2gfx.py")
    lines.append(f"// Font: {font_name}")
    lines.append(f"#pragma once")
    lines.append(f"#include <lgfx/v1/lgfx_fonts.hpp>")
    lines.append(f"")

    # Bitmap array
    lines.append(f"const uint8_t {font_name}Bitmaps[] PROGMEM = {{")
    for i in range(0, len(all_bitmap_bytes), 16):
        chunk = all_bitmap_bytes[i:i+16]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_str},")
    lines.append(f"}};")
    lines.append(f"")

    # Glyph table
    lines.append(f"const GFXglyph {font_name}Glyphs[] PROGMEM = {{")
    lines.append(f"    // offset, width, height, xAdvance, xOffset, yOffset")
    for i, gd in enumerate(glyph_data):
        char_code = first_char + i
        ch = chr(char_code) if 33 <= char_code <= 126 else ' '
        lines.append(f"    {{ {gd['offset']:5d}, {gd['width']:2d}, {gd['height']:2d}, "
                     f"{gd['xadvance']:2d}, {gd['xoffset']:3d}, {gd['yoffset']:3d} }}, // '{ch}' ({char_code})")
    lines.append(f"}};")
    lines.append(f"")

    # Font struct
    lines.append(f"const GFXfont {font_name} PROGMEM = {{")
    lines.append(f"    (uint8_t  *){font_name}Bitmaps,")
    lines.append(f"    (GFXglyph *){font_name}Glyphs,")
    lines.append(f"    {first_char}, {last_char}, {y_advance}")
    lines.append(f"}};")

    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.bdf> <FontName>")
        sys.exit(1)

    bdf_path = sys.argv[1]
    font_name = sys.argv[2]

    print(f"[*] Parsing {bdf_path}...")
    glyphs, props = parse_bdf(bdf_path)
    print(f"    Found {len(glyphs)} glyphs, ascent={props.get('ascent')}, descent={props.get('descent')}")

    header = generate_header(glyphs, props, font_name)

    out_path = Path(bdf_path).with_suffix('.h')
    # Write to fonts dir
    out_path = Path(bdf_path).parent / f"{font_name}.h"
    with open(out_path, 'w') as f:
        f.write(header)

    print(f"[+] Generated {out_path}")

    # Stats
    sample = glyphs.get(65, glyphs.get(32, {}))
    print(f"    Sample glyph 'A': {sample.get('width')}x{sample.get('height')}, xAdvance={sample.get('xadvance')}")


if __name__ == "__main__":
    main()
