#!/usr/bin/env python3
"""Do the walkers FIGHT like players? (2026-11-08) The engagement measures (shim/tools/engage-extract.py + engage-model.py) of the
real players' matches (ReclaimerMac/tracks/engagements, Infection left out) beside the walkers' battles (a folder of
match-recorder.py recordings: walker-battle.sh's WB_TRACKS or ReclaimerMac/battles/tracks), with the ratio, and a fight-likeness
score like walker-compare.py's (100 x exp(-mean |log ratio|)). The walkers' target choices are also scored with the PLAYERS'
choice rule (engage-model.json "target"): how often it names the target the walker took, against how often it names the
players' own (held-out) choices.
    python3 shim/tools/engage-compare.py <walkers' recordings folder> [--json out.json]"""
import glob, gzip, json, math, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__)); SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def report(eng):
    out = tempfile.mktemp(suffix='.json'); mod = tempfile.mktemp(suffix='.json')
    subprocess.run([sys.executable, os.path.join(HERE, 'engage-model.py'), '--eng', eng, '--out', mod, '--json', out], capture_output=True, text=True)
    try: return json.load(open(out))
    except Exception: return None
WDIR = next((a for a in sys.argv[1:] if not a.startswith('--') and os.path.isdir(a)), None)
if not WDIR: print(__doc__); sys.exit(1)
weng = os.path.join(tempfile.mkdtemp(), 'eng')
subprocess.run([sys.executable, os.path.join(HERE, 'engage-extract.py'), '--tracks', WDIR, '--out', weng, '--force', '--quiet'], capture_output=True, text=True)
subprocess.run([sys.executable, os.path.join(HERE, 'engage-extract.py'), '--quiet'], capture_output=True, text=True)   # (the players' newest recordings)
# the players on the walkers' maps when they have 300+ duels there (--all-maps: every map): small Guardian with Assault Rifles
# is no place to fight at Valhalla's Battle Rifle distances
peng = os.path.join(SUP, 'tracks', 'engagements'); wmaps = set()
for f in glob.glob(os.path.join(weng, '*.eng.jsonl.gz')):
    for l in gzip.open(f, 'rt'): wmaps.add(json.loads(l).get('map'))
if '--all-maps' not in sys.argv and wmaps:
    sel = os.path.join(tempfile.mkdtemp(), 'eng'); os.makedirs(sel); n = 0
    with gzip.open(os.path.join(sel, 'players.eng.jsonl.gz'), 'wt') as o:
        for f in glob.glob(os.path.join(peng, '*.eng.jsonl.gz')):
            for l in gzip.open(f, 'rt'):
                e = json.loads(l)
                if e.get('map') in wmaps: o.write(l); n += e.get('kind', 'duel') == 'duel'
    if n >= 300: peng = sel; print('players on %s only (%d duels; --all-maps = every map)' % (', '.join(sorted(m for m in wmaps if m)), n))
P, W = report(peng), report(weng)
if not P or not W: print('missing engagements (players: %s, walkers: %s)' % (bool(P), bool(W))); sys.exit(1)
p, w = P['all_but_infection'], W['all_but_infection']
def g(d, *ks):
    for k in ks:
        if not isinstance(d, dict): return None
        d = d.get(k)
    return d
rows = [('attack distance, median (units)', ('attack_distance_all',)), ('strafe speed RMS while attacking', ('move', 'strafe_rms')),
        ('strafe switch (s)', ('move', 'strafe_switch_s')), ('closing speed, shield up', ('move', 'closing_shield_up')),
        ('share of rows firing', ('move', 'fire_share')), ('aim error while attacking (deg)', ('move', 'aim_err_median')),
        ('attacked: time to aim back (s)', ('b_turns_to_aim_s', 'median')), ('caught looking away (share)', ('b_caught_looking_away',)),
        ('first attacker wins (share)', ('first_attacker_wins',)), ('turns to a second attacker (share)', ('multi', 'second_attacker', 'turned_to_it')),
        ('second attacker: time to turn (s)', ('multi', 'second_attacker', 'turn_s_median')), ('outnumbered: died (share)', ('multi', 'outnumbered', 'two_plus', 'died')),
        ('1v1 pressure: died (share)', ('multi', 'outnumbered', 'one_attacker', 'died'))]
print('%-38s %9s %9s %7s' % ('', 'players', 'walkers', 'ratio')); out = []; lr = []
for label, ks in rows:
    a, b = g(p, *ks), g(w, *ks); r = b / a if (isinstance(a, (int, float)) and isinstance(b, (int, float)) and a) else None
    print('%-38s %9s %9s %7s' % (label, '%.2f' % a if isinstance(a, (int, float)) else '-', '%.2f' % b if isinstance(b, (int, float)) else '-', '%.2f' % r if r is not None else '-'))
    out.append({'metric': label, 'players': a, 'walkers': b})
    if r is not None and r > 0 and a > 0 and b > 0: lr.append(abs(math.log(r)))
    elif isinstance(a, (int, float)) and isinstance(b, (int, float)): lr.append(min(3.0, abs(a - b) / max(abs(a), 0.5)))   # (signed: closing speed -- the gap against the players' size)
# the players' choice rule on the walkers' choices
wt = None
try: wt = json.load(open(os.path.join(SUP, 'tracks', 'engage-model.json'))).get('target')
except Exception: pass
if wt:
    FE = ['log_dist', 'off_aim', 'attacked_me', 'vitality', 'previous_target', 'teammate_on_it', 'just_hurt', 'height']
    def agree(folder):
        n = hit = 0
        for f in glob.glob(os.path.join(folder, '*.eng.jsonl.gz')):
            for l in gzip.open(f, 'rt'):
                c = json.loads(l)
                if c.get('kind') != 'choice' or c['class'] == 'infection': continue
                sc = [sum(wt[k] * x for k, x in zip(FE, fv)) for fv in c['feat']]; n += 1; hit += sc.index(max(sc)) == c['chosen']
        return hit / n if n else None, n
    pa, pn = agree(peng); wa, wn = agree(weng)
    print('%-38s %9s %9s %7s' % ("players' choice rule names the target", '%.2f' % pa if pa else '-', '%.2f' % wa if wa else '-', '%.2f' % (wa / pa) if pa and wa else '-'))
    out.append({'metric': "players' choice rule names the target", 'players': pa, 'walkers': wa})
    if pa and wa: lr.append(abs(math.log(wa / pa)))
score = 100 * math.exp(-sum(lr) / len(lr)) if lr else None
print('fight-likeness: %s over %d measures (players: %d duels, walkers: %d duels)' % ('%.0f%%' % score if score is not None else '-', len(lr), p.get('duels', 0), w.get('duels', 0)))
if '--json' in sys.argv: json.dump({'rows': out, 'score': score}, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
