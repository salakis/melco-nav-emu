# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""scripts/verify-hdd.py's FAT32 reader against images the project's own builder writes."""
from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_reads_map_partition_files_and_lists_extras(tmp_path):
    builder = load("make_hdd_image", ROOT / "scripts/make-hdd-image.py")
    verify = load("verify_hdd", ROOT / "scripts/verify-hdd.py")
    par = tmp_path / "par"
    par.write_bytes(bytes(range(256)) * 37)          # several 512 B clusters
    idx = tmp_path / "idx"
    idx.write_bytes(b"IDX" * 1000)
    junk = tmp_path / "junk"
    junk.write_bytes(b"x")
    out = tmp_path / "two.img"
    builder.build(out, 192, "VOL", [], fat=32, part2_mb=128, label2="MAP",
                  files2=[(par, "Map\\PAR_MNG.ME"), (idx, "Map\\IDX\\ZONZSRC.IDX"),
                          (junk, "Map\\JUNK.BIN")])
    with out.open("rb") as f:
        fat = verify.Fat32(f, 2)
        attr, first, size = fat.lookup("Map\\PAR_MNG.ME")
        assert size == par.stat().st_size
        assert b"".join(fat.reader(first, size)) == par.read_bytes()
        attr, first, size = fat.lookup("Map\\IDX\\ZONZSRC.IDX")
        assert b"".join(fat.reader(first, size)) == idx.read_bytes()
        assert fat.lookup("Map\\REGION.KWI") is None
        targets = [{"name": "PAR_MNG.ME"}, {"name": "ZONZSRC.IDX"}]
        assert verify.extra_files(fat, targets) == ["MAP\\JUNK.BIN"]
