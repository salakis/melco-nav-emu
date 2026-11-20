# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""tools/profile.py: disc set, NK lookup, GPS scenario, HDD build and the
first power-on, without QEMU or disc images."""
import importlib
import struct
import sys
from pathlib import Path

import pytest

import tools.profile as profile


@pytest.fixture
def fresh_profile(monkeypatch):
    for var in ("CUP2006_DISC1", "CUP2006_DISC2", "CUP2006_CARMARKET",
                "CUP2006_SCENARIO"):
        monkeypatch.delenv(var, raising=False)
    importlib.reload(profile)
    yield profile
    monkeypatch.undo()
    importlib.reload(profile)


def test_default_disc_set_is_eu(fresh_profile):
    assert profile.DISC.name == "EU-3350-00_1.ISO"
    assert profile.DISC2.name == "EU-3350-00_2.ISO"
    assert profile.CARMARKET == "04"


def test_na_disc_set_selects_na_market(fresh_profile, monkeypatch, tmp_path):
    na1 = tmp_path / "NA-2450-00_1.ISO"
    na2 = tmp_path / "NA-2450-00_2.ISO"
    na1.write_bytes(b"")
    na2.write_bytes(b"")
    monkeypatch.setenv("CUP2006_DISC1", str(na1))
    monkeypatch.setenv("CUP2006_DISC2", str(na2))
    importlib.reload(profile)
    assert profile.DISC == na1 and profile.DISC2 == na2
    assert profile.CARMARKET == "06"          # NA disc set -> region 2
    assert "dvdimage=%s" % na1 in profile.MACHINE
    assert "dvdnext=%s" % na2 in profile.MACHINE
    assert "carmarket=06" in profile.MACHINE


def test_import_prints_nothing(fresh_profile, monkeypatch, capsys):
    monkeypatch.setenv("CUP2006_SCENARIO", "no-such-scenario")
    importlib.reload(profile)
    assert capsys.readouterr() == ("", "")


def test_find_nk(fresh_profile, tmp_path):
    with pytest.raises(SystemExit, match="make extract flatten"):
        profile.find_nk(tmp_path)
    nk = tmp_path / "CUP2006.SHx.ORG_v00002308.nb0"
    nk.write_bytes(b"")
    (tmp_path / "CUP2006.MLD.ORG_v00000253.nb0").write_bytes(b"")
    assert profile.find_nk(tmp_path) == nk
    (tmp_path / "CUP2006.SHx.ORG_v00002400.nb0").write_bytes(b"")
    with pytest.raises(SystemExit, match="several NK images"):
        profile.find_nk(tmp_path)


def test_gps_scenario_only_when_set(fresh_profile, monkeypatch, tmp_path):
    assert profile.gps_machine() == []
    monkeypatch.setattr(profile, "ROOT", tmp_path)
    monkeypatch.setenv("CUP2006_SCENARIO", "drive1")
    with pytest.raises(SystemExit, match="CUP2006_SCENARIO=drive1"):
        profile.gps_machine()
    scenario = tmp_path / "build/gps/drive1"
    scenario.mkdir(parents=True)
    (scenario / "drive.replay").write_bytes(b"")
    (scenario / "sensors.csv").write_text("")
    assert profile.gps_machine() == [
        "gnssreplay=%s" % (scenario / "drive.replay"),
        "gpstrack=%s" % (scenario / "sensors.csv")]
    # A shipped scenario of the same name comes first.
    shipped = tmp_path / "scenarios/drive1"
    shipped.mkdir(parents=True)
    (shipped / "drive.replay").write_bytes(b"")
    (shipped / "sensors.csv").write_text("")
    assert profile.gps_machine()[0] == "gnssreplay=%s" % (shipped / "drive.replay")


def test_shipped_scenarios_are_complete():
    """Every scenario in the repository has what the machine and its
    provenance need."""
    root = Path(profile.__file__).resolve().parents[1] / "scenarios"
    names = sorted(p.name for p in root.iterdir() if p.is_dir())
    assert names
    for name in names:
        d = root / name
        for f in ("drive.replay", "sensors.csv", "route.json", "README.md"):
            assert (d / f).is_file(), (name, f)
        head = (d / "drive.replay").read_bytes()[:16]
        assert head[:8] == b"GNSSIQ1\0"
        frames, interval = struct.unpack_from("<II", head, 8)
        assert (d / "drive.replay").stat().st_size == 16 + frames * 4096
        assert frames * interval <= 181000         # at most about 3 minutes


def test_argv_names_the_unit_disk_and_nk(fresh_profile, monkeypatch, tmp_path):
    qemu = tmp_path / "qemu-system-sh4"
    qemu.write_text("")
    firmware = tmp_path / "firmware"
    firmware.mkdir()
    nk = firmware / "CUP2006.SHx.ORG_v00002308.nb0"
    nk.write_bytes(b"")
    monkeypatch.setattr(profile, "QEMU", qemu)
    monkeypatch.setattr(profile, "FIRMWARE", firmware)
    unit = tmp_path / "unit"
    unit.mkdir()
    argv = profile._argv(unit, "none", (), (), tmp_path / "shared.img")
    machine = argv[argv.index("-M") + 1].split(",")
    assert "hddimage=%s" % (tmp_path / "shared.img") in machine
    assert "hddwrite=on" not in machine
    assert argv[argv.index("-kernel") + 1] == str(nk)
    (unit / "hdd.img").write_bytes(b"")
    argv = profile._argv(unit, "none", (), (), profile.unit_hdd(unit))
    machine = argv[argv.index("-M") + 1].split(",")
    assert "hddimage=%s" % (unit / "hdd.img") in machine
    assert "hddwrite=on" in machine


def _fake_builder(tmp_path, ok):
    """A stand-in image builder: writes its --out file, then fails or not."""
    script = tmp_path / "builder.py"
    script.write_text(
        "import sys\n"
        "out = sys.argv[sys.argv.index('--out') + 1]\n"
        "open(out, 'wb').write(b'half')\n"
        "sys.exit(%d)\n" % (0 if ok else 1))
    return [sys.executable, str(script)]


def test_failed_build_leaves_no_image(fresh_profile, tmp_path):
    out = tmp_path / "hdd" / "disk.img"
    with pytest.raises(Exception):
        profile.build_atomically(out, _fake_builder(tmp_path, ok=False))
    assert list(out.parent.iterdir()) == []
    profile.build_atomically(out, _fake_builder(tmp_path, ok=True))
    assert out.read_bytes() == b"half"
    assert list(out.parent.iterdir()) == [out]


def test_empty_hdd_fallback_is_not_the_product_image(fresh_profile, monkeypatch,
                                                     tmp_path, capsys):
    monkeypatch.setattr(profile, "HDD", tmp_path / "hdd/product.img")
    monkeypatch.setattr(profile, "HDD_EMPTY", tmp_path / "hdd/empty.img")
    monkeypatch.setattr(profile, "MANIFEST", tmp_path / "manifest.json")
    monkeypatch.setattr(profile, "SCE", tmp_path / "P2006.SCE")
    monkeypatch.setattr(profile, "DISC", tmp_path / "disc1.iso")
    monkeypatch.setattr(profile, "DISC2", tmp_path / "disc2.iso")
    monkeypatch.setattr(profile, "BOOT_PART_MB", 64)
    monkeypatch.setattr(profile, "MAP_PART_MB", 64)
    assert profile.ensure_hdd() == profile.HDD_EMPTY
    assert profile.HDD_EMPTY.is_file() and not profile.HDD.exists()
    assert "EMPTY map partition" in capsys.readouterr().err
    assert not (tmp_path / "hdd/empty.img.partial").exists()


def _fake_qemu(monkeypatch, tmp_path, code):
    script = tmp_path / "fake_qemu.py"
    script.write_text(code)
    monkeypatch.setattr(profile, "_argv",
                        lambda *a, **k: [sys.executable, str(script)])
    monkeypatch.setattr(profile, "POLL_S", 0.05)
    monkeypatch.setattr(profile, "BKRAM_SAVE_S", 0.05)


def test_first_power_on_reports_an_early_qemu_exit(fresh_profile, monkeypatch,
                                                   tmp_path):
    _fake_qemu(monkeypatch, tmp_path,
               "import sys\nprint('qemu: could not open image')\nsys.exit(3)\n")
    unit = tmp_path / "unit"
    with pytest.raises(SystemExit) as exc:
        profile.first_power_on(unit, timeout=30, hdd=tmp_path / "x.img")
    assert "code 3" in str(exc.value)
    assert "could not open image" in str(exc.value)
    assert (unit / profile.FIRST_POWER_ON_LOG).is_file()
    assert not (unit / ".powered-on").exists()


def test_first_power_on_timeout_marks_the_unit(fresh_profile, monkeypatch,
                                               tmp_path, capsys):
    _fake_qemu(monkeypatch, tmp_path, "import time\ntime.sleep(60)\n")
    unit = tmp_path / "unit"
    assert profile.first_power_on(unit, timeout=0.3,
                                  hdd=tmp_path / "x.img") is False
    assert (unit / ".powered-on").exists()
    assert "could not be landed" in capsys.readouterr().err


def test_first_power_on_stops_once_battery_ram_is_formatted(
        fresh_profile, monkeypatch, tmp_path):
    from tools import unit as unit_mod
    unit = tmp_path / "unit"
    bkram = unit / "bkram.bin"
    _fake_qemu(monkeypatch, tmp_path,
               "import time\n"
               "d = bytearray(1 << 20)\n"
               "d[0x40800:0x40808] = b'20080207'\n"
               "open(%r, 'wb').write(bytes(d))\n"
               "time.sleep(60)\n" % str(bkram))
    assert profile.first_power_on(unit, timeout=30,
                                  hdd=tmp_path / "x.img") is True
    assert unit_mod.bkram_dwl_sts(bkram) == [0, 0]
    assert not (unit / ".powered-on").exists()


def test_audiodev_id_parses_every_form():
    assert profile.audiodev_id(["-audiodev", "coreaudio,id=snd0"]) == "snd0"
    # id= need not be the first property
    assert profile.audiodev_id(
        ["-audiodev", "coreaudio,foo=bar,id=snd1,x=1"]) == "snd1"
    assert profile.audiodev_id(["-audiodev=id=snd2,driver=coreaudio"]) == "snd2"
    assert profile.audiodev_id(["-display", "none"]) is None
    assert profile.audiodev_id(["-audiodev"]) is None          # no spec arg
    assert profile.audiodev_id(["-audiodev", "coreaudio"]) is None  # no id=


def _argv_fixture(fresh_profile, monkeypatch, tmp_path):
    qemu = tmp_path / "qemu-system-sh4"
    qemu.write_text("")
    firmware = tmp_path / "firmware"
    firmware.mkdir()
    (firmware / "CUP2006.SHx.ORG_v00002308.nb0").write_bytes(b"")
    monkeypatch.setattr(profile, "QEMU", qemu)
    monkeypatch.setattr(profile, "FIRMWARE", firmware)
    unit = tmp_path / "unit"
    unit.mkdir()
    return unit, tmp_path / "shared.img"


def test_argv_wires_a_caller_audiodev_to_the_machine(
        fresh_profile, monkeypatch, tmp_path):
    unit, hdd = _argv_fixture(fresh_profile, monkeypatch, tmp_path)
    argv = profile._argv(unit, "none", (),
                         ["-audiodev", "coreaudio,id=cap0"], hdd)
    machine = argv[argv.index("-M") + 1].split(",")
    assert "audiodev=cap0" in machine
    # the caller's -audiodev is passed through once; none is added
    assert argv.count("-audiodev") == 1
    assert argv[argv.index("-audiodev") + 1] == "coreaudio,id=cap0"


def test_argv_rejects_audiodev_without_id(
        fresh_profile, monkeypatch, tmp_path):
    unit, hdd = _argv_fixture(fresh_profile, monkeypatch, tmp_path)
    with pytest.raises(SystemExit):
        profile._argv(unit, "none", (), ["-audiodev", "coreaudio"], hdd)


def test_argv_adds_coreaudio_on_cocoa(fresh_profile, monkeypatch, tmp_path):
    unit, hdd = _argv_fixture(fresh_profile, monkeypatch, tmp_path)
    argv = profile._argv(unit, "cocoa,zoom-to-fit=on", (), (), hdd)
    machine = argv[argv.index("-M") + 1].split(",")
    assert "audiodev=snd0" in machine
    i = argv.index("-audiodev")
    assert argv[i + 1] == "coreaudio,id=snd0"


def test_argv_headless_adds_no_audio(fresh_profile, monkeypatch, tmp_path):
    unit, hdd = _argv_fixture(fresh_profile, monkeypatch, tmp_path)
    argv = profile._argv(unit, "none", (), (), hdd)
    assert "-audiodev" not in argv
    assert not any(m.startswith("audiodev=")
                   for m in argv[argv.index("-M") + 1].split(","))
