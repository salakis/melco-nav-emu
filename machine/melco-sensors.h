/*
 * Shared car-sensor track replay for the melco-nav-emu boards.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef MELCO_SENSORS_H
#define MELCO_SENSORS_H

#include "qapi/error.h"

#include "melco-sensors-core.h"

/*
 * melco_track_load: strict CSV loader (see melco-sensors-core.h), with the
 * error reported through errp (the message names the offending line).
 * Returns NULL on error.
 */
MelcoTrack *melco_track_load(const char *path, Error **errp);

#endif /* MELCO_SENSORS_H */
