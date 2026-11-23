/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

static void cup28_track_load(Cup28MachineState *s)
{
    static const char header[] =
        "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps";
    g_autofree char *data = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) lines = NULL;
    int64_t first_ms = 0;
    unsigned line = 0;

    if (!g_file_get_contents(s->gpstrack, &data, NULL, &error)) {
        error_report("cup28: cannot load track: %s", error->message);
        exit(1);
    }
    s->track = g_array_new(false, false, sizeof(Cup28TrackSample));
    lines = g_strsplit(data, "\n", -1);
    /* An empty file leaves lines[0] == NULL; that is an invalid track, not a
     * crash (strcmp on NULL). */
    if (!lines[0] || strcmp(g_strstrip(lines[0]), header)) {
        goto invalid;
    }
    for (line = 1; lines[line]; line++) {
        char *row = g_strstrip(lines[line]);
        g_auto(GStrv) fields = NULL;
        double value[8];
        Cup28TrackSample sample;
        int64_t ms;

        if (!*row) {
            continue;
        }
        fields = g_strsplit(row, ",", -1);
        if (g_strv_length(fields) != G_N_ELEMENTS(value)) {
            goto invalid;
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(value); i++) {
            char *end;

            value[i] = g_ascii_strtod(fields[i], &end);
            if (end == fields[i] || *end || !isfinite(value[i])) {
                goto invalid;
            }
        }
        if (value[0] < 0 || value[0] > 86400 ||
            fabs(value[1]) > 90 || value[2] < -180 || value[2] >= 180 ||
            value[3] < INT16_MIN || value[3] > INT16_MAX ||
            value[4] < 0 || value[4] > 400 ||
            value[5] < 0 || value[5] >= 360 || fabs(value[6]) > 150 ||
            (value[7] != 0 && value[7] != 1)) {
            goto invalid;
        }
        ms = llround(value[0] * 1000);
        if (!s->track->len) {
            first_ms = ms;
        } else if (s->track->len == 1) {
            s->track_interval_ms = ms - first_ms;
        }
        if (s->track->len && (s->track_interval_ms <= 0 ||
            ms - first_ms != s->track->len * s->track_interval_ms)) {
            goto invalid;
        }
        sample = (Cup28TrackSample) {
            .latitude = value[1],
            .longitude = value[2],
            .altitude = value[3],
            .speed_kmh = value[4],
            .heading_deg = value[5],
            .yaw_dps = value[6],
            .fix_valid = value[7] != 0,
        };
        g_array_append_val(s->track, sample);
    }
    if (s->track->len < 2) {
        goto invalid;
    }
    s->gps_track_n = s->track->len;
    s->track_start_ms = -1;
    return;

invalid:
    error_report("cup28: invalid track '%s' at line %u", s->gpstrack, line + 1);
    exit(1);
}

static const Cup28TrackSample *cup28_track_row(Cup28MachineState *s,
                                                int64_t now_ms)
{
    int64_t i;

    if (s->track_start_ms < 0) {
        s->track_start_ms = now_ms;
        s->wheel_track_last_ms = now_ms;
    }
    i = (now_ms - s->track_start_ms) / s->track_interval_ms;
    if (i < s->gps_track_n) {
        return &g_array_index(s->track, Cup28TrackSample, i);
    }

    /* The finite replay ends at rest and loses its receiver fix. */
    s->track_stopped = g_array_index(s->track, Cup28TrackSample, s->gps_track_n - 1);
    s->track_stopped.speed_kmh = 0;
    s->track_stopped.yaw_dps = 0;
    s->track_stopped.fix_valid = false;
    return &s->track_stopped;
}

static double cup28_wheel_distance(Cup28MachineState *s, int64_t now_ms)
{
    int64_t from = s->wheel_track_last_ms - s->track_start_ms;
    int64_t to = now_ms - s->track_start_ms;
    int64_t dt = s->track_interval_ms;
    double metres = 0;

    s->wheel_track_last_ms = now_ms;
    while (from < to && from / dt < s->gps_track_n) {
        unsigned i = from / dt;
        int64_t end = MIN(to, (i + 1) * dt);

        const Cup28TrackSample *sample = &g_array_index(s->track, Cup28TrackSample, i);

        metres += sample->speed_kmh * (end - from) / 3600.0;
        from = end;
    }
    return metres;
}
