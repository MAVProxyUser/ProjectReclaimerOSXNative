#!/usr/bin/env python3
"""You vs the winners (2026-11-08; the user: "theoretically if I played a few matches you could tell me what I do wrong vs what the
winning population of halo that you've observed does eh?"). Your own public matches (recorded by nhook's ntracks.c when the game
runs through our launcher: the recording's header says you_plays and its "you" is your slot) are compared, measure by measure,
with the WINNERS -- the top quarter of players by K/D (5+ minutes in a non-Infection match, you left out) -- and with everyone.
Reads the engagement library (shim/tools/engage-extract.py: duels, radar contacts, pressure, aid, player summaries) and writes
tracks/coach.json (the work board's "You" tab) and prints the report: per measure your number, the winners', everyone's, a
verdict and what to do, biggest gaps first.
    python3 shim/tools/coach.py [--pretend <recording file name> <slot>] [--quiet]
--pretend treats one recorded player as "you" and only prints (to try the report; the You tab never shows it).
Only matches that start inside a session started on the board's You tab count (workboard/you-sessions.json), never a walker's."""
import glob, gzip, json, math, os, sys, time
from collections import defaultdict

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); TR = os.path.join(SUP, 'tracks')
def arg(k, n=1):
    a = sys.argv; i = a.index(k) if k in a else -1; return a[i + 1:i + 1 + n] if i >= 0 else None
QUIET = '--quiet' in sys.argv
def wcls(w):
    w = (w or '').upper()
    for c, keys in (('sniper', ('SNIPER', 'BEAM RIFLE')), ('laser', ('SPARTAN LASER',)), ('rocket', ('ROCKET', 'FUEL ROD', 'MISSILE')), ('shotgun', ('SHOTGUN', 'EXCAVATOR', 'MAULER')),
                    ('sword', ('ENERGY BLADE', 'ENERGY SWORD', 'HAMMER')), ('precision', ('BATTLE RIFLE', 'CARBINE', 'MAGNUM')), ('auto', ('ASSAULT RIFLE', 'SMG', 'PLASMA RIFLE', 'SPIKER', 'NEEDLER', 'BRUTE SHOT'))):
        if any(k in w for k in keys): return c
    return 'other'
def med(v):
    v = sorted(x for x in v if x is not None); return v[len(v) // 2] if v else None

def load():
    R = []
    for p in sorted(glob.glob(os.path.join(TR, 'engagements', '*.eng.jsonl.gz'))):
        try:
            with gzip.open(p, 'rt') as f:
                for l in f:
                    try: R.append(json.loads(l))
                    except Exception: break
        except (EOFError, OSError): pass
    return R

def rows_after(e): return [r for r in e['rows'] if r[0] >= e['attack_t'] - 1e-6]
def measures(keys, R_by_file):
    """the measures of a set of (file, slot) player-matches, pooled"""
    m = defaultdict(list); fight_s = 0.0; kills = deaths = secs = 0
    for (f, sl) in keys:
        for e in R_by_file.get(f, ()):
            k = e.get('kind', 'duel')
            if k == 'duel':
                if e['ctx']['a_veh'] or e['ctx']['b_veh']: continue
                me_a, me_b = e['a'] == sl, e['b'] == sl
                if not (me_a or me_b): continue
                m['first'].append(1 if me_a else 0)
                w = e['out']['win']
                if w in ('a', 'b'): m['win'].append(1 if (w == 'a') == me_a else 0)
                rr = rows_after(e); dur = (rr[-1][0] - rr[0][0]) if rr else 0; fight_s += dur
                if me_b:
                    m['caught_away'].append(1 if e['ctx']['b_err'] > 60 else 0)
                    tb = next((r[0] for r in e['rows'] if r[7] < 8), None)
                    if tb is not None: m['turn_back'].append(tb)
                    v = [r[4] for r in rr if r[4] is not None]
                    if v: m['strafe_def'].append(math.sqrt(sum(x * x for x in v) / len(v)))
                    m['jumps'].append(sum(1 for r in rr if r[15] is not None and r[15] > 2.5)); m['gren'].append(len(e['out'].get('b_throws') or []))
                if me_a:
                    m['dist_' + wcls(e['ctx']['a_wpn'])].append(e['ctx']['dist'])
                    v = [r[2] for r in rr if r[2] is not None]
                    if v: m['strafe_att'].append(math.sqrt(sum(x * x for x in v) / len(v)))
                    c = [r[3] for r in rr if r[3] is not None]
                    if c: m['push'].append(sum(c) / len(c))
                    a = [r[6] for r in rr if r[12]]
                    if a: m['aim'].append(med(a))
                    m['jumps'].append(sum(1 for r in rr if r[14] is not None and r[14] > 2.5)); m['gren'].append(len(e['out'].get('a_throws') or []))
            elif k == 'contact' and e.get('i') == sl and e['dist'] < 8.2 and e['speed'] > 1.2: m['radar'].append(1 if e['turned'] is not None else 0)
            elif k == 'pressure' and e.get('i') == sl and e['level'] == 2: m['outnumbered_died'].append(1 if e['died'] else 0)
            elif k == 'aid':
                if e.get('p') == sl: m['helped'].append(1 if e['aided'] else 0)
                if sl in (e.get('mate_slots') or []): m['helps'].append(1 if e.get('helper') == sl else 0)
            elif k == 'player' and e['slot'] == sl: kills += e['kills']; deaths += e['deaths']; secs += e['seconds']
    out = {}
    for k, v in m.items():
        if not v: continue
        out[k] = (sum(v) / len(v), len(v)) if k in ('first', 'win', 'caught_away', 'radar', 'outnumbered_died', 'helped', 'helps') else (med(v), len(v))
    if fight_s > 0:
        out['jumps_min'] = (sum(m['jumps']) / (fight_s / 60), len(m['jumps'])); out['gren_min'] = (sum(m['gren']) / (fight_s / 60), len(m['gren']))
    out['kd'] = ((kills + 1) / (deaths + 1), kills + deaths); out['kills_min'] = (kills / (secs / 60) if secs else None, int(secs))
    return out

# measure -> (label, kind: 'rate' / 'num', which way is better: +1 higher, -1 lower, 0 = closer to the winners, unit, advice when behind)
SPEC = [('kd', 'K/D (kills per death)', 'num', +1, '', ''),
        ('win', 'duels won', 'rate', +1, '', 'pick your fights: start them with your shields up, from the side, at your weapon\'s range'),
        ('first', 'duels where you shot first', 'rate', +1, '', 'see them first: watch the radar, check corners, move less in the open; the first shot wins 75% of duels'),
        ('caught_away', 'duels where you were caught looking away', 'rate', -1, '', 'keep your view on the lanes enemies come from and turn to moving radar blips; caught looking away you win 12%'),
        ('turn_back', 'time to aim back when shot (s)', 'num', -1, ' s', 'turn on the shooter faster: the moment your shield pops, find the direction of fire'),
        ('aim', 'aim error while firing (deg, median)', 'num', -1, ' deg', 'steadier aim: track the head, not the body; burst at range'),
        ('strafe_def', 'strafing when shot (units/s RMS)', 'num', 0, ' u/s', 'strafe like the winners do when shot -- not more, not less'),
        ('strafe_att', 'strafing while attacking (units/s RMS)', 'num', 0, ' u/s', 'match the winners\' strafe while you shoot: steadier aim beats dancing'),
        ('push', 'closing in while attacking (units/s)', 'num', 0, ' u/s', 'match how hard the winners press in: rushing past your weapon\'s range or backing off both lose duels'),
        ('jumps_min', 'jumps a minute in fights', 'num', 0, '', 'jump in fights about as often as the winners'),
        ('gren_min', 'grenades a minute in fights', 'num', 0, '', 'throw grenades about as often as the winners (most are thrown from 6-14 units)'),
        ('radar', 'turned to an unseen moving enemy within radar range', 'rate', +1, '', 'use the radar: a red blip moving close is a fight about to start -- face it'),
        ('outnumbered_died', 'died when 2+ shot you', 'rate', -1, '', 'avoid being caught by two: stay near a teammate (outnumbered with one near 20% die, alone 45%)'),
        ('helps', 'helped a teammate under fire (team games)', 'rate', +1, '', 'help: shoot whoever is shooting your teammate -- helped players die half as often'),
        ('helped', 'got help when you were shot (team games)', 'rate', +1, '', 'stay close enough to teammates that they can help (within ~8 units)'),
        ('dist_precision', 'fight distance with a BR / Carbine / Magnum', 'num', 0, ' u', 'precision rifles win most at 8-16 units: take those fights there'),
        ('dist_auto', 'fight distance with an Assault Rifle / SMG', 'num', 0, ' u', 'automatics win inside 8 units: close the distance before you open fire'),
        ('dist_sniper', 'fight distance with a Sniper', 'num', 0, ' u', 'snipers win 85-100% at any range they are used from; keep distance from shotguns')]

def main():
    R = load(); by_file = defaultdict(list)
    for e in R:
        if e.get('class') != 'infection': by_file[e['file']].append(e)
    players = [e for e in R if e.get('kind') == 'player' and e['class'] != 'infection' and e['seconds'] >= 300]
    pre = arg('--pretend', 2)
    # SESSIONS (2026-11-09; the user: "the you page should only be activated by a start and stop button on that page ... clear stats
    # that are manufactured and only use real stats from started sessions"): "you" = your slot in a match that STARTED inside a
    # session you started with the board's You tab (workboard/you-sessions.json) and that no walker recorded (shim/tools/walkerrec.py:
    # in the public walker's own recordings its slot was marked "you", and the tab showed the walker's games as yours). No session,
    # no report. --pretend only prints (it never reaches the tab).
    try: SS = json.load(open(os.path.join(SUP, 'workboard', 'you-sessions.json'))).get('sessions', [])
    except Exception: SS = []
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import walkerrec; WF = walkerrec.walker_files()
    start_of = {e['file']: e.get('start') or 0 for e in R if e.get('kind') == 'meta'}
    def in_session(f):
        st = start_of.get(f) or 0
        return any(x['start'] <= st < (x.get('end') or 1e12) for x in SS)
    if pre: you_keys = [(pre[0], int(pre[1]))]
    else: you_keys = [(e['file'], e['slot']) for e in players if e.get('you') and e['file'] not in WF and in_session(e['file'])]
    sess = [{'start': x['start'], 'end': x.get('end'), 'matches': len({f for f, _ in you_keys if x['start'] <= (start_of.get(f) or 0) < (x.get('end') or 1e12)})} for x in SS][-20:]
    if not you_keys:
        note = ('Press Start when you begin playing and Stop when you are done: only matches that start inside a session count.' if not SS else
                'No match of yours recorded in your sessions yet. A match counts when it starts inside a session and the game runs with nhook '
                '(launched through our launcher: shim/native.sh NATIVE_REAL=1, or Reclaimer.app); it is read a couple of minutes after it ends.')
        out = {'updated': time.time(), 'you': None, 'note': note, 'sessions': sess}
        if not pre:
            tmp = os.path.join(TR, 'coach.json.tmp'); json.dump(out, open(tmp, 'w'), indent=1); os.replace(tmp, os.path.join(TR, 'coach.json'))
        print(note); return
    others = [e for e in players if (e['file'], e['slot']) not in set(you_keys)]
    kd = sorted(((e['kills'] + 1) / (e['deaths'] + 1), e['file'], e['slot']) for e in others)
    top = kd[int(len(kd) * 0.75):] if kd else []
    W = measures([(f, s) for _, f, s in top], by_file); A = measures([(e['file'], e['slot']) for e in others], by_file); Y = measures(you_keys, by_file)
    rows = []
    for key, label, kind, better, unit, advice in SPEC:
        y, w, a = Y.get(key), W.get(key), A.get(key)
        if not y or y[0] is None or not w or w[0] is None: continue
        fmt = (lambda v: '%.0f%%' % (100 * v)) if kind == 'rate' else (lambda v: '%.2f%s' % (v, unit))
        gap = y[0] - w[0]; scale = max(abs(w[0]), 0.05 if kind == 'rate' else 0.3)
        if better == 0: off = abs(gap) / scale; behind = off > 0.25
        else: off = (-gap * better) / scale; behind = off > 0.1
        verdict = ('behind the winners' if behind else 'like the winners or better') if better else ('far from the winners' if behind else 'like the winners')
        if behind and better == 0:   # (a measure where the winners' number is the target: say which way you are off)
            advice = 'you %s (%s vs the winners\' %s): %s' % ('are above them' if gap > 0 else 'are below them', fmt(y[0]), fmt(w[0]), advice)
            verdict = 'more than the winners' if gap > 0 else 'less than the winners'
        rows.append({'measure': label, 'you': fmt(y[0]), 'you_n': y[1], 'winners': fmt(w[0]), 'everyone': fmt(a[0]) if a and a[0] is not None else '-', 'verdict': verdict,
                     'advice': advice if behind else '', 'gap': round(off, 3), 'thin': y[1] < 10})
    rows.sort(key=lambda r: (r['thin'], -r['gap'] if r['advice'] else 9))
    yk = Y.get('kd', (None, 0)); out = {'updated': time.time(), 'pretend': bool(pre), 'you': {'matches': len({f for f, _ in you_keys}), 'kd': round(yk[0], 2) if yk[0] else None},
           'winners': {'players': len(top), 'kd_from': round(top[0][0], 2) if top else None}, 'everyone': len(others), 'rows': rows, 'sessions': sess}
    if not pre:
        tmp = os.path.join(TR, 'coach.json.tmp'); json.dump(out, open(tmp, 'w'), indent=1); os.replace(tmp, os.path.join(TR, 'coach.json'))
    if not QUIET:
        print('You vs the winners%s: %d match(es) of yours; winners = the top quarter by K/D (%d players, K/D %.2f+) of %d player-matches' % (' (PRETEND)' if pre else '', out['you']['matches'], len(top), top[0][0] if top else 0, len(others)))
        print('%-52s %10s %10s %10s  %s' % ('', 'you', 'winners', 'everyone', 'verdict'))
        for r in rows: print('%-52s %10s %10s %10s  %s%s%s' % (r['measure'][:52], r['you'], r['winners'], r['everyone'], r['verdict'], ' (thin)' if r['thin'] else '', ('\n' + ' ' * 56 + '-> ' + r['advice']) if r['advice'] else ''))

if __name__ == '__main__':
    main()
