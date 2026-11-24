/*
 * Volvo RTI MMM+ (CU-P2006) head unit - the machine model.
 *
 * The machine boots the WinCE 4.2 SH4 "NK" image carved from LOADING.KWI
 * (CUP2006.SHx.ORG) with -kernel: the image is linked at P1 0x80100000
 * (physical 0x00100000), its B000FF entry is 0x80100004, and the CPU
 * starts at the P2 alias of that entry (0xa0100004).
 *
 * On top of RAM, the SH7750 on-chip peripherals and a serial console, the
 * board devices live in the .c.inc files #included below (DU display,
 * 2DG/MBX graphics, Sub-CPU link, ATA HDD, DVD drive, GPS, ...).  A
 * permissive catch-all covers board registers not modelled yet: it logs
 * (pc, addr, size, value) with LOG_GUEST_ERROR and returns zero.
 *
 * Copyright (c) 2026 melco-nav-emu contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "qemu/error-report.h"
#include "melco-nvram.h"
#include "qemu/log.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "hw/sh4/sh.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/sh4/cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "qemu/audio.h"
#include "migration/blocker.h"
#include "trace.h"

/* QEMU 11 replaced cpu_get_phys_page_debug: same result, -1 if unmapped. */
static hwaddr cpu_get_phys_page_debug(CPUState *cpu, vaddr addr)
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
#define CUP2006_SRAM_SIZE       0x00010000


/* DRF ("DRF1:") flash window, physical 0..0x02000000: ROMD array + AMD command
 * machine (drfimage= loads/saves). STUB: instant erase; STUB: 0xA0 program. */
#define CUP2006_DRF_BASE        0x00000000  /* physical, full window */
#define CUP2006_DRF_SIZE        0x02000000  /* 32 MiB guest window */
#define CUP2006_DRF_SECTOR      0x00040000
#define CUP2006_DRF_UNLOCK1     0xaaaaaaaa
#define CUP2006_DRF_UNLOCK2     0x55555555
#define CUP2006_DRF_ERASE_SET   0x80808080
#define CUP2006_DRF_PROGRAM_SET 0x20202020
#define CUP2006_DRF_PROGRAM     0xa0a0a0a0
#define CUP2006_DRF_AUTOSEL     0x90909090
#define CUP2006_DRF_RESET       0xf0f0f0f0
#define CUP2006_DRF_ERASE_CMD   0x30303030
/* 0xB0B0B0B0 is the AMD erase-suspend command and 0x30303030 sector erase
 * (or erase-resume); a real part consumes them without touching the array.
 * STUB: the model's erase is instant, so both are no-ops. */
#define CUP2006_DRF_ERASE_SUSPEND 0xb0b0b0b0

/* Board register block at 0xff800200 (area-7 alias 0x1f800200), mapped at
 * higher priority than the sh7750 0xff800000 alias so the catch-all sees it;
 * separate regions so the log names the address the guest used. */
#define CUP2006_BOARD_REG_BASE  0xff800200
#define CUP2006_BOARD_A7_BASE   0x1f800200
#define CUP2006_BOARD_REG_SIZE  0x00000e00

/* Board A/D converter at 0xFF93C000 (alias 0x1F93C000, cup2006-board.c.inc
 * "ADC"): ADDRA..ADDRD at +0x0..+0x6, ADCSR at +0x10; mapped by selftest=. */
#define CUP2006_SELFTEST_BASE   0xff93c000
#define CUP2006_SELFTEST_A7_BASE 0x1f93c000  /* area-7 alias: gps.dll's mapping */
#define CUP2006_SELFTEST_SIZE   0x00001000
#define CUP2006_ADC_CSR         0x10
#define CUP2006_ADC_ADF         0x8000    /* conversion end */
#define CUP2006_ADC_ADST        0x2000    /* start / busy */

/* Single-byte I2C/PLL controller at 0xFF907000 (alias 0x1F907000): ctrl
 * +0x04, status +0x0c, div +0x18, TX +0x20, RX +0x24. STUB: no tuner is
 * modelled; every transfer ACKs and RX reads 0. */
#define CUP2006_I2C_BASE          0xff907000
#define CUP2006_I2C_A7_BASE       0x1f907000
#define CUP2006_I2C_SIZE          0x00001000
#define CUP2006_I2C_CTRL          0x004
#define CUP2006_I2C_STATUS        0x00c
#define CUP2006_I2C_DIV           0x018
#define CUP2006_I2C_TX            0x020
#define CUP2006_I2C_RX            0x024
#define CUP2006_I2C_CTRL_BUSY     0x00000020
#define CUP2006_I2C_CTRL_START    0x00000008 /* start/stop enable bit */
#define CUP2006_I2C_ST_SENT       0x00000001
#define CUP2006_I2C_ST_RECV_END   0x00000002
#define CUP2006_I2C_ST_RECV_DONE  0x00000008
#define CUP2006_I2C_ST_RX_READY   0x00000010
#define CUP2006_I2C_ST_SEND_END   0x00000040
/* One byte at 100 kHz (9 bits incl. ACK) ~= 90 us: the I2C standard-mode
 * byte time (CUP2006_I2C_XFER_NS). */
#define CUP2006_I2C_XFER_NS       100000LL

/* GPS receiver latch at 0xFF93D000 (alias 0x1F93D000), a board register
 * undocumented in the SH7751/SH7750 manual: command +0x04, status +0x0c.
 * STUB: the status always reports ready (bits 1:0 = 0b01). */
#define CUP2006_GPS_BASE         0xff93d000
#define CUP2006_GPS_A7_BASE      0x1f93d000
#define CUP2006_GPS_SIZE         0x00001000
#define CUP2006_GPS_CMD          0x004
#define CUP2006_GPS_STATUS       0x00c
#define CUP2006_GPS_STATUS_READY 0x00000001
/* GPS tracking chip (with gpstrack=<csv>): a periodic tick posts a message
 * to the mailbox at 0xFF93D000 (+0x800) and raises INTEVT 0x8A0.  The
 * measurements come from the gnssreplay= recording; without one the chip
 * reports no signal.  gpslib computes every fix itself. */
#define CUP2006_GPS_CTRL         0x000
#define CUP2006_GPS_CTRL_DOORBELL 0x00010000
#define CUP2006_GPS_CTRL_IRQEN   0x00020000
#define CUP2006_GPS_LATCH        0x010
#define CUP2006_GPS_CAUSE        0x014
#define CUP2006_GPS_RX           0x800
#define CUP2006_GPS_INTEVT       0x08a0
#define CUP2006_GPS_INTC_VECT    0x0a40   /* sh7750 PCIC1_PCIDMA2, unused */
#define CUP2006_GPS_INTC_PRIO    4
#define CUP2006_GPS_TICK_MS      500

/* Remote-control (RCD) block at 0xFF937000 (alias 0x1F937000): 32-bit ctrl
 * +0x00, ack/status +0x08, 16-bit received record +0x2C. STUB: writes are
 * latched, +0x2C reads 0, no interrupt is raised. */
#define CUP2006_RCD_BASE         0xff937000
#define CUP2006_RCD_A7_BASE      0x1f937000
#define CUP2006_RCD_SIZE         0x00000100
#define CUP2006_RCD_RECORD       0x02c

/* Board power/ACC interrupt INTEVT 0x240 -> SYSINTR 0x1d (ACC pin = GPIO
 * port 0 bit 19). STUB: the INTC2 source is unmodelled; the machine
 * repurposes the free SH7750 DMAC_DMTE0 slot. */
#define CUP2006_POWER_INTEVT     0x0240
#define CUP2006_POWER_SYSINTR    0x1d
#define CUP2006_POWER_INTC_SLOT  22
#define CUP2006_POWER_INTC_VECT  0x0640
#define CUP2006_POWER_INTC_PRIO  2
#define CUP2006_GPIO_ACC_PORT    0
#define CUP2006_GPIO_ACC_BIT     19
#define CUP2006_GPIO_ACC_PIN     (1u << CUP2006_GPIO_ACC_BIT)

/* Board GPIO block at 0xFF93E000 (alias 0x1F93E000): per port +i*0x1000
 * (dir +0x4, data +0x8, input +0xc). STUB: HDD power-good (port 3 bit 10)
 * reads 1; STUB: disc shutter (port 3 bit 8) reads 1; all other pins 0. */
#define CUP2006_GPIO_BASE        0xff93e000
#define CUP2006_GPIO_A7_BASE     0x1f93e000
#define CUP2006_GPIO_SIZE        0x00004000
#define CUP2006_GPIO_PORT_STRIDE 0x00001000
#define CUP2006_GPIO_PORTS       4
#define CUP2006_GPIO_DIR         0x004
#define CUP2006_GPIO_DATA        0x008
#define CUP2006_GPIO_INPUT       0x00c
#define CUP2006_GPIO_HDD_PORT    3
#define CUP2006_GPIO_HDD_BIT     10
#define CUP2006_GPIO_HDD_PIN     (1u << CUP2006_GPIO_HDD_BIT)
/* dvd.dll arg 0x68 = port 3 bit 8, the disc-insert shutter switch. */
#define CUP2006_GPIO_SHUTTER_PIN (1u << 8)

/* MBX/3DG window at 0xFD000000 (alias 0x1D000000): writes latch and read
 * back; +0x12C status, +0x130 enable, +0x134 ack. STUB: a completed request
 * sets its status bit (STUB: armed descriptor + bit-6 enable completes). */
#define CUP2006_MBX_BASE         0xfd000000
#define CUP2006_MBX_A7_BASE      0x1d000000
#define CUP2006_MBX_SIZE         0x00010000
#define CUP2006_MBX_STATUS       0x0000012c  /* interrupt status, polled */
#define CUP2006_MBX_STATUS_ACK   0x00000134  /* status write-1-to-clear */
#define CUP2006_MBX_STATUS_DONE  0x00000004  /* +0x12C bit 2: render complete */
#define CUP2006_MBX_ST_TA_DONE    0x00000010 /* +0x12C bit 4: TA done */
#define CUP2006_MBX_ST_PM_READY   0x00000040 /* +0x12C bit 6: param mem ready */
#define CUP2006_MBX_ENABLE       0x00000130  /* enable mask (low 16 bits) */
#define CUP2006_MBX_SURFACE_PG_LO 0x00000828 /* client first page */
#define CUP2006_MBX_SURFACE_PG_HI 0x0000082c /* client last page */
#define CUP2006_MBX_SURFACE_END  0x00000838  /* client: 1 = frame armed */
#define CUP2006_MBX_KICK_BIT     0x00000040  /* +0x130 bit 6: PM-ready enable */
/* TA kick: +0x81C = 1, ack +0x12C bit 8 (+0x134 = 0x100), then poll bit 8.
 * STUB: the kick completes at once (no TA modelled); bit 8 is polled only,
 * never on the INTEVT 0x900 line. */
#define CUP2006_MBX_TA_KICK      0x0000081c
#define CUP2006_MBX_ST_TA_KICK   0x00000100  /* +0x12C bit 8, polled only */
/* TA start: +0x810, +0x83C..+0x860, +0x800 = 1, ack +0x12C bit 4 (+0x134 =
 * 0x10), then poll. STUB: the TA run completes at the first status read
 * after the start; bit 4 is polled only. */
#define CUP2006_MBX_TA_START     0x00000800
#define CUP2006_MBX_ST_POLLED    (CUP2006_MBX_ST_TA_KICK | CUP2006_MBX_ST_TA_DONE)
/* 3D render: table +0x608..+0x6E8 (400x234 at +0x654/+0x658, target +0x65C),
 * +0x680 = 1, then wait on the render event. STUB: the render completes at
 * the first +0x12C status read after the start, setting bits 2/6/8 at once;
 * no pixels are drawn. */
#define CUP2006_MBX_RENDER_START 0x00000680

/* Display Unit (DU), SH7785-compatible register file at 0xFFF80000 (alias
 * 0x1FF80000); register map per SH7785 hw manual sec. 19. STUB: a 10 Hz
 * frame tick sets DSSR flags and drives INTEVT 0x780 with the DIER enables. */
#define CUP2006_DU_BASE          0xfff80000
#define CUP2006_DU_A7_BASE       0x1ff80000
#define CUP2006_DU_SIZE          0x00001000
#define CUP2006_DU_DSYSR         0x000
#define CUP2006_DU_DSSR          0x008
#define CUP2006_DU_DSRCR         0x00c
#define CUP2006_DU_DIER          0x010
#define CUP2006_DU_DPPR          0x018
#define CUP2006_DU_PLANE_STRIDE  0x100  /* PnMR block spacing */
#define CUP2006_DU_PNMR          0x000
#define CUP2006_DU_PNMWR         0x004
#define CUP2006_DU_PNDSXR        0x010
#define CUP2006_DU_PNDSYR        0x014
#define CUP2006_DU_PNDPXR        0x018
#define CUP2006_DU_PNDPYR        0x01c
#define CUP2006_DU_PNDSA0R       0x020
#define CUP2006_DU_PNDSA1R       0x024
/* PnMR bit 7 PnDC (19.3.7): 1 requests a frame-buffer switch in manual
 * display change mode; the DU clears it after the switch. PnBM bits 5:4:
 * 00 = manual display change mode (19.4.15). */
#define CUP2006_DU_PNMR_PNDC     0x00000080
#define CUP2006_DU_PNMR_PNBM     0x00000030
/* DSSR DFBn (19.3.3): bit 16+n-1 reads 1 while plane n displays DSA1R. */
#define CUP2006_DU_DSSR_DFB(n)   (1u << (15 + (n)))
#define CUP2006_DU_PNTC2R        0x048
#define CUP2006_DU_PLANES        6
#define CUP2006_DU_DDF_8BPP      0
#define CUP2006_DU_DDF_16BPP     1
#define CUP2006_DU_DDF_ARGB      2
#define CUP2006_DU_DDF_YC        3
/* DU system control: DEN bit 8 starts the display (manual 19.3.1). */
#define CUP2006_DU_DSYSR_DEN     0x00000100
/* Display status flags (manual 19.3.3) and their DIER enables (19.3.5). */
#define CUP2006_DU_DSSR_TVR      0x00008000
#define CUP2006_DU_DSSR_FRM      0x00004000
#define CUP2006_DU_DSSR_VBK      0x00000800
#define CUP2006_DU_DSSR_RINT     0x00000200
#define CUP2006_DU_DSSR_HBK      0x00000100
#define CUP2006_DU_DSSR_RESERVED 0x30000000  /* bits 29,28 read as 1 */
#define CUP2006_DU_DIER_MASK     0x00008f00
#define CUP2006_DU_FRAME_NS      100000000ull /* STUB: 10 Hz frame tick */
#define CUP2006_DU_FLAG_HOLD_NS  5000000ull

/* The guest's active area: PnDSXR/PnDSYR = 400x234 (dtd.dll). */
#define CUP2006_DU_CONSOLE_W     400
#define CUP2006_DU_CONSOLE_H     234

/* 2DG drawing engine at 0xFFE80000 (alias 0x1FE80000): ctrl +0x000, status
 * +0x004, irqen +0x008, irqclr +0x00c, list +0x048, dest +0x050. STUB: a
 * start write completes the job instantly and raises INTEVT 0x8C0. */
#define CUP2006_DG_BASE          0xffe80000
#define CUP2006_DG_A7_BASE       0x1fe80000
#define CUP2006_DG_CTRL          0x000
#define CUP2006_DG_STATUS        0x004
#define CUP2006_DG_IRQEN         0x008
#define CUP2006_DG_IRQCLR        0x00c
#define CUP2006_DG_LIST          0x048
#define CUP2006_DG_SRC           0x04c   /* SSAR, 2-D source area start */
#define CUP2006_DG_DEST          0x050   /* RSAR, rendering-area start */
#define CUP2006_DG_SSTRR         0x058   /* source stride, pixel units */
#define CUP2006_DG_DSTRR         0x05c   /* destination stride, pixel units */
#define CUP2006_DG_STCR          0x080   /* source transparent colour */
#define CUP2006_DG_RCLR          0x0c0   /* rendering control: GBM = 16bpp */
#define CUP2006_DG_SCLMAR        0x0d0   /* system clipping (XMAX<<16)|YMAX */
#define CUP2006_DG_UCLMIR        0x0d4   /* user clipping (XMIN<<16)|YMIN */
#define CUP2006_DG_UCLMAR        0x0d8   /* user clipping (XMAX<<16)|YMAX */
#define CUP2006_DG_RCLR_16BPP    0x00040000u  /* RCLR bit 18 = GBM */
#define CUP2006_DG_RCLR_DPF      0x00100000u  /* RCLR bit 20: dest ARGB1555 */
#define CUP2006_DG_ALPHR         0x088        /* alpha value, bits 7:0 */
#define CUP2006_DG_MODE_AE       0x00000002u  /* draw mode bit 1: alpha blend */
#define CUP2006_DG_MODE_RCLIP    0x00001000u
#define CUP2006_DG_MODE_CLIP     0x00002000u
#define CUP2006_DG_MODE_STRANS   0x00000800u
#define CUP2006_DG_MODE_SS       0x00000100u
#define CUP2006_DG_MODE_STYLE    0x00000040u  /* b6: repeat, not scale */
#define CUP2006_DG_ROP_COPY      0x000000ccu  /* ROP 0xCC = source copy */
#define CUP2006_DG_STATUS_DONE   0x00000001
#define CUP2006_DG_STATUS_CLEAR  0x00000005
/* INTC2 0xFFE00044 bit 20 enables INTEVT 0x8C0 (SYSINTR 0x11). */
#define CUP2006_DG_INTC2_ENABLE  0x00100000u
#define CUP2006_DG_SIZE          0x00001000

/* STUB: MBX mailbox sub-block 0xFD100000-0xFD100FFF (alias 0x1D100000):
 * reads return 0, writes are absorbed. */
#define CUP2006_MBX_MAILBOX_BASE     0xfd100000
#define CUP2006_MBX_MAILBOX_A7_BASE  0x1d100000 /* area-7 alias (upper 3 bits) */
#define CUP2006_MBX_MAILBOX_SIZE     0x00001000

/* CPG shim: shadow the 4 KiB CPG block at 0xFFC00000 (alias 0x1FC00000) so
 * an unknown register returns zero and logs instead of aborting QEMU. */
#define CUP2006_CPG_BASE        0xffc00000
#define CUP2006_CPG_SIZE        0x00001000

/* Boot-ROM INTC shim (intcinit=on, off by default): supply the power-on
 * IPRA the boot ROM would leave - TMU0 priority 1 (IPRA 0xFFD00004 =
 * 0x1000) - so the NK's TMU0 underflow wakes the idle sleep. */
#define CUP2006_INTC_IPRA       0xffd00004
#define CUP2006_INTC_IPRA_VALUE 0x00001000  /* TMU0 priority 1 */

/* Board RTC at 0xFFC80000 (alias 0x1FC80000, rtc=on): binary 16-bit
 * date/time fields at +0x12..+0x1C, status +0x04. STUB: runs on guest time
 * from the scenario epoch (CUP2006_RTC_EPOCH); the guest may set it. */
#define CUP2006_RTC_BASE        0xffc80000
#define CUP2006_RTC_A7_BASE     0x1fc80000
#define CUP2006_RTC_SIZE        0x00000100
#define CUP2006_RTC_LATCH       0x0002
#define CUP2006_RTC_STATUS      0x0004
#define CUP2006_RTC_YEAR        0x0012
#define CUP2006_RTC_MONTH       0x0014
#define CUP2006_RTC_DAY         0x0016
#define CUP2006_RTC_HOUR        0x0018
#define CUP2006_RTC_MINUTE      0x001a
#define CUP2006_RTC_SECOND      0x001c
#define CUP2006_RTC_SECOND_SHIFT 5

#define CUP2006_RTC_SET_YEAR    0x0006  /* +0x06..+0x10: fields to set */
#define CUP2006_RTC_SET_SECOND  0x0010
#define CUP2006_RTC_EPOCH       1252487349  /* 2009-09-09 09:09:09 UTC */


/* SCIF at 0xFF92A000 (alias 0x1F92A000): SCFTDR +0x0c, SCFSR +0x10, SCFRDR
 * +0x14. STUB: SCFSR always reports TEND|TDFE; STUB: RX bytes are paced one
 * per ms; STUB: the EEPROM reply payload comes from the eeprom=<path>
 * file (an erased 0xFFFF image when none is given). */
#define CUP2006_SCIF_BASE        0xff92a000
#define CUP2006_SCIF_A7_BASE     0x1f92a000
#define CUP2006_SCIF_SIZE        0x00001000
#define CUP2006_SCIF_SCFTDR      0x000c
#define CUP2006_SCIF_SCFSR       0x0010
#define CUP2006_SCIF_SCFRDR      0x0014
#define CUP2006_SCIF_SCFSR_RDY   0x0060
#define CUP2006_SCIF_SCFSR_RDF   0x0002
#define CUP2006_SCIF_RXQ         64
#define CUP2006_SCIF_SCSPTR      0x0020     /* bit 0: the RxD pin level */
#define CUP2006_EEPROM_WORDS     64         /* 93C46: 64 x 16 bit */
#define CUP2006_EEPROM_RXQ       (2 * CUP2006_EEPROM_WORDS + 8)
#define CUP2006_EEPROM_CS_PORT   2          /* GPIO pin 83 = port 2 bit 19 */
#define CUP2006_EEPROM_CS_BIT    (1u << 19)
#define CUP2006_SCIF_TXQ         4
#define CUP2006_SCIF_BYTE_NS     (1000 * 1000)

/* SCIF0 page 0xFF923000 (alias 0x1F923000): the SUB1: link to the Sub-CPU
 * (cup2006-subcpu.c.inc). */
#define CUP2006_SCIF0_BASE       0xff923000
#define CUP2006_SCIF0_A7_BASE    0x1f923000
#define CUP2006_SCIF0_SCSCR      0x0008
#define CUP2006_SCIF0_SCFTDR     0x000c
#define CUP2006_SCIF0_SCFSR      0x0010
#define CUP2006_SCIF0_SCFRDR     0x0014
#define CUP2006_SCIF0_SCFDR      0x001c

/* CARSIG/sensor block 0xFFA00000-0xFFA01FFF (alias 0x1FA00000): period
 * +0x0000, ctrl +0x1000 (bit 15 enables sampling), status +0x1004 (ready
 * 0x1000, ack write 0x8002); raises INTEVT 0xB00 -> SYSINTR 0x15. */
#define CUP2006_CARSIG_BASE       0xffa00000
#define CUP2006_CARSIG_A7_BASE    0x1fa00000
#define CUP2006_CARSIG_SIZE       0x00002000
#define CUP2006_CARSIG_PERIOD     0x0000
#define CUP2006_CARSIG_COUNT      0x0004
#define CUP2006_CARSIG_CONTROL    0x1000
#define CUP2006_CARSIG_STATUS     0x1004
#define CUP2006_CARSIG_CTRL_EN    0x8000  /* +0x1000 bit 15: sampling on */
#define CUP2006_CARSIG_READY      0x1000  /* +0x1004 bit 12: ISR gate */
#define CUP2006_CARSIG_INTEVT     0x0b00  /* OAL InterruptTable index 72 */
#define CUP2006_CARSIG_SYSINTR    0x15
/* OAL priority for INTEVT 0xB00 (same IMASK-byte rule as
 * CUP2006_DVD5_DMA_INTC_PRIO): 4. */
#define CUP2006_CARSIG_INTC_PRIO  4
/* Yaw gyro register +0x1048: zero rate 8192 (ADC12 2048 x4), 81.9 counts
 * per deg/s, positive = counter-clockwise. Wheel pulses come from the
 * Sub-CPU, not from this block. */
#define CUP2006_CARSIG_GYRO       0x1048
#define CUP2006_CARSIG_AUX        0x1018
#define CUP2006_GYRO_ZERO         8192
#define CUP2006_GYRO_PER_DPS      81.9
#define CUP2006_CARSIG_DEFAULT_US 50000   /* STUB: if +0x0000 is 0 */

/* SCIF0 receive line INTEVT 0x9A0 -> SYSINTR 30, repurposing the free sh7750
 * WDT source (vect 0x560). STUB: INTC2 enable/priority unmodelled, priority
 * 2; the line is raised only while SCSCR.RIE is set and a byte waits. */
#define CUP2006_SCIF0_INTEVT     0x09a0
#define CUP2006_SCIF0_SYSINTR    30
#define CUP2006_SCIF0_INTC_SLOT  56
#define CUP2006_SCIF0_INTC_VECT  0x0560
#define CUP2006_SCIF0_INTC_PRIO  2
#define CUP2006_SCIF0_SCSCR_RIE  0x0040
#define CUP2006_SCIF0_SCSCR_TIE  0x0080
#define CUP2006_SCIF0_SCFSR_TDFE 0x0020
#define CUP2006_SCIF0_SCFSR_TEND 0x0040

/* Board 16550-compatible UART at 0x1B800000 (16-bit register stride):
 * report a ready transmitter and asserted modem status, as an idle 16550. */
#define CUP2006_UART_BASE           0x1b800000
#define CUP2006_UART_SIZE           0x00000010

/* HDD/ATA controller at 0xFF902000 (alias 0x1F902000): data +0x00,
 * command +0x1c, status +0x38, DMA ctrl/status/addr/len +0x80..+0xa0. ata=on
 * answers IDENTIFY and serves READ/WRITE DMA from hddimage= (writes go to a
 * RAM overlay, or to the file itself with hddwrite=on). */
#define CUP2006_ATA_BASE        0xff902000
#define CUP2006_ATA_A7_BASE     0x1f902000
#define CUP2006_ATA_SIZE        0x00001000
#define CUP2006_ATA_DATA        0x000
#define CUP2006_ATA_COMMAND     0x01c
#define CUP2006_ATA_STATUS      0x038
#define CUP2006_ATA_ALT_STATUS  0x03c
#define CUP2006_ATA_DMA_STATUS  0x084
#define CUP2006_ATA_DMA_ADDR    0x09c
#define CUP2006_ATA_DMA_LEN     0x0a0
#define CUP2006_ATA_IRQ_ACK     0x088   /* completion ISR writes 0 here */

/* ATA completion interrupt INTEVT 0x860, repurposed sh7750 INTC slot 0.
 * STUB: INTC2 enable/priority unmodelled, priority 1; STUB: the line
 * deasserts only on the +0x88 ack write (a status-port read does not). */
#define CUP2006_ATA_INTEVT      0x0860
#define CUP2006_ATA_INTC_PRIO   1

/* STUB: display-stack interrupt lines INTEVT 0x8C0/0x780/0x900 -> SYSINTR
 * 0x11/0x12/0x1A, repurposing three free sh7750 sources (HUDI 0x600, GPIOI
 * 0x620, PCIC0 0xA00). STUB: 0x780 is DU-driven, 0x900 MBX-driven, 0x8C0 2DG. */
#define CUP2006_DISP_LINES       3
#define CUP2006_DISP_INTEVT_DTD0 0x08c0   /* SYSINTR 0x11 */
#define CUP2006_DISP_INTEVT_DTD1 0x0780   /* SYSINTR 0x12 */
#define CUP2006_DISP_INTEVT_MBX  0x0900   /* SYSINTR 0x1A */
#define CUP2006_DISP_INTC_PRIO   1

/* STUB: SH7770 INTC2 ack/enable window at 0xFFE00040 (alias 0x1FE00040):
 * +0x00 ack, +0x04 enable. Modelling just those makes the display-stack
 * lines edge-like: one pulse is one ISR entry. */
#define CUP2006_INTC2_BASE       0xffe00040
#define CUP2006_INTC2_A7_BASE    0x1fe00040 /* area-7 alias (upper 3 bits) */
#define CUP2006_INTC2_SIZE       0x00000020
#define CUP2006_INTC2_ACK_OFF    0x00
#define CUP2006_INTC2_ENABLE_OFF 0x04
#define CUP2006_INTC2_GATE_OFF   0x1c

#define CUP2006_ATA_DMA_NEND    0x02

#define CUP2006_ATA_SR_DRQ      0x08
#define CUP2006_ATA_SR_DRDY     0x40

#define CUP2006_ATA_IDENT_SIZE  512


#define CUP2006_DVD_SECTOR_SIZE     2048
#define CUP2006_DVD_MAX_XFER        (16 * MiB)


/* DVD 5line mecha link register window 0xFF801700-0xFF8018FF (alias
 * 0x1F801700), an external board window: reads return the last value
 * written. Request frame at PA 0x08300000, reply/doorbell window at PA
 * 0x1FDF0000; the model answers the doorbell. */
#define CUP2006_DVD5_BASE        0xff801700
#define CUP2006_DVD5_A7_BASE     0x1f801700
#define CUP2006_DVD5_SIZE        0x00000200
#define CUP2006_DVD5_KICK        0x110      /* 0x1F801810 */
#define CUP2006_DVD5_KICK_VALUE  0x40000000
#define CUP2006_DVD5_WIN_REQ     0x08300000 /* request frame + length at +0x40 */
/* Reply window PA 0x1FDF0000; the doorbell write lands at +0x44. */
#define CUP2006_DVD5_WIN_RPL     0x1fdf0000
#define CUP2006_DVD5_WIN_SIZE    0x00001000
#define CUP2006_DVD5_DOORBELL    0x44
#define CUP2006_DVD5_DOORBELL_V  0x10000000
/* Driver's reply buffer. */
#define CUP2006_DVD5_REPLY_VA    0x01f891a0
/* Loaded DVD-ROM status: see cup2006_dvd5_spinup. */
#define CUP2006_DVD5_ST_ROM      0x48
#define CUP2006_DVD5_TYPE_DVDROM 0x04
/* DVD sector DMA: DAR +0x084 (staging window PA 0x08100000), count +0x088,
 * completion INTEVT 0xF60 -> SYSINTR 0x1B; the IST copies the sectors out
 * of the staging window. */
#define CUP2006_DVD5_DMA_DAR     0x084
#define CUP2006_DVD5_DMA_COUNT   0x088
#define CUP2006_DVD5_DMA_INTEVT  0x0f60
/* sh7750 PCIC1_PCIDMA3 (vect 0xA20): no PCI on this board; verified first. */
#define CUP2006_DVD5_DMA_INTC_VECT 0x0a20
/* OAL priority for INTEVT 0xF60 (kernel IMASK-byte rule): 2. */
#define CUP2006_DVD5_DMA_INTC_PRIO 2

/* Sound (machine/cup2006-audio.c.inc): SSI at 0xFF911000 with its DMA
 * channel at 0xFF801040; common DMA regs at 0xFF801800: status +0x10C,
 * ack +0x110, enable +0x114. */
#define CUP2006_SSI_BASE         0xff911000
#define CUP2006_SSI_A7_BASE      0x1f911000
#define CUP2006_SSI_SIZE         0x10
#define CUP2006_ADMA_BASE        0xff801040
#define CUP2006_ADMA_A7_BASE     0x1f801040
#define CUP2006_ADMA_SIZE        0x38
#define CUP2006_DMAC_STATUS      0x10c      /* 0xFF80180C */
#define CUP2006_DMAC_ENABLE      0x114      /* 0xFF801814 */
#define CUP2006_ADMA_BIT         0x00000002
/* OAL handler table index 78, returns SYSINTR 0x18. */
#define CUP2006_ADMA_INTEVT      0x0bc0
/* sh7750 DMAC_DMTE1: QEMU's SH7751R has no DMAC model, so the source is free. */
#define CUP2006_ADMA_INTC_VECT   0x0660
/* The OAL's IMASK byte for INTEVT 0xBC0 (same rule as
 * CUP2006_DVD5_DMA_INTC_PRIO). */
#define CUP2006_ADMA_INTC_PRIO   7

/* DVD 5line completion line INTEVT 0xB40 -> SYSINTR 0x1C: the ISR reads
 * byte port 0xFFA02008 and acks INTC2 bit 28; repurposes the free sh7750
 * TMU4 source (vect 0xB80). */
#define CUP2006_DVD5_INTEVT      0x0b40
#define CUP2006_DVD5_SYSINTR     0x1c
#define CUP2006_DVD5_INTC2_BIT   0x10000000 /* 0xFFE00044 bit 28 */
#define CUP2006_DVD5_ST_DATA     0x08       /* ISR's data-byte read */
#define CUP2006_DVD5_RX_MAX      48
/* sh7750 registers the TMU4 vector 0xB80 but this machine never programs the
 * 0x1e100000 timer, so no device holds that INTC source (same argument as
 * TMU3/0xB00 for CARSIG). Verify the vector before repurposing. */
#define CUP2006_DVD5_INTC_VECT   0x0b80
/* OAL priority for INTEVT 0xB40 (the kernel's per-line IMASK byte): 3. */
#define CUP2006_DVD5_INTC_PRIO   3

#define CUP2006_DVDST_BASE       0xffa02000
#define CUP2006_DVDST_A7_BASE    0x1fa02000
#define CUP2006_DVDST_SIZE       0x00001000

#define TYPE_CUP2006_MACHINE MACHINE_TYPE_NAME("cup2006")
OBJECT_DECLARE_SIMPLE_TYPE(Cup2006MachineState, CUP2006_MACHINE)

/* Opaque for one PC-card slot's controller-register region. */
typedef struct Cup2006PccRegs {
    Cup2006MachineState *s;
    unsigned slot;
} Cup2006PccRegs;

typedef struct Cup2006PccCis {
    Cup2006MachineState *s;
    unsigned base;              /* index into pcc_cis_seen[] / log counters */
} Cup2006PccCis;

/* PC-card slot layout (see the block comment at cup2006_pcc_regs_read). */
#define CUP2006_PCC_BASE        0x1b000000
#define CUP2006_PCC_SLOTS       2
#define CUP2006_PCC_SLOT_SIZE   0x00400000
#define CUP2006_PCC_SIZE        (CUP2006_PCC_SLOTS * CUP2006_PCC_SLOT_SIZE)
#define CUP2006_PCC_REGS_OFF    0x003fffe0
#define CUP2006_PCC_REGS_SIZE   0x00000020
#define CUP2006_PCC_STATUS      0x08
#define CUP2006_PCC_STATUS_VALUE 0x0120
/* PC-card slot memory: two 256 KiB host apertures (window A at slot+0,
 * window B at slot+0x100000) are backed by one store, so a write through
 * one window reads back through the other. */
#define CUP2006_PCC_CIS_OFF     0x00100000
#define CUP2006_PCC_CIS_OFFS    { 0x00000000, CUP2006_PCC_CIS_OFF }
#define CUP2006_PCC_CIS_BASES   2
#define CUP2006_PCC_CIS_SIZE    0x00000400

/* GNSS radio replay (gnssreplay=): the recording and its playback position. */
typedef struct Cup2006GnssReplay {
    gchar *data;
    gsize size;
    uint32_t frames;
    uint32_t interval;
    uint32_t previous;          /* last frame delivered, UINT32_MAX = none */
    int64_t start;              /* device clock at the first frame, -1 = not started */
    uint8_t pending[12 * 0x30]; /* nav subframes waiting for the next post */
    bool sent_nav;
    bool ended;
} Cup2006GnssReplay;

/* First-touch register logs, one entry per distinct register/direction.
 * Sized here so the machine state can hold them. */
#define CUP2006_BOARD_LOG_MAX 256
#define CUP2006_SCIF_LOG_MAX  32
#define CUP2006_GPS_LOG_MAX   32
#define CUP2006_MBX_LOG_MAX   32
#define CUP2006_RCD_LOG_MAX   32
#define CUP2006_GPIO_LOG_MAX  32

struct Cup2006MachineState {
    MachineState parent_obj;

    MemoryRegion ram;
    MemoryRegion unknown_mmio;
    MemoryRegion board_regs;
    MemoryRegion board_a7_regs;
    MemoryRegion dvd5_regs;
    MemoryRegion dvd5_a7_regs;
    MemoryRegion dvd5_win_req;
    MemoryRegion dvd5_win_rpl;
    MemoryRegion dvdst_regs;
    MemoryRegion dvdst_a7_regs;
    uint8_t dvd5_reg[CUP2006_DVD5_SIZE];
    uint8_t dvd5_req[CUP2006_DVD5_WIN_SIZE];
    uint8_t dvd5_rpl[CUP2006_DVD5_WIN_SIZE];
    uint8_t dvdst_reg[CUP2006_DVDST_SIZE];
    uint64_t dvd5_kicks;
    uint64_t dvd5_replies;
    uint64_t dvd5_reads;     /* b8 93 read requests on the 5line link */
    /* the completion line and the byte port it serves. */
    struct intc_source *dvd5_intc_src;
    uint8_t dvd5_rx[CUP2006_DVD5_RX_MAX];
    unsigned dvd5_rx_len;
    unsigned dvd5_rx_pos;
    uint64_t dvd5_irqs;
    uint64_t dvd5_irq_acks;
    uint64_t dvd5_irq_blocked;
    bool dvd5irq;
    bool hddwrite;               /* guest HDD writes go to the image file */
    hwaddr board_log_addr[CUP2006_BOARD_LOG_MAX];
    unsigned board_log_nr;
    bool board_log_full_warned;
    struct { hwaddr addr; bool is_write; } scif_log[CUP2006_SCIF_LOG_MAX];
    unsigned scif_log_nr;
    bool scif_log_full_warned;
    struct { hwaddr addr; bool is_write; } gps_log[CUP2006_GPS_LOG_MAX];
    unsigned gps_log_nr;
    bool gps_log_full_warned;
    struct { hwaddr addr; bool is_write; } mbx_log[CUP2006_MBX_LOG_MAX];
    unsigned mbx_log_nr;
    bool mbx_log_full_warned;
    struct { hwaddr addr; bool is_write; } rcd_log[CUP2006_RCD_LOG_MAX];
    unsigned rcd_log_nr;
    unsigned rcd_accesses;
    struct { hwaddr addr; bool is_write; } gpio_log[CUP2006_GPIO_LOG_MAX];
    unsigned gpio_log_nr;
    bool gpio_log_full_warned;
    /* The engine's sector DMA completion line, INTEVT 0xF60. */
    struct intc_source *dvd5_dma_src;
    /* The drive's spin-up: loading reported -> one B1 "ready" frame. */
    bool dvd5_spinup_pending;
    bool dvd5_disc_ready;
    bool dvd5_rx_unsol;      /* the staged bytes are an unsolicited frame */
    /* The drive's eject button and disc slot (F11; cup2006-dvd.c.inc). */
    bool dvd5_ejected;       /* the open disc sits ejected in the slot */
    bool dvd_shutter_open;   /* F10: the disc-insert shutter is open */
    char **dvd_discs;        /* dvdimage= then each dvdnext= disc */
    unsigned dvd_ndiscs;
    unsigned dvd_next;       /* index of the next disc to insert */
    QEMUTimer *dvd_insert_timer;
    /* Sound: SSI + its DMA channel (cup2006-audio.c.inc). */
    MemoryRegion ssi_regs;
    MemoryRegion ssi_a7_regs;
    MemoryRegion adma_regs;
    MemoryRegion adma_a7_regs;
    uint32_t ssi_cr;
    uint32_t adma_reg[CUP2006_ADMA_SIZE / 4];
    uint64_t ssi_writes;         /* bounds the SSI write trace */
    uint64_t adma_writes;        /* bounds the DMA write trace */
    bool adma_running;
    unsigned adma_desc;          /* 0 = descriptor A, 1 = B */
    uint32_t adma_pos;           /* words done in the current descriptor */
    double adma_frac;
    int64_t adma_last_ns;
    uint64_t adma_starts;
    uint64_t adma_halves;
    uint64_t audio_stalls;     /* host pulls cut short by an unacked half */
    QEMUTimer *adma_timer;
    struct intc_source *adma_src;
    AudioBackend *audio_be;
    SWVoiceOut *audio_voice;
    unsigned audio_rate;
    uint64_t audio_frames;
    bool audio_playing;           /* SSI enabled with DMA running */
    unsigned adma_pending;        /* finished halves not yet acknowledged */
    int64_t audio_report_ns;
    MemoryRegion uart_regs;
    MemoryRegion selftest_regs;
    MemoryRegion selftest_a7_regs;
    uint16_t adc_csr;            /* the board ADC's ADCSR */
    MemoryRegion gps_regs;
    MemoryRegion gps_a7_regs;
    MemoryRegion gpio_regs;
    MemoryRegion gpio_a7_regs;
    MemoryRegion mbx_regs;
    MemoryRegion mbx_a7_regs;
    MemoryRegion mbx_ta_regs;      /* TA/geometry FIFO capture */
    MemoryRegion mbx_ta_a7_regs;
    MemoryRegion mbx_mailbox_regs;
    MemoryRegion mbx_mailbox_a7_regs;
    MemoryRegion cpg_regs;
    MemoryRegion cpg_a7_regs;
    MemoryRegion rtc_regs;
    MemoryRegion rtc_a7_regs;
    MemoryRegion scif_regs;
    MemoryRegion scif_a7_regs;
    uint8_t scif_file[CUP2006_SCIF_SIZE];
    /* Sub_SCIF sub-CPU reply: TX command frame and paced RX queue. */
    uint8_t scif_tx[CUP2006_SCIF_TXQ];
    unsigned scif_tx_len;
    uint8_t scif_rxq[CUP2006_EEPROM_RXQ];
    unsigned scif_rxq_head;
    unsigned scif_rxq_tail;
    int scif_rx_byte;            /* byte presented in SCFRDR, -1 = none */
    uint16_t eeprom[CUP2006_EEPROM_WORDS];  /* the onboard 93C46 */
    bool eeprom_ewen;            /* its erase/write enable latch */
    bool eeprom_cs;              /* its chip select, GPIO pin 83 */
    char *eeprom_path;           /* eeprom=: the unit's EEPROM file */
    QEMUTimer *scif_rx_timer;
    /* SCIF0 page 0xFF923000: the SUB1: link to the Sub-CPU. */
    MemoryRegion scif0_regs;
    MemoryRegion scif0_a7_regs;
    uint8_t scif0_file[CUP2006_SCIF_SIZE];
    uint8_t scif0_rxq[CUP2006_SCIF_RXQ];
    unsigned scif0_rxq_head;
    unsigned scif0_rxq_tail;
    /* SCIF0 receive line (INTEVT 0x9A0 / SYSINTR 30). */
    struct intc_source *scif0_intc_src;
    bool scif0_irq_raised;
    QEMUTimer *scif0_tx_timer;       /* byte shifted out -> TDFE */
    /* CARSIG/sensor block 0xFFA00000 (INTEVT 0xB00 / SYSINTR 0x15). */
    MemoryRegion carsig_regs;
    MemoryRegion carsig_a7_regs;
    uint8_t carsig_file[CUP2006_CARSIG_SIZE];
    QEMUTimer *carsig_timer;
    struct intc_source *carsig_intc_src;
    bool carsig_irq_raised;
    uint32_t carsig_period_us;
    int64_t carsig_deadline_ns;
    uint32_t carsig_count;
    uint64_t carsig_samples;
    MemoryRegion ata_regs;
    MemoryRegion ata_a7_regs;
    MemoryRegion drf_regs;
    /* PC-card slots 0/1: 4 MiB windows, controller block at +0x3FFFE0. */
    MemoryRegion pcc_mem;
    MemoryRegion pcc_regs_mr[CUP2006_PCC_SLOTS];
    MemoryRegion pcc_cis_mr[CUP2006_PCC_CIS_BASES];
    Cup2006PccCis pcc_cis_opaque[CUP2006_PCC_CIS_BASES];
    uint8_t pcc_ctrl[CUP2006_PCC_SLOTS][CUP2006_PCC_REGS_SIZE];
    /* the card's CIS image + the config-register bytes the driver writes */
    uint8_t pcc_card[CUP2006_PCC_CIS_SIZE];   /* one card memory, both windows */
    uint8_t pcc_cis_seen[CUP2006_PCC_CIS_BASES][CUP2006_PCC_CIS_SIZE];
    unsigned pcc_cis_log_nr[CUP2006_PCC_CIS_BASES];
    /*
     * Per-slot opaque: a MemoryRegionOps callback gets the offset inside its
     * region, not the window address, so the slot identity has to come from
     * the opaque pointer.
 */
    Cup2006PccRegs pcc_regs_opaque[CUP2006_PCC_SLOTS];
    /* bit0/bit1: a read/write of this offset has been logged once. */
    uint8_t pcc_seen[CUP2006_PCC_SLOTS][CUP2006_PCC_REGS_SIZE];
    bool selftest;
    bool intcinit;
    bool ata;
    /* The Sub-CPU as the SCIF0 link's slave: it answers a guest ENQ or
     * command frame with ACK 0x06 and also pushes its own frames
     * (startup report f9 82, vehicle status fd 89/fd 86, keys, wheel). */
    bool subpeer;
    uint8_t subpeer_frame[CUP2006_SCIF_RXQ];
    unsigned subpeer_frame_len;      /* bytes collected, 0 = between frames */
    unsigned subpeer_frame_total;    /* total bytes of the frame in progress */
    unsigned subpeer_polls;          /* guest ENQs answered */
    unsigned subpeer_frames;         /* guest command frames ACKed */
    unsigned subpeer_acks;           /* guest ACK bytes seen */
    unsigned subpeer_naks;           /* guest 0x15 seen */
    /* A known reply frame may be offered after the guest ACKs a command,
     * only while the guest is in its frame-receive state: ENQ -> reply ->
     * guest ACK. The pending reply is a code, not a flag. */
    bool subpeer_reply_pending;      /* a known reply code waits to deliver */
    uint8_t subpeer_reply_code;      /* reply command byte 0 */
    uint8_t subpeer_reply_code1;     /* reply command byte 1 */
    uint8_t subpeer_reply_seq;       /* reply seq byte (payload[2]) */
    uint8_t subpeer_reply_data[6];   /* reply data bytes (payload[3..]) */
    unsigned subpeer_reply_data_len;
    bool subpeer_reply_armed;        /* peer ENQ sent, frame waits for guest 0x06 */
    /* The car on the MOST bus: the Sub-CPU forwards vehicle status (fd 89,
     * market byte carmarket=) and the freeze-frame record (fd 86). */
    int car_market;
    char *carmarket;
    bool subpeer_car_started;        /* the car status went to the pump */
    uint8_t car_voice_reply[2];      /* the car's f3 81 answer (voice channel) */
    /* The guest's state machine refuses a peer ENQ while waiting for an
     * ACK, so a reply offer may only follow a second guest ENQ: the first
     * is ACKed, the next one accepts the offer. */
    bool subpeer_enq_acked;          /* a guest ENQ was ACKed, offer next time */
    int64_t subpeer_grant_ms;        /* guest holds the link since (0 = no) */
    unsigned subpeer_replies;        /* known reply frames sent */
    /* The Sub-CPU's startup status report: offered once, on the guest's
     * first ENQ (code f9 82, handled by the guest's map-update manager). */
    bool subpeer_report_queued;      /* the startup report has been offered */
    int64_t drf_activity_ms;         /* guest time of the last flash program/erase */
    QEMUTimer *power_cut_timer;      /* ignition-off: Sub-CPU cuts power when idle */
    int64_t power_cut_armed_ms;
    /* ACC-off notification: F12 makes the Sub-CPU send NET OFF (f1 8a,
     * data[0]=0x02); held separately so a boot-time reply cannot overwrite it. */
    uint8_t subpeer_acc_off_stage;   /* ignition-off: 1 NET OFF, 2 DeviceMode Shutdown */
    /* The board key pad: a key press is a framed report from the Sub-CPU
     * peer on SCIF0, triggered only by a QEMU input event. */
    bool keys;
    uint16_t key_src;            /* 0x0181 key class (0x0481 = diagnostic only) */
    uint8_t key_val;             /* value byte of the 0x0181 report */
    uint8_t key_type;            /* key-state byte data[0]: E2..E5 */
    /* acckey=<on>: F12 in the QEMU input layer pulses the board power line. */
    bool acckey;
    bool acc_on;                 /* ACC/ignition level seen on gpio pin 0x13 */
    bool power_src_ready;
    int power_pulses;
    QEMUTimer *power_timer;
    QEMUTimer *power_drop_timer;
    struct intc_source *power_intc_src;
    bool power_irq_raised;
    uint64_t power_irq_raises;
    unsigned key_seq;            /* 0: ack+frame, 1: ENQ+frame, 2: frame */
    QemuInputHandlerState *key_handler;
    int key_waiting;             /* latched guest key code, -1 = none */
    /* Press/release queue: each key-down queues its byte and each key-up
     * queues raw 0x00, as a keypad peer reports press and release. */
    int key_q[16];
    bool key_q_raw[16];
    unsigned key_q_head, key_q_n;
    bool key_down[Q_KEY_CODE__MAX]; /* host switch state, excludes auto-repeat */
    bool key_frame_inflight;     /* frame sent; retain key_waiting until ACK */
    QEMUTimer *key_q_timer;
    QEMUTimer *wheel_timer;
    uint64_t wheel_frames;
    double wheel_carry;
    bool key_last_wheel;
    bool key_waiting_raw;        /* latched key goes on the raw 0181 lane */
    unsigned key_frames;         /* reports put on the wire */
    unsigned key_guest_acks;     /* guest 0x06 after a frame */
    int64_t key_ms;              /* time of the input event */
    int64_t key_frame_ms;        /* time the frame was queued */
    /* Peer UART transmitter: one byte in flight, paced. */
    uint8_t peer_q[CUP2006_SCIF_RXQ];
    unsigned peer_q_head;
    unsigned peer_q_tail;
    QEMUTimer *peer_timer;
    bool peer_enq_armed;         /* seq 1: ENQ sent, frame waits for guest 0x06 */
    int64_t peer_enq_ms;         /* when that ENQ went out */
    int64_t peer_enq_stall_logged; /* peer_enq_ms already reported as stalled */
    bool carsig;
    bool rtc;
    uint32_t gps_cmd;
    /* GPS feed (gpstrack=<csv>). See CUP2006_GPS_FEED. */
    char *gpstrack;
    uint32_t gps_regs_w[CUP2006_GPS_SIZE / 4];  /* registers + mailbox */
    QEMUTimer *gps_timer;
    struct intc_source *gps_intc_src;
    bool gps_handshake;            /* 0xA5 reset written, state reads 1 */
    uint64_t gps_ticks;
    unsigned gps_track_n;
    float *gps_track;              /* rows of 7: lat lon alt kmh hdg yaw gps */
    float gps_track_stopped[7];    /* finite replay endpoint, no more motion */
    int64_t wheel_track_last_ms;   /* physical pulse counter's integration time */
    double gps_track_dt;           /* seconds per row */
    int64_t gps_track_start_ms;
    /* Measurement-level GNSS feed: host-assigned PRN per channel. */
    int gnss_chan_prn[12];         /* PRN 1..32, 0 = unassigned */
    /* RCD remote-control window at 0xFF937000 (STUB shadow). */
    MemoryRegion rcd_regs;
    MemoryRegion rcd_a7_regs;
    uint8_t rcd_win[CUP2006_RCD_SIZE];
    /* I2C/PLL controller at 0xFF907000 (STUB state machine). */
    MemoryRegion i2c_regs;
    MemoryRegion i2c_a7_regs;
    uint32_t i2c_ctrl;
    uint32_t i2c_status;
    uint32_t i2c_div;
    uint32_t i2c_tx;
    uint32_t i2c_rx;
    uint32_t i2c_pending;           /* completion flags not yet latched */
    bool i2c_busy;
    QEMUTimer *i2c_timer;
    uint32_t gpio_dir[CUP2006_GPIO_PORTS];
    uint32_t gpio_data[CUP2006_GPIO_PORTS];
    uint32_t *mbx_window;              /* CUP2006_MBX_SIZE/4 dwords */
    bool carmarker;                    /* carmarker=on: approximate 3D marker */
    bool powered;                      /* the cold reset has happened */
    Error *migration_blocker;
    uint32_t *mbx_ta_buf;              /* this frame's FIFO writes (off,val) */
    unsigned mbx_ta_n;
    uint64_t mbx_marker_frames;
    uint32_t mbx_kick_count;           /* STUB +0x130 bit-6 kicks seen */
    uint32_t mbx_arm_count;            /* STUB +0x838 = 1 descriptor arms */
    /* DU register file and scan-out. */
    QemuConsole *disp_con;
    MemoryRegion du_regs;
    MemoryRegion du_a7_regs;
    uint32_t du_shadow[CUP2006_DU_SIZE / 4];
    uint32_t du_status;                /* DSSR flags the model has set */
    uint32_t du_dier;                  /* enabled interrupt flags */
    bool du_enabled;                   /* DSYSR DEN observed as 1 */
    QEMUTimer *du_frame_timer;
    QEMUTimer *du_flag_timer;
    uint64_t du_irqs;                  /* INTEVT 0x780 assertions */
    /* Double buffering: du_displayed[n] is the scanned buffer for plane n;
     * a PnDC 0->1 write applies at the next frame tick. */
    uint8_t du_displayed[CUP2006_DU_PLANES + 1];
    bool du_flip_pending[CUP2006_DU_PLANES + 1];
    MemoryRegion dg_regs;
    MemoryRegion dg_a7_regs;
    /* 2DG job completion model: dg_reg is the register shadow the guest
     * reads back. */
    uint32_t dg_reg[CUP2006_DG_SIZE / 4];
    uint64_t dg_jobs;
    uint64_t dg_irqs;
    /*
     * the completion interrupt is gated by the guest's INTC2 enable
     * bit for the 0x8C0 line (0xFFE00044 bit 20), like the device does.
 */
    uint64_t dg_disabled_jobs;
    /* list executor counters (pixels written from guest commands). */
    uint64_t dg_blits;
    uint64_t dg_skipped;
    uint64_t dg_work_ops;           /* work-plane pixel ops in this job */
    int64_t draw_t0_ms;                /* 2DG log timestamps */
    struct {
        bool valid;
        uint32_t base, w, h, stride, fmt;
    } du_reported[CUP2006_DU_PLANES + 1];
    uint32_t drf_erases;
    uint8_t *drf_data;
    uint32_t drf_program_addr;
    bool drf_program_pending;
    unsigned drf_cmd_state;      /* AMD command sequence position */
    bool drf_bypass;             /* unlock-bypass mode */
    char *drfimage;             /* raw 32 MB save/load of drf_data */
    int drf_fd;
    bool drf_image_full_write;  /* image shorter than the array: write all */
    hwaddr drf_dirty_lo;        /* dirty span [lo, hi), lo >= hi = clean */
    hwaddr drf_dirty_hi;
    QEMUTimer *drf_save_timer;
    Notifier drf_exit;
    MemoryRegion bkram;          /* bkram=: battery RAM (rev >= 3) */
    Notifier bkram_exit;
    QEMUTimer *bkram_timer;
    char *bkram_path;
    char *sramsave_path;         /* sramsave=: battery SRAM file */
    uint8_t *bkram_last;         /* content of the last battery RAM save */
    uint8_t *sramsave_last;      /* content of the last battery SRAM save */
    Notifier sramsave_exit;
    QEMUTimer *sramsave_timer;
    char *dvdimage;
    char *dvdnext;           /* discs inserted in turn after an eject */
    int dvd_fd;
    uint64_t dvd_size;
    char *hddimage;
    char *gnssreplay;            /* GNSSIQ1 radio tracking recording */
    Cup2006GnssReplay gnss_rp;
    char *bkram_file;            /* battery-backed RAM file (rev >= 3) */
    char *sramsave;              /* top-of-DRAM battery SRAM file */
    char *hwrevadc;              /* board-revision ADC average */
    int64_t rtc_base_s;          /* RTC seconds at rtc_base_ms */
    int64_t rtc_base_ms;         /* guest time of the last RTC set */
    uint16_t rtc_set[6];         /* fields staged at +0x06..+0x10 */
    int hdd_fd;
    uint64_t hdd_size;
    /* Guest writes are kept as a sparse RAM overlay of 512-byte sectors
     * (sector index -> g_malloc'd block); the host fixture is read-only. */
    GHashTable *hdd_overlay;
    uint32_t ata_shadow[CUP2006_ATA_SIZE / 4];
    uint8_t ata_ident[CUP2006_ATA_IDENT_SIZE];
    unsigned ata_ident_len;
    unsigned ata_ident_pos;
    uint8_t ata_status;
    uint32_t ata_dma_status;
    bool ata_dma_pending;
    bool ata_dma_pending_write;
    uint32_t ata_dma_cmd_pc;        /* guest PC that issued the DMA command */
    struct intc_desc *ata_intc;
    struct intc_source *ata_intc_src;
    uint64_t ata_irq_raise;
    bool ata_irq_pending;
    QEMUTimer *ata_irq_timer;
    /* always-on completion-edge bookkeeping (ATA summary). */
    uint64_t ata_irq_cmds;          /* commands that scheduled an edge */
    uint64_t ata_irq_drop;          /* fire with the source still asserted */
    uint64_t ata_irq_rearm;         /* pending completion re-armed after hold */
    uint64_t ata_irq_deassert;      /* status/IRQ-ack clears of the line */
    Notifier ata_exit;              /* final summary at QMP quit */
    /* STUB: display-stack interrupt-source slots; see CUP2006_DISP_*. */
    struct intc_source *disp_intc_src[CUP2006_DISP_LINES];
    /* STUB: INTC2 +0x40/+0x44 ack/enable window. */
    MemoryRegion intc2_regs;
    MemoryRegion intc2_a7_regs;
    uint32_t intc2_shadow[CUP2006_INTC2_SIZE / 4];
    /* Sticky enable mask for 0xFFE00044: the contract is set-only, so a
     * later write for another line must not clear an earlier enable. */
    uint32_t intc2_enabled;
    uint64_t intc2_writes;
    uint64_t intc2_other;
    uint64_t disp_ack[CUP2006_DISP_LINES];
    uint64_t disp_done[CUP2006_DISP_LINES];
    uint64_t disp_enable[CUP2006_DISP_LINES];
    /* MBX status/enable/ack and the interrupt-source bookkeeping. */
    uint32_t mbx_status;            /* +0x12C latched status bits, 0 idle */
    uint32_t mbx_arm_seq;           /* +0x838 = 1 arms seen */
    uint32_t mbx_completed_seq;     /* last arm index completed */
    bool mbx_irq_active;            /* last status & enable value != 0 */
    uint64_t mbx_status_done_reads; /* +0x12C reads that saw bit 2 */
    uint64_t mbx_status_done_acks;  /* +0x134 writes that cleared a bit */
    uint64_t mbx_ta_kicks;          /* +0x81C = 1 kicks completed (bit 8) */
    uint64_t mbx_ta_starts;         /* +0x800 = 1 TA starts */
    bool mbx_ta_running;            /* TA started, bit 4 not yet delivered */
    bool mbx_ta_acked;              /* the client acked bit 4 since the start */
    bool mbx_b8_rearm;              /* bit 8 acked: set again at next read */
    unsigned mbx_ta_reads;          /* status reads since the start */
    uint64_t mbx_renders;           /* +0x680 = 1 render starts */
    bool mbx_render_running;        /* render started, bit 2 not delivered */
    uint64_t mbx_irq_raises;        /* status & enable 0 -> 1 edges */
    uint64_t mbx_irq_drops;         /* that edge gone again */
    /* Storage observability counters: the guest-visible event stream is
     * logged a bounded number of times per kind, with a running summary at
     * power-of-two event counts. */
    uint64_t ata_events;
    uint64_t ata_cmd_count;
    uint64_t ata_identify;
    uint64_t ata_read_dma;
    uint64_t ata_read_sectors;
    uint64_t ata_write_dma;
    uint64_t ata_write_sectors;
    uint64_t ata_dma_fail;
    uint64_t ata_other_cmds;
    uint64_t ata_reg_reads;
    uint64_t ata_reg_writes;
    uint64_t ata_summary_at;
    uint8_t ata_cmd_seen[256];
    unsigned ata_read_logged;
    unsigned ata_write_logged;
    unsigned ata_ident_reads;   /* data-port reads since the last IDENTIFY */
    unsigned ata_cmd_logged;
};

typedef struct Cup2006ResetData {
    SuperHCPU *cpu;
    uint32_t vector;
} Cup2006ResetData;

static uint32_t cup2006_guest_pc(void)
{
    CPUState *cs = current_cpu;

    return (cs && cs->cc->get_pc) ? (uint32_t)cs->cc->get_pc(cs) : 0;
}

static uint64_t cup2006_mmio_read(hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: mmio read  pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x0\n",
                  cup2006_guest_pc(), addr, size);
    return 0;
}

static void cup2006_mmio_write(hwaddr addr, uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "cup2006: mmio write pc=0x%08" PRIx32
                  " addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                  "\n",
                  cup2006_guest_pc(), addr, size, value);
}

static uint64_t cup2006_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    return cup2006_mmio_read(addr, size);
}

static void cup2006_unknown_write(void *opaque, hwaddr addr, uint64_t value,
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

static uint64_t cup2006_board_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_REG_BASE + addr;

    cup2006_board_note(s, phys, size, false);
    return cup2006_mmio_read(phys, size);
}

static void cup2006_board_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_REG_BASE + addr;

    cup2006_board_note(s, phys, size, true);
    cup2006_mmio_write(phys, value, size);
}

static uint64_t cup2006_board_a7_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_A7_BASE + addr;

    cup2006_board_note(s, phys, size, false);
    return cup2006_mmio_read(phys, size);
}

static void cup2006_board_a7_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Cup2006MachineState *s = opaque;
    hwaddr phys = CUP2006_BOARD_A7_BASE + addr;

    cup2006_board_note(s, phys, size, true);
    cup2006_mmio_write(phys, value, size);
}

/* Little-endian register access inside a 4-byte aligned word. */
static uint32_t cup2006_reg32_read(const uint8_t *base, hwaddr addr,
                                   unsigned size)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < size && i < 4; i++) {
        value |= (uint32_t)base[addr + i] << (8 * i);
    }
    return value;
}

static void cup2006_reg32_write(uint8_t *base, hwaddr addr, uint64_t value,
                                unsigned size)
{
    unsigned i;

    for (i = 0; i < size && i < 4; i++) {
        base[addr + i] = (value >> (8 * i)) & 0xff;
    }
}

#include "cup2006-dvd.c.inc"
#include "cup2006-board.c.inc"
#include "cup2006-gps.c.inc"
#include "cup2006-rcd-gpio.c.inc"
#include "cup2006-du.c.inc"
#include "cup2006-2dg.c.inc"
#include "cup2006-mbx.c.inc"
#include "cup2006-flash.c.inc"
#include "cup2006-soc.c.inc"
#include "cup2006-subcpu.c.inc"
#include "cup2006-carsig.c.inc"
#include "cup2006-ata.c.inc"
#include "cup2006-audio.c.inc"

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
        trace_cup2006_board_mbx_render_table();
        for (unsigned r = 0x608; r <= 0x6e8; r += 4) {
            trace_cup2006_board_event_3(r, s->mbx_window[r >> 2]);
        }
        trace_cup2006_board_event_4();
    }
    if (s->mbx_marker_frames <= 2 || (s->mbx_marker_frames % 100) == 0) {
        /* Trace events take no floats: coordinates become milli-units. */
        trace_cup2006_board_stub_mbx_marker_triangles(nt, (int32_t)lrint(vp[0] * 1000), (int32_t)lrint(vp[1] * 1000), (int32_t)lrint(vp[2] * 1000), (int32_t)lrint(vp[3] * 1000), nt ? (int32_t)lrint(tri[0].sx[0] * 1000) : 0, nt ? (int32_t)lrint(tri[0].sy[0] * 1000) : 0);
    }
}

/* Called at +0x680 render start: dump (optionally) and reset the frame. */
static void cup2006_mbx_ta_frame(Cup2006MachineState *s)
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
        s->drf_erases = 0;
        s->drf_program_pending = false;
        s->drf_program_addr = 0;

        /* The whole 32 MB window is one flash part, mapped ROMD: the XIP
         * kernel's fetches are served from the array, every write reaches the
         * AMD command machine, and the -kernel image is loaded into it. */
        memory_region_init_rom_device(&s->drf_regs, NULL,
                                                &cup2006_drf_ops, s,
                                                "cup2006-drf-flash",
                                                CUP2006_DRF_SIZE,
                                                &error_fatal);
        s->drf_data = memory_region_get_ram_ptr(&s->drf_regs);
        memset(s->drf_data, 0xff, CUP2006_DRF_SIZE);
        s->drf_fd = -1;
        s->drf_dirty_lo = CUP2006_DRF_SIZE;
        s->drf_dirty_hi = 0;
        s->drf_image_full_write = false;
        if (s->drfimage && s->drfimage[0]) {
            struct stat st;
            ssize_t n;

            s->drf_fd = open(s->drfimage, O_RDWR | O_CREAT, 0644);
            if (s->drf_fd < 0) {
                error_report("cup2006: could not open drfimage '%s': %s",
                             s->drfimage, strerror(errno));
                exit(1);
            }
            if (fstat(s->drf_fd, &st) != 0) {
                error_report("cup2006: could not stat drfimage '%s': %s",
                             s->drfimage, strerror(errno));
                exit(1);
            }
            if (st.st_size > 0) {
                hwaddr len = MIN((uint64_t)st.st_size,
                                 (uint64_t)CUP2006_DRF_SIZE);
                n = pread(s->drf_fd, s->drf_data, len, 0);
                if (n != (ssize_t)len) {
                    error_report("cup2006: could not read drfimage '%s': %s",
                                 s->drfimage, strerror(errno));
                    exit(1);
                }
                /* A short image leaves the rest erased (0xff); the first
                 * save rewrites the whole array so later loads are exact. */
                s->drf_image_full_write = (st.st_size < CUP2006_DRF_SIZE);
            } else {
                s->drf_image_full_write = true;
            }
            s->drf_exit.notify = cup2006_drf_exit;
            qemu_add_exit_notifier(&s->drf_exit);
            s->drf_save_timer =
                timer_new_ms(QEMU_CLOCK_REALTIME, cup2006_drf_save_timer, s);
            timer_mod(s->drf_save_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                      CUP2006_DRF_SAVE_MS);
            trace_cup2006_board_drfimage_bytes_on_disk(s->drfimage, (int64_t)st.st_size, s->drf_image_full_write ? "will be initialised"
                                                : "loaded");
        }
        memory_region_add_subregion_overlap(sysmem, CUP2006_DRF_BASE,
                                            &s->drf_regs, 1);
        trace_cup2006_board_drf_flash_window_at(CUP2006_DRF_BASE, CUP2006_DRF_BASE + CUP2006_DRF_SIZE - 1);
    }

    cup2006_sramsave_init(s);

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
     * control/doorbell window at PA 0x1FDF0000. */
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
    if (s->gpstrack && s->gpstrack[0]) {
        struct intc_desc *desc = cpu->env.intc_handle;

        cup2006_gps_load_track(s);
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
        /* The image is the flash part's shipped content: write the array
         * directly (physical_memory_write would be absorbed by the ROMD
         * command handler). */
        memcpy(s->drf_data + load_addr, data, size);
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
        "file holding the onboard 93C46 EEPROM (128 bytes, big-endian words); "
        "created on the first guest write, all zero before (default: none)");

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
