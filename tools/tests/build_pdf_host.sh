#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "$(uname -s)" = Darwin ]; then export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"; fi
dest=$1
compiler=$2
mkdir -p "$dest/objects"
for source in "$root/components/pdfio/folio_pdf_miniz.c" "$root"/components/pdfio/pdfio-*.c "$root/main/book/folio_pdf.c" "$root/main/book/folio_jpeg.c"; do
    "$compiler" -std=gnu11 -DFOLIO_PDF_USE_MINIZ -DMINIZ_X86_OR_X64_CPU=0 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I"$root/components/pdfio" -c "$source" -o "$dest/objects/$(basename "$source").o"
done
ar rcs "$dest/libfolio_pdf.a" "$dest"/objects/*.o
