#!/bin/sh
set -eu
folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_ledger_base=${TMPDIR:-/tmp}
folio_ledger_tmp=$(mktemp -d "${folio_ledger_base%/}/folio-ledger-test.XXXXXX")
trap 'rm -rf "$folio_ledger_tmp"' EXIT HUP INT TERM
folio_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    folio_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
mkdir "$folio_ledger_tmp/outside"
"$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L "-DFOLIO_SD_ROOT=\"$folio_ledger_tmp/card\"" \
    -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -Drename=folio_test_time_rename -I"$folio_project/main" -I"$folio_project/main/net" \
    -c "$folio_project/main/net/folio_weread_time.c" -o "$folio_ledger_tmp/ledger.o"
"$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L "-DFOLIO_SD_ROOT=\"$folio_ledger_tmp/card\"" \
    -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$folio_project/main" -I"$folio_project/main/net" \
    "$folio_project/tools/tests/test_weread_time.c" "$folio_ledger_tmp/ledger.o" \
    "$folio_project/main/folio_storage.c" -lpthread -o "$folio_ledger_tmp/test"
"$folio_ledger_tmp/test" "$folio_ledger_tmp/outside"
