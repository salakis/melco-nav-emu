# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Prepare a CUP28 unit from its map DVD and launch the stock SHx image."""
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from ..images import ImageFormatError, flatten_b000ff, parse_b000ff
from ..iso import read_root_file, volume_id
from ..loading import parse_loading
from ..unit import POWERED_ON

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_QEMU = ROOT / "qemu" / "src" / "build" / "qemu-system-sh4"
DEFAULT_UNIT = ROOT / "build" / "cup28"

FIRMWARE_MODULE = "CUP28.SHx.ORG"
FLASH_SIZE = 32 << 20
LOAD_ADDRESS = 0x80080000
ENTRY_ADDRESS = LOAD_ADDRESS + 4

# The car the unit sits in, as the vehicle peer announces it over MOST:
# market in F5/2B2 and language in F5/2C1.  A real car has both set in the
# workshop; here they follow the map disc in the drive, whose volume name
# starts with the market (`NA_0900_06A_WESTERN`), so a unit in the emulator
# regionalises as the car the disc was made for.  The unit logs what it
# parsed (`NMOSTHandler::ParseNavigationRegion naviRegion=` and
# `ParseLanguage language=`) and persists it, so the pair is settled on the
# unit's first power-on and kept afterwards.  CUP28_CARMARKET and
# CUP28_CARLANGUAGE override either byte, as hex.
DISC_MARKETS = (
    ("NA", "06", "11"),   # North America: market 06, US English
    ("EU", "04", "00"),   # Europe: market 04, no language: the unit keeps
                          # its own (UK English, so kilometres)
)
# What an unrecognised volume name gets: the tested North American set.
DISC_MARKETS_DEFAULT = DISC_MARKETS[0][1:]


def hex_byte_text(value: str, name: str) -> str:
    """One byte, as two hex digits, from a config value like `04`."""
    try:
        number = int(value, 16)
    except (TypeError, ValueError):
        number = -1
    if not 0 <= number <= 0xff:
        raise ImageFormatError("%s: a hex byte, not %r" % (name, value))
    return "%02x" % number


def car_market(disc: Path) -> Tuple[str, str]:
    """(market, language) the vehicle peer announces for `disc`, as hex."""
    try:
        name = volume_id(disc).upper()
    except (ImageFormatError, OSError):
        name = ""
    market, language = DISC_MARKETS_DEFAULT
    for prefix, market_hex, language_hex in DISC_MARKETS:
        if name.startswith(prefix):
            market, language = market_hex, language_hex
            break
    market = hex_byte_text(os.environ.get("CUP28_CARMARKET", market),
                           "CUP28_CARMARKET")
    language = hex_byte_text(os.environ.get("CUP28_CARLANGUAGE", language),
                             "CUP28_CARLANGUAGE")
    return market, language


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


# Where the machine programs the firmware image into the flash
# (CUP28_FIRMWARE_OFFSET in machine/cup28.c); everything after it is the
# guest's own filesystem.
FIRMWARE_OFFSET = 0x80000
FIRST_POWER_ON_LOG = "first-power-on.log"
FIRST_POWER_ON_S = 120
# How long the guest's flash must stay unchanged before its setup is over.  A
# fresh unit writes for about five seconds (measured) and the write must not
# be interrupted: a power-on cut short mid-write leaves a half-programmed
# block, and the next boot starts the setup again.
FIRST_POWER_ON_QUIET_S = 10
POLL_S = 1.0


def unit_firmware(unit: Path) -> Optional[Path]:
    """The firmware image the unit's flash holds, when there is only one (a
    unit directory used with two disc sets keeps one image per set)."""
    found = sorted(Path(unit).glob("nk-*.nb0"))
    return found[0] if len(found) == 1 else None


def guest_flash(unit: Path, firmware: Optional[Path]) -> Optional[bytes]:
    """What the guest has put in the unit's flash, or None when it has written
    nothing yet.

    The machine programs the firmware image into an erased chip, so the region
    after that image is the guest's own: a unit whose flash holds anything
    there has been powered on before."""
    unit, firmware = Path(unit), (Path(firmware) if firmware else None)
    flash = unit / "flash.bin"
    if not flash.is_file() or firmware is None:
        return None
    try:
        data = flash.read_bytes()
    except OSError:
        return None
    start = FIRMWARE_OFFSET + firmware.stat().st_size
    if len(data) <= start:
        return None
    tail = data[start:]
    return None if tail.count(0xff) == len(tail) else tail


def flash_written(unit: Path, firmware: Optional[Path] = None) -> bool:
    """Has the guest written its own data into the unit's flash yet?"""
    return guest_flash(unit, firmware or unit_firmware(unit)) is not None


def first_power_on(unit: Path, media: dict,
                   timeout: int = FIRST_POWER_ON_S) -> bool:
    """Power a new unit on once, headless, until the guest has finished
    writing its flash, then switch it off.  Once per unit: about 15 s on the
    North American disc, up to a minute on a European one.

    A unit's first boot is the one where the guest builds the files it keeps
    in flash, and it cannot apply the language it is told over MOST until
    they exist: it logs `VPL_DataManager SetLangError` three times and keeps
    its factory default (UK English, metric) for that whole session.  From
    the next power-on the announced market and language are in effect, so
    `boot` spends that boot here — as the CU-P2006 launcher spends its own
    first power-on on the guest's battery RAM — and the window opens on a
    unit that is already the car the disc belongs to.

    If the guest has not written its flash after `timeout` seconds, the unit
    gets a .powered-on marker so that later boots do not repeat this.
    """
    unit = Path(unit)
    unit.mkdir(mode=0o700, parents=True, exist_ok=True)
    log = unit / FIRST_POWER_ON_LOG
    argv = qemu_argv(dict(media, audio=None), display="none")
    print("cup28: first power-on of %s: letting the unit set itself up "
          "(once, up to %d s; log: %s)..." % (unit, timeout, log),
          file=sys.stderr)
    with log.open("wb") as out:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out,
                                stderr=subprocess.STDOUT)
    deadline = time.monotonic() + timeout
    settled = exited = None
    last, quiet_since = None, None
    try:
        while time.monotonic() < deadline:
            time.sleep(POLL_S)
            written = guest_flash(unit, media["firmware"])
            now = time.monotonic()
            if written is None or written != last:
                last, quiet_since = written, now
            elif now - quiet_since >= FIRST_POWER_ON_QUIET_S:
                settled = True
                break
            exited = proc.poll()
            if exited is not None:
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    if settled:
        return True
    if exited is not None:
        tail = log.read_text(errors="replace").splitlines()[-20:]
        raise SystemExit(
            "cup28: QEMU exited with code %d during the first power-on of %s, "
            "before the guest wrote its flash.  Last lines of %s:\n%s"
            % (exited, unit, log, "\n".join(tail)))
    (unit / POWERED_ON).write_text(
        "first power-on ran %d s without the guest writing its flash; delete "
        "this file to try again\n" % timeout)
    print("cup28: WARNING: the guest did not write its flash within %d s, so "
          "this unit is not set up and the next boot will come up in the "
          "guest's factory region and language.  This power-on is not repeated "
          "(delete %s to retry)." % (timeout, unit / POWERED_ON),
          file=sys.stderr)
    return False


def prepare_unit(unit: Path, media: Optional[dict] = None) -> None:
    """The CUP28 machine creates its flash/SRAM/EEPROM files itself; make the
    unit directory and, with `media` (a disc to prepare the unit for), spend
    one headless power-on on a unit that has never been powered on."""
    unit = Path(unit)
    unit.mkdir(mode=0o700, parents=True, exist_ok=True)
    if media is None or (unit / POWERED_ON).exists():
        return
    if flash_written(unit, media["firmware"]):
        return
    first_power_on(unit, media)


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
    market, language = car_market(disc)
    firmware = prepare_firmware(disc, unit)
    media = {"qemu": qemu, "disc": disc, "unit": unit, "firmware": firmware,
             "track": track, "audio": getattr(args, "audio", None),
             "carmarket": market, "carlanguage": language}
    # A unit that has never been powered on gets its setup boot here, headless.
    prepare_unit(unit, media)
    return media


def qemu_argv(media: dict, *, display: str, serial=None, extra=(),
              qmp=None) -> List[str]:
    """The QEMU command line for a unit prepared by prepare()."""
    properties = {
        "dvdimage": media["disc"],
        "drfimage": media["unit"] / "flash.bin",
        "sramsave": media["unit"] / "sram.bin",
        "eeprom": media["unit"] / "eeprom.bin",
        "carmarket": media["carmarket"],
        "carlanguage": media["carlanguage"],
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
