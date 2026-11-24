/*
 * Shared AMD NOR flash state machine for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include <string.h>

#include "melco-amdflash-core.h"

/* Is this write a command byte `cmd` on every lane it occupies? */
static bool core_cmd(const MelcoAmdFlashCore *c, uint64_t value,
                     unsigned size, uint8_t cmd)
{
    unsigned lanes = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint8_t b = (value >> (8 * i)) & 0xff;
        if (b) {
            if (b != cmd) {
                return false;
            }
            lanes++;
        }
    }
    return lanes >= c->interleave && (value & 0xff) == cmd;
}

void melco_amdflash_core_init(MelcoAmdFlashCore *c, uint8_t *data,
                              uint64_t size, uint32_t sector_len,
                              uint8_t interleave, uint16_t id_manufacturer,
                              uint16_t id_device)
{
    memset(c, 0, sizeof(*c));
    c->data = data;
    c->size = size;
    c->sector_len = sector_len;
    c->interleave = interleave;
    c->id_manufacturer = id_manufacturer;
    c->id_device = id_device;
    melco_amdflash_core_reset(c);
}

void melco_amdflash_core_reset(MelcoAmdFlashCore *c)
{
    c->state = MELCO_AMDFLASH_ST_READ;
    c->cycle_cmd = 0;
    c->bypass = false;
    c->autoselect = false;
    c->autoselect_bank = 0;
    c->program_pending = false;
    c->arm_addr = 0;
}

/* The ID block autoselect serves at the bank base. */
static const uint8_t *core_id_bytes(const MelcoAmdFlashCore *c)
{
    static _Thread_local uint8_t id[8];

    memset(id, 0, sizeof(id));
    id[0] = c->id_manufacturer & 0xff;
    id[1] = c->id_manufacturer >> 8;
    id[2] = c->id_device & 0xff;
    id[3] = c->id_device >> 8;
    return id;
}

bool melco_amdflash_core_write(MelcoAmdFlashCore *c, uint64_t addr,
                               uint64_t value, unsigned size,
                               uint64_t *off, uint64_t *len,
                               MelcoAmdFlashOp *op)
{
    *op = MELCO_AMDFLASH_OP_NONE;
    uint8_t cmd = value & 0xff;
    bool is_cmd;
    unsigned i;

    if (addr >= c->size) {
        return false;
    }

    is_cmd = cmd != 0 && core_cmd(c, value, size, cmd);

    /* An armed program cycle takes the next write as data, whatever its
     * value and wherever it lands: the A0 goes to an unlock address and the
     * data to the target address.  The exception is the other half of a
     * doubled A0 (see below): the same command at the paired address, which
     * leaves the cycle armed instead of programming it. */
    if (c->program_pending) {
        if (is_cmd && cmd == 0xa0 &&
            (addr + 4 == c->arm_addr || c->arm_addr + 4 == addr)) {
            return false;
        }
        c->program_pending = false;
        c->state = MELCO_AMDFLASH_ST_READ;
        c->cycle_cmd = 0;
        for (i = 0; i < size && addr + i < c->size; i++) {
            /* A program cycle can only clear bits: old & new.  Only an erase
             * sets bits back to 1. */
            c->data[addr + i] &= (value >> (8 * i)) & 0xff;
        }
        *off = addr;
        *len = i;
        *op = MELCO_AMDFLASH_OP_PROGRAM;
        return i > 0;
    }

    /* F0 resets to read mode from anywhere. */
    if (is_cmd && cmd == 0xf0) {
        melco_amdflash_core_reset(c);
        return false;
    }

    /*
     * The boards' flash bus is 32 bits wide over two 16-bit parts, and their
     * drivers write every command cycle to both halves (two word addresses,
     * four bytes apart, each carrying the byte on both lanes).  The second
     * write is the other half of the same cycle, so a repeat of the command
     * the cycle is on keeps the state instead of restarting it.
     */
    if (is_cmd && c->state != MELCO_AMDFLASH_ST_READ &&
        cmd == c->cycle_cmd) {
        return false;
    }

    switch (c->state) {
    case MELCO_AMDFLASH_ST_BYPASS_EXIT:
        /* 90 00 leaves unlock bypass.  Any other write is handled as in read
         * mode, so a missed 00 cannot wedge the state machine. */
        c->state = MELCO_AMDFLASH_ST_READ;
        if (!cmd) {
            c->bypass = false;
            return false;
        }
        /* fall through */
    case MELCO_AMDFLASH_ST_READ:
        if (is_cmd && cmd == 0xaa) {
            c->state = MELCO_AMDFLASH_ST_U1;
        } else if (is_cmd && cmd == 0xa0) {
            /* A0 arms a program cycle only in unlock bypass mode; otherwise
             * it needs the AA 55 unlock first. */
            if (c->bypass) {
                c->program_pending = true;
                c->arm_addr = addr;
            }
        } else if (is_cmd && cmd == 0x90) {
            if (c->bypass) {
                c->state = MELCO_AMDFLASH_ST_BYPASS_EXIT;
            } else {
                c->autoselect = true;
                c->autoselect_bank =
                    addr & ~(uint64_t)(c->sector_len - 1);
            }
        }
        /* Anything else (a lone 30 included) leaves the array unchanged. */
        break;
    case MELCO_AMDFLASH_ST_U1:
        c->state = (is_cmd && cmd == 0x55) ? MELCO_AMDFLASH_ST_U2
                                            : MELCO_AMDFLASH_ST_READ;
        break;
    case MELCO_AMDFLASH_ST_U2:
        if (is_cmd && cmd == 0xa0) {
            c->program_pending = true;
            c->arm_addr = addr;
            c->state = MELCO_AMDFLASH_ST_READ;
        } else if (is_cmd && cmd == 0x80) {
            c->state = MELCO_AMDFLASH_ST_ERASE;
        } else if (is_cmd && cmd == 0x20) {
            c->bypass = true;
            c->state = MELCO_AMDFLASH_ST_READ;
        } else if (is_cmd && cmd == 0x90) {
            c->autoselect = true;
            c->autoselect_bank = addr & ~(uint64_t)(c->sector_len - 1);
            c->state = MELCO_AMDFLASH_ST_READ;
        } else {
            c->state = MELCO_AMDFLASH_ST_READ;
        }
        break;
    case MELCO_AMDFLASH_ST_ERASE:
        c->state = (is_cmd && cmd == 0xaa) ? MELCO_AMDFLASH_ST_ERASE_U1
                                           : MELCO_AMDFLASH_ST_READ;
        break;
    case MELCO_AMDFLASH_ST_ERASE_U1:
        c->state = (is_cmd && cmd == 0x55) ? MELCO_AMDFLASH_ST_ERASE_ARMED
                                           : MELCO_AMDFLASH_ST_READ;
        break;
    case MELCO_AMDFLASH_ST_ERASE_ARMED:
        if (is_cmd && cmd == 0x30) {
            /* STUB: sector erase completes instantly. */
            uint64_t sector = addr & ~(uint64_t)(c->sector_len - 1);
            uint64_t slen = c->sector_len;

            if (sector + slen > c->size) {
                slen = c->size - sector;
            }
            memset(c->data + sector, 0xff, slen);
            c->state = MELCO_AMDFLASH_ST_READ;
            c->cycle_cmd = 0;
            *off = sector;
            *len = slen;
            *op = MELCO_AMDFLASH_OP_SECTOR_ERASE;
            return true;
        }
        if (is_cmd && cmd == 0x10) {
            /* STUB: chip erase completes instantly. */
            memset(c->data, 0xff, c->size);
            c->state = MELCO_AMDFLASH_ST_READ;
            c->cycle_cmd = 0;
            *off = 0;
            *len = c->size;
            *op = MELCO_AMDFLASH_OP_CHIP_ERASE;
            return true;
        }
        c->state = MELCO_AMDFLASH_ST_READ;
        break;
    }
    c->cycle_cmd = is_cmd ? cmd : 0;
    return false;
}

uint64_t melco_amdflash_core_read(MelcoAmdFlashCore *c, uint64_t addr,
                                  unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint64_t a = addr + i;
        uint8_t b;

        if (c->autoselect && a >= c->autoselect_bank &&
            a - c->autoselect_bank < 8) {
            b = core_id_bytes(c)[a - c->autoselect_bank];
        } else {
            b = (a < c->size) ? c->data[a] : 0;
        }
        value |= (uint64_t)b << (8 * i);
    }
    if (size < 4) {
        value &= (1u << (8 * size)) - 1;
    }
    return value;
}
