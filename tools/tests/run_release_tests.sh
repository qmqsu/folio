#!/bin/sh
set -eu
release_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
release_python=${FOLIO_PYTHON:-python3}
cd "$release_root"
PYTHONDONTWRITEBYTECODE=1 "$release_python" -m unittest discover -s tools/tests -p 'test_release*.py' -v
