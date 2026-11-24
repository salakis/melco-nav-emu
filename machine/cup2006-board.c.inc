/*
 * CU-P2006 machine: board I/O: 16550 UART, self-test/ADC block, GPS register log, I2C.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static uint64_t cup2006_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t value = 0;

    switch (addr & (CUP2006_UART_SIZE - 1)) {
    case 0x04: /* IIR: no interrupt pending */
        value = 0x01;
        break;
    case 0x0a: /* LSR: THRE | TEMT, no data waiting */
        value = 0x60;
        break;
    case 0x0c: /* MSR: CTS | DSR asserted */
        value = 0x30;
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: uart read  pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n", cup2006_guest_pc(),
                  CUP2006_UART_BASE + addr, size, value);
    return value;
}

static void cup2006_uart_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: uart write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n", cup2006_guest_pc(),
                  CUP2006_UART_BASE + addr, size, value);
}

const MemoryRegionOps cup2006_unknown_ops = {
    .read = cup2006_unknown_read,
    .write = cup2006_unknown_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_board_ops = {
    .read = cup2006_board_read,
    .write = cup2006_board_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_board_a7_ops = {
    .read = cup2006_board_a7_read,
    .write = cup2006_board_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_uart_ops = {
    .read = cup2006_uart_read,
    .write = cup2006_uart_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * The board's A/D converter at 0xFF93C000: write ADST | 0x80 | channel to
 * start a conversion, then read the channel data register >> 6 (10 bits,
 * left-justified). STUB: conversion is instant (ADF clears on a 0 write).
 * STUB: channel inputs ch0..ch3 read 0 / 560 / hwrevadc= / 600.
 */
static unsigned cup2006_adc_channel(Cup2006MachineState *s, unsigned ch)
{
    const char *rev = s->hwrevadc;

    switch (ch) {
    case 1:
        return 560;
    case 2:
        return rev && *rev ? (unsigned)atoi(rev) & 0x3ff : 8;
    case 3:
        return 600;
    default:
        return 0;
    }
}

static uint64_t cup2006_selftest_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;

    if (addr < 8) {
        uint64_t v = (cup2006_adc_channel(s, addr >> 1) << 6) & 0xffc0;

        return (addr & 1) ? v >> 8 : v;
    }
    if (addr == CUP2006_ADC_CSR) {
        return s->adc_csr;
    }
    return cup2006_mmio_read(CUP2006_SELFTEST_BASE + addr, size);
}

static void cup2006_selftest_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Cup2006MachineState *s = opaque;

    if (addr == CUP2006_ADC_CSR) {
        uint16_t adf = s->adc_csr & value & CUP2006_ADC_ADF;

        s->adc_csr = (value & ~(CUP2006_ADC_ADF | CUP2006_ADC_ADST)) | adf;
        if (value & CUP2006_ADC_ADST) {
            s->adc_csr |= CUP2006_ADC_ADF;      /* converted at once */
        }
        return;
    }
    cup2006_mmio_write(CUP2006_SELFTEST_BASE + addr, value, size);
}

const MemoryRegionOps cup2006_selftest_ops = {
    .read = cup2006_selftest_read,
    .write = cup2006_selftest_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * GPS register block: command latch at +4, status at +0xC. One stderr line
 * per distinct register/direction keeps a plain run log readable while the
 * -d guest_errors trace still records every poll.
 */
/* The GPS log itself lives in the machine state (s->gps_log; the capacity
 * is defined with the others in cup2006.c). */
void cup2006_gps_note(Cup2006MachineState *s, hwaddr phys,
                             bool is_write, unsigned size, uint64_t value)
{
    unsigned i;

    for (i = 0; i < s->gps_log_nr; i++) {
        if (s->gps_log[i].addr == phys &&
            s->gps_log[i].is_write == is_write) {
            return;
        }
    }
    if (s->gps_log_nr == CUP2006_GPS_LOG_MAX) {
        if (!s->gps_log_full_warned) {
            s->gps_log_full_warned = true;
            trace_cup2006_board_stub_gps_log_full(CUP2006_GPS_LOG_MAX);
        }
        return;
    }
    s->gps_log[s->gps_log_nr].addr = phys;
    s->gps_log[s->gps_log_nr].is_write = is_write;
    s->gps_log_nr++;
    trace_cup2006_board_stub_gps_register_size((uint32_t)phys, is_write ? "write" : "read", size, value, CUP2006_GPS_STATUS_READY);
}

/*
 * I2C/PLL controller. See the register-map comment at
 * CUP2006_I2C_BASE: the flags below are the ones the driver waits for after
 * it writes +0x04; the delay is one standard-mode byte time.
 */
static void cup2006_i2c_start_completion(Cup2006MachineState *s)
{
    /*
     * STUB: a byte written to the address/data register (+0x20/+0x24) or a
     * control write with the start bit (+0x04 bit 0x08) is one byte transfer
     * that reports all of SENT/RECV_END/RECV_DONE/RX_READY/SEND_END at once
     * (the phases are not separately timed).
 */
    s->i2c_busy = true;
    s->i2c_pending |= CUP2006_I2C_ST_SENT | CUP2006_I2C_ST_RECV_END |
                      CUP2006_I2C_ST_RECV_DONE |
                      CUP2006_I2C_ST_RX_READY | CUP2006_I2C_ST_SEND_END;
    timer_mod(s->i2c_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CUP2006_I2C_XFER_NS);
}

void cup2006_i2c_complete(void *opaque)
{
    Cup2006MachineState *s = opaque;

    s->i2c_status |= s->i2c_pending;
    s->i2c_pending = 0;
    s->i2c_busy = false;
}

static uint64_t cup2006_i2c_read(void *opaque, hwaddr base, hwaddr addr,
                                 unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = base + addr;
    uint64_t value;

    switch (addr) {
    case CUP2006_I2C_CTRL:
        s->i2c_ctrl = (s->i2c_ctrl & ~CUP2006_I2C_CTRL_BUSY) |
                      (s->i2c_busy ? CUP2006_I2C_CTRL_BUSY : 0);
        value = s->i2c_ctrl;
        break;
    case CUP2006_I2C_STATUS:
        value = s->i2c_status;
        break;
    case CUP2006_I2C_DIV:
        value = s->i2c_div;
        break;
    case CUP2006_I2C_TX:
        value = s->i2c_tx;
        break;
    case CUP2006_I2C_RX:
        value = s->i2c_rx;
        break;
    default:
        value = 0;
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: i2c read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
    return value;
}

static void cup2006_i2c_write(void *opaque, hwaddr base, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = base + addr;

    switch (addr) {
    case CUP2006_I2C_CTRL:
        s->i2c_ctrl = (uint32_t)value;
        if (value & CUP2006_I2C_CTRL_START) {
            cup2006_i2c_start_completion(s);
        }
        break;
    case CUP2006_I2C_STATUS:
        /* Guest-writable flag register: the driver clears bits by writing
         * the surviving value. */
        s->i2c_status = (uint32_t)value;
        break;
    case CUP2006_I2C_DIV:
        s->i2c_div = (uint32_t)value;
        break;
    case CUP2006_I2C_TX:
        s->i2c_tx = (uint32_t)value;
        cup2006_i2c_start_completion(s);
        break;
    case CUP2006_I2C_RX:
        s->i2c_rx = (uint32_t)value;
        cup2006_i2c_start_completion(s);
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: i2c write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_i2c_phys_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    return cup2006_i2c_read(opaque, CUP2006_I2C_BASE, addr, size);
}

static void cup2006_i2c_phys_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    cup2006_i2c_write(opaque, CUP2006_I2C_BASE, addr, value, size);
}

static uint64_t cup2006_i2c_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_i2c_read(opaque, CUP2006_I2C_A7_BASE, addr, size);
}

static void cup2006_i2c_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_i2c_write(opaque, CUP2006_I2C_A7_BASE, addr, value, size);
}

const MemoryRegionOps cup2006_i2c_ops = {
    .read = cup2006_i2c_phys_read,
    .write = cup2006_i2c_phys_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_i2c_a7_ops = {
    .read = cup2006_i2c_a7_read,
    .write = cup2006_i2c_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
