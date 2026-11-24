/*
 * CU-P2006 machine: ATA/HDD controller and the INTC2 display lines.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Bounded ATA observability: detail lines are capped per kind (first
 * command per opcode, first eight DMA completions) and carry running
 * counts.  They describe real guest accesses only.
 */
static void cup2006_ata_summary(Cup2006MachineState *s)
{
    trace_cup2006_ata_summary_a(s->ata_events, s->ata_cmd_count, s->ata_identify, s->ata_read_dma, s->ata_read_sectors, s->ata_write_dma, s->ata_write_sectors, s->ata_dma_fail, s->ata_other_cmds, s->ata_reg_reads);
    trace_cup2006_ata_summary_b(s->ata_reg_writes, s->ata_irq_cmds, s->ata_irq_raise, s->ata_irq_drop, s->ata_irq_rearm, s->ata_irq_deassert);
}

static void cup2006_ata_event(Cup2006MachineState *s)
{
    s->ata_events++;
    if (s->ata_summary_at && s->ata_events >= s->ata_summary_at) {
        cup2006_ata_summary(s);
        s->ata_summary_at = s->ata_summary_at < (1ull << 20) ?
                            s->ata_summary_at << 1 : 0;
    }
}

/* Exact counters at exit, past the last power-of-two summary line. */
void cup2006_ata_exit(Notifier *n, void *data)
{
    Cup2006MachineState *s = container_of(n, Cup2006MachineState, ata_exit);

    cup2006_ata_summary(s);
}

static void cup2006_ata_put_word(uint8_t *buf, unsigned word, uint16_t value)
{
    buf[word * 2] = value & 0xff;
    buf[word * 2 + 1] = value >> 8;
}

/* An IDENTIFY string field of `len` bytes (even) from `word` on: two
 * characters per word, the first in the high byte, padded with spaces
 * (ATA/ATAPI-4 8.12). */
static void cup2006_ata_put_string(uint8_t *buf, unsigned word,
                                   unsigned len, const char *str)
{
    size_t n = strlen(str);
    unsigned i;

    for (i = 0; i < len; i += 2) {
        uint8_t hi = i < n ? str[i] : ' ';
        uint8_t lo = i + 1 < n ? str[i + 1] : ' ';

        cup2006_ata_put_word(buf, word + i / 2, hi << 8 | lo);
    }
}

/*
 * HDD backing store: the image is streamed from the host file with pread.
 * Guest writes go to a sparse RAM overlay of 512-byte sectors applied on top
 * of the file bytes, or, with hddwrite=on, into the file itself.
 */
#define CUP2006_HDD_SECTOR 512

static uint8_t *cup2006_hdd_overlay_lookup(Cup2006MachineState *s,
                                           uint64_t sector)
{
    return s->hdd_overlay ?
           g_hash_table_lookup(s->hdd_overlay, &sector) : NULL;
}

static uint8_t *cup2006_hdd_overlay_get(Cup2006MachineState *s,
                                        uint64_t sector)
{
    uint8_t *blk = cup2006_hdd_overlay_lookup(s, sector);
    guint64 *key;

    if (blk) {
        return blk;
    }
    blk = g_malloc0(CUP2006_HDD_SECTOR);
    key = g_new(guint64, 1);
    *key = sector;
    g_hash_table_insert(s->hdd_overlay, key, blk);
    return blk;
}

/* Read the unmodified image bytes; bytes past EOF read as zero. */
static size_t cup2006_hdd_pread(Cup2006MachineState *s, uint64_t offset,
                                uint8_t *buf, size_t len)
{
    size_t got = 0;

    if (s->hdd_fd >= 0 && offset < s->hdd_size) {
        size_t want = MIN((uint64_t)len, s->hdd_size - offset);

        while (got < want) {
            ssize_t done = pread(s->hdd_fd, buf + got, want - got,
                                 offset + got);

            if (done <= 0) {
                break;
            }
            got += done;
        }
    }
    if (got < len) {
        memset(buf + got, 0, len - got);
    }
    return got;
}

/* Read with the guest's written sectors applied on top of the image. */
static size_t cup2006_hdd_read(Cup2006MachineState *s, uint64_t offset,
                               uint8_t *buf, size_t len)
{
    uint64_t first, last, sector;
    size_t got;

    got = cup2006_hdd_pread(s, offset, buf, len);

    if (!s->hdd_overlay || len == 0) {
        return got;
    }
    first = offset / CUP2006_HDD_SECTOR;
    last = (offset + len - 1) / CUP2006_HDD_SECTOR;
    for (sector = first; sector <= last; sector++) {
        const uint8_t *blk = cup2006_hdd_overlay_lookup(s, sector);
        uint64_t base = sector * CUP2006_HDD_SECTOR;
        uint64_t lo = MAX(base, offset);
        uint64_t hi = MIN(base + CUP2006_HDD_SECTOR, offset + len);

        if (!blk || hi <= lo) {
            continue;
        }
        memcpy(buf + (lo - offset), blk + (lo - base), hi - lo);
    }
    return got;
}

/* Store the guest's bytes for [offset, offset+len) in the RAM overlay. */
static void cup2006_hdd_write(Cup2006MachineState *s, uint64_t offset,
                              const uint8_t *buf, size_t len)
{
    uint64_t first, last, sector;
    uint64_t end;

    if (len == 0 || offset >= s->hdd_size) {
        return;
    }
    end = MIN(offset + len, s->hdd_size);
    if (!s->hdd_overlay) {
        /* hddwrite=on: the image file is the disk. */
        ssize_t n = pwrite(s->hdd_fd, buf, end - offset, offset);

        if (n != (ssize_t)(end - offset)) {
            warn_report("cup2006: HDD write at %" PRIu64 " failed (%zd)",
                        offset, n);
        }
        return;
    }
    first = offset / CUP2006_HDD_SECTOR;
    last = (end - 1) / CUP2006_HDD_SECTOR;
    for (sector = first; sector <= last; sector++) {
        uint64_t base = sector * CUP2006_HDD_SECTOR;
        uint64_t lo = MAX(base, offset);
        uint64_t hi = MIN(base + CUP2006_HDD_SECTOR, end);
        uint8_t *blk;

        if (hi <= lo) {
            continue;
        }
        if (lo != base || hi != base + CUP2006_HDD_SECTOR) {
            /* Partial-sector write: seed the block from the image first. */
            uint8_t *existing = cup2006_hdd_overlay_lookup(s, sector);

            if (!existing) {
                blk = cup2006_hdd_overlay_get(s, sector);
                cup2006_hdd_pread(s, base, blk, CUP2006_HDD_SECTOR);
            } else {
                blk = existing;
            }
        } else {
            blk = cup2006_hdd_overlay_get(s, sector);
        }
        memcpy(blk + (lo - base), buf + (lo - offset), hi - lo);
    }
}

/*
 * STUB: the IDENTIFY DEVICE block of a generic LBA-28 ATA-4 disk of the
 * image's size; word 128 is zero, so the driver reports "no lock" instead
 * of attempting a security unlock.
 */
static void cup2006_ata_build_identify(Cup2006MachineState *s)
{
    uint8_t *b = s->ata_ident;
    unsigned i;
    uint8_t sum = 0;
    uint32_t sectors = 0x0fffffff;

    if (s->hdd_fd >= 0 && s->hdd_size >= 512) {
        sectors = MIN(s->hdd_size / 512, 0x0fffffffULL);
    }

    memset(b, 0, CUP2006_ATA_IDENT_SIZE);
    cup2006_ata_put_word(b, 0, 0x0040);   /* fixed device */
    cup2006_ata_put_word(b, 1, 16383);    /* cylinders */
    cup2006_ata_put_word(b, 3, 16);       /* heads */
    cup2006_ata_put_word(b, 6, 63);       /* sectors/track */
    cup2006_ata_put_string(b, 10, 20, "CUP2006HDD0000000001");
    cup2006_ata_put_string(b, 23, 8, "00000253");
    cup2006_ata_put_string(b, 27, 40, "VOLVO RTI HDD");
    cup2006_ata_put_word(b, 47, 0x8001);  /* max multiple */
    cup2006_ata_put_word(b, 49, 0x0300);  /* LBA + DMA supported */
    cup2006_ata_put_word(b, 53, 0x0007);  /* words 54-58, 64-70, 88 valid */
    cup2006_ata_put_word(b, 54, 16383);
    cup2006_ata_put_word(b, 55, 16);
    cup2006_ata_put_word(b, 56, 63);
    cup2006_ata_put_word(b, 57, sectors & 0xffff);
    cup2006_ata_put_word(b, 58, sectors >> 16);
    cup2006_ata_put_word(b, 60, sectors & 0xffff);
    cup2006_ata_put_word(b, 61, sectors >> 16);
    cup2006_ata_put_word(b, 63, 0x0007);
    cup2006_ata_put_word(b, 64, 0x0003);
    cup2006_ata_put_word(b, 65, 120);
    cup2006_ata_put_word(b, 66, 120);
    cup2006_ata_put_word(b, 67, 120);
    cup2006_ata_put_word(b, 68, 120);
    cup2006_ata_put_word(b, 80, 0x0004);  /* ATA-4 */
    cup2006_ata_put_word(b, 82, 0x4000);
    cup2006_ata_put_word(b, 83, 0x4000);
    cup2006_ata_put_word(b, 84, 0x4000);
    cup2006_ata_put_word(b, 85, 0x4000);
    cup2006_ata_put_word(b, 86, 0x4000);
    cup2006_ata_put_word(b, 87, 0x4000);
    cup2006_ata_put_word(b, 128, 0x0000); /* not security-locked */

    /* Word 255: signature 0xA5 in the low byte, then the checksum byte that
     * makes all 512 bytes sum to zero. */
    b[510] = 0xa5;
    for (i = 0; i < 511; i++) {
        sum += b[i];
    }
    b[511] = (uint8_t)(0u - sum);
    s->ata_ident_len = CUP2006_ATA_IDENT_SIZE;
    s->ata_ident_pos = 0;
}

/*
 * Register layout: the task-file block starts at +0x08 (count, LBA0..LBA2,
 * device, command); the command register is +0x1c and status is +0x38.
 */
static uint32_t cup2006_ata_lba(Cup2006MachineState *s)
{
    uint32_t lba0 = s->ata_shadow[0x00c >> 2] & 0xff;
    uint32_t lba1 = s->ata_shadow[0x010 >> 2] & 0xff;
    uint32_t lba2 = s->ata_shadow[0x014 >> 2] & 0xff;
    uint32_t lba3 = s->ata_shadow[0x018 >> 2] & 0x0f;

    return lba0 | (lba1 << 8) | (lba2 << 16) | (lba3 << 24);
}

/*
 * Complete the DMA transfer synchronously on the command-register write;
 * DMA address/length are at +0x9c/+0xa0, status at +0x84. Returns true when
 * the transfer completed (NEND set with bytes moved).
 */
static bool cup2006_ata_dma_transfer(Cup2006MachineState *s, bool write)
{
    uint32_t count = s->ata_shadow[0x008 >> 2] & 0xff;
    uint32_t lba = cup2006_ata_lba(s);
    uint32_t dest = s->ata_shadow[CUP2006_ATA_DMA_ADDR >> 2] & 0x1ffffffc;
    uint32_t len = s->ata_shadow[CUP2006_ATA_DMA_LEN >> 2] & 0x1ffffffe;
    uint64_t offset = (uint64_t)lba * 512;
    uint64_t ram_size = memory_region_size(&s->ram);
    uint8_t *buf;

    if (count == 0) {
        count = 256;              /* ATA: sector count 0 means 256 */
    }
    if (len == 0) {
        len = count * 512;
    }
    len = MIN(len, count * 512);

    if (len == 0 || dest >= ram_size || len > ram_size - dest) {
        s->ata_dma_fail++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cup2006: ata dma rejected lba=0x%x count=%u"
                      " len=0x%x dest=0x%08x\n", lba, count, len, dest);
        s->ata_dma_status = CUP2006_ATA_DMA_NEND;
        return false;
    }

    buf = g_malloc(len);
    if (write) {
        physical_memory_read(dest, buf, len);
        cup2006_hdd_write(s, offset, buf, len);
    } else {
        cup2006_hdd_read(s, offset, buf, len);
        physical_memory_write(dest, buf, len);
    }

    s->ata_dma_status = CUP2006_ATA_DMA_NEND;

    if (write) {
        s->ata_write_dma++;
        s->ata_write_sectors += len / 512;
        if (s->ata_write_logged < 8) {
            s->ata_write_logged++;
            trace_cup2006_ata_write_dma_done_lba(lba, count, len, dest, s->ata_dma_cmd_pc, s->hdd_size);
        }
    } else {
        s->ata_read_dma++;
        s->ata_read_sectors += len / 512;
        if (s->ata_read_logged < 8) {
            s->ata_read_logged++;
            trace_cup2006_ata_read_dma_done(lba, count, len, dest, s->ata_dma_cmd_pc, s->hdd_size, len > 511 ? buf[510] : 0, len > 511 ? buf[511] : 0);
            trace_cup2006_ata_read_dma_done_fs(len > 58 ? buf[54] : 0, len > 58 ? buf[55] : 0, len > 58 ? buf[56] : 0, len > 58 ? buf[57] : 0, len > 58 ? buf[58] : 0);
        }
    }
    g_free(buf);
    return true;
}

/*
 * Defer the DMA copy and the NEND latch to the completion point (the delayed
 * IRQ, or the first +0x84 status poll, whichever comes first): the guest
 * clears NEND after issuing, and a real controller sets it at transfer end.
 */
static void cup2006_ata_dma_complete(Cup2006MachineState *s)
{
    if (!s->ata_dma_pending) {
        return;
    }
    s->ata_dma_pending = false;
    cup2006_ata_dma_transfer(s, s->ata_dma_pending_write);
}

/*
 * STUB: an ATA command takes time; deliver the completion interrupt 1 ms
 * after the command-register write, not synchronously inside it.
 */
#define CUP2006_ATA_IRQ_DELAY_NS 1000000

/*
 * The three lines' INTEVT numbers, for messages only; the sources are the DU
 * model (0x780) and the MBX status/enable/ack contract (0x900).
 */
const uint16_t cup2006_disp_intevt[CUP2006_DISP_LINES] = {
    CUP2006_DISP_INTEVT_DTD0,
    CUP2006_DISP_INTEVT_DTD1,
    CUP2006_DISP_INTEVT_MBX,
};

static const uint32_t cup2006_disp_ack_bit[CUP2006_DISP_LINES] = {
    0x00100000,   /* SYSINTR 0x11 / INTEVT 0x8C0, OEMInterruptEnable */
    0x00001000,   /* SYSINTR 0x12 / INTEVT 0x780 */
    0x00400000,   /* SYSINTR 0x1A / INTEVT 0x900 */
};

static void cup2006_intc2_summary(Cup2006MachineState *s)
{
    trace_cup2006_ata_stub_intc2_summary_writes(s->intc2_writes, s->disp_ack[0], s->disp_ack[1], s->disp_ack[2], s->disp_enable[0], s->disp_enable[1], s->disp_enable[2], s->intc2_other);
}

static uint64_t cup2006_intc2_read_at(Cup2006MachineState *s, hwaddr base,
                                      hwaddr addr, unsigned size)
{
    uint32_t off = addr & (CUP2006_INTC2_SIZE - 1);
    uint32_t value = s->intc2_shadow[off >> 2];
    unsigned shift = (addr & 3) * 8;

    value >>= shift;
    value &= size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);

    /* STUB: the IST gate status at +0x5C reads idle (0); 2 would mean an
     * event is pending, which the model never reports. */
    if (off == CUP2006_INTC2_GATE_OFF) {
        value = 0;
    }

    return value;
}

static uint64_t cup2006_intc2_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_intc2_read_at(opaque, CUP2006_INTC2_BASE, addr, size);
}

static uint64_t cup2006_intc2_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_intc2_read_at(opaque, CUP2006_INTC2_A7_BASE, addr, size);
}

void cup2006_intc2_write_at(Cup2006MachineState *s, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    hwaddr off = addr & (CUP2006_INTC2_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    uint32_t val = ((uint32_t)value & mask) << shift;
    uint32_t *slot = &s->intc2_shadow[off >> 2];
    unsigned i;

    *slot = (*slot & ~(mask << shift)) | val;
    s->intc2_writes++;
    if (off == CUP2006_INTC2_ACK_OFF) {
        /*
         * STUB: the display lines' acknowledge bits are write-1-to-ack and
         * read back 0 once taken, so the ISR's read-modify-write of this
         * register carries only the bit of the line it services.  Each set
         * bit deasserts its model source: one pulse, one interrupt.
 */
        for (i = 0; i < CUP2006_DISP_LINES; i++) {
            if (!(val & cup2006_disp_ack_bit[i])) {
                continue;
            }
            s->disp_ack[i]++;
            if (s->disp_intc_src[i] && s->disp_intc_src[i]->asserted) {
                sh_intc_toggle_source(s->disp_intc_src[i], 0, -1);
            }
            *slot &= ~cup2006_disp_ack_bit[i];
        }
    } else if (off == CUP2006_INTC2_ENABLE_OFF) {
        /*
         * OEMInterruptEnable and OEMInterruptDone(sysintr) share the
         * per-SYSINTR bit in +0x44 (0x11 -> bit 20, 0x12 -> bit 12,
         * 0x1A -> bit 22); count by bit to see repeated InterruptDone.
 */
        s->intc2_enabled |= val;
        /*
         * The 5line ISR acks/dones its SYSINTR 0x1C by setting bit 28
         * here (value 0x10000000), so this is the byte consumed; advance
         * the data port.
 */
        if ((val & CUP2006_DVD5_INTC2_BIT) && s->dvd5_intc_src &&
            s->dvd5_intc_src->asserted) {
            cup2006_dvd5_irq_ack(s);
        }
        for (i = 0; i < CUP2006_DISP_LINES; i++) {
            if (val & cup2006_disp_ack_bit[i]) {
                s->disp_enable[i]++;
                if (s->disp_enable[i] > 1) {
                    s->disp_done[i]++;
                }
            }
        }
        /*
         * No delivery on the enable edge: a completion that happens with
         * the line still disabled stays latched in +0x004 and is not turned
         * into an interrupt.
 */
    } else {
        s->intc2_other++;
    }
    if (((s->intc2_writes & (s->intc2_writes - 1)) == 0 || (s->intc2_writes & 63) == 0) &&
        s->intc2_writes <= 4096) {
        cup2006_intc2_summary(s);
    }
}

static void cup2006_intc2_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    cup2006_intc2_write_at(opaque, addr, value, size);
}

static void cup2006_intc2_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    cup2006_intc2_write_at(opaque, addr, value, size);
}

const MemoryRegionOps cup2006_intc2_ops = {
    .read = cup2006_intc2_read,
    .write = cup2006_intc2_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_intc2_a7_ops = {
    .read = cup2006_intc2_a7_read,
    .write = cup2006_intc2_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void cup2006_ata_irq_fire(void *opaque)
{
    Cup2006MachineState *s = opaque;

    /* The transfer finishes when the controller completes it. */
    cup2006_ata_dma_complete(s);

    if (!s->ata_intc_src->asserted) {
        /*
         * INTRQ is a level held until the guest acknowledges it via the ack
         * register (+0x88); a plain status read must not clear it.
 */
        sh_intc_toggle_source(s->ata_intc_src, 0, 1);
        s->ata_irq_pending = false;
        s->ata_irq_raise++;
        if (s->ata_irq_raise <= 8) {
            trace_cup2006_ata_completion_irq_raised_intevt(s->ata_irq_raise, CUP2006_ATA_INTEVT);
        }
    } else {
        /*
         * The line is still asserted: this completion shares the pending
         * INTRQ with an earlier one. Keep ata_irq_pending set so the ack
         * (+0x88) re-raises for it, delivered after the ack.
 */
        s->ata_irq_pending = true;
        s->ata_irq_drop++;
    }
}

static uint64_t cup2006_ata_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    s->ata_reg_reads++;
    if (s->ata_reg_reads + s->ata_reg_writes == 1) {
        cup2006_ata_summary(s);   /* first touch: prove the block was reached */
    }

    /*
     * A status/command/alternate-status read does not deassert the
     * completion line; only the controller ack register (+0x88) clears it
     * (see cup2006_ata_write).
 */

    if (addr != CUP2006_ATA_DATA && s->ata_ident_reads &&
        addr == CUP2006_ATA_COMMAND) {
        trace_cup2006_ata_identify_read_stopped_data(s->ata_ident_reads, s->ata_ident_pos, s->ata_ident_len, cup2006_guest_pc());
        s->ata_ident_reads = 0;
    }

    if (addr == CUP2006_ATA_STATUS || addr == CUP2006_ATA_ALT_STATUS) {
        return s->ata_status;
    }

    if (addr == CUP2006_ATA_DMA_STATUS) {
        /* Polling +0x84 is the completion point too (see above). */
        cup2006_ata_dma_complete(s);
        return s->ata_dma_status;
    }

    /*
     * +0x1c is the ATA command/status port: writes issue a command, reads
     * return device status; the latched status keeps bit 0 (ERR) clear.
 */
    if (addr == CUP2006_ATA_COMMAND) {
        return s->ata_status;
    }

    if (addr == CUP2006_ATA_DATA && size <= 4 &&
        (addr & (size - 1)) == 0) {
        /*
         * The data register is 16 bits wide: one IDENTIFY word per access
         * even for a 32-bit read (word in the low half, zero in the high);
         * an 8-bit access keeps the byte-stream form.
 */
        if (size == 1) {
            uint8_t byte = 0;

            if (s->ata_ident_pos < s->ata_ident_len) {
                byte = s->ata_ident[s->ata_ident_pos++];
            }
            value = byte;
        } else {
            uint16_t word = 0;

            if (s->ata_ident_pos + 1 < s->ata_ident_len) {
                word = (uint16_t)(s->ata_ident[s->ata_ident_pos] |
                                  (s->ata_ident[s->ata_ident_pos + 1] << 8));
                s->ata_ident_pos += 2;
            }
            value = word;
        }
        s->ata_ident_reads++;
        if (s->ata_ident_pos >= s->ata_ident_len &&
            (s->ata_status & CUP2006_ATA_SR_DRQ)) {
            trace_cup2006_ata_identify_data_complete_data(s->ata_ident_reads, s->ata_ident_pos, s->ata_ident_len, cup2006_guest_pc());
            s->ata_status = CUP2006_ATA_SR_DRDY;
        }
        return value;
    }

    if (addr + size <= CUP2006_ATA_SIZE && (addr & 3) == 0 && size == 4) {
        return s->ata_shadow[addr >> 2];
    }

    for (i = 0; i < size && addr + i < CUP2006_ATA_SIZE; i++) {
        unsigned off = addr + i;

        value |= (uint64_t)((s->ata_shadow[off >> 2] >>
                             (8 * (off & 3))) & 0xff) << (8 * i);
    }
    return value;
}

static void cup2006_ata_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup2006MachineState *s = opaque;
    unsigned i;

    s->ata_reg_writes++;
    if (s->ata_reg_reads + s->ata_reg_writes == 1) {
        cup2006_ata_summary(s);   /* first touch: prove the block was reached */
    }

    if (s->ata_ident_reads && addr == CUP2006_ATA_COMMAND) {
        trace_cup2006_ata_identify_read_stopped_data_2(s->ata_ident_reads, s->ata_ident_pos, s->ata_ident_len, cup2006_guest_pc());
        s->ata_ident_reads = 0;
    }

    for (i = 0; i < size && addr + i < CUP2006_ATA_SIZE; i++) {
        unsigned off = addr + i;
        uint32_t mask = 0xffu << (8 * (off & 3));

        s->ata_shadow[off >> 2] =
            (s->ata_shadow[off >> 2] & ~mask) |
            (((uint32_t)value >> (8 * i)) << (8 * (off & 3)));
    }

    if (addr == CUP2006_ATA_DATA) {
        /* PIO data writes are accepted and discarded. */
        return;
    }

    if (addr == CUP2006_ATA_DMA_STATUS) {
        s->ata_dma_status = value;
        return;
    }

    /*
     * INTRQ is cleared by the controller ack register (+0x88), written by
     * the ISR after reading status; a plain status read must not drop
     * the interrupt.
 */
    if (addr == CUP2006_ATA_IRQ_ACK) {
        if (s->ata_intc_src && s->ata_intc_src->asserted) {
            sh_intc_toggle_source(s->ata_intc_src, 0, -1);
            s->ata_irq_deassert++;
        }
        /* A completion that coalesced onto the held line gets its edge. */
        if (s->ata_irq_pending) {
            s->ata_irq_pending = false;
            s->ata_irq_rearm++;
            timer_mod(s->ata_irq_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      CUP2006_ATA_IRQ_DELAY_NS);
        }
        return;
    }

    if (addr == CUP2006_ATA_COMMAND) {
        uint8_t cmd = value & 0xff;
        bool first = !s->ata_cmd_seen[cmd];
        uint8_t dev = s->ata_shadow[0x018 >> 2] & 0xff;

        s->ata_cmd_seen[cmd] = 1;
        s->ata_cmd_count++;
        if (s->ata_cmd_logged < 32) {
            s->ata_cmd_logged++;
            trace_cup2006_ata_cmd_count_lba_dev(cmd, s->ata_cmd_count, cup2006_guest_pc(), s->ata_shadow[0x008 >> 2] & 0xff, cup2006_ata_lba(s), dev, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
        if (first) {
            trace_cup2006_ata_command_first_pc_count(cmd, cup2006_guest_pc(), s->ata_shadow[0x008 >> 2] & 0xff, cup2006_ata_lba(s));
        }

        if (cmd == 0xec) {          /* IDENTIFY DEVICE */
            cup2006_ata_build_identify(s);
            s->ata_status = CUP2006_ATA_SR_DRDY | CUP2006_ATA_SR_DRQ;
            s->ata_identify++;
            s->ata_ident_reads = 0;
        } else if (cmd == 0xc8) {   /* READ DMA */
            s->ata_status = CUP2006_ATA_SR_DRDY;
            /* A transfer still in flight belongs to the previous command. */
            cup2006_ata_dma_complete(s);
            s->ata_dma_pending = true;
            s->ata_dma_pending_write = false;
            s->ata_dma_cmd_pc = cup2006_guest_pc();
        } else if (cmd == 0xca) {   /* WRITE DMA */
            s->ata_status = CUP2006_ATA_SR_DRDY;
            cup2006_ata_dma_complete(s);
            s->ata_dma_pending = true;
            s->ata_dma_pending_write = true;
            s->ata_dma_cmd_pc = cup2006_guest_pc();
        } else {
            s->ata_status = CUP2006_ATA_SR_DRDY;
            s->ata_other_cmds++;
        }
        cup2006_ata_event(s);

        /*
         * Command complete: schedule the SYSINTR-23 completion interrupt
         * (INTEVT 0x860). STUB: a real controller raises INTRQ at command
         * end; the +0x88 ack write clears it.
 */
        if (s->ata_intc_src && s->ata_irq_timer) {
            s->ata_irq_pending = true;
            s->ata_irq_cmds++;
            timer_mod(s->ata_irq_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      CUP2006_ATA_IRQ_DELAY_NS);
        }
    }
}

const MemoryRegionOps cup2006_ata_ops = {
    .read = cup2006_ata_read,
    .write = cup2006_ata_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
