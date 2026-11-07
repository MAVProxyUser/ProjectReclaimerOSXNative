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
if [ "${NATIVE_REAL:-0}" = 1 ]; then PROF="$HOME/Library/Application Support/ProjectReclaimer"; unset RECLAIMER_PROFILE_DIR
     export NHOOK_STATS_OPEN="${NHOOK_STATS_OPEN:-1}"   # the stats page opens in the browser (unless a tab is already open)
else PROF="${NATIVE_PROFILE:-$SUP/native/test-v2}"; export RECLAIMER_PROFILE_DIR="$PROF" RECLAIMER_CACHE_DIR="${RECLAIMER_CACHE_DIR:-$SUP/native/test-cache}"
     export RECLAIMER_TEST_SILENT="${RECLAIMER_TEST_SILENT:-1}"
     export PELOAD_FORENSIC_DIR="${PELOAD_FORENSIC_DIR:-$SUP/native/test-forensics}"   # test runs keep out of the real archive
     export NHOOK_HEATMAP_DIR="${NHOOK_HEATMAP_DIR:-$SUP/native/test-heatmaps}"; mkdir -p "$NHOOK_HEATMAP_DIR"; fi   # ... and the real heatmaps
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
DYLD_INSERT_LIBRARIES="$PWD/shim/build/nhook.dylib" "$APP" "$@" > "$LOG" 2>&1 &
PID=$!; echo "native app pid $PID, log $LOG"
if [ -n "$NATIVE_SECS" ]; then sleep "$NATIVE_SECS"; kill $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; fi
wait $PID 2>/dev/null
