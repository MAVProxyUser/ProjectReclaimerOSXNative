#!/bin/bash
# Do the walkers melee? (2026-11-08; the user watched a walker hold a Gravity Hammer on an enemy at arm's length without a swing.)
# A walker battle on our local server with a melee weapon as every player's start (DED_OPTIONS primary_weapon: 18 Gravity hammer,
# 6 Energy sword, 0 Battle rifle ...; game-options.md), then the walkers' [melee] lines and the server's kills by type (the
# battle's recording, battles/tracks). PASS when hammer / sword / melee kills happened and the walkers swung.
#   bash shim/tools/melee-battle.sh [walkers 2] [seconds 180] [primary weapon 18] [secondary 1]
# NHOOK_BOT_NOMELEE=1 in the environment = the old melee, for an A/B; MELEE_ONLY=1 = no shots, no grenades (every kill a melee). Never beside a running Project Reclaimer (walker-battle.sh
# refuses).
set -u
cd "$(dirname "$0")/../.."
N=${1:-2}; SECS=${2:-180}; PW=${3:-18}; SW=${4:-1}
# (the walkers keep their start weapons: NHOOK_BOT_NOPICK -- they swapped the hammer for a shotgun / sniper rifle otherwise;
#  NHOOK_BOT_NOMELEE=1 in this script's environment reaches them too, for an A/B)
WX=(NHOOK_BOT_NOPICK=1); [ -n "${NHOOK_BOT_NOMELEE:-}" ] && WX+=(NHOOK_BOT_NOMELEE="$NHOOK_BOT_NOMELEE")
# MELEE_ONLY=1: the walkers never fire and never throw grenades -- every kill is then a melee (the button test: does the virtual
# pad's melee button melee?); MELEEPAD=<hex> tries another button (nvpad.m bits: 2 B, 0x20 RB, 0x80 right stick click)
[ "${MELEE_ONLY:-0}" = 1 ] && WX+=(NHOOK_BOT_FIRE=0 NHOOK_BOT_GRENX=1000)
[ -n "${MELEEPAD:-}" ] && WX+=(NHOOK_BOT_MELEEPAD="$MELEEPAD")
OUT=$(DED_OPTIONS="{\"player.primary_weapon\": $PW, \"player.secondary_weapon\": $SW}" bash shim/tools/walker-battle.sh "$N" "$SECS" "${WX[@]}" 2>&1)
echo "$OUT" | grep -v '^\[' | tail -8
T=$(echo "$OUT" | sed -n 's/^logs: //p' | tail -1); [ -d "${T:-/nonexistent}" ] || { echo "melee-battle: no battle logs"; exit 1; }
REC=$(sed -n 's/.*-> \([^ ]*\.jsonl\.gz\).*/\1/p' "$T/rec.txt" | tail -1)
F="$HOME/Library/Application Support/ReclaimerMac/battles/tracks/$REC"
swings=$(cat "$T"/w*.log 2>/dev/null | grep -a -c '\[melee\]')
echo "walkers' melee presses / swings: $swings"; cat "$T"/w*.log 2>/dev/null | grep -a '\[melee\]' | head -5 | cut -c1-160
python3 - "$F" "$swings" <<'EOF'
# every kill: its type, what the killer held and the distance at the victim's last live sample. Our local server names melee and
# hammer kills "" (unnamed; public servers say "Melee" / "Gravity hammer"): an unnamed kill within 4.5 units counts as melee.
import bisect, collections, gzip, json, math, sys
S, E = [], []
try:
    for l in gzip.open(sys.argv[1], 'rt'):
        o = json.loads(l)
        if 'p' in o: S.append(o)
        elif o.get('ev') == 'kill': E.append(o)
except (OSError, EOFError, ValueError): pass
T = [s['t'] for s in S]; c = collections.Counter(); mel = 0
for e in E:
    i = bisect.bisect_right(T, e['t']) - 1; ra = rb = None
    for j in range(i, max(-1, i - 40), -1):
        rb = [r for r in S[j]['p'] if r[0] == e['b'] and r[2]]
        if rb: ra = [r for r in S[j]['p'] if r[0] == e['a']]; break
    d = math.dist(ra[0][3:6], rb[0][3:6]) if ra and rb else 99.0
    how = e.get('how') or ''; c[how or '(unnamed)'] += 1
    m = how in ('Melee', 'Gravity hammer', 'Energy sword') or (how == '' and d <= 4.5) or __import__('os').environ.get('MELEE_ONLY') == '1'; mel += m
    print('  %-14s killer holding %-16s at %5.2f units%s' % (how or '(unnamed)', ra[0][10] if ra else '?', d, '  <- melee' if m else ''))
print('kills by type: %s' % dict(c))
ok = mel > 0 and int(sys.argv[2]) > 0
print('melee battle: %s (%d melee / hammer / sword kills, %s presses)' % ('PASS' if ok else 'FAIL', mel, sys.argv[2])); sys.exit(0 if ok else 1)
EOF
