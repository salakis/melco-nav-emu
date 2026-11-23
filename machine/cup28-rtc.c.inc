/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

#define CUP28_RTC_BASE        0xffc80000
#define CUP28_RTC_A7_BASE     0x1fc80000
#define CUP28_RTC_SIZE        0x00000100
#define CUP28_RTC_LATCH       0x0002
#define CUP28_RTC_STATUS      0x0004
#define CUP28_RTC_YEAR        0x0012
#define CUP28_RTC_MONTH       0x0014
#define CUP28_RTC_DAY         0x0016
#define CUP28_RTC_HOUR        0x0018
#define CUP28_RTC_MINUTE      0x001a
#define CUP28_RTC_SECOND      0x001c
#define CUP28_RTC_SECOND_SHIFT 5
#define CUP28_RTC_SET_YEAR    0x0006  /* +0x06..+0x10: fields to set */
#define CUP28_RTC_SET_SECOND  0x0010
#define CUP28_RTC_EPOCH       1252487349  /* 2009-09-09 09:09:09 UTC */

/* Seconds the RTC reads now (power-on epoch plus guest time since the last set). */
static int64_t cup28_rtc_now_s(Cup28MachineState *s)
{
    return s->rtc_base_s +
           (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - s->rtc_base_ms) / 1000;
}

static bool cup28_rtc_reg(Cup28MachineState *s, hwaddr off, uint16_t *value)
{
    time_t t = (time_t)cup28_rtc_now_s(s);
    struct tm tm;

    gmtime_r(&t, &tm);
    switch (off) {
    case CUP28_RTC_LATCH:
        *value = 0;
        return true;
    case CUP28_RTC_STATUS:
        /* Not busy; bit 0 = 1 means "backup invalid (NG)". */
        *value = 0x0000;
        return true;
    case CUP28_RTC_YEAR:
        *value = tm.tm_year - 100;          /* years since 2000 */
        return true;
    case CUP28_RTC_MONTH:
        *value = tm.tm_mon + 1;
        return true;
    case CUP28_RTC_DAY:
        *value = tm.tm_mday;
        return true;
    case CUP28_RTC_HOUR:
        *value = tm.tm_hour;
        return true;
    case CUP28_RTC_MINUTE:
        *value = tm.tm_min;
        return true;
    case CUP28_RTC_SECOND:
        *value = tm.tm_sec << CUP28_RTC_SECOND_SHIFT;
        return true;
    default:
        *value = 0;
        return false;
    }
}

static uint64_t cup28_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint16_t reg;
        unsigned shift;

        /*
         * The device is a 16-bit register file.  Byte accesses address the
         * little-endian half of that file, matching the SH-4 bus.
         */
        off &= ~(hwaddr)1;
        shift = 8 * ((addr + i) & 1);
        cup28_rtc_reg(s, off, &reg);
        value |= (uint64_t)((reg >> shift) & 0xff) << (8 * i);
    }

    return value;
}

/* STUB: writing 0xffff to the latch commits the staged fields. */
static void cup28_rtc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup28MachineState *s = opaque;

    if (size != 2) {
        return;
    }
    if (addr >= CUP28_RTC_SET_YEAR && addr <= CUP28_RTC_SET_SECOND) {
        s->rtc_set[(addr - CUP28_RTC_SET_YEAR) / 2] = value;
    } else if (addr == CUP28_RTC_LATCH && value == 0xffff) {
        struct tm tm = {
            .tm_year = s->rtc_set[0] + 100, .tm_mon = s->rtc_set[1] - 1,
            .tm_mday = s->rtc_set[2], .tm_hour = s->rtc_set[3],
            .tm_min = s->rtc_set[4],
            .tm_sec = s->rtc_set[5] >> CUP28_RTC_SECOND_SHIFT,
        };

        s->rtc_base_s = (int64_t)timegm(&tm);
        s->rtc_base_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    }
}

static const MemoryRegionOps cup28_rtc_ops = {
    .read = cup28_rtc_read,
    .write = cup28_rtc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

