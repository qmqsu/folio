#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
python=${FOLIO_TEST_PYTHON:-"$HOME/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/bin/python3"}
if [ ! -x "$python" ]; then python=python3; fi
exec "$python" -B "$root/tools/tests/test_wallpaper.py"
