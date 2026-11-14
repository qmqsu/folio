#!/bin/sh
set -eu
folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-power-test.XXXXXX")
trap 'rm -rf "$folio_tmp"' EXIT HUP INT TERM
folio_cc=${CC:-cc}
if [ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang ]; then
    folio_cc=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
    export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
fi
power_variant() {
    folio_variant=$1
    shift
    "$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -Wmissing-braces -Wparentheses -fsanitize=address,undefined -g "$@" \
        -I"$folio_project/tools/tests/power_stubs" -I"$folio_project/main" \
        -I"$folio_project/components/read_pico_pmu/include" \
        "$folio_project/tools/tests/test_power.c" "$folio_project/main/folio_power.c" -o "$folio_tmp/test_power_$folio_variant"
    # Refuse execution if a regression could resolve a host-clock write from libc.
    folio_undefined=$(nm -u "$folio_tmp/test_power_$folio_variant")
    case "$folio_undefined" in
        *settimeofday*) printf '%s\n' "Unsafe undefined settimeofday symbol in $folio_variant" >&2; exit 1 ;;
    esac
    printf '\n=== Power tests: %s (no undefined settimeofday symbol) ===\n' "$folio_variant"
    "$folio_tmp/test_power_$folio_variant"
}
power_variant native
power_variant esp-platform -DESP_PLATFORM
