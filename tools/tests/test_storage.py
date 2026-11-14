#!/usr/bin/env python3
"""All destructive storage tests are confined to a fresh temporary root."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file(): compiler = shutil.which('clang') or shutil.which('cc')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    with tempfile.TemporaryDirectory(prefix='folio-storage-') as temporary:
        temp = Path(temporary); sd = temp / 'sd'; outside = temp / 'outside'; outside.mkdir()
        (outside / 'sentinel.txt').write_text('MUST SURVIVE')
        fixtures = {'books/a.txt': 'aaaa', 'books/b.EPUB': 'bbbbbbb', 'books/sub/notes.bin': '123',
                    'fonts/main.ttf': '1234567', '.folio/cache/chapter.txt': '12345', '.folio/state': '123'}
        for name, value in fixtures.items():
            path = sd / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(value)
        binary = temp / 'storage-tests'
        subprocess.run([compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                        '-Wall', '-Wextra', '-Werror', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        f'-DFOLIO_SD_ROOT="{sd}"', '-I', str(ROOT / 'main'), str(ROOT / 'main/folio_storage.c'),
                        str(ROOT / 'tools/tests/test_storage.c'), '-lpthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(outside)], check=True, timeout=30)
        assert (outside / 'sentinel.txt').read_text() == 'MUST SURVIVE'
        assert list(outside.iterdir()) == [outside / 'sentinel.txt']
        print('Storage host tests passed (ASan + UBSan); no real SD path used')


if __name__ == '__main__': main()
