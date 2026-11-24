/*
 * Volvo CU-P2006 board: cup2006-mbx device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup2006-internal.h"


static void cup2006_mbx_write(Cup2006MachineState *s, hwaddr base,
                              hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr phys = base + addr;
    hwaddr off = addr & (CUP2006_MBX_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    uint32_t *slot = &s->mbx_window[off >> 2];
    uint32_t old = *slot;

    /* Writes latch and read back; the side effects are +0x12C/+0x130/+0x134. */
    *slot = (old & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
    /*
     * +0x838 = 1 arms one descriptor. STUB: the request completes instantly
     * ("+0x12C bit 6 parameter memory ready") when the +0x130 bit-6 enable
     * is set; the render/TA-done status bits are set by the display
     * model at the first status read (cup2006-du.c) — no rasteriser
     * here.
     */
    if (off == CUP2006_MBX_SURFACE_END && *slot == 1) {
        s->mbx_arm_seq++;
        s->mbx_arm_count++;
        if (s->mbx_arm_count <= 16 || (s->mbx_arm_count & 0xff) == 0) {
            trace_cup2006_mbx_arm_pages(s->mbx_arm_count, s->mbx_window[CUP2006_MBX_SURFACE_PG_LO >> 2], s->mbx_window[CUP2006_MBX_SURFACE_PG_HI >> 2]);
        }
        if (s->mbx_window[CUP2006_MBX_ENABLE >> 2] & CUP2006_MBX_KICK_BIT) {
            s->mbx_completed_seq = s->mbx_arm_seq;
            if (!(s->mbx_status & CUP2006_MBX_ST_PM_READY)) {
                s->mbx_status |= CUP2006_MBX_ST_PM_READY;
            }
            trace_cup2006_mbx_stub_mbx_completion_x12c(s->mbx_completed_seq, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    if (off == CUP2006_MBX_TA_START && (*slot & 1)) {
        s->mbx_ta_running = true;
        s->mbx_ta_acked = false;
        s->mbx_ta_reads = 0;
        s->mbx_ta_starts++;
        if (s->mbx_ta_starts <= 8 || (s->mbx_ta_starts & 0xff) == 0) {
            trace_cup2006_mbx_stub_mbx_x800_ta(s->mbx_ta_starts, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    if (off == CUP2006_MBX_RENDER_START && (*slot & 1)) {
        cup2006_mbx_ta_frame(s);
        s->mbx_render_running = true;
        s->mbx_renders++;
        if (s->mbx_renders <= 8 || (s->mbx_renders & 0xff) == 0) {
            trace_cup2006_mbx_stub_mbx_x680_render(s->mbx_renders, s->mbx_window[0x65c >> 2], (s->mbx_window[0x654 >> 2] >> 16) + 1, (s->mbx_window[0x658 >> 2] >> 16) + 1, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    if (off == CUP2006_MBX_TA_KICK && (*slot & 1)) {
        s->mbx_status |= CUP2006_MBX_ST_TA_KICK;
        s->mbx_ta_kicks++;
        if (s->mbx_ta_kicks <= 8 || (s->mbx_ta_kicks & 0xff) == 0) {
            trace_cup2006_mbx_stub_mbx_x81c_kick(s->mbx_ta_kicks, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    /*
     * +0x130 is the enable mask: the frame wait sets bit 6 after arming, but
     * a 0 -> 1 write with a fresh arm completes it too.
     */
    if (off == CUP2006_MBX_ENABLE && s->mbx_render_running &&
        !(*slot & CUP2006_MBX_STATUS_DONE)) {
        /* The client unmasked the render-done bit: the render is done
         * (bits 2 and 6, see the status read above). */
        s->mbx_render_running = false;
        /* STUB: bit 8 is "ready", polled before each frame's +0x81C kick
         * and set only by the kick. */
        s->mbx_status |= CUP2006_MBX_STATUS_DONE | CUP2006_MBX_ST_PM_READY |
                         CUP2006_MBX_ST_TA_KICK;
    }
    if (off == CUP2006_MBX_ENABLE) {
        if ((*slot & CUP2006_MBX_KICK_BIT) && !(old & CUP2006_MBX_KICK_BIT)) {
            s->mbx_kick_count++;
            if (s->mbx_kick_count <= 16 ||
                (s->mbx_kick_count & 0xff) == 0) {
                trace_cup2006_mbx_kick_x130_with_x838(s->mbx_kick_count, *slot, s->mbx_window[CUP2006_MBX_SURFACE_END >> 2]);
            }
        }
        if ((*slot & CUP2006_MBX_KICK_BIT) &&
            s->mbx_window[CUP2006_MBX_SURFACE_END >> 2] == 1 &&
            s->mbx_arm_seq != s->mbx_completed_seq) {
            s->mbx_completed_seq = s->mbx_arm_seq;
            s->mbx_status |= CUP2006_MBX_ST_PM_READY;
            trace_cup2006_mbx_stub_mbx_completion_x12c_2(s->mbx_completed_seq, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    /*
     * +0x134 is the status write-1-to-clear: the ISR accumulates the +0x12C
     * bits it handled and writes them back here.
     */
    if (off == CUP2006_MBX_STATUS_ACK) {
        uint32_t ack = ((uint32_t)value & mask) << shift;
        uint32_t cleared = s->mbx_status & ack;

        if (s->mbx_ta_running && (ack & CUP2006_MBX_ST_TA_DONE)) {
            s->mbx_ta_acked = true;
        }
        /* bit 8 is cleared by the client and re-raised at the next status
         * read. */
        if (ack & CUP2006_MBX_ST_TA_KICK) {
            s->mbx_b8_rearm = true;
        }

        if (cleared) {
            s->mbx_status &= ~cleared;
            s->mbx_status_done_acks++;
            trace_cup2006_mbx_status_ack_pc_value(cup2006_guest_pc(), (uint32_t)value, cleared, s->mbx_status, s->mbx_status_done_acks);
        }
    }
    if (off == CUP2006_MBX_ENABLE || off == CUP2006_MBX_STATUS_ACK ||
        off == CUP2006_MBX_STATUS || off == CUP2006_MBX_SURFACE_END) {
        cup2006_mbx_update_irq(s, off == CUP2006_MBX_ENABLE ? "enable" :
                               off == CUP2006_MBX_STATUS_ACK ? "ack" :
                               off == CUP2006_MBX_SURFACE_END ? "arm" :
                               "status-write");
    }
    cup2006_mbx_note(s, phys, true, size, value);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: mbx write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_mbx_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_mbx_read(opaque, CUP2006_MBX_BASE, addr, size);
}

static void cup2006_mbx_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_mbx_write(opaque, CUP2006_MBX_BASE, addr, value, size);
}

static uint64_t cup2006_mbx_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_mbx_read(opaque, CUP2006_MBX_A7_BASE, addr, size);
}

static void cup2006_mbx_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_mbx_write(opaque, CUP2006_MBX_A7_BASE, addr, value, size);
}

const MemoryRegionOps cup2006_mbx_ops = {
    .read = cup2006_mbx_p4_read,
    .write = cup2006_mbx_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_mbx_a7_ops = {
    .read = cup2006_mbx_a7_read,
    .write = cup2006_mbx_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * STUB: the 0xFD100000 mailbox sub-block (alias 0x1D100000): reads return 0,
 * writes are absorbed.
 */
static uint64_t cup2006_mbx_mailbox_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "cup2006: mbx mailbox read +0x%03" HWADDR_PRIx
                  " size=%u\n", addr, size);
    return 0;
}

static void cup2006_mbx_mailbox_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "cup2006: mbx mailbox write +0x%03" HWADDR_PRIx
                  " size=%u value=0x%" PRIx64 "\n", addr, size, value);
}

const MemoryRegionOps cup2006_mbx_mailbox_ops = {
    .read = cup2006_mbx_mailbox_read,
    .write = cup2006_mbx_mailbox_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
