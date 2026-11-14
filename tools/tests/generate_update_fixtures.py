#!/usr/bin/env python3
"""Synthetic ESP-IDF-format bytes for host tests only; not flashable firmware."""
import hashlib
from pathlib import Path
import struct
import sys


def image(version: str, append_hash: bool = True, payload_size: int = 16384) -> bytes:
    header = bytearray(24)
    header[0:4] = bytes((0xE9, 2, 2, 0x40))
    struct.pack_into("<I", header, 4, 0x40374000)
    header[8] = 0xEE
    struct.pack_into("<H", header, 12, 9)
    header[23] = int(append_hash)
    descriptor = bytearray(256)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    descriptor[16:16+len(version)] = version.encode()
    descriptor[48:53] = b"Folio"
    payload = bytes((i * 73 + 19) % 256 for i in range(payload_size))
    output = header + struct.pack("<II", 0x3C000020, len(descriptor)) + descriptor
    output += struct.pack("<II", 0x40374000, len(payload)) + payload
    checksum = 0xEF
    for value in descriptor + payload:
        checksum ^= value
    output += bytes(15 - len(output) % 16) + bytes((checksum,))
    if append_hash:
        output += hashlib.sha256(output).digest()
    return bytes(output)


root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
for name, version, hashed, payload_size in (("good", "0.6.0-dev", True, 16384),
        ("changed", "0.6.1-dev", True, 16384), ("nohash", "0.6.0-dev", False, 16384),
        ("maximum", "0.6.0-dev", True, 3 * 1024 * 1024 - 336)):
    data = image(version, hashed, payload_size)
    (root / f"{name}.bin").write_bytes(data)
    (root / f"{name}.sha256").write_text(hashlib.sha256(data).hexdigest() + "  folio.bin\n")
