#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Parse the P2006.SCE install script into a machine-readable manifest."""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "src"))

from melco_nav_emu.images import load_sce  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sce", required=True, help="path to P2006.SCE")
    parser.add_argument("--out", required=True, help="output JSON manifest")
    args = parser.parse_args()

    script = load_sce(Path(args.sce))
    manifest = {
        "labels": script.labels,
        "targets": [
            {
                "name": target.name,
                "source": target.source,
                "total_size": target.total_size,
                "count": target.count,
                "files": [
                    {
                        "file": copy.file,
                        "flag": copy.flag,
                        "size1": copy.size1,
                        "size2": copy.size2,
                        "checksum": copy.checksum,
                    }
                    for copy in target.files
                ],
            }
            for target in script.targets
        ],
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Write a temporary file and rename it: an interrupted run never leaves
    # a truncated manifest behind.
    tmp = out.with_name(out.name + ".partial")
    tmp.write_text(json.dumps(manifest, indent=2) + "\n")
    os.replace(tmp, out)

    print("labels: %s" % (", ".join(script.labels) or "-"))
    for target in script.targets:
        print("%-18s %12d B  %4d files" %
              (target.name, target.total_size, len(target.files)))
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
