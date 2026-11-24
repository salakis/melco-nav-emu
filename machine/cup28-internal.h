/*
 * Volvo VNS CUP28 board: internal header shared by cup28.c and the
 * per-device cup28-*.c files (SPDX: GPL-2.0-or-later).
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef CUP28_INTERNAL_H
#define CUP28_INTERNAL_H

#include "qemu/osdep.h"
#include "qemu/units.h"
#include <math.h>
#ifdef CONFIG_POSIX
#include <sys/file.h>
#endif
#include "qemu/audio.h"
#include "melco-nvram.h"
#include "melco-amdflash.h"
#include "melco-sensors.h"
#include "qobject/qdict.h"
#include "system/block-backend.h"
#include "system/block-backend-global-state.h"
#include "block/block.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/sh4/sh.h"
#include "migration/blocker.h"
#include "trace.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/sh4/cpu.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"

#define TYPE_CUP28_MACHINE MACHINE_TYPE_NAME("cup28")
OBJECT_DECLARE_SIMPLE_TYPE(Cup28MachineState, CUP28_MACHINE)

/* Defines used by more than one file. */
#define CUP28_FLASH_SIZE (32 * MiB)
#define CUP28_EEPROM_BASE 0x07800000
#define CUP28_EEPROM_SIZE 0x00800000
#define CUP28_RTC_BASE        0xffc80000
#define CUP28_RTC_A7_BASE     0x1fc80000
#define CUP28_RTC_SIZE        0x00000100
#define CUP28_RTC_EPOCH       1252487349  /* 2009-09-09 09:09:09 UTC */

struct Cup28MachineState {
    MachineState parent_obj;

    bool powered;
    char *dvdimage, *drfimage, *sramsave, *gpstrack;
    Error *migration_blocker;
    MemoryRegion ram, drf_alias_regs;
    MelcoAmdFlash *drf;
    MemoryRegion selftest_regs, rtc_regs, rtc_a7_regs;
    QemuConsole *disp_con;

    int dvd_fd;
    uint64_t dvd_size;
    uint8_t dvd5_rx[32];
    unsigned dvd5_rx_len, dvd5_rx_pos;
    bool dvd5_spinup_pending, dvd5_disc_ready;
    struct intc_source *dvd5_intc_src, *dvd5_intc_src2;


    int64_t rtc_base_s, rtc_base_ms;
    uint16_t rtc_set[6];

    MelcoTrack *track;          /* car-sensor track (melco-sensors.c) */
    MemoryRegion p28_uart_container;
    MemoryRegion p28_q2_regs;
    uint8_t p28_q2_reg[0x800];
    MemoryRegion p28_q2_vram;
    uint8_t *p28_q2_mem;
    QEMUTimer *p28_q2_timer;
    bool p28_q2_list_done;
    uint8_t p28_q2_work[1024 * 1024 / 8]; /* Q2 binary work plane */
    struct intc_source *p28_q2_intc_src;
    MemoryRegion p28_dvd_ports, p28_dvd_control;
    MemoryRegion p28_audio_regs;
    uint32_t p28_audio_reg[2][5];
    QEMUTimer *p28_audio_timer;
    Notifier audio_shutdown;
    AudioBackend *p28_audio_backend;
    SWVoiceOut *p28_audio_out;
    bool p28_audio_second_buffer;
    bool p28_audio_kiwi;
    bool p28_audio_streaming;
    int32_t p28_audio_history[2];
    uint32_t p28_audio_dma_address;
    size_t p28_audio_dma_bytes, p28_audio_dma_remaining;
    uint8_t p28_audio_pcm[MiB];
    size_t p28_audio_pcm_pos, p28_audio_pcm_len;
    uint64_t p28_audio_frames;
    MemoryRegion p28_dmac_io, p28_dmac_a7;
    uint8_t p28_dmac_reg[0x100];
    struct intc_source *p28_dmac_src;
    MemoryRegion p28_most_io;
    struct intc_source *p28_most_src;
    struct intc_source *p28_most_chip_src, *p28_most_async_src;
    QemuInputHandlerState *most_input;
    uint32_t p28_most_control;
    uint16_t p28_most_rx;
    uint16_t p28_most_next_rx;
    bool p28_most_shift_pending, p28_most_rx_ready;
    QEMUTimer *p28_most_timer;
    uint8_t p28_most_registers[4][256];
    uint8_t p28_most_page, p28_most_map, p28_most_phase;
    bool p28_most_reading;
    unsigned p28_most_vc_sent;
    uint8_t p28_most_q[16][17];
    uint8_t p28_most_q_len[16];
    unsigned p28_most_q_head, p28_most_q_count;
    /* Peer-side hold-back queue: while the node FIFO is full, the emulated
     * vehicle peer keeps its own messages here and delivers them as the
     * guest drains, instead of dropping them. */
    uint8_t p28_most_peer_q[64][17];
    uint8_t p28_most_peer_q_len[64];
    unsigned p28_most_peer_q_head, p28_most_peer_q_count;
    bool p28_most_gps_on, p28_most_navsys_listed;
    unsigned p28_most_gps_log;
    uint8_t p28_most_gps_last_position[8];
    bool p28_most_gps_valid;
    bool p28_most_wheel_on, p28_most_vc_complete;
    uint8_t p28_most_wheel_counter;
    QEMUTimer *p28_most_gps_timer;
    /* CUP28 GPS receiver UART (cup28-gps.c.inc) */
    MemoryRegion p28_gps_io;
    struct intc_source *p28_gps_tx_src;
    uint8_t p28_gps_ier, p28_gps_lcr, p28_gps_mcr, p28_gps_scr;
    uint8_t p28_gps_dll, p28_gps_dlm;
    /* CUP28 dead-reckoning sampler (cup28-gps.c.inc) */
    struct intc_source *p28_sns_src;
    QEMUTimer *p28_sns_timer;
    MemoryRegion p28_pulse_io;
    double p28_sns_pulse_fraction;
    bool p28_most_power_on;
    uint8_t p28_dvd_control_reg[0x200];
    uint8_t p28_dvd_tx[24];
    unsigned p28_dvd_tx_len;
    uint8_t *p28_dvd_stream;
    unsigned p28_dvd_stream_len;
    bool p28_dvd_rx_ready;
    bool p28_dvd_tx_wait;
    int64_t p28_dvd_rx_due;
    QEMUTimer *p28_dvd_timer;
    MemoryRegion p28_eeprom_regs, p28_eeprom_bank;
    uint32_t p28_eeprom_reg[0x40];
    uint8_t *p28_eeprom_mem;
    /* earlier-board backup SRAM / parallel EEPROM windows. */
    MemoryRegion p28_bksram, p28_bkeeprom;
    /* flr.dll's 256 KiB FLR window at physical 0x01a00000. */
    MemoryRegion p28_flrwin;
    uint8_t *p28_flr_mem;
};

/* Data shared across files. */
extern const MemoryRegionOps cup28_drf_alias_ops;
extern const MemoryRegionOps cup28_rtc_ops;
extern const MemoryRegionOps cup28_dvd_port_ops;
extern const MemoryRegionOps cup28_dvd_ctrl_ops;
extern const MemoryRegionOps cup28_eeprom_ops;
extern const MemoryRegionOps cup28_flr_ops;
extern const MemoryRegionOps cup28_q2_ops;

/* Functions called across files (non-static, cup28_ prefix kept). */
void cup28_audio_init(Cup28MachineState *s, MemoryRegion *mem,
                       const char *audiodev);
void cup28_bksram_init(Cup28MachineState *s);
void cup28_dmac_init(Cup28MachineState *s, MemoryRegion *sysmem,
                     SuperHCPU *cpu);
void cup28_dmac_try(Cup28MachineState *s);
void cup28_dvd_irq(Cup28MachineState *s);
void cup28_gps_init(Cup28MachineState *s, MemoryRegion *mem, SuperHCPU *cpu);
void cup28_gps_irq(Cup28MachineState *s);
void cup28_most_chip_irq(Cup28MachineState *s);
void cup28_most_gpio(Cup28MachineState *s, uint32_t old, uint32_t value);
void cup28_most_init(Cup28MachineState *s, MemoryRegion *mem, SuperHCPU *cpu);
void cup28_most_irq(Cup28MachineState *s);
void cup28_most_wheel_send(Cup28MachineState *s);
void cup28_q2_irq(Cup28MachineState *s);
void cup28_q2_irq_init(Cup28MachineState *s, SuperHCPU *cpu);
uint64_t cup28_register_read(const uint8_t *regs, hwaddr addr, unsigned size);
void cup28_register_write(uint8_t *regs, hwaddr addr, uint64_t value,
                          unsigned size);
void cup28_sns_init(Cup28MachineState *s, MemoryRegion *mem, SuperHCPU *cpu);
void cup28_sns_irq(Cup28MachineState *s);
void cup28_dvd_tick(void *opaque);
void cup28_q2_tick(void *opaque);

#endif /* CUP28_INTERNAL_H */
