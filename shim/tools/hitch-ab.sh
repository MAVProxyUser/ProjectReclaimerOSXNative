#!/bin/bash
# Full-screen stutter A/B (2026-11-08; the user: the game "choppy as heck" in full-screen play through nhook, "that we NEVER EVER
# EVER had"): the native app in NORMAL mode (the user's own settings and profile: full screen) joined to one public server,
# once per ARM, one after the other, each for <secs>; the app's own frame log ("[capture] frame N at F fps (frame ms p50 p95
# max)", every ~5 s, printed with or without nhook) is the measure -- shim/tools/hitch-summary.py compares the arms.
#   bash shim/tools/hitch-ab.sh <host:port> <secs> <arm> [<arm> ...]
#   arm = none               the app alone (no DYLD_INSERT_LIBRARIES)
#         nhook              shim/build/nhook.dylib as the observer runs it (NHOOK_TRACKS=0, no stats tab opened)
#         nhook:A=1,B=0      the same with extra environment (NHOOK_VMIC=0, NHOOK_NO_RENDER=1, ...)
# HITCH_SAMPLE=1: a 3 s `sample` of the app once its worst frames pass 80 ms (<log>.sample.txt).
# A full server (its "players" = max_players) is waited for (30 s steps, 20 minutes at most) before each arm. Never beside
# another Project Reclaimer: the script refuses to start while one runs. Logs: ReclaimerMac/tracks/logs/hitch-<stamp>-<n>-<arm>.log
set -u
cd "$(dirname "$0")/../.."
[ $# -ge 3 ] || { sed -n 2,12p "$0"; exit 2; }
ADDR=$1; SECS=$2; shift 2
APP="/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer"
LOGS="$HOME/Library/Application Support/ReclaimerMac/tracks/logs"; mkdir -p "$LOGS"; ST=$(date +%Y%m%d-%H%M%S)
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "hitch-ab: Project Reclaimer is running: not starting"; exit 1; fi
room() {   # 0 = the server has a free slot (or did not answer: try anyway)
  timeout 60 python3 shim/tools/servers10.py --json 2>/dev/null | python3 -c '
import json, sys
a = sys.argv[1]
for l in sys.stdin:
    try: o = json.loads(l)
    except Exception: continue
    if o.get("address") == a:
        i = o.get("info") or {}; print("%s/%s players, %s" % (i.get("players"), i.get("max_players"), i.get("map"))); sys.exit(0 if (i.get("players") or 0) < (i.get("max_players") or 99) else 1)
print("no answer"); sys.exit(0)' "$ADDR"; }
n=0; ALL=()
for arm in "$@"; do n=$((n + 1)); L="$LOGS/hitch-$ST-$n-${arm%%:*}.log"; ALL+=("$L")
  for i in $(seq 1 40); do r=$(room) && break; echo "$(date +%H:%M) $ADDR full ($r): waiting"; sleep 30; done
  echo "$(date +%H:%M) arm $n: $arm on $ADDR ($r) for $SECS s -> $L"
  if [ "$arm" = none ]; then env -u RECLAIMER_PROFILE_DIR -u DYLD_INSERT_LIBRARIES "$APP" --join-server "$ADDR" > "$L" 2>&1 &
  else EXTRA=""; [ "$arm" != "${arm#nhook:}" ] && EXTRA=$(echo "${arm#nhook:}" | tr ',' ' ')
    { echo "[hitch-ab] arm $arm"; } > "$L"
    env -u RECLAIMER_PROFILE_DIR $EXTRA NHOOK_STATS_OPEN=0 NHOOK_TRACKS=0 DYLD_INSERT_LIBRARIES="$PWD/shim/build/nhook.dylib" "$APP" --join-server "$ADDR" >> "$L" 2>&1 & fi
  P=$!
  # HITCH_SAMPLE=1: once the stutter shows (the app's worst frame over 80 ms in 3 of its last 4 frame lines), `sample` the app
  # for 3 s (every thread's stack: where the render thread waits) -> <log>.sample.txt, once per arm
  if [ "${HITCH_SAMPLE:-0}" = 1 ]; then ( while kill -0 $P 2>/dev/null; do sleep 15
      if grep -a '^\[capture\] frame' "$L" | tail -4 | awk '{ m = $NF; sub(/\)/, "", m); if (m + 0 > 80) c++ } END { exit !(c >= 3) }'; then
        echo "$(date +%H:%M) arm $n: the stutter is there: sampling the app for 3 s -> $L.sample.txt"; sample $P 3 -file "$L.sample.txt" >/dev/null 2>&1; break; fi; done ) & fi
  sleep "$SECS"; kill $P 2>/dev/null; for i in 1 2 3 4 5 6; do kill -0 $P 2>/dev/null || break; sleep 1; done; kill -9 $P 2>/dev/null; sleep 3
  python3 shim/tools/hitch-summary.py "$L" | tail -1
done
echo; python3 shim/tools/hitch-summary.py "${ALL[@]}"
