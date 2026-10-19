#!/usr/bin/env python3
"""Standing spots past a Halo 3 (MCC) map's boundaries, for exploring with the boundaries switched off (the stats page's
Teleport list; user, 2026-10-18: "several known good locations for standing on the other side of what used to be
boundaries ... for all maps").

    python3 re-tools/h3spots.py <file.map> <out.spots.json> [--cell 0.5] [--max 8] [--quiet] [--keep]

Built like re-tools/h3nav.py's floor field (same collision geometry: structure BSPs, instanced geometry, invisible
player clip, placed scenery and crates), but it keeps exactly the floors h3nav leaves out: a floor (upward surface, slip
terrain included -- walkable with the boundary patches) counts as BEYOND THE BOUNDARIES when it lies in a kill trigger
volume or outside every safe zone (maps without safe zones: kill volumes only). A spot needs STAND_H of free space above
it. Neighbouring beyond-the-boundary floors within STEP of each other form flat regions; a region of at least MIN_AREA
square units gives spots: its most interior cell (farthest from the region's edge -- walls, drops, the way back in),
so a teleport lands on solid, roomy ground, then (big regions) cells spread over the region (farthest-point, at least
a quarter as far from the edge as the first). Up to --max
spots, every region's best first, at least MIN_SPACING apart. --keep (after shim/tools/spots-verify.sh checked the file in
game): the verified spots stay, rejected places are never proposed again, new spots fill up to --max (unchecked again).

out.spots.json: {"map", "version": 1, "centre": [x, y], "spots": [{"id", "label", "x", "y", "z", "area", "room"}]}
  (spots-verify.sh adds "verified" / "check" per spot, "rejected" [spots with "why"] and "checked" <date>)
  z = the floor + 0.35 (where a teleport puts the feet); area = the region's floor in square units; room = the distance
  from the spot to the region's edge. Labels: compass direction from the play area's centre + "high ground" / "low
  ground" (8 units above / below the in-bounds floors' median height) or "edge".
Read-only, static file work. The stats page lists them per map; the host still checks each teleport against the map's
structure (scenario_location_from_point) and refuses a point outside it.
"""
import argparse, collections, json, math, os, sys, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h3map import H3Map  # noqa: E402
from h3bsp import Resources, level_triangles  # noqa: E402
from h3bounds import read_trigger_volumes  # noqa: E402
from h3nav import (placed_object_tris, split_long, sample_count, cell_samples, in_box,  # noqa: E402
                   WALK_NZ, CLEAR_EPS, LEVEL_GAP)

STAND_H = 1.0          # free space above a spot (a standing Spartan)
STEP = 0.5             # neighbouring cells of one flat region differ by at most this
MIN_AREA = 12.0        # square units of floor a region needs to give a spot
MIN_SPACING = 18.0     # spots at least this far apart
DIRS8 = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
COMPASS = ['East', 'North-east', 'North', 'North-west', 'West', 'South-west', 'South', 'South-east']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('map'); ap.add_argument('out')
    ap.add_argument('--cell', type=float, default=0.5)
    ap.add_argument('--max', type=int, default=8)
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--keep', action='store_true', help='keep the verified spots of an existing out file, skip its rejected ones')
    o = ap.parse_args()
    log = (lambda *a: None) if o.quiet else (lambda *a: print(*a, file=sys.stderr))
    t0 = time.time()
    m = H3Map(o.map); res = Resources(m)

    class Opts:
        invisible = True; unmaterialed = True; no_instances = False; render_only = False
    tris, _k, two, flags = level_triangles(m, res, Opts, log=log, with_flags=True)
    sc = m.find('scnr')[0]; d = m.meta(sc, 0x4A0)
    name = os.path.splitext(os.path.basename(o.map))[0]
    tvs, _a, _b = read_trigger_volumes(m, d)
    ot, of = placed_object_tris(m, m.meta(sc, 0x5D4), log)
    if len(ot):
        tris = np.concatenate([tris, ot]); two = np.concatenate([two, np.zeros(len(ot), bool)]); flags = np.concatenate([flags, of])
    kills = [t for t in tvs if t['kind'] == 'kill']; safes = [t for t in tvs if t['kind'] == 'safe']
    nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]); ln = np.linalg.norm(nrm, axis=1)
    ok = ln > 1e-9; tris, two, flags, nrm, ln = tris[ok], two[ok], flags[ok], nrm[ok], ln[ok]
    nz = nrm[:, 2] / ln
    walk = (nz >= WALK_NZ) | (two & (nz <= -WALK_NZ))          # slip terrain included: walkable with the boundaries off
    # the area: where the walkable surfaces are (the play area's safe zones included), not the far shell
    wv = tris[walk].reshape(-1, 3)
    lo, hi = np.percentile(wv[:, :2], 0.5, 0), np.percentile(wv[:, :2], 99.5, 0)
    if safes:
        slo = np.min([[t['aabb']['x'][0], t['aabb']['y'][0]] for t in safes], 0)
        shi = np.max([[t['aabb']['x'][1], t['aabb']['y'][1]] for t in safes], 0)
        lo, hi = np.minimum(lo, slo), np.maximum(hi, shi); centre = (slo + shi) / 2
    else:
        centre = (np.percentile(wv[:, :2], 10, 0) + np.percentile(wv[:, :2], 90, 0)) / 2
    lo, hi = lo - 2.0, hi + 2.0
    tlo, thi = tris[:, :, :2].min(1), tris[:, :, :2].max(1)
    keep = (thi[:, 0] >= lo[0]) & (tlo[:, 0] <= hi[0]) & (thi[:, 1] >= lo[1]) & (tlo[:, 1] <= hi[1])
    giant = (~walk) & (ln / 2 > 5000)                            # the map's outer shells (Forge World)
    keep &= ~giant
    tris, walk = tris[keep], walk[keep]
    cell = o.cell
    area = float(np.linalg.norm(np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]), axis=1).sum()) / 2
    cell = max(cell, 2 * math.sqrt(area / 8e6), math.sqrt((hi[0] - lo[0]) * (hi[1] - lo[1]) / 6e6))
    cell = math.ceil(cell * 8) / 8
    while True:
        st, sw = split_long(tris, walk, cell * 0.5 * 16)
        if sample_count(st, cell * 0.5) <= 10e6:
            break
        cell = math.ceil(cell * 1.25 * 8) / 8
    w, h = int(np.ceil((hi[0] - lo[0]) / cell)), int(np.ceil((hi[1] - lo[1]) / cell)); x0, y0 = float(lo[0]), float(lo[1])
    log('%s: area x %.0f..%.0f y %.0f..%.0f, cells of %.3f (%d x %d); %d kill volumes, %d safe zones' % (
        name, lo[0], hi[0], lo[1], hi[1], cell, w, h, len(kills), len(safes)))
    cid, sz, f = cell_samples(st, sw, cell * 0.5, x0, y0, cell, w, h); del st, sw
    BIG = 1e4
    skey = np.sort(cid * BIG + (sz.astype(np.float64) + 5000.0))
    fc, fz = cid[f], sz[f].astype(np.float64); del sz, cid, f
    order = np.lexsort((fz, fc)); fc, fz = fc[order], fz[order]
    new = np.ones(len(fc), bool); new[1:] = (fc[1:] != fc[:-1]) | (fz[1:] - fz[:-1] > LEVEL_GAP)
    grp = np.cumsum(new) - 1
    lvl_c = fc[new]; lvl_z = np.full(grp[-1] + 1, -np.inf); np.maximum.at(lvl_z, grp, fz)
    a = np.searchsorted(skey, lvl_c * BIG + (lvl_z + CLEAR_EPS + 5000.0), 'right')
    b = np.searchsorted(skey, lvl_c * BIG + (lvl_z + STAND_H + 5000.0), 'left')
    clear = b <= a
    cx = x0 + (lvl_c % w + 0.5) * cell; cy = y0 + (lvl_c // w + 0.5) * cell
    P = np.stack([cx, cy, lvl_z + 0.3], 1)
    inkill = np.zeros(len(P), bool)
    for t in kills:
        inkill |= in_box(P, t)
    insafe = np.zeros(len(P), bool)
    for t in safes:
        insafe |= in_box(P, t)
    beyond = inkill | (~insafe if safes else np.zeros(len(P), bool))
    inside_z = lvl_z[clear & ~beyond]
    zmed = float(np.median(inside_z)) if len(inside_z) else float(np.median(lvl_z))
    cand = clear & beyond
    log('%d floor levels, %d clear for standing; %d beyond the boundaries' % (len(lvl_c), clear.sum(), cand.sum()))
    # one level per cell: the top beyond-the-boundary floor
    top = {}
    for c, z in zip(lvl_c[cand].tolist(), lvl_z[cand].tolist()):
        if c not in top or z > top[c]:
            top[c] = z
    # flat regions (8-neighbours within STEP)
    region = {}; regions = []
    for c0 in top:
        if c0 in region:
            continue
        rid = len(regions); region[c0] = rid; q = [c0]; cells = []
        while q:
            c = q.pop(); cells.append(c); x, y = c % w, c // w; z = top[c]
            for dx, dy in DIRS8:
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h:
                    n = ny * w + nx
                    if n in top and n not in region and abs(top[n] - z) <= STEP:
                        region[n] = rid; q.append(n)
        regions.append(cells)
    picks = []
    for rid, cells in enumerate(regions):
        ar = len(cells) * cell * cell
        if ar < MIN_AREA:
            continue
        cs = set(cells)
        # distance (in cells) to the region's edge: BFS from cells with a missing / off-height neighbour
        dist = {}; q = collections.deque()
        for c in cells:
            x, y = c % w, c // w
            for dx, dy in DIRS8:
                n = (y + dy) * w + (x + dx)
                if n not in cs or abs(top[n] - top[c]) > STEP:
                    dist[c] = 0; q.append(c); break
        while q:
            c = q.popleft(); x, y = c % w, c // w
            for dx, dy in DIRS8:
                n = (y + dy) * w + (x + dx)
                if n in cs and n not in dist:
                    dist[n] = dist[c] + 1; q.append(n)
        # its most interior cell first; a big region gives more spots, spread over it (farthest-point picks among its cells
        # at least a quarter as roomy, MIN_SPACING apart): one large floor past the boundaries -- a Forge world's
        # outskirts -- used to give one spot only
        zm = float(np.mean([top[k] for k in cells[:64]]))
        best = max(cells, key=lambda c: (dist.get(c, 0), -abs(top[c] - zm)))
        broom = dist.get(best, 0) * cell
        if broom < 1.0:                                          # nowhere a metre from an edge: a ridge or a ledge strip
            continue
        cand = [c for c in cells if dist.get(c, 0) * cell >= max(1.0, 0.25 * broom)]
        X = np.array([[x0 + (c % w + 0.5) * cell, y0 + (c // w + 0.5) * cell] for c in cand])
        bx, by = x0 + (best % w + 0.5) * cell, y0 + (best // w + 0.5) * cell
        sel = [best]; md = np.hypot(X[:, 0] - bx, X[:, 1] - by)
        while len(sel) < o.max:
            i = int(np.argmax(md))
            if md[i] < MIN_SPACING:
                break
            sel.append(cand[i]); md = np.minimum(md, np.hypot(X[:, 0] - X[i, 0], X[:, 1] - X[i, 1]))
        mine = [{'x': x0 + (c % w + 0.5) * cell, 'y': y0 + (c // w + 0.5) * cell, 'z': top[c] + 0.35, 'area': ar,
                 'room': dist.get(c, 0) * cell, 'rank': r} for r, c in enumerate(sel)]
        picks += mine
    picks.sort(key=lambda p: (p['rank'], -min(p['room'], 6.0), -p['area']))   # every region's best spot before seconds
    # --keep: a file checked in game (shim/tools/spots-verify.sh) keeps its verified spots; its rejected ones never come back
    prev_ok, prev_bad, prev = [], [], {}
    if o.keep and os.path.exists(o.out):
        prev = json.load(open(o.out))
        prev_ok = [q for q in prev.get('spots', []) if q.get('verified')]; prev_bad = prev.get('rejected', [])
    chosen = list(prev_ok)
    for p in picks:
        if len(chosen) >= o.max:
            break
        if any(math.hypot(p['x'] - r['x'], p['y'] - r['y']) < 4 and abs(p['z'] - r['z']) < 4 for r in prev_bad):
            continue
        if all(math.hypot(p['x'] - q['x'], p['y'] - q['y']) >= MIN_SPACING for q in chosen):
            chosen.append(p)
    used = collections.Counter(q['label'].rstrip('0123456789 ') for q in prev_ok)
    ids = {q['id'] for q in prev_ok}; nid = 1; added = 0
    for p in chosen:
        if 'id' in p:
            continue
        ang = math.atan2(p['y'] - centre[1], p['x'] - centre[0])
        d8 = COMPASS[int(round(ang / (math.pi / 4))) % 8]
        kind = 'high ground' if p['z'] > zmed + 8 else 'low ground' if p['z'] < zmed - 8 else 'edge'
        lab = '%s %s' % (d8, kind); used[lab] += 1
        if used[lab] > 1:
            lab += ' %d' % used[lab]
        while 's%d' % nid in ids:
            nid += 1
        ids.add('s%d' % nid); added += 1
        p.update(id='s%d' % nid, label=lab)
        for k in ('x', 'y', 'z', 'area', 'room'):
            p[k] = round(p[k], 2)
    keys = ('id', 'label', 'x', 'y', 'z', 'area', 'room', 'verified', 'check')
    out = {'map': name, 'version': 1, 'centre': [round(float(centre[0]), 2), round(float(centre[1]), 2)],
           'spots': [{k: p[k] for k in keys if k in p} for p in chosen]}
    if prev_bad:
        out['rejected'] = prev_bad
    if prev.get('checked') and not added:
        out['checked'] = prev['checked']                         # nothing new: still fully checked
    with open(o.out, 'w') as fo:
        json.dump(out, fo, indent=1)
    log('%s: %d regions beyond the boundaries, %d spots (%.1f s)' % (name, len(regions), len(chosen), time.time() - t0))
    for p in out['spots']:
        log('  %-28s (%7.2f %7.2f %6.2f)  area %6.0f  room %.1f' % (p['label'], p['x'], p['y'], p['z'], p['area'], p['room']))


if __name__ == '__main__':
    main()
