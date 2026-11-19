#!/usr/bin/env python3
"""Build a full flash image: ThingsOS v0.2.0 base + Vocab app at 0x220000.

Usage: make_full_image.py BASE.bin APP.bin OUT.bin
The NVS region (0x9000..0xF000) and the app tail up to 0xD00000 are blanked,
so a fresh flash starts with the firmware defaults and no personal data.
"""
import pathlib
import sys

APP_OFF = 0x220000
FONT_OFF = 0xD00000
NVS_OFF = 0x9000
NVS_LEN = 0x6000


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    base = bytearray(pathlib.Path(sys.argv[1]).read_bytes())
    app = pathlib.Path(sys.argv[2]).read_bytes()
    if len(base) < FONT_OFF:
        raise SystemExit("base image is too short")
    if len(app) > FONT_OFF - APP_OFF:
        raise SystemExit("app does not fit in ota_0")
    base[APP_OFF:APP_OFF + len(app)] = app
    base[APP_OFF + len(app):FONT_OFF] = b"\xff" * (FONT_OFF - APP_OFF - len(app))
    base[NVS_OFF:NVS_OFF + NVS_LEN] = b"\xff" * NVS_LEN
    pathlib.Path(sys.argv[3]).write_bytes(bytes(base))
    print(f"wrote {sys.argv[3]} ({len(base)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
