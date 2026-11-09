#!/bin/bash
# Offline test of shim/tools/melee-model.py (2026-11-08): a synthetic recording where player 1 closes in on player 2 from 3 units
# at 1.5 units/s and kills it with "Melee" at 0.8 units, its shield down 0.5 s before (a beatdown), 40 times, plus 10 full-shield
# "Melee" kills at 1.0 unit and 10 "Gravity hammer" kills at 1.2; must give melee median 0.8 / p90 1.0, beatdown share 0.8, the
# hammer at 1.2, and the walker's numbers in melee.json. PASS / FAIL lines; exit 1 on a failure.
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d "${TMPDIR:-/tmp}/melee-test.XXXXXX")
python3 - "$T" <<'PY'
import gzip, json, sys
out = gzip.open(sys.argv[1] + '/20261108-000000_guardian_test.jsonl.gz', 'wt')
out.write(json.dumps({'start': 0, 'map': 'guardian', 'mode': 'Slayer', 'engine': 'Slayer', 'variant': ''}) + '\n')
t = 0.0; ev = []
def row(slot, x, sh, w='ASSAULT RIFLE'): return [slot, slot, 1, x, 0.0, 1.0, 0, 0, sh, 1.0, w, '', 0, 0, 0, 0, 2, 0, '']
for k in range(60):
    d_end, kind, w = (0.8, 'Melee', 'ASSAULT RIFLE') if k < 40 else (1.0, 'Melee', 'BATTLE RIFLE') if k < 50 else (1.2, 'Gravity hammer', 'GRAVITY HAMMER')
    beat = k < 40
    for i in range(30):   # 3 s at 10 Hz: the killer closes in at 1.5 units/s
        d = d_end + 1.5 * (2.9 - i * 0.1); sh = 0.0 if (beat and i >= 20) else 1.0
        out.write(json.dumps({'t': round(t, 2), 'p': [row(1, 0.0, 1.0, w), row(2, d, sh)]}) + '\n'); t += 0.1
    out.write(json.dumps({'t': round(t - 0.05, 2), 'ev': 'kill', 'a': 1, 'b': 2, 'how': kind, 'est': False, 'verb': 'killed', 'pos': [d_end, 0, 1]}) + '\n')
    for i in range(30): out.write(json.dumps({'t': round(t, 2), 'p': [row(1, 0.0, 1.0, w), [2, 2, 0, 0, 0, 0, 0, 0, 0, 0, '', '', 0, 0, 0, 0, 0, 0, '']]}) + '\n'); t += 0.1
out.close()
PY
python3 shim/tools/melee-model.py --dir "$T" | tail -6
python3 - "$T/melee.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); bad = 0
def chk(ok, what):
    global bad; print(('PASS ' if ok else 'FAIL ') + what); bad += not ok
m = d['types']['melee']; h = d['types'].get('hammer', {})
chk(abs(m['distance']['p50'] - 0.8) < 0.05 and abs(m['distance']['p90'] - 1.0) < 0.05, 'melee distance median %s (0.8), p90 %s (1.0)' % (m['distance']['p50'], m['distance']['p90']))
chk(abs(m['beatdown_share'] - 0.8) < 0.03, 'beatdown share %s (40 of 50)' % m['beatdown_share'])
chk(abs(m['closing_1s'] - 1.5) < 0.2, 'closing %s units in the last second (1.5 units/s)' % m['closing_1s'])
chk(h and abs(h['distance']['p50'] - 1.2) < 0.11, 'hammer at %s' % (h and h['distance']['p50']))
w = d.get('walker', {}); chk(w.get('melee_range') and w.get('rush_from') and w.get('hammer_range'), 'walker numbers %s' % w)
print('melee model test: %s' % ('all PASS' if not bad else '%d FAIL' % bad)); sys.exit(1 if bad else 0)
PY
r=$?; rm -rf "$T"; exit $r
