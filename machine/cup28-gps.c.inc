/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

/*
 * CUP28 GPS receiver UART (earlier VNS board).
 *
 * Evidence (ROM CUP28.SHx.ORG_v00001102.nb0, gps.dll "GPS-CUP28-0.01"):
 *  - gps.dll maps a 16550-style UART at 0xa7300000 with a 4-byte register
 *    stride (+0x14 = LSR) and CPLD +0x04 (literals at 0x10820a2..0x10820e8).
 *  - NK RX handler 0x800860a8 reads LSR then RBR per interrupt, stores the
 *    pair in the RAM ring 0xac000810..0xac000aff, frames bytes 0xC6..0xDA
 *    and returns SYSINTR 21; it acknowledges CPLD +0x10 bit 0.
 *  - NK TX handler 0x80086018 writes THR while LSR bit 5 is set and the
 *    0xac000b00 ring has data; when empty it clears IER bit 1 and CPLD +0x04
 *    bit 0; it acknowledges CPLD +0x00 bit 0.
 *  - NK 0x80083a74/0x80083a7c HookInterrupt index 0 (INTEVT 0x200) to the
 *    +0x00 & +0x04 dispatcher 0x80084e00 and index 1 (INTEVT 0x220) to the
 *    +0x10 & +0x14 dispatcher 0x80084ea0; bit 0 of each is this UART.
 *  - NK InterruptEnable case 0x15 (0x80083cdc) clears +0x00/+0x10 bit 0,
 *    writes FCR=0, clears LCR.DLAB, sets IER bit 0 and CPLD +0x14 bit 0.
 *
 * STUB: the local UART has an empty receiver and immediate transmission.
 * The vehicle's GPS receiver is on MOST. The UART's TX-ready interrupt
 * follows the CPLD enable and acknowledgement registers.
 */

#define CUP28_GPS_BASE 0x07300000

static void cup28_gps_line(struct intc_source *src, bool active)
{
    if (!src) {
        return;
    }
    src->prio = 3;
    src->enable_count = src->enable_max;
    if (!!active != !!src->asserted) {
        sh_intc_toggle_source(src, 0, active ? 1 : -1);
        if (active) {
            cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
        }
    }
}

static bool cup28_gps_tx_int(Cup28MachineState *s)
{
    return (s->p28_gps_ier & 2) != 0; /* THR is always empty */
}

void cup28_gps_irq(Cup28MachineState *s)
{
    uint8_t *c = s->p28_dvd_control_reg;
    uint32_t tx = ldl_le_p(c);

    /* Acknowledgement clears the latch once the UART stops requesting IRQs. */
    if (cup28_gps_tx_int(s)) {
        tx |= 1;
    }
    stl_le_p(c, tx);
    cup28_gps_line(s->p28_gps_tx_src, tx & ldl_le_p(c + 0x04) & 1);
}

static uint64_t cup28_gps_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    uint64_t v = 0;
    bool dlab = s->p28_gps_lcr & 0x80;

    switch (addr >> 2) {
    case 0: /* RBR / DLL */
        if (dlab) {
            v = s->p28_gps_dll;
        }
        break;
    case 1:
        v = dlab ? s->p28_gps_dlm : s->p28_gps_ier;
        break;
    case 2: /* IIR: THR empty, or no interrupt pending */
        v = cup28_gps_tx_int(s) ? 0x02 : 0x01;
        break;
    case 3:
        v = s->p28_gps_lcr;
        break;
    case 4:
        v = s->p28_gps_mcr;
        break;
    case 5: /* LSR: THRE | TEMT */
        v = 0x60;
        break;
    case 6: /* MSR: CTS, DSR, DCD asserted by the receiver */
        v = 0xb0;
        break;
    case 7:
        v = s->p28_gps_scr;
        break;
    }
    cup28_gps_irq(s);
    return v;
}

static void cup28_gps_write(void *opaque, hwaddr addr, uint64_t v,
                            unsigned size)
{
    Cup28MachineState *s = opaque;
    bool dlab = s->p28_gps_lcr & 0x80;

    switch (addr >> 2) {
    case 0:
        if (dlab) {
            s->p28_gps_dll = v;
        }
        break;
    case 1:
        if (dlab) {
            s->p28_gps_dlm = v;
        } else {
            s->p28_gps_ier = v & 0x0f;
        }
        break;
    case 3:
        s->p28_gps_lcr = v;
        break;
    case 4:
        s->p28_gps_mcr = v;
        break;
    case 7:
        s->p28_gps_scr = v;
        break;
    }
    cup28_gps_irq(s);
}

static const MemoryRegionOps cup28_gps_ops = {
    .read = cup28_gps_read,
    .write = cup28_gps_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void cup28_gps_init(Cup28MachineState *s, MemoryRegion *mem,
                           SuperHCPU *cpu)
{
    struct intc_desc *desc = cpu->env.intc_handle;

    for (unsigned i = 0; desc && i < desc->nr_sources; i++) {
        struct intc_source *src = &desc->sources[i];

        if (src->vect == 0x200 && !s->p28_gps_tx_src) {
            s->p28_gps_tx_src = src;
            break;
        }
    }
    memory_region_init_io(&s->p28_gps_io, NULL, &cup28_gps_ops, s,
                          "cup28-gps-uart", 0x20);
    memory_region_add_subregion_overlap(mem, CUP28_GPS_BASE, &s->p28_gps_io, 2);
}

/*
 * CUP28 dead-reckoning sensor sampler (gyro, speed pulses).
 *
 * Evidence (NK 0x80083ac4 HookInterrupt index 10 = INTEVT 0x340 to ISR
 * 0x80085998): the ISR clears CPLD +0x108 bit 10 and GPIO +0x10 bit 8,
 * reads 0xa7420000 but replaces its value with zero before writing the
 * pulse field (0x800859cc). It reads the 32-bit gyro word 0xa7800018,
 * the 16-bit reference 0xa7800014 (0 or 0xffff flags the record 0x80) and
 * GPIO +0x54, appends a 12-byte big-endian record to the RAM ring
 * 0xac001020..0xac0017a0, wakes srd.dll every N records (SYSINTR 0x14),
 * then acknowledges CPLD +0x70 bit 3 and restores +0x108 bit 10 and GPIO
 * +0x10 bit 8.  ClassLib NGyroData::ParseDirectData (0x155eefc) reads the
 * pulse field from bytes 2..3 and the rate as 0x140000 - bytes 4..7.
 * ClassLib 0x0155f860 replaces the pulse field with deltas from MOST.
 *
 * STUB: nominal 100 ms sampler and mid-scale reference. With a track,
 * speed and clockwise yaw drive the inputs even when the GPS loses fix.
 * The CPLD routes the pending bit at +0x70 only after the IRL priority
 * is programmed, as for the DVD and MOST sources.
 */
#define CUP28_SNS_PERIOD_NS 100000000LL
/* STUB: gyro scale calibrated against the guest's map position handler.
 * Parsed gyro words 0x80000 and 0x100000 turn about -20 and +60 deg/s.
 * ParseDirectData subtracts raw from 0x140000. */
#define CUP28_SNS_GYRO_CENTRE 0x0a0000u
#define CUP28_SNS_GYRO_PER_DPS 6553.6
#define CUP28_SNS_REFERENCE 0x8000u
/* STUB: nominal transducer scale from the MMM+ locator defaults. */
#define CUP28_SNS_PULSES_PER_M 0.44902

void cup28_sns_irq(Cup28MachineState *s)
{
    uint8_t *c = s->p28_dvd_control_reg;

    if (!s->p28_sns_src) {
        return;
    }
    /* NK 0x80083b2c programs priority 5 in +0x104[11:8] for IRL10
     * (INTEVT 0x340). The disc loader leaves that field at reset. */
    cup28_gps_line(s->p28_sns_src, (ldl_le_p(c + 0x70) & 8) &&
                                   (ldl_le_p(c + 0x108) & 0x400) &&
                                   ((ldl_le_p(c + 0x104) >> 8) & 15) == 5);
}

static void cup28_sns_tick(void *opaque)
{
    Cup28MachineState *s = opaque;
    uint8_t *c = s->p28_dvd_control_reg;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int32_t gyro = CUP28_SNS_GYRO_CENTRE;

    if (s->track) {
        int64_t ms = now / 1000000;
        const MelcoSample *sample = melco_track_sample(s->track, ms);
        /* ClassLib 0x0153be94 rejects deltas above 19.  melco_pulses caps
         * the delta and carries any excess forward if a timer callback
         * ran late. */
        unsigned whole = melco_pulses(&s->p28_sns_pulse_fraction,
                                      melco_track_odometer_m(s->track, ms),
                                      CUP28_SNS_PULSES_PER_M, 19);

        s->p28_most_wheel_counter += whole;
        if (s->p28_most_wheel_on && s->p28_most_vc_complete) {
            cup28_most_wheel_send(s);
        }
        gyro -= lrint(sample->yaw_dps * CUP28_SNS_GYRO_PER_DPS);
    }

    /* Present this sample on the sensor inputs, then raise the line. */
    s->p28_eeprom_reg[0x18 >> 2] = gyro;
    s->p28_eeprom_reg[0x14 >> 2] = CUP28_SNS_REFERENCE;
    stl_le_p(c + 0x70, ldl_le_p(c + 0x70) | 8);
    cup28_sns_irq(s);
    timer_mod(s->p28_sns_timer, now + CUP28_SNS_PERIOD_NS);
}

static uint64_t cup28_pulse_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0; /* STUB: the vehicle supplies its wheel counter over MOST. */
}

static void cup28_pulse_write(void *opaque, hwaddr addr, uint64_t v,
                              unsigned size)
{
}

static const MemoryRegionOps cup28_pulse_ops = {
    .read = cup28_pulse_read,
    .write = cup28_pulse_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void cup28_sns_init(Cup28MachineState *s, MemoryRegion *mem,
                           SuperHCPU *cpu)
{
    struct intc_desc *desc = cpu->env.intc_handle;

    for (unsigned i = 0; desc && i < desc->nr_sources; i++) {
        if (desc->sources[i].vect == 0x340) {
            s->p28_sns_src = &desc->sources[i];
            break;
        }
    }
    /* STUB: CPLD +0x108 is a per-source enable mask (ISRs 0x800858ec,
     * 0x8008591c, 0x80085998, 0x80085b9c each clear their bit on entry).
     * The sampler ISR restores bit 10 on exit, InterruptEnable(0x14) is a
     * no-op (0x80083cdc) and NK init only reads +0x108 (0x80083afe), so the
     * sampler bit must be set at CPLD reset for the first sample to arrive. */
    stl_le_p(s->p28_dvd_control_reg + 0x108,
             ldl_le_p(s->p28_dvd_control_reg + 0x108) | 0x400);
    memory_region_init_io(&s->p28_pulse_io, NULL, &cup28_pulse_ops, s,
                          "cup28-speed-pulse", 0x10);
    memory_region_add_subregion_overlap(mem, 0x07420000, &s->p28_pulse_io, 2);
    s->p28_sns_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cup28_sns_tick, s);
    timer_mod(s->p28_sns_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CUP28_SNS_PERIOD_NS);
}
