#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
sdk=${IDF_PATH:-$HOME/esp/esp-idf-v6.1}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-online-guard-tests.XXXXXX")
cleanup() {
    case "$tmp" in "${TMPDIR:-/tmp}"/folio-online-guard-tests.*) [ -d "$tmp" ] && rm -rf -- "$tmp";; *) echo 'Unsafe cleanup target' >&2;; esac
}
trap cleanup EXIT HUP INT TERM
cd "$project"
python3 tools/tests/extract_online_update_http.py "$sdk" "$tmp/online_update_sdk_http.inc"
cc=${CC:-cc}
toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
mac_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
platform=''
if [ -x "$toolchain/clang" ] && [ -d "$mac_sdk" ]; then
    cc=${CC:-$toolchain/clang};platform="-isysroot $mac_sdk -B$toolchain"
fi
compile() {
    "$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror $platform \
        -Itools/tests/online_update_stubs -Imain -I"$tmp" -I"$sdk/components/http_parser" "$@"
}
compile -DESP_PLATFORM -Dshutdown=online_guard_test_shutdown -c main/folio_online_guard.c -o "$tmp/guard.o"
compile tools/tests/test_online_update_guard.c "$tmp/guard.o" "$sdk/components/http_parser/http_parser.c" \
    -lpthread -o "$tmp/guard-tests"
"$tmp/guard-tests"
