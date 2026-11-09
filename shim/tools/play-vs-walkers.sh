#!/bin/bash
# Play against walkers on our local server (2026-11-08; the user: "load me into a match with 3 headless walkers don't full screen me
# though, I want to watch the web stats at the same time"). The walkers' games run headless (walker-battle.sh: test identities
# Second / Third / Fourth, their coached knobs from walker-coach.py); the user's own game (the real profile, the controller) joins
# them in a WINDOW: graphics.display_mode = "windowed" (+ PV_SIZE, default 1600x900) in the profile's settings.json for this launch,
# the old three values put back when the game closes (shim/tools/play-window.py). The user's stats page opens in the browser (8765); the
# server's own page (every player) is http://127.0.0.1:8767/.
#   bash shim/tools/play-vs-walkers.sh [walkers 3] [minutes 60]        PV_FULL=1 = the user's own (full-screen) settings
# Quit the game to end; the walkers and the server stop when the time is up, or: pkill -f 'walker-battle.sh'; bash
# shim/tools/native-dedicated.sh stop.
set -u
cd "$(dirname "$0")/../.."
N=${1:-3}; MIN=${2:-60}; SUP="$HOME/Library/Application Support/ReclaimerMac"; LOGS="$SUP/battles"; mkdir -p "$LOGS"
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting"; exit 1; fi
# the walkers: identities 1..N (test-v2 stays out: its nickname is the user's), each with its coached knobs
IDS=$(seq 1 "$N" | tr '\n' ' '); ENVS=()
while read -r w rest; do [ -n "$rest" ] && ENVS+=("W${w}_ENV=$rest"); done < <(python3 shim/tools/walker-coach.py show 2>/dev/null | sed -n 's/^walker \([0-3]\): \(.*\)/\1 \2/p')
env ${ENVS[@]+"${ENVS[@]}"} WB_IDS="$IDS" bash shim/tools/walker-battle.sh "$N" $((MIN * 60)) > "$LOGS/play-vs-walkers.txt" 2>&1 &
for i in $(seq 1 60); do grep -q '^battle:' "$LOGS/play-vs-walkers.txt" 2>/dev/null && break; sleep 1; done
sleep 8
# the user's game, windowed unless PV_FULL=1: the app's --settings takes the ENGINE's settings (render_resolution, frame_rate ...),
# not the profile's, so the profile's settings.json gets graphics.display_mode = "windowed" (+ width / height) for this launch and
# those three values -- only those: anything the user changes in the game stays -- are put back when the game closes
PS="$HOME/Library/Application Support/ProjectReclaimer/settings.json"; OLD=""
if [ "${PV_FULL:-0}" != 1 ]; then OLD=$(python3 shim/tools/play-window.py set "$PS" "${PV_SIZE:-1600x900}"); fi
NATIVE_REAL=1 NATIVE_ALLOW_SECOND=1 bash shim/native.sh --join-server 127.0.0.1:49190 &
GP=$!
echo "walkers: $N (headless), for $MIN minutes; your game: $([ "${PV_FULL:-0}" = 1 ] && echo full screen || echo "a ${PV_SIZE:-1600x900} window"); the server's page: http://127.0.0.1:8767/"
wait $GP
[ -n "$OLD" ] && python3 shim/tools/play-window.py restore "$PS" "$OLD"
wait
