# melco-nav-emu

A QEMU-based emulator for the **Volvo RTI MMM+ navigation head unit**
(CU-P2006: Renesas SH7770 / SH-4A, Windows CE 4.2, hardware by Mitsubishi
Electric) from Volvo P1 cars. It boots the real, unmodified head-unit software
from your own dump of a retail map DVD set to the navigation map.

Emulation is at the **hardware** level: the SoC and the board devices are
modelled at their registers and interrupts (display unit, 2DG drawing engine,
Sub-CPU link with keys and car status, ATA disk, DVD drive, GPS tracking chip,
flash, battery RAM, EEPROM, sound). The guest's kernel, drivers and
applications run unmodified from the disc's own image. Where a device is only
approximated, the code says so with a `STUB:` comment.

Status: runs on macOS (Apple Silicon) with the 2017-18 Europe discs
(`EU-3350-00`, Volvo part no. 32214046): boot logo, disclaimer, ENTER, then
the 3D and 2D map views driven from the keyboard, with GPS from a recording,
and voice prompts.
Known gaps are listed under [Limitations](#limitations).

![2D map view on the A-4 (Autovía del Sur), car marker following the `eu-a4` GPS scenario](screenshots/eu-a4-2d-map.png)

No firmware, disc or map data is included. You need your own dumps.

## Requirements

- macOS with the Xcode command line tools, and Homebrew `glib`, `pixman`
  and `pkg-config` (QEMU's build dependencies). Linux builds the same way but
  is untested.
- Python 3.9 or later.
- About **45 GB free disk**: the disc ISOs (~9.5 GB), and the virtual HDD
  (~22 GB, built once from the discs; the build needs about as much again
  for temporary part files until it finishes).

## Install and run

```sh
make qemu        # download the pinned QEMU release, patch it, build (minutes)
make test        # offline unit tests, no images needed
```

Put your dumps of the two discs in place:

```
images/dvd/disc1/EU-3350-00_1.ISO
images/dvd/disc2/EU-3350-00_2.ISO
```

```sh
make extract flatten hdd   # disc root files -> images/dvd{1,2}/,
                           # firmware -> images/firmware/, install manifest
make boot                  # first run builds the virtual HDD (~22 GB, once)
```

`make boot` opens the unit's screen in a window that can be resized (the
400x234 panel scales to fit); the unit's serial console prints on the
terminal. The first boot of a new unit is preceded by one short headless
power-on (about 30 s, log in `build/unit/first-power-on.log`), in which the
guest formats its battery RAM.

Keys:

| key | unit |
|---|---|
| arrows | joystick |
| ENTER | ENTER |
| ESC | BACK |
| F10 | disc slot shutter |
| F11 | disc eject button / hand at the slot |
| F12 | ignition off (the unit saves its state and powers down; QEMU exits) |

The disclaimer is accepted with ENTER, as in the car.

## The unit's state

`build/unit/` holds one head unit's non-volatile memory: its flash, battery
RAM, battery SRAM and EEPROM. It persists across boots like the car's, so
settings and the last position survive. The map itself is on the virtual HDD
in `build/hdd/`, shared by all units.

- `rm -rf build/unit` gives a **new unit with the map installed**: the next
  `make boot` powers it on once to format its memory, then marks the map
  install as complete (STUB: the HDD already holds the complete install, so
  the unit's install record says so).
- `CUP2006_UNIT=<dir> make boot` uses another unit directory.
- `python3 tools/unit.py show build/unit` prints what a unit's memory says.

## Disc sets, region and marque

Another disc set is chosen with environment variables:

```sh
export CUP2006_DISC1=/path/NA-2450-00_1.ISO CUP2006_DISC2=/path/NA-2450-00_2.ISO
rm -rf build/hdd build/unit images/dvd1 images/dvd2 images/firmware images/hdd-manifest.json
make extract flatten hdd
make boot
```

The derived files (extracted roots, firmware, manifest, HDD, unit) belong to
one disc set, hence the `rm` when switching. The firmware image is found in
`images/firmware/` by name pattern, whatever version the discs carry.

The car tells the unit its market over the MOST bus; `CUP2006_CARMARKET=<hex>`
sets that byte, which selects **region and marque**. It defaults to the disc
set: `06` when the disc 1 file name contains `NA-`, else `04`.

| value | region | marque |
|---|---|---|
| `04` | Europe | Volvo |
| `06` | North America | Volvo |
| `a0` | Europe | Aston Martin |
| `a1` | North America | Aston Martin |

A North America set (`NA-2450-00`) uses the same disc format and firmware as
the Europe set. Other and older disc sets have not been tested.

## GPS

Without a recording the GPS receiver has no signal (no fix). Two short
recorded drives ship in `scenarios/`, each 3 minutes at 110 km/h on the
straightest 5.5 km of a motorway, picked from OpenStreetMap:

| scenario | road | discs |
|---|---|---|
| `eu-a4` | A-4 southbound, La Mancha, Spain | Europe |
| `na-i80` | I-80 westbound, Bonneville Salt Flats, Utah | North America |

```sh
CUP2006_SCENARIO=eu-a4 make boot
```

On a new unit the first fix comes about 35 s after power-on; the recording
ends at about 180 s, after which the receiver reports signal loss. Your own
recordings go in `build/gps/<name>/` (gitignored); `tools/gps/README.md`
explains how they are made. A scenario directory holds:

- `drive.replay`: the tracking chip's per-satellite measurements over time.
  Format: the 8 bytes `GNSSIQ1\0`, a little-endian u32 frame count and a u32
  frame interval in ms, then one 4096-byte frame per interval holding 32
  measurement records of 0x50 bytes (one per GPS PRN, first word = PRN,
  0 = no signal) followed by 32 navigation records of 0x30 bytes (same
  indexing). The guest's own GPS library computes every fix from these.
- `sensors.csv`: the car's sensors on the same clock, one row per sample:
  `t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps[,gps]`. Speed
  drives the wheel-pulse frames, yaw the gyro; the position columns are not
  used.

## Other options

| variable / option | effect |
|---|---|
| `CUP2006_CARMARKER=1` | draw an approximation of the 3D car marker (see below) |
| `DISPLAY_BACKEND=sdl` / `none` | another QEMU display instead of `cocoa,zoom-to-fit=on` |
| `python3 tools/profile.py -machine verbose=on` | log device activity (Sub-CPU link, keys, disk, display) on stderr |

`python3 tools/profile.py` is what `make boot` runs; extra arguments are passed
to QEMU. `qemu-system-sh4 -M cup2006,help` lists the machine options.

## Limitations

- **3D map car marker.** Navi draws it with the PowerVR MBX 3D core, which is
  not emulated, so the 3D view has no marker. `CUP2006_CARMARKER=1` draws an
  approximation from the guest's geometry with the emulator's own colours and
  shading. The 2D view's marker is the guest's own drawing.
- **No snapshots or reset.** The board devices have no saved state, so
  `savevm` and migration are refused; a reset powers the unit off (start it
  again).
- **Shared disk.** The guest's writes to the shared HDD image are kept in
  memory for the session only (a unit with its own `build/unit/hdd.img`
  writes to that file). The DVD drive serves ISO images read-only.
- Untested: Linux hosts, big-endian hosts, disc sets other than `EU-3350-00`.

## Layout

```
machine/            the board model: cup2006.c and one .c.inc per device,
                    compiled as one file (symlinked into QEMU's hw/sh4/)
qemu/upstream.json  the pinned QEMU release
qemu/core-patches/  small changes to QEMU's SH-4 CPU and interrupt controller
scenarios/          two 3-minute GPS drives (OpenStreetMap-based)
screenshots/        UI screenshots (plain PNGs, no metadata chunks)
scripts/            build QEMU, extract the discs, carve the firmware,
                    build and verify the HDD
tools/gps/          make GPS scenarios from OpenStreetMap
tools/profile.py    the boot command line (make boot)
tools/unit.py       a unit's persistent state
src/melco_nav_emu/  Python package (image parsers, QMP client, CLI)
tests/              offline unit tests
```

## License and trademarks

GPL-2.0-or-later, see [LICENSE](LICENSE). The built `qemu-system-sh4` contains
QEMU, so the binary is GPLv2.

The GPS scenarios in `scenarios/` are derived from OpenStreetMap data:
© OpenStreetMap contributors, available under the Open Database License
(ODbL 1.0). Their heights come from the Copernicus GLO-90 DEM.

This project is not affiliated with, authorised or endorsed by any of the
companies named here. Volvo and RTI are trademarks of Volvo Car Corporation;
Aston Martin of Aston Martin Lagonda Ltd; Mitsubishi Electric and MELCO of
Mitsubishi Electric Corporation; Renesas and SuperH of Renesas Electronics
Corporation; Microsoft, Windows and Windows CE of Microsoft Corporation;
PowerVR of Imagination Technologies; MOST of the MOST Cooperation. They are
used only to identify the hardware and software this emulator runs.
