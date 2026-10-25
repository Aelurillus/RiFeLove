#!/usr/bin/env python3
"""Wrap a 256x256 PNG in a single-image Windows ICO container."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("png", type=Path)
    parser.add_argument("ico", type=Path)
    parser.add_argument("--force", action="store_true", help="replace an existing output icon")
    args = parser.parse_args()

    if args.ico.exists() and not args.force:
        parser.error(f"refusing to replace existing output: {args.ico}")

    png = args.png.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n" or len(png) < 24:
        parser.error("input is not a valid PNG image")
    width, height = struct.unpack(">II", png[16:24])
    if (width, height) != (256, 256):
        parser.error(f"expected a 256x256 PNG, got {width}x{height}")

    args.ico.parent.mkdir(parents=True, exist_ok=True)
    entry = struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(png), 22)
    args.ico.write_bytes(struct.pack("<HHH", 0, 1, 1) + entry + png)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
