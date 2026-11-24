/*
 * Volvo CU-P2006 board: cup2006-dvd device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup2006-internal.h"


static void cup2006_dvd5_irq_next(Cup2006MachineState *s)
{
    struct intc_source *src = s->dvd5_intc_src;

    if (!s->dvd5irq || !src || s->dvd5_rx_pos >= s->dvd5_rx_len) {
        return;
    }
    if (!(s->intc2_enabled & CUP2006_DVD5_INTC2_BIT)) {
        /*
         * The OAL OEMInterruptEnable for SYSINTR 0x1C sets this bit; without
         * it the line cannot reach the CPU (the 2DG rule).
 */
        s->dvd5_irq_blocked++;
        if (s->dvd5_irq_blocked <= 4) {
            trace_cup2006_dvd_dvd5_intevt_not_raised(CUP2006_DVD5_INTEVT, s->intc2_enabled);
        }
        return;
    }
    if (!src->asserted) {
        /*
         * STUB: the SH7770 INTC2 priority/enable for this line is
         * unmodelled, so the machine owns them; re-assert them at the
         * raise edge and kick the vCPU.
 */
        src->prio = CUP2006_DVD5_INTC_PRIO;
        src->enable_count = src->enable_max;
        sh_intc_toggle_source(src, 0, 1);
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
        s->dvd5_irqs++;
        if (s->dvd5_irqs <= 32) {
            trace_cup2006_dvd_dvd5_intevt_raised_byte(CUP2006_DVD5_INTEVT, s->dvd5_irqs, s->dvd5_rx[s->dvd5_rx_pos], s->dvd5_rx_pos + 1, s->dvd5_rx_len, cup2006_guest_pc(), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
}

/* STUB: instant spin-up: a drive that reported "loading" finishes and tells
 * the host with an unsolicited B1 status frame (frame[1]=0x48 class 0x40
 * ready, frame[3]=4, 11 bytes, rest zero). */
void cup2006_dvd5_spinup(Cup2006MachineState *s)
{
    static const uint8_t b1[11] = {
        0xb1, CUP2006_DVD5_ST_ROM, 0x20, CUP2006_DVD5_TYPE_DVDROM,
    };

    s->dvd5_spinup_pending = false;
    s->dvd5_disc_ready = true;
    if (s->dvd5_rx_pos < s->dvd5_rx_len) {
        /* Bytes are still queued on the wire: put the B1 after them, dropping
         * the oldest if the buffer cannot hold both, instead of discarding
         * the queued status frames. */
        unsigned left = s->dvd5_rx_len - s->dvd5_rx_pos;
        unsigned skip = 0;

        if (left + sizeof(b1) > CUP2006_DVD5_RX_MAX) {
            skip = left + sizeof(b1) - CUP2006_DVD5_RX_MAX;
            warn_report("cup2006: dvd5: %u queued byte(s) dropped to fit"
                        " the spin-up B1", skip);
        }
        memmove(s->dvd5_rx, s->dvd5_rx + s->dvd5_rx_pos + skip, left - skip);
        memcpy(s->dvd5_rx + left - skip, b1, sizeof(b1));
        s->dvd5_rx_len = left - skip + sizeof(b1);
    } else {
        memcpy(s->dvd5_rx, b1, sizeof(b1));
        s->dvd5_rx_len = sizeof(b1);
    }
    s->dvd5_rx_pos = 0;
    s->dvd5_rx_unsol = true;
    trace_cup2006_dvd_stub_dvd5_spin_up(b1[1], b1[2], b1[3]);
}

/*
 * The drive reports a medium change on its own with an unsolicited B1 status
 * frame (DriveThread's ring takes only B1/B2/B5, see the spin-up above), queued
 * behind whatever is still on the wire. Bytes 4..10 stay zero.
 */
static void cup2006_dvd5_status_b1(Cup2006MachineState *s, uint8_t st,
                                   uint8_t qual, uint8_t type,
                                   const char *what)
{
    const uint8_t b1[11] = { 0xb1, st, qual, type };
    unsigned left = 0;

    if (s->dvd5_rx_pos < s->dvd5_rx_len) {
        left = s->dvd5_rx_len - s->dvd5_rx_pos;
        memmove(s->dvd5_rx, s->dvd5_rx + s->dvd5_rx_pos, left);
    }
    if (left + sizeof(b1) > CUP2006_DVD5_RX_MAX) {
        warn_report("cup2006: dvd5 %s: B1 dropped, %u bytes still on the wire",
                    what, left);
        return;
    }
    memcpy(s->dvd5_rx + left, b1, sizeof(b1));
    s->dvd5_rx_len = left + sizeof(b1);
    s->dvd5_rx_pos = 0;
    s->dvd5_rx_unsol = true;
    trace_cup2006_dvd_drive_unsolicited_b1_at(what, st, qual, type, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    cup2006_dvd5_irq_next(s);
}

/* A disc goes in (the next of dvdimage=/dvdnext=, cycling); the drive then
 * reports "loading" (class 0x30 + present, type 0), "packed in" (type 1),
 * and "ready" via the spin-up B1. STUB: type 1's drive-side name is unknown. */
void cup2006_dvd_insert(void *opaque)
{
    Cup2006MachineState *s = opaque;
    Error *err = NULL;
    const char *path;

    if (s->dvd_fd >= 0 || !s->dvd_ndiscs) {
        return;
    }
    path = s->dvd_discs[s->dvd_next];
    s->dvd_next = (s->dvd_next + 1) % s->dvd_ndiscs;
    s->dvd_fd = qemu_open(path, O_RDONLY | O_BINARY, &err);
    if (s->dvd_fd < 0) {
        warn_report("cup2006: drive: could not open '%s': %s", path,
                    error_get_pretty(err));
        error_free(err);
        return;
    }
    s->dvd_size = lseek(s->dvd_fd, 0, SEEK_END);
    g_free(s->dvdimage);
    s->dvdimage = g_strdup(path);
    trace_cup2006_dvd_drive_disc_inserted_bytes(path, s->dvd_size);
    s->dvd5_ejected = false;
    s->dvd5_disc_ready = false;
    cup2006_dvd5_status_b1(s, 0xb0, 0x20, 0x00, "loading");
    cup2006_dvd5_status_b1(s, 0xb0, 0x20, 0x01, "packed in");
    s->dvd5_spinup_pending = true;
}

/*
 * F10: the user's hand on the disc-insert shutter in front of the slot
 * (board GPIO port 3 bit 8). Closed at power-on, as on a unit with its
 * map disc in.
 */
void cup2006_dvd_shutter_key(Cup2006MachineState *s)
{
    s->dvd_shutter_open = !s->dvd_shutter_open;
    trace_cup2006_dvd_drive_shutter_at_ms(s->dvd_shutter_open ? "opened" : "closed", qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

/* F11: eject button / hand at the slot (host input only): loaded -> eject
 * (B1 class 0x10 + present), in-slot -> taken (B1 class 0x00, next disc goes
 * in 3 s later), empty -> insert the next disc now. STUB: while a disc sits
 * ejected, status and read replies carry class 0x90. */
void cup2006_dvd_eject_button(Cup2006MachineState *s)
{
    if (!s->dvd5irq) {
        return;
    }
    if (s->dvd_fd >= 0 && !s->dvd5_ejected) {
        s->dvd5_ejected = true;
        s->dvd5_disc_ready = false;
        s->dvd5_spinup_pending = false;
        cup2006_dvd5_status_b1(s, 0x90, 0x00, 0x00, "eject button");
    } else if (s->dvd_fd >= 0) {
        trace_cup2006_dvd_drive_disc_taken_from(s->dvdimage);
        qemu_close(s->dvd_fd);
        s->dvd_fd = -1;
        s->dvd_size = 0;
        s->dvd5_ejected = false;
        cup2006_dvd5_status_b1(s, 0x00, 0x00, 0x00, "slot empty");
        timer_mod(s->dvd_insert_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 3000);
    } else if (!timer_pending(s->dvd_insert_timer)) {
        cup2006_dvd_insert(s);
    }
}

/* The OAL ISR acked 0xFFE00044 bit 28: the line is serviced; the byte itself
 * is consumed by the ISR's read of the data port (a receive register pops
 * on read). Here only deassert and present whatever is still staged. */
void cup2006_dvd5_irq_ack(Cup2006MachineState *s)
{
    struct intc_source *src = s->dvd5_intc_src;

    if (src && src->asserted) {
        sh_intc_toggle_source(src, 0, -1);
    }
    s->dvd5_irq_acks++;
    if (s->dvd5_irq_acks <= 32) {
        trace_cup2006_dvd_dvd5_intevt_acked(CUP2006_DVD5_INTEVT, s->dvd5_irq_acks, cup2006_guest_pc(), s->dvd5_rx_pos, s->dvd5_rx_len);
    }
    if (s->dvd5_spinup_pending && s->dvd5_rx_pos >= s->dvd5_rx_len) {
        cup2006_dvd5_spinup(s);
    }
    cup2006_dvd5_irq_next(s);
}

/* STUB: instant sector DMA. Copy the requested 2048-byte sectors of the disc
 * image to the destination the driver programmed at +0x84 (bounded by its
 * +0x88 count) and raise the completion line; a missing disc or out-of-range
 * LBA serves zeros. */
static void cup2006_dvd5_read_sectors(Cup2006MachineState *s, uint32_t lba,
                                      uint32_t sectors)
{
    uint32_t dest = cup2006_reg32_read(s->dvd5_reg, CUP2006_DVD5_DMA_DAR, 4) &
                    0x1fffffff;
    uint64_t units = cup2006_reg32_read(s->dvd5_reg, CUP2006_DVD5_DMA_COUNT, 4);
    uint64_t len = (uint64_t)sectors * CUP2006_DVD_SECTOR_SIZE;
    uint64_t offset = (uint64_t)lba * CUP2006_DVD_SECTOR_SIZE;
    uint64_t ram_size = memory_region_size(&s->ram);
    size_t got = 0;
    uint8_t *buf;

    if (units && units * 2 < len) {
        len = units * 2;
    }
    if (len == 0 || len > CUP2006_DVD_MAX_XFER || dest >= ram_size ||
        len > ram_size - dest) {
        trace_cup2006_dvd_stub_dvd5_b8_read(s->dvd5_reads, lba, sectors, dest, units);
        return;
    }
    buf = g_malloc0(len);
    if (s->dvd_fd >= 0 && !s->dvd5_ejected && offset < s->dvd_size) {
        size_t want = MIN(len, s->dvd_size - offset);

        while (got < want) {
            ssize_t done = pread(s->dvd_fd, buf + got, want - got, offset + got);

            if (done <= 0) {
                break;
            }
            got += done;
        }
    }
    physical_memory_write(dest, buf, len);
    g_free(buf);
    if (s->dvd5_reads <= 64) {
        trace_cup2006_dvd_stub_dvd5_b8_read_2(s->dvd5_reads, lba, sectors, dest, len, got);
    }
    if (s->dvd5_dma_src && !s->dvd5_dma_src->asserted) {
        s->dvd5_dma_src->prio = CUP2006_DVD5_DMA_INTC_PRIO;
        s->dvd5_dma_src->enable_count = s->dvd5_dma_src->enable_max;
        sh_intc_toggle_source(s->dvd5_dma_src, 0, 1);
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
    }
}

static void cup2006_dvd5_reply(Cup2006MachineState *s, const uint8_t *req,
                               unsigned len)
{
    uint8_t reply[16];
    hwaddr pa;
    bool spinup_was_pending = s->dvd5_spinup_pending;

    memset(reply, 0, sizeof(reply));

    if (len >= 10 && req[0] == 0xb8 && req[1] == 0x93) {
        /* a read request (b8 93 <lba> <sectors>): sectors go to the engine's
         * DMA destination, completion line raised; reply is the expected 0xB0
         * with the drive's status (class 0x00/0x10 = eject, bit 3 set). */
        s->dvd5_reads++;
        cup2006_dvd5_read_sectors(s,
                (req[2] << 24) | (req[3] << 16) | (req[4] << 8) | req[5],
                (req[6] << 24) | (req[7] << 16) | (req[8] << 8) | req[9]);
        reply[0] = 0xb0;
        if (s->dvd_fd >= 0 && s->dvd5_disc_ready) {
            reply[1] = CUP2006_DVD5_ST_ROM;
            reply[2] = 0x20;
            reply[3] = CUP2006_DVD5_TYPE_DVDROM;
        } else if (s->dvd5_ejected) {
            reply[1] = 0x90;   /* ejected, in the slot (F11 above) */
        }
    } else if (len >= 3 && req[0] == 0xb8 && req[1] == 0x3a) {
        /* Echo the request's command byte. STUB: until the canonical B4
         * command arrives reply[2] carries the retry "resend" bit 0x10, the
         * value the guest's gate accepts; the B4 reply has it clear. */
        reply[0] = req[2];
        reply[1] = 0x3a;
        reply[2] = (req[2] == 0xb4) ? 0x00 : 0x10;
        if (req[2] == 0xb4) {
            reply[4] = 0x00;   /* status: no error */
            reply[5] = 0x00;
        }
    } else if (s->dvd5irq && len >= 2 && req[0] == 0xb8 &&
               (req[1] == 0x80 || req[1] == 0x81 || req[1] == 0x01 ||
                req[1] == 0x04)) {
        /* disc-status poll family (b8 80/81/01/04): reply 0xB0 with the media
         * class in frame[1] (0x30 + present = loaded, 0x00 with bit 7 clear =
         * no media) and frame[2] bit 0x20 the loaded qualifier. STUB: the
         * media state is the board's (dvdimage= opened), not a guest check. */
        reply[0] = 0xb0;
        if (s->dvd_fd >= 0 && s->dvd5_disc_ready) {
            /* After spin-up: the loaded DVD-ROM status (see the B1 frame). */
            reply[1] = CUP2006_DVD5_ST_ROM;
            reply[2] = 0x20;
            reply[3] = CUP2006_DVD5_TYPE_DVDROM;
        } else if (s->dvd5_ejected) {
            reply[1] = 0x90;   /* ejected, in the slot (F11 above) */
        } else if (s->dvd_fd >= 0) {
            reply[1] = 0xb0;   /* class 0x30 + present bit 7 (event 21) */
            reply[2] = 0x20;
            s->dvd5_spinup_pending = true;
        } else {
            reply[1] = 0x00;   /* class 0x00, bit 7 clear (event 20) */
            reply[2] = 0x00;
        }
    } else if (s->dvd5irq && len >= 2 && req[0] == 0xb8 &&
               (req[1] == 0x45 || req[1] == 0x87)) {
        /* the two post-"Loading start" queries: the ACK/resend class is
         * reply[2] bit 0x10 (set for 0x45, clear for 0x87). STUB: the
         * byte's drive-side meaning is unknown; answers only this family. */
        reply[0] = 0xb0;
        reply[1] = req[1];
        /* b8 87 is the driver side of IOCTL_DVD_PREVENT_ALLOW_PARENTAL.
         * STUB: reply[2]=0 (no ACK/resend class) is the value that makes the
         * guest's own code report the ioctl as handled. */
        reply[2] = (req[1] == 0x87) ? 0x00 : 0x10;
    } else {
        /* Non-3A requests: the retry gate's expected byte is 0xB0. */
        reply[0] = 0xb0;
    }

    /* A drive that reported "loading" finishes before it answers the next
     * command: settle the spin-up state and patch the reply to ROM-ready
     * BEFORE the reply is published to any of its three destinations (the
     * reply window, the driver's VA buffer, the byte port). */
    if (spinup_was_pending && s->dvd5_spinup_pending) {
        cup2006_dvd5_spinup(s);
        if (req[0] == 0xb8 && (req[1] == 0x80 || req[1] == 0x81 ||
                               req[1] == 0x01 || req[1] == 0x04 ||
                               req[1] == 0x93)) {
            reply[1] = CUP2006_DVD5_ST_ROM;
            reply[2] = 0x20;
            reply[3] = CUP2006_DVD5_TYPE_DVDROM;
        }
    }

    /* The shared reply window (the driver mapped this PA with VirtualCopy). */
    memcpy(s->dvd5_rpl, reply, 8);
    /* Doorbell/generation word so a later reader sees a new frame. */
    cup2006_reg32_write(s->dvd5_rpl, 0x40, 8, 4);

    /* The engine's DMA destination is the driver's reply buffer. STUB: the
     * driver does not program that address in any modelled register (it is
     * a fixed VA from one firmware build), so translate it with the current
     * process's MMU and write there; see README limitations. */
    if (current_cpu) {
        pa = cpu_get_phys_page_debug(current_cpu, CUP2006_DVD5_REPLY_VA);
    } else {
        pa = (hwaddr)-1;
    }
    if (pa != (hwaddr)-1) {
        physical_memory_write(pa, reply, 8);
    }

    s->dvd5_replies++;
    /* stage the reply at the device's byte port and raise the line. */
    if (s->dvd5_rx_unsol && s->dvd5_rx_pos < s->dvd5_rx_len) {
        /* An unsolicited frame is still on the wire: queue the reply after
         * it.  The drive's buffer holds CUP2006_DVD5_RX_MAX bytes; what does
         * not fit is the oldest unsolicited data, which is dropped. */
        unsigned left = s->dvd5_rx_len - s->dvd5_rx_pos;
        unsigned skip = 0;

        if (left + 6 > CUP2006_DVD5_RX_MAX) {
            skip = left + 6 - CUP2006_DVD5_RX_MAX;
            warn_report("cup2006: dvd5: %u unsolicited byte(s) dropped to fit"
                        " a reply", skip);
        }
        memmove(s->dvd5_rx, s->dvd5_rx + s->dvd5_rx_pos + skip, left - skip);
        memcpy(s->dvd5_rx + left - skip, reply, 6);
        s->dvd5_rx_len = left - skip + 6;
    } else {
        memcpy(s->dvd5_rx, reply, 6);
        s->dvd5_rx_len = 6;
    }
    s->dvd5_rx_pos = 0;
    s->dvd5_rx_unsol = false;
    cup2006_dvd5_irq_next(s);
    if (s->dvd5_replies <= 32 ||
        !(req[0] == 0xb8 && req[1] == 0x93)) {
        char reqstr[3 * 24 + 1];
        char rplstr[3 * 8 + 1];
        unsigned i, n;

        n = 0;
        for (i = 0; i < len && i < 24; i++) {
            n += snprintf(reqstr + n, sizeof(reqstr) - n, "%02x", req[i]);
        }
        n = 0;
        for (i = 0; i < 8; i++) {
            n += snprintf(rplstr + n, sizeof(rplstr) - n, "%02x", reply[i]);
        }
        trace_cup2006_dvd_stub_dvd5_exchange_req(s->dvd5_replies, reqstr, len, rplstr, pa, cup2006_guest_pc(), s->dvd5_reads);
    }
}

static void cup2006_dvd5_doorbell(Cup2006MachineState *s)
{
    uint8_t req[24];
    unsigned len;

    memcpy(req, s->dvd5_req, sizeof(req));
    len = cup2006_reg32_read(s->dvd5_req, 0x40, 4);
    if (len == 0 || len > sizeof(req) || req[0] < 0xb8 || req[0] > 0xbf) {
        trace_cup2006_dvd_dvd5_doorbell_without_frame(req[0], len);
        return;
    }
    cup2006_dvd5_reply(s, req, len);
}

static uint64_t cup2006_dvd5_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint32_t value = cup2006_reg32_read(s->dvd5_reg, addr, size);

    trace_cup2006_dvd_dvd5_read_pc_addr(cup2006_guest_pc(), CUP2006_DVD5_BASE + addr, size, value);
    return value;
}

void cup2006_adma_ack(Cup2006MachineState *s);

void cup2006_dvd5_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Cup2006MachineState *s = opaque;

    cup2006_reg32_write(s->dvd5_reg, addr, value, size);
    trace_cup2006_dvd_dvd5_write_pc_addr(cup2006_guest_pc(), CUP2006_DVD5_BASE + addr, size, value);
    if ((addr & ~3u) == CUP2006_DVD5_KICK &&
        (value & CUP2006_DVD5_KICK_VALUE)) {
        s->dvd5_kicks++;
        /* The DMA-done ISR writes this bit to clear the line. */
        if (s->dvd5_dma_src && s->dvd5_dma_src->asserted) {
            sh_intc_toggle_source(s->dvd5_dma_src, 0, -1);
        }
        /* STUB: a single engine start; per-request handshakes use the
         * shared window doorbell below. */
        if (s->dvd5_kicks <= 4) {
            trace_cup2006_dvd_stub_dvd5_engine_kick(s->dvd5_kicks, value);
        }
    }
    if ((addr & ~3u) == CUP2006_DVD5_KICK && (value & CUP2006_ADMA_BIT)) {
        cup2006_adma_ack(s);    /* the audio DMA ISR */
    }
}

static uint64_t cup2006_dvd5_win_read_common(Cup2006MachineState *s,
                                             uint8_t *buf, hwaddr off,
                                             unsigned size)
{
    return cup2006_reg32_read(buf, off, size);
}

static uint64_t cup2006_dvd5_req_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;

    return cup2006_dvd5_win_read_common(s, s->dvd5_req, addr, size);
}

static uint64_t cup2006_dvd5_rpl_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;

    return cup2006_dvd5_win_read_common(s, s->dvd5_rpl, addr, size);
}

static void cup2006_dvd5_req_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Cup2006MachineState *s = opaque;

    cup2006_reg32_write(s->dvd5_req, addr, value, size);
    /* The sender memcpy's the frame into this window, stores its length at
     * +0x40 and then rings the doorbell (reply window +0x44) as the last
     * step.  Answer once, on the doorbell only: triggering on the length
     * store as well made every frame get two replies. */
    if (addr == CUP2006_DVD5_DOORBELL &&
        (value & CUP2006_DVD5_DOORBELL_V)) {
        cup2006_dvd5_doorbell(s);
    }
}

static void cup2006_dvd5_rpl_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Cup2006MachineState *s = opaque;

    cup2006_reg32_write(s->dvd5_rpl, addr, value, size);
    if (addr == CUP2006_DVD5_DOORBELL &&
        (value & CUP2006_DVD5_DOORBELL_V)) {
        cup2006_dvd5_doorbell(s);
    }
}

static uint64_t cup2006_dvdst_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    uint32_t value;

    if ((addr & ~3u) == CUP2006_DVD5_ST_DATA && s->dvd5_rx_pos < s->dvd5_rx_len) {
        /* 0xFFA02008 is the 5line receive-data port; only the device's own
         * staged reply bytes are returned here, and the read consumes the
         * byte (a hardware receive register pops on read). */
        value = s->dvd5_rx[s->dvd5_rx_pos] >> (8 * (addr & 3));
        s->dvd5_rx_pos++;
    } else {
        value = cup2006_reg32_read(s->dvdst_reg, addr, size);
    }

    trace_cup2006_dvd_dvdst_read_pc_addr(cup2006_guest_pc(), CUP2006_DVDST_BASE + addr, size, value);
    return value;
}

static void cup2006_dvdst_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Cup2006MachineState *s = opaque;

    cup2006_reg32_write(s->dvdst_reg, addr, value, size);
    trace_cup2006_dvd_dvdst_write_pc_addr(cup2006_guest_pc(), CUP2006_DVDST_BASE + addr, size, value);
}

const MemoryRegionOps cup2006_dvd5_ops = {
    .read = cup2006_dvd5_read,
    .write = cup2006_dvd5_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_dvd5_win_ops = {
    .read = cup2006_dvd5_req_read,
    .write = cup2006_dvd5_req_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_dvd5_rpl_ops = {
    .read = cup2006_dvd5_rpl_read,
    .write = cup2006_dvd5_rpl_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps cup2006_dvdst_ops = {
    .read = cup2006_dvdst_read,
    .write = cup2006_dvdst_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
