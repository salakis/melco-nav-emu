# images/ — your own disc dumps (never committed)

Everything here is local only: `images/*` is gitignored except this file.
Do not commit firmware, ISOs or HDD images.

## Expected layout

```
images/
  dvd/disc1/EU-3350-00_1.ISO   # your dump of retail disc 1 (EU-3350-00)
  dvd/disc2/EU-3350-00_2.ISO   # disc 2
  dvd1/, dvd2/                 # extracted DVD root files   (make extract)
  firmware/                    # carved modules, incl. the NK .nb0 (make flatten)
  hdd-manifest.json            # parsed P2006.SCE install manifest (make hdd)
```

`make extract` reads the ISOs directly (macOS: prefers an already-mounted
volume or `hdiutil`; Linux: `7z`) and prints a SHA-1 for every extracted
root file — record them in a local note if you want to verify your dump.
For another disc set (e.g. NA-2450-00) export `CUP2006_DISC1`/`CUP2006_DISC2`
pointing at your ISOs first — the same variables `make boot` reads, and the
extracted roots always land in `dvd1/`, `dvd2/` regardless of disc set.

## Where the run artifacts go

Everything the emulator generates lives under `build/` (also gitignored):

```
build/
  hdd/        the virtual HDD (FAT32, two partitions, map on the second);
              hdd-empty.img stands in while the discs are not extracted
  unit/       the unit's persistent state: flash, battery RAM/SRAM, EEPROM
  gps/<name>/ a GPS scenario, used with CUP2006_SCENARIO=<name>:
              drive.replay (radio recording) + sensors.csv
```

## Provenance

| artifact | source |
|---|---|
| `dvd1/*` | root of `EU-3350-00_1.ISO` |
| `dvd2/*` | root of `EU-3350-00_2.ISO` |
| `firmware/*` | `LOADING.KWI` module table (B000FF containers) |
| `hdd-manifest.json` | `P2006.SCE` install script |
| `build/hdd/*.img` | the discs' own `MAP/` volumes, assembled and checksum-checked |
