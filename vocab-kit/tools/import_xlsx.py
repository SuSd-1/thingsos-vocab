#!/usr/bin/env python3
"""Import a workbook word list into the vocabulary bank CSV.

The workbook keeps the learner's own data: word, part of speech and Chinese
gloss. This script adds a phonetic transcription (offline CMU->IPA) and, when a
Tatoeba pair file is supplied, an example sentence + translation.

    ~/.venvs/piper/bin/python vocab-kit/tools/import_xlsx.py \
        --xlsx 单词背诵记录6591.xlsx --tatoeba ~/vocab-data/cmn.txt \
        --out build/book/bank.csv
"""
from __future__ import annotations

import argparse
import collections
import csv
import re
import sys
from pathlib import Path

import unicodedata

import openpyxl

# The workbook was produced by a tool that substituted Kangxi radicals and
# CJK compatibility ideographs for the real characters (⼈ for 人, ⽬ for 目).
# The device font has no glyphs for those blocks, so map them back.
_COMPAT_RANGES = ((0x2E80, 0x2EFF), (0x2F00, 0x2FDF), (0xF900, 0xFAFF), (0xFE30, 0xFE4F))

# CJK Radicals Supplement has no NFKC mapping of its own.
_RARE = {"幚": "帮", "覚": "觉", "迺": "乃", "囓": "啮", "蹚": "涉", "戶": "户"}

_RADICALS = {
    "⻋": "车", "⻓": "长", "⻛": "风", "⺠": "民", "⻅": "见", "⻝": "食",
    "⻔": "门", "⻢": "马", "⻜": "飞", "⻆": "角", "⻘": "青", "⻥": "鱼",
    "⻩": "黄", "⻣": "骨", "⻤": "鬼", "⻦": "鸟", "⻮": "齿", "⻬": "齐",
    "⻰": "龙", "⻉": "贝", "⻨": "麦", "⻚": "页", "㬵": "胶",
}


def normalize_cjk(text: str) -> str:
    out = []
    for ch in text:
        out.append(_RADICALS.get(ch) or (unicodedata.normalize("NFKC", ch)
                                          if any(lo <= ord(ch) <= hi for lo, hi in _COMPAT_RANGES) else ch))
    return "".join(out)


def to_simplified(text: str) -> str:
    """The device font is simplified-only; Tatoeba pairs are often traditional."""
    try:
        from opencc import OpenCC
    except Exception:
        return text
    text = OpenCC("t2s").convert(text)
    return "".join(_RARE.get(ch, ch) for ch in text)

POS_RE = re.compile(r"^(n|v|vt|vi|adj|adv|prep|conj|pron|num|int|art|aux|phr|abbr)\.\s*", re.I)
POS_ALIASES = {"a.": "adj.", "ad.": "adv."}


POS_GROUP_RE = re.compile(
    r"(?:n|v|vt|vi|adj|adv|prep|conj|pron|num|int|art|aux|phr|abbr|det|vlink)\.", re.I)


def split_senses(text: str) -> list[str]:
    """Split a gloss into one line per part of speech.

    "adj. 根本的 n. 激进分子"  ->  ["adj. 根本的", "n. 激进分子"]
    "vt. vi. 散发；发出 vi. 呈辐射状"  ->  ["vt. vi. 散发；发出", "vi. 呈辐射状"]
    """
    text = " ".join(text.split())

    def has_sense(segment: str) -> bool:
        return POS_GROUP_RE.sub("", segment).strip() not in ("", ";", ",", "；", "，")

    segments: list[str] = []
    current = ""
    i = 0
    n = len(text)
    while i < n:
        match = POS_GROUP_RE.match(text, i)
        boundary = i == 0 or text[i - 1] in " ;；,，、()（）"
        if match and boundary and has_sense(current):
            segments.append(current.strip())
            current = ""
        if match:
            # take the whole run of consecutive markers as this line's prefix
            j = match.end()
            prefix = text[i:j]
            while True:
                k = j
                while k < n and text[k] == " ":
                    k += 1
                nxt = POS_GROUP_RE.match(text, k)
                if not nxt:
                    break
                prefix += " " + text[k:nxt.end()]
                j = nxt.end()
            current += prefix
            i = j
            continue
        current += text[i]
        i += 1
    if current.strip():
        segments.append(current.strip())
    return segments or [text]


def phonetic(word: str) -> str:
    try:
        import eng_to_ipa as ipa
    except Exception:
        return ""
    try:
        result = ipa.convert(word.lower())
    except Exception:
        return ""
    if "*" in result:  # unknown word
        return ""
    return f"/{result.strip()}/"


def load_pairs(path: Path) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 2 and cols[0] and cols[1]:
                pairs.append((cols[0].strip(), cols[1].strip()))
    return pairs


def variants(word: str) -> set[str]:
    w = word.lower()
    out = {w, w + "s", w + "es", w + "ed", w + "ing", w + "d"}
    if w.endswith("e"):
        out |= {w[:-1] + "ing", w + "d"}
    if w.endswith("y"):
        out |= {w[:-1] + "ies", w[:-1] + "ied"}
    return out


def build_examples(pairs: list[tuple[str, str]]) -> dict[str, tuple[str, str]]:
    index: dict[str, list[tuple[str, str]]] = collections.defaultdict(list)
    for en, zh in pairs:
        for token in set(re.findall(r"[a-zA-Z][a-zA-Z'-]+", en.lower())):
            index[token].append((en, zh))
    chosen: dict[str, tuple[str, str]] = {}

    def pick(word: str) -> tuple[str, str] | None:
        best = None
        for variant in variants(word):
            for en, zh in index.get(variant, ())[:40]:
                n = len(en.split())
                if 4 <= n <= 16:
                    score = abs(n - 9)
                    if best is None or score < best[0]:
                        best = (score, en, zh)
        return (best[1], best[2]) if best else None

    return chosen, pick


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xlsx", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--tatoeba", type=Path)
    parser.add_argument("--sheet", default="单词列表")
    args = parser.parse_args()

    workbook = openpyxl.load_workbook(args.xlsx, read_only=True, data_only=True)
    sheet = workbook[args.sheet]
    entries: list[dict[str, str]] = []
    seen: set[str] = set()
    for row in sheet.iter_rows(min_row=3, values_only=True):
        if len(row) < 5:
            continue
        word, pos, meaning = row[2], row[3], row[4]
        if not word or not meaning:
            continue
        word = normalize_cjk(str(word).strip())
        if not word or word.lower() in seen:
            continue
        seen.add(word.lower())
        pos_text = str(pos or "").strip()
        pos_text = POS_ALIASES.get(pos_text, pos_text)
        # Keep the gloss verbatim: it carries every part of speech the book lists
        # (e.g. "vt. vi. 散发… vi. 呈辐射状…"), which a single pos column cannot.
        entries.append({
            "word": word,
            "phonetic": phonetic(word),
            "pos": pos_text,
            "meaning": "\n".join(split_senses(to_simplified(normalize_cjk(str(meaning))))),
            "example_en": "",
            "example_zh": "",
            "root": "",
            "group": "",
            "derived": "",
            "phrase": "",
        })

    covered = 0
    if args.tatoeba and args.tatoeba.is_file():
        pairs = load_pairs(args.tatoeba)
        _, pick = build_examples(pairs)
        for entry in entries:
            found = pick(entry["word"])
            if found:
                entry["example_en"], entry["example_zh"] = found[0], to_simplified(normalize_cjk(found[1]))
                covered += 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    columns = ["word", "phonetic", "pos", "meaning", "example_en", "example_zh",
               "root", "group", "derived", "phrase"]
    with args.out.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(entries)

    print(f"words={len(entries)} duplicates_skipped={len(seen) - len(entries)}")
    print(f"phonetic={sum(1 for e in entries if e['phonetic'])}")
    print(f"examples={covered}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
