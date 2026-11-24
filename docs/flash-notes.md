# Flash notes

Geometry and command sets of the NOR flash arrays on both boards, as the
models implement them (`machine/melco-amdflash-core.c` for the command state
machine, `machine/cup2006-flash.c` / `machine/cup28-flash.c` for the window
and its backing file, and the `drf_*` setup in `machine/cup2006.c` /
`machine/cup28.c`).

## What the models do

- A command write is recognised when every non-zero byte lane of the access
  holds the command byte and at least `interleave` lanes carry it, so the
  32-bit interleaved pair (interleave 4) and the 16-bit bus (interleave 2)
  both work.
- Program cycles can only clear bits: a programmed byte becomes old & new.
  Only an erase sets bits back to 1.
- The unlock sequence is AA at x555, 55 at x2AA, then the command. The
  program command is A0 at the unlock address followed by the data at the
  target address; both may be the same address.
- Unlock bypass (AA 55 20) makes a bare A0 + data program without the unlock
  sequence; 90 then 00 leaves it.
- Autoselect (AA 55 90) serves the manufacturer ID at bank+0 and the device ID
  at bank+2 as reads; the array itself is not modified, so the backing file
  never gains ID bytes.
- Sector erase (AA 55 80 AA 55 30) and chip erase (80 AA 55 10) complete
  immediately (STUB) and leave read mode, so the next command sequence starts
  clean.
- F0 resets to read mode from anywhere.
- Writes are batched into the backing file: the device saves 200 ms of
  activity and flushes the rest when the VM stops for shutdown.

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
- Commands the guest issues (trace boot, 5 min): AA 55 90 (autoselect), AA 55
  80 AA 55 30 (sector erase, 30 written at sector bases e.g.
  0x1d00000/0x1d40000/0x1d80000), F0 (reset). No A0 program, 0x20 bypass or
  0x10 chip erase in that window.

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
- Commands the guest issues (trace boot): AA 55 90 (autoselect; the model
  answers with manufacturer 0x0001 at bank+0 and device 0x227e at bank+2,
  16-bit words), F0 (reset/exit), AA 55 80 AA 55 30 (sector erase), AA 55 20
  (unlock bypass) followed by bare A0 + data program bursts — the A0 command
  and its data word are written to the same address. No 0x10 chip erase was
  observed.

Both boards' command sets fit the `melco-amdflash` properties: size,
sector-length, interleave (4 / 2), id-manufacturer/id-device (autoselect),
drive. STUB: erases and programs complete in one write; the guest sees no
erase/program latency, ready/busy polling or sector protection.
