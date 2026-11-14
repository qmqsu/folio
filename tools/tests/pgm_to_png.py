#!/usr/bin/env python3
"""Convert this renderer's P5 previews to grayscale PNG without extra packages."""
from pathlib import Path
import struct
import sys
import zlib


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def convert(path: Path) -> None:
    with path.open("rb") as stream:
        if stream.readline().strip() != b"P5":
            raise ValueError(f"not a binary PGM: {path}")
        width, height = map(int, stream.readline().split())
        if stream.readline().strip() != b"255" or not (0 < width <= 4096 and 0 < height <= 4096):
            raise ValueError(f"unexpected preview dimensions/depth: {path}")
        pixels = stream.read(width * height + 1)
        if len(pixels) != width * height:
            raise ValueError(f"unexpected preview length: {path}")
    rows = b"".join(b"\0" + pixels[y * width:(y + 1) * width] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    path.with_suffix(".png").write_bytes(png)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: pgm_to_png.py preview_directory")
    for preview in sorted(Path(sys.argv[1]).glob("*.pgm")):
        convert(preview)
