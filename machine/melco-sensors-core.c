/*
 * Shared car-sensor track replay for the melco-nav-emu boards (QEMU-free
 * core).  SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "melco-sensors-core.h"

#define MELCO_TRACK_HEADER \
    "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps"

struct MelcoTrack {
    MelcoSample *rows;
    unsigned n;
    int64_t interval_ms;
    int64_t start_ms;       /* -1 until the clock starts */
    int64_t odo_last_ms;    /* odometer integration cursor */
    MelcoSample stopped;    /* past-end sample */
};

static int64_t imin64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static char *strip(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

/* Split a stripped line on ','.  Returns the field count and fills fields[]
 * (at most 8; more fields are still counted so the caller can reject). */
static unsigned split_fields(char *line, char *fields[], unsigned max)
{
    unsigned n = 0;
    char *p = line;

    for (;;) {
        char *comma = strchr(p, ',');

        if (comma) {
            *comma = '\0';
        }
        if (n < max) {
            fields[n] = p;
        }
        n++;
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return n;
}

MelcoTrack *melco_track_core_load(const char *path, char *err_msg,
                                  size_t err_msg_size)
{
    FILE *f;
    char *data = NULL;
    long size;
    MelcoTrack *t;
    char *line, *next;
    unsigned file_line = 0, err_line = 1;
    int64_t first_ms = 0;
    unsigned n = 0;
    unsigned cap = 0;

    f = fopen(path, "r");
    if (!f) {
        snprintf(err_msg, err_msg_size, "cannot load track: %s",
                 strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        snprintf(err_msg, err_msg_size, "cannot load track: %s",
                 strerror(errno));
        fclose(f);
        return NULL;
    }
    /* The parser below is string based: keep a NUL after the last byte so a
     * file without a trailing newline still ends at the buffer end. */
    data = malloc((size_t)size + 1);
    if (!data || (size && fread(data, 1, (size_t)size, f) != (size_t)size)) {
        snprintf(err_msg, err_msg_size, "cannot load track: %s",
                 strerror(errno));
        free(data);
        fclose(f);
        return NULL;
    }
    data[size] = '\0';
    fclose(f);

    t = calloc(1, sizeof(*t));
    t->start_ms = -1;

    line = data;
    while (line && *line) {
        char *fields[8];
        double value[8];
        unsigned nf;
        int64_t ms;

        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }
        file_line++;
        {
            char *row = strip(line);

            if (file_line == 1) {
                if (strcmp(row, MELCO_TRACK_HEADER)) {
                    err_line = file_line;
                    goto invalid;
                }
                line = next;
                continue;
            }
            if (!*row) {
                line = next;
                continue;
            }
            nf = split_fields(row, fields, 8);
            if (nf != 8) {
                err_line = file_line;
                goto invalid;
            }
            for (unsigned i = 0; i < 8; i++) {
                char *end;

                value[i] = strtod(fields[i], &end);
                if (end == fields[i] || *end || !isfinite(value[i])) {
                    err_line = file_line;
                    goto invalid;
                }
            }
            if (value[0] < 0 || value[0] > 86400 ||
                fabs(value[1]) > 90 || value[2] < -180 || value[2] >= 180 ||
                value[3] < INT16_MIN || value[3] > INT16_MAX ||
                value[4] < 0 || value[4] > 400 ||
                value[5] < 0 || value[5] >= 360 || fabs(value[6]) > 150 ||
                (value[7] != 0 && value[7] != 1)) {
                err_line = file_line;
                goto invalid;
            }
            ms = llround(value[0] * 1000);
            if (!n) {
                first_ms = ms;
            } else if (n == 1) {
                t->interval_ms = ms - first_ms;
            }
            if (n && (t->interval_ms <= 0 ||
                      ms - first_ms != (int64_t)n * t->interval_ms)) {
                err_line = file_line;
                goto invalid;
            }
            if (n == cap) {
                MelcoSample *grown;

                cap = cap ? cap * 2 : 64;
                grown = realloc(t->rows, cap * sizeof(MelcoSample));
                if (!grown) {
                    snprintf(err_msg, err_msg_size,
                             "cannot load track: out of memory");
                    free(data);
                    free(t->rows);
                    free(t);
                    return NULL;
                }
                t->rows = grown;
            }
            t->rows[n] = (MelcoSample) {
                .lat_deg = value[1],
                .lon_deg = value[2],
                .alt_m = value[3],
                .speed_kmh = value[4],
                .heading_deg = value[5],
                .yaw_dps = value[6],
                .fix = value[7] != 0,
            };
            n++;
        }
        line = next;
    }
    if (n < 2) {
        err_line = file_line + 1;
        goto invalid;
    }
    t->n = n;
    free(data);
    return t;

invalid:
    snprintf(err_msg, err_msg_size, "invalid track '%s' at line %u",
             path, err_line);
    free(data);
    free(t->rows);
    free(t);
    return NULL;
}

static void track_ensure_started(MelcoTrack *t, int64_t now_ms)
{
    if (t->start_ms < 0) {
        t->start_ms = now_ms;
        t->odo_last_ms = now_ms;
    }
}

void melco_track_start(MelcoTrack *t, int64_t now_ms)
{
    track_ensure_started(t, now_ms);
}

const MelcoSample *melco_track_sample(MelcoTrack *t, int64_t now_ms)
{
    int64_t i;

    track_ensure_started(t, now_ms);
    i = (now_ms - t->start_ms) / t->interval_ms;
    if (i < (int64_t)t->n) {
        return &t->rows[i];
    }

    /* The finite replay ends at rest and loses its receiver fix. */
    t->stopped = t->rows[t->n - 1];
    t->stopped.speed_kmh = 0;
    t->stopped.yaw_dps = 0;
    t->stopped.fix = false;
    return &t->stopped;
}

double melco_track_odometer_m(MelcoTrack *t, int64_t now_ms)
{
    int64_t from, to, dt = t->interval_ms;
    double metres = 0;

    track_ensure_started(t, now_ms);
    from = t->odo_last_ms - t->start_ms;
    to = now_ms - t->start_ms;
    t->odo_last_ms = now_ms;
    while (from < to && from / dt < (int64_t)t->n) {
        unsigned i = from / dt;
        int64_t end = imin64(to, ((int64_t)i + 1) * dt);

        metres += t->rows[i].speed_kmh * (end - from) / 3600.0;
        from = end;
    }
    return metres;
}

bool melco_track_ended(MelcoTrack *t, int64_t now_ms)
{
    if (t->start_ms < 0) {
        return false;
    }
    return (now_ms - t->start_ms) / t->interval_ms >= (int64_t)t->n;
}

unsigned melco_track_rows(const MelcoTrack *t)
{
    return t->n;
}

int64_t melco_track_interval_ms(const MelcoTrack *t)
{
    return t->interval_ms;
}

void melco_track_free(MelcoTrack *t)
{
    if (!t) {
        return;
    }
    free(t->rows);
    free(t);
}

unsigned melco_pulses(double *carry, double metres_delta,
                      double pulses_per_m, unsigned max_per_call)
{
    double total = *carry + fabs(metres_delta) * pulses_per_m;
    double capped = total < (double)max_per_call ? total
                                                 : (double)max_per_call;
    unsigned whole = (unsigned)capped;

    *carry = total - whole;
    return whole;
}
