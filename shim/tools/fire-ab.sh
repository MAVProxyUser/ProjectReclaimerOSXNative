#!/bin/bash
# A live A/B of one walker knob on the public walker (2026-11-09): windows of <secs> alternating A, B, A, B ... for <cycles> cycles,
# each window measured from the walker's stats page (the scoreboard's shots / hits / kills / deaths) and its [firegate] lines (seconds
# with the trigger down); a window that spans a match change is void. The sums per side at the end. The death tuner is paused for the
# run (NHOOK_TUNE 0) and the knob put back after. (Public matches change every 10-15 minutes: two long windows kept crossing one.)
#   bash shim/tools/fire-ab.sh <KNOB> <A> <B> [secs 60] [cycles 3] [nick: the public walker's] [stats http://127.0.0.1:8771]
set -u
K=$1; VA=$2; VB=$3; SECS=${4:-60}; CYC=${5:-3}; NICK=${6:-$(python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import walkerrec; print(walkerrec.pubwalker_nick())' "$(dirname "$0")")}; ST=${7:-http://127.0.0.1:8771}
S="$HOME/Library/Application Support/ReclaimerMac"; KF="$S/battles/live-knobs/$NICK.json"; L="$S/battles/aiwalker.log"
OUT=$(mktemp /tmp/fire-ab.XXXXXX)
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
tune=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('NHOOK_TUNE', '-'))" "$KF" NHOOK_TUNE)
setk 0 NHOOK_TUNE; trap 'setk "$old"; setk "$tune" NHOOK_TUNE; rm -f "$OUT"' EXIT
win() { setk "$1"; sleep 3; local n0=$(grep -a -c '' "$L") a=$(snap); sleep "$SECS"; local b=$(snap)
  tail -n +"$n0" "$L" | grep -a '\[firegate\]' | python3 -c "
import sys,re
tot=0
for l in sys.stdin:
    m=re.search(r'of the (\d+) it could see: fire allowed (\d+)%.*trigger down in (\d+)% of the allowed', l)
    if m: tot+=int(m[1])*int(m[2])/100*int(m[3])/100*0.033
a='$a'.split(); b='$b'.split(); ds=int(b[0])-int(a[0]); dh=int(b[1])-int(a[1])
if a[4:] != b[4:] or ds < 0: print('$1 void', a[4:], b[4:]); sys.exit(0)
print('$1 ok %d %d %d %d %.2f %s' % (ds, dh, int(b[2])-int(a[2]), int(b[3])-int(a[3]), tot, b[4]))" | tee -a "$OUT"; }
for c in $(seq 1 "$CYC"); do win "$VA"; win "$VB"; done
python3 - "$OUT" "$K" "$VA" "$VB" <<'PY'
import sys
f,k,va,vb=sys.argv[1:5]; S={va:[0,0,0,0,0.0,0], vb:[0,0,0,0,0.0,0]}
for l in open(f):
    p=l.split()
    if len(p) >= 7 and p[1]=='ok' and p[0] in S:
        s=S[p[0]]; s[0]+=int(p[2]); s[1]+=int(p[3]); s[2]+=int(p[4]); s[3]+=int(p[5]); s[4]+=float(p[6]); s[5]+=1
for v in (va, vb):
    s=S[v]; print('%s=%s: %d windows, shots %d, accuracy %.3f, %.1f hits a trigger second, kills %d deaths %d' % (k, v, s[5], s[0], s[1]/max(1,s[0]), s[1]/max(0.1,s[4]), s[2], s[3]))
PY
echo "$K back to $old; the tuner back on"
