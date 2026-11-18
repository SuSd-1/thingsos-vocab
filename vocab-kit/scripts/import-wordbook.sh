#!/bin/bash
# Turn an Excel word list into a ready-to-copy vocab folder:
#   bank.bin + audio/*.mp3
#
# Usage:
#   import-wordbook.sh 我的词书.xlsx [--out build/book] [--jobs 4] [--no-audio]
#                       [--tatoeba ~/vocab-data/cmn.txt] [--to-sd /Volumes/SD]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XLSX=""
OUT="$ROOT/build/book"
JOBS=4
AUDIO=1
TATOEBA="${TATOEBA:-$HOME/vocab-data/cmn.txt}"
VENV="${VOCAB_VENV:-$HOME/.venvs/piper}"
MODEL="${PIPER_MODEL:-$HOME/piper-voices/en_US-lessac-medium.onnx}"
TO_SD=""

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="${2:-}"; shift 2 ;;
    --jobs) JOBS="${2:-}"; shift 2 ;;
    --no-audio) AUDIO=0; shift ;;
    --tatoeba) TATOEBA="${2:-}"; shift 2 ;;
    --no-tatoeba) TATOEBA=""; shift ;;
    --venv) VENV="${2:-}"; shift 2 ;;
    --model) MODEL="${2:-}"; shift 2 ;;
    --to-sd) TO_SD="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) XLSX="$1"; shift ;;
  esac
done

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -n "$XLSX" ] || die "usage: import-wordbook.sh <wordbook.xlsx> [options]"
[ -f "$XLSX" ] || die "workbook not found: $XLSX"
need() { command -v "$1" >/dev/null 2>&1; }
need curl || die "missing curl"
need ffmpeg || die "missing ffmpeg (brew install ffmpeg)"
need python3 || die "missing python3"

say "== wordbook -> bank.bin =="
say "xlsx : $XLSX"
say "out  : $OUT"
say "venv : $VENV"

# --- python environment ---------------------------------------------------
if [ ! -x "$VENV/bin/python" ]; then
  say "creating venv ..."
  python3 -m venv "$VENV"
fi
PY="$VENV/bin/python"
"$PY" -m pip install --quiet --upgrade pip >/dev/null
say "installing python deps (openpyxl, opencc, eng-to-ipa, piper-tts) ..."
"$PY" -m pip install --quiet openpyxl opencc-python-reimplemented eng-to-ipa piper-tts

# --- piper voice ----------------------------------------------------------
if [ "$AUDIO" = "1" ]; then
  if [ ! -f "$MODEL" ]; then
    mkdir -p "$(dirname "$MODEL")"
    say "downloading voice en_US-lessac-medium (~60MB) ..."
    BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium"
    curl -fL --progress-bar "$BASE/en_US-lessac-medium.onnx" -o "$MODEL" || die "voice download failed"
    curl -fL --progress-bar "$BASE/en_US-lessac-medium.onnx.json" -o "$MODEL.json" || die "voice config download failed"
  fi
fi

# --- convert --------------------------------------------------------------
mkdir -p "$OUT"
CSV="$OUT/bank.csv"
TAT=""
if [ -n "$TATOEBA" ] && [ -f "$TATOEBA" ]; then TAT="--tatoeba $TATOEBA"; else say "note: no Tatoeba corpus, examples will be generated/left empty"; fi

say ""
say "[1/4] import workbook ..."
# shellcheck disable=SC2086
"$PY" "$ROOT/vocab-kit/tools/import_xlsx.py" --xlsx "$XLSX" --out "$CSV" $TAT

if [ "$AUDIO" = "1" ]; then
  say ""
  say "[2/4] pronounce words and examples ..."
  "$PY" "$ROOT/vocab-kit/tools/make_audio.py" --csv "$CSV" \
    --out "$OUT/audio" --model "$MODEL" --examples --jobs "$JOBS"
else
  say ""
  say "[2/4] skipped audio (--no-audio)"
fi

say ""
say "[3/4] pack bank.bin ..."
PACK_DIR="$OUT/sd/.thingsos/vocab"
if [ "$AUDIO" = "1" ]; then
  "$PY" "$ROOT/vocab-kit/tools/vocab_pack.py" --csv "$CSV" \
    --audio-dir "$OUT/audio" --out "$PACK_DIR"
else
  "$PY" "$ROOT/vocab-kit/tools/vocab_pack.py" --csv "$CSV" --no-audio --out "$PACK_DIR"
fi

say ""
say "[4/4] summary"
ls -l "$PACK_DIR/bank.bin"
if [ "$AUDIO" = "1" ]; then say "audio: $(find "$PACK_DIR/audio" -name '*.mp3' | wc -l | tr -d ' ') mp3"; fi
say "folder: $PACK_DIR"

if [ -n "$TO_SD" ]; then
  [ -d "$TO_SD" ] || die "SD path not found: $TO_SD"
  say ""
  say "copying to $TO_SD/.thingsos/vocab ..."
  mkdir -p "$TO_SD/.thingsos/vocab"
  cp -R "$PACK_DIR/." "$TO_SD/.thingsos/vocab/"
  say "copied"
fi

say ""
say "done. copy $PACK_DIR to the SD card as .thingsos/vocab/ (or re-run with --to-sd /Volumes/<SD>)"
