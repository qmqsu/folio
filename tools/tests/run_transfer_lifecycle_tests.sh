#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
transfer_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/folio-transfer-lifecycle.XXXXXX")
trap 'rm -rf "$transfer_test_dir"' EXIT HUP INT TERM
transfer_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
 transfer_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
 export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
"$transfer_cc" -std=c11 -D_POSIX_C_SOURCE=200809L "-DFOLIO_SD_ROOT=\"$transfer_test_dir/card\"" -Wall -Wextra -Werror -Wno-string-compare -fsanitize=address,undefined -fno-sanitize-recover=all -g -I"$project_dir/tools/tests/transfer_stubs" -I"$project_dir/main" -I"$project_dir/main/net" "$project_dir/tools/tests/test_transfer_lifecycle.c" "$project_dir/main/net/folio_upload.c" -o "$transfer_test_dir/test"
"$transfer_test_dir/test"
