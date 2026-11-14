#!/usr/bin/env python3
"""Arrange native PGM UI renders into compact grayscale review sheets."""
from pathlib import Path
import struct
import sys
import zlib


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def thumbnail(path: Path) -> bytes:
    with path.open("rb") as stream:
        assert stream.readline().strip() == b"P5"
        assert stream.readline().strip() == b"684 1216"
        assert stream.readline().strip() == b"255"
        raw = stream.read()
    assert len(raw) == 684 * 1216
    # Area averaging keeps thin native text and rules visible at half scale.
    out = bytearray(342 * 608)
    for y in range(608):
        for x in range(342):
            pos = y * 2 * 684 + x * 2
            out[y * 342 + x] = (raw[pos] + raw[pos + 1] + raw[pos + 684] + raw[pos + 685] + 2) // 4
    return bytes(out)


def write_sheet(directory: Path, name: str, pages: tuple[str, ...]) -> None:
    inputs = [directory / (page + ".pgm") for page in pages]
    if not all(path.is_file() for path in inputs):
        return
    gutter, cell_w, cell_h, columns = 18, 342, 608, min(3, len(inputs))
    rows = (len(inputs) + columns - 1) // columns
    width = columns * cell_w + (columns + 1) * gutter
    height = rows * cell_h + (rows + 1) * gutter
    pixels = bytearray([230]) * (width * height)
    for i, path in enumerate(inputs):
        thumb = thumbnail(path)
        left = gutter + (i % columns) * (cell_w + gutter)
        top = gutter + (i // columns) * (cell_h + gutter)
        for row in range(cell_h):
            offset = (top + row) * width + left
            pixels[offset:offset + cell_w] = thumb[row * cell_w:(row + 1) * cell_w]
    png_rows = b"".join(b"\0" + pixels[y * width:(y + 1) * width] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(png_rows, 9)) + chunk(b"IEND", b""))
    output = directory / (name + ".png")
    output.write_bytes(png)
    print(f"UI review sheet: {output}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: preview_contact_sheet.py preview_directory")
    directory = Path(sys.argv[1])
    write_sheet(directory, "weread-overview", (
        "56-weread-network-setup", "57-weread-qr-login", "59-weread-cloud-shelf",
        "61-weread-chapters", "63-weread-sync", "69-weread-offline-reader"))
    write_sheet(directory, "weread-confirmations", (
        "58-weread-otp", "64-weread-time-enable-confirm", "66-weread-time-upload-confirm",
        "67-weread-position-upload-confirm", "60-weread-logout-confirm", "65-weread-time-enabled"))
    write_sheet(directory, "weread-states", (
        "62-weread-downloading", "68-weread-closing", "70-weread-error", "71-weread-connecting"))
    write_sheet(directory, "rounded-overview", ("28-home-heatmap", "01-library-grid", "13-system"))
    write_sheet(directory, "home-comparison", ("28-home-heatmap", "30-home-bars"))
    write_sheet(directory, "calendar-sheet", ("40-heatmap-monday", "41-heatmap-sunday", "42-heatmap-next-month"))
    write_sheet(directory, "home-sheet", (
        "28-home-heatmap", "30-home-bars", "29-home-chart-setting",
        "27-home-empty", "31-home-unknown-time", "34-home-long-title"))
    write_sheet(directory, "home-states-sheet", (
        "32-home-save-warning", "33-home-load-warning", "36-home-history-unavailable",
        "35-home-missing-book", "31-home-unknown-time", "34-home-long-title"))
    write_sheet(directory, "home-recovery-sheet", (
        "38-home-migrated-session", "37-home-lock-save-failure", "33-home-load-warning"))
    write_sheet(directory, "cover-sheet", (
        "28-home-heatmap", "01-library-grid", "02-library-list",
        "39-home-without-cover", "35-home-missing-book", "34-home-long-title"))
    write_sheet(directory, "contact-sheet", (
        "01-library-grid", "03-reading", "04-reading-menu",
        "09-typography", "10-preferences", "18-lockscreen"))
    write_sheet(directory, "details-sheet", (
        "02-library-list", "07-contents", "05-bookmarks",
        "11-fonts", "08-progress", "13-system"))
    write_sheet(directory, "states-sheet", (
        "06-bookmark-confirm", "14-shutdown-confirm", "15-error",
        "16-no-font", "19-large-type", "20-compact-type"))
    write_sheet(directory, "edge-cases-sheet", (
        "22-library-current", "23-library-long-title", "24-library-empty",
        "25-bookmarks-empty", "26-fonts-empty", "21-typography-large"))
