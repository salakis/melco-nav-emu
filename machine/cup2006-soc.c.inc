/*
 * CU-P2006 machine: SoC/board misc: CPG, RTC, LAN DPRAM, PC-card slots.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static uint64_t cup2006_cpg_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_mmio_read(CUP2006_CPG_BASE + addr, size);
}

static void cup2006_cpg_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    cup2006_mmio_write(CUP2006_CPG_BASE + addr, value, size);
}

static const MemoryRegionOps cup2006_cpg_ops = {
    .read = cup2006_cpg_read,
    .write = cup2006_cpg_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Seconds the RTC reads now (power-on epoch plus guest time since the last set). */
static int64_t cup2006_rtc_now_s(Cup2006MachineState *s)
{
    return s->rtc_base_s +
           (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - s->rtc_base_ms) / 1000;
}

static bool cup2006_rtc_reg(Cup2006MachineState *s, hwaddr off, uint16_t *value)
{
    time_t t = (time_t)cup2006_rtc_now_s(s);
    struct tm tm;

    gmtime_r(&t, &tm);
    switch (off) {
    case CUP2006_RTC_LATCH:
        *value = 0;
        return true;
    case CUP2006_RTC_STATUS:
        /* Not busy; bit 0 = 1 means "backup invalid (NG)". */
        *value = 0x0000;
        return true;
    case CUP2006_RTC_YEAR:
        *value = tm.tm_year - 100;          /* years since 2000 */
        return true;
    case CUP2006_RTC_MONTH:
        *value = tm.tm_mon + 1;
        return true;
    case CUP2006_RTC_DAY:
        *value = tm.tm_mday;
        return true;
    case CUP2006_RTC_HOUR:
        *value = tm.tm_hour;
        return true;
    case CUP2006_RTC_MINUTE:
        *value = tm.tm_min;
        return true;
    case CUP2006_RTC_SECOND:
        *value = tm.tm_sec << CUP2006_RTC_SECOND_SHIFT;
        return true;
    default:
        *value = 0;
        return false;
    }
}

static uint64_t cup2006_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint16_t reg;
        unsigned shift;

        /*
         * The device is a 16-bit register file. Byte accesses address the
         * little-endian half of that file, matching the SH-4 bus.
 */
        off &= ~(hwaddr)1;
        shift = 8 * ((addr + i) & 1);
        cup2006_rtc_reg(s, off, &reg);
        value |= (uint64_t)((reg >> shift) & 0xff) << (8 * i);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: rtc read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n", cup2006_guest_pc(), CUP2006_RTC_BASE + addr, size,
                  value);
    return value;
}

/*
 * Setting the clock: year..second to +0x06..+0x10, then 0xffff to +0x02
 * commits. STUB: the commit meaning of 0xffff is inferred.
 */
static void cup2006_rtc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup2006MachineState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: rtc write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n", cup2006_guest_pc(), CUP2006_RTC_BASE + addr, size,
                  value);
    if (size != 2) {
        return;
    }
    if (addr >= CUP2006_RTC_SET_YEAR && addr <= CUP2006_RTC_SET_SECOND) {
        s->rtc_set[(addr - CUP2006_RTC_SET_YEAR) / 2] = value;
    } else if (addr == CUP2006_RTC_LATCH && value == 0xffff) {
        struct tm tm = {
            .tm_year = s->rtc_set[0] + 100, .tm_mon = s->rtc_set[1] - 1,
            .tm_mday = s->rtc_set[2], .tm_hour = s->rtc_set[3],
            .tm_min = s->rtc_set[4],
            .tm_sec = s->rtc_set[5] >> CUP2006_RTC_SECOND_SHIFT,
        };

        s->rtc_base_s = (int64_t)timegm(&tm);
        s->rtc_base_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        trace_cup2006_soc_rtc_set_by_the(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, s->rtc_base_ms);
    }
}

static const MemoryRegionOps cup2006_rtc_ops = {
    .read = cup2006_rtc_read,
    .write = cup2006_rtc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* PC-card slots: two 4 MiB windows at 0x1B000000/0x1B400000, each with a
 * 0x20-byte controller block at +0x3FFFE0. STUB: the slot holds a ready
 * 3.3 V ATA card (status +0x08 = 0x0120); the rest is plain storage. */
/* STUB: the card's initial attribute-memory content at page 0 offset 0:
 * the CIS tuple chain (DEVICE, CONFIG, FUNCID=ATA, FUNCE, CFTABLE_ENTRY,
 * END) a card answers with before the guest writes it. */
static const uint8_t cup2006_pcc_cis[] = {
    0x01, 0x00, 0x02, 0x00, 0x0d, 0x00, 0x00, 0x00,     /* DEVICE */
    0x1a, 0x00, 0x05, 0x00, 0x01, 0x00, 0x01, 0x00,     /* CONFIG */
    0x00, 0x00, 0x02, 0x00, 0x03, 0x00,
    0x21, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,     /* FUNCID: ATA */
    0x22, 0x00, 0x06, 0x00, 0x01, 0x00, 0x04, 0x00,     /* FUNCE */
    0xea, 0x00, 0x01, 0x00, 0x69, 0x00, 0x00, 0x00,
    0x1b, 0x00, 0x08, 0x00, 0x03, 0x00, 0x00, 0x00,     /* CFTABLE_ENTRY */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00,                         /* END */
};

/*
 * One stderr line per distinct slot/offset/direction, quoting the value
 * the guest actually read or wrote.
 */
static void cup2006_pcc_note(Cup2006MachineState *s, unsigned slot, hwaddr off,
                             unsigned size, bool is_write, uint64_t value)
{
    uint8_t bit = is_write ? 2 : 1;

    if (off >= CUP2006_PCC_REGS_SIZE || (s->pcc_seen[slot][off] & bit)) {
        return;
    }
    s->pcc_seen[slot][off] |= bit;
    trace_cup2006_soc_pcc_slot_prix64_size(slot, is_write ? "write" : "read", off, size, value, CUP2006_PCC_BASE + slot * CUP2006_PCC_SLOT_SIZE +
            CUP2006_PCC_REGS_OFF + off);
}

static uint64_t cup2006_pcc_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006PccRegs *regs = opaque;
    Cup2006MachineState *s = regs->s;
    unsigned slot = regs->slot;
    hwaddr off = addr;          /* region-relative, not window-relative */
    hwaddr abs = CUP2006_PCC_BASE + slot * CUP2006_PCC_SLOT_SIZE +
                 CUP2006_PCC_REGS_OFF + addr;
    uint64_t value = 0;
    unsigned i;

    if (slot >= CUP2006_PCC_SLOTS || off + size > CUP2006_PCC_REGS_SIZE) {
        return cup2006_mmio_read(abs, size);
    }
    for (i = 0; i < size; i++) {
        uint8_t byte;

        if (off + i == CUP2006_PCC_STATUS) {
            /* STUB: card present at 3.3 V, ready (see the block comment). */
            byte = CUP2006_PCC_STATUS_VALUE & 0xff;
        } else if (off + i == CUP2006_PCC_STATUS + 1) {
            byte = (CUP2006_PCC_STATUS_VALUE >> 8) & 0xff;
        } else {
            byte = s->pcc_ctrl[slot][off + i];
        }
        value |= (uint64_t)byte << (8 * i);
    }
    cup2006_pcc_note(s, slot, off, size, false, value);
    return value;
}

static void cup2006_pcc_regs_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Cup2006PccRegs *regs = opaque;
    Cup2006MachineState *s = regs->s;
    unsigned slot = regs->slot;
    hwaddr off = addr;          /* region-relative, not window-relative */
    hwaddr abs = CUP2006_PCC_BASE + slot * CUP2006_PCC_SLOT_SIZE +
                 CUP2006_PCC_REGS_OFF + addr;

    if (slot >= CUP2006_PCC_SLOTS || off + size > CUP2006_PCC_REGS_SIZE) {
        cup2006_mmio_write(abs, value, size);
        return;
    }
    cup2006_reg32_write(s->pcc_ctrl[slot], off, value, size);
    cup2006_pcc_note(s, slot, off, size, true, value);
}

static const MemoryRegionOps cup2006_pcc_regs_ops = {
    .read = cup2006_pcc_regs_read,
    .write = cup2006_pcc_regs_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Attribute memory at the CIS base (0x1B100000): the CIS image above plus
 * the card config registers at +0x200 as plain storage.  STUB: a real CIS
 * is read-only; guest writes to the image are stored here instead.
 */
#define CUP2006_PCC_CIS_LOG_MAX 32

static void cup2006_pcc_cis_note(Cup2006PccCis *c, hwaddr off,
                                 unsigned size, bool is_write, uint64_t value)
{
    Cup2006MachineState *s = c->s;
    uint8_t bit = is_write ? 2 : 1;
    static const hwaddr base[CUP2006_PCC_CIS_BASES] = CUP2006_PCC_CIS_OFFS;

    if (s->pcc_cis_seen[c->base][off] & bit) {
        return;
    }
    s->pcc_cis_seen[c->base][off] |= bit;
    if (s->pcc_cis_log_nr[c->base] >= CUP2006_PCC_CIS_LOG_MAX) {
        return;
    }
    s->pcc_cis_log_nr[c->base]++;
    {
        CPUSH4State *env = current_cpu ? (CPUSH4State *)cpu_env(current_cpu) : NULL;

        trace_cup2006_soc_pcc_cis_prix64_size(is_write ? "write" : "read", off, size, value, CUP2006_PCC_BASE + base[c->base] + off, env ? env->pc : 0, env ? env->pr : 0);
    }
}

static uint64_t cup2006_pcc_cis_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006PccCis *c = opaque;
    Cup2006MachineState *s = c->s;
    uint64_t value = 0;
    unsigned i;

    if (addr + size > CUP2006_PCC_CIS_SIZE) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->pcc_card[addr + i] << (8 * i);
    }
    cup2006_pcc_cis_note(c, addr, size, false, value);
    return value;
}

static void cup2006_pcc_cis_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    Cup2006PccCis *c = opaque;
    Cup2006MachineState *s = c->s;
    unsigned i;

    if (addr + size > CUP2006_PCC_CIS_SIZE) {
        return;
    }
    /*
     * One card memory behind both apertures: writes are stored and visible
     * at both 0x1B000000 and 0x1B100000. STUB: both serve page 0.
     */
    for (i = 0; i < size; i++) {
        s->pcc_card[addr + i] = (value >> (8 * i)) & 0xff;
    }
    cup2006_pcc_cis_note(c, addr, size, true, value);
}

static const MemoryRegionOps cup2006_pcc_cis_ops = {
    .read = cup2006_pcc_cis_read,
    .write = cup2006_pcc_cis_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
