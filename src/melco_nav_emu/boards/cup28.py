# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Prepare a CUP28 unit from its map DVD and launch the stock SHx image."""
from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path
from typing import Dict, List, Optional

from ..images import ImageFormatError, flatten_b000ff, parse_b000ff
from ..iso import read_root_file
from ..loading import parse_loading

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_QEMU = ROOT / "qemu" / "src" / "build" / "qemu-system-sh4"
DEFAULT_UNIT = ROOT / "build" / "cup28"

FIRMWARE_MODULE = "CUP28.SHx.ORG"
FLASH_SIZE = 32 << 20
LOAD_ADDRESS = 0x80080000
ENTRY_ADDRESS = LOAD_ADDRESS + 4


def firmware_from_disc(disc: Path) -> bytes:
    loading = parse_loading(read_root_file(disc, "LOADING.KWI"))
    module = next((item for item in loading.modules
                   if item.name == FIRMWARE_MODULE), None)
    if module is None:
        raise ImageFormatError("%s is absent from the disc's LOADING.KWI"
                               % FIRMWARE_MODULE)
    body = loading.body(module)
    image = parse_b000ff(body)
    if (image.image_start != LOAD_ADDRESS or image.exec_start != ENTRY_ADDRESS or
            not 4 < image.image_length <= FLASH_SIZE - (LOAD_ADDRESS & 0x1fffffff)):
        raise ImageFormatError("unsupported CUP28 firmware address or size")
    if not any(record.address <= ENTRY_ADDRESS < record.address + record.size
               for record in image.records):
        raise ImageFormatError("CUP28 firmware has no code at its entry point")
    for record in image.records:
        start = record.file_offset
        checksum = sum(memoryview(body)[start:start + record.size]) & 0xffffffff
        if checksum != record.checksum:
            raise ImageFormatError("firmware checksum failed at %#x" % record.address)
    return flatten_b000ff(body)


def prepare_firmware(disc: Path, unit: Path) -> Path:
    firmware = firmware_from_disc(disc)
    digest = hashlib.sha256(firmware).hexdigest()
    unit.mkdir(mode=0o700, parents=True, exist_ok=True)
    target = unit / ("nk-" + digest + ".nb0")
    if target.is_file() and target.read_bytes() == firmware:
        return target
    with tempfile.NamedTemporaryFile(dir=unit, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(firmware)
            output.close()
            temporary.replace(target)
        finally:
            if temporary.exists():
                temporary.unlink()
    return target


def _escape(value: object) -> str:
    return str(value).replace(",", ",,")


def prepare_unit(unit: Path) -> None:
    """The CUP28 machine creates its flash/SRAM files itself; just make the
    unit directory."""
    Path(unit).mkdir(mode=0o700, parents=True, exist_ok=True)


def prepare(unit, args=None) -> Dict[str, object]:
    """Prepare a CUP28 unit's media from its map DVD and return what
    qemu_argv needs."""
    disc = getattr(args, "disc", None) or os.environ.get("MELCO_DISC") \
        or os.environ.get("CUP28_DISC")
    if not disc:
        raise ImageFormatError("CUP28 requires --disc, MELCO_DISC or CUP28_DISC")
    disc = Path(disc).resolve(strict=True)
    if unit is None:
        unit = os.environ.get("MELCO_UNIT") or os.environ.get("CUP28_UNIT") \
            or DEFAULT_UNIT
    unit = Path(unit)
    if not unit.is_absolute():
        unit = ROOT / unit
    qemu = Path(getattr(args, "qemu", None) or DEFAULT_QEMU).resolve()
    if not qemu.is_file():
        raise ImageFormatError("QEMU not found at %s. Run 'make qemu' first." % qemu)
    track = getattr(args, "track", None)
    track = Path(track).resolve(strict=True) if track else None
    firmware = prepare_firmware(disc, unit)
    return {"qemu": qemu, "disc": disc, "unit": unit, "firmware": firmware,
            "track": track, "audio": getattr(args, "audio", None)}


def qemu_argv(media: dict, *, display: str, serial=None, extra=(),
              qmp=None) -> List[str]:
    """The QEMU command line for a unit prepared by prepare()."""
    properties = {
        "dvdimage": media["disc"],
        "drfimage": media["unit"] / "flash.bin",
        "sramsave": media["unit"] / "sram.bin",
    }
    if media.get("track") is not None:
        properties["gpstrack"] = media["track"]
    options = ["cup28"] + [
        key + "=" + _escape(value) for key, value in properties.items()
    ]
    audio = media.get("audio")
    if audio:
        options.append("audiodev=nav")
    argv = [str(media["qemu"]), "-M", ",".join(options),
            "-kernel", str(media["firmware"]),
            "-display", display, "-monitor", "none", "-no-reboot",
            "-serial", "null", "-serial", "null", "-serial", "stdio",
            "-icount", "shift=auto,sleep=on"]
    if audio:
        if audio == "coreaudio":
            audio += ",out.buffer-count=32"
        argv += ["-audiodev", audio + ",id=nav"]
    if qmp is not None:
        argv += ["-qmp", "unix:%s,server=on,wait=off" % _escape(qmp)]
    return argv + [str(a) for a in extra]
