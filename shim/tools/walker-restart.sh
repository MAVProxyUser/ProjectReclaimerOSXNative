#!/bin/bash
# Restart walkers ONE AT A TIME on the newest build (2026-11-09): a walker that leaves and rejoins goes to the smaller team -- the one it
# left -- so the sides stay as they are (our server's RCON `team` answers "This game has no teams." in these playlists; coach-rounds.sh
# restarts all of them at once and the auto-balance deals the teams anew). Same identities, stats ports and coached knobs as
# coach-rounds.sh's restart; each walker is back in the match (in the server's players list) before the next goes.
#   bash shim/tools/walker-restart.sh <walkers' logs dir> [walker ids, default "1 2 3"]
# WR_ENV="NHOOK_BOT_X=.. .." = knobs after the coached ones (they win: walker-epochs.py's variants)
set -u
cd "$(dirname "$0")/../.."
T=$1; IDS=${2:-1 2 3}
SUP="$HOME/Library/Application Support/ReclaimerMac/native"
NAMES=(test-v2 test-second test-third test-fourth test-fifth); NICKS=(- Second Third Fourth Fifth); PORTS=(8765 8766 8768 8769 8770)
inmatch() { curl -s -m 3 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(1)
sys.exit(0 if any(p.get("name") == sys.argv[1] for p in d.get("players", [])) else 1)' "$1"; }
for k in $IDS; do
  P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/w$k.out" 2>/dev/null)
  if [ -n "$P" ] && ps -p "$P" -o command= | grep -q -- '--headless'; then kill "$P"; sleep 4; ps -p "$P" >/dev/null 2>&1 && kill -9 "$P"; fi
  for i in $(seq 1 15); do inmatch "${NICKS[$k]}" || break; sleep 1; done   # (gone from the server's list)
  d="$SUP/${NAMES[$k]}"; WE=$(python3 shim/tools/walker-coach.py show 2>/dev/null | sed -n "s/^walker $k: //p")
  env $WE ${WR_ENV:-} NATIVE_LOG="$T/w$k.log" NATIVE_PROFILE="$d" RECLAIMER_CACHE_DIR="$d-cache" PELOAD_FORENSIC_DIR="$d-forensics" NHOOK_HEATMAP_DIR="$d-heatmaps" \
    NHOOK_STATS_HTTP="${PORTS[$k]}" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_MUTE_OUTPUT=1 \
    NHOOK_VPAD=1 NHOOK_SPAR=1 NHOOK_NAV=enemy NHOOK_BOT_FIRE=1 NHOOK_BOT_FIGHT=1 NHOOK_SPAR_LOG=1 PELOAD_NAV_LOG=1 \
    bash shim/native.sh --join-server 127.0.0.1:49190 --headless > "$T/w$k.out" 2>&1 &
  for i in $(seq 1 60); do inmatch "${NICKS[$k]}" && break; sleep 1; done
  echo "$(date +%H:%M:%S) ${NICKS[$k]} back in the match"
done
wait   # (the walkers stay children of this run: a finished run must not take them along)
