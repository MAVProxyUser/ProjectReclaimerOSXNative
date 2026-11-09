#!/usr/bin/env python3
"""Stutter per run from the native app's own frame log (2026-11-08, hitch-ab.sh): the app prints, with or without nhook,
  [capture] frame N at F fps (frame ms p50 A p95 B max C)
every ~5 s (the worst frame of that window), plus "[online] session state ... players N". Per log: windows in a match (players
> 0), the median / 90th percentile of the windows' worst frame, the share of windows whose worst frame is over 50 / 100 ms, the
median fps and frame p95, players. The first 60 s of a match (loading) are left out.
    python3 shim/tools/hitch-summary.py <log> [<log> ...]
    python3 shim/tools/hitch-summary.py --curve <log> ...    the worst-frame median per 2 minutes in the match, per log"""
import os, re, sys

CAP = re.compile(r'^\[capture\] frame (\d+) at ([\d.]+) fps \(frame ms p50 ([\d.]+) p95 ([\d.]+) max ([\d.]+)\)')
PL = re.compile(r'^\[online\] session state \d+ mode \d+ peers \d+ players (\d+)')

def q(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(p * (len(v) - 1)))] if v else float('nan')

def one(path):
    W = []; pl = 0; first = None
    for l in open(path, errors='replace'):
        m = PL.match(l)
        if m: pl = int(m[1]); continue
        m = CAP.match(l)
        if m and pl > 0:
            if first is None: first = len(W)
            W.append((float(m[2]), float(m[4]), float(m[5]), pl))
    W = W[12:] if len(W) > 24 else W   # (~60 s of windows: the join / loading)
    if not W: return None
    mx = [w[2] for w in W]
    return {'windows': len(W), 'max_med': q(mx, .5), 'max_p90': q(mx, .9), 'over50': sum(x > 50 for x in mx) / len(mx), 'over100': sum(x > 100 for x in mx) / len(mx),
            'fps': q([w[0] for w in W], .5), 'p95': q([w[1] for w in W], .5), 'players': q([w[3] for w in W], .5)}

def curve(path):
    """the windows' worst frame, median per 2 minutes from the first in-match window (24 windows of ~5 s)"""
    W = []; pl = 0
    for l in open(path, errors='replace'):
        m = PL.match(l)
        if m: pl = int(m[1]); continue
        m = CAP.match(l)
        if m and pl > 0: W.append(float(m[5]))
    return [q(W[i:i + 24], .5) for i in range(0, len(W) - 23, 24)]

def main():
    if '--curve' in sys.argv:   # one row per log: the worst-frame median per 2 minutes in the match (the build-up over a session)
        for p in [a for a in sys.argv[1:] if not a.startswith('--')]:
            print('%-40s %s' % (os.path.basename(p)[:40], ' '.join('%4.0f' % v for v in curve(p))))
        return 0
    for p in sys.argv[1:]:
        r = one(p); arm = open(p, errors='replace').readline().strip() if os.path.exists(p) else ''
        arm = arm.replace('[hitch-ab] arm ', '') if arm.startswith('[hitch-ab]') else 'none (app alone)'
        if not r: print('%-48s no in-match frame lines' % os.path.basename(p)); continue
        print('%-48s %-28s %3d windows: worst frame median %5.1f ms, p90 %5.1f; over 50 ms %3.0f%%, over 100 ms %3.0f%%; %4.0f fps, p95 %4.1f ms; %2.0f players' % (
            os.path.basename(p), arm[:28], r['windows'], r['max_med'], r['max_p90'], 100 * r['over50'], 100 * r['over100'], r['fps'], r['p95'], r['players']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
