#!/bin/bash
# Offline test of the walker's weapon zones (2026-11-08; nbot.c wz_load / wz_eff, the real C cut out of nbot.c): a synthetic
# <map>.wzones.txt where snipers make the kills around (60, 60) and precision rifles everywhere else; the sniper must win there
# (switch: 1.5x and above 1.2 with 8+ kills), the precision rifle elsewhere, and a cell with too few kills must not decide.
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d "${TMPDIR:-/tmp}/wz-test.XXXXXX")
python3 - "$T" <<'EOF'
import sys, re
src = open('shim/native/nbot.c').read(); a = src.index('#define WZ_NC'); b = src.index('static float bot_band_off(')
code = src[a:b]
open(sys.argv[1] + '/t.c', 'w').write('''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdarg.h>
static void nlog(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\\n"); }
''' + code + '''
int main(void) {
    wz_load("testmap"); int bad = 0, t1 = 0, t2 = 0, t3 = 0;
    float in[3] = {62, 62, 0}, out[3] = {-30, 10, 0}, thin[3] = {300, 300, 0};
    float es = wz_eff(wz_ci("sniper"), in, &t1), ep = wz_eff(wz_ci("precision"), in, 0);
    int sw = t1 >= 8 && es > 1.5f * ep && es > 1.2f; printf("%s around (62,62): sniper %.2f vs precision %.2f, %d kills -> switch %d\\n", sw ? "PASS" : "FAIL", es, ep, t1, sw); bad += !sw;
    float es2 = wz_eff(wz_ci("sniper"), out, &t2), ep2 = wz_eff(wz_ci("precision"), out, 0);
    int keep = t2 >= 8 && ep2 > 1.5f * es2; printf("%s around (-30,10): precision %.2f vs sniper %.2f, %d kills -> keep the rifle %d\\n", keep ? "PASS" : "FAIL", ep2, es2, t2, keep); bad += !keep;
    wz_eff(wz_ci("sniper"), thin, &t3); printf("%s a cell with %d kills decides nothing (under 8)\\n", t3 < 8 ? "PASS" : "FAIL", t3); bad += t3 >= 8;
    printf("weapon zones test: %s\\n", bad ? "FAIL" : "all PASS"); return bad != 0; }
''')
open(sys.argv[1] + '/testmap.wzones.txt', 'w').write('wzones 1 cell 6 kills 200\nheld precision=0.6000 auto=0.3000 sniper=0.0500 rocket=0.0200 shotgun=0.0200 melee=0.0100\n'
    '10 10 sniper=12 precision=3\n9 10 sniper=4\n-5 1 precision=40 auto=10 sniper=1\n-6 1 precision=20\n50 50 precision=2\n')
EOF
clang -O1 -w -o "$T/t" "$T/t.c" -lm && NHOOK_WZONES_DIR="$T" "$T/t"; r=$?
rm -rf "$T"; exit $r
