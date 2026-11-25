/*
 * Volvo CU-P2006 board: PowerVR MBX1 3D core, the vendored mbxemu model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 *
 * mbxemu is MIT, (c) 2026 Carsten Munk and the revive-n8x0 contributors; see
 * machine/mbx/README.melco.md for the provenance and the licence split. This
 * file is the board's own glue and is GPL-2.0-or-later.
 *
 * Without mbx3d=on the MBX is cup2006-mbx.c's latch window: the register file
 * reads back what was written, the frame-completion bits are raised by the
 * display model, and the 3D view's car marker is drawn on the host
 * (CUP2006_CARMARKER=1).  With mbx3d=on the same 64 KiB window and the same
 * 8 MiB TA FIFO are decoded by mbxemu, which parses the TA stream, interprets
 * the VGP and rasterises the scene into the guest's own colour buffer.
 *
 * The window is identical, so this is a replacement and not an addition, and
 * three of the latch model's readings were guesses that the model corrects:
 *
 *   0x12C  INT_STATUS.  A write *sets* bits (a doorbell); it does not latch a
 *          value.  Bits 0-14 are the interrupt status, 16-23 the input FIFO's
 *          fill.  The latch model's bit 6 ("parameter memory ready") is
 *          I_EVM_DALLOC, the direct-allocate completion, and its bit 8 ("TA
 *          kick", polled before each +0x81C) is I_TA_CONTEXT, a TA-context
 *          event.
 *   0x130  INT_ENABLE, 0x134 INT_CLEAR (write-1-to-clear).  The line is
 *          level-sensitive: up while `enable & 0x8000` and `status & enable`.
 *   0x680  the render kick.  (0x7F0 is not a register; the model reports a
 *          damage rectangle there for host software.)
 *   0x800  TA start, 0x804 resume, 0x808 close, 0x810 context id,
 *          0x814/0x818 TA context load/store, 0x81C context reset,
 *          0x820 context base.
 *   0x828/0x82C  the free list's first and last page, not a surface extent.
 *   0x838  free-list build.  The latch model read `= 1` as "one descriptor
 *          armed" and completed the request instantly; it builds the
 *          parameter buffer's free list.
 *
 * Nothing here runs on the host clock: a render kick rasterises before the
 * write returns and raises the completion interrupt then, which is what the
 * latch model did too, so guest-visible ordering is unchanged.
 */

#include "cup2006-internal.h"

#include "mbx/mbxemu.h"


/* The MBX as bus master: physical memory, after its own MMU (core.c). */
static void cup2006_mbx3d_mem_read(void *opaque, uint32_t phys, void *buf,
                                   uint32_t len)
{
    Cup2006MachineState *s = opaque;

    address_space_read(&address_space_memory, phys, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    if (s->mbx3d_trace_all) {
        trace_cup2006_mbx3d_master_read(phys, len);
    }
}

static void cup2006_mbx3d_mem_write(void *opaque, uint32_t phys,
                                    const void *buf, uint32_t len)
{
    Cup2006MachineState *s = opaque;

    address_space_write(&address_space_memory, phys, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
    if (s->mbx3d_trace_all) {
        trace_cup2006_mbx3d_master_write(phys, len);
    }
}

/*
 * The interrupt line.  mbxemu keeps its own status and enable; the display
 * model owns the INTEVT 0x900 wiring and decides from s->mbx_status, so the
 * two are kept in step here rather than duplicating the controller.
 */
static void cup2006_mbx3d_irq(void *opaque, int level)
{
    Cup2006MachineState *s = opaque;

    /*
     * mbxemu_new() resets the model before it returns, and the reset drives
     * the line low: s->mbx3d is still NULL at that point.
     */
    if (!s->mbx3d) {
        return;
    }
    s->mbx_status = mbxemu_reg_read(s->mbx3d, 0x12c);
    s->mbx_window[CUP2006_MBX_ENABLE >> 2] =
        mbxemu_reg_read(s->mbx3d, 0x130);
    if (level) {
        struct mbxemu_stats st;

        mbxemu_stats(s->mbx3d, &st);
        trace_cup2006_mbx3d_stats(st.scenes, st.objects, st.prims, st.renders,
                                  st.tiles, st.stream_errors, st.violations);
    }
    trace_cup2006_mbx3d_irq(level, s->mbx_status,
                            s->mbx_window[CUP2006_MBX_ENABLE >> 2],
                            cup2006_guest_pc());
    cup2006_mbx_update_irq(s, level ? "mbx3d" : "mbx3d-drop");
}

static void cup2006_mbx3d_log(void *opaque, const char *line)
{
    (void)opaque;
    trace_cup2006_mbx3d_log(line);
}

static const struct mbxemu_host cup2006_mbx3d_host = {
    .mem_read = cup2006_mbx3d_mem_read,
    .mem_write = cup2006_mbx3d_mem_write,
    .irq = cup2006_mbx3d_irq,
    .log = cup2006_mbx3d_log,
};

void cup2006_mbx3d_init(Cup2006MachineState *s)
{
    struct mbxemu_host host = cup2006_mbx3d_host;

    host.opaque = s;
    s->mbx3d = mbxemu_new(&host);
    if (!s->mbx3d) {
        error_report("cup2006: cannot create the MBX1 model");
        exit(1);
    }
    mbxemu_set_strict(s->mbx3d, 1);
    /* level 2 also prints each TA word; level 1 only the registers */
    mbxemu_set_trace(s->mbx3d, s->mbx3d_trace_all ? 2 : 0);
}

void cup2006_mbx3d_reset(Cup2006MachineState *s)
{
    if (!s->mbx3d) {
        return;
    }
    mbxemu_reset(s->mbx3d);
    /*
     * The latch model's arm/render bookkeeping is inert while the model is
     * present, but the display model still reads mbx_status for INTEVT 0x900,
     * so start it from the model's power-on state rather than from zero.
     */
    s->mbx_status = mbxemu_reg_read(s->mbx3d, 0x12c);
}

uint64_t cup2006_mbx3d_read(Cup2006MachineState *s, uint32_t off,
                            unsigned size)
{
    uint32_t value = mbxemu_read(s->mbx3d, off & (CUP2006_MBX_SIZE - 1));
    unsigned shift = (off & 3) * 8;
    uint64_t mask = size == 4 ? 0xffffffffu : ((1ull << (size * 8)) - 1);

    return (value >> shift) & mask;
}

void cup2006_mbx3d_write(Cup2006MachineState *s, uint32_t off,
                         uint64_t value, unsigned size)
{
    hwaddr o = off & (CUP2006_MBX_SIZE - 1);

    /*
     * mbxemu decodes 32-bit words; the hardware's side effects (a render
     * kick, a free-list build) are word-granular too.  A narrow access
     * therefore assembles the word from the shadow and writes the whole of
     * it, which is also what the latch window's read-back does.
     */
    uint32_t *slot = &s->mbx3d_lane[o >> 2];
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (o & 3) * 8;

    *slot = (*slot & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
    mbxemu_write(s->mbx3d, o & ~3u, *slot);
    if (o < 0x1000) {
        s->mbx_window[o >> 2] = mbxemu_reg_read(s->mbx3d, o);
    }
}

/*
 * The TA window at 0xFD800000: its first half takes stream words (the MBX
 * counts them whichever lane they arrive on, README assumption 11), the second
 * half is the TA control port, whose word ends or restarts an object.
 */
void cup2006_mbx3d_ta_write(Cup2006MachineState *s, hwaddr addr,
                            uint64_t value)
{
    if ((addr & (CUP2006_MBX_TA_SIZE - 1)) >= CUP2006_MBX_TA_CTL) {
        mbxemu_ta_ctl_write(s->mbx3d, (uint32_t)value);
        return;
    }
    mbxemu_ta_write(s->mbx3d, (uint32_t)addr & 0xf, (uint32_t)value);
}
