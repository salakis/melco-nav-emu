# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""bootcheck compare(): two hand-written result directories, no QEMU.

The `run` tests stop at the label check or at _fresh_unit(): a real run
needs a QEMU build and a disc, and must never touch build/.
"""
import json

import pytest

from melco_nav_emu import bootcheck


def _sandbox(tmp_path, monkeypatch):
    """Point the output root and the unit template at tmp_path, so that
    nothing can read or replace the real build/bootcheck or build/unit."""
    base = tmp_path / "build" / "bootcheck"
    monkeypatch.setattr(bootcheck, "OUT_BASE", base)
    monkeypatch.setattr(bootcheck, "TEMPLATE", base / "template")
    return base


def _write_run(base, label, *, board="cup28", milestones=None, shots=None,
               counts=None):
    out = base / label
    out.mkdir(parents=True)
    (out / "mmio-counts.json").write_text(json.dumps(counts or {}))
    result = {
        "label": label,
        "board": board,
        "commit": "0" * 40,
        "argv": [],
        "screenshots": [{"time": t, "file": "shot-%s.ppm" % t, "sha256": sha}
                        for t, sha in (shots or {}).items()],
        "mmio_counts": "mmio-counts.json",
        "milestones": milestones or [],
    }
    (out / "result.json").write_text(json.dumps(result))
    return out


def test_equal_runs_compare_clean(tmp_path):
    base = tmp_path / "bootcheck"
    _write_run(base, "a", shots={"90": "x", "120": "y"},
               counts={"flash": 1000, "uart": 10})
    _write_run(base, "b", shots={"90": "x", "120": "y"},
               counts={"flash": 1100, "uart": 12})
    assert bootcheck.compare("a", "b", base=base) == []


def test_differences_are_reported(tmp_path):
    base = tmp_path / "bootcheck"
    _write_run(base, "a", board="cup28", milestones=["MOST LOCK"],
               shots={"90": "x", "120": "y"}, counts={"flash": 1000, "dvd": 60})
    _write_run(base, "b", board="cup2006", milestones=["MOST LOCK", "LOC_Thread::Run"],
               shots={"90": "x", "120": "z"}, counts={"flash": 2000, "dvd": 60})
    diffs = bootcheck.compare("a", "b", base=base)
    assert any(d.startswith("board:") for d in diffs)
    assert any(d.startswith("milestones:") for d in diffs)
    assert any(d.startswith("screenshot at t=120") for d in diffs)
    assert any(d.startswith("mmio flash:") for d in diffs)
    # t=90 matches, and dvd (60 vs 60) is within tolerance.
    assert not any("t=90" in d for d in diffs)
    assert not any("mmio dvd" in d for d in diffs)


def test_small_counts_are_always_equal(tmp_path):
    base = tmp_path / "bootcheck"
    _write_run(base, "a", counts={"rtc": 3})
    _write_run(base, "b", counts={"rtc": 49})
    assert bootcheck.compare("a", "b", base=base) == []


def test_missing_result_is_a_difference(tmp_path):
    base = tmp_path / "bootcheck"
    _write_run(base, "a")
    assert bootcheck.compare("a", "b", base=base)


def test_hdd_problem_only_applies_to_cup2006(tmp_path):
    empty = tmp_path / "empty.img"
    empty.write_bytes(b"\x00" * 1024)
    assert bootcheck._hdd_problem("cup28", {"hdd": empty}) is None
    assert bootcheck._hdd_problem("cup2006", {}) is None
    assert bootcheck._hdd_problem("cup2006", {"hdd": empty}) == "no MBR"


def test_cup2006_defaults_photograph_the_map():
    d = bootcheck.DEFAULTS["cup2006"]
    ret = max(float(k.split(":")[0]) for k in d["keys"] if k.endswith(":ret"))
    assert any(float(t) > ret for t in d["shots"])
    assert d["seconds"] >= max(float(t) for t in d["shots"])


def test_schedule_orders_by_time_shots_first():
    events = bootcheck._schedule(["240", "120"], ["250:ret", "240:right"])
    assert events == [(120.0, "shot", "120"), (240.0, "shot", "240"),
                      (240.0, "key", "right"), (250.0, "key", "ret")]


class _FakeQMP:
    """Answers the QMP calls _run_events makes; the 'guest' reaches each
    x-stop-at-ns target as soon as it is armed."""

    def __init__(self):
        self.vclock = 0
        self.stop_at = None
        self.paused = False
        self.calls = []

    def execute(self, cmd, args=None):
        self.calls.append((cmd, args))
        if cmd == "qom-get":
            return self.vclock
        if cmd == "qom-set":
            self.stop_at = args["value"]
        elif cmd == "cont":
            self.paused = False
        elif cmd == "query-status":
            if not self.paused and self.stop_at is not None:
                self.vclock, self.paused = self.stop_at, True
            return {"status": "paused" if self.paused else "running"}
        return {}

    def screendump(self, name):
        self.calls.append(("screendump", name))


class _LiveProc:
    def poll(self):
        return None


def test_run_events_pauses_at_virtual_times():
    qmp = _FakeQMP()
    log = bootcheck._run_events(
        qmp, _LiveProc(), bootcheck._schedule(["1.5"], ["2:ret"]), 3)
    assert [(e["kind"], e["vclock_ns"]) for e in log] == [
        ("shot", 1_500_000_000), ("key", 2_000_000_000), ("end", 3_000_000_000)]
    sets = [a["value"] for c, a in qmp.calls if c == "qom-set"]
    assert sets == [1_500_000_000, 2_000_000_000, 3_000_000_000]
    # Each event after the first resumes the paused VM.
    assert sum(1 for c, _ in qmp.calls if c == "cont") == 2
    assert ("screendump", "shot-1.5.ppm") in qmp.calls
    key = [a for c, a in qmp.calls if c == "send-key"][0]
    assert key["keys"] == [{"type": "qcode", "data": "ret"}]


def test_label_must_be_one_plain_directory_name():
    for label in (".", "..", "...", "a/b", "../x", "/etc", "a b", ""):
        with pytest.raises(SystemExit):
            bootcheck._check_label(label)
    # The documented labels still pass, including one that merely has dots.
    for label in ("a..b", "run-1", "f6-final-eu", "x.y_z", "9"):
        assert bootcheck._check_label(label) == label


def test_missing_label_is_a_usage_error():
    with pytest.raises(SystemExit) as excinfo:
        bootcheck.main(["run", "--board", "cup2006"])
    assert excinfo.value.code == 2


@pytest.mark.parametrize("label", ["..", ".", "..."])
def test_dot_label_never_reaches_a_unit_outside_the_root(tmp_path, monkeypatch,
                                                         label):
    base = _sandbox(tmp_path, monkeypatch)
    # ".." makes the output directory build/, so this is the real build/unit.
    outside = base.parent / "unit"
    outside.mkdir(parents=True)
    (outside / "hdd.img").write_bytes(b"unit-owned")
    monkeypatch.setattr(bootcheck, "_qemu_running", lambda: False)

    def stop(*args):
        raise SystemExit("bootcheck: test stopped before the board")

    # A run from here on needs a QEMU build and a disc, so it must not get
    # there: a hostile label is refused by the argument check instead.
    monkeypatch.setattr(bootcheck, "_board_argv", stop)
    with pytest.raises(SystemExit) as excinfo:
        bootcheck.main(["run", "--board", "cup2006", "--label", label])
    # Nothing outside the output root was touched, and the refusal came from
    # the argument check rather than from a later failure.
    assert (outside / "hdd.img").read_bytes() == b"unit-owned"
    assert not (base / "unit").exists()
    assert "--label" in str(excinfo.value)


def test_fresh_unit_refuses_to_delete_outside_the_output_root(tmp_path,
                                                              monkeypatch):
    base = _sandbox(tmp_path, monkeypatch)
    base.mkdir(parents=True)
    outside = base.parent / "unit"
    outside.mkdir()
    (outside / "hdd.img").write_bytes(b"unit-owned")

    # The belt-and-braces guard, for a label or a symlink that slipped past
    # the argument check.
    with pytest.raises(SystemExit) as excinfo:
        bootcheck._fresh_unit(base / "..", None, "cup2006")
    assert "output root" in str(excinfo.value)
    assert (outside / "hdd.img").read_bytes() == b"unit-owned"


def test_a_label_containing_dots_replaces_only_its_own_unit(tmp_path,
                                                            monkeypatch):
    base = _sandbox(tmp_path, monkeypatch)
    template = base / "template" / "cup2006" / "unit"
    template.mkdir(parents=True)
    (template / "sram.bin").write_bytes(b"blank")
    sibling = base / "other" / "unit"
    sibling.mkdir(parents=True)
    (sibling / "flash.img").write_bytes(b"keep")
    outside = base.parent / "unit"
    outside.mkdir()
    (outside / "hdd.img").write_bytes(b"keep")

    outdir = base / "a..b"
    unit = bootcheck._fresh_unit(outdir, None, "cup2006")
    assert unit == outdir / "unit"
    assert [p.name for p in unit.iterdir()] == ["sram.bin"]   # from TEMPLATE
    assert (sibling / "flash.img").read_bytes() == b"keep"
    assert (outside / "hdd.img").read_bytes() == b"keep"


def test_a_stale_unit_inside_the_root_is_replaced(tmp_path, monkeypatch):
    base = _sandbox(tmp_path, monkeypatch)
    template = base / "template" / "cup28" / "unit"
    template.mkdir(parents=True)
    (template / "flash.bin").write_bytes(b"blank")
    unit = base / "run" / "unit"
    unit.mkdir(parents=True)
    (unit / "flash.bin").write_bytes(b"used")

    assert bootcheck._fresh_unit(base / "run", None, "cup28") == unit
    assert (unit / "flash.bin").read_bytes() == b"blank"
    # No template for cup2006: the unit is just created empty.
    other = bootcheck._fresh_unit(base / "run2", None, "cup2006")
    assert other.is_dir() and not any(other.iterdir())


def test_explicit_unit_is_used_as_given_and_never_deleted(tmp_path,
                                                          monkeypatch):
    base = _sandbox(tmp_path, monkeypatch)
    monkeypatch.setattr(bootcheck, "ROOT", tmp_path)
    kept = tmp_path / "my-unit"
    kept.mkdir()
    (kept / "hdd.img").write_bytes(b"unit-owned")

    # --unit is the caller's own directory, even outside the output root: it
    # is reused, never replaced, and a relative one is relative to ROOT.
    assert bootcheck._fresh_unit(base / "run", str(kept), "cup2006") == kept
    assert bootcheck._fresh_unit(base / "run", "my-unit", "cup2006") == kept
    assert (kept / "hdd.img").read_bytes() == b"unit-owned"
