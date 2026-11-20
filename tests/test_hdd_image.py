# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
from __future__ import annotations

import importlib.util
import struct
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "make-hdd-image.py"

SECTOR = 512
PART_START = 2048


def load_builder():
    spec = importlib.util.spec_from_file_location("make_hdd_image", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def builder():
    return load_builder()


def test_fat16_layout_unchanged(builder, tmp_path):
    out = tmp_path / "fat16.img"
    builder.build(out, 64, "VOL", [], fat=16)
    data = out.read_bytes()
    assert len(data) == 64 * 1024 * 1024
    assert data[446 + 4] == 0x0E               # FAT16 partition type
    part = PART_START * SECTOR
    bs = data[part:part + SECTOR]
    assert bs[54:62] == b"FAT16   "
    assert struct.unpack_from("<H", bs, 22)[0] > 0   # 16-bit FAT size
    assert bs[510:512] == b"\x55\xaa"


def test_fat16_refuses_a_root_directory_overflow(builder, tmp_path):
    src = tmp_path / "f"
    src.write_bytes(b"x")
    files = [(src, "F%d.BIN" % n) for n in range(512)]
    with pytest.raises(SystemExit, match="holds 511 files .*got 512"):
        builder.build(tmp_path / "fat16.img", 64, "VOL", files, fat=16)
    # 511 files and the volume label fill the 512-entry table exactly.
    out = tmp_path / "ok.img"
    builder.build(out, 64, "VOL", files[:511], fat=16)
    part = PART_START * SECTOR
    bs = out.read_bytes()[part:part + SECTOR]
    reserved, fats = struct.unpack_from("<H", bs, 14)[0], bs[16]
    fat_sectors = struct.unpack_from("<H", bs, 22)[0]
    data_off = part + (reserved + fats * fat_sectors + 32) * SECTOR
    with out.open("rb") as f:
        f.seek(data_off)
        assert f.read(1) == b"x"          # the first file's data, intact


def test_fat16_refuses_colliding_short_names(builder, tmp_path):
    src = tmp_path / "f"
    src.write_bytes(b"x")
    with pytest.raises(SystemExit, match="same 8.3 name"):
        builder.build(tmp_path / "fat16.img", 64, "VOL",
                      [(src, "LONGNAME1.BIN"), (src, "LONGNAME2.BIN")], fat=16)


def test_fat32_blank_volume(builder, tmp_path):
    out = tmp_path / "fat32.img"
    builder.build(out, 64, "VOL", [], fat=32)
    data = out.read_bytes()
    assert len(data) == 64 * 1024 * 1024

    # Only MBR types 0x0B / 0x0C are FAT32.
    assert data[446 + 4] == 0x0C
    assert data[510:512] == b"\x55\xaa"

    part = PART_START * SECTOR
    bs = data[part:part + SECTOR]
    assert bs[0:3] == b"\xeb\x58\x90"
    assert struct.unpack_from("<H", bs, 11)[0] == SECTOR
    assert bs[13] == 1                          # 512-byte clusters
    assert struct.unpack_from("<H", bs, 14)[0] == 32
    assert bs[16] == 2
    assert struct.unpack_from("<H", bs, 17)[0] == 0    # FAT32 root entries
    assert struct.unpack_from("<H", bs, 19)[0] == 0    # FAT32 total16
    assert struct.unpack_from("<H", bs, 22)[0] == 0    # FAT32 fat size 16
    assert struct.unpack_from("<I", bs, 32)[0] == \
        64 * 1024 * 1024 // SECTOR - PART_START
    fat_sectors = struct.unpack_from("<I", bs, 36)[0]
    assert fat_sectors > 0
    assert struct.unpack_from("<I", bs, 44)[0] == 2    # root cluster
    assert struct.unpack_from("<H", bs, 48)[0] == 1    # FSInfo sector
    assert struct.unpack_from("<H", bs, 50)[0] == 6    # backup boot sector
    # FAT32 FS-type string at BPB offset 0x52.
    assert bs[82:90] == b"FAT32   "
    assert bs[510:512] == b"\x55\xaa"

    # FSInfo and the backup boot sector.
    fs = data[part + SECTOR:part + 2 * SECTOR]
    assert struct.unpack_from("<I", fs, 0)[0] == 0x41615252
    assert struct.unpack_from("<I", fs, 484)[0] == 0x61417272
    assert struct.unpack_from("<I", fs, 508)[0] == 0xAA550000
    assert data[part + 6 * SECTOR:part + 7 * SECTOR] == bs
    assert data[part + 7 * SECTOR:part + 8 * SECTOR] == fs

    # FAT32 entries: media/EOC/FAT[1], root cluster end-of-chain.
    reserved = struct.unpack_from("<H", bs, 14)[0]
    fat = data[part + reserved * SECTOR:
               part + (reserved + fat_sectors) * SECTOR]
    assert struct.unpack_from("<I", fat, 0)[0] & 0x0FFFFFFF == 0x0FFFFFF8
    assert struct.unpack_from("<I", fat, 4)[0] & 0x0FFFFFFF == 0x0FFFFFFF
    assert struct.unpack_from("<I", fat, 2 * 4)[0] & 0x0FFFFFFF == 0x0FFFFFFF
    fat2 = data[part + (reserved + fat_sectors) * SECTOR:
                part + (reserved + 2 * fat_sectors) * SECTOR]
    assert fat2 == fat

    # Volume label lives in the root cluster.
    data_off = part + (reserved + 2 * fat_sectors) * SECTOR
    root = data[data_off:data_off + 32]
    assert root[0:11] == b"VOL        "
    assert root[11] == 0x08


def test_fat32_stores_a_file(builder, tmp_path):
    src = tmp_path / "payload.bin"
    payload = bytes(range(256)) * 8            # 2048 B -> 4 clusters
    src.write_bytes(payload)
    out = tmp_path / "fat32-file.img"
    builder.build(out, 64, "VOL", [(src, "LOADING.KWI")], fat=32)
    data = out.read_bytes()

    part = PART_START * SECTOR
    bs = data[part:part + SECTOR]
    reserved = struct.unpack_from("<H", bs, 14)[0]
    fat_sectors = struct.unpack_from("<I", bs, 36)[0]
    fat = data[part + reserved * SECTOR:part + (reserved + 1) * SECTOR]

    data_off = part + (reserved + 2 * fat_sectors) * SECTOR
    root = data[data_off:data_off + 64]
    entry = root[32:64]
    assert entry[0:11] == b"LOADING KWI"
    assert entry[11] == 0x20
    cluster = struct.unpack_from("<H", entry, 26)[0]
    size = struct.unpack_from("<I", entry, 28)[0]
    assert size == len(payload)
    assert cluster == 3                        # after the root cluster (2)
    assert struct.unpack_from("<I", fat, cluster * 4)[0] == cluster + 1
    assert struct.unpack_from("<I", fat, (cluster + 3) * 4)[0] == 0x0FFFFFFF
    off = data_off + (cluster - 2) * SECTOR
    assert data[off:off + len(payload)] == payload
