/*
 * CU-P2006 machine: the DRF NOR flash array, battery RAM and battery SRAM files.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * DRF flash window: a ROMD device, so CPU reads are served directly from
 * drf_data; this read callback only serves non-CPU readers (gdb/DMA) and
 * is a plain copy-out.
 */
/*
 * drfimage=<path>: load the 32 MB DRF array from the file at machine init
 * and write dirty spans back (200 ms timer + exit notifier), so programmed
 * content survives a reboot. Content persistence only.
 */
#define CUP2006_DRF_SAVE_MS 200

static void cup2006_drf_mark_dirty(Cup2006MachineState *s, hwaddr addr,
                                   hwaddr len)
{
    s->drf_activity_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    /* The CPU executes from this array (XIP): drop translated code. */
    memory_region_flush_rom_device(&s->drf_regs, addr, len);
    if (s->drf_fd < 0) {
        return;
    }
    if (addr < s->drf_dirty_lo) {
        s->drf_dirty_lo = addr;
    }
    if (addr + len > s->drf_dirty_hi) {
        s->drf_dirty_hi = addr + len;
    }
}

static void cup2006_drf_save(Cup2006MachineState *s)
{
    ssize_t n;

    if (s->drf_fd < 0) {
        return;
    }
    if (s->drf_image_full_write) {
        n = pwrite(s->drf_fd, s->drf_data, CUP2006_DRF_SIZE, 0);
        if (n != CUP2006_DRF_SIZE) {
            warn_report("cup2006: drfimage write failed (%zd)", n);
            return;
        }
        s->drf_image_full_write = false;
    } else if (s->drf_dirty_lo < s->drf_dirty_hi) {
        n = pwrite(s->drf_fd, s->drf_data + s->drf_dirty_lo,
                   s->drf_dirty_hi - s->drf_dirty_lo, s->drf_dirty_lo);
        if (n != (ssize_t)(s->drf_dirty_hi - s->drf_dirty_lo)) {
            warn_report("cup2006: drfimage write failed (%zd)", n);
            return;
        }
    }
    s->drf_dirty_lo = CUP2006_DRF_SIZE;      /* nothing dirty */
    s->drf_dirty_hi = 0;
}

/*
 * bkram=<file>: battery-backed backup RAM at physical
 * 0x19400000, 1 MiB. STUB: modelled as plain RAM whose contents are kept
 * in <file> across runs (the backup battery).
 */
#define CUP2006_BKRAM_BASE 0x19400000
#define CUP2006_BKRAM_SIZE 0x00100000

/*
 * Save a battery-backed memory to its file: skipped when unchanged since the
 * last save, written to <file>.tmp and renamed over <file> only when complete,
 * so an interrupted save never leaves a short file.
 */
static void cup2006_battery_save(const char *path, const void *data,
                                 size_t len, uint8_t **last)
{
    g_autofree char *tmp = NULL;
    FILE *f;
    bool ok;

    if (*last && !memcmp(*last, data, len)) {
        return;
    }
    tmp = g_strdup_printf("%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        warn_report("cup2006: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        warn_report("cup2006: saving %s failed: %s", path, strerror(errno));
        unlink(tmp);
        return;
    }
    if (!*last) {
        *last = g_malloc(len);
    }
    memcpy(*last, data, len);
}

static void cup2006_bkram_save(Cup2006MachineState *s)
{
    cup2006_battery_save(s->bkram_path,
                         memory_region_get_ram_ptr(&s->bkram),
                         CUP2006_BKRAM_SIZE, &s->bkram_last);
}

static void cup2006_bkram_exit(Notifier *n, void *data)
{
    Cup2006MachineState *s = container_of(n, Cup2006MachineState, bkram_exit);

    cup2006_bkram_save(s);
}

static void cup2006_bkram_timer_cb(void *opaque)
{
    Cup2006MachineState *s = opaque;

    cup2006_bkram_save(s);
    timer_mod(s->bkram_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

static void cup2006_bkram_init(Cup2006MachineState *s, MemoryRegion *sysmem)
{
    const char *path = s->bkram_file;
    FILE *f;
    size_t n = 0;

    if (!path || !path[0]) {
        return;
    }
    s->bkram_path = g_strdup(path);
    memory_region_init_ram(&s->bkram, NULL, "cup2006.bkram",
                           CUP2006_BKRAM_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, CUP2006_BKRAM_BASE,
                                        &s->bkram, 2);
    f = fopen(path, "rb");
    if (f) {
        n = fread(memory_region_get_ram_ptr(&s->bkram), 1,
                  CUP2006_BKRAM_SIZE, f);
        fclose(f);
    }
    s->bkram_exit.notify = cup2006_bkram_exit;
    qemu_add_exit_notifier(&s->bkram_exit);
    s->bkram_timer = timer_new_ms(QEMU_CLOCK_REALTIME, cup2006_bkram_timer_cb,
                                  s);
    timer_mod(s->bkram_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    cup2006_info(s, "cup2006: backup RAM 0x%08x (1 MiB) <- %s (%zu bytes loaded)",
                CUP2006_BKRAM_BASE, path, n);
}

/*
 * sramsave=<file>: keep the battery-backed SRAM (top 64 KiB of DRAM,
 * physical 0x0FFF0000, see CUP2006_SRAM_SIZE) across runs.
 * STUB: the file stands in for the backup battery.
 */
static void cup2006_sramsave_save(Cup2006MachineState *s)
{
    uint8_t *buf = g_malloc(CUP2006_SRAM_SIZE);
    hwaddr base = memory_region_size(&s->ram) - CUP2006_SRAM_SIZE;

    physical_memory_read(base, buf, CUP2006_SRAM_SIZE);
    cup2006_battery_save(s->sramsave_path, buf, CUP2006_SRAM_SIZE,
                         &s->sramsave_last);
    g_free(buf);
}

static void cup2006_sramsave_exit(Notifier *n, void *data)
{
    cup2006_sramsave_save(container_of(n, Cup2006MachineState,
                                       sramsave_exit));
}

static void cup2006_sramsave_timer_cb(void *opaque)
{
    Cup2006MachineState *s = opaque;

    cup2006_sramsave_save(s);
    timer_mod(s->sramsave_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

static void cup2006_sramsave_init(Cup2006MachineState *s)
{
    const char *path = s->sramsave;
    hwaddr base;
    gchar *data = NULL;
    gsize size = 0;

    if (!path || !path[0]) {
        return;
    }
    s->sramsave_path = g_strdup(path);
    base = memory_region_size(&s->ram) - CUP2006_SRAM_SIZE;
    if (g_file_get_contents(path, &data, &size, NULL) &&
        size == CUP2006_SRAM_SIZE) {
        physical_memory_write(base, data, size);
    } else {
        size = 0;
    }
    g_free(data);
    s->sramsave_exit.notify = cup2006_sramsave_exit;
    qemu_add_exit_notifier(&s->sramsave_exit);
    s->sramsave_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                     cup2006_sramsave_timer_cb, s);
    timer_mod(s->sramsave_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    cup2006_info(s, "cup2006: battery SRAM 0x%08" HWADDR_PRIx " (64 KiB) <- %s"
                " (%zu bytes loaded)", base, path, (size_t)size);
}

static void cup2006_drf_save_timer(void *opaque)
{
    Cup2006MachineState *s = opaque;

    cup2006_drf_save(s);
    timer_mod(s->drf_save_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CUP2006_DRF_SAVE_MS);
}

static void cup2006_drf_exit(Notifier *n, void *data)
{
    Cup2006MachineState *s = container_of(n, Cup2006MachineState, drf_exit);

    cup2006_drf_save(s);
}

static uint64_t cup2006_drf_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < CUP2006_DRF_SIZE; i++) {
        value |= (uint64_t)s->drf_data[addr + i] << (8 * i);
    }

    if (size < 4) {
        value &= (1u << (8 * size)) - 1;
    }
    return value;
}

/*
 * The AMD command sequences the guest's flash driver uses, all values
 * written to the 32-bit interleaved pair:
 *   AA 55 A0 <data>            program one word
 *   AA 55 80 AA 55 30          erase the addressed sector (more 30s queue
 *                              further sectors)
 *   AA 55 20, then A0 <data>   unlock bypass: program without unlock cycles,
 *                              90 00 leaves it
 *   F0                         back to read mode
 * STUB: the unlock cycles' addresses are not checked, program and erase
 * complete instantly, and autoselect (AA 55 90) is not modelled.
 */
enum {
    CUP2006_DRF_ST_READ,
    CUP2006_DRF_ST_U1,           /* AA seen */
    CUP2006_DRF_ST_U2,           /* AA 55 seen: a command follows */
    CUP2006_DRF_ST_ERASE,        /* AA 55 80 */
    CUP2006_DRF_ST_ERASE_U1,     /* AA 55 80 AA */
    CUP2006_DRF_ST_ERASE_U2,     /* AA 55 80 AA 55: 30 erases a sector */
    CUP2006_DRF_ST_BYPASS_EXIT,  /* 90 in unlock bypass: 00 leaves it */
};

static void cup2006_drf_erase_sector(Cup2006MachineState *s, hwaddr addr)
{
    hwaddr sector = addr & ~(hwaddr)(CUP2006_DRF_SECTOR - 1);
    hwaddr len = MIN((hwaddr)CUP2006_DRF_SECTOR, CUP2006_DRF_SIZE - sector);

    s->drf_erases++;
    memset(s->drf_data + sector, 0xff, len);
    cup2006_drf_mark_dirty(s, sector, len);
    qemu_log_mask(LOG_GUEST_ERROR, "cup2006: drf erase sector 0x%08"
                  HWADDR_PRIx " count=%u\n", (hwaddr)(CUP2006_DRF_BASE +
                  sector), s->drf_erases);
}

static void cup2006_drf_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint32_t v = (uint32_t)value;
    unsigned st = s->drf_cmd_state;
    unsigned i;

    if (addr >= CUP2006_DRF_SIZE) {
        return;
    }
    /* An armed program cycle takes the next write to its address as data,
     * whatever its value. */
    if (s->drf_program_pending && addr == s->drf_program_addr) {
        s->drf_program_pending = false;
        for (i = 0; i < size && addr + i < CUP2006_DRF_SIZE; i++) {
            s->drf_data[addr + i] = (v >> (8 * i)) & 0xff;
        }
        cup2006_drf_mark_dirty(s, addr, size);
        return;
    }
    s->drf_cmd_state = CUP2006_DRF_ST_READ;
    if (v == CUP2006_DRF_RESET) {
        s->drf_program_pending = false;
        s->drf_bypass = false;
        return;
    }
    switch (st) {
    case CUP2006_DRF_ST_READ:
        if (v == CUP2006_DRF_UNLOCK1) {
            s->drf_cmd_state = CUP2006_DRF_ST_U1;
        } else if (s->drf_bypass && v == CUP2006_DRF_PROGRAM) {
            s->drf_program_pending = true;
            s->drf_program_addr = addr;
        } else if (s->drf_bypass && v == CUP2006_DRF_AUTOSEL) {
            s->drf_cmd_state = CUP2006_DRF_ST_BYPASS_EXIT;
        } else if (v == CUP2006_DRF_ERASE_SUSPEND || v == CUP2006_DRF_ERASE_CMD) {
            /* erase suspend/resume: erases complete instantly */
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "cup2006: drf write 0x%08"
                          HWADDR_PRIx " value=0x%08" PRIx32
                          " outside a command sequence ignored\n",
                          (hwaddr)(CUP2006_DRF_BASE + addr), v);
        }
        break;
    case CUP2006_DRF_ST_U1:
        if (v == CUP2006_DRF_UNLOCK2) {
            s->drf_cmd_state = CUP2006_DRF_ST_U2;
        }
        break;
    case CUP2006_DRF_ST_U2:
        if (v == CUP2006_DRF_PROGRAM) {
            s->drf_program_pending = true;
            s->drf_program_addr = addr;
        } else if (v == CUP2006_DRF_ERASE_SET) {
            s->drf_cmd_state = CUP2006_DRF_ST_ERASE;
        } else if (v == CUP2006_DRF_PROGRAM_SET) {
            s->drf_bypass = true;
        }
        break;
    case CUP2006_DRF_ST_ERASE:
        if (v == CUP2006_DRF_UNLOCK1) {
            s->drf_cmd_state = CUP2006_DRF_ST_ERASE_U1;
        }
        break;
    case CUP2006_DRF_ST_ERASE_U1:
        if (v == CUP2006_DRF_UNLOCK2) {
            s->drf_cmd_state = CUP2006_DRF_ST_ERASE_U2;
        }
        break;
    case CUP2006_DRF_ST_ERASE_U2:
        if (v == CUP2006_DRF_ERASE_CMD) {
            cup2006_drf_erase_sector(s, addr);
            s->drf_cmd_state = CUP2006_DRF_ST_ERASE_U2;
        }
        break;
    case CUP2006_DRF_ST_BYPASS_EXIT:
        if (v == 0) {
            s->drf_bypass = false;
        }
        break;
    }
}

/*
 * Used by a ROMD region (memory_region_init_rom_device): the read
 * callback only runs for non-CPU accesses, the write callback always runs.
 */
static const MemoryRegionOps cup2006_drf_ops = {
    .read = cup2006_drf_read,
    .write = cup2006_drf_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
