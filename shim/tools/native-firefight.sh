#!/bin/bash
# A walker in a LOCAL Firefight game (2026-11-08; the user: "start testing the walker in local firefight games" -- once the players'
# fights are learned). The 0.10 app plays Halo: Reach's Firefight maps from Reach's own files (`--reach-firefight <map>`; ODST's
# are not installed here): one test identity (test-v2), the fighter + the goal draw + the radar (nbot.c / nbot_engage.c /
# nbot_goals.c) against the AI waves, sound muted, a background window, its stats page on 8765 (NHOOK_STATS_OPEN=1 opens it).
# The walker's view of things goes to the log: [fight] / [goal] / [radar] lines (NHOOK_SPAR_LOG, NHOOK_RADAR_LOG), the app's own
# [firefight] lines (waves, deaths, scoring).
#   bash shim/tools/native-firefight.sh [map, default ff20_courtyard] [seconds, default 300] [extra env...]
#   maps: ff10_prototype ff20_courtyard ff30_waterfront ff45_corvette ff50_park ff60_airview ff60_icecave ff70_holdout ff_unearthed
#   FF_VARIANT=<name> = --reach-firefight-variant (the app's names: firefight_new, firefight_classic, firefight_gen, ...)
# Refuses to start beside another Project Reclaimer (the user's game included). Log: ReclaimerMac/native/firefight-<time>.log
set -u
cd "$(dirname "$0")/../.."
MAP=${1:-ff20_courtyard}; SECS=${2:-300}; shift 2 2>/dev/null; EXTRA=("$@")
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting"; exit 1; fi
SUP="$HOME/Library/Application Support/ReclaimerMac/native"; L="$SUP/firefight-$(date +%Y%m%d-%H%M%S).log"
ARGS=(--reach-firefight "$MAP" --background); [ -n "${FF_VARIANT:-}" ] && ARGS+=(--reach-firefight-variant "$FF_VARIANT")
env NATIVE_LOG="$L" NHOOK_STATS_OPEN="${NHOOK_STATS_OPEN:-0}" NHOOK_MUTE_OUTPUT=1 NHOOK_SPAR=1 NHOOK_NAV=enemy NHOOK_BOT_FIRE=1 NHOOK_BOT_FIGHT=1 \
  NHOOK_SPAR_LOG=1 NHOOK_RADAR_LOG=1 PELOAD_NAV_LOG=1 ${EXTRA[@]+"${EXTRA[@]}"} NATIVE_SECS="$SECS" bash shim/native.sh "${ARGS[@]}" > "$L.out" 2>&1
echo "log: $L"
grep -a '\[firefight\]' "$L" | tail -5
echo "walker: $(grep -a -c 'new target' "$L") targets taken, $(grep -a -c 'FIRING' "$L") firing lines, $(grep -a -c '\[goal\] died' "$L") deaths, $(grep -a -c '\[goal\]' "$L") goal lines"
grep -a '\[nhook\] \[fight\] engagement model\|\[nhook\] \[fight\] behaviour\|no heat or lanes\|player lanes from\|\[nav\] ' "$L" | head -6 | cut -c1-200
