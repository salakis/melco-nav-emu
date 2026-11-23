# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
from __future__ import annotations

from pathlib import Path

import pytest

from melco_nav_emu import cli


def _fake_qemu(tmp_path: Path) -> Path:
    qemu = tmp_path / "qemu-system-sh4"
    qemu.write_text("")
    return qemu


def test_run_dry_run_defaults(tmp_path: Path, capsys) -> None:
    qemu = _fake_qemu(tmp_path)
    rc = cli.main(
        [
            "run",
            "--qemu",
            str(qemu),
            "--kernel",
            str(tmp_path / "nk.nb0"),
            "--dry-run",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "-m 256M" in out
    assert "-M cup2006" in out


def test_run_dry_run_trace_and_serial2(tmp_path: Path, capsys) -> None:
    qemu = _fake_qemu(tmp_path)
    trace = tmp_path / "traces" / "nk.log"
    rc = cli.main(
        [
            "run",
            "--qemu",
            str(qemu),
            "--kernel",
            str(tmp_path / "nk.nb0"),
            "--serial",
            "null",
            "--serial2",
            "file:scif.log",
            "--trace",
            str(trace),
            "--dry-run",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "-serial null -serial file:scif.log" in out
    assert "-D %s" % trace in out
    assert "-d in_asm,exec,guest_errors" in out


def test_run_dry_run_custom_machine_properties(tmp_path: Path, capsys) -> None:
    qemu = _fake_qemu(tmp_path)
    machine = "cup2006,hddimage=disk.img,dvdimage=disc1.iso"
    rc = cli.main(
        [
            "run",
            "--qemu",
            str(qemu),
            "--machine",
            machine,
            "--kernel",
            str(tmp_path / "nk.nb0"),
            "--dry-run",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "-M %s" % machine in out
    assert "nk.nb0" in out


def test_info_finds_firmware_of_any_version(tmp_path: Path, capsys,
                                            monkeypatch) -> None:
    (tmp_path / "firmware").mkdir()
    (tmp_path / "firmware" / "CUP2006.SHx.ORG_v00009999.nb0").write_bytes(b"")
    monkeypatch.setattr(cli, "IMAGES", tmp_path)
    assert cli.main(["info"]) == 0
    out = capsys.readouterr().out
    assert "CUP2006.SHx.ORG_v00009999.nb0" in out
    assert "dvd1/LOADING.KWI" in out and "missing" in out


def test_loading_bad_file_is_a_one_line_error(tmp_path: Path, capsys) -> None:
    bad = tmp_path / "bad.kwi"
    bad.write_bytes(b"xy")
    with pytest.raises(SystemExit) as exc:
        cli.main(["loading", str(bad)])
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "LOADING.KWI is too short" in err
    assert "Traceback" not in err


def test_boot_cup28_bad_disc_is_a_one_line_error(tmp_path: Path, capsys) -> None:
    with pytest.raises(SystemExit) as exc:
        cli.main(["boot", "--model", "cup28", "--disc", str(tmp_path / "no.iso")])
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "No such file" in err
    assert "Traceback" not in err


def test_info_lists_cup28_disc(tmp_path: Path, capsys, monkeypatch) -> None:
    monkeypatch.setattr(cli, "IMAGES", tmp_path)
    monkeypatch.setenv("CUP28_DISC", str(tmp_path / "disc.iso"))
    assert cli.main(["info"]) == 0
    out = capsys.readouterr().out
    assert "CUP28 input" in out and "disc.iso" in out and "missing" in out


def test_screenshot_resolves_relative_output(tmp_path: Path, monkeypatch) -> None:
    sent = {}

    class FakeQMP:
        def __init__(self, address):
            sent["address"] = address

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def screendump(self, filename):
            sent["file"] = filename

    monkeypatch.setattr(cli, "QMPClient", FakeQMP)
    monkeypatch.chdir(tmp_path)
    assert cli.main(["screenshot", "unix:/tmp/q.sock", "shot.png"]) == 0
    assert sent["file"] == str(tmp_path / "shot.png")
