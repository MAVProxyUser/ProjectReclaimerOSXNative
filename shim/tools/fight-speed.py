#!/usr/bin/env python3
"""How fast players move in a fight (2026-11-09; the user: the best player "moves more nimble ... with authority", the walker's
[moves] lines show 0.7-2.1 u/s in its fights). From recorded matches (tracks/*.jsonl.gz, the policy-train.py format): every 0.1 s
sample of a live player on foot with an enemy within 15 units -> its speed across the ground over the last 0.5 s (units / s) and how much of it is
ACROSS the enemy line (strafe) vs along it. Players are grouped: the top fifth of each match by K/D (k + d >= 5), the others, and the
recorder's own slot (the player who recorded: the user, or the walker in its own sessions). Prints medians / quartiles and the share of samples
under 0.5 u/s (standing). No player names are printed.
    python3 shim/tools/fight-speed.py [--files N] [--near 15] [--json] [--dir <recordings>] [--all-groups]
--dir battles/tracks = the walkers' own local battles (every slot a walker: use the "others" + "top" lines; --all-groups puts every
slot in one group, "all"). --spells: per slot of the newest recording, the share of time standing and its longest still spells
(seconds, x, y, z) -- a walker hovering at one spot shows here even when its own log says it walks."""
import glob, gzip, json, math, os, statistics, sys, zlib

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def arg(k, d):
    return type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d
plays = [True]
def load(path):
    S, you = [], None; plays[0] = True
    try:
        with gzip.open(path, 'rt') as f:
            for line in f:
                try: d = json.loads(line)
                except Exception: continue
                if 'p' in d: S.append((d['t'], {r[0]: r for r in d['p']})); you = d.get('you', you)
                elif 'start' in d and not d.get('you_plays'): plays[0] = False
    except (OSError, EOFError, zlib.error): pass
    return S, you
def spells(path):
    S, you = load(path); st = {}; runs = {}; cur = {}; j = 0
    for i in range(1, len(S)):
        t0, P0 = S[i]
        while j < i - 1 and t0 - S[j + 1][0] >= 0.5: j += 1
        tp, Pp = S[j]
        if not 0.45 < t0 - tp < 0.8: continue
        for sl, r in P0.items():
            q = Pp.get(sl)
            if len(r) < 12 or not r[2] or not q or not q[2] or r[11]: cur.pop(sl, None); continue
            sp = math.hypot(r[3] - q[3], r[4] - q[4]) / (t0 - tp); a = st.setdefault(sl, [0, 0]); a[0] += 1; a[1] += sp < 0.5
            if sp < 0.5: cur.setdefault(sl, (t0, r[3], r[4], r[5]))
            elif sl in cur: c = cur.pop(sl); runs.setdefault(sl, []).append((round(t0 - c[0], 1), round(c[1], 1), round(c[2], 1), round(c[3], 1)))
    print(os.path.basename(path))
    for sl, (n, k) in sorted(st.items()):
        print('  slot %s: standing %.0f%% of %d samples; longest still spells (s, x, y, z): %s' % (sl, 100.0 * k / max(1, n), n, sorted(runs.get(sl, []), key=lambda x: -x[0])[:4]))
def main():
    near = arg('--near', 15.0); nf = arg('--files', 400)
    if '--spells' in sys.argv:
        for p in sorted(glob.glob(os.path.join(os.path.expanduser(arg('--dir', os.path.join(SUP, 'tracks'))), '*.jsonl.gz')))[-arg('--files', 1):]: spells(p)
        return 0
    files = sorted(glob.glob(os.path.join(os.path.expanduser(arg('--dir', os.path.join(SUP, 'tracks'))), '*.jsonl.gz')))[-nf:]
    G = {'top': [], 'others': [], 'recorder': [], 'all': []}; ST = {'top': [], 'others': [], 'recorder': [], 'all': []}
    for p in files:
        S, you = load(p)
        if len(S) < 50: continue
        kd = {}
        for _, P in S:
            for sl, r in P.items():
                if len(r) > 13: kd[sl] = (max(kd.get(sl, (0, 0))[0], r[12] or 0), max(kd.get(sl, (0, 0))[1], r[13] or 0))
        ranked = sorted([sl for sl, (k, d) in kd.items() if k + d >= 5 and sl != you], key=lambda s: -(kd[s][0] + 1) / (kd[s][1] + 1))
        top = set(ranked[:max(1, len(ranked) // 5)])
        j = 0
        for i in range(1, len(S)):
            t0, P0 = S[i]
            while j < i - 1 and t0 - S[j + 1][0] >= 0.5: j += 1   # (over 0.5 s: remote players' positions repeat between network updates)
            tp, Pp = S[j]; dt = t0 - tp
            if not 0.45 < dt < 0.8: continue
            for sl, r in P0.items():
                if len(r) < 12 or not r[2] or r[11]: continue
                q = Pp.get(sl)
                if not q or not q[2] or q[11]: continue
                x, y = r[3], r[4]; vx, vy = (x - q[3]) / dt, (y - q[4]) / dt; sp = math.hypot(vx, vy)
                if sp > 12: continue   # (a respawn / teleport)
                en = [(math.hypot(e[3] - x, e[4] - y), e) for s2, e in P0.items() if s2 != sl and len(e) > 9 and e[2] and e[1] != r[1]]
                en = [e for e in en if 0.3 < e[0] < near]
                if not en: continue
                dE, E = min(en, key=lambda e: e[0]); ux, uy = (E[3] - x) / dE, (E[4] - y) / dE
                across = abs(vx * -uy + vy * ux)
                g = 'all' if '--all-groups' in sys.argv else 'recorder' if sl == you else 'top' if sl in top else 'others'
                if g == 'recorder' and not plays[0]: continue   # (an observer: not playing)
                G[g].append(sp); ST[g].append(across)
    out = {}
    for g in G:
        v = G[g]
        if len(v) < 20: continue
        q = statistics.quantiles(v, n=4); a = ST[g]
        out[g] = {'samples': len(v), 'median': round(statistics.median(v), 2), 'p25': round(q[0], 2), 'p75': round(q[2], 2),
                  'standing': round(sum(1 for s in v if s < 0.5) / len(v), 3), 'across_median': round(statistics.median(a), 2),
                  'across_share': round(sum(a) / max(1e-6, sum(v)), 2)}
    if '--json' in sys.argv: print(json.dumps(out, indent=1)); return 0
    print('fight speed (an enemy within %.0f units; %d recordings)' % (near, len(files)))
    for g, o in out.items():
        print('  %-8s %7d samples: median %.2f u/s (quartiles %.2f-%.2f), standing (< 0.5 u/s) %4.1f%%, across the enemy line median %.2f u/s (%.0f%% of the movement)'
              % (g, o['samples'], o['median'], o['p25'], o['p75'], 100 * o['standing'], o['across_median'], 100 * o['across_share']))
    return 0
if __name__ == '__main__':
    sys.exit(main())
