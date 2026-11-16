#!/usr/bin/env python3
"""Extract a ThingsOS source zip.

The official archive stores the bundled CJK font with a literal "??????.TTF"
name (the packer lost the Chinese filename), so we restore it by convention.
"""
import os
import pathlib
import sys
import zipfile

FONT_NAME = "方正屏显雅宋.TTF"


def extract(zip_path: str, dest: str) -> None:
    dest_path = pathlib.Path(dest)
    dest_path.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            name = info.filename
            if name.endswith("/"):
                (dest_path / name).mkdir(parents=True, exist_ok=True)
                continue
            target = dest_path / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as src, open(target, "wb") as dst:
                dst.write(src.read())

    fonts = dest_path / "fonts"
    if fonts.is_dir():
        expected = fonts / FONT_NAME
        if not expected.exists():
            ttf = [p for p in fonts.iterdir() if p.suffix.upper() == ".TTF"]
            if len(ttf) == 1:
                ttf[0].rename(expected)
                print(f"restored font name: {ttf[0].name} -> {FONT_NAME}")
    print(f"extracted {sum(1 for _ in dest_path.rglob('*'))} entries into {dest}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: extract_zip.py <source.zip> <dest-dir>", file=sys.stderr)
        sys.exit(2)
    extract(sys.argv[1], sys.argv[2])
