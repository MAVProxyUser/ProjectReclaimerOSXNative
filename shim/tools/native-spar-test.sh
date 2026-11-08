#!/bin/bash
# Live check of the native sparring aim and walker (nbot.c).
#  1. our native dedicated server (local only) + A (the test profile, standing still) + B (shim/tools/native-spar.sh: the second
#     test identity, aim on). PASS = on the SERVER's page B's aim points at A within 3 degrees (median), B never dies, B's page
#     switches answer (do=spar&on=0) and B's page offers no walker (a joined client's moves are not ours to drive).
#  2. one test client in a local Guardian game with the walker roaming: PASS = it reaches a goal and covers 10+ units.
#   bash shim/tools/native-spar-test.sh
# Test runs only: refuses to run beside another Project Reclaimer (the user's game included).
set -u
cd "$(dirname "$0")/../.."
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/native-spar.XXXXXX"); fail=0
pid_of() { sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$1"; }
stop_app() { P=$(pid_of "$1"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }; }
snap() { curl -s -m 5 "http://127.0.0.1:$1/stats.json"; }
# ---- 1. the sparring partner on our server
bash shim/tools/native-dedicated.sh start || exit 1
NATIVE_LOG="$T/a.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/a.out" 2>&1 &
sleep 8
NATIVE_LOG="$T/b.log" NHOOK_SPAR_LOG=1 bash shim/tools/native-spar.sh start 127.0.0.1:49190 || { stop_app "$T/a.out"; bash shim/tools/native-dedicated.sh stop; exit 1; }
errs=""; deaths=0; seen=0; was=0
for i in $(seq 1 70); do sleep 1
  r=$(snap 8767 | python3 -c '
import json,sys,math
d=json.load(sys.stdin); ps=d.get("players",[]); b=[q for q in ps if q.get("name","")=="Second"]; a=[q for q in ps if q.get("name","")!="Second" and q.get("alive")]
if not b: print("none"); sys.exit()
b=b[0]
if not b.get("alive"): print("dead"); sys.exit()
if not a or not b.get("look") or not b.get("pos") or not a[0].get("pos"): print("alive"); sys.exit()
bp,ap=b["pos"],a[0]["pos"]; want=math.degrees(math.atan2(ap[1]-bp[1],ap[0]-bp[0])); e=(b["look"][0]-want+180)%360-180
print("aim %.2f" % abs(e))' 2>/dev/null)
  case "$r" in dead) [ $was = 1 ] && deaths=$((deaths+1)); was=0;; alive) was=1;; aim*) was=1; seen=$((seen+1)); errs="$errs ${r#aim }";; esac
  [ $seen -ge 30 ] && break; done
med=$(python3 -c 'import sys; v=sorted(float(x) for x in sys.argv[1:]); print("%.2f"%v[len(v)//2] if v else -1)' $errs)
echo "server's view of B's aim on A: $seen samples, median error $med deg; B died $deaths time(s)"
python3 -c 'import sys; v=float(sys.argv[1]); sys.exit(0 if 0<=v<=3 else 1)' "$med" && echo "ok   the server sees B aiming at A ($med deg)" || { echo "FAIL aim on the server (median $med)"; fail=1; }
[ "$deaths" = 0 ] && echo "ok   B stayed alive" || { echo "FAIL B died $deaths time(s)"; fail=1; }
wa=$(snap 8766 | python3 -c 'import json,sys; print(json.load(sys.stdin).get("overlay",{}).get("walk_available"))' 2>/dev/null)
[ "$wa" = False ] && echo "ok   no walker offered on a joined client" || { echo "FAIL walk_available on a joined client: $wa"; fail=1; }
curl -s -m 3 -X POST -H X-Reclaimer-Control:1 'http://127.0.0.1:8766/control?do=spar&on=0' >/dev/null; sleep 1
sp=$(snap 8766 | python3 -c 'import json,sys; print(json.load(sys.stdin).get("overlay",{}).get("spar"))' 2>/dev/null)
[ "$sp" = False ] && echo "ok   page switch (spar off)" || { echo "FAIL page switch: spar $sp"; fail=1; }
bash shim/tools/native-spar.sh stop >/dev/null; stop_app "$T/a.out"; bash shim/tools/native-dedicated.sh stop
# ---- 2. the walker in a local game
NATIVE_LOG="$T/w.log" NHOOK_STATS_OPEN=0 NHOOK_NAV=roam PELOAD_NAV_LOG=1 bash shim/native.sh --map guardian --background --seconds 80 > "$T/w.out" 2>&1 &
sleep 84
goals=$(grep -ac 'reached the goal' "$T/w.log"); stats=$(grep -a '\[nav\] at (' "$T/w.log" | sed 's/.*at (\([^)]*\)).*/\1/')
cov=$(echo "$stats" | python3 -c 'import sys,math; p=[list(map(float,l.split()[:2])) for l in sys.stdin if l.strip()]; print("%.1f"%sum(math.hypot(b[0]-a[0],b[1]-a[1]) for a,b in zip(p,p[1:])))' 2>/dev/null)
echo "walker in a local game: $goals goal(s) reached, ${cov:-0} units covered (2 s samples)"
python3 -c 'import sys; sys.exit(0 if int(sys.argv[1])>=1 and float(sys.argv[2] or 0)>=10 else 1)' "$goals" "${cov:-0}" && echo "ok   the walker walks" || { echo "FAIL walker"; fail=1; }
stop_app "$T/w.out"
echo "logs: $T"; [ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
