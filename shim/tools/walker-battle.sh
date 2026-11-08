#!/bin/bash
# Walkers in a free-for-all (2026-11-08; the user: "make 3-4 walkers have a battle, and see how good you can get them vs each other
# in a rumble pit match"). Our LOCAL-ONLY dedicated server (shim/tools/native-dedicated.sh: Guardian Slayer, no teams) + N test
# clients (the test identities test-v2, test-second, test-third, test-fourth; nothing else), each one a walker: the virtual
# controller (NHOOK_VPAD: its moves reach the server), the sparring aim, the walker hunting the nearest enemy (NHOOK_NAV=enemy),
# firing (NHOOK_BOT_FIRE), sound muted. Every 10 s the server's own view (its stats page, 8767): kills, deaths, shots, hits per
# walker; at the end a scoreboard, and every sample as JSON lines in ~/Library/Application Support/ReclaimerMac/battles/.
#   bash shim/tools/walker-battle.sh [N walkers 2..4, default 4] [seconds, default 300] [extra env for every walker...]
#   W0_ENV..W3_ENV="NHOOK_BOT_SKILL=0.8 ..." = settings for one walker (shim/tools/walker-train.py evolves them)
#   bash shim/tools/walker-battle.sh init       (once: makes the identities test-third / test-fourth -- the app's first start)
# Refuses to start beside another Project Reclaimer (the user's game included).
set -u
cd "$(dirname "$0")/../.."
SUP="$HOME/Library/Application Support/ReclaimerMac/native"; APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
BT="$HOME/Library/Application Support/ReclaimerMac/battles"; mkdir -p "$BT"
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting"; exit 1; fi
NAMES=(test-v2 test-second test-third test-fourth); NICK=("" Second Third Fourth); PORTS=(8765 8766 8768 8769)
if [ "${1:-}" = init ]; then
  for k in 2 3; do d="$SUP/${NAMES[$k]}"; [ -f "$d/profile.json" ] && { echo "${NAMES[$k]}: exists"; continue; }
    mkdir -p "$d"; echo "${NAMES[$k]}: the app's first start makes its identity"
    cp "$SUP/test-second/mcc-folder.txt" "$d/mcc-folder.txt" 2>/dev/null   # (else it stops at "Where is Halo: The Master Chief Collection?")
    NATIVE_LOG="$BT/init-${NAMES[$k]}.log" NATIVE_PROFILE="$d" RECLAIMER_CACHE_DIR="$d-cache" PELOAD_FORENSIC_DIR="$d-forensics" NHOOK_HEATMAP_DIR="$d-heatmaps" \
      NHOOK_STATS_OPEN=0 NHOOK_MUTE_OUTPUT=1 NATIVE_SECS=30 bash shim/native.sh --map guardian --seconds 25 --background > "$BT/init-${NAMES[$k]}.out" 2>&1
    [ -f "$d/profile.json" ] || { echo "  no profile.json made"; continue; }
    python3 - "$d/profile.json" "${NICK[$k]}" <<'EOF'
import json, os, sys
p, n = sys.argv[1], sys.argv[2]; d = json.load(open(p)); d.setdefault('identity', {})['nickname'] = n
t = p + '.tmp'; json.dump(d, open(t, 'w'), indent=2); os.chmod(t, 0o600); os.replace(t, p); print('  named', n)
EOF
  done; exit 0; fi
N=${1:-4}; SECS=${2:-300}; shift 2 2>/dev/null; EXTRA=("$@")
[ "$N" -ge 2 ] && [ "$N" -le 4 ] || { echo "2..4 walkers"; exit 1; }
for k in $(seq 0 $((N - 1))); do [ $k -lt 2 ] || [ -f "$SUP/${NAMES[$k]}/profile.json" ] || { echo "${NAMES[$k]} missing: bash shim/tools/walker-battle.sh init"; exit 1; }; done
T=$(mktemp -d "${TMPDIR:-/tmp}/walker-battle.XXXXXX"); OUT="$BT/$(date +%Y%m%d-%H%M%S)-n$N.jsonl"
bash shim/tools/native-dedicated.sh start >/dev/null || exit 1
for k in $(seq 0 $((N - 1))); do d="$SUP/${NAMES[$k]}"
  WE=$(eval echo "\${W${k}_ENV:-}")   # (per walker: W0_ENV .. W3_ENV, e.g. "NHOOK_BOT_SKILL=0.8 NHOOK_BOT_DIST=0.7": walker-train.py)
  env $WE NATIVE_LOG="$T/w$k.log" NATIVE_PROFILE="$d" RECLAIMER_CACHE_DIR="$d-cache" PELOAD_FORENSIC_DIR="$d-forensics" NHOOK_HEATMAP_DIR="$d-heatmaps" \
    NHOOK_STATS_HTTP="${PORTS[$k]}" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_MUTE_OUTPUT=1 \
    NHOOK_VPAD=1 NHOOK_SPAR=1 NHOOK_NAV=enemy NHOOK_BOT_FIRE=1 NHOOK_BOT_FIGHT=1 NHOOK_SPAR_LOG=1 PELOAD_NAV_LOG=1 ${EXTRA[@]+"${EXTRA[@]}"} \
    bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/w$k.out" 2>&1 &
  sleep 6; done
echo "battle: $N walkers, $SECS s; logs $T; samples -> $OUT"
# the server's view recorded like the real players' (match-recorder.py -> battles/tracks: walker-model.py compares them)
WT="${WB_TRACKS:-$BT/tracks}"; mkdir -p "$WT"; python3 shim/tools/match-recorder.py --port 8767 --seconds "$SECS" --out "$WT" > "$T/rec.txt" 2>&1 & RECP=$!   # (WB_TRACKS: another folder, e.g. one set of settings)
t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt "$SECS" ]; do sleep 10
  curl -s -m 5 http://127.0.0.1:8767/stats.json | python3 -c '
import json, sys, time
d = json.load(sys.stdin); ps = [{k: p.get(k) for k in ("slot", "name", "alive", "kills", "deaths", "shots", "hits", "accuracy", "pos", "weapon")} for p in d.get("players", [])]
print(json.dumps({"t": time.time(), "in_match": d.get("in_match"), "map": d.get("map"), "players": ps}))' >> "$OUT" 2>/dev/null
done
python3 - "$OUT" <<'EOF'
import json, sys
L = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
if not L: print('no samples (the server page did not answer)'); sys.exit()
last = L[-1]['players']; print('scoreboard (the server\'s view):')
print('  %-16s %5s %6s %5s %5s %6s' % ('player', 'kills', 'deaths', 'shots', 'hits', 'acc'))
for p in sorted(last, key=lambda p: -(p.get('kills') or 0)):
    s, h = p.get('shots') or 0, p.get('hits') or 0
    print('  %-16s %5s %6s %5s %5s %5.0f%%' % ((p.get('name') or '?')[:16], p.get('kills'), p.get('deaths'), s, h, 100.0 * h / s if s else 0))
moved = {}
for e in L:
    for p in e['players']:
        q = p.get('pos'); n = p.get('name')
        if q and n: moved.setdefault(n, []).append(q)
for n, qs in moved.items():
    d = sum(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(qs, qs[1:])); print('  %-16s moved %.0f units (10 s samples)' % (n[:16], d))
EOF
for k in $(seq 0 $((N - 1))); do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/w$k.out"); [ -n "$P" ] && kill "$P" 2>/dev/null; done; sleep 3
for k in $(seq 0 $((N - 1))); do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/w$k.out"); [ -n "$P" ] && kill -9 "$P" 2>/dev/null; done
kill $RECP 2>/dev/null; wait $RECP 2>/dev/null
bash shim/tools/native-dedicated.sh stop >/dev/null; echo "logs: $T"
