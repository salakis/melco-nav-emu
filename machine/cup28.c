/*
 * Volvo VNS CUP28 board.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "cup28-internal.h"


#define CUP28_FLASH_BASE 0
#define CUP28_FLASH_LOG_MAX 256
#define CUP28_FLASH_SECTOR (256 * KiB)
#define CUP28_FIRMWARE_OFFSET 0x80000

uint64_t cup28_register_read(const uint8_t *regs, hwaddr addr,
                                    unsigned size)
{
    uint64_t value = 0;

    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)regs[addr + i] << (8 * i);
    }
    return value;
}

void cup28_register_write(uint8_t *regs, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        regs[addr + i] = value >> (8 * i);
    }
}


/* STUB: the M-loader's own entry point is not modelled, so a CPU reset starts
 * the guest's firmware entry (0x80080004 = LOAD_ADDRESS + 4) directly. */
static void cup28_reset_cpu(void *opaque)
{
    SuperHCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    cpu->env.pc = 0x80080004;
}

/* The board devices (MOST peer, DVD, audio, flash autoselect) have no saved
 * state and keep theirs across a CPU-only reset, so a rebooted guest would
 * never see the MOST peer's handshake again.  On the real unit a reset is a
 * power cycle: model that as switching the unit off, as cup2006 does. */
static void cup28_machine_reset(MachineState *machine, ResetType type)
{
    Cup28MachineState *s = CUP28_MACHINE(machine);

    if (s->powered) {
        warn_report("cup28: reset is a power cycle of the whole unit;"
                    " powering off (start QEMU again)");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    s->powered = true;
    qemu_devices_reset(type);
}

/* STUB: the board self-test reports a fixed ADC sample and ready status. */
static uint64_t cup28_selftest_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x04:
        return 0x0200;
    case 0x10:
        return 0xa800;
    default:
        return 0;
    }
}

static void cup28_selftest_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
}

static const MemoryRegionOps cup28_selftest_ops = {
    .read = cup28_selftest_read,
    .write = cup28_selftest_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool cup28_draw(void *opaque)
{
    Cup28MachineState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->disp_con);

    if (surface) {
        qemu_console_update(s->disp_con, 0, 0, surface_width(surface),
                            surface_height(surface));
    }
    return true;
}

static const GraphicHwOps cup28_console_ops = { .gfx_update = cup28_draw };

static void cup28_flash_init(Cup28MachineState *s, MemoryRegion *mem,
                             const char *kernel)
{
    g_autofree char *data = NULL;
    g_autoptr(GError) error = NULL;
    gsize length;
    BlockBackend *blk = NULL;
    bool have_image = false;

    if (s->drfimage && *s->drfimage) {
        QDict *options;
        int fd = open(s->drfimage, O_RDWR | O_CREAT | O_BINARY, 0600);

        if (fd < 0) {
            error_report("cup28: cannot open flash '%s': %s", s->drfimage,
                         strerror(errno));
            exit(1);
        }
        close(fd);
        /* drfimage=<file> becomes the device's block backend, raw, RW. */
        options = qdict_new();
        qdict_put_str(options, "driver", "raw");
        blk = blk_new_open(s->drfimage, NULL, options,
                         BDRV_O_RDWR | BDRV_O_RESIZE,
                           &error_fatal);
        have_image = blk_getlength(blk) > 0;
    }

    /* 16-bit bus: commands carry the byte in every other lane. */
    s->drf = melco_amdflash_create("cup28-flash", CUP28_FLASH_SIZE, CUP28_FLASH_SECTOR, 2,
                                   0x0001, 0x227e, blk);
    memory_region_add_subregion_overlap(mem, CUP28_FLASH_BASE,
                                        melco_amdflash_mmio(s->drf), 1);
    if (blk && blk_getlength(blk) < CUP28_FLASH_SIZE) {
        /* A short or empty image leaves the rest of the array erased: grow
         * the file to the array size and persist the array once. */
        blk_truncate(blk, CUP28_FLASH_SIZE, true, PREALLOC_MODE_OFF, 0,
                     &error_fatal);
        blk_pwrite(blk, 0, CUP28_FLASH_SIZE, melco_amdflash_array(s->drf), 0);
    }

    if (kernel && *kernel) {
        if (!g_file_get_contents(kernel, &data, &length, &error)) {
            error_report("cup28: cannot load firmware: %s", error->message);
            exit(1);
        }
        if (!length || length > CUP28_FLASH_SIZE - CUP28_FIRMWARE_OFFSET) {
            error_report("cup28: firmware does not fit in flash");
            exit(1);
        }
        memcpy(melco_amdflash_array(s->drf) + CUP28_FIRMWARE_OFFSET,
               data, length);
    } else if (!have_image) {
        error_report("cup28: supply a bootable flash image or -kernel");
        exit(1);
    }

    /* Flash A25 is unwired. The loader uses the upper alias to program it. */
    memory_region_init_io(&s->drf_alias_regs, OBJECT(s), &cup28_drf_alias_ops,
                          s, "cup28-flash-alias", CUP28_FLASH_SIZE);
    memory_region_add_subregion_overlap(mem, CUP28_FLASH_SIZE,
                                        &s->drf_alias_regs, 1);
}

/*
 * The board's devices interrupt through the CPU's IRL pins: the SH7750R
 * autovectors IRL_0 .. IRL_E to 0x200, 0x220, ... 0x3c0 and the guest's
 * HookInterrupt entries use exactly those numbers.  Taking the source whose
 * vector is already the INTEVT means no on-chip peripheral (a timer or a DMAC
 * channel the guest may drive itself) is borrowed and nothing is re-vectored.
 */
struct intc_source *cup28_intc_source(SuperHCPU *cpu, unsigned intevt)
{
    struct intc_desc *desc = cpu->env.intc_handle;
    unsigned i;

    for (i = 0; desc && i < desc->nr_sources; i++) {
        if (desc->sources[i].vect == intevt) {
            return &desc->sources[i];
        }
    }
    return NULL;
}

static void cup28_storage_init(Cup28MachineState *s, MemoryRegion *mem,
                               SuperHCPU *cpu)
{
    struct intc_desc *intc = cpu->env.intc_handle;

    s->dvd_fd = -1;
    if (s->dvdimage && *s->dvdimage) {
        struct stat st;

        s->dvd_fd = open(s->dvdimage, O_RDONLY | O_BINARY);
        if (s->dvd_fd < 0 || fstat(s->dvd_fd, &st) < 0) {
            error_report("cup28: cannot open DVD '%s': %s", s->dvdimage,
                         strerror(errno));
            exit(1);
        }
        s->dvd_size = st.st_size;
    }

    /* The loader and NK program different DVD CPLD priorities. */
    s->dvd5_intc_src = cup28_intc_source(cpu, CUP28_INTEVT_DVD_NK);
    s->dvd5_intc_src2 = cup28_intc_source(cpu, CUP28_INTEVT_DVD_MLD);
    if (!s->dvd5_intc_src || !s->dvd5_intc_src2) {
        error_report("cup28: no INTC source for the DVD CPLD interrupts");
        exit(1);
    }
    s->dvd5_intc_src->prio = 3;
    s->dvd5_intc_src->parent = intc;
    s->dvd5_intc_src2->prio = 3;
    s->dvd5_intc_src2->parent = intc;
    s->p28_dvd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cup28_dvd_tick, s);
    memory_region_init_io(&s->p28_dvd_ports, OBJECT(s), &cup28_dvd_port_ops, s,
                          "cup28-dvd-ports", 0x10);
    memory_region_add_subregion_overlap(mem, 0x07410000, &s->p28_dvd_ports, 2);
    memory_region_init_io(&s->p28_dvd_control, OBJECT(s), &cup28_dvd_ctrl_ops,
                          s, "cup28-cpld", 0x200);
    memory_region_add_subregion_overlap(mem, 0x07200000, &s->p28_dvd_control, 2);

    /* STUB: EEPROM program/read-back uses erased RAM behind the registers. */
    memory_region_init_ram(&s->p28_eeprom_bank, NULL, "cup28-eeprom-bank",
                           CUP28_EEPROM_SIZE, &error_fatal);
    s->p28_eeprom_mem = memory_region_get_ram_ptr(&s->p28_eeprom_bank);
    memset(s->p28_eeprom_mem, 0xff, CUP28_EEPROM_SIZE);
    memory_region_add_subregion_overlap(mem, CUP28_EEPROM_BASE,
                                        &s->p28_eeprom_bank, 2);

    memset(s->p28_eeprom_reg, 0xff, sizeof(s->p28_eeprom_reg));
    memory_region_init_io(&s->p28_eeprom_regs, OBJECT(s), &cup28_eeprom_ops,
                          s, "cup28-eeprom-registers", 0x100);
    memory_region_add_subregion_overlap(mem, CUP28_EEPROM_BASE,
                                        &s->p28_eeprom_regs, 3);
    cup28_bksram_init(s);
    memory_region_add_subregion_overlap(mem, 0x12000000, &s->p28_bksram, 1);
    memory_region_init_ram(&s->p28_bkeeprom, NULL, "cup28-backup-eeprom",
                           MiB, &error_fatal);
    memset(memory_region_get_ram_ptr(&s->p28_bkeeprom), 0xff, MiB);
    memory_region_add_subregion_overlap(mem, 0x18000000, &s->p28_bkeeprom, 1);

    /* STUB: the FLR driver sees a blank 256 KiB read/write window. */
    s->p28_flr_mem = g_malloc0(256 * KiB);
    memory_region_init_io(&s->p28_flrwin, OBJECT(s), &cup28_flr_ops, s,
                          "cup28-flr-window", 256 * KiB);
    memory_region_add_subregion_overlap(mem, 0x01a00000, &s->p28_flrwin, 3);
}

static void cup28_display_init(Cup28MachineState *s, MemoryRegion *mem,
                               SuperHCPU *cpu)
{
    s->disp_con = qemu_graphic_console_create(NULL, 0, &cup28_console_ops, s);
    qemu_console_set_surface(s->disp_con, qemu_create_displaysurface(424, 242));
    cup28_q2_irq_init(s, cpu);
    memory_region_init_ram(&s->p28_q2_vram, NULL, "cup28-q2-ugm",
                           8 * MiB, &error_fatal);
    memory_region_add_subregion_overlap(mem, 0x10000000, &s->p28_q2_vram, 2);
    s->p28_q2_mem = memory_region_get_ram_ptr(&s->p28_q2_vram);

    stw_le_p(s->p28_q2_reg, 0xc000);     /* SYSR: SRES | DRES */
    stw_le_p(s->p28_q2_reg + 2, 0x44);   /* SR: FEMP | Q2SD */
    memory_region_init_io(&s->p28_q2_regs, OBJECT(s), &cup28_q2_ops, s,
                          "cup28-q2-registers", sizeof(s->p28_q2_reg));
    memory_region_add_subregion_overlap(mem, 0x11000000, &s->p28_q2_regs, 2);
    s->p28_q2_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cup28_q2_tick, s);
    timer_mod(s->p28_q2_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667);
}

static void cup28_init(MachineState *machine)
{
    Cup28MachineState *s = CUP28_MACHINE(machine);
    MemoryRegion *mem = get_system_memory();
    SuperHCPU *cpu = SUPERH_CPU(cpu_create(machine->cpu_type));

    error_setg(&s->migration_blocker, "CUP28 devices cannot be snapshotted or migrated");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);

    cpu->env.features |= SH_FEATURE_SH4A;
    qemu_register_reset(cup28_reset_cpu, cpu);
    memory_region_init_ram(&s->ram, NULL, "cup28.ram", machine->ram_size,
                           &error_fatal);
    memory_region_add_subregion(mem, 0, &s->ram);
    sh7750_init(cpu, mem);

    cup28_flash_init(s, mem, machine->kernel_filename);
    cup28_storage_init(s, mem, cpu);
    cup28_display_init(s, mem, cpu);
    if (s->gpstrack && *s->gpstrack) {
        Error *err = NULL;

        s->track = melco_track_load(s->gpstrack, &err);
        if (!s->track) {
            error_report("cup28: %s", error_get_pretty(err));
            exit(1);
        }
    }
    cup28_dmac_init(s, mem, cpu);
    cup28_audio_init(s, mem, machine->audiodev);
    cup28_most_init(s, mem, cpu);
    cup28_gps_init(s, mem, cpu);
    cup28_sns_init(s, mem, cpu);

    memory_region_init(&s->p28_uart_container, OBJECT(s), "cup28-uart", 0x20);
    memory_region_add_subregion_overlap(mem, 0x04800000,
                                        &s->p28_uart_container, 2);
    serial_mm_init(&s->p28_uart_container, 0, 2, NULL, 115200,
                   serial_hd(2), DEVICE_LITTLE_ENDIAN);
    memory_region_init_io(&s->selftest_regs, OBJECT(s), &cup28_selftest_ops,
                          s, "cup28-selftest", 0x1000);
    memory_region_add_subregion_overlap(mem, 0xff93c000, &s->selftest_regs, 1);

    s->rtc_base_s = CUP28_RTC_EPOCH;
    memory_region_init_io(&s->rtc_regs, OBJECT(s), &cup28_rtc_ops,
                          s, "cup28-rtc", CUP28_RTC_SIZE);
    memory_region_add_subregion_overlap(mem, CUP28_RTC_BASE, &s->rtc_regs, 1);
    memory_region_init_alias(&s->rtc_a7_regs, OBJECT(s), "cup28-rtc-a7",
                             &s->rtc_regs, 0, CUP28_RTC_SIZE);
    memory_region_add_subregion_overlap(mem, CUP28_RTC_A7_BASE,
                                        &s->rtc_a7_regs, 1);
}

#define CUP28_PATH_PROPERTY(name)                                       \
    static char *cup28_get_##name(Object *obj, Error **errp)             \
    {                                                                   \
        return g_strdup(CUP28_MACHINE(obj)->name);                       \
    }                                                                   \
    static void cup28_set_##name(Object *obj, const char *value,        \
                                 Error **errp)                          \
    {                                                                   \
        Cup28MachineState *s = CUP28_MACHINE(obj);                       \
        g_free(s->name);                                                \
        s->name = g_strdup(value);                                      \
    }

CUP28_PATH_PROPERTY(dvdimage)
CUP28_PATH_PROPERTY(drfimage)
CUP28_PATH_PROPERTY(sramsave)
CUP28_PATH_PROPERTY(gpstrack)

static void cup28_finalize(Object *obj)
{
    Cup28MachineState *s = CUP28_MACHINE(obj);

    g_free(s->dvdimage);
    g_free(s->drfimage);
    g_free(s->sramsave);
    g_free(s->gpstrack);
    if (s->most_input) {
        qemu_input_handler_unregister(s->most_input);
    }
    melco_track_free(s->track);
}

static void cup28_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Volvo VNS CUP28";
    mc->init = cup28_init;
    mc->reset = cup28_machine_reset;
    mc->default_cpu_type = TYPE_SH7751R_CPU;
    mc->default_ram_size = 256 * MiB;
    mc->max_cpus = 1;
    machine_add_audiodev_property(mc);

    object_class_property_add_str(oc, "dvdimage", cup28_get_dvdimage,
                                  cup28_set_dvdimage);
    object_class_property_set_description(oc, "dvdimage", "Read-only DVD ISO");
    object_class_property_add_str(oc, "drfimage", cup28_get_drfimage,
                                  cup28_set_drfimage);
    object_class_property_set_description(oc, "drfimage", "32 MiB flash file");
    object_class_property_add_str(oc, "sramsave", cup28_get_sramsave,
                                  cup28_set_sramsave);
    object_class_property_set_description(oc, "sramsave", "1 MiB backup SRAM file");
    object_class_property_add_str(oc, "gpstrack", cup28_get_gpstrack,
                                  cup28_set_gpstrack);
    object_class_property_set_description(oc, "gpstrack", "GPS and sensor track CSV");
}

static const TypeInfo cup28_types[] = {
    {
        .name = TYPE_CUP28_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(Cup28MachineState),
        .instance_finalize = cup28_finalize,
        .class_init = cup28_class_init,
    },
};

DEFINE_TYPES(cup28_types)
