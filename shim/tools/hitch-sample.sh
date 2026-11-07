#!/bin/bash
# Sample a game client while it hitches: follows the newest per-second performance log (latency.c, perf/*.csv) and, when a
# second in a match falls below MINFPS (default 12), runs macOS `sample` on the process that wrote it for 3 s (at most N times,
# default 3, 20 s apart). Then: python3 shim/tools/sample-threads.py <out>/hitch_<n>.txt
#     bash shim/tools/hitch-sample.sh <out dir> [MINFPS] [N]
set -u
OUT="${1:?usage: hitch-sample.sh <out dir> [minfps] [n]}"; MIN="${2:-12}"; N="${3:-3}"; mkdir -p "$OUT"
DIR="$HOME/Library/Application Support/ReclaimerMac/perf"; START=$(date +%s); F=""
while [ -z "$F" ]; do F=$(ls -t "$DIR"/*.csv 2>/dev/null | head -1); [ -n "$F" ] && [ $(stat -f %m "$F") -ge $START ] || F=""; sleep 2; done
PID=$(basename "$F" .csv | awk -F_ '{print $NF}'); n=0; last=0; echo "following $F (pid $PID)"
tail -n 0 -F "$F" 2>/dev/null | while IFS=, read -r t up inm map pl fps rest; do
  kill -0 "$PID" 2>/dev/null || break
  [ "$inm" = 1 ] || continue; now=$(date +%s)
  if awk -v f="$fps" -v m="$MIN" 'BEGIN{exit !(f<m)}' && [ $((now-last)) -ge 20 ]; then
    n=$((n+1)); last=$now; echo "$t: $fps fps -> sample $n"; sample "$PID" 3 -file "$OUT/hitch_$n.txt" > /dev/null 2>&1
    [ $n -ge "$N" ] && break; fi
done
