#!/bin/bash
# Run the NATIVE Reclaimer app (0.10+, /Applications/Project Reclaimer.app) with our hook dylib (shim/build/nhook.dylib).
# Default = the TEST profile ReclaimerMac/native/test-v2 (services left alone, muted); NATIVE_REAL=1 = the real player folder.
#   bash shim/native.sh [args...]          log: /tmp/reclaimer_native.log
#   NATIVE_SECS=40 bash shim/native.sh     quit after 40 s (test runs: give it time to reach "[menus] open")
cd "$(dirname "$0")/.."
APP="${NATIVE_APP:-/Applications/Project Reclaimer.app}/Contents/MacOS/project-reclaimer"
SUP="$HOME/Library/Application Support/ReclaimerMac"
[ -f shim/build/nhook.dylib ] || bash shim/native/build.sh
if [ "${NATIVE_REAL:-0}" = 1 ]; then PROF="$HOME/Library/Application Support/ProjectReclaimer"; unset RECLAIMER_PROFILE_DIR
else PROF="${NATIVE_PROFILE:-$SUP/native/test-v2}"; export RECLAIMER_PROFILE_DIR="$PROF" RECLAIMER_CACHE_DIR="${RECLAIMER_CACHE_DIR:-$SUP/native/test-cache}"
     export RECLAIMER_TEST_SILENT="${RECLAIMER_TEST_SILENT:-1}"; fi
MCC="$(cat "$PROF/mcc-folder.txt" 2>/dev/null)"
export NHOOK_HALO3="${NHOOK_HALO3:-$MCC/halo3/halo3.dll}"
LOG="${NATIVE_LOG:-/tmp/reclaimer_native.log}"
DYLD_INSERT_LIBRARIES="$PWD/shim/build/nhook.dylib" "$APP" "$@" > "$LOG" 2>&1 &
PID=$!; echo "native app pid $PID, log $LOG"
if [ -n "$NATIVE_SECS" ]; then sleep "$NATIVE_SECS"; kill $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; fi
wait $PID 2>/dev/null
