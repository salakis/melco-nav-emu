# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
from pathlib import Path
import struct

import pytest

from melco_nav_emu import cli, cup28
from melco_nav_emu.images import ImageFormatError
from melco_nav_emu.iso import read_root_file


def make_disc(tmp_path: Path, payload: bytes) -> Path:
    sector = 2048
    iso = bytearray(18 * sector)
    pvd = 16 * sector
    root = pvd + 156
    iso[pvd:pvd + 7] = b"\x01CD001\x01"
    iso[root] = 34
    struct.pack_into("<I", iso, root + 2, 17)
    struct.pack_into("<I", iso, root + 10, sector)
    entry = 17 * sector
    name = b"LOADING.KWI;1"
    iso[entry] = 33 + len(name)
    struct.pack_into("<I", iso, entry + 2, 18)
    struct.pack_into("<I", iso, entry + 10, len(payload))
    iso[entry + 32] = len(name)
    iso[entry + 33:entry + 33 + len(name)] = name
    path = tmp_path / "Western, disc 1.iso"
    path.write_bytes(iso + payload)
    return path


CODE = b"\x09\x00\x09\x00\x0b\x00"


def make_loading(name=b"CUP28.SHx.ORG", *, checksum=sum(CODE),
                 entry=0x80080004) -> bytes:
    header = bytearray(4096)
    struct.pack_into(">H", header, 0, 1)
    header[4:16] = b"MID-01234567"
    struct.pack_into(">H", header, 16, 1)
    header[20] = 1
    header[24:24 + len(name)] = name
    header[76:84] = b"00001102"
    struct.pack_into(">I", header, 84 + 250, 0x200)
    base = 0x80080000
    return (bytes(header) + b"B000FF\n" + struct.pack("<II", base, 8)
            + struct.pack("<III", base, len(CODE), checksum) + CODE
            + struct.pack("<III", 0, entry, 0))


def test_stock_image_and_cache_recovery(tmp_path):
    loading = make_loading()
    disc = make_disc(tmp_path, loading)
    assert read_root_file(disc, "loading.kwi") == loading
    unit = tmp_path / "unit"
    firmware = cup28.prepare_firmware(disc, unit)
    assert firmware.read_bytes() == CODE + b"\x00\x00"
    firmware.write_bytes(b"incomplete")
    assert cup28.prepare_firmware(disc, unit) == firmware
    assert firmware.read_bytes() == CODE + b"\x00\x00"
    assert list(unit.iterdir()) == [firmware]


@pytest.mark.parametrize("loading, error", [
    (make_loading(checksum=0), "checksum"),
    (make_loading(entry=0x80080000), "address"),
    (make_loading(b"CUP2006.SHx.ORG"), "CUP28.SHx.ORG"),
])
def test_firmware_rejects_invalid_inputs(tmp_path, loading, error):
    with pytest.raises(ImageFormatError, match=error):
        cup28.firmware_from_disc(make_disc(tmp_path, loading))


def test_truncated_iso(tmp_path):
    disc = make_disc(tmp_path, b"LOADING")
    disc.write_bytes(disc.read_bytes()[:-1])
    with pytest.raises(ImageFormatError, match="truncated"):
        read_root_file(disc, "LOADING.KWI")


def test_cli_boot_uses_persistent_unit_and_unmodified_firmware(tmp_path, monkeypatch):
    disc = make_disc(tmp_path, make_loading())
    unit = tmp_path / "test unit"
    qemu = tmp_path / "qemu-system-sh4"
    qemu.touch()
    command = []

    def execv(path, argv):
        assert path == str(qemu)
        command.extend(argv)

    monkeypatch.setattr(cli.os, "execv", execv)
    cli.main(["boot", "--model", "cup28", "--disc", str(disc), "--unit",
              str(unit), "--qemu", str(qemu), "--display", "none", "--audio",
              "coreaudio"])
    machine = command[command.index("-M") + 1]
    assert machine == (f"cup28,dvdimage={str(disc).replace(',', ',,')},"
                       f"drfimage={unit / 'flash.bin'},sramsave={unit / 'sram.bin'},"
                       "audiodev=nav")
    assert command[command.index("-audiodev") + 1] == "coreaudio,out.buffer-count=32,id=nav"
    kernel = Path(command[command.index("-kernel") + 1])
    assert kernel.read_bytes() == CODE + b"\x00\x00"


def test_default_boot_uses_existing_cup2006_profile(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(cli.os, "execve",
                        lambda path, argv, env: calls.append((argv, env)))
    unit = tmp_path / "cup2006"
    cli.main(["boot", "--unit", str(unit), "--display", "none"])
    [(argv, env)] = calls
    assert argv[-1] == str(cli.PROJECT_ROOT / "tools" / "profile.py")
    assert env["CUP2006_UNIT"] == str(unit)
    assert env["DISPLAY_BACKEND"] == "none"
