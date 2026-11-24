/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

#define CUP28_FLASH_SAVE_MS 200

static void cup28_flash_mark_dirty(Cup28MachineState *s, hwaddr addr,
                                   hwaddr len)
{
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

static void cup28_flash_save(Cup28MachineState *s)
{
    ssize_t n;

    if (s->drf_fd < 0 || s->drf_autoselect) {
        return;
    }
    if (s->drf_image_full_write) {
        n = pwrite(s->drf_fd, s->drf_data, CUP28_FLASH_SIZE, 0);
        if (n != CUP28_FLASH_SIZE) {
            warn_report("cup28: drfimage write failed (%zd)", n);
            return;
        }
        s->drf_image_full_write = false;
    } else if (s->drf_dirty_lo < s->drf_dirty_hi) {
        n = pwrite(s->drf_fd, s->drf_data + s->drf_dirty_lo,
                   s->drf_dirty_hi - s->drf_dirty_lo, s->drf_dirty_lo);
        if (n != (ssize_t)(s->drf_dirty_hi - s->drf_dirty_lo)) {
            warn_report("cup28: drfimage write failed (%zd)", n);
            return;
        }
    }
    s->drf_dirty_lo = CUP28_FLASH_SIZE;      /* nothing dirty */
    s->drf_dirty_hi = 0;
}

static void cup28_bksram_init(Cup28MachineState *s)
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

static void cup28_flash_save_timer(void *opaque)
{
    Cup28MachineState *s = opaque;

    cup28_flash_save(s);
    timer_mod(s->drf_save_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CUP28_FLASH_SAVE_MS);
}

static void cup28_flash_exit(Notifier *n, void *data)
{
    Cup28MachineState *s = container_of(n, Cup28MachineState, drf_exit);

    cup28_flash_save(s);
}

static uint64_t cup28_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < CUP28_FLASH_SIZE; i++) {
        value |= (uint64_t)s->drf_data[addr + i] << (8 * i);
    }

    if (size < 4) {
        value &= (1u << (8 * size)) - 1;
    }
    return value;
}

/* AMD unlock-sequence states: only ST_ERASE_ARMED (AA 55 80 AA 55)
 * lets a 0x30 write erase a sector. */
enum {
    CUP28_FLASH_ST_READ,
    CUP28_FLASH_ST_U1,          /* AA seen */
    CUP28_FLASH_ST_U2,          /* AA 55 seen */
    CUP28_FLASH_ST_ERASE,       /* AA 55 80 */
    CUP28_FLASH_ST_ERASE_U1,    /* AA 55 80 AA */
    CUP28_FLASH_ST_ERASE_ARMED, /* AA 55 80 AA 55: 30 erases a sector */
};

/*
 * The VNS board uses AMD commands on a 16-bit bus. Each nonzero lane
 * must contain the command byte (e.g. 0x00aa00aa).
 */
static bool cup28_flash_cmd(uint64_t value, unsigned size, unsigned cmd)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        unsigned b = (value >> (8 * i)) & 0xff;
        if (b != 0 && b != cmd) {
            return false;
        }
    }
    return (value & 0xff) == cmd;
}

static void cup28_flash_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup28MachineState *s = opaque;
    hwaddr sector;
    unsigned i;
    /* The loader writes 0xa0 and the data at the same target (0x8c04bf48). */
    bool is_data = s->drf_program_pending && addr == s->drf_program_addr;
    /* Decode an armed write as data even when it matches a command. */
    if (is_data) {
        s->drf_program_pending = false;
        for (i = 0; i < size && addr + i < CUP28_FLASH_SIZE; i++) {
            s->drf_data[addr + i] = ((uint32_t)value >> (8 * i)) & 0xff;
        }
        cup28_flash_mark_dirty(s, addr, size);
        s->drf_programs++;
        /* One line per power-of-two program keeps a 270k-word write
         * bounded while still showing the first addresses. */
        if ((s->drf_programs & (s->drf_programs - 1)) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "cup28: drf program phys=0x%08" HWADDR_PRIx
                          " size=%u value=0x%08" PRIx32 " count=%u\n",
                          (hwaddr)(CUP28_FLASH_BASE + addr), size,
                          (uint32_t)value, s->drf_programs);
        }
        return;
    }

    /*
     * Earlier VNS board: the guest issues AMD autoselect (0x90) and reads
     * the manufacturer/device ID from the bank base.  Its writes are at
     * base+0x2aa8/0x1554, so the
     * ID lives at addr-0x2aa8.  STUB: 0x0001/0x227E (AMD 256-Mbit class)
     * stands in for the unread part marking; the array is restored on 0xF0.
     */
    if (cup28_flash_cmd(value, size, 0x90)) {
        if (!s->drf_autoselect) {
            hwaddr bank = (addr >= 0x2aa8) ?
                          (addr - 0x2aa8) : 0;

            if (bank + 16 <= CUP28_FLASH_SIZE) {
                memcpy(s->drf_id_save, s->drf_data + bank, 16);
                memory_region_flush_rom_device(&s->drf_regs, bank, 16);
                memset(s->drf_data + bank, 0x00, 16);
                s->drf_data[bank + 0] = 0x01;
                s->drf_data[bank + 2] = 0x7e;
                s->drf_data[bank + 3] = 0x22;
                s->drf_autoselect = true;
                s->drf_autoselect_bank = bank;
            }
        }
        return;
    }
    if (cup28_flash_cmd(value, size, 0xf0) && s->drf_autoselect) {
        memory_region_flush_rom_device(&s->drf_regs, s->drf_autoselect_bank,
                                       16);
        memcpy(s->drf_data + s->drf_autoselect_bank, s->drf_id_save, 16);
        s->drf_autoselect = false;
        return;
    }

    if (cup28_flash_cmd(value, size, 0x30) && (addr & 3) == 0 &&
        s->drf_cmd_state == CUP28_FLASH_ST_ERASE_ARMED) {
        s->drf_cmd_state = CUP28_FLASH_ST_READ;
        s->drf_erases++;
        s->drf_program_pending = false;
        sector = addr & ~(hwaddr)(CUP28_FLASH_SECTOR - 1);
        /* STUB: instant erase; the array reads 0xffffffff immediately. */
        memset(s->drf_data + sector, 0xff,
               MIN((hwaddr)CUP28_FLASH_SECTOR, CUP28_FLASH_SIZE - sector));
        cup28_flash_mark_dirty(s, sector,
                               MIN((hwaddr)CUP28_FLASH_SECTOR,
                                   CUP28_FLASH_SIZE - sector));
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cup28: drf erase cmd phys=0x%08" HWADDR_PRIx
                      " target=0x%08" HWADDR_PRIx " count=%u\n",
                      (hwaddr)(CUP28_FLASH_BASE + addr),
                      (hwaddr)(CUP28_FLASH_BASE + sector),
                      s->drf_erases);
        return;
    }

    if (cup28_flash_cmd(value, size, 0xa0) && !s->drf_program_pending) {
        s->drf_cmd_state = CUP28_FLASH_ST_READ;
        s->drf_program_pending = true;
        s->drf_program_addr = addr;
        return;
    }

    /* AMD command-set state machine: only the full unlock sequence
     * AA 55 80 AA 55 arms a 0x30 sector erase, so a stray aligned 0x30
     * write can no longer erase (and persist) a 256 KiB sector.  The
     * unlock addresses are not checked (STUB, as on the cup2006 model). */
    {
        unsigned st = s->drf_cmd_state;

        s->drf_cmd_state = CUP28_FLASH_ST_READ;
        switch (st) {
        case CUP28_FLASH_ST_READ:
            if (cup28_flash_cmd(value, size, 0xaa)) {
                s->drf_cmd_state = CUP28_FLASH_ST_U1;
            }
            break;
        case CUP28_FLASH_ST_U1:
            if (cup28_flash_cmd(value, size, 0x55)) {
                s->drf_cmd_state = CUP28_FLASH_ST_U2;
            }
            break;
        case CUP28_FLASH_ST_U2:
            if (cup28_flash_cmd(value, size, 0x80)) {
                s->drf_cmd_state = CUP28_FLASH_ST_ERASE;
            }
            break;
        case CUP28_FLASH_ST_ERASE:
            if (cup28_flash_cmd(value, size, 0xaa)) {
                s->drf_cmd_state = CUP28_FLASH_ST_ERASE_U1;
            }
            break;
        case CUP28_FLASH_ST_ERASE_U1:
            if (cup28_flash_cmd(value, size, 0x55)) {
                s->drf_cmd_state = CUP28_FLASH_ST_ERASE_ARMED;
            }
            break;
        default:
            break;
        }
    }

    /* Writes without a program command leave the flash array unchanged. */
    s->drf_other_writes++;
    if (s->drf_other_writes <= CUP28_FLASH_LOG_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cup28: drf write phys=0x%08" HWADDR_PRIx
                      " size=%u value=0x%" PRIx64
                      " (no program command; array unchanged)\n",
                      (hwaddr)(CUP28_FLASH_BASE + addr), size, value);
    }
}

/*
 * CUP28: the flash part's A25 is not wired, so physical 0x02000000-
 * 0x03ffffff aliases 0x00000000-0x01ffffff.  The M-loader programs the
 * SHx through the P2 window 0xA2000000+ (the M-loader writes the 0xA0
 * command and data at r4 = 0xA2081000-...) while the XIP code later
 * fetches at VA 0x80080000+ (phys 0x00080000+). Forward to the flash array with bit 25 masked off.
 */
static uint64_t cup28_drf_alias_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    return cup28_flash_read(s, addr & (CUP28_FLASH_SIZE - 1), size);
}

static void cup28_drf_alias_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    Cup28MachineState *s = opaque;
    cup28_flash_write(s, addr & (CUP28_FLASH_SIZE - 1), value, size);
}

static const MemoryRegionOps cup28_drf_alias_ops = {
    .read = cup28_drf_alias_read,
    .write = cup28_drf_alias_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps cup28_flash_ops = {
    .read = cup28_flash_read,
    .write = cup28_flash_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
