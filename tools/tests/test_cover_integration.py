#!/usr/bin/env python3
"""Small end-to-end test: actual EPUB extraction and actual image decoders."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'tools/tests/fixtures'
CONTAINER = '<container><rootfiles><rootfile full-path="OPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'


def package(manifest='', metadata='', guide=''):
    return (f'<package><metadata>{metadata}</metadata><manifest>{manifest}</manifest>'
            f'<spine/>{guide}</package>')


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file():
        compiler = shutil.which('clang') or shutil.which('cc') or ''
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    platform_args = ['-isysroot', str(sdk)] if sdk.is_dir() else []
    with tempfile.TemporaryDirectory(prefix='folio-cover-integration-') as temp:
        folder = Path(temp)
        sd = folder / 'sd'; sd.mkdir()
        binary = folder / 'cover-integration'
        subprocess.run([
            compiler, *platform_args, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-g', '-O1',
            '-Wall', '-Wextra', '-Werror', '-fno-omit-frame-pointer',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-DFOLIO_EPUB_HOST_TEST', f'-DFOLIO_SD_ROOT="{sd}"',
            '-I', str(ROOT / 'tools/tests/cover_stubs'), '-I', str(ROOT / 'main/book'),
            str(ROOT / 'tools/tests/cover_integration_runner.c'),
            *(str(ROOT / 'main/book' / name) for name in (
                'folio_cover.c', 'folio_epub.c', 'folio_zip.c', 'folio_xml.c', 'folio_text.c', 'folio_gbk_table.c')),
            '-lz', '-lm', '-o', str(binary),
        ], check=True)

        def book(name, opf, resources=None):
            path = folder / (name + '.epub')
            with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
                archive.writestr('META-INF/container.xml', CONTAINER)
                archive.writestr('OPS/book.opf', opf)
                for key, value in (resources or {}).items(): archive.writestr(key, value)
            return path

        png_item = '<item id="cover" href="Images/light.png" media-type="image/png" properties="cover-image"/>'
        png = (FIXTURES / 'ui-cover-light.png').read_bytes()
        paths = [book('epub3-png', package(png_item), {'OPS/Images/light.png': png})]
        paths.append(book('epub2-jpeg', package(
            '<item id="old-cover" href="Images/letter.jpg" media-type="image/jpeg"/>',
            '<meta name="cover" content="old-cover"/>'),
            {'OPS/Images/letter.jpg': (FIXTURES / 'ui-cover-letter.jpg').read_bytes()}))
        paths.append(book('guide-svg', package(
            '<item id="page" href="Text/cover.xhtml" media-type="application/xhtml+xml"/>',
            guide='<guide><reference type="cover" href="Text/cover.xhtml"/></guide>'), {
                'OPS/Text/cover.xhtml': '<html><body><svg xmlns:xlink="http://www.w3.org/1999/xlink"><image xlink:href="../Images/mountain.png"/></svg></body></html>',
                'OPS/Images/mountain.png': (FIXTURES / 'ui-cover-mountain.png').read_bytes(),
            }))
        corrupted = bytearray(png); corrupted[-1] ^= 1
        paths.append(book('corrupt-cover', package(png_item), {'OPS/Images/light.png': corrupted}))
        paths.append(book('missing-cover', package()))
        subprocess.run([str(binary), *(str(path) for path in paths)], check=True, timeout=60)
        assert not list(sd.iterdir()), 'Cover drawing must not convert the EPUB text or write a cache to the card'


if __name__ == '__main__':
    main()
