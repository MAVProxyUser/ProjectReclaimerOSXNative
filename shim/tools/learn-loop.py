#!/usr/bin/env python3
"""Learn from one random public game at a time (2026-11-08; the user: "start randomly joining and completing games, and coaching
yourself based on the dataset you JUST collected for that game vs the assumptions you made about the map, and the larger aggregated
data in order to instruct the walker how to play the game in question. iterate with the goal of better vs human scoring").
Every turn:
  1. OBSERVE: shim/tools/observe-public.sh joins a server drawn at random (OBS_RANDOM=1: weights by size and the tome's coverage;
     no Infection, no mods, no downloads) and records ONE whole match (OBS_MATCHES=1). The observer only watches: walkers never go
     on a public server (bot_allowed() refuses anything but our own / private addresses) -- the aim help would be cheating real
     players; they are measured AGAINST the real players instead.
  2. REVIEW: engage-extract.py cuts it; shim/tools/game-review.py compares it with the map's numbers before it and with everything
     (tracks/reviews/<recording>.json: the surprises).
  3. LEARN: engage-model.py refits the aggregate and the per-map models (the walkers' overlay for that map) and the tome.
  4. COACH: on that map (a stock map our local dedicated server can host; CTF / team games as Team Slayer 2v2, the walkers have no
     flag play), walker-coach.py runs --rounds battles of 4 walkers coached against THIS game's winners (x2) blended with the map's.
     "Better vs human scoring" = each walker's distance to the human winners (0 = their numbers; directional measures only count
     when the walker is worse) going down, round after round, game after game.
  5. LOG: tracks/learn-loop.json (one entry a turn: the game, its surprises, the walkers' distance per round) -- the work board.
    python3 shim/tools/learn-loop.py [--turns N] [--rounds 2] [--secs 300] [--walkers 4] [--min-players 4] [--no-battle]
Refuses to start beside a running Project Reclaimer (the user's game included): observe-public.sh and walker-battle.sh both check."""
import glob, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); TR = os.path.join(SUP, 'tracks'); LOG = os.path.join(TR, 'learn-loop.json')
STOCK = {'armory', 'bunkerworld', 'chill', 'chillout', 'construct', 'cyberdyne', 'deadlock', 'descent', 'docks', 'fortress', 'ghosttown', 'guardian', 'isolation',
         'lockout', 'midship', 'riverworld', 'salvation', 'sandbox', 'shrine', 'sidewinder', 'snowbound', 'spacecamp', 'warehouse', 'zanzibar'}
APPRE = '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
def arg(k, d=None):
    a = sys.argv; return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d
def say(*a): print(time.strftime('%H:%M'), *a, flush=True)
def run(cmd, env=None, quiet=False):
    return subprocess.run(cmd, cwd=ROOT, env=env, check=False, stdout=subprocess.DEVNULL if quiet else None).returncode
def load_log():
    try: return json.load(open(LOG))
    except Exception: return {'turns': []}
def save_log(L): t = LOG + '.tmp'; json.dump(L, open(t, 'w'), indent=1); os.replace(t, LOG)

def turn(k, n_turns):
    rounds = int(arg('--rounds', '2')); secs = int(arg('--secs', '300')); nw = arg('--walkers', '4')
    if subprocess.run(['pgrep', '-f', APPRE], stdout=subprocess.DEVNULL).returncode == 0: say('Project Reclaimer is running: waiting 5 minutes'); time.sleep(300); return None
    before = set(glob.glob(os.path.join(TR, '*.jsonl.gz')))
    say('turn %d of %d: observing one whole random public match' % (k, n_turns))
    env = dict(os.environ, OBS_MATCHES='1', OBS_RANDOM='1', OBS_MIN_PLAYERS=arg('--min-players', '4'), OBS_DONE='0')
    run(['bash', 'shim/tools/observe-public.sh', '5400', '3600'], env=env)
    new = sorted(set(glob.glob(os.path.join(TR, '*.jsonl.gz'))) - before, key=os.path.getsize)
    if not new: say('no recording made'); return None
    rec = os.path.basename(new[-1]); say('the game:', rec, '(%d KB)' % (os.path.getsize(new[-1]) // 1024))
    run([sys.executable, os.path.join(HERE, 'engage-extract.py'), '--quiet'])
    run([sys.executable, os.path.join(HERE, 'game-review.py'), rec])
    try: rv = json.load(open(os.path.join(TR, 'reviews', rec.replace('.jsonl.gz', '.json'))))
    except Exception: rv = {}
    run([sys.executable, os.path.join(HERE, 'engage-model.py')], quiet=True); say('models refit (aggregate, per map, tome)')
    entry = {'t': time.time(), 'recording': rec, 'map': rv.get('map'), 'class': rv.get('class'), 'mode': rv.get('mode'), 'players': rv.get('players'),
             'surprises': rv.get('surprises', []), 'battle': None}
    mp = rv.get('map')
    if '--no-battle' in sys.argv: pass
    elif mp not in STOCK: say('%s is not a stock map our server hosts: no walker battle this turn' % mp); entry['battle'] = 'not a stock map'
    else:
        teams = rv.get('team_game') or rv.get('class') == 'ctf'
        say('coaching %s walkers on %s (%s) against this game\'s winners: %d rounds of %d s' % (nw, mp, 'Team Slayer 2v2' if teams else 'free-for-all Slayer', rounds, secs))
        st0 = len(json.load(open(os.path.join(SUP, 'battles', 'coach', 'state.json'))).get('rounds', [])) if os.path.exists(os.path.join(SUP, 'battles', 'coach', 'state.json')) else 0
        cmd = [sys.executable, os.path.join(HERE, 'walker-coach.py'), 'loop', '--rounds', str(rounds), '--secs', str(secs), '--n', nw, '--map', mp, '--game', 'slayer', '--game-file', rec]
        if teams: cmd.append('--teams')
        run(cmd)
        try: rs = json.load(open(os.path.join(SUP, 'battles', 'coach', 'state.json'))).get('rounds', [])[st0:]
        except Exception: rs = []
        entry['battle'] = [{'label': r.get('label'), 'walkers': {w: {'likeness': v.get('likeness'), 'kd': v.get('kd'), 'duels_won': v.get('duels_won')} for w, v in r['walkers'].items()}} for r in rs]
        for r in rs: say('  round %s: distance to the winners %s' % (r.get('label'), ', '.join('w%s %s' % (w, v.get('likeness')) for w, v in sorted(r['walkers'].items()))))
    L = load_log(); L['turns'].append(entry); L['updated'] = time.time(); save_log(L)
    return entry

def main():
    n = int(arg('--turns', '6'))
    for k in range(1, n + 1):
        e = turn(k, n)
        if e is None: time.sleep(30)
        time.sleep(20)
    say('done: %d turns (tracks/learn-loop.json)' % n)

if __name__ == '__main__':
    main()
