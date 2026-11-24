/*
 * Shared car-sensor track replay for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include "qemu/osdep.h"

#include "qapi/error.h"

#include "melco-sensors.h"

MelcoTrack *melco_track_load(const char *path, Error **errp)
{
    char err_msg[512];
    MelcoTrack *t = melco_track_core_load(path, err_msg, sizeof(err_msg));

    if (!t) {
        error_setg(errp, "%s", err_msg);
    }
    return t;
}
