# The 3D core: `CUP2006_MBX3D=1`

The CU-P2006's 3D map view is drawn by a PowerVR MBX on the board's own bus.
`machine/cup2006-mbx.c` models that core's register window only, because until
now there was nothing to run behind it: writes latch and read back, and the
frame-completion bits the guest's driver polls are raised by the display
model. The 3D view therefore draws its map but not the car marker, and
`CUP2006_CARMARKER=1` draws an approximation on the host.

`CUP2006_MBX3D=1` puts the vendored MBX1 model (`machine/mbx/`) behind the same
window. It is experimental: the model decodes this firmware's TA stream and
rasterises the scene, but the guest still stops after the first map
composition, so the marker is not drawn yet (measurements below). The switch
stays off by default.

## What runs

The model is **mbxemu** from the Nokia N800/N810 revival project
(<https://github.com/thehumanweb/revive-n8x0>, directory `mbx/`), MIT,
vendored with four marked local changes and with its own README as the
specification of what it assumes. It decodes:

- the TA stream: object headers, vertex uploads, primitive assembly, clipping,
  projection, culling, the region array and the parameter buffer's free list;
- the VGP interpreter that runs the guest's vertex programs;
- the 3D core: tile walk, depth (on chip, or loaded from and stored to the
  external Z buffer), rasteriser, depth and alpha tests, texturing with the
  word-C combiner and DOT3, fog, blending;
- the 2D core;
- the MMU between the core and memory, so the buffer addresses the guest
  programs are translated, not used as physical addresses.

`machine/cup2006-mbx3d.c` is this project's glue — memory, the interrupt line
and the log — and is GPL-2.0-or-later like the rest of `machine/`.

## What it corrected in the latch model

The window is identical, so the vendored model decoded the same offsets the
latch model had guessed at. Four of those guesses were wrong, and the glue
follows the model:

| offset | latch model | the core |
| --- | --- | --- |
| `+0x12C` bit 6 | "parameter memory ready" | `I_EVM_DALLOC`, the direct-allocate completion |
| `+0x12C` bit 8 | "TA kick", polled before each `+0x81C` | `I_TA_CONTEXT`, a TA-context event |
| `+0x828` / `+0x82C` | "surface first/last page" | the parameter buffer free list's first and last page |
| `+0x838` `= 1` | "one descriptor armed", completed instantly | the free-list build |

`+0x12C` is also a doorbell on a write (bits are set, not latched), and its
bits 16-23 are the input FIFO's fill. The line is level-sensitive: up while
`+0x130 & 0x8000` and `+0x12C & +0x130`.

## Watching it

```
CUP2006_MBX3D=1 CUP2006_MBX3D_TRACE=1 make boot
CUP2006_MBX3D=1 .venv/bin/python -m melco_nav_emu.cli boot -- \
  -trace 'enable=cup2006_mbx3d_*,file=/tmp/mbx.log'
```

`CUP2006_MBX3D_TRACE=1` is the model's own log (registers, and every TA word);
`-trace` is QEMU's, one pattern per option. Several patterns need a file with
one per line: `-trace events=/tmp/events,file=/tmp/mbx.log`.

`cup2006_mbx3d_stats` prints the model's own counters on every interrupt:
scenes started, objects, triangles kept, renders, tiles, and the model's
stream errors and contract violations. A guest that has brought the core up
and is rendering the 3D view shows `scenes` and `renders` climbing; a guest
that only initialises device memory shows `scenes=0` and a `+0x12C` value of
`0x40` (the EVM alloc) with `+0x130` at `0xffffffff`.

## What it does not do

- **No timing.** Every operation completes inside the access that starts it,
  which is what the latch model did too, so guest-visible ordering is
  unchanged; it is not a cycle model.
- **No migration state.** `savevm` refuses this board with or without the
  model, so nothing is lost.
- **Unknown fidelity.** The model was fitted to an N810's MBX1 by observation.
  Where the CU-P2006's slice is configured differently — the translucency
  autosort unit, the VGP's register file 3, line snapping — the model says so
  in `machine/mbx/UPSTREAM-README.md` and in `machine/mbx/emu.h`, and those
  are the first places to look if a scene comes out wrong.
- **Not yet usable.** The model parses and rasterises the scenes this
  firmware sends, but the guest stops after the first map composition, so
  nothing the model draws reaches the panel (measured, below).
- **The setup contract is violated throughout.** This driver does not set up
  the core the way the N810's does, so every scene logs the same six
  violations: `+0x83C` is `0x00C00000` rather than 0, `+0x6D8`/`+0xCE0` are
  never written, `+0x820`/`+0x864` (the TA and EVM contexts) are never set,
  `+0x85C`'s tile grid disagrees with `+0x84C`/`+0x850`, the free-list table
  does not chain, and the region array is not initialised. In strict mode
  (the default) the model gives those the consequences they had on an N810 —
  the scene may come out garbled. Whether the SH7785's slice is configured
  the same way is unknown; `mbxemu_set_strict(e, 0)` in
  `cup2006_mbx3d_init()` is the knob, and it changes nothing the guest can
  see until the 3D view is on screen (measured: the disclaimer frame is
  pixel-identical either way).

## The wedge at the disclaimer, and what fixed it (measured)

Symptom, from the first reported run: ENTER at the disclaimer appeared to do
nothing, and ESC on that screen gave a black panel. Two arms from
byte-identical copies of one unit directory, same disc set, same launch, the
only difference being `mbx3d`:

| arm | before ENTER | +3 s | +18 s | +43 s |
| --- | --- | --- | --- | --- |
| `mbx3d=on`, before the fixes | 6293 B | 6293 B | 6293 B | 6293 B |
| `mbx3d=off` | 6293 B | 6293 B | 2805 B | 2805 B |

With the model on, accepting the disclaimer never took effect: the panel stayed
on the disclaimer frame while the guest ran at ~75 % of a core with its PC
alternating between the navi application and the kernel — busy, not blocked.
The counters said why: `scenes=1 objects=0 prims=0 renders=0 stream_errors=1
violations=6`, i.e. the first TA scene this firmware streams did not parse.

Five defects, in the order the stream exposed them. The first is the board's,
the rest are places where mbxemu's grammar is the N810 DDK's and this
firmware's differs; each is marked `LOCAL (melco-nav-emu)` in the source.

1. **The TA window is two ports, not one** (`cup2006-mbx3d.c`). The 8 MiB at
   `0xFD800000` is split at `+0x400000`: the first half is the input FIFO, the
   second the TA control port, where the firmware writes 1 to end an object and
   0 to restart a strip — nothing else ever lands there. The glue fed those
   control words into the stream as data.
2. **A VGP section block is two words** (`machine/mbx/ta.c`). The `0x8...`
   header and its flags word end the block; the code load is a block of its
   own and can come earlier. This firmware loads code 0..6 with a `0x9` word,
   then binds section 0 to it. Upstream demands a `0x9` right there and stops
   the stream otherwise.
3. **The object header's bit-7 block is six words, not four** (`ta.c`,
   `hdr_words[]`). The scene writes `(0, 1, 0, 1, 0, 1)` — a normalised
   viewport — and the next word is another header.
4. **The sections word is `0x00000001`** (`ta.c`). Its high nibble is the
   client's marker, not the chip's: the N810 writes `0xF0020000 | mask` (or
   `0xB00B0800` for screen space) and sets bit 17, this firmware writes `1`
   with neither. Both N810 shapes still behave exactly as before.
5. **`INT_STATUS` bit 0 livelocks the guest** (`render.c`, `core.c`). Its
   meaning is unknown (upstream assumption 32) and it is raised on a render
   completion and on a context store. This driver enables it and acks it by
   writing 0 to `+0x134`, which clears nothing, so the handler returns with
   the line still high: the guest spun at `0x80105568` forever, re-enabling
   and dropping the line. The two raises no longer set it.

After those, the same scene reads `scenes=1 objects=2 prims=8 renders=1
tiles=5076 stream_errors=0`, and the better test is the guest's own progress:
pressing ENTER now lands on the *same* frame as with the model off — 0 of
93,600 pixels differ. `mbx3d` no longer changes what the guest does, only who
draws.

What the first scene actually is, decoded and self-checking: code 0..6 as 14
words, section 0 + flags `0x00000600`, a six-word viewport, a header
`0x10000067` whose blocks are the state, the address, 3 texture words, the
vertex layout `0x00205102` and `0`, then primitive `0x70200000`, the sections
word `0x00000001`, 3 vertices of 6 words, and a control word. The arithmetic
that confirms it: 1 + 3 x 6 = 19 words lands exactly on the object-end control
write, and `vertex_words(0x00205102)` = 6.

## It still freezes at the first map composition (measured)

Parsing the stream is not enough. Two fresh unit directories, same disc, the
only difference being `mbx3d`, one ENTER each, then three arrow keys:

| arm | after ENTER | keys change | model counters |
| --- | --- | --- | --- |
| `mbx3d=off` | map UI, 124 colours | 77.7 % of the frame | — |
| `mbx3d=on` | stale first composite, 520 colours | **0 %** | `scenes=2 renders=2 stream_errors=0 violations=22` |

The two frames differ in 80 % of their pixels, and the model-on one is the
*earlier* compose: the panel never updates again. The guest is not blocked —
QEMU sits at a full core with the PC cycling through the navi application and
the kernel — it is spinning on state the model never produces.

That points past the stream parser and at the ISP contract, which is where the
violations are: this driver never sets `+0x820`/`+0x864` (the TA and EVM
contexts), puts the parameter buffer at `0x00C00000` rather than 0, leaves the
region array uninitialised and never rebuilds the free list, so six clauses of
the model's contract fail on every scene and `render.c` gives them the chip's
consequences (white tiles, NaN depth, the partial-render hang). The guest's own
bookkeeping — region entry bit 30, the tile lists it reads back — then never
matches what it expects.

`mbxemu_set_strict(e, 0)` in `cup2006_mbx3d_init()` drops those consequences
without dropping the counters; measured, it does not change this freeze either
way. So the switch stays off by default, and none of this affects the tested
paths. What is needed next is the tile-grid and free-list semantics this driver
actually uses — `+0x85C`, `+0x84C`/`+0x850`, `+0x860` and the region entry
format — which is a decoding job against the stream dumps, not a model rewrite.

