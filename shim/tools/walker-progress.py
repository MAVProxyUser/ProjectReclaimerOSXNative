#!/usr/bin/env python3
"""The public walker's progress, match by match (2026-11-09; the user: "how has the walker been improving the last 8 hours?").
Reads the match recordings nhook makes of every public game (tracks/*.jsonl.gz, ntracks.c) whose server tag is <server> and where
"you" played: per match the walker's final kills / deaths / shots / hits (the scoreboard counters in each sample, columns 12-15)
and where it stands among the lobby's players with at least one death or kill (K/D rank, kills rank, accuracy rank among players
with 50+ shots). A match recorded in pieces (the post-game tail is a separate file) counts once: its longest piece.
    python3 shim/tools/walker-progress.py [--since HH:MM | --hours N] [--server <8-hex tag>] [--json]
No names: players are slots."""
import glob, gzip, json, os, sys, time

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
args = sys.argv[1:]
def opt(k, d=None):
    return args[args.index(k) + 1] if k in args and args.index(k) + 1 < len(args) else d
hours = float(opt('--hours', '8')); since = opt('--since'); server = opt('--server')
t0 = time.time() - hours * 3600
if since:
    lt = time.localtime(); h, m = map(int, since.split(':'))
    t0 = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, h, m, 0, 0, 0, -1))

def last_sample(path):
    head, last, n = None, None, 0
    try:
        with gzip.open(path, 'rt', errors='replace') as f:
            for l in f:
                if head is None:
                    head = json.loads(l); continue
                if l.startswith('{"t"'):
                    last = l; n += 1
    except (EOFError, OSError, json.JSONDecodeError):
        pass
    try: last = json.loads(last) if last else None
    except json.JSONDecodeError: last = None
    return head, last, n

rows = []
for p in sorted(glob.glob(os.path.join(SUP, 'tracks', '*.jsonl.gz'))):
    if os.path.getmtime(p) < t0: continue
    head, s, n = last_sample(p)
    if not head or not s or not head.get('you_plays') or head.get('start', 0) < t0 - 3600: continue
    if server and head.get('server') != server: continue
    if s.get('t', 0) < 180: continue
    you = s.get('you'); ps = s.get('p') or []
    me = next((r for r in ps if r[0] == you), None)
    if not me: continue
    def kd(r): return (r[12] + 1) / (r[13] + 1)
    act = [r for r in ps if r[12] + r[13] > 0]
    shooters = [r for r in ps if r[14] >= 50]
    rank = lambda lst, key: 1 + sum(1 for r in lst if key(r) > key(me))
    rows.append({'file': os.path.basename(p), 'start': head['start'], 'map': head.get('map'), 'server': head.get('server'),
                 'mins': round(s['t'] / 60, 1), 'players': len(act), 'k': me[12], 'd': me[13], 'shots': me[14], 'hits': me[15],
                 'acc': me[15] / me[14] if me[14] else None,
                 'kd_rank': rank(act, kd), 'k_rank': rank(act, lambda r: r[12]),
                 'acc_rank': rank(shooters, lambda r: r[15] / r[14]) if me[14] >= 50 else None, 'shooters': len(shooters),
                 'lobby_acc': sorted(r[15] / r[14] for r in shooters)[len(shooters) // 2] if shooters else None,
                 'lobby_kd': sorted(kd(r) for r in act)[len(act) // 2] if act else None})

if '--json' in args:
    print(json.dumps(rows, indent=1)); sys.exit(0)
print('%-5s %-11s %4s %3s  %4s %4s %5s  %5s %5s  %-9s %-9s %-9s' % ('start', 'map', 'min', 'pl', 'K', 'D', 'K/D', 'acc', 'lobby', 'K/D rank', 'kills rk', 'acc rank'))
K = D = S = H = 0
for r in rows:
    K += r['k']; D += r['d']; S += r['shots']; H += r['hits']
    print('%-5s %-11s %4.0f %3d  %4d %4d %5.2f  %5s %5s  %-9s %-9s %-9s' % (
        time.strftime('%H:%M', time.localtime(r['start'])), (r['map'] or '?')[:11], r['mins'], r['players'], r['k'], r['d'],
        r['k'] / max(1, r['d']), '%.2f' % r['acc'] if r['acc'] is not None else '-',
        '%.2f' % r['lobby_acc'] if r['lobby_acc'] is not None else '-',
        '%d/%d' % (r['kd_rank'], r['players']), '%d/%d' % (r['k_rank'], r['players']),
        '%d/%d' % (r['acc_rank'], r['shooters']) if r['acc_rank'] else '-'))
if rows:
    print('all %d matches: K %d D %d (%.2f), accuracy %.3f' % (len(rows), K, D, K / max(1, D), H / max(1, S)))
