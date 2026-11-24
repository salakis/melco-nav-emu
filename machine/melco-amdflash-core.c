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
    c->bypass = false;
    c->autoselect = false;
    c->program_pending = false;
    c->program_addr = 0;
}

static void core_autoselect(MelcoAmdFlashCore *c, uint64_t addr)
{
    uint64_t bank = addr & ~(uint64_t)(c->sector_len - 1);

    if (!c->autoselect && bank + sizeof(c->id_block) <= c->size) {
        memset(c->id_block, 0, sizeof(c->id_block));
        c->id_block[0] = c->id_manufacturer & 0xff;
        c->id_block[1] = c->id_manufacturer >> 8;
        c->id_block[2] = c->id_device & 0xff;
        c->id_block[3] = c->id_device >> 8;
        c->autoselect = true;
        c->autoselect_bank = bank;
    }
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

    /* An armed program cycle takes the next write to its address as data,
     * whatever its value.  AMD NOR: programming can only clear bits. */
    if (c->program_pending && addr == c->program_addr) {
        c->program_pending = false;
        c->state = MELCO_AMDFLASH_ST_READ;
        for (i = 0; i < size && addr + i < c->size; i++) {
            c->data[addr + i] &= (value >> (8 * i)) & 0xff;
        }
        *off = addr;
        *len = i;
        *op = MELCO_AMDFLASH_OP_PROGRAM;
        return i > 0;
    }

    is_cmd = cmd != 0 && core_cmd(c, value, size, cmd);

    /* F0 resets to read mode from anywhere. */
    if (is_cmd && cmd == 0xf0) {
        melco_amdflash_core_reset(c);
        return false;
    }

    switch (c->state) {
    case MELCO_AMDFLASH_ST_READ:
        if (is_cmd && cmd == 0xaa) {
            c->state = MELCO_AMDFLASH_ST_U1;
        } else if (is_cmd && cmd == 0xa0) {
            c->program_pending = true;
            c->program_addr = addr;
        } else if (is_cmd && cmd == 0x90) {
            core_autoselect(c, addr);
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
            c->program_addr = addr;
            c->state = MELCO_AMDFLASH_ST_READ;
        } else if (is_cmd && cmd == 0x80) {
            c->state = MELCO_AMDFLASH_ST_ERASE;
        } else if (is_cmd && cmd == 0x20) {
            c->bypass = true;
            c->state = MELCO_AMDFLASH_ST_READ;
        } else if (is_cmd && cmd == 0x90) {
            core_autoselect(c, addr);
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
            /* STUB: sector erase completes instantly.  More 30s may follow. */
            uint64_t sector = addr & ~(uint64_t)(c->sector_len - 1);
            uint64_t slen = c->sector_len;

            if (sector + slen > c->size) {
                slen = c->size - sector;
            }
            memset(c->data + sector, 0xff, slen);
            *off = sector;
            *len = slen;
            *op = MELCO_AMDFLASH_OP_SECTOR_ERASE;
            return true;
        }
        if (is_cmd && cmd == 0x10) {
            /* STUB: chip erase completes instantly. */
            memset(c->data, 0xff, c->size);
            *off = 0;
            *len = c->size;
            *op = MELCO_AMDFLASH_OP_CHIP_ERASE;
            return true;
        }
        c->state = MELCO_AMDFLASH_ST_READ;
        break;
    }
    return false;
}

uint64_t melco_amdflash_core_read(MelcoAmdFlashCore *c, uint64_t addr,
                                  unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint64_t a = addr + i;
        uint8_t b = 0;

        if (c->autoselect && a >= c->autoselect_bank &&
            a < c->autoselect_bank + sizeof(c->id_block)) {
            b = c->id_block[a - c->autoselect_bank];
        } else if (a < c->size) {
            b = c->data[a];
        }
        value |= (uint64_t)b << (8 * i);
    }
    if (size < 4) {
        value &= (1u << (8 * size)) - 1;
    }
    return value;
}
