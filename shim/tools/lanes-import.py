#!/usr/bin/env python3
"""Add recorded matches to the lanes (2026-11-08; shim/native/nlanes.c has the format and the rules).
match-recorder.py's recordings (ReclaimerMac/tracks/*.jsonl.gz: every player's position 10 times a second) are walked the way
nhook walks live players: each move between two samples of the same live body (at most 0.6 s apart, 0.02 .. 8 units, slower than
40 units/s) through the 0.5-unit grid, the length inside each cell in passes, on foot or in a vehicle apart, the on-foot direction
summed. Added to <heatmaps>/<map>.lanes under the same flock and merge as nhook (read, add, write a temporary file, rename), so it
is safe while a game runs: the game shows the import from its next save (30 s). Each recording's name goes into the file's
source list (never added twice); recordings made by an nhook that records lanes live ("lanes_live" in the recording's first line)
are skipped -- the game counted them already.
    python3 shim/tools/lanes-import.py [--tracks DIR] [--heatmaps DIR] [--map NAME] [--dry-run]
    python3 shim/tools/lanes-import.py --show <map>       what a map's lanes file holds"""
import fcntl, glob, gzip, json, math, os, struct, sys, time

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
TRACKS = arg('--tracks', os.path.join(SUP, 'tracks')); HEAT = arg('--heatmaps', os.environ.get('NHOOK_HEATMAP_DIR') or os.path.join(SUP, 'heatmaps'))
ONLY = arg('--map', None); DRY = '--dry-run' in sys.argv
CELL = 0.5; MAXDT = 0.6; MAXSPEED = 40.0
HDR = struct.Struct('<4sfIId4I'); REC = struct.Struct('<iiffff')

def read_lanes(path):
    g, src, since = {}, [], 0.0
    try: b = open(path, 'rb').read()
    except OSError: return g, src, since
    if len(b) < HDR.size: return g, src, since
    mg, cell, nc, ns, since, *_ = HDR.unpack_from(b, 0)
    if mg != b'RLN1' or cell != CELL: raise SystemExit('%s: not a lanes file of this cell size' % path)
    o = HDR.size
    for i in range(nc):
        x, y, f, v, fx, fy = REC.unpack_from(b, o); o += REC.size; g[(x, y)] = [f, v, fx, fy]
    for i in range(ns):
        src.append(b[o:o + 64].split(b'\0', 1)[0].decode('utf-8', 'replace')); o += 64
    return g, src, since

def write_lanes(path, g, src, since):
    tmp = '%s.tmp%d' % (path, os.getpid())
    with open(tmp, 'wb') as f:
        f.write(HDR.pack(b'RLN1', CELL, len(g), len(src), since, 0, 0, 0, 0))
        for (x, y), (fo, v, fx, fy) in g.items(): f.write(REC.pack(x, y, fo, v, fx, fy))
        for s in src: f.write(s.encode()[:63].ljust(64, b'\0'))
    os.rename(tmp, path)

def add_move(g, a, b, veh):   # nlanes.c ln_move, the same walk
    dx, dy = b[0] - a[0], b[1] - a[1]; d = math.hypot(dx, dy)
    if not (d > 0.02) or d > 8.0: return 0.0
    x0, y0, x1, y1 = a[0] / CELL, a[1] / CELL, b[0] / CELL, b[1] / CELL; ux, uy, L = dx / d, dy / d, d / CELL
    cx, cy, ex, ey = math.floor(x0), math.floor(y0), math.floor(x1), math.floor(y1)
    sx = 1 if x1 > x0 else -1; sy = 1 if y1 > y0 else -1; vx, vy = x1 - x0, y1 - y0
    tdx = abs(1.0 / vx) if vx else 1e30; tdy = abs(1.0 / vy) if vy else 1e30
    tmx = ((cx + 1 - x0) if sx > 0 else (x0 - cx)) * tdx if vx else 1e30
    tmy = ((cy + 1 - y0) if sy > 0 else (y0 - cy)) * tdy if vy else 1e30
    t = 0.0
    for _ in range(64):
        tn = min(tmx, tmy); last = (cx == ex and cy == ey) or tn >= 1.0
        if last: tn = 1.0
        part = (tn - t) * L
        if part > 1e-4:
            c = g.get((cx, cy))
            if c is None: c = g[(cx, cy)] = [0.0, 0.0, 0.0, 0.0]
            if veh: c[1] += part
            else: c[0] += part; c[2] += ux * part; c[3] += uy * part
        if last: break
        t = tn
        if tmx < tmy: tmx += tdx; cx += sx
        else: tmy += tdy; cy += sy
    return d

def walk_recording(path):   # -> map, header, {cell: [foot, veh, fx, fy]}, units on foot, units in vehicles
    g = {}; foot = veh = 0.0; prev = {}
    with gzip.open(path, 'rt') as f:
        try: head = json.loads(f.readline())
        except Exception: return None, {}, g, 0, 0
        while True:
            try: line = f.readline()
            except (EOFError, OSError): break   # (a recording cut off: what came before counts)
            if not line: break
            try: r = json.loads(line)
            except Exception: break
            if 'p' not in r: continue
            t = r.get('t', 0.0); seen = set()
            for p in r['p']:
                sl = p[0]; seen.add(sl); alive = p[2]; pos = (p[3], p[4]); deaths = p[13] if len(p) > 13 else 0; inv = bool(p[11]) if len(p) > 11 else False
                q = prev.get(sl)
                if alive and q and q[3] and q[4] == deaths and 0 < t - q[0] <= MAXDT and (pos[0] or pos[1]):
                    dd = math.hypot(pos[0] - q[1], pos[1] - q[2])
                    if dd / (t - q[0]) <= MAXSPEED:
                        n = add_move(g, (q[1], q[2]), pos, inv)
                        if inv: veh += n
                        else: foot += n
                prev[sl] = (t, pos[0], pos[1], alive, deaths)
            for sl in list(prev):
                if sl not in seen: del prev[sl]
    return head.get('map') or '', head, g, foot, veh

def show(m):
    g, src, since = read_lanes(os.path.join(HEAT, m + '.lanes'))
    f = sum(c[0] for c in g.values()); v = sum(c[1] for c in g.values())
    print('%s: %d cells, %.1f km on foot, %.1f km in vehicles, %d recordings imported, live since %s' % (m, len(g), f * CELL * 3.048 / 1000, v * CELL * 3.048 / 1000, len(src),
          time.strftime('%Y-%m-%d %H:%M', time.localtime(since)) if since else 'never'))
    if g:
        top = sorted(g.items(), key=lambda kv: -kv[1][0])[:5]
        print('  busiest cells (passes on foot):', ', '.join('(%.1f, %.1f) %.0f' % ((x + 0.5) * CELL, (y + 0.5) * CELL, c[0]) for (x, y), c in top))

def main():
    if '--show' in sys.argv: show(arg('--show', '')); return
    os.makedirs(HEAT, exist_ok=True)
    recs = sorted(glob.glob(os.path.join(TRACKS, '*.jsonl.gz')))
    by_map = {}
    for r in recs:
        if time.time() - os.path.getmtime(r) < 120: continue   # (still being recorded: the next run takes it)
        try:
            with gzip.open(r, 'rt') as f: head = json.loads(f.readline())
        except Exception: continue
        m = head.get('map') or ''
        if not m or (ONLY and m != ONLY): continue
        by_map.setdefault(m, []).append((r, head))
    total = 0
    for m, items in sorted(by_map.items()):
        path = os.path.join(HEAT, m + '.lanes'); lk = open(os.path.join(HEAT, m + '.lanes.lock'), 'a+')
        fcntl.flock(lk, fcntl.LOCK_EX)
        try:
            g, src, since = read_lanes(path); have = set(src); added = []; fo = ve = 0.0
            for r, head in items:
                name = os.path.basename(r)
                if name in have: continue
                if head.get('lanes_live'): continue   # (the game counted it live)
                _, _, h, f, v = walk_recording(r)
                for k, c in h.items():
                    o = g.get(k)
                    if o is None: g[k] = c
                    else:
                        for i in range(4): o[i] += c[i]
                added.append(name); fo += f; ve += v
            if added and not DRY: write_lanes(path, g, src + added, since)
            if added:
                total += len(added)
                print('%-28s +%d recordings (%.2f km on foot, %.2f km in vehicles)%s' % (m, len(added), fo * 3.048 / 1000, ve * 3.048 / 1000, ' [dry run]' if DRY else ''))
        finally:
            fcntl.flock(lk, fcntl.LOCK_UN); lk.close()
    print('%d recordings added%s' % (total, ' (dry run: nothing written)' if DRY else ''))

if __name__ == '__main__':
    main()
