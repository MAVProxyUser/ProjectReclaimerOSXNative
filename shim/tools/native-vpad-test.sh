#!/bin/bash
# Does a JOINED walker's movement reach the server? (2026-11-08, backlog N24.) Our local-only server + ONE test client joined to it,
# its walker roaming (NHOOK_NAV=roam). Every second the client's own position and the SERVER's view of it; PASS = the server's
# path is at least 60% of the client's and at least 25 units in 40 s (a client's moves that do not reach the server snap back:
# the server adopts a client's predicted position only within ~2.5 units, net-authority.md). Sound muted.
#   bash shim/tools/native-vpad-test.sh [extra env...]      e.g. NHOOK_BOT_NOPKT=1 (the control packet hook off) / NHOOK_VPAD=1
# Refuses to run beside another Project Reclaimer.
set -u
cd "$(dirname "$0")/../.."
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-vpad.XXXXXX"); fail=0
bash shim/tools/native-dedicated.sh start >/dev/null || exit 1
env "$@" NATIVE_LOG="$T/a.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_MUTE_OUTPUT=1 NHOOK_NAV=roam NHOOK_SPAR_LOG=1 \
  bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/a.out" 2>&1 &
pos() { curl -s -m 5 "http://127.0.0.1:$1/stats.json" | python3 -c 'import json,sys; p=[q for q in json.load(sys.stdin).get("players",[]) if q.get("alive")]; print(" ".join("%.2f" % v for v in p[0]["pos"]) if p else "none")' 2>/dev/null; }
for i in $(seq 1 90); do q=$(pos 8767); [ -n "$q" ] && [ "$q" != none ] && break; sleep 1; done
sleep 8; : > "$T/track.txt"
for i in $(seq 1 40); do echo "$(pos 8765) | $(pos 8767)" >> "$T/track.txt"; sleep 1; done
python3 - "$T/track.txt" <<'EOF'
import math, sys
c, s = [], []
for l in open(sys.argv[1]):
    a, _, b = l.partition('|')
    try: c.append([float(x) for x in a.split()]); s.append([float(x) for x in b.split()])
    except ValueError: pass
L = lambda P: sum(math.hypot(q[0] - p[0], q[1] - p[1]) for p, q in zip(P, P[1:]) if math.hypot(q[0] - p[0], q[1] - p[1]) < 6)
lc, ls = L(c), L(s); print('client path %.1f units, server path %.1f units (40 s)' % (lc, ls))
sys.exit(0 if ls >= 25 and ls >= 0.6 * lc else 1)
EOF
[ $? = 0 ] && echo "ok   the server follows the walker" || { echo "FAIL the server does not follow the walker"; fail=1; }
grep -a 'control packet\|\[vpad\]' "$T/a.log" | head -3 | cut -c1-160
P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/a.out"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }
bash shim/tools/native-dedicated.sh stop >/dev/null
echo "logs: $T"; [ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
