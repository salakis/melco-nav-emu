#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""A unit: one head unit's own non-volatile state, kept in a directory.

    flash.img   32 MiB DRF NOR flash (a new unit's is erased)
    bkram.bin   1 MiB battery RAM (production board: the guest's live
                backup areas, including the APM map-install record)
    sram.bin    64 KiB battery SRAM (GPS backup)
    eeprom.bin  128 B onboard EEPROM
    hdd.img     factory units only: the unit's own disk

    python3 tools/unit.py new DIR       # create/prepare a unit
    python3 tools/unit.py factory DIR   # erased flash + a small empty HDD
                                        # (for experiments: the guest's own
                                        # map install needs a ~24 GB disk)
    python3 tools/unit.py show DIR      # what the unit's backups say
"""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 32 << 20
# The guest's APM (map-install) backup areas start with this ASCII version
# date, followed by two records.  Flash holds three such areas (0x1e40800,
# 0x1e80800, 0x1ec0800); battery RAM holds one, at 0x40800, which
# corresponds to the first flash area.  The status checks below read that
# pair.
APM_DATE = b"20080207"
FLASH_APM_AREA = 0x1e40800    # first APM area in flash
BKRAM_APM_AREA = 0x40800      # the APM area in battery RAM (the one the
                              # guest reads on a production board)
RECORD = 0x540                # record stride; body at area + 0x10 + n * RECORD
MAPDWLSTS = 0x370             # install-status byte in the record body
# Written when a unit's first power-on timed out: do not power it on again.
POWERED_ON = ".powered-on"


def _empty_hdd() -> Path:
    """A formatted, empty two-partition HDD image (build/hdd/, built once)."""
    from tools.profile import build_atomically
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


def _dwl_sts(path, area):
    path = Path(path)
    if not path.is_file():
        return None
    want = 0x10 + 2 * RECORD
    with open(path, "rb") as f:
        f.seek(area)
        data = f.read(want)
    if len(data) < want or data[:8] != APM_DATE:
        return None
    return [data[0x10 + n * RECORD + MAPDWLSTS] for n in (0, 1)]


def map_dwl_sts(flash):
    """Install status of both records of the first APM area in flash, or
    None if the area is not guest-formatted."""
    return _dwl_sts(flash, FLASH_APM_AREA)


def bkram_dwl_sts(bkram):
    """Install status of both APM records in battery RAM, or None if the
    guest has not formatted its backup areas there yet."""
    return _dwl_sts(bkram, BKRAM_APM_AREA)


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


def prepare(unit, first_power_on=None):
    """Create the unit's files on first use and land the install record.

    A new unit starts with erased flash, as a factory one does; the guest
    formats its backup areas itself.

    STUB (the HDD this emulator serves already holds the complete install,
    so the unit's record says so): status 0 ("install complete") plus the
    disc's map identity, re-landed at every boot in every guest-formatted
    APM area (flash and battery RAM) by scripts/apm_record_seed.py; factory
    units are left alone.

    On a production board the guest reads the record from battery RAM, and a
    new unit's battery RAM is blank: the guest formats it on its first
    power-on and records "no map installed".  `first_power_on(unit)`, when
    given, runs that first power-on headless so the record can be landed
    before the unit is used.  It is skipped for a unit carrying the
    .powered-on marker (an earlier first power-on timed out).
    """
    unit = Path(unit)
    unit.mkdir(parents=True, exist_ok=True)
    flash = unit / "flash.img"
    if not flash.exists():
        _erased_flash(flash)
    if (unit / ".factory").exists():
        return unit
    bkram = unit / "bkram.bin"
    if (first_power_on is not None and bkram_dwl_sts(bkram) is None
            and not (unit / POWERED_ON).exists()):
        first_power_on(unit)
    stores = [p for p, sts in ((flash, map_dwl_sts(flash)),
                               (bkram, bkram_dwl_sts(bkram))) if sts is not None]
    if not stores:
        return unit                    # the guest formats it on this boot
    for store in stores:
        subprocess.check_call([sys.executable,
                               str(ROOT / "scripts/apm_record_seed.py"),
                               str(store), str(store)],
                              stdout=subprocess.DEVNULL)
    (unit / ".installed").write_text(
        "map-install record landed (tools/unit.py prepare)\n")
    return unit


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("new", "show", "factory"):
        print(__doc__)
        return 2
    sys.path.insert(0, str(ROOT))
    unit = Path(sys.argv[2])
    if sys.argv[1] == "new":
        from tools.profile import first_power_on
        prepare(unit, first_power_on=first_power_on)
    elif sys.argv[1] == "factory":
        factory(unit)
    flash = unit / "flash.img"
    print("%s: flash %s, MapDwlSts flash %s / battery RAM %s, installed "
          "marker %s, factory %s, powered-on marker %s, sram %s, hdd %s" % (
              unit, "yes" if flash.exists() else "no",
              map_dwl_sts(flash) if flash.exists() else "-",
              bkram_dwl_sts(unit / "bkram.bin"),
              (unit / ".installed").exists(), (unit / ".factory").exists(),
              (unit / POWERED_ON).exists(),
              (unit / "sram.bin").exists(),
              (unit / "hdd.img").exists()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
