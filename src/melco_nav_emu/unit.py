#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""A unit: one head unit's own non-volatile state, kept in a directory.

    flash.img   32 MiB DRF NOR flash (a new unit's is erased)
    bkram.bin   1 MiB battery RAM (CU-P2006 production board: the guest's
                live backup areas, including the APM map-install record)
    sram.bin    64 KiB battery SRAM (GPS backup)
    eeprom.bin  onboard EEPROM (128 B CU-P2006, 1 MiB CUP28 backup
                EEPROM)
    hdd.img     factory units only: the unit's own disk

    python -m melco_nav_emu.unit new DIR       # create/prepare a unit
    python -m melco_nav_emu.unit factory DIR   # erased flash + a small empty HDD
                                               # (for experiments: the guest's own
                                               # map install needs a ~24 GB disk)
    python -m melco_nav_emu.unit show DIR      # what the unit's backups say

The board-specific records a unit carries (the CU-P2006 APM map-install
record) are landed by the board module's prepare_unit() hook, which
prepare() calls.
"""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
FLASH_SIZE = 32 << 20
# Written when a unit's first power-on timed out: do not power it on again.
POWERED_ON = ".powered-on"


def _empty_hdd() -> Path:
    """A formatted, empty two-partition HDD image (build/hdd/, built once)."""
    from .boards.cup2006 import build_atomically
    out = ROOT / "build/hdd/hdd-formatted-empty.img"
    if not out.exists():
        build_atomically(out, [
            sys.executable, str(ROOT / "scripts/make-hdd-image.py"),
            "--size-mb", "128", "--fat", "32",
            "--label", "VOL", "--part2-mb", "64", "--label2", "MAP",
        ])
    return out


def _erased_flash(path: Path) -> None:
    Path(path).write_bytes(b"\xff" * FLASH_SIZE)


def factory(unit, hdd_base=None):
    """A unit as it leaves the factory: erased flash, empty battery RAM, and
    its own copy of a formatted, empty HDD (the guest then installs the map
    onto it itself)."""
    unit = Path(unit)
    unit.mkdir(parents=True, exist_ok=False)
    _erased_flash(unit / "flash.img")
    hdd = unit / "hdd.img"
    hdd_base = hdd_base or _empty_hdd()
    if subprocess.call(["cp", "-c", str(hdd_base), str(hdd)],
                       stderr=subprocess.DEVNULL):
        shutil.copyfile(hdd_base, hdd)
    (unit / ".factory").write_text("factory unit: empty flash and HDD\n")
    return unit


def prepare(unit, board=None):
    """Create the unit's files on first use; the board module lands its own
    records in them (board.prepare_unit(unit)).

    A new unit starts with erased flash, as a factory one does; the guest
    formats its backup areas itself.
    """
    unit = Path(unit)
    unit.mkdir(parents=True, exist_ok=True)
    flash = unit / "flash.img"
    if not flash.exists():
        _erased_flash(flash)
    if (unit / ".factory").exists():
        return unit
    if board is not None:
        from . import boards
        module = boards.get(board) if isinstance(board, str) else board
        module.prepare_unit(unit)
    return unit


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("new", "show", "factory"):
        print(__doc__)
        return 2
    from .boards import cup2006
    unit = Path(sys.argv[2])
    if sys.argv[1] == "new":
        prepare(unit, "cup2006")
    elif sys.argv[1] == "factory":
        factory(unit)
    flash = unit / "flash.img"
    print("%s: flash %s, MapDwlSts flash %s / battery RAM %s, installed "
          "marker %s, factory %s, powered-on marker %s, sram %s, hdd %s" % (
              unit, "yes" if flash.exists() else "no",
              cup2006.map_dwl_sts(flash) if flash.exists() else "-",
              cup2006.bkram_dwl_sts(unit / "bkram.bin"),
              (unit / ".installed").exists(), (unit / ".factory").exists(),
              (unit / POWERED_ON).exists(),
              (unit / "sram.bin").exists(),
              (unit / "hdd.img").exists()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
