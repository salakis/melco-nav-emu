# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""The CU-P2006 board: one place that defines how the unit boots.

Importing this module has no side effects: images are looked up, built and
checked only when a boot command line is made (prepare, first_power_on).
"""
from pathlib import Path
import os
import subprocess
import sys

from ..unit import POWERED_ON

ROOT = Path(__file__).resolve().parents[3]
QEMU = ROOT / "qemu/src/build/qemu-system-sh4"
# The NK image `make flatten` carves from the disc set's LOADING.KWI;
# scripts/flatten-firmware.py names it <module name>_v<version>.nb0, e.g.
# CUP2006.SHx.ORG_v00002308.nb0 for the 2017-18 Europe discs.
FIRMWARE = ROOT / "images/firmware"
NK_GLOB = "CUP2006.SHx.*.nb0"

# The virtual HDD: two FAT32 partitions, the map on the second (mounted as
# hdd2).  It is the complete P2006.SCE install layout assembled from the
# discs' own MAP/ volumes: a ~22 GB image, streamed from the discs' verified
# CPYs straight into the image (no temporary copies).
#
# CUP2006_HDD_SET=core builds a smaller image instead (~6 GB of data): the
# same layout, every target at its installed size, but only the targets in
# CORE_FULL carry their whole data; the other large ones keep a verified
# 8 MiB prefix and a hole.  Navi draws the map where 0.2/REGION/PARCEL cover
# the car's position and throws (`LOC_MapCtl::MapSizeGet inf:1`, then
# `MpRegionMapImp::Init`, JIT debugger, HMI starved) where its data is
# missing; searches of the address/POI indexes find nothing past the prefix.
# The value is validated when an image is built or booted, not at import, so a
# typo cannot break the CLI's other commands.
HDD_SET = os.environ.get("CUP2006_HDD_SET", "full")
CORE_FULL = ["PAR_MNG.ME", "TMCINFO.ME", "VOICE001.ME", "0.2", "REGION.KWI",
             "PARCEL.KWI"]
MANIFEST = ROOT / "images/hdd-manifest.json"
SCE = ROOT / "images/dvd1/P2006.SCE"
# What `make extract` copied into images/dvd{1,2}: the disc set those roots
# belong to (written by scripts/extract-images.sh).
EXTRACTED_SET = ROOT / "images" / ".disc-set"
BOOT_PART_MB = 64
MAP_PART_MB = 3700


def _escape(value: object) -> str:
    """A value for one `key=value` pair of the comma-separated -M string."""
    return str(value).replace(",", ",,")


def _hdd_set() -> str:
    """CUP2006_HDD_SET, checked at the point of use."""
    if HDD_SET not in ("full", "core"):
        raise SystemExit("cup2006: CUP2006_HDD_SET must be 'full' or 'core', "
                         "not %r" % HDD_SET)
    return HDD_SET

# Installed paths that differ from the default naming rule (`*.IDX` under
# `Map\IDX\`, everything else under `Map\`): the guest opens these CSV files
# as `hdd2\Map\IDX\SYSDFEU.CSV`.
MAP_DEST = {"SYSDFEU.CSV": "Map\\IDX\\SYSDFEU.CSV",      # Europe disc set
            "SYSDFNA.CSV": "Map\\IDX\\SYSDFNA.CSV"}      # North America set


def disc_set_id(disc, disc2=None) -> str:
    """The name the two discs share, e.g. "EU-3350-00": their file name without
    the `_1`/`_2` disc number and the extension."""
    names = [Path(p).stem for p in (disc, disc2) if p is not None]
    names = [n[:-2] if n.endswith(("_1", "_2")) else n for n in names]
    # Discs whose file names share no stem keep both, so they stay distinct.
    return names[0] if len(set(names)) == 1 else "+".join(names)


def sibling_disc(disc, number=2) -> Path:
    """Disc `number` of the set `disc` belongs to: its `_<n>` sibling in the
    `disc<n>` directory next to `disc1` (`images/dvd/disc1/X_1.ISO` ->
    `images/dvd/disc2/X_2.ISO`)."""
    disc = Path(disc)
    name = disc.name
    if ("_%d." % number) not in name and ("_1." in name or "_2." in name):
        name = name.replace("_1.", "_%d." % number).replace("_2.", "_%d." % number)
    parent = disc.parent
    if parent.name.endswith(("disc1", "disc2")):
        parent = parent.with_name("disc%d" % number)
    return parent / name


# The two-disc set in the drive.  Defaults are the 2017-18 Europe retail
# discs; CUP2006_DISC1/CUP2006_DISC2 point at another set (e.g. NA-2450-00).
DISC = Path(os.environ.get(
    "CUP2006_DISC1", str(ROOT / "images/dvd/disc1/EU-3350-00_1.ISO"))).resolve()
DISC2 = Path(os.environ.get("CUP2006_DISC2",
                            str(sibling_disc(DISC, 2))))


# The built images belong to one disc set: each set gets its own directory, so
# --disc (or another CUP2006_DISC1/CUP2006_DISC2) never boots one set's install
# with another set's map identity.
DISC_SET = disc_set_id(DISC, DISC2)


def _hdd_paths():
    """(product image, empty stand-in) for the disc set in the drive."""
    directory = ROOT / "build" / "hdd" / DISC_SET
    return (directory / ("hdd-native-verified.img" if HDD_SET != "core"
                         else "hdd-core.img"),
            directory / "hdd-empty.img")


HDD, HDD_EMPTY = _hdd_paths()


def find_nk(firmware=None) -> Path:
    """The flattened NK image in images/firmware/: exactly one must exist."""
    firmware = Path(firmware) if firmware is not None else FIRMWARE
    found = sorted(firmware.glob(NK_GLOB))
    if len(found) == 1:
        return found[0]
    if not found:
        raise SystemExit("cup2006: no NK image (%s) in %s; run `make extract "
                         "flatten` first" % (NK_GLOB, firmware))
    raise SystemExit("cup2006: several NK images in %s (%s); keep only the "
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
    if _hdd_set() == "full":
        full = [t["name"] for t in json.loads(MANIFEST.read_text())["targets"]]
    else:
        full = CORE_FULL
    for name in full:
        args += ["--full", name]
    build_atomically(HDD, args)


def _check_extracted_set():
    """The extracted disc roots are one set's too; refuse to mix them with
    another set in the drive."""
    if not EXTRACTED_SET.is_file():
        return                       # extracted before this record existed
    recorded = EXTRACTED_SET.read_text().strip()
    if recorded and recorded != DISC_SET:
        raise SystemExit(
            "cup2006: images/dvd1 and images/dvd2 were extracted from disc set "
            "%s, but the drive holds %s.  Run `make extract flatten hdd` for "
            "the set in the drive (or set CUP2006_DISC1/CUP2006_DISC2 back), "
            "so the firmware, install manifest and map identity match the "
            "discs." % (recorded, DISC_SET))


# Where the guest's map manager looks for the installed map identity
# (`MU_Get_OldMap_Version` reads `HDD2\MAP\ALLDATA.KWI`).
MAP_PROBE = ("MAP", "ALLDATA.KWI")


def hdd_map_problem(path) -> "str | None":
    """Why the HDD image at `path` holds no installed map, or None if its
    second (hdd2) partition has a non-empty `MAP\\ALLDATA.KWI`.  A boot
    without it ends on "Disc unavailable ... Please insert Disc 1" after the
    disclaimer, whatever disc is in the drive."""
    import struct
    try:
        with open(path, "rb") as f:
            def read(off, n):
                f.seek(off)
                return f.read(n)
            mbr = read(0, 512)
            if mbr[510:512] != b"\x55\xaa":
                return "no MBR"
            entry = mbr[446 + 16:446 + 32]
            if entry[4] not in (0x0B, 0x0C):
                return "no FAT32 second partition (the guest's hdd2)"
            base = struct.unpack_from("<I", entry, 8)[0] * 512
            bs = read(base, 512)
            if bs[0x52:0x5A] != b"FAT32   ":
                return "the second partition is not FAT32"
            bps, spc, rsv, nfats = struct.unpack_from("<HBHB", bs, 11)
            fatsz, root = struct.unpack_from("<I", bs, 36)[0], struct.unpack_from("<I", bs, 44)[0]
            fat_off = base + rsv * bps
            data_off = fat_off + nfats * fatsz * bps
            csize = bps * spc

            def find(cluster, want):
                for _ in range(4096):            # bounded chain walk
                    if not 2 <= cluster < 0x0FFFFFF8:
                        return None
                    data = read(data_off + (cluster - 2) * csize, csize)
                    for i in range(0, len(data), 32):
                        e = data[i:i + 32]
                        if e[0] == 0:
                            return None
                        if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                            continue
                        name = e[0:8].decode("latin-1").rstrip()
                        ext = e[8:11].decode("latin-1").rstrip()
                        if (name + ("." + ext if ext else "")).upper() == want:
                            first = (struct.unpack_from("<H", e, 20)[0] << 16 |
                                     struct.unpack_from("<H", e, 26)[0])
                            return first, struct.unpack_from("<I", e, 28)[0]
                    cluster = struct.unpack("<I", read(fat_off + 4 * cluster, 4))[0] & 0x0FFFFFFF
                return None

            found = find(root, MAP_PROBE[0])
            if found is None:
                return "no MAP folder on the map partition (an empty map volume)"
            found = find(found[0], MAP_PROBE[1])
            if found is None or found[1] == 0:
                return "no MAP\\%s on the map partition" % MAP_PROBE[1]
            head = read(data_off + (found[0] - 2) * csize, min(found[1], csize))
            if not head.strip(b"\x00"):
                return "MAP\\%s holds only zeros" % MAP_PROBE[1]
    except OSError as exc:
        return "cannot read it: %s" % exc
    return None


def _warn_no_map(path, problem):
    print("cup2006: WARNING: the HDD %s (%s) has no installed map: %s.\n"
          "cup2006: the guest will stop on \"Disc unavailable ... Please insert "
          "Disc 1\" after the disclaimer.  Delete it and boot again to build "
          "the map HDD from the discs." % (path, Path(path).resolve(), problem),
          file=sys.stderr)


def ensure_hdd() -> Path:
    """The HDD image for a unit without its own disk.

    The complete install image is built once, from the selected discs, into
    build/hdd/.  While the disc ISOs or the install manifest are missing, an
    empty two-partition image stands in (the guest still mounts hdd2; its
    map file opens fail and the map screen waits); the real image is built
    at the first boot after they appear.
    """
    _hdd_set()
    if HDD.exists():
        problem = hdd_map_problem(HDD)
        if problem:
            _warn_no_map(HDD, problem)
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
    print("cup2006: missing %s.\ncup2006: booting with an EMPTY map partition "
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


# The car on the MOST bus tells the unit its market via the Sub-CPU.  Default
# follows the disc set: NA-2450 -> 06 (North America), anything else -> 04
# (Europe); CUP2006_CARMARKET=<hex> overrides (a0 = Aston Martin marque).
CARMARKET = os.environ.get("CUP2006_CARMARKET",
                           "06" if "NA-" in DISC.name else "04")


def _machine_opts():
    """Board model options for the product boot.  Each stands for hardware
    the real unit has: the self-test ADC sample (board revision, GPS
    antenna), the boot ROM's INTC setup, the RTC, the ATA disk, the Sub-CPU
    on the SUB1: link (power, car status and keys arrive as its frames), the
    DVD drive's mecha-link interrupt, the vehicle-signal sensors, and F12
    as the ACC key.  The disk image (hddimage=) is per unit: unit_machine().

    A map disc is in the drive, as in a real installed unit; F11 ejects it
    and inserts the next one (disc 2, then disc 1 again), as during a map
    install.  CUP2006_MBX3D=1 runs the 3D core on the vendored PowerVR MBX1
    model (machine/mbx, MIT), which is experimental: it parses the guest's TA
    stream and rasterises its scene, but the guest still stops after the first
    map composition (docs/mbx3d.md).  The default keeps the register latch
    window, where the 3D car marker is missing and CUP2006_CARMARKER=1 draws
    an approximation of it (a STUB whose colours and shading are the
    emulator's, see machine/cup2006.c)."""
    opts = ["cup2006", "selftest=on", "intcinit=on", "rtc=on", "ata=on",
            "subpeer=on", "dvd5irq=on", "carsig=on", "acckey=on"]
    if DISC.is_file():
        opts.append("dvdimage=%s" % _escape(DISC))
    if DISC.is_file() and DISC2.is_file():
        opts.append("dvdnext=%s" % _escape(DISC2))
    opts.append("carmarket=%s" % CARMARKET)
    if os.environ.get("CUP2006_MBX3D", "") not in ("", "0"):
        opts.append("mbx3d=on")
    if os.environ.get("CUP2006_CARMARKER", "") not in ("", "0"):
        opts.append("carmarker=on")
    return opts


MACHINE = _machine_opts()


def _set_disc(disc):
    """Select the disc set at runtime (--disc / MELCO_DISC): disc 1, with disc
    2 from CUP2006_DISC2 or its `_2` sibling under `disc2/`.

    The set is one unit of media: the virtual HDD, the market byte and the
    rest of the machine options follow it, because the install layout, the map
    identity and the region all come from the discs."""
    global DISC, DISC2, DISC_SET, HDD, HDD_EMPTY, CARMARKET, MACHINE
    DISC = Path(disc).resolve()
    # CUP2006_DISC2 names disc 2 of its own set: keep it when it belongs to
    # this disc 1, else take the `_2` sibling.
    env2 = os.environ.get("CUP2006_DISC2")
    DISC2 = (Path(env2) if env2 and disc_set_id(DISC, env2) == disc_set_id(DISC)
             else sibling_disc(DISC, 2)).resolve()
    DISC_SET = disc_set_id(DISC, DISC2)
    HDD, HDD_EMPTY = _hdd_paths()
    CARMARKET = os.environ.get("CUP2006_CARMARKET",
                               "06" if "NA-" in DISC.name else "04")
    MACHINE = _machine_opts()
# One unit's persistent state (flash, battery RAM, battery SRAM, EEPROM),
# kept across boots like the car's.  CUP2006_UNIT=<dir> (or MELCO_UNIT, see
# the CLI) picks another unit.
UNIT = Path(os.environ.get("CUP2006_UNIT", str(ROOT / "build" / "unit")))
DEFAULT_UNIT = UNIT


# The guest's APM (map-install) backup areas start with this ASCII version
# date, followed by two records.  Flash holds three such areas (0x1e40800,
# 0x1e80800, 0x1ec0800); battery RAM holds one, at 0x40800, which
# corresponds to the first flash area.  The status checks below read that
# pair.
APM_DATE = b"20080207"
FLASH_APM_AREA = 0x1e40800    # first APM area in flash
BKRAM_APM_AREA = 0x40800      # the APM area in battery RAM (the one the
                              # guest reads on a production board)
RECORD = 0x540                # record stride; body at area + 0x10 + n * RECORD
MAPDWLSTS = 0x370             # install-status byte in the record body


def _dwl_sts(path, area):
    path = Path(path)
    if not path.is_file():
        return None
    want = 0x10 + 2 * RECORD
    with open(path, "rb") as f:
        f.seek(area)
        data = f.read(want)
    if len(data) < want or data[:8] != APM_DATE:
        return None
    return [data[0x10 + n * RECORD + MAPDWLSTS] for n in (0, 1)]


def map_dwl_sts(flash):
    """Install status of both records of the first APM area in flash, or
    None if the area is not guest-formatted."""
    return _dwl_sts(flash, FLASH_APM_AREA)


def bkram_dwl_sts(bkram):
    """Install status of both APM records in battery RAM, or None if the
    guest has not formatted its backup areas there yet."""
    return _dwl_sts(bkram, BKRAM_APM_AREA)


def prepare_unit(unit: Path) -> None:
    """Land the map-install record in the unit's guest-formatted APM areas
    (flash and battery RAM), powering a new unit on once headless first so
    the guest formats its battery RAM.  Called by melco_nav_emu.unit.prepare.

    STUB (the HDD this emulator serves already holds the complete install,
    so the unit's record says so): status 0 ("install complete") plus the
    disc's map identity, re-landed at every boot in every guest-formatted
    APM area by scripts/apm_record_seed.py.
    """
    unit = Path(unit)
    bkram = unit / "bkram.bin"
    if bkram_dwl_sts(bkram) is None and not (unit / POWERED_ON).exists():
        first_power_on(unit, hdd=unit_hdd(unit))
    stores = [p for p, sts in ((unit / "flash.img", map_dwl_sts(unit / "flash.img")),
                               (bkram, bkram_dwl_sts(bkram))) if sts is not None]
    if not stores:
        return                    # the guest formats it on this boot
    for store in stores:
        subprocess.check_call([sys.executable,
                               str(ROOT / "scripts/apm_record_seed.py"),
                               str(store), str(store)],
                              stdout=subprocess.DEVNULL)
    (unit / ".installed").write_text(
        "map-install record landed (boards/cup2006.py prepare_unit)\n")


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
        raise SystemExit("cup2006: CUP2006_SCENARIO=%s, but %s has no %s"
                         % (name, scenario, " or ".join(missing)))
    return ["gnssreplay=%s" % _escape(replay),
            "gpstrack=%s" % _escape(sensors)]


def unit_hdd(unit) -> Path:
    """The disk a unit boots with: its own hdd.img (factory units), else the
    shared product HDD (built on first use)."""
    own = Path(unit) / "hdd.img"
    return own if own.is_file() else ensure_hdd()


def unit_machine(unit, hdd):
    """Machine options for one unit's board revision, non-volatile state
    (melco_nav_emu/unit.py: flash, battery RAM, battery SRAM, onboard EEPROM) and
    disk."""
    unit = Path(unit)
    opts = ["hwrevadc=410",
            "drfimage=%s" % _escape(unit / "flash.img"),
            "bkram=%s" % _escape(unit / "bkram.bin"),
            "sramsave=%s" % _escape(unit / "sram.bin"),
            "eeprom=%s" % _escape(unit / "eeprom.bin"),
            "hddimage=%s" % _escape(hdd)]
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
        raise SystemExit("cup2006: %s not found; run `make qemu` first" % QEMU)
    machine = _machine_opts() + unit_machine(unit, hdd) + gps_machine()
    extra = [str(a) for a in extra]
    # Sound (machine/cup2006-audio.c.inc): a window run plays through the
    # host's audio; headless runs count frames only, unless the caller names
    # its own audiodev (e.g. a wav capture).
    audio = []
    if any(a == "-audiodev" or a.startswith("-audiodev=") for a in extra):
        audio_id = audiodev_id(extra)
        if audio_id is None:
            raise SystemExit("cup2006: -audiodev given without an id=..., so "
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
    unit = Path(unit)
    unit.mkdir(parents=True, exist_ok=True)
    if hdd is None:
        hdd = unit_hdd(unit)
    argv = _argv(unit, "none", (os.devnull,), (), hdd)
    bkram = unit / "bkram.bin"
    log = unit / FIRST_POWER_ON_LOG
    print("cup2006: first power-on of %s: letting the unit format its backup "
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
            "cup2006: QEMU exited with code %d during the first power-on of "
            "%s, before the guest formatted its battery RAM.  Last lines of "
            "%s:\n%s" % (exited, unit, log, "\n".join(tail)))
    (unit / POWERED_ON).write_text(
        "first power-on ran %d s without the guest formatting its battery "
        "RAM APM area; delete this file to try again\n" % timeout)
    print("cup2006: WARNING: the guest did not format its battery RAM APM "
          "area within %d s, so the map-install record could not be landed "
          "(different firmware with another APM version date?).  The unit "
          "boots as the guest leaves it; this power-on is not repeated "
          "(delete %s to retry)." % (timeout, unit / POWERED_ON),
          file=sys.stderr)
    return False


def _default_unit() -> Path:
    return Path(os.environ.get("MELCO_UNIT") or UNIT)


def prepare(unit, args=None) -> dict:
    """Prepare a unit's media and return what qemu_argv needs: the prepared
    unit directory (flash, battery RAM, SRAM, EEPROM; melco_nav_emu.unit)
    and the disk it boots with."""
    from .. import unit as unit_mod
    if args is not None:
        if getattr(args, "disc", None):
            _set_disc(args.disc)
        if getattr(args, "qemu", None):
            global QEMU
            QEMU = Path(args.qemu)
    _check_extracted_set()
    unit = Path(unit) if unit is not None else _default_unit()
    if not unit.is_absolute():
        unit = ROOT / unit
    hdd = unit_hdd(unit)
    unit_mod.prepare(unit, "cup2006")
    return {"unit": unit, "hdd": hdd}


def qemu_argv(media: dict, *, display="cocoa,zoom-to-fit=on",
              serial=("stdio",), extra=(), qmp=None) -> list:
    """The QEMU command line for a unit prepared by prepare()."""
    argv = _argv(media["unit"], display, serial, extra, media["hdd"])
    if qmp is not None:
        argv += ["-qmp", "unix:%s,server=on,wait=off" % _escape(qmp)]
    return argv
