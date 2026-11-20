#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Assemble installed HDD files from the real disc map volumes."""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

SECTOR = 512
ISO_SECTOR = 2048
ROOT = Path(__file__).resolve().parents[1]

_spec = spec_from_file_location("make_hdd_image", ROOT / "scripts" / "make-hdd-image.py")
assert _spec is not None and _spec.loader is not None
make_hdd_image = module_from_spec(_spec)
_spec.loader.exec_module(make_hdd_image)


class MapError(SystemExit):
    pass


# --- ISO9660 (directory walk only; the discs are plain ISO9660) -------------

def _iso_read(f, offset: int, size: int) -> bytes:
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise MapError("short ISO read at 0x%x: %d/%d" % (offset, len(data), size))
    return data


def _le32(rec: bytes, off: int) -> int:
    return struct.unpack_from("<I", rec, off)[0]


def _iso_entries(f, lba: int, size: int) -> dict[str, tuple[int, int, int]]:
    """Directory extent -> {NAME: (extent LBA, size, flags)}, ';1' stripped."""
    blob = _iso_read(f, lba * ISO_SECTOR, size)
    out: dict[str, tuple[int, int, int]] = {}
    i = 0
    while i < len(blob):
        length = blob[i]
        if length == 0:                       # padding to the next sector
            i = (i // ISO_SECTOR + 1) * ISO_SECTOR
            continue
        rec = blob[i:i + length]
        name = rec[33:33 + rec[32]].split(b";", 1)[0].decode("latin-1")
        out[name] = (_le32(rec, 2), _le32(rec, 10), rec[25])
        i += length
    return out


def iso_index(path: Path) -> dict[str, tuple[int, int]]:
    """{CPY name: (extent LBA, size)} from the disc's MAP/ directory."""
    with path.open("rb") as f:
        pvd = _iso_read(f, 16 * ISO_SECTOR, ISO_SECTOR)
        if pvd[0:6] != b"\x01CD001":
            raise MapError("no ISO9660 PVD at LBA 16 in %s" % path)
        root = pvd[156:156 + 34]
        root_entries = _iso_entries(f, _le32(root, 2), _le32(root, 10))
        if "MAP" not in root_entries:
            raise MapError("no MAP/ directory in %s" % path)
        lba, size, _flags = root_entries["MAP"]
        return {name: (ext, sz)
                for name, (ext, sz, flags) in _iso_entries(f, lba, size).items()
                if flags == 0}


# --- checksum law -----------------------------------------------------------

try:                                    # optional numpy accelerator
    import numpy as _np
except Exception:                       # noqa: BLE001 - optional accelerator
    _np = None


def word_sum(data: bytes) -> int:
    """Sum of little-endian u32 words (zero-padded tail word), mod 2^32."""
    if len(data) % 4:
        data += b"\x00" * (4 - len(data) % 4)
    if _np is not None:
        return (int(_np.frombuffer(data, dtype="<u4").sum(dtype=_np.uint64))
                & 0xFFFFFFFF)
    total = 0
    for (word,) in struct.iter_unpack("<I", data):
        total += word
    return total & 0xFFFFFFFF


def read_cpy(iso_path: Path, lba: int, size: int, name: str) -> bytes:
    with iso_path.open("rb") as f:
        raw = _iso_read(f, lba * ISO_SECTOR, size)
    try:
        return zlib.decompress(raw)
    except zlib.error as exc:
        raise MapError("%s: %s is not a zlib stream: %s" % (iso_path.name, name, exc))


# --- manifest ---------------------------------------------------------------

def load_manifest(path: Path) -> dict[str, dict]:
    doc = json.loads(path.read_text())
    return {t["name"]: t for t in doc["targets"]}


def load_manifest_list(path: Path) -> list[dict]:
    return json.loads(path.read_text())["targets"]


def installed_path(name: str) -> str:
    """The installed FAT path of a manifest target.

    The guest opens every `*.IDX` target as `hdd2\\Map\\IDX\\<name>` and every
    other target at the map root `hdd2\\Map\\<name>`.
    """
    return "Map\\IDX\\" + name if name.upper().endswith(".IDX") else "Map\\" + name


def assemble_to(path: Path, target: dict, limit: int | None,
                discs: list[Path],
                indexes: list[dict[str, tuple[int, int]]],
                sparse_full: bool = False) -> tuple[int, int, int]:
    """Stream (a prefix of) the target's CPYs to `path`, verifying each CPY
    one at a time; with `sparse_full` the rest is a hole.  Returns
    (size, cpy_used, data_written)."""
    total = target["total_size"]
    want = total if limit is None else min(limit, total)
    if limit is not None and (limit <= 0 or limit > total):
        raise MapError("slice %s=%d outside 1..%d" % (target["name"], limit, total))
    written = 0
    used = 0
    with path.open("wb") as out:
        for cpy in target["files"]:
            if written >= want:
                break
            name = cpy["file"]
            found = None
            for iso, index in zip(discs, indexes):
                if name in index:
                    found = (iso, index[name])
                    break
            if found is None:
                raise MapError("CPY %s of %s not found on any disc MAP/"
                               % (name, target["name"]))
            iso, (lba, size) = found
            data = read_cpy(iso, lba, size, name)
            if len(data) != cpy["size1"] or len(data) != cpy["size2"]:
                raise MapError("%s: decompressed %d B, manifest says %d/%d"
                               % (name, len(data), cpy["size1"], cpy["size2"]))
            got = word_sum(data)
            if got != cpy["checksum"]:
                raise MapError("%s: word sum 0x%08x != manifest 0x%08x"
                               % (name, got, cpy["checksum"]))
            chunk = data[:want - written]
            out.write(chunk)
            written += len(chunk)
            used += 1
    if written != want:
        raise MapError("%s: assembled %d B, wanted %d"
                       % (target["name"], written, want))
    if sparse_full and want < total:
        out_path = path
        with out_path.open("r+b") as f:
            f.truncate(total)
    return total, used, written


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--size-mb", type=int, default=128)
    ap.add_argument("--fat", type=int, choices=(16, 32), default=32)
    ap.add_argument("--label", default="VOL")
    ap.add_argument("--manifest", type=Path,
                    default=ROOT / "images" / "hdd-manifest.json")
    ap.add_argument("--disc1", type=Path,
                    default=ROOT / "images" / "dvd" / "disc1" / "EU-3350-00_1.ISO")
    ap.add_argument("--disc2", type=Path,
                    default=ROOT / "images" / "dvd" / "disc2" / "EU-3350-00_2.ISO")
    ap.add_argument("--file", action="append", default=[], metavar="TARGET",
                    help="install the whole target (repeatable)")
    ap.add_argument("--slice", action="append", default=[], metavar="TARGET=BYTES",
                    help="install the first BYTES of a target (repeatable)")
    ap.add_argument("--dest", action="append", default=[], metavar="TARGET=PATH",
                    help="FAT path for a target (default: its name; "
                         "directories are created for '\\'-separated paths)")
    ap.add_argument("--prefix", action="append", default=[], metavar="DIR",
                    help="additionally install every selected file under DIR\\ "
                         "(repeatable)")
    ap.add_argument("--partition", type=int, choices=(1, 2), default=1,
                    help="install into MBR partition 1 (default) or the "
                         "second FAT32 partition (the guest's hdd2 volume)")
    ap.add_argument("--part2-mb", type=int, default=None,
                    help="size of the second partition in MiB")
    ap.add_argument("--label2", default=None,
                    help="volume label of the second partition")
    ap.add_argument("--layout", action="store_true",
                    help="install the COMPLETE P2006.SCE layout: every manifest "
                         "target at its total_size, `*.IDX` under Map\\IDX\\, "
                         "the rest under Map\\ (implies --partition 2)")
    ap.add_argument("--full", action="append", default=[], metavar="TARGET",
                    help="with --layout: carry this target's whole data, not "
                         "just a verified prefix (repeatable)")
    ap.add_argument("--max-prefix", type=int, default=8 << 20, metavar="BYTES",
                    help="with --layout: verified prefix per large target "
                         "(default 8 MiB); the file still gets its manifest size")
    ap.add_argument("--cluster-kb", type=int, default=None,
                    help="FAT32 cluster size passed to make-hdd-image.py "
                         "(the full layout needs a large one; default 16)")
    ap.add_argument("--keep-parts", action="store_true",
                    help="keep the per-target part-<NAME> files next to --out "
                         "(the 2.4 GB 0.2 part is deleted by default)")
    args = ap.parse_args()

    targets = load_manifest(args.manifest)
    discs = [d for d in (args.disc1, args.disc2) if d.is_file()]
    if not discs:
        raise MapError("no disc ISO found")
    indexes = [iso_index(d) for d in discs]

    if args.layout:
        if args.file or args.slice:
            raise MapError("--layout installs the whole manifest; drop "
                           "--file/--slice")
        manifest_targets = load_manifest_list(args.manifest)
        full = set(args.full)
        wanted: list[tuple[str, int | None, bool]] = []
        for target in manifest_targets:
            name = target["name"]
            if name in full or target["total_size"] <= args.max_prefix:
                wanted.append((name, None, False))
            else:
                wanted.append((name, args.max_prefix, True))
        # Every --dest still overrides the installed path; the rule below is
        # the guest's own naming (see installed_path()).
        args.partition = 2
        need = sum(t["total_size"] for t in manifest_targets)
        if args.cluster_kb is None:
            args.cluster_kb = 16
        if args.part2_mb is None:
            # Whole tree + FAT/directory slack; 68 MiB up front leaves a
            # 2048-sector-aligned partition 1 (~67 MiB).
            args.part2_mb = (need + (96 << 20) + (1 << 20) - 1) // (1 << 20)
        args.size_mb = args.part2_mb + 68
    else:
        wanted = [(n, None, False) for n in args.file]
        for spec in args.slice:
            name, sep, size = spec.rpartition("=")
            if not sep or not size.isdigit():
                raise MapError("bad --slice %r (expected TARGET=BYTES)" % spec)
            wanted.append((name, int(size), False))
    if not wanted:
        raise MapError("nothing to install (use --file/--slice/--layout)")

    scratch = args.out.parent
    scratch.mkdir(parents=True, exist_ok=True)
    dests = {}
    for spec in args.dest:
        name, sep, path = spec.partition("=")
        if not sep or not path:
            raise MapError("bad --dest %r (expected TARGET=PATH)" % spec)
        dests[name] = path
    files: list[tuple[Path, str]] = []
    parts: list[Path] = []
    for name, limit, sparse in wanted:
        if name not in targets:
            raise MapError("%s is not a manifest target" % name)
        target = targets[name]
        src = scratch / ("part-" + name)
        size, used, data = assemble_to(src, target, limit, discs, indexes,
                                       sparse_full=sparse)
        parts.append(src)
        dest = dests.get(name, installed_path(name) if args.layout else name)
        # Outside --layout the part file is never truncated, so its on-disk
        # size already is the verified byte count.  Only a sparse layout part
        # (truncated to the manifest total_size) needs the third item to say
        # how many leading bytes are real data.
        entry = ((src, dest, data) if args.layout and data < size
                 else (src, dest))
        files.append(entry)
        for prefix in args.prefix:
            files.append((src, prefix.rstrip("\\") + "\\" + dest))
        print("  %-12s %12d B  %3d CPY verified  %10d B data  -> %s"
              % (name, size, used, data, dest))
    if args.partition == 2:
        make_hdd_image.build(args.out, args.size_mb, args.label, [], fat=args.fat,
                             files2=files, part2_mb=args.part2_mb,
                             label2=args.label2, cluster_kb=args.cluster_kb)
    else:
        make_hdd_image.build(args.out, args.size_mb, args.label, files,
                             fat=args.fat, cluster_kb=args.cluster_kb)
    if not args.keep_parts:
        for part in parts:
            part.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
