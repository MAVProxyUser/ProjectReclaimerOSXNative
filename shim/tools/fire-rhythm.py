#!/usr/bin/env python3
"""How players fire automatic weapons by range (2026-11-08; the user, about a walker hosing an assault rifle at range: "it is shanking
shots due to the random bloom on the AR, spreading out at range, so it just felt like lack of weapon proficiency"). From the
recordings (every player 10 times a second: position, look, weapon, SHOTS counter): moments a player holding an assault rifle /
SMG / plasma rifle / spiker aims within 8 deg of an enemy (its look vs the line to the enemy, horizontal) are grouped by distance
(<8, 8-15, 15-25, 25+ units); in them, a BURST = consecutive 0.1 s samples whose shots counter went up, a PAUSE = the samples
between two bursts of the same aiming stretch. Per band: shots per second while aiming, burst length (s) and pause (s) medians,
share of the aiming time spent firing -- for the players in the top quarter by K/D (per match, 3+ kills), all players, and the
walkers (battles/tracks, if --walkers). Writes tracks/fire-rhythm.json (nbot.c: the walker's bursts at range).
    python3 shim/tools/fire-rhythm.py [--walkers] [--quiet]"""
import collections, glob, gzip, json, math, os, sys

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); TR = os.path.join(SUP, 'tracks')
AUTO = ('ASSAULT RIFLE', 'SMG', 'PLASMA RIFLE', 'SPIKE', 'SPIKER')
BANDS = [(0, 8), (8, 15), (15, 25), (25, 60)]; INF = ('infection', 'zombie', 'flood')

def read(p):
    head = {}; S = []
    try:
        with gzip.open(p, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o
                elif 'p' in o: S.append(o)
    except (EOFError, OSError): pass
    return head, S

def aimed_band(r, R):
    """the band of the nearest enemy within 8 deg of r's look, or None"""
    best = None
    for q in R:
        if q is r or not q[2] or (r[1] is not None and q[1] == r[1] and r[1] >= 0 and q[0] != r[0] and False): continue
        if q[0] == r[0] or (q[1] == r[1] and team_game[0]): continue
        dx, dy = q[3] - r[3], q[4] - r[4]; d = math.hypot(dx, dy)
        if d < 0.5 or d > 60: continue
        off = abs((math.degrees(math.atan2(dy, dx)) - (r[6] or 0) + 180) % 360 - 180)
        if off <= 8 and (best is None or d < best): best = d
    if best is None: return None
    for i, (lo, hi) in enumerate(BANDS):
        if lo <= best < hi: return i
    return None
team_game = [False]

def study(paths):
    acc = {}   # (group, band) -> dict
    for p in paths:
        head, S = read(p)
        mode = ' '.join(str(head.get(k) or '') for k in ('engine', 'variant', 'mode')).lower()
        if len(S) < 300 or any(k in mode for k in INF): continue
        teams = collections.defaultdict(set)
        for s in S[::50]:
            for r in s['p']:
                if r[1] is not None and r[1] >= 0: teams[r[1]].add(r[0])
        team_game[0] = sum(1 for v in teams.values() if len(v) >= 2) >= 2
        last = {r[0]: r for r in S[-1]['p']}
        kd = {k: (r[12] + 1) / (r[13] + 1) for k, r in last.items() if r[12] >= 3}
        q75 = sorted(kd.values())[int(0.75 * (len(kd) - 1))] if kd else 99
        prev = {}; run = {}
        for s in S:
            R = [r for r in s['p'] if r[2] and (r[3] or r[4])]
            for r in R:
                k = r[0]; pv = prev.get(k); prev[k] = r
                if not pv or not any(w in (r[10] or '') for w in AUTO): run.pop(k, None); continue
                b = aimed_band(r, R)
                fired = r[14] > pv[14]; n = r[14] - pv[14] if fired else 0
                st = run.get(k)
                if b is None:
                    if st: close(acc, st, kd.get(k), q75); run.pop(k, None)
                    continue
                if not st or st['band'] != b:
                    if st: close(acc, st, kd.get(k), q75)
                    st = run[k] = {'band': b, 'samples': 0, 'shots': 0, 'fire': 0, 'bursts': [], 'pauses': [], 'cur': 0, 'gap': 0}
                st['samples'] += 1; st['shots'] += max(0, n)
                if fired:
                    st['fire'] += 1
                    if st['gap'] and st['cur'] == 0 and st['bursts']: st['pauses'].append(st['gap'] * 0.1)
                    st['cur'] += 1; st['gap'] = 0
                else:
                    if st['cur']: st['bursts'].append(st['cur'] * 0.1); st['cur'] = 0
                    st['gap'] += 1
        for k, st in run.items(): close(acc, st, kd.get(k), q75)
    return acc

def close(acc, st, kd, q75):
    if st['cur']: st['bursts'].append(st['cur'] * 0.1)
    groups = ['all'] + (['top'] if kd is not None and kd >= q75 else [])
    for g in groups:
        a = acc.setdefault((g, st['band']), {'samples': 0, 'shots': 0, 'fire': 0, 'bursts': [], 'pauses': []})
        a['samples'] += st['samples']; a['shots'] += st['shots']; a['fire'] += st['fire']; a['bursts'] += st['bursts']; a['pauses'] += st['pauses']

def med(v):
    v = sorted(v); return round(v[len(v) // 2], 2) if v else None

def summary(acc, label):
    out = {}
    for (g, b), a in sorted(acc.items()):
        if a['samples'] < 20: continue
        out.setdefault(g, {})['%d-%d' % BANDS[b]] = {'aiming_s': round(a['samples'] * 0.1, 1), 'shots_per_s': round(a['shots'] / (a['samples'] * 0.1), 2),
            'firing_share': round(a['fire'] / a['samples'], 3), 'burst_s': med(a['bursts']), 'pause_s': med(a['pauses']), 'bursts': len(a['bursts'])}
    return out

def main():
    pub = [p for p in glob.glob(os.path.join(TR, '*.jsonl.gz')) if not p.endswith('_local.jsonl.gz')]
    res = {'players': summary(study(pub), 'players')}
    if '--walkers' in sys.argv: res['walkers'] = summary(study(glob.glob(os.path.join(SUP, 'battles', 'tracks', '*.jsonl.gz'))), 'walkers')
    json.dump(res, open(os.path.join(TR, 'fire-rhythm.json'), 'w'), indent=1)
    if '--quiet' not in sys.argv:
        for who, G in res.items():
            for g, B in G.items():
                print('== %s (%s): automatic weapons while aiming at an enemy' % (who, g))
                for band, v in B.items(): print('   %-6s units: %6.1f s aiming, %5.2f shots/s, firing %3.0f%% of it, burst %s s, pause %s s (%d bursts)' % (
                    band, v['aiming_s'], v['shots_per_s'], 100 * v['firing_share'], v['burst_s'], v['pause_s'], v['bursts']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
