# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
from __future__ import annotations

import struct

from melco_nav_emu.loading import dsa_pos, parse_loading


def make_loading() -> bytes:
    dsa = 0x00000200  # -> byte offset 4096
    body = b"B000FF\n" + b"\0" * 24

    header = bytearray(4096)
    struct.pack_into(">H", header, 0, 1)          # one system
    header[4:16] = b"MID-01234567"                 # 12-byte MID
    struct.pack_into(">H", header, 16, 1)         # one module

    id_off = 20
    header[id_off] = 1                            # kind = program
    name = b"CUP2006.MLD.ORG"
    header[id_off + 4 : id_off + 4 + len(name)] = name
    version = b"00000253"
    header[id_off + 56 : id_off + 56 + len(version)] = version

    mgmt = id_off + 64
    struct.pack_into(">HH", header, mgmt, 7520, 7520)
    title = b"Main Loader"
    header[mgmt + 4 : mgmt + 4 + len(title)] = title
    struct.pack_into(">I", header, mgmt + 250, dsa)
    struct.pack_into(">H", header, mgmt + 254, 0)

    return bytes(header) + body


def test_dsa_pos() -> None:
    assert dsa_pos(0x00000200) == 4096
    assert dsa_pos(0x00000001) == 32


def test_parse_loading_module_and_extent() -> None:
    data = make_loading()
    image = parse_loading(data)
    assert len(image.systems) == 1
    assert len(image.modules) == 1
    module = image.modules[0]
    assert module.name == "CUP2006.MLD.ORG"
    assert module.version == "00000253"
    assert module.kind == 1
    assert module.dsa == 0x00000200
    assert module.offset == 4096
    assert module.size == len(data) - 4096
    assert image.body(module) == data[4096:]
