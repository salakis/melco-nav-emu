# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# src/ for the melco_nav_emu package, so that every test file also runs on
# its own.
src = ROOT / "src"
if str(src) not in sys.path:
    sys.path.insert(0, str(src))
