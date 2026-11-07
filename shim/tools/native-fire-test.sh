#!/bin/bash
# Live check of the native overlay's bullet path (nhook: the object_delete text hook -> tracers, shots / accuracy) in a LOCAL
# test match: no public server, the test profile, a background window. The app's own test request
# {"campaign":{"hold_trigger":{"seconds":N}}} (sent with --request-at; it works in multiplayer too) makes the local player
# fire; the page's capture (do=capture&overlay=1, every 0.3 s while it fires) must show tracer-coloured pixels, the
# [perf] line must count bullets at object_delete, and the snapshot must give the player shots.
#   bash shim/tools/native-fire-test.sh            (needs Pillow + numpy for arch -arm64 python3, like maptop.sh)
# Refuses to run beside another Project Reclaimer (the user's game included). Captures go to ~/reclaimer_cap_<pid>_<n>.bmp
# and are deleted afterwards.
set -u
cd "$(dirname "$0")/../.."
if pgrep -f 'Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-fire.XXXXXX"); LOG="$T/app.log"; PORT=${NHOOK_STATS_HTTP:-8765}
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
NATIVE_LOG="$LOG" NHOOK_HITBOXES=1 NHOOK_HB_TRACERS=1 PELOAD_STATS_OPEN=0 NHOOK_STATS_OPEN=0 NATIVE_SECS=50 \
  bash shim/native.sh --map guardian --seconds 45 --background --request-at 28 '{"campaign":{"hold_trigger":{"seconds":6}}}' > "$T/out" 2>&1 &
NS=$!
for i in $(seq 1 60); do grep -q 'request 2[0-9]*: Campaign(HoldTrigger' "$LOG" 2>/dev/null && break; sleep 0.5; done
for i in $(seq 1 16); do curl -s -m 2 -X POST -H 'X-Reclaimer-Control: 1' "http://127.0.0.1:$PORT/control?do=capture&overlay=1" >/dev/null; sleep 0.3; done
sleep 3; SNAP=$(curl -s -m 3 "http://127.0.0.1:$PORT/stats.json"); wait $NS
PID=$(grep -o 'native app pid [0-9]*' "$T/out" | grep -o '[0-9]*$')
fail=0
B=$(grep '\[perf\]' "$LOG" | grep -o 'bullets [0-9]*' | awk '{s+=$2} END {print s+0}')
if [ "$B" -gt 0 ]; then echo "ok   object_delete hook: $B bullets"; else echo "FAIL no bullets at object_delete"; fail=1; fi
S=$(printf '%s' "$SNAP" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(sum(p.get("shots",0) for p in d.get("players",[])))' 2>/dev/null || echo 0)
if [ "${S:-0}" -gt 0 ]; then echo "ok   snapshot: $S shots"; else echo "FAIL snapshot: no shots"; fail=1; fi
PX=$(arch -arm64 python3 - "$PID" <<'EOF'
import glob, os, sys
import numpy as np
from PIL import Image
n = 0
for f in glob.glob(os.path.expanduser(f'~/reclaimer_cap_{sys.argv[1]}_*.bmp')):
    a = np.asarray(Image.open(f).convert('RGB')).astype(int)
    n += int(((np.abs(a - np.array([255, 242, 140])).sum(-1) < 60) | (np.abs(a - np.array([255, 128, 25])).sum(-1) < 60)).sum())   # noverlay.c ctrace / cvel
print(n)
EOF
)
if [ "${PX:-0}" -gt 0 ]; then echo "ok   captures: $PX tracer / impact pixels"; else echo "FAIL captures: no tracer pixels"; fail=1; fi
[ -n "$PID" ] && rm -f ~/reclaimer_cap_"$PID"_*.bmp ~/reclaimer_cap_"$PID"_*.txt
rm -rf "$T"
[ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
