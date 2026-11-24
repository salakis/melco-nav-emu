# Flash notes (Spike S3)

Geometry and command sets of the NOR flash arrays on both boards, recorded
from the models (`machine/cup2006-flash.c.inc`, `machine/cup28-flash.c.inc`,
the `drf_*` setup in `machine/cup2006.c` / `machine/cup28.c`) and from trace
boots with `-trace memory_region_ops_write` filtered to the flash region
names.

## cup2006 (CU-P2006, "DRF1:")

- Base 0x00000000, size 0x02000000 (32 MiB), one part, mapped ROMD as region
  `cup2006-drf-flash` (CPU reads served from the array, every write reaches
  the command machine). The `-kernel` image is loaded into the array before
  reset; `drfimage=<file>` is a raw 32 MiB save file.
- Sector size 0x00040000 (256 KiB).
- Interleave 4: a 32-bit bus where every byte lane carries the command byte
  (unlock writes arrive as 0xaaaaaaaa / 0x55555555, commands as 0x90909090
  etc.). Unlock addresses are the usual x555/x2aa pattern per lane
  (0x...1554, 0x...2aa8).
- Commands the guest actually issues (trace boot, 5 min): AA 55 90
  (autoselect), AA 55 80 AA 55 30 (sector erase, 30 written at sector bases
  e.g. 0x1d00000/0x1d40000/0x1d80000), F0 (reset). No A0 program, 0x20 bypass
  or 0x10 chip erase was observed in that window.
- Autoselect is issued but not modelled today; the array is not modified.

## cup28 (VNS CUP28)

- Base 0x00000000, size 32 MiB, region `cup28-flash`; because the part's A25
  is not wired, physical 0x02000000-0x03ffffff aliases 0x00000000-0x01ffffff
  through region `cup28-flash-alias` (addr & (size - 1)). Separately,
  flr.dll's 256 KiB FLR window sits at physical 0x01a00000 (blank RAM).
  The firmware image is loaded at array offset 0x80000 before reset;
  `drfimage=<file>` is a raw 32 MiB save file.
- Sector size 256 KiB (0x40000).
- Interleave 2: a 16-bit bus; a command is recognised when every non-zero
  lane holds the command byte and lane 0 does (e.g. 0x00aa00aa for AA).
- Commands the guest actually issues (trace boot): AA 55 90 (autoselect; the
  model answers with manufacturer 0x0001 at bank+0 and device 0x227e at
  bank+2, 16-bit words), F0 (reset/exit), AA 55 80 AA 55 30 (sector erase),
  AA 55 20 (unlock bypass) followed by bare A0 + data program bursts — the
  A0 command and its data word are written to the same address. No 0x10
  chip erase was observed.

Both boards' command sets fit the `melco-amdflash` properties: size,
sector-length, interleave (4 / 2), id-manufacturer/id-device (autoselect),
drive. Chip erase (80 AA 55 10) is supported by the device although neither
guest issued it in the traced boots.
