#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-online-update-tests.XXXXXX")
# Only the exact, newly created test directory is eligible for cleanup.
cleanup() {
    case "$tmp" in "${TMPDIR:-/tmp}"/folio-online-update-tests.*) [ -d "$tmp" ] && rm -rf -- "$tmp";; *) echo 'Unsafe cleanup target' >&2;; esac
}
trap cleanup EXIT HUP INT TERM
cd "$project"
python=${FOLIO_PYTHON:-python3}
if ! "$python" -c 'import cryptography' 2>/dev/null; then
    python="$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/python"
fi
"$python" tools/tests/generate_remote_manifest_fixtures.py "$tmp/fixtures"
toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
cc=${CC:-cc}
if [ -x "$toolchain/clang" ] && [ -d "$sdk" ]; then
    cc=${CC:-$toolchain/clang}
    platform="-isysroot $sdk -B$toolchain"
    libraries='-framework Security -framework CoreFoundation'
else
    platform=''
    libraries='-lcrypto -lpthread'
fi
compile() {
    # Intentional word splitting for compiler/linker option lists only.
    "$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror \
        -Wmissing-braces -Wparentheses -Wno-deprecated-declarations $platform "-DFOLIO_SD_ROOT=\"$tmp/card\"" \
        "-DFOLIO_RELEASE_KEY_HEADER=\"$tmp/fixtures/test_release_key.h\"" \
        -Itools/tests/online_update_stubs -Itools/tests/update_stubs -Itools/tests/ui_stubs -Imain "$@"
}
compile -Dwrite=online_test_write -Dfsync=online_test_fsync -Dtime=online_test_time -c main/folio_online_update.c -o "$tmp/online.o"
compile -DESP_PLATFORM -c main/folio_online_guard.c -o "$tmp/guard.o"
compile -c main/folio_remote_manifest.c -o "$tmp/manifest.o"
compile -c main/folio_update.c -o "$tmp/update.o"
compile tools/tests/test_online_update.c tools/tests/test_remote_manifest.c \
    "$tmp/online.o" "$tmp/guard.o" "$tmp/manifest.o" "$tmp/update.o" $libraries -o "$tmp/online-test"
"$tmp/online-test" "$tmp/fixtures"
