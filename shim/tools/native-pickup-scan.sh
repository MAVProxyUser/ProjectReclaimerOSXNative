#!/bin/bash
# Which control-record button picks a weapon up? (2026-11-08: the walkers never pick weapons up; real players hold a picked-up
# Carbine 44% of the time.) A LOCAL test match (the test profile, muted, background) on Guardian: for each candidate bit of the
# control record's +0xC8 buttons (not 1 jump, 19 / 21 fire, 30 grenade), the player is put on a weapon lying on the floor that it
# does not hold (the host tools' tpto: a local game is its own host), the bit is held 1.5 s (do=botbtn), and the weapon in hand is
# read again. Prints the bit(s) that swapped it.   [PS_BITS="32 33 ..."] bash shim/tools/native-pickup-scan.sh (default: every bit
# of the 64 but 1, 19, 21, 30)
set -u
cd "$(dirname "$0")/../.."
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-pickup.XXXXXX")
NATIVE_LOG="$T/app.log" NHOOK_STATS_OPEN=0 NHOOK_MUTE_OUTPUT=1 NATIVE_SECS=${PS_SECS:-330} bash shim/native.sh --map guardian --seconds $((${PS_SECS:-330} - 5)) --background > "$T/out" 2>&1 &
for i in $(seq 1 90); do curl -s -m 2 http://127.0.0.1:8765/stats.json 2>/dev/null | grep -q '"in_match":true' && break; sleep 1; done; sleep 8
ctl() { curl -s -m 3 -X POST -H 'X-Reclaimer-Control: 1' "http://127.0.0.1:8765/control?$1" >/dev/null; }
mine() { curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c 'import json,sys; d=json.load(sys.stdin); p=[q for q in d["players"] if q.get("you")]; print(p[0].get("weapon") if p else "")'; }
spot() { python3 -c '
import json, sys, urllib.request
get = lambda u: json.load(urllib.request.urlopen("http://127.0.0.1:8765/" + u, timeout=3))
d, h = get("stats.json"), get("heat.json"); sp = h.get("spawns") or []
me = [q for q in d["players"] if q.get("you")]; w = (me[0].get("weapon") or "").lower() if me else ""
for it in d.get("items") or []:   # [spawn point, kind (0 weapon), state, x, y, z, holder (-1 = on the floor), name when no spawn point]
    if it[1] != 0 or it[6] != -1: continue
    n = str(it[7]) if len(it) > 7 else (str(sp[it[0]][4]) if 0 <= it[0] < len(sp) else "")
    if not n or "grenade" in n.lower() or n.lower() in w or w in n.lower(): continue
    print("%.2f %.2f %.2f %s" % (it[3], it[4], it[5], n.replace(" ", "_"))); break'; }
pos() { curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c 'import json,sys; d=json.load(sys.stdin); p=[q for q in d["players"] if q.get("you")]; print(" ".join("%.1f" % v for v in (p[0].get("pos") or [])) if p else "")'; }
found=""
for k in ${PS_BITS:-0 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 20 22 23 24 25 26 27 28 29 31 $(seq 32 63)}; do
  s=$(spot); [ -z "$s" ] && { echo "no weapon on the floor to try"; break; }
  set -- $s; w0=$(mine); ctl "do=tpto&x=$1&y=$2&z=$(python3 -c "print($3+0.3)")"; sleep 1.5; echo "  at $(pos) (weapon at $1 $2 $3)"
  ctl "do=botbtn&bits=$(python3 -c "print('%x' % (1 << $k))")&secs=1.5"; sleep 2.2; w1=$(mine)
  echo "bit $k on $4: $w0 -> $w1"; [ "$w0" != "$w1" ] && [ -n "$w1" ] && { found="$found $k"; }
done
echo "pickup bit(s):${found:- none found}"
P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/out"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }; rm -rf "$T"
