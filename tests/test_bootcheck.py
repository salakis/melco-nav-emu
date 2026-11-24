# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""bootcheck compare(): two hand-written result directories, no QEMU."""
import json

from melco_nav_emu import bootcheck


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
