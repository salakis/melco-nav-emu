# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Parsers for the Volvo RTI firmware/install image formats."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

B000FF_MAGIC = b"B000FF\n"


class ImageFormatError(ValueError):
    """Raised when an input is not a supported image."""


@dataclass(frozen=True)
class B000FFRecord:
    address: int
    size: int
    checksum: int
    file_offset: int


@dataclass
class B000FFImage:
    image_start: int
    image_length: int
    records: List[B000FFRecord]
    exec_start: int
    container_end: int


def parse_b000ff(data: bytes) -> B000FFImage:
    """Parse a Windows CE B000FF container: "B000FF\\n" magic, ImageStart/
    ImageLength u32 LE, then Address/Size/Checksum records; the last record
    (Address 0) carries the execution start address."""
    if len(data) < 15 or data[:7] != B000FF_MAGIC:
        raise ImageFormatError("not a B000FF container: %r" % (data[:8],))
    image_start, image_length = struct.unpack_from("<II", data, 7)
    records: List[B000FFRecord] = []
    off = 15
    while off + 12 <= len(data):
        address, size, checksum = struct.unpack_from("<III", data, off)
        if address == 0:
            return B000FFImage(
                image_start=image_start,
                image_length=image_length,
                records=records,
                exec_start=size,
                container_end=off,
            )
        payload = data[off + 12 : off + 12 + size]
        if len(payload) != size:
            raise ImageFormatError(
                "truncated B000FF record at 0x%x (want %d bytes)"
                % (off, size)
            )
        records.append(
            B000FFRecord(
                address=address,
                size=size,
                checksum=checksum,
                file_offset=off + 12,
            )
        )
        off += 12 + size
    raise ImageFormatError("B000FF container has no execution-start record")


def flatten_b000ff(data: bytes) -> bytes:
    """Return the load image (ImageLength bytes) for a B000FF container.

    Gaps between records are zero-filled, matching a raw NK.NB0 image.
    """
    image = parse_b000ff(data)
    out = bytearray(image.image_length)
    for rec in image.records:
        if rec.address < image.image_start:
            raise ImageFormatError(
                "record 0x%08x is below ImageStart 0x%08x"
                % (rec.address, image.image_start)
            )
        off = rec.address - image.image_start
        end = off + rec.size
        if end > len(out):
            raise ImageFormatError(
                "record 0x%08x (0x%x bytes) overruns image"
                % (rec.address, rec.size)
            )
        out[off:end] = data[rec.file_offset : rec.file_offset + rec.size]
    return bytes(out)


@dataclass
class SceCopy:
    file: str
    flag: str
    size1: int
    size2: int
    checksum: int


@dataclass
class SceTarget:
    name: str
    source: str
    total_size: int
    count: int
    files: List[SceCopy] = field(default_factory=list)


@dataclass
class SceScript:
    labels: List[str] = field(default_factory=list)
    targets: List[SceTarget] = field(default_factory=list)


def parse_sce(data: bytes) -> SceScript:
    """Parse the plaintext P2006.SCE install script (LBL/CHG/TGT/CPY/END
    lines; TGT names a target, CPY lists its disc volumes with sizes and
    checksum)."""
    script = SceScript()
    current: Optional[SceTarget] = None
    text = data.decode("latin-1")
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        tag = parts[0]
        if tag == "LBL":
            script.labels.append(parts[1] if len(parts) > 1 else "")
            current = None
        elif tag == "TGT":
            if len(parts) < 5:
                raise ImageFormatError("short TGT line: %r" % line)
            current = SceTarget(
                name=parts[1],
                source=parts[2],
                total_size=int(parts[3]),
                count=int(parts[4]),
            )
            script.targets.append(current)
        elif tag == "CPY":
            if current is None:
                raise ImageFormatError("CPY before TGT: %r" % line)
            if len(parts) < 6:
                raise ImageFormatError("short CPY line: %r" % line)
            current.files.append(
                SceCopy(
                    file=parts[1],
                    flag=parts[2],
                    size1=int(parts[3]),
                    size2=int(parts[4]),
                    checksum=int(parts[5]),
                )
            )
        elif tag in ("CHG", "END"):
            continue
    return script


def load_sce(path: Path) -> SceScript:
    return parse_sce(Path(path).read_bytes())
