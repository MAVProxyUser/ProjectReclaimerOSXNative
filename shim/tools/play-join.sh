#!/bin/bash
# The user's own game into a given server, in a window (2026-11-09; the user: "join me a client to where AIWalkerTest is so I can follow
# it around and exercise it"): play-vs-walkers.sh's window handling without walkers or a server of ours -- graphics.display_mode =
# "windowed" (+ PV_SIZE, default 1600x900) in the profile's settings.json for this launch, the three values put back when the game
# closes (shim/tools/play-window.py); the real profile; a walker app may already run (NATIVE_ALLOW_SECOND).
#   bash shim/tools/play-join.sh <host:port>        PV_FULL=1 = your own (full-screen) settings
set -u
cd "$(dirname "$0")/../.."
[ $# -ge 1 ] || { echo "usage: $0 <host:port>"; exit 1; }
PS="$HOME/Library/Application Support/ProjectReclaimer/settings.json"; OLD=""
if [ "${PV_FULL:-0}" != 1 ]; then OLD=$(python3 shim/tools/play-window.py set "$PS" "${PV_SIZE:-1600x900}"); fi
NATIVE_REAL=1 NATIVE_ALLOW_SECOND=1 bash shim/native.sh --join-server "$1"
[ -n "$OLD" ] && python3 shim/tools/play-window.py restore "$PS" "$OLD"
