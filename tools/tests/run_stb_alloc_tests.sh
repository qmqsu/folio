#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-stb-alloc.XXXXXX")
trap 'rm -f "$tmp/test"; rmdir "$tmp"' EXIT
for v in 1 2; do
    "${CC:-cc}" -std=c11 -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
        -fno-sanitize-recover=all -Wall -Wextra -Werror -DSTBTT_RASTERIZER_VERSION=$v \
        -Imain/book tools/tests/stb_alloc_test.c -lm -o "$tmp/test"
    "$tmp/test" "${1:-main/assets/folio_builtin.ttf}"
done
