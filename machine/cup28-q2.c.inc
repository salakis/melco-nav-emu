/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

/* HD64413A Q2SD graphics controller. Section numbers refer to its hardware manual. */
#include "cup28-q2-layout.h"

static uint16_t cup28_q2_reg(Cup28MachineState *s, unsigned off)
{
    return lduw_le_p(s->p28_q2_reg + (off & 0x7fe));
}

/* The guest's NK hooks INTC index 7 (INTEVT 0x2e0): the ISR masks
 * CPLD +0x100[31:28], acknowledges +0x70 bit 0, and returns SYSINTR 0x10.
 * Q2 manual 5.2.4: IRQ = SR & IER. */
void cup28_q2_irq(Cup28MachineState *s)
{
    struct intc_source *src = s->p28_q2_intc_src;
    unsigned priority = ldl_le_p(s->p28_dvd_control_reg + 0x100) >> 28;
    bool active = priority && (cup28_q2_reg(s, 2) &
                               cup28_q2_reg(s, 6) & 0xfe80);
    if (!src || active == !!src->asserted) {
        return;
    }
    /* STUB: the OAL entry masks at level 1; use that level for this
     * board line while the CPLD priority encoding is not established. */
    src->prio = 1;
    src->enable_count = src->enable_max = 0;
    sh_intc_toggle_source(src, 0, active ? 1 : -1);
    if (active) {
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
    }
}

void cup28_q2_irq_init(Cup28MachineState *s, SuperHCPU *cpu)
{
    struct intc_desc *desc = cpu->env.intc_handle;
    for (unsigned i = 0; desc && i < desc->nr_sources; i++) {
        if (desc->sources[i].vect == 0x660) {
            s->p28_q2_intc_src = &desc->sources[i];
            s->p28_q2_intc_src->vect = 0x2e0;
            break;
        }
    }
}

static void cup28_q2_scanout(Cup28MachineState *s)
{
    DisplaySurface *surface;
    unsigned w = cup28_q2_reg(s, 0x10) & 0x7ff;
    unsigned h = cup28_q2_reg(s, 0x12) & 0x3ff;
    unsigned sysr = cup28_q2_reg(s, 0);
    unsigned remr = cup28_q2_reg(s, 0x0c);
    unsigned stride = (remr & 0x40) ? 1024 : 512;
    unsigned bpp = (remr & 1) ? 2 : 1;
    unsigned fb = (cup28_q2_reg(s, 2) & 0x100) ? 0x16 : 0x14;
    unsigned start = (cup28_q2_reg(s, fb) & 0x7f) << 16;
    unsigned x, y;

    if (!s->disp_con || !s->p28_q2_mem || !w || !h || w > 1024 || h > 1024) {
        return;
    }
    surface = qemu_console_surface(s->disp_con);
    if (surface_width(surface) != w || surface_height(surface) != h) {
        surface = qemu_create_displaysurface(w, h);
        qemu_console_set_surface(s->disp_con, surface);
    }
    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(surface_data(surface) +
                                   y * surface_stride(surface));
        for (x = 0; x < w; x++) {
            unsigned off = start + cup28_q2_pixel_offset(x, y, stride, bpp);
            uint32_t rgb = 0;
            if ((sysr & 0x6000) == 0x2000 && off + bpp <= 0x800000) {
                if (bpp == 2) {
                    unsigned p = lduw_le_p(s->p28_q2_mem + off);
                    rgb = cup28_q2_rgb565(p);
                } else {
                    unsigned p = s->p28_q2_mem[off];
                    unsigned hi = cup28_q2_reg(s, 0x200 + p * 4);
                    unsigned lo = cup28_q2_reg(s, 0x202 + p * 4);

                    rgb = cup28_q2_palette(hi, lo);
                }
            }
            row[x] = 0xff000000u | rgb;
        }
    }
    qemu_console_update(s->disp_con, 0, 0, w, h);
}

/*
 * Walk the submitted display list and draw into the guest's rendering
 * area (RSAR or the inactive DSAR).  The Q2SD manual gives the command codes (B.2) and
 * parameter layouts (4.1-4.3, B.3).  Only UGM is read (3.5.4).
 *
 * A backward JUMP ends the current frame; the Q2 would loop, but each
 * guest rendering request rebuilds/reuses the same list for one frame.
 */
typedef struct {
    Cup28MachineState *s;
    int x, y;                   /* current pointer */
    int ox, oy;                 /* local offset (LCOFS adds) */
    int x0, y0, x1, y1;         /* clip */
    bool work;                  /* draw-mode WORK (bit 0): mask by work */
} Q2Canvas;

/*
 * Binary work plane (manual 3.3.x work coordinates, 3.5.2(4), fig. 3.42):
 * FTRAP/RFTRAP, LINEW/RLINEW and CLRW write it; POLYGON4A/B/C with the
 * WORK attribute draw only where it is 1 (4.1.1-3 note 2, p.141).
 * STUB: kept inside the model as a 1024x1024 bit array instead of UGM at
 * WSAR, so the CPU cannot see it; work clipping uses the rendering clip.
 */
#define CUP28_Q2_WORK_W 1024
#define CUP28_Q2_WORK_H 1024

static bool cup28_q2_work_get(Cup28MachineState *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= CUP28_Q2_WORK_W || y >= CUP28_Q2_WORK_H) {
        return false;
    }
    return (s->p28_q2_work[(y * CUP28_Q2_WORK_W + x) >> 3] >> (x & 7)) & 1;
}

static void cup28_q2_work_put(Q2Canvas *c, int x, int y, int op)
{
    unsigned i;

    /* op: 0 clear, 1 set, 2 xor */
    if (x < c->x0 || x > c->x1 || y < c->y0 || y > c->y1 ||
        x < 0 || y < 0 || x >= CUP28_Q2_WORK_W || y >= CUP28_Q2_WORK_H) {
        return;
    }
    i = (y * CUP28_Q2_WORK_W + x) >> 3;
    if (op == 2) {
        c->s->p28_q2_work[i] ^= 1u << (x & 7);
    } else if (op == 1) {
        c->s->p28_q2_work[i] |= 1u << (x & 7);
    } else {
        c->s->p28_q2_work[i] &= ~(1u << (x & 7));
    }
}

static void cup28_q2_work_line(Q2Canvas *c, int x0, int y0, int x1, int y1,
                               int op)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0;

    while (n++ < 4096) {
        cup28_q2_work_put(c, x0, y0, op);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* FTRAP/RFTRAP (4.3.1-2): binary EOR paint of the trapezoid between X = DXL
 * and the segment (xa,ya)-(xb,yb); top/bottom bases horizontal, the bottom
 * base not drawn. */
static void cup28_q2_trap(Q2Canvas *c, int dxl, int xa, int ya, int xb, int yb)
{
    int y0 = MIN(ya, yb), y1 = MAX(ya, yb);

    if (ya == yb) {
        return;
    }
    /* Clip exactly to the clip window and the 1024x1024 surface first; the
     * 2048 counters stay only as a safety net so a guest list cannot stall
     * QEMU for minutes. */
    y0 = MAX(MAX(y0, c->y0), 0);
    y1 = MIN(MIN(y1, c->y1), CUP28_Q2_WORK_H - 1);
    for (int y = y0; y < y1 && y - y0 < 2048; y++) {
        int x = xa + (y - ya) * (xb - xa) / (yb - ya);
        int lo = MAX(MAX(MIN(x, dxl), c->x0), 0);
        int hi = MIN(MIN(MAX(x, dxl), c->x1), CUP28_Q2_WORK_W - 1);

        for (int px = lo; px < hi && px - lo < 2048; px++) {
            cup28_q2_work_put(c, px, y, 2);
        }
    }
}

static int cup28_q2_s8(unsigned b)
{
    return (int8_t)b;
}

static void cup28_q2_put(Q2Canvas *c, int x, int y, unsigned idx)
{
    unsigned off;

    if (x < c->x0 || x > c->x1 || y < c->y0 || y > c->y1 ||
        x < 0 || y < 0 || y >= 1024) {
        return;
    }
    if (c->work && !cup28_q2_work_get(c->s, x, y)) {
        return;
    }
    /* Manual 5.2.8 / 5.3.10: drawing can use RSAR independently of
     * scanout. GBM2 inverts the rendering depth relative to GBM0. */
    unsigned remr = cup28_q2_reg(c->s, 0x0c);
    unsigned bpp = ((remr ^ (remr >> 2)) & 1) ? 2 : 1;
    unsigned width = (remr & 0x40) ? 1024 : 512;
    unsigned start = (remr & 0x8000) ? cup28_q2_reg(c->s, 0x98) :
                     cup28_q2_reg(c->s,
                         (cup28_q2_reg(c->s, 2) & 0x100) ? 0x14 : 0x16);

    if (x >= (int)width) {
        return;
    }
    off = ((start & 0x7f) << 16) +
          cup28_q2_pixel_offset(x, y, width, bpp);
    if (off + bpp <= 0x800000) {
        if (bpp == 2) {
            stw_le_p(c->s->p28_q2_mem + off, idx);
        } else {
            c->s->p28_q2_mem[off] = idx;
        }
    }
}

static void cup28_q2_line(Q2Canvas *c, int x0, int y0, int x1, int y1,
                          unsigned idx, uint16_t cmd)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0;
    bool horizontal = dx >= -dy;
    /* §3.5.3(8), Table 3.10: shift copies perpendicular to the major
     * axis. The two FW bits enable their respective side and W2 adds a
     * second pixel on that side. */
    int above = cmd & 8 ? 1 + !!(cmd & 4) : 0;
    int below = cmd & 2 ? 1 + !!(cmd & 1) : 0;

    while (n++ < 4096) {
        for (int offset = -above; offset <= below; offset++) {
            cup28_q2_put(c, x0 + (horizontal ? 0 : offset),
                         y0 + (horizontal ? offset : 0), idx);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = 2 * err;

            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void cup28_q2_fill4(Q2Canvas *c, const int *xs, const int *ys,
                           unsigned idx)
{
    /* Surface geometry for clamping: 1024 rows, 512/1024 columns by REMR. */
    unsigned remr = cup28_q2_reg(c->s, 0x0c);
    unsigned width = (remr & 0x40) ? 1024 : 512;
    bool rect = (ys[0] == ys[1] && ys[2] == ys[3] &&
                 xs[0] == xs[3] && xs[1] == xs[2]) ||
                (xs[0] == xs[1] && xs[2] == xs[3] &&
                 ys[0] == ys[3] && ys[1] == ys[2]);
    int ymin = ys[0], ymax = ys[0], i, j;

    if (rect) {
        /* §4.1.3: rectangular POLYGON4C includes both endpoint pixels
         * (width DX2 - DX1 + 1 in the fast-fill example).  Clamp exactly to
         * the clip window and the surface, so huge guest coordinates cost
         * no loop iterations. */
        int left = MAX(MAX(MIN(xs[0], xs[2]), c->x0), 0);
        int right = MIN(MIN(MAX(xs[0], xs[2]), c->x1), (int)width - 1);
        int top = MAX(MAX(MIN(ys[0], ys[2]), c->y0), 0);
        int bottom = MIN(MIN(MAX(ys[0], ys[2]), c->y1), CUP28_Q2_WORK_H - 1);

        for (int y = top; y <= bottom; y++) {
            for (int x = left; x <= right; x++) {
                cup28_q2_put(c, x, y, idx);
            }
        }
        return;
    }

    for (i = 1; i < 4; i++) {
        if (ys[i] < ymin) {
            ymin = ys[i];
        }
        if (ys[i] > ymax) {
            ymax = ys[i];
        }
    }
    /* Clamp exactly to the clip window and the surface before looping. */
    if (ymin < c->y0) {
        ymin = c->y0;
    }
    if (ymin < 0) {
        ymin = 0;
    }
    if (ymax > c->y1) {
        ymax = c->y1;
    }
    if (ymax > CUP28_Q2_WORK_H - 1) {
        ymax = CUP28_Q2_WORK_H - 1;
    }
    for (int y = ymin; y <= ymax; y++) {
        int xi[4], n = 0, x;

        for (i = 0; i < 4; i++) {
            j = (i + 1) & 3;
            if ((ys[i] <= y && ys[j] > y) ||
                (ys[j] <= y && ys[i] > y)) {
                xi[n++] = xs[i] + (y - ys[i]) * (xs[j] - xs[i]) /
                                  (ys[j] - ys[i]);
            }
        }
        for (i = 1; i < n; i++) {
            int v = xi[i];

            for (j = i; j > 0 && xi[j - 1] > v; j--) {
                xi[j] = xi[j - 1];
            }
            xi[j] = v;
        }
        for (i = 0; i + 1 < n; i += 2) {
            int lo = MAX(MAX(xi[i], c->x0), 0);
            int hi = MIN(MIN(xi[i + 1], c->x1), (int)width - 1);

            for (x = lo; x <= hi; x++) {
                cup28_q2_put(c, x, y, idx);
            }
        }
    }
}

static int cup28_q2_s12(uint16_t w)
{
    return (int16_t)(w << 4) >> 4;
}

static uint16_t cup28_q2_word(Cup28MachineState *s, unsigned off)
{
    /* The Q2 fetches display lists from UGM only (manual 3.5.4). */
    if (off + 2 <= 0x800000) {
        return lduw_le_p(s->p28_q2_mem + off);
    }
    return 0;
}

static bool cup28_q2_triangle_sample(const int *dx, const int *dy,
                                     int a, int b, int x, int y,
                                     int tdx, int tdy, int *sx, int *sy)
{
    int64_t ax = dx[a] - dx[0], ay = dy[a] - dy[0];
    int64_t bx = dx[b] - dx[0], by = dy[b] - dy[0];
    int64_t px = x - dx[0], py = y - dy[0];
    int64_t area = ax * by - ay * bx;
    int64_t wa = px * by - py * bx;
    int64_t wb = ax * py - ay * px;

    if (area < 0) {
        area = -area;
        wa = -wa;
        wb = -wb;
    }
    if (!area || wa < 0 || wb < 0 || wa + wb > area) {
        return false;
    }
    /* Vertices 0, 1, 2, 3 map to TL, TR, BR, BL. */
    int64_t u = a == 1 ? wa + wb : wa;
    int64_t v = a == 1 ? wb : wa + wb;

    *sx = (u * (tdx - 1) + area / 2) / area;
    *sy = (v * (tdy - 1) + area / 2) / area;
    return true;
}

/* Manual 3.3.3, 3.5.2 and 4.1.1-2. Linear rows are TDX pixels
 * wide, not the virtual screen width. Binary sources are LSB first. */
static void cup28_q2_blit(Q2Canvas *c, unsigned source, bool linear,
                          bool binary, unsigned cmd, unsigned color0,
                          unsigned color1, int txs, int tys, int tdx, int tdy,
                          const int *dx, const int *dy)
{
    unsigned remr = cup28_q2_reg(c->s, 0x0c);
    unsigned bpp = ((remr ^ (remr >> 2)) & 1) ? 2 : 1;
    unsigned width = (remr & 0x40) ? 1024 : 512;
    bool transparent = cmd & 0x200, repeat = cmd & 0x100;
    unsigned ssar = cup28_q2_reg(c->s, 0x1c);
    unsigned base = ((ssar & 0x7f) << 16) | (ssar & 0xe000);
    int minx = dx[0], maxx = dx[0], miny = dy[0], maxy = dy[0];
    bool straight = dy[0] == dy[1] && dy[2] == dy[3] &&
                    dx[0] == dx[3] && dx[1] == dx[2];
    bool quarter_turn = dx[0] == dx[1] && dx[2] == dx[3] &&
                        dy[0] == dy[3] && dy[1] == dy[2];
    int x, y, i, w, h;

    for (i = 1; i < 4; i++) {
        minx = MIN(minx, dx[i]); maxx = MAX(maxx, dx[i]);
        miny = MIN(miny, dy[i]); maxy = MAX(maxy, dy[i]);
    }
    w = maxx - minx + 1;
    h = maxy - miny + 1;
    if (w <= 0 || h <= 0 || tdx <= 0 || tdy <= 0) {
        return;
    }
    /* STUB: inverse sampling replaces Q2's diagonal scan and gap fill.
     * For skewed quads, interpolate two triangles along corner 0 to 2.
     * STYL repeat is implemented for rectangles. */
    for (y = MAX(miny, c->y0); y <= MIN(MIN(maxy, c->y1), 1023); y++) {
        int ry = dy[0] == miny ? y - miny : maxy - y;

        for (x = MAX(minx, c->x0); x <= MIN(MIN(maxx, c->x1), (int)width - 1); x++) {
            int rx = dx[0] == minx ? x - minx : maxx - x;
            int sx, sy;
            unsigned off, pixel;

            if (straight) {
                sx = repeat ? rx % tdx : rx * tdx / w;
                sy = repeat ? ry % tdy : ry * tdy / h;
            } else if (quarter_turn) {
                sx = repeat ? ry % tdx : ry * tdx / h;
                sy = repeat ? rx % tdy : rx * tdy / w;
            } else if (!cup28_q2_triangle_sample(dx, dy, 1, 2, x, y,
                                                tdx, tdy, &sx, &sy) &&
                       !cup28_q2_triangle_sample(dx, dy, 2, 3, x, y,
                                                tdx, tdy, &sx, &sy)) {
                continue;
            }
            if (binary) {
                off = source + sy * (tdx / 8) + sx / 8;
            } else if (linear) {
                off = source + (sy * tdx + sx) * bpp;
            } else {
                off = base + cup28_q2_pixel_offset(txs + sx, tys + sy,
                                                   width, bpp);
            }
            if (off + (binary ? 1 : bpp) > 0x800000) {
                continue;
            }
            if (binary) {
                unsigned bit = (c->s->p28_q2_mem[off] >> (sx & 7)) & 1;
                if (transparent && !bit) {
                    continue;
                }
                pixel = bit ? color1 : color0;
            } else {
                pixel = bpp == 2 ? lduw_le_p(c->s->p28_q2_mem + off) :
                                  c->s->p28_q2_mem[off];
                if (transparent && !pixel) {
                    continue;
                }
            }
            cup28_q2_put(c, x, y, pixel);
        }
    }
}

static void cup28_q2_exec_list(Cup28MachineState *s, unsigned dl)
{
    unsigned pc = dl, n = 0, depth = 0;
    unsigned stack[8];
    unsigned stack_nv[8];       /* nvisited at each GOSUB, for RET to unmark */
    unsigned visited[256];
    unsigned nvisited = 0, vi;
    bool ended = false;
    Q2Canvas c = { .s = s };

    unsigned last_code = 0xff;

    while (n++ < 8192 && pc + 2 <= 0x800000) {
        uint16_t cmd;
        unsigned code;
        unsigned step = 0;

        for (vi = 0; vi < nvisited; vi++) {
            if (visited[vi] == pc) {
                ended = true;
                break;
            }
        }
        if (ended) {
            break;
        }
        if (nvisited < ARRAY_SIZE(visited)) {
            visited[nvisited++] = pc;
        }
        cmd = cup28_q2_word(s, pc);
        code = cmd >> 11;
        /* §3.5.3(3): SCLIP always applies. Bit 7 enables UCLIP as well. */
        c.x0 = c.y0 = 0;
        c.x1 = cup28_q2_reg(s, 0x90);
        c.y1 = cup28_q2_reg(s, 0x92);
        if (cmd & 0x80) {
            c.x0 = MAX(0, (int16_t)cup28_q2_reg(s, 0x88));
            c.y0 = MAX(0, (int16_t)cup28_q2_reg(s, 0x8a));
            c.x1 = MIN(c.x1, (int16_t)cup28_q2_reg(s, 0x8c));
            c.y1 = MIN(c.y1, (int16_t)cup28_q2_reg(s, 0x8e));
        }
        /* Fig. 3.43: WORK is draw-mode bit 0 for POLYGON4A/B/C only. */
        c.work = code <= 0x02 && (cmd & 1);
        switch (code) {
        case 0x00:                       /* POLYGON4A (multi-valued source) */
        case 0x01: {                     /* POLYGON4B (binary source) */
            bool binary = code == 0x01;
            bool lni = binary || (cmd & 0x04);
            unsigned src = 0;
            int txs, tys, tdx, tdy, i;
            int dx[4], dy[4];

            if (lni) {
                /* Linear source address: H is the first word (A22-A13),
                 * L the second (A12-A0).  The guest-built list uses this
                 * order. */
                src = (cup28_q2_word(s, pc + 2) << 13) |
                      (cup28_q2_word(s, pc + 4) & 0x1ffe);
                if (cmd & 0x40) { /* REL, signed 23-bit byte displacement */
                    int displacement = (int)(src << 9) >> 9;
                    src = (pc + displacement) & 0x7fffff;
                }
                txs = 0;
                tys = 0;
                tdx = cup28_q2_word(s, pc + 6) & 0x3ff;
                tdy = cup28_q2_word(s, pc + 8) & 0x3ff;
            } else {
                txs = cup28_q2_word(s, pc + 2) & 0x3ff;
                tys = cup28_q2_word(s, pc + 4) & 0x3ff;
                tdx = cup28_q2_word(s, pc + 6) & 0x3ff;
                tdy = cup28_q2_word(s, pc + 8) & 0x3ff;
            }
            for (i = 0; i < 4; i++) {
                dx[i] = c.ox + cup28_q2_s12(cup28_q2_word(s,
                                                   pc + 10 + i * 4));
                dy[i] = c.oy + cup28_q2_s12(cup28_q2_word(s,
                                                   pc + 12 + i * 4));
            }
            cup28_q2_blit(&c, src, lni, binary, cmd,
                          binary ? cup28_q2_word(s, pc + 26) : 0,
                          binary ? cup28_q2_word(s, pc + 28) : 0,
                          txs, tys, tdx, tdy, dx, dy);
            c.x = dx[3];
            c.y = dy[3];
            step = binary ? 14 : 12;
            break;
        }
        case 0x02: {                     /* POLYGON4C (specified colour) */
            int xs[4], ys[4], i;
            unsigned color = cup28_q2_word(s, pc + 18);

            for (i = 0; i < 4; i++) {
                xs[i] = c.ox + cup28_q2_s12(cup28_q2_word(s,
                                                           pc + 2 + i * 4));
                ys[i] = c.oy + cup28_q2_s12(cup28_q2_word(s,
                                                           pc + 4 + i * 4));
            }
            cup28_q2_fill4(&c, xs, ys, color);
            c.x = xs[3];
            c.y = ys[3];
            step = 9;
            break;
        }
        case 0x08: case 0x09: {          /* FTRAP/RFTRAP, 4.3.1-2 */
            unsigned cnt = cup28_q2_word(s, pc + 2);
            int dxl = c.ox + cup28_q2_s12(cup28_q2_word(s, pc + 4));
            bool relative = code == 0x09;
            int px = c.x, py = c.y, fx, fy;
            unsigned i;

            if (cnt > 4096) {
                cnt = 4096;
            }
            for (i = 0; i < cnt; i++) {
                int x, y;

                if (relative) {
                    uint16_t w = cup28_q2_word(s, pc + 6 + i * 2);
                    x = px + cup28_q2_s8(w >> 8);
                    y = py + cup28_q2_s8(w & 0xff);
                } else {
                    x = c.ox + cup28_q2_s12(cup28_q2_word(s,
                                                       pc + 6 + i * 4));
                    y = c.oy + cup28_q2_s12(cup28_q2_word(s,
                                                       pc + 8 + i * 4));
                }
                if (i || relative) {
                    cup28_q2_trap(&c, dxl, px, py, x, y);
                }
                if (!i) {
                    fx = x; fy = y;
                }
                px = x;
                py = y;
            }
            if (cmd & 0x08) {            /* EDG: edge line, value = EOS */
                int ex = relative ? c.x : fx, ey = relative ? c.y : fy;
                int qx = ex, qy = ey;

                for (i = 0; i < cnt; i++) {
                    int x, y;

                    if (relative) {
                        uint16_t w = cup28_q2_word(s, pc + 6 + i * 2);
                        x = qx + cup28_q2_s8(w >> 8);
                        y = qy + cup28_q2_s8(w & 0xff);
                    } else {
                        x = c.ox + cup28_q2_s12(cup28_q2_word(s,
                                                           pc + 6 + i * 4));
                        y = c.oy + cup28_q2_s12(cup28_q2_word(s,
                                                           pc + 8 + i * 4));
                    }
                    if (i || relative) {
                        cup28_q2_work_line(&c, qx, qy, x, y,
                                           (cmd & 0x10) ? 1 : 0);
                    }
                    qx = x;
                    qy = y;
                }
            }
            if (cnt) {
                c.x = px;
                c.y = py;
            }
            step = 2 + (relative ? cnt : 2 * cnt);
            break;
        }
        case 0x0e: case 0x0f: {          /* PLINE/RPLINE, 4.2.3-4 */
            /* LINE COLOR 0/1, source H/L (H << 13 | L, as POLYGON4B),
             * LPPT | TDX, n, vertices.  Pixels along the line take the
             * binary source pattern (LSB first, period TDX, start LPPT):
             * 1 = COLOR1, 0 = COLOR0 or transparent (TRNS, bit 9).
             * STUB: optional solid edges (EDG1 and EDG2) are unmodelled. */
            bool relative = code == 0x0f;
            unsigned color0 = cup28_q2_word(s, pc + 2);
            unsigned color1 = cup28_q2_word(s, pc + 4);
            unsigned src = (cup28_q2_word(s, pc + 6) << 13) |
                           (cup28_q2_word(s, pc + 8) & 0x1fff);
            unsigned lp = cup28_q2_word(s, pc + 10);
            unsigned tdx = lp & 0x3ff, k = (lp >> 10) & 0x1f;
            unsigned cnt = cup28_q2_word(s, pc + 12);
            bool trns = cmd & 0x200;
            int px = c.x, py = c.y;
            unsigned i;

            if (cnt > 4096) {
                cnt = 4096;
            }
            if (!tdx) {
                tdx = 1;
            }
            for (i = 0; i < cnt; i++) {
                int x, y;

                if (relative) {
                    uint16_t w = cup28_q2_word(s, pc + 14 + i * 2);
                    x = px + cup28_q2_s8(w >> 8);
                    y = py + cup28_q2_s8(w & 0xff);
                } else {
                    x = c.ox + cup28_q2_s12(cup28_q2_word(s,
                                                       pc + 14 + i * 4));
                    y = c.oy + cup28_q2_s12(cup28_q2_word(s,
                                                       pc + 16 + i * 4));
                }
                if (i || relative) {
                    int ax = px, ay = py;
                    int ddx = abs(x - ax), sx = ax < x ? 1 : -1;
                    int ddy = abs(y - ay), sy = ay < y ? 1 : -1;
                    int err = 0;

                    for (;;) {
                        unsigned b = k % tdx, off = src + b / 8, bit = 0;

                        if (off < 0x800000) {
                            bit = (s->p28_q2_mem[off] >> (b & 7)) & 1;
                        }
                        if (bit || !trns) {
                            cup28_q2_put(&c, ax, ay, bit ? color1 : color0);
                        }
                        k++;
                        if (ax == x && ay == y) {
                            break;
                        }
                        /* §§4.2.3–4 and appendix C: patterned lines choose
                         * the nearer orthogonal pixel at each step. */
                        if (ay == y ||
                            (ax != x && abs(err + ddy) <= abs(err - ddx))) {
                            err += ddy;
                            ax += sx;
                        } else {
                            err -= ddx;
                            ay += sy;
                        }
                    }
                }
                px = x;
                py = y;
            }
            if (cnt) {
                c.x = px;
                c.y = py;
            }
            step = 6 + (relative ? cnt : 2 * cnt);
            break;
        }
        case 0x0a: case 0x0b:            /* LINEW/RLINEW, 4.4.1-2 */
        case 0x0c: case 0x0d: {          /* LINE/RLINE, 4.2.1-2 */
            bool work = code <= 0x0b;
            bool relative = code & 1;
            unsigned base = work ? 2 : 4; /* LINE has a LINE COLOR word */
            unsigned cnt = cup28_q2_word(s, pc + base);
            unsigned color = cup28_q2_word(s, pc + 2);
            int op = (cmd & 0x10) ? 1 : 0;   /* LINEW value = EOS */
            unsigned i;
            int px = c.x, py = c.y;

            if (cnt > 4096) {
                cnt = 4096;
            }
            for (i = 0; i < cnt; i++) {
                int x, y;

                if (relative) {
                    uint16_t w = cup28_q2_word(s, pc + base + 2 + i * 2);
                    x = px + cup28_q2_s8(w >> 8);
                    y = py + cup28_q2_s8(w & 0xff);
                } else {
                    x = c.ox + cup28_q2_s12(cup28_q2_word(s,
                                                   pc + base + 2 + i * 4));
                    y = c.oy + cup28_q2_s12(cup28_q2_word(s,
                                                   pc + base + 4 + i * 4));
                }
                if (i || relative) {
                    if (work) {
                        cup28_q2_work_line(&c, px, py, x, y, op);
                    } else {
                        cup28_q2_line(&c, px, py, x, y, color, cmd);
                    }
                }
                px = x;
                py = y;
            }
            if (cnt) {
                c.x = px;
                c.y = py;
            }
            step = (base / 2) + (relative ? cnt : 2 * cnt);
            break;
        }
        case 0x10: case 0x11:            /* MOVE/RMOVE */
            if (code == 0x10) {
                c.x = cup28_q2_s12(cup28_q2_word(s, pc + 2));
                c.y = cup28_q2_s12(cup28_q2_word(s, pc + 4));
            } else {
                c.x += cup28_q2_s12(cup28_q2_word(s, pc + 2));
                c.y += cup28_q2_s12(cup28_q2_word(s, pc + 4));
            }
            step = 2;
            break;
        case 0x12: case 0x13:            /* LCOFS/RLCOFS, manual 4.5.3-4 */
            if (code == 0x12) {
                c.ox = c.oy = 0;
            }
            c.ox += (int16_t)cup28_q2_word(s, pc + 2);
            c.oy += (int16_t)cup28_q2_word(s, pc + 4);
            step = 2;
            break;
        case 0x14: {                     /* CLRW */
            int xmin = c.ox + cup28_q2_s12(cup28_q2_word(s, pc + 2));
            int ymin = c.oy + cup28_q2_s12(cup28_q2_word(s, pc + 4));
            int xmax = c.ox + cup28_q2_s12(cup28_q2_word(s, pc + 6));
            int ymax = c.oy + cup28_q2_s12(cup28_q2_word(s, pc + 8));
            int x, y;

            /* 4.3.3: CLRW zeroes the work coordinates, not the frame. */
            for (y = ymin; y <= ymax && y - ymin < 2048; y++) {
                for (x = xmin; x <= xmax && x - xmin < 2048; x++) {
                    cup28_q2_work_put(&c, x, y, 0);
                }
            }
            step = 4;
            break;
        }
        case 0x15:                       /* UCLIP */
            for (unsigned i = 0; i < 4; i++) {
                stw_le_p(s->p28_q2_reg + 0x88 + i * 2,
                         cup28_q2_s12(cup28_q2_word(s, pc + 2 + i * 2)));
            }
            step = 4;
            break;
        case 0x16: {                     /* WPR */
            /* Manual 4.5.7: RN is a word register number, not a byte
             * offset (e.g. RN 0x0b selects DSAR1 at 0x16). */
            unsigned rn = (cup28_q2_word(s, pc + 2) & 0x3ff) * 2;
            uint16_t data = cup28_q2_word(s, pc + 4);

            if (rn < 0x400) {
                stw_le_p(s->p28_q2_reg + rn, data);
            }
            step = 2;
            break;
        }
        case 0x17:                       /* SCLIP */
            stw_le_p(s->p28_q2_reg + 0x90,
                     cup28_q2_word(s, pc + 2) & 0x7ff);
            stw_le_p(s->p28_q2_reg + 0x92,
                     cup28_q2_word(s, pc + 4) & 0x3ff);
            step = 2;
            break;
        case 0x18: {                     /* JUMP */
            /* H is A22-A13 (10 bits), L is A12-A0 (13 bits, A0 fixed 0):
             * (H << 13) | L (manual p.189). */
            unsigned t = (cup28_q2_word(s, pc + 2) << 13) |
                         (cup28_q2_word(s, pc + 4) & 0x1ffe);

            if (t + 2 > 0x800000) {
                ended = true;
                break;
            }
            pc = t;
            continue;
        }
        case 0x19: {                     /* GOSUB */
            unsigned t = (cup28_q2_word(s, pc + 2) << 13) |
                         (cup28_q2_word(s, pc + 4) & 0x1ffe);

            if (depth >= 8 || t + 2 > 0x800000) {
                ended = true;
                break;
            }
            stack[depth] = pc + 6;
            stack_nv[depth] = nvisited;
            depth++;
            pc = t;
            continue;
        }
        case 0x1a: step = 2; break;      /* VBKEM */
        case 0x1b:                       /* RET */
            if (depth > 0) {
                depth--;
                pc = stack[depth];
                /* Visited marks are scoped to a call frame: unmark what the
                 * subroutine added, so calling it a second time is not
                 * mistaken for a loop. */
                if (stack_nv[depth] <= nvisited) {
                    nvisited = stack_nv[depth];
                }
                continue;
            }
            ended = true;
            break;
        case 0x1e: step = 2; break;      /* NOP3 */
        case 0x1f: ended = true; break;  /* TRAP */
        default:
            ended = true;
            break;
        }
        if (ended) {
            last_code = code;
            break;
        }
        pc += 2 + step * 2;
    }
    /* List completion raises TRA. Only the guest enables display output. */
    stw_le_p(s->p28_q2_reg + 2, cup28_q2_reg(s, 2) | 0x0400);
    stw_le_p(s->p28_q2_reg, cup28_q2_reg(s, 0) & ~0x100);
    s->p28_q2_list_done = last_code == 0x1f;
    cup28_q2_irq(s);
}

void cup28_q2_tick(void *opaque)
{
    Cup28MachineState *s = opaque;
    unsigned sysr = cup28_q2_reg(s, 0);
    unsigned sr = cup28_q2_reg(s, 2);

    /* STUB: fixed 60-Hz frame timing; guest memory/registers determine
     * every pixel. SR/IER drive the board IRQ through the CPLD mask. */
    if (!(sysr & 0x4000)) {
        sr |= 0x4800; /* FRM | VBK: a display frame has elapsed */
        /* §3.3.4: auto rendering flips once at the frame after TRAP. */
        if ((sysr & 0xc0) == 0x40 && s->p28_q2_list_done) {
            sr ^= 0x100;
        }
        s->p28_q2_list_done = false;
        if ((sysr & 0x200) && (sysr & 0xc0) == 0x80) {
            sr ^= 0x100;
            stw_le_p(s->p28_q2_reg, sysr & ~0x200);
        }
        stw_le_p(s->p28_q2_reg + 2, sr);
    }
    cup28_q2_irq(s);
    cup28_q2_scanout(s);
    timer_mod(s->p28_q2_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667);
}

static uint64_t cup28_q2_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    return cup28_register_read(s->p28_q2_reg, addr, size);
}

static void cup28_q2_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    Cup28MachineState *s = opaque;
    unsigned off = addr & ~1;

    if (off == 2) { /* SR is read only */
        return;
    }
    if (off == 4) { /* SRCR, manual 5.2.3: write-one-to-clear flags */
        /* A byte write carries its value in one lane of the 16-bit register;
         * shift it into the register's bit positions before masking. */
        if (size == 1) {
            value <<= 8 * (addr & 1);
        }
        stw_le_p(s->p28_q2_reg + 2,
                 cup28_q2_reg(s, 2) & ~(value & 0xfe80));
        cup28_q2_irq(s);
        return;
    }
    if (off == 0) {
        /* Manual 5.2.1: DC is cleared by frame switching only. A software
         * SYSR write cannot cancel an outstanding change — including a byte
         * write to the high byte, where DC is bit 1 of that byte. */
        if (cup28_q2_reg(s, 0) & 0x200) {
            if (size == 1) {
                value |= (addr & 1) ? 0x02 : 0;
            } else {
                value |= 0x200;
            }
        }
    }
    cup28_register_write(s->p28_q2_reg, addr, value, size);
    if (off == 0 && (cup28_q2_reg(s, 0) & 0x100)) {
        unsigned dl = ((cup28_q2_reg(s, 0x18) & 0x7f) << 16) |
                      (cup28_q2_reg(s, 0x1a) & 0xffe0);
        s->p28_q2_list_done = false;
        cup28_q2_exec_list(s, dl);
    }
    cup28_q2_irq(s);
}

const MemoryRegionOps cup28_q2_ops = {
    .read = cup28_q2_read,
    .write = cup28_q2_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
