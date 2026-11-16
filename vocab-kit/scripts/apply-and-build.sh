#!/bin/bash
# Build the firmware (and run host tests). Assumes ESP-IDF is installed.
# Usage: apply-and-build.sh [version-suffix]   default: 0.2.0-vocab
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${1:-0.2.0-vocab}
IDF_PATH=${IDF_PATH:-$HOME/esp/esp-idf}

cd "$ROOT"

# ESP-IDF lives outside the workspace; the component manager cache must stay inside it.
. "$IDF_PATH/export.sh" >/dev/null
export IDF_COMPONENT_CACHE_PATH="$ROOT/build/component-cache"
mkdir -p build "$IDF_COMPONENT_CACHE_PATH"

echo "==> pinned vendor dependency check"
python3 vocab-kit/scripts/check_deps.py

echo "==> assets (font image)"
(cd native && python3 prepare_assets.py)

echo "==> host tests"
cmake -S native -B build/native-host -DCMAKE_BUILD_TYPE=Debug >/dev/null
# Upstream wifi_tx_power_test.c redefines gets() and does not build on macOS;
# build the targets this kit touches instead of the whole suite.
cmake --build build/native-host --target thingsos_vocab_test -j \
  "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
ctest --test-dir build/native-host -R vocab_bank_and_scheduler --output-on-failure

echo "==> firmware build"
(cd native/firmware && idf.py --preview -B ../../build/native-idf \
   -DTHINGSOS_BUILD_VERSION="$VERSION" reconfigure build)

echo "==> JPEG decoder link gate"
python3 native/verify_jpeg_link.py build/native-idf/ThingsOS_native.map

echo "==> flash budget"
bash vocab-kit/scripts/check-size.sh
