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
It also writes the WEAPON ZONES the walker uses (2026-11-08; the user: "walker should use its best weapon based on situation that
can be derived from game observations of where kills happen with certain weapons. if opponent is in a certain zone and you have a
certain high kill weapon for that zone and aren't using it switch to it"): per map, tracks/maps/<map>.wzones.txt -- the kills of
every recorded match on that map by the victim's 6-unit cell and the killer's weapon CLASS (the walker's classes: precision, auto,
sniper, shotgun, rocket, melee = sword / hammer), and each class's share of the weapons carried on that map (players' samples),
so a class that kills more in a zone than it is carried shows (nbot.c wz_*).
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
WCLS = (('sniper', 'sniper'), ('beam_rifle', 'sniper'), ('rocket', 'rocket'), ('spartan_laser', 'rocket'), ('fuel_rod', 'rocket'), ('missile', 'rocket'), ('shotgun', 'shotgun'),
        ('mauler', 'shotgun'), ('energy_sword', 'melee'), ('energy_blade', 'melee'), ('hammer', 'melee'), ('battle_rifle', 'precision'), ('carbine', 'precision'), ('magnum', 'precision'),
        ('dmr', 'precision'), ('assault_rifle', 'auto'), ('smg', 'auto'), ('plasma_rifle', 'auto'), ('spike', 'auto'), ('needler', 'auto'), ('brute_shot', 'auto'),
        ('machinegun', 'auto'), ('plasma_pistol', 'auto'), ('plasma_cannon', 'auto'), ('sentinel', 'auto'))
def wclass(name):   # nbot_engage.c eg_wcls's classes (a name in any case, spaces or underscores)
    k = (name or '').lower().replace(' ', '_')
    if not k: return None
    for key, c in WCLS:
        if key in k: return c
    return None
CELL = 6.0
def mode_class(head):
    t = ' '.join(str(head.get(k) or '') for k in ('engine', 'variant', 'mode')).lower()
    if any(k in t for k in ('infection', 'zombie', 'flood')): return 'infection'
    if any(k in t for k in ('capture', 'flag', 'ctf')): return 'ctf'
    if any(k in t for k in ('slayer', 'swat', 'snipe', 'br ', 'fiesta', 'ffa', 'rumble', 'doubles', 'mlg', 'kills', 'team')): return 'slayer'
    return 'other'

def read(path):
    """one recording -> its game entry and {weapon: [kills, headshots, betrayals]}"""
    head = {}; w = defaultdict(lambda: [0, 0, 0]); players = set(); kills = 0; zones = defaultdict(lambda: defaultdict(int)); held = defaultdict(int)
    try:
        with gzip.open(path, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o; continue
                if 'p' in o:
                    for r in o['p']:
                        if len(players) < 256: players.add(r[0])
                        if len(r) > 11 and r[2] and not r[11]: c = wclass(r[10]); held[c or 'other'] += 1   # (on foot: the class in hand)
                    continue
                if o.get('ev') != 'kill': continue
                how = (o.get('how') or '').strip(); hs = how.lower().endswith(', headshot')
                if hs: how = how[:-len(', headshot')].strip()
                how = how or '(not named)'
                if o.get('verb') == 'killed':
                    w[how][0] += 1; w[how][1] += hs; kills += 1; c = wclass(how); p = o.get('pos')
                    if c and isinstance(p, list) and len(p) >= 2 and (p[0] or p[1]): zones['%d,%d' % (int(p[0] // CELL), int(p[1] // CELL))][c] += 1
                elif o.get('verb') == 'betrayed': w[how][2] += 1
    except (EOFError, OSError): pass
    g = {'file': os.path.basename(path), 'map': head.get('map'), 'class': mode_class(head), 'mode': head.get('mode') or head.get('variant') or head.get('engine'),
         'start': head.get('start'), 'players': len(players), 'kills': kills}
    return g, dict(w), {k: dict(v) for k, v in zones.items()}, dict(held)

def update(quiet=True):
    try: cache = json.load(open(CACHE))
    except Exception: cache = {}
    files = sorted(p for p in glob.glob(os.path.join(TR, '*.jsonl.gz')) if not p.endswith('_local.jsonl.gz'))
    now = time.time(); fresh = {}
    for p in files:
        st = os.stat(p); key = os.path.basename(p); c = cache.get(key)
        if c and c['size'] == st.st_size and c['mtime'] == st.st_mtime and 'zones' in c: fresh[key] = c; continue
        g, w, z, h = read(p); fresh[key] = {'size': st.st_size, 'mtime': st.st_mtime, 'game': g, 'w': w, 'zones': z, 'held': h}
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
    # weapon zones per map (the walker: nbot.c wz_load) -- text, one cell a line: "<cx> <cy> <class>=<kills> ..."
    Z = defaultdict(lambda: defaultdict(lambda: defaultdict(int))); H = defaultdict(lambda: defaultdict(int))
    for key, c in fresh.items():
        mp = c['game'].get('map')
        if not mp or c['game']['class'] == 'infection': continue   # (Infection's sword kills are the zombies' only weapon)
        for cell, by in (c.get('zones') or {}).items():
            for cl, n in by.items(): Z[mp][cell][cl] += n
        for cl, n in (c.get('held') or {}).items(): H[mp][cl] += n
    os.makedirs(os.path.join(TR, 'maps'), exist_ok=True)
    for mp, cells in Z.items():
        tot = sum(H[mp].values()) or 1
        lines = ['wzones 1 cell %g kills %d' % (CELL, sum(sum(v.values()) for v in cells.values())), 'held ' + ' '.join('%s=%.4f' % (cl, n / tot) for cl, n in sorted(H[mp].items()) if cl != 'other')]
        for cell, by in sorted(cells.items()):
            cx, cy = cell.split(','); lines.append('%s %s %s' % (cx, cy, ' '.join('%s=%d' % kv for kv in sorted(by.items()))))
        p = os.path.join(TR, 'maps', '%s.wzones.txt' % mp); t = p + '.tmp'; open(t, 'w').write('\n'.join(lines) + '\n'); os.replace(t, p)
    if not quiet:
        print('%d games, %d kills -> %s' % (len(games), out['kills'], OUT))
        for n, v in sorted(W.items(), key=lambda kv: -kv[1]['kills'])[:25]:
            print('  %-24s %-8s %6d kills  %5d headshots  in %3d games' % (n[:24], v['kind'], v['kills'], v['headshots'], sum(1 for k in v['per_game'] if k)))
    return out

if __name__ == '__main__':
    update(quiet='--quiet' in sys.argv)
