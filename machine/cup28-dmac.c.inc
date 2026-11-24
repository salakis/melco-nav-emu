/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

/* Earlier-board SH775x DMAC channel 0.
 * Evidence: dvd.dll 0x01017c4c programs SAR0=b7000000, DAR0=ac008800,
 * TCR0=0x1402, CHCR0=0x4025.
 * SH7751 manual 14.2.1-5: register offsets, TS=010 (16 bits), DE/TE/IE.
 * STUB: only external DVD-source requests are supported; transfer is instant.
 */
static void cup28_dmac_irq(Cup28MachineState *s)
{
    struct intc_source *src = s->p28_dmac_src;
    uint32_t chcr = ldl_le_p(s->p28_dmac_reg + 0x0c);
    bool active = (chcr & 6) == 6;

    if (!src || active == !!src->asserted) {
        return;
    }
    /* STUB: fixed priority; the IRQ still follows CHCR.IE/TE and its ack. */
    src->prio = 1;
    src->enable_max = 0;
    src->enable_count = 0;
    sh_intc_toggle_source(src, 0, active ? 1 : -1);
    if (active) {
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
    }
}

void cup28_dmac_try(Cup28MachineState *s)
{
    uint32_t chcr = ldl_le_p(s->p28_dmac_reg + 0x0c);
    uint32_t dmaor = ldl_le_p(s->p28_dmac_reg + 0x40);
    uint32_t sar = ldl_le_p(s->p28_dmac_reg);
    uint32_t dar = ldl_le_p(s->p28_dmac_reg + 4);
    uint32_t count = ldl_le_p(s->p28_dmac_reg + 8) & 0xffffff;
    unsigned ts = (chcr >> 4) & 7;
    static const unsigned sizes[8] = {8, 1, 2, 4, 32, 0, 0, 0};
    size_t available = s->p28_dvd_stream_len;
    size_t len = (size_t)count * sizes[ts];

    if (!available || !len || (chcr & 3) != 1 || (dmaor & 7) != 1 ||
        !sizes[ts] || (chcr & 0xf000) != 0x4000) {
        return;
    }
    /* The device source window is 0xb7000000 for the OS (dvd.dll
     * 0x01017c4c) and 0xb4000020 for the M-loader (CUP28.MLD.ORG
     * programs SAR0=b4000020). */
    if ((sar & 0x1fffffff) != 0x17000000 &&
        (sar & 0x1ffffff0) != 0x14000020) {
        return;
    }
    /* First-read mode requests more than the stream carries (the MLD's
     * priming read asks 0x3800 for a 0x3004 stream and discards it);
     * the device supplies what it has and the DMAC ends at its count.
     * Transfer the stream, never invented bytes past it. */
    if (len > available) {
        len = available;
    }
    /* The guest programs the destination. Never use a software buffer VA. */
    if (address_space_write(&address_space_memory, dar & 0x1fffffff,
                            MEMTXATTRS_UNSPECIFIED, s->p28_dvd_stream,
                            len) != MEMTX_OK) {
        stl_le_p(s->p28_dmac_reg + 0x40, dmaor | 4); /* address error */
        return;
    }
    stl_le_p(s->p28_dmac_reg + 4, dar + len);
    stl_le_p(s->p28_dmac_reg + 8, 0);
    stl_le_p(s->p28_dmac_reg + 0x0c, chcr | 2);
    g_free(s->p28_dvd_stream);
    s->p28_dvd_stream = NULL;
    s->p28_dvd_stream_len = 0;
    cup28_dmac_irq(s);
}

static uint64_t cup28_dmac_read(void *opaque, hwaddr off, unsigned size)
{
    Cup28MachineState *s = opaque;
    return cup28_register_read(s->p28_dmac_reg, off, size);
}

static void cup28_dmac_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    Cup28MachineState *s = opaque;
    cup28_register_write(s->p28_dmac_reg, off, value, size);
    cup28_dmac_irq(s);
    cup28_dmac_try(s);
}

static const MemoryRegionOps cup28_dmac_ops = {
    .read = cup28_dmac_read, .write = cup28_dmac_write,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void cup28_dmac_init(Cup28MachineState *s, MemoryRegion *sysmem,
                             SuperHCPU *cpu)
{
    struct intc_desc *desc = cpu->env.intc_handle;
    for (unsigned i = 0; desc && i < desc->nr_sources; i++) {
        if (desc->sources[i].vect == 0x640) {
            s->p28_dmac_src = &desc->sources[i];
            break;
        }
    }
    memory_region_init_io(&s->p28_dmac_io, NULL, &cup28_dmac_ops, s,
                          "cup28-dmac", 0x100);
    memory_region_add_subregion_overlap(sysmem, 0xffa00000, &s->p28_dmac_io, 3);
    memory_region_init_alias(&s->p28_dmac_a7, NULL, "cup28-dmac-a7",
                             &s->p28_dmac_io, 0, 0x100);
    memory_region_add_subregion_overlap(sysmem, 0x1fa00000, &s->p28_dmac_a7, 3);
}
