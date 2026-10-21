#!/bin/bash
# Offline test of the loader's DirectSound (shim/src/dsound.c): a looping secondary buffer through the real CoreAudio output at
# volume -10000 (nothing is heard), then the silent device (PELOAD_AUDIO=0); each cursor must advance at the sample rate
# (PELOAD_DSOUND_SELFTEST). No game, no window.
cd "$(dirname "$0")/../.."
[ -x shim/build/peload ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
fail=0
for a in 1 0; do PELOAD_AUDIO=$a PELOAD_DSOUND_SELFTEST=1 arch -x86_64 shim/build/peload 2>/dev/null | grep -E '^(PASS|FAIL)' || fail=1
  [ ${PIPESTATUS[0]} = 0 ] || fail=1; done
[ $fail = 0 ] && echo "dsound-test: PASS" || echo "dsound-test: FAIL"; exit $fail
