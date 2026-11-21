#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
reader_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/folio-reader-tests.XXXXXX")
reader_test_dir=$(CDPATH= cd -- "$reader_test_dir" && pwd -P)
trap 'rm -rf "$reader_test_dir"' EXIT HUP INT TERM
reader_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    reader_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
python3 "$project_dir/tools/tests/pdf_fixtures.py" "$reader_test_dir/card/books"
sh "$project_dir/tools/tests/build_pdf_host.sh" "$reader_test_dir/pdf" "$reader_cc"
"$reader_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g -pthread \
    "-DFOLIO_SD_ROOT=\"$reader_test_dir/card\"" \
    -I"$project_dir/tools/tests/reader_stubs" -I"$project_dir/main/book" \
    "$project_dir/tools/tests/test_reader.c" "$project_dir/main/book/folio_text.c" \
    "$project_dir/main/book/folio_gbk_table.c" "$project_dir/main/book/folio_weread_cache.c" \
    "$project_dir/main/folio_storage.c" "$reader_test_dir/pdf/libfolio_pdf.a" -lz -o "$reader_test_dir/test_reader"
"$reader_test_dir/test_reader"
