// nstats.c -- the live stats page for the NATIVE app (included by nhook.c): the loader's stats page (shim/src/stats_page.html,
// unchanged) on http://127.0.0.1:8765/, fed from halo3's tables inside the app (the engine thread's TLS block, nhook.c
// tls_block) and the app's own log lines. The snapshot keeps the loader's JSON shape (shim/src/stats.c st_snapshot); what
// the native app doesn't have yet (overlay, recording, voice, session, controls) is sent as "off" / empty.
// NHOOK_STATS_HTTP=<port> (default 8765, 0 = off), NHOOK_STATS_PAGE=<file> serves the page from disk (edits show on a
// refresh), NHOOK_FEED_LOG=1 prints each event. Heatmaps and map pictures are the loader's files (ReclaimerMac/heatmaps,
// maptop): both clients add to the same ones.
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <libproc.h>
#include <spawn.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>

static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double st_wall(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }

// ---- the engine accessors h3maps.c expects -------------------------------------------------------------------------
static int hb_read(uint64_t a, void* dst, size_t n) { return a >= 0x10000 && !rd(a, dst, n); }
static uint64_t hb_object_array_tls(uint64_t* tls) {      // TLS block +0x38: the "object" data array
    uint64_t blk = tls_block(), arr = 0; uint8_t hdr[0x60];
    if (!blk || !hb_read(blk + 0x38, &arr, 8) || !arr || !hb_read(arr, hdr, sizeof hdr)) return 0;
    if (*(uint32_t*)(hdr + 0x2c) != 0x64407440 || memcmp(hdr, "object\0", 7)) return 0;
    if (tls) *tls = blk;
    return arr;
}
static uint64_t hb_object_array(void) { return hb_object_array_tls(0); }
static int hb_scenario_name(char* out, size_t n) {         // as hitbox.c: game_options (game_globals +0x10) holds "maps\<name>"
    static int off = -1; uint64_t tls = 0, gg = 0; out[0] = 0;
    if (!hb_object_array_tls(&tls) || !hb_read(tls + 0x48, &gg, 8) || !gg) return 0;
    char b[0x100 + 272]; if (!hb_read(gg + 0x10, b, sizeof b)) return 0; b[sizeof b - 1] = 0;
    if (off < 0) {
        for (int i = 0; i + 5 < 0x100; i++) if (!strncasecmp(b + i, "maps\\", 5)) { int st = i; while (st > 0 && b[st - 1] >= 0x20 && b[st - 1] < 0x7f) st--; off = st; break; }
        if (off < 0) return 0;
    }
    const char* p = b + off; if (!strcasestr(p, "maps\\")) return 0;
    const char* q = strrchr(p, '\\'); q = q ? q + 1 : p; size_t k = 0;
    for (; q[k] && q[k] != '.' && k + 1 < n; k++) out[k] = (char)tolower((unsigned char)q[k]);
    out[k] = 0; return k > 0;
}
#define HB_MAXOBJ 16384                                   // the app's object table: 0x3fc0 slots ([capacity] objects.count)
#include "../src/h3maps.c"

// ---- JSON buffer (stats.c) ------------------------------------------------------------------------------------------
typedef struct { char* b; size_t n, cap; } StBuf;
static void sb_put(StBuf* s, const char* fmt, ...) {
    va_list ap;
    for (;;) {
        size_t room = s->cap - s->n; va_start(ap, fmt); int k = vsnprintf(s->b ? s->b + s->n : 0, room, fmt, ap); va_end(ap);
        if (k < 0) return;
        if ((size_t)k < room) { s->n += (size_t)k; return; }
        size_t nc = s->cap ? s->cap * 2 : 16384; while (nc < s->n + (size_t)k + 1) nc *= 2;
        char* nb = realloc(s->b, nc); if (!nb) return; s->b = nb; s->cap = nc;
    }
}
static void sb_str(StBuf* s, const char* v) {
    sb_put(s, "\"");
    for (const unsigned char* p = (const unsigned char*)v; p && *p; p++) {
        if (*p == '"' || *p == '\\') sb_put(s, "\\%c", *p); else if (*p < 0x20) sb_put(s, "\\u%04x", *p); else sb_put(s, "%c", *p);
    }
    sb_put(s, "\"");
}
static void sb_f3(StBuf* s, const float* v) { if (isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2])) sb_put(s, "[%.2f,%.2f,%.2f]", v[0], v[1], v[2]); else sb_put(s, "null"); }

// ---- event feed (stats.c's ring and /feed.json) --------------------------------------------------------------------
typedef struct { uint32_t seq; int flags, ta, tb, haspos; double t; float pos[3]; char kind[12], a[64], b[64], how[64], verb[24], text[320]; } StEvt;
enum { ST_NEV = 1024 };
static StEvt g_ev[ST_NEV]; static uint32_t g_ev_seq; static pthread_mutex_t g_ev_mx = PTHREAD_MUTEX_INITIALIZER;
static void st_eventv(const float* pos, const char* kind, const char* a, const char* b, const char* how, const char* verb, int ta, int tb, int flags, const char* fmt, va_list ap) {
    StEvt e; memset(&e, 0, sizeof e); e.t = st_wall(); e.flags = flags; e.ta = ta; e.tb = tb; if (verb) snprintf(e.verb, sizeof e.verb, "%s", verb);
    if (pos) { memcpy(e.pos, pos, sizeof e.pos); e.haspos = 1; }
    snprintf(e.kind, sizeof e.kind, "%s", kind); snprintf(e.a, sizeof e.a, "%s", a ? a : ""); snprintf(e.b, sizeof e.b, "%s", b ? b : "");
    snprintf(e.how, sizeof e.how, "%s", how ? how : "");
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    pthread_mutex_lock(&g_ev_mx); e.seq = ++g_ev_seq; g_ev[e.seq % ST_NEV] = e; pthread_mutex_unlock(&g_ev_mx);
    if (getenv("NHOOK_FEED_LOG")) nlog("[nhook] [feed] %s: %s", e.kind, e.text);
}
static void st_event_pos(const float* pos, const char* kind, const char* a, const char* b, const char* how, int ta, int tb, int flags, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); st_eventv(pos, kind, a, b, how, 0, ta, tb, flags, fmt, ap); va_end(ap); }
static void st_event_verb(const float* pos, const char* kind, const char* a, const char* b, const char* how, const char* verb, int ta, int tb, int flags, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); st_eventv(pos, kind, a, b, how, verb, ta, tb, flags, fmt, ap); va_end(ap); }
static void st_feed_json(StBuf* s, uint32_t since) {
    pthread_mutex_lock(&g_ev_mx);
    uint32_t hi = g_ev_seq, lo = hi >= ST_NEV ? hi - ST_NEV + 1 : 1; if (since >= lo && since <= hi) lo = since + 1;
    sb_put(s, "{\"seq\":%u,\"events\":[", hi);
    for (uint32_t q = lo; q <= hi; q++) {
        const StEvt* e = &g_ev[q % ST_NEV];
        sb_put(s, "%s{\"seq\":%u,\"t\":%.3f,\"kind\":\"%s\",\"text\":", q > lo ? "," : "", e->seq, e->t, e->kind); sb_str(s, e->text);
        if (e->a[0]) { sb_put(s, ",\"a\":"); sb_str(s, e->a); }
        if (e->b[0]) { sb_put(s, ",\"b\":"); sb_str(s, e->b); }
        if (e->how[0]) { sb_put(s, ",\"how\":"); sb_str(s, e->how); }
        if (e->ta >= 0) sb_put(s, ",\"ta\":%d", e->ta);
        if (e->tb >= 0) sb_put(s, ",\"tb\":%d", e->tb);
        if (e->flags & 1) sb_put(s, ",\"estimated\":true");
        if (e->flags & 4) sb_put(s, ",\"betrayal\":true");
        if (e->verb[0]) { sb_put(s, ",\"verb\":"); sb_str(s, e->verb); }
        if (e->haspos) sb_put(s, ",\"pos\":[%.2f,%.2f,%.2f]", e->pos[0], e->pos[1], e->pos[2]);
        sb_put(s, "}");
    }
    sb_put(s, "]}"); pthread_mutex_unlock(&g_ev_mx);
}

#include "nnet.c"   // the network session and the packet log

// ---- the app's log lines -> match state and events ------------------------------------------------------------------
static volatile double g_gameplay_t0;                      // "[switch] game Playing" (mono time), 0 = not in a match
static volatile int g_match_pending; static char g_loading[96];
// where the app moved halo3's tables: it logs each move ("[capacity] <what>: <old> -> <new>", "[c1] ...")
static volatile int64_t g_sb_off = 0x159c0;                // game engine globals (TLS +0x20) -> statborg (halo3: +0xfeb8)
static uint64_t cap_new(const char* l, const char* what) { // "[capacity] <what>: 0x.. -> 0x..": the new value, 0 = not this line
    size_t wl = strlen(what); if (strncmp(l, "[capacity] ", 11) || strncmp(l + 11, what, wl) || l[11 + wl] != ':') return 0;
    const char* a = strstr(l + 11 + wl, "-> 0x"); return a ? strtoull(a + 5, 0, 16) : 0;
}
static void nst_layout_line(const char* l) {
    uint64_t v;
    if ((v = cap_new(l, "game_globals.statborg"))) { g_sb_off = (int64_t)v; return; }
    if ((v = cap_new(l, "session.peers_shift"))) { g_ses_peers_shift = (int64_t)v; return; }
    if ((v = cap_new(l, "session.players_shift"))) { g_ses_players_shift = (int64_t)v; return; }
    const char* m = "[c1] network sessions moved to 0x"; if (!strncmp(l, m, strlen(m))) g_ses_store = strtoull(l + strlen(m), 0, 16);
}
static void nst_on_line(const char* p, size_t n) {
    char l[600]; if (n >= sizeof l) n = sizeof l - 1; memcpy(l, p, n); l[n] = 0;
    if (!strncmp(l, "[capacity] ", 11) || !strncmp(l, "[c1] ", 5)) { nst_layout_line(l); return; }
    if (!strncmp(l, "[switch] game Playing", 21)) { if (!g_gameplay_t0) { g_gameplay_t0 = mono_s(); g_match_pending = 1; } return; }
    if (!strncmp(l, "[loading] map ", 14)) {               // "[loading] map 320: Guardian, MCC's art of Guardian"
        const char* c = strchr(l, ':'); if (c) { c++; while (*c == ' ') c++; size_t k = 0; while (c[k] && c[k] != ',' && k + 1 < sizeof g_loading) { g_loading[k] = c[k]; k++; } g_loading[k] = 0; }
        g_gameplay_t0 = 0; g_hb_map_recheck = 1; return;
    }
    if (!strncmp(l, "[host] stopping", 15) || !strncmp(l, "[menus] open", 12)) { g_gameplay_t0 = 0; return; }
    // chat: "[chat] line [<channel>] <name> (<id>): <text>", "[chat] line [SERVER #...] <text>", "[chat] line [NOTICE] <text>"
    if (!strncmp(l, "[chat] line [", 13)) {
        char* ch = l + 13; char* cb = strchr(ch, ']'); if (!cb) return; *cb = 0; char* rest = cb + 1; while (*rest == ' ') rest++;
        if (!strncmp(ch, "SERVER", 6) || !strcmp(ch, "NOTICE")) { st_event_pos(0, "server", 0, 0, ch, -1, -1, 0, "%s", rest); return; }
        char* paren = strstr(rest, " ("); char* colon = paren ? strstr(paren, "): ") : 0;
        if (paren && colon) { *paren = 0; st_event_pos(0, "chat", rest, 0, ch, -1, -1, 0, "%s: %s", rest, colon + 3); }
        else st_event_pos(0, "chat", 0, 0, ch, -1, -1, 0, "%s", rest);
        return;
    }
    if (!strncmp(l, "[playlist] ballot ", 18) || !strncmp(l, "[playlist] voting ", 18)) { st_event_pos(0, "vote", 0, 0, 0, -1, -1, 0, "%s", l + 11); return; }
}

// ---- heatmap (stats.c, unchanged: the same files as the loader's) ---------------------------------------------------
#define HM_CELL 0.5f
typedef struct { int32_t x, y; uint32_t n; } HmCell;
typedef struct { float x, y, z; int32_t team, slot; char name[48]; uint32_t match; float t; } HmDeath;
typedef struct { float x, y, z; uint32_t n, match; uint8_t kind; char name[43]; } HmSpawn;
_Static_assert(sizeof(HmSpawn) == 64, "spawn record");
static struct { char map[64]; HmCell* cells; uint32_t cap, used; HmDeath* d; uint32_t nd, dcap, match, maxmatch; int dirty; double saved, match_t0;
                HmSpawn* sp; uint32_t nsp, spcap; } g_hm;
static pthread_mutex_t g_hm_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_hm_cur[64];
static void hm_path(const char* map, char* out, size_t n) { const char* h = getenv("HOME"); snprintf(out, n, "%s/Library/Application Support/ReclaimerMac/heatmaps/%s.bin", h ? h : "/tmp", map); }
static void hm_cell_add(int32_t x, int32_t y, uint32_t n) {
    if (g_hm.used * 10 >= g_hm.cap * 7) {
        uint32_t oc = g_hm.cap, nc = oc ? oc * 2 : 65536; HmCell* nt = calloc(nc, sizeof *nt), *ot = g_hm.cells; if (!nt) return;
        g_hm.cells = nt; g_hm.cap = nc; g_hm.used = 0;
        for (uint32_t i = 0; i < oc; i++) if (ot[i].n) hm_cell_add(ot[i].x, ot[i].y, ot[i].n);
        free(ot);
    }
    uint32_t m = g_hm.cap - 1, i = ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & m;
    for (;; i = (i + 1) & m) { HmCell* c = &g_hm.cells[i]; if (!c->n) { c->x = x; c->y = y; c->n = n; g_hm.used++; return; } if (c->x == x && c->y == y) { c->n += n; return; } }
}
static void hm_save(void) {
    if (!g_hm.map[0] || !g_hm.dirty) return;
    char p[1024]; hm_path(g_hm.map, p, sizeof p);
    char dir[1024]; snprintf(dir, sizeof dir, "%s", p); char* sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdir(dir, 0755); }
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", p); FILE* f = fopen(tmp, "wb"); if (!f) return;
    float cell = HM_CELL; fwrite("RHM2", 1, 4, f); fwrite(&cell, 4, 1, f); fwrite(&g_hm.used, 4, 1, f); fwrite(&g_hm.nd, 4, 1, f); fwrite(&g_hm.maxmatch, 4, 1, f);
    for (uint32_t i = 0; i < g_hm.cap; i++) if (g_hm.cells[i].n) fwrite(&g_hm.cells[i], sizeof(HmCell), 1, f);
    fwrite(g_hm.d, sizeof(HmDeath), g_hm.nd, f); int ok = !ferror(f); fclose(f); if (ok) rename(tmp, p); else unlink(tmp);
    g_hm.dirty = 0;
    if (g_hm.nsp) {
        char sp[1100], st[1200]; snprintf(sp, sizeof sp, "%.*s.spawns", (int)(strlen(p) - 4), p); snprintf(st, sizeof st, "%s.tmp", sp);
        FILE* g = fopen(st, "wb");
        if (g) { fwrite("RSP1", 1, 4, g); fwrite(&g_hm.nsp, 4, 1, g); fwrite(g_hm.sp, sizeof(HmSpawn), g_hm.nsp, g); int ok2 = !ferror(g); fclose(g); if (ok2) rename(st, sp); else unlink(st); }
    }
}
static void hm_switch(const char* map) {                  // (g_hm_mx held)
    if (!strcmp(map, g_hm.map)) return;
    hm_save(); free(g_hm.cells); free(g_hm.d); free(g_hm.sp); memset(&g_hm, 0, sizeof g_hm);
    snprintf(g_hm.map, sizeof g_hm.map, "%s", map); if (!map[0]) return;
    char p[1024]; hm_path(map, p, sizeof p); FILE* f = fopen(p, "rb"); uint32_t nc = 0, nd = 0;
    if (f) {
        char mg[4]; float cell = 0;
        if (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "RHM2", 4) && fread(&cell, 4, 1, f) == 1 && cell == HM_CELL && fread(&nc, 4, 1, f) == 1 && fread(&nd, 4, 1, f) == 1
            && fread(&g_hm.maxmatch, 4, 1, f) == 1 && nc < (1u << 24) && nd < (1u << 22)) {
            for (uint32_t i = 0; i < nc; i++) { HmCell c; if (fread(&c, sizeof c, 1, f) != 1) break; hm_cell_add(c.x, c.y, c.n); }
            g_hm.d = malloc((nd ? nd : 1) * sizeof(HmDeath)); g_hm.dcap = nd ? nd : 1; g_hm.nd = g_hm.d ? (uint32_t)fread(g_hm.d, sizeof(HmDeath), nd, f) : 0;
            for (uint32_t i = 0; i < g_hm.nd; i++) g_hm.d[i].name[sizeof g_hm.d[i].name - 1] = 0;
        }
        fclose(f);
    }
    { char sp[1100]; snprintf(sp, sizeof sp, "%.*s.spawns", (int)(strlen(p) - 4), p); FILE* g = fopen(sp, "rb"); uint32_t n = 0; char mg[4];
      if (g) { if (fread(mg, 1, 4, g) == 4 && !memcmp(mg, "RSP1", 4) && fread(&n, 4, 1, g) == 1 && n < (1u << 20) && (g_hm.sp = malloc((n ? n : 1) * sizeof(HmSpawn)))) {
                   g_hm.spcap = n ? n : 1; g_hm.nsp = (uint32_t)fread(g_hm.sp, sizeof(HmSpawn), n, g);
                   for (uint32_t i = 0; i < g_hm.nsp; i++) g_hm.sp[i].name[sizeof g_hm.sp[i].name - 1] = 0; }
               fclose(g); } }
    g_hm.match = ++g_hm.maxmatch; g_hm.match_t0 = mono_s(); g_hm.saved = mono_s(); g_hm.dirty = 0;
    nlog("[nhook] heatmap %s: %u cells, %u deaths, %u spawn points from earlier matches", map, g_hm.used, g_hm.nd, g_hm.nsp);
}
static void hm_death(const float* pos, int team, int slot, const char* name) {   // (g_hm_mx held)
    hm_switch(g_hm_cur);
    if (g_hm.nd >= g_hm.dcap) { uint32_t nc = g_hm.dcap ? g_hm.dcap * 2 : 256; HmDeath* nd = realloc(g_hm.d, nc * sizeof *nd); if (!nd) return; g_hm.d = nd; g_hm.dcap = nc; }
    HmDeath* d = &g_hm.d[g_hm.nd++]; memset(d, 0, sizeof *d); d->x = pos[0]; d->y = pos[1]; d->z = pos[2]; d->team = team; d->slot = slot;
    snprintf(d->name, sizeof d->name, "%s", name ? name : ""); d->match = g_hm.match; d->t = (float)(mono_s() - g_hm.match_t0); g_hm.dirty = 1;
}
static volatile int g_mt_state; static char g_mt_name[96];  // the map picture: 0 none, 1 ready, 2 making, 3 failed, 4 no tool
static void hm_json(StBuf* s) {
    pthread_mutex_lock(&g_hm_mx);
    { static const char* const mts[5] = {"none", "ready", "making", "failed", "no tool"}; int st = g_mt_state;
      sb_put(s, "{\"maptop\":\"%s\",", strcmp(g_mt_name, g_hm.map) ? "none" : mts[st < 0 || st > 4 ? 0 : st]); }
    sb_put(s, "\"map\":"); sb_str(s, g_hm.map); sb_put(s, ",\"cell\":%.2f,\"match\":%u,\"cells\":[", HM_CELL, g_hm.match);
    int first = 1;
    for (uint32_t i = 0; i < g_hm.cap; i++) if (g_hm.cells[i].n) { sb_put(s, "%s%d,%d,%u", first ? "" : ",", g_hm.cells[i].x, g_hm.cells[i].y, g_hm.cells[i].n); first = 0; }
    sb_put(s, "],\"deaths\":[");
    for (uint32_t i = 0; i < g_hm.nd; i++) { HmDeath* d = &g_hm.d[i]; sb_put(s, "%s[%.2f,%.2f,%.2f,%d,", i ? "," : "", d->x, d->y, d->z, d->team); sb_str(s, d->name); sb_put(s, ",%u,%.0f,%d]", d->match, d->t, d->slot); }
    sb_put(s, "],\"spawns\":[");
    for (uint32_t i = 0; i < g_hm.nsp; i++) { HmSpawn* q = &g_hm.sp[i]; sb_put(s, "%s[%.2f,%.2f,%.2f,%d,", i ? "," : "", q->x, q->y, q->z, q->kind); sb_str(s, q->name); sb_put(s, ",%u,%u]", q->n, q->match); }
    sb_put(s, "]}"); pthread_mutex_unlock(&g_hm_mx);
}
// the map picture behind the heatmap: the loader's tools make missing ones (re-tools/h3bsp.py; the repo = two folders up
// from this dylib, shim/build/nhook.dylib)
static char g_repo[1024];
static void* st_reap(void* a) { int st; waitpid((pid_t)(intptr_t)a, &st, 0); g_mt_state = 0; return 0; }
static void st_maptop_ensure(const char* mp, const char* name) {
    static char last[96]; if (!name[0] || !mp[0] || !strcmp(last, name)) return; snprintf(last, sizeof last, "%s", name);
    snprintf(g_mt_name, sizeof g_mt_name, "%s", name);
    if ((name[0] >= '0' && name[0] <= '9') || !strcmp(name, "mainmenu") || !strcmp(name, "shared") || !strcmp(name, "campaign")) { g_mt_state = 0; return; }
    const char* home = getenv("HOME"); char dir[1024], png[1100], js[1100], lg[1100], tool[1100];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/ReclaimerMac/maptop", home ? home : "/tmp");
    snprintf(png, sizeof png, "%s/%s.png", dir, name); snprintf(js, sizeof js, "%s/%s.json", dir, name); snprintf(lg, sizeof lg, "%s/%s.log", dir, name);
    if (!access(png, R_OK) && !access(js, R_OK)) { g_mt_state = 1; return; }
    snprintf(tool, sizeof tool, "%s/re-tools/h3bsp.py", g_repo);
    if (!g_repo[0] || access(tool, R_OK) || getenv("NHOOK_NO_MAPTOP")) { g_mt_state = 4; return; }
    mkdir(dir, 0755);
    char* argv[] = {(char*)"/usr/bin/env", (char*)"python3", tool, (char*)mp, png, (char*)"--size", (char*)"1024", 0}; extern char** environ;   // python3 with numpy + Pillow on PATH
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, lg, O_WRONLY | O_CREAT | O_TRUNC, 0644); posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at; posix_spawnattr_init(&at); posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT); posix_spawnattr_setpgroup(&at, 0);
    // the child must not get our dylib: DYLD_INSERT_LIBRARIES stays in its environment, so it runs without one
    char* env2[512]; int ne = 0; for (char** e = environ; *e && ne < 510; e++) if (strncmp(*e, "DYLD_INSERT_LIBRARIES=", 22)) env2[ne++] = *e; env2[ne] = 0;
    pid_t pid = 0; int r = posix_spawn(&pid, "/usr/bin/env", &fa, &at, argv, env2);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if (r) { g_mt_state = 3; return; }
    g_mt_state = 2; nlog("[nhook] making the map picture for %s", name);
    pthread_t th; pthread_create(&th, 0, st_reap, (void*)(intptr_t)pid); pthread_detach(th);
}

// ---- players and tracking (stats.c) ---------------------------------------------------------------------------------
typedef struct { uint32_t unit; int slot; char name[64]; uint64_t rec; } StPlayer;
static int st_players(StPlayer* out, int max) {
    uint64_t blk = tls_block(), parr = 0; uint8_t hdr[0x60];
    if (!blk || !hb_read(blk + 0x10, &parr, 8) || !parr || !hb_read(parr, hdr, sizeof hdr) || *(uint32_t*)(hdr + 0x2c) != 0x64407440) return 0;
    int32_t es = *(int32_t*)(hdr + 0x24), hi = *(int32_t*)(hdr + 0x3c); uint64_t pd = *(uint64_t*)(hdr + 0x48);
    if (es < 0x98 || es > 0x10000 || hi <= 0 || hi > 256) return 0;
    int n = 0;
    for (int i = 0; i < hi && n < max; i++) {
        uint8_t rec[0x98]; if (!hb_read(pd + (uint64_t)i * es, rec, sizeof rec) || !*(uint16_t*)rec) continue;
        StPlayer* p = &out[n]; p->unit = *(uint32_t*)(rec + 0x28); p->slot = i; p->rec = pd + (uint64_t)i * es; int j = 0; const uint16_t* w = (const uint16_t*)(rec + 0x58);
        for (int c = 0; c < 32 && w[c] && j < 60; c++) {
            uint16_t u = w[c];
            if (u < 0x80) p->name[j++] = (char)u;
            else if (u < 0x800) { p->name[j++] = (char)(0xC0 | (u >> 6)); p->name[j++] = (char)(0x80 | (u & 63)); }
            else { p->name[j++] = (char)(0xE0 | (u >> 12)); p->name[j++] = (char)(0x80 | ((u >> 6) & 63)); p->name[j++] = (char)(0x80 | (u & 63)); }
        }
        p->name[j] = 0; n++;
    }
    return n;
}
typedef struct { char name[64]; int slot, team, deaths, alive; uint32_t unit; double life_start, longest, dist; float pos[3], top_speed; } StTrack;
static StTrack g_trk[256]; static int g_ntrk; static char g_trk_map[64];
static StTrack* st_track(const StPlayer* p) {
    for (int i = 0; i < g_ntrk; i++) if (g_trk[i].slot == p->slot && !strcmp(g_trk[i].name, p->name)) return &g_trk[i];
    if (g_ntrk >= 256) return 0;
    StTrack* t = &g_trk[g_ntrk++]; memset(t, 0, sizeof *t); snprintf(t->name, sizeof t->name, "%s", p->name); t->slot = p->slot; t->team = -1; t->unit = 0xFFFFFFFFu; return t;
}
typedef struct { uint32_t h; int alive, ti; } StBip;
static StBip g_bip[2048]; static int g_nbip;
static char g_nick[64];                                    // the local player: the profile's nickname (profile.json identity.nickname)
static void nick_load(void) {
    const char* pd = getenv("RECLAIMER_PROFILE_DIR"); char p[1100]; const char* h = getenv("HOME");
    if (pd && *pd) snprintf(p, sizeof p, "%s/profile.json", pd); else snprintf(p, sizeof p, "%s/Library/Application Support/ProjectReclaimer/profile.json", h ? h : "");
    FILE* f = fopen(p, "r"); if (!f) return; char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
    const char* k = strstr(b, "\"nickname\""); if (!k) return; k = strchr(k + 10, '"'); if (!k) return; k++;
    size_t j = 0; while (k[j] && k[j] != '"' && j + 1 < sizeof g_nick) { g_nick[j] = k[j]; j++; } g_nick[j] = 0;
}
static void st_tag(StBuf* s, uint32_t def) { sb_str(s, hb_tag_name(def)); }

// ---- game variant and player traits (stats.c, verbatim; layout: shim/docs/halo3-variant-traits.md) ------------------
// game_globals (TLS +0x48) + 0x1608 is not among the members the app moves ([capacity] game_globals.*)
#define g_st_tls (tls_block())
static const char*const tl_res[]={"Unchanged","10%","50%","90%","100%","110%","150%","200%","300%","500%","1000%","2000%","Invulnerable","Invulnerable (incl. assassinations)"};
static const char*const tl_rech[]={"Unchanged","-25%","-10%","-5%","Disabled","50%","90%","100%","110%","200%"};
static const char*const tl_vamp[]={"Unchanged","Off","10%","25%","50%","100%"};
static const char*const tl_head[]={"Unchanged","Immune","Vulnerable"};
static const char*const tl_shield[]={"Unchanged","None","Normal","2x","3x","4x"};
static const char*const tl_gren[]={"Unchanged","Map default","None"};
static const char*const tl_dmg[]={"Unchanged","0%","25%","50%","75%","90%","100%","110%","125%","150%","200%","300%","Instant kill"};
static const char*const tl_regen[]={"Unchanged","On","Off","On, with firebombs"};
static const char*const tl_ammo[]={"Unchanged","Normal","Infinite","Bottomless clip"};
static const char*const tl_pick[]={"Unchanged","Allowed","Disabled"};
static const char*const tl_speed[]={"Unchanged","25%","50%","75%","90%","100%","110%","125%","150%","200%","300%"};
static const char*const tl_grav[]={"Unchanged","50%","75%","100%","150%","200%"};
static const char*const tl_veh[]={"Unchanged","None","Passenger only","Full use"};
static const char*const tl_camo[]={"Unchanged","Off","Poor","Good","Invisible"};
static const char*const tl_wp[]={"Unchanged","Off","Allies","Everyone"};
static const char*const tl_aura[]={"Unchanged","Off","Team color","Black","White"};
static const char*const tl_radar[]={"Unchanged","Off","Allies only","Normal","Enhanced"};
static const char*const tl_range[]={"Unchanged","10m","15m","25m","50m","75m","100m","150m"};
static const char*const tl_weap[]={"Battle rifle","Assault rifle","Plasma pistol","Spiker","SMG","Carbine","Energy sword","Magnum","Needler",
    "Plasma rifle","Rocket launcher","Shotgun","Sniper rifle","Brute shot","Unarmed","Beam rifle","Spartan laser","No weapon","Gravity hammer",
    "Mauler","Flamethrower","Missile pod","Sentinel beam","Fuel rod gun","Silenced SMG","Automag","Brute plasma rifle","Machine gun turret",
    "Plasma turret","Golf club"};
#define TL(a) a,(int)(sizeof a/sizeof*a)
typedef struct { const char*key; int off, sz; const char*const*lab; int nlab; int def; } StTraitF;   // sz: 1 u8, 2 u16, -1 i8 weapon, 0 colour
static const StTraitF g_trait_f[]={
    {"resistance",0x00,1,TL(tl_res),4},{"recharge",0x01,1,TL(tl_rech),7},{"vampirism",0x02,1,TL(tl_vamp),1},{"headshots",0x03,1,TL(tl_head),2},
    {"shields",0x04,1,TL(tl_shield),2},{"grenades",0x08,2,TL(tl_gren),1},{"primary_weapon",0x0A,-1,TL(tl_weap),-1},{"secondary_weapon",0x0B,-1,TL(tl_weap),-1},
    {"damage",0x0C,1,TL(tl_dmg),6},{"grenade_regeneration",0x0D,1,TL(tl_regen),2},{"ammo",0x0E,1,TL(tl_ammo),1},{"pickup",0x0F,1,TL(tl_pick),1},
    {"speed",0x10,1,TL(tl_speed),5},{"gravity",0x11,1,TL(tl_grav),3},{"vehicles",0x12,1,TL(tl_veh),3},{"camouflage",0x14,1,TL(tl_camo),1},
    {"waypoint",0x15,1,TL(tl_wp),2},{"aura",0x16,1,TL(tl_aura),1},{"color",0x17,0,0,0,1},{"radar",0x18,2,TL(tl_radar),3},{"radar_range",0x1A,2,TL(tl_range),3}};
// active=0: a variant's trait set, listing the fields it changes; active=1: a player's resolved traits, listing the
// fields that differ from Halo 3's defaults
static void st_traits(StBuf*s,const uint8_t*t,int active){
    sb_put(s,"{"); int first=1;
    for(size_t i=0;i<sizeof g_trait_f/sizeof*g_trait_f;i++){ const StTraitF*f=&g_trait_f[i]; int v; char buf[48]; const char*lab=0;
        if(f->sz==2) v=*(const uint16_t*)(t+f->off); else if(f->sz==-1) v=*(const int8_t*)(t+f->off); else v=t[f->off];
        if(f->sz==-1){ if(active?v==f->def:v==-2) continue;
            if(v==-3) lab="Random"; else if(v==-1) lab="Map default"; else if(v==-2) lab="Unchanged"; else if(v>=0 && v<f->nlab) lab=f->lab[v]; }
        else { if(active?v==f->def:v==0) continue;
            if(f->lab && v>=0 && v<f->nlab) lab=f->lab[v]; else if(!f->lab){ if(v==0) lab="Unchanged"; else if(v==1) lab="Off"; else { snprintf(buf,sizeof buf,"Color %d",v-2); lab=buf; } } }
        if(!lab){ snprintf(buf,sizeof buf,"%d",v); lab=buf; }
        sb_put(s,"%s\"%s\":",first?"":",",f->key); sb_str(s,lab); first=0; }
    sb_put(s,"}"); }
static uint8_t g_st_var[0x4C4]; static int g_st_var_ok;   // this snapshot's variant (for the players' powerup durations)
static void st_variant(StBuf*s){
    g_st_var_ok=0; uint64_t gg=0; uint8_t mode=0;
    if(!g_st_tls || !hb_read(g_st_tls+0x48,&gg,8) || !gg || !hb_read(gg+0x10,&mode,1) || mode!=2 || !hb_read(gg+0x1608,g_st_var,sizeof g_st_var)) return;
    const uint8_t*v=g_st_var; int32_t eng=*(const int32_t*)v; if(eng<1 || eng>12 || eng==11) return; g_st_var_ok=1;
    static const char*const en[13]={"","Capture the Flag","Slayer","Oddball","King of the Hill","Forge","VIP","Juggernaut","Territories","Assault","Infection","","Gun Game"};
    char nm[64]; int j=0; for(int c=0;c<16 && j<60;c++){ uint16_t u=*(const uint16_t*)(v+0x14+c*2); if(!u) break;
        if(u<0x80) nm[j++]=(char)u; else if(u<0x800){ nm[j++]=(char)(0xC0|(u>>6)); nm[j++]=(char)(0x80|(u&63)); } else { nm[j++]=(char)(0xE0|(u>>12)); nm[j++]=(char)(0x80|((u>>6)&63)); nm[j++]=(char)(0x80|(u&63)); } }
    nm[j]=0;
    static const char*const tc[3]={"Disabled","Enabled","Balancing only"}; uint16_t soc=*(const uint16_t*)(v+0x12C), tch=*(const uint16_t*)(v+0x12E); uint32_t mo=*(const uint32_t*)(v+0x130);
    sb_put(s,",\"variant\":{\"engine\":%d,\"mode\":",eng); sb_str(s,en[eng]); sb_put(s,",\"name\":"); sb_str(s,nm);
    // the raw variant (0x4C4 bytes): the page decodes every option from it with the client's option table
    { static const char hx[]="0123456789abcdef"; char raw[sizeof g_st_var*2+1]; for(size_t i=0;i<sizeof g_st_var;i++){ raw[2*i]=hx[g_st_var[i]>>4]; raw[2*i+1]=hx[g_st_var[i]&15]; }
      raw[sizeof g_st_var*2]=0; sb_put(s,",\"raw\":\"%s\"",raw); }
    sb_put(s,",\"rounds\":{\"teams\":%s,\"team_changing\":\"%s\",\"friendly_fire\":%s,\"betrayal_booting\":%s,\"reset_players\":%s,\"reset_map\":%s,\"minutes\":%d,\"limit\":%d,\"early_victory\":%d}",
        v[0x104]&1?"true":"false",tch<3?tc[tch]:"?",soc&1?"true":"false",soc&2?"true":"false",v[0x104]&2?"true":"false",v[0x104]&4?"true":"false",v[0x105],v[0x106],v[0x107]);
    sb_put(s,",\"respawn\":{\"lives\":%d,\"team_lives\":%d,\"seconds\":%d,\"suicide\":%d,\"betrayal\":%d,\"growth\":%d,\"traits_duration\":%d,\"synchronize\":%s,\"on_kills\":%s}",
        v[0x109],v[0x10A],v[0x10B],v[0x10C],v[0x10D],v[0x10E],v[0x10F],v[0x108]&2?"true":"false",v[0x108]&8?"true":"false");
    sb_put(s,",\"map\":{\"grenades\":\"%s\",\"indestructible_vehicles\":%s}",mo&1?"Map default":"None",mo&2?"true":"false");
    sb_put(s,",\"powerups\":{\"red_duration\":%d,\"blue_duration\":%d,\"yellow_duration\":%d}",v[0x1A8],v[0x1A9],v[0x1AA]);
    { int sw=0; switch(eng){ case 2: case 6: case 7: sw=0x1B4; break; case 3: case 4: sw=0x1B8; break; case 1: case 9: sw=0x1BC; break; case 12: sw=0x1BE; break; }
      if(sw) sb_put(s,",\"score_to_win\":%d",*(const int16_t*)(v+sw)); }
    if(eng==2){ static const char*const pk[12]={"assist","death","suicide","betrayal","leader_kill","elimination","assassination","headshot","beatdown","sticky","splatter","spree"};
        sb_put(s,",\"points\":{\"kill\":%d",*(const int16_t*)(v+0x1B6)); for(int k=0;k<12;k++) sb_put(s,",\"%s\":%d",pk[k],(int8_t)v[0x1B8+k]); sb_put(s,"}"); }
    // trait sets: the base ones, then this mode's role sets
    typedef struct { const char*n; int off; } TS; static const TS base[5]={{"player",0x134},{"respawn_traits",0x110},{"red_powerup",0x154},{"blue_powerup",0x170},{"yellow_powerup",0x18C}};
    TS role[8]; int nr=0;
    switch(eng){ case 1: role[nr++]=(TS){"flag_carrier",0x1C0}; break; case 2: role[nr++]=(TS){"leader",0x1C4}; break; case 3: role[nr++]=(TS){"ball_carrier",0x1C4}; break;
        case 4: role[nr++]=(TS){"hill_occupant",0x1C0}; break; case 5: role[nr++]=(TS){"editor",0x1B8}; break;
        case 6: role[nr++]=(TS){"vip_team",0x1C6}; role[nr++]=(TS){"vip_proximity",0x1E2}; role[nr++]=(TS){"vip",0x1FE}; break;
        case 7: role[nr++]=(TS){"juggernaut",0x1C4}; break; case 8: role[nr++]=(TS){"defender",0x1BC}; role[nr++]=(TS){"attacker",0x1D8}; break;
        case 9: role[nr++]=(TS){"bomb_carrier",0x1C8}; role[nr++]=(TS){"bomb_arming",0x1E4}; break;
        case 10: role[nr++]=(TS){"infected",0x1C0}; role[nr++]=(TS){"initial_infected",0x1DC}; role[nr++]=(TS){"safe_haven",0x1F8}; role[nr++]=(TS){"last_survivor",0x214}; break;
        case 12: role[nr++]=(TS){"final_tier",0x2D0}; role[nr++]=(TS){"leader",0x2EC}; break; }
    sb_put(s,",\"traits\":{"); for(int k=0;k<5;k++){ sb_put(s,"%s\"%s\":",k?",":"",base[k].n); st_traits(s,v+base[k].off,0); }
    for(int k=0;k<nr;k++){ sb_put(s,",\"%s\":",role[k].n); st_traits(s,v+role[k].off,0); }
    sb_put(s,"}}"); }
// A player's active traits and what is driving them right now (a powerup, respawn traits)
static void st_player_traits(StBuf*s,uint64_t rec){
    uint8_t r[0x228]; if(!rec || !hb_read(rec,r,sizeof r)) return;
    sb_put(s,",\"traits\":"); st_traits(s,r+0x1FC,1);
    sb_put(s,",\"effects\":["); int first=1;
    uint64_t t=0; int32_t now=0; float spt=0;
    if(g_st_tls && hb_read(g_st_tls+0xc8,&t,8) && t && hb_read(t+0xC,&now,4) && hb_read(t+0x8,&spt,4) && spt>0 && spt<1 && g_st_var_ok){
        static const char*const pn[3]={"red powerup","blue powerup","yellow powerup"}; int best=-1; int32_t bt=0;
        for(int k=0;k<3;k++){ int32_t tk=*(const int32_t*)(r+0x218+k*4); if(tk>0 && tk>bt){ bt=tk; best=k; } }
        if(best>=0){ double left=g_st_var[0x1A8+best]-(now-bt)*(double)spt; if(left>0){ sb_put(s,"{\"what\":\"%s\",\"left\":%.1f}",pn[best],left); first=0; } }
        int16_t rt=*(const int16_t*)(r+0x1D4); if(rt>0 && !r[0x1D6]){ sb_put(s,"%s{\"what\":\"respawn traits\",\"left\":%.1f}",first?"":",",rt*(double)spt); first=0; } }
    sb_put(s,"]"); }
#undef g_st_tls

// joins and leaves: a name entering / leaving the players table (the lobby's session list is not read in the native app yet)
static void st_roster(const StPlayer* pl, int np, const char* map) {
    static char prev[256][64]; static int nprev = -1; static char pmap[64];
    if (strcmp(pmap, map)) { snprintf(pmap, sizeof pmap, "%s", map); nprev = -1; }   // a new map: the table fills again
    if (nprev >= 0) {
        for (int k = 0; k < np; k++) { int seen = 0; for (int i = 0; i < nprev && !seen; i++) seen = !strcmp(prev[i], pl[k].name); if (!seen) st_event_pos(0, "join", pl[k].name, 0, 0, -1, -1, 0, "%s joined the game", pl[k].name); }
        for (int i = 0; i < nprev; i++) { int still = 0; for (int k = 0; k < np && !still; k++) still = !strcmp(prev[i], pl[k].name); if (!still) st_event_pos(0, "leave", prev[i], 0, 0, -1, -1, 0, "%s left the game", prev[i]); }
    }
    nprev = np < 256 ? np : 256; for (int k = 0; k < nprev; k++) snprintf(prev[k], sizeof prev[k], "%s", pl[k].name);
}

// ---- the scoreboard: halo3's statborg (static RE of halo3.dll, shim/docs/native-client.md "Kills") -------------------
// c_game_statborg (halo3+0x10057c returns game engine globals (TLS +0x20) + 0xfeb8; the app moves it, see g_sb_off):
// player p's 32 i16 stats at +p*0x40, adjusted by halo3+0x1da7e0 (statborg, player, stat, delta, results stat), which
// also feeds the game results' named statistics -- that names them: 0 score, 2 kills (results 9 "kills"), 3 deaths
// (11), 4 suicides (13), 5 betrayals (12), 6 assists (10), 7 rounds won (6). The game engine replicates it to every
// client (each change marks the player's network bit), so a joined client reads the server's scoreboard.
enum { SB_SCORE = 0, SB_KILLS = 2, SB_DEATHS = 3, SB_SUICIDES = 4, SB_BETRAYALS = 5, SB_ASSISTS = 6 };
static int st_statborg(int slot, int16_t out[8]) {
    uint64_t blk = tls_block(), geg = 0;
    if (!blk || slot < 0 || slot >= 128 || !hb_read(blk + 0x20, &geg, 8) || !geg) return 0;
    if (!hb_read(geg + (uint64_t)g_sb_off + (uint64_t)slot * 0x40, out, 16)) return 0;
    for (int i = 0; i < 7; i++) if (out[i] < -30000 || out[i] > 30000) return 0;
    return 1;
}
// The kill feed from the scoreboard: a killer's kills (or betrayals) +1 and a victim's deaths +1 in the same ~second
// are one kill; a victim whose suicides went up too killed themselves. The pairing is inferred (several kills in one
// 100 ms snapshot pair in slot order) and the weapon is the killer's weapon in hand (or vehicle) at that moment:
// these events are marked estimated. The counts themselves are the game's own.
typedef struct { int slot, team, kind; double t; char name[64], how[96]; } SbKiller;   // kind 0 kill, 1 betrayal
typedef struct { int slot, team, suicide, haspos; double t; float pos[3]; char name[64]; } SbVictim;
static SbKiller g_sbk[64]; static int g_nsbk; static SbVictim g_sbv[64]; static int g_nsbv;
static struct { int16_t s[8]; int ok; char name[64]; } g_sbprev[128];
typedef struct { char name[64], how[96]; int team, have, dead; float pos[3]; double died; } SbNow;   // this snapshot's view of a slot
static SbNow g_sbnow[128];
static void sb_reset(void) { memset(g_sbprev, 0, sizeof g_sbprev); g_nsbk = g_nsbv = 0; }
static void sb_tick(const StPlayer* pl, int np, double now) {
    for (int k = 0; k < np; k++) {
        int sl = pl[k].slot; int16_t s[8]; if (sl < 0 || sl >= 128 || !st_statborg(sl, s)) continue;
        if (!g_sbprev[sl].ok || strcmp(g_sbprev[sl].name, pl[k].name) || s[SB_DEATHS] < g_sbprev[sl].s[SB_DEATHS] || s[SB_KILLS] < g_sbprev[sl].s[SB_KILLS]) {
            memcpy(g_sbprev[sl].s, s, sizeof s); g_sbprev[sl].ok = 1; snprintf(g_sbprev[sl].name, sizeof g_sbprev[sl].name, "%s", pl[k].name); continue; }
        const int16_t* o = g_sbprev[sl].s; const SbNow* w = &g_sbnow[sl];
        for (int d = 0; d < s[SB_KILLS] - o[SB_KILLS] && g_nsbk < 64; d++) { SbKiller* x = &g_sbk[g_nsbk++]; x->slot = sl; x->team = w->team; x->kind = 0; x->t = now; snprintf(x->name, sizeof x->name, "%s", pl[k].name); snprintf(x->how, sizeof x->how, "%s", w->how); }
        for (int d = 0; d < s[SB_BETRAYALS] - o[SB_BETRAYALS] && g_nsbk < 64; d++) { SbKiller* x = &g_sbk[g_nsbk++]; x->slot = sl; x->team = w->team; x->kind = 1; x->t = now; snprintf(x->name, sizeof x->name, "%s", pl[k].name); snprintf(x->how, sizeof x->how, "%s", w->how); }
        int sui = s[SB_SUICIDES] - o[SB_SUICIDES];
        for (int d = 0; d < s[SB_DEATHS] - o[SB_DEATHS] && g_nsbv < 64; d++) { SbVictim* v = &g_sbv[g_nsbv++]; v->slot = sl; v->team = w->team; v->suicide = d < sui; v->t = now;
            snprintf(v->name, sizeof v->name, "%s", pl[k].name); v->haspos = w->died > 0 && now - w->died < 3; memcpy(v->pos, w->pos, 12); }
        memcpy(g_sbprev[sl].s, s, sizeof s);
    }
    // pair: victims oldest first; a victim waits up to 1.5 s for its killer's count (they replicate separately)
    for (int i = 0; i < g_nsbv; i++) {
        SbVictim* v = &g_sbv[i]; const float* pos = v->haspos ? v->pos : 0; int done = 0;
        if (v->suicide) { st_event_pos(pos, "death", 0, v->name, 0, -1, v->team, 0, "%s killed themselves", v->name); done = 1; }
        else {
            int best = -1;
            for (int j = 0; j < g_nsbk; j++) { if (g_sbk[j].slot == v->slot) continue; if (best < 0) best = j;
                if (g_sbk[j].kind == 1 && g_sbk[j].team == v->team && v->team >= 0) { best = j; break; } }
            if (best >= 0) { SbKiller x = g_sbk[best]; memmove(&g_sbk[best], &g_sbk[best + 1], (size_t)(g_nsbk - best - 1) * sizeof x); g_nsbk--;
                char hw[110] = ""; if (x.how[0]) snprintf(hw, sizeof hw, " (%s)", x.how);
                const char* verb = x.kind ? "betrayed" : "killed";
                st_event_verb(pos, "kill", x.name, v->name, x.how, verb, x.team, v->team, 1 | (x.kind ? 4 : 0), "%s %s %s%s", x.name, verb, v->name, hw); done = 1; }
            else if (now - v->t > 1.5) { st_event_pos(pos, "death", 0, v->name, 0, -1, v->team, 0, "%s was killed", v->name); done = 1; }
        }
        if (done) { memmove(&g_sbv[i], &g_sbv[i + 1], (size_t)(g_nsbv - i - 1) * sizeof *v); g_nsbv--; i--; }
    }
    for (int j = 0; j < g_nsbk; j++) if (now - g_sbk[j].t > 2.0) { memmove(&g_sbk[j], &g_sbk[j + 1], (size_t)(g_nsbk - j - 1) * sizeof g_sbk[0]); g_nsbk--; j--; }
}

// ---- halo3's game engine events (display_event, halo3+0x11b0ec: kills, joins, flag / bomb / hill lines) -------------
// The loader read the 0x2c-byte event (type +0, index +4, cause / effect player +0x10 / +0x18 ...); the app enlarges it
// ([capacity] event.outgoing: 0x2c -> 0x40, event.bytes 0x40 -> 0x60) and its new layout is not known yet: each event's
// 0x60 bytes are kept and the first NHOOK_EVENT_LOG (default 200) are logged as "[nhook] [event]" lines, to learn it
// from a real match. The kill feed comes from the scoreboard meanwhile (sb_tick).
static uint8_t g_gev[256][0x60]; static double g_gev_t[256]; static volatile unsigned g_gev_n;
static void on_display_event(uint64_t* ctx) {             // (engine thread; rcx = the event)
    unsigned i = g_gev_n; uint64_t ev = ctx[1];
    if (ev >= 0x10000 && !rd(ev, g_gev[i & 255], 0x60)) { g_gev_t[i & 255] = st_wall(); __atomic_store_n(&g_gev_n, i + 1, __ATOMIC_RELEASE); }
}
static void st_engine_events(const StPlayer* pl, int np) {
    static unsigned seen; static int logged, lim = -1; if (lim < 0) lim = getenv("NHOOK_EVENT_LOG") ? atoi(getenv("NHOOK_EVENT_LOG")) : 200;
    unsigned n = __atomic_load_n(&g_gev_n, __ATOMIC_ACQUIRE); if (n - seen > 256) seen = n - 256;
    for (; seen != n; seen++) {
        const uint8_t* e = g_gev[seen & 255]; if (logged >= lim) continue; logged++;
        char hx[0x60 * 2 + 16]; int k = 0; for (int i = 0; i < 0x60; i++) { k += snprintf(hx + k, sizeof hx - (size_t)k, "%02x", e[i]); if ((i & 3) == 3 && i < 0x5f) hx[k++] = ' '; } hx[k] = 0;
        char who[200] = ""; int w = 0;   // which words look like a player handle (index + salt of a live player)
        for (int o = 0; o + 4 <= 0x60; o += 4) { uint32_t h = *(const uint32_t*)(e + o);
            for (int q = 0; q < np; q++) if (pl[q].slot == (int)(h & 0xFFFF) && h != 0xFFFFFFFFu) { uint16_t salt = 0;
                if (hb_read(pl[q].rec, &salt, 2) && salt == (uint16_t)(h >> 16)) w += snprintf(who + w, sizeof who - (size_t)w, " +0x%x=%s", o, pl[q].name); } }
        nlog("[nhook] [event] %.3f %s |%s", g_gev_t[seen & 255], hx, who);
    }
}

// ---- the snapshot (stats.c st_snapshot's shape) ---------------------------------------------------------------------
static void st_snapshot(StBuf* s) {
    static const char* teams[8] = {"red", "blue", "green", "orange", "purple", "gold", "brown", "pink"};
    double now = mono_s(), t0 = g_gameplay_t0;
    sb_put(s, "{\"time\":%.2f,\"in_match\":%s,\"inst\":%d,\"native\":true", t0 ? now - t0 : 0.0, t0 ? "true" : "false", (int)getpid());
    { pthread_mutex_lock(&g_ev_mx); uint32_t fs = g_ev_seq; pthread_mutex_unlock(&g_ev_mx); sb_put(s, ",\"feed_seq\":%u", fs); }
    sb_put(s, ",\"rec\":{\"state\":\"idle\",\"mode\":\"av\",\"secs\":0,\"elapsed\":0,\"left\":0,\"file\":\"\",\"width\":0,\"height\":0,\"frames\":0,\"dropped\":0,\"video_ok\":false,\"dir\":\"~/Movies/Reclaimer\",\"last_file\":\"\",\"last_error\":\"not in the native app yet\"}");
    sb_put(s, ",\"perf\":null,\"latch\":{\"mode\":\"off\",\"applying\":false,\"deg_per_count\":0,\"r2\":0,\"input_age_ms\":0,\"moving_frames\":0,\"last_deg\":0,\"in_vehicle_or_dead\":false}");
    sb_put(s, ",\"autovote\":{\"calls\":\"off\",\"map\":\"off\"}");
    char m[64] = "";
    { char mp[1024] = ""; hb_current_map(mp, sizeof mp);
      const char* b = strrchr(mp, '/'); b = b ? b + 1 : mp; snprintf(m, sizeof m, "%s", b); char* dot = strrchr(m, '.'); if (dot) *dot = 0;
      sb_put(s, ",\"map\":"); sb_str(s, m);
      if (__atomic_exchange_n(&g_match_pending, 0, __ATOMIC_ACQ_REL)) st_event_pos(0, "game", m, 0, 0, -1, -1, 0, "Match started: %s", g_loading[0] ? g_loading : m[0] ? m : "?");
      if (t0) st_maptop_ensure(mp, m); }
    sb_put(s, ",\"zone_show\":0,\"weak_show\":7,\"overlay\":{\"vehicles\":false,\"zones\":false,\"hitboxes\":false,\"tracers\":false,\"info\":false,\"items\":false,\"visible\":false,"
              "\"teams\":\"all\",\"teleport\":false,\"host\":false,\"tp_forward\":false,\"controls\":false,\"text_px\":7,\"spar\":false,\"spar_aim\":-1.0,\"spar_available\":false,"
              "\"bounds_off\":null,\"invwalls_off\":null,\"mic\":\"off\",\"mic_in_use\":false,\"speed_idx\":-1,\"quality\":\"\",\"features\":{}");
    sb_put(s, ",\"spots\":[]}");
    if (strcmp(m, g_trk_map)) { snprintf(g_trk_map, sizeof g_trk_map, "%s", m); g_ntrk = 0; g_nbip = 0; sb_reset(); }   // a new map: new match stats
    pthread_mutex_lock(&g_hm_mx); snprintf(g_hm_cur, sizeof g_hm_cur, "%s", m); if (g_hm.dirty && now - g_hm.saved > 30) { hm_save(); g_hm.saved = now; } pthread_mutex_unlock(&g_hm_mx);
    static uint8_t tab[HB_MAXOBJ * 0x18]; int hi = 0; uint64_t arr = hb_object_array(); uint8_t hdr[0x60];
    if (arr && hb_read(arr, hdr, sizeof hdr) && *(uint32_t*)(hdr + 0x2c) == 0x64407440 && *(int32_t*)(hdr + 0x24) == 0x18) {
        hi = *(int32_t*)(hdr + 0x3c);
        if (hi < 0 || hi > HB_MAXOBJ || hi > *(int32_t*)(hdr + 0x20) || !hb_read(*(uint64_t*)(hdr + 0x48), tab, (size_t)hi * 0x18)) hi = 0;
    }
    #define ST_DATUM(h) ((((h) & 0xFFFF) < (uint32_t)hi && (h) != 0xFFFFFFFFu && *(uint16_t*)(tab + ((h) & 0xFFFF) * 0x18) == (uint16_t)((h) >> 16)) ? *(uint64_t*)(tab + ((h) & 0xFFFF) * 0x18 + 0x10) : 0)
    #define ST_TYPE(h) (tab[((h) & 0xFFFF) * 0x18 + 3])
    StPlayer pl[256]; int np = st_players(pl, 256);
    // deaths: every biped, alive -> dead (stats.c); the killer is not known here yet
    { StBip nb[2048]; int nn = 0;
      for (int i = 0; i < hi && nn < 2048; i++) {
          const uint8_t* e = tab + i * 0x18; if (!*(const uint16_t*)e || e[3] != 0) continue;
          uint64_t p = *(const uint64_t*)(e + 0x10); uint8_t d[0x178]; if (!p || !hb_read(p, d, sizeof d)) continue;
          uint32_t h = ((uint32_t)*(const uint16_t*)e << 16) | (uint32_t)i; int alive = !((*(const uint32_t*)(d + 0x110) >> 2) & 1), ti = -1;
          for (int k = 0; k < np; k++) if (pl[k].unit == h) { StTrack* t = st_track(&pl[k]); if (t) ti = (int)(t - g_trk); break; }
          int prev = -1; for (int b = 0; b < g_nbip; b++) if (g_bip[b].h == h) { prev = b; break; }
          if (prev >= 0) {
              if (ti < 0) ti = g_bip[prev].ti;
              if (g_bip[prev].alive && !alive) {
                  StTrack* t = ti >= 0 ? &g_trk[ti] : 0; int team = *(const int32_t*)(d + 0x174);
                  if (t) { t->deaths++; t->alive = 0; if (now - t->life_start > t->longest) t->longest = now - t->life_start;
                           int16_t sbs[8];
                           if (t->slot >= 0 && t->slot < 128) { g_sbnow[t->slot].died = now; memcpy(g_sbnow[t->slot].pos, d + 0x1c, 12); }
                           if (!st_statborg(t->slot, sbs)) st_event_pos((const float*)(d + 0x1c), "death", 0, t->name, 0, -1, team, 0, "%s died", t->name);
                           pthread_mutex_lock(&g_hm_mx); hm_death((const float*)(d + 0x1c), team, t->slot, t->name); pthread_mutex_unlock(&g_hm_mx); }
              }
          }
          nb[nn].h = h; nb[nn].alive = alive; nb[nn].ti = ti; nn++;
      }
      memcpy(g_bip, nb, (size_t)nn * sizeof *nb); g_nbip = nn; }
    st_variant(s);   // the game's options and trait sets
    st_roster(pl, np, m);
    { static StSess ss[128]; uint32_t sst = 0; int nss = st_session(ss, 128, &sst), hostplayer = 0;
      sb_put(s, ",\"session\":["); for (int k = 0; k < nss; k++) { sb_put(s, "%s{\"name\":", k ? "," : ""); sb_str(s, ss[k].name);
          sb_put(s, ",\"index\":%d,\"peer\":%d,\"host\":%s,\"voice\":\"%016llx\",\"muted\":false,\"talking\":false}", ss[k].idx, ss[k].peer, ss[k].host ? "true" : "false", (unsigned long long)ss[k].voice);
          if (ss[k].host) hostplayer = 1; }
      sb_put(s, "],\"session_state\":%u,\"host_peer\":%d,\"local_peer\":%d,\"dedicated\":%s,\"mute\":false", sst, g_st_hostpeer, g_st_localpeer, g_st_hostpeer >= 0 && nss && !hostplayer ? "true" : "false");
      int mx = st_max_players(); sb_put(s, ",\"slots\":{\"used\":%d", nss ? nss : np); if (mx) sb_put(s, ",\"max\":%d,\"src\":\"session\"}", mx); else sb_put(s, ",\"max\":null}");
      sb_put(s, ",\"addr_role\":\"hidden\""); }
    sb_put(s, ",\"players\":["); int first = 1;
    for (int k = 0; k < np; k++) {
        StTrack* t = st_track(&pl[k]); uint32_t h = pl[k].unit; uint64_t p = ST_DATUM(h); static uint8_t d[0x380];
        int have = p && ST_TYPE(h) == 0 && hb_read(p, d, sizeof d); int dead = !have || ((*(const uint32_t*)(d + 0x110) >> 2) & 1);
        const float* o = have ? (const float*)(d + 0x1c) : (t ? t->pos : (const float[3]){0, 0, 0}); float spd = 0; int team = have ? *(const int32_t*)(d + 0x174) : (t ? t->team : -1);
        if (have) { const float* v = (const float*)(d + 0x74); spd = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
        if (t) {
            if (have && !dead) {
                if (!t->alive || t->unit != h) { t->alive = 1; t->life_start = now; }
                else { float dx = o[0] - t->pos[0], dy = o[1] - t->pos[1], dz = o[2] - t->pos[2], dd = sqrtf(dx * dx + dy * dy + dz * dz); if (dd < 20) t->dist += dd; }
                if (spd > t->top_speed && spd < 50) t->top_speed = spd;
                memcpy(t->pos, o, 12);
                if (t0) { pthread_mutex_lock(&g_hm_mx); hm_switch(g_hm_cur); hm_cell_add((int32_t)floorf(o[0] / HM_CELL), (int32_t)floorf(o[1] / HM_CELL), 1); g_hm.dirty = 1; pthread_mutex_unlock(&g_hm_mx); }
            } else if (t->alive && !have) t->alive = 0;
            t->unit = h; if (team >= 0) t->team = team;
        }
        if (pl[k].slot >= 0 && pl[k].slot < 128) { SbNow* w = &g_sbnow[pl[k].slot]; snprintf(w->name, sizeof w->name, "%s", pl[k].name); w->team = team; w->have = have; w->dead = dead; w->how[0] = 0;
            if (have && !dead) { uint32_t parent = *(const uint32_t*)(d + 0x10); uint64_t vd = parent != 0xFFFFFFFFu ? ST_DATUM(parent) : 0; uint32_t df = 0;
                if (vd && hb_read(vd, &df, 4) && *(const int16_t*)(d + 0x24E) == 0) snprintf(w->how, sizeof w->how, "%s", hb_tag_name(df));   // the driver: a splatter is possible
                else { int ws = *(const int8_t*)(d + 0x262); const uint32_t* wp = (const uint32_t*)(d + 0x268); uint64_t wd = (ws >= 0 && ws < 4) ? ST_DATUM(wp[ws]) : 0;
                       if (wd && hb_read(wd, &df, 4)) snprintf(w->how, sizeof w->how, "%s", hb_tag_name(df)); } } }
        int you = g_nick[0] && !strcmp(pl[k].name, g_nick);
        sb_put(s, "%s{\"name\":", first ? "" : ","); first = 0; sb_str(s, pl[k].name); sb_put(s, ",\"slot\":%d", pl[k].slot);
        sb_put(s, ",\"handle\":\"%08x\",\"team\":%d,\"team_name\":\"%s\",\"you\":%s,\"alive\":%s,\"health\":%.3f,\"shield\":%.3f,\"camo\":%.3f",
               h, team, (team >= 0 && team < 8) ? teams[team] : "none", you ? "true" : "false", dead ? "false" : "true", dead ? 0 : *(const float*)(d + 0xF4), dead ? 0 : *(const float*)(d + 0xF8), have ? *(const float*)(d + 0x360) : 0);
        sb_put(s, ",\"weapon\":");
        if (have && !dead) {
            int ws = *(const int8_t*)(d + 0x262); const uint32_t* wp = (const uint32_t*)(d + 0x268); uint64_t wd = (ws >= 0 && ws < 4) ? ST_DATUM(wp[ws]) : 0; uint32_t def = 0;
            if (wd && hb_read(wd, &def, 4)) st_tag(s, def); else sb_put(s, "null");
            sb_put(s, ",\"weapons\":[");
            for (int w = 0, fw = 1; w < 4; w++) { uint64_t wd2 = ST_DATUM(wp[w]); uint32_t df = 0; if (!wd2 || !hb_read(wd2, &df, 4)) continue; sb_put(s, "%s", fw ? "" : ","); fw = 0; st_tag(s, df); }
            sb_put(s, "]");
            const int8_t* gr = (const int8_t*)(d + 0x294); sb_put(s, ",\"grenades\":{\"frag\":%d,\"plasma\":%d,\"spike\":%d,\"firebomb\":%d}", gr[0], gr[1], gr[2], gr[3]);
            uint32_t parent = *(const uint32_t*)(d + 0x10); sb_put(s, ",\"vehicle\":");
            { uint64_t vd = parent != 0xFFFFFFFFu ? ST_DATUM(parent) : 0; uint32_t df = 0;
              if (vd && hb_read(vd, &df, 4)) {
                  st_tag(s, df); sb_put(s, ",\"seat\":%d", *(const int16_t*)(d + 0x24E));
                  uint32_t rh = parent;
                  for (int dep = 0; dep < 4; dep++) { uint64_t r2 = ST_DATUM(rh); uint32_t up = 0xFFFFFFFFu; if (!r2 || !hb_read(r2 + 0x10, &up, 4) || up == 0xFFFFFFFFu || !ST_DATUM(up)) break; rh = up; }
                  sb_put(s, ",\"in\":\"%08x\"", rh);
              } else sb_put(s, "null"); }
            const float* fwv = (const float*)(d + 0x5c); sb_put(s, ",\"facing\":%.0f", atan2f(fwv[1], fwv[0]) * 57.2958f);
        } else sb_put(s, "null,\"weapons\":[],\"grenades\":{\"frag\":0,\"plasma\":0,\"spike\":0,\"firebomb\":0},\"vehicle\":null");
        int16_t sbs[8]; int sbok = st_statborg(pl[k].slot, sbs);
        sb_put(s, ",\"pos\":"); sb_f3(s, o); sb_put(s, ",\"speed\":%.2f,\"kills\":%d", spd, sbok ? sbs[SB_KILLS] : 0);
        if (sbok) sb_put(s, ",\"score\":%d,\"assists\":%d,\"betrayals\":%d,\"suicides\":%d", sbs[SB_SCORE], sbs[SB_ASSISTS], sbs[SB_BETRAYALS], sbs[SB_SUICIDES]);
        if (t) sb_put(s, ",\"deaths\":%d,\"hits\":0,\"accuracy\":0,\"shots\":0,\"life\":%.1f,\"longest_life\":%.1f,\"distance\":%.1f,\"top_speed\":%.2f", sbok ? sbs[SB_DEATHS] : t->deaths, t->alive ? now - t->life_start : 0.0,
                      t->longest > (t->alive ? now - t->life_start : 0) ? t->longest : (t->alive ? now - t->life_start : 0), t->dist, t->top_speed);
        if (g_st_var_ok) st_player_traits(s, pl[k].rec);
        sb_put(s, "}");
    }
    if (t0) sb_tick(pl, np, now);
    st_engine_events(pl, np);
    sb_put(s, "],\"vehicles\":["); first = 1;
    for (int i = 0; i < hi; i++) {
        const uint8_t* e = tab + i * 0x18; if (!*(const uint16_t*)e || e[3] != 1) continue;
        uint64_t p = *(const uint64_t*)(e + 0x10); uint8_t d[0x120]; if (!p || !hb_read(p, d, sizeof d)) continue;
        if (*(const uint32_t*)(d + 0x10) != 0xFFFFFFFFu) continue;
        uint32_t def = *(const uint32_t*)d; int dead = (*(const uint32_t*)(d + 0x110) >> 2) & 1; const float* fwv = (const float*)(d + 0x5c);
        sb_put(s, "%s{\"type\":", first ? "" : ","); first = 0; st_tag(s, def);
        sb_put(s, ",\"id\":\"%08x\",\"def\":\"%08x\",\"yaw\":%.0f,\"dead\":%s", ((uint32_t)*(const uint16_t*)e << 16) | (uint32_t)i, def, atan2f(fwv[1], fwv[0]) * 57.2958f, dead ? "true" : "false");
        sb_put(s, ",\"health\":%.3f,\"shield\":%.3f,\"pos\":", dead ? 0 : *(const float*)(d + 0xF4), dead ? 0 : *(const float*)(d + 0xF8)); sb_f3(s, (const float*)(d + 0x50)); sb_put(s, "}");
    }
    sb_put(s, "],\"projectiles\":["); first = 1;
    for (int i = 0; i < hi; i++) {
        const uint8_t* e = tab + i * 0x18; if (!*(const uint16_t*)e || e[3] != 5) continue;
        uint64_t p = *(const uint64_t*)(e + 0x10); uint8_t d[0xC8]; if (!p || !hb_read(p, d, sizeof d)) continue;
        uint32_t own = *(const uint32_t*)(d + 0xC0); const char* on = 0; for (int k = 0; k < np; k++) if (pl[k].unit == own) { on = pl[k].name; break; }
        const float* v = (const float*)(d + 0x74); sb_put(s, "%s{\"type\":", first ? "" : ","); first = 0; st_tag(s, *(const uint32_t*)d);
        sb_put(s, ",\"owner\":"); sb_str(s, on ? on : ""); sb_put(s, ",\"pos\":"); sb_f3(s, (const float*)(d + 0x50)); sb_put(s, ",\"speed\":%.1f}", sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    sb_put(s, "],\"items\":[],\"deployed\":[]}");
    #undef ST_DATUM
    #undef ST_TYPE
}

// ---- HTTP (stats.c's server, the read-only part) --------------------------------------------------------------------
static int g_st_port = 8765;
static volatile unsigned long g_st_hits;                   // page requests (/events, /stats.json)
static pthread_mutex_t g_st_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_st_cv = PTHREAD_COND_INITIALIZER;
static char* g_st_json; static size_t g_st_len; static unsigned long g_st_seq;
static void* st_snap_thread(void* a) {
    (void)a; pthread_setname_np("nhook-stats-snap");
    for (;;) {
        StBuf s = {0}; st_snapshot(&s);
        if (s.b) { pthread_mutex_lock(&g_st_mx); free(g_st_json); g_st_json = s.b; g_st_len = s.n; g_st_seq++; pthread_cond_broadcast(&g_st_cv); pthread_mutex_unlock(&g_st_mx); }
        usleep(100000);
    }
    return 0;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const char g_st_page[] = {
#embed "../src/stats_page.html"
, 0};
static const char g_st_optmap[] = {
#embed "../docs/option-map.json"
, 0};
#pragma clang diagnostic pop
static int st_send(int fd, const char* b, size_t n) { while (n) { ssize_t k = send(fd, b, n, 0); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; } b += k; n -= (size_t)k; } return 1; }
static const char* st_hdr(const char* hdrs, const char* name, char* out, size_t on) {
    size_t nl = strlen(name);
    for (const char* p = strstr(hdrs, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char* l = p + 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') { l += nl + 1; while (*l == ' ' || *l == '\t') l++; size_t k = 0; while (l[k] && l[k] != '\r' && k + 1 < on) { out[k] = l[k]; k++; }
            while (k && (out[k - 1] == ' ' || out[k - 1] == '\t')) k--; out[k] = 0; return out; }
    }
    return 0;
}
static int st_host_ok(const char* hdrs) {                  // the Host must name us by IP literal or localhost (DNS rebinding)
    char v[256]; if (!st_hdr(hdrs, "Host", v, sizeof v)) return 0;
    char* port = 0; if (v[0] == '[') { char* rb = strchr(v, ']'); if (!rb) return 0; if (rb[1] == ':') port = rb + 2; else if (rb[1]) return 0; rb[1] = 0; }
    else { port = strrchr(v, ':'); if (port) *port++ = 0; }
    if (port ? atoi(port) != g_st_port : g_st_port != 80) return 0;
    if (!strcasecmp(v, "localhost") || v[0] == '[') return 1;
    struct in_addr ia; return inet_pton(AF_INET, v, &ia) == 1;
}
static void st_reply(int fd, const char* ctype, const char* body, size_t n, const char* extra) {
    char h[512]; int k = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\n%sConnection: close\r\n\r\n", ctype, n, extra ? extra : "");
    if (st_send(fd, h, (size_t)k) && n) st_send(fd, body, n);
}
static void* st_conn(void* a) {
    int fd = (int)(intptr_t)a; pthread_setname_np("nhook-stats-http");
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval rtv = {5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
    char req[4096]; size_t n = 0; char* he = 0;
    while (n < sizeof req - 1) { ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0); if (r <= 0) break; n += (size_t)r; req[n] = 0; if ((he = strstr(req, "\r\n\r\n"))) break; }
    if (!he) { close(fd); return 0; }
    he[2] = 0;
    if (!st_host_ok(req)) { const char* fb = "HTTP/1.1 421 Misdirected Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd, fb, strlen(fb)); close(fd); return 0; }
    char meth[8] = "", path[1024] = "/", qs[768] = ""; sscanf(req, "%7s %1023s", meth, path); char* q = strchr(path, '?'); if (q) { snprintf(qs, sizeof qs, "%s", q + 1); *q = 0; }
    if (!strcmp(path, "/control")) {                        // no controls in the native app yet
        const char* body = "{\"ok\":false,\"error\":\"controls are not in the native app yet\"}"; char h[300];
        int k = snprintf(h, sizeof h, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(body), body); st_send(fd, h, (size_t)k);
    } else if (strcmp(meth, "GET")) { const char* na = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd, na, strlen(na)); }
    else if (!strcmp(path, "/events")) {
        g_st_hits++; char h[256]; int k = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
        if (st_send(fd, h, (size_t)k))
            for (unsigned long seen = 0;;) {
                pthread_mutex_lock(&g_st_mx); while (g_st_seq == seen) pthread_cond_wait(&g_st_cv, &g_st_mx);
                seen = g_st_seq; size_t len = g_st_len; char* j = malloc(len + 16); if (j) { memcpy(j, "data: ", 6); memcpy(j + 6, g_st_json, len); memcpy(j + 6 + len, "\n\n", 2); }
                pthread_mutex_unlock(&g_st_mx);
                int ok = j && st_send(fd, j, len + 8); free(j); if (!ok) break;
            }
    } else if (!strcmp(path, "/stats.json")) {
        g_st_hits++; pthread_mutex_lock(&g_st_mx); size_t len = g_st_len; char* j = malloc(len + 1); if (j && g_st_json) memcpy(j, g_st_json, len); else len = 0; pthread_mutex_unlock(&g_st_mx);
        st_reply(fd, "application/json", j, len, 0); free(j);
    } else if (!strcmp(path, "/feed.json")) {
        const char* v = strstr(qs, "since="); StBuf b = {0}; st_feed_json(&b, v ? (uint32_t)strtoul(v + 6, 0, 10) : 0); st_reply(fd, "application/json", b.b, b.n, 0); free(b.b);
    } else if (!strcmp(path, "/heat.json")) { StBuf b = {0}; hm_json(&b); st_reply(fd, "application/json", b.b, b.n, 0); free(b.b); }
    else if (!strcmp(path, "/vshapes.json")) st_reply(fd, "application/json", "{\"gen\":0,\"shapes\":{}}", 21, 0);
    else if (!strcmp(path, "/recordings.json")) st_reply(fd, "application/json", "[]", 2, 0);
    else if (!strcmp(path, "/packets.json")) { static pthread_mutex_t pmx = PTHREAD_MUTEX_INITIALIZER; StBuf b = {0};
        pthread_mutex_lock(&pmx); pk_json(&b, qs); pthread_mutex_unlock(&pmx); st_reply(fd, "application/json", b.b, b.n, 0); free(b.b); }
    else if (!strcmp(path, "/option-map.json")) st_reply(fd, "application/json", g_st_optmap, sizeof g_st_optmap - 1, 0);
    else if (!strncmp(path, "/maptop/", 8)) {               // the loader's map pictures: ReclaimerMac/maptop/<map>.png|json|z.bin
        const char* nm = path + 8; size_t L = strlen(nm); int ok = L > 4 && L < 64;
        for (size_t i = 0; ok && i < L; i++) { char c = nm[i]; ok = isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'; }
        const char* ct = ok && !strcmp(nm + L - 4, ".png") ? "image/png" : ok && L > 5 && !strcmp(nm + L - 5, ".json") ? "application/json" : ok && L > 6 && !strcmp(nm + L - 6, ".z.bin") ? "application/octet-stream" : 0;
        char fp[1100]; const char* home = getenv("HOME"); FILE* f = 0; char* buf = 0; long fl = 0;
        if (ct && !strstr(nm, "..")) { snprintf(fp, sizeof fp, "%s/Library/Application Support/ReclaimerMac/maptop/%s", home ? home : "", nm); f = fopen(fp, "rb"); }
        if (f) { fseek(f, 0, SEEK_END); fl = ftell(f); fseek(f, 0, SEEK_SET); if (fl > 0 && fl < (32 << 20) && (buf = malloc((size_t)fl)) && fread(buf, 1, (size_t)fl, f) == (size_t)fl) {} else { free(buf); buf = 0; } fclose(f); }
        if (buf) { st_reply(fd, ct, buf, (size_t)fl, 0); free(buf); }
        else { const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd, nf, strlen(nf)); }
    } else if (!strcmp(path, "/")) {
        const char* pg = g_st_page; size_t pn = sizeof g_st_page - 1; char* disk = 0; const char* pf = getenv("NHOOK_STATS_PAGE");
        if (pf && *pf) { FILE* f = fopen(pf, "rb"); if (f) { fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
                if (fl > 0 && fl < (8 << 20) && (disk = malloc((size_t)fl)) && fread(disk, 1, (size_t)fl, f) == (size_t)fl) { pg = disk; pn = (size_t)fl; } fclose(f); } }
        st_reply(fd, "text/html; charset=utf-8", pg, pn, 0); free(disk);
    } else { const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd, nf, strlen(nf)); }
    close(fd); return 0;
}
// NHOOK_STATS_OPEN=1: open the page in the browser unless a tab asked for it within 5 s (an open tab reconnects by itself
// and reloads for a new game through the snapshot's "inst")
static void* st_open_thread(void* a) {
    (void)a; sleep(5); if (g_st_hits) return 0;
    char url[64]; snprintf(url, sizeof url, "http://127.0.0.1:%d/", g_st_port);
    char* argv[] = {(char*)"/usr/bin/open", url, 0}; extern char** environ; pid_t pid;
    if (!posix_spawn(&pid, "/usr/bin/open", 0, 0, argv, environ)) { int st; waitpid(pid, &st, 0); nlog("[nhook] opened %s in the browser", url); }
    return 0;
}
static void* st_listen_thread(void* a) {
    (void)a; pthread_setname_np("nhook-stats-listen");
    { pthread_t t; pthread_create(&t, 0, st_snap_thread, 0); pthread_detach(t); }
    int ls = -1;
    for (int tr = 0; ls < 0; tr++) {
        ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); fcntl(ls, F_SETFD, FD_CLOEXEC);
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_st_port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (struct sockaddr*)&sa, sizeof sa) || listen(ls, 16)) { int e = errno; close(ls); ls = -1;
            if (tr == 0) nlog("[nhook] cannot listen on 127.0.0.1:%d (%s), retrying every second", g_st_port, strerror(e)); sleep(1); }
    }
    nlog("[nhook] live game stats on http://127.0.0.1:%d/", g_st_port);
    if (getenv("NHOOK_STATS_OPEN") && atoi(getenv("NHOOK_STATS_OPEN"))) { pthread_t ot; pthread_create(&ot, 0, st_open_thread, 0); pthread_detach(ot); }
    for (;;) {
        int fd = accept(ls, 0, 0); if (fd < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        pthread_t c; if (pthread_create(&c, 0, st_conn, (void*)(intptr_t)fd)) close(fd); else pthread_detach(c);
    }
    return 0;
}
static void nstats_start(void) {
    const char* e = getenv("NHOOK_STATS_HTTP"); if (e) g_st_port = atoi(e); if (g_st_port <= 0 || g_st_port > 65535) return;
    Dl_info di; if (dladdr((void*)nstats_start, &di) && di.dli_fname) {   // the repo: <repo>/shim/build/nhook.dylib
        snprintf(g_repo, sizeof g_repo, "%s", di.dli_fname); for (int k = 0; k < 3; k++) { char* sl = strrchr(g_repo, '/'); if (sl) *sl = 0; } }
    nick_load(); { const char* pe = getenv("NHOOK_PKTLOG"); if (pe && *pe) g_pk_on = atoi(pe) != 0; }
    pthread_t t; pthread_create(&t, 0, st_listen_thread, 0); pthread_detach(t);
}
