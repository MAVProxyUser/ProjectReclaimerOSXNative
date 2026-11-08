#!/bin/bash
# Offline test of the lanes (shim/native/nlanes.c + shim/tools/lanes-import.py, 2026-11-08): nlanes.c built alone with stubs.
# A move's passes add up to its length / cell (straight, diagonal, backwards); a save writes RLN1 that the importer reads; a
# second writer's cells (the importer's format) survive nhook's next save (the merge), and so do the source names.
#   bash shim/tools/lanes-test.sh        -> PASS / FAIL lines, exit 0 when all pass
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d "${TMPDIR:-/tmp}/lanes-test.XXXXXX"); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
typedef struct { char* b; size_t n, cap; } StBuf;
static void sb_put(StBuf* s, const char* fmt, ...) { va_list a; va_start(a, fmt); char tmp[512]; int k = vsnprintf(tmp, sizeof tmp, fmt, a); va_end(a);
    if (s->n + (size_t)k + 1 > s->cap) { s->cap = (s->n + k + 1) * 2; s->b = realloc(s->b, s->cap); } memcpy(s->b + s->n, tmp, (size_t)k + 1); s->n += (size_t)k; }
static void sb_str(StBuf* s, const char* v) { sb_put(s, "\"%s\"", v); }
static void nlog(const char* fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
static double mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void hm_path(const char* map, char* out, size_t n) { snprintf(out, n, "%s/%s.bin", getenv("NHOOK_HEATMAP_DIR"), map); }
#include "nlanes.c"
static double total(const LnGrid* g, int veh) { double s = 0; for (uint32_t i = 0; i < g->cap; i++) s += veh ? g->c[i].veh : g->c[i].foot; return s; }
static int fails;
static void check(const char* what, double got, double want) { int ok = fabs(got - want) < 1e-3 * (1 + fabs(want)); printf("%s %s: %.4f (want %.4f)\n", ok ? "PASS" : "FAIL", what, got, want); if (!ok) fails++; }
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "merge")) {   // stage 2: the file now also holds the importer's cells
        ln_tick("testmap", 1.0, 100.0); float a[3] = {0, 0, 0}, b[3] = {0, 3, 0}; ln_move(a, b, 0.1, 0);
        double before = total(&g_ln.all, 0); ln_save(); check("after the merge save, nhook's cells + the file's", total(&g_ln.all, 0), before);
        LnGrid g = {0}; LnSrc* src = 0; uint32_t ns = 0; double since = 0; char p[1100]; ln_path("testmap", ".lanes", p, sizeof p); ln_read(p, &g, &src, &ns, &since);
        check("the file after the merge", total(&g, 0), before); check("sources kept", ns, 1); printf("%s source name: %s\n", ns && !strcmp(src[0], "imported.jsonl.gz") ? "PASS" : "FAIL", ns ? src[0] : "-");
        if (!(ns && !strcmp(src[0], "imported.jsonl.gz"))) fails++;
        return fails ? 1 : 0; }
    ln_tick("testmap", 1.0, 0.0);
    float a[3] = {0.1f, 0.25f, 0}, b[3] = {6.1f, 0.25f, 0}; ln_move(a, b, 0.5, 0);         // 6 units along x: 12 passes in 13 cells
    check("straight move, passes = length / cell", total(&g_ln.all, 0), 6 / LN_CELL);
    float c[3] = {3.3f, -2.7f, 0}, e[3] = {-1.2f, 1.9f, 0}; ln_move(c, e, 0.5, 0);         // diagonal, backwards
    double dl = hypot(-4.5, 4.6); check("diagonal backwards move adds its length too", total(&g_ln.all, 0), (6 + dl) / LN_CELL);
    float v0[3] = {0, 0, 0}, v1[3] = {0, 7.5f, 0}; ln_move(v0, v1, 0.5, 1);               // in a vehicle
    check("vehicle move apart", total(&g_ln.all, 1), 7.5 / LN_CELL);
    float f0[3] = {0, 0, 0}, f1[3] = {30, 0, 0}; ln_move(f0, f1, 0.1, 0);                 // 300 u/s: a teleport, not a lane
    check("teleport left out", total(&g_ln.all, 0), (6 + dl) / LN_CELL);
    float s0[3] = {5, 5, 0}, s1[3] = {5.01f, 5, 0}; ln_move(s0, s1, 0.1, 0);              // standing still
    check("standing still left out", total(&g_ln.all, 0), (6 + dl) / LN_CELL);
    check("this match's lanes = all of it here", total(&g_ln.match, 0), (6 + dl) / LN_CELL);
    ln_save(); printf("saved %.4f passes on foot\n", total(&g_ln.all, 0));
    return fails ? 1 : 0;
}
EOF
cp shim/native/nlanes.c "$T/"
clang -O1 -Wall -Wno-unused-function -o "$T/t" "$T/t.c" || { echo "FAIL build"; exit 1; }
export NHOOK_HEATMAP_DIR="$T"
"$T/t" 2>/dev/null; rc=$?
# the importer reads what nhook wrote, then adds a "recording" of its own (a second writer) and a source name
python3 - "$T" <<'EOF' || rc=1
import importlib.util, sys, os, math
spec = importlib.util.spec_from_file_location('li', 'shim/tools/lanes-import.py'); li = importlib.util.module_from_spec(spec); sys.argv = ['x']; spec.loader.exec_module(li)
p = os.path.join(sys.argv[0] if False else os.environ['NHOOK_HEATMAP_DIR'], 'testmap.lanes')
g, src, since = li.read_lanes(p); tot = sum(c[0] for c in g.values()); want = (6 + math.hypot(4.5, 4.6)) / 0.5
print('%s the importer reads nhook\'s file: %.4f passes (want %.4f), live since set: %s' % ('PASS' if abs(tot - want) < 1e-3 else 'FAIL', tot, want, bool(since)))
li.add_move(g, (0, 0), (0, -2), False)   # the importer's walk: 4 more passes
py = sum(c[0] for c in g.values()); print('%s the importer\'s walk adds its length: %.4f (want %.4f)' % ('PASS' if abs(py - want - 4) < 1e-3 else 'FAIL', py, want + 4))
li.write_lanes(p, g, src + ['imported.jsonl.gz'], since)
sys.exit(0 if abs(tot - want) < 1e-3 and abs(py - want - 4) < 1e-3 else 1)
EOF
"$T/t" merge 2>/dev/null || rc=1
python3 shim/tools/lanes-import.py --heatmaps "$T" --show testmap
[ $rc = 0 ] && echo "lanes test: all PASS" || echo "lanes test: FAIL"
exit $rc
