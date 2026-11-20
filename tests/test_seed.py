# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""scripts/apm_record_seed.py: lands the completed-install APM record in the
guest-formatted areas of a flash image or battery RAM file."""
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "scripts" / "apm_record_seed.py"
STRIDE = 0x540
BODY = 0x538
FLASH_AREAS = (0x1e40800, 0x1e80800, 0x1ec0800)
BKRAM_AREA = 0x40800


def _formatted(size, areas, erased_second=False):
    """A store whose APM areas carry a guest-style record: status 99, base 1,
    serial 2, a non-zero byte elsewhere in the body."""
    d = bytearray(b"\xff" * size)
    for area in areas:
        d[area:area + 8] = b"20080207"
        d[area + 8] = 1
        for n in range(2):
            rec = area + 0x10 + n * STRIDE
            if erased_second and n == 1:
                continue
            body = bytearray(BODY)
            body[0x370], body[0x80], body[0x83], body[0x10] = 99, 1, 2, 0x5a
            d[rec:rec + BODY] = body
    return d


def _check_record(d, rec, base, serial):
    body = d[rec:rec + BODY]
    assert body[0x370] == 0            # MapDwlSts: install complete
    assert body[0x80] == base
    assert body[0x83] == serial
    assert body[0x10] == 0x5a          # the rest of the guest's body is kept
    s = sum(struct.unpack_from("<I", body, i)[0]
            for i in range(0, BODY, 4)) & 0xFFFFFFFF
    assert struct.unpack_from("<II", d, rec + BODY) == (s, (~s) & 0xFFFFFFFF)


def _seed(*args):
    subprocess.check_call([sys.executable, str(SEED)] + [str(a) for a in args],
                          stdout=subprocess.DEVNULL)


def test_seeds_every_apm_area_in_flash(tmp_path):
    flash = tmp_path / "flash.img"
    before = _formatted(32 << 20, FLASH_AREAS, erased_second=True)
    flash.write_bytes(bytes(before))
    _seed(flash, flash, "--base", "33", "--serial", "65")
    d = flash.read_bytes()
    assert len(d) == 32 << 20
    for area in FLASH_AREAS:
        _check_record(d, area + 0x10, 33, 65)
        rec = area + 0x10 + STRIDE                 # erased record: untouched
        assert d[rec:rec + BODY + 8] == b"\xff" * (BODY + 8)
    assert d[:FLASH_AREAS[0]] == before[:FLASH_AREAS[0]]


def test_seeds_battery_ram(tmp_path):
    bkram = tmp_path / "bkram.bin"
    bkram.write_bytes(bytes(_formatted(1 << 20, (BKRAM_AREA,))))
    out = tmp_path / "out.bin"
    _seed(bkram, out, "--base", "7", "--serial", "8")
    d = out.read_bytes()
    assert len(d) == 1 << 20
    for n in range(2):
        _check_record(d, BKRAM_AREA + 0x10 + n * STRIDE, 7, 8)


def test_identity_defaults_to_alldata(tmp_path):
    alldata = tmp_path / "ALLDATA.KWI"
    frame = bytearray(0x58)
    frame[0x53:0x55] = b"42"
    frame[0x56] = 9
    alldata.write_bytes(bytes(frame))
    bkram = tmp_path / "bkram.bin"
    bkram.write_bytes(bytes(_formatted(1 << 20, (BKRAM_AREA,))))
    _seed(bkram, bkram, "--alldata", alldata)
    _check_record(bkram.read_bytes(), BKRAM_AREA + 0x10, 42, 9)


def test_unformatted_store_is_an_error(tmp_path):
    bkram = tmp_path / "bkram.bin"
    bkram.write_bytes(b"\0" * (1 << 20))
    r = subprocess.run([sys.executable, str(SEED), str(bkram), str(bkram)],
                       capture_output=True, text=True)
    assert r.returncode != 0 and "no APM area" in r.stderr
    assert bkram.read_bytes() == b"\0" * (1 << 20)


def test_dump_lists_every_known_backup_area(tmp_path):
    bkram = tmp_path / "bkram.bin"
    d = _formatted(1 << 20, (BKRAM_AREA,))
    d[0x1000:0x1008] = b"20051228"
    bkram.write_bytes(bytes(d))
    out = subprocess.check_output([sys.executable, str(SEED), "--dump",
                                   str(bkram)], text=True)
    assert "area 0x1000 " in out and "area 0x40800 " in out
    assert "MapDwlSts(+0x370)=99" in out
    out = subprocess.check_output([sys.executable, str(SEED), "--dump",
                                   "--date", "20080207", str(bkram)], text=True)
    assert "area 0x1000 " not in out and "area 0x40800 " in out
