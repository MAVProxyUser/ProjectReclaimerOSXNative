#!/bin/bash
# Watch real players (2026-11-08; the user asked: "join several games that don't require map downloads, and watch and sample how
# real players move around the map, and skirmish"). Picks a public 0.10 server that needs NO download (no mods), has no password,
# is open and busy (non-Infection, free-for-all / slayer first), joins it with the user's own profile in a background window
# (the 0.10 app lets only a real profile into public servers; nothing is pressed: the player stands where it spawns -- nhook's
# sparring aim / walker refuse public servers by themselves), records it with shim/tools/match-recorder.py for SESSION seconds,
# quits the app and picks again, until TOTAL seconds. Recordings: ~/Library/Application Support/ReclaimerMac/tracks (no names).
#   bash shim/tools/observe-public.sh [TOTAL seconds, default 3600] [SESSION seconds, default 1500]
# Refuses to start beside a running Project Reclaimer (the user's game included); stops at once if one appears that is not its own.
set -u
cd "$(dirname "$0")/../.."
TOTAL=${1:-3600}; SESSION=${2:-1500}; TR="$HOME/Library/Application Support/ReclaimerMac/tracks"; mkdir -p "$TR/logs"
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting"; exit 1; fi
t0=$(date +%s); skip=""
pick() {   # host:port of the best server now ("" = none)
  timeout 60 python3 shim/tools/servers10.py --json 2>/dev/null | python3 -c '
import json, sys
skip = set(sys.argv[1].split())
best = None
for l in sys.stdin:
    try: s = json.loads(l)
    except Exception: continue
    i = s.get("info") or {}; a = s.get("address") or ""
    if not a or a in skip or i.get("password") or i.get("mods") or i.get("joins_closed") or i.get("version") != "0.10.1": continue
    n, mx = i.get("players") or 0, i.get("max_players") or 0
    if n < 4 or n >= mx: continue
    mode = ((i.get("mode") or "") + " " + (i.get("name") or "")).lower()
    if "flood" in mode and "cov" in mode: continue                       # (memory: skip unsc vs flood vs cov)
    score = n + (40 if ("ffa" in mode or "free" in mode or "rumble" in mode) else 0) + (15 if "slayer" in mode else 0) - (60 if "infection" in mode else 0)
    if best is None or score > best[0]: best = (score, a, i.get("map"), i.get("mode"), n)
if best: print(best[1], best[2], "|", best[3], "|", best[4], "players")' "$skip"; }
while [ $(( $(date +%s) - t0 )) -lt "$TOTAL" ]; do
  line=$(pick); addr=${line%% *}
  if [ -z "$addr" ]; then echo "$(date +%H:%M) no suitable server: waiting 5 minutes"; sleep 300; continue; fi
  echo "$(date +%H:%M) joining $line"
  L="$TR/logs/$(date +%Y%m%d-%H%M%S).log"
  NATIVE_REAL=1 NHOOK_STATS_OPEN=0 NATIVE_LOG="$L" bash shim/native.sh --join-server "$addr" --background > "$L.out" 2>&1 &
  for i in $(seq 1 30); do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$L.out" 2>/dev/null); [ -n "$P" ] && break; sleep 1; done
  [ -z "${P:-}" ] && { echo "the app did not start"; sleep 30; continue; }
  left=$(( TOTAL - ($(date +%s) - t0) )); secs=$(( SESSION < left ? SESSION : left ))
  mode=$(echo "$line" | cut -d'|' -f2 | sed 's/^ *//; s/ *$//')
  ts=$(date +%s); python3 shim/tools/match-recorder.py --port 8765 --seconds "$secs" --server "$addr" --mode "$mode" &
  RP=$!
  # the game stays in the server for the whole session: a recorder that stops early is restarted, the app is not rejoined
  while kill -0 "$P" 2>/dev/null && [ $(( $(date +%s) - ts )) -lt "$secs" ]; do sleep 5
    kill -0 $RP 2>/dev/null || { echo "the recorder stopped: restarting it"; sleep 10; python3 shim/tools/match-recorder.py --port 8765 --seconds $(( secs - ($(date +%s) - ts) )) --server "$addr" --mode "$mode" & RP=$!; }
    for q in $(pgrep -f "$APPRE"); do [ "$q" != "$P" ] && { echo "another Project Reclaimer started (pid $q): stopping"; kill $RP; kill "$P"; sleep 3; kill -9 "$P" 2>/dev/null; exit 1; }; done
    grep -aq 'Public join timed out\|join failed\|could not join\|server is full' "$L" 2>/dev/null && { echo "join failed: $addr"; skip="$skip $addr"; break; }; done
  kill $RP 2>/dev/null; wait $RP 2>/dev/null
  kill "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; sleep 2
  grep -a '\[switch\] game Playing' "$L" >/dev/null || { echo "never got into a match on $addr: skipping it"; skip="$skip $addr"; }
  w=$(( 180 - ($(date +%s) - ts) )); [ $w -gt 0 ] && { echo "waiting $w s before the next join"; sleep $w; }   # (joins at least 3 minutes apart)
done
echo "$(date +%H:%M) done; recordings in $TR"
