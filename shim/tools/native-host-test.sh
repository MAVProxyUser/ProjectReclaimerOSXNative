#!/bin/bash
# Live checks of nhook's host tools (shim/native/nhost.c) that need more than one look at a position.
#   bash shim/tools/native-host-test.sh walls    a LOCAL test match on Guardian (the test profile, a background window):
#        the player is put 1.8 units north of a thin player-clip wall (x -13.98..-8.10, y -19.35, z 21.31..32.67: the
#        loader's 2026-10-18 test wall, re-tools/h3inv.py) and walks south at it, once with the invisible walls on (it must
#        stop at the wall) and once with them off (it must walk through it).
#   bash shim/tools/native-host-test.sh tpnext   our native dedicated server (shim/tools/native-dedicated.sh) + TWO test
#        clients (the test profile and a second one with its own identity, ReclaimerMac/native/test-second): the server
#        moves the first client next to the second (do=tpnext&slot=&who=); both clients must see it there. Then B is put at
#        a ledge on Guardian (-3.95, -23.71: the open drop beside it once took a player put "right of" B into the kill
#        volume) and A next to B again: A must stand there (no fall) 4 s later.
# Test runs only: refuses to run beside another Project Reclaimer (the user's game included).
set -u
cd "$(dirname "$0")/../.."
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/native-host.XXXXXX"); C='-s -m 3 -X POST -H X-Reclaimer-Control:1'
ctl() { curl $C "http://127.0.0.1:$1/control?do=$2" >/dev/null; }
pos() {   # pos <port> [slot]: x y z alive of the player (the page's own one, else that slot)
  curl -s -m 3 "http://127.0.0.1:$1/stats.json" | python3 -c '
import json, sys
d = json.load(sys.stdin); want = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1] else None
ps = [p for p in d.get("players", []) if (p.get("slot") == want if want is not None else p.get("you"))]
p = ps[0] if ps else {}; q = p.get("pos") or [0, 0, 0]
print("%.2f %.2f %.2f %s" % (q[0], q[1], q[2], "alive" if p.get("alive") else "dead"))' "${2:-}" 2>/dev/null || echo "0 0 0 none"; }
fail=0
case "${1:-}" in
walls)
  WALK='{"campaign":{"walk_to":{"point":[-11.0,-23.0,21.4]}}}'
  NATIVE_LOG="$T/app.log" NHOOK_STATS_OPEN=0 NATIVE_SECS=95 bash shim/native.sh --map guardian --seconds 90 --background \
    --request-at 30 '{"campaign":{"walk_by_input":true}}' --request-at 36 "$WALK" --request-at 58 "$WALK" > "$T/out" 2>&1 &
  NS=$!; t0=$(date +%s); at() { while [ $(( $(date +%s) - t0 )) -lt "$1" ]; do sleep 0.2; done; }
  at 32; ctl 8765 'invwalls&off=0'; ctl 8765 'tpto&x=-11&y=-17.55&z=21.6'
  for t in 36 39 42 45; do at $t; echo "  t=$t $(pos 8765)"; done
  at 46; P1=$(pos 8765); echo "walls on:  $P1"
  at 52; ctl 8765 'invwalls&off=1'; ctl 8765 'tpto&x=-11&y=-17.55&z=21.6'
  for t in 56 59 62 65; do at $t; echo "  t=$t $(pos 8765)"; done
  at 68; P2=$(pos 8765); echo "walls off: $P2"
  curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c 'import json,sys; o=json.load(sys.stdin).get("overlay",{}); print("host_stats", o.get("host_stats"), "|", o.get("host_msg"))'
  wait $NS
  python3 - "$P1" "$P2" <<'EOF' || fail=1
import sys
y1 = float(sys.argv[1].split()[1]); y2 = float(sys.argv[2].split()[1])
ok1 = -19.6 < y1 < -18.9; ok2 = y2 < -19.7
print("ok   " if ok1 else "FAIL ", "walls on: stopped at the wall (y %.2f; the wall is at -19.35)" % y1)
print("ok   " if ok2 else "FAIL ", "walls off: walked through it (y %.2f)" % y2)
sys.exit(0 if ok1 and ok2 else 1)
EOF
  grep -E '\[nhook\] \[host\]' "$T/app.log" | tail -6 ;;
tpnext)
  SUP="$HOME/Library/Application Support/ReclaimerMac/native"; mkdir -p "$SUP/test-second"
  # the second client's profile: the first one's settings and first-run markers, NOT its profile.json (the app makes a new
  # identity there on its first start; its nickname is set to "Second" once it exists)
  for f in mcc-folder.txt settings.json appearance.json default-name-checked service-tag-checked; do
    [ -e "$SUP/test-second/$f" ] || cp "$SUP/test-v2/$f" "$SUP/test-second/$f" 2>/dev/null; done
  [ -f "$SUP/test-second/profile.json" ] && python3 -c '
import json, sys; p = sys.argv[1]; d = json.load(open(p))
if d.get("identity", {}).get("nickname") != "Second": d["identity"]["nickname"] = "Second"; json.dump(d, open(p, "w"), indent=2)' "$SUP/test-second/profile.json"
  NHOOK_HOST_LOG=1 bash shim/tools/native-dedicated.sh start || exit 1
  JOIN='--join-server 127.0.0.1:49190'
  NATIVE_LOG="$T/a.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 bash shim/native.sh $JOIN --background > "$T/a.out" 2>&1 & A=$!
  sleep 8
  NATIVE_LOG="$T/b.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_STATS_HTTP=8766 NATIVE_PROFILE="$SUP/test-second" \
    RECLAIMER_CACHE_DIR="$SUP/test-second-cache" PELOAD_FORENSIC_DIR="$SUP/test-second-forensics" NHOOK_HEATMAP_DIR="$SUP/test-second-heatmaps" \
    bash shim/native.sh $JOIN --background > "$T/b.out" 2>&1 & B=$!
  # both in the game: the server sees two players with bodies
  for i in $(seq 1 120); do n=$(curl -s -m 3 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys; print(sum(1 for p in json.load(sys.stdin).get("players",[]) if p.get("alive")))' 2>/dev/null || echo 0)
    [ "$n" -ge 2 ] && break; sleep 1; done
  echo "players alive on the server: $n"
  me() { for i in $(seq 1 15); do v=$(curl -s -m 8 "http://127.0.0.1:$1/stats.json" | python3 -c 'import json,sys; print([p["slot"] for p in json.load(sys.stdin)["players"] if p.get("you")][0])' 2>/dev/null)
           [ -n "$v" ] && { echo "$v"; return; }; sleep 1; done; }   # (each client's own slot: join order decides them)
  SA=$(me 8765); SB=$(me 8766)
  echo "client A = slot $SA, client B = slot $SB"
  if [ -n "$SA" ] && [ -n "$SB" ] && [ "$n" -ge 2 ]; then
    echo "before: server A $(pos 8767 "$SA") B $(pos 8767 "$SB")"
    ctl 8767 "tpnext&slot=$SB&who=$SA"; sleep 3
    curl -s -m 3 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys; print("server:", json.load(sys.stdin).get("overlay",{}).get("host_msg"))'
    PS=$(pos 8767 "$SA"); PB=$(pos 8767 "$SB"); PA_A=$(pos 8765); PA_B=$(pos 8766 "$SA")
    echo "after:  server A $PS B $PB | client A sees itself $PA_A | client B sees A $PA_B"
    python3 - "$PS" "$PB" "$PA_A" "$PA_B" <<'EOF' || fail=1
import sys, math
v = [list(map(float, a.split()[:3])) for a in sys.argv[1:5]]
d = lambda a, b: math.dist(a[:2], b[:2])
ok = d(v[0], v[1]) < 2.0 and d(v[2], v[0]) < 0.6 and d(v[3], v[0]) < 0.6
print("ok  " if ok else "FAIL", "A next to B on the server (%.2f units apart); client A agrees within %.2f, client B within %.2f" % (d(v[0], v[1]), d(v[2], v[0]), d(v[3], v[0])))
sys.exit(0 if ok else 1)
EOF
    ctl 8767 "tpto&x=-3.95&y=-23.71&z=17.2&slot=$SB"; sleep 2; ctl 8767 "tpnext&slot=$SB&who=$SA"; sleep 1
    curl -s -m 3 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys; print("server:", json.load(sys.stdin).get("overlay",{}).get("host_msg"))'
    grep 'next to: ' "$HOME/Library/Application Support/ReclaimerMac/native/test-dedicated/server.log.nhook" | tail -4
    sleep 4; PL=$(pos 8767 "$SA"); echo "A 4 s after the move next to B at the ledge: $PL"
    python3 - "$PL" <<'EOF' || fail=1
import sys
x, y, z, st = sys.argv[1].split(); ok = st == "alive" and float(z) > 16.0
print("ok  " if ok else "FAIL", "A stands next to B at the ledge (z %s, %s; the floor there is ~16.9, the kill volume below 13.6)" % (z, st))
sys.exit(0 if ok else 1)
EOF
  else echo "FAIL the two clients did not both get into the game"; fail=1; fi
  for o in "$T/a.out" "$T/b.out"; do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$o"); [ -n "$P" ] && kill "$P" 2>/dev/null; done; sleep 2
  for o in "$T/a.out" "$T/b.out"; do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$o"); [ -n "$P" ] && kill -9 "$P" 2>/dev/null; done; wait $A $B 2>/dev/null
  bash shim/tools/native-dedicated.sh stop ;;
*) echo "usage: $0 walls|tpnext"; exit 2 ;;
esac
echo "logs: $T"; [ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
