#!/usr/bin/env python3
"""Pack a CSV word list (+ optional audio) into a ThingsOS vocab SD bundle.

Output layout (copy the directory contents to /sdcard/.thingsos/vocab/):

    bank.bin
    audio/<id>.mp3
    report.txt

CSV columns (UTF-8, header required):
    word, phonetic, pos, meaning, example_en, example_zh, root, group

`meaning` may contain several senses separated by ';' (only the first is shown
on the device). Audio files are looked up as <word>.mp3 / .wav / .m4a; anything
that is not MP3 is converted with ffmpeg to mono 32 kbps.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

VERSION = 3  # bank format: 32-byte index entries, 8 x uint32 (incl. pos)
HEADER = 32
INDEX_ENTRY = 28
COLUMNS = ["word", "phonetic", "pos", "meaning", "example_en", "example_zh", "root", "group"]


def read_rows(path: Path) -> tuple[list[dict[str, str]], list[str]]:
    problems: list[str] = []
    rows: list[dict[str, str]] = []
    seen: dict[str, int] = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = [c for c in COLUMNS if c not in (reader.fieldnames or [])]
        if "word" not in (reader.fieldnames or []) or "meaning" not in (reader.fieldnames or []):
            raise SystemExit(f"CSV needs at least 'word' and 'meaning' columns, got {reader.fieldnames}")
        for line_no, row in enumerate(reader, start=2):
            word = (row.get("word") or "").strip()
            meaning = (row.get("meaning") or "").strip()
            if not word or not meaning:
                problems.append(f"line {line_no}: missing word/meaning, skipped")
                continue
            key = word.casefold()
            if key in seen:
                problems.append(f"line {line_no}: duplicate '{word}', kept first")
                continue
            seen[key] = line_no
            rows.append({c: (row.get(c) or "").strip() for c in COLUMNS})
    return rows, problems


def find_audio(audio_dir: Path | None, word: str) -> Path | None:
    if audio_dir is None:
        return None
    for ext in (".mp3", ".wav", ".m4a", ".ogg"):
        candidate = audio_dir / f"{word}{ext}"
        if candidate.is_file():
            return candidate
    return None


def convert_audio(source: Path, target: Path) -> bool:
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.suffix.lower() == ".mp3":
        shutil.copyfile(source, target)
        return True
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return False
    result = subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-i", str(source),
         "-ac", "1", "-ar", "24000", "-b:a", "32k", str(target)],
        capture_output=True,
    )
    return result.returncode == 0 and target.is_file()


def build_bank(rows: list[dict[str, str]]) -> bytes:
    strings = bytearray(b"\0")
    offsets: list[dict[str, int]] = []
    for row in rows:
        entry = {}
        for column in COLUMNS:
            value = row.get(column, "").encode("utf-8")
            entry[column] = len(strings)
            strings += value + b"\0"
        offsets.append(entry)

    index = bytearray()
    for entry in offsets:
        index += struct.pack(
            "<IIIIIIII",
            entry["word"], entry["meaning"], entry["example_en"], entry["example_zh"],
            entry["root"], entry["phonetic"], entry["group"], entry["pos"],
        )

    index_off = HEADER
    strings_off = index_off + len(index)
    payload = bytes(index) + bytes(strings)
    bank_hash = zlib.crc32(payload) & 0xFFFFFFFF

    head = bytearray(HEADER)
    head[0:4] = b"VOCB"
    struct.pack_into("<HH", head, 4, VERSION, len(rows))
    struct.pack_into("<I", head, 8, index_off)
    struct.pack_into("<I", head, 12, strings_off)
    struct.pack_into("<I", head, 16, bank_hash)
    struct.pack_into("<I", head, 28, zlib.crc32(bytes(head[:28])) & 0xFFFFFFFF)
    return bytes(head) + payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--audio-dir", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--no-audio", action="store_true")
    args = parser.parse_args()

    rows, problems = read_rows(args.csv)
    if not rows:
        print("no usable rows", file=sys.stderr)
        return 1

    out: Path = args.out
    audio_out = out / "audio"
    audio_out.mkdir(parents=True, exist_ok=True)
    (out / "bank.bin").write_bytes(build_bank(rows))

    missing_audio: list[str] = []
    converted = 0
    if not args.no_audio:
        for index, row in enumerate(rows):
            source = find_audio(args.audio_dir, row["word"])
            if source is None:
                missing_audio.append(row["word"])
                continue
            if convert_audio(source, audio_out / f"{index}.mp3"):
                converted += 1
            else:
                problems.append(f"{row['word']}: audio conversion failed")

    report = [
        f"words: {len(rows)}",
        f"audio converted: {converted}",
        f"missing audio: {len(missing_audio)}",
        f"bank bytes: {(out / 'bank.bin').stat().st_size}",
    ]
    if missing_audio:
        report.append("missing list: " + ", ".join(missing_audio[:40]))
    report.extend(problems)
    text = "\n".join(report) + "\n"
    (out / "report.txt").write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
