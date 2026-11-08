#!/bin/bash
# Every live test of nhook in the native app, one after another (2026-11-08: the check after an app update, with
# re-tools/machoport.py's port): the hooks check first, then the features -- shots / tracers, aim, host tools (invisible walls,
# teleports between two clients on our local server), the voice channel, the sparring aim and the walker. Test profiles, local
# matches and our local-only dedicated server only; each test refuses to start beside another Project Reclaimer (the user's
# game included), and so does this.  ~20 minutes.
#   bash shim/tools/native-test-all.sh [test ...]     (default: hooks fire aim walls tpnext voice spar)
set -u
cd "$(dirname "$0")/../.."
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting the tests"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-all.XXXXXX"); tests="${*:-hooks fire aim walls tpnext voice spar}"; res=""; fail=0
for t in $tests; do
  case $t in hooks) c="bash shim/tools/native-hooks-test.sh";; fire) c="bash shim/tools/native-fire-test.sh";; aim) c="bash shim/tools/native-aim-test.sh";;
    walls) c="bash shim/tools/native-host-test.sh walls";; tpnext) c="bash shim/tools/native-host-test.sh tpnext";;
    voice) c="bash shim/tools/native-voice-test.sh";; spar) c="bash shim/tools/native-spar-test.sh";; *) echo "unknown test $t"; continue;; esac
  echo "=== $t: $c"; s=$(date +%s); $c > "$T/$t.txt" 2>&1; rc=$?; e=$(( $(date +%s) - s ))
  grep -E '^(ok|FAIL|PASS)' "$T/$t.txt" | grep -v '^ok   [a-z_ ]*address\b' | tail -12
  [ $rc = 0 ] && r=PASS || { r=FAIL; fail=1; }; res="$res$(printf '%-8s %s  (%d s)' "$t" "$r" "$e")\n"
  # (a test that left an app behind would make the next refuse: wait for it to end)
  for i in $(seq 1 20); do pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null || break; sleep 1; done
done
echo; echo "=== results (logs: $T)"; printf "$res"; [ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
