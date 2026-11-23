# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""The product boot profile: one place that defines how the CU-P2006 boots.

Importing this module has no side effects: images are looked up, built and
checked only when a boot command line is made (qemu_argv, first_power_on).
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:      # `make boot` runs this file as a script
    sys.path.insert(0, str(ROOT))
QEMU = ROOT / "qemu/src/build/qemu-system-sh4"
# The NK image `make flatten` carves from the disc set's LOADING.KWI;
# scripts/flatten-firmware.py names it <module name>_v<version>.nb0, e.g.
# CUP2006.SHx.ORG_v00002308.nb0 for the 2017-18 Europe discs.
FIRMWARE = ROOT / "images/firmware"
NK_GLOB = "CUP2006.SHx.*.nb0"

# The virtual HDD: two FAT32 partitions, the map on the second (mounted as
# hdd2).  It is the complete P2006.SCE install layout assembled from the
# discs' own MAP/ volumes: a ~22 GB image, and the build needs about as much
# again for its per-target part files until it finishes.
HDD = ROOT / "build/hdd/hdd-native-verified.img"
# Stand-in while the disc ISOs or the install manifest are missing: the same
# two partitions, empty.  Never used once the real image can be built.
HDD_EMPTY = ROOT / "build/hdd/hdd-empty.img"
MANIFEST = ROOT / "images/hdd-manifest.json"
SCE = ROOT / "images/dvd1/P2006.SCE"
BOOT_PART_MB = 64
MAP_PART_MB = 3700

# Installed paths that differ from the default naming rule (`*.IDX` under
# `Map\IDX\`, everything else under `Map\`): the guest opens these CSV files
# as `hdd2\Map\IDX\SYSDFEU.CSV`.
MAP_DEST = {"SYSDFEU.CSV": "Map\\IDX\\SYSDFEU.CSV",      # Europe disc set
            "SYSDFNA.CSV": "Map\\IDX\\SYSDFNA.CSV"}      # North America set


# The two-disc set in the drive.  Defaults are the 2017-18 Europe retail
# discs; CUP2006_DISC1/CUP2006_DISC2 point at another set (e.g. NA-2450-00).
DISC = Path(os.environ.get(
    "CUP2006_DISC1", str(ROOT / "images/dvd/disc1/EU-3350-00_1.ISO")))
DISC2 = Path(os.environ.get(
    "CUP2006_DISC2", str(ROOT / "images/dvd/disc2/EU-3350-00_2.ISO")))


def find_nk(firmware=None) -> Path:
    """The flattened NK image in images/firmware/: exactly one must exist."""
    firmware = Path(firmware) if firmware is not None else FIRMWARE
    found = sorted(firmware.glob(NK_GLOB))
    if len(found) == 1:
        return found[0]
    if not found:
        raise SystemExit("profile: no NK image (%s) in %s; run `make extract "
                         "flatten` first" % (NK_GLOB, firmware))
    raise SystemExit("profile: several NK images in %s (%s); keep only the "
                     "one carved from the disc set in use, or delete the "
                     "directory and run `make flatten` again"
                     % (firmware, ", ".join(p.name for p in found)))


def build_atomically(out: Path, argv) -> Path:
    """Run an image builder with `--out <out>.partial`, then rename the result
    to `out`.  A failed or interrupted build leaves no file at `out`, so a
    later boot never takes a half-written image for a finished one."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".partial")
    try:
        subprocess.check_call([str(a) for a in argv] + ["--out", str(tmp)])
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    os.replace(tmp, out)
    return out


def _build_map_hdd():
    import json
    args = [sys.executable, str(ROOT / "scripts/make-hdd-map.py"),
            "--fat", "32", "--layout",
            "--cluster-kb", "16", "--label2", "MAP",
            "--disc1", str(DISC), "--disc2", str(DISC2),
            "--manifest", str(MANIFEST)]
    for name, path in MAP_DEST.items():
        args += ["--dest", "%s=%s" % (name, path)]
    for target in json.loads(MANIFEST.read_text())["targets"]:
        args += ["--full", target["name"]]
    try:
        build_atomically(HDD, args)
    except BaseException:
        # make-hdd-map.py keeps its per-target part files (up to the size of
        # the whole map) next to the image until the image is complete.
        for part in HDD.parent.glob("part-*"):
            part.unlink(missing_ok=True)
        raise


def ensure_hdd() -> Path:
    """The HDD image for a unit without its own disk.

    The complete install image is built once, from the selected discs, into
    build/hdd/.  While the disc ISOs or the install manifest are missing, an
    empty two-partition image stands in (the guest still mounts hdd2; its
    map file opens fail and the map screen waits); the real image is built
    at the first boot after they appear.
    """
    if HDD.exists():
        return HDD
    if not MANIFEST.is_file() and SCE.is_file():
        # `make hdd` not run yet: parse the install manifest from extracted
        # disc 1 (written atomically by make-hdd.py).
        subprocess.check_call([sys.executable, str(ROOT / "scripts/make-hdd.py"),
                               "--sce", str(SCE), "--out", str(MANIFEST)])
    missing = [str(p) for p in (DISC, DISC2, MANIFEST) if not p.is_file()]
    if not missing:
        _build_map_hdd()
        return HDD
    print("profile: missing %s.\nprofile: booting with an EMPTY map partition "
          "(%s); the map cannot load.  Put the disc ISOs in place (or set "
          "CUP2006_DISC1/CUP2006_DISC2) and run `make extract hdd`: the real "
          "HDD is then built at the next boot." % (", ".join(missing), HDD_EMPTY),
          file=sys.stderr)
    if not HDD_EMPTY.exists():
        build_atomically(HDD_EMPTY, [
            sys.executable, str(ROOT / "scripts/make-hdd-image.py"),
            "--size-mb", str(BOOT_PART_MB + MAP_PART_MB),
            "--fat", "32", "--label", "VOL", "--part2-mb", str(MAP_PART_MB),
        ])
    return HDD_EMPTY


# Board model options for the product boot.  Each stands for hardware the
# real unit has: the self-test ADC sample (board revision, GPS antenna),
# the boot ROM's INTC setup, the RTC, the ATA disk, the Sub-CPU on the SUB1:
# link (power, car status and keys arrive as its frames), the DVD drive's
# mecha-link interrupt, the vehicle-signal sensors, and F12 as the ACC key.
# The disk image (hddimage=) is per unit: see unit_machine().
MACHINE = ["cup2006", "selftest=on", "intcinit=on", "rtc=on", "ata=on",
           "subpeer=on",
           "dvd5irq=on",
           "carsig=on",
           "acckey=on"]
# A map disc is in the drive, as in a real installed unit; F11 ejects it and
# inserts the next one (disc 2, then disc 1 again), as during a map install.
if DISC.is_file():
    MACHINE.append("dvdimage=%s" % DISC)
if DISC.is_file() and DISC2.is_file():
    MACHINE.append("dvdnext=%s" % DISC2)
# The car on the MOST bus tells the unit its market via the Sub-CPU.  Default
# follows the disc set: NA-2450 -> 06 (North America), anything else -> 04
# (Europe); CUP2006_CARMARKET=<hex> overrides (a0 = Aston Martin marque).
CARMARKET = os.environ.get("CUP2006_CARMARKET",
                           "06" if "NA-" in DISC.name else "04")
MACHINE.append("carmarket=%s" % CARMARKET)
# The MBX 3D core is not emulated, so Navi's 3D car marker is missing from
# the 3D map view; CUP2006_CARMARKER=1 draws an approximation of it (a STUB
# whose colours and shading are the emulator's, see machine/cup2006.c).
if os.environ.get("CUP2006_CARMARKER", "") not in ("", "0"):
    MACHINE.append("carmarker=on")
# One unit's persistent state (flash, battery RAM, battery SRAM, EEPROM),
# kept across boots like the car's.  CUP2006_UNIT=<dir> picks another unit.
UNIT = Path(os.environ.get("CUP2006_UNIT", str(ROOT / "build" / "unit")))


def gps_machine():
    """GPS: replay a GNSS radio recording through the tracking chip (the fix
    is computed from the signal, never injected).  CUP2006_SCENARIO=<name>
    selects scenarios/<name>/ (shipped) or build/gps/<name>/ (your own), each
    with drive.replay and sensors.csv; without it the receiver has no
    signal."""
    name = os.environ.get("CUP2006_SCENARIO")
    if not name:
        return []
    # The repository's own scenarios, then local ones in build/gps/.
    scenario = ROOT / "scenarios" / name
    if not scenario.is_dir():
        scenario = ROOT / "build" / "gps" / name
    replay, sensors = scenario / "drive.replay", scenario / "sensors.csv"
    missing = [p.name for p in (replay, sensors) if not p.is_file()]
    if missing:
        raise SystemExit("profile: CUP2006_SCENARIO=%s, but %s has no %s"
                         % (name, scenario, " or ".join(missing)))
    return ["gnssreplay=%s" % replay, "gpstrack=%s" % sensors]


def unit_hdd(unit) -> Path:
    """The disk a unit boots with: its own hdd.img (factory units), else the
    shared product HDD (built on first use)."""
    own = Path(unit) / "hdd.img"
    return own if own.is_file() else ensure_hdd()


def unit_machine(unit, hdd):
    """Machine options for one unit's board revision, non-volatile state
    (tools/unit.py: flash, battery RAM, battery SRAM, onboard EEPROM) and
    disk."""
    unit = Path(unit)
    opts = ["hwrevadc=410", "drfimage=%s" % (unit / "flash.img"),
            "bkram=%s" % (unit / "bkram.bin"), "sramsave=%s" % (unit / "sram.bin"),
            "eeprom=%s" % (unit / "eeprom.bin"), "hddimage=%s" % hdd]
    # A unit with its own disk keeps the guest's writes in it.
    if Path(hdd) == unit / "hdd.img":
        opts.append("hddwrite=on")
    return opts


def audiodev_id(extra):
    """The id= of the first -audiodev option in extra QEMU args, or None.

    -audiodev takes its spec as the next argument (or after -audiodev=);
    every comma-separated key=value pair is checked, so id= need not be the
    first property (e.g. "-audiodev coreaudio,drv=...,id=snd0").
    """
    for i, arg in enumerate(extra):
        if arg == "-audiodev" and i + 1 < len(extra):
            spec = extra[i + 1]
        elif arg.startswith("-audiodev="):
            spec = arg[len("-audiodev="):]
        else:
            continue
        for kv in spec.split(","):
            if kv.startswith("id="):
                return kv[len("id="):]
    return None


def _argv(unit, display, serial, extra, hdd):
    if not QEMU.is_file():
        raise SystemExit("profile: %s not found; run `make qemu` first" % QEMU)
    machine = list(MACHINE) + unit_machine(unit, hdd) + gps_machine()
    extra = [str(a) for a in extra]
    # Sound (machine/cup2006-audio.c.inc): a window run plays through the
    # host's audio; headless runs count frames only, unless the caller names
    # its own audiodev (e.g. a wav capture).
    audio = []
    if any(a == "-audiodev" or a.startswith("-audiodev=") for a in extra):
        audio_id = audiodev_id(extra)
        if audio_id is None:
            raise SystemExit("profile: -audiodev given without an id=..., so "
                             "the machine's audiodev= cannot name it")
        machine.append("audiodev=%s" % audio_id)
    elif display.startswith("cocoa"):
        audio = ["-audiodev", "coreaudio,id=snd0"]
        machine.append("audiodev=snd0")
    argv = [str(QEMU), "-M", ",".join(machine), "-m", "256M", "-display", display,
            "-monitor", "none", "-kernel", str(find_nk())] + audio
    for target in serial:
        argv += ["-serial", target if target == "stdio" else "file:%s" % target]
    # Keep virtual device deadlines tied to executed guest time (icount),
    # unless the caller chose its own clock.
    clock = [] if "-icount" in extra else ["-icount", "shift=auto,sleep=on"]
    return argv + clock + extra


FIRST_POWER_ON_LOG = "first-power-on.log"
POLL_S = 1.0            # battery RAM check interval during the first power-on
BKRAM_SAVE_S = 3.0      # one battery-RAM save period of the machine model


def _stop(proc):
    """SIGTERM, then wait: QEMU's exit handlers save battery RAM."""
    import signal
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def first_power_on(unit, timeout=120, hdd=None):
    """Power a new unit on once, headless, until the guest has formatted its
    battery RAM backup areas (they hold the map-install record on this
    board revision), then switch it off.  Takes ~15-30 s, once per unit.

    QEMU's output goes to <unit>/first-power-on.log.  If QEMU exits before
    the battery RAM is formatted, this stops with QEMU's exit code and the
    log's last lines.  If the guest has not formatted it after `timeout`
    seconds, the unit gets a .powered-on marker so that later boots do not
    repeat this power-on.
    """
    import time
    from tools.unit import POWERED_ON, bkram_dwl_sts
    unit = Path(unit)
    unit.mkdir(parents=True, exist_ok=True)
    if hdd is None:
        hdd = unit_hdd(unit)
    argv = _argv(unit, "none", (os.devnull,), (), hdd)
    bkram = unit / "bkram.bin"
    log = unit / FIRST_POWER_ON_LOG
    print("profile: first power-on of %s: letting the unit format its backup "
          "memory (once, up to %d s; log: %s)..." % (unit, timeout, log),
          file=sys.stderr)
    with log.open("wb") as out:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out,
                                stderr=subprocess.STDOUT)
    deadline = time.monotonic() + timeout
    exited = None
    try:
        while time.monotonic() < deadline:
            time.sleep(POLL_S)
            if bkram_dwl_sts(bkram) is not None:
                time.sleep(BKRAM_SAVE_S)   # let one more save period pass
                break
            exited = proc.poll()
            if exited is not None:
                break
    finally:
        _stop(proc)
    if bkram_dwl_sts(bkram) is not None:
        return True
    if exited is not None:
        tail = log.read_text(errors="replace").splitlines()[-20:]
        raise SystemExit(
            "profile: QEMU exited with code %d during the first power-on of "
            "%s, before the guest formatted its battery RAM.  Last lines of "
            "%s:\n%s" % (exited, unit, log, "\n".join(tail)))
    (unit / POWERED_ON).write_text(
        "first power-on ran %d s without the guest formatting its battery "
        "RAM APM area; delete this file to try again\n" % timeout)
    print("profile: WARNING: the guest did not format its battery RAM APM "
          "area within %d s, so the map-install record could not be landed "
          "(different firmware with another APM version date?).  The unit "
          "boots as the guest leaves it; this power-on is not repeated "
          "(delete %s to retry)." % (timeout, unit / POWERED_ON),
          file=sys.stderr)
    return False


def qemu_argv(display="cocoa,zoom-to-fit=on", serial=("stdio",), extra=(),
              unit=None):
    from tools.unit import prepare
    unit = Path(unit) if unit is not None else UNIT
    if not unit.is_absolute():
        unit = ROOT / unit
    hdd = unit_hdd(unit)
    unit = prepare(unit,
                   first_power_on=lambda u: first_power_on(u, hdd=hdd))
    return _argv(unit, display, serial, extra, hdd)


def default_display() -> str:
    """The window backend when DISPLAY_BACKEND is unset: cocoa on macOS,
    sdl elsewhere (cocoa does not exist off Darwin, so `make boot` would
    fail on Linux)."""
    if os.environ.get("DISPLAY_BACKEND"):
        return os.environ["DISPLAY_BACKEND"]
    return "cocoa,zoom-to-fit=on" if sys.platform == "darwin" else "sdl"


if __name__ == "__main__":
    # zoom-to-fit makes QEMU's cocoa window resizable (the 400x234 panel
    # scales with it); without it the window is fixed at the guest's size.
    argv = qemu_argv(display=default_display())
    os.execv(argv[0], argv + sys.argv[1:])
