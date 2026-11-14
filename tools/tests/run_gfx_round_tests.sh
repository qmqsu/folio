#!/bin/sh
set -eu
folio_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-gfx-round-tests.XXXXXX")
trap 'rm -rf "$folio_tmp"' EXIT HUP INT TERM
cd "$folio_root"
folio_toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
folio_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
folio_cc=${CC:-cc}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then folio_cc=${CC:-$folio_toolchain/clang}; fi
compile_gfx() {
    "$folio_cc" -std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Werror "$@" -Itools/tests/ui_stubs -Imain \
        tools/tests/test_gfx_round_rects.c main/folio_gfx.c -o "$folio_tmp/gfx_tests"
}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    compile_gfx -isysroot "$folio_sdk" -B"$folio_toolchain"
else
    compile_gfx
fi
"$folio_tmp/gfx_tests" "$@"
