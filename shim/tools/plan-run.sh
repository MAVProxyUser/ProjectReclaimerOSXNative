#!/bin/bash
# The plan, unattended (2026-11-08; the user: "observe 10 games of relative large size first, then observe 4 smaller games, and
# THEN move on to the 1 2 3 plan" + "after the walkers finish the firefight ... start randomly joining and completing games, and
# coaching yourself"): one step after the other, each logged with its time to ReclaimerMac/tracks/plan.txt (the work board's
# Observer panel shows the observer's own lines):
#   sample-large  observe-public.sh: 10 whole matches of 12+ players (OBS_STRICT, no Infection; PLAN_DONE_LARGE = done already)
#   sample-small  observe-public.sh: 4 whole matches of 4-11 players
#   refit         engage-extract -> engage-model (aggregate, per map, tome) -> learnings -> coach
#   spar-ffa      walker-coach.py loop: 4 walkers, free-for-all Slayer on Guardian, 2 coached rounds of 5 minutes (radar log on)
#   spar-teams    walker-coach.py loop: Team Slayer 2v2 on Guardian, 1 coached round
#   firefight     native-firefight.sh: one walker on Courtyard, 5 minutes (walkers together in Firefight: not yet -- co-op joins)
#   learn         learn-loop.py: random public games, each reviewed, refit, walkers coached on its map (PLAN_TURNS, default 6)
#   bash shim/tools/plan-run.sh [first step, default sample-large]
# Click the observer's window to play its game yourself (controller included): the visit lasts until you quit it, then the plan
# goes on. Every step refuses to start beside a Project Reclaimer that is not its own (the user's game included).
set -u
cd "$(dirname "$0")/../.."
TR="$HOME/Library/Application Support/ReclaimerMac/tracks"; LOG="$TR/plan.txt"; mkdir -p "$TR"
say() { echo "$(date +%H:%M) $*" | tee -a "$LOG"; }
STEPS="sample-large sample-small refit spar-ffa spar-teams firefight learn"; FROM=${1:-sample-large}; on=0
for s in $STEPS; do
  [ "$s" = "$FROM" ] && on=1; [ $on = 1 ] || continue
  say "step $s: starting"
  case $s in
    sample-large) OBS_MATCHES=10 OBS_MIN_PLAYERS=12 OBS_STRICT=1 OBS_DONE=${PLAN_DONE_LARGE:-0} bash shim/tools/observe-public.sh 28800 2700 >> "$TR/observe-large.txt" 2>&1;;
    sample-small) OBS_MATCHES=4 OBS_MIN_PLAYERS=4 OBS_MAX_PLAYERS=11 OBS_STRICT=1 bash shim/tools/observe-public.sh 14400 2700 >> "$TR/observe-small.txt" 2>&1;;
    refit) python3 shim/tools/engage-extract.py --quiet | tail -1 | tee -a "$LOG"; python3 shim/tools/engage-model.py > /dev/null 2>&1
           python3 shim/tools/learnings.py --quiet; python3 shim/tools/coach.py --quiet | tail -1 | tee -a "$LOG";;
    spar-ffa) NHOOK_RADAR_LOG=1 python3 shim/tools/walker-coach.py loop --rounds 2 --secs 300 --n 4 --map guardian 2>&1 | tee -a "$LOG" | grep -a 'round\|walker\|->' | tail -20;;
    spar-teams) python3 shim/tools/walker-coach.py loop --rounds 1 --secs 300 --n 4 --map guardian --teams 2>&1 | tee -a "$LOG" | grep -a 'round\|walker\|->' | tail -12;;
    firefight) bash shim/tools/native-firefight.sh ff20_courtyard 300 2>&1 | tee -a "$LOG" | tail -8;;
    learn) python3 shim/tools/learn-loop.py --turns "${PLAN_TURNS:-6}" 2>&1 | tee -a "$LOG";;
  esac
  say "step $s: done"
done
say "plan done"
