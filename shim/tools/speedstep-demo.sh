#!/bin/bash
# Speed-step demonstration, for a bug report to the Reclaimer developers (shim/docs/net-authority.md): a dedicated server
# takes the positions a client reports for its own player when they move up to ~2.5 units per update, with no speed
# check. LOCAL ONLY: it starts this Mac's own dedicated server (shim/dedicated.sh: 127.0.0.1, hidden, listed nowhere),
# joins one input-less test client, and records the player's position as the SERVER holds it (its stats page) while
#   1. the client walks normally for 6 s (scripted stick input): the reference speed;
#   2. the client moves its own player STEP units forward every INTERVAL s, COUNT times (the loader's client-side test
#      hook PELOAD_TELEPORT_CLIENT_TEST: POST /control?do=tpby on the client's own stats page).
# Then it stops both and writes into OUT (default ~/speedstep-demo-<date>):
#   positions.csv        time, phase, server x/y/z, client x/y/z
#   summary.txt          the speeds the server accepted: walking vs stepping
#   speedstep-demo.mp4   the server-side track over the map picture, with those speeds (needs ffmpeg)
#     bash shim/tools/speedstep-demo.sh                 # STEP=2.0 INTERVAL=0.25 COUNT=24
# Needs python3 with Pillow, the map pictures (shim/tools/maptop.sh). Nothing else may be running (one game at a time).
cd "$(dirname "$0")/../.."
STEP="${STEP:-2.0}"; INTERVAL="${INTERVAL:-0.25}"; COUNT="${COUNT:-24}"
OUT="${OUT:-$HOME/speedstep-demo-$(date +%Y%m%d-%H%M%S)}"
others(){ pgrep -f '^[^ ]*build/peload'; }
[ -z "$(others)" ] || { echo "a game is running: close it first (this runs its own server and client)"; exit 3; }
python3 -c 'import PIL' 2>/dev/null || { echo "needs python3 with Pillow"; exit 1; }
mkdir -p "$OUT" || exit 1
echo "local dedicated server (Valhalla Slayer on 127.0.0.1:49190, stats on 127.0.0.1:8767)"
DED_ENV="PELOAD_STATS_HTTP=8767" bash shim/dedicated.sh >"$OUT/server-start.log" 2>&1
cleanup(){ bash shim/dedicated.sh stop >/dev/null 2>&1; [ -n "${QR:-}" ] && kill "$QR" 2>/dev/null; for p in $(others); do kill -9 -- -"$(ps -o pgid= -p "$p" | tr -d ' ')" 2>/dev/null; done; }
trap cleanup EXIT
sleep 8
echo "test client joining it (no input, no microphone; scripted walk 3 s after the match starts)"
QR_PAD=1 QR_MINIMIZE=1 bash shim/tools/quiet-run.sh "PELOAD_STATS_HTTP=8765 PELOAD_TELEPORT_CLIENT_TEST=1 PELOAD_ALLOW_TELEPORT=1 PELOAD_INPUT_SCRIPT=6:0,1,0,0;600:0,0,0,0 PELOAD_INPUT_DELAY=3" 600 "reclaimer://join?server=127.0.0.1:49190" >/dev/null 2>&1 &
QR=$!
python3 shim/tools/speedstep-demo.py --client 127.0.0.1:8765 --server 127.0.0.1:8767 --step "$STEP" --interval "$INTERVAL" --count "$COUNT" --out "$OUT"
rc=$?
echo "results: $OUT"; ls "$OUT"
exit $rc
