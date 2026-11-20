PYTHON ?= python3
VENV := .venv
PY := $(VENV)/bin/python

.PHONY: help venv qemu test boot extract flatten hdd clean

help:
	@echo "melco-nav-emu (CU-P2006 / Volvo RTI MMM+, SH7770/SH-4A, WinCE 4.2)"
	@echo "  make qemu      build QEMU: pinned release + qemu/core-patches + machine/*.c"
	@echo "  make test      offline unit tests (no images needed)"
	@echo "  make extract   copy DVD root files from your own ISOs in images/dvd/disc{1,2}"
	@echo "  make flatten   carve the NK from LOADING.KWI into images/firmware"
	@echo "  make hdd       HDD manifest from P2006.SCE (images/hdd-manifest.json)"
	@echo "  make boot      interactive boot in a window (serial on this terminal)"

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
	$(PY) tools/profile.py

extract:
	./scripts/extract-images.sh

flatten:
	$(PYTHON) scripts/flatten-firmware.py --loading images/dvd1/LOADING.KWI --out images/firmware

hdd:
	$(PYTHON) scripts/make-hdd.py --sce images/dvd1/P2006.SCE --out images/hdd-manifest.json

clean:
	find . -name __pycache__ -type d -prune -not -path "./qemu/*" -exec rm -rf {} +
	@echo "Inputs and the QEMU tree are preserved."
