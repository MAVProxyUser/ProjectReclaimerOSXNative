#!/bin/bash
# The disclosed walker (AIWalkerTest) on ONE owner-approved public server, patiently (2026-11-09; the user: "if we lost the slot...
# patiently wait to rejoin. that is ok"). Its own home (ReclaimerMac/native/public-walker/home: a regular profile, a copy of a test
# identity named AIWalkerTest -- never the user's), NHOOK_BOT_ALLOW_SERVER = that address only, its console chat lines off. With
# --follow it shadows the player whose profile this Mac holds (the nickname is read here at launch, never written anywhere).
# Loop: launch; in the game within 180 s (its stats page lists it) -> stay while it is; else / once out -> stop it, wait 30 s, again.
#   bash shim/tools/walker-public.sh <host:port> [--follow] [--id 4]      log: battles/aiwalker.log, stats http://127.0.0.1:8771/
set -u
cd "$(dirname "$0")/../.."
ADDR=$1; shift; FOLLOW=0; WID=4
while [ $# -gt 0 ]; do case "$1" in --follow) FOLLOW=1;; --id) WID=$2; shift;; esac; shift; done
S="$HOME/Library/Application Support/ReclaimerMac"; FH="$S/native/public-walker/home"; PORT=8771
[ -f "$FH/Library/Application Support/ProjectReclaimer/profile.json" ] || { echo "no walker home at $FH"; exit 1; }
FN=""; [ $FOLLOW = 1 ] && FN=$(python3 -c 'import json,os; print(json.load(open(os.path.expanduser("~/Library/Application Support/ProjectReclaimer/profile.json")))["identity"]["nickname"])')
ingame() { curl -s -m 3 "http://127.0.0.1:$PORT/stats.json" | python3 -c 'import json,sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(1)
sys.exit(0 if any(p.get("you") for p in d.get("players", [])) else 1)'; }
apps() { pgrep -f "^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer --join-server $ADDR --headless"; }
while true; do
  WE=$(python3 shim/tools/walker-coach.py show 2>/dev/null | sed -n "s/^walker $WID: //p")
  env $WE HOME="$FH" NATIVE_REAL=1 NATIVE_ALLOW_SECOND=1 NATIVE_LOG="$S/battles/aiwalker.log" NHOOK_STATS_HTTP=$PORT NHOOK_STATS_OPEN=0 NHOOK_MUTE_OUTPUT=1 \
    PELOAD_FORENSIC_DIR="$S/native/public-walker/forensics" NHOOK_HEATMAP_DIR="$S/native/public-walker/heatmaps" \
    NHOOK_VPAD=1 NHOOK_SPAR=1 NHOOK_NAV=enemy NHOOK_BOT_FIRE=1 NHOOK_BOT_FIGHT=1 NHOOK_SPAR_LOG=1 PELOAD_NAV_LOG=1 \
    NHOOK_BOT_ALLOW_SERVER="$ADDR" NHOOK_BOT_CAMPCHAT=0 ${FN:+NHOOK_BOT_FOLLOW="$FN"} \
    bash shim/native.sh --join-server "$ADDR" --headless &
  L=$!; ok=0
  for i in $(seq 1 90); do sleep 2; ingame && { ok=1; break; }; kill -0 $L 2>/dev/null || break; done
  if [ $ok = 1 ]; then echo "$(date +%H:%M:%S) in the game"; miss=0
    while kill -0 $L 2>/dev/null; do sleep 10; if ingame; then miss=0; else miss=$((miss + 1)); [ $miss -ge 6 ] && break; fi; done   # (a minute out of it: rejoin)
    echo "$(date +%H:%M:%S) out of the game"
  else echo "$(date +%H:%M:%S) not in within 180 s (the server full?): trying again in 30 s"; fi
  for p in $(apps); do kill "$p"; done; sleep 4; for p in $(apps); do kill -9 "$p" 2>/dev/null; done; wait $L 2>/dev/null
  # (2026-11-09 05:42: the walker was KICKED -- "You were kicked from this server. You can rejoin in 9 minutes." -- any player can boot it
  #  and that is the signal that someone has an issue: the loop stops instead of rejoining after the cooldown)
  if tail -n 400 "$S/battles/aiwalker.log" 2>/dev/null | grep -a -q 'You were kicked from this server'; then
    echo "$(date +%H:%M:%S) the walker was kicked from $ADDR: not rejoining (stopped)"; exit 0; fi
  sleep 30
done
