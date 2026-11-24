/*
 * Shared AMD NOR flash device for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef MELCO_AMDFLASH_H
#define MELCO_AMDFLASH_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/block-backend.h"

#define TYPE_MELCO_AMDFLASH "melco-amdflash"

typedef struct MelcoAmdFlash MelcoAmdFlash;

DECLARE_INSTANCE_CHECKER(MelcoAmdFlash, MELCO_AMDFLASH,
                         TYPE_MELCO_AMDFLASH)

/*
 * Create and realize one flash device.  The caller maps the returned
 * MMIO region itself (see melco_amdflash_mmio) so the mapping priority
 * stays the machine's choice.  @blk is the optional backing file.
 */
MelcoAmdFlash *melco_amdflash_create(uint64_t size, uint32_t sector_len,
                                     uint8_t interleave,
                                     uint16_t id_manufacturer,
                                     uint16_t id_device,
                                     BlockBackend *blk);

MemoryRegion *melco_amdflash_mmio(MelcoAmdFlash *s);

/* The flash array, e.g. to load a kernel image into it before reset. */
uint8_t *melco_amdflash_array(MelcoAmdFlash *s);

/* Guest time (ms) of the last program/erase, for power-cut logic. */
int64_t melco_amdflash_activity_ms(MelcoAmdFlash *s);

/* Read/write through the device, e.g. for an alias window. */
uint64_t melco_amdflash_read(MelcoAmdFlash *s, hwaddr addr, unsigned size);
void melco_amdflash_write(MelcoAmdFlash *s, hwaddr addr, uint64_t value,
                          unsigned size);

#endif /* MELCO_AMDFLASH_H */
