#!/usr/bin/env python3
"""Cancellation at ZIP, XML and text-conversion stages must clean up."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from test_epub import ROOT, CONTAINER


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file(): compiler = shutil.which('clang') or shutil.which('cc')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    with tempfile.TemporaryDirectory(prefix='folio-epub-cancel-') as temporary:
        temp = Path(temporary); sd = temp / 'sd'; sd.mkdir(); binary = temp / 'cancel-tests'
        subprocess.run([compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DFOLIO_EPUB_HOST_TEST',
                        f'-DFOLIO_SD_ROOT="{sd}"', '-I', str(ROOT / 'main/book'), str(ROOT / 'tools/tests/epub_host_runner.c'),
                        *(str(ROOT / 'main/book' / p) for p in ('folio_epub.c', 'folio_zip.c', 'folio_xml.c')),
                        '-lz', '-o', str(binary)], check=True)
        for compressed in (False, True):
            book = temp / f'{compressed}.epub'
            with zipfile.ZipFile(book, 'w', compression=zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED) as archive:
                archive.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
                archive.writestr('META-INF/container.xml', CONTAINER)
                archive.writestr('OPS/book.opf', '<package><metadata><title>Cancel test</title></metadata><manifest>'
                                 '<item id="text" href="text.xhtml" media-type="application/xhtml+xml"/>'
                                 '</manifest><spine><itemref idref="text"/></spine></package>')
                archive.writestr('OPS/text.xhtml', '<html><body>' + '<p>中文 words 123.</p>' * 15000 + '</body></html>')
            for stage in (0, 5, 25, 100, 500, 2500):
                result = subprocess.run([str(binary), str(book)], env={**os.environ, 'FOLIO_CANCEL_AFTER': str(stage)},
                                        capture_output=True, text=True, timeout=5)
                assert result.returncode == 1 and 'cancelled' in result.stderr, (compressed, stage, result.returncode, result.stderr)
                assert 'Sanitizer' not in result.stderr and 'runtime error' not in result.stderr
                assert not [p for p in sd.rglob('*') if p.is_file()], 'Cancelled conversion left a partial cache'
            result = subprocess.run([str(binary), str(book)], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, result.stderr
            for cached in sd.rglob('*'):
                if cached.is_file(): cached.unlink()
        print('EPUB cancellation: 12 stage injections + 2 complete conversions passed (ASan + UBSan)')


if __name__ == '__main__': main()
