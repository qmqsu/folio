#!/bin/sh
# Host-only storage tests: generated roots, never a mounted user SD card.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
python3 "$ROOT/tools/tests/test_storage.py"
python3 "$ROOT/tools/tests/test_picture_migration.py"
python3 "$ROOT/tools/tests/test_wallpaper_storage.py"
