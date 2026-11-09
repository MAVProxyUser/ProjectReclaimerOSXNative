#!/bin/bash
# Run the NATIVE Reclaimer app (0.10+, /Applications/Project Reclaimer.app) with our hook dylib (shim/build/nhook.dylib).
# Default = the TEST profile ReclaimerMac/native/test-v2 (services left alone, muted); NATIVE_REAL=1 = the real player folder.
#   bash shim/native.sh [args...]          log: /tmp/reclaimer_native.log
#   NATIVE_SECS=40 bash shim/native.sh     quit after 40 s (test runs: give it time to reach "[menus] open")
#   bash shim/native.sh --map guardian --seconds 60 --background    a test match in a window that never takes the focus
# Stats page: http://127.0.0.1:8765/ (NHOOK_STATS_HTTP=<port>, 0 = off). Never starts while the app already runs.
cd "$(dirname "$0")/.."
APP="${NATIVE_APP:-/Applications/Project Reclaimer.app}/Contents/MacOS/project-reclaimer"
SUP="$HOME/Library/Application Support/ReclaimerMac"
[ -f shim/build/nhook.dylib ] || bash shim/native/build.sh
# The app updates itself. A build nhook has no addresses for (shim/native/napp_builds.h, by the app binary's UUID) is ported
# here once: re-tools/machoport.py finds every point again from the 0.10.0 reference (re-tools/machoport-sigs.json) and checks
# each one; the row goes into napp_builds.h (+ the dylib is rebuilt when the sources are here) and into
# ReclaimerMac/native/app-builds.txt (a prebuilt dylib reads that). What it could not find stays off: the stats page's Hooks
# check window and GET /hooks.json say which. NATIVE_NO_AUTOPORT=1 = never (the old behaviour: hooks off on a new build).
if [ "${NATIVE_NO_AUTOPORT:-0}" != 1 ] && [ -f re-tools/machoport.py ] && [ -x "$APP" ]; then
  python3 re-tools/machoport.py check "$APP" >/dev/null 2>&1; rc=$?
  if [ $rc = 2 ]; then
    REF="${NATIVE_PORT_REF:-re-tools/machoport-sigs.json}"   # (the 0.10.0 reference as signatures: no old app needed)
    if [ -f "$REF" ]; then echo "native.sh: a new Project Reclaimer build -- porting nhook's addresses to it (re-tools/machoport.py)" >&2
      python3 re-tools/machoport.py port "$REF" "$APP" --apply 2>&1 | sed 's/^/  /' >&2
      if [ -f shim/native/nhook.c ]; then bash shim/native/build.sh >/dev/null 2>&1 && echo "native.sh: nhook rebuilt for it" >&2 || echo "native.sh: nhook did not rebuild (it reads ReclaimerMac/native/app-builds.txt instead)" >&2; fi
    else echo "native.sh: this app build is not in nhook's address table, and the reference to port from is not at $REF: the hooks stay off (see the stats page's Hooks check)" >&2; fi
  fi
fi
if [ "${NATIVE_REAL:-0}" = 1 ]; then PROF="$HOME/Library/Application Support/ProjectReclaimer"; unset RECLAIMER_PROFILE_DIR
     export NHOOK_STATS_OPEN="${NHOOK_STATS_OPEN:-1}"   # the stats page opens in the browser (unless a tab is already open)
else PROF="${NATIVE_PROFILE:-$SUP/native/test-v2}"; export RECLAIMER_PROFILE_DIR="$PROF" RECLAIMER_CACHE_DIR="${RECLAIMER_CACHE_DIR:-$SUP/native/test-cache}"
     export RECLAIMER_TEST_SILENT="${RECLAIMER_TEST_SILENT:-1}"
     export PELOAD_FORENSIC_DIR="${PELOAD_FORENSIC_DIR:-$SUP/native/test-forensics}"   # test runs keep out of the real archive
     export NHOOK_HEATMAP_DIR="${NHOOK_HEATMAP_DIR:-$SUP/native/test-heatmaps}"; mkdir -p "$NHOOK_HEATMAP_DIR"; fi   # ... and the real heatmaps
# a HOME of its own (walker-public.sh, a public server's walker): Python's user packages (numpy for the walking-graph builder,
# re-tools/h3nav.py) stay under the user's real home -- found from the user database, not $HOME (2026-11-09: Last Resort's graph
# failed "No module named 'numpy'" and the walker stood still)
RH=$(eval echo "~$(id -un)")
if [ "$HOME" != "$RH" ] && [ -z "${PYTHONUSERBASE:-}" ]; then export PYTHONUSERBASE="$RH/Library/Python/$(/usr/bin/python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"; fi
MCC="$(cat "$PROF/mcc-folder.txt" 2>/dev/null)"
export NHOOK_HALO3="${NHOOK_HALO3:-$MCC/halo3/halo3.dll}"
# the stats page from the checkout (edits show on a browser refresh; the dylib's built-in copy otherwise)
[ -f shim/src/stats_page.html ] && export NHOOK_STATS_PAGE="${NHOOK_STATS_PAGE:-$PWD/shim/src/stats_page.html}"
[ -f shim/src/forensic_page.html ] && export PELOAD_FORENSIC_PAGE="${PELOAD_FORENSIC_PAGE:-$PWD/shim/src/forensic_page.html}"
LOG="${NATIVE_LOG:-/tmp/reclaimer_native.log}"
# one game at a time (the user's own included): wait up to 30 s for a test instance that is ending, refuse otherwise
[ "${NATIVE_ALLOW_SECOND:-0}" = 1 ] || for i in $(seq 1 30); do pgrep -f 'Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null || break; sleep 1; done
if pgrep -f 'Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null && [ "${NATIVE_ALLOW_SECOND:-0}" != 1 ]; then
  echo "native.sh: Project Reclaimer is already running (pid $(pgrep -f 'Project Reclaimer.app/Contents/MacOS/project-reclaimer' | tr '\n' ' ')): not starting a second one" >&2; exit 1; fi
# NATIVE_DYLIB=<file>: another copy of the hook dylib (a long run keeps the one it started with while shim/build/ is rebuilt)
DYLD_INSERT_LIBRARIES="${NATIVE_DYLIB:-$PWD/shim/build/nhook.dylib}" "$APP" "$@" > "$LOG" 2>&1 &
PID=$!; echo "native app pid $PID, log $LOG"
if [ -n "$NATIVE_SECS" ]; then sleep "$NATIVE_SECS"; kill $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; fi
wait $PID 2>/dev/null
