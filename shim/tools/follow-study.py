#!/usr/bin/env python3
"""What a shadowing walker learns from the player it follows, every few minutes (2026-11-09; the user, in a public game with the walker
AIWalkerTest: "have it try to shadow me and be my near by wingman. and update every 3 minutes what it learns. it should always find me ...
and try to shadow while I teach it"). Reads the walker's own recording (match-recorder.py polling the WALKER's stats page; slots only, no
names) and, every --every seconds, over that window:
  shadow   the distance between the walker and its leader while both live: median, the share within 6 units, the longest stretch apart
  leader   kills / deaths, accuracy (the server's shots / hits counters), the distance and weapon of each of its kills
  walker   the same for the walker
  lessons  where the two differ: kill distance, accuracy, weapons that killed, deaths per kill
  grenades the walker's throws in the window from its log ("[nade] ...: hit / missed / KILLED / target gone"), and the HONING: the lead
           per unit of distance (NHOOK_BOT_NADE_TPU) is tried among 0.06..0.14 s / unit, each window's throws credited to the value
           that ran, the next value the best hit rate so far (a never-tried one first; 1 in 4 windows a random one) -> written to the
           walker's LIVE knobs file (battles/live-knobs/<its name>.json: it re-reads it within a second, no restart)
-> stdout and battles/follow/learned.jsonl (the windows, for the walker's next settings). The leader is found by name on the walker's
stats page (--leader-from-profile: the player profile's nickname, never printed; else --leader NAME); the walker is its own "you".
    python3 shim/tools/follow-study.py --port 8771 --dir <recordings dir> [--every 180] [--leader-from-profile]"""
import glob, gzip, json, math, os, random, statistics, sys, time, urllib.request

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); OUT = os.path.join(SUP, 'battles', 'follow')

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d

def read(p):
    head = {}; S = []; E = []
    try:
        with gzip.open(p, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o
                elif 'p' in o: S.append(o)
                elif o.get('ev') == 'kill': E.append(o)
    except (EOFError, OSError): pass
    return head, S, E

TPU = [0.06, 0.08, 0.10, 0.12, 0.14]
def nade_lines(logp, state):
    """the walker's [nade] results written since the last call"""
    try:
        with open(logp, 'rb') as f:
            f.seek(state.get('off', 0)); b = f.read(); state['off'] = f.tell()
    except OSError: return []
    return [l for l in b.decode('utf-8', 'replace').splitlines() if '[nade] ' in l]

def hone(results, nstate, knobs_path):
    cur = nstate.get('cur', 0.10); st = nstate.setdefault('stats', {})
    t = st.setdefault('%.2f' % cur, [0, 0]); t[0] += len(results); t[1] += sum(1 for l in results if ': hit' in l or 'KILLED' in l)
    untried = [v for v in TPU if '%.2f' % v not in st or st['%.2f' % v][0] == 0]
    if untried: nxt = untried[0]
    elif random.random() < 0.25: nxt = random.choice(TPU)
    else: nxt = max(TPU, key=lambda v: (st['%.2f' % v][1] + 1) / (st['%.2f' % v][0] + 2))
    nstate['cur'] = nxt
    try: k = json.load(open(knobs_path))
    except Exception: k = {}
    k['NHOOK_BOT_NADE_TPU'] = nxt
    os.makedirs(os.path.dirname(knobs_path), exist_ok=True); tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
    return cur, nxt, st

def slots(port, leader):
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3))
    except Exception: return None, None
    me = next((p.get('slot') for p in d.get('players', []) if p.get('you')), None)
    ld = next((p.get('slot') for p in d.get('players', []) if p.get('name') == leader), None)
    WALKER_NAME[0] = next((p.get('name') for p in d.get('players', []) if p.get('you')), WALKER_NAME[0])
    return me, ld
WALKER_NAME = ['AIWalkerTest']

def med(v): return round(statistics.median(v), 1) if v else None

def window(rec_dir, t0, t1, ws, ls):
    o = {'shadow': [], 'w': {'k': 0, 'd': 0, 'shots': 0, 'hits': 0, 'kd': [], 'kw': []}, 'l': {'k': 0, 'd': 0, 'shots': 0, 'hits': 0, 'kd': [], 'kw': []}, 'apart_s': 0.0}
    for path in sorted(glob.glob(os.path.join(rec_dir, '*.jsonl.gz')), key=os.path.getmtime)[-2:]:
        if os.path.getmtime(path) < t0: continue
        head, S, E = read(path); st = head.get('start') or 0
        W = [s for s in S if st and t0 <= st + s['t'] <= t1]
        if len(W) < 2: continue
        rows = [{r[0]: r for r in s['p']} for s in W]
        for who, sl in (('w', ws), ('l', ls)):
            a = next((r[sl] for r in rows if sl in r), None); b = next((r[sl] for r in reversed(rows) if sl in r), None)
            if a and b:
                for k, i in (('k', 12), ('d', 13), ('shots', 14), ('hits', 15)): o[who][k] += max(0, (b[i] or 0) - (a[i] or 0))
        run = 0.0
        for r in rows:
            a, b = r.get(ws), r.get(ls)
            if a and b and a[2] and b[2]:
                d = math.hypot(a[3] - b[3], a[4] - b[4]); o['shadow'].append(d)
                run = run + 0.1 if d > 6 else 0.0; o['apart_s'] = max(o['apart_s'], run)
        for e in E:
            te = st + e['t']
            if not (t0 <= te <= t1) or e.get('a') == e.get('b'): continue
            for who, sl in (('w', ws), ('l', ls)):
                if e.get('a') != sl: continue
                s0 = min(W, key=lambda s: abs(st + s['t'] - te)); r = {x[0]: x for x in s0['p']}; k = r.get(sl); v = r.get(e.get('b'))
                if k and v: o[who]['kd'].append(math.hypot(k[3] - v[3], k[4] - v[4]))
                o[who]['kw'].append((e.get('how') or (k[10] if k else '') or '?'))
    return o

def report(o, n):
    acc = lambda x: '%.0f%%' % (100 * x['hits'] / x['shots']) if x['shots'] else '-'
    sh = o['shadow']; w, l = o['w'], o['l']
    lines = ['%s window %d (the last %d s):' % (time.strftime('%H:%M:%S'), n, EVERY)]
    lines.append('  shadowing you: median %s units apart, within 6 units %s of the time, longest apart %.0f s' % (
        med(sh), '%.0f%%' % (100 * sum(d <= 6 for d in sh) / len(sh)) if sh else '-', o['apart_s']))
    lines.append('  you:    %d kills / %d deaths, accuracy %s, kill distance median %s, kills with %s' % (l['k'], l['d'], acc(l), med(l['kd']), ', '.join(sorted(set(l['kw']))) or '-'))
    lines.append('  walker: %d kills / %d deaths, accuracy %s, kill distance median %s, kills with %s' % (w['k'], w['d'], acc(w), med(w['kd']), ', '.join(sorted(set(w['kw']))) or '-'))
    les = []
    if l['kd'] and w['kd'] and abs(med(l['kd']) - med(w['kd'])) > 3: les.append('you kill at %s units, it at %s: %s' % (med(l['kd']), med(w['kd']), 'close in more' if med(l['kd']) < med(w['kd']) else 'fight from further out'))
    elif l['kd'] and not w['kd']: les.append('you kill at %s units; it has no kills to compare yet' % med(l['kd']))
    if l['shots'] and w['shots']:
        la, wa = l['hits'] / l['shots'], w['hits'] / w['shots']
        if la - wa > 0.08: les.append('you land %.0f%% of shots, it %.0f%%: its aim / bursts are the gap' % (100 * la, 100 * wa))
    if w['d'] > 2 * max(1, w['k']) and w['d'] > l['d']: les.append('it dies %d times to your %d: it is taking fights it loses' % (w['d'], l['d']))
    if sh and statistics.median(sh) > 8: les.append('it is losing you: median %.0f units apart' % statistics.median(sh))
    lines.append('  lessons: ' + ('; '.join(les) if les else 'nothing stands out yet'))
    return '\n'.join(lines)

EVERY = 180
def main():
    global EVERY
    port = int(arg('--port', '8771')); rec = arg('--dir', ''); EVERY = int(arg('--every', '180'))
    leader = arg('--leader', '')
    if '--leader-from-profile' in sys.argv:
        leader = json.load(open(os.path.expanduser('~/Library/Application Support/ProjectReclaimer/profile.json')))['identity']['nickname']
    os.makedirs(OUT, exist_ok=True); n = 0
    logp = arg('--log', os.path.join(SUP, 'battles', 'aiwalker.log')); nst = {}; nade_lines(logp, nst)   # (from now on)
    print('%s following-study every %d s (the walker on %d, its leader by name)' % (time.strftime('%H:%M:%S'), EVERY, port), flush=True)
    while True:
        t0 = time.time(); time.sleep(EVERY); t1 = time.time(); n += 1
        ws, ls = slots(port, leader)
        if ws is None or ls is None: print('%s window %d: the walker or its leader is not in the game' % (time.strftime('%H:%M:%S'), n), flush=True); continue
        o = window(rec, t0, t1, ws, ls)
        print(report(o, n), flush=True)
        res = nade_lines(logp, nst); kp = os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json')
        cur, nxt, st = hone(res, nst, kp)
        print('  grenades: %d thrown, %d hit (%d killed), lead %.2f s a unit -> next %.2f; so far %s' % (len(res), sum(1 for l in res if ': hit' in l or 'KILLED' in l),
              sum(1 for l in res if 'KILLED' in l), cur, nxt, ', '.join('%s: %d/%d' % (k, v[1], v[0]) for k, v in sorted(st.items()))), flush=True)
        open(os.path.join(OUT, 'learned.jsonl'), 'a').write(json.dumps({'t': round(t1), 'window': n, 'shadow_median': med(o['shadow']), 'walker': {k: v for k, v in o['w'].items()},
                                                                       'leader': {k: v for k, v in o['l'].items()}}) + '\n')

if __name__ == '__main__':
    sys.exit(main())
