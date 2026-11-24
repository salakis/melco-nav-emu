/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

#ifndef CUP28_Q2_LAYOUT_H
#define CUP28_Q2_LAYOUT_H

#include <stdint.h>

/* HD64413A manual figs.3.7-3.9: 16-row, 32-byte UGM tiles. */
static inline unsigned cup28_q2_pixel_offset(unsigned x, unsigned y,
                                             unsigned width, unsigned bpp)
{
    unsigned xb = x * bpp;
    return (y / 16) * width * bpp * 16 + (xb / 32) * 512 +
           (y % 16) * 32 + xb % 32;
}

/* Manual section 3.2 and fig.3.68: R5 G6 B5. */
static inline uint32_t cup28_q2_rgb565(unsigned p)
{
    unsigned r = ((p >> 11) & 31) * 255 / 31;
    unsigned g = ((p >> 5) & 63) * 255 / 63;
    unsigned b = (p & 31) * 255 / 31;
    return (r << 16) | (g << 8) | b;
}

/* Manual section 5.4.12: R at CPnH[7:2], G/B at CPnL[15:10]/[7:2]. */
static inline uint32_t cup28_q2_palette(unsigned hi, unsigned lo)
{
    unsigned r = ((hi >> 2) & 63) * 255 / 63;
    unsigned g = ((lo >> 10) & 63) * 255 / 63;
    unsigned b = ((lo >> 2) & 63) * 255 / 63;
    return (r << 16) | (g << 8) | b;
}

#endif
