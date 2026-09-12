"""Generate an Adafruit/TFT_eSPI ``GFXfont`` header from a TrueType font.

TFT_eSPI's built-in bitmap fonts (2, 4, 6, 7, 8) only contain ASCII 0x20-0x7F,
so accented Latin-1 characters such as ``a-umlaut``/``u-umlaut`` are silently
dropped when they reach ``drawString``. The bundled Adafruit "FreeFonts" are
ASCII-only too (they declare ``first = 0x20, last = 0x7E``). This script produces
a font that actually contains the Latin-1 range so those characters render.

The output uses the exact layout TFT_eSPI expects:

* ``GFXglyph`` = ``{ bitmapOffset, width, height, xAdvance, xOffset, yOffset }``
* glyph bitmaps are a *contiguous*, MSB-first bit stream (rows are **not**
  padded to byte boundaries), which is what ``TFT_eSPI::drawChar`` /
  ``TFT_eSprite::drawChar`` read
* ``yOffset`` is the signed distance from the text baseline to the top of the
  glyph bitmap (negative = above the baseline)

Usage::

    python tools/generate_gfxfont.py --font C:/Windows/Fonts/arial.ttf \\
        --size 24 --name Latin1Font24 --output src/latin1_font.h

Regenerate the header whenever you change the font/size; the generated file is
checked in so a normal build needs no font tooling.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

# Threshold above which an anti-aliased pixel counts as "on".
PIXEL_THRESHOLD = 128
# Margin around the render canvas so negative left bearings never clip.
CANVAS_MARGIN = 4


def load_codepoints(font_path: str) -> set:
    """Return the set of Unicode code points the font actually maps."""
    ttf = TTFont(font_path, fontNumber=0, lazy=True)
    try:
        cmap = ttf.getBestCmap()
    finally:
        ttf.close()
    return {cp for cp, name in cmap.items() if name != ".notdef"}


def render_glyph(font: ImageFont.FreeTypeFont, ascent: int, descent: int, ch: str):
    """Return (bitmap_rows, x_offset, y_offset) for one character."""
    left, top, right, bottom = font.getbbox(ch)
    width, height = right - left, bottom - top
    if width <= 0 or height <= 0:
        return [], left, top - ascent

    advance = int(round(font.getlength(ch)))
    origin_x = max(0, -left) + CANVAS_MARGIN
    origin_y = CANVAS_MARGIN

    canvas_w = origin_x + max(advance, right) + CANVAS_MARGIN
    canvas_h = (
        origin_y + ascent + descent + max(0, bottom - (ascent + descent)) + CANVAS_MARGIN
    )

    canvas = Image.new("L", (canvas_w, canvas_h), 0)
    ImageDraw.Draw(canvas).text((origin_x, origin_y), ch, font=font, fill=255)

    glyph = canvas.crop(
        (origin_x + left, origin_y + top, origin_x + right, origin_y + bottom)
    )
    rows = [
        [1 if glyph.getpixel((x, y)) >= PIXEL_THRESHOLD else 0 for x in range(width)]
        for y in range(height)
    ]
    return rows, left, top - ascent


def pack_rows(rows) -> list:
    """Pack 0/1 pixels into a contiguous MSB-first bit stream (no row padding)."""
    packed = []
    current = 0
    filled = 0
    for row in rows:
        for pixel in row:
            current = (current << 1) | pixel
            filled += 1
            if filled == 8:
                packed.append(current)
                current = 0
                filled = 0
    if filled:
        packed.append(current << (8 - filled))
    return packed


def build(font_path: str, size: int, first: int, last: int) -> dict:
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    available = load_codepoints(font_path)

    bitmaps: list = []
    glyphs: list = []

    for cp in range(first, last + 1):
        ch = chr(cp)
        # 0x7F-0x9F are control characters with no displayable glyph.
        is_control = 0x7F <= cp <= 0x9F
        has_glyph = cp in available and not is_control

        if has_glyph:
            rows, x_offset, y_offset = render_glyph(font, ascent, descent, ch)
            width = len(rows[0]) if rows else 0
            height = len(rows)
            advance = int(round(font.getlength(ch)))
        else:
            rows, x_offset, y_offset = [], 0, 0
            width = height = advance = 0

        glyphs.append(
            {
                "bitmap_offset": len(bitmaps),
                "width": width,
                "height": height,
                "advance": advance,
                "x_offset": x_offset,
                "y_offset": y_offset,
            }
        )
        bitmaps.extend(pack_rows(rows))

    return {
        "bitmaps": bitmaps,
        "glyphs": glyphs,
        "first": first,
        "last": last,
        "y_advance": ascent + descent,
        "ascent": ascent,
        "descent": descent,
        "size": size,
    }


def format_bytes(data: list) -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i : i + 12])
        lines.append(f"  {chunk},")
    return "\n".join(lines)


def format_glyphs(glyphs: list, first: int) -> str:
    lines = []
    for index, g in enumerate(glyphs):
        lines.append(
            "  {{ {:>6}, {:>3}, {:>3}, {:>3}, {:>4}, {:>4} }}, // 0x{:02X}".format(
                g["bitmap_offset"],
                g["width"],
                g["height"],
                g["advance"],
                g["x_offset"],
                g["y_offset"],
                first + index,
            )
        )
    return "\n".join(lines)


def generate_header(info: dict, name: str, font_path: str) -> str:
    return f"""// Auto-generated by tools/generate_gfxfont.py - do not edit by hand.
//
// Source font : {font_path}
// Pixel size  : {info['size']} (ascent {info['ascent']}, descent {info['descent']})
// Coverage    : U+{info['first']:04X} .. U+{info['last']:04X} (Latin-1)
// Glyphs      : {len(info['glyphs'])}
// Bitmap bytes: {len(info['bitmaps'])}
//
// Regenerate with:
//   python tools/generate_gfxfont.py --font "{font_path}" --size {info['size']} ^
//       --name {name} --output src/latin1_font.h

#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

constexpr int {name}_YADVANCE = {info['y_advance']};

const uint8_t {name}Bitmaps[] PROGMEM = {{
{format_bytes(info['bitmaps'])}
}};

const GFXglyph {name}Glyphs[] PROGMEM = {{
{format_glyphs(info['glyphs'], info['first'])}
}};

const GFXfont {name} PROGMEM = {{
  (uint8_t  *){name}Bitmaps,
  (GFXglyph *){name}Glyphs,
  0x{info['first']:02X}, 0x{info['last']:02X}, {info['y_advance']}
}};
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, help="Path to a .ttf/.otf file")
    parser.add_argument("--size", type=int, default=24, help="Pixel size (default 24)")
    parser.add_argument("--name", required=True, help="C identifier for the font")
    parser.add_argument("--output", required=True, help="Header file to write")
    parser.add_argument("--first", type=lambda v: int(v, 0), default=0x20)
    parser.add_argument("--last", type=lambda v: int(v, 0), default=0xFF)
    args = parser.parse_args()

    info = build(args.font, args.size, args.first, args.last)

    if len(info["bitmaps"]) >= 0x10000:
        raise SystemExit(
            f"Bitmap array is {len(info['bitmaps'])} bytes; TFT_eSPI reads the glyph "
            "offset as 16-bit, so keep it under 65536 (reduce --size or --last)."
        )

    header = generate_header(info, args.name, args.font)
    Path(args.output).write_text(header, encoding="utf-8")

    print(
        f"Wrote {args.output}: {len(info['glyphs'])} glyphs, "
        f"{len(info['bitmaps'])} bitmap bytes, yAdvance={info['y_advance']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
