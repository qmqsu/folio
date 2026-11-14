#!/bin/sh
# No device/USB access and no dependency installation. Preserve artifacts.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/artifacts/firmware-0830/font-assets"
mkdir -p "$out"
"${CC:-/usr/bin/clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -Wall -Wextra -Werror -Wno-unused-function \
    -I"$root/tools/tests/font_stubs" -I"$root/main/book" \
    "$root/tools/tests/test_font_asset_symbols.c" \
    "$root/main/book/folio_text.c" "$root/main/book/folio_gbk_table.c" \
    -lm -o "$out/font-asset-symbols-test"
"$out/font-asset-symbols-test" "$root/main/assets/folio_builtin.ttf" \
    "$root/main/assets/fonts/folio_symbols.ttf" \
    "$root/main/assets/fonts/charsets/SYMBOLS_ALL.txt"
