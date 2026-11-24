/*
 * Shared AMD NOR flash state machine for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef MELCO_AMDFLASH_CORE_H
#define MELCO_AMDFLASH_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* What a write changed, for tracing. */
typedef enum MelcoAmdFlashOp {
    MELCO_AMDFLASH_OP_NONE,
    MELCO_AMDFLASH_OP_PROGRAM,
    MELCO_AMDFLASH_OP_SECTOR_ERASE,
    MELCO_AMDFLASH_OP_CHIP_ERASE,
} MelcoAmdFlashOp;

/* Command state machine positions. */
enum {
    MELCO_AMDFLASH_ST_READ,
    MELCO_AMDFLASH_ST_U1,           /* AA seen */
    MELCO_AMDFLASH_ST_U2,           /* AA 55 seen: a command follows */
    MELCO_AMDFLASH_ST_ERASE,        /* AA 55 80 */
    MELCO_AMDFLASH_ST_ERASE_U1,     /* AA 55 80 AA */
    MELCO_AMDFLASH_ST_ERASE_ARMED,  /* AA 55 80 AA 55: 30/10 erase */
    MELCO_AMDFLASH_ST_BYPASS_EXIT,  /* 90 in bypass: 00 leaves it */
};

/*
 * QEMU-free core of the melco-amdflash device: the array plus the AMD
 * command state machine.  A command write is recognised when every lane
 * it occupies holds the command byte (or zero) and at least `interleave`
 * lanes carry it, so both the 32-bit interleaved pair (interleave 4) and
 * the 16-bit bus (interleave 2) work.
 */
typedef struct MelcoAmdFlashCore {
    uint8_t *data;              /* the flash array */
    uint64_t size;
    uint32_t sector_len;
    uint8_t interleave;         /* 1, 2 or 4 lanes */
    uint16_t id_manufacturer;
    uint16_t id_device;

    unsigned state;
    uint8_t cycle_cmd;          /* command of the cycle the state is in */
    bool bypass;                /* unlock bypass entered (AA 55 20) */
    bool autoselect;            /* 90 seen: reads of the bank serve the IDs */
    uint64_t autoselect_bank;
    bool program_pending;       /* A0 seen: the next write is the data */
    uint64_t arm_addr;          /* address of the A0 that armed it */
} MelcoAmdFlashCore;

void melco_amdflash_core_init(MelcoAmdFlashCore *c, uint8_t *data,
                              uint64_t size, uint32_t sector_len,
                              uint8_t interleave, uint16_t id_manufacturer,
                              uint16_t id_device);
void melco_amdflash_core_reset(MelcoAmdFlashCore *c);

/*
 * Handle one write.  Returns true when the array changed, with the
 * changed range in the off/len out-params and the operation in op.
 */
bool melco_amdflash_core_write(MelcoAmdFlashCore *c, uint64_t addr,
                               uint64_t value, unsigned size,
                               uint64_t *off, uint64_t *len,
                               MelcoAmdFlashOp *op);

/*
 * Read callback: serves the ID block at the autoselect bank base while
 * autoselect is on, the array else.  The array itself is never touched, so a
 * backing file written at any time holds the real contents.
 */
uint64_t melco_amdflash_core_read(MelcoAmdFlashCore *c, uint64_t addr,
                                  unsigned size);

#endif /* MELCO_AMDFLASH_CORE_H */
