#!/bin/bash
# Check the generated spots past the boundaries (re-tools/h3spots.py -> maptop/<map>.spots.json) IN THE GAME, map by map:
# our local dedicated server plays the map (Slayer), one minimized test client joins it, the boundaries go off (server +
# the client's own movement), and the server teleports the player to each spot; 5 s and 10 s later the player must be
# alive, standing where it was put (within 3 units, not more than 2 lower) and still (the two readings within 2 units).
# Spots that pass get "verified": true; the rest move to "rejected" with the reason (the stats page lists only the
# verified ones of a checked map). Needs: no game running (one client at a time), the stats ports 8765 / 8767 free.
#   bash shim/tools/spots-verify.sh [map stem ...]      # default: every maptop/*.spots.json with spots not yet checked
#   SPOTS_RECHECK=1 ... checks maps already checked again.     Log: /tmp/spots-verify.log
cd "$(dirname "$0")/../.."
SUP="$HOME/Library/Application Support/ReclaimerMac"; MT="$SUP/maptop"; PFX="$SUP/prefix/drive_c"
SRV="$PFX/ReclaimerTestServer"; WS="$PFX/Program Files/Steam/steamapps/workshop/content/976730"
LOG=/tmp/spots-verify.log; exec > >(tee -a "$LOG") 2>&1
# file stem -> the name the server's config takes
disp(){ case "$1" in armory) echo "Rat's Nest";; bunkerworld) echo Standoff;; chill) echo Narrows;; chillout) echo "Cold Storage";;
  construct) echo Construct;; cyberdyne) echo "The Pit";; deadlock) echo "High Ground";; descent) echo Assembly;; docks) echo Longshore;;
  fortress) echo Citadel;; ghosttown) echo "Ghost Town";; guardian) echo Guardian;; isolation) echo Isolation;; lockout) echo Blackout;;
  midship) echo Heretic;; riverworld) echo Valhalla;; s3d_edge) echo Edge;; s3d_turf) echo Icebox;; s3d_waterfall) echo Waterfall;;
  salvation) echo Epitaph;; sandbox) echo Sandbox;; shrine) echo Sandtrap;; sidewinder) echo Avalanche;; snowbound) echo Snowbound;;
  spacecamp) echo Orbital;; warehouse) echo Foundry;; zanzibar) echo "Last Resort";;
  *) f=$(find "$WS" -name "$1.map" 2>/dev/null | head -1); [ -n "$f" ] || return 1; id=${f#"$WS/"}; echo "workshop:${id%%/*}/$1";; esac; }
srv(){ curl -s -m 3 127.0.0.1:8767/stats.json; }
player(){ srv | python3 -c "import json,sys
d=json.load(sys.stdin); p=d.get('players',[])
print(('%d %.2f %.2f %.2f' % (1 if p[0].get('alive') else 0, *p[0]['pos'])) if p and p[0].get('pos') else 'none')" 2>/dev/null; }
stop_all(){ for p in $(pgrep -f '^[^ ]*build/peload'); do ps -o command= -p $p | grep -q 'dedicated\|local-server\|probe-engine-worker C:\\Rec' || kill -9 -$(ps -o pgid= -p $p | tr -d ' ') 2>/dev/null; done
  pkill -f 'quiet-run.sh' 2>/dev/null; bash shim/dedicated.sh stop >/dev/null 2>&1; sleep 3; }
wait_alive(){ local t=$1; for i in $(seq 1 $t); do set -- $(player); [ "$1" = 1 ] && return 0; sleep 1; done; return 1; }

maps=("$@"); if [ ${#maps[@]} = 0 ]; then for f in "$MT"/*.spots.json; do n=$(basename "$f" .spots.json)
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d.get('spots') and (not d.get('checked') or sys.argv[2]=='1') else 1)" "$f" "${SPOTS_RECHECK:-0}" && maps+=("$n"); done; fi
echo "== $(date '+%H:%M:%S') checking ${#maps[@]} maps: ${maps[*]}"
for m in "${maps[@]}"; do
  f="$MT/$m.spots.json"; name=$(disp "$m") || { echo "$m: no server name for it, skipped"; continue; }
  stop_all
  { sed '/^\[\[server\]\]/,$d' "$SRV/samemap.toml"; printf '[[server]]\nname = "Spot check"\nport = 49190\nmap = "%s"\ngame = "Slayer"\n' "$name"; } > "$SRV/spotcheck.toml"
  DED_CONFIG=spotcheck.toml bash shim/dedicated.sh >/dev/null 2>&1
  up=0; for i in $(seq 1 60); do srv >/dev/null 2>&1 && { up=1; break; }; grep -a -q '^Error:' /tmp/reclaimer_dedicated.log 2>/dev/null && break; sleep 2; done
  [ $up = 1 ] || { echo "$m ($name): the server did not start ($(grep -a -m1 '^Error:' /tmp/reclaimer_dedicated.log | cut -c1-100)) -- not checked"; continue; }
  ok=0
  for try in 1 2; do
    QR_MINIMIZE=1 QR_STATS_OPEN=0 nohup bash shim/tools/quiet-run.sh "PELOAD_GL_HELPER=1 PELOAD_ALLOW_TELEPORT=1 PELOAD_TELEPORT_HOST=127.0.0.1:8767" 900 "reclaimer://join?server=127.0.0.1:49190" >/dev/null 2>&1 &
    sleep 15; if wait_alive 200; then ok=1; break; fi
    echo "$m: join $try failed ($(grep -a -E 'Joining server failed|join timed out|startup exception' /tmp/quiet-run.log | tail -1 | cut -c1-90))"
    for p in $(pgrep -f '^[^ ]*build/peload'); do ps -o command= -p $p | grep -q 'dedicated\|local-server\|probe-engine-worker C:\\Rec' || kill -9 -$(ps -o pgid= -p $p | tr -d ' ') 2>/dev/null; done; sleep 50
  done
  [ $ok = 1 ] || { echo "$m ($name): no player on the server -- not checked"; continue; }
  sleep 5; curl -s -X POST -H 'X-Reclaimer-Control: 1' 'http://127.0.0.1:8765/control?do=bounds&off=1' >/dev/null; sleep 3
  res=$(mktemp)
  python3 -c "import json,sys; [print(s['id'],s['x'],s['y'],s['z'],s['label'].replace(' ','_')) for s in json.load(open(sys.argv[1]))['spots']]" "$f" |
  while read -r id x y z lab; do
    wait_alive 30 || { echo "$id dead -" >> "$res"; continue; }
    nl=$(cat /tmp/reclaimer_dedicated_*.log 2>/dev/null | wc -l)
    curl -s -X POST -H 'X-Reclaimer-Control: 1' "http://127.0.0.1:8767/control?do=tpto&x=$x&y=$y&z=$z&slot=0" >/dev/null
    sleep 5; a=$(player); sleep 5; b=$(player)
    refused=$(cat /tmp/reclaimer_dedicated_*.log 2>/dev/null | tail -n +$((nl+1)) | grep -a -c 'refused')
    v=$(python3 - "$x" "$y" "$z" "$a" "$b" "$refused" <<'EOF'
import sys, math
x, y, z = map(float, sys.argv[1:4]); a, b = sys.argv[4].split(), sys.argv[5].split(); refused = int(sys.argv[6])
if refused: print('no refused_outside_the_map_structure'); sys.exit()
if len(a) < 4 or len(b) < 4: print('no no_reading'); sys.exit()
if a[0] != '1' or b[0] != '1': print('no died'); sys.exit()
ax, ay, az = map(float, a[1:]); bx, by, bz = map(float, b[1:])
if math.hypot(ax - x, ay - y) > 3: print('no not_there_(%.1f_away)' % math.hypot(ax - x, ay - y)); sys.exit()
if az < z - 2: print('no fell_%.1f' % (z - az)); sys.exit()
if math.hypot(bx - ax, by - ay) > 2 or abs(bz - az) > 2: print('no slid_%.1f' % math.hypot(bx - ax, by - ay)); sys.exit()
print('yes standing_at_%.1f_%.1f_%.1f' % (bx, by, bz))
EOF
)
    echo "$id $v" >> "$res"; echo "  $m $id ${lab//_/ }: $v"
  done
  python3 - "$f" "$res" <<'EOF'
import json, sys, time
f, r = sys.argv[1], sys.argv[2]; d = json.load(open(f)); res = {}
for line in open(r):
    p = line.split()
    if len(p) >= 3: res[p[0]] = (p[1] == 'yes', p[2].replace('_', ' '))
keep, rej = [], list(d.get('rejected', []))
for s in d['spots']:
    ok, why = res.get(s['id'], (False, 'not checked'))
    if ok: s['verified'] = True; s['check'] = why; keep.append(s)
    else: s['verified'] = False; s['why'] = why; rej.append(s)
d['spots'], d['rejected'], d['checked'] = keep, rej, time.strftime('%Y-%m-%d %H:%M')
json.dump(d, open(f, 'w'), indent=1)
print('%s: %d verified, %d rejected' % (d['map'], len(keep), len(rej)))
EOF
  rm -f "$res"
done
stop_all; echo "== $(date '+%H:%M:%S') done"
