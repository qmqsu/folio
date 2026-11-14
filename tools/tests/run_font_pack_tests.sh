#!/bin/sh
set -eu

folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-font-pack.XXXXXX")
folio_tmp=$(CDPATH= cd -- "$folio_tmp" && pwd -P)
cleanup() {
    # Only remove the canonical, private directory created by mktemp above.
    case "$folio_tmp" in
        /*/folio-font-pack.??????) rm -rf -- "${folio_tmp:?}" ;;
        *) printf '%s\n' "Refusing unexpected temporary path: $folio_tmp" >&2 ;;
    esac
}
trap cleanup EXIT HUP INT TERM

folio_toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
folio_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
folio_cc=${CC:-cc}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    folio_cc=${CC:-$folio_toolchain/clang}
fi

compile_font_pack_tests() {
    "$folio_cc" -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Wpedantic -Werror -Wframe-larger-than=2048 \
        "$@" -I"$folio_project/main/book" \
        "$folio_project/tools/tests/test_font_pack.c" \
        "$folio_project/main/book/folio_font_pack.c" \
        -lz -o "$folio_tmp/font_pack"
}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    compile_font_pack_tests -isysroot "$folio_sdk" -B"$folio_toolchain"
else
    compile_font_pack_tests
fi

# An explicit fixture is required to exist; the repository fixture is optional.
if [ "$#" -eq 0 ] && [ -f "$folio_project/main/assets/folio_fonts.bin" ]; then
    set -- "$folio_project/main/assets/folio_fonts.bin"
fi
"$folio_tmp/font_pack" "$@"
