#!/bin/bash
# Coach the walkers between rounds (2026-11-08; the user: "Can the coach process at the end of every round, and restart the walkers
# with the new training?", "pay attention to which walker is doing the best too, and train the other walkers to be more like the good
# walker", "let the walkers train each other for a while"). Beside a running battle on our server (walker-battle.sh / play-vs-walkers.sh
# / the walkers' games started by hand): a round ends when the battle's recorder starts a NEW file (battles/tracks/*_local.jsonl.gz:
# one per match -- the server's "in a match" flag can stay on through a short lobby, the first version missed a round that way).
# For the round that ended:
#   1. walker-coach.py round on THAT recording alone (a folder with just it: the battle folder holds older battles whose slots meant
#      other players) -- each walker's measures vs the players' winners -> knob steps (battles/coach/state.json); slots -> walkers
#      from the server's players list, written here every poll (battles/coach-rounds-samples.jsonl);
#   2. the round's BEST walker by its final score ((kills + 1) / (deaths + 1), walker-scores.py --recording) pulls the others
#      FOLLOW_W (0.5) of the way toward its knobs (walker-coach.py follow);
#   3. match-study.py on the round (the human's slot when one plays: fights, replies, standing still, levels apart);
#   4. every walker's game restarted headless -- same identity and stats port -- with its new knobs and the newest build.
#   bash shim/tools/coach-rounds.sh <walkers' logs dir> [walker ids, default "1 2 3"] [rounds, default 20]
# COACH_LAST=<recording> coaches that finished round at once. Log: battles/coach-rounds.txt.
set -u
cd "$(dirname "$0")/../.."
T=$1; IDS=${2:-1 2 3}; ROUNDS=${3:-20}
SUP="$HOME/Library/Application Support/ReclaimerMac/native"; BT="$HOME/Library/Application Support/ReclaimerMac/battles"; LOG="$BT/coach-rounds.txt"
SMP="$BT/coach-rounds-samples.jsonl"
NAMES=(test-v2 test-second test-third test-fourth test-fifth); PORTS=(8765 8766 8768 8769 8770)
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; }
newest() { ls -t "$BT/tracks/"*_local.jsonl.gz 2>/dev/null | head -1; }
poll() {   # the server's players (slot, name) -> the samples file (one line), the human's slot (a name that is no walker's)
  curl -s -m 3 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys
try: d = json.load(sys.stdin)
except Exception: print("-1"); sys.exit()
P = [{"slot": p.get("slot"), "name": p.get("name")} for p in d.get("players", [])]
open(sys.argv[1], "w").write(json.dumps({"players": P}) + "\n")
h = [p["slot"] for p in P if p["name"] not in ("Second", "Third", "Fourth", "Fifth")]
print(h[0] if h else "")' "$SMP"; }
coach() {   # $1 = the finished round's recording
  local R=$1 D; D=$(mktemp -d "${TMPDIR:-/tmp}/coach-round.XXXXXX"); ln -s "$R" "$D/$(basename "$R")"
  say "round over ($(basename "$R")): coaching"
  python3 shim/tools/walker-coach.py round "$D" "$SMP" 2>&1 | grep -a 'walker\|->\|round' | tail -10 | tee -a "$LOG"
  python3 shim/tools/walker-scores.py --recording "$R" "$SMP" "$IDS" > "$D/scores"; say "scores: $(tr '\n' ';' < "$D/scores")"
  B=$(python3 shim/tools/walker-scores.py --best "$D/scores"); [ -n "$B" ] && python3 shim/tools/walker-coach.py follow "$B" $IDS --w "${FOLLOW_W:-0.5}" 2>&1 | tee -a "$LOG"
  if [ -n "${HS:-}" ]; then python3 shim/tools/match-study.py "$R" --human "$HS" 2>&1 | sed -n '2,12p;/levels apart/,/melee-range/p' | grep -v '^ *[0-9.]* s ' | tee -a "$LOG"
  else python3 shim/tools/match-study.py "$R" 2>&1 | sed -n '2,12p' | grep -v '^ *[0-9.]* s ' | tee -a "$LOG"; fi
  rm -rf "$D"; }
restart() {
  for k in $IDS; do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/w$k.out" 2>/dev/null); [ -n "$P" ] && kill "$P" 2>/dev/null; done
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
  say "walkers restarted with their new training"; }
r=0; CUR=$(newest); HS=$(poll); [ "$HS" = -1 ] && { say "our server does not answer on 8767"; exit 1; }
say "watching the rounds: walkers $IDS, the round being recorded $(basename "$CUR"), a human at slot ${HS:-none}"
if [ -n "${COACH_LAST:-}" ]; then coach "$COACH_LAST"; restart; r=1; fi
while [ $r -lt "$ROUNDS" ]; do
  sleep 5
  H=$(poll); [ "$H" = -1 ] && { say "the server is gone: stopping"; break; }; HS=$H
  N=$(newest)
  if [ -n "$N" ] && [ "$N" != "$CUR" ]; then r=$((r + 1)); coach "$CUR"; restart; CUR=$N; fi
done
say "coach-rounds done ($r rounds)"
