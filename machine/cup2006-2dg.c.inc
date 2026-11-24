/*
 * CU-P2006 machine: 2DG drawing engine (display lists) and plane/band diagnostics.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static uint64_t cup2006_dg_read_at(Cup2006MachineState *s, hwaddr base,
                                   hwaddr addr, unsigned size)
{
    hwaddr off = addr & (CUP2006_DG_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t value;

    value = s->dg_reg[off >> 2];
    if (size < 4) {
        value >>= shift;
        value &= (1u << (size * 8)) - 1;
    }
    return value;
}

/*
 * 2DG list execution: opcode = b31..b24; WPR/JUMP/GOSUB/RET/LCOFS/TRAP
 * control flow (SH7764 G2D manual R01UH0360EJ0300 sec. 23). STUB: only
 * modelled opcodes execute; the rest are logged and skipped.
 */
#define CUP2006_DG_WALK_MAX      100000
/* The panel surface the DU scanout presents: 512 pixels per 1024-byte row
 * at 16 bpp, 234 lines.  Every drawing loop clips to it exactly (in addition
 * to the guest's clip window), so one packet cannot loop billions of times
 * on a huge SCLMAR; CUP2006_DG_WALK_MAX stays as the packet-level safety net. */
#define CUP2006_DG_SURF_W 512
#define CUP2006_DG_SURF_H 234

uint8_t *cup2006_dg_ram_ptr(Cup2006MachineState *s, uint32_t addr,
                                   uint32_t len)
{
    uint32_t pa = addr & 0x1fffffffu;   /* P1/P2 alias of the same RAM */
    uint64_t size = memory_region_size(&s->ram);

    if ((uint64_t)pa + len > size) {
        return NULL;
    }
    return memory_region_get_ram_ptr(&s->ram) + pa;
}

static bool cup2006_dg_fetch(Cup2006MachineState *s, uint32_t addr,
                             uint32_t *out)
{
    uint8_t *p = cup2006_dg_ram_ptr(s, addr, 4);

    if (!p) {
        return false;
    }
    *out = ldl_le_p(p);
    return true;
}

static void cup2006_dg_skip(Cup2006MachineState *s, uint32_t pc,
                            const char *why, uint32_t word)
{
    s->dg_skipped++;
    trace_cup2006_2dg_dg_skip_pc_word(pc, word, why);
}

/*
 * Execute one BITBLT packet. lx/ly are the current LCOFS local offset.
 * Returns the number of pixels written.
 */
static uint64_t cup2006_dg_bitblt(Cup2006MachineState *s, uint32_t pc,
                                  uint32_t w0, const uint32_t *f,
                                  int lx, int ly)
{
    uint32_t op = w0 >> 24;
    uint32_t mode = w0 & 0x00ffffffu;
    uint32_t allowed = op == 0xa0 ? CUP2006_DG_MODE_CLIP
                                  : (CUP2006_DG_MODE_CLIP |
                                     CUP2006_DG_MODE_STRANS |
                                     CUP2006_DG_MODE_SS);
    int lw = f[2] >> 16, rw = f[2] & 0xffff;
    int th = f[3] >> 16, bh = f[3] & 0xffff;
    int bxc = (int16_t)(f[4] >> 16), byc = (int16_t)(f[4] & 0xffff);
    int dx0 = bxc + lx - lw, dy0 = byc + ly - th;
    int nx = lw + rw + 1, ny = th + bh + 1;
    int x, y, xmin, ymin, xmax, ymax;
    uint32_t sc, dstrr, stcr = 0, stcr_mask = 0xffff;
    bool ss = (mode & CUP2006_DG_MODE_SS) != 0;
    bool strans = (mode & CUP2006_DG_MODE_STRANS) != 0;
    uint32_t src_pa = 0, fill = 0;
    int src_pitch = nx;
    int txs = 0, tys = 0;
    uint64_t written = 0;

    if (f[0] != CUP2006_DG_ROP_COPY) {
        cup2006_dg_skip(s, pc, "ROP not modelled (only 0xCC)", w0);
        return 0;
    }
    if (mode & ~allowed) {
        cup2006_dg_skip(s, pc, "draw-mode bits not modelled", w0);
        return 0;
    }
    if (mode & CUP2006_DG_MODE_RCLIP) {
        cup2006_dg_skip(s, pc, "relative clip not modelled", w0);
        return 0;
    }
    if (!(s->dg_reg[CUP2006_DG_RCLR >> 2] & CUP2006_DG_RCLR_16BPP)) {
        cup2006_dg_skip(s, pc, "RCLR does not select 16-bit/pixel", w0);
        return 0;
    }
    if (nx <= 0 || ny <= 0) {
        cup2006_dg_skip(s, pc, "empty destination rectangle", w0);
        return 0;
    }

    sc = s->dg_reg[CUP2006_DG_SCLMAR >> 2];
    xmin = 0;
    ymin = 0;
    xmax = (sc >> 16) & 0xffff;
    ymax = sc & 0xffff;
    if (mode & CUP2006_DG_MODE_CLIP) {
        uint32_t mn = s->dg_reg[CUP2006_DG_UCLMIR >> 2];
        uint32_t mx = s->dg_reg[CUP2006_DG_UCLMAR >> 2];

        /* The user clip is in screen coordinates: no local offset (23.1.4). */
        xmin = MAX(xmin, (int)(mn >> 16));
        ymin = MAX(ymin, (int)(mn & 0xffff));
        xmax = MIN(xmax, (int)(mx >> 16));
        ymax = MIN(ymax, (int)(mx & 0xffff));
    }
    xmax = MIN(xmax, CUP2006_DG_SURF_W - 1);
    ymax = MIN(ymax, CUP2006_DG_SURF_H - 1);
    xmin = MAX(xmin, dx0);
    ymin = MAX(ymin, dy0);
    xmax = MIN(xmax, dx0 + nx - 1);
    ymax = MIN(ymax, dy0 + ny - 1);
    if (xmin > xmax || ymin > ymax) {
        return 0;                   /* fully clipped */
    }

    dstrr = s->dg_reg[CUP2006_DG_DSTRR >> 2];
    if (op == 0xa2) {
        if (ss) {
            uint32_t sstrr = s->dg_reg[CUP2006_DG_SSTRR >> 2];

            src_pa = s->dg_reg[CUP2006_DG_SRC >> 2];
            src_pitch = sstrr;
            txs = f[1] >> 16;
            tys = f[1] & 0xffff;
        } else {
            src_pa = f[1];
        }
        if (src_pitch <= 0) {
            cup2006_dg_skip(s, pc, "zero source pitch", w0);
            return 0;
        }
        if (strans) {
            /* ARGB1555 does not compare the alpha bit (SH7764 23.3.3(1)). */
            if (s->dg_reg[CUP2006_DG_RCLR >> 2] & (1u << 21)) {
                stcr_mask = 0x7fff;
            }
            stcr = s->dg_reg[CUP2006_DG_STCR >> 2] & stcr_mask;
        }
    } else {
        fill = f[1] & 0xffff;
    }


    for (y = ymin; y <= ymax; y++) {
        int row = y - dy0;
        uint32_t dst_base = s->dg_reg[CUP2006_DG_DEST >> 2] +
                            ((uint32_t)y * dstrr + xmin) * 2;

        for (x = xmin; x <= xmax; x++) {
            uint32_t val;
            uint8_t *dp = cup2006_dg_ram_ptr(s, dst_base +
                                             (uint32_t)(x - xmin) * 2, 2);

            if (!dp) {
                cup2006_dg_skip(s, pc, "destination outside RAM", w0);
                return written;
            }
            if (op == 0xa0) {
                val = fill;
            } else {
                int col = x - dx0;
                uint32_t spa = src_pa +
                               (uint32_t)((row + tys) * src_pitch + col + txs) * 2;
                uint8_t *sp = cup2006_dg_ram_ptr(s, spa, 2);

                if (!sp) {
                    cup2006_dg_skip(s, pc, "source outside RAM", w0);
                    return written;
                }
                val = lduw_le_p(sp);
                if (strans && (val & stcr_mask) == stcr) {
                    continue;
                }
            }
            stw_le_p(dp, val);
            written++;
        }
    }
    s->dg_blits++;
    return written;
}

#define CUP2006_DG_MODE_WORK   (1u << 9)   /* POLYGON4/BITBLT only */
#define CUP2006_DG_MODE_BLKE   (1u << 5)

/*
 * POLYGON4C (op 0x80, SH7764 23.2.1(3)): [0x80|mode][0|Color][DX1|DY1]..
 * [DX4|DY4], six words; quad filled with Color, scanline even-odd, clipped
 * like BITBLT.
 */
static bool cup2006_dg_work_get(Cup2006MachineState *s, int x, int y);
/*
 * Coordinate transformation (SH7764 R01UH0360EJ0300 23.1.2(2), 23.3): each
 * vertex goes through the 3x3 float matrix at +0x104..+0x124 with GTROFSX/Y;
 * the hardware's 16.16 fixed point is approximated with doubles.
 */
#define CUP2006_DG_GTRCR       0x100
#define CUP2006_DG_MTRAR       0x104
#define CUP2006_DG_GTROFSX     0x128
#define CUP2006_DG_GTROFSY     0x12c
#define CUP2006_DG_MODE_MTRE   (1u << 15)

static double cup2006_dg_mtr(Cup2006MachineState *s, unsigned i)
{
    union { uint32_t u; float f; } v;

    v.u = s->dg_reg[(CUP2006_DG_MTRAR >> 2) + i];
    return v.f;
}

static void cup2006_dg_xf(Cup2006MachineState *s, uint32_t mode, int *x,
                          int *y)
{
    uint32_t gtr = s->dg_reg[CUP2006_DG_GTRCR >> 2];
    double X = *x, Y = *y, tx, ty, w, rx, ry;

    /*
     * STUB: MTRE alone enables the transform here (GTRCR.GTE is not set by
     * the guest).
     */
    if (!(mode & CUP2006_DG_MODE_MTRE)) {
        return;
    }
    tx = cup2006_dg_mtr(s, 0) * X + cup2006_dg_mtr(s, 1) * Y +
         cup2006_dg_mtr(s, 2);
    ty = cup2006_dg_mtr(s, 3) * X + cup2006_dg_mtr(s, 4) * Y +
         cup2006_dg_mtr(s, 5);
    if (gtr & 1) {
        rx = tx;
        ry = ty;
    } else {
        w = cup2006_dg_mtr(s, 6) * X + cup2006_dg_mtr(s, 7) * Y +
            cup2006_dg_mtr(s, 8);
        if (w < 1.0 / 65536) {
            w = 1.0 / 65536;            /* the hardware keeps W >= 1 LSB */
        }
        rx = tx / w + (int16_t)s->dg_reg[CUP2006_DG_GTROFSX >> 2];
        ry = ty / w + (int16_t)s->dg_reg[CUP2006_DG_GTROFSY >> 2];
    }
    /* NaN/inf would make the int conversion undefined; clamp to 0 first. */
    if (!isfinite(rx)) {
        rx = 0;
    }
    if (!isfinite(ry)) {
        ry = 0;
    }
    *x = (int)MAX(-32768.0, MIN(32767.0, floor(rx)));
    *y = (int)MAX(-32768.0, MIN(32767.0, floor(ry)));
}

/* Clip segment (x0,y0)-(x1,y1) to [xmin,xmax]x[ymin,ymax] (Liang-Barsky);
 * false if nothing is left. Keeps long transformed lines cheap. */
static bool cup2006_dg_clipseg(int *x0, int *y0, int *x1, int *y1,
                               int xmin, int ymin, int xmax, int ymax)
{
    double t0 = 0, t1 = 1, dx = *x1 - *x0, dy = *y1 - *y0;
    double p[4] = { -dx, dx, -dy, dy };
    double q[4] = { *x0 - xmin, xmax - *x0, *y0 - ymin, ymax - *y0 };
    double ax = *x0, ay = *y0;
    int i;

    for (i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) {
                return false;
            }
        } else {
            double r = q[i] / p[i];

            if (p[i] < 0) {
                t0 = MAX(t0, r);
            } else {
                t1 = MIN(t1, r);
            }
        }
    }
    if (t0 > t1) {
        return false;
    }
    *x0 = (int)floor(ax + t0 * dx + 0.5);
    *y0 = (int)floor(ay + t0 * dy + 0.5);
    *x1 = (int)floor(ax + t1 * dx + 0.5);
    *y1 = (int)floor(ay + t1 * dy + 0.5);
    return true;
}

/*
 * Alpha blend (SH7764 23.3.3(16), ALPHR 23.4): dst = src*ALPH/255 +
 * dst*(1-ALPH/255) per colour channel; the ARGB1555 A bit is not blended.
 */
static uint16_t cup2006_dg_blend(uint16_t src, uint16_t dst, unsigned a,
                                 bool argb)
{
    unsigned sr, sg, sb, dr, dg, db, gb = argb ? 5 : 6;
    unsigned gm = (1u << gb) - 1;

    sb = src & 31; db = dst & 31;
    sg = (src >> 5) & gm; dg = (dst >> 5) & gm;
    sr = (src >> (5 + gb)) & 31; dr = (dst >> (5 + gb)) & 31;
    sb = (sb * a + db * (255 - a) + 127) / 255;
    sg = (sg * a + dg * (255 - a) + 127) / 255;
    sr = (sr * a + dr * (255 - a) + 127) / 255;
    return (uint16_t)((argb ? (src & 0x8000) : 0) | (sr << (5 + gb)) |
                      (sg << 5) | sb);
}

/*
 * The pixel spans a quadrilateral covers on scanline y, clipped to
 * [xmin, xmax], as sorted, non-overlapping inclusive ranges [sa[i], sb[i]];
 * returns their number.  Edge crossings use the half-open rule (an edge
 * covers lo <= y < hi), so a vertex between two edges is counted once and
 * the row through it is not lost; vertices and horizontal edges lying on the
 * row are added as spans of their own, so the top and bottom rows are drawn.
 */
#define CUP2006_DG_QUAD_SPANS 12

static int cup2006_dg_quad_spans(const int *px, const int *py, int y,
                                 int xmin, int xmax, int *sa, int *sb)
{
    int xs[4], n = 0, ns = 0, i, k;
    int ra[CUP2006_DG_QUAD_SPANS], rb[CUP2006_DG_QUAD_SPANS];

    for (i = 0; i < 4; i++) {
        int x0 = px[i], y0 = py[i];
        int x1 = px[(i + 1) & 3], y1 = py[(i + 1) & 3];

        if (y0 == y1) {
            if (y == y0) {                       /* horizontal edge */
                ra[ns] = MIN(x0, x1);
                rb[ns++] = MAX(x0, x1);
            }
            continue;
        }
        if (y0 == y) {                           /* vertex on the row */
            ra[ns] = rb[ns] = x0;
            ns++;
        }
        if (y >= MIN(y0, y1) && y < MAX(y0, y1)) {
            /* x = x0 + (y - y0) * (x1 - x0) / (y1 - y0), toward zero */
            xs[n++] = x0 + (int)(((int64_t)(y - y0) * (x1 - x0)) / (y1 - y0));
        }
    }
    for (i = 1; i < n; i++) {                    /* n <= 4: insertion sort */
        int v = xs[i];

        for (k = i; k > 0 && xs[k - 1] > v; k--) {
            xs[k] = xs[k - 1];
        }
        xs[k] = v;
    }
    for (i = 0; i + 1 < n; i += 2) {
        ra[ns] = xs[i];
        rb[ns++] = xs[i + 1];
    }
    for (i = 1; i < ns; i++) {                   /* sort by start */
        int a = ra[i], b = rb[i];

        for (k = i; k > 0 && ra[k - 1] > a; k--) {
            ra[k] = ra[k - 1];
            rb[k] = rb[k - 1];
        }
        ra[k] = a;
        rb[k] = b;
    }
    n = 0;                                       /* merge and clip */
    for (i = 0; i < ns; i++) {
        int a = MAX(ra[i], xmin), b = MIN(rb[i], xmax);

        if (a > b) {
            continue;
        }
        if (n && a <= sb[n - 1] + 1) {
            sb[n - 1] = MAX(sb[n - 1], b);
        } else {
            sa[n] = a;
            sb[n++] = b;
        }
    }
    return n;
}

static uint64_t cup2006_dg_polygon4c(Cup2006MachineState *s, uint32_t pc,
                                     uint32_t w0, const uint32_t *f,
                                     int lx, int ly)
{
    uint32_t mode = w0 & 0x00ffffffu;
    uint32_t color = f[0] & 0xffff;
    int px[4], py[4], i, y;
    int xmin = 0, ymin = 0, xmax, ymax;
    uint32_t dstrr, sc;
    bool work = (mode & CUP2006_DG_MODE_WORK) != 0;
    uint64_t written = 0;

    bool alpha = (mode & CUP2006_DG_MODE_AE) != 0;
    unsigned alph = s->dg_reg[CUP2006_DG_ALPHR >> 2] & 0xff;
    bool argb = (s->dg_reg[CUP2006_DG_RCLR >> 2] & CUP2006_DG_RCLR_DPF) != 0;

    if (mode & ~(CUP2006_DG_MODE_CLIP | CUP2006_DG_MODE_WORK |
                 CUP2006_DG_MODE_BLKE | CUP2006_DG_MODE_MTRE |
                 CUP2006_DG_MODE_AE)) {
        cup2006_dg_skip(s, pc, "POLYGON4C draw-mode bits not modelled", w0);
        return 0;
    }
    if (!(s->dg_reg[CUP2006_DG_RCLR >> 2] & CUP2006_DG_RCLR_16BPP)) {
        cup2006_dg_skip(s, pc, "RCLR does not select 16-bit/pixel", w0);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        px[i] = (int16_t)(f[1 + i] >> 16) + lx;
        py[i] = (int16_t)(f[1 + i] & 0xffff) + ly;
        cup2006_dg_xf(s, mode, &px[i], &py[i]);
    }

    sc = s->dg_reg[CUP2006_DG_SCLMAR >> 2];
    xmax = (sc >> 16) & 0xffff;
    ymax = sc & 0xffff;
    if (mode & CUP2006_DG_MODE_CLIP) {
        uint32_t mn = s->dg_reg[CUP2006_DG_UCLMIR >> 2];
        uint32_t mx = s->dg_reg[CUP2006_DG_UCLMAR >> 2];

        xmin = MAX(xmin, (int)(mn >> 16));
        ymin = MAX(ymin, (int)(mn & 0xffff));
        xmax = MIN(xmax, (int)(mx >> 16));
        ymax = MIN(ymax, (int)(mx & 0xffff));
    }
    xmax = MIN(xmax, CUP2006_DG_SURF_W - 1);
    ymax = MIN(ymax, CUP2006_DG_SURF_H - 1);
    ymin = MAX(ymin, MIN(MIN(py[0], py[1]), MIN(py[2], py[3])));
    ymax = MIN(ymax, MAX(MAX(py[0], py[1]), MAX(py[2], py[3])));
    if (xmin > xmax || ymin > ymax) {
        return 0;
    }

    dstrr = s->dg_reg[CUP2006_DG_DSTRR >> 2];
    for (y = ymin; y <= ymax; y++) {
        int sa[CUP2006_DG_QUAD_SPANS], sb[CUP2006_DG_QUAD_SPANS];
        int ns = cup2006_dg_quad_spans(px, py, y, xmin, xmax, sa, sb);

        for (i = 0; i < ns; i++) {
            int xa = sa[i], xb = sb[i];
            int x;

            for (x = xa; x <= xb; x++) {
                uint8_t *dp;

                /* WORK = 1: draw only where the work bit is 1. */
                if (work && !cup2006_dg_work_get(s, x, y)) {
                    continue;
                }
                dp = cup2006_dg_ram_ptr(s,
                    s->dg_reg[CUP2006_DG_DEST >> 2] +
                    ((uint32_t)y * dstrr + (uint32_t)x) * 2, 2);
                if (!dp) {
                    cup2006_dg_skip(s, pc, "destination outside RAM", w0);
                    return written;
                }
                if (alpha) {
                    stw_le_p(dp, cup2006_dg_blend(color, lduw_le_p(dp), alph,
                                                  argb));
                } else {
                    stw_le_p(dp, color);
                }
                written++;
            }
        }
    }
    s->dg_blits++;
    return written;
}

/*
 * POLYGON4A/B (op 0x82/0x81, STYLE=1, SS=0, SH7764 23.2.1): 4A
 * [0x82|mode][Base][TDX|TDY][TXOFS|TYOFS][4 vertices], 4B the same with
 * [Color1|Color0] first; STYLE=1 tiles the source over the quad.
 */
static uint64_t cup2006_dg_polygon4ab(Cup2006MachineState *s, uint32_t pc,
                                      uint32_t w0, const uint32_t *f,
                                      int lx, int ly)
{
    uint32_t op = w0 >> 24;
    uint32_t mode = w0 & 0x00ffffffu;
    uint32_t allowed = CUP2006_DG_MODE_CLIP | CUP2006_DG_MODE_STRANS |
                       CUP2006_DG_MODE_STYLE | CUP2006_DG_MODE_WORK |
                       CUP2006_DG_MODE_BLKE | CUP2006_DG_MODE_MTRE;
    bool work = (mode & CUP2006_DG_MODE_WORK) != 0;
    bool binary = (op == 0x81);
    uint32_t c1 = 0, c0 = 0, base, td_word, ofs_word;
    const uint32_t *v;
    int tdx, tdy, txofs, tyofs;
    int px[4], py[4], i, y;
    int xmin = 0, ymin = 0, xmax, ymax;
    uint32_t dstrr, sc, stcr = 0, stcr_mask = 0xffff;
    bool strans = (mode & CUP2006_DG_MODE_STRANS) != 0;
    bool scaled;
    int qx0, qx1, qy0, qy1;
    uint64_t written = 0;

    if (mode & ~allowed) {
        cup2006_dg_skip(s, pc, "POLYGON4A/B draw-mode bits not modelled", w0);
        return 0;
    }
    /* SS = 0 only: SS = 1 (2-D source area) is not handled here. */
    if (mode & CUP2006_DG_MODE_SS) {
        cup2006_dg_skip(s, pc, "POLYGON4A/B SS=1 not modelled", w0);
        return 0;
    }
    /*
     * STYLE = 0: the source is scaled to the quad ("enlarged or reduced in
     * proportion to the size of the rendering area", SH7764 23.1.5(3));
     * approximated as nearest-neighbour over the bounding box.
     */
    scaled = !(mode & CUP2006_DG_MODE_STYLE);
    if (!(s->dg_reg[CUP2006_DG_RCLR >> 2] & CUP2006_DG_RCLR_16BPP)) {
        cup2006_dg_skip(s, pc, "RCLR does not select 16-bit/pixel", w0);
        return 0;
    }
    if (binary) {
        c1 = f[0] >> 16;
        c0 = f[0] & 0xffff;
        base = f[1];
        td_word = f[2];
        ofs_word = f[3];
        v = &f[4];
    } else {
        base = f[0];
        td_word = f[1];
        ofs_word = f[2];
        v = &f[3];
    }
    tdx = td_word >> 16;
    tdy = td_word & 0xffff;
    txofs = ofs_word >> 16;
    tyofs = ofs_word & 0xffff;
    if (tdx <= 0 || tdy <= 0 || (tdx & 7) || txofs >= tdx || tyofs >= tdy) {
        cup2006_dg_skip(s, pc, "POLYGON4A/B source size not modelled", w0);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        px[i] = (int16_t)(v[i] >> 16) + lx;
        py[i] = (int16_t)(v[i] & 0xffff) + ly;
        cup2006_dg_xf(s, mode, &px[i], &py[i]);
    }

    sc = s->dg_reg[CUP2006_DG_SCLMAR >> 2];
    xmax = (sc >> 16) & 0xffff;
    ymax = sc & 0xffff;
    if (mode & CUP2006_DG_MODE_CLIP) {
        uint32_t mn = s->dg_reg[CUP2006_DG_UCLMIR >> 2];
        uint32_t mx = s->dg_reg[CUP2006_DG_UCLMAR >> 2];

        xmin = MAX(xmin, (int)(mn >> 16));
        ymin = MAX(ymin, (int)(mn & 0xffff));
        xmax = MIN(xmax, (int)(mx >> 16));
        ymax = MIN(ymax, (int)(mx & 0xffff));
    }
    xmax = MIN(xmax, CUP2006_DG_SURF_W - 1);
    ymax = MIN(ymax, CUP2006_DG_SURF_H - 1);
    qx0 = MIN(MIN(px[0], px[1]), MIN(px[2], px[3]));
    qx1 = MAX(MAX(px[0], px[1]), MAX(px[2], px[3]));
    qy0 = MIN(MIN(py[0], py[1]), MIN(py[2], py[3]));
    qy1 = MAX(MAX(py[0], py[1]), MAX(py[2], py[3]));
    ymin = MAX(ymin, qy0);
    ymax = MIN(ymax, qy1);
    if (xmin > xmax || ymin > ymax) {
        return 0;
    }
    if (strans) {
        /* RCLR bit 21 = SPF: compare the ARGB1555 source without its A bit. */
        if (s->dg_reg[CUP2006_DG_RCLR >> 2] & (1u << 21)) {
            stcr_mask = 0x7fff;
        }
        stcr = s->dg_reg[CUP2006_DG_STCR >> 2] & stcr_mask;
    }

    dstrr = s->dg_reg[CUP2006_DG_DSTRR >> 2];
    for (y = ymin; y <= ymax; y++) {
        int sa[CUP2006_DG_QUAD_SPANS], sb[CUP2006_DG_QUAD_SPANS];
        int ns = cup2006_dg_quad_spans(px, py, y, xmin, xmax, sa, sb);
        int srow = scaled ?
            (tyofs + (int)((int64_t)(y - qy0) * tdy / (qy1 - qy0 + 1))) % tdy :
            (tyofs + y - py[0]) % tdy;

        if (srow < 0) {
            srow += tdy;
        }
        for (i = 0; i < ns; i++) {
            int xa = sa[i], xb = sb[i];
            int x;

            for (x = xa; x <= xb; x++) {
                int scol = scaled ?
                    (txofs + (int)((int64_t)(x - qx0) * tdx /
                                   (qx1 - qx0 + 1))) % tdx :
                    (txofs + x - px[0]) % tdx;
                uint32_t val;
                uint8_t *dp;

                if (work && !cup2006_dg_work_get(s, x, y)) {
                    continue;
                }
                if (scol < 0) {
                    scol += tdx;
                }
                if (binary) {
                    uint8_t *sb = cup2006_dg_ram_ptr(s,
                        base + (uint32_t)srow * (uint32_t)(tdx / 8) +
                        (uint32_t)(scol >> 3), 1);

                    if (!sb) {
                        cup2006_dg_skip(s, pc, "POLYGON4B source outside RAM",
                                        w0);
                        return written;
                    }
                    val = (*sb & (1u << (scol & 7))) ? c1 : c0;
                } else {
                    uint8_t *sp = cup2006_dg_ram_ptr(s,
                        base + ((uint32_t)srow * (uint32_t)tdx +
                                (uint32_t)scol) * 2, 2);

                    if (!sp) {
                        cup2006_dg_skip(s, pc, "POLYGON4A source outside RAM",
                                        w0);
                        return written;
                    }
                    val = lduw_le_p(sp);
                }
                dp = cup2006_dg_ram_ptr(s,
                    s->dg_reg[CUP2006_DG_DEST >> 2] +
                    ((uint32_t)y * dstrr + (uint32_t)x) * 2, 2);
                if (!dp) {
                    cup2006_dg_skip(s, pc, "destination outside RAM", w0);
                    return written;
                }
                if (strans && (val & stcr_mask) == stcr) {
                    continue;
                }
                stw_le_p(dp, val);
                written++;
            }
        }
    }
    s->dg_blits++;
    return written;
}

/*
 * LINEC (op 0xB0, SH7764 23.2.3(3)): [0xB0|mode][Color|n][0|W][DX1|DY1]..
 * (LINKE=0) or [LINK Address] (LINKE=1); Bresenham, bold lines as W x W
 * squares per point.
 */
#define CUP2006_DG_MODE_LINKE  (1u << 10)
#define CUP2006_DG_MODE_LREL   (1u << 9)

static void cup2006_dg_plot(Cup2006MachineState *s, int x, int y, int w,
                            uint32_t color, int xmin, int ymin, int xmax,
                            int ymax, uint64_t *written)
{
    uint32_t dstrr = s->dg_reg[CUP2006_DG_DSTRR >> 2];
    int r = w >= 2 ? w / 2 : 0, xx, yy;

    for (yy = y - r; yy <= y - r + (w >= 2 ? w - 1 : 0); yy++) {
        for (xx = x - r; xx <= x - r + (w >= 2 ? w - 1 : 0); xx++) {
            uint8_t *dp;

            if (xx < xmin || xx > xmax || yy < ymin || yy > ymax) {
                continue;
            }
            dp = cup2006_dg_ram_ptr(s, s->dg_reg[CUP2006_DG_DEST >> 2] +
                                    ((uint32_t)yy * dstrr + (uint32_t)xx) * 2,
                                    2);
            if (dp) {
                stw_le_p(dp, color);
                (*written)++;
            }
        }
    }
}

static uint64_t cup2006_dg_linec(Cup2006MachineState *s, uint32_t pc,
                                 uint32_t w0, uint32_t f0, uint32_t f1,
                                 uint32_t vaddr, int lx, int ly)
{
    uint32_t mode = w0 & 0x00ffffffu;
    uint32_t color = f0 >> 16;
    unsigned n = f0 & 0xffff, i;
    int w = f1 & 0x3f;
    int xmin = 0, ymin = 0, xmax, ymax, px = 0, py = 0;
    uint32_t sc;
    uint64_t written = 0;

    if (!(s->dg_reg[CUP2006_DG_RCLR >> 2] & CUP2006_DG_RCLR_16BPP)) {
        cup2006_dg_skip(s, pc, "RCLR does not select 16-bit/pixel", w0);
        return 0;
    }
    sc = s->dg_reg[CUP2006_DG_SCLMAR >> 2];
    xmax = (sc >> 16) & 0xffff;
    ymax = sc & 0xffff;
    if (mode & CUP2006_DG_MODE_CLIP) {
        uint32_t mn = s->dg_reg[CUP2006_DG_UCLMIR >> 2];
        uint32_t mx = s->dg_reg[CUP2006_DG_UCLMAR >> 2];

        xmin = MAX(xmin, (int)(mn >> 16));
        ymin = MAX(ymin, (int)(mn & 0xffff));
        xmax = MIN(xmax, (int)(mx >> 16));
        ymax = MIN(ymax, (int)(mx & 0xffff));
    }
    xmax = MIN(xmax, CUP2006_DG_SURF_W - 1);
    ymax = MIN(ymax, CUP2006_DG_SURF_H - 1);
    for (i = 0; i < n; i++) {
        uint32_t v;
        int x, y;

        if (!cup2006_dg_fetch(s, vaddr + 4 * i, &v)) {
            cup2006_dg_skip(s, pc, "LINEC vertex outside RAM", w0);
            break;
        }
        x = (int16_t)(v >> 16) + lx;
        y = (int16_t)(v & 0xffff) + ly;
        cup2006_dg_xf(s, mode, &x, &y);
        if (i == 0) {
            cup2006_dg_plot(s, x, y, w, color, xmin, ymin, xmax, ymax,
                            &written);
        } else {
            int cx = px, cy = py, ex = x, ey = y, r = w / 2 + 1;
            int dx, sx, dy, sy, err;

            if (!cup2006_dg_clipseg(&cx, &cy, &ex, &ey, xmin - r, ymin - r,
                                    xmax + r, ymax + r)) {
                px = x;
                py = y;
                continue;
            }
            dx = abs(ex - cx);
            sx = cx < ex ? 1 : -1;
            dy = -abs(ey - cy);
            sy = cy < ey ? 1 : -1;
            err = dx + dy;

            for (;;) {
                cup2006_dg_plot(s, cx, cy, w, color, xmin, ymin, xmax, ymax,
                                &written);
                int e2 = 2 * err;

                if (cx == ex && cy == ey) {
                    break;
                }
                if (e2 >= dy) {
                    err += dy;
                    cx += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    cy += sy;
                }
            }
        }
        px = x;
        py = y;
    }
    s->dg_blits++;
    return written;
}

/*
 * Binary work plane (SH7764 23.1.3, 23.1.5(8)): 1 bit/pixel at WSAR (+0x054),
 * stride DSTRR; CLRWC/FTRAPC/LINEWC paint it, POLYGON4 with WORK=1 uses it.
 * Bit order within a byte: LSB first.
 */
#define CUP2006_DG_WSAR        0x054
#define CUP2006_DG_MODE_EDG    (1u << 4)
#define CUP2006_DG_MODE_EOS    (1u << 3)

static uint8_t *cup2006_dg_work_byte(Cup2006MachineState *s, int x, int y,
                                     unsigned *bit)
{
    uint32_t dstrr = s->dg_reg[CUP2006_DG_DSTRR >> 2];

    if (x < 0 || y < 0 || (uint32_t)x >= dstrr || y > 4095) {
        return NULL;
    }
    *bit = x & 7;
    return cup2006_dg_ram_ptr(s, (s->dg_reg[CUP2006_DG_WSAR >> 2] &
                                  0x1ffffff0u) +
                              (uint32_t)y * (dstrr / 8) + (uint32_t)x / 8, 1);
}

static bool cup2006_dg_work_get(Cup2006MachineState *s, int x, int y)
{
    unsigned bit;
    uint8_t *p = cup2006_dg_work_byte(s, x, y, &bit);

    return p && (*p & (1u << bit));
}

#define CUP2006_DG_WORK_BUDGET 20000000ull  /* pixel ops per job */

static void cup2006_dg_work_op(Cup2006MachineState *s, int x, int y, int op)
{
    unsigned bit;
    uint8_t *p;

    if (++s->dg_work_ops > CUP2006_DG_WORK_BUDGET) {
        if (s->dg_work_ops == CUP2006_DG_WORK_BUDGET + 1) {
            trace_cup2006_2dg_dg_work_budget_exceeded(s->dg_jobs);
        }
        return;
    }
    p = cup2006_dg_work_byte(s, x, y, &bit);

    if (!p) {
        return;
    }
    if (op == 0) {
        *p &= ~(1u << bit);
    } else if (op == 1) {
        *p |= 1u << bit;
    } else {
        *p ^= 1u << bit;
    }
}

/* Work-plane clip: the system clip, narrowed by the user clip on CLIP. */
static void cup2006_dg_work_clip(Cup2006MachineState *s, uint32_t mode,
                                 int *xmin, int *ymin, int *xmax, int *ymax)
{
    uint32_t sc = s->dg_reg[CUP2006_DG_SCLMAR >> 2];

    *xmin = 0;
    *ymin = 0;
    *xmax = (sc >> 16) & 0xffff;
    *ymax = sc & 0xffff;
    if (mode & CUP2006_DG_MODE_CLIP) {
        uint32_t mn = s->dg_reg[CUP2006_DG_UCLMIR >> 2];
        uint32_t mx = s->dg_reg[CUP2006_DG_UCLMAR >> 2];

        *xmin = MAX(*xmin, (int)(mn >> 16));
        *ymin = MAX(*ymin, (int)(mn & 0xffff));
        *xmax = MIN(*xmax, (int)(mx >> 16));
        *ymax = MIN(*ymax, (int)(mx & 0xffff));
    }
    *xmax = MIN(*xmax, CUP2006_DG_SURF_W - 1);
    *ymax = MIN(*ymax, CUP2006_DG_SURF_H - 1);
}

/* 1-bit line at work coordinates (LINEWC, FTRAPC's EDG): Bresenham, final
 * point drawn. */
static void cup2006_dg_work_line(Cup2006MachineState *s, int x0, int y0,
                                 int x1, int y1, int val, int xmin, int ymin,
                                 int xmax, int ymax)
{
    int dx, sx, dy, sy, err;

    if (!cup2006_dg_clipseg(&x0, &y0, &x1, &y1, xmin, ymin, xmax, ymax)) {
        return;
    }
    dx = abs(x1 - x0);
    sx = x0 < x1 ? 1 : -1;
    dy = -abs(y1 - y0);
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        if (x0 >= xmin && x0 <= xmax && y0 >= ymin && y0 <= ymax) {
            cup2006_dg_work_op(s, x0, y0, val);
        }
        int e2 = 2 * err;          /* once per step, or the walk can miss
                                    * the end point and never stop */

        if (x0 == x1 && y0 == y1) {
            break;
        }
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

/* CLRWC (op 0xE0): [0xE0|mode][Xmin|Ymin][Xmax|Ymax] -> work bits 0. */
static void cup2006_dg_clrwc(Cup2006MachineState *s, uint32_t w0,
                             uint32_t mn, uint32_t mx, int lx, int ly)
{
    int cx0, cy0, cx1, cy1, x, y, i;
    int x0 = (int16_t)(mn >> 16) + lx, y0 = (int16_t)(mn & 0xffff) + ly;
    int x1 = (int16_t)(mx >> 16) + lx, y1 = (int16_t)(mx & 0xffff) + ly;
    int qx[4] = { x0, x1, x1, x0 }, qy[4] = { y0, y0, y1, y1 };

    /* "obtaining the four vertices ... transforming ... converted into a
     * circumscribed rectangle" (CLRWC description). */
    for (i = 0; i < 4; i++) {
        cup2006_dg_xf(s, w0 & 0xffff, &qx[i], &qy[i]);
    }
    x0 = MIN(MIN(qx[0], qx[1]), MIN(qx[2], qx[3]));
    x1 = MAX(MAX(qx[0], qx[1]), MAX(qx[2], qx[3]));
    y0 = MIN(MIN(qy[0], qy[1]), MIN(qy[2], qy[3]));
    y1 = MAX(MAX(qy[0], qy[1]), MAX(qy[2], qy[3]));

    cup2006_dg_work_clip(s, w0 & 0xffff, &cx0, &cy0, &cx1, &cy1);
    if (s->dg_work_ops > CUP2006_DG_WORK_BUDGET) {
        return;
    }
    for (y = MAX(y0, cy0); y <= MIN(y1, cy1); y++) {
        for (x = MAX(x0, cx0); x <= MIN(x1, cx1); x++) {
            cup2006_dg_work_op(s, x, y, 0);
        }
    }
}

/*
 * FTRAPC (op 0xD0): [0xD0|mode][0|n][Xmin|Ymin][Xmax|Ymax][vertices] or a
 * LINK Address; paints n-1 binary-EOR trapezoids on the work plane, EDG then
 * draws the edge line with the EOS value.
 */
static void cup2006_dg_ftrapc(Cup2006MachineState *s, uint32_t pc,
                              uint32_t w0, uint32_t n, uint32_t mn,
                              uint32_t mx, uint32_t vaddr, int lx, int ly)
{
    uint32_t mode = w0 & 0xffff;
    int cx0, cy0, cx1, cy1, x0 = 0, y0 = 0, k;
    int bx0 = (int16_t)(mn >> 16) + lx, by0 = (int16_t)(mn & 0xffff) + ly;
    int bx1 = (int16_t)(mx >> 16) + lx, by1 = (int16_t)(mx & 0xffff) + ly;
    int qx[4] = { bx0, bx1, bx1, bx0 }, qy[4] = { by0, by0, by1, by1 };
    int xl;
    unsigned i;

    /* The left side is the circumscribed rectangle's left edge after the
     * four bounding vertices are transformed (FTRAPC description). */
    for (k = 0; k < 4; k++) {
        cup2006_dg_xf(s, mode, &qx[k], &qy[k]);
    }
    xl = MIN(MIN(qx[0], qx[1]), MIN(qx[2], qx[3]));

    cup2006_dg_work_clip(s, mode, &cx0, &cy0, &cx1, &cy1);
    if (s->dg_work_ops > CUP2006_DG_WORK_BUDGET) {
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t v;
        int x1, y1;

        if (!cup2006_dg_fetch(s, vaddr + 4 * i, &v)) {
            cup2006_dg_skip(s, pc, "FTRAPC vertex outside RAM", w0);
            return;
        }
        x1 = (int16_t)(v >> 16) + lx;
        y1 = (int16_t)(v & 0xffff) + ly;
        cup2006_dg_xf(s, mode, &x1, &y1);
        if (i > 0 && y0 != y1) {
            int ya = MIN(y0, y1), yb = MAX(y0, y1), y;

            for (y = MAX(ya, cy0); y < yb && y <= cy1; y++) {
                int xe = x0 + (int)(((int64_t)(y - y0) * (x1 - x0)) /
                                    (y1 - y0));
                int x;

                for (x = MAX(xl, cx0); x <= MIN(xe, cx1); x++) {
                    cup2006_dg_work_op(s, x, y, 2);
                }
            }
        }
        x0 = x1;
        y0 = y1;
    }
    if (mode & CUP2006_DG_MODE_EDG) {
        int val = (mode & CUP2006_DG_MODE_EOS) ? 1 : 0;
        int px = 0, py = 0;

        for (i = 0; i < n; i++) {
            uint32_t v;
            int x1, y1;

            if (!cup2006_dg_fetch(s, vaddr + 4 * i, &v)) {
                return;
            }
            x1 = (int16_t)(v >> 16) + lx;
            y1 = (int16_t)(v & 0xffff) + ly;
            cup2006_dg_xf(s, mode, &x1, &y1);
            if (i > 0) {
                cup2006_dg_work_line(s, px, py, x1, y1, val, cx0, cy0, cx1,
                                     cy1);
            }
            px = x1;
            py = y1;
        }
    }
    s->dg_blits++;
}

/* LINEWC (op 0xF0): [0xF0|mode][0|n][DX1|DY1].. 1-bit line, value = EOS. */
static void cup2006_dg_linewc(Cup2006MachineState *s, uint32_t pc,
                              uint32_t w0, uint32_t n, uint32_t vaddr,
                              int lx, int ly)
{
    uint32_t mode = w0 & 0xffff;
    int cx0, cy0, cx1, cy1, px = 0, py = 0;
    int val = (mode & CUP2006_DG_MODE_EOS) ? 1 : 0;
    unsigned i;

    cup2006_dg_work_clip(s, mode, &cx0, &cy0, &cx1, &cy1);
    for (i = 0; i < n; i++) {
        uint32_t v;
        int x1, y1;

        if (!cup2006_dg_fetch(s, vaddr + 4 * i, &v)) {
            cup2006_dg_skip(s, pc, "LINEWC vertex outside RAM", w0);
            return;
        }
        x1 = (int16_t)(v >> 16) + lx;
        y1 = (int16_t)(v & 0xffff) + ly;
        cup2006_dg_xf(s, mode, &x1, &y1);
        if (i == 0) {
            if (n == 1) {
                cup2006_dg_work_line(s, x1, y1, x1, y1, val, cx0, cy0, cx1,
                                     cy1);
            }
        } else {
            cup2006_dg_work_line(s, px, py, x1, y1, val, cx0, cy0, cx1, cy1);
        }
        px = x1;
        py = y1;
    }
    s->dg_blits++;
}

/*
 * BITBLTB (op 0xA1, SH7764 23.2.5(2)): 1-bit source colour-expanded to
 * Color1/Color0, rows (LW+RW+1)/8 bytes LSB-first; STRANS drops STCR
 * matches, SS=1 and ROP != 0xCC are skipped.
 */
static uint64_t cup2006_dg_bitbltb(Cup2006MachineState *s, uint32_t pc,
                                   uint32_t w0, const uint32_t *f,
                                   int lx, int ly)
{
    uint32_t mode = w0 & 0x00ffffffu;
    uint32_t allowed = CUP2006_DG_MODE_CLIP | CUP2006_DG_MODE_STRANS;
    uint32_t c1 = f[1] >> 16, c0 = f[1] & 0xffff;
    uint32_t src_pa = f[2];
    int lw = f[3] >> 16, rw = f[3] & 0xffff;
    int th = f[4] >> 16, bh = f[4] & 0xffff;
    int bxc = (int16_t)(f[5] >> 16), byc = (int16_t)(f[5] & 0xffff);
    int nx = lw + rw + 1, ny = th + bh + 1;
    int dx0 = bxc + lx - lw, dy0 = byc + ly - th;
    int x, y, xmin = 0, ymin = 0, xmax, ymax, rowbytes;
    uint32_t dstrr, sc, stcr = 0, stcr_mask = 0xffff;
    bool strans = (mode & CUP2006_DG_MODE_STRANS) != 0;
    uint64_t written = 0;

    if ((f[0] & 0xff) != CUP2006_DG_ROP_COPY) {
        cup2006_dg_skip(s, pc, "BITBLTB ROP not modelled (only 0xCC)", w0);
        return 0;
    }
    if (mode & ~allowed) {
        cup2006_dg_skip(s, pc, "BITBLTB draw-mode bits not modelled", w0);
        return 0;
    }
    if (!(s->dg_reg[CUP2006_DG_RCLR >> 2] & CUP2006_DG_RCLR_16BPP)) {
        cup2006_dg_skip(s, pc, "RCLR does not select 16-bit/pixel", w0);
        return 0;
    }
    if (nx <= 0 || ny <= 0 || (nx & 7)) {
        cup2006_dg_skip(s, pc, "BITBLTB width not a multiple of 8", w0);
        return 0;
    }
    rowbytes = nx / 8;

    sc = s->dg_reg[CUP2006_DG_SCLMAR >> 2];
    xmax = (sc >> 16) & 0xffff;
    ymax = sc & 0xffff;
    if (mode & CUP2006_DG_MODE_CLIP) {
        uint32_t mn = s->dg_reg[CUP2006_DG_UCLMIR >> 2];
        uint32_t mx = s->dg_reg[CUP2006_DG_UCLMAR >> 2];

        xmin = MAX(xmin, (int)(mn >> 16));
        ymin = MAX(ymin, (int)(mn & 0xffff));
        xmax = MIN(xmax, (int)(mx >> 16));
        ymax = MIN(ymax, (int)(mx & 0xffff));
    }
    xmax = MIN(xmax, CUP2006_DG_SURF_W - 1);
    ymax = MIN(ymax, CUP2006_DG_SURF_H - 1);
    xmin = MAX(xmin, dx0);
    ymin = MAX(ymin, dy0);
    xmax = MIN(xmax, dx0 + nx - 1);
    ymax = MIN(ymax, dy0 + ny - 1);
    if (xmin > xmax || ymin > ymax) {
        return 0;
    }
    if (strans) {
        if (s->dg_reg[CUP2006_DG_RCLR >> 2] & (1u << 21)) {
            stcr_mask = 0x7fff;
        }
        stcr = s->dg_reg[CUP2006_DG_STCR >> 2] & stcr_mask;
    }

    dstrr = s->dg_reg[CUP2006_DG_DSTRR >> 2];
    for (y = ymin; y <= ymax; y++) {
        int row = y - dy0;
        uint8_t *sb = cup2006_dg_ram_ptr(s, src_pa + (uint32_t)row * rowbytes,
                                         rowbytes);

        if (!sb) {
            cup2006_dg_skip(s, pc, "BITBLTB source outside RAM", w0);
            return written;
        }
        for (x = xmin; x <= xmax; x++) {
            int col = x - dx0;
            uint32_t val = (sb[col >> 3] & (1u << (col & 7))) ? c1 : c0;
            uint8_t *dp = cup2006_dg_ram_ptr(s,
                s->dg_reg[CUP2006_DG_DEST >> 2] +
                ((uint32_t)y * dstrr + (uint32_t)x) * 2, 2);

            if (!dp) {
                cup2006_dg_skip(s, pc, "destination outside RAM", w0);
                return written;
            }
            if (strans && (val & stcr_mask) == stcr) {
                continue;
            }
            stw_le_p(dp, val);
            written++;
        }
    }
    s->dg_blits++;
    return written;
}

static void cup2006_dg_execute(Cup2006MachineState *s)
{
    uint32_t pc = s->dg_reg[CUP2006_DG_LIST >> 2];
    uint32_t retstack[64];
    unsigned sp = 0, packets = 0;
    uint64_t pixels = 0;
    uint64_t blits0 = s->dg_blits, skip0 = s->dg_skipped;
    int lx = 0, ly = 0;
    unsigned steps = 0;

    s->dg_work_ops = 0;
    while (steps++ < CUP2006_DG_WALK_MAX) {
        uint32_t w0, f[8];
        uint32_t op;

        if (!cup2006_dg_fetch(s, pc, &w0)) {
            trace_cup2006_2dg_dg_walk_stop_fetch(pc);
            break;
        }
        op = w0 >> 24;
        if (op == 0x00) {                       /* TRAP ends this walk */
            break;
        }
        packets++;
        if (op == 0x18) {                       /* WPR */
            uint32_t ctl;
            unsigned n, i;
            uint32_t bytem = w0 & 0x0f;         /* b3..b0, 1 = byte masked */
            uint32_t keep = ((bytem & 1) ? 0xff000000u : 0) |
                            ((bytem & 2) ? 0x00ff0000u : 0) |
                            ((bytem & 4) ? 0x0000ff00u : 0) |
                            ((bytem & 8) ? 0x000000ffu : 0);

            if (!cup2006_dg_fetch(s, pc + 4, &ctl)) {
                break;
            }
            n = ((ctl >> 16) & 0xffff) + 1;
            for (i = 0; i < n; i++) {
                uint32_t reg = ctl & 0x1ff;
                uint32_t data;

                if (!cup2006_dg_fetch(s, pc + 8 + 4 * i, &data) ||
                    reg + 4 * i + 4 > CUP2006_DG_SIZE) {
                    break;
                }
                s->dg_reg[(reg + 4 * i) >> 2] =
                    (s->dg_reg[(reg + 4 * i) >> 2] & keep) | (data & ~keep);
            }
            pc += 8 + 4 * n;
        } else if (op == 0x28) {                /* JUMP */
            if (!cup2006_dg_fetch(s, pc + 4, &pc)) {
                break;
            }
        } else if (op == 0x30) {                /* GOSUB */
            uint32_t tgt;

            if (!cup2006_dg_fetch(s, pc + 4, &tgt) || sp == ARRAY_SIZE(retstack)) {
                break;
            }
            retstack[sp++] = pc + 8;
            pc = tgt;
        } else if (op == 0x38) {                /* RET */
            if (!sp) {
                break;
            }
            pc = retstack[--sp];
        } else if (op == 0x40) {                /* LCOFS */
            uint32_t v;

            if (!cup2006_dg_fetch(s, pc + 4, &v)) {
                break;
            }
            lx = (int16_t)(v >> 16);
            ly = (int16_t)(v & 0xffff);
            pc += 8;
        } else if (op == 0xa0 || op == 0xa2) {  /* BITBLTC / BITBLTA */
            unsigned i;

            for (i = 0; i < 5; i++) {
                if (!cup2006_dg_fetch(s, pc + 4 + 4 * i, &f[i])) {
                    break;
                }
            }
            if (i != 5) {
                break;
            }
            pixels += cup2006_dg_bitblt(s, pc, w0, f, lx, ly);
            pc += 24;
        } else if (op == 0xa1) {                /* BITBLTB binary source */
            unsigned i;

            for (i = 0; i < 6; i++) {
                if (!cup2006_dg_fetch(s, pc + 4 + 4 * i, &f[i])) {
                    break;
                }
            }
            if (i != 6) {
                break;
            }
            pixels += cup2006_dg_bitbltb(s, pc, w0, f, lx, ly);
            pc += 28;
        } else if (op == 0x80) {                /* POLYGON4C */
            unsigned i;

            for (i = 0; i < 5; i++) {
                if (!cup2006_dg_fetch(s, pc + 4 + 4 * i, &f[i])) {
                    break;
                }
            }
            if (i != 5) {
                break;
            }
            pixels += cup2006_dg_polygon4c(s, pc, w0, f, lx, ly);
            pc += 24;
        } else if (op == 0x82 || op == 0x81) {  /* POLYGON4A/B textured */
            unsigned n = op == 0x82 ? 7 : 8, i;

            for (i = 0; i < n; i++) {
                if (!cup2006_dg_fetch(s, pc + 4 + 4 * i, &f[i])) {
                    break;
                }
            }
            if (i != n) {
                break;
            }
            pixels += cup2006_dg_polygon4ab(s, pc, w0, f, lx, ly);
            pc += 4 + 4 * n;
        } else if (op == 0xb0) {                /* LINEC */
            uint32_t f0, f1, link;
            uint32_t mode = w0 & 0xffff;

            if (!cup2006_dg_fetch(s, pc + 4, &f0) ||
                !cup2006_dg_fetch(s, pc + 8, &f1)) {
                break;
            }
            if (mode & CUP2006_DG_MODE_LINKE) {
                if (!cup2006_dg_fetch(s, pc + 12, &link)) {
                    break;
                }
                link &= 0x1ffffffcu;
                if (mode & CUP2006_DG_MODE_LREL) {
                    link = pc + (uint32_t)(((int32_t)(link << 3)) >> 3);
                }
                pixels += cup2006_dg_linec(s, pc, w0, f0, f1, link, lx, ly);
                pc += 16;
            } else {
                pixels += cup2006_dg_linec(s, pc, w0, f0, f1, pc + 12,
                                           lx, ly);
                pc += 12 + 4 * (f0 & 0xffff);
            }
        } else if (op == 0xe0) {                /* CLRWC */
            uint32_t mn, mx;

            if (!cup2006_dg_fetch(s, pc + 4, &mn) ||
                !cup2006_dg_fetch(s, pc + 8, &mx)) {
                break;
            }
            cup2006_dg_clrwc(s, w0, mn, mx, lx, ly);
            pc += 12;
        } else if (op == 0xd0) {                /* FTRAPC */
            uint32_t nw, mn, mx, link;
            uint32_t mode = w0 & 0xffff;

            if (!cup2006_dg_fetch(s, pc + 4, &nw) ||
                !cup2006_dg_fetch(s, pc + 8, &mn) ||
                !cup2006_dg_fetch(s, pc + 12, &mx)) {
                break;
            }
            if (mode & CUP2006_DG_MODE_LINKE) {
                if (!cup2006_dg_fetch(s, pc + 16, &link)) {
                    break;
                }
                link &= 0x1ffffffcu;
                if (mode & CUP2006_DG_MODE_LREL) {
                    link = pc + (uint32_t)(((int32_t)(link << 3)) >> 3);
                }
                cup2006_dg_ftrapc(s, pc, w0, nw & 0xffff, mn, mx, link, lx, ly);
                pc += 20;
            } else {
                cup2006_dg_ftrapc(s, pc, w0, nw & 0xffff, mn, mx, pc + 16, lx, ly);
                pc += 16 + 4 * (nw & 0xffff);
            }
        } else if (op == 0xf0) {                /* LINEWC */
            uint32_t nw;

            if (!cup2006_dg_fetch(s, pc + 4, &nw)) {
                break;
            }
            cup2006_dg_linewc(s, pc, w0, nw & 0xffff, pc + 8, lx, ly);
            pc += 8 + 4 * (nw & 0xffff);
        } else if (op == 0x08) {                /* NOP/INT */
            /* SH7764 23.2.9(9): INT = 1 would halt with SR.INT;
             * logged and treated as NOP. */
            if (w0 & 0x8000) {
                cup2006_dg_skip(s, pc, "NOP with INT=1 treated as NOP", w0);
            }
            pc += 4;
        } else if (op == 0xb1) {                /* LINEB */
            /*
             * SH7764 23.2.3(2): [0xB1|mode][Color1|Color0][Base][TDX|TDY]
             * [TXOFS|n][0|W] + vertices or LINK. STUB: drawn as a solid
             * Color1 line via LINEC; the pattern is not sampled.
 */
            uint32_t fw[5], link;
            uint32_t mode = w0 & 0xffff;
            unsigned i;

            for (i = 0; i < 5; i++) {
                if (!cup2006_dg_fetch(s, pc + 4 + 4 * i, &fw[i])) {
                    break;
                }
            }
            if (i != 5) {
                break;
            }
            if (mode & CUP2006_DG_MODE_LINKE) {
                if (!cup2006_dg_fetch(s, pc + 24, &link)) {
                    break;
                }
                link &= 0x1ffffffcu;
                if (mode & CUP2006_DG_MODE_LREL) {
                    link = pc + (uint32_t)(((int32_t)(link << 3)) >> 3);
                }
                pixels += cup2006_dg_linec(s, pc, w0 & ~0x0fffu,
                                           (fw[0] & 0xffff0000u) |
                                           (fw[3] & 0xffff), fw[4], link,
                                           lx, ly);
                pc += 28;
            } else {
                pixels += cup2006_dg_linec(s, pc, w0 & ~0x0fffu,
                                           (fw[0] & 0xffff0000u) |
                                           (fw[3] & 0xffff), fw[4], pc + 24,
                                           lx, ly);
                pc += 24 + 4 * (fw[3] & 0xffff);
            }
        } else if (op == 0xb3) {                /* LINED */
            /*
             * SH7764 23.2.3(4): work-reference edge antialiasing. STUB: not
             * drawn; the command is consumed ([0|n] + n vertices or LINK).
 */
            uint32_t nw;

            if (!cup2006_dg_fetch(s, pc + 4, &nw)) {
                break;
            }
            pc += (w0 & CUP2006_DG_MODE_LINKE) ? 12 : 8 + 4 * (nw & 0xffff);
        } else if (op >= 0xb1 && op <= 0xb7) {  /* other LINE family */
            cup2006_dg_skip(s, pc, "LINE family op not modelled", w0);
            break;
        } else {
            cup2006_dg_skip(s, pc, "opcode not modelled", w0);
            break;
        }
    }
    trace_cup2006_2dg_dg_raster_job_st(s->dg_jobs, s->dg_reg[CUP2006_DG_LIST >> 2], steps, packets, s->dg_blits - blits0, pixels, s->dg_skipped - skip0, s->dg_reg[CUP2006_DG_DEST >> 2]);
}

/*
 * STUB: instant job completion. Execute the guest's display list into the
 * destination buffer (cup2006_dg_execute), then complete the job.
 */
static void cup2006_dg_start(Cup2006MachineState *s)
{
    struct intc_source *src = s->disp_intc_src[0];   /* INTEVT 0x8C0 */
    uint32_t enable = s->dg_reg[CUP2006_DG_IRQEN >> 2];
    /*
     * The completion interrupt is only delivered when the guest has enabled
     * the 0x8C0 source in INTC2 (0xFFE00044 bit 20); the job itself always
     * completes.
 */
    uint32_t intc2_en = s->intc2_enabled & CUP2006_DG_INTC2_ENABLE;

    s->dg_jobs++;
    cup2006_dg_execute(s);

    /* A start re-arms the flag pair: done rises, error is clear. */
    s->dg_reg[CUP2006_DG_STATUS >> 2] =
        (s->dg_reg[CUP2006_DG_STATUS >> 2] & ~CUP2006_DG_STATUS_CLEAR) |
        CUP2006_DG_STATUS_DONE;
    s->dg_reg[CUP2006_DG_CTRL >> 2] = 0;         /* idle again immediately */

    if (!intc2_en) {
        s->dg_disabled_jobs++;
        if (s->dg_disabled_jobs <= 4) {
            trace_cup2006_2dg_dg_job_one_intc2(s->dg_jobs);
        }
        return;
    }
    if (!(enable & CUP2006_DG_STATUS_DONE)) {
        trace_cup2006_2dg_dg_job_one_x008(s->dg_jobs, enable);
        return;
    }
    if (!src) {
        trace_cup2006_2dg_dg_job_one_but(s->dg_jobs);
        return;
    }
    if (!src->asserted) {
        sh_intc_toggle_source(src, 0, 1);
        s->dg_irqs++;
        trace_cup2006_2dg_dg_intevt_x8c0_raised(s->dg_irqs, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - s->draw_t0_ms, s->dg_jobs, s->dg_reg[CUP2006_DG_STATUS >> 2], enable, s->dg_reg[CUP2006_DG_LIST >> 2], s->dg_reg[CUP2006_DG_DEST >> 2]);
    }
}

static void cup2006_dg_write_at(Cup2006MachineState *s, hwaddr base,
                                hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr off = addr & (CUP2006_DG_SIZE - 1);
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
    uint32_t val = ((uint32_t)value & mask) << shift;
    uint32_t wmask = mask << shift;


    switch (off) {
    case CUP2006_DG_CTRL:
        if (val & 1) {
            cup2006_dg_start(s);
        } else {
            s->dg_reg[off >> 2] = (s->dg_reg[off >> 2] & ~wmask) | val;
        }
        break;
    case CUP2006_DG_STATUS:
        /*
         * Write-1-to-clear: writing the register never sets a flag.
 */
        s->dg_reg[CUP2006_DG_STATUS >> 2] &= ~val;
        break;
    case CUP2006_DG_IRQCLR:
        s->dg_reg[CUP2006_DG_STATUS >> 2] &= ~val;
        s->dg_reg[off >> 2] = (s->dg_reg[off >> 2] & ~wmask) | val;
        break;
    default:
        s->dg_reg[off >> 2] = (s->dg_reg[off >> 2] & ~wmask) | val;
        break;
    }
}

static uint64_t cup2006_dg_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_dg_read_at(opaque, CUP2006_DG_BASE, addr, size);
}

static void cup2006_dg_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    cup2006_dg_write_at(opaque, CUP2006_DG_BASE, addr, value, size);
}

static uint64_t cup2006_dg_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_dg_read_at(opaque, CUP2006_DG_A7_BASE, addr, size);
}

static void cup2006_dg_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    cup2006_dg_write_at(opaque, CUP2006_DG_A7_BASE, addr, value, size);
}

const MemoryRegionOps cup2006_dg_ops = {
    .read = cup2006_dg_p4_read,
    .write = cup2006_dg_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_dg_a7_ops = {
    .read = cup2006_dg_a7_read,
    .write = cup2006_dg_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

