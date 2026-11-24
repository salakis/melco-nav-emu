/*
 * Shared AMD NOR flash device for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "qemu/osdep.h"

#include "hw/block/block.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qemu/notify.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "trace.h"

#include "melco-amdflash.h"
#include "melco-amdflash-core.h"

struct MelcoAmdFlash {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MelcoAmdFlashCore core;
    MemoryRegion mem;           /* the ROMD flash window */
    BlockBackend *blk;          /* optional backing file */

    uint64_t size;
    uint32_t sector_len;
    uint8_t interleave;
    uint16_t id_manufacturer;
    uint16_t id_device;
    char *name;

    int64_t activity_ms;        /* guest time of the last array change */
    hwaddr dirty_lo, dirty_hi;  /* dirty span [lo, hi), lo >= hi = clean */
    QEMUTimer *save_timer;
    Notifier exit_notifier;
    Error *migration_blocker;
};

#define MELCO_AMDFLASH_SAVE_MS 200

/* Persist the dirty span to the backing file, widened to 512 bytes. */
static void melco_amdflash_save(MelcoAmdFlash *s)
{
    uint64_t off, end;
    int ret;

    if (!s->blk || s->dirty_lo >= s->dirty_hi) {
        return;
    }
    off = s->dirty_lo;
    end = s->dirty_hi;
    s->dirty_lo = s->size;      /* nothing dirty */
    s->dirty_hi = 0;
    off = QEMU_ALIGN_DOWN(off, BDRV_SECTOR_SIZE);
    end = QEMU_ALIGN_UP(end, BDRV_SECTOR_SIZE);
    if (end > s->size) {
        end = s->size;
    }
    ret = blk_pwrite(s->blk, off, end - off, s->core.data + off, 0);
    if (ret < 0) {
        error_report("melco-amdflash: backing file write failed: %s",
                     strerror(-ret));
    }
}

static void melco_amdflash_save_timer_cb(void *opaque)
{
    MelcoAmdFlash *s = opaque;

    melco_amdflash_save(s);
    timer_mod(s->save_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
              MELCO_AMDFLASH_SAVE_MS);
}

static void melco_amdflash_exit(Notifier *n, void *data)
{
    melco_amdflash_save(container_of(n, MelcoAmdFlash, exit_notifier));
}

static void melco_amdflash_write_dev(MelcoAmdFlash *s, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    uint64_t off, len;
    MelcoAmdFlashOp op;
    bool was_autoselect = s->core.autoselect;

    if (addr >= s->size) {
        return;
    }
    if (melco_amdflash_core_write(&s->core, addr, value, size, &off, &len,
                                  &op)) {
        /* The CPU may execute from this array (XIP): drop translated code. */
        memory_region_flush_rom_device(&s->mem, off, len);
        if (off < s->dirty_lo) {
            s->dirty_lo = off;
        }
        if (off + len > s->dirty_hi) {
            s->dirty_hi = off + len;
        }
        s->activity_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        switch (op) {
        case MELCO_AMDFLASH_OP_PROGRAM:
            trace_melco_amdflash_program(off, value, size);
            break;
        case MELCO_AMDFLASH_OP_SECTOR_ERASE:
            trace_melco_amdflash_sector_erase(off, len);
            break;
        case MELCO_AMDFLASH_OP_CHIP_ERASE:
            trace_melco_amdflash_chip_erase();
            break;
        default:
            break;
        }
    }
    /* Autoselect swaps the ID bytes into the array: drop translated code. */
    if (s->core.autoselect != was_autoselect) {
        memory_region_flush_rom_device(&s->mem, s->core.autoselect_bank,
                                       sizeof(s->core.id_save));
    }
}

static uint64_t melco_amdflash_read_op(void *opaque, hwaddr addr,
                                       unsigned size)
{
    MelcoAmdFlash *s = MELCO_AMDFLASH(opaque);

    return melco_amdflash_core_read(&s->core, addr, size);
}

static void melco_amdflash_write_op(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    melco_amdflash_write_dev(MELCO_AMDFLASH(opaque), addr, value, size);
}

static const MemoryRegionOps melco_amdflash_ops = {
    .read = melco_amdflash_read_op,
    .write = melco_amdflash_write_op,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void melco_amdflash_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    MelcoAmdFlash *s = MELCO_AMDFLASH(dev);

    if (!s->size || !s->sector_len ||
        (s->size % s->sector_len) || !is_power_of_2(s->sector_len) ||
        s->interleave < 1 || s->interleave > 4 ||
        !is_power_of_2(s->interleave)) {
        error_setg(errp, "melco-amdflash: bad size/sector-length/interleave");
        return;
    }

    memory_region_init_rom_device(&s->mem, OBJECT(dev), &melco_amdflash_ops,
                                  s, s->name ?: "melco-amdflash", s->size,
                                  errp);
    if (*errp) {
        return;
    }
    s->core.data = memory_region_get_ram_ptr(&s->mem);
    memset(s->core.data, 0xff, s->size);
    melco_amdflash_core_init(&s->core, s->core.data, s->size, s->sector_len,
                             s->interleave, s->id_manufacturer,
                             s->id_device);
    /* Read mode serves the array directly. */
    memory_region_rom_device_set_romd(&s->mem, true);

    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE;
        int64_t len;

        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        len = blk_getlength(s->blk);
        if (len < 0) {
            error_setg(errp, "melco-amdflash: cannot get backing file size");
            return;
        }
        if (len > 0) {
            uint64_t n = MIN((uint64_t)len, s->size);

            if (blk_pread(s->blk, 0, n, s->core.data, 0) < 0) {
                error_setg(errp, "melco-amdflash: cannot read backing file");
                return;
            }
        }
    }

    s->dirty_lo = s->size;      /* nothing dirty */
    s->dirty_hi = 0;
    s->save_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                 melco_amdflash_save_timer_cb, s);
    timer_mod(s->save_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
              MELCO_AMDFLASH_SAVE_MS);
    s->exit_notifier.notify = melco_amdflash_exit;
    qemu_add_exit_notifier(&s->exit_notifier);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mem);

    /* The device has no vmstate: snapshots would silently lose its state. */
    error_setg(&s->migration_blocker,
               "melco-amdflash: the flash device does not support snapshots"
               " or migration");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);
}

static void melco_amdflash_reset(DeviceState *dev)
{
    MelcoAmdFlash *s = MELCO_AMDFLASH(dev);

    melco_amdflash_core_reset(&s->core);
}

static const Property melco_amdflash_properties[] = {
    DEFINE_PROP_DRIVE("drive", MelcoAmdFlash, blk),
    DEFINE_PROP_UINT64("size", MelcoAmdFlash, size, 0),
    DEFINE_PROP_UINT32("sector-length", MelcoAmdFlash, sector_len, 0),
    DEFINE_PROP_UINT8("interleave", MelcoAmdFlash, interleave, 1),
    DEFINE_PROP_UINT16("id-manufacturer", MelcoAmdFlash, id_manufacturer, 0),
    DEFINE_PROP_UINT16("id-device", MelcoAmdFlash, id_device, 0),
    DEFINE_PROP_STRING("name", MelcoAmdFlash, name),
};

static void melco_amdflash_unrealize(DeviceState *dev)
{
    MelcoAmdFlash *s = MELCO_AMDFLASH(dev);

    timer_del(s->save_timer);
    qemu_remove_exit_notifier(&s->exit_notifier);
}

static void melco_amdflash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = melco_amdflash_realize;
    dc->unrealize = melco_amdflash_unrealize;
    device_class_set_legacy_reset(dc, melco_amdflash_reset);
    device_class_set_props(dc, melco_amdflash_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo melco_amdflash_info = {
    .name           = TYPE_MELCO_AMDFLASH,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(MelcoAmdFlash),
    .class_init     = melco_amdflash_class_init,
};

static void melco_amdflash_register_types(void)
{
    type_register_static(&melco_amdflash_info);
}

type_init(melco_amdflash_register_types)

MelcoAmdFlash *melco_amdflash_create(const char *name, uint64_t size,
                                     uint32_t sector_len, uint8_t interleave,
                                     uint16_t id_manufacturer,
                                     uint16_t id_device,
                                     BlockBackend *blk)
{
    DeviceState *dev = qdev_new(TYPE_MELCO_AMDFLASH);

    qdev_prop_set_string(dev, "name", name);
    if (blk) {
        qdev_prop_set_drive(dev, "drive", blk);
    }
    qdev_prop_set_uint64(dev, "size", size);
    qdev_prop_set_uint32(dev, "sector-length", sector_len);
    qdev_prop_set_uint8(dev, "interleave", interleave);
    qdev_prop_set_uint16(dev, "id-manufacturer", id_manufacturer);
    qdev_prop_set_uint16(dev, "id-device", id_device);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    return MELCO_AMDFLASH(dev);
}

MemoryRegion *melco_amdflash_mmio(MelcoAmdFlash *s)
{
    return &s->mem;
}

uint8_t *melco_amdflash_array(MelcoAmdFlash *s)
{
    return s->core.data;
}

int64_t melco_amdflash_activity_ms(MelcoAmdFlash *s)
{
    return s->activity_ms;
}

uint64_t melco_amdflash_read(MelcoAmdFlash *s, hwaddr addr, unsigned size)
{
    return melco_amdflash_core_read(&s->core, addr, size);
}

void melco_amdflash_write(MelcoAmdFlash *s, hwaddr addr, uint64_t value,
                          unsigned size)
{
    melco_amdflash_write_dev(s, addr, value, size);
}
