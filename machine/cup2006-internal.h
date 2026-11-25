/*
 * Volvo CU-P2006 board: internal header shared by cup2006.c and the
 * per-device cup2006-*.c files.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef CUP2006_INTERNAL_H
#define CUP2006_INTERNAL_H

#include "qemu/osdep.h"
#include "qemu/units.h"
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "qemu/error-report.h"
#include "melco-nvram.h"
#include "melco-amdflash.h"
#include "melco-sensors.h"
#include "qobject/qdict.h"
#include "system/block-backend.h"
#include "system/block-backend-global-state.h"
#include "block/block.h"
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

#define TYPE_CUP2006_MACHINE MACHINE_TYPE_NAME("cup2006")
OBJECT_DECLARE_SIMPLE_TYPE(Cup2006MachineState, CUP2006_MACHINE)

/* Defines used by more than one file. */
#define CUP2006_GPIO_ACC_BIT     19
#define CUP2006_PCC_CIS_OFF     0x00100000
#define CUP2006_DVD5_SIZE        0x00000200
#define CUP2006_DVD5_WIN_SIZE    0x00001000
#define CUP2006_DVDST_SIZE       0x00001000
#define CUP2006_BOARD_LOG_MAX 256

#define CUP2006_SRAM_SIZE       0x00010000
#define CUP2006_SELFTEST_BASE   0xff93c000
#define CUP2006_ADC_CSR         0x10
#define CUP2006_ADC_ADF         0x8000    /* conversion end */
#define CUP2006_ADC_ADST        0x2000    /* start / busy */
#define CUP2006_I2C_BASE          0xff907000
#define CUP2006_I2C_A7_BASE       0x1f907000
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
#define CUP2006_I2C_XFER_NS       100000LL
#define CUP2006_GPS_BASE         0xff93d000
#define CUP2006_GPS_A7_BASE      0x1f93d000
#define CUP2006_GPS_SIZE         0x00001000
#define CUP2006_GPS_CMD          0x004
#define CUP2006_GPS_STATUS       0x00c
#define CUP2006_GPS_STATUS_READY 0x00000001
#define CUP2006_GPS_CTRL         0x000
#define CUP2006_GPS_CTRL_DOORBELL 0x00010000
#define CUP2006_GPS_CTRL_IRQEN   0x00020000
#define CUP2006_GPS_LATCH        0x010
#define CUP2006_GPS_CAUSE        0x014
#define CUP2006_GPS_RX           0x800
#define CUP2006_GPS_INTEVT       0x08a0
#define CUP2006_GPS_TICK_MS      500
#define CUP2006_RCD_BASE         0xff937000
#define CUP2006_RCD_A7_BASE      0x1f937000
#define CUP2006_RCD_SIZE         0x00000100
#define CUP2006_RCD_RECORD       0x02c
#define CUP2006_POWER_INTEVT     0x0240
#define CUP2006_POWER_SYSINTR    0x1d
#define CUP2006_POWER_INTC_PRIO  2
#define CUP2006_GPIO_ACC_PORT    0
#define CUP2006_GPIO_ACC_PIN     (1u << CUP2006_GPIO_ACC_BIT)
#define CUP2006_GPIO_BASE        0xff93e000
#define CUP2006_GPIO_A7_BASE     0x1f93e000
#define CUP2006_GPIO_PORT_STRIDE 0x00001000
#define CUP2006_GPIO_PORTS       4
#define CUP2006_GPIO_DIR         0x004
#define CUP2006_GPIO_DATA        0x008
#define CUP2006_GPIO_INPUT       0x00c
#define CUP2006_GPIO_HDD_PORT    3
#define CUP2006_GPIO_HDD_BIT     10
#define CUP2006_GPIO_HDD_PIN     (1u << CUP2006_GPIO_HDD_BIT)
#define CUP2006_GPIO_SHUTTER_PIN (1u << 8)
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
#define CUP2006_MBX_TA_KICK      0x0000081c
#define CUP2006_MBX_ST_TA_KICK   0x00000100  /* +0x12C bit 8, polled only */
#define CUP2006_MBX_TA_START     0x00000800
#define CUP2006_MBX_ST_POLLED    (CUP2006_MBX_ST_TA_KICK | CUP2006_MBX_ST_TA_DONE)
#define CUP2006_MBX_RENDER_START 0x00000680
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
#define CUP2006_DU_PNMR_PNDC     0x00000080
#define CUP2006_DU_PNMR_PNBM     0x00000030
#define CUP2006_DU_DSSR_DFB(n)   (1u << (15 + (n)))
#define CUP2006_DU_PNTC2R        0x048
#define CUP2006_DU_PLANES        6
#define CUP2006_DU_DDF_8BPP      0
#define CUP2006_DU_DDF_16BPP     1
#define CUP2006_DU_DDF_ARGB      2
#define CUP2006_DU_DDF_YC        3
#define CUP2006_DU_DSYSR_DEN     0x00000100
#define CUP2006_DU_DSSR_TVR      0x00008000
#define CUP2006_DU_DSSR_FRM      0x00004000
#define CUP2006_DU_DSSR_VBK      0x00000800
#define CUP2006_DU_DSSR_RINT     0x00000200
#define CUP2006_DU_DSSR_HBK      0x00000100
#define CUP2006_DU_DSSR_RESERVED 0x30000000  /* bits 29,28 read as 1 */
/* The enables mirror the DSSR flag bits (TVR 15, FRM 14, VBK 11, RINT 9,
 * HBK 8), so a guest that enables only one flag gets exactly that one. */
#define CUP2006_DU_DIER_MASK     0x0000cb00
#define CUP2006_DU_FRAME_NS      100000000ull /* STUB: 10 Hz frame tick */
#define CUP2006_DU_FLAG_HOLD_NS  5000000ull
#define CUP2006_DU_CONSOLE_W     400
#define CUP2006_DU_CONSOLE_H     234
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
#define CUP2006_DG_INTC2_ENABLE  0x00100000u
#define CUP2006_DG_SIZE          0x00001000
#define CUP2006_CPG_BASE        0xffc00000
#define CUP2006_RTC_BASE        0xffc80000
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
#define CUP2006_SCIF0_BASE       0xff923000
#define CUP2006_SCIF0_A7_BASE    0x1f923000
#define CUP2006_SCIF0_SCSCR      0x0008
#define CUP2006_SCIF0_SCFTDR     0x000c
#define CUP2006_SCIF0_SCFSR      0x0010
#define CUP2006_SCIF0_SCFRDR     0x0014
#define CUP2006_SCIF0_SCFDR      0x001c
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
#define CUP2006_CARSIG_GYRO       0x1048
#define CUP2006_CARSIG_AUX        0x1018
#define CUP2006_GYRO_ZERO         8192
#define CUP2006_GYRO_PER_DPS      81.9
#define CUP2006_CARSIG_DEFAULT_US 50000   /* STUB: if +0x0000 is 0 */
#define CUP2006_SCIF0_INTEVT     0x09a0
#define CUP2006_SCIF0_SYSINTR    30
#define CUP2006_SCIF0_INTC_PRIO  2
#define CUP2006_SCIF0_SCSCR_RIE  0x0040
#define CUP2006_SCIF0_SCSCR_TIE  0x0080
#define CUP2006_SCIF0_SCFSR_TDFE 0x0020
#define CUP2006_SCIF0_SCFSR_TEND 0x0040
#define CUP2006_UART_BASE           0x1b800000
#define CUP2006_UART_SIZE           0x00000010
#define CUP2006_ATA_SIZE        0x00001000
#define CUP2006_ATA_DATA        0x000
#define CUP2006_ATA_COMMAND     0x01c
#define CUP2006_ATA_STATUS      0x038
#define CUP2006_ATA_ALT_STATUS  0x03c
#define CUP2006_ATA_DMA_STATUS  0x084
#define CUP2006_ATA_DMA_ADDR    0x09c
#define CUP2006_ATA_DMA_LEN     0x0a0
#define CUP2006_ATA_IRQ_ACK     0x088   /* completion ISR writes 0 here */
#define CUP2006_ATA_INTEVT      0x0860
#define CUP2006_DISP_LINES       3
#define CUP2006_DISP_INTEVT_DTD0 0x08c0   /* SYSINTR 0x11 */
#define CUP2006_DISP_INTEVT_DTD1 0x0780   /* SYSINTR 0x12 */
#define CUP2006_DISP_INTEVT_MBX  0x0900   /* SYSINTR 0x1A */
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
#define CUP2006_DVD5_BASE        0xff801700
#define CUP2006_DVD5_KICK        0x110      /* 0x1F801810 */
#define CUP2006_DVD5_KICK_VALUE  0x40000000
#define CUP2006_DVD5_REPLY_VA    0x01f891a0
#define CUP2006_DVD5_ST_ROM      0x48
#define CUP2006_DVD5_TYPE_DVDROM 0x04
#define CUP2006_DVD5_DMA_DAR     0x084
#define CUP2006_DVD5_DMA_COUNT   0x088
#define CUP2006_DVD5_DMA_INTC_PRIO 2
#define CUP2006_SSI_BASE         0xff911000
#define CUP2006_SSI_A7_BASE      0x1f911000
#define CUP2006_SSI_SIZE         0x10
#define CUP2006_ADMA_BASE        0xff801040
#define CUP2006_ADMA_A7_BASE     0x1f801040
#define CUP2006_ADMA_SIZE        0x38
#define CUP2006_DMAC_STATUS      0x10c      /* 0xFF80180C */
#define CUP2006_DMAC_ENABLE      0x114      /* 0xFF801814 */
#define CUP2006_ADMA_BIT         0x00000002
#define CUP2006_ADMA_INTEVT      0x0bc0
#define CUP2006_ADMA_INTC_VECT   0x0660
#define CUP2006_ADMA_INTC_PRIO   7
#define CUP2006_DVD5_INTEVT      0x0b40
#define CUP2006_DVD5_INTC2_BIT   0x10000000 /* 0xFFE00044 bit 28 */
#define CUP2006_DVD5_ST_DATA     0x08       /* ISR's data-byte read */
#define CUP2006_DVD5_RX_MAX      48
#define CUP2006_DVD5_INTC_PRIO   3
#define CUP2006_DVDST_BASE       0xffa02000
#define CUP2006_PCC_BASE        0x1b000000
#define CUP2006_PCC_SLOTS       2
#define CUP2006_PCC_SLOT_SIZE   0x00400000
#define CUP2006_PCC_REGS_OFF    0x003fffe0
#define CUP2006_PCC_REGS_SIZE   0x00000020
#define CUP2006_PCC_STATUS      0x08
#define CUP2006_PCC_STATUS_VALUE 0x0120
#define CUP2006_PCC_CIS_OFFS    { 0x00000000, CUP2006_PCC_CIS_OFF }
#define CUP2006_PCC_CIS_BASES   2
#define CUP2006_PCC_CIS_SIZE    0x00000400
#define CUP2006_SCIF_LOG_MAX  32
#define CUP2006_GPS_LOG_MAX   32
#define CUP2006_MBX_LOG_MAX   32
#define CUP2006_RCD_LOG_MAX   32
#define CUP2006_GPIO_LOG_MAX  32
#define CUP2006_WHEEL_PERIOD_MS  100

typedef struct Cup2006PccRegs {
    Cup2006MachineState *s;
    unsigned slot;
} Cup2006PccRegs;
typedef struct Cup2006PccCis {
    Cup2006MachineState *s;
    unsigned base;              /* index into pcc_cis_seen[] / log counters */
} Cup2006PccCis;
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
    bool dvd5_req_staged;     /* a request frame waits for the doorbell */
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
    /* The drive's eject button and disc slot (F11; cup2006-dvd.c). */
    bool dvd5_ejected;       /* the open disc sits ejected in the slot */
    bool dvd_shutter_open;   /* F10: the disc-insert shutter is open */
    char **dvd_discs;        /* dvdimage= then each dvdnext= disc */
    unsigned dvd_ndiscs;
    unsigned dvd_next;       /* index of the next disc to insert */
    QEMUTimer *dvd_insert_timer;
    /* Sound: SSI + its DMA channel (cup2006-audio.c). */
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
    MelcoAmdFlash *drf;
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
    MelcoTrack *track;             /* car-sensor track (melco-sensors.c) */
    bool track_clock_started;      /* the scenario clock start was taken, so
                                    * the wheel and gyro feeds may read the
                                    * track; shared with the GNSS radio replay
                                    * (gnssreplay=) */
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
    uint64_t dg_bounds;
    uint64_t dg_work_ops;           /* work-plane pixel ops in this job */
    uint64_t dg_frame_ops;          /* frame-plane pixel ops in this job */
    int64_t draw_t0_ms;                /* 2DG log timestamps */
    struct {
        bool valid;
        uint32_t base, w, h, stride, fmt;
    } du_reported[CUP2006_DU_PLANES + 1];
    char *drfimage;             /* raw 32 MB backing file of the flash */
    MemoryRegion bkram;          /* bkram=: battery RAM (rev >= 3) */
    MemoryRegion sramsave_mr;    /* sramsave=: battery SRAM over top of RAM */
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

/* Data shared across files. */
extern const MemoryRegionOps cup2006_ata_ops;
extern const MemoryRegionOps cup2006_board_a7_ops;
extern const MemoryRegionOps cup2006_board_ops;
extern const MemoryRegionOps cup2006_carsig_a7_ops;
extern const MemoryRegionOps cup2006_carsig_ops;
extern const MemoryRegionOps cup2006_cpg_ops;
extern const MemoryRegionOps cup2006_dg_a7_ops;
extern const MemoryRegionOps cup2006_dg_ops;
extern const uint16_t cup2006_disp_intevt[CUP2006_DISP_LINES];
extern const MemoryRegionOps cup2006_du_ops;
extern const MemoryRegionOps cup2006_dvd5_ops;
extern const MemoryRegionOps cup2006_dvd5_rpl_ops;
extern const MemoryRegionOps cup2006_dvd5_win_ops;
extern const MemoryRegionOps cup2006_dvdst_ops;
extern const MemoryRegionOps cup2006_gpio_a7_ops;
extern const MemoryRegionOps cup2006_gpio_ops;
extern const MemoryRegionOps cup2006_gps_a7_ops;
extern const MemoryRegionOps cup2006_gps_ops;
extern const MemoryRegionOps cup2006_i2c_a7_ops;
extern const MemoryRegionOps cup2006_i2c_ops;
extern const MemoryRegionOps cup2006_intc2_a7_ops;
extern const MemoryRegionOps cup2006_intc2_ops;
extern const QemuInputHandler cup2006_key_handler;
extern const MemoryRegionOps cup2006_mbx_a7_ops;
extern const MemoryRegionOps cup2006_mbx_mailbox_ops;
extern const MemoryRegionOps cup2006_mbx_ops;
extern const uint8_t cup2006_pcc_cis[71];
extern const MemoryRegionOps cup2006_pcc_cis_ops;
extern const MemoryRegionOps cup2006_pcc_regs_ops;
extern const MemoryRegionOps cup2006_rcd_a7_ops;
extern const MemoryRegionOps cup2006_rcd_ops;
extern const MemoryRegionOps cup2006_rtc_ops;
extern const MemoryRegionOps cup2006_scif0_a7_ops;
extern const MemoryRegionOps cup2006_scif0_ops;
extern const MemoryRegionOps cup2006_scif_a7_ops;
extern const MemoryRegionOps cup2006_scif_ops;
extern const MemoryRegionOps cup2006_selftest_ops;
extern const MemoryRegionOps cup2006_uart_ops;
extern const MemoryRegionOps cup2006_unknown_ops;

/* Functions called across files (non-static, cup2006_ prefix kept). */
hwaddr cup2006_phys_page_debug(CPUState *cpu, vaddr addr);
void cup2006_adma_ack(Cup2006MachineState *s);
void cup2006_ata_exit(Notifier *n, void *data);
void cup2006_ata_irq_fire(void *opaque);
void cup2006_audio_init(Cup2006MachineState *s, MachineState *ms, MemoryRegion *sysmem, SuperHCPU *cpu);
void cup2006_bkram_init(Cup2006MachineState *s, MemoryRegion *sysmem);
uint64_t cup2006_board_a7_read(void *opaque, hwaddr addr, unsigned size);
void cup2006_board_a7_write(void *opaque, hwaddr addr, uint64_t value, unsigned size);
uint64_t cup2006_board_read(void *opaque, hwaddr addr, unsigned size);
void cup2006_board_write(void *opaque, hwaddr addr, uint64_t value, unsigned size);
void cup2006_carsig_tick(void *opaque);
uint8_t *cup2006_dg_ram_ptr(Cup2006MachineState *s, uint32_t addr, uint32_t len);
void cup2006_display_create(Cup2006MachineState *s);
void cup2006_du_flag_off(void *opaque);
void cup2006_du_frame_tick(void *opaque);
void cup2006_dvd5_irq_ack(Cup2006MachineState *s);
void cup2006_dvd5_host_doorbell(Cup2006MachineState *s);
void cup2006_dvd5_spinup(Cup2006MachineState *s);
void cup2006_dvd5_write(void *opaque, hwaddr addr, uint64_t value, unsigned size);
void cup2006_dvd_eject_button(Cup2006MachineState *s);
void cup2006_dvd_insert(void *opaque);
void cup2006_dvd_shutter_key(Cup2006MachineState *s);
void cup2006_eeprom_cs(Cup2006MachineState *s, bool level);
void cup2006_eeprom_load(Cup2006MachineState *s);
void cup2006_gps_note(Cup2006MachineState *s, hwaddr phys, bool is_write, unsigned size, uint64_t value);
void cup2006_gps_tick(void *opaque);
uint32_t cup2006_guest_pc(void);
void cup2006_i2c_complete(void *opaque);
void cup2006_intc2_write_at(Cup2006MachineState *s, hwaddr addr, uint64_t value, unsigned size);
void cup2006_key_pump(void *opaque);
void cup2006_mbx_note(Cup2006MachineState *s, hwaddr phys, bool is_write, unsigned size, uint64_t value);
uint64_t cup2006_mbx_read(Cup2006MachineState *s, hwaddr base, hwaddr addr, unsigned size);
void cup2006_mbx_ta_frame(Cup2006MachineState *s);
void cup2006_mbx_update_irq(Cup2006MachineState *s, const char *why);
uint64_t cup2006_mmio_read(hwaddr addr, unsigned size);
void cup2006_mmio_write(hwaddr addr, uint64_t value, unsigned size);
uint64_t cup2006_pcc_regs_read(void *opaque, hwaddr addr, unsigned size);
void cup2006_power_drop(void *opaque);
void cup2006_power_pulse(void *opaque);
uint32_t cup2006_reg32_read(const uint8_t *base, hwaddr addr, unsigned size);
void cup2006_reg32_write(uint8_t *base, hwaddr addr, uint64_t value, unsigned size);
void cup2006_scif0_peer_pump(void *opaque);
void cup2006_scif0_tx_tick(void *opaque);
void cup2006_scif_rx_tick(void *opaque);
void cup2006_sramsave_init(Cup2006MachineState *s, MemoryRegion *sysmem);
void cup2006_track_clock_start(Cup2006MachineState *s, int64_t now_ms);
bool cup2006_gps_enabled(const Cup2006MachineState *s);
uint64_t cup2006_unknown_read(void *opaque, hwaddr addr, unsigned size);
void cup2006_unknown_write(void *opaque, hwaddr addr, uint64_t value, unsigned size);
void cup2006_wheel_tick(void *opaque);

#endif /* CUP2006_INTERNAL_H */
