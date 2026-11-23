# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Headless regression boots: run a board, record what the guest did, compare.

    python -m melco_nav_emu.bootcheck run --board cup2006|cup28 --label NAME \
        [--seconds N] [--shots T1,T2,...] [--keys T:KEY,...] \
        [--scenario PATH] [--disc ISO] [--icount SPEC] [--unit DIR]
    python -m melco_nav_emu.bootcheck compare LABEL_A LABEL_B

`run` boots one board headless from a fresh unit directory, takes QMP
screendumps and sends keys at fixed times, counts MMIO accesses per region
via -trace, and writes build/bootcheck/<label>/result.json.  `compare` checks
that two runs of the same board reached the same screens and did a comparable
amount of MMIO traffic (see compare()).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .qmp import QMPClient, QMPError

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:      # tools.* lives at the repo root (phase 0)
    sys.path.insert(0, str(ROOT))

QEMU = ROOT / "qemu/src/build/qemu-system-sh4"
OUT_BASE = ROOT / "build" / "bootcheck"
# A pristine per-board unit to copy, if present (template/<board>/unit).
TEMPLATE = OUT_BASE / "template"

# Per-board defaults (chosen in spike S2, phase 0.2).  cup2006's headless
# serial console prints almost nothing, so its boot progress is judged from
# screendumps; cup28 logs console milestones (below).
DEFAULTS = {
    "cup2006": {
        "seconds": 260,
        "shots": ["120", "180", "240", "255"],
        "keys": ["240:right", "250:ret"],
        "disc": None,
        "scenario": None,
        # With the launcher's shift=auto,sleep=on the serial-poll MMIO count
        # varies by more than the compare tolerance between identical boots;
        # a fixed shift makes the run deterministic (spike S2).
        "icount": "shift=3,sleep=off",
    },
    "cup28": {
        "seconds": 150,
        "shots": ["90", "120", "140"],
        "keys": [],
        "disc": None,      # falls back to $CUP28_DISC
        "scenario": None,
        "icount": None,    # the launcher's shift=auto,sleep=on is stable here
    },
}

# Console milestones (regexes searched in console.log).
MILESTONES = {
    "cup28": ["MOST LOCK", r"MOST_VC_GPSRECEIVER Set", r"LOC_Thread::Run"],
    "cup2006": [],
}

TRACE_EVENTS = ["memory_region_ops_read", "memory_region_ops_write"]
MMIO_NAME_RE = re.compile(r"name '([^']*)'")


def _qemu_running() -> bool:
    return subprocess.run(["pgrep", "-f", "qemu-system"],
                          capture_output=True).returncode == 0


def _git_commit() -> str:
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                          capture_output=True, text=True,
                          check=True).stdout.strip()


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _board_argv(board: str, unit: Path, outdir: Path, disc: Optional[str],
                scenario: Optional[str], icount: Optional[str]) -> List[str]:
    """The QEMU command line from the board's launcher, plus the bootcheck's
    QMP socket (relative to the output directory) and MMIO trace events."""
    qmp_sock = "unix:qmp.sock,server=on,wait=off"
    trace = []
    for event in TRACE_EVENTS:
        trace += ["-trace", event]
    if board == "cup2006":
        import tools.profile as profile
        console = outdir / "console.log"
        extra = ["-qmp", qmp_sock] + trace
        if icount:
            extra += ["-icount", icount]
        if scenario:
            # The launcher takes a scenario by name (scenarios/<name>/ or
            # build/gps/<name>/), so name it; the launcher adds gnssreplay=
            # and gpstrack= itself.
            scen = Path(scenario).resolve()
            if scen.parent.parent not in (ROOT / "scenarios",
                                          ROOT / "build" / "gps"):
                raise SystemExit(
                    "bootcheck: --scenario for cup2006 must be a scenario's "
                    "sensors.csv under scenarios/ or build/gps/")
            import os
            os.environ["CUP2006_SCENARIO"] = scen.parent.name
        return profile.qemu_argv(display="none", serial=(str(console),),
                                 extra=extra, unit=unit)
    if board == "cup28":
        from . import cup28
        disc_input = disc or __import__("os").environ.get("CUP28_DISC")
        if not disc_input:
            raise SystemExit("bootcheck: cup28 needs --disc or $CUP28_DISC")
        disc_path = Path(disc_input).resolve(strict=True)
        firmware = cup28.prepare_firmware(disc_path, unit)
        track = Path(scenario).resolve(strict=True) if scenario else None
        argv = cup28.qemu_argv(QEMU, disc=disc_path, unit=unit,
                               firmware=firmware, display="none", audio=None,
                               track=track, qmp="qmp.sock")
        if icount:
            argv[argv.index("-icount") + 1] = icount
        return argv + trace
    raise SystemExit("bootcheck: unknown board %r" % board)


def _fresh_unit(outdir: Path, unit_arg: Optional[str], board: str) -> Path:
    if unit_arg:
        unit = Path(unit_arg)
        if not unit.is_absolute():
            unit = ROOT / unit
        unit.mkdir(parents=True, exist_ok=True)
        return unit
    unit = outdir / "unit"
    if unit.exists():
        shutil.rmtree(unit)
    template = TEMPLATE / board / "unit"
    if template.is_dir():
        shutil.copytree(template, unit)
    else:
        unit.mkdir(parents=True)
    return unit


def _count_mmio(stream, counts: Dict[str, int], tail: List[str]) -> None:
    """Count -trace memory_region_ops_* accesses per region name."""
    for line in stream:
        line = line.rstrip("\n")
        tail.append(line)
        del tail[:-100]
        if line.startswith("memory_region_ops_"):
            m = MMIO_NAME_RE.search(line)
            if m:
                counts[m.group(1)] = counts.get(m.group(1), 0) + 1


def cmd_run(args: argparse.Namespace) -> int:
    if _qemu_running():
        raise SystemExit("bootcheck: a qemu-system process is already running "
                         "(pgrep -f qemu-system); refusing to start another")
    board = args.board
    defaults = DEFAULTS[board]
    seconds = args.seconds if args.seconds is not None else defaults["seconds"]
    shots = args.shots.split(",") if args.shots else list(defaults["shots"])
    keys = args.keys.split(",") if args.keys else list(defaults["keys"])
    disc = args.disc or defaults["disc"]
    scenario = args.scenario or defaults["scenario"]
    icount = args.icount if args.icount is not None else defaults["icount"]
    label = args.label
    if not re.fullmatch(r"[A-Za-z0-9._-]+", label):
        raise SystemExit("bootcheck: --label must be [A-Za-z0-9._-]+")

    outdir = OUT_BASE / label
    outdir.mkdir(parents=True, exist_ok=True)
    unit = _fresh_unit(outdir, args.unit, board)
    argv = _board_argv(board, unit, outdir, disc, scenario, icount)

    console = outdir / "console.log"
    if board == "cup28":
        # cup28's third serial port is stdio: its console is QEMU's stdout.
        stdout = console.open("wb")
    else:
        stdout = (outdir / "qemu-stdout.log").open("wb")
    counts: Dict[str, int] = {}
    tail: List[str] = []
    proc = subprocess.Popen(argv, cwd=outdir, stdin=subprocess.DEVNULL,
                            stdout=stdout, stderr=subprocess.PIPE, text=True)
    reader = threading.Thread(target=_count_mmio,
                              args=(proc.stderr, counts, tail), daemon=True)
    reader.start()
    start = time.monotonic()
    try:
        sock = outdir / "qmp.sock"
        qmp: Optional[QMPClient] = None
        deadline = start + 120
        while qmp is None:
            if proc.poll() is not None:
                raise SystemExit(
                    "bootcheck: QEMU exited with code %d before QMP came up; "
                    "stderr tail:\n%s" % (proc.returncode, "\n".join(tail[-20:])))
            try:
                qmp = QMPClient("unix:" + str(sock)).connect()
            except (OSError, QMPError):
                if time.monotonic() > deadline:
                    raise SystemExit("bootcheck: QMP socket did not appear "
                                     "within 120 s; stderr tail:\n%s"
                                     % "\n".join(tail[-20:]))
                time.sleep(0.2)
        events = ([(float(t), "shot", t) for t in shots] +
                  [(float(t), "key", k) for t, k in
                   (k.split(":", 1) for k in keys)])
        events.sort(key=lambda e: e[0])
        for when, kind, value in events:
            delay = when - (time.monotonic() - start)
            if delay > 0:
                time.sleep(delay)
            if proc.poll() is not None:
                raise SystemExit("bootcheck: QEMU exited with code %d during "
                                 "the run" % proc.returncode)
            if kind == "shot":
                qmp.screendump("shot-%s.ppm" % value)
            else:
                qmp.execute("send-key", {
                    "keys": [{"type": "qcode", "data": value}],
                    "hold-time": 100,
                })
        delay = seconds - (time.monotonic() - start)
        if delay > 0:
            time.sleep(delay)
        qmp.quit()
        qmp.close()
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        reader.join(5)
        stdout.close()
        (outdir / "qemu-stderr-tail.log").write_text("\n".join(tail[-100:]))

    (outdir / "mmio-counts.json").write_text(
        json.dumps(counts, indent=2, sort_keys=True) + "\n")
    screenshots = []
    for t in shots:
        shot = outdir / ("shot-%s.ppm" % t)
        screenshots.append({"time": t, "file": shot.name,
                            "sha256": _sha256(shot) if shot.is_file() else None})
    console_text = ""
    if console.is_file():
        console_text = console.read_text(errors="replace")
    milestones = [m for m in MILESTONES[board]
                  if re.search(m, console_text)]
    result = {
        "label": label,
        "board": board,
        "commit": _git_commit(),
        "argv": argv,
        "seconds": seconds,
        "screenshots": screenshots,
        "mmio_counts": "mmio-counts.json",
        "milestones": milestones,
        "console_log": "console.log",
    }
    (outdir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("bootcheck: %s (%s) -> %s: %d shots, %d regions, milestones %s"
          % (label, board, outdir, len(screenshots), len(counts),
             milestones or "-"))
    return 0


def compare(label_a: str, label_b: str, base: Optional[Path] = None) -> List[str]:
    """Differences between two runs; an empty list means equal."""
    base = Path(base) if base is not None else OUT_BASE
    diffs: List[str] = []
    results = {}
    for label in (label_a, label_b):
        path = base / label / "result.json"
        if not path.is_file():
            diffs.append("missing result.json for %s (%s)" % (label, path))
            return diffs
        results[label] = json.loads(path.read_text())
    a, b = results[label_a], results[label_b]
    if a["board"] != b["board"]:
        diffs.append("board: %s vs %s" % (a["board"], b["board"]))
    if set(a["milestones"]) != set(b["milestones"]):
        diffs.append("milestones: %s vs %s" % (a["milestones"], b["milestones"]))
    shots_a = {s["time"]: s["sha256"] for s in a["screenshots"]}
    shots_b = {s["time"]: s["sha256"] for s in b["screenshots"]}
    for t in sorted(set(shots_a) & set(shots_b)):
        if shots_a[t] != shots_b[t]:
            diffs.append("screenshot at t=%s differs (%s vs %s)"
                         % (t, shots_a[t], shots_b[t]))
    counts = {}
    for label in (label_a, label_b):
        path = base / label / a["mmio_counts"]
        counts[label] = json.loads(path.read_text()) if path.is_file() else {}
    for region in sorted(set(counts[label_a]) | set(counts[label_b])):
        ca = counts[label_a].get(region, 0)
        cb = counts[label_b].get(region, 0)
        if ca < 50 and cb < 50:
            continue
        if max(ca, cb) == 0 or abs(ca - cb) <= 0.3 * max(ca, cb):
            continue
        diffs.append("mmio %s: %d vs %d" % (region, ca, cb))
    return diffs


def cmd_compare(args: argparse.Namespace) -> int:
    diffs = compare(args.label_a, args.label_b)
    if diffs:
        for d in diffs:
            print("bootcheck: %s vs %s: %s" % (args.label_a, args.label_b, d))
        return 1
    print("bootcheck: %s == %s" % (args.label_a, args.label_b))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="bootcheck",
                                     description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("run", help="boot one board headless and record it")
    p.add_argument("--board", choices=sorted(DEFAULTS), required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--seconds", type=float, default=None)
    p.add_argument("--shots", default=None, help="comma-separated times (s)")
    p.add_argument("--keys", default=None, help="comma-separated T:KEY")
    p.add_argument("--scenario", default=None,
                   help="sensors.csv path (cup28: gpstrack; cup2006: scenario "
                        "name under scenarios/ or build/gps/)")
    p.add_argument("--disc", default=None, help="cup28 map DVD ISO")
    p.add_argument("--icount", default=None,
                   help="replace the launcher's -icount setting, e.g. "
                        "shift=3,sleep=off")
    p.add_argument("--unit", default=None,
                   help="reuse this unit directory instead of a fresh one")
    p.set_defaults(func=cmd_run)
    p = sub.add_parser("compare", help="compare two recorded runs")
    p.add_argument("label_a")
    p.add_argument("label_b")
    p.set_defaults(func=cmd_compare)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
