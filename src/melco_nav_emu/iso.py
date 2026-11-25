# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Read files from the root of a plain ISO9660 disc, and its volume name."""
from __future__ import annotations

import struct
from pathlib import Path
from typing import BinaryIO

from .images import ImageFormatError

SECTOR = 2048


def _primary_volume_descriptor(stream: BinaryIO) -> bytes:
    """The primary volume descriptor, from sectors 16..79."""
    for sector in range(16, 80):
        stream.seek(sector * SECTOR)
        pvd = stream.read(SECTOR)
        if len(pvd) != SECTOR:
            raise ImageFormatError('truncated ISO data at %#x'
                                   % (sector * SECTOR))
        if pvd[1:7] != b'CD001\x01' or pvd[0] == 255:
            raise ImageFormatError('no ISO9660 primary volume descriptor')
        if pvd[0] == 1:
            return pvd
    raise ImageFormatError('no ISO9660 primary volume descriptor')


def volume_id(path: Path) -> str:
    """The disc's volume identifier, e.g. `NA_0900_06A_WESTERN`.

    The map discs name their market there, which is how the launcher picks
    the car the unit sits in (`boards/cup28.py`)."""
    with Path(path).open('rb') as stream:
        pvd = _primary_volume_descriptor(stream)
    return pvd[40:72].decode('latin-1').strip()


def read_root_file(path: Path, name: str) -> bytes:
    """Read one ordinary root-directory file; reject truncated disc data."""
    with Path(path).open('rb') as stream:
        def read(offset, size):
            stream.seek(offset)
            data = stream.read(size)
            if len(data) != size:
                raise ImageFormatError('truncated ISO data at %#x' % offset)
            return data

        pvd = _primary_volume_descriptor(stream)
        lba = struct.unpack_from('<I', pvd, 158)[0]
        size = struct.unpack_from('<I', pvd, 166)[0]
        if size > 16 * 1024 * 1024:
            raise ImageFormatError('ISO root directory is too large')
        data = read(lba * SECTOR, size)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if not length:
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + length]
            if (length < 34 or len(rec) != length or
                    pos % SECTOR + length > SECTOR or rec[32] > length - 33):
                raise ImageFormatError('invalid ISO directory record')
            entry = rec[33:33 + rec[32]].split(b';', 1)[0].decode('latin-1')
            if entry.upper() == name.upper() and not (rec[25] & 2):
                if rec[25] & 0x80 or rec[26] or rec[27]:
                    raise ImageFormatError('unsupported multi-extent or interleaved file')
                extent = struct.unpack_from('<I', rec, 2)[0]
                count = struct.unpack_from('<I', rec, 10)[0]
                if count > 64 * 1024 * 1024:
                    raise ImageFormatError('ISO file is too large')
                return read((extent + rec[1]) * SECTOR, count)
            pos += length
    raise ImageFormatError('%s is absent from %s' % (name, path))
