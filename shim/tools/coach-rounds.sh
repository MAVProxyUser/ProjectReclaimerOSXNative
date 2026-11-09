#!/bin/bash
# Coach the walkers between rounds while the user plays (2026-11-08; the user: "Can the coach process at the end of every round, and
# restart the walkers with the new training?"). Beside a running walker-battle.sh / play-vs-walkers.sh: it watches our server's
# match state (stats on 8767); when a round ends (in a match -> not), it
#   1. coaches every walker on that round: walker-coach.py round (each walker's measures vs the players' winners -> knob steps,
#      battles/coach/state.json) and match-study.py (the round against the human: fights, replies, standing still, levels apart),
#   2. restarts each walker's game -- headless, the same identity and stats port -- with its new knobs and the newest build
#      (shim/build/nhook.dylib: code changes made meanwhile come in too); they rejoin in the lobby. The user's game and the server
#      keep running.
#   bash shim/tools/coach-rounds.sh <walker-battle logs dir> <battle samples .jsonl> [walker ids, default "1 2 3"] [rounds, default 20]
# The logs dir and samples file are walker-battle.sh's ("battle: ...; logs <dir>; samples -> <file>"). Log: battles/coach-rounds.txt.
set -u
cd "$(dirname "$0")/../.."
T=$1; OUT=$2; IDS=${3:-1 2 3}; ROUNDS=${4:-20}
SUP="$HOME/Library/Application Support/ReclaimerMac/native"; BT="$HOME/Library/Application Support/ReclaimerMac/battles"; LOG="$BT/coach-rounds.txt"
NAMES=(test-v2 test-second test-third test-fourth); PORTS=(8765 8766 8768 8769)
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; }
inm() { curl -s -m 3 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys
try: print(1 if json.load(sys.stdin).get("in_match") else 0)
except Exception: print(-1)'; }
human_slot() { for port in 8765; do curl -s -m 3 http://127.0.0.1:$port/stats.json | python3 -c 'import json,sys
try: print(next((p["slot"] for p in json.load(sys.stdin).get("players", []) if p.get("you")), ""))
except Exception: print("")'; done; }
r=0; was=$(inm); HS=$(human_slot)
say "watching the rounds (in a match: $was); walkers $IDS; you are slot ${HS:-?}"
while [ $r -lt "$ROUNDS" ]; do
  now=$(inm); [ "$now" = 1 ] && { HS2=$(human_slot); [ -n "$HS2" ] && HS=$HS2; }
  if [ "$now" = -1 ]; then say "the server is gone: stopping"; break; fi
  if [ "$was" = 1 ] && [ "$now" = 0 ] || [ $r = 0 ] && [ "$now" = 0 ] && [ "${COACH_NOW:-1}" = 1 ]; then
    r=$((r + 1)); COACH_NOW=0; sleep 3
    REC=$(ls -t "$BT/tracks/"*_local.jsonl.gz | head -1)
    say "round $r over ($(basename "$REC")): coaching"
    python3 shim/tools/walker-coach.py round "$BT/tracks" "$OUT" 2>&1 | grep -a 'walker\|->\|round' | tail -8 | tee -a "$LOG"
    [ -n "$HS" ] && python3 shim/tools/match-study.py "$REC" --human "$HS" 2>&1 | sed -n '2,10p;/levels apart/,/melee-range/p' | grep -v '^ *[0-9.]* s ' | tee -a "$LOG"
    # restart the walkers with their new knobs (and the newest build)
    for k in $IDS; do
      P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/w$k.out" 2>/dev/null); [ -n "$P" ] && kill "$P" 2>/dev/null; done
    sleep 4
    for k in $IDS; do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/w$k.out" 2>/dev/null); [ -n "$P" ] && kill -9 "$P" 2>/dev/null; done
    for k in $IDS; do d="$SUP/${NAMES[$k]}"
      WE=$(python3 shim/tools/walker-coach.py show 2>/dev/null | sed -n "s/^walker $k: //p")
      say "walker $k: ${WE:-the defaults}"
      env $WE NATIVE_LOG="$T/w$k.log" NATIVE_PROFILE="$d" RECLAIMER_CACHE_DIR="$d-cache" PELOAD_FORENSIC_DIR="$d-forensics" NHOOK_HEATMAP_DIR="$d-heatmaps" \
        NHOOK_STATS_HTTP="${PORTS[$k]}" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_MUTE_OUTPUT=1 \
        NHOOK_VPAD=1 NHOOK_SPAR=1 NHOOK_NAV=enemy NHOOK_BOT_FIRE=1 NHOOK_BOT_FIGHT=1 NHOOK_SPAR_LOG=1 PELOAD_NAV_LOG=1 \
        bash shim/native.sh --join-server 127.0.0.1:49190 --headless > "$T/w$k.out" 2>&1 &
      sleep 5; done
    say "round $r: walkers restarted with their new training"
  fi
  was=$now; sleep 3
done
say "coach-rounds done"
