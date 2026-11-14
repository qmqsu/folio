#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
upload_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/folio-upload-tests.XXXXXX")
trap 'rm -rf "$upload_test_dir"' EXIT HUP INT TERM
upload_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
 upload_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
 export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
"$upload_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g -I"$project_dir/main/net" "$project_dir/tools/tests/test_upload.c" "$project_dir/main/net/folio_upload.c" -o "$upload_test_dir/test_upload"
"$upload_test_dir/test_upload" "$upload_test_dir/card"
