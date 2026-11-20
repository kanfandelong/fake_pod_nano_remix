#!/usr/bin/env python3
"""Pack the complete POD NANO flash image into one 16 MB binary.

Layout (offsets come from ``partitions.csv`` and ``build/flasher_args.json``)::

    0x000000  bootloader         build/bootloader/bootloader.bin
    0x008000  partition table    build/partition_table/partition-table.bin
    0x010000  factory app        build/fake_pod_nano_remix.bin
    0x410000  font, 8 MB         font_data/font_partition.bin
    0xC30000  littlefs, empty    generated (formatted, no files)

Gaps are filled with 0xFF, so the result can be written to offset 0 in a
single flash operation.

The littlefs partition is a real, empty, formatted littlefs image (on-disk
version 2.1, 4096-byte blocks, no entries) so the firmware mounts it without
reformatting on first boot.  The format matches the configuration used by
``components/joltwallet__littlefs``, whose block size is fixed at 4096 and
whose ``LFS_DISK_VERSION`` is 0x00020001.

Usage::

    python tools/pack_firmware.py
    python tools/pack_firmware.py --output build/my_16mb.bin

Building the littlefs image needs the system ``littlefs`` package
(``pip install littlefs-python``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
PARTITIONS_CSV = ROOT / "partitions.csv"
FLASHER_ARGS = BUILD / "flasher_args.json"
FONT_IMAGE = ROOT / "font_data" / "font_partition.bin"

DEFAULT_OUTPUT = BUILD / "fake_pod_nano_remix_full_16mb.bin"
DEFAULT_FLASH_SIZE = 16 * 1024 * 1024
PAD_BYTE = 0xFF

# Must match components/joltwallet__littlefs: fixed 4096-byte blocks and
# LFS_DISK_VERSION 0x00020001.
LFS_BLOCK_SIZE = 4096
LFS_DISK_VERSION = 0x00020001


@dataclass
class Partition:
    name: str
    type: str
    subtype: str
    offset: int
    size: int


@dataclass
class Section:
    name: str
    offset: int
    data: bytes
    source: str

    @property
    def end(self) -> int:
        return self.offset + len(self.data)


def parse_partitions(path: Path) -> dict[str, Partition]:
    parts: dict[str, Partition] = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5:
            raise SystemExit(f"{path}:{lineno}: malformed partition line: {raw!r}")
        name, ptype, subtype, offset, size = fields[:5]
        parts[name] = Partition(name, ptype, subtype, int(offset, 16), int(size, 16))
    return parts


def read_bin(path: Path) -> bytes:
    if not path.is_file():
        raise SystemExit(
            f"missing {path.relative_to(ROOT)}\n"
            "build the project first: idf.py build"
        )
    return path.read_bytes()


def make_empty_littlefs(size: int) -> bytes:
    """Return a formatted littlefs image containing no files."""
    if size <= 0 or size % LFS_BLOCK_SIZE:
        raise SystemExit(
            f"littlefs partition size 0x{size:X} is not a multiple of "
            f"{LFS_BLOCK_SIZE}"
        )
    try:
        from littlefs import LittleFS, UserContext
    except ImportError:
        raise SystemExit(
            "the littlefs-python package is required to format the empty "
            "partition: pip install littlefs-python"
        )

    fs = LittleFS(
        block_size=LFS_BLOCK_SIZE,
        block_count=size // LFS_BLOCK_SIZE,
        disk_version=LFS_DISK_VERSION,
        mount=False,
    )
    fs.format()
    fs.mount()
    fs.unmount()
    image = bytes(fs.context.buffer)
    if len(image) != size:
        raise SystemExit(
            f"littlefs image is {len(image)} bytes, expected {size}"
        )

    # Prove the image is mountable and empty before it goes into the bundle.
    check = LittleFS(
        context=UserContext(buffer=bytearray(image)),
        block_size=LFS_BLOCK_SIZE,
        block_count=size // LFS_BLOCK_SIZE,
    )
    entries = check.listdir("/")
    version = check.fs_stat().disk_version
    check.unmount()
    if entries:
        raise SystemExit(f"littlefs image is not empty: {entries}")
    if version != LFS_DISK_VERSION:
        raise SystemExit(
            f"littlefs disk version 0x{version:08X}, expected 0x{LFS_DISK_VERSION:08X}"
        )
    return image


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def littlefs_entries(image: bytes) -> list[str]:
    """Mount a littlefs image with the device's geometry and list its root."""
    from littlefs import LittleFS, UserContext

    fs = LittleFS(
        context=UserContext(buffer=bytearray(image)),
        block_size=LFS_BLOCK_SIZE,
        block_count=len(image) // LFS_BLOCK_SIZE,
    )
    entries = fs.listdir("/")
    fs.unmount()
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--output", type=Path, default=DEFAULT_OUTPUT,
        help=f"merged image path (default: {DEFAULT_OUTPUT.relative_to(ROOT)})",
    )
    parser.add_argument(
        "--flash-size", type=lambda s: int(s, 0), default=DEFAULT_FLASH_SIZE,
        help="total flash size in bytes (default: 16 MiB)",
    )
    args = parser.parse_args()

    flash_size = args.flash_size
    if flash_size <= 0 or flash_size % 4096:
        raise SystemExit("--flash-size must be a positive multiple of 4096")

    parts = parse_partitions(PARTITIONS_CSV)
    for required in ("factory", "font", "littlefs"):
        if required not in parts:
            raise SystemExit(f"partitions.csv has no {required!r} partition")
    app_part = parts["factory"]
    font_part = parts["font"]
    lfs_part = parts["littlefs"]

    flasher = json.loads(FLASHER_ARGS.read_text())
    flash_files = {int(addr, 16): BUILD / name
                   for addr, name in flasher["flash_files"].items()}
    bootloader_path = flash_files.get(0x0)
    table_path = flash_files.get(0x8000)
    app_path = flash_files.get(app_part.offset)
    if not bootloader_path or not table_path or not app_path:
        raise SystemExit(
            f"flasher_args.json does not map bootloader/table/app to "
            f"0x0/0x8000/0x{app_part.offset:X}"
        )

    font_image = read_bin(FONT_IMAGE)
    if len(font_image) > font_part.size:
        raise SystemExit(
            f"font image {len(font_image)} bytes exceeds partition "
            f"0x{font_part.size:X}"
        )

    lfs_image = make_empty_littlefs(lfs_part.size)

    sections = [
        Section("bootloader", 0x0, read_bin(bootloader_path),
                bootloader_path.relative_to(ROOT).as_posix()),
        Section("partition-table", 0x8000, read_bin(table_path),
                table_path.relative_to(ROOT).as_posix()),
        Section("factory", app_part.offset, read_bin(app_path),
                app_path.relative_to(ROOT).as_posix()),
        Section("font", font_part.offset, font_image,
                FONT_IMAGE.relative_to(ROOT).as_posix()),
        Section("littlefs", lfs_part.offset, lfs_image,
                "generated: formatted, empty"),
    ]
    sections.sort(key=lambda s: s.offset)

    if len(sections[2].data) > app_part.size:
        raise SystemExit(
            f"app image {len(sections[2].data)} bytes exceeds factory "
            f"partition 0x{app_part.size:X}"
        )

    previous_end = 0
    for section in sections:
        if section.offset < previous_end:
            raise SystemExit(
                f"section {section.name} at 0x{section.offset:X} overlaps "
                f"the previous section ending at 0x{previous_end:X}"
            )
        if section.end > flash_size:
            raise SystemExit(
                f"section {section.name} ends at 0x{section.end:X}, beyond "
                f"flash size 0x{flash_size:X}"
            )
        previous_end = section.end

    if lfs_part.offset + lfs_part.size > flash_size:
        raise SystemExit("littlefs partition does not fit the flash size")
    if sections[-1].end != flash_size:
        print(
            f"note: image ends at 0x{sections[-1].end:X}, padding to "
            f"0x{flash_size:X}",
            file=sys.stderr,
        )

    image = bytearray(bytes([PAD_BYTE]) * flash_size)
    for section in sections:
        image[section.offset:section.end] = section.data

    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)

    # Re-read the file and verify every section landed at the right offset.
    written = output.read_bytes()
    if len(written) != flash_size:
        raise SystemExit(f"wrote {len(written)} bytes, expected {flash_size}")
    if written[0x9000:0xF000] != bytes([PAD_BYTE]) * 0x6000:
        raise SystemExit("NVS region is not blank")

    for section in sections:
        if written[section.offset:section.end] != section.data:
            raise SystemExit(f"readback mismatch for {section.name}")

    if written[0x10000] != 0xE9:
        raise SystemExit("factory image has no ESP image magic at 0x10000")
    if written[font_part.offset:font_part.offset + 4] != b"FONT":
        raise SystemExit("font partition has no FONT magic")

    lfs_entries = littlefs_entries(
        written[lfs_part.offset:lfs_part.offset + lfs_part.size]
    )
    if lfs_entries != []:
        raise SystemExit(f"littlefs partition is not empty: {lfs_entries}")

    manifest = {
        "output": output.relative_to(ROOT).as_posix(),
        "size": len(written),
        "sha256": hashlib.sha256(written).hexdigest(),
        "flash_size": flash_size,
        "pad_byte": PAD_BYTE,
        "sections": [
            {
                "name": s.name,
                "offset": f"0x{s.offset:06X}",
                "size": len(s.data),
                "md5": md5(s.data),
                "source": s.source,
            }
            for s in sections
        ],
    }
    manifest_path = output.with_suffix(".json")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"{'section':<16} {'offset':>9} {'size':>10}  md5")
    for s in sections:
        print(f"{s.name:<16} 0x{s.offset:06X} {len(s.data):>10}  {md5(s.data)}")
    print(f"{'total':<16} 0x{0:06X} {len(written):>10}  {hashlib.sha256(written).hexdigest()[:32]}...")
    print(f"\nwrote {output.relative_to(ROOT)} ({len(written)} bytes)")
    print(f"wrote {manifest_path.relative_to(ROOT)}")
    print(
        "\nflash with:\n"
        f"  esptool --chip esp32s3 -p COMx write_flash 0x0 "
        f"{output.relative_to(ROOT).as_posix()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
