#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/folio-weread-cache.XXXXXX")
test_dir=$(CDPATH= cd -- "$test_dir" && pwd -P)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
cache_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    cache_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
"$cache_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g -pthread \
    "-DFOLIO_SD_ROOT=\"$test_dir/card\"" -I"$project_dir/main/book" -I"$project_dir/main" \
    "$project_dir/tools/tests/test_weread_cache.c" "$project_dir/main/folio_storage.c" \
    -o "$test_dir/test_cache"
"$test_dir/test_cache"
