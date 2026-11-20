#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Carve LOADING.KWI modules and flatten B000FF containers to NK.NB0."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "src"))

from melco_nav_emu.images import flatten_b000ff, parse_b000ff  # noqa: E402
from melco_nav_emu.loading import parse_loading  # noqa: E402


def safe_stem(name: str, version: str) -> str:
    label = version if version.startswith("v") else "v" + version
    stem = "%s_%s" % (name, label)
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in stem)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loading", required=True, help="path to LOADING.KWI")
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument(
        "--raw-only", action="store_true", help="write only the B000FF bodies"
    )
    args = parser.parse_args()

    data = Path(args.loading).read_bytes()
    image = parse_loading(data)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print("LOADING.KWI: %d B, %d system(s), %d module(s)"
          % (len(data), len(image.systems), len(image.modules)))
    for mod in image.modules:
        body = image.body(mod)
        stem = safe_stem(mod.name, mod.version)
        raw_path = out / (stem + ".bin")
        raw_path.write_bytes(body)
        print("  %-34s %8d B  DSA=%#010x  -> %s"
              % (mod.name, len(body), mod.dsa, raw_path.name))
        if not args.raw_only:
            try:
                container = parse_b000ff(body)
            except ValueError as exc:
                print("    not B000FF: %s" % exc)
                continue
            nb0 = flatten_b000ff(body)
            nb0_path = out / (stem + ".nb0")
            nb0_path.write_bytes(nb0)
            print("    B000FF ImageStart=%#010x ImageLength=%#x exec=%#x "
                  "records=%d -> %s"
                  % (container.image_start, container.image_length,
                     container.exec_start, len(container.records),
                     nb0_path.name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
