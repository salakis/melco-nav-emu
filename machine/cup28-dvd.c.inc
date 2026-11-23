/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

/* CUP28 byte-port drive link. NK hooks 0x80083a8e and 0x80083a96
 * select the CPLD interrupt lines. Status framing follows dvd.dll
 * 0x01016fbc and 0x01019c38. Sector data uses the board DMAC.
 */
#define CUP28_DVD_SECTOR_SIZE 2048
#define CUP28_DVD_MAX_XFER (16 * MiB)

static void cup28_dvd_status(Cup28MachineState *s, unsigned command)
{
    uint8_t reply[4] = { 0xb0 };
    unsigned length = 0;
    bool status = command == 0x80 || command == 0x81 ||
                  command == 0x01 || command == 0x04;

    /* STUB: spin-up completes between the loading report and next poll. */
    if (s->dvd5_spinup_pending) {
        const uint8_t ready[11] = { 0xb1, 0x48, 0x20, 0x08 };
        memcpy(s->dvd5_rx, ready, sizeof(ready));
        length = sizeof(ready);
        s->dvd5_disc_ready = true;
        s->dvd5_spinup_pending = false;
    }
    if (status && s->dvd_fd >= 0) {
        reply[1] = s->dvd5_disc_ready ? 0x48 : 0xb0;
        reply[2] = 0x20;
        reply[3] = s->dvd5_disc_ready ? 0x08 : 0;
        s->dvd5_spinup_pending = !s->dvd5_disc_ready;
    } else if (command == 0x45 || command == 0x87) {
        reply[1] = command;
        reply[2] = command == 0x45 ? 0x10 : 0;
    }
    memcpy(s->dvd5_rx + length, reply, sizeof(reply));
    s->dvd5_rx_len = length + sizeof(reply);
    s->dvd5_rx_pos = 0;
}

static void cup28_dvd_irq_src(Cup28MachineState *s, struct intc_source *src,
                              unsigned active)
{
    if (!src) {
        return;
    }
    src->prio = 3;
    src->enable_count = src->enable_max;
    if (!!active != !!src->asserted) {
        sh_intc_toggle_source(src, 0, active ? 1 : -1);
        if (active) {
            cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
        }
    }
}

static void cup28_dvd_irq(Cup28MachineState *s)
{
    unsigned active = ldl_le_p(s->p28_dvd_control_reg + 0x40) &
                      ldl_le_p(s->p28_dvd_control_reg + 0x44) & 3;
    uint32_t priorities = ldl_le_p(s->p28_dvd_control_reg + 0x100);
    unsigned priority = (priorities >> 16) & 15;
    bool audio = ((priorities >> 12) & 15) == 12 &&
                 (ldl_le_p(s->p28_dvd_control_reg + 0x30) &
                  ldl_le_p(s->p28_dvd_control_reg + 0x34) & 2);

    /* The CPLD puts 15 minus the programmed priority on the IRL pins.
     * MLD 8c04b194 selects 12 (INTEVT 260); NK 80083b1a selects 11 (280). */
    cup28_dvd_irq_src(s, s->dvd5_intc_src, active && priority == 11);
    /* NK 80083a8e installs the audio CPLD handler at the same vector
     * used by the loader for DVD. The active source owns the line. */
    cup28_dvd_irq_src(s, s->dvd5_intc_src2, (active && priority == 12) || audio);
}

static bool cup28_dvd_rx_avail(Cup28MachineState *s)
{
    return s->dvd5_rx_pos < s->dvd5_rx_len;
}

static void cup28_dvd_schedule(Cup28MachineState *s)
{
    if (s->p28_dvd_timer) {
        /* STUB: 10 us/byte for a sector stream, 1 ms for the first byte. */
        s->p28_dvd_rx_due = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            (s->p28_dvd_rx_ready ? 10000 : 1000000);
        s->p28_dvd_rx_ready = false;
        timer_mod(s->p28_dvd_timer, s->p28_dvd_rx_due);
    }
}

static void cup28_dvd_tick(void *opaque)
{
    Cup28MachineState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->p28_dvd_tx_wait) {
        s->p28_dvd_tx_wait = false;
        stl_le_p(s->p28_dvd_control_reg + 0x40,
                 ldl_le_p(s->p28_dvd_control_reg + 0x40) | 2);
    }
    if (cup28_dvd_rx_avail(s)) {
        if (now >= s->p28_dvd_rx_due) {
            s->p28_dvd_rx_ready = true;
            stl_le_p(s->p28_dvd_control_reg + 0x40,
                     ldl_le_p(s->p28_dvd_control_reg + 0x40) | 1);
        } else {
            timer_mod(s->p28_dvd_timer, s->p28_dvd_rx_due);
        }
    }
    cup28_dvd_irq(s);
}

static uint64_t cup28_dvd_port_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    if (addr == 0) {
        /* Firmware polled receiver uses bit 0 low = receive active,
         * bit 1 = RX byte available; polled transmitter tests bits 0/2.
         * The M-loader (BLD-CUP28E) additionally requires bit 5 (0x20)
         * set before it writes the first command byte to +4, so the idle
         * value is 0x25. */
        if (cup28_dvd_rx_avail(s)) {
            return (s->p28_dvd_rx_ready ? 2 : 0) | 0x20;
        }
        return 5 | 0x20;
    }
    if (addr == 8) {
        uint8_t b;

        if (s->dvd5_rx_pos < s->dvd5_rx_len) {
            b = s->dvd5_rx[s->dvd5_rx_pos++];
            cup28_dvd_schedule(s);
            return b;
        }
    }
    return 0;
}

static void cup28_dvd_port_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    Cup28MachineState *s = opaque;
    unsigned expected = 0;
    if (addr != 4) {
        return; /* status write acknowledges port errors */
    }
    if (s->p28_dvd_tx_len >= sizeof(s->p28_dvd_tx)) {
        s->p28_dvd_tx_len = 0;
    }
    s->p28_dvd_tx[s->p28_dvd_tx_len++] = value;
    s->p28_dvd_tx_wait = true;
    if (s->p28_dvd_tx_len >= 2) {
        switch (s->p28_dvd_tx[1]) {
        case 0x3a: case 0x81: expected = 3; break;
        case 0x80: expected = 8; break;
        case 0x45: expected = 4; break;
        case 0x93: expected = 10; break;
        /* M-loader ATAPI reset: b8 90; the caller ignores the reply body
         * but the send-wait needs an RX frame or it retries five times
         * and desyncs the byte stream. */
        case 0x90: expected = 2; break;
        case 0x01: case 0x04: case 0x87: expected = 2; break;
        /* Sector data uses the earlier native DMAC, not the newer
         * CU-P2006 engine's transfer registers. */
        default: break; /* unknown wire command: do not invent a reply */
        }
    }
    if (expected && s->p28_dvd_tx_len == expected) {
        s->p28_dvd_rx_ready = false;
        if (s->p28_dvd_tx[0] == 0xb8 && s->p28_dvd_tx[1] == 0x3a) {
            /* STUB: reply shaped so the guest dvd.dll's 0x3a parser
             * accepts it (byte1 bit3 set, bytes 4/5 = 0x20/1); the real
             * drive's 0x3a echo semantics would fail that check. */
            memset(s->dvd5_rx, 0, sizeof(s->dvd5_rx));
            s->dvd5_rx[0] = s->p28_dvd_tx[2];
            s->dvd5_rx[1] = 0x38;   /* class 0x30, bit 3 set */
            s->dvd5_rx[2] = 0x20;
            s->dvd5_rx[3] = 0x08; /* earlier DVD-ROM media type */
            s->dvd5_rx[4] = 0x20;
            s->dvd5_rx[5] = 0x01;
            s->dvd5_rx_len = s->dvd5_rx[0] == 0xb4 ? 6 : 4;
            s->dvd5_rx_pos = 0;
        } else if (s->p28_dvd_tx[0] == 0xb8 && s->p28_dvd_tx[1] == 0x90) {
            /* ATAPI reset: answer with the drive's idle status frame. */
            s->dvd5_rx[0] = 0xb4;
            s->dvd5_rx[1] = 0x38;
            s->dvd5_rx[2] = 0x20;
            s->dvd5_rx[3] = 0x08;
            s->dvd5_rx[4] = 0x20;
            s->dvd5_rx[5] = 0x01;
            s->dvd5_rx_len = 6;
            s->dvd5_rx_pos = 0;
        } else if (s->p28_dvd_tx[0] == 0xb8 && s->p28_dvd_tx[1] == 0x93) {
            uint32_t lba = ldl_be_p(s->p28_dvd_tx + 2);
            uint32_t sectors = ldl_be_p(s->p28_dvd_tx + 6);
            uint64_t off = (uint64_t)lba * CUP28_DVD_SECTOR_SIZE;
            uint64_t length = (uint64_t)sectors * CUP28_DVD_SECTOR_SIZE;
            size_t got = 0;

            if (length > 0 && length <= CUP28_DVD_MAX_XFER) {
                size_t len = length;
                g_free(s->p28_dvd_stream);
                s->p28_dvd_stream = g_malloc0(4 + len);
                s->p28_dvd_stream[len] = 0xb0;
                /* DVD_Read 0x01016960 rejects classes 0/0x10 as EJECT;
                 * accepted media must also set the ATAPI-on bit 3. */
                s->p28_dvd_stream[len + 1] = 0x38;
                s->p28_dvd_stream[len + 2] = 0x20;
                s->p28_dvd_stream[len + 3] = 0x08;
                if (s->dvd_fd >= 0 && off < s->dvd_size) {
                    size_t want = MIN(len, s->dvd_size - off);

                    while (got < want) {
                        ssize_t done = pread(s->dvd_fd,
                                             s->p28_dvd_stream + got,
                                             want - got, off + got);
                        if (done < 0 && errno == EINTR) {
                            continue;
                        }
                        if (done <= 0) {
                            break;
                        }
                        got += done;
                    }
                }
                if (got != len) {
                    /* Unknown mecha error framing: leave the receiver idle
                     * rather than DMA invented zero-filled sectors. */
                    cup28_dbg(s, "cup28: DVD short read lba=%x wanted=%zx got=%zx",
                                lba, len, got);
                    g_free(s->p28_dvd_stream);
                    s->p28_dvd_stream = NULL;
                    s->p28_dvd_stream_len = 0;
                    s->dvd5_rx_len = s->dvd5_rx_pos = 0;
                    s->p28_dvd_tx_len = 0;
                    timer_mod(s->p28_dvd_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
                    cup28_dvd_irq(s);
                    return;
                }
                s->p28_dvd_stream_len = 4 + len;
                /* Status is on the byte link; sectors use the external
                 * request at b7000000 through the guest-programmed DMAC. */
                /* DVD's consumer 0x01017cf4 copies sector bytes directly
                 * from the DMA base: no leading header. STUB: carry the
                 * mecha status in the four surplus trailing DMA bytes;
                 * the exact real trailer is unknown and not consumed. */
                memcpy(s->dvd5_rx, s->p28_dvd_stream + len, 4);
                /* STUB: the B0 frame is padded to six bytes (b0 38 20 08
                 * 20 01) because the guest's RX parser waits for more at
                 * four; the real drive's trailer is unknown. */
                s->dvd5_rx[4] = 0x20;
                s->dvd5_rx[5] = 0x01;
                s->dvd5_rx_len = 6;
                s->dvd5_rx_pos = 0;
                cup28_dmac_try(s);
            }
        } else {
            cup28_dvd_status(s, s->p28_dvd_tx[1]);
        }
        s->p28_dvd_tx_len = 0;
    }
    /* Transfer completion follows the sender's acknowledgement writes.
     * STUB: 100 us per byte, 1 ms drive response after a full request. */
    timer_mod(s->p28_dvd_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    cup28_dvd_irq(s);
}

static uint64_t cup28_dvd_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    return cup28_register_read(s->p28_dvd_control_reg, addr, size);
}

static void cup28_dvd_ctrl_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    Cup28MachineState *s = opaque;
    if (addr == 0x00 || addr == 0x10 || addr == 0x30 || addr == 0x40 ||
        addr == 0x50 || addr == 0x70) {
        /* OAL handlers acknowledge by writing ~bit to pending (+0x00/+0x10:
         * GPS UART, NK 0x80086018/0x800860a8). */
        value &= ldl_le_p(s->p28_dvd_control_reg + addr);
    }
    cup28_register_write(s->p28_dvd_control_reg, addr, value, size);
    cup28_dvd_irq(s);
    cup28_q2_irq(s);
    cup28_most_irq(s);
    cup28_most_chip_irq(s);
    cup28_gps_irq(s);
    cup28_sns_irq(s);
}

static const MemoryRegionOps cup28_dvd_port_ops = {
    .read = cup28_dvd_port_read, .write = cup28_dvd_port_write,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps cup28_dvd_ctrl_ops = {
    .read = cup28_dvd_ctrl_read, .write = cup28_dvd_ctrl_write,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* STUB: EEPROM program/read-back uses a RAM bank. NK 0x80085510
 * polls bit 8 of +0x54 for device readiness. */
static uint64_t cup28_eeprom_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < 0x100; i++) {
        value |= (uint64_t)((s->p28_eeprom_reg[(addr + i) >> 2]
                 >> (8 * ((addr + i) & 3))) & 0xff) << (8 * i);
    }
    if (addr <= 0x55 && addr + size > 0x55) {
        value |= (uint64_t)0x01 << (8 * (0x55 - addr)); /* ready */
    }
    /* STUB: external sensor-replay DIP is open (pulled high). This is
     * an input, independent of writes to the shared GPIO output latch.
     * SRD PIO_Init 0x010a23e0 maps a7800000; PIO_Read(15), 0x010a2724,
     * samples +0x54 bit 15. IOCTL 0x1b0004 returns it to ClassLib's
     * NSensor constructor, which selects replay when it reads zero.
     */
    if (addr <= 0x55 && addr + size > 0x55) {
        value |= (uint64_t)0x80 << (8 * (0x55 - addr));
    }
    return value;
}

static void cup28_eeprom_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Cup28MachineState *s = opaque;
    unsigned i;

    for (i = 0; i < size && addr + i < 0x100; i++) {
        uint32_t *r = &s->p28_eeprom_reg[(addr + i) >> 2];
        unsigned sh = 8 * ((addr + i) & 3);
        if (addr + i == 0x54) {
            cup28_most_gpio(s, *r, (*r & ~0xffu) | ((uint32_t)value >> (8 * i) & 0xff));
        }
        *r = (*r & ~(0xffu << sh)) | (((uint32_t)value >> (8 * i)) & 0xff)
             << sh;
    }
}

static const MemoryRegionOps cup28_eeprom_ops = {
    .read = cup28_eeprom_read, .write = cup28_eeprom_write,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

#define CUP28_EEPROM_BASE 0x07800000
#define CUP28_EEPROM_SIZE 0x00800000

/* FLR_Read maps this 256 KiB window (ROM literals 0x00fd1db4/0x00fd1dbc). */
static uint64_t cup28_flr_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < 0x40000; i++) {
        value |= (uint64_t)s->p28_flr_mem[addr + i] << (8 * i);
    }
    return value;
}

static void cup28_flr_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    Cup28MachineState *s = opaque;
    unsigned i;

    for (i = 0; i < size && addr + i < 0x40000; i++) {
        s->p28_flr_mem[addr + i] = (value >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps cup28_flr_ops = {
    .read = cup28_flr_read, .write = cup28_flr_write,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
