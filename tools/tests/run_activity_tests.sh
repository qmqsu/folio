#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
activity_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/folio-activity-tests.XXXXXX")
trap 'rm -rf "$activity_test_dir"' EXIT HUP INT TERM
activity_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    activity_cc=${CC:-/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang}
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
"$activity_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "-DFOLIO_SD_ROOT=\"$activity_test_dir/card\"" \
    "$project_dir/tools/tests/test_activity.c" -o "$activity_test_dir/test_activity"
"$activity_test_dir/test_activity"
