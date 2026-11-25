# mbxemu — vendored PowerVR MBX1 model

`core.c`, `ta.c`, `vgp.c`, `render.c`, `texture.c`, `twod.c`, `pb.c`,
`rcptab.c`, `emu.h` and `mbxemu.h` are taken from **mbxemu**, the functional
PowerVR MBX1 model in the Nokia N800/N810 revival project:

- <https://github.com/thehumanweb/revive-n8x0>, directory `mbx/`
- Copyright (c) 2026 Carsten Munk and the revive-n8x0 contributors
- Licence: MIT (SPDX-License-Identifier: MIT in every file above)

`ta.c`, `render.c` and `core.c` carry marked local changes; everything else is
upstream byte for byte. Run `diff` against the upstream tree to see them.

## Local changes, and why

mbxemu's grammar is the N810 DDK's. The CU-P2006 firmware is a different
client of the same core, and four of its choices stop the model dead. Each
change below is marked `LOCAL (melco-nav-emu)` at the site, and the measurement
behind it is in `../../docs/mbx3d.md`.

| file | change | evidence |
| --- | --- | --- |
| `ta.c` | a VGP section block is the `0x8...` header and its flags word; the code load is a block of its own and may come earlier | this firmware loads code 0..6 with a `0x9` word, then binds section 0 to it; a constants block follows the flags word |
| `ta.c` | `hdr_words[7]` is 6, not 4 | the viewport block is `(0, 1, 0, 1, 0, 1)` and the next word is another header |
| `ta.c` | the sections word's high nibble is the client's marker: only `0xf`/`0xb` are held to the bit-17 rule | the firmware writes `0x00000001`; both N810 shapes behave as before |
| `render.c`, `core.c` | `INT_STATUS` bit 0 is not raised on a render completion or a context store | its meaning is unknown (upstream assumption 32); this driver enables it and can only ack by writing 0 to `+0x134`, which livelocks the ISR |

The first two N810 shapes are unchanged in every case: the code word is still
taken when it is there, and bit 17 still hangs the TA on `0xf`/`0xb` words
without it.

`machine/cup2006-mbx3d.c` is this project's own glue and is
GPL-2.0-or-later, like the rest of `machine/`. It also splits the board's
8 MiB TA window at `+0x400000` into the input FIFO and the control port,
which is the board's decode and not the core's.

`UPSTREAM-README.md` is that project's `mbx/README.md`, kept verbatim because
it is the model's specification: it lists, with evidence, every place where the
hardware's behaviour is assumed, fitted or observed. Read it before treating a
rendering difference as a bug in this board.

`machine/cup2006-mbx3d.c` is this project's own glue and is
GPL-2.0-or-later, like the rest of `machine/`.

## Why it is here

The CU-P2006's `cup2006-mbx.c` models only the register window, because the
3D core was not emulated and the map's car marker therefore had none. The
register offsets that stub latches are the MBX ones: `0x12C` INT_STATUS,
`0x130` INT_ENABLE, `0x134` INT_CLEAR, `0x680` render kick, `0x800` TA start,
`0x81C` TA context reset, `0x828`/`0x82C` free-list first/last page and `0x838`
free-list build. mbxemu decodes the same window, so the stub's offsets are the
model's, and the model supplies what the stub could not: the TA stream parser,
the VGP interpreter, the tile rasteriser, depth and alpha tests, texturing,
blending and the external Z buffer.

Three of the stub's readings are corrected by the model rather than merely
extended, and `machine/cup2006-mbx3d.c` documents them where it takes over:
`+0x12C` bit 6 is the parameter-memory direct-allocate ("EVM") event, bit 8 is
a TA-context event, and `+0x828`/`+0x82C` are the free list's first and last
page rather than a surface extent.

## What it changes and does not

`-M cup2006,mbx3d=on` selects it; without the option the board keeps the latch
window and the host-drawn marker, so nothing that is tested today changes. The
model completes every operation inside the access that starts it and has no
vmstate — `savevm` refuses this board either way — so it adds no timing
behaviour and no migration state of its own.

## Licence compatibility

MIT is compatible with this project's GPL-2.0-or-later: the combined work is
distributed under the GPL, and the MIT notices above are preserved. The
upstream project states the same split for its own GPL-covered parts
(`mbx/kmod`, kernel and QEMU patches), none of which are vendored here.
