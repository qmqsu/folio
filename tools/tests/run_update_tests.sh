#!/bin/sh
set -eu
update_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
update_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-update-tests.XXXXXX")
trap 'rm -rf "$update_tmp"' EXIT HUP INT TERM
cd "$update_project"
update_python=${FOLIO_PYTHON:-python3}
"$update_python" tools/tests/generate_update_fixtures.py "$update_tmp/fixtures"
update_toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
update_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
update_cc=${CC:-cc}
if [ -x "$update_toolchain/clang" ] && [ -d "$update_sdk" ]; then update_cc=${CC:-$update_toolchain/clang}; fi
compile_update() {
    "$update_cc" -std=c11 -D_POSIX_C_SOURCE=200809L "-DFOLIO_SD_ROOT=\"$update_tmp/card\"" \
        -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Werror "$@" -Itools/tests/update_stubs -Itools/tests/ui_stubs -Imain \
        tools/tests/test_update.c -o "$update_tmp/update_test"
}
if [ -x "$update_toolchain/clang" ] && [ -d "$update_sdk" ]; then
    compile_update -isysroot "$update_sdk" -B"$update_toolchain"
else
    compile_update -lcrypto -Wno-deprecated-declarations
fi
"$update_tmp/update_test" "$update_tmp/fixtures" "$@"
