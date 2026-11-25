/*
 * Volvo RTI MMM+ (CU-P2006) head unit - the machine model.
 *
 * The machine boots the WinCE 4.2 SH4 "NK" image carved from LOADING.KWI
 * (CUP2006.SHx.ORG) with -kernel: the image is linked at P1 0x80100000
 * (physical 0x00100000), its B000FF entry is 0x80100004, and the CPU
 * starts at the P2 alias of that entry (0xa0100004).
 *
 * On top of RAM, the SH7750 on-chip peripherals and a serial console, the
 * board devices live in the cup2006-*.c files of the same directory (DU display,
 * 2DG/MBX graphics, Sub-CPU link, ATA HDD, DVD drive, GPS, ...).  A
 * permissive catch-all covers board registers not modelled yet: it logs
 * (pc, addr, size, value) with LOG_GUEST_ERROR and returns zero.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cup2006-internal.h"
#include "cup2006-socregs.h"


/* QEMU 11 replaced cpu_get_phys_page_debug: same result, -1 if unmapped. */
hwaddr cup2006_phys_page_debug(CPUState *cpu, vaddr addr)
{
    TranslateForDebugResult r;

    return cpu_translate_for_debug(cpu, addr, &r) ? r.physaddr : (hwaddr)-1;
}

#define CUP2006_RAM_BASE        0x00000000




/* The flattened NK image loads at physical 0x00100000. */
#define CUP2006_NK_ADDR         0x00100000
#define CUP2006_NK_RESET_PC     0xa0100004


/* Battery-backed SRAM at the top of DRAM (256 MiB RAM: physical 0x0fff0000).
 * sramsave=<path> persists up to 64 KiB there. */


/* DRF ("DRF1:") flash window, physical 0..0x02000000: one 32 MiB flash part
 * (melco-amdflash, 32-bit interleaved pair); drfimage= is its backing file. */
#define CUP2006_DRF_BASE        0x00000000  /* physical, full window */
#define CUP2006_DRF_SIZE        0x02000000  /* 32 MiB guest window */
#define CUP2006_DRF_SECTOR      0x00040000
/* Autoselect IDs the DRF part answers with.  STUB: the mark of the board's
 * own part is not known (no guest in the boot traces issues AA 55 90 here),
 * so the values are CUP28's until a dump of the part replaces them. */
#define CUP2006_DRF_ID_MANUFACTURER 0x0001
#define CUP2006_DRF_ID_DEVICE       0x227e

/* Board register block at 0xff800200 (area-7 alias 0x1f800200), mapped at
 * higher priority than the sh7750 0xff800000 alias so the catch-all sees it;
 * separate regions so the log names the address the guest used. */
#define CUP2006_BOARD_REG_BASE  0xff800200
#define CUP2006_BOARD_A7_BASE   0x1f800200
#define CUP2006_BOARD_REG_SIZE  0x00000e00

/* Board A/D converter at 0xFF93C000 (alias 0x1F93C000, cup2006-board.c
 * "ADC"): ADDRA..ADDRD at +0x0..+0x6, ADCSR at +0x10; mapped by selftest=. */
#define CUP2006_SELFTEST_A7_BASE 0x1f93c000  /* area-7 alias: gps.dll's mapping */
#define CUP2006_SELFTEST_SIZE   0x00001000

/* Single-byte I2C/PLL controller at 0xFF907000 (alias 0x1F907000): ctrl
 * +0x04, status +0x0c, div +0x18, TX +0x20, RX +0x24. STUB: no tuner is
 * modelled; every transfer ACKs and RX reads 0. */
#define CUP2006_I2C_SIZE          0x00001000
/* One byte at 100 kHz (9 bits incl. ACK) ~= 90 us: the I2C standard-mode
 * byte time (CUP2006_I2C_XFER_NS). */

/* GPS receiver latch at 0xFF93D000 (alias 0x1F93D000), a board register
 * undocumented in the SH7751/SH7750 manual: command +0x04, status +0x0c.
 * STUB: the status always reports ready (bits 1:0 = 0b01). */
/* GPS tracking chip (with gpstrack=<csv>): a periodic tick posts a message
 * to the mailbox at 0xFF93D000 (+0x800) and raises INTEVT 0x8A0.  The
 * measurements come from the gnssreplay= recording; without one the chip
 * reports no signal.  gpslib computes every fix itself. */
#define CUP2006_GPS_INTC_VECT    0x0a40   /* sh7750 PCIC1_PCIDMA2, unused */
#define CUP2006_GPS_INTC_PRIO    4

/* Remote-control (RCD) block at 0xFF937000 (alias 0x1F937000): 32-bit ctrl
 * +0x00, ack/status +0x08, 16-bit received record +0x2C. STUB: writes are
 * latched, +0x2C reads 0, no interrupt is raised. */

/* Board power/ACC interrupt INTEVT 0x240 -> SYSINTR 0x1d (ACC pin = GPIO
 * port 0 bit 19). STUB: the INTC2 source is unmodelled; the machine
 * repurposes the free SH7750 DMAC_DMTE0 slot. */
#define CUP2006_POWER_INTC_SLOT  22
#define CUP2006_POWER_INTC_VECT  0x0640

/* Board GPIO block at 0xFF93E000 (alias 0x1F93E000): per port +i*0x1000
 * (dir +0x4, data +0x8, input +0xc). STUB: HDD power-good (port 3 bit 10)
 * reads 1; STUB: disc shutter (port 3 bit 8) reads 1; all other pins 0. */
#define CUP2006_GPIO_SIZE        0x00004000
/* dvd.dll arg 0x68 = port 3 bit 8, the disc-insert shutter switch. */

/* MBX/3DG window at 0xFD000000 (alias 0x1D000000): writes latch and read
 * back; +0x12C status, +0x130 enable, +0x134 ack. STUB: a completed request
 * sets its status bit (STUB: armed descriptor + bit-6 enable completes). */
/* TA kick: +0x81C = 1, ack +0x12C bit 8 (+0x134 = 0x100), then poll bit 8.
 * STUB: the kick completes at once (no TA modelled); bit 8 is polled only,
 * never on the INTEVT 0x900 line. */
/* TA start: +0x810, +0x83C..+0x860, +0x800 = 1, ack +0x12C bit 4 (+0x134 =
 * 0x10), then poll. STUB: the TA run completes at the first status read
 * after the start; bit 4 is polled only. */
/* 3D render: table +0x608..+0x6E8 (400x234 at +0x654/+0x658, target +0x65C),
 * +0x680 = 1, then wait on the render event. STUB: the render completes at
 * the first +0x12C status read after the start, setting bits 2/6/8 at once;
 * no pixels are drawn. */

/* Display Unit (DU), SH7785-compatible register file at 0xFFF80000 (alias
 * 0x1FF80000); register map per SH7785 hw manual sec. 19. STUB: a 10 Hz
 * frame tick sets DSSR flags and drives INTEVT 0x780 with the DIER enables. */
#define CUP2006_DU_BASE          0xfff80000
#define CUP2006_DU_A7_BASE       0x1ff80000
/* PnMR bit 7 PnDC (19.3.7): 1 requests a frame-buffer switch in manual
 * display change mode; the DU clears it after the switch. PnBM bits 5:4:
 * 00 = manual display change mode (19.4.15). */
/* DSSR DFBn (19.3.3): bit 16+n-1 reads 1 while plane n displays DSA1R. */
/* DU system control: DEN bit 8 starts the display (manual 19.3.1). */
/* Display status flags (manual 19.3.3) and their DIER enables (19.3.5). */

/* The guest's active area: PnDSXR/PnDSYR = 400x234 (dtd.dll). */

/* 2DG drawing engine at 0xFFE80000 (alias 0x1FE80000): ctrl +0x000, status
 * +0x004, irqen +0x008, irqclr +0x00c, list +0x048, dest +0x050. STUB: a
 * start write completes the job instantly and raises INTEVT 0x8C0. */
/* INTC2 0xFFE00044 bit 20 enables INTEVT 0x8C0 (SYSINTR 0x11). */

/* STUB: MBX mailbox sub-block 0xFD100000-0xFD100FFF (alias 0x1D100000):
 * reads return 0, writes are absorbed. */
#define CUP2006_MBX_MAILBOX_BASE     0xfd100000
#define CUP2006_MBX_MAILBOX_A7_BASE  0x1d100000 /* area-7 alias (upper 3 bits) */
#define CUP2006_MBX_MAILBOX_SIZE     0x00001000

/* CPG shim: shadow the 4 KiB CPG block at 0xFFC00000 (alias 0x1FC00000) so
 * an unknown register returns zero and logs instead of aborting QEMU. */
#define CUP2006_CPG_SIZE        0x00001000

/* Boot-ROM INTC shim (intcinit=on, off by default): supply the power-on
 * IPRA the boot ROM would leave - TMU0 priority 1 (IPRA 0xFFD00004 =
 * 0x1000) - so the NK's TMU0 underflow wakes the idle sleep. */
#define CUP2006_INTC_IPRA       0xffd00004
#define CUP2006_INTC_IPRA_VALUE 0x00001000  /* TMU0 priority 1 */

/* Board RTC at 0xFFC80000 (alias 0x1FC80000, rtc=on): binary 16-bit
 * date/time fields at +0x12..+0x1C, status +0x04. STUB: runs on guest time
 * from the scenario epoch (CUP2006_RTC_EPOCH); the guest may set it. */
#define CUP2006_RTC_A7_BASE     0x1fc80000
#define CUP2006_RTC_SIZE        0x00000100

#define CUP2006_RTC_EPOCH       1252487349  /* 2009-09-09 09:09:09 UTC */


/* SCIF at 0xFF92A000 (alias 0x1F92A000): SCFTDR +0x0c, SCFSR +0x10, SCFRDR
 * +0x14. STUB: SCFSR always reports TEND|TDFE; STUB: RX bytes are paced one
 * per ms; STUB: the EEPROM reply payload comes from the eeprom=<path>
 * file (an erased 0xFFFF image when none is given). */

/* SCIF0 page 0xFF923000 (alias 0x1F923000): the SUB1: link to the Sub-CPU
 * (cup2006-subcpu.c). */

/* CARSIG/sensor block 0xFFA00000-0xFFA01FFF (alias 0x1FA00000): period
 * +0x0000, ctrl +0x1000 (bit 15 enables sampling), status +0x1004 (ready
 * 0x1000, ack write 0x8002); raises INTEVT 0xB00 -> SYSINTR 0x15. */
/* OAL priority for INTEVT 0xB00 (same IMASK-byte rule as
 * CUP2006_DVD5_DMA_INTC_PRIO): 4. */
#define CUP2006_CARSIG_INTC_PRIO  4
/* Yaw gyro register +0x1048: zero rate 8192 (ADC12 2048 x4), 81.9 counts
 * per deg/s, positive = counter-clockwise. Wheel pulses come from the
 * Sub-CPU, not from this block. */

/* SCIF0 receive line INTEVT 0x9A0 -> SYSINTR 30, repurposing the free sh7750
 * WDT source (vect 0x560). STUB: INTC2 enable/priority unmodelled, priority
 * 2; the line is raised only while SCSCR.RIE is set and a byte waits. */
#define CUP2006_SCIF0_INTC_SLOT  56
#define CUP2006_SCIF0_INTC_VECT  0x0560

/* Board 16550-compatible UART at 0x1B800000 (16-bit register stride):
 * report a ready transmitter and asserted modem status, as an idle 16550. */

/* HDD/ATA controller at 0xFF902000 (alias 0x1F902000): data +0x00,
 * command +0x1c, status +0x38, DMA ctrl/status/addr/len +0x80..+0xa0. ata=on
 * answers IDENTIFY and serves READ/WRITE DMA from hddimage= (writes go to a
 * RAM overlay, or to the file itself with hddwrite=on). */
#define CUP2006_ATA_BASE        0xff902000
#define CUP2006_ATA_A7_BASE     0x1f902000

/* ATA completion interrupt INTEVT 0x860, repurposed sh7750 INTC slot 0.
 * STUB: INTC2 enable/priority unmodelled, priority 1; STUB: the line
 * deasserts only on the +0x88 ack write (a status-port read does not). */
#define CUP2006_ATA_INTC_PRIO   1

/* STUB: display-stack interrupt lines INTEVT 0x8C0/0x780/0x900 -> SYSINTR
 * 0x11/0x12/0x1A, repurposing three free sh7750 sources (HUDI 0x600, GPIOI
 * 0x620, PCIC0 0xA00). STUB: 0x780 is DU-driven, 0x900 MBX-driven, 0x8C0 2DG. */
#define CUP2006_DISP_INTC_PRIO   1

/* STUB: SH7770 INTC2 ack/enable window at 0xFFE00040 (alias 0x1FE00040):
 * +0x00 ack, +0x04 enable. Modelling just those makes the display-stack
 * lines edge-like: one pulse is one ISR entry. */







/* DVD 5line mecha link register window 0xFF801700-0xFF8018FF (alias
 * 0x1F801700), an external board window: reads return the last value
 * written. Request frame at PA 0x08300000 (length at +0x40), reply window
 * at PA 0x1FDF0000; the doorbell is INTC2 0xFFE00044 bit 28 (see
 * cup2006_dvd5_host_doorbell). */
#define CUP2006_DVD5_A7_BASE     0x1f801700
#define CUP2006_DVD5_WIN_REQ     0x08300000 /* request frame + length at +0x40 */
/* Reply window PA 0x1FDF0000 (the model copies each reply there). */
#define CUP2006_DVD5_WIN_RPL     0x1fdf0000
/* Driver's reply buffer. */
/* Loaded DVD-ROM status: see cup2006_dvd5_spinup. */
/* DVD sector DMA: DAR +0x084 (staging window PA 0x08100000), count +0x088,
 * completion INTEVT 0xF60 -> SYSINTR 0x1B; the IST copies the sectors out
 * of the staging window. */
#define CUP2006_DVD5_DMA_INTEVT  0x0f60
/* sh7750 PCIC1_PCIDMA3 (vect 0xA20): no PCI on this board; verified first. */
#define CUP2006_DVD5_DMA_INTC_VECT 0x0a20
/* OAL priority for INTEVT 0xF60 (kernel IMASK-byte rule): 2. */

/* Sound (machine/cup2006-audio.c): SSI at 0xFF911000 with its DMA
 * channel at 0xFF801040; common DMA regs at 0xFF801800: status +0x10C,
 * ack +0x110, enable +0x114. */
/* OAL handler table index 78, returns SYSINTR 0x18. */
/* sh7750 DMAC_DMTE1: QEMU's SH7751R has no DMAC model, so the source is free. */
/* The OAL's IMASK byte for INTEVT 0xBC0 (same rule as
 * CUP2006_DVD5_DMA_INTC_PRIO). */

/* DVD 5line completion line INTEVT 0xB40 -> SYSINTR 0x1C: the ISR reads
 * byte port 0xFFA02008 and acks INTC2 bit 28; repurposes the free sh7750
 * TMU4 source (vect 0xB80). */
#define CUP2006_DVD5_SYSINTR     0x1c
/* sh7750 registers the TMU4 vector 0xB80 but this machine never programs the
 * 0x1e100000 timer, so no device holds that INTC source (same argument as
 * TMU3/0xB00 for CARSIG). Verify the vector before repurposing. */
#define CUP2006_DVD5_INTC_VECT   0x0b80
/* OAL priority for INTEVT 0xB40 (the kernel's per-line IMASK byte): 3. */

#define CUP2006_DVDST_A7_BASE    0x1fa02000

/* PC-card slot layout (see the block comment at cup2006_pcc_regs_read). */
#define CUP2006_PCC_SIZE        (CUP2006_PCC_SLOTS * CUP2006_PCC_SLOT_SIZE)
/* PC-card slot memory: two 256 KiB host apertures (window A at slot+0,
 * window B at slot+0x100000) are backed by one store, so a write through
 * one window reads back through the other. */

/* First-touch register logs, one entry per distinct register/direction.
 * Sized here so the machine state can hold them. */

typedef struct Cup2006ResetData {
    SuperHCPU *cpu;
    uint32_t vector;
} Cup2006ResetData;

uint32_t cup2006_guest_pc(void)
{
    CPUState *cs = current_cpu;

    return (cs && cs->cc->get_pc) ? (uint32_t)cs->cc->get_pc(cs) : 0;
}

/*
 * Is this address one of the SH7785's own on-chip registers?  If so the
 * manual (REJ09B0261-0100 section 31, machine/cup2006-socregs.h) says which
 * one and what it reads at power-on, so an unmodelled access reports the
 * register rather than a bare address, and a read that does not match the
 * power-on value is called out: that is a register the board models as
 * unmodelled but which the guest is actually relying on.
 */
static const Cup2006SocReg *cup2006_socreg_find(hwaddr addr)
{
    unsigned lo = 0, hi = ARRAY_SIZE(cup2006_socregs);

    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        const Cup2006SocReg *r = &cup2006_socregs[mid];

        if (addr < r->addr) {
            hi = mid;
        } else if (addr >= r->addr + r->size) {
            lo = mid + 1;
        } else {
            return r;
        }
    }
    return NULL;
}

uint64_t cup2006_mmio_read(hwaddr addr, unsigned size)
{
    const Cup2006SocReg *r = cup2006_socreg_find(addr);

    if (r) {
        trace_cup2006_soc_unmodelled_read(r->name, (uint32_t)addr, size,
                                          r->reset, r->undefined,
                                          cup2006_guest_pc());
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cup2006: mmio read  pc=0x%08" PRIx32
                      " addr=0x%08" HWADDR_PRIx " size=%u value=0x0\n",
                      cup2006_guest_pc(), addr, size);
    }
    /*
     * The manual's power-on value, not 0: a guest that reads a register this
     * board does not model and gets the manual's reset value behaves as it
     * would on a unit that has just left reset, instead of seeing one that is
     * zero everywhere.
     */
    return r ? r->reset : 0;
}

void cup2006_mmio_write(hwaddr addr, uint64_t value, unsigned size)
{
    const Cup2006SocReg *r = cup2006_socreg_find(addr);

    if (r) {
        trace_cup2006_soc_unmodelled_write(r->name, (uint32_t)addr, size,
                                           r->reset, cup2006_guest_pc());
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: mmio write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n",
                  cup2006_guest_pc(), addr, size, value);
}

uint64_t cup2006_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_mmio_read(addr, size);
}

void cup2006_unknown_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    cup2006_mmio_write(addr, value, size);
}

/* First-touch enumeration of the board/FPGA/pin-mux block (P4 0xff800200
 * and its area-7 alias 0x1f800200): log each unmodelled board register once.
 * Reads return 0 and writes are ignored. */
static void cup2006_board_note(Cup2006MachineState *s, hwaddr addr,
                                unsigned size, bool is_write)
{
    unsigned i;

    for (i = 0; i < s->board_log_nr; i++) {
        if (s->board_log_addr[i] == addr) {
            return;
        }
    }
    if (s->board_log_nr == CUP2006_BOARD_LOG_MAX) {
        if (!s->board_log_full_warned) {
            s->board_log_full_warned = true;
            trace_cup2006_board_stub_board_register_list(CUP2006_BOARD_LOG_MAX);
        }
        return;
    }
    s->board_log_addr[s->board_log_nr++] = addr;
    trace_cup2006_board_stub_unmodelled_board_register((uint32_t)addr, is_write ? "write" : "read", size);
}

uint64_t cup2006_board_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_REG_BASE + addr;

    cup2006_board_note(s, phys, size, false);
    return cup2006_mmio_read(phys, size);
}

void cup2006_board_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_REG_BASE + addr;

    cup2006_board_note(s, phys, size, true);
    cup2006_mmio_write(phys, value, size);
}

uint64_t cup2006_board_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_A7_BASE + addr;

    cup2006_board_note(s, phys, size, false);
    return cup2006_mmio_read(phys, size);
}

void cup2006_board_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_A7_BASE + addr;

    cup2006_board_note(s, phys, size, true);
    cup2006_mmio_write(phys, value, size);
}

/* Little-endian register access inside a 4-byte aligned word. */
uint32_t cup2006_reg32_read(const uint8_t *base, hwaddr addr,
                                   unsigned size)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < size && i < 4; i++) {
        value |= (uint32_t)base[addr + i] << (8 * i);
    }
    return value;
}

void cup2006_reg32_write(uint8_t *base, hwaddr addr, uint64_t value,
                                unsigned size)
{
    unsigned i;

    for (i = 0; i < size && i < 4; i++) {
        base[addr + i] = (value >> (8 * i)) & 0xff;
    }
}


static void cup2006_cpu_reset(void *opaque)
{
    Cup2006ResetData *s = opaque;

    cpu_reset(CPU(s->cpu));
    s->cpu->env.pc = s->vector;
}

/* MBX TA/geometry FIFO 0xFD800000..0xFDFFFFFF (alias 0x1D800000).  STUB: the
 * 3D core is not emulated; reads return 0 and writes are dropped, except that
 * carmarker=on keeps one frame's writes for cup2006_mbx_draw_marker. */
#define CUP2006_MBX_TA_BASE     0xfd800000
#define CUP2006_MBX_TA_A7_BASE  0x1d800000
#define CUP2006_MBX_TA_SIZE     0x00800000
#define CUP2006_MBX_TA_MAX      65536

static void cup2006_mbx_ta_note(Cup2006MachineState *s, hwaddr addr,
                                uint64_t value, unsigned size)
{
    if (!s->mbx_ta_buf) {
        s->mbx_ta_buf = g_new0(uint32_t, CUP2006_MBX_TA_MAX * 2);
    }
    if (s->mbx_ta_n < CUP2006_MBX_TA_MAX) {
        s->mbx_ta_buf[s->mbx_ta_n * 2] = (uint32_t)addr | (size << 28);
        s->mbx_ta_buf[s->mbx_ta_n * 2 + 1] = (uint32_t)value;
        s->mbx_ta_n++;
    }
}

/* carmarker=on only.  STUB, approximate: Navi draws its car marker with the
 * MBX 3D core, which is not emulated.  This takes the marker's transform,
 * viewport and triangles from the guest's TA stream but shades them with
 * colours, a light direction and depth thresholds chosen here, so the pixels
 * are the emulator's approximation, not the guest's rendering. */
static void cup2006_mbx_draw_marker(Cup2006MachineState *s)
{
    const uint32_t *b = s->mbx_ta_buf;
    unsigned n = s->mbx_ta_n, i, k, mi = 0, vi = 0, ti = 0;
    float m[16], vp[4], f;
    uint32_t target = s->mbx_window[0x65c >> 2];
    unsigned tw = (s->mbx_window[0x654 >> 2] >> 16) + 1;
    unsigned th = (s->mbx_window[0x658 >> 2] >> 16) + 1;
    const unsigned stride = 512;           /* pixels, = the DU plane stride */
    struct { float sx[3], sy[3], y, shade; } tri[16];
    unsigned nt = 0;

#define W(j) (b[(j) * 2 + 1])
    if (!b || n < 64) {
        return;
    }
    for (i = 0; i + 60 < n; i++) {
        if (!mi && W(i) == 0xa0000016) {
            mi = i + 41;
        }
        if (!vi && W(i) == 0x10000080) {
            memcpy(&f, &b[(i + 1) * 2 + 1], 4);
            if (f > 1.0f) {
                vi = i + 1;
            }
        }
        if (!ti && W(i) == 0x70000001) {
            ti = i + 1;
        }
    }
    if (!mi || !vi || !ti || mi + 16 > n || vi + 4 > n) {
        return;
    }
    for (k = 0; k < 16; k++) {
        memcpy(&m[k], &b[(mi + k) * 2 + 1], 4);
    }
    for (k = 0; k < 4; k++) {
        memcpy(&vp[k], &b[(vi + k) * 2 + 1], 4);
        if (!isfinite(vp[k])) {
            return;                 /* a non-finite viewport is not drawable */
        }
    }
    for (i = ti; i + 24 <= n && nt < 16; i += 25) {
        float v[24], ys = 0, nx = 0, ny = 0, nz = 0;
        bool ok = true;

        for (k = 0; k < 24; k++) {
            memcpy(&v[k], &b[(i + k) * 2 + 1], 4);
        }
        for (k = 0; k < 3; k++) {
            float *p = &v[k * 8];
            float X = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
            float Y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
            float Wc = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];

            if (!(Wc > 1e-3f) || !isfinite(X) || !isfinite(Y)) {
                ok = false;
                break;
            }
            tri[nt].sx[k] = vp[0] + X / Wc * vp[2];
            /* STUB: vertical centre clamped to the menu-mode centre 103.5. */
            tri[nt].sy[k] = MIN(vp[1], 103.5f) + Y / Wc * vp[3];
            ys += p[1];
            nx += p[3]; ny += p[4]; nz += p[5];
        }
        if (!ok) {
            break;
        }
        tri[nt].y = ys / 3;
        /* light direction from the stream (0, -0.633, -0.774): use |n.L| */
        tri[nt].shade = fabsf((ny * -0.633f + nz * -0.774f) / 3.0f);
        (void)nx;
        nt++;
        if (i + 24 < n && W(i + 24) == 0x00000001) {
            break;                       /* 00000001 f0000000: end */
        }
    }
#undef W
    /* painter's order: lowest layer first */
    for (unsigned pass = 0; pass < 3; pass++) {
        for (k = 0; k < nt; k++) {
            int layer = tri[k].y < 0.7f ? 0 : tri[k].y < 0.9f ? 1 : 2;
            uint16_t col;
            float xmin, xmax, ymin, ymax;
            int x, y;

            if (layer != (int)pass) {
                continue;
            }
            if (layer == 0) {
                col = 0x8000 | (6 << 10) | (6 << 5) | 6;          /* shadow */
            } else if (layer == 1) {
                col = 0x8000 | (31 << 10) | (31 << 5) | 31;      /* outline */
            } else {
                float l = 0.35f + 0.65f * MIN(1.0f, tri[k].shade);
                col = 0x8000 | ((int)(4 * l) << 10) | ((int)(12 * l) << 5) |
                      (int)(31 * l);                              /* body */
            }
            xmin = MIN(tri[k].sx[0], MIN(tri[k].sx[1], tri[k].sx[2]));
            xmax = MAX(tri[k].sx[0], MAX(tri[k].sx[1], tri[k].sx[2]));
            ymin = MIN(tri[k].sy[0], MIN(tri[k].sy[1], tri[k].sy[2]));
            ymax = MAX(tri[k].sy[0], MAX(tri[k].sy[1], tri[k].sy[2]));
            if (xmax - xmin > 200 || ymax - ymin > 200) {
                continue;                /* implausible: skip */
            }
            for (y = MAX(0, (int)floorf(ymin)); y <= MIN((int)th - 1,
                 (int)ceilf(ymax)); y++) {
                for (x = MAX(0, (int)floorf(xmin)); x <= MIN((int)tw - 1,
                     (int)ceilf(xmax)); x++) {
                    float px = x + 0.5f, py = y + 0.5f, e[3];
                    uint8_t *dp;

                    for (i = 0; i < 3; i++) {
                        unsigned j = (i + 1) % 3;
                        e[i] = (tri[k].sx[j] - tri[k].sx[i]) *
                               (py - tri[k].sy[i]) -
                               (tri[k].sy[j] - tri[k].sy[i]) *
                               (px - tri[k].sx[i]);
                    }
                    if (!((e[0] >= 0 && e[1] >= 0 && e[2] >= 0) ||
                          (e[0] <= 0 && e[1] <= 0 && e[2] <= 0))) {
                        continue;
                    }
                    dp = cup2006_dg_ram_ptr(s, target +
                                            (y * stride + x) * 2, 2);
                    if (dp) {
                        stw_le_p(dp, col);
                    }
                }
            }
        }
    }
    s->mbx_marker_frames++;
    if (s->mbx_marker_frames == 2 || s->mbx_marker_frames == 200) {
        trace_cup2006_board_mbx_marker_table();
        for (unsigned r = 0x608; r <= 0x6e8; r += 4) {
            trace_cup2006_board_mbx_marker_register(r, s->mbx_window[r >> 2]);
        }
    }
    if (s->mbx_marker_frames <= 2 || (s->mbx_marker_frames % 100) == 0) {
        /* Trace events take no floats: coordinates become milli-units. */
        trace_cup2006_board_stub_mbx_marker_triangles(nt, (int32_t)lrint(vp[0] * 1000), (int32_t)lrint(vp[1] * 1000), (int32_t)lrint(vp[2] * 1000), (int32_t)lrint(vp[3] * 1000), nt ? (int32_t)lrint(tri[0].sx[0] * 1000) : 0, nt ? (int32_t)lrint(tri[0].sy[0] * 1000) : 0);
    }
}

/* Called at +0x680 render start: dump (optionally) and reset the frame. */
void cup2006_mbx_ta_frame(Cup2006MachineState *s)
{
    if (s->carmarker) {
        cup2006_mbx_draw_marker(s);
    }
    s->mbx_ta_n = 0;
}

static uint64_t cup2006_mbx_ta_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void cup2006_mbx_ta_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    Cup2006MachineState *s = opaque;

    if (s->carmarker) {
        cup2006_mbx_ta_note(s, addr, value, size);
    }
}

static const MemoryRegionOps cup2006_mbx_ta_ops = {
    .read = cup2006_mbx_ta_read,
    .write = cup2006_mbx_ta_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * The board devices keep their state in this machine object and have no
 * reset of their own, so a reset after power-on would leave them out of step
 * with the restarted CPU.  On the real unit a reset is a power cycle by the
 * Sub-CPU: model that as switching the unit off.
 */
static void cup2006_machine_reset(MachineState *machine, ResetType type)
{
    Cup2006MachineState *s = CUP2006_MACHINE(machine);

    if (s->powered) {
        warn_report("cup2006: reset is a power cycle of the whole unit;"
                    " powering off (start QEMU again)");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    s->powered = true;
    qemu_devices_reset(type);
}

static void cup2006_init(MachineState *machine)
{
    Cup2006MachineState *s = CUP2006_MACHINE(machine);
    Cup2006ResetData *reset_info;
    SuperHCPU *cpu;
    MemoryRegion *sysmem = get_system_memory();
    hwaddr load_addr = CUP2006_NK_ADDR;
    uint32_t reset_pc = CUP2006_NK_RESET_PC;

    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));
    /* The CU-P2006 SoC is an SH7770 (SH-4A core): enables icbi, synco, movli/movco. */
    cpu->env.features |= SH_FEATURE_SH4A;

    reset_info = g_new0(Cup2006ResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = reset_pc;
    qemu_register_reset(cup2006_cpu_reset, reset_info);

    memory_region_init_ram(&s->ram, NULL, "cup2006.ram",
                           machine->ram_size, &error_fatal);
    memory_region_add_subregion(sysmem, CUP2006_RAM_BASE, &s->ram);
    cup2006_bkram_init(s, sysmem);

    /* The SH7770 is a superset of the SH7750 model: its extra on-chip
     * registers read 0 instead of stopping QEMU (core patch 0004). */
    sh7750_unknown_regs_nonfatal = true;
    sh7750_init(cpu, sysmem);

    /*
     * STUB: present the display surface as graphic console 0.
     * Coldplugged here, before machine-ready, so QEMU's console ordering puts
     * this graphic console ahead of the text consoles (index 0).
     */
    cup2006_display_create(s);

    /* SH7785-compatible DU register file at P4 0xFFF80000 with area-7 alias
     * 0x1FF80000; the frame tick driving DSSR/DIER is the STUB in
     * cup2006_du_frame_tick. */
    memory_region_init_io(&s->du_regs, NULL, &cup2006_du_ops, s,
                          "cup2006-du", CUP2006_DU_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DU_BASE,
                                        &s->du_regs, 2);
    memory_region_init_io(&s->du_a7_regs, NULL, &cup2006_du_ops, s,
                          "cup2006-du-a7", CUP2006_DU_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DU_A7_BASE,
                                        &s->du_a7_regs, 2);
    s->du_frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     cup2006_du_frame_tick, s);
    s->du_flag_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    cup2006_du_flag_off, s);

    /* 2DG drawing engine (0xFFE80000, alias 0x1FE80000). */
    s->draw_t0_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    memory_region_init_io(&s->dg_regs, NULL, &cup2006_dg_ops, s,
                          "cup2006-2dg", CUP2006_DG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DG_BASE,
                                        &s->dg_regs, 1);
    memory_region_init_io(&s->dg_a7_regs, NULL, &cup2006_dg_a7_ops, s,
                          "cup2006-2dg-a7", CUP2006_DG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DG_A7_BASE,
                                        &s->dg_a7_regs, 1);
    /* The ATA completion line is an SH7770 INTC2 source at INTEVT 0x860,
     * which the SH7750 INTC does not have: append it to the SoC INTC source
     * array so sh_intc_get_pending_vector can deliver it. */
    {
        struct intc_desc *desc = cpu->env.intc_handle;

        if (desc && desc->nr_sources > 0 && desc->sources[0].vect == 0) {
            /* Slot 0 is the enum-0 "UNUSED" source: no device holds it and
             * vect 0 is ignored, so repurposing it keeps the array valid. */
            struct intc_source *src = &desc->sources[0];

            src->vect = CUP2006_ATA_INTEVT;
            src->parent = desc;
            src->prio = CUP2006_ATA_INTC_PRIO;
            src->enable_count = 0;
            src->enable_max = 0;
            src->asserted = 0;
            src->pending = 0;
            s->ata_intc = desc;
            s->ata_intc_src = src;
            trace_cup2006_board_intc_source_ata_completion(CUP2006_ATA_INTEVT);
        } else {
            trace_cup2006_board_no_free_sh_intc();
        }
    }

    /* SCIF0 receive INTEVT 0x9A0, SYSINTR 30: repurpose the sh7750 WDT
     * source (enum 56, vect 0x560, no WDT device instantiated); verify the
     * vector first so a QEMU table change cannot mis-route it. */
    {
        struct intc_desc *desc = cpu->env.intc_handle;

        if (desc && desc->nr_sources > CUP2006_SCIF0_INTC_SLOT &&
            desc->sources[CUP2006_SCIF0_INTC_SLOT].vect ==
                CUP2006_SCIF0_INTC_VECT) {
            struct intc_source *src =
                &desc->sources[CUP2006_SCIF0_INTC_SLOT];

            src->vect = CUP2006_SCIF0_INTEVT;
            src->parent = desc;
            src->prio = CUP2006_SCIF0_INTC_PRIO;
            src->enable_count = 0;
            src->enable_max = 0;
            src->asserted = 0;
            src->pending = 0;
            s->scif0_intc_src = src;
            trace_cup2006_board_intc_source_wdt_scif0(CUP2006_SCIF0_INTC_SLOT, CUP2006_SCIF0_INTEVT, CUP2006_SCIF0_SYSINTR);
        } else {
            trace_cup2006_board_no_free_sh_intc_2(CUP2006_SCIF0_INTEVT);
        }
    }

    /* CARSIG INTEVT 0xB00, SYSINTR 0x15: 0xB00 is the sh7750 TMU3 vector and
     * the machine never programs the 0x1E100000 timer, so the source is
     * free; verify the vector before repurposing it. */
    if (s->carsig) {
        struct intc_desc *desc = cpu->env.intc_handle;
        unsigned i;
        bool found = false;

        if (desc) {
            for (i = 0; i < desc->nr_sources; i++) {
                struct intc_source *src = &desc->sources[i];

                if (src->vect != CUP2006_CARSIG_INTEVT) {
                    continue;
                }
                src->prio = CUP2006_CARSIG_INTC_PRIO;
                src->enable_count = 0;
                src->enable_max = 0;
                src->asserted = 0;
                src->pending = 0;
                s->carsig_intc_src = src;
                found = true;
                trace_cup2006_board_intc_source_tmu3_vector(i, CUP2006_CARSIG_INTEVT, CUP2006_CARSIG_SYSINTR);
                break;
            }
        }
        if (!found) {
            trace_cup2006_board_no_free_sh_intc_3(CUP2006_CARSIG_INTEVT);
        }
    }

    {
    /* STUB: display-stack INTEVT lines: repurpose three sh7750 sources
     * no device here holds (HUDI 0x600, GPIOI 0x620, PCIC0 0xa00),
     * verifying each vector first. */
    static const unsigned disp_slot[CUP2006_DISP_LINES] = { 20, 21, 31 };
    static const uint16_t disp_slot_vect[CUP2006_DISP_LINES] = {
        0x0600, 0x0620, 0x0a00,
    };
    struct intc_desc *desc = cpu->env.intc_handle;
    unsigned i;
    unsigned ok = 0;

    if (desc && desc->nr_sources > disp_slot[CUP2006_DISP_LINES - 1]) {
        for (i = 0; i < CUP2006_DISP_LINES; i++) {
            struct intc_source *src = &desc->sources[disp_slot[i]];

            if (src->vect != disp_slot_vect[i]) {
                break;
            }
            src->vect = cup2006_disp_intevt[i];
            src->prio = CUP2006_DISP_INTC_PRIO;
            src->enable_count = 0;
            src->enable_max = 0;
            src->asserted = 0;
            src->pending = 0;
            src->parent = desc;
            s->disp_intc_src[i] = src;
            ok++;
        }
    }
    if (ok == CUP2006_DISP_LINES) {
        memory_region_init_io(&s->intc2_regs, NULL, &cup2006_intc2_ops, s,
                              "cup2006-intc2", CUP2006_INTC2_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_INTC2_BASE,
                                            &s->intc2_regs, 2);
        /* STUB: area 7 ignores the upper three physical address bits, so
         * the guest's 0xFFE00000 mapping presents 0x1FE0005C; alias the
         * file there (region there as well. */
        memory_region_init_io(&s->intc2_a7_regs, NULL,
                              &cup2006_intc2_a7_ops, s,
                              "cup2006-intc2-a7", CUP2006_INTC2_SIZE);
        memory_region_add_subregion_overlap(sysmem,
                                            CUP2006_INTC2_A7_BASE,
                                            &s->intc2_a7_regs, 2);
        trace_cup2006_board_display_lines_intevt_is(CUP2006_DISP_INTEVT_DTD1, CUP2006_DISP_INTEVT_MBX, CUP2006_DISP_INTEVT_DTD0);
    } else {
        trace_cup2006_board_display_interrupt_sources_unavailable(ok);
    }
    }

    /* DVD 5line completion line INTEVT 0xB40 (SYSINTR 0x1C): repurpose the
     * free sh7750 TMU4 source (vect 0xB80); verify the vector first. */
    {
        struct intc_desc *desc = cpu->env.intc_handle;
        unsigned i;

        if (desc) {
            for (i = 0; i < desc->nr_sources; i++) {
                struct intc_source *src = &desc->sources[i];

                if (src->vect != CUP2006_DVD5_INTC_VECT) {
                    continue;
                }
                src->vect = CUP2006_DVD5_INTEVT;
                src->prio = CUP2006_DVD5_INTC_PRIO;
                src->enable_count = 0;
                src->enable_max = 0;
                src->asserted = 0;
                src->pending = 0;
                src->parent = desc;
                s->dvd5_intc_src = src;
                trace_cup2006_board_intc_source_tmu4_vector(i, CUP2006_DVD5_INTEVT, CUP2006_DVD5_SYSINTR);
                break;
            }
        }
        if (!s->dvd5_intc_src) {
            trace_cup2006_board_no_free_sh_intc_4(CUP2006_DVD5_INTEVT);
        }
        /* The engine's sector-DMA completion line, INTEVT 0xF60. */
        for (i = 0; desc && i < desc->nr_sources; i++) {
            struct intc_source *src = &desc->sources[i];

            if (src->vect != CUP2006_DVD5_DMA_INTC_VECT) {
                continue;
            }
            src->vect = CUP2006_DVD5_DMA_INTEVT;
            src->prio = CUP2006_DVD5_DMA_INTC_PRIO;
            src->enable_count = 0;
            src->enable_max = 0;
            src->asserted = 0;
            src->pending = 0;
            src->parent = desc;
            s->dvd5_dma_src = src;
            break;
        }
    }

    if (s->intcinit) {
        address_space_stw_le(&address_space_memory, CUP2006_INTC_IPRA,
                             CUP2006_INTC_IPRA_VALUE,
                             MEMTXATTRS_UNSPECIFIED, NULL);
        trace_cup2006_board_boot_rom_intc_shim(CUP2006_INTC_IPRA_VALUE);
    }

    {
        BlockBackend *blk = NULL;

        /* The whole 32 MB window is one flash part, mapped ROMD: the XIP
         * kernel's fetches are served from the array, every write reaches the
         * AMD command machine, and the -kernel image is loaded into it. */
        if (s->drfimage && s->drfimage[0]) {
            QDict *options;
            int fd = open(s->drfimage, O_RDWR | O_CREAT, 0644);

            if (fd < 0) {
                error_report("cup2006: could not open drfimage '%s': %s",
                             s->drfimage, strerror(errno));
                exit(1);
            }
            close(fd);
            /* drfimage=<file> becomes the device's block backend, raw, RW. */
            options = qdict_new();
            qdict_put_str(options, "driver", "raw");
            blk = blk_new_open(s->drfimage, NULL, options,
                         BDRV_O_RDWR | BDRV_O_RESIZE,
                               &error_fatal);
            trace_cup2006_board_drfimage_bytes_on_disk(s->drfimage,
                                                       blk_getlength(blk),
                                                       "loaded");
        }
        /* 32-bit interleaved pair: the command byte sits in every lane.
         * The autoselect IDs are the ones CUP28's flash model answers with:
         * STUB, the mark of the CU-P2006's own part is not known (no guest
         * in the boot traces issues AA 55 90 on this board), so a dump of
         * the board's part would replace them. */
        s->drf = melco_amdflash_create("cup2006-drf-flash", CUP2006_DRF_SIZE,
                                       CUP2006_DRF_SECTOR, 4,
                                       CUP2006_DRF_ID_MANUFACTURER,
                                       CUP2006_DRF_ID_DEVICE, blk);
        memory_region_add_subregion_overlap(sysmem, CUP2006_DRF_BASE,
                                            melco_amdflash_mmio(s->drf), 1);
        if (blk && blk_getlength(blk) < CUP2006_DRF_SIZE) {
            /* A short or empty image leaves the rest of the array erased:
             * grow the file to the array size and persist the array once. */
            blk_truncate(blk, CUP2006_DRF_SIZE, true, PREALLOC_MODE_OFF, 0,
                         &error_fatal);
            blk_pwrite(blk, 0, CUP2006_DRF_SIZE, melco_amdflash_array(s->drf),
                       0);
        }
        trace_cup2006_board_drf_flash_window_at(CUP2006_DRF_BASE, CUP2006_DRF_BASE + CUP2006_DRF_SIZE - 1);
    }

    cup2006_sramsave_init(s, sysmem);

    /*
     * Decode gaps: lowest priority, so real devices and RAM win. Any
     * unimplemented physical address still gets a readable zero and a
     * trace line instead of a QEMU abort.
     */
    memory_region_init_io(&s->unknown_mmio, NULL, &cup2006_unknown_ops, s,
                          "cup2006-unknown-mmio", 4 * GiB);
    memory_region_add_subregion_overlap(sysmem, 0, &s->unknown_mmio, -1);

    /*
     * Board/FPGA/pin-mux block in the BSC window. Higher priority than
     * the sh7750 0xff800000 alias, lower than no other device there.
     */
    memory_region_init_io(&s->board_regs, NULL, &cup2006_board_ops, s,
                          "cup2006-board-regs", CUP2006_BOARD_REG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_BOARD_REG_BASE,
                                        &s->board_regs, 1);

    /*
     * Area-7 alias of the same board block. sh7750_init maps 0x1f800000
     * with an abort-on-unknown-register handler, so the alias needs its own
     * region at the same priority as the P4 mapping.
     */
    memory_region_init_io(&s->board_a7_regs, NULL, &cup2006_board_a7_ops, s,
                          "cup2006-board-a7-regs", CUP2006_BOARD_REG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_BOARD_A7_BASE,
                                        &s->board_a7_regs, 1);

    /* DVD 5line mecha link: 0xFF801700-0xFF8018FF is an external board
     * window (not an SH7751 BSC register); request frame at PA 0x08300000,
     * reply window at PA 0x1FDF0000, doorbell INTC2 0xFFE00044 bit 28. */
    memory_region_init_io(&s->dvd5_regs, NULL, &cup2006_dvd5_ops, s,
                          "cup2006-dvd5-regs", CUP2006_DVD5_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DVD5_BASE,
                                        &s->dvd5_regs, 1);
    memory_region_init_io(&s->dvd5_a7_regs, NULL, &cup2006_dvd5_ops, s,
                          "cup2006-dvd5-a7-regs", CUP2006_DVD5_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DVD5_A7_BASE,
                                        &s->dvd5_a7_regs, 1);
    cup2006_audio_init(s, machine, sysmem, cpu);
    memory_region_init_io(&s->dvd5_win_req, NULL, &cup2006_dvd5_win_ops, s,
                          "cup2006-dvd5-win-req", CUP2006_DVD5_WIN_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DVD5_WIN_REQ,
                                        &s->dvd5_win_req, 2);
    memory_region_init_io(&s->dvd5_win_rpl, NULL, &cup2006_dvd5_rpl_ops, s,
                          "cup2006-dvd5-win-rpl", CUP2006_DVD5_WIN_SIZE);
    /* Priority 2, like the other board windows: above the catch-all and
     * the area-7 alias regions. */
    memory_region_add_subregion_overlap(sysmem, CUP2006_DVD5_WIN_RPL,
                                        &s->dvd5_win_rpl, 2);

    /* Status page used by the driver's init: *0xffa02000=0x30
     * *0xffa02010=0xf8, read *0xffa02008. */
    memory_region_init_io(&s->dvdst_regs, NULL, &cup2006_dvdst_ops, s,
                          "cup2006-dvdst-regs", CUP2006_DVDST_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DVDST_BASE,
                                        &s->dvdst_regs, 1);
    memory_region_init_io(&s->dvdst_a7_regs, NULL, &cup2006_dvdst_ops, s,
                          "cup2006-dvdst-a7-regs", CUP2006_DVDST_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_DVDST_A7_BASE,
                                        &s->dvdst_a7_regs, 1);

    /* Board 16550 UART (COM16550.Dll polls LSR/MSR during init). */
    memory_region_init_io(&s->uart_regs, NULL, &cup2006_uart_ops, s,
                          "cup2006-uart-regs", CUP2006_UART_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_UART_BASE,
                                        &s->uart_regs, 2);

    /*
     * CPG block: hide the sh7750 model's abort-on-unknown-register path.
     * Mapped on both the P4 (0xffc00000) and A7 (0x1fc00000) aliases.
     */
    memory_region_init_io(&s->cpg_regs, NULL, &cup2006_cpg_ops, s,
                          "cup2006-cpg-regs", CUP2006_CPG_SIZE);
    memory_region_add_subregion_overlap(sysmem, 0xffc00000, &s->cpg_regs, 1);
    memory_region_init_io(&s->cpg_a7_regs, NULL, &cup2006_cpg_ops, s,
                          "cup2006-cpg-a7-regs", CUP2006_CPG_SIZE);
    memory_region_add_subregion_overlap(sysmem, 0x1fc00000,
                                        &s->cpg_a7_regs, 1);

    /*
     * Board RTC/backup time. Off by default: a normal run keeps the
     * catch-all zero value, which is non-blocking but gives an empty date.
     */
    if (s->rtc) {
        s->rtc_base_s = CUP2006_RTC_EPOCH;
        s->rtc_base_ms = 0;
        memory_region_init_io(&s->rtc_regs, NULL, &cup2006_rtc_ops, s,
                              "cup2006-rtc-regs", CUP2006_RTC_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_RTC_BASE,
                                            &s->rtc_regs, 1);
        memory_region_init_io(&s->rtc_a7_regs, NULL, &cup2006_rtc_ops, s,
                              "cup2006-rtc-a7-regs", CUP2006_RTC_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_RTC_A7_BASE,
                                            &s->rtc_a7_regs, 1);
        trace_cup2006_board_rtc_at_runs_on(CUP2006_RTC_BASE);
    }

    /* Two PC-card slots: each 4 MiB window is card memory (plain RAM), the
     * top 0x20 bytes the slot controller (see cup2006_pcc_regs_read). */
    {
        unsigned slot;

        memory_region_init_ram(&s->pcc_mem, NULL, "cup2006-pcc-mem",
                               CUP2006_PCC_SIZE, &error_fatal);
        memory_region_add_subregion_overlap(sysmem, CUP2006_PCC_BASE,
                                            &s->pcc_mem, 0);
        /*
         * STUB: the card's CIS in slot A's attribute memory; the rest of the
         * window stays zero RAM (card attribute/common memory).
        */
        {
            static const hwaddr cisoff[CUP2006_PCC_CIS_BASES] =
                CUP2006_PCC_CIS_OFFS;
            unsigned b;

            memset(s->pcc_card, 0, sizeof(s->pcc_card));
            memset(s->pcc_cis_seen, 0, sizeof(s->pcc_cis_seen));
            memcpy(s->pcc_card, cup2006_pcc_cis, sizeof(cup2006_pcc_cis));
            for (b = 0; b < CUP2006_PCC_CIS_BASES; b++) {
                s->pcc_cis_log_nr[b] = 0;
                s->pcc_cis_opaque[b].s = s;
                s->pcc_cis_opaque[b].base = b;
                memory_region_init_io(&s->pcc_cis_mr[b], NULL,
                                      &cup2006_pcc_cis_ops,
                                      &s->pcc_cis_opaque[b],
                                      "cup2006-pcc-card", CUP2006_PCC_CIS_SIZE);
                memory_region_add_subregion_overlap(sysmem,
                                                    CUP2006_PCC_BASE + cisoff[b],
                                                    &s->pcc_cis_mr[b], 1);
            }
        }
        for (slot = 0; slot < CUP2006_PCC_SLOTS; slot++) {
            char *name = g_strdup_printf("cup2006-pcc-slot%u-regs", slot);

            s->pcc_regs_opaque[slot].s = s;
            s->pcc_regs_opaque[slot].slot = slot;
            memory_region_init_io(&s->pcc_regs_mr[slot], NULL,
                                  &cup2006_pcc_regs_ops,
                                  &s->pcc_regs_opaque[slot], name,
                                  CUP2006_PCC_REGS_SIZE);
            memory_region_add_subregion_overlap(
                sysmem,
                CUP2006_PCC_BASE + slot * CUP2006_PCC_SLOT_SIZE +
                CUP2006_PCC_REGS_OFF,
                &s->pcc_regs_mr[slot], 1);
            g_free(name);
        }
        trace_cup2006_board_stub_pc_card_slots(CUP2006_PCC_BASE, CUP2006_PCC_SLOT_SIZE >> 20);
    }

    if (s->ata) {
        memory_region_init_io(&s->ata_regs, NULL, &cup2006_ata_ops, s,
                              "cup2006-ata-regs", CUP2006_ATA_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_ATA_BASE,
                                            &s->ata_regs, 2);
        /* Area-7 alias of the ATA controller: ATA_HDD.dll reaches its
         * registers at 0x1F9020xx, not at the P4 base, so without this
         * mapping its traffic lands in the unknown catch-all. */
        memory_region_init_io(&s->ata_a7_regs, NULL, &cup2006_ata_ops, s,
                              "cup2006-ata-a7-regs", CUP2006_ATA_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_ATA_A7_BASE,
                                            &s->ata_a7_regs, 2);

        if (s->ata_intc_src) {
            s->ata_irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            cup2006_ata_irq_fire, s);
        }
        s->ata_exit.notify = cup2006_ata_exit;
        qemu_add_exit_notifier(&s->ata_exit);

        if (s->hddimage && s->hddimage[0]) {
            Error *err = NULL;
            struct stat st;

            s->hdd_fd = qemu_open(s->hddimage,
                                  (s->hddwrite ? O_RDWR : O_RDONLY) | O_BINARY,
                                  &err);
            if (s->hdd_fd < 0) {
                error_report("cup2006: could not open HDD image '%s': %s",
                             s->hddimage, error_get_pretty(err));
                error_free(err);
                exit(1);
            }
            if (fstat(s->hdd_fd, &st) < 0 || st.st_size <= 0) {
                error_report("cup2006: could not size HDD image '%s'",
                             s->hddimage);
                exit(1);
            }
            s->hdd_size = (uint64_t)st.st_size;
            if (!s->hddwrite) {
                s->hdd_overlay = g_hash_table_new_full(g_int64_hash,
                                                       g_int64_equal,
                                                       g_free, g_free);
            }
            trace_cup2006_board_hdd_image_bytes_ectors(s->hddimage, s->hdd_size, s->hdd_size / 512, s->hddwrite ? "go to the file" : "are RAM-only");
        }
    }

    /* The disc image is the drive's media. */
    if (s->dvdimage && s->dvdimage[0]) {
        Error *err = NULL;

        s->dvd_fd = qemu_open(s->dvdimage, O_RDONLY | O_BINARY, &err);
        if (s->dvd_fd < 0) {
            error_report("cup2006: could not open DVD image '%s': %s",
                         s->dvdimage, error_get_pretty(err));
            error_free(err);
            exit(1);
        }
        s->dvd_size = lseek(s->dvd_fd, 0, SEEK_END);
        trace_cup2006_board_dvd_image_bytes(s->dvdimage, s->dvd_size);
    }
    /* Disc queue: the loaded image, then dvdnext= in turn, cycling. */
    {
        g_autofree char *all = g_strjoin(":", s->dvdimage, s->dvdnext, NULL);
        char **v = g_strsplit(all, ":", -1);
        unsigned i, n = 0;

        for (i = 0; v[i]; i++) {
            if (v[i][0]) {
                v[n++] = v[i];
            } else {
                g_free(v[i]);
            }
        }
        v[n] = NULL;
        s->dvd_discs = v;
        s->dvd_ndiscs = n;
        s->dvd_next = (s->dvd_fd >= 0 && n > 1) ? 1 : 0;
    }
    /* The drive's slot: a disc goes in a few seconds after the last came out. */
    s->dvd_insert_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       cup2006_dvd_insert, s);

    if (s->selftest) {
        memory_region_init_io(&s->selftest_regs, NULL, &cup2006_selftest_ops,
                              s, "cup2006-selftest", CUP2006_SELFTEST_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_SELFTEST_BASE,
                                            &s->selftest_regs, 1);
        /* Drivers map the block through the MMU and so reach its area-7
         * physical address (gps.dll maps 0xFF93C000 -> PA 0x1F93C000);
         * the OAL uses the P4 address above. */
        memory_region_init_io(&s->selftest_a7_regs, NULL, &cup2006_selftest_ops,
                              s, "cup2006-adc-a7", CUP2006_SELFTEST_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_SELFTEST_A7_BASE,
                                            &s->selftest_a7_regs, 1);
    }

    /* I2C/PLL controller at 0xFF907000 and its area-7 alias, always on; the
     * A7 alias gets its own region so the log names the address used. */
    s->i2c_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cup2006_i2c_complete, s);
    memory_region_init_io(&s->i2c_regs, NULL, &cup2006_i2c_ops, s,
                          "cup2006-i2c", CUP2006_I2C_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_I2C_BASE,
                                        &s->i2c_regs, 1);
    memory_region_init_io(&s->i2c_a7_regs, NULL, &cup2006_i2c_a7_ops, s,
                          "cup2006-i2c-a7", CUP2006_I2C_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_I2C_A7_BASE,
                                        &s->i2c_a7_regs, 1);
    trace_cup2006_board_i2c_pll_controller_at(CUP2006_I2C_BASE, CUP2006_I2C_A7_BASE);

    /* GPS command/status latch, always on; the A7 alias needs its own region
     * so the log names the address the guest used (0x1F93D000). */
    memory_region_init_io(&s->gps_regs, NULL, &cup2006_gps_ops,
                          s, "cup2006-gps", CUP2006_GPS_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_GPS_BASE,
                                        &s->gps_regs, 1);
    memory_region_init_io(&s->gps_a7_regs, NULL, &cup2006_gps_a7_ops,
                          s, "cup2006-gps-a7", CUP2006_GPS_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_GPS_A7_BASE,
                                        &s->gps_a7_regs, 1);

    /*
     * RCD remote-control window at 0xFF937000 and its area-7 alias, always
     * on: a passive latch (no interrupt source), so the guest's own driver
     * decides when it is touched. */
    memory_region_init_io(&s->rcd_regs, NULL, &cup2006_rcd_ops, s,
                          "cup2006-rcd", CUP2006_RCD_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_RCD_BASE,
                                        &s->rcd_regs, 1);
    memory_region_init_io(&s->rcd_a7_regs, NULL, &cup2006_rcd_a7_ops, s,
                          "cup2006-rcd-a7", CUP2006_RCD_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_RCD_A7_BASE,
                                        &s->rcd_a7_regs, 1);
    trace_cup2006_board_rcd_block_at_alias(CUP2006_RCD_BASE, CUP2006_RCD_A7_BASE);

    /*
     * SCIF page 0xFF92A000 and its alias 0x1F92A000, always on. STUB: SCFSR
     * always reads 0x60 (TEND|TDFE). */
    s->scif_rx_byte = -1;
    cup2006_eeprom_load(s);
    s->scif_rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    cup2006_scif_rx_tick, s);
    /* SCIF0 byte-time timer that sets SCFSR.TDFE/TEND after a TX byte. */
    s->scif0_tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     cup2006_scif0_tx_tick, s);
    /* SCFSR resets to H'0060 (TEND=1, TDFE=1), sh7751 manual: the transmit
     * side is ready before the first SCFTDR write. */
    s->scif0_file[CUP2006_SCIF0_SCFSR] =
        CUP2006_SCIF0_SCFSR_TEND | CUP2006_SCIF0_SCFSR_TDFE;
    memory_region_init_io(&s->scif_regs, NULL, &cup2006_scif_ops,
                          s, "cup2006-scif", CUP2006_SCIF_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_SCIF_BASE,
                                        &s->scif_regs, 1);
    memory_region_init_io(&s->scif_a7_regs, NULL, &cup2006_scif_a7_ops,
                          s, "cup2006-scif-a7", CUP2006_SCIF_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_SCIF_A7_BASE,
                                        &s->scif_a7_regs, 1);

    /* SCIF0 page 0xFF923000: the Sub-CPU link (SUB1:). */
    if (s->keys || s->subpeer) {
        memory_region_init_io(&s->scif0_regs, NULL, &cup2006_scif0_ops, s,
                              "cup2006-scif0", CUP2006_SCIF_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_SCIF0_BASE,
                                            &s->scif0_regs, 1);
        memory_region_init_io(&s->scif0_a7_regs, NULL, &cup2006_scif0_a7_ops,
                              s, "cup2006-scif0-a7", CUP2006_SCIF_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_SCIF0_A7_BASE,
                                            &s->scif0_a7_regs, 1);
    }

    /* The peer's UART pacing and report queue serve key reports and the
     * Sub-CPU's own status frames alike, so they exist whenever either runs. */
    if (s->keys || s->subpeer) {
        s->peer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     cup2006_scif0_peer_pump, s);
        s->key_q_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cup2006_key_pump, s);
    }
    /* The board key pad: keys reach the guest only as the Sub-CPU's framed
     * reports on the SCIF0 link, driven by QEMU's input layer. */
    if (s->keys) {
        s->key_handler = qemu_input_handler_register((DeviceState *)s,
                                                     &cup2006_key_handler);
        trace_cup2006_board_key_pad_qemu_input(s->key_src, s->key_seq);
    }

    /*
     * Board power/ACC interrupt (INTEVT 0x240 -> SYSINTR 0x1d) on the SH7750
     * DMAC_DMTE0 source (slot 22, vect 0x640), which no device here uses.
     * acckey=on (default off) makes F12 pulse it.
     */
    s->power_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cup2006_power_pulse, s);
    s->power_drop_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       cup2006_power_drop, s);
    if (s->acckey) {
        struct intc_desc *desc = cpu->env.intc_handle;

        if (desc && desc->nr_sources > CUP2006_POWER_INTC_SLOT &&
            desc->sources[CUP2006_POWER_INTC_SLOT].vect ==
                CUP2006_POWER_INTC_VECT) {
            struct intc_source *src =
                &desc->sources[CUP2006_POWER_INTC_SLOT];

            src->vect = CUP2006_POWER_INTEVT;
            src->parent = desc;
            src->prio = CUP2006_POWER_INTC_PRIO;
            src->enable_count = 0;
            src->enable_max = 0;
            src->asserted = 0;
            src->pending = 0;
            s->power_intc_src = src;
            s->power_src_ready = true;
            trace_cup2006_board_intc_source_dmac_dmte0(CUP2006_POWER_INTC_SLOT, CUP2006_POWER_INTEVT, CUP2006_POWER_SYSINTR);
        }
        if (!s->power_src_ready) {
            trace_cup2006_board_no_free_sh_intc_5(CUP2006_POWER_INTEVT);
        }
    }

    /*
     * CARSIG/sensor block 0xFFA00000, mapped only with carsig=on; off, the
     * hunk is inert (no region, no INTC source, timer never armed). */
    s->carsig_period_us = CUP2006_CARSIG_DEFAULT_US;
    s->carsig_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   cup2006_carsig_tick, s);
    if (cup2006_gps_enabled(s)) {
        struct intc_desc *desc = cpu->env.intc_handle;
        Error *err = NULL;

        if (s->gpstrack && s->gpstrack[0]) {
            s->track = melco_track_load(s->gpstrack, &err);
            if (!s->track) {
                error_report("cup2006: %s", error_get_pretty(err));
                exit(1);
            }
            /* One scenario clock for radio, wheel pulses and gyro: it starts
             * with the track, so gpstrack= alone (no gnssreplay=) still
             * drives the wheel and gyro.  Host-side sensor timing only. */
            cup2006_track_clock_start(s,
                                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            /* Trace events take no floats: the rate becomes milli-Hz. */
            trace_cup2006_gps_car_sensor_track_rows(
                melco_track_rows(s->track), s->gpstrack,
                (int32_t)lrint(1000000.0 / melco_track_interval_ms(s->track)),
                (s->gnssreplay && *s->gnssreplay) ? "" :
                "; no GNSS recording, the receiver has no signal");
        }
        for (unsigned i = 0; desc && i < desc->nr_sources; i++) {
            struct intc_source *src = &desc->sources[i];

            if (src->vect != CUP2006_GPS_INTC_VECT) {
                continue;
            }
            src->vect = CUP2006_GPS_INTEVT;
            src->prio = CUP2006_GPS_INTC_PRIO;
            src->enable_count = 0;
            src->enable_max = 0;
            src->asserted = 0;
            src->pending = 0;
            src->parent = desc;
            s->gps_intc_src = src;
            break;
        }
        if (!s->gps_intc_src) {
            error_report("cup2006: no free INTC source for GPS INTEVT 0x%03x",
                         CUP2006_GPS_INTEVT);
            exit(1);
        }
        s->gps_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cup2006_gps_tick, s);
        timer_mod(s->gps_timer, CUP2006_GPS_TICK_MS);
        if (s->keys) {
            s->wheel_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                          cup2006_wheel_tick, s);
            timer_mod(s->wheel_timer, CUP2006_WHEEL_PERIOD_MS);
        }
        trace_cup2006_board_gps_feed_measurement_level((s->gnssreplay && *s->gnssreplay) ?
                    "radio tracking replay" : "no recording: no signal");
    }
    if (s->carsig) {
        memory_region_init_io(&s->carsig_regs, NULL, &cup2006_carsig_ops, s,
                              "cup2006-carsig", CUP2006_CARSIG_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_CARSIG_BASE,
                                            &s->carsig_regs, 1);
        memory_region_init_io(&s->carsig_a7_regs, NULL,
                              &cup2006_carsig_a7_ops, s,
                              "cup2006-carsig-a7", CUP2006_CARSIG_SIZE);
        memory_region_add_subregion_overlap(sysmem, CUP2006_CARSIG_A7_BASE,
                                            &s->carsig_a7_regs, 1);
    }

    /*
     * Board GPIO block 0xFF93E000 + 0x4000, always on, with a separate
     * area-7 alias so the log names the address used; the HDD power-good
     * pin must read present or InitDisk exits. */
    memory_region_init_io(&s->gpio_regs, NULL, &cup2006_gpio_ops,
                          s, "cup2006-gpio", CUP2006_GPIO_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_GPIO_BASE,
                                        &s->gpio_regs, 1);
    memory_region_init_io(&s->gpio_a7_regs, NULL, &cup2006_gpio_a7_ops,
                          s, "cup2006-gpio-a7", CUP2006_GPIO_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_GPIO_A7_BASE,
                                        &s->gpio_a7_regs, 1);

    /*
     * MBX/3DG register window, always on; both aliases are mapped so the log
     * names the address the guest used (the driver uses 0x1D000000). */
    s->mbx_window = g_new0(uint32_t, CUP2006_MBX_SIZE / 4);
    memory_region_init_io(&s->mbx_regs, NULL, &cup2006_mbx_ops,
                          s, "cup2006-mbx", CUP2006_MBX_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_MBX_BASE,
                                        &s->mbx_regs, 1);
    memory_region_init_io(&s->mbx_a7_regs, NULL, &cup2006_mbx_a7_ops,
                          s, "cup2006-mbx-a7", CUP2006_MBX_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_MBX_A7_BASE,
                                        &s->mbx_a7_regs, 1);
    /* The MBX TA capture window and the mailbox sub-block (STUBs). */
    memory_region_init_io(&s->mbx_ta_regs, NULL, &cup2006_mbx_ta_ops, s,
                          "cup2006-mbx-ta", CUP2006_MBX_TA_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_MBX_TA_BASE,
                                        &s->mbx_ta_regs, 1);
    memory_region_init_io(&s->mbx_ta_a7_regs, NULL, &cup2006_mbx_ta_ops, s,
                          "cup2006-mbx-ta-a7", CUP2006_MBX_TA_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_MBX_TA_A7_BASE,
                                        &s->mbx_ta_a7_regs, 1);
    memory_region_init_io(&s->mbx_mailbox_regs, NULL,
                          &cup2006_mbx_mailbox_ops, s,
                          "cup2006-mbx-mailbox", CUP2006_MBX_MAILBOX_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_MBX_MAILBOX_BASE,
                                        &s->mbx_mailbox_regs, 1);
    memory_region_init_io(&s->mbx_mailbox_a7_regs, NULL,
                          &cup2006_mbx_mailbox_ops, s,
                          "cup2006-mbx-mailbox-a7", CUP2006_MBX_MAILBOX_SIZE);
    memory_region_add_subregion_overlap(sysmem, CUP2006_MBX_MAILBOX_A7_BASE,
                                        &s->mbx_mailbox_a7_regs, 1);

    if (machine->kernel_filename) {
        gchar *data = NULL;
        gsize size = 0;
        GError *err = NULL;

        /*
         * Load the raw flattened image directly into guest RAM rather than
         * through load_image_targphys: the latter registers a ROM blob
         * that is re-applied on every CPU reset.
        */
        if (!g_file_get_contents(machine->kernel_filename, &data, &size,
                                 &err)) {
            error_report("cup2006: could not load kernel '%s': %s",
                         machine->kernel_filename, err->message);
            g_error_free(err);
            exit(1);
        }
        if ((uint64_t)load_addr + size > CUP2006_DRF_SIZE) {
            error_report("cup2006: kernel '%s' does not fit in RAM",
                         machine->kernel_filename);
            g_free(data);
            exit(1);
        }
        /* The image is the flash part's shipped content: write the device's
         * array directly, without touching the backing file. */
        memcpy(melco_amdflash_array(s->drf) + load_addr, data, size);
        g_free(data);

        trace_cup2006_board_loaded_at_prix64_bytes(machine->kernel_filename, load_addr, (int64_t)size, reset_pc);
    }

    /* The device models have no vmstate: snapshots and migration would
     * silently lose their state. */
    error_setg(&s->migration_blocker,
               "cup2006: the board devices do not support snapshots or"
               " migration");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);
}

static bool cup2006_get_selftest(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->selftest;
}

static void cup2006_set_selftest(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->selftest = value;
}

static bool cup2006_get_intcinit(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->intcinit;
}

static void cup2006_set_intcinit(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->intcinit = value;
}

static bool cup2006_get_hddwrite(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->hddwrite;
}

static void cup2006_set_hddwrite(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->hddwrite = value;
}

static bool cup2006_get_dvd5irq(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->dvd5irq;
}

static void cup2006_set_dvd5irq(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->dvd5irq = value;
}

static bool cup2006_get_rtc(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->rtc;
}

static void cup2006_set_rtc(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->rtc = value;
}

static bool cup2006_get_ata(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->ata;
}

static void cup2006_set_ata(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->ata = value;
}

static bool cup2006_get_subpeer(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->subpeer;
}

static void cup2006_set_subpeer(Object *obj, bool value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    s->subpeer = value;
    if (!value) {
        s->subpeer_frame_len = 0;
        s->subpeer_frame_total = 0;
        s->subpeer_reply_pending = false;
        s->subpeer_reply_armed = false;
        s->subpeer_acc_off_stage = 0;
    }
}

static char *cup2006_get_carmarket(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->carmarket ?: "");
}

static void cup2006_set_carmarket(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);
    char *end;
    unsigned long v;

    g_free(s->carmarket);
    s->carmarket = g_strdup(value ? value : "");
    if (!value || !value[0] || !strcmp(value, "off")) {
        s->car_market = -1;
        return;
    }
    v = strtoul(value, &end, 16);
    if (*end || v > 0xff) {
        error_setg(errp, "carmarket: a hex byte or 'off', not '%s'", value);
        return;
    }
    s->car_market = v;
}

static bool cup2006_get_acckey(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->acckey;
}

static void cup2006_set_acckey(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->acckey = value;
}

static bool cup2006_get_keys(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->keys;
}

static void cup2006_set_keys(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->keys = value;
}

static char *cup2006_get_gpstrack(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->gpstrack);
}

static void cup2006_set_gpstrack(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->gpstrack);
    s->gpstrack = g_strdup(value ? value : "");
}

static bool cup2006_get_carsig(Object *obj, Error **errp){
    return CUP2006_MACHINE(obj)->carsig;
}

static void cup2006_set_carsig(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->carsig = value;
}

static char *cup2006_get_dvdimage(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->dvdimage);
}

static void cup2006_set_dvdimage(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->dvdimage);
    s->dvdimage = g_strdup(value ? value : "");
}

static char *cup2006_get_eeprom(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->eeprom_path ?: "");
}

static void cup2006_set_eeprom(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->eeprom_path);
    s->eeprom_path = g_strdup(value ? value : "");
}

static char *cup2006_get_dvdnext(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->dvdnext);
}

static void cup2006_set_dvdnext(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->dvdnext);
    s->dvdnext = g_strdup(value ? value : "");
}

static char *cup2006_get_gnssreplay(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->gnssreplay ?: "");
}

static void cup2006_set_gnssreplay(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->gnssreplay);
    s->gnssreplay = g_strdup(value ? value : "");
}

static char *cup2006_get_bkram(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->bkram_file ?: "");
}

static void cup2006_set_bkram(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->bkram_file);
    s->bkram_file = g_strdup(value ? value : "");
}

static char *cup2006_get_sramsave(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->sramsave ?: "");
}

static void cup2006_set_sramsave(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->sramsave);
    s->sramsave = g_strdup(value ? value : "");
}

static char *cup2006_get_hwrevadc(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->hwrevadc ?: "");
}

static void cup2006_set_hwrevadc(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->hwrevadc);
    s->hwrevadc = g_strdup(value ? value : "");
}

static char *cup2006_get_hddimage(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->hddimage);
}

static void cup2006_set_hddimage(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->hddimage);
    s->hddimage = g_strdup(value ? value : "");
}

static char *cup2006_get_drfimage(Object *obj, Error **errp)
{
    return g_strdup(CUP2006_MACHINE(obj)->drfimage);
}

static void cup2006_set_drfimage(Object *obj, const char *value, Error **errp)
{
    Cup2006MachineState *s = CUP2006_MACHINE(obj);

    g_free(s->drfimage);
    s->drfimage = g_strdup(value ? value : "");
}

static bool cup2006_get_carmarker(Object *obj, Error **errp)
{
    return CUP2006_MACHINE(obj)->carmarker;
}

static void cup2006_set_carmarker(Object *obj, bool value, Error **errp)
{
    CUP2006_MACHINE(obj)->carmarker = value;
}

static void cup2006_machine_instance_init(Object *obj)
{
    /* Off by default. */
    CUP2006_MACHINE(obj)->selftest = false;
    CUP2006_MACHINE(obj)->intcinit = false;
    CUP2006_MACHINE(obj)->rtc = false;
    CUP2006_MACHINE(obj)->ata = false;
    /* The Sub-CPU as the SCIF0 link's peer, off by default (the product
     * profile turns it on).
     */
    CUP2006_MACHINE(obj)->subpeer = false;
    CUP2006_MACHINE(obj)->subpeer_frame_len = 0;
    CUP2006_MACHINE(obj)->subpeer_reply_pending = false;
    CUP2006_MACHINE(obj)->subpeer_reply_armed = false;
    CUP2006_MACHINE(obj)->subpeer_acc_off_stage = 0;
    CUP2006_MACHINE(obj)->subpeer_frame_total = 0;
    /* The key pad is part of the board, so it defaults on. */
    CUP2006_MACHINE(obj)->keys = true;
    CUP2006_MACHINE(obj)->acckey = false;
    /* The car's ACC/ignition line is on while the head unit runs. */
    CUP2006_MACHINE(obj)->acc_on = true;
    CUP2006_MACHINE(obj)->power_src_ready = false;
    CUP2006_MACHINE(obj)->power_pulses = 0;
    CUP2006_MACHINE(obj)->power_irq_raised = false;
    CUP2006_MACHINE(obj)->power_irq_raises = 0;
    CUP2006_MACHINE(obj)->key_type = 0xE2;
    CUP2006_MACHINE(obj)->key_src = 0x0181;
    CUP2006_MACHINE(obj)->key_val = 1;   /* press value byte of 01 81/E2 */
    CUP2006_MACHINE(obj)->key_seq = 1;
    CUP2006_MACHINE(obj)->key_waiting = -1;
    CUP2006_MACHINE(obj)->key_waiting_raw = false;
    CUP2006_MACHINE(obj)->dvdimage = g_strdup("");
    CUP2006_MACHINE(obj)->dvdnext = g_strdup("");
    CUP2006_MACHINE(obj)->eeprom_path = g_strdup("");
    CUP2006_MACHINE(obj)->carmarket = g_strdup("off");
    CUP2006_MACHINE(obj)->car_market = -1;
    CUP2006_MACHINE(obj)->gnss_rp.previous = UINT32_MAX;
    CUP2006_MACHINE(obj)->gnss_rp.start = -1;
    CUP2006_MACHINE(obj)->dvd_fd = -1;
    CUP2006_MACHINE(obj)->hddimage = g_strdup("");
    CUP2006_MACHINE(obj)->hdd_fd = -1;
    CUP2006_MACHINE(obj)->drfimage = g_strdup("");
    CUP2006_MACHINE(obj)->ata_status = CUP2006_ATA_SR_DRDY;
    CUP2006_MACHINE(obj)->ata_dma_status = CUP2006_ATA_DMA_NEND;
    CUP2006_MACHINE(obj)->ata_summary_at = 1;
}

static void cup2006_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Volvo RTI MMM+ (CU-P2006)";
    mc->init = cup2006_init;
    mc->reset = cup2006_machine_reset;
    mc->default_cpu_type = TYPE_SH7751R_CPU;
    machine_add_audiodev_property(mc);
    /*
     * 128 MiB is not enough: the NK startup installs a stack at physical
     * 0x0c200000 and clears a block ending at 0x08386ffc. 256 MiB is the
     * smallest power-of-two mapping that covers both.
     */
    mc->default_ram_size = 256 * MiB;

    object_class_property_add_bool(oc, "carmarker",
                                   cup2006_get_carmarker,
                                   cup2006_set_carmarker);
    object_class_property_set_description(oc, "carmarker",
        "STUB, off by default: draw an approximation of Navi's 3D car marker "
        "(the MBX 3D core is not emulated, so the marker is otherwise missing)");


    /*
     * The board's self-test sample register at 0xff93c000 (hardware
     * version, GPS antenna sense).  The product profile enables it.
     */
    object_class_property_add_bool(oc, "selftest",
                                   cup2006_get_selftest,
                                   cup2006_set_selftest);
    object_class_property_set_description(oc, "selftest",
        "provide the synthetic 0xff93c000 self-test sample register");

    /*
     * Supply the power-on INTC IPRA state the boot ROM leaves for the NK:
     * TMU0 priority 1, so its underflow wakes the kernel idle sleep.
     */
    object_class_property_add_bool(oc, "intcinit",
                                   cup2006_get_intcinit,
                                   cup2006_set_intcinit);
    object_class_property_set_description(oc, "intcinit",
        "set boot-ROM INTC IPRA (TMU0 priority 1) at machine init");

    object_class_property_add_str(oc, "drfimage",
                                  cup2006_get_drfimage,
                                  cup2006_set_drfimage);
    object_class_property_set_description(oc, "drfimage",
        "raw 32 MiB DRF flash image loaded at init and written back "
        "(dirty spans, 200 ms + exit), so the guest's own flash records "
        "survive a reboot");

    /* Board RTC at 0xffc80000: 16-bit date/time fields. Off by default so
     * the default machine keeps returning zero from the catch-all. */
    object_class_property_add_bool(oc, "rtc",
                                   cup2006_get_rtc,
                                   cup2006_set_rtc);
    object_class_property_set_description(oc, "rtc",
        "board RTC at 0xffc80000: runs on guest time from the scenario epoch, the guest may set it");

    /* Minimal HDD/ATA controller at 0xff902000: HddDDInit sends IDENTIFY
     * DEVICE (0xec), polls status 0xff902038 for DRQ, reads 512 bytes from
     * the data register. Off by default. */
    object_class_property_add_bool(oc, "ata",
                                   cup2006_get_ata,
                                   cup2006_set_ata);
    object_class_property_set_description(oc, "ata",
        "model the ATA disk controller at 0xff902000");

    /* The Sub-CPU peer's half of the SCIF0 link protocol: ACK 0x06 to the
     * guest's ENQ and command frames, answer the ones with a known reply.
     * Default off; the product profile turns it on. */
    object_class_property_add_bool(oc, "subpeer",
                                   cup2006_get_subpeer,
                                   cup2006_set_subpeer);
    object_class_property_set_description(oc, "subpeer",
        "Sub-CPU link peer: ACK 0x06 to the guest's SCIF0 ENQ and command "
        "frames, capture the guest's frames (on in the product boot)");

    object_class_property_add_str(oc, "carmarket",
                                  cup2006_get_carmarket,
                                  cup2006_set_carmarket);
    object_class_property_set_description(oc, "carmarket",
        "the car's MOST vehicle-status market byte (hex), forwarded by the "
        "Sub-CPU as fd 89 after link-up with the freeze-frame record fd 86 "
        "(a0 = Europe, customer code 2); "
        "default off = no car on the bus");

    object_class_property_add_bool(oc, "keys",
                                   cup2006_get_keys,
                                   cup2006_set_keys);
    object_class_property_set_description(oc, "keys",
        "board key pad: QEMU input events become Sub-CPU framed key reports "
        "on SCIF0 (on by default)");
    object_class_property_add_bool(oc, "acckey",
                                   cup2006_get_acckey,
                                   cup2006_set_acckey);
    object_class_property_set_description(oc, "acckey",
        "F12 in QEMU's input layer pulses the board power/ACC line "
        "(INTEVT 0x240 -> SYSINTR 0x1d) so the guest runs its own shutdown "
        "and backup flush (off by default; never sent to the guest)");

    object_class_property_add_bool(oc, "hddwrite",
                                   cup2006_get_hddwrite,
                                   cup2006_set_hddwrite);
    object_class_property_set_description(oc, "hddwrite",
        "write the guest's HDD writes into the hddimage file (default: an "
        "in-RAM overlay, the file is never modified)");

    object_class_property_add_bool(oc, "dvd5irq",
                                   cup2006_get_dvd5irq,
                                   cup2006_set_dvd5irq);
    object_class_property_set_description(oc, "dvd5irq",
        "raise the DVD 5line completion line INTEVT 0xB40 (SYSINTR 0x1C) on "
        "a modelled mailbox exchange, byte port 0xFFA02008 (off by default)");

    object_class_property_add_str(oc, "gpstrack",
                                  cup2006_get_gpstrack, cup2006_set_gpstrack);
    object_class_property_set_description(oc, "gpstrack",
        "car-sensor track CSV, one row per sample: t_s,lat_deg,lon_deg,alt_m,"
        "speed_kmh,heading_deg,yaw_dps[,gps]; speed drives the Sub-CPU's wheel "
        "pulses and yaw the gyro (STUB: the position columns are not used)");
    object_class_property_add_bool(oc, "carsig",
                                   cup2006_get_carsig,
                                   cup2006_set_carsig);
    object_class_property_set_description(oc, "carsig",
        "serve the CARSIG sensor block 0xFFA00000 and raise its INTEVT 0xB00 "
        "sample line (stub, off by default)");

    object_class_property_add_str(oc, "dvdimage",
                                  cup2006_get_dvdimage,
                                  cup2006_set_dvdimage);
    object_class_property_set_description(oc, "dvdimage",
        "read-only raw ISO image served for B8 93 sector reads");

    object_class_property_add_str(oc, "eeprom",
                                  cup2006_get_eeprom,
                                  cup2006_set_eeprom);
    object_class_property_set_description(oc, "eeprom",
        "file holding the onboard 93C46 EEPROM (64 16-bit words, 128 bytes, "
        "big-endian); created on the first guest write, erased (all 0xffff) "
        "before (default: none)");

    object_class_property_add_str(oc, "dvdnext",
                                  cup2006_get_dvdnext,
                                  cup2006_set_dvdnext);
    object_class_property_set_description(oc, "dvdnext",
        "':'-separated ISO images inserted in turn after F11 ejects a disc "
        "(the drive's eject button); the turn then returns to dvdimage=");

    object_class_property_add_str(oc, "gnssreplay",
                                  cup2006_get_gnssreplay,
                                  cup2006_set_gnssreplay);
    object_class_property_set_description(oc, "gnssreplay",
        "GNSSIQ1 recording of the GPS tracking chip's per-satellite "
        "measurements, replayed to the guest (gpslib computes the fix)");

    object_class_property_add_str(oc, "bkram",
                                  cup2006_get_bkram,
                                  cup2006_set_bkram);
    object_class_property_set_description(oc, "bkram",
        "file backing the 1 MiB battery RAM at 0x19400000 (board rev >= 3)");

    object_class_property_add_str(oc, "sramsave",
                                  cup2006_get_sramsave,
                                  cup2006_set_sramsave);
    object_class_property_set_description(oc, "sramsave",
        "file backing the 64 KiB battery SRAM at the top of DRAM");

    object_class_property_add_str(oc, "hwrevadc",
                                  cup2006_get_hwrevadc,
                                  cup2006_set_hwrevadc);
    object_class_property_set_description(oc, "hwrevadc",
        "board-revision ADC average read by the OAL (410 = T0 R2 or later)");

    object_class_property_add_str(oc, "hddimage",
                                  cup2006_get_hddimage,
                                  cup2006_set_hddimage);
    object_class_property_set_description(oc, "hddimage",
        "raw HDD image served for ATA READ/WRITE DMA (read-only unless hddwrite=on)");
}

static const TypeInfo cup2006_machine_types[] = {
    {
        .name = TYPE_CUP2006_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(Cup2006MachineState),
        .instance_init = cup2006_machine_instance_init,
        .class_init = cup2006_machine_class_init,
    },
};

DEFINE_TYPES(cup2006_machine_types)
