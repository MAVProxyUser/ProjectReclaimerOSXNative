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
AIMH = [0.55, 0.65, 0.75, 0.85]   # (the old aim heights over the bounding sphere's centre, x its radius: the fallback, no longer honed)
HDZ = [-0.5, -0.25, 0.0, 0.25, 0.5]   # (07:55: head radii above the HEAD HITBOX's centre -- the brain aims at the real head now)
def aim_counts(port):
    """the walker's own judged shots so far (its stats page, the player marked 'you'): (aimed shots, through a hitbox, through the head)"""
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3))
    except Exception: return None
    a = next((p.get('aim') for p in d.get('players', []) if p.get('you')), None)
    if not a or not a.get('n'): return None
    return a['n'], a['n'] * (a.get('on') or 0), a['n'] * (a.get('on') or 0) * (a.get('head') or 0)
def hone_aim(astate, knobs_path, port):
    """the aim point's height over the HEAD HITBOX's centre (NHOOK_BOT_HEAD_DZ, head radii; was NHOOK_BOT_AIM_HEAD) by HEAD HITS a shot -- measured by the WALKER'S OWN client (its stats page judges
    its shots against the hitboxes, headless too since 2026-11-09: n aimed shots, 'on' = through a hitbox, 'head' = of those the
    topmost; the player marked 'you') -- the window's change
    in those cumulative counts credited to the height that ran; next: untried first, else the best (1 in 4 random)"""
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3))
    except Exception: return None
    a = next((p.get('aim') for p in d.get('players', []) if p.get('you')), None)
    if not a or not a.get('n'): return None
    shots, heads = a['n'], a['n'] * (a.get('on') or 0) * (a.get('head') or 0)
    cur = astate.get('dz_cur', 0.0); st = astate.setdefault('dz_stats', {}); last = astate.get('last')
    if last and shots >= last[0]:
        t = st.setdefault('%.2f' % cur, [0, 0.0]); t[0] += shots - last[0]; t[1] += max(0.0, heads - last[1])
    astate['last'] = [shots, heads]
    untried = [v for v in HDZ if st.get('%.2f' % v, [0, 0])[0] < 20]
    nxt = untried[0] if untried else random.choice(HDZ) if random.random() < 0.25 else max(HDZ, key=lambda v: (st['%.2f' % v][1] + 0.5) / (st['%.2f' % v][0] + 5))
    astate['dz_cur'] = nxt
    try: k = json.load(open(knobs_path))
    except Exception: k = {}
    k['NHOOK_BOT_HEAD_DZ'] = nxt; tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
    return cur, nxt, st
LEADS = [0.0, 0.02]   # (10:25: the aim lead in seconds; 11:15: 0.04 s registered 0.20 hits a shot vs 0.70 with none -- this client registers where it sees: only 0 / 0.02 tried now)
def hone_lead(astate, knobs_path, port):
    """the aim lead (NHOOK_BOT_AIMLEAD, s) by REGISTERED hits a shot -- the walker's own aim block: reg x reg_n (shots through a hitbox
    that the target's shield / health answered within 0.6 s) + miss_reg x miss_n (damage after a visible miss), over its aimed shots n;
    the window's change credited to the lead that ran; next: untried first (25+ shots), else the best (1 in 4 random)"""
    try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % port, timeout=3))
    except Exception: return None
    a = next((p.get('aim') for p in d.get('players', []) if p.get('you')), None)
    if not a or not a.get('n'): return None
    shots = a['n']; reg = (a.get('reg') or 0) * (a.get('reg_n') or 0) + (a.get('miss_reg') or 0) * (a.get('miss_n') or 0)
    cur = astate.get('lead_cur', 0.06); st = astate.setdefault('lead_stats', {}); last = astate.get('lead_last')
    if last and shots >= last[0]:
        t = st.setdefault('%.2f' % cur, [0, 0.0]); t[0] += shots - last[0]; t[1] += max(0.0, reg - last[1])
    astate['lead_last'] = [shots, reg]
    untried = [v for v in LEADS if st.get('%.2f' % v, [0, 0])[0] < 25]
    nxt = untried[0] if untried else random.choice(LEADS) if random.random() < 0.25 else max(LEADS, key=lambda v: (st['%.2f' % v][1] + 0.5) / (st['%.2f' % v][0] + 5))
    astate['lead_cur'] = nxt
    try: k = json.load(open(knobs_path))
    except Exception: k = {}
    k['NHOOK_BOT_AIMLEAD'] = nxt; tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
    return cur, nxt, st
def policy_ab(w, knobs_path, inc_w):
    """--policy-ab N (2026-11-09 14:00): N windows alternating the learnt policy OFF (NHOOK_BOT_POLICY 0) and ON (the trainer's weight),
    written at each window start; each window's kills / deaths credited to the phase that ran it (battles/follow/policy-ab.json)"""
    n_total = int(arg('--policy-ab', '0'))
    if n_total <= 0: return None
    p = os.path.join(SUP, 'battles', 'follow', 'policy-ab.json')
    try: st = json.load(open(p))
    except Exception: st = {'n': 0, 'on': [0, 0, 0], 'off': [0, 0, 0], 'cur': None}
    if st.get('cur') in ('on', 'off') and w is not None:
        t = st[st['cur']]; t[0] += w['k']; t[1] += w['d']; t[2] += 1; st['n'] += 1
    line = None
    if st['n'] < n_total:
        nxt = 'off' if st.get('cur') != 'off' else 'on'; st['cur'] = nxt
        try: k = json.load(open(knobs_path))
        except Exception: k = {}
        k['NHOOK_BOT_POLICY'] = 0.0 if nxt == 'off' else inc_w; tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
        line = 'policy A/B (%d of %d windows): next window policy %s' % (st['n'], n_total, 'OFF' if nxt == 'off' else 'ON (%.2f)' % inc_w)
    else:
        st['cur'] = None; line = 'policy A/B done (%d windows)' % st['n']
    on, off = st['on'], st['off']
    line += '; so far ON %d / %d in %d windows (K/D %.2f), OFF %d / %d in %d windows (K/D %.2f)' % (on[0], on[1], on[2], on[0] / max(1, on[1]), off[0], off[1], off[2], off[0] / max(1, off[1]))
    json.dump(st, open(p, 'w')); return line
def knob_ab(w, knobs_path):
    """--ab N --ab-set "K=v,K=v" [--ab-name NAME] (2026-11-09 14:35): N windows alternating the given live knobs SET (phase B) and
    REMOVED (phase A: the brain's defaults), written at each window start; each window's kills / deaths credited to the phase that ran it
    (battles/follow/ab-<name>.json)"""
    n_total = int(arg('--ab', '0')); spec = arg('--ab-set', '')
    if n_total <= 0 or not spec: return None
    name = arg('--ab-name', 'knobs'); kv = {}
    for part in spec.split(','):
        if '=' in part: k, v = part.split('=', 1); kv[k.strip()] = float(v)
    p = os.path.join(SUP, 'battles', 'follow', 'ab-%s.json' % name)
    try: st = json.load(open(p))
    except Exception: st = {'n': 0, 'A': [0, 0, 0], 'B': [0, 0, 0], 'cur': None, 'set': kv}
    if st.get('cur') in ('A', 'B') and w is not None:
        t = st[st['cur']]; t[0] += w['k']; t[1] += w['d']; t[2] += 1; st['n'] += 1
    try: k = json.load(open(knobs_path))
    except Exception: k = {}
    if st['n'] < n_total:
        nxt = 'B' if st.get('cur') != 'B' else 'A'; st['cur'] = nxt
        for kk, vv in kv.items():
            if nxt == 'B': k[kk] = vv
            else: k.pop(kk, None)
        line = 'A/B %s (%d of %d windows): next window %s' % (name, st['n'], n_total, 'B (%s)' % spec if nxt == 'B' else 'A (defaults)')
    else:
        for kk in kv: k.pop(kk, None)
        st['cur'] = None; line = 'A/B %s done (%d windows; defaults back)' % (name, st['n'])
    tmp = knobs_path + '.tmp'; json.dump(k, open(tmp, 'w')); os.replace(tmp, knobs_path)
    a, b = st['A'], st['B']
    line += '; so far A (defaults) %d / %d in %d windows (K/D %.2f), B %d / %d in %d windows (K/D %.2f)' % (a[0], a[1], a[2], a[0] / max(1, a[1]), b[0], b[1], b[2], b[0] / max(1, b[1]))
    json.dump(st, open(p, 'w')); return line
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
    # (07:50: one model build on this Mac at a time -- refresh-loop.sh and an earlier run of this script build the same models; two at
    #  once only doubled the CPU, each took 10-20 minutes)
    if subprocess.run(['pgrep', '-f', 'shim/tools/engage-(extract|model)\\.py'], capture_output=True).returncode == 0:
        REFIT['p'] = None; return msg + 'another model build is running: this one waits'
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    cmd = ('taskpolicy -b nice -n 20 python3 shim/tools/engage-extract.py --tracks "%s" --out "%s" --quiet >/dev/null 2>&1; '
           'taskpolicy -b nice -n 20 python3 shim/tools/engage-model.py >/dev/null 2>&1') % (rec_dir, os.path.join(SUP, 'tracks', 'engagements'))
    REFIT['p'] = subprocess.Popen(['bash', '-c', cmd], cwd=root); REFIT['t0'] = time.time()
    return msg + 'refit started: this game\'s fights -> the models (the walker re-reads them when they change)'

POLICY = {'p': None, 't0': 0, 'n': 0}
def policy_retrain(every=6):
    """the learnt movement policy (policy-train.py) again from the newest matches, on the GPU, every `every` windows, one at a time;
    the walker's brain reloads policy.json when it changes"""
    POLICY['n'] += 1; p = POLICY['p']
    if p is not None and p.poll() is None: return 'the policy is still training (%.0f s)' % (time.time() - POLICY['t0'])
    msg = ''
    if p is not None:
        try:
            J = json.load(open(os.path.join(SUP, 'battles', 'follow', 'policy.json'))); msg = 'policy retrained %s: move error %.2f (standing still %.2f); ' % (J.get('trained', '?'), J.get('val_move_err', 0), J.get('val_still_err', 0))
        except Exception: pass
        POLICY['p'] = None
    if POLICY['n'] % every: return msg + 'next policy training in %d windows' % (every - POLICY['n'] % every)
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    POLICY['p'] = subprocess.Popen(['bash', '-c', 'nice -n 10 python3 shim/tools/policy-train.py --epochs 8 > "%s" 2>&1' % os.path.join(SUP, 'battles', 'follow', 'policy-train.log')], cwd=root); POLICY['t0'] = time.time()
    return msg + 'policy training started (GPU)'

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

AIMP = [None]   # (the walker's aim counts at the last window's end)
TRAINED = {'NHOOK_BOT_DIST': (0.5, 2.5), 'NHOOK_BOT_FJUMP': (0.0, 12.0), 'NHOOK_BOT_LIBSCALE': (0.5, 3.0), 'NHOOK_BOT_PRESS': (0.2, 0.95),
           'NHOOK_BOT_REACT': (0.3, 3.0), 'NHOOK_BOT_RADSCALE': (0.3, 2.5), 'NHOOK_BOT_NOISE': (0.05, 2.0), 'NHOOK_BOT_GRENX': (0.1, 3.0),
           'NHOOK_BOT_HEADGATE': (0.6, 2.0), 'NHOOK_BOT_POLICY': (0.2, 1.0)}   # (10:20: the learnt policy's share of the fight moves)   # (07:35: "headshots being main goal": the head gate trained too; the aim HEIGHT is hone_aim's alone)
def lvl_knobs(level, inc):
    """the look sensitivity a level brings: the turn rate once reacting (7 rad/s at level 0, +10% a level, at most 14), the one before
    reacting (0.73 rad/s, +10% a level, at most 2), the aim noise x (1 - 0.04 a level, at least 0.5)"""
    return {'NHOOK_BOT_TURN': round(min(14.0, 7.0 * (1 + 0.1 * level)), 2), 'NHOOK_BOT_TURN0': round(min(2.0, 0.73 * (1 + 0.1 * level)), 3),
            'NHOOK_BOT_NOISE': round(max(0.05, inc.get('NHOOK_BOT_NOISE', 1.0) * max(0.5, 1 - 0.04 * level)), 3)}   # (09:30: the floor 0.3 -> 0.05: 'stays on the headshots')
def write_knobs(knobs_path, k):
    try: cur = json.load(open(knobs_path))
    except Exception: cur = {}
    cur.update(k); os.makedirs(os.path.dirname(knobs_path), exist_ok=True); tmp = knobs_path + '.tmp'; json.dump(cur, open(tmp, 'w')); os.replace(tmp, knobs_path)
def train(T, score, o, knobs_path):
    """the trainer (07:15, the user: "the walker should do better every match. the ranking should steadily move up ... increase look
    sensitivity as you level up skills"): an INCUMBENT knob set and its window scores; every 2 of its windows a CHALLENGER = halfway from
    it toward the role model's numbers + one random knob nudged (x e^N(0, 0.2)); the challenger plays 2 windows and is KEPT (level + 1)
    only if its mean beats the incumbent's last 4 by 0.05, else the incumbent comes back. Returns the lines to print."""
    out = []; inc = T['inc']
    if T.get('ch') is not None:
        T['ch_scores'].append(score)
        if len(T['ch_scores']) >= 2:
            ref = statistics.mean(T['inc_scores'][-4:]) if T['inc_scores'] else -9; m = statistics.mean(T['ch_scores'])
            if m > ref + 0.05:
                T['inc'] = T['ch']; T['inc_scores'] = list(T['ch_scores']); T['level'] += 1
                out.append('trainer: the challenger WON (%.2f vs %.2f): kept -- level %d (%s)' % (m, ref, T['level'], T.get('ch_desc', '')))
            else: out.append('trainer: the challenger lost (%.2f vs %.2f): back to the incumbent (%s)' % (m, ref, T.get('ch_desc', '')))
            T['ch'] = None; T['ch_scores'] = []; T['since'] = 0
            k = dict(T['inc']); k.update(lvl_knobs(T['level'], T['inc'])); write_knobs(knobs_path, k)
        else: out.append('trainer: the challenger has played %d of 2 windows (%s)' % (len(T['ch_scores']), T.get('ch_desc', '')))
        return out
    T['inc_scores'] = (T['inc_scores'] + [score])[-6:]; T['since'] = T.get('since', 0) + 1
    if T['since'] >= 2:
        ch = dict(inc); desc = []
        if o and o.get('rm') is not None:
            x = o['P'][o['rm']]; me = o['P'].get(o.get('ws'))
            # (12:00: not from a SNIPER / precision role model -- today's best player killed from 26 units with a sniper rifle, and its
            #  distance pulled the walker's assault-rifle fights out to sniper range: only when its kills are mostly close-range weapons)
            top_w = ' '.join(w for w, _ in x['kw'].most_common(2)).lower()
            far_rm = any(k in top_w for k in ('sniper', 'battle', 'carbine', 'beam', 'dmr', 'magnum'))
            if len(x['kd']) >= 3 and len(o['allkd']) >= 10 and not far_rm:
                t = min(2.5, max(0.5, statistics.median(x['kd']) / max(0.5, statistics.median(o['allkd'])))); ch['NHOOK_BOT_DIST'] = round((ch.get('NHOOK_BOT_DIST', 1.0) + t) / 2, 3); desc.append('DIST->%.2f' % ch['NHOOK_BOT_DIST'])
            if x['near_s'] > 30:
                t = min(12.0, x['jumps'] / (x['near_s'] / 60.0)); ch['NHOOK_BOT_FJUMP'] = round((ch.get('NHOOK_BOT_FJUMP', 0.0) + t) / 2, 2); desc.append('FJUMP->%.1f' % ch['NHOOK_BOT_FJUMP'])
            if me and len(x['lat']) >= 30 and len(me['lat']) >= 20 and statistics.median(me['lat']) > 0.05:
                r = statistics.median(x['lat']) / statistics.median(me['lat']); ch['NHOOK_BOT_LIBSCALE'] = round(ch.get('NHOOK_BOT_LIBSCALE', 1.0) * min(1.25, max(0.8, r)), 3); desc.append('LIBSCALE->%.2f' % ch['NHOOK_BOT_LIBSCALE'])
            if me and x['alive_s'] > 60 and me['alive_s'] > 30:
                si, ms = x['still_s'] / x['alive_s'], me['still_s'] / me['alive_s']
                if abs(ms - si) > 0.05: ch['NHOOK_BOT_PRESS'] = round(ch.get('NHOOK_BOT_PRESS', 0.4) + (0.1 if ms > si else -0.05), 2); desc.append('PRESS->%.2f' % ch['NHOOK_BOT_PRESS'])
            if me and len(x['react']) >= 5 and len(me['react']) >= 3 and statistics.median(x['react']) < statistics.median(me['react']):
                ch['NHOOK_BOT_REACT'] = round(ch.get('NHOOK_BOT_REACT', 1.0) * max(0.8, statistics.median(x['react']) / statistics.median(me['react'])), 3); desc.append('REACT->%.2f' % ch['NHOOK_BOT_REACT'])
        kn = random.choice(list(TRAINED)); ch[kn] = round(ch.get(kn, 1.0 if kn != 'NHOOK_BOT_PRESS' else 0.4) * math.exp(random.gauss(0, 0.2)), 3); desc.append('%s~%.2f' % (kn.replace('NHOOK_BOT_', ''), ch[kn]))
        for kk, (lo, hi) in TRAINED.items():
            if kk in ch: ch[kk] = min(hi, max(lo, ch[kk]))
        T['ch'] = ch; T['ch_scores'] = []; T['ch_desc'] = ' '.join(desc)
        k = dict(ch); k.update(lvl_knobs(T['level'], ch)); write_knobs(knobs_path, k)
        out.append('trainer: a challenger plays the next 2 windows: %s' % T['ch_desc'])
    else: out.append('trainer: the incumbent plays (%d window%s since the last challenger)' % (T['since'], '' if T['since'] == 1 else 's'))
    return out

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
    TRF = os.path.join(OUT, 'trainer.json'); TR = None   # (the trainer's incumbent / challenger / level: kept across restarts)
    try: TR = json.load(open(TRF))
    except Exception: pass
    if not TR:
        try: live = json.load(open(os.path.join(SUP, 'battles', 'live-knobs', 'AIWalkerTest.json')))
        except Exception: live = {}
        inc = {k: BASE.get(k, live.get(k)) for k in TRAINED if BASE.get(k, live.get(k)) is not None}; inc.update({k: live[k] for k in TRAINED if k in live})
        TR = {'level': 0, 'inc': inc, 'inc_scores': [], 'ch': None, 'ch_scores': [], 'since': 0}
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
        try:   # (the user: "each training session reload you should describe what the new brain is trying to show that it learned")
            les = json.loads(open(os.path.join(SUP, 'battles', 'follow', 'brain-lessons.jsonl')).read().strip().splitlines()[-1])
            print('  brain in play (built %s): %s' % (les['built'], les['lesson']), flush=True)
        except Exception: pass
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
        if w['k'] + w['d'] > 0:   # (a window the walker fought in: the trainer's turn)
            # (07:35: the score counts accuracy and HEADSHOT kills too -- "try to constantly bump up accuracy too ... headshots being main goal")
            hs_k = sum(1 for kw in w['kw'] if 'headshot' in (kw or '').lower())
            # (07:50: + the HEAD-HIT rate of its own judged shots this window -- through the head / aimed shots, the walker's own aim meter)
            ac = aim_counts(port); hr = None
            if ac and AIMP[0] and ac[0] - AIMP[0][0] >= 5: hr = max(0.0, ac[2] - AIMP[0][2]) / (ac[0] - AIMP[0][0])
            AIMP[0] = ac or AIMP[0]
            sc = math.log((w['k'] + 1) / (w['d'] + 1)) + 0.5 * (w['hits'] / w['shots'] if w['shots'] else 0.0) + 0.2 * hs_k + (hr or 0.0)
            print('  window score %.2f: K/D term %.2f, accuracy %s, head hits a shot %s, headshot kills %d' % (sc, math.log((w['k'] + 1) / (w['d'] + 1)),
                  '%.0f%%' % (100 * w['hits'] / w['shots']) if w['shots'] else '-', '%.2f' % hr if hr is not None else '-', hs_k), flush=True)
            for line in train(TR, sc, rmo, os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json')): print('  ' + line, flush=True)
            print('  trainer: level %d, incumbent %s, its last windows %s' % (TR['level'], ' '.join('%s=%s' % (k2.replace('NHOOK_BOT_', ''), v2) for k2, v2 in sorted(TR['inc'].items())),
                  ' '.join('%.2f' % v for v in TR['inc_scores'][-4:])), flush=True)
            json.dump(TR, open(TRF, 'w'), indent=1)
        res = nade_lines(logp, nst); kp = os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json')
        cur, nxt, st = hone(res, nst, kp)
        try: json.dump({k: v for k, v in nst.items() if k != 'off'}, open(NST, 'w'))
        except OSError: pass
        ha = hone_aim(ast, kp, port)
        hl = hone_lead(ast, kp, port)
        if hl: print('  aim lead: %.2f s -> next %.2f; registered hits a shot so far %s' % (hl[0], hl[1], ', '.join('%s s: %.2f (%d shots)' % (k2, v[1] / v[0] if v[0] else 0, v[0]) for k2, v in sorted(hl[2].items()))), flush=True)
        if ha: print('  aim height: %+.2f head radii over the head hitbox -> next %+.2f; head hits a shot so far %s' % (ha[0], ha[1], ', '.join('%s: %.2f (%d shots)' % (k2, v[1] / v[0] if v[0] else 0, v[0]) for k2, v in sorted(ha[2].items()))), flush=True)
        try: json.dump(ast, open(AST, 'w'))
        except OSError: pass
        if '--refit' in sys.argv: print('  learning: ' + refit(rec), flush=True)
        if '--refit' in sys.argv: print('  policy: ' + policy_retrain(), flush=True)
        pab = policy_ab(w, os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json'), TR['inc'].get('NHOOK_BOT_POLICY', 0.5))
        if pab: print('  ' + pab, flush=True)
        kab = knob_ab(w, os.path.join(SUP, 'battles', 'live-knobs', WALKER_NAME[0] + '.json'))
        if kab: print('  ' + kab, flush=True)
        print('  grenades: %d thrown, %d hit (%d killed) with %s -> next %s; so far %s' % (len(res), sum(1 for l in res if ': hit' in l or 'KILLED' in l),
              sum(1 for l in res if 'KILLED' in l), cur, nxt, ', '.join('%s: %d/%d' % (k, v[1], v[0]) for k, v in sorted(st.items()))), flush=True)
        open(os.path.join(OUT, 'learned.jsonl'), 'a').write(json.dumps({'t': round(t1), 'window': n, 'shadow_median': med(o['shadow']), 'walker': {k: v for k, v in o['w'].items()},
                                                                       'leader': {k: v for k, v in o['l'].items()}}) + '\n')

if __name__ == '__main__':
    sys.exit(main())
