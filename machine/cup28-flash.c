/*
 * Volvo VNS CUP28 board: backup SRAM and the flash alias window.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup28-internal.h"

void cup28_bksram_init(Cup28MachineState *s)
{
    const char *name = "cup28-backup-sram";
    const char *path = s->sramsave;
    const size_t size = MiB;
    struct stat st;

    melco_nvram_init(&s->p28_bksram, OBJECT(s), name,
                     (path && *path) ? path : NULL, size, 0xff);
    if (path && *path && stat(path, &st) == 0) {
        trace_cup28_flash_backup_sram(st.st_size ? "restored from" : "initialized in", path);
    }
}

/*
 * CUP28: the flash part's A25 is not wired, so physical 0x02000000-
 * 0x03ffffff aliases 0x00000000-0x01ffffff.  The M-loader programs the
 * SHx through the P2 window while the XIP code later fetches at
 * VA 0x80080000+ (phys 0x00080000+). Forward to the flash device with
 * bit 25 masked off.
 */
static uint64_t cup28_drf_alias_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;

    return melco_amdflash_read(s->drf, addr & (CUP28_FLASH_SIZE - 1), size);
}

static void cup28_drf_alias_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    Cup28MachineState *s = opaque;

    melco_amdflash_write(s->drf, addr & (CUP28_FLASH_SIZE - 1), value, size);
}

const MemoryRegionOps cup28_drf_alias_ops = {
    .read = cup28_drf_alias_read,
    .write = cup28_drf_alias_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
