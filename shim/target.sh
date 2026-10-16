#!/bin/bash
# Start a second copy of the client as a stationary TARGET player: it hosts a hidden Online game on
# Valhalla (or TARGET_MAP) by itself and stands in the match, so you can join and shoot at it:
#     bash shim/target.sh                                   # start the target (runs in the background)
#     RECLAIMER_JOIN=127.0.0.1:11774 bash shim/play.sh       # join it (about 40 s after starting the target)
#     bash shim/target.sh stop                               # stop it
# The target has its own profile ("Target"), no controller/keyboard/mouse, no audio and never takes focus,
# so it doesn't steal your input. It drives its own menu with scripted clicks (PELOAD_AUTOCLICK) in its
# window: Host Game -> map search -> Network Online -> Hidden server -> Start Game.
#   TARGET_MAP=<name>   map to host (searched in the map picker), default Valhalla
# Log: /tmp/reclaimer_target.log
cd "$(dirname "$0")/.."
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
PIDFILE="$HOME/Library/Application Support/ReclaimerMac/target.pgid"
XA="exe+460294:909090909090,exe+4602cb:909090909090,exe+460302:909090909090,exe+460335:9090,exe+65ddf9:eb"

stop_target(){ [ -f "$PIDFILE" ] && kill -9 -- -"$(cat "$PIDFILE")" 2>/dev/null; rm -f "$PIDFILE"; }
if [ "${1:-}" = "stop" ]; then stop_target; echo "target stopped"; exit 0; fi
stop_target

EXE="${RECLAIMER_EXE:-}"
if [ -z "$EXE" ]; then
  for c in client/project-reclaimer-v0.9.13.exe analysis/project-reclaimer-v0.9.13.exe; do [ -f "$c" ] && EXE="$c" && break; done
fi
[ -n "$EXE" ] && [ -f "$EXE" ] || { echo "Project Reclaimer v0.9.13 exe not found (see README)"; exit 1; }
[ -x shim/build/peload ] || { echo "shim/build/peload missing: run 'bash shim/build.sh'"; exit 1; }
mkdir -p "$PFX/ReclaimerProfileTarget"

MAP="${TARGET_MAP:-Valhalla}"
# Window coordinates (1280x720 client space) of Reclaimer v0.9.13's menu:
#   Host Game | Map | search box | type map | USE <MAP> | Network -> Online | Host Settings | scroll down |
#   Hidden server | Apply changes (returns to the lobby page) | Start Game
CLICKS="110,503;230,183;300,143;t:$(echo "$MAP" | tr 'A-Z' 'a-z');1047,663;384,440;230,497;w:450,500,-15;808,592;967,668;s:2;230,625"

set -m
env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 PELOAD_PATCH="$XA" PELOAD_WORKER_INHERIT_STDIO=1 \
    PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_PRESENT_HIDDEN=1 PELOAD_D3DGL=1 \
    PELOAD_NO_PAD=1 PELOAD_NO_KBM=1 PELOAD_NO_SYS_CURSOR=1 PELOAD_NO_FOCUS=1 PELOAD_AUDIO=0 \
    PELOAD_AUTOCLICK="$CLICKS" RECLAIMER_PROFILE_DIR='C:\ReclaimerProfileTarget' \
    arch -x86_64 shim/build/peload "$EXE" game-client \
    --mcc-path "C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection" \
    < /dev/null > /tmp/reclaimer_target.log 2>&1 &
echo $! > "$PIDFILE"
echo "target starting: hosting $MAP (hidden, Online) in ~40 s; log: /tmp/reclaimer_target.log"
echo "then join it:  RECLAIMER_JOIN=127.0.0.1:11774 bash shim/play.sh"
echo "stop it:       bash shim/target.sh stop"
