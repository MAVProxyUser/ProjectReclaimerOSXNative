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
  refit    (--refit) the LIVE match joins the learning: engage-extract.py cuts this game's recordings (every current player's fights)
           into tracks/engagements, engage-model.py refits the models + tome at background priority (taskpolicy -b, nice 20; one at
           a time) -- the walker re-reads changed model files by itself (nbot.c bot_models_tick): no restart
-> stdout and battles/follow/learned.jsonl (the windows, for the walker's next settings). The leader is found by name on the walker's
stats page (--leader-from-profile: the player profile's nickname, never printed; else --leader NAME); the walker is its own "you".
    python3 shim/tools/follow-study.py --port 8771 --dir <recordings dir> [--every 180] [--leader-from-profile]"""
import collections, glob, gzip, json, math, os, random, statistics, subprocess, sys, time, urllib.request

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

ARC = [0.015, 0.025, 0.035]
AIMH = [0.55, 0.65, 0.75, 0.85]
def hone_aim(astate, knobs_path, watcher_port=8765, name='AIWalkerTest'):
    """the aim point's height (NHOOK_BOT_AIM_HEAD) by HEAD HITS a shot -- measured by the watching player's client (its stats page
    analyses every other player's shots: n aimed shots, 'on' = through a hitbox, 'head' = of those the topmost) -- the window's change
    in those cumulative counts credited to the height that ran; next: untried first, else the best (1 in 4 random)"""
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % watcher_port, timeout=3))
    except Exception: return None
    a = next((p.get('aim') for p in d.get('players', []) if p.get('name') == name), None)
    if not a or not a.get('n'): return None
    shots, heads = a['n'], a['n'] * (a.get('on') or 0) * (a.get('head') or 0)
    cur = astate.get('cur', 0.55); st = astate.setdefault('stats', {}); last = astate.get('last')
    if last and shots >= last[0]:
        t = st.setdefault('%.2f' % cur, [0, 0.0]); t[0] += shots - last[0]; t[1] += max(0.0, heads - last[1])
    astate['last'] = [shots, heads]
    untried = [v for v in AIMH if st.get('%.2f' % v, [0, 0])[0] < 20]
    nxt = untried[0] if untried else random.choice(AIMH) if random.random() < 0.25 else max(AIMH, key=lambda v: (st['%.2f' % v][1] + 0.5) / (st['%.2f' % v][0] + 5))
    astate['cur'] = nxt
    try: k = json.load(open(knobs_path))
    except Exception: k = {}
    k['NHOOK_BOT_AIM_HEAD'] = nxt; tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
    return cur, nxt, st
def hone(results, nstate, knobs_path):
    """stage 1: the lead per unit (TPU) among 0.06..0.14; once every value has 12+ throws, stage 2: the arc per unit (ARC) among
    0.015 / 0.025 / 0.035 with the best lead held. Each window's throws go to the setting that ran; the next is untried first, else
    the best hit rate (1 in 4 windows a random one)"""
    pick = lambda vals, st: ([v for v in vals if st.get('%.3f' % v, [0, 0])[0] == 0] or [random.choice(vals) if random.random() < 0.25 else
                             max(vals, key=lambda v: (st['%.3f' % v][1] + 1) / (st['%.3f' % v][0] + 2))])[0]
    hits = sum(1 for l in results if ': hit' in l or 'KILLED' in l)
    stage = nstate.get('stage', 1); key = 'stats' if stage == 1 else 'arc_stats'; st = nstate.setdefault(key, {})
    cur = nstate.get('cur', 0.10) if stage == 1 else nstate.get('arc', 0.025)
    t = st.setdefault('%.3f' % cur, [0, 0]); t[0] += len(results); t[1] += hits
    if stage == 1 and all(nstate['stats'].get('%.3f' % v, [0, 0])[0] >= 12 for v in TPU):
        best = max(TPU, key=lambda v: (nstate['stats']['%.3f' % v][1] + 1) / (nstate['stats']['%.3f' % v][0] + 2))
        nstate['stage'] = 2; nstate['cur'] = best; stage = 2; st = nstate.setdefault('arc_stats', {})
    if stage == 1: nxt = pick(TPU, st); nstate['cur'] = nxt
    else: nxt = pick(ARC, st); nstate['arc'] = nxt
    try: k = json.load(open(knobs_path))
    except Exception: k = {}
    k['NHOOK_BOT_NADE_TPU'] = nstate['cur']
    if stage == 2: k['NHOOK_BOT_NADE_ARC'] = nstate['arc']
    os.makedirs(os.path.dirname(knobs_path), exist_ok=True); tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
    return ('lead %.2f s a unit' % cur if key == 'stats' else 'arc %.3f rad a unit (lead %.2f)' % (cur, nstate['cur'])), \
           ('lead %.2f' % nxt if stage == 1 else 'arc %.3f' % nxt), dict(list(nstate['stats'].items()) + [('arc ' + a, b) for a, b in nstate.get('arc_stats', {}).items()])

REFIT = {'p': None, 't0': 0}
BASE = {}   # the walker's launch knobs (walker-coach.py show, --id): where the live knobs start from
def refit(rec_dir):
    """the live match into the models, in the background (one at a time)"""
    p = REFIT['p']
    if p is not None and p.poll() is None: return 'a refit is still running (%.0f s)' % (time.time() - REFIT['t0'])
    msg = ('the last refit took %.0f s; ' % REFIT.get('took', 0)) if p is not None else ''
    if p is not None: REFIT['took'] = time.time() - REFIT['t0']
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    cmd = ('taskpolicy -b nice -n 20 python3 shim/tools/engage-extract.py --tracks "%s" --out "%s" --quiet >/dev/null 2>&1; '
           'taskpolicy -b nice -n 20 python3 shim/tools/engage-model.py >/dev/null 2>&1') % (rec_dir, os.path.join(SUP, 'tracks', 'engagements'))
    REFIT['p'] = subprocess.Popen(['bash', '-c', cmd], cwd=root); REFIT['t0'] = time.time()
    return msg + 'refit started: this game\'s fights -> the models (the walker re-reads them when they change)'

def slots(port, leader):
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3))
    except Exception: return None, None
    me = next((p.get('slot') for p in d.get('players', []) if p.get('you')), None)
    ld = next((p.get('slot') for p in d.get('players', []) if p.get('name') == leader), None)
    WALKER_NAME[0] = next((p.get('name') for p in d.get('players', []) if p.get('you')), WALKER_NAME[0])
    return me, ld
WALKER_NAME = ['AIWalkerTest']

def med(v): return round(statistics.median(v), 1) if v else None

def role_model(rec_dir, port, ws):
    """the whole current match (the newest recording): every player's K / D, kill distances, weapons, speed, jumps near enemies,
    where they kill -> the walker's rank and the best player (the role model), by name from the walker's stats page"""
    paths = sorted(glob.glob(os.path.join(rec_dir, '*.jsonl.gz')), key=os.path.getmtime)
    if not paths: return None
    head, S, E = read(paths[-1])
    if len(S) < 50: return None
    try: names = {p.get('slot'): p.get('name') for p in json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3)).get('players', [])}
    except Exception: names = {}
    first = {r[0]: r for r in S[0]['p']}; last = {r[0]: r for r in S[-1]['p']}
    P = {}
    for sl, r in last.items():
        f = first.get(sl, r); P[sl] = {'k': max(0, (r[12] or 0) - (f[12] or 0) if sl in first else r[12] or 0), 'd': max(0, (r[13] or 0) - (f[13] or 0) if sl in first else r[13] or 0),
                                     'kd': [], 'kw': collections.Counter(), 'held': collections.Counter(), 'spd': [], 'jumps': 0, 'near_s': 0.0, 'spots': collections.Counter()}
    prev = {}; jt = {}; infront = {}
    for sl in P: P[sl].update({'alive_s': 0.0, 'still_s': 0.0, 'lat': [], 'react': []})
    for s0 in S:
        R = {r[0]: r for r in s0['p']}
        for sl, r in R.items():
            if sl not in P or not r[2]: prev.pop(sl, None); infront.pop(sl, None); continue
            P[sl]['held'][r[10] or '?'] += 1
            # HESITATION: an enemy within 40 units comes within 15 deg of the look -> seconds to the first shot (within 3 s)
            yw = math.radians(r[6] or 0); f = None
            for q in R.values():
                if q is r or not q[2] or (q[1] == r[1] and r[1] is not None and r[1] >= 0 and False): continue
                dx, dy = q[3] - r[3], q[4] - r[4]; dd = math.hypot(dx, dy)
                if 2 < dd < 40 and abs((math.degrees(math.atan2(dy, dx) - yw) + 180) % 360 - 180) < 15: f = q[0]; break
            st = infront.get(sl)
            if f is not None and st is None: infront[sl] = (s0['t'], r[14] or 0)
            elif st is not None:
                if (r[14] or 0) > st[1]: P[sl]['react'].append(s0['t'] - st[0]); infront[sl] = None if f is None else (1e18, 1e18)
                elif f is None or s0['t'] - st[0] > 3.0: infront.pop(sl, None)
            q = prev.get(sl)
            if q:
                dt = s0['t'] - q[0]
                if 0.05 < dt < 0.5:
                    vx, vy = (r[3] - q[1][3]) / dt, (r[4] - q[1][4]) / dt; sp = math.hypot(vx, vy)
                    P[sl]['spd'].append(sp); P[sl]['alive_s'] += dt; P[sl]['still_s'] += dt if sp < 0.3 else 0.0
                    near = any(o is not r and o[2] and o[1] != r[1] or (o is not r and o[2] and r[1] is None) for o in R.values() if math.hypot(o[3] - r[3], o[4] - r[4]) < 20)
                    if near:
                        P[sl]['near_s'] += dt; P[sl]['lat'].append(abs(-math.sin(yw) * vx + math.cos(yw) * vy))   # (sideways to the look: strafing)
                        if r[5] - q[1][5] > 0.12 and s0['t'] - jt.get(sl, -9) > 1.0: P[sl]['jumps'] += 1; jt[sl] = s0['t']
            prev[sl] = (s0['t'], r)
    T = [s0['t'] for s0 in S]
    for e in E:
        a, b = e.get('a'), e.get('b')
        if a not in P or a == b: continue
        i = min(range(len(T)), key=lambda j: abs(T[j] - e['t'])) if len(T) < 4000 else max(0, min(len(T) - 1, int((e['t'] - T[0]) * 10)))
        R = {r[0]: r for r in S[i]['p']}; k, v = R.get(a), R.get(b)
        if k and v: P[a]['kd'].append(math.hypot(k[3] - v[3], k[4] - v[4])); P[a]['spots'][(int(k[3] // 6) * 6, int(k[4] // 6) * 6)] += 1
        P[a]['kw'][e.get('how') or '?'] += 1
    score = lambda x: (x['k'] + 1) / (x['d'] + 1)
    order = sorted(P, key=lambda sl: (score(P[sl]), P[sl]['k']), reverse=True)
    cands = [sl for sl in order if sl != ws and P[sl]['k'] >= 5] or [sl for sl in order if sl != ws]
    rm = cands[0] if cands else None
    allkd = [d for sl, x in P.items() if sl != ws for d in x['kd']]
    return {'P': P, 'order': order, 'rm': rm, 'names': names, 'allkd': allkd, 'score': score, 'ws': ws}

def copy_role_model(o, knobs_path):
    """the walker takes the role model's fighting distance (vs everyone's) and its jumps near enemies, through the live knobs"""
    x = o['P'][o['rm']]; k = {}
    if len(x['kd']) >= 3 and len(o['allkd']) >= 10:
        k['NHOOK_BOT_DIST'] = round(min(2.5, max(0.5, statistics.median(x['kd']) / max(0.5, statistics.median(o['allkd'])))), 2)
    if x['near_s'] > 30: k['NHOOK_BOT_FJUMP'] = round(min(12.0, x['jumps'] / (x['near_s'] / 60.0)), 1)
    try: cur = json.load(open(knobs_path))
    except Exception: cur = {}
    me = o['P'].get(o.get('ws'))
    if me:   # the movement: a step (at most 25%) a window toward the role model -- reaction, strafing, pushing on instead of standing
        step = lambda c, r: c * min(1.25, max(0.8, r))
        if len(x['react']) >= 5 and len(me['react']) >= 3 and statistics.median(me['react']) > 0 and statistics.median(x['react']) < statistics.median(me['react']):   # (only toward a FASTER trigger: copying a slower one made the walker sluggish)
            k['NHOOK_BOT_REACT'] = round(min(3.0, max(0.3, step(cur.get('NHOOK_BOT_REACT', BASE.get('NHOOK_BOT_REACT', 1.0)), statistics.median(x['react']) / statistics.median(me['react'])))), 3)
        if len(x['lat']) >= 30 and len(me['lat']) >= 20 and statistics.median(me['lat']) > 0.05:
            k['NHOOK_BOT_LIBSCALE'] = round(min(3.0, max(0.5, step(cur.get('NHOOK_BOT_LIBSCALE', BASE.get('NHOOK_BOT_LIBSCALE', 1.0)), statistics.median(x['lat']) / statistics.median(me['lat'])))), 3)
        if x['alive_s'] > 60 and me['alive_s'] > 30:
            si, ms = x['still_s'] / x['alive_s'], me['still_s'] / me['alive_s']; pr = cur.get('NHOOK_BOT_PRESS', BASE.get('NHOOK_BOT_PRESS', 0.4))
            k['NHOOK_BOT_PRESS'] = round(min(0.95, pr + 0.1) if ms > si + 0.05 else max(0.2, pr - 0.05) if ms < si - 0.05 else pr, 2)
    cur.update(k); os.makedirs(os.path.dirname(knobs_path), exist_ok=True); tmp = knobs_path + '.tmp'; json.dump(cur, open(tmp, 'w')); os.replace(tmp, knobs_path)
    return k

def window(rec_dir, t0, t1, ws, ls):
    o = {'shadow': [], 'w': {'k': 0, 'd': 0, 'shots': 0, 'hits': 0, 'kd': [], 'kw': []}, 'l': {'k': 0, 'd': 0, 'shots': 0, 'hits': 0, 'kd': [], 'kw': []}, 'apart_s': 0.0}
    for path in sorted(glob.glob(os.path.join(rec_dir, '*.jsonl.gz')), key=os.path.getmtime)[-2:]:
        if os.path.getmtime(path) < t0: continue
        head, S, E = read(path); st = head.get('start') or 0
        if st and S: st -= S[0]['t']   # (a sample's t counts from the RECORDER's start; 'start' is this file's first sample, wall time)
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
    r0 = subprocess.run(['python3', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'walker-coach.py'), 'show'], capture_output=True, text=True)
    for l in r0.stdout.splitlines():
        if l.startswith('walker %s: ' % arg('--id', '4')): BASE.update({kv.split('=')[0]: float(kv.split('=')[1]) for kv in l.split(': ', 1)[1].split() if '=' in kv})
    logp = arg('--log', os.path.join(SUP, 'battles', 'aiwalker.log')); nst = {}
    NST = os.path.join(OUT, 'nade-state.json')   # (the grenade honing survives a restart of this study)
    try: nst.update({k: v for k, v in json.load(open(NST)).items() if k != 'off'})
    except Exception: pass
    nade_lines(logp, nst)   # (from now on)
    AST = os.path.join(OUT, 'aim-state.json'); ast = {}
    try: ast.update(json.load(open(AST)))
    except Exception: pass
    print('%s following-study every %d s (the walker on %d, its leader by name)' % (time.strftime('%H:%M:%S'), EVERY, port), flush=True)
    while True:
        t0 = time.time(); time.sleep(EVERY); t1 = time.time(); n += 1
        ws, ls = slots(port, leader)
        if ws is None: print('%s window %d: the walker is not in the game' % (time.strftime('%H:%M:%S'), n), flush=True); continue
        # (the leader away -- 2026-11-09, the user: "leave walker on his own refining for now": the role model, grenades and the refit
        #  go on; only the shadowing line needs the leader)
        o = window(rec, t0, t1, ws, ls if ls is not None else -999)
        sh = o['shadow']; w = o['w']
        print('%s window %d (the last %d s):' % (time.strftime('%H:%M:%S'), n, EVERY), flush=True)
        try: att = sum(1 for l in open(logp, 'rb').read()[nst.get('off', 0):].decode('utf-8', 'replace').splitlines() if 'shooting my leader' in l)
        except OSError: att = 0
        if ls is None: print('  protecting you: you are not in the game -- the walker plays on its own', flush=True)
        else: print('  protecting you: median %s units from you, within 6 units %s of the time, longest apart %.0f s; went after %d of your attackers' % (
            med(sh), '%.0f%%' % (100 * sum(d <= 6 for d in sh) / len(sh)) if sh else '-', o['apart_s'], att), flush=True)
        print('  walker this window: %d kills / %d deaths, accuracy %s, kills with %s' % (w['k'], w['d'], '%.0f%%' % (100 * w['hits'] / w['shots']) if w['shots'] else '-',
              ', '.join(sorted(set(w['kw']))) or '-'), flush=True)
        rmo = role_model(rec, port, ws)
        if rmo and rmo['rm'] is not None:
            P, nm = rmo['P'], rmo['names']; x = P[rmo['rm']]; me = P.get(ws)
            rank = rmo['order'].index(ws) + 1 if ws in rmo['order'] else None
            if me: print('  RANK vs the crowd: #%s of %d by K/D (%d kills / %d deaths this match)' % (rank, len(rmo['order']), me['k'], me['d']), flush=True)
            mv = lambda z: 'stands still %s, strafes %s u/s in fights, fires %s s after an enemy comes in front' % (
                '%.0f%%' % (100 * z['still_s'] / z['alive_s']) if z['alive_s'] > 10 else '-', med(z['lat']), med(z['react']))
            if me: print('  movement: role model %s\n            walker     %s' % (mv(x), mv(me)), flush=True)
            top = x['held'].most_common(2); spots = ', '.join('(%d, %d)' % c for c, _ in x['spots'].most_common(2))
            print('  role model: %s, %d kills / %d deaths: fights at %s units (everyone %s), %s jumps a minute near enemies, moves %s u/s; kills with %s; holds %s; kills from %s' % (
                nm.get(rmo['rm'], 'slot %s' % rmo['rm']), x['k'], x['d'], med(x['kd']), med(rmo['allkd']), round(x['jumps'] / (x['near_s'] / 60.0), 1) if x['near_s'] > 30 else '-',
                med(x['spd']), ', '.join('%s %d' % kv for kv in x['kw'].most_common(3)) or '-', ', '.join(h for h, _ in top) or '-', spots or '-'), flush=True)
            k = copy_role_model(rmo, os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json'))
            print('  copying the role model: %s' % (' '.join('%s=%s' % (a.replace('NHOOK_BOT_', ''), b) for a, b in k.items()) or 'not enough of his fights yet'), flush=True)
        res = nade_lines(logp, nst); kp = os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json')
        cur, nxt, st = hone(res, nst, kp)
        try: json.dump({k: v for k, v in nst.items() if k != 'off'}, open(NST, 'w'))
        except OSError: pass
        ha = hone_aim(ast, kp)
        if ha: print('  aim height: %.2f -> next %.2f; head hits a shot so far %s' % (ha[0], ha[1], ', '.join('%s: %.2f (%d shots)' % (k2, v[1] / v[0] if v[0] else 0, v[0]) for k2, v in sorted(ha[2].items()))), flush=True)
        try: json.dump(ast, open(AST, 'w'))
        except OSError: pass
        if '--refit' in sys.argv: print('  learning: ' + refit(rec), flush=True)
        print('  grenades: %d thrown, %d hit (%d killed) with %s -> next %s; so far %s' % (len(res), sum(1 for l in res if ': hit' in l or 'KILLED' in l),
              sum(1 for l in res if 'KILLED' in l), cur, nxt, ', '.join('%s: %d/%d' % (k, v[1], v[0]) for k, v in sorted(st.items()))), flush=True)
        open(os.path.join(OUT, 'learned.jsonl'), 'a').write(json.dumps({'t': round(t1), 'window': n, 'shadow_median': med(o['shadow']), 'walker': {k: v for k, v in o['w'].items()},
                                                                       'leader': {k: v for k, v in o['l'].items()}}) + '\n')

if __name__ == '__main__':
    sys.exit(main())
