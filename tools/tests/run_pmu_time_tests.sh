#!/bin/sh
set -eu
folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_driver=${1:-"$folio_project/components/read_pico_pmu/read_pico_pmu.c"}
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-pmu-time-test.XXXXXX")
folio_tmp=$(CDPATH= cd -- "$folio_tmp" && pwd -P)
case "$folio_tmp" in
    /*/folio-pmu-time-test.*) ;;
    *) echo "Unexpected test directory: $folio_tmp" >&2; exit 1 ;;
esac
# The canonical directory above belongs to this invocation (including clang dSYM).
trap 'rm -rf -- "${folio_tmp:?}"' EXIT HUP INT TERM
folio_cc=${CC:-cc}
if [ -z "${CC:-}" ] && [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    folio_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
printf 'Driver source: %s\n' "$folio_driver"
printf 'Host C11 build: -Wall -Wextra -Werror -fsanitize=address,undefined\n'
"$folio_cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer -g \
    -I"$folio_project/tools/tests/pmu_stubs" \
    -I"$folio_project/components/read_pico_pmu/include" \
    "$folio_project/tools/tests/test_pmu_time.c" "$folio_driver" \
    -o "$folio_tmp/test_pmu_time"
"$folio_tmp/test_pmu_time"
