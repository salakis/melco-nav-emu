#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Land the completed-map-install record in a unit's APM backup areas.

The guest keeps its map-install state (APM) in backup areas that begin with
an 8-byte ASCII version date (20080207): three in the 32 MiB DRF flash
image (0x1e40800, 0x1e80800, 0x1ec0800) and one in the 1 MiB battery RAM
file (0x40800).  Each area holds two records 0x540 bytes apart, starting at
area + 0x10: a 0x538-byte body, then the sum of its little-endian 32-bit
words and that sum's complement.

In every non-erased record of every APM area found, this sets the install
status MapDwlSts (+0x370) to 0 ("install complete") and the map base
(+0x80) and serial (+0x83) to the installed disc's, then rewrites the
checksums.  Base and serial come from --base/--serial, else from the
disc's ALLDATA.KWI (+0x53..0x54 base as two ASCII digits, +0x56 serial).

    apm_record_seed.py SRC DST        # SRC may be DST: a flash.img or bkram.bin
    apm_record_seed.py --dump FILE    # list the backup areas and APM records
"""
import argparse
import os
import struct
import sys
from pathlib import Path

STRIDE = 0x540          # record slot stride
BODY = 0x538            # checksummed body
FLASH_BACKUP = 0x1c00000  # flash: the backup areas sit in the top 4 MiB
APM_DATE = b"20080207"  # version date of the APM areas
# Version dates of the guest's backup areas (APM and others), for --dump.
AREA_DATES = (APM_DATE, b"20021210", b"20051228", b"20061226")
BASE_OFF = 0x80         # record body: map base number
SERIAL_OFF = 0x83       # record body: map serial number
MAPDWLSTS_OFF = 0x370   # record body: install status (0 = complete)
COMPLETED_MAPDWLSTS = 0
DEFAULT_BASE, DEFAULT_SERIAL = 0x31, 0x35   # used without ALLDATA.KWI
DEFAULT_ALLDATA = Path(__file__).resolve().parents[1] / "images/dvd1/ALLDATA.KWI"


def body_sum(b):
    return sum(struct.unpack_from("<I", b, i)[0] for i in range(0, BODY, 4)) & 0xFFFFFFFF


def find_areas(d, dates=AREA_DATES):
    """Offsets of backup areas: one of `dates` at the area's offset 0.

    A 32 MiB flash image is searched in its top 4 MiB, anything smaller
    (the 1 MiB battery RAM) as a whole."""
    start = FLASH_BACKUP if len(d) > FLASH_BACKUP else 0
    out = set()
    for date in dates:
        off = d.find(date, start)
        while off != -1:
            out.add(off)
            off = d.find(date, off + 1)
    return sorted(out)


def disc_identity(alldata):
    """(base, serial) a real install writes into the APM record, read from
    the installed map's ALLDATA.KWI frame (+0x53..0x54 base as 2 ASCII
    digits, +0x56 serial)."""
    h = Path(alldata).read_bytes()[:0x58]
    base = int(h[0x53:0x55].decode("ascii", "replace").strip() or "255")
    return base, h[0x56]


def seed(src, dst, base=DEFAULT_BASE, serial=DEFAULT_SERIAL, date=APM_DATE):
    """Land status 0 plus base/serial in every APM record of `src`; write
    the result to `dst` (replaced in one step)."""
    d = bytearray(Path(src).read_bytes())
    areas = find_areas(d, (date,))
    if not areas:
        raise SystemExit("no APM area (version date %s) found in %s"
                         % (date.decode("ascii", "replace"), src))
    print("APM areas:", [hex(a) for a in areas])
    for area in areas:
        for n in range(2):
            rec = area + n * STRIDE + 0x10
            body = bytearray(d[rec:rec + BODY])
            if bytes(body) == b"\xff" * BODY:
                print("  record %#x erased, skipped" % rec)
                continue
            body[MAPDWLSTS_OFF] = COMPLETED_MAPDWLSTS
            body[BASE_OFF] = base & 0xFF
            body[SERIAL_OFF] = serial & 0xFF
            s = body_sum(body)
            d[rec:rec + BODY] = body
            struct.pack_into("<II", d, rec + BODY, s, (~s) & 0xFFFFFFFF)
            print("  record %#x base=%d serial=%d sum=%08x"
                  % (rec, base & 0xFF, serial & 0xFF, s))
    dst = Path(dst)
    tmp = dst.with_name(dst.name + ".seed-tmp")
    tmp.write_bytes(bytes(d))
    os.replace(tmp, dst)


def dump(path, dates=AREA_DATES):
    d = Path(path).read_bytes()
    for base in find_areas(d, dates):
        print("area %#x header=%s" % (base, d[base:base + 0x10].hex()))
        if d[base:base + 8] != APM_DATE:
            continue
        for n in range(2):
            rec = base + n * STRIDE + 0x10
            body = d[rec:rec + BODY]
            s = body_sum(body)
            stored = struct.unpack_from("<II", d, rec + BODY)
            print("  rec %#x body[0:0x20]=%s" % (rec, body[:0x20].hex()))
            print("    sum=%08x stored=%08x/%08x %s" %
                  (s, stored[0], stored[1], "OK" if s == stored[0] and
                   (~s) & 0xFFFFFFFF == stored[1] else "NG"))
            print("    MapDwlSts(+0x%x)=%d base(+0x%x)=%d serial(+0x%x)=%d" %
                  (MAPDWLSTS_OFF, body[MAPDWLSTS_OFF], BASE_OFF, body[BASE_OFF],
                   SERIAL_OFF, body[SERIAL_OFF]))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--dump", action="store_true",
                    help="list the backup areas in SRC and the APM records")
    ap.add_argument("--date", default=None,
                    help="only areas carrying this version date (default: "
                         "seed the APM areas, %s; --dump lists every known "
                         "backup area)" % APM_DATE.decode())
    ap.add_argument("--base", type=int, default=None,
                    help="map base for body +0x80 (default: from ALLDATA.KWI)")
    ap.add_argument("--serial", type=int, default=None,
                    help="map serial for body +0x83 (default: from ALLDATA.KWI)")
    ap.add_argument("--alldata", type=Path, default=DEFAULT_ALLDATA,
                    help="installed map frame whose +0x53/+0x56 give base/serial "
                         "(default: images/dvd1/ALLDATA.KWI)")
    a = ap.parse_args()
    date = a.date.encode() if a.date else None
    if a.dump:
        dump(a.src, (date,) if date else AREA_DATES)
        return 0
    if not a.dst:
        ap.error("dst is required unless --dump")
    if (a.base is None) != (a.serial is None):
        ap.error("--base and --serial go together")
    if a.base is not None:
        base, serial = a.base, a.serial
    elif a.alldata.is_file():
        base, serial = disc_identity(a.alldata)
        print("disc identity: base=%d serial=%d (%s +0x53/+0x56)"
              % (base, serial, a.alldata))
    else:
        base, serial = DEFAULT_BASE, DEFAULT_SERIAL
        print("no %s: default map identity base=%d serial=%d"
              % (a.alldata, base, serial))
    seed(a.src, a.dst, base, serial, date or APM_DATE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
