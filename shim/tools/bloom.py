#!/usr/bin/env python3
"""Bullet spread ("bloom") from nhook's bullet log (2026-11-08; the user: "what can you tell me about the 'random bloom' on both
the AR, and the BR? ... do they seem random? any disadvantages or flaws under certain situations?"). Reads NHOOK_SHOTLOG files
(noverlay.c: every bullet with its owner's aim at that moment) and, per weapon, measures each bullet's angle off the shooter's aim
-- in the aim's own frame (right / up), so a bias shows -- and how that error depends on:
  sustained fire      the bullet's index in a string of fire (shots under 0.2 s apart) -- the AR's bloom;
  burst position      1st / 2nd / 3rd round of a BR burst (rounds under 0.12 s apart), and the time since the previous burst;
  movement            the shooter's ground speed (standing / walking / running) and airborne (vertical speed);
  distance            how far the bullet flew (an angular spread is the same at any distance; magnetism / correction is not);
  hits                bullets that ended in a target's sphere vs misses (the server's own estimate);
and whether it is random: the radial shape (a uniform disc gives err^2 uniform up to the cone's edge; a Gaussian gives err^2
exponential), the correlation between consecutive bullets' errors, a pattern per burst position (a fixed pattern = the same mean
offset every time). Writes tracks/bloom.json.
    python3 shim/tools/bloom.py <shotlog> [<shotlog> ...] [--weapon NAME] [--json]"""
import json, math, os, sys
from collections import defaultdict

def load(paths):
    # (2026-11-08: the app deletes most projectiles TWICE, ~1 ms apart -- the same start, end and length, the aim sampled again --
    #  and the object_delete hook logs both: every bullet appeared twice, which made consecutive bullets look perfectly
    #  correlated. The first report of a (shooter, start, end) within 0.1 s is kept.)
    S = []
    for p in paths:
        seen = {}
        for l in open(p, errors='replace'):
            t = l.split()
            if len(t) != 18: continue
            try:
                r = {'t': float(t[0]), 'own': t[1], 'w': t[2].replace('_', ' '), 'p': t[3].replace('_', ' '), 'vic': t[4], 's': [float(x) for x in t[5:8]], 'e': [float(x) for x in t[8:11]],
                     'trav': float(t[11]), 'a': [float(x) for x in t[12:15]], 'v': [float(x) for x in t[15:18]], 'file': os.path.basename(p)}
            except ValueError: continue
            k = (t[1], tuple(t[5:11])); t0 = seen.get(k)
            seen[k] = r['t']
            if t0 is not None and abs(r['t'] - t0) < 0.1: continue
            S.append(r)
    return S

def unit(v):
    n = math.sqrt(sum(x * x for x in v)); return [x / n for x in v] if n > 1e-6 else None
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def dot(a, b): return sum(x * y for x, y in zip(a, b))

def prep(S):
    out = []
    for r in S:
        d = [r['e'][i] - r['s'][i] for i in range(3)]; L = math.sqrt(dot(d, d))
        if L < 1.0 or r['trav'] > L * 1.05 + 0.3: continue                    # (straight flights only: no bounces, no grenades)
        dn = [x / L for x in d]; a = unit(r['a'])
        if not a or abs(a[2]) > 0.95: continue
        rt = unit(cross(a, [0, 0, 1])); up = cross(rt, a)
        f = dot(dn, a); ex = math.degrees(math.atan2(dot(dn, rt), f)); ey = math.degrees(math.atan2(dot(dn, up), f))
        r.update(L=L, ex=ex, ey=ey, err=math.degrees(math.acos(max(-1.0, min(1.0, f)))), spd=math.hypot(r['v'][0], r['v'][1]) * 30, vz=r['v'][2] * 30)   # (velocity: units a tick -> a second)
        out.append(r)
    # strings of fire and bursts, per shooter and weapon
    by = defaultdict(list)
    for r in out: by[(r['file'], r['own'], r['w'])].append(r)
    for seq in by.values():
        seq.sort(key=lambda r: r['t']); si = bi = 0; prev = None; last_burst = None
        for r in seq:
            gap = r['t'] - prev['t'] if prev else 9.0
            si = si + 1 if gap < 0.2 else 1; r['idx'] = si
            if gap < 0.12: bi += 1
            else: r['since_burst'] = gap; bi = 1
            r['bpos'] = bi; prev = r
            if bi > 1: r['since_burst'] = prev.get('since_burst', 9.0)
    return out

def q(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(p * (len(v) - 1)))] if v else None
def corr(x, y):
    n = len(x)
    if n < 10: return None
    mx, my = sum(x) / n, sum(y) / n; sx = math.sqrt(sum((a - mx) ** 2 for a in x)); sy = math.sqrt(sum((b - my) ** 2 for b in y))
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy) if sx > 0 and sy > 0 else None

def summary(R):
    e = [r['err'] for r in R]
    if len(e) < 20: return None
    mx = q(e, 0.995); ex = [r['ex'] for r in R]; ey = [r['ey'] for r in R]; n = len(R)
    # shape: KS distance of err^2 against a uniform disc (uniform in [0, mx^2]) and against a Gaussian (Rayleigh: err^2 exponential)
    e2 = sorted(min(x, mx) ** 2 for x in e); m2 = sum(e2) / n
    ks_disc = max(abs((i + 1) / n - x / (mx * mx)) for i, x in enumerate(e2)) if mx > 0 else None
    ks_gauss = max(abs((i + 1) / n - (1 - math.exp(-x / m2))) for i, x in enumerate(e2)) if m2 > 0 else None
    return {'n': n, 'median': round(q(e, .5), 3), 'mean': round(sum(e) / n, 3), 'p90': round(q(e, .9), 3), 'max': round(mx, 3),
            'bias_right': round(sum(ex) / n, 3), 'bias_up': round(sum(ey) / n, 3), 'bias_se': round(math.sqrt(sum(x * x for x in ex) / n) / math.sqrt(n), 3),
            'ks_uniform_disc': ks_disc and round(ks_disc, 3), 'ks_gaussian': ks_gauss and round(ks_gauss, 3)}

def groups(R, key, bins):
    out = []
    for name, lo, hi in bins:
        g = [r for r in R if key(r) is not None and lo <= key(r) < hi]
        if len(g) >= 15: out.append({'group': name, 'n': len(g), 'median': round(q([r['err'] for r in g], .5), 3), 'p90': round(q([r['err'] for r in g], .9), 3),
                                     'bias_right': round(sum(r['ex'] for r in g) / len(g), 3), 'bias_up': round(sum(r['ey'] for r in g) / len(g), 3)})
    return out

def main():
    paths = [a for a in sys.argv[1:] if not a.startswith('--')]; wf = None
    if '--weapon' in sys.argv: wf = sys.argv[sys.argv.index('--weapon') + 1].lower(); paths = [p for p in paths if p.lower() != wf]
    R = prep(load(paths))
    if not R: print('no usable bullets (NHOOK_SHOTLOG files)'); return 1
    W = defaultdict(list)
    for r in R: W[r['w']].append(r)
    out = {'files': sorted({r['file'] for r in R}), 'weapons': {}}
    for w, g in sorted(W.items(), key=lambda kv: -len(kv[1])):
        if wf and wf not in w.lower(): continue
        sm = summary(g)
        if not sm: continue
        res = {'all': sm,
               'by_string_index': groups(g, lambda r: r['idx'], [('1st', 1, 2), ('2-3', 2, 4), ('4-7', 4, 8), ('8-15', 8, 16), ('16+', 16, 999)]),
               'by_burst_position': groups(g, lambda r: r['bpos'], [('1', 1, 2), ('2', 2, 3), ('3', 3, 4), ('4+', 4, 99)]),
               'by_time_since_last_burst': groups([r for r in g if r['bpos'] == 1], lambda r: r.get('since_burst'), [('<0.3 s', 0, 0.3), ('0.3-0.5 s', 0.3, 0.5), ('0.5-1 s', 0.5, 1), ('1 s+', 1, 99)]),
               'by_speed': groups(g, lambda r: r['spd'], [('standing <0.3', 0, 0.3), ('walking 0.3-1.5', 0.3, 1.5), ('running 1.5+', 1.5, 99)]),
               'by_airborne': groups(g, lambda r: abs(r['vz']), [('on the ground |vz|<0.5', 0, 0.5), ('airborne |vz|>1', 1, 99)]),
               'by_distance': groups(g, lambda r: r['L'], [('<10 u', 0, 10), ('10-25 u', 10, 25), ('25-50 u', 25, 50), ('50+ u', 50, 9999)]),
               'by_hit': groups(g, lambda r: 0 if r['vic'] == 'ffffffff' else 1, [('miss', 0, 1), ('hit', 1, 2)])}
        # randomness: consecutive bullets of one string, and each burst position's own mean offset (a fixed pattern)
        by = defaultdict(list)
        for r in g: by[(r['file'], r['own'])].append(r)
        x1, x2, y1, y2 = [], [], [], []
        for seq in by.values():
            seq.sort(key=lambda r: r['t'])
            for a, b in zip(seq, seq[1:]):
                if b['idx'] == a['idx'] + 1: x1.append(a['ex']); x2.append(b['ex']); y1.append(a['ey']); y2.append(b['ey'])
        res['consecutive_corr_right'] = corr(x1, x2) and round(corr(x1, x2), 3); res['consecutive_corr_up'] = corr(y1, y2) and round(corr(y1, y2), 3); res['consecutive_pairs'] = len(x1)
        out['weapons'][w] = res
        a = sm
        print('\n== %s: %d bullets' % (w, a['n']))
        print('   off the aim: median %.2f deg, mean %.2f, 90%% within %.2f, max %.2f; bias right %+.3f up %+.3f (+-%.3f)' % (a['median'], a['mean'], a['p90'], a['max'], a['bias_right'], a['bias_up'], a['bias_se']))
        print('   shape: KS vs a uniform disc %.3f, vs a Gaussian %.3f (smaller = closer)' % (a['ks_uniform_disc'] or -1, a['ks_gaussian'] or -1))
        print('   consecutive bullets: error correlation right %s, up %s (%d pairs; ~0 = independent)' % (res['consecutive_corr_right'], res['consecutive_corr_up'], len(x1)))
        for k in ('by_string_index', 'by_burst_position', 'by_time_since_last_burst', 'by_speed', 'by_airborne', 'by_distance', 'by_hit'):
            if res[k]: print('   %-26s %s' % (k.replace('_', ' '), '  '.join('%s: %.2f/%.2f (%d)' % (x['group'], x['median'], x['p90'], x['n']) for x in res[k])))
    tr = os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks'); json.dump(out, open(os.path.join(tr, 'bloom.json'), 'w'), indent=1)
    return 0

if __name__ == '__main__':
    sys.exit(main())
