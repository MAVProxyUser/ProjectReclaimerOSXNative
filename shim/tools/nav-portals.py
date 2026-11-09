#!/usr/bin/env python3
"""Links between the walking graph's separate parts, learned from real players (2026-11-08; the user: "enemy radar on multi level
maps ... above or below you. the engagement struggled with that"). re-tools/h3nav.py's floor graph only joins floors a player can
WALK between (steps of 0.35, drops up to 2): gravity lifts, man cannons and jumps are missing, so on Guardian the upper levels
(z ~22) are separate parts from the main floor (z 15-20) -- 423 parts in all -- and a walker under an enemy had "no path" (718
goals dropped in one round). Every recorded player that went from one part to another within 4 s, alive all the way (a lift, a
cannon, a jump), is such a link: its start node (the last node in the part it left) and end node (the first in the part it reached),
grouped by both nodes' 2-unit cells; groups seen 3 times or more become one-way portals with the players' median travel time; a STEP
(ends under 3 units apart, under 1.5 up / down: floor the graph's 0.35 step split) counts from one sighting, both ways.
Reads every recording of the map (tracks/ + battles/tracks/), writes maptop/<map>.portals.txt ("from to seconds count" per line;
nav.c loads it beside <map>.nav).
    python3 shim/tools/nav-portals.py <map> [--min 3] [--quiet]"""
import collections, glob, gzip, json, math, os, struct, sys

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
arg = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d

class Nav:
    def __init__(self, path):
        b = open(path, 'rb').read(); assert b[:4] == b'RNV1'
        self.x0, self.y0, self.cell = struct.unpack_from('<fff', b, 8); self.w, self.h, self.n = struct.unpack_from('<iii', b, 20)
        self.first = struct.unpack_from('<%dI' % (self.w * self.h + 1), b, 64); off = 64 + 4 * (self.w * self.h + 1)
        self.z = []; self.comp = []
        for i in range(self.n): z, c = struct.unpack_from('<fH', b, off + 40 * i); self.z.append(z); self.comp.append(c)
        self.cellof = [0] * self.n
        for ci in range(self.w * self.h):
            for i in range(self.first[ci], self.first[ci + 1]): self.cellof[i] = ci
    def pos(self, i):
        ci = self.cellof[i]; return (self.x0 + (ci % self.w + 0.5) * self.cell, self.y0 + (ci // self.w + 0.5) * self.cell, self.z[i])
    def node_at(self, x, y, z):
        """the floor node under a player (its feet ~z - 0.35): the nearest floor at or below z + 0.5 in its cell or the 8 around"""
        cx, cy = int((x - self.x0) // self.cell), int((y - self.y0) // self.cell); best = -1; bd = 1e9
        for dx in (0, -1, 1):
            for dy in (0, -1, 1):
                X, Y = cx + dx, cy + dy
                if not (0 <= X < self.w and 0 <= Y < self.h): continue
                ci = Y * self.w + X
                for i in range(self.first[ci], self.first[ci + 1]):
                    dz = (z - 0.35) - self.z[i]
                    if -0.6 <= dz <= 1.2:
                        d = abs(dz) + 0.5 * (abs(dx) + abs(dy))
                        if d < bd: bd = d; best = i
            if best >= 0 and bd < 0.8: break
        return best

def tracks_of(m):
    for p in glob.glob(os.path.join(SUP, 'tracks', '*.jsonl.gz')) + glob.glob(os.path.join(SUP, 'battles', 'tracks', '*.jsonl.gz')):
        if '_%s_' % m not in os.path.basename(p) and not os.path.basename(p).endswith('_%s_local.jsonl.gz' % m): continue
        yield p

def main():
    m = [a for a in sys.argv[1:] if not a.startswith('--')][0]; MIN = int(arg('--min', '3'))
    nav = Nav(os.path.join(SUP, 'maptop', m + '.nav')); T = collections.defaultdict(list); files = 0
    for p in tracks_of(m):
        files += 1; last = {}
        try:
            with gzip.open(p, 'rt') as f:
                for l in f:
                    try: o = json.loads(l)
                    except Exception: break
                    if 'p' not in o: continue
                    t = o['t']
                    for r in o['p']:
                        k = r[0]
                        if not r[2] or not (r[3] or r[4]): last.pop(k, None); continue
                        nd = nav.node_at(r[3], r[4], r[5])
                        if nd < 0: continue   # (in the air / on a lift: the last floor node stays)
                        pv = last.get(k)
                        if pv and nav.comp[nd] != nav.comp[pv[1]] and t - pv[0] <= 4.0:
                            a, b = pv[1], nd; pa, pb = nav.pos(a), nav.pos(b)
                            key = (nav.comp[a], nav.comp[b], round(pa[0] / 2), round(pa[1] / 2), round(pb[0] / 2), round(pb[1] / 2))
                            T[key].append((a, b, t - pv[0]))
                        last[k] = (t, nd)
        except (EOFError, OSError): pass
    # STEPS (2026-11-09, Hugegrass: the spawn room's floor tiles differ by ~0.9 units, over the graph's 0.35 step, so the room fell apart
    #  into dozens of parts and the walker stood on one with no way out -- players walk over them): a crossing between parts whose ends
    #  are under 3 units apart across and 1.5 up / down is FLOOR, not a lift: kept from ONE sighting, both ways
    portals = []; have = set()
    for key, v in T.items():
        a = collections.Counter(x[0] for x in v).most_common(1)[0][0]; b = collections.Counter(x[1] for x in v).most_common(1)[0][0]
        pa, pb = nav.pos(a), nav.pos(b); step = math.hypot(pa[0] - pb[0], pa[1] - pb[1]) < 3.0 and abs(pa[2] - pb[2]) < 1.5
        if len(v) < MIN and not step: continue
        dt = sorted(x[2] for x in v)[len(v) // 2]; portals.append((a, b, dt, len(v))); have.add((a, b))
        if step and (b, a) not in have: portals.append((b, a, dt, len(v))); have.add((b, a))
    portals.sort(key=lambda x: -x[3])
    out = os.path.join(SUP, 'maptop', m + '.portals.txt')
    with open(out + '.tmp', 'w') as f:
        f.write('# nav-portals.py: from to seconds count (one-way links between the walking graph\'s parts, from %d recordings)\n' % files)
        for a, b, dt, n in portals: f.write('%d %d %.2f %d\n' % (a, b, dt, n))
    os.replace(out + '.tmp', out)
    if '--quiet' not in sys.argv:
        print('%s: %d recordings, %d crossings seen, %d portals (seen %d+ times) -> %s' % (m, files, sum(len(v) for v in T.values()), len(portals), MIN, out))
        for a, b, dt, n in portals[:15]:
            pa, pb = nav.pos(a), nav.pos(b)
            print('  (%.1f %.1f %.1f) part %d -> (%.1f %.1f %.1f) part %d: %.1f s, %d times' % (pa[0], pa[1], pa[2], nav.comp[a], pb[0], pb[1], pb[2], nav.comp[b], dt, n))
    return 0

if __name__ == '__main__':
    sys.exit(main())
