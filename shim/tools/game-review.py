#!/usr/bin/env python3
"""One game against what we assumed (2026-11-08; the user: "start randomly joining and completing games, and coaching yourself
based on the dataset you JUST collected for that game vs the assumptions you made about the map, and the larger aggregated data in
order to instruct the walker how to play the game in question"). For one recorded public match (engage-extract.py has cut it):
the same measures over THIS GAME, over the MAP before it (every other recorded match on that map, same game class) and over
EVERYTHING before it (that class on every map) -- first-shot wins, looking away, high ground, weapon classes' wins and distances,
radar turns, dying outnumbered, help, items, revenge, kills a minute, the winners' K/D line and coach.py's fight measures. A
measure is a SURPRISE when this game differs from the map's number by more than its noise (shares: 2 standard errors of a game
this size; medians / rates: 30% with 15+ samples). Writes tracks/reviews/<recording>.json (the work board shows the latest) and
prints the table; the per-map model (engage-model.py) then takes the game in, and walker-coach.py coaches the walkers against
this game's winners blended with the map's and everyone's (--game-file).
    python3 shim/tools/game-review.py [<recording file name> | latest] [--quiet]"""
import glob, gzip, importlib.util, json, math, os, statistics, sys, time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('coach', os.path.join(HERE, 'coach.py')); coach = importlib.util.module_from_spec(spec)
_a = sys.argv; sys.argv = _a[:1]; spec.loader.exec_module(coach); sys.argv = _a
TR = coach.TR; QUIET = '--quiet' in sys.argv

def share(v): return (sum(v) / len(v), len(v)) if v else (None, 0)
def median(v): v = [x for x in v if x is not None]; return (statistics.median(v), len(v)) if v else (None, 0)

def measures(R, keys):
    """R: the records of a pool; keys: its (file, slot) player-matches -> {measure: (value, n)}"""
    D = [e for e in R if e.get('kind', 'duel') == 'duel' and not e['ctx']['a_veh'] and not e['ctx']['b_veh'] and e['out']['win'] in ('a', 'b')]
    m = {}
    m['first_shot_wins'] = share([e['out']['win'] == 'a' for e in D])
    m['caught_away_survives'] = share([e['out']['win'] == 'b' for e in D if e['ctx']['b_err'] > 60])
    m['high_ground_wins'] = share([e['out']['win'] == 'a' for e in D if e['ctx']['dz'] > 1.5])
    m['low_ground_wins'] = share([e['out']['win'] == 'a' for e in D if e['ctx']['dz'] < -1.5])
    by = defaultdict(list); dist = defaultdict(list)
    for e in D: c = coach.wcls(e['ctx']['a_wpn']); by[c].append(e['out']['win'] == 'a'); dist[c].append(e['ctx']['dist'])
    for c in ('precision', 'auto', 'sniper', 'shotgun', 'rocket', 'sword'):
        if by.get(c): m['wins_with_' + c] = share(by[c]); m['distance_' + c] = median(dist[c])
    C = [e for e in R if e.get('kind') == 'contact' and e['dist'] < 8.2 and e['speed'] > 1.2]; m['radar_turns'] = share([e['turned'] is not None for e in C])
    m['outnumbered_dies'] = share([e['died'] for e in R if e.get('kind') == 'pressure' and e['level'] == 2])
    A = [e for e in R if e.get('kind') == 'aid']; m['gets_help'] = share([bool(e['aided']) for e in A])
    m['dies_alone'] = share([e['died5'] for e in A if not e['aided']]); m['dies_helped'] = share([e['died5'] for e in A if e['aided']])
    I = [e for e in R if e.get('kind') == 'item']; m['item_nearest_first'] = share([bool(e['first_was_nearest']) for e in I]); m['item_first_s'] = median([e['first_s'] for e in I])
    P = [e for e in R if e.get('kind') == 'respawn']; m['back_to_death_spot'] = share([e['reached'] is not None for e in P if e.get('base_reached') != 'none'])
    pl = [e for e in R if e.get('kind') == 'player' and e['seconds'] >= 120]
    m['kills_a_minute'] = median([e['kills'] / (e['seconds'] / 60) for e in pl])
    kd = sorted((e['kills'] + 1) / (e['deaths'] + 1) for e in pl); m['winners_kd_from'] = (kd[int(len(kd) * 0.75)] if kd else None, len(kd))
    # weapons: kills per minute held (the "weapons" records), top ones
    held = defaultdict(float); kills = defaultdict(int)
    for e in R:
        if e.get('kind') == 'weapons':
            for w, s in (e.get('held_s') or {}).items(): held[w] += s
            for w, k in (e.get('kills_holding') or {}).items(): kills[w] += k
    for w in sorted(held, key=lambda w: -held[w])[:6]:
        if held[w] >= 60: m['kills_per_min_' + w.replace(' ', '_')] = (kills[w] / (held[w] / 60), int(held[w]))
    by_file = defaultdict(list)
    for e in R: by_file[e['file']].append(e)
    fm = coach.measures(keys, by_file)
    for k in ('aim', 'push', 'strafe_def', 'strafe_att', 'turn_back', 'jumps_min', 'gren_min'):
        if k in fm and fm[k][0] is not None: m['fight_' + k] = fm[k]
    return m

LABEL = {'first_shot_wins': 'the first shot wins the duel', 'caught_away_survives': 'caught looking away, survives', 'high_ground_wins': 'attacking from above (1.5+ u) wins',
         'low_ground_wins': 'attacking from below wins', 'radar_turns': 'turns to a moving radar contact in range', 'outnumbered_dies': 'shot by 2+, dies',
         'gets_help': 'gets help when shot (team games)', 'dies_alone': 'shot with no help, dies in 5 s', 'dies_helped': 'shot and helped, dies in 5 s',
         'item_nearest_first': 'the nearest player takes a power item first', 'item_first_s': 'a power item taken after (s)', 'back_to_death_spot': 'goes back to where it died',
         'kills_a_minute': 'kills a minute (median player)', 'winners_kd_from': 'the winners\' K/D starts at', 'fight_aim': 'aim error while firing (deg)',
         'fight_push': 'closing in while attacking (u/s)', 'fight_strafe_def': 'strafe when shot (u/s)', 'fight_strafe_att': 'strafe while attacking (u/s)',
         'fight_turn_back': 'aims back when shot (s)', 'fight_jumps_min': 'jumps a fight minute', 'fight_gren_min': 'grenades a fight minute'}
def label(k):
    if k in LABEL: return LABEL[k]
    if k.startswith('wins_with_'): return 'attacker with a %s wins' % k[10:]
    if k.startswith('distance_'): return 'fight distance with a %s (u)' % k[9:]
    if k.startswith('kills_per_min_'): return 'kills a minute holding a %s' % k[14:].replace('_', ' ')
    return k
def is_share(k): return k in ('first_shot_wins', 'caught_away_survives', 'high_ground_wins', 'low_ground_wins', 'radar_turns', 'outnumbered_dies', 'gets_help', 'dies_alone',
                              'dies_helped', 'item_nearest_first', 'back_to_death_spot') or k.startswith('wins_with_')

def surprise(k, g, mref):
    (gv, gn), (mv, mn) = g, mref
    if gv is None or mv is None or gn < (8 if is_share(k) else 15) or mn < 15: return None
    if is_share(k):
        se = math.sqrt(max(mv * (1 - mv), 0.02) / gn); z = (gv - mv) / se; return round(z, 1) if abs(z) > 2 else None
    rel = (gv - mv) / max(abs(mv), 1e-6); return round(rel, 2) if abs(rel) > 0.3 else None

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    R = coach.load()
    files = sorted({e['file'] for e in R if e.get('kind') == 'meta'})
    target = a[0] if a and a[0] != 'latest' else (files[-1] if files else None)
    if target and not target.endswith('.jsonl.gz'): target += '.jsonl.gz'
    G = [e for e in R if e['file'] == target]; meta = next((e for e in G if e.get('kind') == 'meta'), None)
    if not meta: print('no cut recording', target, '(run engage-extract.py first)'); return 1
    mp, cls = meta['map'], meta['class']; t0 = meta.get('start') or 0
    before = [e for e in R if e['file'] != target and e['file'] < target and e.get('class') == cls]   # (file names start with the date: "before" = earlier)
    M = [e for e in before if e.get('map') == mp]
    keys = lambda S: [(e['file'], e['slot']) for e in S if e.get('kind') == 'player' and e['seconds'] >= 120 and not e.get('you')]
    mg, mm, ma = measures(G, keys(G)), measures(M, keys(M)), measures(before, keys(before))
    rows = []
    for k in mg:
        g = mg[k]; m = mm.get(k, (None, 0)); e = ma.get(k, (None, 0))
        if g[0] is None: continue
        rows.append({'key': k, 'measure': label(k), 'share': is_share(k), 'game': round(g[0], 3), 'game_n': g[1], 'map': None if m[0] is None else round(m[0], 3), 'map_n': m[1],
                     'all': None if e[0] is None else round(e[0], 3), 'all_n': e[1], 'surprise_vs_map': surprise(k, g, m), 'surprise_vs_all': surprise(k, g, e)})
    rows.sort(key=lambda r: (r['surprise_vs_map'] is None, -abs(r['surprise_vs_map'] or 0), r['key']))
    games_on_map = len({e['file'] for e in M if e.get('kind') == 'meta'})
    out = {'updated': time.time(), 'file': target, 'map': mp, 'class': cls, 'mode': meta.get('mode'), 'players': meta.get('players_max'), 'minutes': round(meta.get('seconds', 0) / 60, 1),
           'games_on_map_before': games_on_map, 'rows': rows, 'team_game': any(e.get('team_game') for e in G if e.get('kind', 'duel') == 'duel'),
           'surprises': [r['measure'] for r in rows if r['surprise_vs_map'] is not None]}
    os.makedirs(os.path.join(TR, 'reviews'), exist_ok=True)
    p = os.path.join(TR, 'reviews', target.replace('.jsonl.gz', '.json')); json.dump(out, open(p + '.tmp', 'w'), indent=1); os.replace(p + '.tmp', p)
    if not QUIET:
        print('%s: %s %s, %s players, %.0f min; the map before it: %d game(s)' % (target, mp, cls, out['players'], out['minutes'], games_on_map))
        f = lambda v, sh: '-' if v is None else ('%.0f%%' % (100 * v) if sh else '%.2f' % v)
        print('%-46s %12s %12s %12s  %s' % ('', 'this game', 'the map', 'everything', 'surprise'))
        for r in rows:
            s = r['surprise_vs_map']; tag = '' if s is None else ('%+.1f SE' % s if r['share'] else '%+.0f%%' % (100 * s))
            print('%-46s %8s %3d %8s %3d %8s %3d  %s' % (r['measure'][:46], f(r['game'], r['share']), min(999, r['game_n']), f(r['map'], r['share']), min(999, r['map_n']),
                                                      f(r['all'], r['share']), min(999, r['all_n']), tag))
    return 0

if __name__ == '__main__':
    sys.exit(main())
