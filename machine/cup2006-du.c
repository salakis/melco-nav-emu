/*
 * Volvo CU-P2006 board: cup2006-du device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup2006-internal.h"


/*
 * MBX / 3DG register window: see the CUP2006_MBX_* comment above. One
 * stderr line per distinct register/direction; every access still goes to
 * the -d guest_errors trace.
 */
/* The MBX log itself lives in the machine state (s->mbx_log; the capacity
 * is defined with the others in cup2006.c). */
void cup2006_mbx_note(Cup2006MachineState *s, hwaddr phys,
                             bool is_write, unsigned size, uint64_t value)
{
    unsigned i;

    for (i = 0; i < s->mbx_log_nr; i++) {
        if (s->mbx_log[i].addr == phys &&
            s->mbx_log[i].is_write == is_write) {
            return;
        }
    }
    if (s->mbx_log_nr == CUP2006_MBX_LOG_MAX) {
        if (!s->mbx_log_full_warned) {
            s->mbx_log_full_warned = true;
            trace_cup2006_du_stub_mbx_log_full(CUP2006_MBX_LOG_MAX);
        }
        return;
    }
    s->mbx_log[s->mbx_log_nr].addr = phys;
    s->mbx_log[s->mbx_log_nr].is_write = is_write;
    s->mbx_log_nr++;
    trace_cup2006_du_stub_mbx_register_size((uint32_t)phys, is_write ? "write" : "read", size, value, CUP2006_MBX_STATUS);
}

/*
 * MBX interrupt line: INTEVT 0x900 is raised on an enabled 0 -> 1 status
 * transition in +0x12C and released when the guest's +0x134 ack clears it.
 *
 * STUB departure: +0x130 is read here as a MASK, the opposite of the enable
 * that cup2006-mbx.c, machine/mbx/core.c and the driver itself use
 * (+0x130 = 0xffffffff before each +0x838 arm, 0xffffffbf after acking bit
 * 6).  Read as an enable, the boot is instruction-identical to 71.29 s and
 * then stops on the disclaimer: the only 0x900 interrupt the latch model can
 * raise is its instant completion at driver-init time (t=1.5 s), which the
 * driver answers by polling.
 *
 * The handler behind it is this device's own.  It reads +0x12C and acks bits
 * through +0x134.  The compositing that carries the boot along is delivered
 * by other sources: on a normal boot the 2D engine's 0x8C0 asserts 130 times
 * for 130 jobs from 3.85 s and the display unit's 0x780 in the same run, with
 * no INTC2 source disabled or left unenabled in between.
 *
 * What the line is missing is events.  The bits below are raised when the
 * guest reads +0x12C: a TA run "finishes" at the first status read after an
 * ack, a render completes with bits 2 and 6 at once.  The core in machine/mbx/
 * raises them where they happen instead - I_RENDER 0x4, I_ISP 0x8 and
 * I_EVM_DALLOC 0x40 with the render's completion, I_TA_DONE 0x10 at the end of
 * the TA stream, I_TA_CONTEXT 0x100 on the context writes at +0x814 and
 * +0x818, the free list at +0x838 for the direct allocate.  A bit made up at
 * a status read has no timing to be wrong about, which is why it cannot stand
 * in for those.
 *
 * The boot that reaches the map gets there through a phantom raise: at
 * 71.29 s the driver kicks the render at +0x680, writes 0x7fbb to +0x130 with
 * bits 2 and 6 pending, a mask reads that as enabling them, and the code the
 * resulting interrupt unblocks is the guest's 0x900 handler.  So the sense is
 * load-bearing only because the events are missing: read as the hardware has
 * it and without those events, the boot is instruction-identical to 71.29 s
 * and then stops on the disclaimer, and with mbx3d=1 the same sense stops the
 * 3D map.  Both hold until the events exist.
 */
static uint32_t cup2006_mbx_active_status(Cup2006MachineState *s)
{
    /* STUB: a mask, not the enable the hardware has - see above. */
    uint32_t mask = s->mbx_window[CUP2006_MBX_ENABLE >> 2] & 0xffff;

    return s->mbx_status & ~mask & 0xffff & ~CUP2006_MBX_ST_POLLED;
}

void cup2006_mbx_update_irq(Cup2006MachineState *s, const char *why)
{
    struct intc_source *src = s->disp_intc_src[2];   /* INTEVT 0x900 */
    uint32_t active = cup2006_mbx_active_status(s);
    bool now = active != 0;

    if (!src) {
        return;
    }
    if (now && !s->mbx_irq_active) {
        sh_intc_toggle_source(src, 0, 1);
        s->mbx_irq_raises++;
        trace_cup2006_du_mbx_intevt_raised_why(CUP2006_DISP_INTEVT_MBX, s->mbx_irq_raises, why, s->mbx_status, s->mbx_window[CUP2006_MBX_ENABLE >> 2] & 0xffff, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    } else if (!now && s->mbx_irq_active) {
        if (src->asserted) {
            sh_intc_toggle_source(src, 0, -1);
        }
        s->mbx_irq_drops++;
        trace_cup2006_du_mbx_intevt_released_why(CUP2006_DISP_INTEVT_MBX, s->mbx_irq_drops, why, s->mbx_status, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    s->mbx_irq_active = now;
}

uint64_t cup2006_mbx_read(Cup2006MachineState *s, hwaddr base,
                                 hwaddr addr, unsigned size)
{
    hwaddr phys = base + addr;
    hwaddr off = addr & (CUP2006_MBX_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t value;
    bool mbx_status_bits = false;

    /*
     * With the MBX1 model present it owns the window: its status word, its
     * write-1-to-clear and its own FIFO-fill bits are the real ones, so the
     * latch model's inferred bits below must not be added on top.
     */
    if (s->mbx3d) {
        value = cup2006_mbx3d_read(s, off, size);
        cup2006_mbx_note(s, phys, false, size, value);
        return value;
    }

    /* Stored read-back, except +0x12C: the real status word. */
    value = s->mbx_window[off >> 2] >> shift;

    /* STUB: the TA run is done at the first status read after the
     * client's bit-4 ack; 64 reads without an ack also finish it. */
    /* STUB: bit 8 is re-raised at the first status read after the
     * client clears it ("ready"). */
    if (off == CUP2006_MBX_STATUS && s->mbx_b8_rearm) {
        s->mbx_b8_rearm = false;
        s->mbx_status |= CUP2006_MBX_ST_TA_KICK;
        mbx_status_bits = true;
    }
    if (off == CUP2006_MBX_STATUS && s->mbx_ta_running &&
        (s->mbx_ta_acked || ++s->mbx_ta_reads > 64)) {
        s->mbx_ta_running = false;
        s->mbx_status |= CUP2006_MBX_ST_TA_DONE;
        mbx_status_bits = true;
    }
    /* A render completes with bit 2 (render done) AND bit 6 (parameter
     * memory released). STUB: both bits at once, no timing. */
    if (off == CUP2006_MBX_STATUS && s->mbx_render_running) {
        s->mbx_render_running = false;
        /* + bit 8 ("ready"): set only by the +0x81C kick. STUB: meaning
         * inferred. */
        s->mbx_status |= CUP2006_MBX_STATUS_DONE | CUP2006_MBX_ST_PM_READY |
                         CUP2006_MBX_ST_TA_KICK;
        mbx_status_bits = true;
    }
    if (mbx_status_bits) {
        /* A status bit set by a read is a 0 -> 1 transition like any other,
         * so it must move the 0x900 line. */
        cup2006_mbx_update_irq(s, "x12c-status-read");
    }
    if (off == CUP2006_MBX_STATUS) {
        value = s->mbx_status >> shift;
        if (s->mbx_status & CUP2006_MBX_STATUS_DONE) {
            s->mbx_status_done_reads++;
            if (s->mbx_status_done_reads <= 8 ||
                (s->mbx_status_done_reads & 0xff) == 0) {
                trace_cup2006_du_mbx_status_x12c_done(cup2006_guest_pc(), value, s->mbx_status_done_reads);
            }
        }
    }
    value &= size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    cup2006_mbx_note(s, phys, false, size, value);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: mbx read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, (uint64_t)value);
    return value;
}

/* DU scan-out -> graphic console 0: guest planes scanned out of guest
 * physical memory at the guest-programmed geometry (PnMR, PnMWR, PnDSX/Y,
 * PnDPX/Y, PnDSA0R). */
static uint32_t cup2006_du_reg(Cup2006MachineState *s, unsigned off)
{
    return s->du_shadow[(off & (CUP2006_DU_SIZE - 1)) >> 2];
}

/* The buffer plane n is displaying: PnDSA0R or PnDSA1R. */
static uint32_t cup2006_du_display_base(Cup2006MachineState *s, unsigned plane)
{
    unsigned off = CUP2006_DU_PLANE_STRIDE * plane;

    return cup2006_du_reg(s, off + (s->du_displayed[plane] ?
                                    CUP2006_DU_PNDSA1R :
                                    CUP2006_DU_PNDSA0R)) & ~0xfu;
}

static void cup2006_du_scanout(Cup2006MachineState *s)
{
    DisplaySurface *surface;
    uint8_t *base;
    int dst_stride, plane, y, oi, nplanes = 0;
    int order[6];
    uint32_t dppr = cup2006_du_reg(s, CUP2006_DU_DPPR);
    static uint32_t dppr_logged = 0xffffffffu;

    /* Composite in the guest's DPPR order (SH7785 19.3.7: nibble n-1 =
     * priority n), priority 1 in front: draw priority 6 down to 1. */
    for (oi = 0; oi < 6; oi++) {
        int pr = 5 - oi;
        uint32_t nib = (dppr >> (4 * pr)) & 0xf;

        if ((nib & 8) && (nib & 7) + 1 <= CUP2006_DU_PLANES) {
            order[nplanes++] = (nib & 7) + 1;
        }
    }
    if (!nplanes) {                    /* DPPR not programmed: plane order */
        for (oi = 1; oi <= CUP2006_DU_PLANES && nplanes < 6; oi++) {
            order[nplanes++] = oi;
        }
    }
    if (dppr != dppr_logged) {
        dppr_logged = dppr;
        trace_cup2006_du_dppr(dppr);
    }

    if (!s->disp_con) {
        return;
    }
    surface = qemu_console_surface(s->disp_con);
    if (!surface || surface_width(surface) != CUP2006_DU_CONSOLE_W ||
        surface_height(surface) != CUP2006_DU_CONSOLE_H) {
        return;
    }
    base = surface_data(surface);
    dst_stride = surface_stride(surface);
    /* No plane: the panel shows the display-off/background colour. */
    for (y = 0; y < CUP2006_DU_CONSOLE_H; y++) {
        memset(base + (size_t)y * dst_stride, 0,
               (size_t)CUP2006_DU_CONSOLE_W * 4);
    }

    for (oi = 0; oi < nplanes; oi++) {
        unsigned off = CUP2006_DU_PLANE_STRIDE * (plane = order[oi]);
        uint32_t mr = cup2006_du_reg(s, off + CUP2006_DU_PNMR);
        uint32_t mwr = cup2006_du_reg(s, off + CUP2006_DU_PNMWR);
        uint32_t dsx = cup2006_du_reg(s, off + CUP2006_DU_PNDSXR) & 0x7ff;
        uint32_t dsy = cup2006_du_reg(s, off + CUP2006_DU_PNDSYR) & 0x3ff;
        uint32_t dpx = cup2006_du_reg(s, off + CUP2006_DU_PNDPXR) & 0x7ff;
        uint32_t dpy = cup2006_du_reg(s, off + CUP2006_DU_PNDPYR) & 0x3ff;
        uint32_t tc2 = cup2006_du_reg(s, off + CUP2006_DU_PNTC2R);
        uint32_t ddf = mr & 3;
        unsigned bpp = (ddf == CUP2006_DU_DDF_8BPP ||
                        ddf == CUP2006_DU_DDF_YC) ? 1 : 2;
        uint32_t width_px = (mwr >> 4) * 16;   /* PnMWX in 16-pixel units */
        uint64_t stride = (uint64_t)width_px * bpp;
        uint64_t pa = cup2006_du_display_base(s, plane);
        uint16_t row[CUP2006_DU_CONSOLE_W];
        unsigned x, yy;

        /* Display conditions, manual 19.4.2: size, memory width and
         * start address must be non-zero; the format must be one we can
         * render (ARGB1555 or RGB565). */
        if (!dsx || !dsy || !mwr || !pa ||
            (ddf != CUP2006_DU_DDF_ARGB && ddf != CUP2006_DU_DDF_16BPP)) {
            continue;
        }
        if (dsx > CUP2006_DU_CONSOLE_W) {
            dsx = CUP2006_DU_CONSOLE_W;
        }
        for (yy = 0; yy < dsy; yy++) {
            int ty = (int)dpy + (int)yy;

            if (ty < 0 || ty >= CUP2006_DU_CONSOLE_H) {
                continue;
            }
            if (stride < (uint64_t)dsx * bpp) {
                continue;
            }
            address_space_read(&address_space_memory,
                               pa + (uint64_t)yy * stride,
                               MEMTXATTRS_UNSPECIFIED, row,
                               dsx * bpp);
            for (x = 0; x < dsx; x++) {
                uint32_t *dst = (uint32_t *)(base +
                                             (size_t)ty * dst_stride);
                int tx = (int)dpx + (int)x;
                uint16_t c = lduw_le_p(&row[x]);    /* guest is LE */
                unsigned r, g, b;

                if (tx < 0 || tx >= CUP2006_DU_CONSOLE_W) {
                    continue;
                }
                if (ddf == CUP2006_DU_DDF_ARGB) {
                    /* ARGB555: A:1 R:5 G:5 B:5; PnTC2R bits 14..0 are the
                     * transparent colour (manual 19.4.5). */
                    if ((c & 0x7fff) == (tc2 & 0x7fff)) {
                        continue;
                    }
                    r = (c >> 10) & 0x1f;
                    g = (c >> 5) & 0x1f;
                    b = c & 0x1f;
                    dst[tx] = 0xff000000u |
                              (((r << 3) | (r >> 2)) << 16) |
                              (((g << 3) | (g >> 2)) << 8) |
                              ((b << 3) | (b >> 2));
                } else {
                    if (c == (uint16_t)tc2) {
                        continue;
                    }
                    r = (c >> 11) & 0x1f;
                    g = (c >> 5) & 0x3f;
                    b = c & 0x1f;
                    dst[tx] = 0xff000000u |
                              (((r << 3) | (r >> 2)) << 16) |
                              (((g << 2) | (g >> 4)) << 8) |
                              ((b << 3) | (b >> 2));
                }
            }
        }
    }
}

static void cup2006_display_update(Cup2006MachineState *s)
{
    if (!s->disp_con) {
        return;
    }
    cup2006_du_scanout(s);
    qemu_console_update(s->disp_con, 0, 0, CUP2006_DU_CONSOLE_W,
                        CUP2006_DU_CONSOLE_H);
}

static bool cup2006_display_gfx_update(void *opaque)
{
    Cup2006MachineState *s = opaque;

    /* The host UI may refresh between DU frames. Present the last DU
     * scanout then, rather than sampling guest buffers at an unrelated
     * host refresh time (possibly during drawing). The DU frame timer
     * owns scanout and applies PnDC switches at its frame boundary;
     * SH7785 manual 19.4.15, PDF p.985. */
    if (s->disp_con) {
        qemu_console_update(s->disp_con, 0, 0, CUP2006_DU_CONSOLE_W,
                            CUP2006_DU_CONSOLE_H);
    }
    return true;    /* handled synchronously */
}

static const GraphicHwOps cup2006_display_ops = {
    .gfx_update = cup2006_display_gfx_update,
};

void cup2006_display_create(Cup2006MachineState *s)
{
    DisplaySurface *surface;

    s->disp_con = qemu_graphic_console_create(NULL, 0, &cup2006_display_ops, s);
    surface = qemu_create_displaysurface(CUP2006_DU_CONSOLE_W,
                                         CUP2006_DU_CONSOLE_H);
    qemu_console_set_surface(s->disp_con, surface);
    cup2006_display_update(s);
    trace_cup2006_du_graphic_console_active_area(CUP2006_DU_CONSOLE_W, CUP2006_DU_CONSOLE_H);
}

/* ----------------------------------------------------------------*/
/* DU register file. */
/* ----------------------------------------------------------------*/

static void cup2006_du_start(Cup2006MachineState *s);

static const char *cup2006_du_fmt_name(uint32_t mr)
{    switch (mr & 3) {
    case CUP2006_DU_DDF_8BPP:
        return "8bpp";
    case CUP2006_DU_DDF_16BPP:
        return "RGB565";
    case CUP2006_DU_DDF_ARGB:
        return "ARGB1555";
    default:
        return "YC";
    }
}

/*
 * Report a plane setup once per distinct (base, size, stride, format)
 * change, as "cup2006: EVT du plane=<n> base=... w= h= stride= fmt=".
 */
static void cup2006_du_report(Cup2006MachineState *s, unsigned plane)
{
    unsigned off = CUP2006_DU_PLANE_STRIDE * plane;
    uint32_t mr = cup2006_du_reg(s, off + CUP2006_DU_PNMR);
    uint32_t mwr = cup2006_du_reg(s, off + CUP2006_DU_PNMWR);
    uint32_t dsx = cup2006_du_reg(s, off + CUP2006_DU_PNDSXR) & 0x7ff;
    uint32_t dsy = cup2006_du_reg(s, off + CUP2006_DU_PNDSYR) & 0x3ff;
    uint32_t ddf = mr & 3;
    unsigned bpp = (ddf == CUP2006_DU_DDF_8BPP ||
                    ddf == CUP2006_DU_DDF_YC) ? 1 : 2;
    uint32_t stride = (mwr >> 4) * 16 * bpp;
    uint32_t base = cup2006_du_display_base(s, plane);

    if (!dsx || !dsy || !mwr || !base) {
        return;
    }
    if (s->du_reported[plane].valid &&
        s->du_reported[plane].base == base &&
        s->du_reported[plane].w == dsx &&
        s->du_reported[plane].h == dsy &&
        s->du_reported[plane].stride == stride &&
        s->du_reported[plane].fmt == ddf) {
        return;
    }
    s->du_reported[plane].valid = true;
    s->du_reported[plane].base = base;
    s->du_reported[plane].w = dsx;
    s->du_reported[plane].h = dsy;
    s->du_reported[plane].stride = stride;
    s->du_reported[plane].fmt = ddf;
    trace_cup2006_du_evt_du_plane_base(plane, base, dsx, dsy, stride, cup2006_du_fmt_name(mr));
    /*
     * Start the frame tick on the first complete plane setup so DSSR/DIER
     * and the scan-out are live.
     */
    cup2006_du_start(s);
}

static void cup2006_du_update_irq(Cup2006MachineState *s)
{
    struct intc_source *src = s->disp_intc_src[1];   /* INTEVT 0x780 */
    uint32_t active;

    if (s->du_dier & CUP2006_DU_DIER_MASK) {
        /* Manual 19.3.5: interrupt = DSSR flag AND the matching DIER bit. */
        active = s->du_status & s->du_dier & CUP2006_DU_DIER_MASK;
    } else {
        /* STUB: while DIER = 0, the frame/blank flags raise the line
         * whenever a plane is programmed. */
        active = s->du_status & (CUP2006_DU_DSSR_FRM | CUP2006_DU_DSSR_VBK);
    }
    if (!active || !src) {
        return;
    }
    if (!src->asserted) {
        sh_intc_toggle_source(src, 0, 1);
        s->du_irqs++;
        if (s->du_irqs <= 8 || (s->du_irqs & 0xff) == 0) {
            trace_cup2006_du_intevt_x780_raised_dssr(s->du_irqs, s->du_status, s->du_dier);
        }
    }
}

void cup2006_du_flag_off(void *opaque)
{
    Cup2006MachineState *s = opaque;

    /* The status flags are latched by the guest's DSRCR write on real
     * hardware; the model also drops the pulse so one flag is one frame
     * event. The INTC source itself is deasserted by the guest's
     * 0xFFE00040 ack (cup2006_intc2_write_at). */
    s->du_status &= ~(CUP2006_DU_DSSR_FRM | CUP2006_DU_DSSR_VBK |
                      CUP2006_DU_DSSR_RINT | CUP2006_DU_DSSR_HBK);
}

/*
 * Apply one PnDC switch at a frame boundary (manual 19.4.15): switch to
 * PnDSA1R/PnDSA0R, clear PnDC, report DSSR DFBn (19.3.3). STUB: 10 Hz.
 */
static void cup2006_du_flip_plane(Cup2006MachineState *s, unsigned plane)
{
    unsigned off = CUP2006_DU_PLANE_STRIDE * plane;
    unsigned mr_off = off + CUP2006_DU_PNMR;
    uint32_t mr;

    s->du_displayed[plane] ^= 1;
    mr = s->du_shadow[mr_off >> 2] & ~CUP2006_DU_PNMR_PNDC;
    s->du_shadow[mr_off >> 2] = mr;
    if (s->du_displayed[plane]) {
        s->du_status |= CUP2006_DU_DSSR_DFB(plane);
    } else {
        s->du_status &= ~CUP2006_DU_DSSR_DFB(plane);
    }
    cup2006_du_report(s, plane);
}

void cup2006_du_frame_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned plane;

    /* STUB: frame timing. Frame and vertical-blanking flags are held
     * 5 ms per frame; the interrupt is gated by DIER (manual 19.3.5). */
    for (plane = 1; plane <= CUP2006_DU_PLANES; plane++) {
        if (s->du_flip_pending[plane]) {
            s->du_flip_pending[plane] = false;
            cup2006_du_flip_plane(s, plane);
        }
    }
    s->du_status |= CUP2006_DU_DSSR_FRM | CUP2006_DU_DSSR_VBK;
    cup2006_du_update_irq(s);
    timer_mod(s->du_flag_timer, now + CUP2006_DU_FLAG_HOLD_NS);
    /* Scan out guest memory: the drawing engines write it, not the DU. */
    cup2006_display_update(s);
    timer_mod(s->du_frame_timer, now + CUP2006_DU_FRAME_NS);
}

static void cup2006_du_start(Cup2006MachineState *s)
{
    if (s->du_enabled) {
        return;
    }
    s->du_enabled = true;
    timer_mod(s->du_frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CUP2006_DU_FRAME_NS);
    trace_cup2006_du_display_enabled_dsysr_den();
}

static uint64_t cup2006_du_read_at(Cup2006MachineState *s, hwaddr addr,
                                   unsigned size)
{
    uint32_t off = addr & (CUP2006_DU_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t value;

    if (off == CUP2006_DU_DSSR) {
        value = CUP2006_DU_DSSR_RESERVED | s->du_status;
    } else if (off == CUP2006_DU_DSRCR) {
        value = 0;
    } else if (off == CUP2006_DU_DIER) {
        value = s->du_dier;
    } else {
        value = s->du_shadow[off >> 2];
    }
    value >>= shift;
    value &= size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    return value;
}

static void cup2006_du_write_at(Cup2006MachineState *s, hwaddr addr,
                                uint64_t value, unsigned size)
{
    uint32_t off = addr & (CUP2006_DU_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    uint32_t val = ((uint32_t)value & mask) << shift;
    uint32_t old, neu;

    if (off == CUP2006_DU_DSRCR) {
        /* Write-1-to-clear DSSR flags (manual 19.3.4).  Use the lane-shifted
         * value, so a sub-word write at a non-zero byte offset clears the
         * intended DSSR bits. */
        s->du_status &= ~(val &
                          (CUP2006_DU_DSSR_TVR | CUP2006_DU_DSSR_FRM |
                           CUP2006_DU_DSSR_VBK | CUP2006_DU_DSSR_RINT |
                           CUP2006_DU_DSSR_HBK));
        return;
    }
    old = s->du_shadow[off >> 2];
    neu = (old & ~(mask << shift)) | val;
    s->du_shadow[off >> 2] = neu;
    if (off == CUP2006_DU_DIER) {
        s->du_dier = neu & CUP2006_DU_DIER_MASK;
    } else if (off == CUP2006_DU_DSYSR && (neu & CUP2006_DU_DSYSR_DEN)) {
        cup2006_du_start(s);
    } else if (off >= CUP2006_DU_PLANE_STRIDE &&
               off < CUP2006_DU_PLANE_STRIDE * (CUP2006_DU_PLANES + 1)) {
        unsigned plane = off / CUP2006_DU_PLANE_STRIDE;
        unsigned r = off % CUP2006_DU_PLANE_STRIDE;

        /* Manual display change: a 0->1 PnDC write (PnBM = 00) is
         * performed at the next frame tick, which clears PnDC and
         * updates DSSR DFBn. */
        if (r == CUP2006_DU_PNMR &&
            !(old & CUP2006_DU_PNMR_PNDC) &&
            (neu & CUP2006_DU_PNMR_PNDC) &&
            (neu & CUP2006_DU_PNMR_PNBM) == 0) {
            s->du_flip_pending[plane] = true;
        }
        if (r == CUP2006_DU_PNMR || r == CUP2006_DU_PNMWR ||
            r == CUP2006_DU_PNDSXR || r == CUP2006_DU_PNDSYR ||
            r == CUP2006_DU_PNDPXR || r == CUP2006_DU_PNDPYR ||
            r == CUP2006_DU_PNDSA0R || r == CUP2006_DU_PNDSA1R) {
            cup2006_du_report(s, plane);
        }
    }
}

static uint64_t cup2006_du_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_du_read_at(opaque, addr, size);
}

static void cup2006_du_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    cup2006_du_write_at(opaque, addr, value, size);
}

const MemoryRegionOps cup2006_du_ops = {
    .read = cup2006_du_read,
    .write = cup2006_du_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
