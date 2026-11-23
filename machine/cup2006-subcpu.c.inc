/*
 * CU-P2006 machine: SCIFs and the SUB1: link: Sub-CPU peer, keys, wheel pulses, power/ACC.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* SCIF page 0xFF92A000: writes are stored so the guest's init reads back;
 * SCFSR overlaid with the TX-ready STUB value; one stderr line per
 * distinct offset/direction.  The log lives in the machine state
 * (s->scif_log; the capacity is defined with the others in cup2006.c). */
static void cup2006_scif_note(Cup2006MachineState *s, hwaddr phys,
                              bool is_write, unsigned size, uint64_t value)
{
    unsigned i;

    if (!s->verbose) {
        return;
    }

    for (i = 0; i < s->scif_log_nr; i++) {
        if (s->scif_log[i].addr == phys &&
            s->scif_log[i].is_write == is_write) {
            return;
        }
    }
    if (s->scif_log_nr == CUP2006_SCIF_LOG_MAX) {
        if (!s->scif_log_full_warned) {
            s->scif_log_full_warned = true;
            cup2006_dbg(s, "cup2006: STUB scif log full (%u addresses);"
                    " further accesses are not listed\n", CUP2006_SCIF_LOG_MAX);
        }
        return;
    }
    s->scif_log[s->scif_log_nr].addr = phys;
    s->scif_log[s->scif_log_nr].is_write = is_write;
    s->scif_log_nr++;
    cup2006_dbg(s, "cup2006: STUB scif register 0x%08" PRIx32
            " (%s, size=%u) value=0x%" PRIx64
            "; SCFSR +0x010 = 0x%04x (TEND|TDFE) | RDF\n",
            (uint32_t)phys, is_write ? "write" : "read", size, value,
            CUP2006_SCIF_SCFSR_RDY);
}

/* 8-bit reversal, the transform Sub_SCIF applies to every wire byte. */
static uint8_t cup2006_scif_rev8(uint8_t v)
{
    v = (uint8_t)(((v & 0xf0) >> 4) | ((v & 0x0f) << 4));
    v = (uint8_t)(((v & 0xcc) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xaa) >> 1) | ((v & 0x55) << 1));
    return v;
}

static void cup2006_scif_rx_arm(Cup2006MachineState *s)
{
    timer_mod(s->scif_rx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CUP2006_SCIF_BYTE_NS);
}

/*
 * STUB: one reply byte per simulated millisecond. The SH7751 SCIF would
 * clock bytes out at the SCBRR rate; the driver only needs each byte to
 * arrive after its clear-RDF drain, not inside it.
 */
static void cup2006_scif_rx_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;

    if (s->scif_rx_byte < 0 && s->scif_rxq_head != s->scif_rxq_tail) {
        s->scif_rx_byte = s->scif_rxq[s->scif_rxq_head++ % CUP2006_EEPROM_RXQ];
    }
}

static void cup2006_scif_rx_push(Cup2006MachineState *s, uint8_t byte)
{
    if (s->scif_rxq_tail - s->scif_rxq_head >= CUP2006_EEPROM_RXQ) {
        return;
    }
    s->scif_rxq[s->scif_rxq_tail++ % CUP2006_EEPROM_RXQ] = byte;
}

/* The onboard EEPROM is a Microwire 93C46, 64 x 16-bit words, on this SCIF
 * in clocked mode (SCSMR 0x80), bytes bit-reversed on the wire.
 * STUB: programming is instant, so DO reads ready at once. */
/* Save the EEPROM image to <path>.tmp and rename it over <path> only when
 * complete, so an interrupted save never leaves an empty or short file
 * (which would load back as erased and lose the old contents). */
static void cup2006_eeprom_save(Cup2006MachineState *s)
{
    uint8_t img[2 * CUP2006_EEPROM_WORDS];
    g_autofree char *tmp = NULL;
    FILE *f;
    unsigned i;
    bool ok;

    if (!s->eeprom_path || !s->eeprom_path[0]) {
        return;
    }
    for (i = 0; i < CUP2006_EEPROM_WORDS; i++) {
        img[2 * i] = s->eeprom[i] >> 8;
        img[2 * i + 1] = s->eeprom[i] & 0xff;
    }
    tmp = g_strdup_printf("%s.tmp", s->eeprom_path);
    f = fopen(tmp, "wb");
    if (!f) {
        warn_report("cup2006: eeprom: cannot write '%s': %s", tmp,
                    strerror(errno));
        return;
    }
    ok = fwrite(img, sizeof(img), 1, f) == 1;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, s->eeprom_path) != 0) {
        warn_report("cup2006: eeprom: saving '%s' failed: %s", s->eeprom_path,
                    strerror(errno));
        unlink(tmp);
    }
}

static void cup2006_eeprom_load(Cup2006MachineState *s)
{
    uint8_t img[2 * CUP2006_EEPROM_WORDS];
    FILE *f;
    unsigned i;

    /* An erased Microwire 93C46 reads 0xFFFF on every word, so a fresh or
     * unreadable unit starts erased, never all-zero. */
    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    if (!s->eeprom_path || !s->eeprom_path[0]) {
        return;
    }
    f = fopen(s->eeprom_path, "rb");
    if (!f) {
        return;                     /* a fresh unit: erased (0xFFFF) */
    }
    if (fread(img, sizeof(img), 1, f) == 1) {
        for (i = 0; i < CUP2006_EEPROM_WORDS; i++) {
            s->eeprom[i] = img[2 * i] << 8 | img[2 * i + 1];
        }
    }
    /* A short file leaves the rest of the array erased. */
    fclose(f);
}

/* Chip select (GPIO pin 83): its falling edge ends the transaction. */
static void cup2006_eeprom_cs(Cup2006MachineState *s, bool level)
{
    if (s->eeprom_cs && !level) {
        s->scif_tx_len = 0;
        s->scif_rxq_head = s->scif_rxq_tail;   /* unread output is dropped */
        s->scif_rx_byte = -1;
    }
    s->eeprom_cs = level;
}

static void cup2006_scif_tx_byte(Cup2006MachineState *s, uint8_t byte)
{
    uint8_t op, addr;
    unsigned i;

    if (s->scif_tx_len < CUP2006_SCIF_TXQ) {
        s->scif_tx[s->scif_tx_len++] = byte;
    }
    if (s->scif_tx[0] != cup2006_scif_rev8(0x01)) {
        s->scif_tx_len = 0;             /* not a start bit: no command */
        return;
    }
    if (s->scif_tx_len < 2) {
        return;
    }
    op = cup2006_scif_rev8(s->scif_tx[1]) >> 6;
    addr = cup2006_scif_rev8(s->scif_tx[1]) & 0x3f;
    if ((op == 1 || (op == 0 && addr >> 4 == 1)) && s->scif_tx_len < 4) {
        return;                         /* WRITE / WRAL: the data word follows */
    }
    switch (op) {
    case 2:                             /* READ: words from addr on */
        for (i = addr; i < CUP2006_EEPROM_WORDS; i++) {
            cup2006_scif_rx_push(s, cup2006_scif_rev8(s->eeprom[i] >> 8));
            cup2006_scif_rx_push(s, cup2006_scif_rev8(s->eeprom[i] & 0xff));
        }
        if (s->scif_rx_byte < 0) {
            cup2006_scif_rx_arm(s);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "cup2006: eeprom read @0x%02x\n", addr);
        break;
    case 1:                             /* WRITE */
        if (s->eeprom_ewen) {
            s->eeprom[addr] = cup2006_scif_rev8(s->scif_tx[2]) << 8 |
                              cup2006_scif_rev8(s->scif_tx[3]);
            cup2006_eeprom_save(s);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "cup2006: eeprom write @0x%02x=0x%04x%s\n",
                      addr, s->eeprom[addr], s->eeprom_ewen ? "" : " (disabled)");
        break;
    case 3:                             /* ERASE */
        if (s->eeprom_ewen) {
            s->eeprom[addr] = 0xffff;
            cup2006_eeprom_save(s);
        }
        break;
    default:
        switch (addr >> 4) {
        case 0:                         /* EWDS */
            s->eeprom_ewen = false;
            break;
        case 1:                         /* WRAL */
            if (s->eeprom_ewen) {
                for (i = 0; i < CUP2006_EEPROM_WORDS; i++) {
                    s->eeprom[i] = cup2006_scif_rev8(s->scif_tx[2]) << 8 |
                                   cup2006_scif_rev8(s->scif_tx[3]);
                }
                cup2006_eeprom_save(s);
            }
            break;
        case 2:                         /* ERAL */
            if (s->eeprom_ewen) {
                memset(s->eeprom, 0xff, sizeof(s->eeprom));
                cup2006_eeprom_save(s);
            }
            break;
        default:                        /* EWEN */
            s->eeprom_ewen = true;
            break;
        }
        break;
    }
    s->scif_tx_len = 0;
}

static uint64_t cup2006_scif_read(Cup2006MachineState *s, hwaddr base,
                                  hwaddr addr, unsigned size)
{
    hwaddr phys = base + addr;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint8_t byte = 0;

        if (addr + i < CUP2006_SCIF_SIZE) {
            byte = s->scif_file[addr + i];
        }
        if (addr + i == CUP2006_SCIF_SCFSR ||
            addr + i == CUP2006_SCIF_SCFSR + 1) {
            uint16_t sr = CUP2006_SCIF_SCFSR_RDY;

            if (s->scif_rx_byte >= 0) {
                sr |= CUP2006_SCIF_SCFSR_RDF;
            }
            byte = (sr >> (8 * (addr + i - CUP2006_SCIF_SCFSR))) & 0xff;
        } else if (addr + i == CUP2006_SCIF_SCSPTR) {
            byte |= 1;      /* RxD = the EEPROM's DO: ready (STUB, instant) */
        } else if (addr + i == CUP2006_SCIF_SCFRDR) {
            if (s->scif_rx_byte >= 0) {
                byte = (uint8_t)s->scif_rx_byte;
                s->scif_rx_byte = -1;
                if (s->scif_rxq_head != s->scif_rxq_tail) {
                    cup2006_scif_rx_arm(s);
                }
            }
        }
        value |= (uint64_t)byte << (8 * i);
    }
    cup2006_scif_note(s, phys, false, size, value);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: scif read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
    return value;
}

static void cup2006_scif_write(Cup2006MachineState *s, hwaddr base,
                               hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr phys = base + addr;
    unsigned i;

    if (addr + size <= CUP2006_SCIF_SIZE) {
        for (i = 0; i < size; i++) {
            s->scif_file[addr + i] = (value >> (8 * i)) & 0xff;
        }
    }
    for (i = 0; i < size; i++) {
        hwaddr off = addr + i;

        if (off == CUP2006_SCIF_SCFTDR) {
            cup2006_scif_tx_byte(s, (value >> (8 * i)) & 0xff);
        }
        if (off == CUP2006_SCIF_SCFSR && !(value & CUP2006_SCIF_SCFSR_RDF)) {
            /* Writing 0 to RDF clears it; the unread byte is discarded. */
            s->scif_rx_byte = -1;
            if (s->scif_rxq_head != s->scif_rxq_tail) {
                cup2006_scif_rx_arm(s);
            }
        }
    }
    cup2006_scif_note(s, phys, true, size, value);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: scif write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_scif_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_scif_read(opaque, CUP2006_SCIF_BASE, addr, size);
}

static void cup2006_scif_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    cup2006_scif_write(opaque, CUP2006_SCIF_BASE, addr, value, size);
}

static uint64_t cup2006_scif_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_scif_read(opaque, CUP2006_SCIF_A7_BASE, addr, size);
}

static void cup2006_scif_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    cup2006_scif_write(opaque, CUP2006_SCIF_A7_BASE, addr, value, size);
}

static const MemoryRegionOps cup2006_scif_ops = {
    .read = cup2006_scif_p4_read,
    .write = cup2006_scif_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps cup2006_scif_a7_ops = {
    .read = cup2006_scif_a7_read,
    .write = cup2006_scif_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* SCIF0 (NaviSub link, 0xFF923000): SCFDR (+0x1C) = receive FIFO count;
 * SCFSR.RDF set while a byte is pending, SCFRDR read pops one; the RXI line
 * is raised while SCSCR.RIE is set and a byte is pending. */
static unsigned cup2006_scif0_rx_count(Cup2006MachineState *s)
{
    return s->scif0_rxq_tail - s->scif0_rxq_head;
}

static bool cup2006_scif0_rx_enabled(Cup2006MachineState *s)
{
    uint16_t scscr = s->scif0_file[CUP2006_SCIF0_SCSCR] |
                     (s->scif0_file[CUP2006_SCIF0_SCSCR + 1] << 8);

    return (scscr & CUP2006_SCIF0_SCSCR_RIE) != 0;
}

static uint16_t cup2006_scif0_scscr(Cup2006MachineState *s)
{
    return s->scif0_file[CUP2006_SCIF0_SCSCR] |
           (s->scif0_file[CUP2006_SCIF0_SCSCR + 1] << 8);
}

/* The transmit side of the shared SCIF0 interrupt line: SCSCR.TIE (bit 7)
 * enables the TX request, raised while SCFSR.TDFE (bit 5) is 1 (sh7751
 * manual p.733-739). STUB: instant TX, no baud pacing. */
static bool cup2006_scif0_txi_pending(Cup2006MachineState *s)
{
    uint16_t scscr = cup2006_scif0_scscr(s);
    uint16_t sr = s->scif0_file[CUP2006_SCIF0_SCFSR] |
                  (s->scif0_file[CUP2006_SCIF0_SCFSR + 1] << 8);

    return (scscr & CUP2006_SCIF0_SCSCR_TIE) &&
           (sr & CUP2006_SCIF0_SCFSR_TDFE);
}

/* Drive the SH INTC source from the SCIF0 interrupt conditions: the line is
 * shared by RXI (RIE + a pending byte) and TXI (TIE + TDFE). */
static void cup2006_scif0_update_irq(Cup2006MachineState *s)
{
    struct intc_source *src = s->scif0_intc_src;
    bool rx = cup2006_scif0_rx_enabled(s) && cup2006_scif0_rx_count(s) > 0;
    bool tx = cup2006_scif0_txi_pending(s);
    bool want = rx || tx;

    if (!src || want == s->scif0_irq_raised) {
        return;
    }
    s->scif0_irq_raised = want;
    if (want) {
        /*
         * STUB: the SH7770 INTC2 priority/enable for this line is
         * unmodelled, so the machine owns them; re-assert them at the
         * raise edge.
 */
        src->prio = CUP2006_SCIF0_INTC_PRIO;
        src->enable_count = src->enable_max;
    }
    sh_intc_toggle_source(src, 0, want ? 1 : -1);
    if (want) {
        /*
         * STUB: re-assert the CPU interrupt line at the raise edge;
         * sh_intc_toggle_source only kicks the vCPU on a 0 -> 1 pending
         * transition.
 */
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
    }
    cup2006_info(s, "cup2006: SCIF0 INTEVT 0x%03x %s (SYSINTR %d, %u byte%s"
                " pending)",
                CUP2006_SCIF0_INTEVT, want ? "raised" : "released",
                CUP2006_SCIF0_SYSINTR, cup2006_scif0_rx_count(s),
                cup2006_scif0_rx_count(s) == 1 ? "" : "s");
}

/* End of one transmitted byte: one byte time later the transmit FIFO is
 * empty again (TDFE/TEND set, sh7751 manual p.738-739). STUB: the byte
 * time is a fixed 1 ms; the model only dates the status flags. */
static void cup2006_scif0_tx_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;

    s->scif0_file[CUP2006_SCIF0_SCFSR] |=
        CUP2006_SCIF0_SCFSR_TDFE | CUP2006_SCIF0_SCFSR_TEND;
    cup2006_scif0_update_irq(s);
}

static uint64_t cup2006_scif0_read(Cup2006MachineState *s, hwaddr base,
                                   hwaddr addr, unsigned size)
{
    hwaddr phys = base + addr;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint8_t byte = 0;

        if (addr + i < CUP2006_SCIF_SIZE) {
            byte = s->scif0_file[addr + i];
        }
        if (addr + i == CUP2006_SCIF0_SCFSR ||
            addr + i == CUP2006_SCIF0_SCFSR + 1) {
            /* Report the guest's own SCFSR bits (cleared by its writes)
             * plus RDF while a byte waits. */
            uint16_t sr = s->scif0_file[CUP2006_SCIF0_SCFSR] |
                          (s->scif0_file[CUP2006_SCIF0_SCFSR + 1] << 8);

            if (cup2006_scif0_rx_count(s) > 0) {
                sr |= CUP2006_SCIF_SCFSR_RDF;
            }
            byte = (sr >> (8 * (addr + i - CUP2006_SCIF0_SCFSR))) & 0xff;
        } else if (addr + i == CUP2006_SCIF0_SCFDR) {
            byte = cup2006_scif0_rx_count(s) & 0x1f;
        } else if (addr + i == CUP2006_SCIF0_SCFDR + 1) {
            byte = 0;
        } else if (addr + i == CUP2006_SCIF0_SCFRDR) {
            if (cup2006_scif0_rx_count(s) > 0) {
                byte = s->scif0_rxq[s->scif0_rxq_head++ % CUP2006_SCIF_RXQ];
                if (s->keys) {
                    cup2006_info(s, "cup2006: keys: guest read 0x%02x at %" PRId64
                                " ms pc=0x%08" PRIx32 " (%u left)",
                                byte, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                                cup2006_guest_pc(),
                                cup2006_scif0_rx_count(s));
                }
                cup2006_scif0_update_irq(s);
            }
        }
        value |= (uint64_t)byte << (8 * i);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: scif0 read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
    return value;
}


/* Board key pad -> Sub-CPU key report on SCIF0: cmd 0x0181, seq,
 * data[0]=0xE2 (type 1), a 16-bit key id, a value byte; QEMU's input layer
 * latches one key code and the peer runs one transfer (key_seq handshake). */
typedef struct Cup2006KeyEntry {
    QKeyCode qcode;
    const char *name;
    uint16_t code;               /* E2-form 16-bit guest key id */
    int raw;                     /* raw 0181 key byte, -1 = none known */
} Cup2006KeyEntry;

/* Keyboard keys as Sub-CPU 0x0181 reports on SCIF0: 16-bit key id (E2
 * form) plus, where known, a raw 0181 lane byte. */
static const Cup2006KeyEntry cup2006_keys[] = {
    { Q_KEY_CODE_RET,   "ret",   0x0008, 0x59 },
    { Q_KEY_CODE_ESC,   "esc",   0x0001, 0x5A },
    { Q_KEY_CODE_TAB,   "tab",   0x0010, -1 },
    { Q_KEY_CODE_UP,    "up",    0x0002, 0x51 },
    { Q_KEY_CODE_DOWN,  "down",  0x0004, 0x55 },
    { Q_KEY_CODE_LEFT,  "left",  0x0020, 0x57 },
    { Q_KEY_CODE_RIGHT, "right", 0x0040, 0x53 },
};

/* UART pacing: one byte per millisecond, only after the guest drained the
 * previous one (a one-byte peer FIFO, the conservative reading of the SH
 * SCIF FIFO the OAL ISR drains). */
#define CUP2006_PEER_BYTE_NS 1000000

static const Cup2006KeyEntry *cup2006_key_lookup(QKeyCode q)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(cup2006_keys); i++) {
        if (cup2006_keys[i].qcode == q) {
            return &cup2006_keys[i];
        }
    }
    return NULL;
}

/* Peer UART transmitter: move at most one paced byte into the RX FIFO. */
static void cup2006_scif0_peer_pump(void *opaque)
{
    Cup2006MachineState *s = opaque;

    if (s->peer_q_head == s->peer_q_tail) {
        return;
    }
    if (cup2006_scif0_rx_count(s) != 0) {
        timer_mod(s->peer_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  CUP2006_PEER_BYTE_NS);
        return;
    }
    s->scif0_rxq[s->scif0_rxq_tail++ % CUP2006_SCIF_RXQ] =
        s->peer_q[s->peer_q_head++ % CUP2006_SCIF_RXQ];
    cup2006_scif0_update_irq(s);
    if (s->peer_q_head != s->peer_q_tail) {
        timer_mod(s->peer_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  CUP2006_PEER_BYTE_NS);
    }
}

static void cup2006_scif0_peer_send(Cup2006MachineState *s, const uint8_t *buf,
                                    unsigned n)
{
    unsigned j;
    bool idle = s->peer_q_head == s->peer_q_tail;

    for (j = 0; j < n; j++) {
        if (s->peer_q_tail - s->peer_q_head >= CUP2006_SCIF_RXQ) {
            cup2006_info(s, "cup2006: keys: peer TX queue full, byte 0x%02x lost",
                        buf[j]);
            break;
        }
        s->peer_q[s->peer_q_tail++ % CUP2006_SCIF_RXQ] = buf[j];
    }
    if (idle && s->peer_q_head != s->peer_q_tail) {
        timer_mod(s->peer_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  CUP2006_PEER_BYTE_NS);
    }
}

/* One key-report frame: STX 02 | LEN 09 | 01 81 seq E2 k_lo k_hi val |
 * ETX 03 | CS; a key with a known raw byte uses the minimal raw shape. */
static unsigned cup2006_key_frame_enc(Cup2006MachineState *s, uint8_t *out,
                                      uint16_t key, uint8_t type, uint8_t val,
                                      bool raw)
{
    uint8_t payload[7];
    unsigned n, i;
    uint8_t sum;

    payload[0] = (s->key_src >> 8) & 0xff;
    payload[1] = s->key_src & 0xff;
    payload[2] = (uint8_t)(s->key_frames + 1);       /* seq */
    if (!raw) {
        payload[3] = type;                           /* key state E2..E5 */
        payload[4] = key & 0xff;                     /* key, little endian */
        payload[5] = (key >> 8) & 0xff;
        payload[6] = val;
        n = 7;
    } else {
        /* cmd 0181 with data[0] outside E1..E5: the minimal raw shape
         * 02 06 01 81 <seq> <keybyte> 03 <cs>. */
        payload[3] = (uint8_t)key;                   /* data[0] / legacy byte */
        n = 4;
    }
    out[0] = 0x02;
    out[1] = n + 2;                                  /* LEN = payload + ETX */
    memcpy(&out[2], payload, n);
    out[2 + n] = 0x03;
    sum = out[1] + 0x03;
    for (i = 0; i < n; i++) {
        sum += payload[i];
    }
    out[3 + n] = sum;
    return n + 4;
}

static void cup2006_key_put_frame_enc(Cup2006MachineState *s, uint16_t key,
                                      uint8_t type, uint8_t val, bool raw)
{
    uint8_t frame[12];
    unsigned len = cup2006_key_frame_enc(s, frame, key, type, val, raw);

    cup2006_scif0_peer_send(s, frame, len);
    s->key_frames++;
    s->key_frame_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    cup2006_info(s, "cup2006: keys: key report cmd %04x key 0x%04x type 0x%02x"
                " val 0x%02x raw %d queued at %" PRId64 " ms (%u bytes)",
                s->key_src, (unsigned)key, type, val, raw, s->key_frame_ms,
                len);
}

/* Wheel pulses: Navi's distance pulses arrive as Sub-CPU frames, command
 * 0x0285 (d[0]=0x1F format 1: s16 LE delta per wheel, d[1]=0, four s16).
 * STUB: the simulated transducer produces 449.02 pulses/km; one frame per
 * 100 ms from the track speed. STUB: the real producer is not modelled. */
#define CUP2006_WHEEL_FLAG       0x10000
#define CUP2006_STATUS_FLAG      0x20000    /* | 16-bit code: a Sub-CPU status frame */
#define CUP2006_WHEEL_PERIOD_MS  100
#define CUP2006_PULSES_PER_M     0.44902

static void cup2006_wheel_put_frame(Cup2006MachineState *s, int16_t pulses)
{
    uint8_t f[17], sum;
    unsigned i;

    f[0] = 0x02;
    f[1] = 0x0f;                         /* 13 payload + ETX + CS */
    f[2] = 0x02;
    f[3] = 0x85;
    f[4] = (uint8_t)(s->key_frames + 1); /* seq, shared with key reports */
    f[5] = 0x1f;
    f[6] = 0x00;
    for (i = 0; i < 4; i++) {
        f[7 + i * 2] = (uint16_t)pulses & 0xff;
        f[8 + i * 2] = ((uint16_t)pulses >> 8) & 0xff;
    }
    f[15] = 0x03;
    sum = f[1] + 0x03;
    for (i = 2; i < 15; i++) {
        sum += f[i];
    }
    f[16] = sum;
    cup2006_scif0_peer_send(s, f, sizeof(f));
    s->key_frames++;
    s->wheel_frames++;
    s->key_last_wheel = true;
    if (s->wheel_frames <= 3 || (s->wheel_frames % 600) == 0) {
        cup2006_dbg(s,  "cup2006: wheel frame #%" PRIu64 " 0285 pulses %d\n",
                s->wheel_frames, pulses);
    }
}

static void cup2006_key_enqueue(Cup2006MachineState *s, int code, bool raw);

/* One Sub-CPU status frame `02 LEN <code:2> seq data.. 03 CS`, sent by the
 * peer on its own (like the key and wheel reports): the car's status after
 * MOST start-up and the ignition-off pair (the guest does not always poll,
 * so these cannot wait for one). */
static void cup2006_status_put_frame(Cup2006MachineState *s, unsigned code)
{
    static const uint8_t freeze_frame[12];   /* STUB, see car status below */
    static const uint8_t net_off[1] = { 0x02 }, shutdown[1] = { 0x01 };
    uint8_t f[32], sum, market = s->car_market;
    const uint8_t *data = code == 0xfd89 ? &market :
                          code == 0xf18a ? net_off :
                          code == 0xfc81 ? shutdown :
                          code == 0xf381 ? s->car_voice_reply : freeze_frame;
    unsigned n = code == 0xfd86 ? sizeof(freeze_frame) :
                 code == 0xf381 ? sizeof(s->car_voice_reply) : 1, i;

    f[0] = 0x02;
    f[1] = 3 + n + 2;                    /* payload + ETX + CS */
    f[2] = code >> 8;
    f[3] = code & 0xff;
    f[4] = (uint8_t)(s->key_frames + 1); /* seq, shared with key reports */
    memcpy(&f[5], data, n);
    f[5 + n] = 0x03;
    sum = f[1] + 0x03;
    for (i = 2; i < 5 + n; i++) {
        sum += f[i];
    }
    f[6 + n] = sum;
    cup2006_scif0_peer_send(s, f, 7 + n);
    s->key_frames++;
    s->key_last_wheel = false;
    cup2006_info(s, "cup2006: subpeer: status frame %02x %02x (%u data byte(s))"
                " sent at %" PRId64 " ms", f[2], f[3], n,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

static void cup2006_key_put_frame(Cup2006MachineState *s)
{
    if (s->key_waiting >= CUP2006_STATUS_FLAG) {
        cup2006_status_put_frame(s, s->key_waiting & 0xffff);
        s->key_frame_inflight = true;
        return;
    }
    if (s->key_waiting >= CUP2006_WHEEL_FLAG) {
        cup2006_wheel_put_frame(s, (int16_t)(s->key_waiting & 0xffff));
        s->key_frame_inflight = true;
        return;
    }
    s->key_last_wheel = false;
    cup2006_key_put_frame_enc(s, s->key_waiting, s->key_type, s->key_val,
                              s->key_waiting_raw);
    s->key_frame_inflight = true;
}

/* Every guest TX byte on SCIF0 is a chance to answer; a pending report
 * rides the guest's idle ENQ (0x05). key_seq: 0 = ACK 0x06 then frame,
 * 1 = peer ENQ 0x05 then frame on the guest's 0x06, 2 = frame alone. */
static void cup2006_key_guest_tx(Cup2006MachineState *s, uint8_t b)
{
    /* The guest's ACK 0x06 retires a peer ENQ or an in-flight frame whether
     * or not keys are enabled: with subpeer=on and keys=off the Sub-CPU's
     * own status frames still need the ACK to flow, or the link stalls after
     * the first frame.  Key reports themselves only exist when keys=on (no
     * input handler is registered otherwise, so key_waiting stays -1). */
    if (s->peer_enq_armed && b == 0x06) {
        s->peer_enq_armed = false;
        cup2006_key_put_frame(s);
        return;
    }
    if (s->key_frame_inflight && b == 0x06) {
        int done = s->key_waiting;

        s->key_frame_inflight = false;
        s->key_waiting = -1;
        if (done >= CUP2006_STATUS_FLAG) {
            /* The next frame of the pair: the freeze-frame record after the
             * vehicle status, DeviceMode Shutdown after NET OFF. */
            if ((done & 0xffff) == 0xfd89) {
                cup2006_key_enqueue(s, CUP2006_STATUS_FLAG | 0xfd86, false);
            } else if ((done & 0xffff) == 0xf18a) {
                s->subpeer_acc_off_stage = 2;
                cup2006_key_enqueue(s, CUP2006_STATUS_FLAG | 0xfc81, false);
            } else if (s->key_q_n && s->key_q_timer) {
                timer_mod(s->key_q_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
            }
            return;
        }
        s->key_guest_acks++;
        if (s->key_q_n && s->key_q_timer) {
            timer_mod(s->key_q_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      (s->key_last_wheel ? 5 : 40));
        }
        if (s->key_last_wheel) {
            return;
        }
        cup2006_info(s, "cup2006: keys: guest ACKed key report #%u (%u ack%s"
                    " since the report)", s->key_frames, s->key_guest_acks,
                    s->key_guest_acks == 1 ? "" : "s");
        return;
    }
    if (!s->keys) {
        return;
    }
    if (b == 0x15 && (s->peer_enq_armed || s->key_frame_inflight)) {
        /* The receiver rejected our offer/frame. Keep the report, including
         * a key release, and retry the wire exchange after the collision.
         * No input is generated by this timer: it retries the pending event.
         * the guest's collision response is NAK 0x15. */
        s->peer_enq_armed = false;
        s->key_frame_inflight = false;
        timer_mod(s->key_q_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 40);
        return;
    }
    if (s->key_waiting >= 0 && b == 0x05 && !s->key_frame_inflight &&
        s->key_seq == 1) {
        /* The guest wants the link: a guest ENQ must be ACKed, never answered
         * with a peer ENQ. Withdraw any offer of ours; the key is offered
         * again when the guest's turn is over (cup2006_key_pump). */
        s->peer_enq_armed = false;
        if (s->key_q_timer) {
            timer_mod(s->key_q_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
        }
        return;
    }
    if (s->key_waiting >= 0 && b == 0x05 && !s->peer_enq_armed &&
        !s->key_frame_inflight) {
        if (s->key_seq == 0) {
            static const uint8_t ack = 0x06;

            cup2006_scif0_peer_send(s, &ack, 1);
            cup2006_key_put_frame(s);
        } else {
            cup2006_key_put_frame(s);
        }
        return;
    }
}

/* The Sub-CPU peer's answer to what the guest puts on the wire: the guest
 * is link master, so its ENQ (0x05) is answered with ACK 0x06 and its
 * frame with ACK 0x06 after checksum validation; only replies known
 * from the Sub-CPU firmware's command table are pushed toward the guest. */
static void cup2006_subpeer_reset(Cup2006MachineState *s)
{
    s->subpeer_frame_len = 0;
    s->subpeer_frame_total = 0;
    s->subpeer_grant_ms = 0;        /* the guest's turn on the link is over */
}

static void cup2006_subpeer_report(Cup2006MachineState *s, const char *what)
{
    char hex[3 * CUP2006_SCIF_RXQ + 1];
    unsigned i, off = 0;

    if (!s->verbose) {
        return;
    }

    hex[0] = '\0';
    for (i = 0; i < s->subpeer_frame_len && off + 4 < sizeof(hex); i++) {
        off += snprintf(hex + off, sizeof(hex) - off, "%02x ",
                        s->subpeer_frame[i]);
    }
    cup2006_info(s, "cup2006: subpeer: %s frame %s(%u bytes) at %" PRId64 " ms"
                " pc=0x%08" PRIx32, what, hex, s->subpeer_frame_len,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), cup2006_guest_pc());
}

static void cup2006_subpeer_ack(Cup2006MachineState *s)
{
    static const uint8_t ack = 0x06;

    cup2006_scif0_peer_send(s, &ack, 1);
}

/* ACK a guest ENQ: the guest now holds the link until its
 * frame is in, or CUP2006_SUBPEER_GRANT_MS passes if it was an idle poll. */
static void cup2006_subpeer_grant(Cup2006MachineState *s)
{
    cup2006_subpeer_ack(s);
    s->subpeer_grant_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

/* The known reply frame(s): reply code = request code | 0x80 from the
 * firmware row matching the request. STUB: seq byte 0x00 and no data bytes
 * unless a known row gives them (the decoder's minimum payload is
 * `<code:2> <seq>`). */
static void cup2006_subpeer_reply_frame(Cup2006MachineState *s, uint8_t *out,
                                        unsigned *n)
{
    uint8_t payload[3 + sizeof(s->subpeer_reply_data)];
    unsigned plen = 3 + s->subpeer_reply_data_len;
    uint8_t sum;
    unsigned i;

    payload[0] = s->subpeer_reply_code;    /* known row code */
    payload[1] = s->subpeer_reply_code1;
    payload[2] = s->subpeer_reply_seq;     /* STUB unless a known row gives it */
    for (i = 0; i < s->subpeer_reply_data_len; i++) {
        payload[3 + i] = s->subpeer_reply_data[i];
    }
    out[0] = 0x02;
    out[1] = plen + 2;                 /* LEN = payload + ETX*/
    memcpy(&out[2], payload, plen);
    out[2 + plen] = 0x03;
    sum = out[1] + 0x03;
    for (i = 0; i < plen; i++) {
        sum += payload[i];
    }
    out[3 + plen] = sum;
    *n = plen + 4;
}

/* Offer the guest a peer ENQ 0x05: its state-0 handler answers 0x06 and
 * enters the frame-receive state; the reply frame follows that 0x06.
 * Nothing is sent unless a known reply is pending. */
static void cup2006_subpeer_reply_arm(Cup2006MachineState *s)
{
    static const uint8_t enq = 0x05;

    if (s->subpeer_reply_armed || !s->subpeer_reply_pending) {
        return;
    }
    cup2006_scif0_peer_send(s, &enq, 1);
    s->subpeer_reply_armed = true;
    cup2006_info(s, "cup2006: subpeer: peer ENQ 0x05 for the known %02x %02x"
                " reply at %" PRId64 " ms, frame follows the guest's 0x06",
                s->subpeer_reply_code, s->subpeer_reply_code1,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

/* Ignition-off, second frame: after NET OFF the peer reports DeviceMode
 * Shutdown (`fc 81 01`), routed by the guest's dispatcher to the APM
 * device-mode handler. */
static void cup2006_power_cut_arm(Cup2006MachineState *s);

/* The car's status after MOST start-up: the peer reports fd 89 (market)
 * then fd 86 (freeze frame) on its own once the link is up. STUB: the
 * freeze-frame record's 12 bytes are zero. */
static void cup2006_subpeer_car_next(Cup2006MachineState *s, unsigned sent)
{
    if (s->car_market < 0 || s->subpeer_acc_off_stage || !s->key_q_timer ||
        sent != 0xf982 || s->subpeer_car_started) {
        return;
    }
    /* The pump offers it with its own peer ENQ once the link is idle (the
     * guest does not poll while its HMI waits for this very data). */
    s->subpeer_car_started = true;
    cup2006_key_enqueue(s, CUP2006_STATUS_FLAG | 0xfd89, false);
}

static void cup2006_subpeer_reply_send(Cup2006MachineState *s)
{
    uint8_t frame[16];
    unsigned n;
    unsigned sent = s->subpeer_reply_code << 8 | s->subpeer_reply_code1;

    cup2006_subpeer_reply_frame(s, frame, &n);
    cup2006_scif0_peer_send(s, frame, n);
    s->subpeer_replies++;
    s->subpeer_reply_armed = false;
    s->subpeer_reply_pending = false;
    cup2006_subpeer_car_next(s, sent);
    cup2006_info(s, "cup2006: subpeer: known reply frame %02x %02x %02x %02x %02x"
                " %02x %02x (%u bytes) sent at %" PRId64 " ms",
                frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                frame[6], n, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

static void cup2006_subpeer_guest_tx(Cup2006MachineState *s, uint8_t b)
{
    bool car_answers;
    unsigned total;
    uint8_t sum;
    unsigned i;

    if (!s->subpeer) {
        return;
    }
    /* A key report owns the exchange while its ENQ is unanswered or its frame
     * is in flight (no competing 0x06 then); a latched key yields to a guest
     * ENQ (cup2006_key_guest_tx withdraws it first). */
    if (s->keys && (s->peer_enq_armed || s->key_frame_inflight)) {
        return;
    }
    if (s->subpeer_frame_len == 0 && b == 0x02) {
        s->subpeer_frame[0] = b;
        s->subpeer_frame_len = 1;
        s->subpeer_frame_total = 0;
        return;
    }
    if (s->subpeer_frame_len > 0) {
        if (s->subpeer_frame_len == 1) {
            s->subpeer_frame[1] = b;
            s->subpeer_frame_len = 2;
            if (b < 2 || (unsigned)b + 2 > CUP2006_SCIF_RXQ) {
                cup2006_info(s, "cup2006: subpeer: guest frame LEN 0x%02x is out"
                            " of range, refused with 0x15", b);
                cup2006_subpeer_reset(s);
                {
                    static const uint8_t nak = 0x15;
                    cup2006_scif0_peer_send(s, &nak, 1);
                }
                return;
            }
            s->subpeer_frame_total = (unsigned)b + 2;   /* STX LEN ... CS */
            return;
        }
        s->subpeer_frame[s->subpeer_frame_len++] = b;
        if (s->subpeer_frame_len < s->subpeer_frame_total) {
            return;
        }
        /* Checksum: a running sum seeded with LEN that accumulates LEN-1
         * read bytes (ETX included), compared to the next byte. */
        total = s->subpeer_frame_total;
        sum = (uint8_t)s->subpeer_frame[1];
        for (i = 2; i < total - 1; i++) {              /* LEN-1 bytes incl. ETX */
            sum += s->subpeer_frame[i];
        }
        if (s->subpeer_frame[total - 2] != 0x03 ||
            s->subpeer_frame[total - 1] != sum) {
            cup2006_info(s, "cup2006: subpeer: guest frame failed the parser"
                        " checks (ETX 0x%02x, CS 0x%02x, expected 0x%02x),"
                        " refused with 0x15",
                        s->subpeer_frame[total - 2],
                        s->subpeer_frame[total - 1], sum);
            cup2006_subpeer_report(s, "bad");
            cup2006_subpeer_reset(s);
            {
                static const uint8_t nak = 0x15;
                cup2006_scif0_peer_send(s, &nak, 1);
            }
            return;
        }
        cup2006_subpeer_report(s, "guest cmd");
        /* The navigation voice channel: the guest asks with `f3 01 <kind>
         * <source>` and takes cmd f3 81 with p1=1 as the grant. With the car
         * on the bus (carmarket=) it grants every request: f3 81 <kind> 01.
         * STUB: the car's arbitration is not modelled. */
        car_answers = s->car_market >= 0 && !s->subpeer_acc_off_stage &&
                      total >= 9 && s->subpeer_frame[2] == 0xf3 &&
                      s->subpeer_frame[3] == 0x01;
        if (car_answers) {
            s->car_voice_reply[0] = s->subpeer_frame[5];
            s->car_voice_reply[1] = 0x01;
            cup2006_info(s, "cup2006: car: voice channel request %02x %02x ->"
                        " f3 81 %02x 01", s->subpeer_frame[5],
                        s->subpeer_frame[6], s->subpeer_frame[5]);
            cup2006_key_enqueue(s, CUP2006_STATUS_FLAG | 0xf381, false);
        }
        if (s->subpeer_acc_off_stage == 2 && total >= 8 &&
            s->subpeer_frame[2] == 0xfc && s->subpeer_frame[3] == 0x01 &&
            s->subpeer_frame[5] == 0x01) {
            cup2006_power_cut_arm(s);
        }
        s->subpeer_frames++;
        /* Every reply frame carries a code the guest's own table decodes,
         * using the Sub-CPU firmware's reply row (f5 01 05 -> fd 83, f1 05 ->
         * f1 85, reply = request | 0x80); everything else is ACK-only. */
        if (s->subpeer_frame_total >= 5 &&
            s->subpeer_frame[2] == 0xf5 && s->subpeer_frame[3] == 0x01 &&
            s->subpeer_frame[4] == 0x05) {
            s->subpeer_reply_code = 0xfd;
            s->subpeer_reply_code1 = 0x83;
            s->subpeer_reply_seq = 0x00;    /* STUB: the reply seq byte is 0x00 */
            s->subpeer_reply_data_len = 0;
            s->subpeer_reply_pending = true;
            cup2006_info(s, "cup2006: subpeer: f5 01 05 takes the known reply"
                        " fd 83; offering the"
                        " peer ENQ after this ACK");
        } else if (s->subpeer_frame_total >= 4 &&
                   s->subpeer_frame[2] == 0xf1 && s->subpeer_frame[3] == 0x05) {
            /* f1 05's reply code is f1 85, but no reply data is known and a
             * data-less reply makes the guest repeat the request: ACK only. */
            cup2006_info(s, "cup2006: subpeer: f1 05 is ACKed without a reply");
        } else if (s->subpeer_frame_total >= 4 &&
                   s->subpeer_frame[2] == 0xf2 && s->subpeer_frame[3] == 0x09) {
            s->subpeer_reply_code = 0xf2;
            s->subpeer_reply_code1 = 0x89;
            s->subpeer_reply_seq = 0x00;    /* STUB: the reply seq byte is 0x00 */
            s->subpeer_reply_data_len = 0;
            s->subpeer_reply_pending = true;
            cup2006_info(s, "cup2006: subpeer: f2 09 takes the known reply"
                        " f2 89; offering the peer ENQ after this ACK");
        /* `f2 02` (Sub-CPU firmware check) -> `f2 82` by the same
         * request|0x80 rule as f2 09 -> f2 89; seq 0x00, no data. */
        } else if (s->subpeer_frame_total >= 4 &&
                   s->subpeer_frame[2] == 0xf2 && s->subpeer_frame[3] == 0x02) {
            s->subpeer_reply_code = 0xf2;
            s->subpeer_reply_code1 = 0x82;
            s->subpeer_reply_seq = 0x00;    /* STUB: the reply seq byte is 0x00 */
            s->subpeer_reply_data_len = 0;
            s->subpeer_reply_pending = true;
            cup2006_info(s, "cup2006: subpeer: f2 02 takes the known reply"
                        " f2 82;"
                        " offering the peer ENQ after this ACK");
        /* `fd 04 06` -> `fd 84`, the same row-pair rule as `f5 01 05` ->
         * `fd 83`; seq 0x00 (STUB) and no data bytes (the decoder's minimum
         * payload is `<code:2> <seq>`). */
        } else if (s->subpeer_frame_total >= 6 &&
                   s->subpeer_frame[2] == 0xfd && s->subpeer_frame[3] == 0x04 &&
                   s->subpeer_frame[4] == 0x06) {
            s->subpeer_reply_code = 0xfd;
            s->subpeer_reply_code1 = 0x84;
            s->subpeer_reply_seq = 0x00;    /* STUB: the reply seq byte is 0x00 */
            s->subpeer_reply_data_len = 0;
            s->subpeer_reply_pending = true;
            cup2006_info(s, "cup2006: subpeer: fd 04 06 takes the known reply"
                        " fd 84; offering the"
                        " peer ENQ after this ACK");
        } else if (s->subpeer_frame_total >= 5 && !car_answers) {
            cup2006_info(s, "cup2006: subpeer: cmd %02x%02x has no known reply,"
                        " ACK only", s->subpeer_frame[2], s->subpeer_frame[3]);
        }
        cup2006_subpeer_reset(s);
        cup2006_subpeer_ack(s);
        cup2006_info(s, "cup2006: subpeer: ACK 0x06 sent for guest frame #%u",
                    s->subpeer_frames);
        cup2006_subpeer_reply_arm(s);
        return;
    }
    if (b == 0x06 && s->subpeer_reply_armed) {
        cup2006_subpeer_reply_send(s);
        return;
    }
    if (b == 0x05) {
        /* A running Sub-CPU reports its status when the link comes up: queue
         * the known f9 82 report on the guest's first ENQ; it rides the
         * existing ACK-then-peer-ENQ handshake. */
        if (!s->subpeer_report_queued) {
            s->subpeer_report_queued = true;
            s->subpeer_reply_code = 0xf9;
            s->subpeer_reply_code1 = 0x82;
            s->subpeer_reply_seq = 0x00;   /* STUB: no row gives the seq */
            s->subpeer_reply_data_len = 0;
            cup2006_info(s, "cup2006: subpeer: startup status report f9 82"
                        " queued on the guest's first ENQ at %" PRId64 " ms",
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            s->subpeer_reply_pending = true;
        }
        if (s->subpeer_reply_pending && s->subpeer_enq_acked) {
            /* Second ENQ in a row: the guest is idle in state 0. */
            s->subpeer_enq_acked = false;
            cup2006_subpeer_reply_arm(s);
            return;
        }
        if (s->subpeer_reply_pending) {
            /* The guest just sent this ENQ and would refuse a peer ENQ
             * with 0x15; ACK it instead so its cycle completes. */
            s->subpeer_enq_acked = true;
            cup2006_subpeer_grant(s);
            cup2006_info(s, "cup2006: subpeer: guest ENQ ACKed with a reply"
                        " %02x %02x pending at %" PRId64 " ms",
                        s->subpeer_reply_code, s->subpeer_reply_code1,
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            return;
        }
        s->subpeer_polls++;
        cup2006_subpeer_grant(s);
        cup2006_info(s, "cup2006: subpeer: guest ENQ 0x%02x #%u answered with"
                    " ACK 0x06 at %" PRId64 " ms pc=0x%08" PRIx32,
                    b, s->subpeer_polls,
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                    cup2006_guest_pc());
        return;
    }
    if (b == 0x06) {
        s->subpeer_acks++;
        cup2006_info(s, "cup2006: subpeer: guest ACK 0x06 #%u at %" PRId64 " ms",
                    s->subpeer_acks, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        return;
    }
    if (b == 0x15) {
        s->subpeer_naks++;
        if (s->subpeer_reply_armed) {
            s->subpeer_reply_armed = false;   /* keep pending, retry on ENQ */
            s->subpeer_enq_acked = false;     /* re-learn the guest's state */
        }
        cup2006_info(s, "cup2006: subpeer: guest NAK 0x15 #%u (its collision"
                    " guard / parser refusal) at %" PRId64 " ms",
                    s->subpeer_naks, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        return;
    }
    cup2006_info(s, "cup2006: subpeer: unexpected guest byte 0x%02x at %" PRId64
                " ms pc=0x%08" PRIx32, b,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), cup2006_guest_pc());
}

/* Start the peer's transfer for a latched key (driven by the input event). */
static void cup2006_key_start(Cup2006MachineState *s)
{
    if (s->key_seq == 1) {
        static const uint8_t enq = 0x05;

        if (s->peer_enq_armed) {
            return;
        }
        cup2006_scif0_peer_send(s, &enq, 1);
        s->peer_enq_armed = true;
            s->peer_enq_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        cup2006_info(s, "cup2006: keys: peer ENQ 0x05 sent at %" PRId64 " ms, the"
                    " frame follows the guest's 0x06",
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    } else if (s->key_seq == 0) {
        static const uint8_t ack = 0x06;

        cup2006_scif0_peer_send(s, &ack, 1);
        cup2006_key_put_frame(s);
    } else {
        cup2006_key_put_frame(s);
    }
}

/* How long an ACKed guest ENQ keeps the link the guest's; an idle poll
 * sends nothing and the grant lapses. */
#define CUP2006_SUBPEER_GRANT_MS 50

/* Latch the next queued report if the exchange is idle. */
static void cup2006_key_pump(void *opaque)
{
    Cup2006MachineState *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (s->peer_enq_armed && now - s->peer_enq_ms > 1000 &&
        s->peer_enq_stall_logged != s->peer_enq_ms) {
        s->peer_enq_stall_logged = s->peer_enq_ms;
        cup2006_info(s, "cup2006: keys: peer ENQ sent at %" PRId64 " ms is still"
                    " unanswered at %" PRId64 " ms (link stalled)",
                    s->peer_enq_ms, now);
    }
    if (s->peer_enq_armed || s->key_frame_inflight ||
        (!s->key_q_n && s->key_waiting < 0)) {
        return;
    }
    /* The guest holds the link from our ACK of its ENQ until its frame is
     * in; a peer ENQ in that window kills the exchange, so wait out the
     * grant. */
    if (s->subpeer_frame_len || s->subpeer_reply_armed ||
        (s->subpeer_grant_ms &&
         now - s->subpeer_grant_ms < CUP2006_SUBPEER_GRANT_MS)) {
        timer_mod(s->key_q_timer, now + 5);
        return;
    }
    /* Queuing an ENQ while the previous frame is still in the UART lets
     * its ACK accept the new offer before the receiver has seen it. */
    if (s->peer_q_head != s->peer_q_tail || cup2006_scif0_rx_count(s)) {
        timer_mod(s->key_q_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
        return;
    }
    if (s->key_waiting < 0) {
        s->key_waiting = s->key_q[s->key_q_head];
        s->key_waiting_raw = s->key_q_raw[s->key_q_head];
        s->key_q_head = (s->key_q_head + 1) % 16;
        s->key_q_n--;
    }
    if (s->key_waiting < CUP2006_WHEEL_FLAG) {
        cup2006_info(s, "cup2006: keys: guest key 0x%02x (raw %d) latched for the"
                    " next SCIF0 poll (%u queued)", s->key_waiting,
                    s->key_waiting_raw, s->key_q_n);
    }
    cup2006_key_start(s);
}

static void cup2006_key_enqueue(Cup2006MachineState *s, int code, bool raw)
{
    unsigned t;

    /* Reserve the last queue slot for the raw no-key report. Losing that
     * report leaves the guest's real held-key/repeat state active. */
    if (s->key_q_n >= 15 && !(raw && code == 0)) {
        cup2006_info(s, "cup2006: keys: queue full, 0x%02x dropped", code);
        return;
    }
    if (s->key_q_n == 16) {
        /* A release already occupies the reserved tail slot; another
         * no-key report describes the same switch state. */
        return;
    }
    t = (s->key_q_head + s->key_q_n) % 16;
    s->key_q[t] = code;
    s->key_q_raw[t] = raw;
    s->key_q_n++;
    cup2006_key_pump(s);
}

/* 100 ms: one 0x0285 wheel frame from the track speed (once GPS runs). */
static void cup2006_wheel_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;
    double p;
    long n;

    timer_mod(s->wheel_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              CUP2006_WHEEL_PERIOD_MS);
    if (!s->gps_track_n || !s->gps_track_start_ms) {
        return;                          /* NEventWatcher not reading yet */
    }
    /* Start once the Sub-CPU link is up: the guest has taken the startup
     * status report (or a key report). Frames sent during the guest's link
     * handshake stall it. */
    if (!s->subpeer_replies && !s->key_guest_acks) {
        return;
    }
    /* A wheel frame whose ENQ the guest has not answered in 500 ms is
     * dropped so it can never hold up a key report. */
    if (s->peer_enq_armed && s->key_waiting >= CUP2006_WHEEL_FLAG &&
        s->key_waiting < CUP2006_STATUS_FLAG &&
        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - s->peer_enq_ms > 500) {
        /* The offered frame was never sent. Keep its pulse credits for
         * the next report even though the stale ENQ is withdrawn. */
        s->wheel_carry += (int16_t)(s->key_waiting & 0xffff);
        s->peer_enq_armed = false;
        s->key_waiting = -1;
        cup2006_key_pump(s);
    }
    /* Service existing reports even when the queue is full. An ACK's
     * one-shot pump can find the link busy and return; relying on the
     * next enqueue to pump then deadlocks at the fullness check below. */
    cup2006_key_pump(s);
    {
        /* Feed the distance as real pulses: the CARSIG model samples every
         * 20 ms (a sample counter at +0x0004 is not a pulse count); without
         * pulses the guest's locator assumes the car is parked and the
         * position moves in jumps. */
        p = cup2006_wheel_distance(s, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)) *
            CUP2006_PULSES_PER_M + s->wheel_carry;
    }
    n = MAX(INT16_MIN, MIN(INT16_MAX, lrint(floor(p))));
    s->wheel_carry = p - n;
    if (s->key_q_n >= 8) {
        s->wheel_carry += n;              /* defer, never lose distance */
        return;
    }
    cup2006_key_enqueue(s, CUP2006_WHEEL_FLAG | ((uint16_t)(int16_t)n), true);
}

/* ACC-off / power-down line: the board's power-change IRQ raised by the
 * Sub-CPU when the car's ignition/ACC line drops (INTEVT 0x240 ->
 * SYSINTR 0x1d). Host-controlled (F12 under acckey=on), off by default;
 * no guest state is written. */
static void cup2006_power_drop(void *opaque)
{
    Cup2006MachineState *s = opaque;

    if (!s->power_intc_src) {
        return;
    }
    sh_intc_toggle_source(s->power_intc_src, 0, -1);
    s->power_irq_raised = false;
}

static void cup2006_power_pulse(void *opaque)
{
    Cup2006MachineState *s = opaque;

    if (!s->power_intc_src) {
        return;
    }
    if (!s->power_irq_raised) {
        s->power_intc_src->prio = CUP2006_POWER_INTC_PRIO;
        s->power_intc_src->enable_count = s->power_intc_src->enable_max;
        sh_intc_toggle_source(s->power_intc_src, 0, 1);
        s->power_irq_raised = true;
        s->power_irq_raises++;
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
        cup2006_info(s, "cup2006: power: INTEVT 0x%03x pulse #%" PRIu64
                    " (SYSINTR 0x%x, acc %s)", CUP2006_POWER_INTEVT,
                    s->power_irq_raises, CUP2006_POWER_SYSINTR,
                    s->acc_on ? "on" : "off");
    }
    timer_mod(s->power_drop_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    s->power_pulses--;
    if (s->power_pulses > 0) {
        timer_mod(s->power_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

/* ACC-off: besides dropping the board power line, the peer tells the
 * application layer the MOST network went away ("NET OFF", `f1 8a 02`),
 * then DeviceMode Shutdown (`fc 81 01`). The seq byte is the same STUB as
 * the other known frames. */
static void cup2006_subpeer_queue_acc_off(Cup2006MachineState *s)
{
    if (!s->subpeer || !s->key_q_timer) {
        return;
    }
    s->subpeer_acc_off_stage = 1;
    cup2006_key_enqueue(s, CUP2006_STATUS_FLAG | 0xf18a, false);
    cup2006_info(s, "cup2006: subpeer: ACC-off queues f1 8a 02 (MOST NetService"
                " NET OFF), then fc 81 01");
}

/* Ignition-off, last step: the Sub-CPU removes the SH's power once the
 * guest reports DeviceMode Shutdown. STUB: the board waits until the
 * guest's flash has been quiet for CUP2006_POWER_CUT_QUIET_MS, then powers
 * off (QEMU exits). */
#define CUP2006_POWER_CUT_QUIET_MS 10000

static void cup2006_power_cut_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    int64_t last = MAX(s->power_cut_armed_ms, s->drf_activity_ms);

    if (now - last < CUP2006_POWER_CUT_QUIET_MS) {
        timer_mod(s->power_cut_timer, now + 500);
        return;
    }
    cup2006_info(s, "cup2006: power: Sub-CPU cuts power at %" PRId64 " ms (guest"
                " shut down at %" PRId64 " ms, last flash write %" PRId64
                " ms)", now, s->power_cut_armed_ms, s->drf_activity_ms);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
}

static void cup2006_power_cut_arm(Cup2006MachineState *s)
{
    if (s->power_cut_timer) {
        return;
    }
    s->power_cut_armed_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    s->power_cut_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      cup2006_power_cut_tick, s);
    timer_mod(s->power_cut_timer, s->power_cut_armed_ms + 500);
    cup2006_info(s, "cup2006: power: guest reports DeviceMode Shutdown (fc 01 01);"
                " power goes once its flash writes are done");
}

static void cup2006_power_start(Cup2006MachineState *s)
{
    if (s->subpeer_acc_off_stage) {
        return;                     /* the ignition is already off */
    }
    /* The application-layer notification is independent of the INTC stub. */
    cup2006_subpeer_queue_acc_off(s);
    if (!s->power_src_ready) {
        cup2006_info(s, "cup2006: power: F12 ignored, the power INTEVT source"
                    " is not available");
        return;
    }
    s->power_pulses = 8;
    timer_mod(s->power_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

/* QEMU input layer: queue key press and release reports. Never called by a
 * timer or PC. */
static void cup2006_key_input(DeviceState *dev, QemuConsole *src,
                              QemuInputEvent *evt)
{
    Cup2006MachineState *s = CUP2006_MACHINE(dev);
    QemuInputKeyEvent *key;
    const Cup2006KeyEntry *e;
    int qcode;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    /* QEMU 11: key events carry a Linux keycode; map it to the qcode the
     * key table is written in. */
    key = &evt->key;
    qcode = qemu_input_linux_to_qcode(key->key);
    if (qcode <= 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }
    if (s->acckey && qcode == Q_KEY_CODE_F12) {
        /* Host ACC-off control, off by default; never reaches the guest as a
         * key report. F12 down pulses the board power line. */
        if (key->down) {
            s->acc_on = false;
            cup2006_power_start(s);
        }
        return;
    }
    if (qcode == Q_KEY_CODE_F10 || qcode == Q_KEY_CODE_F11) {
        /* The DVD slot's shutter (F10) and the drive's eject button (F11):
         * board inputs, not guest key reports. */
        if (key->down) {
            if (qcode == Q_KEY_CODE_F10) {
                cup2006_dvd_shutter_key(s);
            } else {
                cup2006_dvd_eject_button(s);
            }
        }
        return;
    }
    e = cup2006_key_lookup(qcode);
    if (!e) {
        return;
    }
    /* QEMU's Cocoa keyboard forwards auto-repeat as repeated key-downs
     * (ui/kbd-state.c:qkbd_state_key_event). This is a joystick switch
     * not a keyboard: send its edges; the guest handles held-key repeat. */
    if (s->key_down[e->qcode] == key->down) {
        return;
    }
    s->key_down[e->qcode] = key->down;
    if (!key->down) {
        /* Release: raw 0x00 -> key word 0, for raw-lane keys only. */
        if (e->raw >= 0) {
            cup2006_key_enqueue(s, 0x00, true);
        }
        return;
    }
    s->key_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    cup2006_info(s, "cup2006: keys: qcode %s down at %" PRId64 " ms", e->name,
                s->key_ms);
    /* A key with a known raw byte is reported on the raw 0181 lane; the
     * others keep the E2 form. */
    cup2006_key_enqueue(s, e->raw >= 0 ? e->raw : e->code, e->raw >= 0);
}

static const QemuInputHandler cup2006_key_handler = {
    .name  = "cup2006-keypad",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = cup2006_key_input,
    .sync  = NULL,
};

static void cup2006_scif0_write(Cup2006MachineState *s, hwaddr base,
                                hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr phys = base + addr;
    unsigned i;

    if (addr + size <= CUP2006_SCIF_SIZE) {
        for (i = 0; i < size; i++) {
            if (addr + i == CUP2006_SCIF0_SCFSR ||
                addr + i == CUP2006_SCIF0_SCFSR + 1) {
                /* SCFSR status bits are cleared by writing 0; writing 1
                 * leaves them alone (sh7751 manual), so a write of 1 must
                 * not set TDFE/TEND/RDF. */
                s->scif0_file[addr + i] &= (value >> (8 * i)) & 0xff;
            } else {
                s->scif0_file[addr + i] = (value >> (8 * i)) & 0xff;
            }
        }
    }
    /* Writing SCFTDR is the transmit operation the manual names as TDFE's
     * setting condition: TDFE/TEND clear now and are set again one byte
     * time later (cup2006_scif0_tx_tick). */
    if (addr <= CUP2006_SCIF0_SCFTDR && addr + size > CUP2006_SCIF0_SCFTDR) {
        s->scif0_file[CUP2006_SCIF0_SCFSR] &=
            (uint8_t)~(CUP2006_SCIF0_SCFSR_TDFE | CUP2006_SCIF0_SCFSR_TEND);
        timer_mod(s->scif0_tx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  CUP2006_SCIF_BYTE_NS);
    }
    for (i = 0; i < size; i++) {
        hwaddr off = addr + i;

        /* The ACK consumer runs for the Sub-CPU link too (subpeer=on with
         * keys=off still needs the guest's 0x06 to retire peer frames); the
         * key-report paths inside stay inert without keys=on because no
         * input handler is registered then. */
        if (off == CUP2006_SCIF0_SCFTDR && (s->keys || s->subpeer)) {
            cup2006_key_guest_tx(s, (value >> (8 * i)) & 0xff);
        }
        if (off == CUP2006_SCIF0_SCFTDR && s->subpeer) {
            cup2006_subpeer_guest_tx(s, (value >> (8 * i)) & 0xff);
        }
    }
    /* SCSCR/SCFCR/SCLSR writes can change the receive condition. */
    cup2006_scif0_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: scif0 write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_scif0_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_scif0_read(opaque, CUP2006_SCIF0_BASE, addr, size);
}

static void cup2006_scif0_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    cup2006_scif0_write(opaque, CUP2006_SCIF0_BASE, addr, value, size);
}

static uint64_t cup2006_scif0_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_scif0_read(opaque, CUP2006_SCIF0_A7_BASE, addr, size);
}

static void cup2006_scif0_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    cup2006_scif0_write(opaque, CUP2006_SCIF0_A7_BASE, addr, value, size);
}

static const MemoryRegionOps cup2006_scif0_ops = {
    .read = cup2006_scif0_p4_read,
    .write = cup2006_scif0_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps cup2006_scif0_a7_ops = {
    .read = cup2006_scif0_a7_read,
    .write = cup2006_scif0_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
