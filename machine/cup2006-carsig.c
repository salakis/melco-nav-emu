/*
 * Volvo CU-P2006 board: cup2006-carsig device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup2006-internal.h"


/*
 * CARSIG / sensor block model; see CUP2006_CARSIG_* above.
 * The ready bit in +0x1004 is the only dynamic bit: reading it returns the
 * bit while a sample waits; the OAL ISR's ack, a +0x1004 write with bit 15
 * set (0x8002), clears it. Everything else reads back the guest's own writes.
 */
static uint32_t cup2006_carsig_word(Cup2006MachineState *s, hwaddr off)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < 4; i++) {
        v |= (uint32_t)s->carsig_file[off + i] << (8 * i);
    }
    if (off == CUP2006_CARSIG_COUNT) {
        return s->carsig_count;
    }
    if (off == CUP2006_CARSIG_STATUS) {
        if (s->carsig_irq_raised) {
            v |= CUP2006_CARSIG_READY;
        }
    }
    return v;
}

static bool cup2006_carsig_enabled(Cup2006MachineState *s)
{
    return (cup2006_carsig_word(s, CUP2006_CARSIG_CONTROL) &
            CUP2006_CARSIG_CTRL_EN) != 0;
}

static void cup2006_carsig_update_irq(Cup2006MachineState *s)
{
    struct intc_source *src = s->carsig_intc_src;
    bool want = s->carsig_irq_raised;

    if (!src || want == (src->asserted != 0)) {
        return;
    }
    sh_intc_toggle_source(src, 0, want ? 1 : -1);
}

static void cup2006_carsig_sample(Cup2006MachineState *s)
{
    uint32_t ready = cup2006_carsig_word(s, CUP2006_CARSIG_STATUS) &
                     CUP2006_CARSIG_READY;

    if (ready) {
        /* Previous sample not acknowledged: overrun, no new interrupt. */
        return;
    }
    s->carsig_count++;
    s->carsig_file[CUP2006_CARSIG_COUNT] = s->carsig_count & 0xff;
    s->carsig_file[CUP2006_CARSIG_COUNT + 1] =
        (s->carsig_count >> 8) & 0xff;
    s->carsig_samples++;
    s->carsig_irq_raised = true;
    if (s->carsig_samples <= 8 ||
        (s->carsig_samples & (s->carsig_samples - 1)) == 0) {
        trace_cup2006_carsig_sample_ready_intevt_sysintr(s->carsig_samples, CUP2006_CARSIG_INTEVT, CUP2006_CARSIG_SYSINTR);
    }
    cup2006_carsig_update_irq(s);
}

void cup2006_carsig_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;

    if (!cup2006_carsig_enabled(s)) {
        return;
    }
    cup2006_carsig_sample(s);
    /* Re-arm from the previous deadline, not from now. */
    s->carsig_deadline_ns += (int64_t)s->carsig_period_us * 1000;
    if (s->carsig_deadline_ns < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        s->carsig_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    timer_mod(s->carsig_timer, s->carsig_deadline_ns);
}

static uint32_t cup2006_carsig_gyro(Cup2006MachineState *s)
{
    double yaw = 0;
    long v;

    if (s->track && s->track_clock_started) {
        const MelcoSample *sample = melco_track_sample(
            s->track, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));

        yaw = sample->yaw_dps;             /* deg/s, clockwise positive */
    }
    v = lrint(CUP2006_GYRO_ZERO - yaw * CUP2006_GYRO_PER_DPS);
    return (uint32_t)MAX(0, MIN(16380, v));
}

static uint64_t cup2006_carsig_read(void *opaque, hwaddr base, hwaddr addr,
                                    unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = (addr + i) & (CUP2006_CARSIG_SIZE - 1);
        uint8_t byte;

        if ((off & ~3u) == CUP2006_CARSIG_GYRO ||
            (off & ~3u) == CUP2006_CARSIG_AUX) {
            uint32_t w = (off & ~3u) == CUP2006_CARSIG_GYRO
                         ? cup2006_carsig_gyro(s) : 0x800;

            byte = (w >> (8 * (off & 3))) & 0xff;
        } else if (off == CUP2006_CARSIG_COUNT ||
                   off == CUP2006_CARSIG_COUNT + 1) {
            /* No speed-pulse wire is attached, so the pulse field reads 0. */
            byte = 0;
        } else if (off == CUP2006_CARSIG_STATUS + 1) {
            /* bit 12 is bit 4 of the high byte */
            byte = s->carsig_file[off];
            if (s->carsig_irq_raised) {
                byte |= 0x10;
            }
        } else {
            byte = s->carsig_file[off];
        }
        value |= (uint64_t)byte << (8 * i);
    }
    return value;
}

static void cup2006_carsig_write(void *opaque, hwaddr base, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = base + addr;
    uint32_t v = (uint32_t)value;
    bool ack = false;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = (addr + i) & (CUP2006_CARSIG_SIZE - 1);
        uint8_t byte = (v >> (8 * i)) & 0xff;

        /*
         * Any STATUS write with bit 15 set clears the sample, keeping the
         * line edge-like (a level line storms).
        */
        if (off == CUP2006_CARSIG_STATUS + 1 && (byte & 0x80)) {
            ack = true;
        }
        if (off == CUP2006_CARSIG_COUNT) {
            s->carsig_count = (s->carsig_count & 0xff00) |
                              ((v >> (8 * i)) & 0xff);
        } else if (off == CUP2006_CARSIG_COUNT + 1) {
            s->carsig_count = (s->carsig_count & 0x00ff) |
                              (((v >> (8 * i)) & 0xff) << 8);
        } else {
            s->carsig_file[off] = (v >> (8 * i)) & 0xff;
        }
    }
    if (ack) {
        /*
         * The ack clears the pending sample and drops the line (otherwise
         * the INTC source would stay asserted and the ISR would re-run).
        */
        s->carsig_irq_raised = false;
        /* bit 12 of +0x1004 is bit 4 of its high byte */
        s->carsig_file[CUP2006_CARSIG_STATUS + 1] &= ~0x10;
        cup2006_carsig_update_irq(s);
    }
    /* The period applies when +0x0000 or +0x1000 is written. */
    if ((addr <= CUP2006_CARSIG_CONTROL &&
         addr + size > CUP2006_CARSIG_CONTROL) ||
        addr < CUP2006_CARSIG_PERIOD + 4) {
        uint32_t per = cup2006_carsig_word(s, CUP2006_CARSIG_PERIOD);

        /* +0x0000 counts a 2.5 MHz clock; convert to microseconds. */
        s->carsig_period_us = per ? (uint32_t)((uint64_t)per * 2 / 5)
                                  : CUP2006_CARSIG_DEFAULT_US;
        if (s->carsig_period_us < 1000) {
            s->carsig_period_us = 1000;      /* STUB: cap at 1 kHz */
        } else if (s->carsig_period_us > 1000000) {
            s->carsig_period_us = 1000000;   /* STUB: slowest 1 Hz */
        }
        if (cup2006_carsig_enabled(s)) {
            s->carsig_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                (int64_t)s->carsig_period_us * 1000;
            timer_mod(s->carsig_timer, s->carsig_deadline_ns);
        } else {
            timer_del(s->carsig_timer);
        }
    }
    cup2006_carsig_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: carsig write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n", cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_carsig_p4_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    return cup2006_carsig_read(opaque, CUP2006_CARSIG_BASE, addr, size);
}

static void cup2006_carsig_p4_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    cup2006_carsig_write(opaque, CUP2006_CARSIG_BASE, addr, value, size);
}

static uint64_t cup2006_carsig_a7_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    return cup2006_carsig_read(opaque, CUP2006_CARSIG_A7_BASE, addr, size);
}

static void cup2006_carsig_a7_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    cup2006_carsig_write(opaque, CUP2006_CARSIG_A7_BASE, addr, value, size);
}

const MemoryRegionOps cup2006_carsig_ops = {
    .read = cup2006_carsig_p4_read,
    .write = cup2006_carsig_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_carsig_a7_ops = {
    .read = cup2006_carsig_a7_read,
    .write = cup2006_carsig_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
