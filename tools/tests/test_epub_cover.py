#!/usr/bin/env python3
"""Real PNG/JPEG cover extraction without text conversion; ASan + UBSan."""
from __future__ import annotations

import base64
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import zipfile
import zlib

from test_epub import ROOT, CONTAINER


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))


PNG = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0))
       + png_chunk(b'IDAT', zlib.compress(b'\x00\x30\x50\x80\xF0\x80\x40\x00\xF0\x80\x40\x30\x50\x80'))
       + png_chunk(b'IEND', b''))
# Original 2x2 test pattern, encoded once with macOS ImageIO; no external image/license.
JPEG = base64.b64decode('''
/9j/4AAQSkZJRgABAQAASABIAAD/4QBMRXhpZgAATU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAAAqADAAQAAAABAAAAAgAAAAD/7QA4UGhvdG9zaG9wIDMuMAA4QklNBAQAAAAAAAA4QklNBCUAAAAAABDUHYzZjwCyBOmACZjs+EJ+/8AAEQgAAgACAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/bAEMAAgICAgICAwICAwUDAwMFBgUFBQUGCAYGBgYGCAoICAgICAgKCgoKCgoKCgwMDAwMDA4ODg4ODw8PDw8PDw8PD//bAEMBAgICBAQEBwQEBxALCQsQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEP/dAAQAAf/aAAwDAQACEQMRAD8A+c/FPinxNpnibV9N03V7y0tLS8uIoYYriSOOOOORlVEVWAVVAAAAwBwKwv8AhNvGf/Qf1D/wKl/+Ko8bf8jnr/8A2ELr/wBGtXMV8mfqh//Z
''')


def item(identifier: str, href: str, media_type: str, properties: str = '') -> str:
    return f'<item id="{identifier}" href="{href}" media-type="{media_type}" properties="{properties}"/>'


def package(items: str = '', metadata: str = '', guide: str = '') -> str:
    return f'<package><metadata>{metadata}</metadata><manifest>{items}</manifest>{guide}<spine/></package>'


def main() -> None:
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file():
        compiler = shutil.which('clang') or shutil.which('cc') or ''
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    platform_args = ['-isysroot', str(sdk)] if sdk.is_dir() else []
    with tempfile.TemporaryDirectory(prefix='folio-epub-cover-') as temp:
        folder = Path(temp)
        sd = folder / 'sd'; sd.mkdir()
        binary = folder / 'epub-reader'
        subprocess.run([
            compiler, *platform_args, '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
            '-fsanitize=address,undefined', '-DFOLIO_EPUB_HOST_TEST', f'-DFOLIO_SD_ROOT="{sd}"',
            '-I', str(ROOT / 'main/book'), str(ROOT / 'tools/tests/epub_host_runner.c'),
            *(str(ROOT / 'main/book' / name) for name in ('folio_epub.c', 'folio_zip.c', 'folio_xml.c')),
            '-lz', '-o', str(binary),
        ], check=True)
        count = 0
        zip_count = 0

        def book(name: str, opf: str, resources: dict[str, str | bytes] | None = None, *, stored: bool = False) -> Path:
            path = folder / (name + '.epub')
            with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
                archive.writestr('META-INF/container.xml', CONTAINER)
                archive.writestr('OPS/book.opf', opf)
                for key, value in (resources or {}).items(): archive.writestr(key, value)
            return path

        def run(path: Path, expected: bytes | None = None, *, error: bool = False) -> None:
            nonlocal count
            output = folder / f'image-{count}.bin'
            result = subprocess.run([str(binary), '--cover', str(path), str(output)], capture_output=True, text=True, timeout=30)
            assert 'Sanitizer' not in result.stderr and 'runtime error' not in result.stderr, result.stderr
            status = 1 if error else 4 if expected is None else 0
            assert result.returncode == status, (path.name, result.returncode, result.stderr)
            assert bool(result.stderr.strip()) == error, (path.name, result.stderr)
            if expected is not None: assert output.read_bytes() == expected, path.name
            else: assert not output.exists()
            assert not list(sd.iterdir()), 'Cover discovery must not create/convert text caches'
            count += 1

        def check_zip_modes(path: Path, expected: bool = True) -> None:
            nonlocal zip_count
            messages = []
            for mode in ('--zip-full', '--zip-directory'):
                result = subprocess.run([str(binary), mode, str(path)], capture_output=True, text=True, timeout=30)
                assert result.returncode == (0 if expected else 1), (path.name, mode, result.stderr)
                assert 'Sanitizer' not in result.stderr and 'runtime error' not in result.stderr, result.stderr
                if expected:
                    crc, size = result.stdout.split()
                    assert int(size) == path.stat().st_size
                    assert int(crc, 16) == (0 if mode == '--zip-directory' else zlib.crc32(path.read_bytes()))
                else:
                    assert result.stderr.strip()
                    messages.append(result.stderr)
                zip_count += 1
            if not expected: assert messages[0] == messages[1], 'Both opens must share all structural safety checks'

        png_item = item('png', 'Images/cover.png', 'image/png', 'cover-image')
        jpeg_item = item('jpg', 'Images/cover.jpg', 'image/jpeg')
        png_resources = {'OPS/Images/cover.png': PNG}
        both = {**png_resources, 'OPS/Images/cover.jpg': JPEG}
        run(book('epub3-png', package(png_item), png_resources), PNG)
        run(book('epub3-stored', package(png_item), png_resources, stored=True), PNG)
        run(book('epub3-jpeg', package(jpeg_item.replace('properties=""', 'properties="cover-image"')), both), JPEG)
        legacy = '<meta name="cover" content="jpg"/>'
        guide_png = '<guide><reference type="cover" href="Images/cover.png"/></guide>'
        run(book('epub2-meta', package(jpeg_item, legacy), both), JPEG)
        run(book('priority-epub3', package(png_item + jpeg_item, legacy, guide_png), both), PNG)
        run(book('priority-epub2', package(jpeg_item, legacy, guide_png), both), JPEG)
        run(book('guide-image', package(guide=guide_png), png_resources), PNG)
        run(book('guide-relative-percent', package(guide=guide_png.replace('Images/cover.png', 'Text/../Images/cov%65r.png')), png_resources), PNG)
        html_item = item('page', 'Text/cover.xhtml', 'application/xhtml+xml')
        guide_html = '<guide><reference type="cover" href="Text/cover.xhtml"/></guide>'
        html = '<html><body><img src="../Images/cover.jpg"/></body></html>'
        run(book('guide-xhtml', package(html_item, guide=guide_html), {**both, 'OPS/Text/cover.xhtml': html}), JPEG)
        run(book('guide-xhtml-no-manifest', package(guide=guide_html), {**both, 'OPS/Text/cover.xhtml': html}), JPEG)
        svg = '<svg xmlns:xlink="http://www.w3.org/1999/xlink"><image xlink:href="../Images/cover.png"/></svg>'
        run(book('guide-inline-svg', package(html_item, guide=guide_html), {**both, 'OPS/Text/cover.xhtml': f'<html><body>{svg}</body></html>'}), PNG)
        svg_item = item('svg', 'Text/cover.svg', 'image/svg+xml', 'cover-image')
        run(book('epub3-svg-image', package(svg_item), {**both, 'OPS/Text/cover.svg': svg}), PNG)
        run(book('svg2-href', package(svg_item), {**both, 'OPS/Text/cover.svg': svg.replace('xlink:href', 'href')}), PNG)
        fragment_html = '<html><body><img src="../Images/cover.png"/><div id="front"><img src="../Images/cover.jpg"/></div></body></html>'
        run(book('guide-fragment', package(html_item, guide=guide_html.replace('.xhtml"', '.xhtml#front"')), {**both, 'OPS/Text/cover.xhtml': fragment_html}), JPEG)
        # Malformed/oversized body and empty/unsupported spine do not prevent cover discovery.
        run(book('no-body-conversion', package(png_item + item('text', 'body.xhtml', 'application/xhtml+xml'),
                                             '<meta property="rendition:layout">pre-paginated</meta>'),
                 {**png_resources, 'OPS/body.xhtml': '<invalid>' + 'x' * (600 * 1024)}), PNG)
        run(book('no-cover', package(item('png', 'Images/cover.png', 'image/png')), png_resources))
        run(book('no-cover-metadata', package()))
        run(book('property-is-token', package(png_item.replace('cover-image', 'not-cover-image')), png_resources))
        padding_size = 2 * 1024 * 1024 - len(PNG) - 12
        exact_limit = PNG[:-12] + png_chunk(b'vpAg', b'\0' * padding_size) + PNG[-12:]
        assert len(exact_limit) == 2 * 1024 * 1024
        run(book('exact-image-limit', package(png_item), {'OPS/Images/cover.png': exact_limit}), exact_limit)
        random_padding = random.Random(42).randbytes(padding_size)
        compressed_over_limit = PNG[:-12] + png_chunk(b'vpAg', random_padding) + PNG[-12:]
        compressed_path = book('compressed-image-limit', package(png_item), {'OPS/Images/cover.png': compressed_over_limit})
        with zipfile.ZipFile(compressed_path) as archive:
            assert archive.getinfo('OPS/Images/cover.png').compress_size > 2 * 1024 * 1024
        run(compressed_path, error=True)

        corrupt_png = bytearray(PNG); corrupt_png[45] ^= 1
        for name, opf, resources in [
            ('missing-cover', package(png_item), {}),
            ('missing-cover-id', package(jpeg_item, legacy.replace('jpg', 'missing')), both),
            ('duplicate-cover', package(png_item + jpeg_item.replace('properties=""', 'properties="cover-image"')), both),
            ('duplicate-id', package(png_item + png_item), both),
            ('unsupported-gif', package(png_item.replace('image/png', 'image/gif')), {'OPS/Images/cover.png': b'GIF89a'}),
            ('wrong-media-type', package(png_item), {'OPS/Images/cover.png': JPEG}),
            ('png-bad-crc', package(png_item), {'OPS/Images/cover.png': bytes(corrupt_png)}),
            ('png-truncated', package(png_item), {'OPS/Images/cover.png': PNG[:-1]}),
            ('png-header-only', package(png_item), {'OPS/Images/cover.png': PNG[:33]}),
            ('jpeg-truncated', package(jpeg_item, legacy), {'OPS/Images/cover.jpg': JPEG[:-2]}),
            ('jpeg-invalid-segment', package(jpeg_item, legacy), {'OPS/Images/cover.jpg': JPEG[:4] + b'\xff\xff' + JPEG[6:]}),
            ('oversize-image', package(png_item), {'OPS/Images/cover.png': PNG + b'\0' * (2 * 1024 * 1024)}),
            ('remote-manifest', package(png_item.replace('Images/cover.png', 'https://example.org/c.png')), both),
            ('traversal-manifest', package(png_item.replace('Images/cover.png', '../../c.png')), both),
            ('encoded-traversal', package(png_item.replace('Images/cover.png', '%2e%2e/%2e%2e/c.png')), both),
            ('remote-guide', package(guide=guide_png.replace('Images/cover.png', '//example.org/c.png')), both),
            ('traversal-guide', package(guide=guide_png.replace('Images/cover.png', '../../c.png')), both),
            ('remote-img', package(html_item, guide=guide_html), {'OPS/Text/cover.xhtml': html.replace('../Images/cover.jpg', 'https://example.org/c.jpg')}),
            ('data-img', package(html_item, guide=guide_html), {'OPS/Text/cover.xhtml': html.replace('../Images/cover.jpg', 'data:image/jpeg;base64,AAA=')}),
            ('traversal-img', package(html_item, guide=guide_html), {'OPS/Text/cover.xhtml': html.replace('../Images/cover.jpg', '../../../c.jpg')}),
            ('broken-xml', package(html_item, guide=guide_html), {'OPS/Text/cover.xhtml': '<html><body></html>'}),
            ('empty-svg', package(svg_item), {'OPS/Text/cover.svg': '<svg><path d="M0 0"/></svg>'}),
            ('nested-cover-document', package(svg_item), {'OPS/Text/cover.svg': '<svg><image href="next.svg"/></svg>', 'OPS/Text/next.svg': svg}),
            ('base-url', package(html_item, guide=guide_html), {**both, 'OPS/Text/cover.xhtml': html.replace('<body>', '<body xml:base="../">')}),
            ('encrypted', package(png_item), {**png_resources, 'META-INF/encryption.xml': '<encryption/>'}),
        ]:
            run(book(name, opf, resources), error=True)

        crc_book = book('zip-crc', package(png_item), png_resources, stored=True)
        corrupted = bytearray(crc_book.read_bytes())
        offset = corrupted.index(PNG); corrupted[offset + 45] ^= 1
        crc_book.write_bytes(corrupted)
        run(crc_book, error=True)
        # Entry CRC validation still happens on extraction, independent of the
        # archive-wide fingerprint. Directory open must not manufacture one.
        check_zip_modes(crc_book)
        unread = b'A' * (1024 * 1024)
        fast_book = book('unread-body', package(png_item), {**png_resources, 'OPS/unread.bin': unread}, stored=True)
        run(fast_book, PNG)
        check_zip_modes(fast_book)
        before = fast_book.read_bytes()
        changed = bytearray(before)
        payload = changed.index(unread)
        changed[payload + len(unread) // 2] ^= 1
        fast_book.write_bytes(changed)
        assert zlib.crc32(before) != zlib.crc32(changed)
        run(fast_book, PNG)  # Unread body CRC damage cannot block an intact cover.
        check_zip_modes(fast_book)

        for name in ('../escape', '/escape', 'OPS\\escape'):
            check_zip_modes(book('unsafe-' + str(zip_count), package(png_item), {**png_resources, name: b'x'}), False)
        original = book('zip-structure', package(png_item), png_resources, stored=True).read_bytes()
        central = original.index(b'PK\x01\x02')
        def structural_mutation(name: str, damaged: bytes) -> None:
            path = folder / (name + '.epub'); path.write_bytes(damaged)
            check_zip_modes(path, False)
            run(path, error=True)
        modified = bytearray(original); modified[30] ^= 1
        structural_mutation('local-name-mismatch', modified)
        modified = bytearray(original); modified[central + 16] ^= 1
        structural_mutation('header-crc-mismatch', modified)
        modified = bytearray(original); modified[central + 20:central + 24] = struct.pack('<I', 0xFFFFFFFF)
        structural_mutation('zip64-size', modified)
        modified = bytearray(original); modified[6:8] = struct.pack('<H', 1); modified[central + 8:central + 10] = struct.pack('<H', 1)
        structural_mutation('encrypted-member', modified)
        modified = bytearray(original); modified[8:10] = struct.pack('<H', 12); modified[central + 10:central + 12] = struct.pack('<H', 12)
        structural_mutation('unsupported-method', modified)
        modified = bytearray(original); modified[central + 38:central + 42] = struct.pack('<I', 0xA000 << 16)
        structural_mutation('symlink', modified)
        modified = bytearray(original)
        # First stored member extends one byte into the next local header.
        for offset in (18, 22, central + 20, central + 24): modified[offset:offset + 4] = struct.pack('<I', 21)
        structural_mutation('overlapping-members', modified)
        for missing in (1, 21, 80, len(original) // 2): structural_mutation('truncated-' + str(missing), original[:-missing])
        print(f'EPUB cover regressions passed: {count} fixtures, {zip_count // 2} ZIP validation pairs '
              '(ASan + UBSan; real PNG/JPEG; no full fingerprint or text cache)')


if __name__ == '__main__':
    main()
