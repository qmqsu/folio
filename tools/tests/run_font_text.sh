#!/bin/sh
set -eu

folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-font-test.XXXXXX")
trap 'rm -rf "$folio_tmp"' EXIT HUP INT TERM
cd "$folio_project"

folio_toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
folio_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
folio_cc=${CC:-cc}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    folio_cc=${CC:-$folio_toolchain/clang}
fi

compile_font_tests() {
    "$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined -Wall -Wextra -Werror -Wno-unused-function \
        "$@" "-DFOLIO_BUILTIN_FONT_PATH=\"$folio_project/main/assets/folio_builtin.ttf\"" \
        "-DFOLIO_FONT_PACK_PATH=\"$folio_project/main/assets/folio_fonts.bin\"" \
        -Itools/tests/font_stubs -Imain/book tools/tests/test_font_text.c \
        main/book/folio_text.c main/book/folio_gbk_table.c main/book/folio_font_pack.c -lm -lz -o "$folio_tmp/font_text"
}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    compile_font_tests -isysroot "$folio_sdk" -B"$folio_toolchain"
else
    compile_font_tests
fi
"$folio_tmp/font_text" "$@"
