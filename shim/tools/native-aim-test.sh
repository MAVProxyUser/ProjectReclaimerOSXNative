#!/bin/bash
# Live check of the aim meter and hits (nhook: noverlay.c nov_aim_shot / nov_aim_tick, nstats.c st_aim_tick) in a LOCAL test
# match: no public server, the test profile, a background window. The app's own test requests (--request-at) put a biped
# 5 units in front of the local player ({"campaign":{"target_practice":...}}) and hold the trigger at it
# ({"campaign":{"hold_trigger":...}}); the snapshot must then show aimed shots measured against the target's hitboxes, some
# of them through a hitbox, and hits > 0 (a hit = a shot through an enemy's hitbox).
#   bash shim/tools/native-aim-test.sh
# Refuses to run beside another Project Reclaimer (the user's game included).
set -u
cd "$(dirname "$0")/../.."
if pgrep -f 'Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-aim.XXXXXX"); LOG="$T/app.log"; PORT=${NHOOK_STATS_HTTP:-8765}
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
TP='{"campaign":{"target_practice":{"tag":"objects\\characters\\masterchief\\mp_masterchief\\mp_masterchief","player":0,"distance":5,"span":0,"speed":0,"seconds":25}}}'
NATIVE_LOG="$LOG" PELOAD_STATS_OPEN=0 NHOOK_STATS_OPEN=0 NATIVE_SECS=55 \
  bash shim/native.sh --map guardian --seconds 50 --background --request-at 24 "$TP" --request-at 28 '{"campaign":{"hold_trigger":{"seconds":6}}}' > "$T/out" 2>&1 &
NS=$!
for i in $(seq 1 90); do grep -q 'HoldTrigger' "$LOG" 2>/dev/null && break; sleep 0.5; done
sleep 9; SNAP=$(curl -s -m 3 "http://127.0.0.1:$PORT/stats.json"); wait $NS
fail=0
printf '%s' "$SNAP" | python3 -c '
import json, sys
d = json.load(sys.stdin); ps = [p for p in d.get("players", []) if p.get("you") or p.get("shots")]
p = max(ps, key=lambda p: p.get("shots", 0)) if ps else {}
a = p.get("aim") or {}; print("shots %s, hits %s, aim %s" % (p.get("shots"), p.get("hits"), json.dumps(a)))
ok = a.get("n", 0) > 0 and a.get("on", 0) > 0 and p.get("hits", 0) > 0
print("ok   aimed shots measured, some through the hitbox, hits counted" if ok else "FAIL no aimed shot through the target")
sys.exit(0 if ok else 1)' || fail=1
grep -E '\[perf\]' "$LOG" | tail -1 | grep -o 'aimed [0-9]* / measured [0-9]*'
rm -rf "$T"
[ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
