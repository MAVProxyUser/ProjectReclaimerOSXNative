#!/usr/bin/env python3
"""Five-minute epochs for a lone walker beside human players (2026-11-09; the user: "walkers should read the game corpus every 5 minutes
and retrain and understand where their K/D, shots fired vs landed, melee kills, powerup kills, power weapon kills, and heat maps per
level come into play in being better. the better walker variant will win out every 5 minutes in a weighted vote for the best model").

A POPULATION of walker variants (battles/epochs/population.json): each a full set of the walker's knobs (NHOOK_BOT_*). Variant A starts
as the coached knobs of the walker (walker-coach.py show), B..D as log-normal mutations of them. Every EPOCH (default 300 s):
  1. the epoch's numbers for the walker, from the battle's recording (battles/tracks/*_local.jsonl.gz -- the server's match recorder,
     10 samples a second + kill events): kills, deaths, shots and hits (the server's counters), melee kills (within 1.5 units or "melee"),
     power-weapon kills (rockets, sniper, laser, hammer, sword, shotgun, fuel rod, missile pod, brute shot, flamethrower, beam rifle),
     power-up kills (the walker overshielded or camouflaged at the kill), seconds holding the ball, and HEAT: the share of its deaths in
     the map's hot cells -- the top tenth of 3-unit cells by deaths over the WHOLE corpus of this map (tracks/ public recordings and
     battles/tracks), re-read every epoch;
  2. the epoch's SCORE = log((k+1)/(d+1)) + 0.8 accuracy + 0.3 (melee + power-weapon + power-up kills) + 0.02 ball seconds
     - 0.5 hot-death share, averaged into the variant's record (a running mean);
  3. a WEIGHTED VOTE for the next epoch: each variant's votes = exp(mean / 0.5) (one never tried: as if the best + 0.5 -- each gets its
     turn), the next variant drawn from them; every 3rd epoch the worst tried variant is replaced by a mutation of the best (the better
     variant wins out);
  4. a different variant -> the walker restarts with its knobs (walker-restart.sh, WR_ENV), else it plays on.
Each epoch -> battles/epochs/epochs.jsonl and a line on stdout.
    python3 shim/tools/walker-epochs.py <walkers' logs dir> [--walker Second] [--id 1] [--epoch 300] [--size 4]"""
import collections, glob, gzip, json, math, os, random, subprocess, sys, time, urllib.request

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); BT = os.path.join(SUP, 'battles'); EP = os.path.join(BT, 'epochs')
STATS = 8766   # (--stats: the walker's own stats page; main() sets it)
KNOBS = {'NHOOK_BOT_NOISE': (0.3, 3.0), 'NHOOK_BOT_REACT': (0.3, 3.0), 'NHOOK_BOT_DIST': (0.4, 3.0), 'NHOOK_BOT_GRENX': (0.1, 4.0),
         'NHOOK_BOT_RADSCALE': (0.3, 2.5), 'NHOOK_BOT_LIBSCALE': (0.3, 2.5), 'NHOOK_BOT_FJUMP': (0.0, 8.0), 'NHOOK_BOT_BALLBONUS': (1.0, 20.0)}
POWER = ('ROCKET', 'SNIPER', 'LASER', 'HAMMER', 'SWORD', 'BLADE', 'SHOTGUN', 'FUEL ROD', 'MISSILE', 'BRUTE SHOT', 'FLAME', 'BEAM RIFLE')

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d

def jget(url):
    try: return json.load(urllib.request.urlopen(url, timeout=3))
    except Exception: return None

def read_rec(p):
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

def hot_cells(mapname):
    """3-unit cells holding the top tenth of deaths on this map over the whole corpus (victims' last positions, kill events' pos)"""
    c = collections.Counter()
    for p in glob.glob(os.path.join(SUP, 'tracks', '*%s*.jsonl.gz' % mapname)) + glob.glob(os.path.join(BT, 'tracks', '*%s*.jsonl.gz' % mapname)):
        _, S, E = read_rec(p)
        for e in E:
            pos = e.get('pos')
            if pos: c[(int(pos[0] // 3), int(pos[1] // 3))] += 1
    if not c: return set(), 0
    v = sorted(c.values(), reverse=True); cut = v[max(0, len(v) // 10 - 1)]
    return {k for k, n in c.items() if n >= cut}, sum(c.values())

def epoch_numbers(t0, t1, walker):
    """the walker's numbers between wall times t0 and t1 from the battle's recordings"""
    out = {'kills': 0, 'deaths': 0, 'shots': 0, 'hits': 0, 'melee': 0, 'power': 0, 'powerup': 0, 'ball_s': 0.0, 'deaths_hot': 0, 'map': None, 'samples': 0}
    slot = None
    for port in (STATS, 8767):   # (the walker's own page first: "you" is the walker; the server's by name)
        d = jget('http://127.0.0.1:%d/stats.json' % port) or {}
        for p in d.get('players', []):
            if (port == STATS and p.get('you')) or p.get('name') == walker: slot = p.get('slot')
        if slot is not None: break
    if slot is None: return out
    for path in sorted(glob.glob(os.path.join(BT, 'tracks', '*_local.jsonl.gz')), key=os.path.getmtime)[-3:]:
        if os.path.getmtime(path) < t0: continue
        head, S, E = read_rec(path); st = head.get('start') or head.get('wall') or 0
        if st and S: st -= S[0]['t']   # (a sample's t counts from the RECORDER's start; 'start' is this file's first sample, wall time)
        if not st or not S: continue
        out['map'] = S[-1].get('map') or out['map']
        W = [s for s in S if t0 <= st + s['t'] <= t1]
        if len(W) < 2: continue
        out['samples'] += len(W)
        rows = [{r[0]: r for r in s['p']} for s in W]
        a = next((r[slot] for r in rows if slot in r), None); b = next((r[slot] for r in reversed(rows) if slot in r), None)
        if a and b:
            for k, i in (('kills', 12), ('deaths', 13), ('shots', 14), ('hits', 15)): out[k] += max(0, (b[i] or 0) - (a[i] or 0))
        for r in rows:
            q = r.get(slot)
            if q and q[2] and any(x in (q[10] or '').upper() for x in ('BALL', 'SKULL')): out['ball_s'] += 0.1
        hot, _ = hot_cells(out['map'] or '') if out['map'] else (set(), 0)
        for e in E:
            te = st + e['t']
            if not (t0 <= te <= t1): continue
            how = (e.get('how') or '').upper()
            if e.get('a') == slot and e.get('b') != slot:
                pos = e.get('pos'); near = None
                if 'MELEE' in how or 'BEAT' in how: out['melee'] += 1
                if any(x in how for x in POWER): out['power'] += 1
                s0 = min(W, key=lambda s: abs(st + s['t'] - te)); q = {r[0]: r for r in s0['p']}.get(slot)
                if q and ((q[8] or 0) > 1.05 or (len(q) > 17 and (q[17] or 0) > 0.5)): out['powerup'] += 1
            if e.get('b') == slot and e.get('pos') and (int(e['pos'][0] // 3), int(e['pos'][1] // 3)) in hot: out['deaths_hot'] += 1
    return out

def score(n):
    acc = n['hits'] / n['shots'] if n['shots'] else 0.0
    hot = n['deaths_hot'] / n['deaths'] if n['deaths'] else 0.0
    return math.log((n['kills'] + 1) / (n['deaths'] + 1)) + 0.8 * acc + 0.3 * (n['melee'] + n['power'] + n['powerup']) + 0.02 * n['ball_s'] - 0.5 * hot

def mutate(k, sigma=0.25):
    m = {}
    for name, (lo, hi) in KNOBS.items():
        v = float(k.get(name, 1.0 if name != 'NHOOK_BOT_BALLBONUS' else 8.0))
        v = v * math.exp(random.gauss(0, sigma)) if v > 0 else random.uniform(lo, hi) * 0.5
        m[name] = round(min(hi, max(lo, v)), 3)
    return m

def coached(wid):
    r = subprocess.run(['python3', 'shim/tools/walker-coach.py', 'show'], capture_output=True, text=True)
    for l in r.stdout.splitlines():
        if l.startswith('walker %s: ' % wid):
            return {kv.split('=')[0]: float(kv.split('=')[1]) for kv in l.split(': ', 1)[1].split() if '=' in kv}
    return {}

def main():
    global STATS; STATS = int(arg('--stats', '8766'))
    T = sys.argv[1]; walker = arg('--walker', 'Second'); wid = arg('--id', '1'); epoch = float(arg('--epoch', '300')); size = int(arg('--size', '4'))
    os.makedirs(EP, exist_ok=True); popf = os.path.join(EP, 'population.json')
    pop = json.load(open(popf)) if os.path.exists(popf) else None
    if not pop:
        base = coached(wid); base.setdefault('NHOOK_BOT_BALLBONUS', 8.0)
        pop = {'variants': [{'id': 'A', 'knobs': base, 'n': 0, 'mean': 0.0}] + [{'id': chr(66 + i), 'knobs': mutate(base), 'n': 0, 'mean': 0.0} for i in range(size - 1)],
               'current': 'A', 'epochs': 0, 'next_id': 66 + size - 1}
        json.dump(pop, open(popf, 'w'), indent=1)
    print('%s epochs of %.0f s for %s; variants %s; running %s' % (time.strftime('%H:%M:%S'), epoch, walker, ' '.join(v['id'] for v in pop['variants']), pop['current']), flush=True)
    restart(T, wid, pop)
    while True:
        t0 = time.time(); time.sleep(epoch); t1 = time.time()
        n = epoch_numbers(t0, t1, walker); s = score(n)
        cur = next(v for v in pop['variants'] if v['id'] == pop['current'])
        if n['samples'] > 100:
            cur['n'] += 1; cur['mean'] += (s - cur['mean']) / cur['n']
        pop['epochs'] += 1
        tried = [v for v in pop['variants'] if v['n']]
        if pop['epochs'] % 3 == 0 and len(tried) >= 2:   # the better variant wins out: the worst tried one -> a mutation of the best
            best = max(tried, key=lambda v: v['mean']); worst = min(tried, key=lambda v: v['mean'])
            if worst is not best:
                nid = chr(pop.get('next_id', 70) + 1) if pop.get('next_id', 70) < 90 else 'V%d' % pop['epochs']; pop['next_id'] = pop.get('next_id', 70) + 1
                pop['variants'][pop['variants'].index(worst)] = {'id': nid, 'knobs': mutate(best['knobs'], 0.15), 'n': 0, 'mean': 0.0, 'parent': best['id']}
                print('   %s (mean %.2f) out; %s in, from %s (mean %.2f)' % (worst['id'], worst['mean'], nid, best['id'], best['mean']), flush=True)
        top = max([v['mean'] for v in pop['variants'] if v['n']] or [0.0])
        votes = [(v, math.exp(((v['mean'] if v['n'] else top + 0.5) - top) / 0.5)) for v in pop['variants']]
        r = random.random() * sum(w for _, w in votes); nxt = votes[-1][0]
        for v, w in votes:
            r -= w
            if r <= 0: nxt = v; break
        rec = {'t': round(t1), 'variant': cur['id'], 'score': round(s, 3), 'numbers': n, 'votes': {v['id']: round(w, 3) for v, w in votes}, 'next': nxt['id']}
        open(os.path.join(EP, 'epochs.jsonl'), 'a').write(json.dumps(rec) + '\n')
        print('%s epoch %d: %s scored %.2f (K %d / D %d, %d of %d shots, melee %d, power weapon %d, power-up %d, ball %.0f s, deaths in hot cells %d) -> vote: %s; next %s' % (
            time.strftime('%H:%M:%S'), pop['epochs'], cur['id'], s, n['kills'], n['deaths'], n['hits'], n['shots'], n['melee'], n['power'], n['powerup'], n['ball_s'], n['deaths_hot'],
            ' '.join('%s %.2f' % (v['id'], w) for v, w in votes), nxt['id']), flush=True)
        if nxt['id'] != pop['current']:
            pop['current'] = nxt['id']; json.dump(pop, open(popf, 'w'), indent=1); restart(T, wid, pop)
        else: json.dump(pop, open(popf, 'w'), indent=1)

def restart(T, wid, pop):
    v = next(x for x in pop['variants'] if x['id'] == pop['current'])
    env = dict(os.environ); env['WR_ENV'] = ' '.join('%s=%s' % kv for kv in sorted(v['knobs'].items()))
    subprocess.Popen(['bash', 'shim/tools/walker-restart.sh', T, wid], env=env, stdout=open(os.path.join(EP, 'restart.log'), 'a'), stderr=subprocess.STDOUT)
    print('   walker restarting as variant %s: %s' % (v['id'], env['WR_ENV']), flush=True)

if __name__ == '__main__':
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    sys.exit(main())
