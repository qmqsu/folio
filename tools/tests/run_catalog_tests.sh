#!/bin/sh
set -eu
catalog_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
catalog_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-catalog-tests.XXXXXX")
trap 'rm -rf "$catalog_tmp"' EXIT HUP INT TERM
cd "$catalog_project"
catalog_toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
catalog_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
catalog_cc=${CC:-cc}
if [ -x "$catalog_toolchain/clang" ] && [ -d "$catalog_sdk" ]; then catalog_cc=${CC:-$catalog_toolchain/clang}; fi
compile_catalog() {
    "$catalog_cc" -std=c11 -D_POSIX_C_SOURCE=200809L "-DFOLIO_SD_ROOT=\"$catalog_tmp/card\"" \
        -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Werror "$@" -Itools/tests/ui_stubs -Imain/book \
        tools/tests/test_catalog.c main/book/folio_text.c main/book/folio_gbk_table.c \
        -o "$catalog_tmp/catalog_test"
}
if [ -x "$catalog_toolchain/clang" ] && [ -d "$catalog_sdk" ]; then
    compile_catalog -isysroot "$catalog_sdk" -B"$catalog_toolchain"
else
    compile_catalog
fi
"$catalog_tmp/catalog_test"
