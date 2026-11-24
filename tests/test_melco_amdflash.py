# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Unit test for the QEMU-free melco-amdflash core state machine.

Compiles machine/melco-amdflash-core.c with cc -std=c11 together with a
test main (same pattern as test_key_transport.py) and checks the AMD
command sequences both boards' guests issue.
"""

import subprocess
from pathlib import Path

MAIN = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "melco-amdflash-core.h"

#define SZ (64 * 1024)
#define SECTOR (4 * 1024)

static uint8_t arr[SZ];
static MelcoAmdFlashCore c;
static uint64_t off, len;
static MelcoAmdFlashOp op;

static bool wr(uint64_t addr, uint64_t value, unsigned size)
{
    return melco_amdflash_core_write(&c, addr, value, size, &off, &len, &op);
}

static void unlock55(void)
{
    wr(0x2aa8, 0xaaaaaaaa, 4);   /* AA */
    wr(0x1554, 0x55555555, 4);   /* 55 */
}

int main(void)
{
    /* interleave 4: every lane must carry the command byte */
    melco_amdflash_core_init(&c, arr, SZ, SECTOR, 4, 0x0001, 0x227e);
    memset(arr, 0xff, SZ);

    /* a lone 0x30 without unlock does nothing */
    assert(!wr(0x1000, 0x30303030, 4));
    assert(arr[0x1000] == 0xff && op == MELCO_AMDFLASH_OP_NONE);

    /* unlock + program ANDs the data into the array (A0 itself changes
     * nothing, so it returns false but arms the address) */
    unlock55();
    assert(!wr(0x1000, 0xa0a0a0a0, 4));     /* A0 arms the address */
    assert(c.program_pending && c.program_addr == 0x1000);
    arr[0x1000] = 0xf0;
    assert(wr(0x1000, 0x3c3c3c3c, 4));      /* data: old & new */
    assert(op == MELCO_AMDFLASH_OP_PROGRAM && off == 0x1000 && len == 4);
    assert(arr[0x1000] == (0xf0 & 0x3c));

    /* sector erase after the full 80 AA 55 sequence fills 0xff */
    arr[0x2000] = 0x00;
    unlock55();
    wr(0x2aa8, 0x80808080, 4);              /* 80 */
    wr(0x2aa8, 0xaaaaaaaa, 4);              /* AA */
    wr(0x1554, 0x55555555, 4);              /* 55: armed */
    assert(wr(0x2123, 0x30303030, 4));      /* 30 erases the sector */
    assert(op == MELCO_AMDFLASH_OP_SECTOR_ERASE);
    assert(off == 0x2000 && len == SECTOR);
    assert(arr[0x2000] == 0xff && arr[0x2000 + SECTOR - 1] == 0xff);
    assert(arr[0x1fff] == 0xff);            /* untouched neighbour */

    /* chip erase: 80 AA 55 10 (F0 first, as the guests do) */
    arr[0x100] = 0x00;
    wr(0x0, 0xf0f0f0f0, 4);
    unlock55();
    wr(0x2aa8, 0x80808080, 4);
    wr(0x2aa8, 0xaaaaaaaa, 4);
    wr(0x1554, 0x55555555, 4);
    assert(wr(0x0, 0x10101010, 4));         /* 10: chip erase */
    assert(op == MELCO_AMDFLASH_OP_CHIP_ERASE && off == 0 && len == SZ);
    assert(arr[0x100] == 0xff);

    /* autoselect: 90 serves the IDs at the bank base */
    wr(0x0, 0xf0f0f0f0, 4);
    unlock55();
    wr(0x2aa8, 0x90909090, 4);              /* 90: autoselect */
    assert(c.autoselect);
    assert(melco_amdflash_core_read(&c, 0x0, 2) == 0x0001);   /* manufacturer */
    assert(melco_amdflash_core_read(&c, 0x2, 2) == 0x227e);   /* device */
    /* F0 leaves autoselect; reads serve the array again */
    wr(0x0, 0xf0f0f0f0, 4);
    assert(!c.autoselect);
    assert(melco_amdflash_core_read(&c, 0x0, 2) == 0xffff);

    /* interleave 2: commands with the byte in two lanes (0x00aa00aa) */
    melco_amdflash_core_init(&c, arr, SZ, SECTOR, 2, 0x0001, 0x227e);
    memset(arr, 0xff, SZ);
    assert(wr(0x2aa8, 0x00aa00aa, 4) == false);  /* AA: no array change */
    assert(c.state == MELCO_AMDFLASH_ST_U1);
    wr(0x1554, 0x00550055, 4);
    assert(c.state == MELCO_AMDFLASH_ST_U2);
    assert(!wr(0x1000, 0x00a000a0, 4));     /* A0 */
    assert(c.program_pending && c.program_addr == 0x1000);
    assert(wr(0x1000, 0x0000000f, 4));      /* data write, one lane */
    assert(arr[0x1000] == 0x0f);
    /* a single-lane 0xaa is not a command at interleave 2 */
    melco_amdflash_core_reset(&c);
    wr(0x2aa8, 0x000000aa, 4);
    assert(c.state == MELCO_AMDFLASH_ST_READ);

    printf("melco-amdflash-core: all checks passed\n");
    return 0;
}
'''


def test_amdflash_core_state_machine(tmp_path):
    machine = Path(__file__).parents[1] / "machine"
    core_c = (machine / "melco-amdflash-core.c").read_text()
    core_h = (machine / "melco-amdflash-core.h").read_text()
    src = tmp_path / "amdflash_core_test.c"
    src.write_text(
        '#include "melco-amdflash-core.h"\n' + MAIN + "\n" + core_c
    )
    inc = tmp_path / "melco-amdflash-core.h"
    inc.write_text(core_h)
    binary = tmp_path / "amdflash_core_test"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Werror", "-I", str(tmp_path),
         str(src), "-o", str(binary)],
        check=True, capture_output=True, text=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
