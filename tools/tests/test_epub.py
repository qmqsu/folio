#!/usr/bin/env python3
"""Host regression tests for the bounded EPUB parser (system zlib inflater).

Run with Python 3. The target build exercises the ESP32-S3 ROM inflater instead.
Creates fixtures in a temporary SD root, leaves the project and book originals intact.
"""
from __future__ import annotations

import os
import io
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
CONTAINER = '<container><rootfiles><rootfile full-path="OPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
OPF = '''<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>页间 &amp; 测试</dc:title></metadata>
<manifest><item id="one" href="Text/one.xhtml" media-type="application/xhtml+xml"/>
<item id="two" href="Text/two.xhtml" media-type="application/xhtml+xml"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="one"/><itemref idref="two"/></spine></package>'''
NAV = '''<html xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="landmarks"><a href="Text/two.xhtml">忽略</a></nav>
<nav epub:type="toc"><ol><li><a href="Text/one.xhtml">第一章</a></li><li><a href="Text/one.xhtml#inside">片段</a></li>
<li><a href="Text/two.xhtml#end">第二章</a></li></ol></nav></body></html>'''
BODY1 = '''<?xml version="1.0" encoding="UTF-8"?><html><head><title>不要显示</title><style>坏样式</style></head>
<body><h1>首章</h1><p>你好，<em>世界</em>！ &amp; &#x4E66; &#20013;</p><p id="inside">片段正文</p>
<script>坏脚本</script><p hidden="hidden">隐藏正文</p><img alt="封面说明" src="cover.jpg"/></body></html>'''
BODY2 = '<html><body><h1>末章</h1><p id="end">完结。</p></body></html>'


def main() -> None:
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file():
        compiler = shutil.which('clang') or shutil.which('cc') or ''
    if not compiler:
        raise SystemExit('A C11 host compiler and zlib are required')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    platform_args = ['-isysroot', str(sdk)] if sdk.is_dir() else []
    with tempfile.TemporaryDirectory(prefix='folio-epub-tests-') as temp:
        folder = Path(temp)
        sd = folder / 'sd'
        sd.mkdir()
        binary = folder / 'epub-reader'
        subprocess.run([
            compiler, *platform_args, '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
            '-fsanitize=address,undefined', '-DFOLIO_EPUB_HOST_TEST',
            f'-DFOLIO_SD_ROOT="{sd}"', '-I', str(ROOT / 'main/book'),
            str(ROOT / 'tools/tests/epub_host_runner.c'),
            *(str(ROOT / 'main/book' / name) for name in ('folio_epub.c', 'folio_zip.c', 'folio_xml.c')),
            '-lz', '-o', str(binary),
        ], check=True)
        count = 0

        def write_book(name: str, replacements: dict[str, str | bytes] | None = None, *, compression: int = zipfile.ZIP_DEFLATED) -> Path:
            entries: dict[str, str | bytes] = {
                'META-INF/container.xml': CONTAINER, 'OPS/book.opf': OPF,
                'OPS/nav.xhtml': NAV, 'OPS/Text/one.xhtml': BODY1, 'OPS/Text/two.xhtml': BODY2,
            }
            entries.update(replacements or {})
            path = folder / name
            with zipfile.ZipFile(path, 'w', compression=compression) as archive:
                archive.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
                for key, value in entries.items():
                    archive.writestr(key, value)
            return path

        def run(path: Path, expected: bool = True) -> tuple[Path | None, str, list[tuple[int, str]]]:
            nonlocal count
            outcome = subprocess.run([str(binary), str(path)], capture_output=True, text=True, timeout=30)
            assert outcome.returncode in (0, 1), (path.name, outcome.returncode, outcome.stderr)
            assert (outcome.returncode == 0) == expected, (path.name, outcome.stdout, outcome.stderr)
            assert 'Sanitizer' not in outcome.stderr and 'runtime error' not in outcome.stderr, outcome.stderr
            count += 1
            if not expected:
                assert outcome.stderr.strip(), 'Failure should report a visible reason'
                return None, '', []
            cache = None
            toc: list[tuple[int, str]] = []
            title = ''
            for line in outcome.stdout.splitlines():
                parts = line.split('\t')
                if parts[0] == 'CACHE': cache = Path(parts[1])
                elif parts[0] == 'TITLE': title = parts[1]
                elif parts[0] == 'TOC': toc.append((int(parts[1]), parts[2]))
            assert cache is not None and cache.is_file()
            text = cache.read_text()
            assert toc == sorted(toc, key=lambda value: value[0])
            assert all(0 <= offset < cache.stat().st_size for offset, _ in toc)
            assert all(cache.read_bytes()[offset:].decode('utf-8') for offset, _ in toc)
            return cache, title, toc

        good = write_book('basic.epub')
        cache, title, toc = run(good)
        assert cache is not None
        content = cache.read_text()
        assert title == '页间 & 测试'
        assert '你好，世界！ & 书 中' in content
        assert all(hidden not in content for hidden in ('不要显示', '坏样式', '坏脚本', '隐藏正文'))
        assert '[图片] 封面说明' in content
        assert [label for _, label in toc] == ['第一章', '片段', '第二章']
        assert cache.read_bytes()[toc[1][0]:].startswith('片段正文'.encode())
        assert cache.read_bytes()[toc[2][0]:].startswith('完结。'.encode())
        cache2, _, _ = run(good)
        assert cache2 == cache
        cache.write_text('damaged cache')
        repaired, _, _ = run(good)
        assert repaired is not None and repaired.read_text() == content
        good = write_book('basic.epub', {'OPS/Text/two.xhtml': BODY2.replace('完结。', '新的结尾。')})
        changed, _, _ = run(good)
        assert changed != cache
        run(write_book('stored.epub', compression=zipfile.ZIP_STORED))
        legacy = folder / 'legacy-585.epub'
        with zipfile.ZipFile(legacy, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('mimetype', 'application/epub+zip\r\n', compress_type=zipfile.ZIP_STORED)
            archive.writestr('META-INF/container.xml', CONTAINER)
            items, spine = [], []
            for i in range(585):
                items.append(f'<item id="c{i}" href="{i}.xhtml" media-type="application/xhtml+xml"/>')
                spine.append(f'<itemref idref="c{i}"/>')
                archive.writestr(f'OPS/{i}.xhtml', f'<html><body><h1>Chapter {i+1}</h1></body></html>')
            archive.writestr('OPS/book.opf', '<package><manifest>' + ''.join(items) +
                             '</manifest><spine>' + ''.join(spine) + '</spine></package>')
        with zipfile.ZipFile(legacy) as archive:
            central = archive.start_dir
        raw = bytearray(legacy.read_bytes())
        raw[6] |= 16
        raw[central + 8] |= 16
        legacy.write_bytes(raw)
        _, _, chapters = run(legacy)
        assert len(chapters) == 585 and chapters[-1][1] == 'Chapter 585'

        class NonSeekable(io.BytesIO):
            def seek(self, *_args: object) -> int:
                raise io.UnsupportedOperation('stream')
        stream = NonSeekable()
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
            for key, value in {'META-INF/container.xml': CONTAINER, 'OPS/book.opf': OPF,
                               'OPS/nav.xhtml': NAV, 'OPS/Text/one.xhtml': BODY1, 'OPS/Text/two.xhtml': BODY2}.items():
                archive.writestr(key, value)
        descriptor_book = folder / 'descriptor.epub'
        descriptor_book.write_bytes(stream.getvalue())
        run(descriptor_book)

        ncx_opf = OPF.replace('href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"', 'href="toc.ncx" media-type="application/x-dtbncx+xml"').replace('<spine>', '<spine toc="nav">')
        ncx = '''<ncx><navMap><navPoint><navLabel><text>主章节</text></navLabel><content src="Text/one.xhtml"/>
        <navPoint><navLabel><text>内章节</text></navLabel><content src="Text/one.xhtml#inside"/></navPoint></navPoint>
        <navPoint><navLabel><text>后章节</text></navLabel><content src="Text/two.xhtml"/></navPoint></navMap></ncx>'''
        _, _, ncx_toc = run(write_book('ncx.epub', {'OPS/book.opf': ncx_opf, 'OPS/toc.ncx': ncx}))
        assert [label for _, label in ncx_toc] == ['主章节', '内章节', '后章节']
        no_nav = OPF.replace('<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>', '')
        _, _, fallback = run(write_book('fallback.epub', {'OPS/book.opf': no_nav}))
        assert [label for _, label in fallback] == ['首章', '末章']
        percent_opf = OPF.replace('href="Text/two.xhtml"', 'href="Text/../Text/tw%6f.xhtml"')
        run(write_book('relative.epub', {'OPS/book.opf': percent_opf}))
        randomizer = random.Random(42)
        long_text = ''.join(chr(0x4E00 + randomizer.randrange(4000)) for _ in range(50000))
        large, _, _ = run(write_book('chunked.epub', {'OPS/Text/one.xhtml': f'<html><body><p>{long_text}</p></body></html>'}))
        assert large is not None and long_text in large.read_text()
        run(write_book('doctype.epub', {'OPS/Text/one.xhtml': '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">' + BODY1}))

        for name, replacements in {
            'traversal-entry': {'../escape.xhtml': 'bad'},
            'absolute-entry': {'/escape.xhtml': 'bad'},
            'backslash-entry': {'OPS\\escape.xhtml': 'bad'},
            'traversal-uri': {'OPS/book.opf': OPF.replace('Text/one.xhtml', '../../escape.xhtml')},
            'remote-uri': {'OPS/book.opf': OPF.replace('Text/one.xhtml', 'https://example.com/read.xhtml')},
            'duplicate-id': {'OPS/book.opf': OPF.replace('id="two"', 'id="one"')},
            'unknown-spine': {'OPS/book.opf': OPF.replace('idref="two"', 'idref="missing"')},
            'empty-spine': {'OPS/book.opf': OPF.replace('<itemref idref="one"/><itemref idref="two"/>', '')},
            'xml-mismatch': {'OPS/Text/one.xhtml': '<html><body><p>bad</body></p></html>'},
            'bad-utf8': {'OPS/Text/one.xhtml': b'<html><body>\xff</body></html>'},
            'nul-xml': {'OPS/Text/one.xhtml': b'<html><body>\x00</body></html>'},
            'entity-attack': {'OPS/Text/one.xhtml': '<!DOCTYPE html [<!ENTITY x "bad">]><html><body>&x;</body></html>'},
            'missing-semicolon': {'OPS/Text/one.xhtml': '<html><body>&amp</body></html>'},
            'bad-scalar': {'OPS/Text/one.xhtml': '<html><body>&#xD800;</body></html>'},
            'encrypted-resource': {'META-INF/encryption.xml': '<encryption/>'},
            'oversized-chapter': {'OPS/Text/one.xhtml': '<html><body>' + 'x' * (512 * 1024) + '</body></html>'},
            'overdeep': {'OPS/Text/one.xhtml': '<html><body>' + '<div>' * 65 + 'x' + '</div>' * 65 + '</body></html>'},
            'fixed-layout': {'OPS/book.opf': OPF.replace('</metadata>', '<meta property="rendition:layout">pre-paginated</meta></metadata>')},
        }.items():
            run(write_book(name + '.epub', replacements), False)

        original = write_book('mutation-base.epub').read_bytes()
        central = original.index(b'PK\x01\x02')
        def mutate(name: str, data: bytes) -> None:
            path = folder / (name + '.epub')
            path.write_bytes(data)
            run(path, False)
        mismatch = bytearray(original); mismatch[30] ^= 1
        mutate('local-name-mismatch', mismatch)
        mismatch = bytearray(original); mismatch[central + 16] ^= 1
        mutate('central-crc-mismatch', mismatch)
        mismatch = bytearray(original); mismatch[central + 20:central + 24] = struct.pack('<I', 0xFFFFFFFF)
        mutate('zip64-size', mismatch)
        mismatch = bytearray(original); mismatch[6:8] = struct.pack('<H', 1); mismatch[central + 8:central + 10] = struct.pack('<H', 1)
        mutate('encrypted-flag', mismatch)
        mismatch = bytearray(stream.getvalue()); descriptor = mismatch.index(b'PK\x07\x08'); mismatch[descriptor + 4] ^= 1
        mutate('descriptor-crc', mismatch)
        for missing in (1, 10, 21, 80, len(original) // 2):
            mutate('truncated-' + str(missing), original[:-missing])
        assert not list((sd / '.folio/cache').glob('*.tmp')), 'Failed parses must clean temporary output'
        print(f'EPUB regressions passed: {count} fixtures/runs (ASan + UBSan)')


if __name__ == '__main__':
    main()
