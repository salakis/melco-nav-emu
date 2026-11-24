# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""scripts/make-hdd-map.py's streamed targets and the cup2006 board's
installed-map check, on small images the project's own builder writes."""
from __future__ import annotations

import importlib.util
import zlib
from pathlib import Path

import pytest

from melco_nav_emu.boards import cup2006

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def builder():
    return load("make_hdd_image", ROOT / "scripts/make-hdd-image.py")


@pytest.fixture(scope="module")
def hddmap():
    return load("make_hdd_map", ROOT / "scripts/make-hdd-map.py")


@pytest.fixture(scope="module")
def verify():
    return load("verify_hdd", ROOT / "scripts/verify-hdd.py")


def _fake_disc(hddmap, tmp_path, chunks, bad=None):
    """A disc file holding zlib CPYs at 2048-byte extents, its index and the
    manifest target that lists them (checksums by the guest's word-sum law)."""
    disc = tmp_path / "disc.iso"
    index, files, blob = {}, [], bytearray()
    for n, data in enumerate(chunks):
        name = "T%03d.CPY" % n
        packed = zlib.compress(data)
        index[name] = (len(blob) // 2048, len(packed))
        blob += packed + b"\x00" * (-len(packed) % 2048)
        checksum = hddmap.word_sum(data)
        if n == bad:
            checksum ^= 1
        files.append({"file": name, "size1": len(data), "size2": len(data),
                      "checksum": checksum})
    disc.write_bytes(bytes(blob))
    total = sum(len(c) for c in chunks)
    target = {"name": "T.BIN", "total_size": total, "files": files}
    return disc, index, target


def test_target_streams_verified_prefix_and_leaves_a_hole(builder, hddmap,
                                                          verify, tmp_path):
    chunks = [bytes([n + 1]) * 3000 for n in range(4)]
    disc, index, target = _fake_disc(hddmap, tmp_path, chunks)
    data = 4500                                    # into the second CPY
    src = hddmap.TargetSource(target, data, "Map\\T.BIN", [disc], [index])
    assert src.stat().st_size == 12000
    out = tmp_path / "map.img"
    builder.build(out, 192, "VOL", [], fat=32, part2_mb=128, label2="MAP",
                  files2=[(src, "Map\\T.BIN", data)])
    with out.open("rb") as f:
        fat = verify.Fat32(f, 2)
        _attr, first, size = fat.lookup("Map\\T.BIN")
        assert size == 12000
        got = b"".join(fat.reader(first, size))
    assert got[:data] == b"".join(chunks)[:data]
    assert got[data:] == b"\x00" * (12000 - data)
    # Streamed straight into the image: no part file next to it.
    assert sorted(p.name for p in tmp_path.iterdir()) == ["disc.iso", "map.img"]


def test_target_with_a_bad_cpy_fails_the_build(builder, hddmap, tmp_path):
    chunks = [b"a" * 2000, b"b" * 2000]
    disc, index, target = _fake_disc(hddmap, tmp_path, chunks, bad=1)
    src = hddmap.TargetSource(target, 4000, "Map\\T.BIN", [disc], [index])
    with pytest.raises(SystemExit, match="word sum"):
        builder.build(tmp_path / "bad.img", 192, "VOL", [], fat=32,
                      part2_mb=128, label2="MAP", files2=[(src, "Map\\T.BIN")])


def _map_image(builder, tmp_path, files):
    out = tmp_path / "hdd.img"
    builder.build(out, 192, "VOL", [], fat=32, part2_mb=128, label2="MAP",
                  files2=files)
    return out


def test_installed_map_is_recognised(builder, tmp_path):
    alldata = tmp_path / "ALLDATA.KWI"
    alldata.write_bytes(b"FORMAT VERSION KIWI01-22-00" + b"\x01" * 4000)
    par = tmp_path / "PAR_MNG.ME"
    par.write_bytes(b"p" * 100)
    image = _map_image(builder, tmp_path, [(par, "Map\\PAR_MNG.ME"),
                                            (alldata, "Map\\ALLDATA.KWI")])
    assert cup2006.hdd_map_problem(image) is None


@pytest.mark.parametrize("files, why", [
    ([], "no MAP folder"),
    (["PAR_MNG.ME"], "no MAP\\ALLDATA.KWI"),
    (["ALLDATA.KWI:zeros"], "only zeros"),
])
def test_missing_map_is_reported(builder, tmp_path, files, why):
    entries = []
    for spec in files:
        name, _, kind = spec.partition(":")
        path = tmp_path / name
        path.write_bytes(b"\x00" * 4096 if kind == "zeros" else b"x" * 64)
        entries.append((path, "Map\\" + name))
    image = _map_image(builder, tmp_path, entries)
    assert why in cup2006.hdd_map_problem(image)


def test_map_check_rejects_non_images(tmp_path):
    junk = tmp_path / "junk.img"
    junk.write_bytes(b"\x00" * 1024)
    assert cup2006.hdd_map_problem(junk) == "no MBR"
    assert "cannot read" in cup2006.hdd_map_problem(tmp_path / "missing.img")


def test_existing_hdd_without_a_map_warns(monkeypatch, builder, tmp_path,
                                          capsys):
    image = _map_image(builder, tmp_path, [])
    monkeypatch.setattr(cup2006, "HDD", image)
    assert cup2006.ensure_hdd() == image
    err = capsys.readouterr().err
    assert "has no installed map" in err and "Disc unavailable" in err
