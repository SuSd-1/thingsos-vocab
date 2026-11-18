#!/bin/bash
# One-click install of a published ThingsOS Vocab app image.
# Downloads the GitHub release asset, verifies its SHA256 and flashes ONLY the
# application partition at 0x220000 (bootloader / partitions / NVS untouched).
#
# Usage: install-release.sh [--port PORT] [--tag TAG] [--dry-run] [--yes]
set -euo pipefail

REPO="SuSd-1/thingsos-vocab"
TAG=""
PORT=""
DRY_RUN=0
ASSUME_YES=0
CHECK_HEALTH=1
CHIP="esp32s31"
ADDR="0x220000"

while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="${2:-}"; shift 2 ;;
    --tag) TAG="${2:-}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --yes) ASSUME_YES=1; shift ;;
    --no-health) CHECK_HEALTH=0; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1; }

say "== ThingsOS Vocab installer =="
say "此脚本只写 app 分区 ($ADDR)，不会清空 SD 卡上的词库/进度，也不会动 NVS。"
say "前提：设备已经刷入原版 ThingsOS 0.2.0（含引导与分区）。"
say "免责声明：本固件仅限尝鲜/个人备份使用，不承诺修复 bug 或长期维护。"
say ""

# --- tools ---------------------------------------------------------------
need curl || die "missing curl"
need shasum || die "missing shasum"

supports_chip() {
  # $1 = candidate command; succeeds when this esptool knows the target chip.
  $1 --chip "$CHIP" version >/dev/null 2>&1
}

ESPTOOL=""
IDF_PY="${IDF_PYTHON:-$HOME/.espressif/python_env/idf6.1_py3.11_env/bin/python}"
for candidate in \
  "$IDF_PY -m esptool" \
  "$HOME/.espressif/python_env/idf6.1_py3.11_env/bin/python -m esptool" \
  "esptool" \
  "python3 -m esptool"; do
  if supports_chip "$candidate"; then ESPTOOL="$candidate"; break; fi
done
[ -n "$ESPTOOL" ] || die "no esptool with $CHIP support; install ESP-IDF 6.1 or: python3 -m pip install -U esptool"
say "esptool: $ESPTOOL"

# --- port ----------------------------------------------------------------
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  [ -n "$PORT" ] || die "no /dev/cu.usbmodem* port found; connect the device or pass --port"
fi
say "port: $PORT"
say "chip: $CHIP   address: $ADDR"

# --- release -------------------------------------------------------------
if [ -z "$TAG" ]; then
  TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
        | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
  [ -n "$TAG" ] || die "cannot determine the latest release tag"
fi
say "release: $TAG"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
BASE="https://github.com/$REPO/releases/download/$TAG"
say "downloading ThingsOS_native.bin ..."
curl -fL --progress-bar "$BASE/ThingsOS_native.bin" -o "$TMP/ThingsOS_native.bin" || die "download failed"
curl -fsSL "$BASE/SHA256SUMS.txt" -o "$TMP/SHA256SUMS.txt" || die "checksum download failed"

EXPECT=$(awk '{print $1}' "$TMP/SHA256SUMS.txt" | head -1)
ACTUAL=$(shasum -a 256 "$TMP/ThingsOS_native.bin" | awk '{print $1}')
[ "$EXPECT" = "$ACTUAL" ] || die "SHA256 mismatch: expected $EXPECT got $ACTUAL"
say "sha256: $ACTUAL (verified)"

if [ "$DRY_RUN" = "1" ]; then
  say "dry-run: would flash $TMP/ThingsOS_native.bin to $ADDR on $PORT"
  exit 0
fi

if [ "$ASSUME_YES" != "1" ]; then
  printf 'flash now? [y/N] '
  read -r answer
  case "$answer" in y|Y|yes|YES) ;; *) say "aborted"; exit 0 ;; esac
fi

# --- flash ---------------------------------------------------------------
$ESPTOOL --chip "$CHIP" -p "$PORT" -b 460800 \
  --before default-reset --after hard-reset \
  write-flash "$ADDR" "$TMP/ThingsOS_native.bin"

say ""
if [ "$CHECK_HEALTH" = "1" ] && python3 -c 'import serial' >/dev/null 2>&1; then
  say "waiting for the app to confirm health (up to 30s) ..."
  RESULT=$(python3 - "$PORT" <<'PYHEALTH'
import sys, time
import serial
port = sys.argv[1]
try:
    s = serial.Serial(port, 115200, timeout=0.3)
except Exception:
    print("skip"); raise SystemExit(0)
deadline = time.time() + 30
seen = "none"
while time.time() < deadline:
    try:
        data = s.read(4096)
    except Exception:
        break
    if not data:
        continue
    text = data.decode("utf-8", "replace")
    if "Startup health confirmed" in text:
        seen = "healthy"; break
    if "ThingsOS_rescue" in text:
        seen = "rescue"; break
try:
    s.close()
except Exception:
    pass
print(seen)
PYHEALTH
)
  case "$RESULT" in
    healthy) say "boot health: confirmed (display / font / touch)" ;;
    rescue)
      say ""
      say "!! 设备进入了救援系统（连续 3 次未确认启动）。"
      say "!! 通常是因为短时间内反复复位。请彻底断电（拔掉 USB）等 3 秒再上电一次，"
      say "!! 不要连续多次复位，也不要立刻重刷。"
      ;;
    *) say "boot health: not observed (device may still be booting); check the screen." ;;
  esac
else
  say "boot health: skipped"
fi
say ""
say "done. next steps:"
say "  1) put bank.bin and audio/ into /sdcard/.thingsos/vocab/ (see docs/VOCAB-DATA.md)"
say "  2) Finder hides dot-folders: press Cmd+Shift+. to show .thingsos,"
say "     or use Terminal: open /Volumes/<SD>/.thingsos/vocab"
say "  3) to keep progress, do not run erase-flash; app-only flashing preserves SD and NVS."
say "  4) if the screen shows the rescue system, unplug power for 3s and plug it back in once."
