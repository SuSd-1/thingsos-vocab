#!/usr/bin/env python3
"""Generate word pronunciation with a local Piper voice.

    python3 vocab-kit/tools/make_audio.py --csv bank.csv --out my-audio \
        --model ~/piper-voices/en_US-lessac-medium.onnx

Each CSV `word` becomes `<out>/<word>.mp3` (mono, 24 kHz, 32 kbps).
Existing files are skipped, so interrupted runs can be resumed. With
`--examples` the `example_en` column is also spoken as `<word>_ex.mp3`.
"""
from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PIPER = os.environ.get("PIPER_BIN", str(Path.home() / ".venvs/piper/bin/piper"))


def synth(text: str, model: Path, out_mp3: Path) -> bool:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg is required to write MP3")
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        wav = Path(tmp.name)
    try:
        run = subprocess.run(
            [PIPER, "--model", str(model), "--output_file", str(wav)],
            input=text.encode("utf-8"),
            capture_output=True,
        )
        if run.returncode != 0 or not wav.is_file() or wav.stat().st_size == 0:
            return False
        out_mp3.parent.mkdir(parents=True, exist_ok=True)
        conv = subprocess.run(
            [ffmpeg, "-y", "-loglevel", "error", "-i", str(wav),
             "-ac", "1", "-ar", "24000", "-b:a", "32k", str(out_mp3)],
            capture_output=True,
        )
        return conv.returncode == 0 and out_mp3.is_file() and out_mp3.stat().st_size > 0
    finally:
        wav.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--examples", action="store_true", help="also speak example_en")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()

    if not args.model.is_file():
        print(f"voice model not found: {args.model}", file=sys.stderr)
        return 1
    if not Path(PIPER).is_file():
        print(f"piper binary not found: {PIPER}", file=sys.stderr)
        return 1

    with args.csv.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    jobs: list[tuple[str, str, Path]] = []
    for row in rows:
        word = (row.get("word") or "").strip()
        if not word:
            continue
        jobs.append((word, word, args.out / f"{word}.mp3"))
        if args.examples:
            sentence = (row.get("example_en") or "").strip()
            if sentence:
                jobs.append((sentence, f"{word}_ex", args.out / f"{word}_ex.mp3"))

    pending = [(text, key, path) for text, key, path in jobs if not path.is_file()]
    skipped = len(jobs) - len(pending)
    print(f"words={len(rows)} jobs={len(jobs)} pending={len(pending)} skipped={skipped}")

    failed: list[str] = []

    def work(item: tuple[str, str, Path]) -> tuple[str, bool]:
        text, key, path = item
        return key, synth(text, args.model, path)

    done = 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for key, ok in pool.map(work, pending):
            if ok:
                done += 1
            else:
                failed.append(key)

    total = sum(p.stat().st_size for _, _, p in jobs if p.is_file())
    print(f"generated={done} failed={len(failed)} bundled={total / 1024:.0f} KiB")
    if failed:
        print("failed list: " + ", ".join(failed[:40]))
    (args.out / "report.txt").write_text(
        f"jobs={len(jobs)} generated={done} failed={len(failed)} bytes={total}\n"
        + ("failed: " + ", ".join(failed) + "\n" if failed else ""),
        encoding="utf-8",
    )
    return 0 if not failed else 2


if __name__ == "__main__":
    sys.exit(main())
