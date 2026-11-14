#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${FOLIO_BUILD_DIR:-${TMPDIR:-/tmp}/folio-readpico-build}
case "$build_dir" in
    /*) ;;
    *) build_dir="${TMPDIR:-/tmp}/$build_dir" ;;
esac
cd "$project_dir"
exec idf.py -B "$build_dir" "$@"
