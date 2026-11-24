# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Unit test for the QEMU-free melco-sensors core (track loader, sampler,
odometer, pulse helper).

Compiles machine/melco-sensors-core.c with cc -std=c11 together with a test
main (same pattern as test_melco_amdflash.py) and checks the strict CSV
loader rules moved from cup28-track.c.inc plus the replay semantics.
"""

import subprocess
from pathlib import Path

MAIN = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "melco-sensors-core.h"

static char path[512];

static void write_csv(const char *name, const char *content)
{
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", getenv("SCRATCH"), name);
    f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static MelcoTrack *load(const char *name, char *err, size_t err_size)
{
    snprintf(path, sizeof(path), "%s/%s", getenv("SCRATCH"), name);
    return melco_track_core_load(path, err, err_size);
}

int main(void)
{
    char err[512];
    MelcoTrack *t;
    const MelcoSample *s;
    double carry;
    unsigned whole;

    /* empty file: invalid track at line 1 */
    write_csv("empty.csv", "");
    assert(!load("empty.csv", err, sizeof(err)));
    assert(strstr(err, "invalid track") && strstr(err, "at line 1"));

    /* bad header: invalid track at line 1 */
    write_csv("badhdr.csv", "t_s,lat,lon\n0,1,2\n");
    assert(!load("badhdr.csv", err, sizeof(err)));
    assert(strstr(err, "invalid track") && strstr(err, "at line 1"));

    /* uneven spacing: rows at t=0, 0.1, 0.25 -> invalid at line 4 */
    write_csv("uneven.csv",
              "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps\n"
              "0.0,10,20,30,36,90,5,1\n"
              "0.1,10,20,30,72,91,6,1\n"
              "0.25,10,20,30,0,92,0,0\n");
    assert(!load("uneven.csv", err, sizeof(err)));
    assert(strstr(err, "invalid track") && strstr(err, "at line 4"));

    /* out-of-range field: speed 500 km/h is rejected */
    write_csv("range.csv",
              "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps\n"
              "0.0,10,20,30,36,90,5,1\n"
              "0.1,10,20,30,500,91,6,1\n");
    assert(!load("range.csv", err, sizeof(err)));
    assert(strstr(err, "at line 3"));

    /* valid track: 3 rows at 100 ms */
    write_csv("good.csv",
              "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps\n"
              "0.0,10.0,20.0,30.0,36.0,90.0,5.0,1\n"
              "0.1,10.1,20.1,31.0,72.0,91.0,6.0,1\n"
              "0.2,10.2,20.2,32.0,0.0,92.0,7.0,0\n");
    t = load("good.csv", err, sizeof(err));
    assert(t);

    /* clock starts on first use; row = (now - start) / interval */
    s = melco_track_sample(t, 1000);
    assert(s->lat_deg == 10.0 && s->speed_kmh == 36.0 && s->fix);
    s = melco_track_sample(t, 1100);
    assert(s->lat_deg == 10.1 && s->speed_kmh == 72.0);
    s = melco_track_sample(t, 1250);
    assert(s->lat_deg == 10.2 && s->speed_kmh == 0.0 && !s->fix);
    assert(!melco_track_ended(t, 1299));
    assert(melco_track_ended(t, 1300));

    /* past the end: last position, speed 0, yaw 0, fix false */
    s = melco_track_sample(t, 1300);
    assert(s->lat_deg == 10.2 && s->lon_deg == 20.2 && s->alt_m == 32.0);
    assert(s->heading_deg == 92.0);
    assert(s->speed_kmh == 0.0 && s->yaw_dps == 0.0 && !s->fix);
    s = melco_track_sample(t, 999999);
    assert(s->lat_deg == 10.2 && s->speed_kmh == 0.0 && !s->fix);
    melco_track_free(t);

    /* odometer over two samples: 36 km/h for 100 ms = 1 m, then 72 km/h
     * for 100 ms = 2 m (3 m in total); past the end it stops integrating */
    t = load("good.csv", err, sizeof(err));
    assert(t);
    melco_track_start(t, 1000);
    assert(fabs(melco_track_odometer_m(t, 1100) - 1.0) < 1e-9);
    assert(fabs(melco_track_odometer_m(t, 1250) - 2.0) < 1e-9);
    assert(fabs(melco_track_odometer_m(t, 1500) - 0.0) < 1e-9);
    melco_track_free(t);

    /* pulses: carry fraction, cap at max_per_call and keep the excess */
    carry = 0.0;
    whole = melco_pulses(&carry, 10.0, 2.0, 19);   /* 20 pulses, cap 19 */
    assert(whole == 19 && carry == 1.0);
    whole = melco_pulses(&carry, 0.0, 2.0, 19);    /* the carried 1 */
    assert(whole == 1 && carry == 0.0);
    carry = 0.5;
    whole = melco_pulses(&carry, 9.0, 2.0, 19);    /* 18.5 -> 18 + 0.5 */
    assert(whole == 18 && fabs(carry - 0.5) < 1e-9);
    carry = 0.0;
    whole = melco_pulses(&carry, 1.0, 2.0, 19);    /* below the cap */
    assert(whole == 2 && carry == 0.0);

    printf("melco-sensors-core: all checks passed\n");
    return 0;
}
'''


def test_sensors_core(tmp_path):
    machine = Path(__file__).parents[1] / "machine"
    core_c = (machine / "melco-sensors-core.c").read_text()
    core_h = (machine / "melco-sensors-core.h").read_text()
    src = tmp_path / "sensors_core_test.c"
    src.write_text(
        '#include "melco-sensors-core.h"\n' + MAIN + "\n" + core_c
    )
    inc = tmp_path / "melco-sensors-core.h"
    inc.write_text(core_h)
    binary = tmp_path / "sensors_core_test"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Werror", "-I", str(tmp_path),
         str(src), "-o", str(binary), "-lm"],
        check=True, capture_output=True, text=True,
    )
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            env={"SCRATCH": str(scratch), "PATH": "/usr/bin:/bin"})
    assert result.returncode == 0, result.stdout + result.stderr
