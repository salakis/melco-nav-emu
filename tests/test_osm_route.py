# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""gnss/osm_route.py: carriageway joining and the straightness pick,
on a synthetic OSM extract (no network)."""
from melco_nav_emu.gnss import osm_route


def _osm(ways, nodes):
    return {"elements": [{"type": "node", "id": i, "lat": la, "lon": lo}
                         for i, (la, lo) in nodes.items()] +
                        [{"type": "way", "id": w, "nodes": n, "tags": {}}
                         for w, n in ways.items()]}


def test_ways_join_into_one_carriageway():
    nodes = {1: (40.0, 0.0), 2: (40.0, 0.01), 3: (40.0, 0.02), 4: (40.0, 0.03)}
    osm = _osm({10: [1, 2], 11: [2, 3, 4]}, nodes)
    chains = osm_route.carriageways(osm)
    assert len(chains) == 1
    assert chains[0]["ways"] == [10, 11]
    assert chains[0]["points"] == [nodes[k] for k in (1, 2, 3, 4)]


def test_straightest_window_avoids_the_bend():
    # 3 km east, then a 1 km kink north-east: a 1 km window must sit on
    # the straight part.
    pts = [(40.0, 0.0), (40.0, 0.0352), (40.0064, 0.0436)]
    line = osm_route.resample(pts)
    dev, start = osm_route.straightest(line, 1000)
    assert dev < 0.5
    assert start + 1000 <= 3000
