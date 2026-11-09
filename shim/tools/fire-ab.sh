#!/bin/bash
# A live A/B of one walker knob on the public walker (2026-11-09): <secs> with the knob at A, then <secs> at B, each window measured
# from the walker's stats page (the scoreboard's shots / hits) and its [firegate] lines (seconds with the trigger down). The death tuner
# is paused for the run (NHOOK_TUNE 0) and the knob put back after.
#   bash shim/tools/fire-ab.sh <KNOB> <A> <B> [secs 90] [nick AIWalkerTest] [stats http://127.0.0.1:8771]
set -u
K=$1; VA=$2; VB=$3; SECS=${4:-90}; NICK=${5:-AIWalkerTest}; ST=${6:-http://127.0.0.1:8771}
S="$HOME/Library/Application Support/ReclaimerMac"; KF="$S/battles/live-knobs/$NICK.json"; L="$S/battles/aiwalker.log"
setk() { python3 - "$KF" "${2:-$K}" "$1" <<'PY'
import json,os,sys
p,k,v=sys.argv[1],sys.argv[2],sys.argv[3]; d=json.load(open(p))
if v=='-': d.pop(k,None)
else: d[k]=float(v)
t=p+'.ab.tmp'; open(t,'w').write(json.dumps(d)+'\n'); os.replace(t,p)
PY
}
snap() { curl -s -m 3 "$ST/stats.json" | python3 -c 'import json,sys; d=json.load(sys.stdin); me=[p for p in d["players"] if p.get("you")][0]; print(me["shots"], me["hits"], me["kills"], me["deaths"], d.get("map"))'; }
old=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get(sys.argv[2], '-'))" "$KF" "$K")
win() { setk "$1"; sleep 3; local n0=$(grep -a -c '' "$L") a=$(snap); sleep "$SECS"; local b=$(snap)
  tail -n +"$n0" "$L" | grep -a '\[firegate\]' | python3 -c "
import sys,re
tot=0
for l in sys.stdin:
    m=re.search(r'of the (\d+) it could see: fire allowed (\d+)%.*trigger down in (\d+)% of the allowed', l)
    if m: tot+=int(m[1])*int(m[2])/100*int(m[3])/100*0.033
a='$a'.split(); b='$b'.split(); ds=int(b[0])-int(a[0]); dh=int(b[1])-int(a[1])
if a[4:] != b[4:] or ds < 0: print('$K=$1: window VOID -- the match changed (%s -> %s)' % (a[4:], b[4:])); sys.exit(0)
print('$K=$1: shots %d (%.1f per trigger second, %.1f s down), accuracy %.3f, kills +%d deaths +%d, map %s' % (ds, ds/max(0.1,tot), tot, dh/max(1,ds), int(b[2])-int(a[2]), int(b[3])-int(a[3]), b[4]))"; }
tune=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('NHOOK_TUNE', '-'))" "$KF"); setk 0 NHOOK_TUNE
trap 'setk "$old"; setk "$tune" NHOOK_TUNE' EXIT
win "$VA"; win "$VB"; echo "$K back to $old; the tuner back on"
