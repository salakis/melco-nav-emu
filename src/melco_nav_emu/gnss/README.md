# Making a GPS scenario

A scenario is a directory with two inputs for the machine:

- `drive.replay`: what the unit's GPS tracking chip measures, satellite by
  satellite, over time (format in the top-level README). The guest's own GPS
  library computes every position from it.
- `sensors.csv`: the car's wheel-speed and gyro signals on the same clock.

The replay is made from a real GPS L1 signal rendered for the route, then
tracked by a software receiver, so the guest gets what the chip would have
measured on that road. Nothing in it is a position.

## Pipeline

1. **Route** from OpenStreetMap: the straightest stretch of a motorway in a
   box, driven at constant speed, with heights from the Copernicus DEM.

   ```sh
   python -m melco_nav_emu.gnss.osm_route --ref A-4 --bbox 38.80,-3.75,39.40,-3.30 \
       --out scenarios/eu-a4 --seconds 180 --kmh 110
   ```

   Writes `sensors.csv`, `motion.csv`, `track.gpx` and `route.json`
   (provenance: the Overpass query, the OSM data timestamp, way ids).

2. **Signal** with [GPS-SDR-SIM](https://github.com/osqzss/gps-sdr-sim)
   (MIT licence), built with room for a long motion file, and a broadcast
   ephemeris file (`brdc0010.22n` ships with it). Run it from the directory
   that holds the inputs: it keeps file names in short fixed buffers.

   The start time matters. A new unit's GPS library commits its first fix at
   a fixed second of the 30 s navigation frame (GPS 09:09:48 in these
   scenarios), and only once it has decoded the satellites' ephemeris.
   Measured on new units, first fix after power-on by GPS start time:
   09:09:03 46.5 s, :06 43.5 s, :09 40.5 s, :12 37.5 s, **:15 34.5 s**,
   :18 49.5 s, :21 46.5 s, :27 46.5 s. The shipped scenarios start at
   09:09:15.

   ```sh
   gcc -O3 -DUSER_MOTION_SIZE=5000 gpssim.c -lm -o gps-sdr-sim
   gps-sdr-sim -e brdc0010.22n -x motion.csv -d 180.0 -s 2500000 -b 16 \
       -o drive.bin -T 2009/09/09,09:09:15 -i -p 32
   ```

   180 s give a 1.8 GB I/Q file; it is only needed for the next step.

3. **Tracking** with [PocketSDR](https://github.com/tomojitakasu/PocketSDR)'s
   `pocket_trk` (BSD-2-Clause), built without SDR hardware support
   (`make USE_SOAPY=0` in `lib/build` and `app/pocket_trk`):

   ```sh
   pocket_trk -sig L1CA -prn 1-32 -fmt CS16 -f 2.5 -fo 0 -log drive.log \
       -ti 0 drive.bin
   ```

4. **Replay** from the tracking log (`$CH` and `$NAV` lines only; the
   receiver's own positions are never read):

   ```sh
   python -m melco_nav_emu.gnss.export_replay drive.log scenarios/eu-a4/drive.replay \
       --gps-tow 292155
   ```

   292155 s is the GPS time of week of 2009-09-09 09:09:15.

Use it with `CUP2006_SCENARIO=<name> make boot`; the profile looks in
`scenarios/<name>/` and then in `build/gps/<name>/`.
