#!/bin/bash
# Write ONLY the application partition (0x220000, ota_0). Bootloader, partition
# table, rescue and NVS are left untouched. The device must be in BOOT/download mode.
# Usage: flash-app.sh [port]   (default /dev/cu.usbmodem101)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${1:-/dev/cu.usbmodem101}
IDF_PATH=${IDF_PATH:-$HOME/esp/esp-idf}

if [ ! -f "$ROOT/build/native-idf/ThingsOS_native.bin" ]; then
  echo "no build found; run apply-and-build.sh first"
  exit 1
fi

echo "==> app image"
ls -l "$ROOT/build/native-idf/ThingsOS_native.bin"
echo "==> device must be in BOOT mode (设置 → BOOT 刷机, or hold BOOT at power-on)"
. "$IDF_PATH/export.sh" >/dev/null
cd "$ROOT/native/firmware"
idf.py --preview -B ../../build/native-idf -p "$PORT" app-flash

echo "==> done; device reboots automatically"
