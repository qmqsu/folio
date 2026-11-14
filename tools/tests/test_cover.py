#!/usr/bin/env python3
"""Run the actual cover decoders; all image fixtures are original test data."""
import binascii
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', binascii.crc32(kind + data) & 0xffffffff)


def png(width, height, color=6, depth=8, interlace=0, raw=None, extra=b''):
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    if raw is None:
        pixel = {0: b'\x00', 2: b'\x30\x70\x90', 3: b'\x00', 4: b'\x00\x00', 6: b'\x30\x70\x90\xff'}[color]
        raw = (b'\x00' + pixel * width) * height
    palette = chunk(b'PLTE', b'\x10\x50\x80') if color == 3 else b''
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, color, 0, 0, interlace)) + palette + extra + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def main():
    with tempfile.TemporaryDirectory(prefix='folio-cover-') as temporary:
        folder = Path(temporary)
        fixtures = {
            'wallpaper.png': png(684,1216,0,raw=(b'\x00'+bytes([0,255])*342)*1216),
            'rgba.png': png(600, 900), 'rgba-large.png': png(800, 1200),
            'palette.png': png(100, 100, 3), 'transparent.png': png(100, 50, 4),
            'wide.png': png(400, 100, 0), 'interlaced.png': png(20, 20, interlace=1),
            'depth16.png': png(20, 20, depth=16),
            'huge.png': png(0xffffffff, 1, raw=b'\x00'),
            'too-many-pixels.png': png(2000, 1001, raw=b'\x00'),
            'excess-inflate.png': png(20, 20, raw=(b'\x00'+b'\x00\x00\x00\xff'*20)*21),
            'short-inflate.png': png(20, 20, raw=(b'\x00'+b'\x00\x00\x00\xff'*20)*19),
            'bad.jpg': b'\xff\xd8invalid\xff\xd9',
        }
        fixtures['bad-crc.png'] = fixtures['rgba.png'][:-1] + bytes([fixtures['rgba.png'][-1] ^ 1])
        raw = (b'\x00' + b'\x7f' * 127) * 256
        fixtures['missing-adler.png'] = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 127, 256, 8, 0, 0, 0, 0))
                                         + chunk(b'IDAT', zlib.compress(raw)[:-4]) + chunk(b'IEND', b''))
        fixtures['truncated.png'] = fixtures['rgba.png'][:-15]
        jpg = (ROOT / 'tools/tests/fixtures/cover/gradient-800x1200.jpg').read_bytes()
        fixtures['large.jpg'] = jpg
        fixtures['no-scan.jpg'] = jpg[:jpg.index(b'\xff\xda')] + b'\xff\xd9'
        sos = jpg.index(b'\xff\xda')
        fixtures['missing-restart.jpg'] = jpg[:sos] + b'\xff\xdd\x00\x04\x00\x01' + jpg[sos:]
        fixtures['bad-post-sof.jpg'] = jpg[:sos] + b'\xff\xf0\x00\x02' + jpg[sos:]
        fixtures['truncated.jpg'] = jpg[:len(jpg)//2]
        for name, data in fixtures.items():
            (folder/name).write_bytes(data)
        toolchain = Path('/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin')
        sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
        compiler = os.environ.get('CC', str(toolchain/'clang') if (toolchain/'clang').exists() else 'cc')
        flags = ['-isysroot', str(sdk), '-B'+str(toolchain)] if sdk.exists() else []
        command = [compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-g', '-O1', '-fno-omit-frame-pointer',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
                   *flags, '-Itools/tests/cover_stubs', '-Imain/book', 'tools/tests/test_cover.c', '-lm', '-o', str(folder/'test-cover')]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(folder/'test-cover'), str(folder)], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
