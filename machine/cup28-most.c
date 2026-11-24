/*
 * Volvo VNS CUP28 board: MOST network (chip, vehicle peer, GPS and wheel reports).
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup28-internal.h"

/* CUP28 MOST control-port host and vehicle peer.
 * Evidence: mst.dll 010395a4 maps 07400000;
 * NK 80085144/80085190 drain one received byte through CPLD +50/+54 bit 0
 * and return SYSINTR 1c at the programmed transfer length. Hook index 5
 * is INTEVT 2a0. The guest reverses byte bits in 0103957c.
 * OS8104 data sheet pp.30-32: I2C 40/41, MAP auto-increment, page at FF.
 * STUB: acknowledged control-port bytes take 100 us. The peer models a
 * two-node ring with immediate PLL lock and link acknowledgement. */
#define CUP28_MOST_PEER_ADDR 0x0154
#define CUP28_MOST_SYNC_QUADLETS 14

static uint8_t cup28_most_reverse(uint8_t b)
{
    b = (b >> 4) | (b << 4);
    b = ((b & 0xcc) >> 2) | ((b & 0x33) << 2);
    return ((b & 0xaa) >> 1) | ((b & 0x55) << 1);
}

/* Drive the two CPLD sources independently. NK 80085b4c/80085b74
 * acknowledge bits 5 and 6, respectively, and both return SYSINTR 16. */
static void cup28_most_irq_line(struct intc_source *src, bool active)
{
    if (!src || active == !!src->asserted) {
        return;
    }
    src->prio = 1;
    src->enable_count = src->enable_max = 0;
    sh_intc_toggle_source(src, 0, active ? 1 : -1);
    if (active) {
        cpu_interrupt(first_cpu, CPU_INTERRUPT_HARD);
    }
}

void cup28_most_chip_irq(Cup28MachineState *s)
{
    if (!s->p28_most_chip_src) {
        return;
    }
    const uint8_t *r = s->p28_most_registers[0];
    bool control = s->p28_most_power_on || (r[0x86] & r[0x88] & 15);
    bool async = r[0xe3] & 0x1b;
    uint8_t *status = s->p28_dvd_control_reg + 0x70;
    uint32_t pending = ldl_le_p(status);
    uint32_t enable = ldl_le_p(s->p28_dvd_control_reg + 0x104);

    /* A low pin remains visible to driver polls even when its interrupt is
     * masked by the OAL. Control INT is OS8104 §13.2, AINT is §15.2. */
    stl_le_p(status, (pending & ~0x60) | (control ? 0x20 : 0) |
            (async ? 0x40 : 0));
    cup28_most_irq_line(s->p28_most_chip_src, control && (enable & 0xf0000));
    cup28_most_irq_line(s->p28_most_async_src, async && (enable & 0xf00000));
}

static void cup28_most_write_rcmb(Cup28MachineState *s, const uint8_t *message,
                                  size_t length)
{
    uint8_t *r = s->p28_most_registers[0];
    uint8_t *rcmb = r + 0xa0;

    memset(rcmb, 0, 0x14);
    rcmb[1] = CUP28_MOST_PEER_ADDR >> 8;
    rcmb[2] = CUP28_MOST_PEER_ADDR & 0xff;
    memcpy(rcmb + 3, message, length);
    r[0x86] |= 0x81; /* bMSGS.RBS and MRX */
}

/* Move held-back peer messages into the node FIFO (or straight into mRCMB)
 * while there is room. */
static void cup28_most_flush_peer_q(Cup28MachineState *s)
{
    uint8_t *r = s->p28_most_registers[0];

    while (s->p28_most_peer_q_count) {
        unsigned i = s->p28_most_peer_q_head;

        if (!(r[0x86] & 0x80) && !s->p28_most_q_count) {
            cup28_most_write_rcmb(s, s->p28_most_peer_q[i],
                                  s->p28_most_peer_q_len[i]);
        } else if (s->p28_most_q_count < G_N_ELEMENTS(s->p28_most_q)) {
            unsigned j = (s->p28_most_q_head + s->p28_most_q_count++) %
                         G_N_ELEMENTS(s->p28_most_q);
            memcpy(s->p28_most_q[j], s->p28_most_peer_q[i],
                   s->p28_most_peer_q_len[i]);
            s->p28_most_q_len[j] = s->p28_most_peer_q_len[i];
        } else {
            break;
        }
        s->p28_most_peer_q_head = (i + 1) % G_N_ELEMENTS(s->p28_most_peer_q);
        s->p28_most_peer_q_count--;
    }
}

/* STUB: the vehicle peer retransmits while the node's single receive buffer
 * (mRCMB, OS8104 13.2.6) is still locked, modelled as a small FIFO that is
 * drained one message per RBE unlock.  When that FIFO is full the peer holds
 * its message back in p28_most_peer_q instead of dropping it. */
static void cup28_most_receive(Cup28MachineState *s, const uint8_t *message,
                               size_t length)
{
    uint8_t *r = s->p28_most_registers[0];

    cup28_most_flush_peer_q(s);
    if (length > sizeof(s->p28_most_q[0])) {
        length = sizeof(s->p28_most_q[0]);
    }
    if (!(r[0x86] & 0x80) && !s->p28_most_q_count) {
        cup28_most_write_rcmb(s, message, length);
        return;
    }
    if (s->p28_most_q_count < G_N_ELEMENTS(s->p28_most_q)) {
        unsigned i = (s->p28_most_q_head + s->p28_most_q_count++) %
                     G_N_ELEMENTS(s->p28_most_q);
        memcpy(s->p28_most_q[i], message, length);
        s->p28_most_q_len[i] = length;
    } else if (s->p28_most_peer_q_count < G_N_ELEMENTS(s->p28_most_peer_q)) {
        unsigned i = (s->p28_most_peer_q_head + s->p28_most_peer_q_count++) %
                     G_N_ELEMENTS(s->p28_most_peer_q);
        memcpy(s->p28_most_peer_q[i], message, length);
        s->p28_most_peer_q_len[i] = length;
    } else {
        trace_cup28_most_peer_hold_back_queue();
    }
}

/* Send one control message as segmented telegrams (TelID 1/2/3, 11 data
 * bytes each, message counter in Data0), function 0xE0<func_lo>, OpType
 * Status.  Same wire shape as the verified 52/E00 send. */
static void cup28_most_gps_msg(Cup28MachineState *s, uint8_t func_lo,
                               const uint8_t *rec, unsigned len)
{
    uint8_t msg[17];
    unsigned sent = 0, seg = 0;

    while (sent < len) {
        unsigned n = MIN(11u, len - sent);
        unsigned id = !sent ? 1 : (sent + n >= len ? 3 : 2);

        msg[0] = 0x52; msg[1] = 1;
        msg[2] = 0xe0; msg[3] = (func_lo << 4) | 0x0c;
        msg[4] = (id << 4) | (n + 1);
        msg[5] = seg++;
        memcpy(msg + 6, rec + sent, n);
        cup28_most_receive(s, msg, 6 + n);
        sent += n;
    }
}

static bool cup28_most_dequeue(Cup28MachineState *s)
{
    unsigned i;

    if (!s->p28_most_q_count || (s->p28_most_registers[0][0x86] & 0x80)) {
        return false;
    }
    i = s->p28_most_q_head;
    s->p28_most_q_head = (i + 1) % G_N_ELEMENTS(s->p28_most_q);
    s->p28_most_q_count--;
    cup28_most_write_rcmb(s, s->p28_most_q[i], s->p28_most_q_len[i]);
    cup28_most_flush_peer_q(s);
    return true;
}

static void cup28_most_vc_deliver(Cup28MachineState *s)
{
    /* STUB: a fixed vehicle peer announces application mode, language and
     * region through the OS8104 receive control buffer (§13.2.6).
     * Status (0xC) handlers in mst.dll: 02/A00 -> 0103814c (mode).
     * The F5 reports supply the vehicle settings read by HMIManager
     * 0003b810, with a valid vehicle and optional receivers disabled. */
    static const uint8_t messages[][6] = {
        { 0x02, 0, 0xa0, 0x0c, 1, 1 },
        { 0xf5, 0, 0x2b, 0x1c, 1, 2 }, /* navigation vehicle valid */
        { 0xf5, 0, 0x2b, 0x2c, 1, 0x06 }, /* North America, Volvo */
        { 0xf5, 0, 0x2b, 0xbc, 1, 1 }, /* GPS receiver setting (guest logs gpsReceiver=1) */
        { 0xf5, 0, 0x2b, 0xcc, 1, 1 }, /* TMC receiver absent */
        { 0xf5, 0, 0x2b, 0xec, 1, 1 }, /* voice vehicle valid */
        { 0xf5, 0, 0x2b, 0xfc, 1, 1 }, /* voice control absent */
        { 0xf5, 0, 0x2c, 0x1c, 1, 0x11 }, /* North American English */
        /* DeviceMode Normal (F3/200 SetGet, mst.dll 010458fc -> event
         * 0x64; HMIManager "NMOSTDeviceModeEvent 0x00:Normal" powers the
         * DVD), then ApplicationMode Active for NAVI (F1 instance 4,
         * F1/201 SetGet, mst.dll 0103ea38 -> event 0x4d; modes 0 Active,
         * 1 Limited, 2 Passive, 3 Shutdown per HMIManager's log table;
         * SetMonitorOnOff(1)). Without these HMI never logs a device or
         * application mode. */
        { 0xf3, 0, 0x20, 0x02, 1, 0 },
        { 0xf1, 4, 0x20, 0x12, 1, 0 },
        /* F1/101 StartResult, source 1. MST 0103ddfc posts event
         * 0x4f and ClassLib 01542fb8 requests synchronous channels. */
        { 0xf1, 4, 0x10, 0x12, 1, 1 },
    };
    unsigned message = s->p28_most_vc_sent;
    if (message >= G_N_ELEMENTS(messages)) {
        return;
    }
    cup28_most_receive(s, messages[message], sizeof(messages[message]));
    s->p28_most_vc_sent++;
    s->p28_most_vc_complete = s->p28_most_vc_sent == G_N_ELEMENTS(messages);
}

/* STUB: the vehicle peer reports its wrapping wheel counter at the gyro
 * sampling cadence. F5/201 Status -> mst.dll 0x0103fa2c -> event 0x6f.
 * ClassLib 0x015445c0 queues byte 1 and 0x0153be94 takes modulo-256 deltas. */
void cup28_most_wheel_send(Cup28MachineState *s)
{
    uint8_t message[] = { 0xf5, 0, 0x20, 0x1c, 1,
                          s->p28_most_wheel_counter };

    cup28_most_receive(s, message, sizeof(message));
    cup28_most_chip_irq(s);
}


static uint8_t cup28_most_chip_read(Cup28MachineState *s, uint8_t map)
{
    uint8_t xcr = s->p28_most_registers[0][0x80];
    bool active = xcr & 2;
    bool master = xcr & 0x80;
    unsigned delay = active && !master && !(xcr & 4);

    if (map == 0xff) {
        return s->p28_most_page;
    }
    if (!s->p28_most_page) {
        /* STUB: one peer supplies the opposite timing role to this
         * node, with an otherwise empty ring before vehicle configuration.
         * OS8104 pp.37-42: bypassed nodes do not count; the first slave is
         * NPR=1, MPR counts active nodes, SBY suppresses node delay. The
         * master supplies fourteen synchronous quadlets (p.41). Instant
         * lock/network enumeration replaces propagation and PLL timing. */
        switch (map) {
        case 0x80: return xcr & ~0x10; /* LPW is write-only */
        case 0x81: return s->p28_most_registers[0][map] & 0x70;
        case 0x87: return active && !master ? 1 : 0;
        /* OS8104 p.90: CM2.NAC reports RX activity independently of ABY;
         * LOK=0 means the selected clock source is locked. */
        case 0x8e: return (s->p28_most_registers[0][map] & 0x30) | 0x40;
        case 0x8f: return delay;
        case 0x90: return active ? 2 : 1;
        case 0x91: return delay;
        case 0x96:
            if (!(xcr & 0x80)) {
                return CUP28_MOST_SYNC_QUADLETS;
            }
            break;
        }
    }
    return s->p28_most_registers[s->p28_most_page][map];
}

/* STUB: the vehicle timing master completes allocation synchronously.
 * OS8104 tables 13-18..21 and 14-1: mXCMB holds the reply and mCRA
 * records the first channel as the label for every granted byte. */
static void cup28_most_sync_resource(Cup28MachineState *s, bool allocate)
{
    uint8_t *r = s->p28_most_registers[0];
    uint8_t *channels = s->p28_most_registers[3] + 0x80;
    unsigned count = CUP28_MOST_SYNC_QUADLETS * 4;
    unsigned request = r[0xc5];
    unsigned free_channels = 0;

    r[0xc7] = 1;
    r[0xc8] = 0;
    if (!allocate) {
        if (request > 0x7f) {
            r[0xc7] = 4;
        } else {
            for (unsigned i = 0; i < count; i++) {
                if (request == 0x7f || channels[i] == request) {
                    channels[i] = 0x70;
                }
            }
        }
    } else {
        for (unsigned i = 0; i < count; i++) {
            free_channels += channels[i] == 0x70;
        }
        memset(r + 0xc9, 0xff, 8);
        if (request == 0 || request > 8) {
            r[0xc7] = 4;
        } else if (request > free_channels) {
            r[0xc7] = 3;
        } else {
            for (unsigned i = 0, granted = 0; i < count && granted < request; i++) {
                if (channels[i] == 0x70) {
                    r[0xc9 + granted++] = i;
                    channels[i] = r[0xc9];
                }
            }
            free_channels -= request;
        }
        r[0xc8] = free_channels;
    }
}

static void cup28_most_chip_write(Cup28MachineState *s, uint8_t map, uint8_t byte)
{
    uint8_t *r = s->p28_most_registers[0];
    if (s->p28_most_page) {
        s->p28_most_registers[s->p28_most_page][map] = byte;
        return;
    }
    /* OS8104 pp.115-120: command strobes, status acknowledgements and
     * unique-address validation are transceiver operations, not RAM. */
    if (map == 0x85) {
        unsigned target = (r[0xc2] << 8) | r[0xc3];
        bool peer = target == CUP28_MOST_PEER_ADDR;

        r[0x86] &= ~(byte & 15);
        if (byte & 4) {
            s->p28_most_power_on = false;
        }
        if (byte & 0x40) { /* RBE: unlock mRCMB */
            bool received = r[0x86] & 0x80;
            r[0x86] &= ~0x80;
            if (!cup28_most_dequeue(s) && received &&
                s->p28_most_vc_sent > 1) {
                cup28_most_vc_deliver(s);
            }
        }
        if ((byte & 0x10)) {
            /* STUB: the peer is the only other registered node. */
            bool unique = !peer;
            r[0x86] = (r[0x86] & ~0x40) | (unique ? 0x40 : 0);
            if (unique) {
                r[0x8a] = r[0xc2];
                r[0x8b] = r[0xc3];
            }
        }
        if ((byte & 0x80)) {
            /* STUB: immediate link acknowledgement by the fixed peer for
             * its logical address or broadcast. The vehicle peer announces its
             * configuration on an F5/001 Set subscription. */
            unsigned type = r[0xc1] & 0x7f;
            bool resource = type == 3 || type == 4;
            bool master = target == 0x0400 || peer;
            bool ack = resource ? master : r[0xc2] == 0xff || peer;
            r[0xd5] = ack ? 0x10 : 0;
            if (resource && master) {
                cup28_most_sync_resource(s, type == 3);
            }
            /* F5/001 Set subscription to vehicle property 0x0201.
             * The guest sends this one-property request on startup. */
            if (type == 0 && peer && s->track &&
                !s->p28_most_wheel_on && r[0xc4] == 0xf5 && r[0xc5] == 1 &&
                r[0xc6] == 0 && r[0xc7] == 0x10 && r[0xc8] == 5 &&
                r[0xc9] == 1 && r[0xcc] == 2 && r[0xcd] == 1) {
                s->p28_most_wheel_on = true;
            }
            if (type == 0 && peer &&
                r[0xc4] == 0xf0 && r[0xc6] == 0x40 && r[0xc7] == 0x02 &&
                r[0xc8] >= 2) {
                /* STUB: the vehicle peer acts as resource master and grants
                 * F0/400 StartResult (navi's SendResourceAlloc) with a
                 * Result: source byte echoed, one channel (the requested
                 * count byte), empty second list. mst.dll 010460c4 parses
                 * [src, u16 n1, n1 bytes, u16 n2, n2 bytes] into event 0xbe,
                 * which ClassLib 0154367c records and uses to signal
                 * aloc_result_event. */
                /* MST 01036ed4 shifts the first count byte left by 8. */
                uint8_t res[] = { 0xf0, r[0xc5], 0x40, 0x0c, 6,
                                  r[0xc9], 0, 1, r[0xca], 0, 0 };
                cup28_most_receive(s, res, sizeof(res));
            }
            if (type == 0 && peer && r[0xc4] == 0x02 &&
                r[0xc6] == 0xa0 && r[0xc7] == 0x11 && r[0xc8] >= 2) {
                /* STUB: the vehicle peer acts as NetworkMaster and answers
                 * CentralRegistry (02/A01) Get for the requested FBlock and
                 * instance with Status [node address, FBlock, instance],
                 * placing that FBlock on the peer itself.  The guest asks
                 * for FBlock 0x41 instance 1 (MOST tx); ClassLib logs
                 * "ParseCentralRegistry id=%02x add=%03x" and needs it for
                 * "DevWrite ... GPS_NordAddr". */
                uint8_t res[] = { 0x02, r[0xc5], 0xa0, 0x1c, 4,
                                  CUP28_MOST_PEER_ADDR >> 8,
                                  CUP28_MOST_PEER_ADDR & 0xff,
                                  r[0xc9], r[0xca] };
                cup28_most_receive(s, res, sizeof(res));
                /* Also list the NavigationSystem FBlocks 0x50/0x51 that
                 * mst.dll 0x102d268 needs ("MST_Write: NavigationSystem Not
                 * Exist" while their node address is 0), one entry per
                 * Status as for the requested FBlock. */
                if (!s->p28_most_navsys_listed) {
                    for (unsigned fb = 0x50; fb <= 0x51; fb++) {
                        uint8_t nav[] = { 0x02, r[0xc5], 0xa0, 0x1c, 4,
                                          CUP28_MOST_PEER_ADDR >> 8,
                                          CUP28_MOST_PEER_ADDR & 0xff,
                                          fb, 1 };
                        cup28_most_receive(s, nav, sizeof(nav));
                    }
                    s->p28_most_navsys_listed = true;
                    /* The vehicle peer's GPS receiver (FBlock 0x52) starts
                     * reporting once the registry is known. */
                    s->p28_most_gps_on = true;
                }
            }
            if (type == 0 && peer &&
                r[0xc4] == 0xf0 && r[0xc6] == 0x41 &&
                ((r[0xc7] == 0x02 && r[0xc8] == 4) ||
                 (r[0xc7] == 0x12 && r[0xc8] == 1))) {
                /* STUB: connect/disconnect the vehicle amplifier sink.
                 * F0/410 Result has no parameters (MST 0104661c). */
                uint8_t res[] = { 0xf0, r[0xc5], 0x41,
                                  (r[0xc7] & 0xf0) | 0x0c, 0 };
                cup28_most_receive(s, res, sizeof(res));
            }
            if (type == 0 && peer &&
                r[0xc4] == 0xf0 && r[0xc6] == 0x40 && r[0xc7] == 0x32) {
                /* STUB: F0/403 (navi's SendResourceDeAlloc) granted with an
                 * empty Result; mst.dll 01046318 accepts only length 0. */
                uint8_t res[] = { 0xf0, r[0xc5], 0x40, 0x3c, 0 };
                cup28_most_receive(s, res, sizeof(res));
            }
            r[0x86] = (r[0x86] & ~0x40) | 2 | (ack ? 0x40 : 0);
            if (type == 0 && peer && s->p28_most_vc_sent == 1 &&
                !(r[0x86] & 0x80) &&
                r[0xc4] == 0xf5 && r[0xc6] == 0 && r[0xc7] == 0x10) {
                cup28_most_vc_deliver(s);
            }
        }
    } else if (map == 0xE2) {
        /* bPCTC command strobes (OS8104 Table 15-6), cleared automatically. */
        if (byte & 0x01) { /* RARX: clear ARX and unlock mARP */
            r[0xe3] &= ~1;
        }
        if (byte & 0x02) r[0xE3] &= ~0x02; /* RATX */
        if (byte & 0x08) r[0xE3] &= ~0x08; /* RAC */
        if (byte & 0x10) r[0xE3] &= ~0x10; /* RAF */
    } else if (map != 0x86 && map != 0x87 && map != 0x8f &&
               map != 0x90 && map != 0x91 && map != 0xd5) {
        if (map == 0x80 && ((r[map] ^ byte) & 6)) {
            r[0x86] |= 8; /* membership/delay change: bMSGS.ALC */
        }
        r[map] = byte;
        /* STUB: the peer announces application mode when the node
         * joins the ring and registers its logical address. */
        if (map == 0x8b && (r[0x80] & 2) && r[0x8a] &&
            byte != 0xfe && s->p28_most_vc_sent == 0) {
            cup28_most_vc_deliver(s);
        }
    }
    cup28_most_chip_irq(s);
}

void cup28_most_irq(Cup28MachineState *s)
{
    struct intc_source *src = s->p28_most_src;
    bool active = (s->p28_most_control & 0x38) &&
        (ldl_le_p(s->p28_dvd_control_reg + 0x50) &
         ldl_le_p(s->p28_dvd_control_reg + 0x54) & 1);
    cup28_most_irq_line(src, active);
}

static void cup28_most_tick(void *opaque)
{
    Cup28MachineState *s = opaque;
    if (!s->p28_most_shift_pending || !(s->p28_most_control & 0x38)) {
        return;
    }
    s->p28_most_shift_pending = false;
    s->p28_most_rx_ready = true;
    s->p28_most_rx = s->p28_most_next_rx;
    stl_le_p(s->p28_dvd_control_reg + 0x50,
             ldl_le_p(s->p28_dvd_control_reg + 0x50) | 1);
    cup28_most_irq(s);
}

static void cup28_most_schedule(Cup28MachineState *s)
{
    if (s->p28_most_timer && s->p28_most_shift_pending &&
        (s->p28_most_control & 0x38)) {
        /* STUB: about one acknowledged byte at a 100-kHz I2C clock.
         * Completion must yield the CPU to threads waiting on the bus. */
        timer_mod(s->p28_most_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

void cup28_most_gpio(Cup28MachineState *s, uint32_t old, uint32_t value)
{
    /* The guest's repeated-start flag 70 toggles GPIO bit 4 high then
     * low between MAP and read address (NK 80085190). */
    if ((s->p28_most_control & 0x38) && (old & 0x10) && !(value & 0x10)) {
        s->p28_most_phase = 0;
    }
}

static uint64_t cup28_most_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    /* NK 800827b4 polls bit 1 before reading a completed byte from +8. */
    return addr == 0 ? s->p28_most_control | (s->p28_most_rx_ready ? 2 : 0) :
           addr == 8 ? s->p28_most_rx : 0;
}

static void cup28_most_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Cup28MachineState *s = opaque;
    if (addr == 0) {
        s->p28_most_control = value;
        if (!(value & 0x38)) {
            s->p28_most_phase = 0;
            s->p28_most_shift_pending = false;
            s->p28_most_rx_ready = false;
        }
    } else if (addr == 0xc) {
        unsigned byte = cup28_most_reverse(value), result = 0;
        if (!s->p28_most_phase) {
            s->p28_most_reading = byte & 1;
            s->p28_most_phase = s->p28_most_reading ? 2 : 1;
            if ((byte & 0xfe) != 0x40) {
                result = 0x100; /* no slave at another I2C address */
                s->p28_most_phase = 0;
            }
        } else if (s->p28_most_phase == 1) {
            s->p28_most_map = byte;
            s->p28_most_phase = 2;
        } else {
            uint8_t map = s->p28_most_map++;
            if (s->p28_most_reading) {
                result = cup28_most_reverse(cup28_most_chip_read(s, map));
            } else if (map == 0xff) {
                s->p28_most_page = byte & 3;
            } else {
                cup28_most_chip_write(s, map, byte);
            }
        }
        s->p28_most_next_rx = result;
        s->p28_most_rx_ready = false;
        s->p28_most_shift_pending = true;
    }
    cup28_most_schedule(s);
    cup28_most_irq(s);
}

static const MemoryRegionOps cup28_most_ops = {
    .read = cup28_most_read, .write = cup28_most_write,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* mst.dll's F1/410 Set callback at 0x0103dbb8 takes an index and state. */
static void cup28_most_key_input(DeviceState *dev, QemuConsole *src,
                                 QemuInputEvent *evt)
{
    Cup28MachineState *s = CUP28_MACHINE(dev);
    int code = qemu_input_linux_to_qcode(evt->key.key);

    /* Wheel cursor buttons (F1/410 index): 0 ENTER, 1 up, 2 down, 3 left,
     * 4 right, 5 BACK. */
    int index;

    switch (code) {
    case Q_KEY_CODE_RET:   index = 0; break;
    case Q_KEY_CODE_UP:    index = 1; break;
    case Q_KEY_CODE_DOWN:  index = 2; break;
    case Q_KEY_CODE_LEFT:  index = 3; break;
    case Q_KEY_CODE_RIGHT: index = 4; break;
    case Q_KEY_CODE_ESC:   index = 5; break;
    default:
        return;
    }
    uint8_t message[] = { 0xf1, 0, 0x41, 0x00, 2, index, evt->key.down };
    cup28_most_receive(s, message, sizeof(message));
    cup28_most_chip_irq(s);
}

static const QemuInputHandler cup28_most_key_handler = {
    .name = "CUP28 MOST remote",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = cup28_most_key_input,
};

/*
 * Bench GPS receiver on MOST: FBlock 0x52, function 0xE00 (GPS fix).
 *
 * Evidence:
 *  - With gpsReceiver=1 the guest asks the NetworkMaster CentralRegistry
 *    (02/A01 Get) for FBlocks 0x41 (TMC tuner, mst.dll "MST_Write: TMCTuner
 *    Not Exist" 0x010294ac) and 0x52, then registers notifications on
 *    52/E00 and 52/E01.
 *  - mst.dll dispatches FBlock -> function table -> op table
 *    (0x01036c20).  In device.exe memory the table {0x001, 0xE00, 0xE01}
 *    maps E00 Status to 0x01041554 and E01 Status to 0x010417d4
 *  - 0x01041554 requires a 26-byte big-endian payload: int32 longitude
 *    then latitude in semicircles, fix type, int16 fields bounded <= 1000,
 *    <= 1000, speed, heading <= 3600 (0.1 deg), altitude -999..9999,
 *    year, month, day, hour, minute, second; posts event 0x96 to ClassLib
 *    0x1547bf4 -> NGPSData::ParseMOSTData 0x155e988 (fix 3 = valid 2D,
 *    4 = 3D; others invalid). Longitude first is verified at map-panel
 *    current-position handler 0x002afb50.
 *  - 26 bytes do not fit one control telegram: segmented TelID 1/2/3 with
 *    a message counter in Data0, 11 bytes per telegram, reassembles to
 *    length 0x1a (0x01041554).
 *  - 52/E01 (satellite data) is handled by mst.dll 0x010417d4: the
 *    reassembled payload has a u16 length (+0xa) and starts at +0xc with a
 *    marker byte.  Marker 1..12 with length 6 updates one satellite entry
 *    (marker, pad, one big-endian 4-byte entry); marker 0 with length
 *    0x32 carries all 12 entries.  The 0x34-byte record is posted to
 *    ClassLib 0x01547c7c (entry stored at slot marker-1) ->
 *    NGPSData::ParseMOSTSatData 0x0155eb4c, which decodes each entry as
 *    u32: bits 0-5 PRN (0 if > 0x20), bits 6-7 status (2 = used in the
 *    fix), bits 8-14 azimuth, bits 15-23 elevation ((u32>>15) & 0x1ff),
 *    bits 24-30 SNR; it counts status==2 entries into NGPSData+0x28 and
 *    raises NSatelliteInfo/NumReplyEvent to navi.
 *
 * STUB: a vehicle receiver that reports fix status once per second from the
 * gpstrack replay when set, else a fixed position in San Francisco (on the
 * "NA_0900_06A_WESTERN" disc).  Accuracy fields are fixed.  The 52/E01
 * constellation is a fixed 8-satellite pattern (6 used), not computed from
 * the track position. The list is cleared during an outage. Time of day
 * starts at 12:00 virtual time.
 */
/* 52/E01 Status: the full list (marker 0, pad, twelve 4-byte entries)
 * fits in five telegrams. Sending twelve separate entries would need
 * 24 telegrams and overrun the vehicle peer's receive FIFO.
 * mst.dll 0x010417d4 accepts the 0x32-byte list and ClassLib
 * ParseMOSTSatData 0x0155eb4c decodes the entries as u32:
 * bits 0-5 PRN (0 if > 0x20), 6-7 status (2 = used in fix), 8-14 azimuth,
 * 15-23 elevation, 24-30 SNR. */
static void cup28_most_gps_sat_send(Cup28MachineState *s, bool valid)
{
    /* STUB: fixed constellation; not derived from the track. */
    const unsigned nsat = valid ? 8 : 0;
    const unsigned nused = valid ? 6 : 0;

    uint8_t rec[2 + 12 * 4] = { 0, nsat };

    for (unsigned i = 0; i < nsat; i++) {
        unsigned prn = i + 1;
        unsigned status = i < nused ? 2 : 1;
        unsigned az = i * 45 & 0x7f;
        unsigned elev = 20 + i * 5;
        uint32_t entry = prn | (status << 6) | (az << 8) |
                         (elev << 15) | (35u << 24);

        /* MST 0x010417d4 uses the same BE32 reader as the E00 position. */
        stl_be_p(rec + 2 + i * 4, entry);
    }
    cup28_most_gps_msg(s, 0x01, rec, sizeof(rec));
}

/* Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm). */
static void cup28_civil_from_days(int64_t z, int *y, int *m, int *d)
{
    int64_t era, yy;
    unsigned doe, yoe, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = (int64_t)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static bool cup28_most_gps_send(Cup28MachineState *s)
{
    uint8_t rec[26] = { 0 };
    double lat = 37.774900, lon = -122.419400, kmh = 0, hdg = 0, alt = 20;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    const MelcoSample *sample = s->track ?
                                melco_track_sample(s->track, now) : NULL;
    int64_t t = now / 1000;

    bool valid = !sample || sample->fix;

    if (sample && valid) {
        lat = sample->lat_deg;
        lon = sample->lon_deg;
        alt = sample->alt_m;
        kmh = sample->speed_kmh;
        hdg = sample->heading_deg;
    }
    if (valid) {
        stl_be_p(rec + 0, MIN(llround(lon * 2147483648.0 / 180.0), INT32_MAX));
        stl_be_p(rec + 4, (int32_t)llround(lat * 2147483648.0 / 180.0));
        memcpy(s->p28_most_gps_last_position, rec, 8);
    } else {
        /* STUB: retain the last reported coordinates when the fix is lost. */
        memcpy(rec, s->p28_most_gps_last_position, 8);
        alt = 0;
    }
    /* NGPSData::ParseMOSTData (0x155e988): 3 is 2D, 1 is invalid. */
    rec[8] = valid ? 3 : 1;
    stw_be_p(rec + 9, 10);               /* accuracy fields (<=1000) */
    stw_be_p(rec + 11, 10);
    stw_be_p(rec + 13, (int)(kmh * 10)); /* speed (unit assumed 0.1 km/h) */
    stw_be_p(rec + 15, (int)(hdg * 10) % 3600);
    stw_be_p(rec + 17, (int)alt);
    /* STUB: the receiver reports a made-up clock: a fixed start date,
     * 2026-11-22 12:00:00, advancing monotonically with virtual time
     * (it never jumps back at 24 h). */
    {
        int64_t tod = t + 12 * 3600;    /* the fixed clock starts at 12:00 */
        int64_t days = tod / 86400, secs = tod % 86400;
        int y, m, d;

        cup28_civil_from_days(20779 + days, &y, &m, &d); /* 2026-11-22 */
        stw_be_p(rec + 19, y);
        rec[21] = m; rec[22] = d;
        rec[23] = secs / 3600; rec[24] = (secs / 60) % 60; rec[25] = secs % 60;
    }
    cup28_most_gps_msg(s, 0x00, rec, sizeof(rec));
    if (s->p28_most_gps_log++ < 3 || valid != s->p28_most_gps_valid) {
        /* Trace events take no floats: degrees become micro-degrees. */
        trace_cup28_most_gps_e00_ms_lat(valid ? "fix" : "no fix", now, (int32_t)lrint((int32_t)ldl_be_p(rec + 4) *
                                   (180.0 / 2147483648.0) * 1e6), (int32_t)lrint((int32_t)ldl_be_p(rec) *
                                   (180.0 / 2147483648.0) * 1e6));
    }
    s->p28_most_gps_valid = valid;
    return valid;
}

static void cup28_most_gps_tick(void *opaque)
{
    Cup28MachineState *s = opaque;

    if (s->p28_most_gps_on) {
        bool valid = cup28_most_gps_send(s);
        cup28_most_gps_sat_send(s, valid);
    }
    timer_mod(s->p28_most_gps_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000LL);
}

void cup28_most_init(Cup28MachineState *s, MemoryRegion *mem, SuperHCPU *cpu)
{
    struct intc_desc *desc = cpu->env.intc_handle;
    for (unsigned i = 0; desc && i < desc->nr_sources; i++) {
        if (desc->sources[i].vect == 0x680) {
            s->p28_most_src = &desc->sources[i];
            s->p28_most_src->vect = 0x2a0;
        } else if (desc->sources[i].vect == 0x6a0) {
            s->p28_most_chip_src = &desc->sources[i];
            s->p28_most_chip_src->vect = 0x380;
        } else if (desc->sources[i].vect == 0x3a0) {
            s->p28_most_async_src = &desc->sources[i];
        }
    }
    s->p28_most_registers[0][0x80] = 0x20; /* bXCR reset, manual p.37 */
    s->p28_most_registers[0][0x81] = 0x50; /* bXSR reset, manual p.38 */
    s->p28_most_registers[0][0x86] = 4; /* power-on complete, manual p.117 */
    memset(s->p28_most_registers[3] + 0x80, 0x70, 60); /* mCRA reset, p.131 */
    s->p28_most_power_on = true;
    s->p28_most_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cup28_most_tick, s);
    s->p28_most_gps_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         cup28_most_gps_tick, s);
    timer_mod(s->p28_most_gps_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000LL);
    memory_region_init_io(&s->p28_most_io, NULL, &cup28_most_ops, s,
                          "cup28-most-control-host", 0x20);
    memory_region_add_subregion_overlap(mem, 0x07400000, &s->p28_most_io, 2);
    cup28_most_chip_irq(s);
    s->most_input = qemu_input_handler_register((DeviceState *)s,
                                               &cup28_most_key_handler);
}
