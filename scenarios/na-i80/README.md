# na-i80: I-80 westbound, Bonneville Salt Flats, Utah

3 minutes at a constant 110 km/h on the westbound carriageway of Interstate
80 across the Bonneville Salt Flats, Tooele County: 5502 m from 40.73152 N
113.48609 W to 40.73301 N 113.55136 W, one straight OpenStreetMap segment
(0.00 m off the straight line), heading 271.7°, 1286-1288 m above sea level.

```sh
CUP2006_SCENARIO=na-i80 make boot       # with the North America discs
```

Measured on a new unit (no saved GPS data): the guest's first fix comes
34.5 s after power-on and holds until the recording ends about 180 s after
power-on; after that the receiver reports signal loss. The map view was
checked with the Europe discs only, which have no map here; the fix itself
was checked from the guest setting its clock from GPS time.

| file | what |
|---|---|
| `drive.replay` | the GPS tracking chip's measurements, 359 frames of 500 ms |
| `sensors.csv` | wheel speed and gyro (`gpstrack=`), 10 Hz |
| `motion.csv` | the signal generator's input: t, lat, lon, height |
| `track.gpx` | the OSM geometry driven |
| `route.json` | provenance: Overpass query, OSM data timestamp, way ids, heights, signal settings |

Made with `tools/gps/osm_route.py --ref "I 80" --bbox 40.69,-114.00,40.80,-113.35
--seconds 180` and the pipeline in `tools/gps/README.md`; the GPS signal
starts at GPS time 2009-09-09 09:09:15 (week 1548, TOW 292155).

Map data © OpenStreetMap contributors, ODbL 1.0 (see scenarios/LICENSE).
Heights: Copernicus GLO-90 DEM (© Copernicus Programme), via the Open-Meteo
elevation API (CC BY 4.0, open-meteo.com).
