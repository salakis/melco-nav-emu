# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""boards/cup2006.py: CUP2006_MBX3D, the switch that puts the vendored
PowerVR MBX1 model behind the core's register window.

Offline: only the board's machine-option string is built, so no image or
media is needed. The switch adds an option to the same machine; it never
selects a second one.
"""
import importlib

import pytest

from melco_nav_emu.boards import cup2006 as profile


@pytest.fixture
def fresh_profile(monkeypatch):
    for var in ("CUP2006_MBX3D", "CUP2006_CARMARKER"):
        monkeypatch.delenv(var, raising=False)
    importlib.reload(profile)
    yield profile
    monkeypatch.undo()
    importlib.reload(profile)


def machine_opts(fresh_profile, monkeypatch, **env):
    for var in ("CUP2006_MBX3D", "CUP2006_CARMARKER"):
        monkeypatch.delenv(var, raising=False)
    for var, value in env.items():
        monkeypatch.setenv(var, value)
    importlib.reload(profile)
    return list(fresh_profile._machine_opts())


def test_the_model_is_off_unless_asked(fresh_profile, monkeypatch):
    assert "mbx3d=on" not in machine_opts(fresh_profile, monkeypatch)
    assert "mbx3d=on" not in machine_opts(fresh_profile, monkeypatch,
                                          CUP2006_MBX3D="")


def test_the_model_is_off_for_zero(fresh_profile, monkeypatch):
    assert "mbx3d=on" not in machine_opts(fresh_profile, monkeypatch,
                                          CUP2006_MBX3D="0")


@pytest.mark.parametrize("value", ["1", "on", "yes", "true"])
def test_the_model_is_on_when_asked(fresh_profile, monkeypatch, value):
    opts = machine_opts(fresh_profile, monkeypatch, CUP2006_MBX3D=value)
    assert "mbx3d=on" in opts
    # It is an addition to the same machine, never a second one.
    assert opts[0] == "cup2006"
    assert sum(o.startswith("mbx3d") for o in opts) == 1


def test_the_marker_approximation_stays_independent(fresh_profile, monkeypatch):
    """CARMARKER is a separate switch: with the model on it still appends."""
    opts = machine_opts(fresh_profile, monkeypatch,
                        CUP2006_MBX3D="1", CUP2006_CARMARKER="1")
    assert "mbx3d=on" in opts
    assert "carmarker=on" in opts
