# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""The board registry: the supported machines and their launcher modules.

Each board module exposes:

- prepare(unit, args) -> dict: prepares the unit's media and returns what
  qemu_argv needs;
- qemu_argv(media, *, display, serial, extra, qmp=None) -> list[str]: the
  QEMU command line;
- prepare_unit(unit) -> None: the board-specific records to land in a unit's
  non-volatile state (called by melco_nav_emu.unit.prepare).
"""
from . import cup2006, cup28

BOARDS = {"cup2006": cup2006, "cup28": cup28}


def get(name: str):
    """The board module for a machine name ("cup2006", "cup28")."""
    try:
        return BOARDS[name]
    except KeyError:
        raise KeyError("unknown board %r (have: %s)"
                       % (name, ", ".join(sorted(BOARDS)))) from None
