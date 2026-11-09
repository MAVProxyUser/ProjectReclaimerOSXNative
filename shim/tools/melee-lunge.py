#!/usr/bin/env python3
"""How players melee (2026-11-09): from the match recordings (match-recorder.py), every melee kill -- the distance between killer and
victim when the killer's lunge began (its fastest 0.1 s step in the last 0.6 s before the kill), at the kill, and both shields then.
Set the walker's melee knobs from it (nbot.c NHOOK_BOT_MELEE_LUNGE / _LUNGE_SH / _RACE).
    python3 shim/tools/melee-lunge.py [DIR ...]   (default: ReclaimerMac/tracks and battles/follow/tracks; the newest 120 files)"""
import bisect, glob, gzip, json, math, os, sys, zlib
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
dirs = sys.argv[1:] or [os.path.join(SUP, 'tracks'), os.path.join(SUP, 'battles', 'follow', 'tracks')]
files = sorted(f for d in dirs for f in glob.glob(os.path.join(d, '*.jsonl.gz')))[-120:]
start_d, land_d, vsh, ksh = [], [], [], []
for p in files:
    S, E = [], []
    try:
        for l in gzip.open(p, 'rt'):
            try: d = json.loads(l)
            except Exception: continue
            if 'p' in d: S.append((d['t'], {r[0]: r for r in d['p']}))
            elif d.get('ev') == 'kill' and (d.get('how') or '').startswith('Melee'): E.append(d)
    except (OSError, EOFError, zlib.error): pass
    if not S or not E: continue
    T = [s[0] for s in S]
    for e in E:
        a, b, t = e.get('a'), e.get('b'), e.get('t')
        if a is None or b is None or t is None: continue
        i = bisect.bisect_left(T, t)
        if i < 12 or i >= len(S): continue
        pts = []
        for tt, P in S[i - 12:i]:
            ra, rb = P.get(a), P.get(b)
            if not ra or not rb or not ra[2] or not rb[2]: pts = []; continue
            pts.append((tt, math.hypot(ra[3] - rb[3], ra[4] - rb[4]), ra, rb))
        if len(pts) < 6: continue
        sp = [(math.hypot(pts[k][2][3] - pts[k - 1][2][3], pts[k][2][4] - pts[k - 1][2][4]) / max(0.05, pts[k][0] - pts[k - 1][0]), k) for k in range(1, len(pts))]
        last = [x for x in sp if pts[x[1]][0] > pts[-1][0] - 0.6]
        if not last: continue
        v, k = max(last); st = pts[k - 1]
        start_d.append(st[1]); land_d.append(pts[-1][1]); vsh.append(st[3][8] or 0); ksh.append(st[2][8] or 0)
q = lambda a, p: sorted(a)[int(p * (len(a) - 1))] if a else float('nan')
print('melee kills measured: %d (%d recordings)' % (len(start_d), len(files)))
print('distance when the lunge began: p25 %.2f  median %.2f  p75 %.2f  p90 %.2f' % (q(start_d, .25), q(start_d, .5), q(start_d, .75), q(start_d, .9)))
print('distance at the kill: median %.2f' % q(land_d, .5))
print('victim shield then: median %.2f, under 0.15: %.0f%%' % (q(vsh, .5), 100 * sum(1 for x in vsh if x < 0.15) / max(1, len(vsh))))
print('killer shield then: median %.2f, under 0.15: %.0f%%' % (q(ksh, .5), 100 * sum(1 for x in ksh if x < 0.15) / max(1, len(ksh))))
