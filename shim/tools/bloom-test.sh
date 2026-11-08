#!/bin/bash
# Offline test of shim/tools/bloom.py (2026-11-08): a synthetic bullet log with known spread -- an AR whose cone grows with
# sustained fire (1 deg at the first round to 4 deg by the 15th, uniform disc), a BR with a 0.8 deg Gaussian plus a fixed +0.5 deg
# up offset on every third round of a burst -- must give: AR median rising with the string index, BR 3rd-round bias up ~+0.5,
# both with consecutive correlation near 0. PASS / FAIL lines; exit 1 on a failure.
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d "${TMPDIR:-/tmp}/bloom-test.XXXXXX")
python3 - "$T/shots.txt" <<'EOF'
import math, random, sys
rnd = random.Random(5); out = open(sys.argv[1], 'w'); t = 100.0
def shot(t, own, w, ex, ey, yaw=30.0, pitch=2.0):
    yr, pr = math.radians(yaw), math.radians(pitch)
    a = [math.cos(pr) * math.cos(yr), math.cos(pr) * math.sin(yr), math.sin(pr)]
    rt = [math.sin(yr), -math.cos(yr), 0.0]; up = [-math.sin(pr) * math.cos(yr), -math.sin(pr) * math.sin(yr), math.cos(pr)]
    tx, ty = math.tan(math.radians(ex)), math.tan(math.radians(ey)); d = [a[i] + tx * rt[i] + ty * up[i] for i in range(3)]; n = math.sqrt(sum(x * x for x in d)); d = [x / n for x in d]
    L = 30.0; s = [1.0, 2.0, 3.0]; e = [s[i] + L * d[i] for i in range(3)]
    out.write('%.4f %s %s bullet ffffffff %.3f %.3f %.3f %.3f %.3f %.3f %.2f %.5f %.5f %.5f 0 0 0\n' % (t, own, w, *s, *e, L, *a))
for string in range(60):                                      # AR: strings of 15 rounds, 0.066 s apart
    for k in range(15):
        r = (1 + 3 * k / 14) * math.sqrt(rnd.random()); th = rnd.random() * 2 * math.pi
        shot(t, '00010001', 'ASSAULT_RIFLE', r * math.cos(th), r * math.sin(th)); t += 0.066
    t += 1.0
for burst in range(300):                                      # BR: bursts of 3, 0.05 s apart, 0.4 s between bursts
    for k in range(3):
        shot(t, '00020002', 'BATTLE_RIFLE', rnd.gauss(0, 0.8), rnd.gauss(0, 0.8) + (0.5 if k == 2 else 0)); t += 0.05
    t += 0.35
EOF
python3 shim/tools/bloom.py "$T/shots.txt" > "$T/out.txt"; cat "$T/out.txt"
python3 - "$HOME/Library/Application Support/ReclaimerMac/tracks/bloom.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))['weapons']; bad = 0
def chk(ok, what):
    global bad; print(('PASS ' if ok else 'FAIL ') + what); bad += not ok
ar = d['ASSAULT RIFLE']; si = {g['group']: g['median'] for g in ar['by_string_index']}
chk(si.get('1st', 9) < si.get('8-15', 0) < 4, 'AR: spread grows with sustained fire %s' % si)
br = d['BATTLE RIFLE']; bp = {g['group']: g['bias_up'] for g in br['by_burst_position']}
chk(abs(bp.get('3', 0) - 0.5) < 0.2 and abs(bp.get('1', 9)) < 0.15, 'BR: a fixed +0.5 deg up on the 3rd round found %s' % bp)
chk(abs(ar['consecutive_corr_right'] or 1) < 0.15 and abs(br['consecutive_corr_up'] or 1) < 0.3, 'consecutive errors independent (AR %s, BR %s)' % (ar['consecutive_corr_right'], br['consecutive_corr_up']))
chk(ar['all']['ks_uniform_disc'] is not None and br['all']['ks_gaussian'] < 0.1, 'shape: the BR fits a Gaussian (KS %.3f)' % br['all']['ks_gaussian'])
print('bloom test: %s' % ('all PASS' if not bad else '%d FAIL' % bad)); sys.exit(1 if bad else 0)
EOF
r=$?; rm -rf "$T"; rm -f "$HOME/Library/Application Support/ReclaimerMac/tracks/bloom.json"; exit $r
