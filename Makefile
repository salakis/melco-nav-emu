# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
PYTHON ?= python3
VENV := .venv
PY := $(VENV)/bin/python
MODEL ?= cup2006

.PHONY: help venv qemu test boot extract flatten hdd clean

help:
	@echo "melco-nav-emu (Volvo RTI CU-P2006 and VNS CUP28)"
	@echo "  make qemu      build QEMU: pinned release + qemu/core-patches + machine/*.c"
	@echo "  make test      offline unit tests (no images needed)"
	@echo "  make extract   copy DVD root files from your own ISOs in images/dvd/disc{1,2}"
	@echo "  make flatten   carve the NK from LOADING.KWI into images/firmware"
	@echo "  make hdd       HDD manifest from P2006.SCE (images/hdd-manifest.json)"
	@echo "  make boot      interactive CU-P2006 boot"
	@echo "  CUP28_DISC=/path/to/disc.iso make boot MODEL=cup28"

venv: $(VENV)/.installed
$(VENV)/.installed: pyproject.toml
	test -x $(VENV)/bin/pip || $(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --quiet --upgrade pip
	$(VENV)/bin/pip install --quiet -e ".[test]" ninja tomli
	touch $@

qemu: venv
	./scripts/qemu.sh

test: venv
	$(PY) -m pytest -q

boot: venv
	$(PY) -m melco_nav_emu.cli boot --model "$(MODEL)"

extract:
	./scripts/extract-images.sh

flatten:
	$(PYTHON) scripts/flatten-firmware.py --loading images/dvd1/LOADING.KWI --out images/firmware

hdd:
	$(PYTHON) scripts/make-hdd.py --sce images/dvd1/P2006.SCE --out images/hdd-manifest.json

clean:
	find . -name __pycache__ -type d -prune -not -path "./qemu/*" -exec rm -rf {} +
	@echo "Inputs and the QEMU tree are preserved."
