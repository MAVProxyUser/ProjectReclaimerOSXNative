#!/usr/bin/env python3
"""How human are the walkers? (2026-11-08) The same measurements (shim/tools/walker-model.py) over the real players' recordings
(~/Library/Application Support/ReclaimerMac/tracks) and the walkers' battles (.../battles/tracks), side by side, with the ratio.
    python3 shim/tools/walker-compare.py [--json out.json]"""
import json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def model(d):
    out = tempfile.mktemp(suffix='.json')
    subprocess.run([sys.executable, os.path.join(HERE, 'walker-model.py'), d, '--out', out, '--heatmaps', '/nonexistent'], capture_output=True, text=True)
    try: return json.load(open(out))
    except Exception: return None
real, bots = model(os.path.join(SUP, 'tracks')), model(os.path.join(SUP, 'battles', 'tracks'))
if not real or not bots: print('missing recordings (real: %s, walkers: %s)' % (bool(real), bool(bots))); sys.exit(1)
rows = [('move', 'speed_median', 'running speed (units/s)'), ('move', 'stand_share', 'share of time standing'),
        ('fight', 'dist_median', 'fighting distance, median'), ('fight', 'dist_p25', 'fighting distance, 25%'), ('fight', 'dist_p75', 'fighting distance, 75%'),
        ('fight', 'strafe_rms', 'strafe speed RMS (units/s)'), ('fight', 'strafe_switch_s', 'strafe switch (s)'),
        ('fight', 'close_high_shield', 'closing speed, shields up'), ('fight', 'close_low_shield', 'closing speed, shields low'),
        ('fight', 'aim_err_median', 'aim error, median (deg)'), ('fight', 'aim_err_p75', 'aim error, 75% (deg)'),
        ('fight', 'reaction_s', 'reaction (s)'), ('fight', 'shots_per_s', 'shots a second in fights'), ('fight', 'hit_share', 'share of shots through a hitbox'),
        ('fight', 'jumps_per_min', 'jumps a minute in fights')]
print('%-34s %10s %10s %7s' % ('', 'players', 'walkers', 'ratio'))
out = []
for sec, k, label in rows:
    a, b = (real.get(sec) or {}).get(k), (bots.get(sec) or {}).get(k)
    r = (b / a) if (a not in (None, 0) and b is not None) else None
    print('%-34s %10s %10s %7s' % (label, '%.2f' % a if a is not None else '-', '%.2f' % b if b is not None else '-', '%.2f' % r if r is not None else '-'))
    out.append({'metric': label, 'players': a, 'walkers': b})
print('sources: players %s; walkers %s' % (real['source'], bots['source']))
if '--json' in sys.argv: json.dump({'rows': out, 'players_source': real['source'], 'walkers_source': bots['source']}, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
