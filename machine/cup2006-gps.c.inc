/*
 * CU-P2006 machine: GPS tracking chip (measurement level, radio replay), car-sensor track, read-only GPS/locator diagnostics.
 *
 * Part of machine/cup2006.c, compiled as one translation unit (it is
 * #included there, in this order); not a standalone source file.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static void cup2006_mbx_ta_frame(Cup2006MachineState *s);

static void cup2006_gps_update_irq(Cup2006MachineState *s)
{
    /* STUB: RX-ready is a level source until the host consumes the mailbox;
     * a latch clear must not hide an unread message when the IRQ is
     * re-enabled. */
    bool pending = (s->gps_regs_w[CUP2006_GPS_LATCH >> 2] & 1) ||
                   (s->gps_regs_w[CUP2006_GPS_CAUSE >> 2] & 3);
    bool want = pending &&
                (s->gps_regs_w[CUP2006_GPS_CTRL >> 2] & CUP2006_GPS_CTRL_IRQEN);
    struct intc_source *src = s->gps_intc_src;

    if (!src || want == (src->asserted != 0)) {
        return;
    }
    sh_intc_toggle_source(src, 0, want ? 1 : -1);
}
static const float *cup2006_track_row(Cup2006MachineState *s, int64_t now_ms,
                                      unsigned *idx);

/* Post one device->host mailbox message at +0x800 (header: +0 u32
 * +4 u16 len, +6 u8 xor-fold of the payload words), set the cause bit(s)
 * and the latch, raise the IRQ. */
static void cup2006_gnss_post(Cup2006MachineState *s, uint32_t cause,
                              const uint8_t *payload, uint16_t len)
{
    uint32_t *r = s->gps_regs_w;
    uint8_t *rx = (uint8_t *)&r[CUP2006_GPS_RX >> 2];
    uint32_t x = 0;
    unsigned i;

    if (len > 0x800 - 8) {
        return;
    }
    memset(rx, 0, 8 + len);
    memcpy(rx + 8, payload, len);
    for (i = 0; i + 4 <= len; i += 4) {
        x ^= ldl_le_p(payload + i);
    }
    stl_le_p(rx, 0);
    stw_le_p(rx + 4, len);
    rx[6] = (uint8_t)(x ^ (x >> 8) ^ (x >> 16) ^ (x >> 24));
    r[CUP2006_GPS_STATUS >> 2] |= 0x00010000;
    r[CUP2006_GPS_CAUSE >> 2] |= cause;
    r[CUP2006_GPS_LATCH >> 2] |= 1;
    cup2006_gps_update_irq(s);
}

/* Replay tracking observations (never a PVT solution) from a radio
 * recording. STUB: the replay runs against the device clock; assigned PRNs
 * still select channels through the host mailbox. */
static bool cup2006_gnss_radio_tick(Cup2006MachineState *s)
{
    const char *path = s->gnssreplay;
    Cup2006GnssReplay *rp = &s->gnss_rp;
    uint8_t meas[12 * 0x50] = { 0 };
    uint32_t frame, ch, first, f;
    int64_t now;

    if (!path || !*path) {
        return false;
    }
    if (!rp->data) {
        GError *err = NULL;

        if (!g_file_get_contents(path, &rp->data, &rp->size, &err)) {
            error_report("cup2006: cannot use radio replay: %s",
                         err->message);
            exit(1);
        }
        if (rp->size < 16 || memcmp(rp->data, "GNSSIQ1\0", 8)) {
            error_report("cup2006: invalid GNSS radio replay header");
            exit(1);
        }
        rp->frames = ldl_le_p(rp->data + 8);
        rp->interval = ldl_le_p(rp->data + 12);
        if (!rp->frames || !rp->interval || rp->frames > 8192 ||
            rp->size != 16 + (gsize)rp->frames * 4096) {
            error_report("cup2006: invalid GNSS radio replay length");
            exit(1);
        }
        cup2006_info(s, "cup2006: radio tracking replay %s: %u frames, %u ms,"
                     " starts at %" PRId64 " ms", path, rp->frames,
                     rp->interval, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (rp->start < 0) {
        rp->start = now;
        /* One scenario clock for radio, wheel pulses and gyro: keep the
         * clock that started with the track (gpstrack=), if any.  This is
         * host-side sensor timing, not a change to any guest state. */
        if (!s->gps_track_start_ms) {
            s->gps_track_start_ms = now;
        }
    }
    /* Keep the pending message intact until acknowledgement: OR-ing another
     * cause over a different payload would make the type bits and the payload
     * disagree. */
    if (s->gps_regs_w[CUP2006_GPS_CAUSE >> 2] & 3) {
        /* Mailbox backpressure must not consume a 500 ms measurement
         * period. Retry after the host has had a chance to copy/ACK it. */
        timer_mod(s->gps_timer, now + 1);
        return true;
    }
    frame = (now - rp->start) / rp->interval;
    if (frame >= rp->frames && !rp->ended) {
        cup2006_info(s, "cup2006: radio recording ended at %" PRId64
                    " ms (%u frames); reporting signal loss", now,
                    rp->frames);
        rp->ended = true;
    }
    first = rp->previous == UINT32_MAX ? 0 : rp->previous + 1;
    for (f = first; f <= frame && f < rp->frames; f++) {
        const uint8_t *nav = (uint8_t *)rp->data + 16 + f * 4096 + 32 * 0x50;

        for (ch = 0; ch < 12; ch++) {
            int prn = s->gnss_chan_prn[ch];

            if (prn >= 1 && prn <= 32 && ldl_le_p(nav + (prn - 1) * 0x30)) {
                memcpy(rp->pending + ch * 0x30, nav + (prn - 1) * 0x30, 0x30);
            }
        }
    }
    rp->previous = frame;
    for (ch = 0; ch < 12; ch++) {
        int prn = s->gnss_chan_prn[ch];
        uint8_t *r = meas + ch * 0x50;

        if (prn < 1 || prn > 32) {
            continue;
        }
        if (frame < rp->frames) {
            memcpy(r, rp->data + 16 + frame * 4096 + (prn - 1) * 0x50, 0x50);
        }
        if (!ldl_le_p(r)) {
            stl_le_p(r, prn);
            r[0x0b] = 1;  /* no acquired signal: host may search another PRN */
        }
        if (ldl_le_p(rp->pending + ch * 0x30) != prn) {
            memset(rp->pending + ch * 0x30, 0, 0x30);
        }
    }
    if (!rp->sent_nav &&
        memcmp(rp->pending, (uint8_t[12 * 0x30]){0}, sizeof(rp->pending))) {
        cup2006_gnss_post(s, 2, rp->pending, sizeof(rp->pending));
        memset(rp->pending, 0, sizeof(rp->pending));
        rp->sent_nav = true;
        /* Nav and measurements are separate mailbox messages: deliver this
         * frame's measurements once the nav copy is ACKed. */
        timer_mod(s->gps_timer, now + 1);
    } else {
        cup2006_gnss_post(s, 0x80000001, meas, sizeof(meas));
        rp->sent_nav = false;
        timer_mod(s->gps_timer,
                  rp->start + ((int64_t)frame + 1) * rp->interval);
    }
    return true;
}

/* Host->device message at +0x400 (doorbell bit 16): type-2 TX = channel/PRN
 * assignment (0x134 bytes; +0 type, +4 u16 len, +6 xor, then 12 x 0x18
 * channel commands), type-3 = aiding (ignored). */
static void cup2006_gnss_host_tx(Cup2006MachineState *s)
{
    const uint8_t *tx = (const uint8_t *)&s->gps_regs_w[0x400 >> 2];
    unsigned ch;
    uint32_t type = ldl_le_p(tx);
    unsigned len = lduw_le_p(tx + 4);

    if (type == 2 && len >= 0x14 + 12 * 0x18) {
        /* The mailbox message adds its 8-byte header, so the command array
         * starts at tx+0x1c. */
        for (ch = 0; ch < 12; ch++) {
            const uint8_t *c = tx + 8 + 0x14 + ch * 0x18;
            uint32_t cmd = ldl_le_p(c);
            uint32_t prn = ldl_le_p(c + 4);

            if (cmd == 2) {
                /* The device must echo the command word unchanged. */
                s->gnss_chan_prn[ch] = (int)(prn & 0xff);
            }
        }
    }
}

static void cup2006_gps_tick(void *opaque)
{
    Cup2006MachineState *s = opaque;
    uint32_t *r = s->gps_regs_w;

    timer_mod(s->gps_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              CUP2006_GPS_TICK_MS);
    if (!s->gps_handshake && !s->gps_ticks) {
        return;                      /* gpslib not started yet */
    }
    s->gps_handshake = false;        /* running: state 3 */
    if (cup2006_gnss_radio_tick(s)) {
        s->gps_ticks++;
        return;
    }
    /* No radio recording: the tracking chip has no signal and reports an
     * empty type-1 epoch every 500 ms (gpslib keeps running, no fix). */
    memset(&r[CUP2006_GPS_RX >> 2], 0, 0x3c8);
    r[(CUP2006_GPS_RX + 4) >> 2] = 0x3c0;   /* u16 len, u8 xor of zeros */
    r[CUP2006_GPS_STATUS >> 2] |= 0x00010000;
    r[CUP2006_GPS_CAUSE >> 2] |= 0x80000001;
    r[CUP2006_GPS_LATCH >> 2] |= 1;
    s->gps_ticks++;
    if (s->gps_ticks <= 4 || (s->gps_ticks & (s->gps_ticks - 1)) == 0) {
        cup2006_dbg(s,  "cup2006: STUB gps tick #%" PRIu64 " (empty type-1,"
                " INTEVT 0x%03x, irq-enable %u)\n",
                s->gps_ticks, CUP2006_GPS_INTEVT,
                !!(r[CUP2006_GPS_CTRL >> 2] & CUP2006_GPS_CTRL_IRQEN));
    }
    cup2006_gps_update_irq(s);
}


/*
 * The track clock: row = time since the first use / row interval. Tracks
 * loop; finite radio replays stop at EOF.
 */
static const float *cup2006_track_row(Cup2006MachineState *s, int64_t now_ms,
                                      unsigned *idx)
{
    uint64_t i;

    if (!s->gps_track_start_ms) {
        s->gps_track_start_ms = now_ms;
    }
    i = (uint64_t)((now_ms - s->gps_track_start_ms) /
                   (s->gps_track_dt * 1000.0));
    if ((s->gnssreplay && *s->gnssreplay)) {
        *idx = (unsigned)MIN(i, s->gps_track_n - 1);
        if (i >= s->gps_track_n) {
            /* STUB: a finite recording ends at rest; only host sensor
             * values are stopped, the guest's last fix is never changed. */
            memcpy(s->gps_track_stopped, &s->gps_track[*idx * 7],
                   sizeof(s->gps_track_stopped));
            s->gps_track_stopped[3] = 0;
            s->gps_track_stopped[5] = 0;
            s->gps_track_stopped[6] = 0;
            return s->gps_track_stopped;
        }
    } else {
        *idx = (unsigned)(i % s->gps_track_n);
    }
    return &s->gps_track[*idx * 7];
}

/* Integrate the transducer input on the scenario clock, independently of
 * serial delivery latency. A late UART report must not shorten the drive.
 * Speed is held over each source sample, as in the track sampler above. */
static double cup2006_wheel_distance(Cup2006MachineState *s, int64_t now_ms)
{
    double dt = s->gps_track_dt * 1000.0;
    double from = MAX(s->wheel_track_last_ms, s->gps_track_start_ms) -
                  s->gps_track_start_ms;
    double to = now_ms - s->gps_track_start_ms;
    double metres = 0;
    bool finite = (s->gnssreplay && *s->gnssreplay);

    s->wheel_track_last_ms = now_ms;
    while (from < to) {
        uint64_t i = (uint64_t)floor(from / dt);
        double end = MIN(to, (i + 1) * dt);

        if (finite && i >= s->gps_track_n) {
            break;
        }
        metres += s->gps_track[(i % s->gps_track_n) * 7 + 3] / 3.6 *
                  ((end - from) / 1000.0);
        from = end;
    }
    return metres;
}

static void cup2006_gps_load_track(Cup2006MachineState *s)
{
    FILE *f = fopen(s->gpstrack, "r");
    char line[256];
    unsigned cap = 0;
    double t0 = 0;

    if (!f) {
        error_report("cup2006: gpstrack %s: cannot open", s->gpstrack);
        exit(1);
    }
    while (fgets(line, sizeof(line), f)) {
        double t, la, lo, al, sp, hd, yw = 0, ok = 1;
        int nf = sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &t, &la, &lo,
                        &al, &sp, &hd, &yw, &ok);

        if (nf < 6) {
            continue;
        }
        if (s->gps_track_n == 1) {
            s->gps_track_dt = t - t0;
        }
        if (s->gps_track_n == 0) {
            t0 = t;
        }
        if (s->gps_track_n == cap) {
            cap = cap ? cap * 2 : 1024;
            s->gps_track = g_renew(float, s->gps_track, cap * 7);
        }
        float *r = &s->gps_track[s->gps_track_n++ * 7];
        r[0] = la; r[1] = lo; r[2] = al; r[3] = sp; r[4] = hd; r[5] = yw;
        r[6] = nf >= 8 ? ok : 1;
    }
    fclose(f);
    if (!s->gps_track_n) {
        error_report("cup2006: gpstrack %s: no rows", s->gpstrack);
        exit(1);
    }
    if (s->gps_track_dt <= 0) {
        s->gps_track_dt = 1.0;
    }
    /* One scenario clock for radio, wheel pulses and gyro starts when the
     * track is loaded, so gpstrack= alone (no gnssreplay=) still drives the
     * wheel and gyro; this is host-side sensor timing, not guest state. */
    if (!s->gps_track_start_ms) {
        s->gps_track_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    }
    cup2006_info(s, "cup2006: car sensor track: %u rows from %s (%.1f Hz)%s",
                 s->gps_track_n, s->gpstrack, 1.0 / s->gps_track_dt,
                 (s->gnssreplay && *s->gnssreplay) ? "" :
                 "; no GNSS recording, the receiver has no signal");
}

static uint64_t cup2006_gps_read(Cup2006MachineState *s, hwaddr base,
                                 hwaddr addr, unsigned size)
{
    hwaddr phys = base + addr;
    uint64_t value;

    switch (addr) {
    case CUP2006_GPS_STATUS:
        if (!s->gpstrack || !s->gpstrack[0]) {
            /* STUB: the latch reports its command as accepted. */
            value = CUP2006_GPS_STATUS_READY;
        } else {
            /* Status bits 1:0 select the pending message type (1 right after
             * the handshake, 2 = type-2 message ready, 3 = running);
             * bit 16 = RX ready. */
            uint32_t cause = s->gps_regs_w[CUP2006_GPS_CAUSE >> 2];
            uint32_t lo;

            if (cause & 2) {
                lo = 2;                  /* type-2 message ready */
            } else if (s->gps_handshake) {
                lo = 1;
            } else {
                lo = 3;                  /* running; bit0 selects type 1 */
            }
            value = (s->gps_regs_w[addr >> 2] & ~3u) | lo;
        }
        break;
    case CUP2006_GPS_CMD:
        value = s->gps_cmd;
        break;
    default:
        value = s->gps_regs_w[(addr & (CUP2006_GPS_SIZE - 1)) >> 2];
        if (addr == CUP2006_GPS_CTRL) {
            value &= ~(uint64_t)CUP2006_GPS_CTRL_DOORBELL; /* TX consumed */
        }
        break;
    }
    if (size < 4) {
        value = (value >> ((addr & 3) * 8)) & ((1ull << (size * 8)) - 1);
    }
    cup2006_gps_note(s, phys, false, size, value);
    cup2006_dbg(s,
                  "cup2006: gps read pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
    return value;
}

static void cup2006_gps_write(Cup2006MachineState *s, hwaddr base,
                              hwaddr addr, uint64_t value, unsigned size)
{
    hwaddr phys = base + addr;

    if (addr == CUP2006_GPS_CMD) {
        s->gps_cmd = (uint32_t)value;
        if ((value >> 24) == 0xa5) {
            s->gps_handshake = true;
        }
    }
    {
        uint32_t *w = &s->gps_regs_w[(addr & (CUP2006_GPS_SIZE - 1)) >> 2];
        unsigned sh = (addr & 3) * 8;
        uint32_t m = size >= 4 ? 0xffffffffu : (((1u << (size * 8)) - 1) << sh);

        *w = (*w & ~m) | (((uint32_t)value << sh) & m);
    }
    if (addr == CUP2006_GPS_CTRL && (value & CUP2006_GPS_CTRL_DOORBELL)) {
        cup2006_gnss_host_tx(s);
    }
    if (addr < 0x20) {
        cup2006_gps_update_irq(s);
    }
    cup2006_gps_note(s, phys, true, size, value);
    cup2006_dbg(s,
                  "cup2006: gps write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  cup2006_guest_pc(), phys, size, value);
}

static uint64_t cup2006_gps_p4_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_gps_read(opaque, CUP2006_GPS_BASE, addr, size);
}

static void cup2006_gps_p4_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_gps_write(opaque, CUP2006_GPS_BASE, addr, value, size);
}

static uint64_t cup2006_gps_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_gps_read(opaque, CUP2006_GPS_A7_BASE, addr, size);
}

static void cup2006_gps_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    cup2006_gps_write(opaque, CUP2006_GPS_A7_BASE, addr, value, size);
}

static const MemoryRegionOps cup2006_gps_ops = {
    .read = cup2006_gps_p4_read,
    .write = cup2006_gps_p4_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps cup2006_gps_a7_ops = {
    .read = cup2006_gps_a7_read,
    .write = cup2006_gps_a7_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
