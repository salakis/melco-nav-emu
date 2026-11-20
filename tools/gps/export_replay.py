#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Turn a PocketSDR tracking log ($CH/$NAV) into the emulator's GNSSIQ1 replay.

This is the measurement-level half of the scenario pipeline: the log is the
receiver's own correlator/navigation output, not a position solution.  The
emulator replays these records through the GPS tracking-chip mailbox and
gpslib computes its own fix.

    pocket_trk ... -log track.log track.bin
    python3 tools/gps/export_replay.py track.log scenarios/<name>/drive.replay \\
        --gps-tow 292149

Format (radio replay v1): 16-byte header "GNSSIQ1\\0",
u32 frame count, u32 interval ms; each frame is 32 x 0x50 measurement records
then 32 x 0x30 navigation records, PRN-1 indexed, little endian.

Only $CH and $NAV are consumed.  $POS/NMEA are never read.  The receiver time
is normalised to the first tracking epoch; the 500 ms replay clock then runs
on the emulator's own GPS tick.  The receive-time tag (+0x34) uses the
scenario GPS epoch derived from the $CH TOW, so it stays in the same time
frame as gpslib's satellite TOW.
"""
import argparse
import math
import struct
import sys
from collections import defaultdict


def read_hex_bits(text, size):
    """PocketSDR hex dump: MSB-first bits, `size` bits."""
    text = text.strip()
    if len(text) * 4 < size:
        raise ValueError("short $NAV data: %d bits for %d" % (len(text) * 4, size))
    value = int(text[: (size + 3) // 4], 16)
    shift = ((size + 3) // 4) * 4 - size
    return [(value >> (size - 1 - i + shift)) & 1 for i in range(size)]


def word_parity(sf, w, prev_d29, prev_d30):
    """GPS LNAV parity: logical 24 bits in, polarity-encoded 30 bits out."""
    d = [(sf[w * 30 + i] ^ prev_d30) & 1 for i in range(24)]
    p = [
        prev_d29 ^ prev_d30 ^ d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[5] ^ d[9] ^
        d[10] ^ d[11] ^ d[12] ^ d[13] ^ d[16] ^ d[17] ^ d[19] ^ d[22],
        prev_d30 ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[6] ^ d[10] ^ d[11] ^
        d[12] ^ d[13] ^ d[14] ^ d[17] ^ d[18] ^ d[20] ^ d[23],
        prev_d29 ^ d[0] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[7] ^ d[11] ^
        d[12] ^ d[13] ^ d[14] ^ d[15] ^ d[18] ^ d[19] ^ d[21],
        prev_d30 ^ d[1] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[8] ^ d[12] ^
        d[13] ^ d[14] ^ d[15] ^ d[16] ^ d[19] ^ d[20] ^ d[22],
        prev_d30 ^ d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^
        d[13] ^ d[14] ^ d[15] ^ d[16] ^ d[17] ^ d[20] ^ d[21] ^ d[23],
        prev_d29 ^ d[2] ^ d[4] ^ d[5] ^ d[7] ^ d[8] ^ d[9] ^ d[10] ^
        d[12] ^ d[14] ^ d[15] ^ d[16] ^ d[17] ^ d[18] ^ d[21] ^ d[22] ^
        d[23],
    ]
    for i in range(24):
        sf[w * 30 + i] = d[i]
    for i in range(6):
        sf[w * 30 + 24 + i] = p[i] & 1
    return p[4] & 1, p[5] & 1


def nav_record(prn, bits, prev):
    """Build one 0x30 type-2 record from 240 logical LNAV data bits."""
    sf = [0] * 300
    for w in range(10):
        for i in range(24):
            sf[w * 30 + i] = bits[w * 24 + i]
    d29, d30 = prev
    for w in range(10):
        d29, d30 = word_parity(sf, w, d29, d30)
    raw = [d29, d30] + sf + [0, 0]
    rec = bytearray(0x30)
    struct.pack_into("<II", rec, 0, prn, 1)
    for i in range(9):
        word = 0
        for b in range(32):
            word = (word << 1) | raw[i * 32 + b]
        struct.pack_into("<I", rec, 8 + i * 4, word & 0xffffffff)
    word = 0
    for b in range(16):
        word = (word << 1) | raw[288 + b]
    # The guest unpacker reads +0x2c as u32 and shifts right 16, so the
    # final 16 bits live in the upper half.
    struct.pack_into("<I", rec, 0x2c, (word & 0xffff) << 16)
    return rec, (d29, d30)


def lost_record(prn):
    rec = bytearray(0x50)
    struct.pack_into("<I", rec, 0, prn)
    rec[0x0b] = 1
    return rec


def measurement_record(prn, lock, cn0, t_tx, dop, fsync):
    """One 0x50 type-1 record.  t_tx is the true satellite transmit time (s).

    gpslib's builder adds a fixed +5 ms and +0.5 chip; the record's code phase
    is therefore the true t_tx minus those offsets."""
    rec = bytearray(0x50)
    t_enc = t_tx - 0.005 - 0.5 / 1023.0 / 1000.0
    sf = math.floor(t_enc / 6.0)
    sec = t_enc - sf * 6.0
    d = math.floor(sec * 50.0)
    bit_frac = sec - d / 50.0
    ms = math.floor(bit_frac * 1000.0)
    if ms < 0:
        ms = 0
    if ms > 19:
        ms = 19
    chip_f = (bit_frac * 1000.0 - ms) * 1023.0
    if chip_f < 0.0:
        chip_f = 0.0
    chip_i = math.floor(chip_f)
    frac = chip_f - chip_i
    state = 2 if fsync else (1 if lock > 0.2 else 0)
    struct.pack_into("<I", rec, 0, prn)
    struct.pack_into("<I", rec, 4, state)
    if state == 0:
        rec[0x0b] = 1
    struct.pack_into("<I", rec, 0x0c, max(0, min(0xffffffff, int(round(
        max(0.0, cn0) * 41.0 / 39.0 * 128.0 / 5.0)))))
    nco = int(round((3900000.0 + dop) / 16777216.0 * (1 << 32)))
    struct.pack_into("<I", rec, 0x10, nco & 0xffffffff)
    struct.pack_into("<I", rec, 0x1c,
                     ((chip_i & 0xffff) << 16) | (int(frac * 65536.0) & 0xffff))
    struct.pack_into("<I", rec, 0x20, 19 - ms)
    struct.pack_into("<I", rec, 0x24, 0)
    struct.pack_into("<I", rec, 0x28, d & 0xffffffff)
    struct.pack_into("<I", rec, 0x2c, sf % 100800)
    struct.pack_into("<I", rec, 0x30, 1)
    return rec


def parse_log(path):
    ch = []            # (time, prn, lock, cn0, coff, dop, tow, tow_v, fsync)
    nav = []           # (time, prn, bits)
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("$CH,"):
            f = line.rstrip("\n").split(",")
            if len(f) < 26:
                continue
            try:
                t = float(f[1])
                prn = int(f[6])
                lock = float(f[7])
                cn0 = float(f[8])
                coff = float(f[9]) * 1e-3
                dop = float(f[10])
                fsync = int(f[14])
                tow_v = int(f[19])
                tow = int(f[20])
            except ValueError:
                continue
            if 1 <= prn <= 32:
                ch.append((t, prn, lock, cn0, coff, dop, tow, tow_v, fsync))
        elif line.startswith("$NAV,"):
            f = line.rstrip("\n").split(",")
            if len(f) < 8:
                continue
            try:
                t = float(f[1])
                prn = int(f[4])
                size = int(f[6])
            except ValueError:
                continue
            if 1 <= prn <= 32 and size == 240:
                try:
                    nav.append((t, prn, read_hex_bits(f[7], size)))
                except ValueError:
                    pass
    return ch, nav


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("out")
    ap.add_argument("--interval", type=int, default=500, help="replay frame ms")
    ap.add_argument("--start", type=float, default=0.0,
                    help="first replay time in the log clock (s)")
    ap.add_argument("--end", type=float, default=None,
                    help="last replay time (s); default: last $CH")
    ap.add_argument("--gps-tow", type=float, default=None,
                    help="receiver TOW at log t=0, seconds (e.g. 292149 for the "
                         "2009-09-09 09:09:09 scenario).  If omitted, the median "
                         "$CH TOW is used; a common clock offset is absorbed by "
                         "the solver, but the known epoch is more physical.")
    args = ap.parse_args()
    ch, nav = parse_log(args.log)
    if not ch:
        sys.exit("no $CH records in %s" % args.log)
    start = args.start
    end = args.end if args.end is not None else max(r[0] for r in ch)
    interval = args.interval / 1000.0
    frames = int(math.floor((end - start) / interval + 1e-9)) + 1
    if frames <= 0 or frames > 8192:
        sys.exit("bad frame count %d" % frames)

    epochs = defaultdict(list)
    for r in ch:
        epochs[round(r[0], 3)].append(r)
    times = sorted(epochs)
    # Scenario GPS epoch.  Prefer the epoch the I/Q was generated with; the
    # $CH TOW is the satellite transmit TOW (range included), so tow - t is
    # low by the pseudorange.  A common offset is a receiver clock bias, but
    # the known epoch keeps the tag physical.
    if args.gps_tow is not None:
        base_ms = args.gps_tow * 1000.0
    else:
        bases = [r[6] - r[0] * 1000.0 for r in ch if r[7] > 0 and r[6] >= 0]
        if not bases:
            sys.exit("no $CH line with a valid TOW")
        bases.sort()
        base_ms = bases[len(bases) // 2] + 70.0  # nominal pseudorange
        print("warning: no --gps-tow; rec34 base %.1f ms" % base_ms,
              file=sys.stderr)

    # Navigation records, one per PRN per frame.  Parity runs per PRN across
    # subframes; the first subframe starts with the conventional zero state.
    parity = {}
    nav_by_frame = defaultdict(dict)
    for t, prn, bits in sorted(nav):
        prev = parity.get(prn, (0, 0))
        rec, prev = nav_record(prn, bits, prev)
        parity[prn] = prev
        f = int(math.floor((t - start) / interval + 1e-9))
        if 0 <= f < frames:
            nav_by_frame[f][prn] = rec

    data = bytearray()
    cursor = 0
    for f in range(frames):
        ft = start + f * interval
        meas = bytearray(32 * 0x50)
        navrec = bytearray(32 * 0x30)
        for prn, rec in nav_by_frame.get(f, {}).items():
            navrec[(prn - 1) * 0x30:(prn - 1) * 0x30 + 0x30] = rec
        # Latest tracking epoch at or before this frame.
        while cursor + 1 < len(times) and times[cursor + 1] <= ft + 1e-9:
            cursor += 1
        epoch_time = times[cursor] if times[cursor] <= ft + 1e-9 else None
        by_prn = {}
        if epoch_time is not None:
            for r in epochs[epoch_time]:
                old = by_prn.get(r[1])
                if old is None or (r[8], r[2]) > (old[8], old[2]):
                    by_prn[r[1]] = r
        for prn in range(1, 33):
            r = by_prn.get(prn)
            if r is None:
                meas[(prn - 1) * 0x50:(prn - 1) * 0x50 + 0x50] = lost_record(prn)
                continue
            t, _, lock, cn0, coff, dop, tow, tow_v, fsync = r
            if tow_v <= 0 or tow < 0:
                meas[(prn - 1) * 0x50:(prn - 1) * 0x50 + 0x50] = lost_record(prn)
                continue
            # Satellite transmit time at the receiver epoch (PocketSDR
            # sdr_pvt.c: tau = t_rx - TOW + coff, P = c (tau + T/2 fd/fc)):
            #   TOW - code offset - half-integration Doppler correction,
            # then advanced to the frame time. Over receiver time dt the
            # transmit time advances by dt (1 + fd/fc): the log has one epoch
            # per second and the replay two frames, so without the Doppler
            # term every other frame's range was off by 0.5 s * fd * 0.19 m
            # (hundreds of metres; the guest's fix sat ~400 m off the track).
            t_tx = (tow * 1e-3 - coff - 0.5 * 0.001 * dop / 1575.42e6 +
                    (ft - t) * (1.0 + dop / 1575.42e6))
            rec = measurement_record(prn, lock, cn0, t_tx, dop, fsync)
            struct.pack_into("<I", rec, 0x34,
                             int((base_ms + ft * 1000.0) / 20.0))
            meas[(prn - 1) * 0x50:(prn - 1) * 0x50 + 0x50] = rec
        data += meas + navrec

    out = bytearray(16)
    out[0:8] = b"GNSSIQ1\0"
    struct.pack_into("<II", out, 8, frames, args.interval)
    out += data
    with open(args.out, "wb") as f:
        f.write(out)
    print("%d frames @ %d ms (%.2f s), %d PRNs, %d nav events, %d bytes -> %s"
          % (frames, args.interval, frames * interval, len({r[1] for r in ch}),
             len(nav), len(out), args.out))


if __name__ == "__main__":
    main()
