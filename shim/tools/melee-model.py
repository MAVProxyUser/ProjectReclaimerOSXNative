#!/usr/bin/env python3
"""How players melee (2026-11-08; the user, watching a walker hold a Gravity Hammer on an enemy without swinging: "clearly you
never trained on melees sooooo many opportunities for the walker to melee"). Melee is the second biggest killer in the public
recordings (3,865 kills to the battle rifle's 6,206 on 2026-11-08). From every recorded match (tracks/*.jsonl.gz: positions,
shields, weapons 10 times a second, kill events with the damage type), each kill whose type is "Melee" (the melee button; sword
and hammer kills are their own types, measured as well):
  distance        killer -> victim at the victim's last live sample (the kill event's own time lags the death: it comes from
                  the game's results, polled) (3D, world units)
  closing         how far the killer closed in over the last 1 s / 2 s before it (distance then minus distance at the kill)
  beatdown        the victim's shield at 0.5 s before the kill under 0.05 (shots took it, the melee finished) vs a full-shield melee
  held            the killer's weapon class when it meleed
and, as the walker's rule: melee range = the 75th percentile of the distance, the rush distance = the 75th percentile of the
distance 1 s before a beatdown (players close in from there once the shield is down). Infection is left out (zombies only melee).
Writes tracks/melee.json (nbot.c reads it: NHOOK_MELEE_FILE), prints the report.
    python3 shim/tools/melee-model.py [--quiet] [--dir <recordings folder>]   (offline test: shim/tools/melee-model-test.sh)"""
import glob, gzip, json, math, os, sys
from bisect import bisect_right

TR = sys.argv[sys.argv.index('--dir') + 1] if '--dir' in sys.argv else os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks'); QUIET = '--quiet' in sys.argv
INF = ('infection', 'zombie', 'flood')
TYPES = {'Melee': 'melee', 'Energy sword': 'sword', 'Gravity hammer': 'hammer'}
CLS = (('melee', ('ENERGY BLADE', 'ENERGY SWORD', 'GRAVITY HAMMER', 'HAMMER')), ('precision', ('BATTLE RIFLE', 'CARBINE', 'MAGNUM', 'DMR')),
       ('auto', ('ASSAULT RIFLE', 'SMG', 'SPIKER', 'SPIKE RIFLE', 'PLASMA RIFLE', 'NEEDLER', 'PLASMA PISTOL', 'SENTINEL BEAM')), ('sniper', ('SNIPER', 'BEAM RIFLE')),
       ('shotgun', ('SHOTGUN', 'MAULER')), ('rocket', ('ROCKET', 'SPARTAN LASER', 'FUEL ROD', 'BRUTE SHOT', 'MISSILE POD', 'FLAMETHROWER')))

def wcls(w):
    w = (w or '').upper()
    if not w: return 'none'
    for c, keys in CLS:
        if any(k in w for k in keys): return c
    return 'other'

def q(v, p):
    v = sorted(v); return round(v[min(len(v) - 1, int(p * (len(v) - 1)))], 2) if v else None

def read(path):
    head = {}; S = []; E = []
    try:
        with gzip.open(path, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o
                elif 'p' in o: S.append(o)
                elif o.get('ev') == 'kill': E.append(o)
    except (EOFError, OSError): pass
    return head, S, E

def at(S, T, t, slot):
    """the row of <slot> in the last sample at or before t"""
    i = bisect_right(T, t) - 1
    if i < 0: return None
    for r in S[i]['p']:
        if r[0] == slot: return r
    return None

def d3(a, b): return math.sqrt((a[3] - b[3]) ** 2 + (a[4] - b[4]) ** 2 + (a[5] - b[5]) ** 2)

def main():
    K = {k: [] for k in TYPES.values()}; files = 0
    for p in sorted(glob.glob(os.path.join(TR, '*.jsonl.gz'))):
        if p.endswith('_local.jsonl.gz'): continue
        head, S, E = read(p)
        mode = ' '.join(str(head.get(k) or '') for k in ('engine', 'variant', 'mode')).lower()
        if len(S) < 100 or any(k in mode for k in INF): continue
        files += 1; T = [s['t'] for s in S]
        for e in E:
            ty = TYPES.get(e.get('how'))
            if not ty or e.get('a') is None or e.get('b') is None or e['a'] == e['b'] or e.get('est'): continue
            # the moment of the kill = the victim's last live sample at or before the event: the event's time comes from the game's
            # results (polled), up to ~1-2 s after the death -- looked for up to 4 s back
            t = e['t']; i = bisect_right(T, t + 1e-6) - 1; a0 = b0 = None
            for j in range(i, max(-1, i - 41), -1):
                rb = [r for r in S[j]['p'] if r[0] == e['b']]
                if rb and rb[0][2] and (rb[0][3] or rb[0][4]):
                    ra = [r for r in S[j]['p'] if r[0] == e['a']]
                    if ra: a0, b0 = ra[0], rb[0]; t = T[j]
                    break
            if not a0 or not b0 or not a0[2]: continue
            d0 = d3(a0, b0)
            if d0 > 15: continue   # (a stale sample: the victim had respawned / moved by teleport)
            rec = {'d': d0, 'held': wcls(a0[10])}
            for back, key in ((1.0, 'd1'), (2.0, 'd2')):
                a1, b1 = at(S, T, t - back, e['a']), at(S, T, t - back, e['b'])
                if a1 and b1 and a1[2] and b1[2]: rec[key] = d3(a1, b1)
            b5 = at(S, T, t - 0.5, e['b'])
            if b5 and b5[2]: rec['shield_before'] = b5[8]
            K[ty].append(rec)
    out = {'files': files, 'types': {}}
    for ty, R in K.items():
        if not R: continue
        bd = [r for r in R if r.get('shield_before') is not None and r['shield_before'] < 0.05]
        fs = [r for r in R if r.get('shield_before') is not None and r['shield_before'] >= 0.5]
        held = {}
        for r in R: held[r['held']] = held.get(r['held'], 0) + 1
        out['types'][ty] = {'kills': len(R), 'distance': {'p25': q([r['d'] for r in R], .25), 'p50': q([r['d'] for r in R], .5), 'p75': q([r['d'] for r in R], .75), 'p90': q([r['d'] for r in R], .9)},
            'distance_1s_before': {'p25': q([r['d1'] for r in R if 'd1' in r], .25), 'p50': q([r['d1'] for r in R if 'd1' in r], .5), 'p75': q([r['d1'] for r in R if 'd1' in r], .75)},
            'closing_1s': q([r['d1'] - r['d'] for r in R if 'd1' in r], .5), 'closing_2s': q([r['d2'] - r['d'] for r in R if 'd2' in r], .5),
            'beatdown_share': round(len(bd) / max(1, len(bd) + len(fs)), 3), 'full_shield_share': round(len(fs) / max(1, len(bd) + len(fs)), 3),
            'beatdown_from_p75': q([r['d1'] for r in bd if 'd1' in r], .75), 'held': dict(sorted(held.items(), key=lambda kv: -kv[1]))}
    m = out['types'].get('melee')
    if m: out['walker'] = {'melee_range': m['distance']['p75'], 'rush_from': m['beatdown_from_p75'] or m['distance_1s_before']['p75'], 'beatdown_share': m['beatdown_share']}
    for w in ('sword', 'hammer'):
        if w in out['types']: out.setdefault('walker', {})[w + '_range'] = out['types'][w]['distance']['p75']
    json.dump(out, open(os.path.join(TR, 'melee.json'), 'w'), indent=1)
    if not QUIET:
        print('%d matches' % files)
        for ty, v in out['types'].items():
            print('== %s: %d kills; distance p25 %s / median %s / p75 %s / p90 %s units; 1 s before: median %s, p75 %s; closing %s units in the last 1 s, %s in 2 s' % (
                ty, v['kills'], v['distance']['p25'], v['distance']['p50'], v['distance']['p75'], v['distance']['p90'], v['distance_1s_before']['p50'], v['distance_1s_before']['p75'], v['closing_1s'], v['closing_2s']))
            print('   victim shield 0.5 s before: down (beatdown) %.0f%%, half or more %.0f%%; beatdowns closed in from %s units (p75); held: %s' % (
                100 * v['beatdown_share'], 100 * v['full_shield_share'], v['beatdown_from_p75'], ', '.join('%s %d' % kv for kv in list(v['held'].items())[:5])))
        print('walker: %s' % out.get('walker'))
    return 0

if __name__ == '__main__':
    sys.exit(main())
