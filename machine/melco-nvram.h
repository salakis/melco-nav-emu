/*
 * Shared battery-backed memory for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef MELCO_NVRAM_H
#define MELCO_NVRAM_H

#include "qom/object.h"
#include "system/memory.h"

/*
 * melco_nvram_init: file-mapped battery-backed memory.
 *
 * A shared file stands in for the backup battery: the region is mmap-ed
 * from @path (created and filled with @fill when new), so writes survive
 * closing the window without a save timer.  A NULL or empty @path falls
 * back to plain RAM filled with @fill.
 *
 * An existing @path must be exactly @size bytes.
 */
void melco_nvram_init(MemoryRegion *mr, Object *owner, const char *name,
                      const char *path, uint64_t size, uint8_t fill);

#endif /* MELCO_NVRAM_H */
