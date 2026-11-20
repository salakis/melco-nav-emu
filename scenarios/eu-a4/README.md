# eu-a4: A-4 southbound, La Mancha, Spain

3 minutes at a constant 110 km/h on the southbound carriageway of the A-4
(Autovía del Sur, E-5) between Manzanares and Valdepeñas, Ciudad Real:
5502 m from 38.88768 N 3.39226 W to 38.83824 N 3.39482 W, the straightest
5.5 km of the A-4 in OpenStreetMap within 38.80-39.40 N, 3.75-3.30 W (at
most 0.92 m off the straight line), heading 182.1-183.6°, 673-687 m above sea level.

```sh
CUP2006_SCENARIO=eu-a4 make boot        # with the Europe discs
```

Measured on a new unit (no saved GPS data): the guest's first fix comes
34.5 s after power-on; the map then follows the car down the A-4 past the
junction with the Shell station until the recording ends 180.5 s after
power-on. After that the receiver reports signal loss and the position stays
put. A unit that has had a fix starts faster.

| file | what |
|---|---|
| `drive.replay` | the GPS tracking chip's measurements, 359 frames of 500 ms |
| `sensors.csv` | wheel speed and gyro (`gpstrack=`), 10 Hz |
| `motion.csv` | the signal generator's input: t, lat, lon, height |
| `track.gpx` | the OSM geometry driven |
| `route.json` | provenance: Overpass query, OSM data timestamp, way ids, heights, signal settings |

Made with `tools/gps/osm_route.py --ref A-4 --bbox 38.80,-3.75,39.40,-3.30
--seconds 180` and the pipeline in `tools/gps/README.md`; the GPS signal
starts at GPS time 2009-09-09 09:09:15 (week 1548, TOW 292155).

Map data © OpenStreetMap contributors, ODbL 1.0. Heights: Copernicus GLO-90 DEM.
