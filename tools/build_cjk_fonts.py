#!/usr/bin/env python3
"""Build zero-copy CJK font faces for the POD NANO font partition.

The firmware maps the raw ``font`` partition and hands glyph bitmaps to LVGL
directly from flash.  Nothing except a tiny ``lv_font_t`` and a few range
records is allocated from internal RAM, so the full 20k-glyph CJK sets can be
kept without the multi-megabyte heap cost of ``lv_binfont_create()``.

Face layout (little endian)::

    pod_font_face_t
    pod_font_range_t[range_count]
    pod_font_glyph_t[glyph_count]
    <16-byte aligned bitmap blob>

Glyph indices are assigned in ascending code point order.  A range maps a
contiguous code point span to a contiguous glyph index span.  Glyph bitmaps are
stored tightly packed, row-major, at 4 bpp for body text and 2 bpp for titles.

Examples::

    python tools/build_cjk_fonts.py
    python tools/build_cjk_fonts.py --sizes 16,24
    python tools/pack_font_partition.py
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import ImageFont
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FONT = ROOT / "managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
OUT = ROOT / "font_data"

FACE_MAGIC = 0x45434146  # "FACE"
FACE_VERSION = 1

FACE_HDR = struct.Struct("<IHHHHHHhhIIIII")
RANGE = struct.Struct("<III")
GLYPH = struct.Struct("<IHBBbbH")

# Code point blocks that are useful for music metadata and Chinese file names.
CODEPOINT_BLOCKS = (
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x00FF),  # Latin-1 supplement
    (0x2000, 0x206F),  # general punctuation
    (0x3000, 0x303F),  # CJK punctuation
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs
    (0xFF00, 0xFFEF),  # fullwidth forms
)

# The OTF's hhea metrics are far too tall for a dense 460 px UI.  These are the
# line metrics LVGL's built-in Source Han 16 face uses, scaled by 1.25 for 24 px.
SIZE_METRICS = {
    16: (20, 5),
    24: (30, 7),
}


def align16(n: int) -> int:
    return (n + 15) & ~15


def collect_codepoints(font_path: Path) -> list[int]:
    tt = TTFont(str(font_path), lazy=True)
    cmap = tt.getBestCmap()
    codepoints: list[int] = []
    for lo, hi in CODEPOINT_BLOCKS:
        for cp in range(lo, hi + 1):
            if cp in cmap:
                codepoints.append(cp)
    # De-duplicate and sort; glyph ids are assigned in this order.
    return sorted(set(codepoints))


def make_ranges(codepoints: list[int]) -> list[tuple[int, int, int]]:
    ranges: list[tuple[int, int, int]] = []
    start = prev = codepoints[0]
    glyph_start = 0
    for idx, cp in enumerate(codepoints[1:], start=1):
        if cp != prev + 1:
            ranges.append((start, prev, glyph_start))
            start = cp
            glyph_start = idx
        prev = cp
    ranges.append((start, prev, glyph_start))
    return ranges


def pack_mask(mask, bpp: int) -> bytes:
    w, h = mask.size
    if w == 0 or h == 0:
        return b""
    arr = np.frombuffer(bytes(mask), dtype=np.uint8).reshape(h, w)
    maxv = (1 << bpp) - 1
    q = ((arr.astype(np.uint16) * maxv + 127) // 255).astype(np.uint8)
    if bpp == 4:
        if w & 1:
            q = np.pad(q, ((0, 0), (0, 1)), mode="constant")
        packed = (q[:, 0::2] << 4) | q[:, 1::2]
    elif bpp == 2:
        pad = (-w) % 4
        if pad:
            q = np.pad(q, ((0, 0), (0, pad)), mode="constant")
        packed = ((q[:, 0::4] << 6) | (q[:, 1::4] << 4) |
                  (q[:, 2::4] << 2) | q[:, 3::4])
    else:
        raise ValueError(f"unsupported bpp {bpp}")
    return packed.tobytes()


def build_face(font_path: Path, size: int, bpp: int, codepoints: list[int]) -> bytes:
    metrics = SIZE_METRICS.get(size)
    if metrics is None:
        line_height = round(size * 1.25)
        base_line = round(size * 0.3)
    else:
        line_height, base_line = metrics

    pil_font = ImageFont.truetype(str(font_path), size=size)
    pil_ascent, _pil_descent = pil_font.getmetrics()
    ranges = make_ranges(codepoints)

    glyphs: list[tuple[int, int, int, int, int, int]] = []
    bitmap = bytearray()

    for cp in codepoints:
        ch = chr(cp)
        bbox = pil_font.getbbox(ch)
        mask = pil_font.getmask(ch, mode="L")
        w, h = mask.size
        if w == 0 or h == 0:
            adv_w = max(0, round(pil_font.getlength(ch) * 16))
            glyphs.append((len(bitmap), adv_w, 0, 0, 0, 0, 0))
            continue

        x0 = bbox[0]
        bottom = bbox[1] + h
        ofs_y = pil_ascent - bottom
        adv_w = max(0, round(pil_font.getlength(ch) * 16))
        glyphs.append((len(bitmap), adv_w, w, h, max(-128, min(127, x0)),
                       max(-128, min(127, ofs_y)), 0))
        bitmap.extend(pack_mask(mask, bpp))

    ranges_offset = FACE_HDR.size
    glyphs_offset = ranges_offset + RANGE.size * len(ranges)
    bitmap_offset = align16(glyphs_offset + GLYPH.size * len(glyphs))
    bitmap_size = len(bitmap)

    face = bytearray()
    face.extend(FACE_HDR.pack(
        FACE_MAGIC, FACE_VERSION, bpp, size, 0, len(ranges), 0,
        line_height, base_line, len(glyphs),
        ranges_offset, glyphs_offset, bitmap_offset, bitmap_size,
    ))
    for start, end, glyph_start in ranges:
        face.extend(RANGE.pack(start, end, glyph_start))
    for entry in glyphs:
        face.extend(GLYPH.pack(*entry))
    if len(face) < bitmap_offset:
        face.extend(b"\x00" * (bitmap_offset - len(face)))
    face.extend(bitmap)

    print(f"  {size}px {bpp}bpp: {len(glyphs)} glyphs, {len(ranges)} ranges, "
          f"{len(bitmap)} bitmap bytes, face {len(face)} bytes")
    return bytes(face)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT,
                        help="default font for faces without an override")
    parser.add_argument("--font-16", type=Path,
                        help="optional 16px font override")
    parser.add_argument("--font-24", type=Path,
                        help="optional 24px font override")
    parser.add_argument("--sizes", default="16,24")
    parser.add_argument("--bpp", default="16:4,24:2",
                        help="comma separated size:bpp pairs")
    args = parser.parse_args()

    if not args.font.exists():
        print(f"missing font: {args.font}", file=sys.stderr)
        return 1

    bpp_map = {}
    for item in args.bpp.split(","):
        size_s, bpp_s = item.split(":", 1)
        bpp_map[int(size_s)] = int(bpp_s)

    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    OUT.mkdir(exist_ok=True)

    faces = []
    faces_by_size = {}
    for size in sizes:
        bpp = bpp_map.get(size)
        if bpp not in (1, 2, 4, 8):
            print(f"no/unsupported bpp for size {size}", file=sys.stderr)
            return 1

        size_font = getattr(args, f"font_{size}", None) or args.font
        if not size_font.exists():
            print(f"missing font for {size}px: {size_font}", file=sys.stderr)
            return 1
        codepoints = collect_codepoints(size_font)
        print(f"font {size}px: {size_font.name} ({len(codepoints)} code points)")
        blob = build_face(size_font, size, bpp, codepoints)
        out = OUT / f"cjk{size}.bin"
        out.write_bytes(blob)
        faces.append({"pixel_size": size, "bpp": bpp,
                      "file": out.name, "length": len(blob)})
        faces_by_size[str(size)] = size_font.name

    manifest = {
        "font": args.font.name,
        "fonts": faces_by_size,
        "faces": faces,
    }
    (OUT / "faces.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
