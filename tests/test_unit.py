# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""tools/unit.py: new units start erased; the completed-install record is
landed in guest-formatted APM areas, never in a factory unit's."""
from tools import unit


def _format_apm(data, area, status=99):
    data[area:area + 8] = unit.APM_DATE
    for n in (0, 1):
        data[area + 0x10 + n * unit.RECORD + unit.MAPDWLSTS] = status


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


def test_new_unit_starts_with_erased_flash(tmp_path):
    u = unit.prepare(tmp_path / "u")
    flash = (u / "flash.img").read_bytes()
    assert len(flash) == 32 << 20 and set(flash) == {0xFF}
    assert unit.map_dwl_sts(u / "flash.img") is None
    assert not (u / ".installed").exists()


def test_prepare_leaves_a_guest_written_factory_record_alone(tmp_path):
    base = tmp_path / "base.img"
    base.write_bytes(b"\0" * 4096)
    u = unit.factory(tmp_path / "u", base)
    flash = u / "flash.img"
    data = bytearray(flash.read_bytes())
    # The guest has formatted its APM area and recorded an install in progress.
    _format_apm(data, unit.FLASH_APM_AREA)
    flash.write_bytes(bytes(data))
    unit.prepare(u, first_power_on=lambda d: _fail("powered on"))
    assert flash.read_bytes() == bytes(data)
    assert unit.map_dwl_sts(flash) == [99, 99]
    assert not (u / ".installed").exists()


def test_prepare_relands_the_completed_install_record(tmp_path):
    """A non-factory unit gets the completed-install record at every boot
    start, so a reboot does not fail the guest's map version check."""
    u = tmp_path / "u"
    u.mkdir()
    flash = u / "flash.img"
    data = bytearray(b"\xff" * (32 << 20))
    for area in (0x1e40800, 0x1e80800, 0x1ec0800):
        _format_apm(data, area)
    flash.write_bytes(bytes(data))
    unit.prepare(u)
    assert unit.map_dwl_sts(flash) == [0, 0]
    assert unit._dwl_sts(flash, 0x1ec0800) == [0, 0]
    assert (u / ".installed").exists()
    # And again on the next boot (the guest rewrote 99 in between).
    data = bytearray(flash.read_bytes())
    _format_apm(data, unit.FLASH_APM_AREA)
    flash.write_bytes(bytes(data))
    unit.prepare(u)
    assert unit.map_dwl_sts(flash) == [0, 0]


def test_prepare_lands_the_record_in_battery_ram(tmp_path):
    """On a production board the guest reads the APM record from battery
    RAM; a guest-formatted battery RAM gets the completed-install record."""
    u = tmp_path / "u"
    u.mkdir()
    (u / "flash.img").write_bytes(b"\xff" * (32 << 20))
    bkram = u / "bkram.bin"
    data = bytearray(1 << 20)
    _format_apm(data, unit.BKRAM_APM_AREA)
    bkram.write_bytes(bytes(data))
    unit.prepare(u)
    assert unit.bkram_dwl_sts(bkram) == [0, 0]
    assert len(bkram.read_bytes()) == 1 << 20


def test_prepare_powers_a_new_unit_on_once(tmp_path):
    """A unit whose battery RAM is not formatted gets one first power-on;
    afterwards the record is landed there."""
    u = tmp_path / "u"
    calls = []

    def power_on(unit_dir):
        calls.append(unit_dir)
        data = bytearray(1 << 20)
        _format_apm(data, unit.BKRAM_APM_AREA)
        (unit_dir / "bkram.bin").write_bytes(bytes(data))

    unit.prepare(u, first_power_on=power_on)
    unit.prepare(u, first_power_on=power_on)
    assert calls == [u]
    assert unit.bkram_dwl_sts(u / "bkram.bin") == [0, 0]


def test_prepare_skips_power_on_after_a_timed_out_one(tmp_path):
    u = tmp_path / "u"
    u.mkdir()
    (u / unit.POWERED_ON).write_text("timed out\n")
    unit.prepare(u, first_power_on=lambda d: _fail("powered on again"))
    assert not (u / ".installed").exists()


def test_status_of_a_short_or_missing_store_is_none(tmp_path):
    bkram = tmp_path / "bkram.bin"
    assert unit.bkram_dwl_sts(bkram) is None
    data = bytearray(unit.BKRAM_APM_AREA + 0x100)      # cut off mid-area
    data[unit.BKRAM_APM_AREA:unit.BKRAM_APM_AREA + 8] = unit.APM_DATE
    bkram.write_bytes(bytes(data))
    assert unit.bkram_dwl_sts(bkram) is None
