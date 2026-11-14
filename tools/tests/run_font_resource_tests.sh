#!/bin/sh
# Read-only source assets; all mutable fixtures and binaries stay in mktemp.
set -eu

folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-font-resource.XXXXXX")
folio_tmp=$(CDPATH= cd -- "$folio_tmp" && pwd -P)
cleanup() {
    case "$folio_tmp" in
        /*/folio-font-resource.??????) rm -rf -- "${folio_tmp:?}" ;;
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
compile_resource_tests() {
    "$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 \
        -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Werror -Wno-unused-function "$@" \
        -DFONT_RESOURCE_CANCEL_HOOK -c "$folio_project/tools/tests/test_font_resources.c" \
        -o "$folio_tmp/cancel-hook.o"
    "$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 \
        -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Werror -Wno-unused-function "$@" \
        -I"$folio_project/tools/tests/font_stubs" -I"$folio_project/main/book" \
        "$folio_project/tools/tests/test_font_resources.c" "$folio_tmp/cancel-hook.o" \
        "$folio_project/main/book/folio_font_pack.c" \
        "$folio_project/main/book/folio_text.c" "$folio_project/main/book/folio_gbk_table.c" \
        -lm -lz -o "$folio_tmp/font-resources"
}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    compile_resource_tests -isysroot "$folio_sdk" -B"$folio_toolchain"
else
    compile_resource_tests
fi
"$folio_tmp/font-resources" "$folio_project/main/assets/folio_builtin.ttf" \
    "$folio_project/main/assets/folio_fonts.bin" \
    "$folio_project/main/assets/fonts/charsets/B.txt" \
    "$folio_project/main/assets/fonts/charsets/SYMBOLS_REQUIRED.txt" "$folio_tmp"
