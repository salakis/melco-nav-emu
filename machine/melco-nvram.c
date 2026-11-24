/*
 * Shared battery-backed memory for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "melco-nvram.h"

void melco_nvram_init(MemoryRegion *mr, Object *owner, const char *name,
                      const char *path, uint64_t size, uint8_t fill)
{
    if (path && *path) {
#ifdef CONFIG_POSIX
        struct stat st;
        int fd = open(path, O_RDWR | O_CREAT, 0600);

        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0 ||
            fstat(fd, &st) < 0) {
            error_report("%s: cannot open '%s': %s",
                         name, path, strerror(errno));
            exit(1);
        }
        if (st.st_size != 0 && (uint64_t)st.st_size != size) {
            error_report("%s: '%s' must be %" PRIu64 " bytes",
                         name, path, size);
            exit(1);
        }
        /* STUB: a shared file stands in for the backup battery. */
        memory_region_init_ram_from_fd(mr, owner, name, size,
                                       RAM_SHARED, fd, 0, &error_fatal);
        if (!st.st_size) {
            memset(memory_region_get_ram_ptr(mr), fill, size);
        }
        return;
#else
        error_report("%s: file-backed NVRAM requires a POSIX host", name);
        exit(1);
#endif
    }

    memory_region_init_ram(mr, owner, name, size, &error_fatal);
    memset(memory_region_get_ram_ptr(mr), fill, size);
}
