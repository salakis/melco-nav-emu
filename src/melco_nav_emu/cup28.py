# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Prepare a CUP28 unit from its map DVD and launch the stock SHx image."""
from __future__ import annotations

import hashlib
import tempfile
from pathlib import Path
from typing import List, Optional

from .images import ImageFormatError, flatten_b000ff, parse_b000ff
from .iso import read_root_file
from .loading import parse_loading

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


def qemu_argv(qemu: Path, *, disc: Path, unit: Path, firmware: Path,
              display: str, audio: Optional[str] = None,
              track: Optional[Path] = None, qmp: Optional[Path] = None) -> List[str]:
    properties = {
        "dvdimage": disc,
        "drfimage": unit / "flash.bin",
        "sramsave": unit / "sram.bin",
    }
    if track is not None:
        properties["gpstrack"] = track
    options = ["cup28"] + [
        key + "=" + _escape(value) for key, value in properties.items()
    ]
    if audio:
        options.append("audiodev=nav")
    argv = [str(qemu), "-M", ",".join(options), "-kernel", str(firmware),
            "-display", display, "-monitor", "none", "-no-reboot",
            "-serial", "null", "-serial", "null", "-serial", "stdio",
            "-icount", "shift=auto,sleep=on"]
    if audio:
        if audio == "coreaudio":
            audio += ",out.buffer-count=32"
        argv += ["-audiodev", audio + ",id=nav"]
    if qmp is not None:
        argv += ["-qmp", "unix:%s,server=on,wait=off" % _escape(qmp)]
    return argv
