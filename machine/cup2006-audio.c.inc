/*
 * CU-P2006 machine: sound output. The SSI at 0xFF911000 (SH7764 hw manual
 * sec. 18), fed by a ping-pong DMA channel, streams 16-bit stereo PCM to a
 * QEMU audio backend at the SSI's sample rate. STUB: the SSI's own serial
 * bus is not modelled.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* The SSI's bits (SH7764 SSICR / SSISR). */
#define CUP2006_SSICR_EN        0x00000001
#define CUP2006_SSICR_MUEN      0x00000008
#define CUP2006_SSICR_CKDV      0x00000070
#define CUP2006_SSICR_DMEN      0x10000000
#define CUP2006_SSISR_IDST      0x00000001
#define CUP2006_SSISR_DIRQ      0x01000000

static unsigned cup2006_ssi_rate(Cup2006MachineState *s)
{
    switch (s->ssi_cr & CUP2006_SSICR_CKDV) {
    case 0x60:
        return 11025;
    case 0x50:
        return 22050;
    default:
        return 44100;
    }
}

static uint64_t cup2006_ssi_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint32_t v = 0;

    switch (addr & ~3u) {
    case 0x0:
        v = s->ssi_cr;
        break;
    case 0x4:
        /* Idle while disabled; once enabled the transmit side always wants
         * data (DIRQ): the DMA keeps it fed. */
        v = (s->ssi_cr & CUP2006_SSICR_EN) ? CUP2006_SSISR_DIRQ
                                            : CUP2006_SSISR_IDST;
        break;
    default:
        break;
    }
    return v >> (8 * (addr & 3));
}

static void cup2006_audio_voice(Cup2006MachineState *s);

/* PCM frames the guest delivered; printed about once a second
 * while it plays and when it stops. */
static void cup2006_audio_summary(Cup2006MachineState *s)
{
    s->audio_report_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    trace_cup2006_audio_summary_frames_lves_rate(s->audio_frames, s->adma_halves, cup2006_ssi_rate(s), s->audio_stalls);
}

static void cup2006_ssi_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup2006MachineState *s = opaque;

    if (s->ssi_writes++ < 16) {
        trace_cup2006_audio_ssi_write_prix64_pc(addr, value, cup2006_guest_pc());
    }
    if ((addr & ~3u) == 0x0 && size == 4) {
        s->ssi_cr = value;
        cup2006_audio_voice(s);
    }
}

static const MemoryRegionOps cup2006_ssi_ops = {
    .read = cup2006_ssi_read,
    .write = cup2006_ssi_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void cup2006_audio_callback(void *opaque, int avail);

/* Open/activate the host voice while the SSI plays, at its rate. */
static void cup2006_audio_voice(Cup2006MachineState *s)
{
    bool on = s->adma_running && (s->ssi_cr & CUP2006_SSICR_EN) &&
              (s->ssi_cr & CUP2006_SSICR_DMEN);
    unsigned rate = cup2006_ssi_rate(s);

    if (s->audio_playing && !on) {
        cup2006_audio_summary(s);
    }
    s->audio_playing = on;
    if (!s->audio_be) {
        return;
    }
    if (on && (!s->audio_voice || s->audio_rate != rate)) {
        struct audsettings as = {
            .freq = rate, .nchannels = 2, .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        s->audio_voice = audio_be_open_out(s->audio_be, s->audio_voice,
                                           "cup2006.ssi", s,
                                           cup2006_audio_callback, &as);
        s->audio_rate = rate;
    }
    if (s->audio_voice) {
        audio_be_set_active_out(s->audio_be, s->audio_voice, on);
    }
}

#define CUP2006_ADMA_TICK_MS    5

/* Status bit 1, and the interrupt if enabled, for a finished half. */
static void cup2006_adma_raise(Cup2006MachineState *s)
{
    uint32_t enable = cup2006_reg32_read(s->dvd5_reg, CUP2006_DMAC_ENABLE, 4);
    uint32_t status = cup2006_reg32_read(s->dvd5_reg, CUP2006_DMAC_STATUS, 4);

    cup2006_reg32_write(s->dvd5_reg, CUP2006_DMAC_STATUS,
                        status | CUP2006_ADMA_BIT, 4);
    if ((enable & CUP2006_ADMA_BIT) && s->adma_src && !s->adma_src->asserted) {
        s->adma_src->prio = CUP2006_ADMA_INTC_PRIO;
        s->adma_src->enable_count = s->adma_src->enable_max;
        sh_intc_toggle_source(s->adma_src, 0, 1);
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
    }
}

/* One finished half: the driver is told to refill it. */
static void cup2006_adma_complete(Cup2006MachineState *s)
{
    uint32_t enable = cup2006_reg32_read(s->dvd5_reg, CUP2006_DMAC_ENABLE, 4);

    s->adma_halves++;
    if ((enable & CUP2006_ADMA_BIT) && s->adma_src && s->adma_pending < 2) {
        s->adma_pending++;
    }
    cup2006_adma_raise(s);
}

/* The ISR's write of bit 1 to 0xFF801810 (cup2006_dvd5_write).  If the other
 * half finished before this acknowledge, its interrupt follows at once: the
 * driver refills one half per interrupt. */
static void cup2006_adma_ack(Cup2006MachineState *s)
{
    uint32_t status = cup2006_reg32_read(s->dvd5_reg, CUP2006_DMAC_STATUS, 4);

    cup2006_reg32_write(s->dvd5_reg, CUP2006_DMAC_STATUS,
                        status & ~CUP2006_ADMA_BIT, 4);
    if (s->adma_pending) {
        s->adma_pending--;
    }
    if (s->adma_src && s->adma_src->asserted) {
        sh_intc_toggle_source(s->adma_src, 0, -1);
    }
    if (s->adma_pending) {
        cup2006_adma_raise(s);
    }
}

/*
 * Move up to `want` frames from the descriptors to the SSI, finishing
 * halves as they empty. Returns the frames moved.
 */
static uint64_t cup2006_adma_pull(Cup2006MachineState *s, uint64_t want)
{
    uint8_t buf[4 * 512];
    uint64_t moved = 0;

    while (moved < want) {
        unsigned d = s->adma_desc;
        /*
         * Ping-pong: the other half plays on while the driver refills the
         * one that just finished.  STUB: when both halves have finished and
         * neither interrupt has been acknowledged, the DMA waits instead of
         * replaying a half the driver has not refilled (a real channel would
         * run on; this only happens when the guest falls behind real time,
         * or within one large host pull).
         */
        if (s->adma_pos == 0 && s->adma_pending >= 2) {
            s->audio_stalls++;
            break;
        }
        uint32_t sar = s->adma_reg[d ? 0x0c / 4 : 0x00 / 4] & 0x1fffffff;
        uint32_t count = s->adma_reg[d ? 0x14 / 4 : 0x08 / 4];
        uint64_t take = MIN(want - moved, (uint64_t)(count > s->adma_pos ?
                                                     count - s->adma_pos : 0));

        take = MIN(take, sizeof(buf) / 4);
        if (take) {
            physical_memory_read(sar + 4 * s->adma_pos, buf, 4 * take);
            if (s->ssi_cr & CUP2006_SSICR_MUEN) {
                memset(buf, 0, 4 * take);
            }
            if (s->audio_voice) {
                take = audio_be_write(s->audio_be, s->audio_voice, buf,
                                      4 * take) / 4;
                if (!take) {
                    break;              /* the backend is full: wait */
                }
            }
            s->adma_pos += take;
            s->audio_frames += take;
            moved += take;
        }
        if (s->adma_pos >= count) {
            cup2006_adma_complete(s);
            s->adma_desc ^= 1;
            s->adma_pos = 0;
            if (!take) {
                break;                  /* an empty descriptor: once per call */
            }
        }
    }
    return moved;
}

/*
 * With a backend open, the backend paces the stream (QEMU's sound-card
 * convention), so the guest's DMA runs at the real output rate.
 */
static void cup2006_audio_callback(void *opaque, int avail)
{
    Cup2006MachineState *s = opaque;

    if (s->audio_playing && avail >= 4) {
        cup2006_adma_pull(s, avail / 4);
    }
}

/* Without a backend, virtual time paces the stream at the SSI's rate. */
static void cup2006_adma_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->adma_last_ns;
    double due;
    uint64_t n;

    timer_mod(s->adma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              CUP2006_ADMA_TICK_MS);
    s->adma_last_ns = now;
    if (!s->audio_playing || elapsed <= 0) {
        return;
    }
    if (!s->audio_voice) {
        due = (double)elapsed * cup2006_ssi_rate(s) / NANOSECONDS_PER_SECOND +
              s->adma_frac;
        n = (uint64_t)due;
        s->adma_frac = due - n;
        cup2006_adma_pull(s, n);
    }
    if (now - s->audio_report_ns >= NANOSECONDS_PER_SECOND) {
        cup2006_audio_summary(s);
    }
}

static uint64_t cup2006_adma_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;

    return cup2006_reg32_read((uint8_t *)s->adma_reg, addr, size);
}

static void cup2006_adma_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Cup2006MachineState *s = opaque;

    cup2006_reg32_write((uint8_t *)s->adma_reg, addr, value, size);
    if (s->adma_writes++ < 24) {
        trace_cup2006_audio_dma_write_prix64_pc(addr, value, cup2006_guest_pc());
    }
    if ((addr & ~3u) == 0x2c && (value & 3)) {
        /* Start: descriptor A first, then B, A,... */
        s->adma_running = true;
        s->adma_pending = 0;
        s->adma_desc = 0;
        s->adma_pos = 0;
        s->adma_frac = 0;
        s->adma_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (s->adma_starts++ < 8) {
            trace_cup2006_audio_dma_start_words_hz(s->adma_starts, s->adma_reg[0x08 / 4], s->adma_reg[0x14 / 4], cup2006_ssi_rate(s));
        }
        cup2006_audio_voice(s);
    } else if ((addr & ~3u) == 0x30 && (value & 1)) {
        s->adma_running = false;
        cup2006_audio_voice(s);
    }
}

static const MemoryRegionOps cup2006_adma_ops;

static void cup2006_audio_init(Cup2006MachineState *s, MachineState *ms,
                               MemoryRegion *sysmem, SuperHCPU *cpu)
{
    struct intc_desc *desc = cpu->env.intc_handle;
    unsigned i;

    memory_region_init_io(&s->ssi_regs, NULL, &cup2006_ssi_ops, s,
                          "cup2006-ssi", CUP2006_SSI_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_SSI_BASE,
                                        &s->ssi_regs, 1);
    memory_region_init_io(&s->ssi_a7_regs, NULL, &cup2006_ssi_ops, s,
                          "cup2006-ssi-a7", CUP2006_SSI_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_SSI_A7_BASE,
                                        &s->ssi_a7_regs, 1);
    memory_region_init_io(&s->adma_regs, NULL, &cup2006_adma_ops, s,
                          "cup2006-audio-dma", CUP2006_ADMA_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_ADMA_BASE,
                                        &s->adma_regs, 1);
    memory_region_init_io(&s->adma_a7_regs, NULL, &cup2006_adma_ops, s,
                          "cup2006-audio-dma-a7", CUP2006_ADMA_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_ADMA_A7_BASE,
                                        &s->adma_a7_regs, 1);
    s->adma_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cup2006_adma_tick, s);
    timer_mod(s->adma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              CUP2006_ADMA_TICK_MS);

    /* INTEVT 0xBC0 on the free sh7750 DMAC_DMTE1 source (no DMAC here). */
    for (i = 0; desc && i < desc->nr_sources; i++) {
        struct intc_source *src = &desc->sources[i];

        if (src->vect != CUP2006_ADMA_INTC_VECT) {
            continue;
        }
        src->vect = CUP2006_ADMA_INTEVT;
        src->prio = CUP2006_ADMA_INTC_PRIO;
        src->enable_count = 0;
        src->enable_max = 0;
        src->asserted = 0;
        src->pending = 0;
        src->parent = desc;
        s->adma_src = src;
        break;
    }
    if (!s->adma_src) {
        trace_cup2006_audio_no_free_sh_intc(CUP2006_ADMA_INTEVT);
    }

    if (ms->audiodev) {
        Error *err = NULL;

        s->audio_be = audio_be_by_name(ms->audiodev, &err);
        if (!s->audio_be) {
            error_report_err(err);
            exit(1);
        }
        trace_cup2006_audio_sound_output_on_audiodev(ms->audiodev);
    }
}

static const MemoryRegionOps cup2006_adma_ops = {
    .read = cup2006_adma_read,
    .write = cup2006_adma_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
