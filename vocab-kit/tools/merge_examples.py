#!/usr/bin/env python3
"""Merge generated example sentences back into the bank CSV.

Reads every `build/book/batches/ex-*.tsv` (word, example_en, example_zh) and
fills the matching row in the bank CSV, validating that the headword really
appears in the sentence. Rows that fail validation keep their previous example
(or stay empty) and are listed in the report.

    python3 vocab-kit/tools/merge_examples.py --csv build/book/bank.csv \
        --examples build/book/batches --out build/book/bank.csv
"""
from __future__ import annotations

import argparse
import csv
import glob
import re
import sys
from pathlib import Path

CJK = re.compile(r"[\u4e00-\u9fff]")

_RARE = {"幚": "帮", "覚": "觉", "迺": "乃", "囓": "啮", "蹚": "涉", "戶": "户"}


def fix_zh(text: str) -> str:
    try:
        from import_xlsx import normalize_cjk, to_simplified
        text = to_simplified(normalize_cjk(text))
    except Exception:
        pass
    return "".join(_RARE.get(ch, ch) for ch in text)


def headword_forms(word: str) -> set[str]:
    w = word.lower().strip()
    forms = {w}
    if " " in w:
        return forms
    if w.endswith("e"):
        forms |= {w + "d", w + "s", w[:-1] + "ing"}
    if w.endswith("y"):
        forms |= {w[:-1] + "ies", w[:-1] + "ied"}
    forms |= {w + "s", w + "es", w + "ed", w + "ing"}
    return forms


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--examples", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    rows = list(csv.DictReader(args.csv.open("r", encoding="utf-8")))
    by_word = {row["word"].lower(): row for row in rows}

    generated = 0
    bad: list[str] = []
    seen_words: set[str] = set()
    files = sorted(glob.glob(str(args.examples / "ex-*.tsv")))
    files = [f for f in files if not re.search(r"\.part\d+\.", f)]
    for path in files:
        with open(path, "r", encoding="utf-8") as handle:
            first = handle.readline()
            handle.seek(0)
            if "example_en" in first.split("\t")[:3]:
                handle.readline()  # skip header row
            for line in handle:
                cols = line.rstrip("\n").split("\t")
                if len(cols) < 3:
                    continue
                word, en, zh = cols[0].strip(), cols[1].strip(), cols[2].strip()
                row = by_word.get(word.lower())
                if row is None:
                    bad.append(f"{word}: not in bank")
                    continue
                if word.lower() in seen_words:
                    bad.append(f"{word}: duplicate")
                    continue
                words = en.split()
                if not (4 <= len(words) <= 18):
                    bad.append(f"{word}: bad length")
                    continue
                if not any(form in en.lower() for form in headword_forms(word)):
                    bad.append(f"{word}: headword missing")
                    continue
                if not CJK.search(zh):
                    bad.append(f"{word}: no chinese")
                    continue
                row["example_en"] = en
                row["example_zh"] = fix_zh(zh)
                seen_words.add(word.lower())
                generated += 1

    with args.out.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    filled = sum(1 for row in rows if row["example_en"].strip())
    print(f"files={len(files)} merged={generated} rejected={len(bad)}")
    print(f"rows_with_example={filled}/{len(rows)}")
    if bad:
        print("rejected sample: " + "; ".join(bad[:20]))
        (Path(str(args.out) + ".rejected.txt")).write_text("\n".join(bad), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
