#!/usr/bin/env python3
"""The walkers coach themselves (2026-11-08; the user: "the walkers can use the coach on themselves eh!?"). shim/tools/coach.py
compares a player's fights with the WINNERS (the top quarter of real players by K/D); here every walker of a battle gets the same
report and each measure moves one of its knobs part of the way toward the winners, then the next battle runs with the new knobs.
    python3 shim/tools/walker-coach.py loop [--rounds N] [--secs S] [--n 2..4] [--map guardian] [--game slayer] [--teams] [--fresh]
                                            [--ded-map <the server's map name>] [--game-file <a public recording: its winners x2>]
    python3 shim/tools/walker-coach.py round <battle tracks dir> <battle samples .jsonl> [--map M] [--class C]   (coach one battle)
    python3 shim/tools/walker-coach.py show                                                                      (the knobs + history)
measure (coach.py)                     knob (nbot.c / nbot_goals.c)
  closing in while attacking           NHOOK_BOT_RADSCALE  x the replayed closing / backing off
  aim error while firing               NHOOK_BOT_NOISE     x the aim noise
  time to aim back when shot           NHOOK_BOT_REACT     x the reaction delay
  strafing when shot / attacking       NHOOK_BOT_LIBSCALE  x the replayed strafe
  jumps a minute in fights             NHOOK_BOT_FJUMP     extra jumps a fight minute
  grenades a minute in fights          NHOOK_BOT_GRENX     x the time between grenades
  turned to a moving radar contact     NHOOK_BOT_RADARLOOK x the radar-look odds
  fight distance (per weapon class)    NHOOK_BOT_DIST      x the players' distance bands
Outcomes (duels won, K/D, shot first) are not knobs: they are what the loop is judged by. A step is half the gap in log space,
at most x2 / x0.5 a round, and only from 8+ samples of that measure. The winners: real players of the same game class on the same
map when 40+ player-matches exist, else that class on every map, else every non-Infection match. "like the winners" = the walker's
distance to them (coach.py's gap, mean over the measures; 0 = the winners' numbers) -- written per round.
State: ~/Library/Application Support/ReclaimerMac/battles/coach/state.json (the knobs per walker identity 0..3 and every round)."""
import importlib.util, json, math, os, subprocess, sys, time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location('coach', os.path.join(HERE, 'coach.py')); coach = importlib.util.module_from_spec(spec)
sys.argv, _a = sys.argv[:1], sys.argv; spec.loader.exec_module(coach); sys.argv = _a   # (coach.py reads its own flags at import)
SUP = coach.SUP; BT = os.path.join(SUP, 'battles'); CO = os.path.join(BT, 'coach'); STATE = os.path.join(CO, 'state.json')
def arg(k, d=None):
    a = sys.argv; return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d

# knob: default, low, high, kind ('x' multiplicative, '+' additive)
KNOBS = {'RADSCALE': (1.0, 0.2, 2.5, 'x'), 'NOISE': (1.0, 0.3, 3.0, 'x'), 'REACT': (1.6, 0.4, 4.0, 'x'), 'LIBSCALE': (1.0, 0.3, 2.5, 'x'),
         'FJUMP': (0.0, 0.0, 8.0, '+'), 'GRENX': (1.0, 0.3, 4.0, 'x'), 'RADARLOOK': (1.0, 0.2, 5.0, 'x'), 'DIST': (1.0, 0.5, 2.0, 'x')}
MIN_N = 8; STEP = 0.5
H3MAPS = {'armory': "Rat's Nest", 'bunkerworld': 'Standoff', 'chill': 'Narrows', 'chillout': 'Cold Storage', 'construct': 'Construct', 'cyberdyne': 'The Pit',
          'deadlock': 'High Ground', 'descent': 'Assembly', 'docks': 'Longshore', 'fortress': 'Citadel', 'ghosttown': 'Ghost Town', 'guardian': 'Guardian',
          'isolation': 'Isolation', 'lockout': 'Blackout', 'midship': 'Heretic', 'riverworld': 'Valhalla', 'salvation': 'Epitaph', 'sandbox': 'Sandbox',
          'shrine': 'Sandtrap', 'sidewinder': 'Avalanche', 'snowbound': 'Snowbound', 'spacecamp': 'Orbital', 'warehouse': 'Foundry', 'zanzibar': 'Last Resort'}
NICKS = {'Second': 1, 'Third': 2, 'Fourth': 3}   # (walker-battle.sh's test identities; any other name = test-v2, index 0 -- names are never written)

def load_state():
    try: return json.load(open(STATE))
    except Exception: return {'knobs': {}, 'rounds': []}
def save_state(st):
    os.makedirs(CO, exist_ok=True); t = STATE + '.tmp'; json.dump(st, open(t, 'w'), indent=1); os.replace(t, STATE)
def knobs_of(st, k):
    kn = {n: v[0] for n, v in KNOBS.items()}; kn.update(st['knobs'].get(str(k), {})); return kn
def env_of(kn): return ' '.join('NHOOK_BOT_%s=%.3f' % (n, v) for n, v in sorted(kn.items()) if abs(v - KNOBS[n][0]) > 1e-6)

def blend(levels):
    """[(measures, weight)] -> one measures dict: each measure the weighted mean of the levels that have it (n summed)"""
    out = {}
    for k in set().union(*[set(W) for W, _ in levels]):
        v = [(W[k][0], W[k][1], w) for W, w in levels if k in W and W[k][0] is not None]
        if v: out[k] = (sum(a * w for a, _, w in v) / sum(w for _, _, w in v), sum(n for _, n, _ in v))
    return out

def winners(R, by_file, cls, mp, game_file=None):
    """the winners' measures. With game_file (learn-loop.py: the public game just recorded): THAT game's top quarter (8+ players of
    2+ minutes) weighted 2, blended with the map's (or the class's) winners weighted 1 -- "the dataset you JUST collected for that
    game vs the assumptions you made about the map, and the larger aggregated data" """
    if game_file:
        if not game_file.endswith('.jsonl.gz'): game_file += '.jsonl.gz'
        gp = [e for e in R if e.get('kind') == 'player' and e['file'] == game_file and e['seconds'] >= 120 and not e.get('you')]
        base, bdesc, bsame = winners(R, by_file, cls, mp)
        if len(gp) >= 8:
            kd = sorted(((e['kills'] + 1) / (e['deaths'] + 1), e['file'], e['slot']) for e in gp); top = kd[int(len(kd) * 0.75):]
            G = coach.measures([(f, s) for _, f, s in top], by_file)
            lv = [(G, 2.0)] + ([(base, 1.0)] if base else [])
            return blend(lv), 'this game\'s winners (%d of %d players, K/D %.2f+) x2 + %s' % (len(top), len(gp), top[0][0], bdesc if base else 'nothing else'), True
        return base, bdesc + ' (this game: too few players)', bsame
    pl = [e for e in R if e.get('kind') == 'player' and e['class'] != 'infection' and e['seconds'] >= 300 and not e.get('you')]
    for pool, why in (([e for e in pl if e['class'] == cls and e.get('map') == mp], '%s on %s' % (cls, mp)), ([e for e in pl if e['class'] == cls], '%s, every map' % cls), (pl, 'every match')):
        if len(pool) >= 40:
            kd = sorted(((e['kills'] + 1) / (e['deaths'] + 1), e['file'], e['slot']) for e in pool); top = kd[int(len(kd) * 0.75):]
            return coach.measures([(f, s) for _, f, s in top], by_file), '%s (%d players, top quarter K/D %.2f+)' % (why, len(top), top[0][0]), mp in why
    return None, 'too few real players recorded', False

def ratio(w, y, lo=0.5, hi=2.0):
    return max(lo, min(hi, (w / y) ** STEP)) if w > 0 and y > 0 else 1.0

BETTER = {s[0]: s[3] for s in coach.SPEC}
def steps(Y, W, kn, same_map=False):
    """the knob changes one walker's measures ask for: [(knob, old, new, why)]. A measure with a better direction (aim error,
    time to aim back, radar turns) moves its knob only while the walker is BEHIND the winners -- the goal is to beat the players,
    not to copy their mistakes; the others (closing, strafe, jumps, grenades, distance) move toward the winners' number. The fight
    distance only moves against winners on the same map (distances are the map's: Sandtrap's are not Guardian's)."""
    out = []; g = lambda k: ((Y.get(k) or (None, 0)), (W.get(k) or (None, 0)))
    def ok(k):
        (y, n), (w, _) = g(k)
        if y is None or w is None or n < MIN_N: return False
        b = BETTER.get(k, 0); return not (b and (y - w) * b >= 0)
    def setk(n, v, why):
        d, lo, hi, _ = KNOBS[n]; v = max(lo, min(hi, v))
        if abs(v - kn[n]) > 1e-3: out.append((n, kn[n], v, why)); kn[n] = v
    if ok('push'):
        (y, _), (w, _) = g('push')
        setk('RADSCALE', kn['RADSCALE'] * ratio(w, y) if y > 0.1 and w > 0.1 else kn['RADSCALE'] + STEP * (w - y) / 2.1, 'closing %.2f u/s vs the winners\' %.2f' % (y, w))
    if ok('aim'):
        (y, _), (w, _) = g('aim'); setk('NOISE', kn['NOISE'] * ratio(w, y), 'aim error %.1f deg vs %.1f' % (y, w))
    if ok('turn_back'):
        (y, _), (w, _) = g('turn_back'); setk('REACT', kn['REACT'] * ratio(w, y), 'aims back in %.2f s vs %.2f' % (y, w))
    lr = []; why = []
    for k in ('strafe_def', 'strafe_att'):
        if ok(k): (y, n), (w, _) = g(k); lr.append((math.log(ratio(w, y)), n)); why.append('%s %.2f vs %.2f' % (k.replace('strafe_', 'strafe '), y, w))
    if lr: setk('LIBSCALE', kn['LIBSCALE'] * math.exp(sum(a * n for a, n in lr) / sum(n for _, n in lr)), ', '.join(why))
    if ok('jumps_min'):
        (y, _), (w, _) = g('jumps_min'); setk('FJUMP', kn['FJUMP'] + STEP * (w - y), 'jumps %.2f a fight minute vs %.2f' % (y, w))
    if ok('gren_min'):
        (y, _), (w, _) = g('gren_min'); setk('GRENX', kn['GRENX'] * ratio(y, w), 'grenades %.2f a fight minute vs %.2f' % (y, w))
    if ok('radar'):
        (y, _), (w, _) = g('radar'); setk('RADARLOOK', kn['RADARLOOK'] * ratio(w + 0.02, y + 0.02), 'turned to radar contacts %.0f%% vs %.0f%%' % (100 * y, 100 * w))
    dl = []; why = []
    for k in (Y if same_map else ()):
        if k.startswith('dist_') and k != 'dist_other' and ok(k): (y, n), (w, _) = g(k); dl.append((math.log(ratio(w, y)), n)); why.append('%s %.1f u vs %.1f' % (k[5:], y, w))
    if dl: setk('DIST', kn['DIST'] * math.exp(sum(a * n for a, n in dl) / sum(n for _, n in dl)), 'fight distance ' + ', '.join(why))
    return out

def likeness(Y, W):
    """coach.py's gap per measure (0 = the winners' number), averaged over the measures with samples: lower = more like the winners"""
    gaps = {}
    for key, label, kind, better, unit, advice in coach.SPEC:
        y, w = Y.get(key), W.get(key)
        if not y or y[0] is None or not w or w[0] is None or y[1] < MIN_N or key == 'kd': continue
        scale = max(abs(w[0]), 0.05 if kind == 'rate' else 0.3); gap = y[0] - w[0]
        gaps[key] = round(abs(gap) / scale if better == 0 else max(0.0, -gap * better / scale), 3)
    return (round(sum(gaps.values()) / len(gaps), 3) if gaps else None), gaps

def coach_round(tracks, samples, mp=None, cls=None, apply=True, label='', game_file=None):
    eng = os.path.join(tracks, 'engagements')
    subprocess.run([sys.executable, os.path.join(HERE, 'engage-extract.py'), '--tracks', tracks, '--out', eng, '--force', '--quiet'], check=False, stdout=subprocess.DEVNULL)
    # slot -> walker identity from the battle's samples (the server's own view: names + slots)
    who = {}
    for l in open(samples):
        try: d = json.loads(l)
        except Exception: continue
        for p in d.get('players') or []:
            if p.get('slot') is not None: who[p['slot']] = NICKS.get(p.get('name'), 0)
    B = []
    import glob, gzip
    for p in sorted(glob.glob(os.path.join(eng, '*.eng.jsonl.gz'))):
        with gzip.open(p, 'rt') as f:
            for l in f:
                try: B.append(json.loads(l))
                except Exception: break
    bfile = defaultdict(list)
    for e in B: bfile[e['file']].append(e)
    files = sorted(bfile)
    if not files: print('no battle recording in', tracks); return None
    if not mp or not cls:
        pr = [e for e in B if e.get('kind') == 'player']
        mp = mp or (pr[0].get('map') if pr else 'guardian'); cls = cls or (pr[0].get('class') if pr else 'slayer')
    if cls == 'other': cls = 'slayer'   # (our dedicated server's recordings carry no game engine name: walker battles are Slayer)
    R = coach.load(); by_file = defaultdict(list)
    for e in R:
        if e.get('class') != 'infection': by_file[e['file']].append(e)
    W, wdesc, same_map = winners(R, by_file, cls, mp, game_file)
    if not W: print(wdesc); return None
    st = load_state(); rnd = {'t': time.time(), 'label': label, 'map': mp, 'class': cls, 'winners': wdesc, 'game_file': game_file, 'walkers': {}}
    print('round %s: %s %s; winners = %s' % (label or '-', mp, cls, wdesc))
    for sl in sorted(set(who)):
        k = who[sl]; Y = coach.measures([(f, sl) for f in files], bfile)
        kd = Y.get('kd', (None, 0)); like, gaps = likeness(Y, W); kn = knobs_of(st, k); old = dict(kn); ch = steps(Y, W, kn, same_map)
        dw = Y.get('win', (None, 0)); fs = Y.get('first', (None, 0))
        rnd['walkers'][str(k)] = {'kd': kd[0] and round(kd[0], 2), 'duels_won': dw[0] and round(dw[0], 3), 'duels': dw[1], 'shot_first': fs[0] and round(fs[0], 3),
                                  'likeness': like, 'gaps': gaps, 'knobs_before': old, 'knobs_after': kn, 'changes': [[n, round(a, 3), round(b, 3), w] for n, a, b, w in ch]}
        print('  walker %d (slot %d): K/D %s, duels won %s of %d, distance to the winners %s' % (k, sl, '%.2f' % kd[0] if kd[0] else '-', '%.0f%%' % (100 * dw[0]) if dw[0] is not None else '-', dw[1], like))
        for n, a, b, w in ch: print('      %-9s %.3f -> %.3f   (%s)' % (n, a, b, w))
        if apply: st['knobs'][str(k)] = kn
    if apply: st['rounds'].append(rnd); save_state(st)   # (--dry: printed only)
    return rnd

def show():
    st = load_state()
    for k in sorted(st['knobs']): print('walker %s: %s' % (k, env_of(knobs_of(st, k)) or '(defaults)'))
    for r in st['rounds'][-12:]:
        print(time.strftime('%H:%M', time.localtime(r['t'])), r.get('label', ''), r['map'], r['class'], ' '.join('w%s like %s kd %s' % (k, v['likeness'], v['kd']) for k, v in sorted(r['walkers'].items())))

def loop():
    rounds = int(arg('--rounds', '4')); secs = int(arg('--secs', '300')); n = int(arg('--n', '4')); mp = arg('--map', 'guardian'); game = arg('--game', 'slayer')
    dmap = arg('--ded-map') or H3MAPS.get(mp, mp); gfile = arg('--game-file')   # (our server takes the display name: riverworld -> Valhalla)
    st = load_state()
    if '--fresh' in sys.argv: st = {'knobs': {}, 'rounds': st.get('rounds', [])}; save_state(st)
    for i in range(rounds):
        st = load_state(); env = dict(os.environ); ids = (os.environ.get('WB_IDS') or '0 1 2 3').split()[:n]
        ammo_known = os.path.exists(os.path.join(SUP, 'native', 'ammo.json'))
        for k in ids: env['W%s_ENV' % k] = (env_of(knobs_of(st, k)) + ('' if ammo_known else ' NHOOK_WPNDUMP=1')).strip()   # (no ammo field known yet: the walkers dump their weapon -> ammo-scan.py)
        tag = time.strftime('%Y%m%d-%H%M%S'); tr = os.path.join(CO, 'tracks', tag); os.makedirs(tr, exist_ok=True)
        env.update(WB_TRACKS=tr, DED_MAP=dmap, DED_GAME={'slayer': 'Slayer', 'ctf': 'Slayer'}.get(game, game))   # (CTF: the walkers have no flag play -- Slayer on the map)
        if '--teams' in sys.argv: env['DED_TEAMS'] = '1'
        before = set(os.listdir(BT))
        print('== round %d of %d: %d walkers, %d s on %s %s%s' % (i + 1, rounds, n, secs, mp, game, ' (teams)' if '--teams' in sys.argv else ''), flush=True)
        for k in ids: print('   walker %s: %s' % (k, env['W%s_ENV' % k] or '(defaults)'))
        logs = None
        pr = subprocess.Popen(['bash', os.path.join(HERE, 'walker-battle.sh'), str(n), str(secs)], cwd=ROOT, env=env, stdout=subprocess.PIPE, text=True)
        for line in pr.stdout:
            print(line, end='', flush=True)
            if line.startswith('logs: '): logs = line[6:].strip()
        pr.wait()
        if logs and not ammo_known:   # the walkers' weapon dumps -> the rounds-loaded field (ReclaimerMac/native/ammo.json when clear)
            import glob as _g; subprocess.run([sys.executable, os.path.join(HERE, 'ammo-scan.py'), '--write'] + sorted(_g.glob(os.path.join(logs, 'w*.log'))), cwd=ROOT, check=False)
        new = sorted(f for f in set(os.listdir(BT)) - before if f.endswith('.jsonl'))
        if not new: print('no battle samples written: stopping'); return
        coach_round(tr, os.path.join(BT, new[-1]), mp=mp, cls={'ctf': 'ctf'}.get(game, 'slayer'), label='%s %d/%d' % (tag, i + 1, rounds), game_file=gfile)
        subprocess.run(['bash', os.path.join(HERE, 'native-dedicated.sh'), 'stop'], cwd=ROOT, check=False, stdout=subprocess.DEVNULL)
        time.sleep(5)

if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'round': coach_round(a[1], a[2], mp=arg('--map'), cls=arg('--class'), apply='--dry' not in a, game_file=arg('--game-file'))
    elif a and a[0] == 'loop': loop()
    elif a and a[0] == 'show': show()
    else: print(__doc__)
