# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# src/ for the melco_nav_emu package, the repo root for tools.*, so that
# every test file also runs on its own.
for path in (ROOT / "src", ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))
