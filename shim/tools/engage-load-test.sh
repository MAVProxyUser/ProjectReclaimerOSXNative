#!/bin/bash
# Offline test of the fighter's engagement model loader (shim/native/nbot_engage.c, 2026-11-08): nbot_engage.c built alone with
# stubs, eg_load() on an engage-model.json (default ReclaimerMac/tracks/engage-model.json), and the same numbers read by Python:
# the 8 target weights, every weapon class's distance band, the second-attacker odds and delay, the number of movement pieces
# (and one piece's 20 values), the weapon classes of some tag names, eg_band's 20-duel rule.
#   bash shim/tools/engage-load-test.sh [model.json]      -> PASS / FAIL lines, exit 0 when all pass
set -u
cd "$(dirname "$0")/../.."
M="${1:-$HOME/Library/Application Support/ReclaimerMac/tracks/engage-model.json}"
[ -f "$M" ] || { echo "no model at $M (python3 shim/tools/engage-model.py first)"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/engage-load-test.XXXXXX"); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdarg.h>
typedef struct { float c[3], r, o[3], vel[3]; int type; uint32_t handle; uint64_t p; uint32_t def; uint16_t nmsz, nmoff;
                 float health, shield, camo; int team, seat, wslot, dead; uint32_t weapons[4], parent, owner; int8_t gren[4]; int rcount; uint16_t roff; } NovObj;
static void nlog(const char* fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
static int hb_read(uint64_t p, void* out, size_t n) { (void)p; (void)out; (void)n; return 0; }
static float bot_wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static int bot_enemy(const NovObj* a, const NovObj* b) { return a->team < 0 || a->team != b->team; }
static float bot_rand01(void) { return 0.5f; }
static int bot_log(void) { return 0; }
#include "nbot_engage.c"
static int rd_contact(const NovObj* a, const NovObj* b) { (void)a; (void)b; return 0; }   // (nbot_goals.c's radar: not needed here)
int main(void) {
    eg_load();
    printf("W"); for (int i = 0; i < 8; i++) printf(" %.4f", g_eg.w[i]); printf("\n");
    for (int i = 0; i < g_eg.nband; i++) printf("B %s %d %.3f %.3f %.3f\n", g_eg.band[i].cls, g_eg.band[i].n, g_eg.band[i].p25, g_eg.band[i].med, g_eg.band[i].p75);
    printf("S %.4f %.4f %.4f\n", g_eg.p_view, g_eg.p_behind, g_eg.turn_s);
    printf("P %d\n", g_eg.npiece);
    printf("H %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n", g_eg.rd_turn, g_eg.rd_turn_s, g_eg.p_killer, g_eg.p_spot, g_eg.it_near, g_eg.it_mid, g_eg.it_far);
    if (g_eg.npiece) { printf("Q %s", g_eg.lib[0].grp); for (int k = 0; k < 20; k++) printf(" %.3f", g_eg.lib[0].lat[k]); printf("\n"); }
    const char* tags[] = {"objects\\weapons\\rifle\\battle_rifle\\battle_rifle", "objects\\weapons\\rifle\\assault_rifle\\assault_rifle", "objects\\weapons\\melee\\energy_blade\\energy_blade",
                          "objects\\weapons\\rifle\\sniper_rifle\\sniper_rifle", "objects\\weapons\\support_high\\rocket_launcher\\rocket_launcher", "objects\\weapons\\pistol\\needler\\needler", 0};
    for (int i = 0; tags[i]; i++) printf("C %s\n", eg_wcls(tags[i]));
    float a, b; for (int i = 0; i < g_eg.nband; i++) printf("E %s %d\n", g_eg.band[i].cls, eg_band(g_eg.band[i].cls, &a, &b));
    return 0; }
EOF
cp shim/native/nbot_engage.c "$T/"
clang -O1 -Wall -Wno-unused-function -o "$T/t" "$T/t.c" 2>&1 | grep -v '^$' | head -5
[ -x "$T/t" ] || { echo "FAIL build"; exit 1; }
NHOOK_BOT_ENGAGE="$M" "$T/t" 2>"$T/log" > "$T/out"; sed 's/^/  /' "$T/log"
python3 - "$M" "$T/out" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1])); L = open(sys.argv[2]).read().splitlines(); fails = 0
def check(what, ok, got=''):
    global fails; print('%s %s %s' % ('PASS' if ok else 'FAIL', what, got)); fails += 0 if ok else 1
FE = ['log_dist', 'off_aim', 'attacked_me', 'vitality', 'previous_target', 'teammate_on_it', 'just_hurt', 'height']
w = [float(x) for x in next(l for l in L if l.startswith('W')).split()[1:]]
check('target weights', all(abs(w[i] - (m['target'] or {}).get(k, 0)) < 1e-3 for i, k in enumerate(FE)), str(w))
B = {l.split()[1]: l.split()[2:] for l in L if l.startswith('B ')}
check('distance bands: every class', set(B) == set(m['distance_by_weapon']), '%d of %d' % (len(B), len(m['distance_by_weapon'])))
check('distance bands: numbers', all(abs(float(B[k][1]) - v['p25']) < 1e-2 and abs(float(B[k][3]) - v['p75']) < 1e-2 and int(B[k][0]) == v['n'] for k, v in m['distance_by_weapon'].items() if k in B))
s = [float(x) for x in next(l for l in L if l.startswith('S')).split()[1:]]; sa = m['multi']['second_attacker']
check('second attacker odds and delay', abs(s[0] - sa['turned_if_new_one_in_view'][0]) < 1e-3 and abs(s[1] - sa['turned_if_new_one_behind'][0]) < 1e-3 and abs(s[2] - sa['turn_s_median']) < 1e-3, str(s))
n = int(next(l for l in L if l.startswith('P')).split()[1]); want = sum(len(v) for v in m['library'].values())
check('movement pieces', n == min(want, 1200), '%d of %d' % (n, want))
q = next((l for l in L if l.startswith('Q ')), None)
if q:
    g = q.split()[1]; vals = [float(x) for x in q.split()[2:]]; first = m['library'][g][0]['lat']
    check('a piece\'s 20 values', all(abs(a - (b or 0)) < 1e-3 for a, b in zip(vals, first)) and len(vals) == 20)
bh = m.get('behaviour') or {}
if bh:
    h = [float(x) for x in next(l for l in L if l.startswith('H ')).split()[1:]]; rd, rv, it = bh.get('radar') or {}, bh.get('revenge') or {}, bh.get('items') or {}
    want = [rd.get('turn_moving_in_range', 0.28), rd.get('turn_s_median', 1.24),
            max(0.05, rv['next_target_killer'] - rv['chance']) if rv else 0.05, max(0.08, rv['back_at_death_spot'] - rv['baseline_spot']) if rv else 0.08,
            max(0.03, 2 * (it['heading_in_2_10'] - it['baseline_2_10'])) if it else 0.03, max(0.03, 2 * (it['heading_in_10_20'] - it['baseline_10_20'])) if it else 0.11,
            max(0.03, 2 * (it['heading_in_20_40'] - it['baseline_20_40'])) if it else 0.05]
    check('behaviour: radar look, revenge odds, item odds', all(abs(a - b) < 2e-3 for a, b in zip(h, want)), str(h))
C = [l.split()[1] for l in L if l.startswith('C ')]
check('weapon classes', C == ['precision', 'auto', 'melee', 'sniper', 'rocket', 'auto'], str(C))
E = {l.split()[1]: int(l.split()[2]) for l in L if l.startswith('E ')}
check('a band is used from 20 duels', all(E[k] == (1 if v['n'] >= 20 and v['p75'] > v['p25'] else 0) for k, v in m['distance_by_weapon'].items() if k in E))
print('engage load test: %s' % ('all PASS' if not fails else '%d FAIL' % fails)); sys.exit(1 if fails else 0)
EOF
