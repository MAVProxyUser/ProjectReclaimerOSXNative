#!/usr/bin/env python3
"""Memory over a session, per game client (2026-11-08; the user: "I also noticed over time the choppiness came. I wonder what's
causing that memory leak"). Every minute, for the user's game (stats 8765) and the walkers (8766 / 8768 / 8769 / 8770): the process's
memory footprint (top's MEM column: the task's phys_footprint -- no memory scanning), the worst frame of the last second and the fps
from its stats page, its player's kills / deaths and the players in the match -> battles/mem-watch.csv. --report prints, per client,
the footprint's growth per hour (a least-squares line) and whether it goes with time, with deaths or with the worst frame.
    python3 shim/tools/mem-watch.py [--minutes 240]        python3 shim/tools/mem-watch.py --report"""
import csv, json, os, subprocess, sys, time, urllib.request

BT = os.path.expanduser('~/Library/Application Support/ReclaimerMac/battles'); OUT = os.path.join(BT, 'mem-watch.csv')
PORTS = {8765: 'you', 8766: 'Second', 8768: 'Third', 8769: 'Fourth', 8770: 'Fifth'}

def pid_on(port):
    r = subprocess.run(['lsof', '-nP', '-iTCP:%d' % port, '-sTCP:LISTEN', '-t'], capture_output=True, text=True)
    return int(r.stdout.split()[0]) if r.stdout.split() else None

def footprint_mb(pid):
    r = subprocess.run(['top', '-l', '1', '-pid', str(pid), '-stats', 'pid,mem'], capture_output=True, text=True)
    for l in reversed(r.stdout.splitlines()):
        t = l.split()
        if len(t) == 2 and t[0] == str(pid):
            v = t[1].rstrip('+-'); mul = {'K': 1 / 1024, 'M': 1, 'G': 1024}.get(v[-1], 1 / 1048576)
            try: return round(float(v[:-1] if v[-1] in 'KMG' else v) * mul, 1)
            except ValueError: return None
    return None

def stats(port):
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3))
    except Exception: return None
    me = [p for p in d.get('players', []) if p.get('you')]; pf = d.get('perf') or {}; fm = pf.get('frame_ms') or [None, None, None]
    return {'kills': me[0].get('kills') if me else None, 'deaths': me[0].get('deaths') if me else None, 'players': len(d.get('players', [])),
            'fps': pf.get('fps'), 'frame_max': fm[2] if len(fm) > 2 else None, 'in_match': d.get('in_match')}

def watch(minutes):
    new = not os.path.exists(OUT)
    with open(OUT, 'a', newline='') as f:
        w = csv.writer(f)
        if new: w.writerow(['t', 'who', 'pid', 'footprint_mb', 'frame_max_ms', 'fps', 'kills', 'deaths', 'players', 'in_match'])
        t_end = time.time() + 60 * minutes
        while time.time() < t_end:
            for port, who in PORTS.items():
                pid = pid_on(port)
                if not pid: continue
                s = stats(port) or {}
                w.writerow([round(time.time()), who, pid, footprint_mb(pid), s.get('frame_max'), s.get('fps'), s.get('kills'), s.get('deaths'), s.get('players'), s.get('in_match')])
            f.flush(); time.sleep(60)

def report():
    rows = list(csv.DictReader(open(OUT)))
    by = {}
    for r in rows:
        if r['footprint_mb'] in ('', 'None'): continue
        by.setdefault((r['who'], r['pid']), []).append(r)
    for (who, pid), R in sorted(by.items(), key=lambda kv: kv[1][0]['t']):
        if len(R) < 5: continue
        t = [(int(r['t']) - int(R[0]['t'])) / 3600 for r in R]; m = [float(r['footprint_mb']) for r in R]
        n = len(t); mt, mm = sum(t) / n, sum(m) / n; vt = sum((x - mt) ** 2 for x in t)
        slope = sum((x - mt) * (y - mm) for x, y in zip(t, m)) / vt if vt > 0 else 0
        fr = [float(r['frame_max_ms']) for r in R if r['frame_max_ms'] not in ('', 'None')]
        d0 = next((int(r['deaths']) for r in R if r['deaths'] not in ('', 'None')), None); d1 = next((int(r['deaths']) for r in reversed(R) if r['deaths'] not in ('', 'None')), None)
        print('%-7s pid %-6s %5.1f h: footprint %6.0f -> %6.0f MB (%+5.0f MB/h); worst frame first / last 10 min %s / %s ms; deaths %s -> %s' % (
            who, pid, t[-1], m[0], m[-1], slope, round(sum(fr[:10]) / max(1, len(fr[:10])), 1) if fr else '-', round(sum(fr[-10:]) / max(1, len(fr[-10:])), 1) if fr else '-', d0, d1))
    return 0

if __name__ == '__main__':
    if '--report' in sys.argv: sys.exit(report())
    sys.exit(watch(float(sys.argv[sys.argv.index('--minutes') + 1]) if '--minutes' in sys.argv else 240))
