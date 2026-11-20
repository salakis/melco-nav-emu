# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
from __future__ import annotations

import struct

import pytest

from melco_nav_emu.images import (
    ImageFormatError,
    flatten_b000ff,
    parse_b000ff,
    parse_sce,
)


def make_b000ff() -> bytes:
    payload = b"\xde\xad\xbe\xef"
    header = b"B000FF\n" + struct.pack("<II", 0x80100000, 0x20)
    record = struct.pack("<III", 0x80100010, len(payload), sum(payload)) + payload
    exec_record = struct.pack("<III", 0, 0x80100004, 0)
    return header + record + exec_record


def test_parse_b000ff() -> None:
    image = parse_b000ff(make_b000ff())
    assert image.image_start == 0x80100000
    assert image.image_length == 0x20
    assert image.exec_start == 0x80100004
    assert len(image.records) == 1
    assert image.records[0].address == 0x80100010
    assert image.records[0].size == 4


def test_flatten_b000ff_zero_fills_gaps() -> None:
    out = flatten_b000ff(make_b000ff())
    assert len(out) == 0x20
    assert out[:0x10] == b"\0" * 0x10
    assert out[0x10:0x14] == b"\xde\xad\xbe\xef"
    assert out[0x14:] == b"\0" * 0x0C


def test_bad_magic() -> None:
    with pytest.raises(ImageFormatError):
        parse_b000ff(b"not a container")


def test_parse_sce() -> None:
    data = (
        b"LBL Test\r\n"
        b"CHG 1\r\n"
        b"TGT 0.1 ? 100 2\r\n"
        b"CPY A.KWI N 10 10 123\r\n"
        b"CPY B.KWI N 20 20 456\r\n"
        b"END\r\n"
    )
    script = parse_sce(data)
    assert script.labels == ["Test"]
    assert len(script.targets) == 1
    target = script.targets[0]
    assert target.name == "0.1"
    assert target.total_size == 100
    assert target.count == 2
    assert [c.file for c in target.files] == ["A.KWI", "B.KWI"]
    assert target.files[1].checksum == 456


def test_sce_copy_before_target() -> None:
    with pytest.raises(ImageFormatError):
        parse_sce(b"CPY A.KWI N 1 1 0\n")
