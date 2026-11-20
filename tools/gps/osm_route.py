#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Pick the straightest stretch of a motorway from OpenStreetMap and write a
GPS scenario's truth track for it.

The motorway is taken from OSM through the Overpass API: every way tagged
highway=motorway whose ref list contains --ref inside --bbox.  Ways are joined
into carriageways (OSM motorways are one-way; a way continues into the way that
starts at its last node), and a window of the drive's length slides along each
carriageway.  The window whose points stay closest to the straight line between
its ends wins.  Heights along it come from the Copernicus GLO-90 DEM through
the Open-Meteo elevation API.

The car drives the window at a constant --kmh from t=0.  Output in --out:

    sensors.csv  the machine's gpstrack= input: t,lat,lon,alt,kmh,hdg,yaw,gps
    motion.csv   GPS-SDR-SIM -x input: t,lat,lon,height
    track.gpx    the window's OSM geometry
    route.json   provenance: query, OSM data timestamp, way ids, straightness

    python3 tools/gps/osm_route.py --ref A-4 --bbox 38.80,-3.75,39.40,-3.30 \\
        --out scenarios/eu-a4 --seconds 180 [--kmh 110]

Map data (c) OpenStreetMap contributors, ODbL 1.0.
"""
import argparse
import json
import math
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

OVERPASS = "https://overpass-api.de/api/interpreter"
ELEVATION = "https://api.open-meteo.com/v1/elevation"
AGENT = "melco-nav-emu scenario builder"
R_EARTH = 6371008.8


def fetch_json(url, data=None, tries=3):
    """GET/POST with a few retries: public Overpass servers rate-limit."""
    for attempt in range(tries):
        try:
            return _fetch_json(url, data)
        except (urllib.error.URLError, subprocess.CalledProcessError) as e:
            if attempt == tries - 1:
                raise
            print("fetch failed (%s); retrying in 20 s" % e, file=sys.stderr)
            time.sleep(20)


def _fetch_json(url, data=None):
    req = urllib.request.Request(url, data=data, headers={"User-Agent": AGENT})
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.loads(r.read().decode())
    except urllib.error.URLError:
        # Some Python builds (e.g. macOS's system 3.9) cannot complete a
        # modern TLS handshake; curl can.
        cmd = ["curl", "-sSf", "-m", "180", "-A", AGENT, url]
        if data is not None:
            cmd += ["--data-binary", "@-"]
        out = subprocess.run(cmd, input=data, capture_output=True, check=True)
        return json.loads(out.stdout.decode())


def overpass(ref, bbox):
    s, w, n, e = bbox
    pat = "(^|;)%s($|;)" % re.escape(ref)
    query = ('[out:json][timeout:120];way["highway"="motorway"]["ref"~"%s"]'
             '(%f,%f,%f,%f);(._;>;);out body;' % (pat, s, w, n, e))
    data = urllib.parse.urlencode({"data": query}).encode()
    return query, fetch_json(OVERPASS, data)


def elevations(points):
    """Heights (m) for (lat, lon) points, 100 per request."""
    out = []
    for i in range(0, len(points), 100):
        chunk = points[i:i + 100]
        q = urllib.parse.urlencode({
            "latitude": ",".join("%.6f" % p[0] for p in chunk),
            "longitude": ",".join("%.6f" % p[1] for p in chunk)})
        out += fetch_json(ELEVATION + "?" + q)["elevation"]
    return out


def local_xy(p, origin):
    """Metres east/north of origin (equirectangular, fine over a few km)."""
    k = math.cos(math.radians(origin[0]))
    return (math.radians(p[1] - origin[1]) * R_EARTH * k,
            math.radians(p[0] - origin[0]) * R_EARTH)


def dist(a, b):
    x, y = local_xy(b, a)
    return math.hypot(x, y)


def carriageways(osm):
    """Directed node chains of the motorway ways, as lists of (lat, lon)."""
    nodes = {e["id"]: (e["lat"], e["lon"]) for e in osm["elements"]
             if e["type"] == "node"}
    ways = []
    for e in osm["elements"]:
        if e["type"] != "way" or len(e["nodes"]) < 2:
            continue
        nds = list(e["nodes"])
        if e.get("tags", {}).get("oneway") == "-1":
            nds.reverse()
        ways.append({"id": e["id"], "nodes": nds})
    starts = {}
    for w in ways:
        starts.setdefault(w["nodes"][0], []).append(w)
    ends = {w["nodes"][-1] for w in ways}
    chains, used = [], set()

    def heading(a, b):
        x, y = local_xy(nodes[b], nodes[a])
        return math.atan2(x, y)

    for first in ways:
        if first["id"] in used or first["nodes"][0] in ends:
            continue
        chain, ids, w = [], [], first
        while w and w["id"] not in used:
            used.add(w["id"])
            ids.append(w["id"])
            chain += w["nodes"] if not chain else w["nodes"][1:]
            nxt = [c for c in starts.get(w["nodes"][-1], ())
                   if c["id"] not in used]
            if not nxt:
                break
            h0 = heading(chain[-2], chain[-1])
            w = min(nxt, key=lambda c: abs((heading(c["nodes"][0], c["nodes"][1])
                                            - h0 + math.pi) % (2 * math.pi)
                                           - math.pi))
        chains.append({"ways": ids, "points": [nodes[n] for n in chain]})
    return chains


def resample(pts, step=1.0):
    out, carry = [pts[0]], 0.0
    for a, b in zip(pts, pts[1:]):
        seg = dist(a, b)
        if seg <= 0:
            continue
        at = step - carry
        while at <= seg:
            f = at / seg
            out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
            at += step
        carry = (carry + seg) % step
    return out


def straightest(line, length, step=25):
    """(deviation m, start index) of the window of `length` 1 m samples whose
    points stay closest to the chord between its ends."""
    best = None
    for i in range(0, len(line) - length - 1, step):
        a, b = line[i], line[i + length]
        bx, by = local_xy(b, a)
        chord = math.hypot(bx, by)
        dev = max(abs(local_xy(p, a)[0] * by - local_xy(p, a)[1] * bx) / chord
                  for p in line[i:i + length + 1:10])
        if best is None or dev < best[0]:
            best = (dev, i)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="motorway ref, e.g. A-4 or I 80")
    ap.add_argument("--bbox", required=True,
                    help="south,west,north,east in degrees")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--kmh", type=float, default=110.0)
    ap.add_argument("--rate", type=float, default=10.0)
    args = ap.parse_args()
    bbox = [float(v) for v in args.bbox.split(",")]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    query, osm = overpass(args.ref, bbox)
    v = args.kmh / 3.6
    length = int(math.ceil(v * args.seconds)) + 2
    best = None
    for c in carriageways(osm):
        line = resample(c["points"])
        if len(line) <= length + 2:
            continue
        found = straightest(line, length)
        if found and (best is None or found[0] < best[0]):
            best = (found[0], found[1], line, c)
    if best is None:
        sys.exit("no carriageway of %s in the box is %d m long" % (args.ref, length))
    dev, i0, line, chain = best
    win = line[i0:i0 + length + 1]

    # Heights every 100 m, linear in between.
    marks = list(range(0, len(win), 100)) + [len(win) - 1]
    hs = elevations([win[m] for m in marks])

    def alt(s):
        for (ma, ha), (mb, hb) in zip(zip(marks, hs), zip(marks[1:], hs[1:])):
            if s <= mb:
                return ha + (hb - ha) * (s - ma) / max(1, mb - ma)
        return hs[-1]

    def at(s):
        k = min(int(s), len(win) - 2)
        f = s - k
        a, b = win[k], win[k + 1]
        return a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f

    def heading(s):
        a, b = at(max(0.0, s - 10.0)), at(min(len(win) - 1.001, s + 10.0))
        x, y = local_xy(b, a)
        return math.degrees(math.atan2(x, y)) % 360.0

    rows, last, dt = [], None, 1.0 / args.rate
    for n in range(int(round(args.seconds * args.rate)) + 1):
        t, s = n * dt, n * dt * v
        lat, lon = at(s)
        h = heading(s)
        yaw = 0.0 if last is None else ((h - last + 180.0) % 360.0 - 180.0) / dt
        rows.append((t, lat, lon, alt(s), args.kmh, h, yaw))
        last = h

    (out / "sensors.csv").write_text(
        "t_s,lat_deg,lon_deg,alt_m,speed_kmh,heading_deg,yaw_dps,gps\n" +
        "".join("%.1f,%.7f,%.7f,%.1f,%.2f,%.2f,%.2f,1\n" % r for r in rows))
    (out / "motion.csv").write_text(
        "".join("%.1f,%.9f,%.9f,%.3f\n" % r[:4] for r in rows))
    (out / "track.gpx").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<gpx version="1.1" creator="melco-nav-emu tools/gps/osm_route.py">\n'
        '<metadata><copyright author="OpenStreetMap contributors">'
        '<license>https://opendatacommons.org/licenses/odbl/1-0/</license>'
        '</copyright></metadata>\n<trk><name>%s</name><trkseg>\n' % args.ref +
        "".join('<trkpt lat="%.7f" lon="%.7f"><ele>%.1f</ele></trkpt>\n'
                % (p[0], p[1], alt(k)) for k, p in enumerate(win) if k % 50 == 0
                or k == len(win) - 1) + '</trkseg></trk></gpx>\n')
    hdgs = [r[5] for r in rows]
    route = {
        "ref": args.ref, "bbox": bbox, "overpass_query": query,
        "osm_timestamp": osm.get("osm3s", {}).get("timestamp_osm_base"),
        "osm_way_ids": chain["ways"],
        "start": [round(win[0][0], 7), round(win[0][1], 7)],
        "end": [round(win[-1][0], 7), round(win[-1][1], 7)],
        "length_m": len(win) - 1, "max_deviation_from_chord_m": round(dev, 2),
        "heading_deg": [round(min(hdgs), 2), round(max(hdgs), 2)],
        "height_m": [round(min(hs), 1), round(max(hs), 1)],
        "height_source": "Copernicus GLO-90 DEM via api.open-meteo.com",
        "seconds": args.seconds, "kmh": args.kmh, "rate_hz": args.rate,
        "license": "Map data (c) OpenStreetMap contributors, ODbL 1.0",
    }
    (out / "route.json").write_text(json.dumps(route, indent=2) + "\n")
    print("%s: %d m from %.6f,%.6f to %.6f,%.6f, max %.2f m off the chord,"
          " heading %.1f..%.1f deg, height %.0f..%.0f m (OSM %s)"
          % (args.ref, len(win) - 1, win[0][0], win[0][1], win[-1][0],
             win[-1][1], dev, min(hdgs), max(hdgs), min(hs), max(hs),
             route["osm_timestamp"]))


if __name__ == "__main__":
    main()
