/*
 * Shared car-sensor track replay for the melco-nav-emu boards (QEMU-free
 * core).  SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#ifndef MELCO_SENSORS_CORE_H
#define MELCO_SENSORS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One row of the sensor CSV. */
typedef struct MelcoSample {
    double lat_deg, lon_deg, alt_m, speed_kmh, heading_deg, yaw_dps;
    bool fix;
} MelcoSample;

typedef struct MelcoTrack MelcoTrack;   /* opaque */

/*
 * Strict CSV loader (moved unchanged from cup28-track.c.inc): header
 * "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps", one row
 * per sample, evenly spaced, at least 2 rows.  On error returns NULL and
 * writes a message ("invalid track '<path>' at line N" or
 * "cannot load track: ...") into err_msg.
 */
MelcoTrack *melco_track_core_load(const char *path, char *err_msg,
                                  size_t err_msg_size);

/* Clock start (first call wins; sample/odometer start it on first use). */
void melco_track_start(MelcoTrack *t, int64_t now_ms);

/*
 * Row for now_ms: index (now - start) / interval.  Past the end: the last
 * position with speed 0, yaw 0 and fix false.
 */
const MelcoSample *melco_track_sample(MelcoTrack *t, int64_t now_ms);

/*
 * Cumulative distance in metres on the scenario clock, speed held over
 * each sample (the wheel transducer integrates between calls).
 */
double melco_track_odometer_m(MelcoTrack *t, int64_t now_ms);

/* True once now_ms is past the last row. */
bool melco_track_ended(MelcoTrack *t, int64_t now_ms);

/* Row count and sample interval (ms) of a loaded track. */
unsigned melco_track_rows(const MelcoTrack *t);
int64_t melco_track_interval_ms(const MelcoTrack *t);

void melco_track_free(MelcoTrack *t);

/*
 * Wheel pulses: returns the whole pulses in *carry + metres_delta *
 * pulses_per_m, capped at max_per_call, and keeps the fraction (and any
 * excess over the cap) in *carry.
 */
unsigned melco_pulses(double *carry, double metres_delta,
                      double pulses_per_m, unsigned max_per_call);

#endif /* MELCO_SENSORS_CORE_H */
