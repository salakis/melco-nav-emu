# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""melco_nav_emu/unit.py: new units start erased; the completed-install
record is landed in guest-formatted APM areas, never in a factory unit's."""
import pytest

from melco_nav_emu import unit
from melco_nav_emu.boards import cup2006


@pytest.fixture
def no_power_on(monkeypatch, tmp_path):
    """Keep prepare() offline: no real first power-on, no HDD lookup."""
    monkeypatch.setattr(cup2006, "first_power_on", lambda *a, **k: None)
    monkeypatch.setattr(cup2006, "unit_hdd", lambda u: tmp_path / "hdd.img")


def _format_apm(data, area, status=99):
    data[area:area + 8] = cup2006.APM_DATE
    for n in (0, 1):
        data[area + 0x10 + n * cup2006.RECORD + cup2006.MAPDWLSTS] = status


def _fail(msg):
    raise AssertionError(msg)


def test_factory_unit_is_erased_and_owns_its_disk(tmp_path):
    base = tmp_path / "base.img"
    base.write_bytes(b"\0" * 4096)
    u = unit.factory(tmp_path / "u", base)
    flash = (u / "flash.img").read_bytes()
    assert len(flash) == 32 << 20 and set(flash) == {0xFF}
    assert (u / "hdd.img").read_bytes() == base.read_bytes()
    assert (u / ".factory").exists()


def test_new_unit_starts_with_erased_flash(tmp_path, no_power_on):
    u = unit.prepare(tmp_path / "u", "cup2006")
    flash = (u / "flash.img").read_bytes()
    assert len(flash) == 32 << 20 and set(flash) == {0xFF}
    assert cup2006.map_dwl_sts(u / "flash.img") is None
    assert not (u / ".installed").exists()


def test_prepare_leaves_a_guest_written_factory_record_alone(tmp_path, no_power_on):
    base = tmp_path / "base.img"
    base.write_bytes(b"\0" * 4096)
    u = unit.factory(tmp_path / "u", base)
    flash = u / "flash.img"
    data = bytearray(flash.read_bytes())
    # The guest has formatted its APM area and recorded an install in progress.
    _format_apm(data, cup2006.FLASH_APM_AREA)
    flash.write_bytes(bytes(data))
    unit.prepare(u, "cup2006")
    assert flash.read_bytes() == bytes(data)
    assert cup2006.map_dwl_sts(flash) == [99, 99]
    assert not (u / ".installed").exists()


def test_prepare_relands_the_completed_install_record(tmp_path, no_power_on):
    """A non-factory unit gets the completed-install record at every boot
    start, so a reboot does not fail the guest's map version check."""
    u = tmp_path / "u"
    u.mkdir()
    flash = u / "flash.img"
    data = bytearray(b"\xff" * (32 << 20))
    for area in (0x1e40800, 0x1e80800, 0x1ec0800):
        _format_apm(data, area)
    flash.write_bytes(bytes(data))
    unit.prepare(u, "cup2006")
    assert cup2006.map_dwl_sts(flash) == [0, 0]
    assert cup2006._dwl_sts(flash, 0x1ec0800) == [0, 0]
    assert (u / ".installed").exists()
    # And again on the next boot (the guest rewrote 99 in between).
    data = bytearray(flash.read_bytes())
    _format_apm(data, cup2006.FLASH_APM_AREA)
    flash.write_bytes(bytes(data))
    unit.prepare(u, "cup2006")
    assert cup2006.map_dwl_sts(flash) == [0, 0]


def test_prepare_lands_the_record_in_battery_ram(tmp_path, no_power_on):
    """On a production board the guest reads the APM record from battery
    RAM; a guest-formatted battery RAM gets the completed-install record."""
    u = tmp_path / "u"
    u.mkdir()
    (u / "flash.img").write_bytes(b"\xff" * (32 << 20))
    bkram = u / "bkram.bin"
    data = bytearray(1 << 20)
    _format_apm(data, cup2006.BKRAM_APM_AREA)
    bkram.write_bytes(bytes(data))
    unit.prepare(u, "cup2006")
    assert cup2006.bkram_dwl_sts(bkram) == [0, 0]
    assert len(bkram.read_bytes()) == 1 << 20


def test_prepare_powers_a_new_unit_on_once(tmp_path, monkeypatch):
    """A unit whose battery RAM is not formatted gets one first power-on;
    afterwards the record is landed there."""
    monkeypatch.setattr(cup2006, "unit_hdd", lambda u: tmp_path / "hdd.img")
    u = tmp_path / "u"
    calls = []

    def power_on(unit_dir, **kwargs):
        calls.append(unit_dir)
        data = bytearray(1 << 20)
        _format_apm(data, cup2006.BKRAM_APM_AREA)
        (unit_dir / "bkram.bin").write_bytes(bytes(data))

    monkeypatch.setattr(cup2006, "first_power_on", power_on)
    unit.prepare(u, "cup2006")
    unit.prepare(u, "cup2006")
    assert calls == [u]
    assert cup2006.bkram_dwl_sts(u / "bkram.bin") == [0, 0]


def test_prepare_skips_power_on_after_a_timed_out_one(tmp_path, monkeypatch):
    monkeypatch.setattr(cup2006, "first_power_on",
                        lambda *a, **k: _fail("powered on again"))
    u = tmp_path / "u"
    u.mkdir()
    (u / unit.POWERED_ON).write_text("timed out\n")
    unit.prepare(u, "cup2006")
    assert not (u / ".installed").exists()


def test_status_of_a_short_or_missing_store_is_none(tmp_path):
    bkram = tmp_path / "bkram.bin"
    assert cup2006.bkram_dwl_sts(bkram) is None
    data = bytearray(cup2006.BKRAM_APM_AREA + 0x100)  # cut off mid-area
    data[cup2006.BKRAM_APM_AREA:cup2006.BKRAM_APM_AREA + 8] = cup2006.APM_DATE
    bkram.write_bytes(bytes(data))
    assert cup2006.bkram_dwl_sts(bkram) is None


def test_prepare_lands_the_eeprom_part_number(tmp_path):
    u = tmp_path / "u"
    u.mkdir()
    (u / unit.POWERED_ON).write_text("timed out\n")   # no power-on here
    unit.prepare(u, "cup2006")
    image = (u / "eeprom.bin").read_bytes()
    assert len(image) == cup2006.EEPROM_BYTES
    assert image[112:120] == bytes(5) + b" AA"
    # Everything else stays as the part shipped: erased.
    assert image[:112] == b"\xff" * 112 and image[120:] == b"\xff" * 8
    # Idempotent, and it leaves the guest's own bytes (the config word the
    # driver writes) alone.
    image = bytearray(image)
    image[0x17] = 0x0c
    (u / "eeprom.bin").write_bytes(bytes(image))
    unit.prepare(u, "cup2006")
    after = (u / "eeprom.bin").read_bytes()
    assert after == bytes(image)
