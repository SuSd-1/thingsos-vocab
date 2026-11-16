#!/bin/bash
# Enforce the flash budget: compare the built app image against the recorded baseline.
# Baseline lives in vocab-kit/size-baseline.json (created on first run).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP="$ROOT/build/native-idf/ThingsOS_native.bin"
BASELINE="$ROOT/vocab-kit/size-baseline.json"
LIMIT_KB=${LIMIT_KB:-120}

if [ ! -f "$APP" ]; then
  echo "missing $APP; build first"
  exit 1
fi

SIZE=$(stat -f%z "$APP")

if [ ! -f "$BASELINE" ]; then
  printf '{\n  "baseline_bytes": %s,\n  "note": "official 0.2.0 app image, built on macOS"\n}\n' "$SIZE" > "$BASELINE"
  echo "recorded baseline: $SIZE bytes -> $BASELINE"
  exit 0
fi

BASE=$(python3 -c "import json;print(json.load(open('$BASELINE'))['baseline_bytes'])")
DELTA=$((SIZE - BASE))
LIMIT=$((LIMIT_KB * 1024))
printf 'app bytes: %s (baseline %s, delta %+d, limit %+d)\n' "$SIZE" "$BASE" "$DELTA" "$LIMIT"
if [ "$DELTA" -gt "$LIMIT" ]; then
  echo "FLASH BUDGET EXCEEDED"
  exit 1
fi
if [ "$DELTA" -gt 0 ]; then
  echo "within budget"
fi
