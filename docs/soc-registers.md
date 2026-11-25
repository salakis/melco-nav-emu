# Unmodelled SH7785 registers

`machine/cup2006-socregs.h` is the SH7785 hardware manual's own register list
(REJ09B0261-0100 section 31): 610 entries of address, access width, power-on
reset value and name, transcribed by
<https://github.com/kmehltretter82/qemu-superh> (`hw/sh4/sh7785_regs.h`,
GPL-2.0-or-later).

`cup2006_mmio_read()` / `cup2006_mmio_write()` consult it whenever the guest
touches an address no device on the board claims. Two things follow:

1. The access is reported by name instead of by address, and a read returns
   the manual's reset value rather than 0.
2. The list is a work queue. A register in it is one the board does not model;
   if `-trace 'cup2006_soc_unmodelled_*'` shows the guest reading and writing
   it, the guest is relying on behaviour that is missing.

```
make boot -- -trace 'cup2006_soc_unmodelled_*'
CUP2006_MBX3D=1 .venv/bin/python -m melco_nav_emu.cli boot -- \
  -trace 'cup2006_soc_unmodelled_*'
```

## What it found on the tested set

One boot to the first frames, Europe 2012.1 (`EU-2250-01`), CU-P2006 profile:

| register | address | access | from |
| --- | --- | --- | --- |
| `ICR0` | `0xffd00000` | read, write | `0xa0103922`, the OAL's interrupt setup |
| `ICR1` | `0xffd0001c` | read, write, ×3 | `0x80104db4`, `0x80104f9c`, `0x8010508e` |
| `INTMSK0` | `0xffd00044` | write | `0x801052a2` |
| `INTMSKCLR0` | `0xffd00064` | write | `0x801052aa` |

These are the SH7785's INTC interrupt-control registers: `ICR0`/`ICR1` select
the detection mode of the external pins and the mask registers gate the
INTC-2 channels. QEMU's `sh_intc` has the priority registers but not this
block, so the writes are dropped and the reads return the manual's reset value
of 0. The guest does not appear to depend on the result — it boots — but the
board's interrupt configuration is therefore not the one the firmware asks
for, and that is worth knowing before chasing an interrupt-timing bug.

This is exactly the class of defect the table is for, and none of it was
visible before: the same accesses were previously logged as bare addresses on
`-d guest_error` and returned 0 either way.
