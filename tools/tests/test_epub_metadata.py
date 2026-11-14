#!/usr/bin/env python3
"""Read real ZIP/container/OPF metadata without opening chapter contents."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

from test_epub import ROOT, CONTAINER


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file(): compiler = shutil.which('clang') or shutil.which('cc')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    with tempfile.TemporaryDirectory(prefix='folio-metadata-') as temp:
        folder = Path(temp); sd = folder / 'sd'; sd.mkdir(); binary = folder / 'metadata'
        subprocess.run([compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DFOLIO_EPUB_HOST_TEST',
                        f'-DFOLIO_SD_ROOT="{sd}"', '-I', str(ROOT / 'main/book'), str(ROOT / 'tools/tests/epub_host_runner.c'),
                        *(str(ROOT / 'main/book' / p) for p in ('folio_epub.c', 'folio_zip.c', 'folio_xml.c')),
                        '-lz', '-o', str(binary)], check=True)
        count = 0

        def make(name, opf, container=CONTAINER, extra=None):
            path = folder / (name + '.epub')
            with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as archive:
                archive.writestr('mimetype', 'application/epub+zip')
                archive.writestr('META-INF/container.xml', container)
                archive.writestr('OPS/book.opf', opf)
                for key, value in (extra or {}).items(): archive.writestr(key, value)
            return path

        def run(path, title='', author='', invalid=False):
            nonlocal count
            result = subprocess.run([str(binary), '--metadata', str(path)], capture_output=True, text=True, timeout=10)
            assert result.returncode == int(invalid), (path.name, result.returncode, result.stderr)
            assert 'Sanitizer' not in result.stderr and 'runtime error' not in result.stderr
            if invalid: assert result.stderr.strip()
            else: assert result.stdout == title + '\n' + author + '\n', (path.name, result.stdout)
            assert not list(sd.iterdir()), 'Metadata must not create text or cover caches'
            count += 1

        def package(text): return '<package><metadata>' + text + '</metadata></package>'
        run(make('unicode', package('<dc:title> 城南旧事 </dc:title><dc:creator> 林海音 </dc:creator>')), '城南旧事', '林海音')
        run(make('entities', package('<title>A &amp; B</title><creator> Jane\n Austen </creator>')), 'A & B', 'Jane Austen')
        run(make('first', package('<title>First</title><title>Second</title><creator>A</creator><creator>B</creator>')), 'First', 'A')
        run(make('missing-author', package('<title>Book</title>')), 'Book')
        run(make('missing-metadata', '<package><manifest/><spine/></package>'))
        run(make('scope', '<package><manifest><title>Ignore</title></manifest><metadata><title>Real</title></metadata></package>'), 'Real')
        run(make('utf8-bound', package('<title>' + '中' * 100 + '</title><creator>' + '文' * 100 + '</creator>')), '中' * 42, '文' * 42)
        run(make('cdata', package('<title><![CDATA[A & B]]></title><creator>&#x6797;</creator>')), 'A & B', '林')
        run(make('badxml', '<package><metadata><title>Bad</metadata></package>'), invalid=True)
        run(make('bad-root', '<html/>'), invalid=True)
        run(make('traversal', package(''), CONTAINER.replace('OPS/book.opf', '../OPS/book.opf')), invalid=True)
        run(make('bad-utf8', b'<package><metadata><title>\xff</title></metadata></package>'), invalid=True)
        # Chapter CRC is deliberately corrupted; metadata never decompresses it.
        chapter = make('unused-chapter', package('<title>Fast</title>'), extra={'OPS/text.xhtml': 'a' * 10000})
        data = bytearray(chapter.read_bytes())
        with zipfile.ZipFile(chapter) as archive:
            entry = archive.getinfo('OPS/text.xhtml')
            start = entry.header_offset + 30 + len(entry.filename.encode()) + len(entry.extra)
        data[start] ^= 1; chapter.write_bytes(data); run(chapter, 'Fast')
        # The OPF being read is always CRC verified.
        bad = make('opf-crc', package('<title>Bad</title>'))
        data = bytearray(bad.read_bytes())
        with zipfile.ZipFile(bad) as archive:
            entry = archive.getinfo('OPS/book.opf')
            start = entry.header_offset + 30 + len(entry.filename.encode()) + len(entry.extra)
        data[start + 3] ^= 1; bad.write_bytes(data); run(bad, invalid=True)
        print(f'EPUB metadata: {count} cases passed (ASan + UBSan)')


if __name__ == '__main__': main()
