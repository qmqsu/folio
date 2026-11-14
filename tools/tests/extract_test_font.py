#!/usr/bin/env python3
"""Extract a glyf face from a local TTC into a temporary TTF for UI tests only.

The local font's license still applies. Do not distribute the derived font with
Folio; this helper only avoids requiring a fontTools installation for previews.
"""
from pathlib import Path
import argparse
import struct


def checksum(data: bytes) -> int:
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def subset_tables(tables: dict[bytes, bytearray], characters: set[int]) -> None:
    """Keep named BMP characters and their composite glyphs, preserving glyph IDs.

    Only the temporary test artifact changes. The names/author information remain
    intact, and the resulting file is not a distributable project font.
    """
    cmap = tables[b"cmap"]
    mapping = None
    for i in range(struct.unpack_from(">H", cmap, 2)[0]):
        platform, encoding, offset = struct.unpack_from(">HHI", cmap, 4 + i * 8)
        if platform in (0, 3) and (platform == 0 or encoding in (1, 10)) and struct.unpack_from(">H", cmap, offset)[0] == 4:
            mapping = offset
            break
    if mapping is None:
        raise ValueError("test subset requires a format-4 cmap")
    n = struct.unpack_from(">H", cmap, mapping + 6)[0] // 2
    end = mapping + 14; start = end + n * 2 + 2; delta = start + n * 2; ranges = delta + n * 2
    glyphs = {}
    for cp in sorted(characters | set(range(32, 127))):
        if not 0 <= cp < 65535 or 0xD800 <= cp <= 0xDFFF:
            continue
        for i in range(n):
            last = struct.unpack_from(">H", cmap, end + i * 2)[0]
            if cp > last:
                continue
            first, add, shift = (struct.unpack_from(">H", cmap, base + i * 2)[0] for base in (start, delta, ranges))
            if cp < first:
                break
            gid = cp if shift == 0 else struct.unpack_from(">H", cmap, ranges + i * 2 + shift + (cp - first) * 2)[0]
            if shift == 0 or gid != 0:
                gid = (gid + add) & 65535
            if gid:
                glyphs[cp] = gid
            break
    count = struct.unpack_from(">H", tables[b"maxp"], 4)[0]
    long_loca = struct.unpack_from(">H", tables[b"head"], 50)[0] == 1
    offsets = list(struct.unpack(f">{count + 1}{'I' if long_loca else 'H'}", tables[b"loca"][:(count + 1) * (4 if long_loca else 2)]))
    if not long_loca:
        offsets = [n * 2 for n in offsets]
    selected = set(glyphs.values()) | {0}
    todo = list(selected)
    while todo:
        gid = todo.pop()
        data = tables[b"glyf"][offsets[gid]:offsets[gid + 1]]
        if not data or struct.unpack_from(">h", data)[0] >= 0:
            continue
        pos = 10
        while True:
            flags, child = struct.unpack_from(">HH", data, pos)
            if child >= count:
                raise ValueError("composite glyph ID outside maxp")
            if child not in selected:
                selected.add(child); todo.append(child)
            pos += 4 + (4 if flags & 1 else 2)
            pos += 2 if flags & 8 else 4 if flags & 64 else 8 if flags & 128 else 0
            if not flags & 32:
                break
    glyf = bytearray(); loca = bytearray()
    for gid in range(count):
        loca.extend(struct.pack(">I", len(glyf)))
        if gid in selected:
            glyf.extend(tables[b"glyf"][offsets[gid]:offsets[gid + 1]])
            glyf.extend(b"\0" * (-len(glyf) % 4))
    loca.extend(struct.pack(">I", len(glyf)))
    tables[b"glyf"] = glyf; tables[b"loca"] = loca
    struct.pack_into(">H", tables[b"head"], 50, 1)
    codes = sorted(glyphs) + [65535]; n = len(codes)
    if 16 + 8 * n > 65535:
        raise ValueError("too many characters for this test subset format")
    power = 1 << (n.bit_length() - 1)
    sub = bytearray(struct.pack(">HHHHHHH", 4, 16 + 8 * n, 0, n * 2, power * 2, power.bit_length() - 1, n * 2 - power * 2))
    sub.extend(struct.pack(f">{n}H", *codes)); sub.extend(b"\0\0")
    sub.extend(struct.pack(f">{n}H", *codes))
    sub.extend(struct.pack(f">{n}H", *((glyphs.get(cp, 0) - cp) & 65535 for cp in codes)))
    sub.extend(b"\0\0" * n)
    tables[b"cmap"] = bytearray(struct.pack(">HHHHI", 0, 1, 3, 1, 12)) + sub


def extract(source: Path, target: Path, face: int, text_files: list[Path]) -> None:
    data = source.read_bytes()
    if data[:4] != b"ttcf" or len(data) < 16:
        raise ValueError("source must be a TrueType collection")
    count = struct.unpack_from(">I", data, 8)[0]
    if face < 0 or face >= count or 12 + count * 4 > len(data):
        raise ValueError("face index out of range")
    offset = struct.unpack_from(">I", data, 12 + face * 4)[0]
    if offset + 12 > len(data) or data[offset:offset + 4] not in (b"\0\1\0\0", b"true"):
        raise ValueError("face is not a TrueType glyf font")
    tables_count = struct.unpack_from(">H", data, offset + 4)[0]
    if offset + 12 + tables_count * 16 > len(data):
        raise ValueError("truncated table directory")
    tables = {}
    for i in range(tables_count):
        tag, _, start, size = struct.unpack_from(">4sIII", data, offset + 12 + i * 16)
        if start + size > len(data):
            raise ValueError("truncated font table")
        tables[tag] = bytearray(data[start:start + size])
    tags = [b"cmap", b"glyf", b"head", b"hhea", b"hmtx", b"loca", b"maxp", b"name"]
    if any(tag not in tables for tag in tags):
        raise ValueError("face does not contain the required glyf tables")
    if text_files:
        characters = {ord(ch) for path in text_files for ch in path.read_text()}
        subset_tables(tables, characters)
    struct.pack_into(">I", tables[b"head"], 8, 0)
    n = len(tags)
    selector = n.bit_length() - 1
    search_range = 16 << selector
    result = bytearray(struct.pack(">4sHHHH", b"\0\1\0\0", n, search_range, selector, n * 16 - search_range))
    result += b"\0" * (n * 16)
    head_offset = 0
    for i, tag in enumerate(tags):
        while len(result) % 4:
            result.append(0)
        start = len(result)
        table = tables[tag]
        result.extend(table)
        struct.pack_into(">4sIII", result, 12 + i * 16, tag, checksum(table), start, len(table))
        if tag == b"head":
            head_offset = start
    struct.pack_into(">I", result, head_offset + 8, (0xB1B0AFBA - checksum(result)) & 0xFFFFFFFF)
    target.write_bytes(result)
    print(f"Test-only TTF: {target} ({len(result)} bytes)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("collection", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--face", type=int, default=0)
    parser.add_argument("--text-file", type=Path, action="append", default=[],
                        help="Subset this test-only font to characters in the supplied UTF-8 file (repeatable)")
    args = parser.parse_args()
    extract(args.collection, args.output, args.face, args.text_file)
