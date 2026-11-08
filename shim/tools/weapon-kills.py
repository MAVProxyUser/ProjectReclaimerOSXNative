#!/usr/bin/env python3
"""Kills by weapon, game by game (2026-11-08; the user: "on the learnings tab can you add some running kills counts to each weapon in
playertable like arrangement. add vehicle guns too ... grenades should also be accounted for. scrape the history and generate a full
table of kills. I want to be able to see the per game progression any time I go to the learnings tab").
Every recorded public match (tracks/*.jsonl.gz: the observer's match-recorder.py and nhook's ntracks.c; our own local servers'
recordings are left out) is read for its kill events -- the game's own kill feed: "<a> killed <b>" with how (the weapon, the
vehicle or its gun, the grenade, melee; ", headshot" counted as a headshot of that weapon) -- and summed per weapon per match.
    weapons  {name: {kind: weapon / vehicle (its driver's weapon or a splatter) / vehicle gun (a gunner seat) / turret / grenade / melee / other, per_game: [kills in each game, in game order],
              headshots, betrayals}}
    games    [{file, map, class, mode, start, players, kills, live}] oldest first (live = still being recorded)
tracks/weapon-kills.json (the work board's Learnings tab: a sortable table, each weapon's per-game line, a class filter). A cache
(tracks/weapon-kills.cache.json, per file by size and time) makes a rerun read only new or growing recordings, so the work board
reruns it every minute: the counts run while a match is played.
    python3 shim/tools/weapon-kills.py [--quiet]"""
import glob, gzip, json, os, sys, time
from collections import defaultdict

TR = os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks'); OUT = os.path.join(TR, 'weapon-kills.json'); CACHE = os.path.join(TR, 'weapon-kills.cache.json')
VEH = ('banshee', 'warthog', 'wraith', 'ghost', 'scorpion', 'hornet', 'mongoose', 'chopper', 'prowler', 'elephant', 'mammoth', 'falcon', 'revenant', 'gauss', 'chaingun', 'gunner', 'tank')
TUR = ('turret', 'machine gun', 'plasma cannon', 'missile pod', 'flamethrower')
def kind(w):
    l = w.lower()
    if not l or l == '(not named)' or l in ('explosion', 'fall', 'falling', 'collision', 'guardians', 'suicide', 'unknown', 'environment') or 'fall' in l: return 'other'
    if 'grenade' in l: return 'grenade'
    if l == 'melee': return 'melee'
    if any(k in l for k in ('gunner', 'chaingun', 'gauss')): return 'vehicle gun'   # (a vehicle's gun seat: the Warthog's chaingun / gauss, the Wraith's gun)
    if any(k in l for k in TUR): return 'turret'
    if any(k in l for k in VEH): return 'vehicle'
    return 'weapon'
def mode_class(head):
    t = ' '.join(str(head.get(k) or '') for k in ('engine', 'variant', 'mode')).lower()
    if any(k in t for k in ('infection', 'zombie', 'flood')): return 'infection'
    if any(k in t for k in ('capture', 'flag', 'ctf')): return 'ctf'
    if any(k in t for k in ('slayer', 'swat', 'snipe', 'br ', 'fiesta', 'ffa', 'rumble', 'doubles', 'mlg', 'kills', 'team')): return 'slayer'
    return 'other'

def read(path):
    """one recording -> its game entry and {weapon: [kills, headshots, betrayals]}"""
    head = {}; w = defaultdict(lambda: [0, 0, 0]); players = set(); kills = 0
    try:
        with gzip.open(path, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o; continue
                if 'p' in o:
                    if len(players) < 256:
                        for r in o['p']: players.add(r[0])
                    continue
                if o.get('ev') != 'kill': continue
                how = (o.get('how') or '').strip(); hs = how.lower().endswith(', headshot')
                if hs: how = how[:-len(', headshot')].strip()
                how = how or '(not named)'
                if o.get('verb') == 'killed': w[how][0] += 1; w[how][1] += hs; kills += 1
                elif o.get('verb') == 'betrayed': w[how][2] += 1
    except (EOFError, OSError): pass
    g = {'file': os.path.basename(path), 'map': head.get('map'), 'class': mode_class(head), 'mode': head.get('mode') or head.get('variant') or head.get('engine'),
         'start': head.get('start'), 'players': len(players), 'kills': kills}
    return g, dict(w)

def update(quiet=True):
    try: cache = json.load(open(CACHE))
    except Exception: cache = {}
    files = sorted(p for p in glob.glob(os.path.join(TR, '*.jsonl.gz')) if not p.endswith('_local.jsonl.gz'))
    now = time.time(); fresh = {}
    for p in files:
        st = os.stat(p); key = os.path.basename(p); c = cache.get(key)
        if c and c['size'] == st.st_size and c['mtime'] == st.st_mtime: fresh[key] = c; continue
        g, w = read(p); fresh[key] = {'size': st.st_size, 'mtime': st.st_mtime, 'game': g, 'w': w}
    t = CACHE + '.tmp'; json.dump(fresh, open(t, 'w'), separators=(',', ':')); os.replace(t, CACHE)
    games = []; per = []
    for key in sorted(fresh):   # (file names start with the date and time: game order)
        c = fresh[key]; g = dict(c['game']); g['live'] = now - c['mtime'] < 120
        if g['kills'] == 0 and not g['live']: continue   # (a recording of a lobby / a few seconds)
        games.append(g); per.append(c['w'])
    names = sorted({n for w in per for n in w})
    W = {}
    for n in names:
        pg = [w.get(n, [0, 0, 0])[0] for w in per]
        W[n] = {'kind': kind(n), 'per_game': pg, 'kills': sum(pg), 'headshots': sum(w.get(n, [0, 0, 0])[1] for w in per), 'betrayals': sum(w.get(n, [0, 0, 0])[2] for w in per)}
    out = {'updated': now, 'games': games, 'weapons': W, 'kills': sum(g['kills'] for g in games)}
    t = OUT + '.tmp'; json.dump(out, open(t, 'w'), separators=(',', ':')); os.replace(t, OUT)
    if not quiet:
        print('%d games, %d kills -> %s' % (len(games), out['kills'], OUT))
        for n, v in sorted(W.items(), key=lambda kv: -kv[1]['kills'])[:25]:
            print('  %-24s %-8s %6d kills  %5d headshots  in %3d games' % (n[:24], v['kind'], v['kills'], v['headshots'], sum(1 for k in v['per_game'] if k)))
    return out

if __name__ == '__main__':
    update(quiet='--quiet' in sys.argv)
