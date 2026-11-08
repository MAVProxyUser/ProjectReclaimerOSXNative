#!/bin/bash
# The learning refresh (2026-11-08): new recordings -> engagement library -> models + tome -> learnings -> coach, every 20 minutes.
# Background priority (taskpolicy -b + nice 20: the Mac's efficiency cores, below everything else), and it WAITS while the user
# plays -- a Project Reclaimer running without --background (the user: "something you did is making input feel choppy": the
# model refit ran at ~90% of a core for minutes beside the game). REFRESH_EVERY=<s> another period.
set -u
cd "$(dirname "$0")/../.."
bg() { taskpolicy -b nice -n 20 "$@"; }
playing() { ps -Ao command | grep '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' | grep -v -- '--background' | grep -v -- '--server' | grep -q .; }
while true; do
  if playing; then echo "$(date +%H:%M) a game is being played: the refresh waits"; while playing; do sleep 60; done; fi
  echo "$(date +%H:%M) refresh: new recordings -> models + tome -> learnings -> coach"
  bg python3 shim/tools/engage-extract.py --quiet | tail -1
  bg python3 shim/tools/engage-model.py > /dev/null 2>&1 && echo "  models + tome refit"
  bg python3 shim/tools/learnings.py --quiet && echo "  learnings written"
  bg python3 shim/tools/coach.py --quiet | tail -1
  sleep "${REFRESH_EVERY:-1200}"
done
