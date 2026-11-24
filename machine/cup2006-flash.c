/*
 * Volvo CU-P2006 board: cup2006-flash device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup2006-internal.h"


/*
 * bkram=<file>: battery-backed backup RAM at physical
 * 0x19400000, 1 MiB. STUB: modelled as plain RAM whose contents are kept
 * in <file> across runs (the backup battery).
 */
#define CUP2006_BKRAM_BASE 0x19400000
#define CUP2006_BKRAM_SIZE 0x00100000

void cup2006_bkram_init(Cup2006MachineState *s, MemoryRegion *sysmem)
{
    const char *path = s->bkram_file;
    struct stat st;
    size_t n = 0;

    if (!path || !path[0]) {
        return;
    }
    melco_nvram_init(&s->bkram, OBJECT(s), "cup2006.bkram", path,
                     CUP2006_BKRAM_SIZE, 0x00);
    memory_region_add_subregion_overlap(sysmem, CUP2006_BKRAM_BASE,
                                        &s->bkram, 2);
    if (stat(path, &st) == 0) {
        n = st.st_size;
    }
    trace_cup2006_flash_backup_ram_mib_bytes(CUP2006_BKRAM_BASE, path, n);
}

/*
 * sramsave=<file>: keep the battery-backed SRAM (top 64 KiB of DRAM,
 * physical 0x0FFF0000, see CUP2006_SRAM_SIZE) across runs: a 64 KiB
 * NVRAM region mapped over that part of RAM.
 * STUB: the file stands in for the backup battery.
 */
void cup2006_sramsave_init(Cup2006MachineState *s, MemoryRegion *sysmem)
{
    const char *path = s->sramsave;
    hwaddr base = memory_region_size(&s->ram) - CUP2006_SRAM_SIZE;
    struct stat st;

    melco_nvram_init(&s->sramsave_mr, OBJECT(s), "cup2006.sramsave",
                     (path && *path) ? path : NULL, CUP2006_SRAM_SIZE, 0x00);
    memory_region_add_subregion_overlap(sysmem, base, &s->sramsave_mr, 1);
    if (path && *path) {
        size_t n = (stat(path, &st) == 0) ? st.st_size : 0;
        trace_cup2006_flash_battery_sram_prix64_kib(base, path, n);
    }
}
