#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Verify an installed CU-P2006 HDD image against the retail discs."""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = spec_from_file_location("make_hdd_map", ROOT / "scripts" / "make-hdd-map.py")
assert _spec is not None and _spec.loader is not None
hddmap = module_from_spec(_spec)
_spec.loader.exec_module(hddmap)

SECTOR = 512
# Installed paths where the guest's own open differs from installed_path()'s
# naming rule (the product profile's MAP_DEST).
sys.path.insert(0, str(ROOT / "src"))
from melco_nav_emu.boards.cup2006 import MAP_DEST  # noqa: E402


# The guest's installer stages every target under HDD2\UPDATE\ first and
# renames UPDATE to Map on a later boot; --folder UPDATE checks a disk in
# that staged state.
FOLDER = "Map"


def target_path(name: str) -> str:
    path = MAP_DEST.get(name, hddmap.installed_path(name))
    if FOLDER != "Map" and path.upper().startswith("MAP\\"):
        path = FOLDER + path[3:]
    return path


class Fat32:
    """Minimal FAT32 reader for one MBR partition of a raw image."""

    def __init__(self, f, partition: int):
        self.f = f
        f.seek(0)
        mbr = f.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            raise SystemExit("no MBR signature")
        entry = mbr[446 + 16 * (partition - 1):446 + 16 * partition]
        ptype = entry[4]
        if ptype not in (0x0B, 0x0C):
            raise SystemExit("partition %d type 0x%02x is not FAT32" % (partition, ptype))
        self.base = struct.unpack_from("<I", entry, 8)[0] * SECTOR
        f.seek(self.base)
        bs = f.read(SECTOR)
        if bs[0x52:0x5A] != b"FAT32   ":
            raise SystemExit("partition %d boot sector is not FAT32" % partition)
        bps, spc, rsv, nfats = struct.unpack_from("<HBHB", bs, 11)
        fatsz = struct.unpack_from("<I", bs, 36)[0]
        self.root = struct.unpack_from("<I", bs, 44)[0]
        self.csize = bps * spc
        self.fat_off = self.base + rsv * bps
        self.data_off = self.fat_off + nfats * fatsz * bps
        f.seek(self.fat_off)
        raw = f.read(fatsz * bps)
        self.fat = struct.unpack("<%dI" % (len(raw) // 4), raw)

    def chain(self, first: int) -> list[int]:
        out, c = [], first
        while 2 <= c < 0x0FFFFFF8:
            out.append(c)
            c = self.fat[c] & 0x0FFFFFFF
            if len(out) > len(self.fat):
                raise SystemExit("FAT loop at cluster %d" % first)
        return out

    def runs(self, first: int):
        """(byte offset, length) runs of a cluster chain, contiguous runs merged."""
        runs = []
        for c in self.chain(first):
            off = self.data_off + (c - 2) * self.csize
            if runs and runs[-1][0] + runs[-1][1] == off:
                runs[-1][1] += self.csize
            else:
                runs.append([off, self.csize])
        return runs

    def listdir(self, first: int) -> dict[str, tuple[int, int, int]]:
        """8.3 name -> (attr, first cluster, size); LFN and deleted entries skipped."""
        data = b"".join(self._read(o, n) for o, n in self.runs(first))
        out = {}
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            name = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            full = name + ("." + ext if ext else "")
            if full in (".", ".."):
                continue
            hi, lo = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0]
            out[full.upper()] = (e[11], (hi << 16) | lo, struct.unpack_from("<I", e, 28)[0])
        return out

    def lookup(self, path: str):
        cluster, entry = self.root, None
        for part in path.split("\\"):
            entries = self.listdir(cluster)
            entry = entries.get(part.upper())
            if entry is None:
                return None
            cluster = entry[1]
        return entry

    def _read(self, off: int, n: int) -> bytes:
        self.f.seek(off)
        return self.f.read(n)

    def reader(self, first: int, size: int):
        """Yield the file's bytes as it lies on disk, in order."""
        left = size
        for off, n in self.runs(first) if first else []:
            if left <= 0:
                break
            take = min(n, left)
            pos = 0
            while pos < take:
                step = min(take - pos, 16 << 20)
                yield self._read(off + pos, step)
                pos += step
            left -= take


def compare(fat: Fat32, target: dict, discs, indexes) -> dict:
    name = target["name"]
    path = target_path(name)
    entry = fat.lookup(path)
    res = {"target": name, "path": path, "size": target["total_size"]}
    if entry is None:
        res["status"] = "missing"
        return res
    attr, first, size = entry
    if size != target["total_size"]:
        res.update(status="size", on_disk=size)
        return res
    disk = fat.reader(first, size)
    buf = b""
    pos = diff_bytes = 0
    first_diff = None
    for cpy in target["files"]:
        iso, (lba, csize) = next((iso, ix[cpy["file"]]) for iso, ix in zip(discs, indexes)
                                 if cpy["file"] in ix)
        data = hddmap.read_cpy(iso, lba, csize, cpy["file"])
        if (len(data) != cpy["size1"] or hddmap.word_sum(data) != cpy["checksum"]):
            res["status"] = "disc-cpy-bad:%s" % cpy["file"]
            return res
        while len(buf) < len(data):
            chunk = next(disk, b"")
            if not chunk:
                break
            buf += chunk
        got, buf = buf[:len(data)], buf[len(data):]
        if got != data:
            n = min(len(got), len(data))
            d = sum(1 for a, b in zip(got[:n], data[:n]) if a != b) + abs(len(data) - len(got))
            if first_diff is None:
                first_diff = pos + next((i for i in range(n) if got[i] != data[i]), n)
            diff_bytes += d
        pos += len(data)
    res["status"] = "ok" if diff_bytes == 0 else "differs"
    if diff_bytes:
        res.update(diff_bytes=diff_bytes, first_diff=first_diff)
    return res


def extra_files(fat: Fat32, targets: list[dict]) -> list[str]:
    """Files on the map partition that no P2006.SCE target installs."""
    expected = {target_path(t["name"]).upper() for t in targets}
    found = []

    def walk(cluster: int, prefix: str):
        for name, (attr, first, _size) in sorted(fat.listdir(cluster).items()):
            path = prefix + name
            if attr & 0x10:
                walk(first, path + "\\")
            elif path.upper() not in expected:
                found.append(path)
    walk(fat.root, "")
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    ap.add_argument("--partition", type=int, default=2)
    ap.add_argument("--manifest", type=Path, default=ROOT / "images/hdd-manifest.json")
    ap.add_argument("--disc1", type=Path, default=ROOT / "images/dvd/disc1/EU-3350-00_1.ISO")
    ap.add_argument("--disc2", type=Path, default=ROOT / "images/dvd/disc2/EU-3350-00_2.ISO")
    ap.add_argument("--only", action="append", default=[], metavar="TARGET")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--folder", default="Map",
                    help="top folder of the targets (UPDATE: the installer's "
                         "staged copy before its rename)")
    args = ap.parse_args()
    global FOLDER
    FOLDER = args.folder
    discs = [args.disc1, args.disc2]
    indexes = [hddmap.iso_index(d) for d in discs]
    targets = hddmap.load_manifest_list(args.manifest)
    if args.only:
        targets = [t for t in targets if t["name"] in args.only]
    results = []
    t0 = time.time()
    with args.image.open("rb") as f:
        fat = Fat32(f, args.partition)
        for t in targets:
            r = compare(fat, t, discs, indexes)
            results.append(r)
            if r["status"] != "ok":
                print("%-14s %-8s %s" % (r["target"], r["status"],
                                         {k: v for k, v in r.items()
                                          if k not in ("target", "status", "path")}),
                      flush=True)
        extras = extra_files(fat, hddmap.load_manifest_list(args.manifest))
    for path in extras:
        print("%-14s extra" % path, flush=True)
    bad = [r for r in results if r["status"] != "ok"]
    summary = {"image": str(args.image), "targets": len(results),
               "ok": len(results) - len(bad), "bad": len(bad), "extra": len(extras),
               "seconds": round(time.time() - t0, 1)}
    print(json.dumps(summary))
    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "results": results,
                                         "extra": extras}, indent=1) + "\n")
    return 1 if bad or extras else 0


if __name__ == "__main__":
    sys.exit(main())
