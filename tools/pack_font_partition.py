#!/usr/bin/env python3
"""Pack generated POD NANO font faces into the raw 8 MB font partition.

Run ``tools/build_cjk_fonts.py`` first.  The resulting image is written to
``font_data/font_partition.bin`` and described by ``font_data/font_manifest.json``.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "font_data"
OUT = DATA / "font_partition.bin"
PARTITION_SIZE = 8 * 1024 * 1024
MAGIC = 0x544E4F46  # FONT
VERSION = 2
HEADER = struct.Struct("<IHHIII")
ENTRY = struct.Struct("<HHII")


def align16(n: int) -> int:
    return (n + 15) & ~15


def main() -> int:
    faces_path = DATA / "faces.json"
    if not faces_path.exists():
        print(f"missing {faces_path}; run tools/build_cjk_fonts.py first", file=sys.stderr)
        return 1

    faces = json.loads(faces_path.read_text())["faces"]
    if not faces:
        print("no faces listed in faces.json", file=sys.stderr)
        return 1

    entries = []
    payload = bytearray()
    cursor = 0

    # Header + aligned entry table first, then each face blob.
    header_size = HEADER.size + ENTRY.size * len(faces)
    cursor = align16(header_size)
    payload.extend(b"\xff" * (cursor - header_size))

    for face in faces:
        path = DATA / face["file"]
        if not path.exists():
            print(f"missing {path}", file=sys.stderr)
            return 1
        blob = path.read_bytes()
        offset = align16(cursor)
        if offset != cursor:
            payload.extend(b"\xff" * (offset - cursor))
            cursor = offset
        entries.append((int(face["pixel_size"]), int(face["bpp"]),
                        offset, len(blob)))
        payload.extend(blob)
        cursor += len(blob)

    total = cursor
    if total > PARTITION_SIZE:
        print(f"font image too large: {total} > {PARTITION_SIZE}", file=sys.stderr)
        return 1

    header = HEADER.pack(MAGIC, VERSION, len(entries), header_size, total, 0)
    index = b"".join(ENTRY.pack(size, bpp, off, length)
                     for size, bpp, off, length in entries)
    image = bytearray(header + index)
    if len(image) < header_size:
        image.extend(b"\xff" * (header_size - len(image)))
    image.extend(payload)
    image.extend(b"\xff" * (PARTITION_SIZE - len(image)))
    OUT.write_bytes(image)

    manifest = {
        "version": VERSION,
        "header_size": header_size,
        "blob_size": total,
        "image_size": len(image),
        "free_bytes": PARTITION_SIZE - total,
        "faces": [{"pixel_size": s, "bpp": bpp, "offset": o, "length": l}
                  for s, bpp, o, l in entries],
    }
    (DATA / "font_manifest.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2))
    print(f"wrote {OUT} ({len(image)} bytes, {PARTITION_SIZE - total} bytes free)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
