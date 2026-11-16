#!/bin/bash
# Full 16 MiB flash backup before any device write.
# Usage: backup-flash.sh <port> [out.bin]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${1:?usage: backup-flash.sh <serial-port> [out.bin]}
OUT=${2:-$ROOT/build/flash-backup-$(date +%Y%m%d-%H%M%S).bin}
IDF_PATH=${IDF_PATH:-$HOME/esp/esp-idf}

mkdir -p "$(dirname "$OUT")"
. "$IDF_PATH/export.sh" >/dev/null

echo "==> reading 16 MiB from $PORT (takes ~90 s)"
esptool --port "$PORT" --before default-reset read-flash 0 0x1000000 "$OUT"
ls -l "$OUT"
shasum -a 256 "$OUT"
