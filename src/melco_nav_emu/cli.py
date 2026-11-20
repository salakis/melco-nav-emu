# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Command-line entry point for the Volvo RTI emulator."""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

from . import __version__
from .images import flatten_b000ff, load_sce, parse_b000ff
from .loading import parse_loading
from .qmp import QMPClient

PROJECT_ROOT = Path(__file__).resolve().parents[2]
IMAGES = PROJECT_ROOT / "images"
DEFAULT_QEMU = PROJECT_ROOT / "qemu" / "src" / "build" / "qemu-system-sh4"

EXPECTED = [
    "dvd1/LOADING.KWI",
    "dvd1/SUBCPU.ME",
    "dvd1/MMM02400.SUB",
    "dvd1/P2006.SCE",
    "dvd1/DKLABEL.DAT",
    "dvd1/ALLDATA.KWI",
    "dvd2/LOADING.KWI",
    # `make flatten` names modules <name>_v<version>.nb0; the version
    # depends on the disc set.
    "firmware/CUP2006.MLD.*.nb0",
    "firmware/CUP2006.SHx.*.nb0",
]


def cmd_info(args: argparse.Namespace) -> int:
    print("melco-nav-emu %s" % __version__)
    print("project: %s" % PROJECT_ROOT)
    print("images:  %s" % IMAGES)
    print("qemu:    %s" % (args.qemu or DEFAULT_QEMU))
    print("")
    for rel in EXPECTED:
        found = sorted(IMAGES.glob(rel))
        if not found:
            status = "missing"
        elif "*" in rel:
            status = ", ".join(p.name for p in found)
        else:
            status = "ok"
        print("  %-55s %s" % (rel, status))
    return 0


def cmd_flatten(args: argparse.Namespace) -> int:
    data = Path(args.module).read_bytes()
    image = parse_b000ff(data)
    out = flatten_b000ff(data)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(out)
    print(
        "%s -> %s: %d B (ImageStart %#x, ImageLength %#x, exec %#x, %d records)"
        % (
            args.module,
            args.out,
            len(out),
            image.image_start,
            image.image_length,
            image.exec_start,
            len(image.records),
        )
    )
    return 0


def cmd_sce(args: argparse.Namespace) -> int:
    script = load_sce(Path(args.sce))
    print("labels: %s" % (", ".join(script.labels) or "-"))
    total = 0
    for target in script.targets:
        total += target.total_size
        print(
            "%-16s %12d B  %4d files  source=%s"
            % (target.name, target.total_size, len(target.files), target.source)
        )
    print("%d targets, %d bytes total" % (len(script.targets), total))
    return 0


def cmd_loading(args: argparse.Namespace) -> int:
    image = parse_loading(Path(args.loading).read_bytes())
    print("systems=%d management_end=%d" % (len(image.systems), image.management_end))
    for mod in image.modules:
        print(
            "  %-16s v%-8s kind=%d DSA=%#010x @%d (%d B)"
            % (mod.name, mod.version, mod.kind, mod.dsa, mod.offset, mod.size)
        )
    return 0


def cmd_qmp(args: argparse.Namespace) -> int:
    arguments = json.loads(args.arguments) if args.arguments else None
    with QMPClient(args.address) as client:
        result = client.execute(args.command, arguments)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def cmd_screenshot(args: argparse.Namespace) -> int:
    with QMPClient(args.address) as client:
        client.screendump(str(args.output))
    print(str(args.output))
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    qemu = Path(args.qemu or DEFAULT_QEMU)
    if not qemu.exists():
        print("QEMU not found: %s\nRun 'make qemu' first." % qemu,
              file=sys.stderr)
        return 1

    # The product boot (discs, HDD, unit state) is `make boot`
    # (tools/profile.py); this launches a raw machine for experiments.
    # Disks and discs attach through machine properties (hddimage=,
    # dvdimage=), not -drive/-cdrom: the sh4 machine has no IDE bus.
    cmd: List[str] = [
        str(qemu),
        "-M",
        args.machine,
        "-m",
        args.memory,
        "-display",
        args.display,
        "-monitor",
        "none",
    ]
    if args.serial is not None:
        cmd += ["-serial", args.serial]
    if args.serial2 is not None:
        cmd += ["-serial", args.serial2]
    if args.qmp:
        qmp = Path(args.qmp)
        if qmp.parent and not qmp.parent.exists():
            qmp.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["-qmp", "unix:%s,server,nowait" % qmp]
    if args.kernel:
        cmd += ["-kernel", str(args.kernel)]
    if args.icount:
        cmd += ["-icount", args.icount]

    extra = shlex.split(args.extra) if args.extra else []
    if args.trace:
        trace = Path(args.trace)
        trace.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["-D", str(trace)]
        if "-d" not in extra:
            cmd += ["-d", args.trace_events]
    cmd += extra

    if args.dry_run:
        print(" ".join(shlex.quote(part) for part in cmd))
        return 0
    print("+ " + " ".join(shlex.quote(part) for part in cmd), file=sys.stderr)

    if args.timeout and args.timeout > 0:
        try:
            return subprocess.run(cmd, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            print(
                "melco-nav-emu: run stopped after %.1f s (--timeout)"
                % args.timeout,
                file=sys.stderr,
            )
            return 124

    os.execvp(cmd[0], cmd)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="melco-nav-emu",
        description="Volvo RTI MMM+ (CU-P2006) emulator",
    )
    parser.add_argument("--version", action="version", version=__version__)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("info", help="show project/image paths and expected files")
    p.add_argument("--qemu", help="path to qemu-system-sh4")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("flatten", help="flatten a B000FF module to NK.NB0")
    p.add_argument("module")
    p.add_argument("out")
    p.set_defaults(func=cmd_flatten)

    p = sub.add_parser("sce", help="summarise P2006.SCE")
    p.add_argument("sce")
    p.set_defaults(func=cmd_sce)

    p = sub.add_parser("loading", help="list LOADING.KWI modules")
    p.add_argument("loading")
    p.set_defaults(func=cmd_loading)

    p = sub.add_parser("qmp", help="send a raw QMP command")
    p.add_argument("address", help="unix:/path or host:port")
    p.add_argument("command")
    p.add_argument("arguments", nargs="?", help="JSON object")
    p.set_defaults(func=cmd_qmp)

    p = sub.add_parser("screenshot", help="capture a QMP screendump")
    p.add_argument("address")
    p.add_argument("output")
    p.set_defaults(func=cmd_screenshot)

    p = sub.add_parser("run", help="launch the emulator (raw; see `make boot` "
                                   "for the product boot)")
    p.add_argument("--qemu", help="path to qemu-system-sh4")
    p.add_argument("--machine", default="cup2006",
                   help="machine properties, e.g. 'cup2006,hddimage=...,dvdimage=...'")
    p.add_argument("--memory", default="256M")
    p.add_argument("--kernel", help="flattened NK image")
    p.add_argument("--display", default="cocoa")
    p.add_argument("--serial", default="stdio",
                   help="first serial port (SH7750 SCI), default stdio")
    p.add_argument("--serial2", default=None,
                   help="second serial port (SH7750 SCIF), e.g. stdio or null")
    p.add_argument("--qmp", help="QMP UNIX socket path")
    p.add_argument("--icount", help="QEMU icount argument for deterministic runs")
    p.add_argument("--trace", metavar="PATH",
                   help="write the QEMU -d log to PATH (enables guest_errors)")
    p.add_argument("--trace-events",
                   default="in_asm,exec,guest_errors",
                   help="events for --trace (default: in_asm,exec,guest_errors)")
    p.add_argument("--timeout", type=float, default=0.0, metavar="SECONDS",
                   help="kill the VM after SECONDS (0 = no limit)")
    p.add_argument("--extra", help="extra QEMU arguments (shell-quoted)")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=cmd_run)

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
