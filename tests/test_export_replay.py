# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Unit checks for the PocketSDR log -> GNSSIQ1 exporter."""
import struct

from tools.gps.export_replay import measurement_record, nav_record


def _unpack_nav(rec):
    raw = []
    for i in range(9):
        w = struct.unpack_from("<I", rec, 8 + i * 4)[0]
        raw += [(w >> (31 - b)) & 1 for b in range(32)]
    w = struct.unpack_from("<I", rec, 0x2c)[0] >> 16
    raw += [(w >> (15 - b)) & 1 for b in range(16)]
    return raw


def test_nav_record_round_trip_and_upper_halfword():
    bits = [(i * 7 + 3) % 2 for i in range(240)]
    rec, prev = nav_record(7, bits, (0, 0))
    assert struct.unpack_from("<I", rec, 0)[0] == 7
    assert struct.unpack_from("<I", rec, 4)[0] != 0
    # The final 16 bits are in the upper half of the +0x2c word; the guest
    # unpacker shifts that word right by 16.
    assert struct.unpack_from("<H", rec, 0x2c)[0] == 0
    raw = _unpack_nav(rec)
    assert len(raw) == 304
    out = []
    prev_d30 = raw[1]
    for wi in range(10):
        word = raw[2 + wi * 30:2 + (wi + 1) * 30]
        for bi in range(24):
            out.append(word[bi] ^ (prev_d30 if wi else 0))
        prev_d30 = word[29]
    assert out == bits
    assert prev  # parity state advanced


def test_measurement_record_inverts_gpslib_offsets():
    # A true transmit time just after a subframe boundary.
    t_tx = 292149.0 + 1.234567
    rec = measurement_record(5, 1.0, 45.0, t_tx, 1000.0, 1)
    code = struct.unpack_from("<I", rec, 0x1c)[0]
    ms = struct.unpack_from("<I", rec, 0x20)[0]
    bc = struct.unpack_from("<I", rec, 0x28)[0]
    tow = struct.unpack_from("<I", rec, 0x2c)[0]
    sub = struct.unpack_from("<I", rec, 0x30)[0]
    hi = code >> 16
    if hi >= 0x8000:
        hi -= 0x10000
    chips = hi + ((code & 0xffff) / 65536.0 + 0.5)
    d = bc - (sub - 1)
    decoded = 6 * tow + d / 50.0 + ((19 - ms) + 5 + chips / 1023.0) / 1000.0
    assert abs(decoded - t_tx) < 1e-6
