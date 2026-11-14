#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
settings_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/folio-settings-tests.XXXXXX")
trap 'rm -rf "$settings_test_dir"' EXIT HUP INT TERM
settings_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    settings_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
"$settings_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "-DFOLIO_SD_ROOT=\"$settings_test_dir/card\"" -I"$project_dir/tools/tests/settings_stubs" \
    "$project_dir/tools/tests/test_settings.c" -o "$settings_test_dir/test_settings"
"$settings_test_dir/test_settings"
