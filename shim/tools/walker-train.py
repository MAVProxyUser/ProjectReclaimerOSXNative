#!/usr/bin/env python3
"""Train the walkers' tactics against each other (2026-11-08; the user: "see how good you can get them vs each other in a rumble pit
match ... train your sparring partners well"). Each generation is one free-for-all on our local server (shim/tools/walker-battle.sh,
4 test identities); every walker gets its own tactics (nbot.c's NHOOK_BOT_* knobs) and the same skill (reaction and aim noise:
NHOOK_BOT_SKILL, fixed, so tactics -- not aim -- decide). Fitness = kills - 0.7 x deaths + 2 x accuracy (the server's own count).
The best two are kept, two children are mutated from them (and crossed). Everything is logged to
~/Library/Application Support/ReclaimerMac/battles/train.jsonl; the best tactics so far to battles/best-tactics.json.
    python3 shim/tools/walker-train.py [generations, default 6] [seconds per battle, default 300] [--skill 0.6] [--resume]
"""
import json, os, random, re, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
BT = os.path.expanduser('~/Library/Application Support/ReclaimerMac/battles'); os.makedirs(BT, exist_ok=True)
LOG = os.path.join(BT, 'train.jsonl'); BEST = os.path.join(BT, 'best-tactics.json')
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac/native')
KNOBS = {   # name: (env, low, high, start)
    'dist':    ('NHOOK_BOT_DIST', 0.5, 1.6, 1.0),
    'strafe':  ('NHOOK_BOT_STRAFE', 0.3, 1.6, 1.0),
    'switch':  ('NHOOK_BOT_SWITCH', 0.5, 2.0, 1.0),
    'fire':    ('NHOOK_BOT_FIRE_EXTRA', 0.3, 3.0, 1.0),
    'retreat': ('NHOOK_BOT_RETREAT', 0.0, 0.7, 0.35),
    'gren':    ('NHOOK_BOT_GREN', 4.0, 30.0, 9.0),
}
PROFILES = ['test-v2', 'test-second', 'test-third', 'test-fourth']

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
def nick(p):
    try: return json.load(open(os.path.join(SUP, p, 'profile.json')))['identity']['nickname']
    except Exception: return None
def clamp(k, v): lo, hi = KNOBS[k][1], KNOBS[k][2]; return max(lo, min(hi, v))
def mutate(g, s=0.18):
    c = dict(g)
    for k in KNOBS:
        if random.random() < 0.6: c[k] = round(clamp(k, c[k] + random.gauss(0, s) * (KNOBS[k][2] - KNOBS[k][1])), 3)
    return c
def cross(a, b): return {k: (a[k] if random.random() < 0.5 else b[k]) for k in KNOBS}
def env(g, skill): return ' '.join('%s=%s' % (KNOBS[k][0], g[k]) for k in KNOBS) + ' NHOOK_BOT_SKILL=%s' % skill

def battle(pop, secs, skill):
    e = dict(os.environ)
    for i, g in enumerate(pop): e['W%d_ENV' % i] = env(g, skill)
    before = set(os.listdir(BT))
    r = subprocess.run(['bash', 'shim/tools/walker-battle.sh', str(len(pop)), str(secs)], cwd=ROOT, env=e, capture_output=True, text=True)
    print(r.stdout[-1500:], flush=True)
    new = sorted(f for f in set(os.listdir(BT)) - before if f.endswith('.jsonl') and '-n' in f)
    if not new: return None, r.stdout
    L = [json.loads(l) for l in open(os.path.join(BT, new[-1])) if l.strip()]
    return (L[-1]['players'] if L else None), new[-1]

def main():
    gens = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 6
    secs = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 300
    skill = float(arg('--skill', '0.6'))
    names = [nick(p) for p in PROFILES]
    if None in names: print('a test identity is missing: bash shim/tools/walker-battle.sh init'); sys.exit(1)
    base = {k: v[3] for k, v in KNOBS.items()}
    if '--resume' in sys.argv and os.path.exists(BEST): base = json.load(open(BEST))['tactics']
    pop = [base, mutate(base), mutate(base), mutate(base, 0.3)]
    for gen in range(gens):
        print('=== generation %d: %s' % (gen, time.strftime('%H:%M')), flush=True)
        for i, g in enumerate(pop): print('  walker %d: %s' % (i, env(g, skill)), flush=True)
        players, src = battle(pop, secs, skill)
        if not players: print('  no result (%s): trying again' % (src[-300:] if isinstance(src, str) else src), flush=True); continue
        by = {p.get('name'): p for p in players}; res = []
        for i, g in enumerate(pop):
            p = by.get(names[i]) or {}; k, d, s, h = p.get('kills') or 0, p.get('deaths') or 0, p.get('shots') or 0, p.get('hits') or 0
            fit = k - 0.7 * d + 2.0 * (h / s if s else 0); res.append((fit, i, k, d, s, h))
        res.sort(reverse=True)
        for fit, i, k, d, s, h in res: print('  walker %d: fitness %.2f  kills %d  deaths %d  shots %d  hits %d' % (i, fit, k, d, s, h), flush=True)
        with open(LOG, 'a') as f: f.write(json.dumps({'t': time.time(), 'gen': gen, 'secs': secs, 'skill': skill, 'battle': src,
                                                     'walkers': [{'tactics': pop[i], 'fitness': fit, 'kills': k, 'deaths': d, 'shots': s, 'hits': h} for fit, i, k, d, s, h in res]}) + '\n')
        a, b = pop[res[0][1]], pop[res[1][1]]
        json.dump({'t': time.time(), 'gen': gen, 'fitness': res[0][0], 'tactics': a, 'env': env(a, skill)}, open(BEST, 'w'), indent=1)
        pop = [a, b, mutate(a), mutate(cross(a, b))]
    print('best tactics:', json.load(open(BEST)), flush=True)

if __name__ == '__main__':
    main()
