#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Build a minimal MBR + FAT16/FAT32 HDD image."""
from __future__ import annotations

import argparse
import array
import datetime
import struct
import sys
from pathlib import Path

SECTOR = 512
PART_START = 2048
COPY_CHUNK = 4 << 20


class SparseImage:
    """The output image as a sparse file: seek+write, never a RAM bytearray.

    Holes come from truncating the file to its final size and writing only
    the bytes the layout actually uses; file data is streamed in COPY_CHUNK
    pieces instead of `read_bytes()`.
    """

    def __init__(self, path: Path, total_bytes: int) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._f = open(path, "w+b")
        self._f.truncate(total_bytes)
        self.size = total_bytes

    def __setitem__(self, key: slice, data: bytes) -> None:
        if key.step not in (None, 1):
            raise SystemExit("SparseImage: stepped slice writes unsupported")
        self._f.seek(key.start)
        self._f.write(data)

    def __getitem__(self, key: slice) -> bytes:
        self._f.seek(key.start)
        return self._f.read(key.stop - key.start)

    def write_at(self, off: int, data: bytes) -> None:
        self._f.seek(off)
        self._f.write(data)

    def copy_at(self, off: int, src: Path, limit: int | None = None) -> None:
        """Stream `src` into the image at `off` (no whole-file buffer).

        `limit` copies only that many leading bytes: a sparse part file
        carries verified prefix data and a hole up to the target's manifest
        size, and copying the hole would read gigabytes of zeros.
        """
        self._f.seek(off)
        left = limit
        with src.open("rb") as f:
            while left is None or left > 0:
                want = COPY_CHUNK if left is None else min(COPY_CHUNK, left)
                chunk = f.read(want)
                if not chunk:
                    break
                self._f.write(chunk)
                if left is not None:
                    left -= len(chunk)

    def close(self) -> None:
        self._f.close()


def _fill_chain(fat, start: int, count: int, width: int, eoc: int) -> None:
    """FAT[start+i] = start+i+1 for i < count-1, `eoc` for the last cluster."""
    if width == 4 and sys.byteorder == "little":
        vals = array.array("I", range(start + 1, start + count))
        vals.append(eoc)
        fat[start * 4:(start + count) * 4] = vals.tobytes()
        return
    fmt = "<I" if width == 4 else "<H"
    mask = 0xFFFFFFFF if width == 4 else 0xFFFF
    for i in range(count):
        cluster = start + i
        value = eoc if i == count - 1 else (cluster + 1) & mask
        struct.pack_into(fmt, fat, cluster * width, value)


def _name83(name: str) -> bytes:
    """Encode a FAT 8.3 name (no subdirectories)."""
    if name in (".", ".."):
        return name.ljust(11).encode("ascii")
    base, _, ext = name.upper().partition(".")
    base = base[:8].ljust(8)
    ext = ext[:3].ljust(3)
    return (base + ext).encode("ascii")


def _dirent(name: bytes, attr: int, cluster: int, size: int,
            hi_cluster: int = 0) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = attr
    now = datetime.datetime.now()
    dos_time = (now.hour << 11) | (now.minute << 5) | (now.second // 2)
    dos_date = ((now.year - 1980) << 9) | (now.month << 5) | now.day
    struct.pack_into("<H", entry, 20, hi_cluster)   # FAT32 DIR_FstClusHI
    struct.pack_into("<HH", entry, 22, dos_time, dos_date)
    struct.pack_into("<HH", entry, 24, dos_date, 0)
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def _allocate(
    files: list[tuple[Path, str]],
    first_cluster: int,
    clusters: int,
    cluster_bytes: int,
) -> list[tuple[Path, str, int, int, int]]:
    """Allocate clusters contiguously from first_cluster."""
    next_cluster = first_cluster
    allocations: list[tuple[Path, str, int, int, int, int]] = []
    for entry in files:
        src, dest = entry[0], entry[1]
        data_len = src.stat().st_size
        count = (data_len + cluster_bytes - 1) // cluster_bytes
        if count == 0:
            count = 1
        if next_cluster + count > clusters + 2:
            raise SystemExit(f"image too small for {src}")
        allocations.append((src, dest, next_cluster, count, data_len,
                            _data_limit(entry, data_len)))
        next_cluster += count
    return allocations


def _data_limit(entry, size: int) -> int:
    """Bytes to copy from a file entry: optional third tuple item, else all."""
    return entry[2] if len(entry) > 2 and entry[2] is not None else size


def _fat32_geometry(part_sectors: int,
                    cluster_kb: int | None = None) -> tuple[int, int, int, int, int]:
    """Return (sectors_per_cluster, reserved, fat_sectors, clusters, root):
    one sector per cluster by default, grown only if the FAT would not fit;
    --cluster-kb forces a larger cluster for big volumes."""
    # FAT32 needs more than 65525 clusters (Microsoft's FAT32 rule); start at
    # one sector per cluster and grow it only if the FAT would not fit.
    reserved = 32
    num_fats = 2
    order = (1, 2, 4, 8, 16, 32, 64)
    if cluster_kb is not None:
        spc = cluster_kb * 1024 // SECTOR
        if cluster_kb <= 0 or spc & (spc - 1) or not 1 <= spc <= 128:
            raise SystemExit("--cluster-kb must be a power of two in 1..64")
        # The forced cluster only makes sense on the large volume; a small
        # volume (the 67 MiB boot partition) cannot reach the FAT32 cluster
        # rule at that size, so fall back to the automatic geometry there.
        order = (spc,) + tuple(s for s in order if s < spc) + \
            tuple(s for s in order if s > spc)
    for sectors_per_cluster in order:
        fat_sectors = 1
        for _ in range(32):
            data_sectors = part_sectors - reserved - num_fats * fat_sectors
            clusters = data_sectors // sectors_per_cluster
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
        else:
            continue
        if 65525 <= clusters <= 0x0FFFFFF5:
            return sectors_per_cluster, reserved, fat_sectors, clusters, 2
    raise SystemExit("could not size a FAT32 volume for this image size")


def _fat32_bpb(label: str, total_sectors: int, part_sectors: int,
               sectors_per_cluster: int, reserved: int, fat_sectors: int,
               part_start: int = PART_START) -> bytearray:
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x58\x90"
    bs[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = sectors_per_cluster
    struct.pack_into("<H", bs, 14, reserved)
    bs[16] = 2                              # number of FATs
    struct.pack_into("<H", bs, 17, 0)       # root entries (FAT32: 0)
    struct.pack_into("<H", bs, 19, 0)       # 16-bit total sectors (FAT32: 0)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, 0)       # 16-bit FAT size (FAT32: 0)
    struct.pack_into("<H", bs, 24, 63)      # sectors per track
    struct.pack_into("<H", bs, 26, 255)     # heads
    struct.pack_into("<I", bs, 28, part_start)
    struct.pack_into("<I", bs, 32, part_sectors)
    struct.pack_into("<I", bs, 36, fat_sectors)   # 32-bit FAT size
    struct.pack_into("<H", bs, 40, 0)       # ext flags: both FATs active
    struct.pack_into("<H", bs, 42, 0)       # FS version 0.0
    struct.pack_into("<I", bs, 44, 2)       # root directory cluster
    struct.pack_into("<H", bs, 48, 1)       # FSInfo sector
    struct.pack_into("<H", bs, 50, 6)       # backup boot sector
    bs[64] = 0x80                           # drive number
    bs[66] = 0x29                           # extended boot signature
    struct.pack_into("<I", bs, 67, 0x20061019)
    bs[71:82] = label.upper().ljust(11)[:11].encode("ascii")
    # The guest reads this string at BPB offset 0x52 (82), not the FAT16
    # offset 54.
    bs[82:90] = b"FAT32   "
    bs[510:512] = b"\x55\xaa"
    return bs


def _fsinfo(free_clusters: int, next_free: int) -> bytearray:
    fs = bytearray(SECTOR)
    struct.pack_into("<I", fs, 0, 0x41615252)    # lead signature
    struct.pack_into("<I", fs, 484, 0x61417272)  # structure signature
    struct.pack_into("<I", fs, 488, free_clusters)
    struct.pack_into("<I", fs, 492, next_free)
    struct.pack_into("<I", fs, 508, 0xAA550000)  # trail signature
    return fs


def _build_fat16(image: SparseImage, out: Path, size_mb: int, label: str,
                 files: list[tuple[Path, str]], total_sectors: int,
                 part_sectors: int, part_start: int = PART_START) -> None:
    sectors_per_cluster = 8 if size_mb >= 64 else 4
    reserved = 4
    num_fats = 2
    root_entries = 512
    root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR

    # The FAT16 root directory is a fixed table right before the data area;
    # this builder puts every file in it (no subdirectories), after the
    # volume label entry.
    if len(files) + 1 > root_entries:
        raise SystemExit(f"FAT16 root directory holds {root_entries - 1} files "
                         f"besides the volume label; got {len(files)} "
                         f"(use --fat 32)")
    names: dict[bytes, str] = {}
    for entry in files:
        dest = entry[1]
        if "\\" in dest or "/" in dest:
            raise SystemExit(f"FAT16 builder has no subdirectories: {dest!r} "
                             f"(use --fat 32)")
        short = _name83(dest)
        if short in names:
            raise SystemExit(f"{dest!r} and {names[short]!r} have the same "
                             f"8.3 name {short.decode()!r}")
        names[short] = dest

    # Iterate until the FAT is large enough to describe the data area.
    fat_sectors = 1
    for _ in range(32):
        data_sectors = part_sectors - reserved - num_fats * fat_sectors - root_sectors
        clusters = data_sectors // sectors_per_cluster
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need <= fat_sectors:
            break
        fat_sectors = need
    else:
        raise SystemExit("could not size the FAT")

    data_sectors = part_sectors - reserved - num_fats * fat_sectors - root_sectors
    clusters = data_sectors // sectors_per_cluster
    if clusters + 2 > 0xFFF0:
        raise SystemExit(f"too many clusters for FAT16: {clusters}")

    allocations = _allocate(files, 2, clusters, sectors_per_cluster * SECTOR)

    # --- FAT16 boot sector ---------------------------------------------
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = sectors_per_cluster
    struct.pack_into("<H", bs, 14, reserved)
    bs[16] = num_fats
    struct.pack_into("<H", bs, 17, root_entries)
    struct.pack_into("<H", bs, 19, 0 if part_sectors > 0xFFFF else part_sectors)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, fat_sectors)
    struct.pack_into("<H", bs, 24, 63)
    struct.pack_into("<H", bs, 26, 255)
    struct.pack_into("<I", bs, 28, part_start)
    struct.pack_into("<I", bs, 32, part_sectors)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x20061019)
    bs[43:54] = label.upper().ljust(11)[:11].encode("ascii")
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xaa"
    part = part_start * SECTOR
    image[part:part + SECTOR] = bs

    # --- FAT tables ----------------------------------------------------
    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<H", fat, 0, 0xFFF8)
    struct.pack_into("<H", fat, 2, 0xFFFF)
    for _src, _dest, start, count, _size, _limit in allocations:
        _fill_chain(fat, start, count, 2, 0xFFFF)
    for n in range(num_fats):
        off = part + (reserved + n * fat_sectors) * SECTOR
        image[off:off + len(fat)] = fat

    # --- root directory -------------------------------------------------
    root_off = part + (reserved + num_fats * fat_sectors) * SECTOR
    entries = bytearray()
    entries += _dirent(_name83(""), 0x08 | 0x10, 0, 0)  # placeholder
    entries[0:11] = label.upper().ljust(11)[:11].encode("ascii")
    entries[11] = 0x08  # volume label
    for _src, dest, start, _count, size, _limit in allocations:
        entries += _dirent(_name83(dest), 0x20, start, size)
    entries += b"\x00" * (root_sectors * SECTOR - len(entries))
    image[root_off:root_off + len(entries)] = entries

    # --- file data ------------------------------------------------------
    data_off = part + (reserved + num_fats * fat_sectors + root_sectors) * SECTOR
    for src, _dest, start, _count, _size, _limit in allocations:
        off = data_off + (start - 2) * sectors_per_cluster * SECTOR
        image.copy_at(off, src, _limit)

    print(
        f"{out}: {size_mb} MiB, FAT16, label {label.upper()!r}, "
        f"partition LBA {part_start}, cluster {sectors_per_cluster * SECTOR} B, "
        f"{clusters} clusters, FAT {fat_sectors} sectors"
    )
    for _src, dest, start, count, size, _limit in allocations:
        print(f"  {dest}: {size} bytes, cluster {start}, {count} clusters")


def _fat32_tree(files: list[tuple[Path, str]]) -> dict:
    """Group flat `A\\B\\NAME` destinations into a directory tree."""
    root: dict = {"name": "", "dirs": [], "files": []}
    for entry in files:
        src, dest = entry[0], entry[1]
        parts = [p for p in dest.replace("/", "\\").split("\\") if p]
        if not parts:
            raise SystemExit("empty destination name in %r" % (dest,))
        node = root
        for part in parts[:-1]:
            child = next((d for d in node["dirs"]
                          if d["name"].upper() == part.upper()), None)
            if child is None:
                child = {"name": part, "dirs": [], "files": []}
                node["dirs"].append(child)
            node = child
        # third item (optional): how many leading bytes of the file carry
        # verified data; the rest is a hole up to the manifest size.
        node["files"].append((src, parts[-1],
                              _data_limit(entry, src.stat().st_size)))
    return root


def _walk_tree(node: dict, root_cluster: int, first_free: int,
               clusters: int, cluster_bytes: int):
    """Assign one cluster to every directory, then contiguous file runs.

    Returns (dirs, allocations) where allocations is the same tuple shape as
    _allocate: (src, dest, start cluster, cluster count, size).
    """
    dirs: list[dict] = []
    node["cluster"] = root_cluster
    next_cluster = first_free

    def assign_dirs(n: dict) -> None:
        nonlocal next_cluster
        for child in n["dirs"]:
            child["cluster"] = next_cluster
            next_cluster += 1
            child["parent"] = n["cluster"]
            dirs.append(child)
            assign_dirs(child)

    assign_dirs(node)

    allocations: list[tuple[Path, str, int, int, int, int]] = []

    def assign_files(n: dict, prefix: str) -> None:
        nonlocal next_cluster
        n["alloc"] = []
        for src, name, data_limit in n["files"]:
            size = src.stat().st_size
            count = max(1, (size + cluster_bytes - 1) // cluster_bytes)
            if next_cluster + count > clusters + 2:
                raise SystemExit("image too small for %s" % src)
            entry = (src, name, next_cluster, count, size, data_limit)
            n["alloc"].append(entry)
            allocations.append((src, prefix + name) + entry[2:])
            next_cluster += count
        for child in n["dirs"]:
            assign_files(child, prefix + child["name"] + "\\")

    assign_files(node, "")
    return dirs, allocations


def _build_fat32(image: SparseImage, out: Path, size_mb: int, label: str,
                 files: list[tuple[Path, str]], total_sectors: int,
                 part_sectors: int, part_start: int = PART_START,
                 cluster_kb: int | None = None) -> None:
    (spc, reserved, fat_sectors,
     clusters, root_cluster) = _fat32_geometry(part_sectors, cluster_kb)

    tree = _fat32_tree(files)
    dirs, allocations = _walk_tree(tree, root_cluster, root_cluster + 1,
                                   clusters, spc * SECTOR)

    part = part_start * SECTOR
    # --- FAT32 boot sector, FSInfo and their backups ---------------------
    image[part:part + SECTOR] = _fat32_bpb(label, total_sectors, part_sectors,
                                           spc, reserved, fat_sectors,
                                           part_start)
    free = (clusters - 1 - len(dirs)
            - sum(count for _s, _d, _c, count, _z, _l in allocations))
    image[part + SECTOR:part + 2 * SECTOR] = _fsinfo(free, 0xFFFFFFFF)
    if reserved >= 8:
        image[part + 6 * SECTOR:part + 7 * SECTOR] = \
            bytes(image[part:part + SECTOR])
        image[part + 7 * SECTOR:part + 8 * SECTOR] = \
            bytes(image[part + SECTOR:part + 2 * SECTOR])

    # --- FAT tables (4-byte entries, 28 significant bits) ----------------
    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<I", fat, 0, 0x0FFFFFF8)
    struct.pack_into("<I", fat, 4, 0x0FFFFFFF)
    struct.pack_into("<I", fat, root_cluster * 4, 0x0FFFFFFF)  # root: one cluster
    for d in dirs:                              # each directory: one cluster
        struct.pack_into("<I", fat, d["cluster"] * 4, 0x0FFFFFFF)
    for _src, _dest, start, count, _size, _limit in allocations:
        _fill_chain(fat, start, count, 4, 0x0FFFFFFF)
    for n in range(2):
        off = part + (reserved + n * fat_sectors) * SECTOR
        image[off:off + len(fat)] = fat

    # --- directories (one cluster each; root is a cluster chain) ---------
    data_off = part + (reserved + 2 * fat_sectors) * SECTOR
    cluster_bytes = spc * SECTOR

    def write_dir(node: dict, parent_cluster: int | None, label_entry: bool) -> None:
        entries = bytearray()
        if label_entry:
            entries += _dirent(_name83(""), 0x08 | 0x10, 0, 0)
            entries[0:11] = label.upper().ljust(11)[:11].encode("ascii")
            entries[11] = 0x08  # volume label
        else:
            entries += _dirent(_name83("."), 0x10, node["cluster"] & 0xFFFF,
                               0, node["cluster"] >> 16)
            pc = parent_cluster or 0
            entries += _dirent(_name83(".."), 0x10, pc & 0xFFFF, 0, pc >> 16)
        for child in node["dirs"]:
            entries += _dirent(_name83(child["name"]), 0x10,
                               child["cluster"] & 0xFFFF, 0,
                               child["cluster"] >> 16)
        for _src, name, start, _count, size, _limit in node["alloc"]:
            entries += _dirent(_name83(name), 0x20, start & 0xFFFF, size,
                               start >> 16)
        if len(entries) > cluster_bytes:
            raise SystemExit("directory %r needs more than one cluster"
                             % node["name"])
        entries += b"\x00" * (cluster_bytes - len(entries))
        off = data_off + (node["cluster"] - 2) * cluster_bytes
        image[off:off + len(entries)] = entries
        for child in node["dirs"]:
            write_dir(child, node["cluster"] if not label_entry else 0, False)

    write_dir(tree, None, True)

    # --- file data ------------------------------------------------------
    for src, _dest, start, _count, _size, _limit in allocations:
        off = data_off + (start - 2) * cluster_bytes
        image.copy_at(off, src, _limit)

    print(
        f"{out}: {size_mb} MiB, FAT32, label {label.upper()!r}, "
        f"partition LBA {part_start}, cluster {spc * SECTOR} B, "
        f"{clusters} clusters, FAT {fat_sectors} sectors, "
        f"{reserved} reserved sectors"
    )
    for _src, dest, start, count, size, _limit in allocations:
        print(f"  {dest}: {size} bytes, cluster {start}, {count} clusters")


def build(out: Path, size_mb: int, label: str, files: list[tuple[Path, str]],
          fat: int = 16, files2: list[tuple[Path, str]] | None = None,
          part2_mb: int | None = None, label2: str | None = None,
          cluster_kb: int | None = None) -> None:
    """Write one or two MBR partitions into `out`.

    `files2` (with `part2_mb`) puts a second FAT32 partition at the end of
    the image.  The guest mounts MBR partition 0 as HDD1 and partition 1 as
    HDD2 — the map volume is `hdd2`, not a folder named HDD2.
    """
    total_sectors = size_mb * 1024 * 1024 // SECTOR

    image = SparseImage(out, total_sectors * SECTOR)

    # --- MBR -----------------------------------------------------------
    mbr = bytearray(SECTOR)
    mbr[446 + 0] = 0x80                       # active
    mbr[446 + 1:446 + 4] = bytes((0x00, 0x02, 0x00))  # CHS start (unused)
    # The guest's format check accepts only partition type 0x0B or 0x0C and
    # rejects any other non-zero type; the MLD's own MBR scan separately
    # accepts 0x04/0x0E as FAT16.
    ptype = 0x0C if fat == 32 else 0x0E

    if files2 is None:
        part_sectors = total_sectors - PART_START
        mbr[446 + 4] = ptype
        mbr[446 + 5:446 + 8] = bytes((0xFE, 0xFF, 0xFF))
        struct.pack_into("<I", mbr, 446 + 8, PART_START)
        struct.pack_into("<I", mbr, 446 + 12, part_sectors)
        mbr[510:512] = b"\x55\xaa"
        image[0:SECTOR] = mbr
        if fat == 32:
            _build_fat32(image, out, size_mb, label, files, total_sectors,
                         part_sectors,
                         cluster_kb=cluster_kb)
        else:
            _build_fat16(image, out, size_mb, label, files, total_sectors,
                         part_sectors)
        image.close()
        return

    if part2_mb is None:
        part2_mb = max(64, size_mb // 4)
    part2_sectors = part2_mb * 1024 * 1024 // SECTOR
    part2_start = total_sectors - part2_sectors
    if part2_start <= PART_START:
        raise SystemExit("image too small for two partitions")
    part1_sectors = part2_start - PART_START

    for idx, (start, count, ptype_i) in enumerate((
            (PART_START, part1_sectors, ptype),
            (part2_start, part2_sectors, 0x0C))):
        e = 446 + idx * 16
        mbr[e + 0] = 0x80 if idx == 0 else 0x00
        mbr[e + 1:e + 4] = bytes((0xFE, 0xFF, 0xFF))
        mbr[e + 4] = ptype_i
        mbr[e + 5:e + 8] = bytes((0xFE, 0xFF, 0xFF))
        struct.pack_into("<I", mbr, e + 8, start)
        struct.pack_into("<I", mbr, e + 12, count)
    mbr[510:512] = b"\x55\xaa"
    image[0:SECTOR] = mbr

    if fat == 32:
        _build_fat32(image, out, size_mb, label, files, total_sectors,
                     part1_sectors, part_start=PART_START,
                     cluster_kb=cluster_kb)
    else:
        _build_fat16(image, out, size_mb, label, files, total_sectors,
                     part1_sectors, part_start=PART_START)
    _build_fat32(image, out, size_mb, label2 or label, files2, total_sectors,
                 part2_sectors, part_start=part2_start,
                 cluster_kb=cluster_kb)
    image.close()
    print(f"{out}: {size_mb} MiB, two partitions: "
          f"LBA {PART_START}+{part1_sectors}, "
          f"LBA {part2_start}+{part2_sectors} (map)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--size-mb", type=int, default=128)
    ap.add_argument("--label", default="VOL")
    ap.add_argument("--fat", type=int, choices=(16, 32), default=16,
                    help="FAT16 (default) or FAT32 volume")
    ap.add_argument(
        "--file",
        action="append",
        default=[],
        metavar="SRC:DEST",
        help="add SRC at the FAT root as DEST (repeatable)",
    )
    ap.add_argument(
        "--file2",
        action="append",
        default=[],
        metavar="SRC:DEST",
        help="add SRC to the second (map) partition as DEST (repeatable)",
    )
    ap.add_argument("--part2-mb", type=int, default=None,
                    help="size of the second FAT32 partition in MiB "
                         "(default: a quarter of the image, at least 64)")
    ap.add_argument("--label2", default=None, help="label of the second volume")
    ap.add_argument("--cluster-kb", type=int, default=None,
                    help="FAT32 cluster size in KiB (power of two, default: "
                         "auto - 1 sector, grown only if the FAT would not fit)")
    args = ap.parse_args()

    def parse_files(specs):
        out = []
        for spec in specs:
            src, sep, dest = spec.rpartition(":")
            if not sep:
                print(f"error: bad --file {spec!r} (expected SRC:DEST)",
                      file=sys.stderr)
                raise SystemExit(2)
            out.append((Path(src), dest))
        return out

    files = parse_files(args.file)
    # `--part2-mb` alone makes a two-partition image with an empty map volume
    # (the guest still mounts it as HDD2) — the fallback when the disc inputs
    # for scripts/make-hdd-map.py are absent.
    files2 = parse_files(args.file2) if args.file2 else (
        [] if args.part2_mb else None)
    build(args.out, args.size_mb, args.label, files, fat=args.fat,
          files2=files2, part2_mb=args.part2_mb, label2=args.label2,
          cluster_kb=args.cluster_kb)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
