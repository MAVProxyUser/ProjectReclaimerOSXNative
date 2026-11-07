#!/bin/bash
# Offline test of the native kill feed from halo3's game results (shim/native/nstats.c rs_tick): extracts the REAL code by
# marker (StPlayer, the scoreboard feed's state, the results block), compiles it natively with stubs (rd = memcpy from a
# fake results table, st_event_verb = a list) and plays scripted changes into the statistics / versus tables:
#   a kill (matrix row = killer), the same with the matrix the other way round, a trade (two kills between the same two
#   players in one snapshot), a betrayal (same team), a suicide, a headshot, a pending scoreboard entry it must drop, and a
#   new match (counts back to 0: no events).
#   bash shim/tools/results-feed-test.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/rsfeed.XXXXXX"); trap 'rm -rf "$T"' EXIT
cut_block(){ awk -v s="$2" -v e="$3" 'index($0,s)==1{on=1} on && index($0,e)==1 && !(index($0,s)==1){exit} on{print}' "$1"; }
N=native/nstats.c
{
cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
static volatile uint64_t g_res_base; static volatile int64_t g_res_stats = 0x6c98, g_res_vs = 0x2cf98, g_res_vslog = 7;
static int rd(uint64_t a, void* out, size_t n) { memcpy(out, (const void*)(uintptr_t)a, n); return 0; }
static void nlog(const char* f, ...) { va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap); printf("\n"); }
static char g_ev[64][200]; static int g_ev_fl[64], g_nev;
static void st_event_verb(const float* pos, const char* kind, const char* a, const char* b, const char* how, const char* verb, int ta, int tb, int flags, const char* fmt, ...) {
    (void)pos; (void)kind; (void)a; (void)b; (void)how; (void)verb; (void)ta; (void)tb;
    va_list ap; va_start(ap, fmt); vsnprintf(g_ev[g_nev], sizeof g_ev[0], fmt, ap); va_end(ap); g_ev_fl[g_nev++] = flags; }
static void st_event_pos(const float* pos, const char* kind, const char* a, const char* b, const char* how, int ta, int tb, int flags, const char* fmt, ...) {
    (void)pos; (void)kind; (void)a; (void)b; (void)how; (void)ta; (void)tb;
    va_list ap; va_start(ap, fmt); vsnprintf(g_ev[g_nev], sizeof g_ev[0], fmt, ap); va_end(ap); g_ev_fl[g_nev++] = flags; }
EOF
grep -m1 '^typedef struct { uint32_t unit; int slot; char name\[64\]; uint64_t rec; } StPlayer;' $N
grep -m1 '^enum { SB_SCORE' $N
cut_block $N '// The kill feed from the scoreboard' 'static void sb_tick('
cut_block $N '// ---- exact kills from halo3' '// the page'"'"'s controls'
cat <<'EOF'
static uint8_t* g_mem; static int g_fail;
static uint16_t* ST(int sl, int i) { return (uint16_t*)(g_mem + 0x6c98 + (size_t)sl * 0x4c6 + (size_t)i * 2); }
static uint32_t* VS(int a, int b) { return (uint32_t*)(g_mem + 0x2cf98 + ((size_t)a * 128 + (size_t)b) * 4); }
static void kill(int k, int v, int t, int row_killer, int betray, int hs) {   // what halo3 writes for one kill
    (*ST(k, betray ? 12 : 9))++; (*ST(v, 11))++; (*ST(k, 234 + (betray ? 2 : 0) + 6 * t))++; (*ST(v, 234 + 1 + 6 * t))++; if (hs) (*ST(k, 234 + 4 + 6 * t))++;
    if (row_killer) { *VS(k, v) += 1; *VS(v, k) += 0x10000; } else { *VS(v, k) += 1; *VS(k, v) += 0x10000; } }
static StPlayer pl[4]; static int np = 4;
static void tick(const char* what, int want, const char* const* texts, const int* flags) {
    g_nev = 0; static double now = 100; now += 0.1; rs_tick(pl, np, now);
    int ok = g_nev == want; for (int i = 0; ok && i < want; i++) ok = !strcmp(g_ev[i], texts[i]) && (!flags || g_ev_fl[i] == flags[i]);
    printf("  %s %s:", ok ? "ok  " : "FAIL", what); for (int i = 0; i < g_nev; i++) printf(" [%s|%d]", g_ev[i], g_ev_fl[i]); printf("\n"); if (!ok) g_fail = 1; }
int main(void) {
    g_mem = calloc(1, 0x4a940); g_res_base = (uint64_t)(uintptr_t)g_mem;
    const char* nm[4] = {"Alice", "Bob", "Carol", "Dave"}; int team[4] = {0, 1, 0, 1};
    for (int i = 0; i < 4; i++) { pl[i].slot = i; snprintf(pl[i].name, 64, "%s", nm[i]); g_sbnow[i].team = team[i]; g_sbprev[i].ok = 1; }
    tick("baseline", 0, 0, 0);
    { *ST(3, 11) = 1; *ST(3, 13) = 1; const char* w[] = {"Dave killed themselves"}; tick("a suicide of an unnamed type", 1, w, 0); }
    { kill(0, 1, 11, 1, 0, 0); const char* w[] = {"Alice killed Bob (Battle rifle)"}; int f[] = {0}; tick("kill, matrix row = killer", 1, w, f); }
    { kill(1, 0, 16, 0, 0, 0); const char* w[] = {"Bob killed Alice (Assault rifle)"}; tick("kill, matrix the other way round", 1, w, 0); }
    { kill(0, 1, 11, 1, 0, 0); kill(0, 1, 11, 1, 0, 0); const char* w[] = {"Alice killed Bob (Battle rifle)", "Alice killed Bob (Battle rifle)"}; tick("double kill", 2, w, 0); }
    { kill(0, 1, 13, 1, 0, 0); kill(1, 0, 25, 1, 0, 0); const char* w[] = {"Alice killed Bob (Shotgun)", "Bob killed Alice (Energy sword)"}; tick("trade in one snapshot", 2, w, 0); }
    { kill(0, 2, 27, 1, 1, 0); const char* w[] = {"Alice betrayed Carol (Frag grenade)"}; int f[] = {4}; tick("betrayal", 1, w, f); }
    { (*ST(1, 13))++; (*ST(1, 11))++; (*ST(1, 234 + 3 + 6 * 1))++; (*ST(1, 234 + 1 + 6 * 1))++; *VS(1, 1) += 0x10001; const char* w[] = {"Bob killed themselves (Falling)"}; tick("suicide", 1, w, 0); }
    { kill(3, 0, 14, 1, 0, 1); const char* w[] = {"Dave killed Alice (Sniper rifle, headshot)"}; tick("headshot", 1, w, 0); }
    { g_nsbv = 1; g_sbv[0].slot = 2; g_sbv[0].suicide = 0; kill(1, 2, 11, 1, 0, 0); const char* w[] = {"Bob killed Carol (Battle rifle)"}; tick("drops the scoreboard's pending victim", 1, w, 0);
      if (g_nsbv != 0) { printf("  FAIL the pending victim stayed\n"); g_fail = 1; } else printf("  ok   the pending victim is gone\n"); }
    { int16_t before = g_sbprev[0].s[SB_KILLS]; kill(0, 3, 11, 1, 0, 0); const char* w[] = {"Alice killed Dave (Battle rifle)"}; tick("absorbs the scoreboard's coming kill", 1, w, 0);
      if (g_sbprev[0].s[SB_KILLS] != before + 1) { printf("  FAIL scoreboard not told\n"); g_fail = 1; } else printf("  ok   the scoreboard expects that kill\n"); }
    { for (int i = 0; i < 4; i++) (*ST(i, 1))++; (*ST(0, 3))++; (*ST(2, 3))++; const char* w[] = {"Game over: Alice, Carol won"}; tick("game over (games-completed, games-won)", 1, w, 0); }
    memset(g_mem, 0, 0x4a940); tick("new match (counts back to 0)", 0, 0, 0);
    { kill(2, 3, 0, 1, 0, 0); const char* w[] = {"Carol killed Dave"}; tick("an unnamed weapon (type 0, a modded map's)", 1, w, 0); }
    printf(g_fail ? "SOME CHECKS FAILED\n" : "PASS\n"); return g_fail; }
EOF
} > "$T/t.c"
clang -O1 -Wall -Wno-unused-function -Wno-unused-variable -o "$T/t" "$T/t.c" && "$T/t"
