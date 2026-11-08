#!/bin/bash
# The hooks check as a self test (2026-11-08; the user: "can you add a hooks check? ... detect IF on a new version the hook
# loaded properly or function. that could be an interesting self test"). Run it after every app update (and after porting
# nhook to it with re-tools/machoport.py): one LOCAL test match (the test profile, a background window, no server) that uses
# everything a local game can use -- the player fires (the app's hold_trigger test request), the overlay is on (hitboxes +
# tracers: the camera hook), the walker roams (the player-control hook), the host tools run (boundaries off / on, invisible
# walls off / on, a teleport: the tick / frame / kill-volume / soft-ceiling / shape-builder hooks) -- then reads GET /hooks.json
# (shim/native/nhchk.c) and judges every item:
#   FAIL  anything FAILED (not installed / not found in this app build), or an item this match must exercise that never fired
#   ok    the rest; items a local match cannot exercise (voice, chat, online lines, the mic) are listed as "not exercised here"
#   bash shim/tools/native-hooks-test.sh [--keep]     (--keep: the logs and /hooks.json stay in the temp folder)
# Refuses to run beside another Project Reclaimer (the user's game included).
set -u
cd "$(dirname "$0")/../.."
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-hooks.XXXXXX"); LOG="$T/app.log"; PORT=8765
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
post() { curl -s -m 3 -X POST -H 'X-Reclaimer-Control: 1' "http://127.0.0.1:$PORT/control?$1"; echo; }
NATIVE_LOG="$LOG" NHOOK_STATS_OPEN=0 NHOOK_HITBOXES=1 NHOOK_HB_TRACERS=1 NHOOK_NAV=roam NATIVE_SECS=125 \
  bash shim/native.sh --map guardian --seconds 120 --background \
  --request-at 35 '{"campaign":{"hold_trigger":{"seconds":5}}}' --request-at 70 '{"campaign":{"hold_trigger":{"seconds":5}}}' > "$T/out" 2>&1 &
NS=$!
for i in $(seq 1 120); do grep -aq '\[switch\] game Playing' "$LOG" 2>/dev/null && break; sleep 1; done
grep -aq '\[switch\] game Playing' "$LOG" || { echo "FAIL the test match never started (log: $LOG)"; wait $NS; exit 1; }
echo "match started; using the host tools"
sleep 20
{ post 'do=bounds&off=1'; sleep 2; post 'do=bounds&off=0'; post 'do=invwalls&off=1'; sleep 3; post 'do=invwalls&off=0'; post 'do=tpby&dz=0.5'; sleep 2; } > "$T/controls.txt" 2>&1
sleep 50
curl -s -m 5 "http://127.0.0.1:$PORT/hooks.json" > "$T/hooks.json"
wait $NS
python3 - "$T/hooks.json" <<'EOF'
import json, sys
try: j = json.load(open(sys.argv[1]))
except Exception as e: print('FAIL no /hooks.json (%s)' % e); sys.exit(1)
# what this local match exercises: these must have fired (the rest is judged only by FAILED)
MUST = {'engine_clock', 'object_delete', 'view constants', 'player control (sparring aim / walker)', 'kill volume test', 'soft ceiling test',
        'shape builder (invisible walls)', 'log tap (write)', 'presentDrawable', 'depth attachment', 'line: [switch] game', 'line: [loading] map',
        'line: [capacity] statborg', 'line: [c1] game results', 'players table', 'statborg', 'game results', 'halo3.dll image check'}
MUST_ONE = [('game tick (host tools)', 'frame (host tools)')]   # the teleport runs from one of them (both must INSTALL: FAILED fails)
NOT_HERE = {'opus decode (voice)', 'voice mixer', 'line: [voice] active', 'line: [chat] line', 'line: [online] joining', 'line: [online] peer',
            'line: [engine] HUD notice', 'AudioUnitRender (mic)', 'network session', 'display_event', 'sockets (send / receive)',
            'line: [menus] open'}   # (a --map launch goes straight into the match: no menus)
a = j['app']; print('app %s (UUID %s)%s' % (a['version'], a['uuid'], '' if a['known'] else ' -- NOT in the address table'))
bad = 0; by = {x['name']: x for x in j['items']}
for x in j['items']:
    n, v = x['name'], x['verdict']; fired = x['fires'] > 0
    if v == 'FAILED': res = 'FAIL'; bad += 1
    elif n in MUST and not fired: res = 'FAIL'; bad += 1; v = 'never fired (this match uses it)'
    elif n in NOT_HERE and not fired: res = '-   '; v = 'not exercised here (%s)' % x['when']
    else: res = 'ok  '
    print('%s %-40s %-12s %-30s fired %-8d %s' % (res, n, x['kind'], v, x['fires'], x['why'][:110]))
for grp in MUST_ONE:
    if not any(by.get(n, {}).get('fires', 0) for n in grp): print('FAIL none of %s fired (the teleport)' % ', '.join(grp)); bad += 1
print('summary:', j['summary'])
print('PASS' if not bad and a['known'] else 'FAIL (%d)' % bad); sys.exit(0 if not bad and a['known'] else 1)
EOF
rc=$?
grep -a '^\[hooks\]' "$LOG" | tail -12
if [ "${1:-}" = --keep ] || [ $rc != 0 ]; then echo "logs: $T"; else rm -rf "$T"; fi
exit $rc
