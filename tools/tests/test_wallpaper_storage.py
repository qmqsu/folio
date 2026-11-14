#!/usr/bin/env python3
"""Actual storage/cache code, generated files and deterministic IO failpoints only."""
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file():
        compiler = shutil.which('clang') or shutil.which('cc')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    with tempfile.TemporaryDirectory(prefix='folio-wallpaper-storage-') as temporary:
        root = Path(temporary).resolve()
        card = root / 'card'
        sentinel = root / 'outside-user-file'
        sentinel.write_bytes(b'OUTSIDE PAYLOAD MUST SURVIVE\0\xff')
        original = hashlib.sha256(sentinel.read_bytes()).digest()
        binary = root / 'test-wallpaper-storage'
        subprocess.run([compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11',
                        '-D_POSIX_C_SOURCE=200809L', f'-DFOLIO_SD_ROOT="{card}"',
                        '-Wall', '-Wextra', '-Werror', '-g', '-O1', '-fno-omit-frame-pointer',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'main'), str(ROOT / 'tools/tests/test_wallpaper_storage.c'),
                        str(ROOT / 'tools/tests/test_wallpaper_storage_cancel.c'),
                        '-lpthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(sentinel)], check=True, timeout=120)
        assert hashlib.sha256(sentinel.read_bytes()).digest() == original
        # Compile the exact small ESP-only allocator and yield helper from the
        # module as well; the main suite exercises its native-host malloc branch.
        source = (ROOT / 'main/folio_storage.c').read_text()
        allocator = re.search(r'static void\* wallpaper_cache_alloc\(size_t size\) \{.*?\n\}', source, re.S)
        yielding = re.search(r'static void wallpaper_cache_yield\(size_t processed\) \{.*?\n\}', source, re.S)
        assert allocator and yielding
        helper_source = root / 'cache-esp-helper.c'
        helper_source.write_text('''#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_PLATFORM 1
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
static unsigned allocations,yields;static bool fail_psram;
static void* heap_caps_malloc(size_t size,unsigned flags) {
    ++allocations;assert(flags==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    return fail_psram?NULL:malloc(size);
}
static void storage_yield(void){++yields;}
#define malloc forbidden_internal_malloc
''' + allocator.group(0) + '\n' + yielding.group(0) + '''
#undef malloc
int main(void) {
    fail_psram=true;assert(!wallpaper_cache_alloc(415872) && allocations==1);
    fail_psram=false;void* p=wallpaper_cache_alloc(415872);assert(p && allocations==2);free(p);
    for(size_t pos=4096;pos<415872;pos+=4096)wallpaper_cache_yield(pos);
    wallpaper_cache_yield(415872);assert(yields==6);
    puts("wallpaper ESP helper: PSRAM-only success/failure; 6 yields per full-screen payload, no internal fallback");
    return 0;
}
''')
        helper_binary = root / 'cache-esp-helper'
        subprocess.run([compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11',
                        '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        str(helper_source), '-o', str(helper_binary)], check=True)
        subprocess.run([str(helper_binary)], check=True)
        print('Prepared wallpaper storage ASan/UBSan checks passed; external sentinel unchanged')


if __name__ == '__main__':
    main()
