#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: sh tools/tests/run_ui_preview.sh font.ttf [output_directory]" >&2
    exit 2
fi
folio_font_path=$1
case "$folio_font_path" in /*) ;; *) folio_font_path="$(pwd)/$folio_font_path" ;; esac
folio_output=${2:-$(mktemp -d "${TMPDIR:-/tmp}/folio-ui-preview.XXXXXX")}
case "$folio_output" in /*) ;; *) folio_output="$(pwd)/$folio_output" ;; esac
mkdir -p "$folio_output"
folio_project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
folio_tmp=$(mktemp -d "${TMPDIR:-/tmp}/folio-ui-test.XXXXXX")
folio_tmp=$(CDPATH= cd -- "$folio_tmp" && pwd)
trap 'rm -rf "$folio_tmp"' EXIT HUP INT TERM
cd "$folio_project"

folio_toolchain=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
folio_sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
folio_cc=${CC:-cc}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then folio_cc=${CC:-$folio_toolchain/clang}; fi
# Keep the host descriptor aligned with the build's single version source.
folio_python=${FOLIO_PYTHON:-python3}
folio_version=$("$folio_python" -c 'import pathlib,re; print(re.search(r"set\(PROJECT_VER \"([^\"]+)\"\)",pathlib.Path("CMakeLists.txt").read_text()).group(1))')
compile_ui() {
    sh tools/tests/build_pdf_host.sh "$folio_tmp/pdf" "$folio_cc"
    "$folio_cc" -std=c11 -D_POSIX_C_SOURCE=200809L "-DFOLIO_SD_ROOT=\"$folio_tmp/card\"" \
        "-DFOLIO_TEST_FIXTURES=\"$folio_project/tools/tests/fixtures\"" \
        "-DFOLIO_TEST_APP_VERSION=\"$folio_version\"" \
        "-DFOLIO_BUILTIN_FONT_PATH=\"$folio_project/main/assets/folio_builtin.ttf\"" \
        "-DFOLIO_FONT_PACK_PATH=\"$folio_project/main/assets/folio_fonts.bin\"" \
        -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wall -Wextra -Werror -Wno-unused-function "$@" \
        -DFOLIO_EPUB_HOST_TEST -Itools/tests/ui_stubs -Imain -Imain/book -Imain/net -idirafter tools/tests/online_update_stubs tools/tests/test_ui_preview.c \
        main/net/folio_upload.c main/net/folio_transfer_validate.c main/book/folio_xml.c main/book/folio_zip.c \
        main/folio_gfx.c main/folio_activity.c main/folio_storage.c main/book/folio_catalog.c main/book/folio_weread_cache.c \
        main/vendor/qrcodegen/qrcodegen.c main/book/folio_cover.c main/book/folio_wallpaper.c main/book/folio_font.c main/book/folio_reader.c \
        main/book/folio_text.c main/book/folio_gbk_table.c main/book/folio_font_pack.c "$folio_tmp/pdf/libfolio_pdf.a" -lm -lz -o "$folio_tmp/ui_preview"
}
if [ -x "$folio_toolchain/clang" ] && [ -d "$folio_sdk" ]; then
    compile_ui -isysroot "$folio_sdk" -B"$folio_toolchain"
else
    compile_ui
fi
"$folio_tmp/ui_preview" "$folio_font_path" "$folio_output"
folio_python=${FOLIO_PYTHON:-python3}
"$folio_python" tools/tests/pgm_to_png.py "$folio_output"
"$folio_python" tools/tests/preview_contact_sheet.py "$folio_output"
echo "Native grayscale previews: $folio_output"
