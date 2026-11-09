#!/usr/bin/env python3
"""Hold the teams on our local server (2026-11-09; the user wants Third as the gunner of his Warthog: a walker can man a teammate's turret
only -- Halo 3 never seats an enemy there -- and coach-rounds.sh restarts the walkers every round, after which the server's auto-balance
puts them on any team). Every 5 s: the server's players (its stats page, 8767) and their teams; a player on the wrong team is moved with
RCON `team <name> <colour>` (native-dedicated.sh rcon). Walkers by name; every other player is the human (--human, default red).
    python3 shim/tools/hold-teams.py [--human red] [Third=red Second=blue Fourth=blue ...]   (default: Third red, Second / Fourth / Fifth blue)
The human's name is never printed (the user's nickname stays out of logs)."""
import json, os, subprocess, sys, time, urllib.request

COLOURS = ('red', 'blue', 'green', 'orange', 'purple', 'gold', 'brown', 'pink')
WALKERS = ('Second', 'Third', 'Fourth', 'Fifth')

def main():
    want = {'Third': 'red', 'Second': 'blue', 'Fourth': 'blue', 'Fifth': 'blue'}; human = 'red'
    a = sys.argv[1:]
    if '--human' in a: i = a.index('--human'); human = a[i + 1]; del a[i:i + 2]
    for kv in a:
        k, _, v = kv.partition('='); want[k] = v
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    last = {}
    while True:
        try: d = json.load(urllib.request.urlopen('http://127.0.0.1:8767/stats.json', timeout=3))
        except Exception: time.sleep(5); continue
        if d.get('in_match') and not (d.get('variant') or {}).get('rounds', {}).get('teams', True):
            if last.get('ffa') is None: print('hold-teams: the game has no teams (FFA): nothing to hold', flush=True)
            last['ffa'] = 1; time.sleep(5); continue
        last.pop('ffa', None)
        for p in d.get('players', []):
            n = p.get('name') or ''; c = want.get(n) if n in WALKERS else human; tn = p.get('team_name')
            if not c or c not in COLOURS or tn is None or tn == c: continue
            key = (n, c)
            if time.time() - last.get(key, 0) < 10: continue
            last[key] = time.time()
            r = subprocess.run(['bash', 'shim/tools/native-dedicated.sh', 'rcon', 'team', n, c], cwd=root, capture_output=True, text=True, timeout=20)
            who = n if n in WALKERS else 'the human'
            print('hold-teams: %s was %s -> %s (%s)' % (who, tn, c, (r.stdout.strip() or r.stderr.strip())[:80].replace(n, who) if n not in WALKERS else (r.stdout.strip() or r.stderr.strip())[:80]), flush=True)
        time.sleep(5)

if __name__ == '__main__':
    sys.exit(main())
