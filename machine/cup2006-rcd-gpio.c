/*
 * Volvo CU-P2006 board: cup2006-rcd-gpio device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup2006-internal.h"


/*
 * RCD device window (256 bytes, shadow-backed so the guest's
 * read-modify-write of +0x00 round-trips; +0x2C record register reads 0).
 * Every access is logged: one stderr line per distinct address/direction
 * and one qemu_log_mask line per access.
 */
/* The RCD log lives in the machine state (s->rcd_log; the capacity is
 * defined with the others in cup2006.c). */
static void cup2006_rcd_note(Cup2006MachineState *s, hwaddr phys,
                             bool is_write, unsigned size, uint64_t value)
{
    unsigned i;

    s->rcd_accesses++;
    for (i = 0; i < s->rcd_log_nr; i++) {
        if (s->rcd_log[i].addr == phys &&
            s->rcd_log[i].is_write == is_write) {
            return;
        }
    }
    if (s->rcd_log_nr == CUP2006_RCD_LOG_MAX) {
        return;
    }
    s->rcd_log[s->rcd_log_nr].addr = phys;
    s->rcd_log[s->rcd_log_nr].is_write = is_write;
    s->rcd_log_nr++;
    trace_cup2006_rcd_gpio_stub_rcd_register_size((uint32_t)phys, is_write ? "write" : "read", size, value);
}

static uint64_t cup2006_rcd_read(Cup2006MachineState *s, hwaddr base,
                                 hwaddr addr, unsigned size)
{
    hwaddr phys = base + addr;
    uint64_t value = 0;
    unsigned i;

    if (addr == CUP2006_RCD_RECORD) {
        /* STUB: no remote-control front end, so no record is ever received. */
        value = 0;
    } else if (addr + size <= CUP2006_RCD_SIZE) {
        for (i = 0; i < size; i++) {
            value |= (uint64_t)s->rcd_win[addr + i] << (8 * i);
        }
    }
    cup2006_rcd_note(s, phys, false, size, value);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: rcd read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
    return value;
}

static void cup2006_rcd_write(Cup2006MachineState *s, hwaddr base,
                              hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr phys = base + addr;
    unsigned i;

    if (addr + size <= CUP2006_RCD_SIZE) {
        for (i = 0; i < size; i++) {
            s->rcd_win[addr + i] = (value >> (8 * i)) & 0xff;
        }
    }
    cup2006_rcd_note(s, phys, true, size, value);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: rcd write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_rcd_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_rcd_read(opaque, CUP2006_RCD_BASE, addr, size);
}

static void cup2006_rcd_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_rcd_write(opaque, CUP2006_RCD_BASE, addr, value, size);
}

static uint64_t cup2006_rcd_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_rcd_read(opaque, CUP2006_RCD_A7_BASE, addr, size);
}

static void cup2006_rcd_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_rcd_write(opaque, CUP2006_RCD_A7_BASE, addr, value, size);
}

const MemoryRegionOps cup2006_rcd_ops = {
    .read = cup2006_rcd_p4_read,
    .write = cup2006_rcd_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_rcd_a7_ops = {
    .read = cup2006_rcd_a7_read,
    .write = cup2006_rcd_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Board GPIO/ADC block: see the CUP2006_GPIO_* comment above. One stderr
 * line per distinct register/direction; every access still goes to the
 * -d guest_errors trace.
 */
/* The GPIO log lives in the machine state (s->gpio_log; the capacity is
 * defined with the others in cup2006.c). */
static void cup2006_gpio_note(Cup2006MachineState *s, hwaddr phys,
                              bool is_write, unsigned size, uint64_t value)
{
    unsigned i;

    for (i = 0; i < s->gpio_log_nr; i++) {
        if (s->gpio_log[i].addr == phys &&
            s->gpio_log[i].is_write == is_write) {
            return;
        }
    }
    if (s->gpio_log_nr == CUP2006_GPIO_LOG_MAX) {
        if (!s->gpio_log_full_warned) {
            s->gpio_log_full_warned = true;
            trace_cup2006_rcd_gpio_stub_gpio_log_full(CUP2006_GPIO_LOG_MAX);
        }
        return;
    }
    s->gpio_log[s->gpio_log_nr].addr = phys;
    s->gpio_log[s->gpio_log_nr].is_write = is_write;
    s->gpio_log_nr++;
    trace_cup2006_rcd_gpio_stub_gpio_register_size((uint32_t)phys, is_write ? "write" : "read", size, value, CUP2006_GPIO_HDD_PORT, CUP2006_GPIO_HDD_BIT);
}

static uint64_t cup2006_gpio_read(Cup2006MachineState *s, hwaddr base,
                                  hwaddr addr, unsigned size)
{
    hwaddr phys = base + addr;
    unsigned port = addr / CUP2006_GPIO_PORT_STRIDE;
    hwaddr reg = addr % CUP2006_GPIO_PORT_STRIDE;
    uint64_t value;

    if (port >= CUP2006_GPIO_PORTS) {
        value = 0;
    } else {
        switch (reg) {
        case CUP2006_GPIO_DIR:
            value = s->gpio_dir[port];
            break;
        case CUP2006_GPIO_DATA:
            /* STUB: the power-good pin is strapped high in the data latch. */
            value = s->gpio_data[port];
            if (port == CUP2006_GPIO_HDD_PORT) {
                value |= CUP2006_GPIO_HDD_PIN;
            }
            break;
        case CUP2006_GPIO_INPUT:
            /* STUB: HDD power-good reads high; the DVD shutter-closed switch
             * reads 1 while the shutter is closed; every other pin reads 0.
             * The ACC line is driven by the Sub-CPU, normally high. */
            value = 0;
            if (port == CUP2006_GPIO_HDD_PORT) {
                value = CUP2006_GPIO_HDD_PIN;
                if (!s->dvd_shutter_open) {
                    value |= CUP2006_GPIO_SHUTTER_PIN;
                }
            }
            if (port == CUP2006_GPIO_ACC_PORT) {
                /* ACC/ignition (bit 19) and MOST/LAN-alive (bit 20) levels
                 * follow acc_on (normally high); acckey=on only lets F12
                 * change acc_on from the QEMU input layer. */
                if (s->acc_on) {
                    value |= CUP2006_GPIO_ACC_PIN;
                }
                value |= 1u << 20;          /* LAN alive */
            }
            break;
        default:
            value = 0;
            break;
        }
    }
    cup2006_gpio_note(s, phys, false, size, value);
    return value;
}

/* Defined in cup2006-subcpu.c: the onboard EEPROM's chip select (pin 83). */
void cup2006_eeprom_cs(Cup2006MachineState *s, bool level);

static void cup2006_gpio_write(Cup2006MachineState *s, hwaddr base,
                               hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr phys = base + addr;
    unsigned port = addr / CUP2006_GPIO_PORT_STRIDE;
    hwaddr reg = addr % CUP2006_GPIO_PORT_STRIDE;

    /* STUB: the direction and data registers latch; everything else is a
     * no-op (the input register is read-only). */
    if (port < CUP2006_GPIO_PORTS) {
        if (reg == CUP2006_GPIO_DIR) {
            s->gpio_dir[port] = (uint32_t)value;
        } else if (reg == CUP2006_GPIO_DATA) {
            s->gpio_data[port] = (uint32_t)value;
            if (port == CUP2006_EEPROM_CS_PORT) {
                cup2006_eeprom_cs(s, value & CUP2006_EEPROM_CS_BIT);
            }
        }
    }
    cup2006_gpio_note(s, phys, true, size, value);
}

static uint64_t cup2006_gpio_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_gpio_read(opaque, CUP2006_GPIO_BASE, addr, size);
}

static void cup2006_gpio_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    cup2006_gpio_write(opaque, CUP2006_GPIO_BASE, addr, value, size);
}

static uint64_t cup2006_gpio_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_gpio_read(opaque, CUP2006_GPIO_A7_BASE, addr, size);
}

static void cup2006_gpio_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    cup2006_gpio_write(opaque, CUP2006_GPIO_A7_BASE, addr, value, size);
}

const MemoryRegionOps cup2006_gpio_ops = {
    .read = cup2006_gpio_p4_read,
    .write = cup2006_gpio_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_gpio_a7_ops = {
    .read = cup2006_gpio_a7_read,
    .write = cup2006_gpio_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
