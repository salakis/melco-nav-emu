# Car-sensor track replay (sensors.csv)

Both boards replay the same uniformly sampled car-sensor CSV through the
shared `machine/melco-sensors.c` module (`gpstrack=` on CU-P2006, `--track` /
`gpstrack=` on CUP28).

## CSV format

```
t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps
0.0,38.8876761,-3.3922602,675.0,110.00,183.60,0.00,1
0.1,38.8876487,-3.3922626,675.0,110.00,183.27,-3.31,1
```

- The first line must be exactly the header above.
- One row per sample, **evenly spaced** in `t_s` (the first two rows set the
  interval; every later row must sit on it), at least 2 rows.
- Ranges: `t_s` 0..86400, |`lat_deg`| ≤ 90, `lon_deg` −180..180 (exclusive),
  `alt_m` int16, `speed_kmh` 0..400, `heading_deg` 0..360 (exclusive),
  |`yaw_dps`| ≤ 150, `gps` 0 or 1. Every field must parse fully as a finite
  number and there must be exactly 8 fields.
- A malformed file is rejected at startup with
  `invalid track '<path>' at line <N>`.

## End-of-track rule

The replay is finite: past the last row the car is at the last position with
speed 0, yaw 0 and no fix (`gps` ignored), and the odometer stops.

## Clock start per board

- **CUP28**: the clock starts at the first use of the track (the first
  sensor/MOST sample).
- **CU-P2006**: the clock starts when the track is loaded, or at the first
  radio-replay tick when only `gnssreplay=` is set.

## Which board signals each column drives

| column | CUP28 | CU-P2006 |
|---|---|---|
| `lat_deg`, `lon_deg` | position reported over MOST (E00) | not used (the fix comes from `drive.replay`) |
| `alt_m` | altitude over MOST | not used |
| `speed_kmh` | MOST wheel-pulse count (cumulative odometer, ≤19 pulses per 100 ms tick) and, with `gps=1`, the speed in the E00 report | wheel-pulse frames to the Sub-CPU (0.44902 pulses/m, carry kept) |
| `heading_deg` | heading in the E00 report | not used |
| `yaw_dps` | analog gyro input | CARSIG gyro |
| `gps` | fix valid/invalid in the E00 report | not used |

Without a track, CUP28 stays at a fixed fix in San Francisco and CU-P2006's
receiver has no signal.
