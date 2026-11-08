#!/bin/bash
# Which control-record button picks a weapon up? (2026-11-08: the walkers never pick weapons up; real players hold a picked-up
# Carbine 44% of the time.) A LOCAL test match (the test profile, muted, background) on Guardian: for each candidate bit of the
# control record's +0xC8 buttons (not 1 jump, 19 / 21 fire, 30 grenade), the player is put on a weapon lying on the floor that it
# does not hold (the host tools' tpto: a local game is its own host), the bit is held 1.5 s (do=botbtn), and the weapon in hand is
# read again. Prints the bit(s) that swapped it.   bash shim/tools/native-pickup-scan.sh
set -u
cd "$(dirname "$0")/../.."
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-pickup.XXXXXX")
NATIVE_LOG="$T/app.log" NHOOK_STATS_OPEN=0 NHOOK_MUTE_OUTPUT=1 NATIVE_SECS=200 bash shim/native.sh --map guardian --seconds 195 --background > "$T/out" 2>&1 &
for i in $(seq 1 90); do curl -s -m 2 http://127.0.0.1:8765/stats.json 2>/dev/null | grep -q '"in_match":true' && break; sleep 1; done; sleep 8
ctl() { curl -s -m 3 -X POST -H 'X-Reclaimer-Control: 1' "http://127.0.0.1:8765/control?$1" >/dev/null; }
mine() { curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c 'import json,sys; d=json.load(sys.stdin); p=[q for q in d["players"] if q.get("you")]; print(p[0].get("weapon") if p else "")'; }
spot() { curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c '
import json, sys
d = json.load(sys.stdin); me = [q for q in d["players"] if q.get("you")]; w = (me[0].get("weapon") or "").lower() if me else ""
for it in d.get("items") or []:
    n = str(it[-1]); x, y, z = it[3], it[4], it[5]
    if "grenade" in n.lower() or n.lower() == w or n.lower() in w: continue
    print("%.2f %.2f %.2f %s" % (x, y, z, n.replace(" ", "_"))); break'; }
found=""
for k in 0 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 20 22 23 24 25 26 27 28 29 31; do
  s=$(spot); [ -z "$s" ] && { echo "no weapon on the floor to try"; break; }
  set -- $s; w0=$(mine); ctl "do=tpto&x=$1&y=$2&z=$(python3 -c "print($3+0.3)")"; sleep 1.5
  ctl "do=botbtn&bits=$(printf '%x' $((1 << k)))&secs=1.5"; sleep 2.2; w1=$(mine)
  echo "bit $k on $4: $w0 -> $w1"; [ "$w0" != "$w1" ] && [ -n "$w1" ] && { found="$found $k"; }
done
echo "pickup bit(s):${found:- none found}"
P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/out"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }; rm -rf "$T"
