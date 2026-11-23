# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""JIS D 0810 section-18 LOADING.KWI module table parser."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List

from .images import ImageFormatError


def dsa_pos(dsa: int) -> int:
    """DSA -> byte position within the volume (sector 2 KiB + 32-B block)."""
    return (dsa >> 8) * 2048 + (dsa & 0x3F) * 32


def _u16(d: bytes, off: int) -> int:
    return struct.unpack_from(">H", d, off)[0]


def _u32(d: bytes, off: int) -> int:
    return struct.unpack_from(">I", d, off)[0]


def _cstr(d: bytes, off: int, size: int) -> str:
    return d[off : off + size].split(b"\0", 1)[0].decode("latin-1")


@dataclass
class LoadingModule:
    system: int
    kind: int
    name: str
    version: str
    effective: int
    expires: int
    title: str
    vendor: bytes
    dsa: int
    bs: int
    offset: int
    size: int


@dataclass
class LoadingImage:
    data: bytes
    systems: List[bytes] = field(default_factory=list)
    modules: List[LoadingModule] = field(default_factory=list)
    management_end: int = 0

    def body(self, module: LoadingModule) -> bytes:
        return self.data[module.offset : module.offset + module.size]


def parse_loading(data: bytes) -> LoadingImage:
    if len(data) < 4:
        raise ImageFormatError("LOADING.KWI is too short")
    n_sys = _u16(data, 0)
    off = 4
    image = LoadingImage(data=data)
    for sys_index in range(n_sys):
        if off + 16 > len(data):
            raise ImageFormatError("truncated system record")
        mid = data[off : off + 12]
        n_mod = _u16(data, off + 12)
        off += 16
        image.systems.append(mid)

        ids = []
        for _ in range(n_mod):
            if off + 64 > len(data):
                raise ImageFormatError("truncated module id record")
            ids.append(
                {
                    "kind": data[off],
                    "name": _cstr(data, off + 4, 52),
                    "version": _cstr(data, off + 56, 8),
                }
            )
            off += 64

        modules = []
        for ident in ids:
            if off + 256 > len(data):
                raise ImageFormatError("truncated module management record")
            modules.append(
                LoadingModule(
                    system=sys_index,
                    kind=ident["kind"],
                    name=ident["name"],
                    version=ident["version"],
                    effective=_u16(data, off),
                    expires=_u16(data, off + 2),
                    title=_cstr(data, off + 4, 64),
                    vendor=bytes(data[off + 68 : off + 250]),
                    dsa=_u32(data, off + 250),
                    bs=_u16(data, off + 254),
                    offset=dsa_pos(_u32(data, off + 250)),
                    size=0,
                )
            )
            off += 256

        starts = sorted(m.offset for m in modules)
        for m in modules:
            nxt = min((s for s in starts if s > m.offset), default=len(data))
            m.size = max(0, nxt - m.offset)
            image.modules.append(m)
    image.management_end = off
    return image
