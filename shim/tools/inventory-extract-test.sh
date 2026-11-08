#!/bin/bash
# Offline test of engage-extract.py's inventory records (2026-11-08): a synthetic recording with the camo (17) and other-weapon (18)
# columns -- player 0 switches BR -> shotgun with player 1 six units away while shooting, then runs 3 s under camo firing for the
# last second; an old-format recording (17 columns) must give no inventory records. PASS / FAIL lines; exit 1 on a failure.
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d "${TMPDIR:-/tmp}/inv-test.XXXXXX")
python3 - "$T" <<'EOF'
import gzip, json, sys
d = sys.argv[1]
def rec(name, new):
    with gzip.open('%s/%s.jsonl.gz' % (d, name), 'wt') as f:
        f.write(json.dumps({'start': 1, 'map': 'guardian', 'server': 'test', 'mode': 'Slayer', 'variant': 'Slayer', 'source': 'test'}) + '\n')
        shots = 0
        for k in range(400):
            t = k * 0.1; held, other = ('BATTLE RIFLE', 'SHOTGUN') if k < 100 else ('SHOTGUN', 'BATTLE RIFLE')
            camo = 0.8 if 200 <= k < 230 else 0.0
            if 95 <= k < 105 or 220 <= k < 230: shots += 1
            p0 = [0, 0, 1, 1.0 + 0.01 * k, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, held, '', 0, 0, shots, 0, 2]
            p1 = [1, 1, 1, 7.0 + 0.01 * k, 1.0, 0.0, 180.0, 0.0, 1.0, 1.0, 'ASSAULT RIFLE', '', 0, 0, 0, 0, 2]
            if new: p0 += [camo, other]; p1 += [0.0, '']
            f.write(json.dumps({'t': t, 'you': -1, 'p': [p0, p1], 'proj': 0}) + '\n')
rec('20261108-000000_guardian_new', True); rec('20261108-000001_guardian_old', False)
EOF
python3 shim/tools/engage-extract.py --tracks "$T" --out "$T/eng" --force --quiet > /dev/null
python3 - "$T/eng" <<'EOF'
import glob, gzip, json, sys
R = {p.rsplit('/', 1)[1]: [json.loads(l) for l in gzip.open(p, 'rt')] for p in glob.glob(sys.argv[1] + '/*.eng.jsonl.gz')}
new = R.get('20261108-000000_guardian_new.eng.jsonl.gz', []); old = R.get('20261108-000001_guardian_old.eng.jsonl.gz', [])
ws = [e for e in new if e['kind'] == 'wswitch']; cm = [e for e in new if e['kind'] == 'camo']; bad = 0
def chk(ok, what):
    global bad; print(('PASS ' if ok else 'FAIL ') + what); bad += not ok
chk(len(ws) == 1 and ws[0]['frm_c'] == 'precision' and ws[0]['to_c'] == 'shotgun', 'one switch BR -> shotgun: %s' % [(e['frm'], e['to']) for e in ws])
chk(len(ws) == 1 and ws[0]['dist'] is not None and 5.5 < ws[0]['dist'] < 6.5 and ws[0]['fight'], 'the switch: enemy 6 units away, in a fight: %s' % [(e['dist'], e['fight']) for e in ws])
chk(len(cm) == 1 and 2.5 < cm[0]['dur'] < 3.1, 'one camo stretch of ~3 s: %s' % [e['dur'] for e in cm])
chk(len(cm) == 1 and 0.2 < cm[0]['fired_share'] < 0.5 and cm[0]['first_shot_dist'] is not None, 'camo: fired in its last part, first shot distance known: %s' % [(e['fired_share'], e['first_shot_dist']) for e in cm])
chk(not [e for e in old if e['kind'] in ('wswitch', 'camo')], 'an old recording (17 columns): no inventory records')
print('inventory extract test: %s' % ('all PASS' if not bad else '%d FAIL' % bad)); sys.exit(1 if bad else 0)
EOF
r=$?; rm -rf "$T"; exit $r
