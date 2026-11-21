#!/usr/bin/env python3
"""Exercise real EPUB, font and reader modules together under ASan/UBSan."""
from __future__ import annotations
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PARAGRAPH = '中文阅读ABC123，页间排版。测试内容完整保留！'


def fixture_font() -> bytes:
    """Tiny original box-glyph font: ASCII half em and Chinese full em.

    Glyph appearance is deliberately synthetic; real cmap/glyf/hmtx loading and
    rasterization run unchanged, with no bundled third-party font license.
    """
    be16 = lambda value: struct.pack('>H', value & 65535)
    head = bytearray(54)
    head[18:20] = be16(1000)
    head[50:52] = be16(1)
    hhea = bytearray(36)
    hhea[4:8] = be16(800) + be16(-200)
    hhea[34:36] = be16(3)
    def rectangle(width: int) -> bytes:
        return (be16(1) + be16(0) + be16(0) + be16(width) + be16(800) + be16(3) + be16(0)
                + bytes([1, 1, 1, 1])
                + b''.join(be16(value) for value in [0, width, 0, -width, 0, 0, 800, 0]))
    codes = sorted(set(range(32, 127)) | {ord(c) for c in PARAGRAPH + '章节一二三标题'}) + [65535]
    n = len(codes)
    power = 1 << (n.bit_length() - 1)
    cmap4 = (be16(4) + be16(16 + 8 * n) + be16(0) + be16(2 * n)
             + be16(2 * power) + be16(power.bit_length() - 1) + be16(2 * n - 2 * power)
             + b''.join(be16(cp) for cp in codes) + be16(0)
             + b''.join(be16(cp) for cp in codes)
             + b''.join(be16((0 if cp == 65535 else 1 if cp < 128 else 2) - cp) for cp in codes)
             + b'\x00\x00' * n)
    tables = {
        'head': bytes(head), 'maxp': struct.pack('>IH', 0x00010000, 3), 'hhea': bytes(hhea),
        'hmtx': struct.pack('>HHHHHH', 500, 0, 500, 0, 1000, 0),
        'loca': struct.pack('>IIII', 0, 0, 34, 68), 'glyf': rectangle(450) + rectangle(900),
        'cmap': struct.pack('>HHHHI', 0, 1, 3, 1, 12) + cmap4,
    }
    offset = 12 + 16 * len(tables)
    directory = bytearray(struct.pack('>IHHHH', 0x00010000, len(tables), 0, 0, 0))
    payload = bytearray()
    for tag, content in tables.items():
        pad = (-offset) % 4
        payload.extend(b'\x00' * pad); offset += pad
        directory.extend(tag.encode() + struct.pack('>III', 0, offset, len(content)))
        payload.extend(content); offset += len(content)
    return bytes(directory + payload)


def main() -> None:
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file(): compiler = shutil.which('clang') or shutil.which('cc') or ''
    assert compiler, 'A C11 compiler is required'
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    platform_args = ['-isysroot', str(sdk)] if sdk.is_dir() else []
    with tempfile.TemporaryDirectory(prefix='folio-real-epub-') as temp:
        folder = Path(temp).resolve()
        sd = folder / 'card'; sd.mkdir()
        (sd / 'books').mkdir()
        font_path = folder / 'fixture.ttf'; font_path.write_bytes(fixture_font())
        epub = sd / 'books' / 'integration.epub'
        with zipfile.ZipFile(epub, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
            z.writestr('META-INF/container.xml', '<container><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
            manifest = ''.join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(3))
            spine = ''.join(f'<itemref idref="c{i}"/>' for i in range(3))
            z.writestr('book.opf', '<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>真实集成</dc:title></metadata><manifest>' + manifest + '</manifest><spine>' + spine + '</spine></package>')
            for i in range(3):
                z.writestr(f'c{i}.xhtml', f'<html><body><h1>章节{i + 1}</h1>' + f'<p>{PARAGRAPH * 5}</p>' * 100 + '</body></html>')
        subprocess.run(['sh', str(ROOT/'tools/tests/build_pdf_host.sh'), str(folder/'pdf'), compiler], check=True)
        binary = folder / 'integration'
        subprocess.run([
            compiler, *platform_args, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DFOLIO_EPUB_HOST_TEST',
            f'-DFOLIO_SD_ROOT="{sd}"', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(ROOT / 'tools/tests/epub_integration_stubs'),
            '-I', str(ROOT / 'tools/tests/reader_stubs'), '-I', str(ROOT / 'main/book'), '-I', str(ROOT / 'main'),
            str(ROOT / 'tools/tests/test_epub_integration.c'),
            *(str(ROOT / 'main/book' / name) for name in ('folio_font.c', 'folio_text.c', 'folio_gbk_table.c', 'folio_epub.c', 'folio_zip.c', 'folio_xml.c', 'folio_weread_cache.c')),
            str(ROOT / 'main/folio_storage.c'),
            str(folder/'pdf/libfolio_pdf.a'), '-pthread', '-lz', '-lm', '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary), str(epub), str(font_path)], check=True, timeout=60)


if __name__ == '__main__':
    main()
