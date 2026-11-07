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
static volatile int g_fx_on; static void fx_event(const StEvt* e); static __thread uint64_t t_st_chat_id;   // forensic.c
static void st_eventv(const float* pos, const char* kind, const char* a, const char* b, const char* how, const char* verb, int ta, int tb, int flags, const char* fmt, va_list ap) {
    StEvt e; memset(&e, 0, sizeof e); e.t = st_wall(); e.flags = flags; e.ta = ta; e.tb = tb; if (verb) snprintf(e.verb, sizeof e.verb, "%s", verb);
    if (pos) { memcpy(e.pos, pos, sizeof e.pos); e.haspos = 1; }
    snprintf(e.kind, sizeof e.kind, "%s", kind); snprintf(e.a, sizeof e.a, "%s", a ? a : ""); snprintf(e.b, sizeof e.b, "%s", b ? b : "");
    snprintf(e.how, sizeof e.how, "%s", how ? how : "");
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    pthread_mutex_lock(&g_ev_mx); e.seq = ++g_ev_seq; g_ev[e.seq % ST_NEV] = e; pthread_mutex_unlock(&g_ev_mx);
    if (g_fx_on) fx_event(&e);                                // the forensic archive (forensic.c)
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

static double st_talk_age(uint64_t id);   // (below: from the app's voice reports)
static void st_voice_report(const char* l);
#include "nnet.c"   // the network session and the packet log

// ---- the app's log lines -> match state and events ------------------------------------------------------------------
static volatile double g_gameplay_t0;                      // "[switch] game Playing" (mono time), 0 = not in a match
static volatile int g_match_pending; static char g_loading[96];
// the app's own game state ("[switch] game Playing" .. "[switch] game Idle / Loading"): it STAYS Playing when the server moves
// the session to its next map ("[loading] map ..." with no new "game Playing" line): that map's match starts once its
// players table fills (st_snapshot). The page used to sit at "not in a match", overlay off, for the whole next map (2026-11-07).
static volatile int g_app_playing;
static char g_srv_addr[96];                                // the server this app joined ("[online] joining <ip>:<port>")
static volatile int g_sp_join;                             // joined a game in progress: its first spawn burst is not spawn points (nspawns.c)
// where the app moved halo3's tables: it logs each move ("[capacity] <what>: <old> -> <new>", "[c1] ...")
static volatile int64_t g_sb_off = 0x159c0;                // game engine globals (TLS +0x20) -> statborg (halo3: +0xfeb8)
static uint64_t cap_new(const char* l, const char* what) { // "[capacity] <what>: 0x.. -> 0x..": the new value, 0 = not this line
    size_t wl = strlen(what); if (strncmp(l, "[capacity] ", 11) || strncmp(l + 11, what, wl) || l[11 + wl] != ':') return 0;
    const char* a = strstr(l + 11 + wl, "-> 0x"); return a ? strtoull(a + 5, 0, 16) : 0;
}
// halo3's game results ([c1] game results: 128 slots, 0x4a940 bytes at 0x...; [capacity] results.statistics / results.versus):
// per player 0x263 u16 statistics (halo3+0x792c80 names them: 9 kills, 10 assists, 11 deaths, 12 betrayals, 13 suicides ...;
// from 0xea per damage type: damage-kills / -deaths / -betrayals / -suicides / -headshots ...), then a killer x victim matrix
// (row = 1 << versus_row_log2 entries of 4 bytes). NHOOK_RESULTS_LOG=1 logs every change (to learn whether a client gets them).
static volatile uint64_t g_res_base; static volatile int64_t g_res_stats = 0x6c98, g_res_vs = 0x2cf98, g_res_vslog = 7;
static void nst_layout_line(const char* l) {
    uint64_t v;
    { const char* m = "[c1] game results: "; const char* at; if (!strncmp(l, m, strlen(m)) && (at = strstr(l, " at 0x"))) { g_res_base = strtoull(at + 6, 0, 16); return; } }
    if ((v = cap_new(l, "results.address"))) { g_res_base = v; return; }
    if ((v = cap_new(l, "results.statistics"))) { g_res_stats = (int64_t)v; return; }
    if ((v = cap_new(l, "results.versus"))) { g_res_vs = (int64_t)v; return; }
    if ((v = cap_new(l, "results.versus_row_log2"))) { g_res_vslog = (int64_t)v; return; }
    if ((v = cap_new(l, "game_globals.statborg"))) { g_sb_off = (int64_t)v; return; }
    if ((v = cap_new(l, "session.peers_shift"))) { g_ses_peers_shift = (int64_t)v; return; }
    if ((v = cap_new(l, "session.players_shift"))) { g_ses_players_shift = (int64_t)v; return; }
    const char* m = "[c1] network sessions moved to 0x"; if (!strncmp(l, m, strlen(m))) g_ses_store = strtoull(l + strlen(m), 0, 16);
}
static void nst_on_line(const char* p, size_t n) {
    char l[4096]; if (n >= sizeof l) n = sizeof l - 1; memcpy(l, p, n); l[n] = 0;   // (a voice report names every sender: long)
    if (!strncmp(l, "[voice] active ", 15)) { st_voice_report(l); return; }
    if (!strncmp(l, "[capacity] ", 11) || !strncmp(l, "[c1] ", 5)) { nst_layout_line(l); return; }
    if (!strncmp(l, "[menus] joining the game in progress", 36)) { g_sp_join = 1; return; }
    if (!strncmp(l, "[online] joining ", 17)) { size_t k = 0; const char* q = l + 17; char a[96]; while (*q && *q != ' ' && k + 1 < sizeof a) a[k++] = *q++; a[k] = 0; if (k) memcpy(g_srv_addr, a, k + 1); return; }
    if (!strncmp(l, "[switch] game Playing", 21)) { g_app_playing = 1; if (!g_gameplay_t0) { g_gameplay_t0 = mono_s(); g_match_pending = 1; } return; }
    if (!strncmp(l, "[loading] map ", 14)) {               // "[loading] map 320: Guardian, MCC's art of Guardian"
        const char* c = strchr(l, ':'); if (c) { c++; while (*c == ' ') c++; size_t k = 0; while (c[k] && c[k] != ',' && k + 1 < sizeof g_loading) { g_loading[k] = c[k]; k++; } g_loading[k] = 0; }
        g_gameplay_t0 = 0; g_hb_map_recheck = 1; return;
    }
    // out of a match: the app's own game state ("[switch] game Idle / Loading", "leaving the game"). NOT "[menus] open":
    // the in-game menu (scoreboard, settings) logs that too, and the page used to drop out of the match there (2026-11-07)
    if (!strncmp(l, "[host] stopping", 15) || !strncmp(l, "[switch] leaving the game", 25) || (!strncmp(l, "[switch] game ", 14) && strncmp(l + 14, "Playing", 7))) { g_gameplay_t0 = 0; g_app_playing = 0; return; }
    // "[online] peer 0x<id> at <ip>:<port>: rtt ..." (every few seconds per peer): which session peer an address is
    if (!strncmp(l, "[online] peer 0x", 16)) { char* at = strstr(l, " at "); if (at) { uint64_t id = strtoull(l + 16, 0, 16); char ip[64]; size_t k = 0; const char* q = at + 4;
            while (*q && *q != ' ' && k + 1 < sizeof ip) ip[k++] = *q++; ip[k] = 0; if (k && ip[k - 1] == ':') ip[k - 1] = 0; pk_peer_note(id, ip); } return; }
    // chat (the app prints these only with its chat_log byte set: chat_log_tick): "[chat] line [GLOBAL|TEAM|NEARBY] <name>
    // (0x<sender id>): <text>", "[chat] line [SERVER #rrggbb] <text>", "[chat] line [SERVER] <text>", "[chat] line [NOTICE] <text>".
    // Recorded as the loader's stats.c does: kind chat, a = the sender, how = "" (everyone) / team / nearby / server / notice,
    // text = the message alone, the sender's peer id for the forensic archive (t_st_chat_id).
    if (!strncmp(l, "[chat] line [", 13)) {
        char* ch = l + 13; char* cb = strchr(ch, ']'); if (!cb) return; *cb = 0; char* rest = cb + 1; while (*rest == ' ') rest++;
        if (!strncmp(ch, "SERVER", 6)) { st_event_pos(0, "chat", 0, 0, "server", -1, -1, 0, "%s", rest); return; }
        if (!strcmp(ch, "NOTICE")) { st_event_pos(0, "chat", 0, 0, "notice", -1, -1, 0, "%s", rest); return; }
        const char* how = !strcmp(ch, "TEAM") ? "team" : !strcmp(ch, "NEARBY") ? "nearby" : "";
        char* paren = strstr(rest, " (0x"); char* x = 0; uint64_t id = paren ? strtoull(paren + 2, &x, 16) : 0;
        if (paren && x && !strncmp(x, "): ", 3)) { *paren = 0; t_st_chat_id = id; st_event_pos(0, "chat", rest, 0, how, -1, -1, 0, "%s", x + 3); t_st_chat_id = 0; }
        else st_event_pos(0, "chat", 0, 0, how, -1, -1, 0, "%s", rest);
        return;
    }
    if (!strncmp(l, "[playlist] ballot ", 18) || !strncmp(l, "[playlist] voting ", 18)) { st_event_pos(0, "vote", 0, 0, 0, -1, -1, 0, "%s", l + 11); return; }
}

// ---- the app's chat lines in its log (2026-11-07) -------------------------------------------------------------------
// The client's chat code (0x100d23d78) logs every new chat line ("[chat] line ...", parsed above) only when the byte at
// 0x103efcc9b is set: the dedicated server's `chat_log` option, which a client leaves off (so the native page and the
// forensic archive had no chat; machoref.py dataref finds that byte read in two places, both this gate, and written
// nowhere by an address of its own: it is a field of the chat state at 0x103efcc58). nhook sets it once the gate's code is
// the expected 0.10.0 instructions (adrp x8, 0x103efc000 ; ldrb w8, [x8, #0xc9b] at 0x100d2a6fc and 0x100d2a720) and
// keeps it set from the snapshot tick (the state is rebuilt with each session). NHOOK_NO_CHAT_LOG=1 = off.
static volatile uint8_t* g_chatlog;
static void chat_log_tick(void) {
    static int tried; if (!tried && g_img) { tried = 1; if (getenv("NHOOK_NO_CHAT_LOG")) return;
        static const uint32_t w[2] = {0xd0018e88u, 0x39726d08u}; uint64_t sl = g_img - APP_IMAGE; uint32_t a[2], b[2];
        if (rd(0x100d2a6fcull + sl, a, 8) || rd(0x100d2a720ull + sl, b, 8) || memcmp(a, w, 8) || memcmp(b, w, 8)) { nlog("[nhook] chat lines: the chat_log gate is not the 0.10.0 code; left off"); return; }
        g_chatlog = (volatile uint8_t*)(uintptr_t)(0x103efcc9bull + sl); nlog("[nhook] chat lines: the app logs them (its chat_log byte set)"); }
    if (g_chatlog && !*g_chatlog) *g_chatlog = 1;
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
static void hm_path(const char* map, char* out, size_t n) {   // NHOOK_HEATMAP_DIR: another folder (shim/native.sh test runs: native/test-heatmaps)
    const char* dd = getenv("NHOOK_HEATMAP_DIR"); if (dd && *dd) { snprintf(out, n, "%s/%s.bin", dd, map); return; }
    const char* h = getenv("HOME"); snprintf(out, n, "%s/Library/Application Support/ReclaimerMac/heatmaps/%s.bin", h ? h : "/tmp", map); }
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
    pthread_t th; nh_thread(&th, st_reap, (void*)(intptr_t)pid, QOS_CLASS_BACKGROUND);
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
typedef struct { char name[64]; int slot, team, deaths, alive, shots, hits, hits_sphere; uint32_t unit; double life_start, longest, dist; float pos[3], top_speed;
                 int aim_n, aim_on, aim_head, aim_on_judged, aim_on_reg, aim_off_judged, aim_off_reg, aim_ei; float aim_err[64];
                 int aim_open_n, aim_open_on, aim_far_n, aim_far_on; double aim_last; } StTrack;   // aim: st_aim_tick
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
// Infection's sides from the traits: the first trait field the variant's infected set changes to a value a human doesn't
// have (a human's active value = the base set's, or Halo's default where the base set leaves it unchanged; e.g. infected
// "shield recharge 100%" IS the default, so it tells nothing -- shields None vs Normal does) tells a zombie -- a player whose active traits (player +0x1FC) hold that value; the initial
// infected set (the first zombies) likewise. -1 = not an Infection variant or no telling field.
static int nov_trait_get(const uint8_t* t, const StTraitF* f) { return f->sz == 2 ? *(const uint16_t*)(t + f->off) : f->sz == -1 ? *(const int8_t*)(t + f->off) : t[f->off]; }
static int st_is_zombie(uint64_t rec) {
    if (!g_st_var_ok || *(const int32_t*)g_st_var != 10) return -1;
    uint8_t act[0x1C]; if (!rec || !hb_read(rec + 0x1FC, act, sizeof act)) return -1;
    const uint8_t* base = g_st_var + 0x134; int told = 0;
    for (int set = 0; set < 2; set++) { const uint8_t* z = g_st_var + (set ? 0x1DC : 0x1C0);
        for (size_t i = 0; i < sizeof g_trait_f / sizeof *g_trait_f; i++) { const StTraitF* f = &g_trait_f[i]; if (f->sz == 0) continue;
            int zv = nov_trait_get(z, f), bv = nov_trait_get(base, f), unch = f->sz == -1 ? -2 : 0;
            int hv = bv != unch ? bv : f->def;                         // what a human's ACTIVE traits hold: the base set's value, else Halo's default
            if (zv == unch || zv == hv || (f->sz == -1 && zv < 0)) continue;   // unchanged, the same as a human's, or random / map default
            told = 1; if (nov_trait_get(act, f) == zv) return 1; break; } }
    return told ? 0 : -1;
}

// joins and leaves: a name entering / leaving the players table (the lobby's session list is not read here). Debounced:
// "left" once a name has been missing for 3 s, "joined" for a name not seen in the last 30 s -- a snapshot that misses the
// table (a read on the wrong engine thread before tls_block pinned the right one) once made every player leave and rejoin,
// ~160 feed lines a second in a 72-player game (2026-11-07).
static void st_roster(const StPlayer* pl, int np, const char* map) {
    static struct { char name[64]; double seen; int in; } r[512]; static int nr; static char pmap[64]; static double t_map;
    double now = mono_s();
    if (strcmp(pmap, map)) { snprintf(pmap, sizeof pmap, "%s", map); nr = 0; t_map = now; }   // a new map: the table fills again
    int quiet = now - t_map < 5.0;                             // the first seconds of a map: everyone "joins" -- not news
    for (int k = 0; k < np; k++) { int i = 0; for (; i < nr; i++) if (!strcmp(r[i].name, pl[k].name)) break;
        if (i == nr) { if (nr >= 512) continue; nr++; snprintf(r[i].name, sizeof r[i].name, "%s", pl[k].name); r[i].in = 0; r[i].seen = 0; }
        if (!r[i].in) { if (!quiet && now - r[i].seen > 30.0) st_event_pos(0, "join", pl[k].name, 0, 0, -1, -1, 0, "%s joined the game", pl[k].name); r[i].in = 1; }
        r[i].seen = now; }
    if (!np) return;                                           // (an empty read says nothing about who left)
    for (int i = 0; i < nr; i++) if (r[i].in && now - r[i].seen > 3.0) { r[i].in = 0; st_event_pos(0, "leave", r[i].name, 0, 0, -1, -1, 0, "%s left the game", r[i].name); }
}

// ---- the scoreboard: halo3's statborg (static RE of halo3.dll, shim/docs/native-client.md "Kills") -------------------
// c_game_statborg (halo3+0x10057c returns game engine globals (TLS +0x20) + 0xfeb8; the app moves it, see g_sb_off):
// player p's 32 i16 stats at +p*0x40, adjusted by halo3+0x1da7e0 (statborg, player, stat, delta, results stat), which
// also feeds the game results' named statistics -- that names them: 0 score, 2 kills (results 9 "kills"), 3 deaths
// (11), 4 suicides (13), 5 betrayals (12), 6 assists (10), 7 rounds won (6). The game engine replicates it to every
// client (each change marks the player's network bit), so a joined client reads the server's scoreboard.
enum { SB_SCORE = 0, SB_KILLS = 2, SB_DEATHS = 3, SB_SUICIDES = 4, SB_BETRAYALS = 5, SB_ASSISTS = 6 };
static __thread int t_sb_memo; static __thread uint8_t t_sb_blk[128 * 0x40]; static __thread int t_sb_ok;   // (the snapshot: one read per run)
static int st_statborg(int slot, int16_t out[8]) {
    if (slot < 0 || slot >= 128) return 0;
    if (t_sb_memo) { if (t_sb_memo == 1) { uint64_t blk = tls_block(), geg = 0; t_sb_ok = blk && hb_read(blk + 0x20, &geg, 8) && geg && hb_read(geg + (uint64_t)g_sb_off, t_sb_blk, sizeof t_sb_blk); t_sb_memo = 2; }
        if (!t_sb_ok) return 0; memcpy(out, t_sb_blk + slot * 0x40, 16); }
    else { uint64_t blk = tls_block(), geg = 0;
        if (!blk || !hb_read(blk + 0x20, &geg, 8) || !geg) return 0;
        if (!hb_read(geg + (uint64_t)g_sb_off + (uint64_t)slot * 0x40, out, 16)) return 0; }
    for (int i = 0; i < 7; i++) if (out[i] < -30000 || out[i] > 30000) return 0;
    return 1;
}
// The kill feed from the scoreboard: a killer's kills (or betrayals) +1 and a victim's deaths +1 in the same ~second
// are one kill; a victim whose suicides went up too killed themselves. The pairing is inferred (several kills in one
// 100 ms snapshot pair in slot order) and the weapon is the killer's weapon in hand (or vehicle) at that moment:
// these events are marked estimated. The counts themselves are the game's own.
typedef struct { int slot, team, kind, haspos; double t; float pos[3]; char name[64], how[96]; } SbKiller;   // kind 0 kill, 1 betrayal
typedef struct { int slot, team, suicide, haspos, wait_body; double t; float pos[3]; char name[64]; } SbVictim;   // wait_body: no body death seen yet
static SbKiller g_sbk[64]; static int g_nsbk; static SbVictim g_sbv[64]; static int g_nsbv;
static struct { int16_t s[8]; int ok; char name[64]; } g_sbprev[128];
typedef struct { char name[64], how[96]; int team, have, dead; float pos[3], cur[3]; double died; } SbNow;   // this snapshot's view of a slot (cur: where it is now)
static SbNow g_sbnow[128];
static double g_sb_kill_t[128];                       // when the scoreboard last showed this slot's kill (sb_tick; rs_medals)
static void st_nice_case(char* s) {   // "ASSAULT RIFLE" -> "Assault rifle" (a name in capitals only; mixed case stays)
    for (const char* q = s; *q; q++) if (*q >= 'a' && *q <= 'z') return;
    for (char* q = s + 1; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a'); }
static void sb_reset(void) { memset(g_sbprev, 0, sizeof g_sbprev); g_nsbk = g_nsbv = 0; }
static volatile int g_rs_live;                             // the game results count deaths in this match (rs_tick)
static struct { int slot; double t; } g_sbe[64]; static int g_nsbe;   // deaths sb_tick reported (an exact report then comes too late)
static void sb_emitted(int slot, double now) { if (g_nsbe < 64) { g_sbe[g_nsbe].slot = slot; g_sbe[g_nsbe].t = now; g_nsbe++; } else { memmove(g_sbe, g_sbe + 1, 63 * sizeof g_sbe[0]); g_sbe[63].slot = slot; g_sbe[63].t = now; } }
// History is not a kill: a client gets the statborg in pieces after joining a game in progress -- every slot's counts jumped
// from 0 to the match's so far in one snapshot 0.8 s after a join into a 50-player game (2026-11-07), and ~60 old kills
// came out. A slot up by more than one death (or 6 kills) in one snapshot, more than SB_BURST slots dying in one snapshot,
// is taken as it is, without events; a death whose player still has a live body that didn't die in the last 4 s (the death
// tracker, g_sbnow .died / .have) waits up to 3 s for that body to die (the statborg can come a moment before the player's
// state: 7 of ~250 deaths in one game) and is history if it never does (NHOOK_RESULTS_LOG=1 logs both).
#define SB_BURST 8
static void sb_tick(const StPlayer* pl, int np, double now) {
    static int16_t ss[128][8]; static uint8_t sok[128]; int nd = 0, lg = getenv("NHOOK_RESULTS_LOG") != 0;
    for (int k = 0; k < np; k++) { int sl = pl[k].slot; if (sl < 0 || sl >= 128) continue; sok[sl] = (uint8_t)st_statborg(sl, ss[sl]);
        if (sok[sl] && g_sbprev[sl].ok && !strcmp(g_sbprev[sl].name, pl[k].name) && ss[sl][SB_DEATHS] > g_sbprev[sl].s[SB_DEATHS]) nd++; }
    if (nd > SB_BURST && lg) nlog("[nhook] [results] scoreboard: %d players' deaths went up in one snapshot: filling in, not reported", nd);
    for (int k = 0; k < np; k++) {
        int sl = pl[k].slot; if (sl < 0 || sl >= 128 || !sok[sl]) continue; int16_t s[8]; memcpy(s, ss[sl], sizeof s);
        if (!g_sbprev[sl].ok || strcmp(g_sbprev[sl].name, pl[k].name) || s[SB_DEATHS] < g_sbprev[sl].s[SB_DEATHS] || s[SB_KILLS] < g_sbprev[sl].s[SB_KILLS]) {
            memcpy(g_sbprev[sl].s, s, sizeof s); g_sbprev[sl].ok = 1; snprintf(g_sbprev[sl].name, sizeof g_sbprev[sl].name, "%s", pl[k].name); continue; }
        const int16_t* o = g_sbprev[sl].s; const SbNow* w = &g_sbnow[sl];
        { int dd = s[SB_DEATHS] - o[SB_DEATHS], dk = s[SB_KILLS] - o[SB_KILLS] + s[SB_BETRAYALS] - o[SB_BETRAYALS];
          if (nd > SB_BURST || dd > 1 || dk > 6) {
              if (lg && (dd > 0 || dk > 0) && nd <= SB_BURST) nlog("[nhook] [results] scoreboard: %s deaths +%d kills +%d at once: history, not reported", pl[k].name, dd, dk);
              memcpy(g_sbprev[sl].s, s, sizeof s); continue; } }
        int body = (w->died > 0 && now - w->died < 4) || !w->have;   // (a death: its body died, or there is no live body)
        if (s[SB_KILLS] > o[SB_KILLS] && sl >= 0 && sl < 128) g_sb_kill_t[sl] = now;   // (rs_medals: a kill medal needs this)
        for (int d = 0; d < s[SB_KILLS] - o[SB_KILLS] && g_nsbk < 64; d++) { SbKiller* x = &g_sbk[g_nsbk++]; x->slot = sl; x->team = w->team; x->kind = 0; x->t = now; snprintf(x->name, sizeof x->name, "%s", pl[k].name); snprintf(x->how, sizeof x->how, "%s", w->how); x->haspos = w->have; memcpy(x->pos, w->cur, 12); }
        for (int d = 0; d < s[SB_BETRAYALS] - o[SB_BETRAYALS] && g_nsbk < 64; d++) { SbKiller* x = &g_sbk[g_nsbk++]; x->slot = sl; x->team = w->team; x->kind = 1; x->t = now; snprintf(x->name, sizeof x->name, "%s", pl[k].name); snprintf(x->how, sizeof x->how, "%s", w->how); x->haspos = w->have; memcpy(x->pos, w->cur, 12); }
        int sui = s[SB_SUICIDES] - o[SB_SUICIDES];
        for (int d = 0; d < s[SB_DEATHS] - o[SB_DEATHS] && g_nsbv < 64; d++) { SbVictim* v = &g_sbv[g_nsbv++]; v->slot = sl; v->team = w->team; v->suicide = d < sui; v->t = now; v->wait_body = !body;
            snprintf(v->name, sizeof v->name, "%s", pl[k].name); v->haspos = w->died > 0 && now - w->died < 3; memcpy(v->pos, w->pos, 12); }
        memcpy(g_sbprev[sl].s, s, sizeof s);
    }
    // pair: victims oldest first; a victim waits up to 4 s for its killer's count (they replicate separately: in a 70-player
    // game they came more than 1.5 s apart -- 57 of 68 deaths in one Infection game had no killer), and takes the nearest
    // waiting killer (a betrayal by a teammate first)
    for (int i = 0; i < g_nsbv; i++) {
        SbVictim* v = &g_sbv[i]; int done = 0;
        if (v->wait_body) { const SbNow* w = &g_sbnow[v->slot];
            if ((w->died > 0 && w->died > v->t - 4) || !w->have) { v->wait_body = 0; v->haspos = w->died > 0 && now - w->died < 3; memcpy(v->pos, w->pos, 12);
                if (lg) nlog("[nhook] [results] scoreboard: %s's body died %.1f s after the count", v->name, now - v->t); }
            else if (now - v->t > 3.0) { if (lg) nlog("[nhook] [results] scoreboard: %s death +1 but alive, no death seen in 3 s: history, not reported", v->name);
                memmove(&g_sbv[i], &g_sbv[i + 1], (size_t)(g_nsbv - i - 1) * sizeof *v); g_nsbv--; i--; continue; }
            else continue; }
        const float* pos = v->haspos ? v->pos : 0;
        if (g_rs_live && now - v->t < 1.5) continue;   // the game results usually name this death exactly (rs_tick): wait for them
        if (v->suicide) { st_event_pos(pos, "death", 0, v->name, 0, -1, v->team, 0, "%s killed themselves", v->name); done = 1; }
        else {
            int best = -1; float bd = 1e30f;
            for (int j = 0; j < g_nsbk; j++) { if (g_sbk[j].slot == v->slot) continue;
                if (g_sbk[j].kind == 1 && g_sbk[j].team == v->team && v->team >= 0) { best = j; break; }
                float d = 1e20f; if (pos && g_sbk[j].haspos) { float dx = g_sbk[j].pos[0] - pos[0], dy = g_sbk[j].pos[1] - pos[1], dz = g_sbk[j].pos[2] - pos[2]; d = dx * dx + dy * dy + dz * dz; }
                if (best < 0 || d < bd) { best = j; bd = d; } }
            if (best >= 0) { SbKiller x = g_sbk[best]; memmove(&g_sbk[best], &g_sbk[best + 1], (size_t)(g_nsbk - best - 1) * sizeof x); g_nsbk--;
                char hw[110] = ""; if (x.how[0]) snprintf(hw, sizeof hw, " (%s)", x.how);
                const char* verb = x.kind ? "betrayed" : "killed";
                st_event_verb(pos, "kill", x.name, v->name, x.how, verb, x.team, v->team, 1 | (x.kind ? 4 : 0), "%s %s %s%s", x.name, verb, v->name, hw); done = 1; }
            else if (now - v->t > 4.0) { st_event_pos(pos, "death", 0, v->name, 0, -1, v->team, 0, "%s was killed", v->name); done = 1; }
        }
        if (done) { sb_emitted(v->slot, now); memmove(&g_sbv[i], &g_sbv[i + 1], (size_t)(g_nsbv - i - 1) * sizeof *v); g_nsbv--; i--; }
    }
    for (int j = 0; j < g_nsbk; j++) if (now - g_sbk[j].t > 5.0) { memmove(&g_sbk[j], &g_sbk[j + 1], (size_t)(g_nsbk - j - 1) * sizeof g_sbk[0]); g_nsbk--; j--; }
}

// ---- exact kills from halo3's game results (2026-11-07) ---------------------------------------------------------------
// The results (see g_res_base) count per player: 9 kills, 11 deaths, 12 betrayals, 13 suicides, and per damage type t
// (0..62, g_dmg_name) from 234 + 6t: damage-kills, -deaths, -betrayals, -suicides, -headshots; and a killer x victim matrix
// (4 bytes: two u16 counts). Checked live on a suicide: 11, 13, 235, 237 and versus[0][0] 0 -> 0x00010001. Each snapshot:
// a versus entry that went up names the pair; which side killed is the one whose kills (or betrayals) went up -- so the
// matrix's own orientation never has to be assumed; the killer's damage-kills (-betrayals) that went up names the weapon,
// its damage-headshots a headshot. Suicides: 13 + damage-suicides. These events are exact (not "estimated"); the pending
// scoreboard entries they explain are dropped (sb_tick waits 1.5 s while results are live). A client that never gets the
// results (stats stay 0) keeps the scoreboard pairing. NHOOK_NO_RESULTS_FEED=1 = off.
static const char* const g_dmg_name[63] = {"Guardians","Falling","Collision","Melee","Explosion","Magnum","Plasma pistol","Needler","Mauler","SMG",
    "Plasma rifle","Battle rifle","Carbine","Shotgun","Sniper rifle","Beam rifle","Assault rifle","Spiker","Fuel rod gun","Missile pod",
    "Rocket launcher","Spartan laser","Brute shot","Flamethrower","Sentinel beam","Energy sword","Gravity hammer","Frag grenade","Plasma grenade","Spike grenade",
    "Firebomb","Flag","Bomb","Bomb explosion","Oddball","Machine gun turret","Plasma cannon","Plasma mortar","Plasma turret","Banshee",
    "Ghost","Mongoose","Scorpion","Scorpion gunner","Warthog","Warthog gunner","Gauss warthog","Wraith","Wraith gunner","Scorpion cannon",
    "Chopper","Hornet","Mantis","Prowler","Sentinel beam turret","Sentinel RPG","Teleporter","Trip mine","Elephant turret","Shade turret",
    "Silenced SMG","Automag","Brute plasma rifle"};   // (stats.c's table: halo3's damage reporting types)
#define RS_NSTAT 0x263
enum { RS_KILLS = 9, RS_DEATHS = 11, RS_BETRAYALS = 12, RS_SUICIDES = 13, RS_DMG = 234 };
static uint16_t g_rs_prev[128][RS_NSTAT], g_rs_cur[128][RS_NSTAT]; static uint32_t g_rs_vprev[128 * 128], g_rs_vcur[128 * 128]; static int g_rs_have;
static void sb_consume(int slot, int field) {   // an exact event explains one scoreboard step: drop its pending entry, or absorb it
    if (slot < 0 || slot >= 128) return;
    if (field == SB_DEATHS || field == SB_SUICIDES) { for (int i = 0; i < g_nsbv; i++) if (g_sbv[i].slot == slot && g_sbv[i].suicide == (field == SB_SUICIDES)) {
            memmove(&g_sbv[i], &g_sbv[i + 1], (size_t)(g_nsbv - i - 1) * sizeof g_sbv[0]); g_nsbv--; return; } }
    else { int kind = field == SB_BETRAYALS; for (int i = 0; i < g_nsbk; i++) if (g_sbk[i].slot == slot && g_sbk[i].kind == kind) {
            memmove(&g_sbk[i], &g_sbk[i + 1], (size_t)(g_nsbk - i - 1) * sizeof g_sbk[0]); g_nsbk--; return; } }
    if (g_sbprev[slot].ok) { g_sbprev[slot].s[field]++; if (field == SB_SUICIDES) g_sbprev[slot].s[SB_DEATHS]++; }
}
static int rs_take(int sl, int base) {   // a damage type whose stat (base + 6t) went up for this slot: -1 none (one step used)
    for (int t = 0; t < 63; t++) { int i = RS_DMG + base + 6 * t; if (i >= RS_NSTAT) break;
        if ((g_rs_cur[sl][i] & 0x7fff) > (g_rs_prev[sl][i] & 0x7fff)) { g_rs_prev[sl][i]++; return t; } }
    return -1;
}
#define RS_FLOOD 16     // more changes than this in one snapshot = the table filling in
#define RS_WAIT 3.0     // seconds a change waits for the scoreboard's death before it counts as history
typedef struct { int killer, victim, betray; double t; char kn[64], vn[64], how[96]; } RsCand;   // killer -1 = a suicide
static RsCand g_rsq[128]; static int g_nrsq;
static void rs_cand(int killer, int victim, int betray, const char* how, const int* idx, const StPlayer* pl, double now) {
    if (g_nrsq >= 128) { memmove(g_rsq, g_rsq + 1, 127 * sizeof g_rsq[0]); g_nrsq--; }
    RsCand* c = &g_rsq[g_nrsq++]; c->killer = killer; c->victim = victim; c->betray = betray; c->t = now;
    snprintf(c->kn, sizeof c->kn, "%s", killer >= 0 && idx[killer] >= 0 ? pl[idx[killer]].name : "?");
    snprintf(c->vn, sizeof c->vn, "%s", idx[victim] >= 0 ? pl[idx[victim]].name : "?");
    snprintf(c->how, sizeof c->how, "%s", how);
}
static void rs_confirm(double now) {   // report the candidates the scoreboard backs (a pending victim of theirs), drop the rest
    int lg = getenv("NHOOK_RESULTS_LOG") != 0;
    for (int i = 0; i < g_nrsq; i++) {
        RsCand* c = &g_rsq[i]; int sui = c->killer < 0, v = -1, why = 0;   // 1 reported, 2 the scoreboard reported it already, 3 no death behind it
        for (int j = 0; j < g_nsbv && v < 0; j++) if (g_sbv[j].slot == c->victim && g_sbv[j].suicide == sui && !g_sbv[j].wait_body) v = j;
        if (v >= 0) why = 1;
        else { for (int e = 0; e < g_nsbe; e++) if (g_sbe[e].slot == c->victim && now - g_sbe[e].t < 6) why = 2;
               if (!why && now - c->t > RS_WAIT) why = 3; }
        if (!why) continue;
        if (why == 1) {
            SbVictim sv = g_sbv[v]; memmove(&g_sbv[v], &g_sbv[v + 1], (size_t)(g_nsbv - v - 1) * sizeof sv); g_nsbv--;
            const float* pos = sv.haspos ? sv.pos : 0; int tv = g_sbnow[c->victim].team;
            if (sui) st_event_verb(pos, "death", 0, c->vn, c->how, "killed themselves", -1, tv, 0, "%s killed themselves%s%s%s", c->vn, c->how[0] ? " (" : "", c->how, c->how[0] ? ")" : "");
            else { sb_consume(c->killer, c->betray ? SB_BETRAYALS : SB_KILLS); const char* verb = c->betray ? "betrayed" : "killed";
                st_event_verb(pos, "kill", c->kn, c->vn, c->how, verb, g_sbnow[c->killer].team, tv, c->betray ? 4 : 0, "%s %s %s%s%s%s", c->kn, verb, c->vn, c->how[0] ? " (" : "", c->how, c->how[0] ? ")" : ""); } }
        if (lg) nlog("[nhook] [results] %s%s%s%s%s%s%s after %.1f s: %s", sui ? "" : c->kn, sui ? "" : c->betray ? " betrayed " : " killed ", c->vn, sui ? " killed themselves" : "",
                     c->how[0] ? " (" : "", c->how, c->how[0] ? ")" : "", now - c->t, why == 1 ? "reported" : why == 2 ? "the scoreboard reported it already" : "no death on the scoreboard: history, not reported");
        memmove(&g_rsq[i], &g_rsq[i + 1], (size_t)(g_nrsq - i - 1) * sizeof g_rsq[0]); g_nrsq--; i--;
    }
}
// ---- medals (2026-11-07): the game results count each player's medals at statistic 63 + i for medal i, in the order of the
// app's own medal table (0.10.0 0x103ed23c8: 112 entries of id / name / description, 48 bytes each -- Extermination ...
// Guardians). Checked against live counts: a melee kill -> 106 Beat Down (43), a first-kill sniper headshot -> 104 Sniper
// Kill (41), 140 First Strike (77), 147 Snapshot (84); a player's 149 Assist (86) equal to their 17 assists. The feed gets
// each medal except Kill / Assist (one per kill / assist: the kill feed has them) and Unused; like the kills, a medal is
// history unless backed: a kill medal waits up to 3 s for that player's kill on the scoreboard (g_sb_kill_t), and a player
// whose medal counts rose by more than 3 in one snapshot is the table filling in.
#define RS_MEDAL 63
static const char* const g_medal[112] = {"Extermination", "Perfection", "Double Kill", "Triple Kill", "Overkill", "Killtacular", "Killtrocity", "Killimanjaro", "Killtastrophe", "Killpocalypse", "Killionaire", "Killing Spree", "Killing Frenzy", "Running Riot", "Rampage", "Untouchable", "Invincible", "Sniper Spree", "Sharpshooter", "Shotgun Spree", "Open Season", "Splatter Spree", "Vehicular Manslaughter", "Sword Spree", "Slice 'n Dice", "Juggernaut Spree", "Unstoppable Force", "Infection Spree", "Mmm... Brains", "Zombie Killing Spree", "Hell's Janitor", "Zombie Exterminator", "Hail to the King", "Bulltrue", "Splatter", "Hijack", "Skyjack", "Kill from the Grave", "Killjoy", "Laser Kill", "Grenade Stick", "Sniper Kill", "Assassin", "Beat Down", "Incineration", "Wheelman", "Bomb Planted", "Killed Bomb Carrier", "Killed VIP", "Killed Juggernaut", "Oddball Kill", "Flag Score", "Killed Flag Carrier", "Flag Kill", "Last Man Standing", "Unused", "Unused", "Unused", "Triple Double", "Steaktacular", "Unfrigginbelievable", "Unfrigginbelievable II", "Brute Shot Kill", "Fuel Rod Kill", "Grenade Kill", "Hammer Kill", "Plasma Grenade Kill", "Rocket Kill", "Needler Kill", "Sentinel Beam Kill", "Sword Kill", "Turret Kill", "Comeback Kill", "Headshot", "Kill", "Vehicle Kill", "Environmental Kill", "First Strike", "Last Strike", "Protector", "Avenger", "Close Call", "Reload This", "Revenge", "Snapshot", "Vehicle Destroyed", "Assist", "Vehicle Destroy Assist", "Ball Holder", "Ball Keeper", "Ball Master", "Ball Hog", "Magic Hands", "Smooth Moves", "Ball Carrier Kill", "First Touch", "First Point", "Hill Defense", "Hill Offense", "Hill Dominance", "Flag Taken", "Flag Returned", "Flag Runner", "Flag Champion", "Bomb Defense", "Bomb Defused", "Disarmed", "Saboteur", "Territory Captured", "Territorial", "Triumvirate", "Guardians"};
static const uint8_t g_medal_obj[112] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};   // 1 = no kill behind it (objectives, game end): reported without the scoreboard's kill
typedef struct { int slot, m; double t; char name[64]; } RsMedal;
static RsMedal g_rsm[64]; static int g_nrsm;
static void rs_medals(const int* idx, const StPlayer* pl, double now, int flood) {
    int lg = getenv("NHOOK_RESULTS_LOG") != 0;
    for (int sl = 0; sl < 128 && !flood; sl++) { int tot = 0;
        for (int m = 0; m < 112; m++) { int i = RS_MEDAL + m, d = (g_rs_cur[sl][i] & 0x7fff) - (g_rs_prev[sl][i] & 0x7fff); if (d > 0) tot += d; }
        if (!tot) continue;
        if (tot > 3) { if (lg) nlog("[nhook] [results] slot %d: %d medals at once: history, not reported", sl, tot); continue; }
        for (int m = 0; m < 112; m++) { int i = RS_MEDAL + m, d = (g_rs_cur[sl][i] & 0x7fff) - (g_rs_prev[sl][i] & 0x7fff);
            if (d <= 0 || m == 74 || m == 86 || (m >= 55 && m <= 57)) continue;
            for (int q = 0; q < d && g_nrsm < 64; q++) { RsMedal* r = &g_rsm[g_nrsm++]; r->slot = sl; r->m = m; r->t = now; snprintf(r->name, sizeof r->name, "%s", idx[sl] >= 0 ? pl[idx[sl]].name : "?"); } } }
    for (int i = 0; i < g_nrsm; i++) { RsMedal* r = &g_rsm[i]; int ok = g_medal_obj[r->m] || (g_sb_kill_t[r->slot] > 0 && fabs(g_sb_kill_t[r->slot] - r->t) < 5);
        if (!ok && now - r->t < 3) continue;
        if (ok) st_event_pos(0, "medal", r->name, 0, g_medal[r->m], -1, g_sbnow[r->slot].team, 0, "%s: %s", r->name, g_medal[r->m]);
        else if (lg) nlog("[nhook] [results] %s: %s with no kill on the scoreboard: history, not reported", r->name, g_medal[r->m]);
        g_rsm[i] = g_rsm[--g_nrsm]; i--; } }
static void rs_tick(const StPlayer* pl, int np, double now) {
    static int off = -1; if (off < 0) off = getenv("NHOOK_NO_RESULTS_FEED") != 0;
    if (off || !g_res_base || g_res_vslog <= 0 || g_res_vslog > 7) return;
    int rowl = 1 << g_res_vslog; size_t vn = (size_t)rowl * (size_t)rowl;
    if (rd(g_res_base + (uint64_t)g_res_stats, g_rs_cur, sizeof g_rs_cur) || rd(g_res_base + (uint64_t)g_res_vs, g_rs_vcur, vn * 4)) return;
    int idx[128]; for (int i = 0; i < 128; i++) idx[i] = -1; for (int k = 0; k < np; k++) if (pl[k].slot >= 0 && pl[k].slot < 128) idx[pl[k].slot] = k;
    int fresh = !g_rs_have, dead = 0;
    static const int counted[4] = {RS_KILLS, RS_DEATHS, RS_BETRAYALS, RS_SUICIDES};   // (per-life stats like seconds-alive go back down)
    for (int sl = 0; sl < 128 && !fresh; sl++) for (int c = 0; c < 4; c++) if ((g_rs_cur[sl][counted[c]] & 0x7fff) < (g_rs_prev[sl][counted[c]] & 0x7fff)) { fresh = 1; break; }   // a new match / a new player in the slot
    for (size_t i = 0; i < vn && !fresh; i++) if ((g_rs_vcur[i] & 0xffff) < (g_rs_vprev[i] & 0xffff) || (g_rs_vcur[i] >> 16) < (g_rs_vprev[i] >> 16)) fresh = 1;
    for (int sl = 0; sl < 128; sl++) dead |= g_rs_cur[sl][RS_DEATHS] & 0x7fff;
    g_rs_live = 1; (void)dead;   // (the results can be read: every death waits up to 1.5 s for them -- a fresh launch's first kills too, which
                                 //  came out estimated while this waited for the first death the results counted; 2026-11-07)
    if (fresh) { memcpy(g_rs_prev, g_rs_cur, sizeof g_rs_prev); memcpy(g_rs_vprev, g_rs_vcur, vn * 4); g_rs_have = 1; rs_confirm(now); return; }
    int kd[128], bd[128], dd[128]; for (int sl = 0; sl < 128; sl++) { kd[sl] = (g_rs_cur[sl][RS_KILLS] & 0x7fff) - (g_rs_prev[sl][RS_KILLS] & 0x7fff); bd[sl] = (g_rs_cur[sl][RS_BETRAYALS] & 0x7fff) - (g_rs_prev[sl][RS_BETRAYALS] & 0x7fff);
        dd[sl] = (g_rs_cur[sl][RS_DEATHS] & 0x7fff) - (g_rs_prev[sl][RS_DEATHS] & 0x7fff) - ((g_rs_cur[sl][RS_SUICIDES] & 0x7fff) - (g_rs_prev[sl][RS_SUICIDES] & 0x7fff)); }   // (deaths by someone else)
    // the game's end: games-completed (1) went up; games-won (3) / games-tied (4) name the outcome
    { char won[400] = ""; int w = 0, done = 0, tied = 0, nw = 0;
      for (int sl = 0; sl < 128; sl++) { if ((g_rs_cur[sl][1] & 0x7fff) > (g_rs_prev[sl][1] & 0x7fff)) done = 1;
          if ((g_rs_cur[sl][4] & 0x7fff) > (g_rs_prev[sl][4] & 0x7fff)) tied = 1;
          if ((g_rs_cur[sl][3] & 0x7fff) > (g_rs_prev[sl][3] & 0x7fff) && idx[sl] >= 0 && w < 300) { w += snprintf(won + w, sizeof won - (size_t)w, "%s%s", nw ? ", " : "", pl[idx[sl]].name); nw++; } }
      if (done) st_event_pos(0, "game", 0, 0, 0, -1, -1, 0, "Game over%s%s%s", nw ? ": " : tied ? ": a tie" : "", won, nw ? (nw > 1 ? " won" : " won") : ""); }
    // What changed this snapshot becomes candidates (rs_cand); one is REPORTED only once the scoreboard (statborg) shows
    // that victim's death too (rs_confirm: a pending scoreboard victim): a client gets the results table in pieces -- after
    // joining a game in progress it fills in over minutes (0 -> N, a slot's statistics long after its versus row: a
    // 52-player Valhalla game, 2026-11-07, replayed ~200 old kills and showed live ones twice) -- so a change the scoreboard
    // doesn't back is history. Big jumps are history outright: a pair's count or a slot's suicides up by more than 1 in one
    // snapshot, or more than RS_FLOOD changes in one snapshot. NHOOK_RESULTS_LOG=1 logs every decision.
    int lg = getenv("NHOOK_RESULTS_LOG") != 0, nch = 0;
    for (int sl = 0; sl < 128; sl++) nch += abs((g_rs_cur[sl][RS_SUICIDES] & 0x7fff) - (g_rs_prev[sl][RS_SUICIDES] & 0x7fff));
    for (size_t i = 0; i < vn; i++) if ((int)(i / (size_t)rowl) != (int)(i % (size_t)rowl)) nch += (int)(g_rs_vcur[i] & 0xffff) - (int)(g_rs_vprev[i] & 0xffff);
    if (nch > RS_FLOOD) { if (lg) nlog("[nhook] [results] %d changes in one snapshot: the table filling in, not reported", nch); nch = -1; }
    // suicides
    for (int sl = 0; sl < 128 && nch >= 0; sl++) { int d = (g_rs_cur[sl][RS_SUICIDES] & 0x7fff) - (g_rs_prev[sl][RS_SUICIDES] & 0x7fff);
        if (d > 1 || dd[sl] + d > 1) { if (lg && d > 0) nlog("[nhook] [results] slot %d: %d suicides at once, not reported", sl, d); continue; }
        for (int q = 0; q < d; q++) { int t = rs_take(sl, 3); char how[96] = ""; if (t > 0 && t < 63) snprintf(how, sizeof how, "%s", g_dmg_name[t]);
            rs_cand(-1, sl, 0, how, idx, pl, now); } }
    // kills: versus entry [a][b]'s low half counts a's kills of b; its high half is the same kill from b's row ([b][a] high =
    // a killed b; checked live: the player in slot 12 killing slot 49 = [12][49] low +1 and [49][12] high +1), so only the low halves
    // are read -- each kill once. Which side killed is still checked against the kills / betrayals and deaths deltas where
    // they tell (a release could store the matrix the other way round); with both possible (a trade in one snapshot), the
    // orientation seen before (g_rs_or[0] row = killer, [1] column = killer; row = killer to start with).
    for (size_t i = 0; i < vn && nch >= 0; i++) {
        uint32_t c = g_rs_vcur[i], p0 = g_rs_vprev[i]; int a = (int)(i / (size_t)rowl), b = (int)(i % (size_t)rowl); if (a == b || c == p0) continue;
        int d = (int)(c & 0xffff) - (int)(p0 & 0xffff); if (d <= 0) continue;
        if (d > 1) { if (lg) nlog("[nhook] [results] versus [%d][%d] up by %d at once: history, not reported", a, b, d); continue; }
        static int g_rs_or[2]; int ka = kd[a] > 0 || bd[a] > 0, kb = kd[b] > 0 || bd[b] > 0, o1 = ka && dd[b] > 0, o2 = kb && dd[a] > 0, rowk;
        if (o1 != o2) rowk = o1; else if (ka != kb) rowk = ka; else rowk = g_rs_or[0] >= g_rs_or[1];
        if (o1 != o2 || ka != kb) g_rs_or[rowk ? 0 : 1]++;
        int killer = rowk ? a : b, victim = rowk ? b : a, betray;
        { int tk = g_sbnow[killer].team, tv = g_sbnow[victim].team; betray = bd[killer] > 0 && (kd[killer] <= 0 || (tk >= 0 && tk == tv)); }
        if (betray) bd[killer]--; else if (kd[killer] > 0) kd[killer]--; if (dd[victim] > 0) dd[victim]--;
        int t = rs_take(killer, betray ? 2 : 0), hs = rs_take(killer, 4) >= 0; rs_take(victim, 1);
        char how[96] = ""; if (t > 0 && t < 63) snprintf(how, sizeof how, "%s%s", g_dmg_name[t], hs ? ", headshot" : ""); else if (hs) snprintf(how, sizeof how, "headshot");
        rs_cand(killer, victim, betray, how, idx, pl, now); }
    rs_medals(idx, pl, now, nch < 0);
    rs_confirm(now);
    memcpy(g_rs_prev, g_rs_cur, sizeof g_rs_prev); memcpy(g_rs_vprev, g_rs_vcur, vn * 4);
}

// the page's controls: on unless NHOOK_STATS_READONLY=1 (POST + an X-Reclaimer-Control header + no foreign Origin)
static char g_st_curmap[64];                               // the map being played (recordings are named after it)
int nr_perf(double* fps, double* p50, double* p95, double* mx);   // nrender.m
static int np_for_perf;                                    // players in the last snapshot (the [perf] log line)
static int st_controls_ok(void) { static int v = -1; if (v < 0) { const char* e = getenv("NHOOK_STATS_READONLY"); v = !(e && *e && strcmp(e, "0")); } return v; }
#include "noverlay.c"   // the overlay's lines (drawn by nrender.m)
#include "../src/rec.h"
extern volatile int g_nr_cap_req, g_nr_w, g_nr_h; extern volatile double g_nr_last; extern volatile unsigned long g_nr_frames;

// ---- halo3's game engine events (display_event, halo3+0x11b0ec: kills, joins, flag / bomb / hill lines) -------------
// The loader read the 0x2c-byte event (type +0, index +4, cause / effect player +0x10 / +0x18 ...); the app enlarges it
// ([capacity] event.outgoing: 0x2c -> 0x40, event.bytes 0x40 -> 0x60) and its new layout is not known yet: each event's
// 0x60 bytes are kept and the first NHOOK_EVENT_LOG (default 200) are logged as "[nhook] [event]" lines, to learn it
// from a real match. The kill feed comes from the scoreboard meanwhile (sb_tick).
static uint8_t g_gev[256][0x60]; static double g_gev_t[256]; static volatile unsigned g_gev_n;
static void on_display_event(uint64_t* ctx) {             // (engine thread; x0 = the guest context, rcx = the event)
    unsigned i = g_gev_n; uint64_t ev = 0; if (!ctx || rd((uint64_t)(uintptr_t)ctx + 8, &ev, 8)) return;
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

#include "nspawns.c"   // weapon / vehicle / power-up spawns and pickups (the loader's st_spawns)
// ---- the hook cost meter: rates per second, the fps estimate, and the real on/off measurement (nhcost.h) ------------------
// A measurement (POST /control?do=hookab&h=<id>[&secs=36][&per=3]; do=hookab&stop=1 ends it) switches ONE hook (g_hc_off) or
// overlay feature (its g_nov_on bit; "overlay" = every feature you had on) on and off in turns of `per` seconds, starting
// on. hc_frame (nrender.m, every presented frame) counts frames per phase after a 0.5 s settle; each on/off pair gives a
// difference, the result is their median (spread = the lowest and highest pair). The state is put back as it was.
static volatile unsigned long g_snap_rd_n, g_snap_rd_b, g_snap_oc_hits, g_snap_oc_mb100;   // the last snapshot: reads (system calls), bytes, reads answered from the object copy, its size (MB x 100)
// What the snapshot reads, copied once per run (rd answers the snapshot thread's reads inside these copies): the engine's
// object memory (every object's data: 1,162 objects in 4-6 MB on an 80-player Hugegrass, 2026-11-07, plus 4 KB for the last
// one), the players data array and the network session's player / peer tables -- read in 16 KB pieces (a piece that fails is
// marked: reads there go to the system again). 4,032 system calls a run became ~350. NHOOK_NO_OCACHE=1 = off.
typedef struct { uint8_t* buf, * ok; size_t cap; } OcBuf;
static OcBuf g_ocb[RD_NCOPY];
static void oc_add(uint64_t lo, uint64_t hp) {   // copy [lo, hp) as the next region
    if (t_noc >= RD_NCOPY || hp <= lo || hp - lo > (64u << 20)) return; OcBuf* b = &g_ocb[t_noc];
    lo &= ~(uint64_t)(RD_CHUNK - 1); size_t nc = (size_t)((hp - lo + RD_CHUNK - 1) / RD_CHUNK), n = nc * RD_CHUNK;
    if (n > b->cap) { free(b->buf); free(b->ok); b->buf = malloc(n); b->ok = malloc(nc); b->cap = b->buf && b->ok ? n : 0; if (!b->cap) return; }
    for (size_t c = 0; c < nc; c++) { mach_vm_size_t got = 0; b->ok[c] = mach_vm_read_overwrite(mach_task_self(), lo + c * RD_CHUNK, RD_CHUNK, (mach_vm_address_t)(b->buf + c * RD_CHUNK), &got) == KERN_SUCCESS && got == RD_CHUNK; }
    t_rd_n += nc; RdCopy* q = &t_oc[t_noc++]; q->buf = b->buf; q->ok = b->ok; q->lo = lo; q->hi = lo + n; g_snap_oc_mb100 += (unsigned long)(n / 10485.76);
}
static uint64_t st_sess_ptr(void);
static void st_ocache_fill(void) {
    static int off = -1; if (off < 0) off = getenv("NHOOK_NO_OCACHE") != 0; t_noc = 0; g_snap_oc_mb100 = 0; if (off) return;
    uint64_t arr = hb_object_array(); uint8_t hdr[0x60]; static uint8_t tb[HB_MAXOBJ * 0x18];
    if (arr && hb_read(arr, hdr, sizeof hdr) && *(uint32_t*)(hdr + 0x2c) == 0x64407440 && *(int32_t*)(hdr + 0x24) == 0x18) {
        int hi = *(int32_t*)(hdr + 0x3c);
        if (hi > 0 && hi <= HB_MAXOBJ && hb_read(*(uint64_t*)(hdr + 0x48), tb, (size_t)hi * 0x18)) {
            uint64_t lo = ~0ull, hp = 0; for (int i = 0; i < hi; i++) { uint64_t q = *(uint64_t*)(tb + i * 0x18 + 0x10); if (!*(uint16_t*)(tb + i * 0x18) || !q) continue; if (q < lo) lo = q; if (q > hp) hp = q; }
            if (hp) oc_add(lo, hp + 4096); } }
    uint64_t blk = tls_block(), parr = 0;   // the players data array: count x element size from its header
    if (blk && hb_read(blk + 0x10, &parr, 8) && parr && hb_read(parr, hdr, sizeof hdr) && *(uint32_t*)(hdr + 0x2c) == 0x64407440) {
        int32_t es = *(int32_t*)(hdr + 0x24), hi = *(int32_t*)(hdr + 0x3c); uint64_t pd = *(uint64_t*)(hdr + 0x48);
        if (es >= 0x98 && es <= 0x10000 && hi > 0 && hi <= 256 && pd) oc_add(pd, pd + (uint64_t)es * (uint64_t)hi); }
    uint64_t sess = st_sess_ptr();          // the session: its player table (+0x1270, 0xc8 each) and peer table (+0x58, 0x110 each)
    if (sess) { uint64_t pb = sess + 0x1270 + (uint64_t)g_ses_players_shift, qb = sess + 0x58 + (uint64_t)g_ses_peers_shift; oc_add(pb, pb + 128 * 0xc8); oc_add(qb, qb + 128 * 0x110); }
}
typedef struct { int have, pairs; double on_fps, off_fps, loss, lo, hi, when, p95_on, p95_off, max_on, max_off; } HcRes;   // (p95 / max: frame times, ms)
static HcRes g_hcres[HC_N];
static struct { int state, k, on, nph; uint32_t bits, saved_bits; uint8_t saved_off; double secs, per, t0, ph_t0, ph_tc, last; unsigned frames; float fps[64], p95[64], fmax[64]; int niv; float iv[2048]; } g_hcab;
static pthread_mutex_t g_hcab_mx = PTHREAD_MUTEX_INITIALIZER;
static int hc_find(const char* id) { for (int k = 0; k < HC_N; k++) if (!strcmp(g_hc[k].id, id)) return k; return -1; }
static uint32_t hc_bits(int k) { switch (k) { case HC_OV_HIT: return 1u << NOV_HIT; case HC_OV_VEH: return 1u << NOV_VEH; case HC_OV_TRC: return 1u << NOV_TRC;
    case HC_OV_INF: return 1u << NOV_INF; case HC_OV_ITM: return 1u << NOV_ITM; case HC_OV_ZON: return 1u << NOV_ZON; default: return 0; } }
static void hc_apply(int on) {   // (g_hcab_mx held)
    int k = g_hcab.k; g_hcab.on = on;
    if (k == HC_OV) { uint32_t all = (1u << NOV_HIT) | (1u << NOV_VEH) | (1u << NOV_TRC) | (1u << NOV_INF) | (1u << NOV_ITM) | (1u << NOV_ZON);
        uint32_t want = g_hcab.saved_bits & all ? g_hcab.saved_bits : (g_hcab.saved_bits | all); g_nov_on = on ? want : (g_hcab.saved_bits & ~all); }
    else if (hc_bits(k)) g_nov_on = on ? (g_hcab.saved_bits | hc_bits(k)) : (g_hcab.saved_bits & ~hc_bits(k));
    else g_hc_off[k] = (uint8_t)!on; }
static void hc_restore(void) { if (hc_bits(g_hcab.k) || g_hcab.k == HC_OV) g_nov_on = g_hcab.saved_bits; else g_hc_off[g_hcab.k] = g_hcab.saved_off; }
static int hc_fcmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static void hc_finish(double now) {   // (g_hcab_mx held)
    HcRes* r = &g_hcres[g_hcab.k]; double on[32], off[32], d[32]; int n = 0;
    for (int i = 0; i + 1 < g_hcab.nph && n < 32; i += 2) if (g_hcab.fps[i] > 0 && g_hcab.fps[i + 1] > 0) { on[n] = g_hcab.fps[i]; off[n] = g_hcab.fps[i + 1]; d[n] = off[n] - on[n]; n++; }
    double p95on[32], p95off[32], mxon[32], mxoff[32]; int m = 0;
    for (int i = 0; i + 1 < g_hcab.nph && m < 32; i += 2) if (g_hcab.fps[i] > 0 && g_hcab.fps[i + 1] > 0) { p95on[m] = g_hcab.p95[i]; p95off[m] = g_hcab.p95[i + 1]; mxon[m] = g_hcab.fmax[i]; mxoff[m] = g_hcab.fmax[i + 1]; m++; }
    hc_restore(); g_hcab.state = 0;
    if (!n) return; memset(r, 0, sizeof *r); r->have = 1; r->pairs = n; r->when = now;
    qsort(p95on, (size_t)m, 8, hc_fcmp); qsort(p95off, (size_t)m, 8, hc_fcmp); qsort(mxon, (size_t)m, 8, hc_fcmp); qsort(mxoff, (size_t)m, 8, hc_fcmp);
    r->p95_on = p95on[m / 2]; r->p95_off = p95off[m / 2]; r->max_on = mxon[m / 2]; r->max_off = mxoff[m / 2];
    qsort(on, (size_t)n, 8, hc_fcmp); qsort(off, (size_t)n, 8, hc_fcmp); qsort(d, (size_t)n, 8, hc_fcmp);
    r->on_fps = on[n / 2]; r->off_fps = off[n / 2]; r->loss = d[n / 2]; r->lo = d[0]; r->hi = d[n - 1];
    nlog("[nhook] hook cost: %s on %.1f fps, off %.1f fps -> costs %.2f fps (pairs %d, %.2f .. %.2f); frame p95 %.1f / %.1f ms, max %.1f / %.1f ms (on / off)", g_hc[g_hcab.k].id, r->on_fps, r->off_fps, r->loss, n, r->lo, r->hi, r->p95_on, r->p95_off, r->max_on, r->max_off); }
void hc_frame(double now) {   // nrender.m nr_frame: every presented frame
    if (!__atomic_load_n(&g_hcab.state, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_hcab_mx);
    if (g_hcab.state == 1) { g_hcab.state = 2; g_hcab.t0 = now; g_hcab.nph = 0; g_hcab.niv = 0; hc_apply(1); g_hcab.ph_t0 = now; g_hcab.frames = 0; g_hcab.ph_tc = 0; }
    else if (g_hcab.state == 2) {
        if (now - g_hcab.ph_t0 >= 0.5) { if (!g_hcab.ph_tc) g_hcab.ph_tc = now; else { g_hcab.frames++; if (g_hcab.niv < 2048 && g_hcab.last > 0) g_hcab.iv[g_hcab.niv++] = (float)((now - g_hcab.last) * 1e3); } }   // (frames after the settle)
        if (now - g_hcab.ph_t0 >= g_hcab.per) {
            double el = g_hcab.ph_tc ? now - g_hcab.ph_tc : 0;
            if (g_hcab.nph < 64) { int ni = g_hcab.niv; float* v = g_hcab.iv; for (int i = 1; i < ni; i++) { float x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
                g_hcab.p95[g_hcab.nph] = ni ? v[(ni * 95) / 100 < ni ? (ni * 95) / 100 : ni - 1] : 0; g_hcab.fmax[g_hcab.nph] = ni ? v[ni - 1] : 0;
                g_hcab.fps[g_hcab.nph++] = el > 0.2 ? (float)(g_hcab.frames / el) : 0; } g_hcab.niv = 0;
            if (now - g_hcab.t0 >= g_hcab.secs && !(g_hcab.nph & 1)) hc_finish(now);
            else { hc_apply(!g_hcab.on); g_hcab.ph_t0 = now; g_hcab.frames = 0; g_hcab.ph_tc = 0; } } }
    g_hcab.last = now; pthread_mutex_unlock(&g_hcab_mx); }
static int hc_set(const char* qs) {   // do=hookset&h=<id>&on=0|1: switch a hook's handler off / on (not the overlay's features: their own buttons)
    const char* h = strstr(qs, "h="); char id[32] = ""; if (h) { h += 2; size_t i = 0; while (*h && *h != '&' && i + 1 < sizeof id) id[i++] = *h++; id[i] = 0; }
    int k = hc_find(id); if (k < 0 || hc_bits(k) || k == HC_OV || k == HC_SNAP) return 0; int on = !strstr(qs, "on=0");
    pthread_mutex_lock(&g_hcab_mx); int busy = g_hcab.state != 0; if (!busy) g_hc_off[k] = (uint8_t)!on; pthread_mutex_unlock(&g_hcab_mx);
    if (!busy) nlog("[nhook] hook %s switched %s from the page", id, on ? "on" : "off"); return !busy; }
static int hc_control(const char* qs) {   // do=hookab
    pthread_mutex_lock(&g_hcab_mx); int ok = 0;
    if (strstr(qs, "stop=1")) { if (g_hcab.state) { if (g_hcab.state == 2) hc_restore(); g_hcab.state = 0; } ok = 1; }
    else if (!g_hcab.state) { const char* h = strstr(qs, "h="); char id[32] = "";   // (h=ov%3Ahitboxes: %XX decoded)
        if (h) { h += 2; size_t i = 0; while (*h && *h != '&' && i + 1 < sizeof id) { if (h[0] == '%' && h[1] && h[2]) { char x[3] = {h[1], h[2], 0}; id[i++] = (char)strtol(x, 0, 16); h += 3; } else id[i++] = *h++; } id[i] = 0; }
        int k = hc_find(id); const char* sv = strstr(qs, "secs="), * pv = strstr(qs, "per=");
        if (k >= 0) { g_hcab.k = k; g_hcab.secs = sv ? atof(sv + 5) : 36; g_hcab.per = pv ? atof(pv + 4) : 3;
            if (g_hcab.secs < 6) g_hcab.secs = 6; if (g_hcab.secs > 600) g_hcab.secs = 600; if (g_hcab.per < 1) g_hcab.per = 1; if (g_hcab.per > 30) g_hcab.per = 30;
            g_hcab.saved_bits = g_nov_on; g_hcab.saved_off = g_hc_off[k]; g_hcab.t0 = mono_s(); __atomic_store_n(&g_hcab.state, 1, __ATOMIC_RELEASE); ok = 1;
            nlog("[nhook] hook cost: measuring %s (%.0f s, %.0f s phases)", g_hc[k].id, g_hcab.secs, g_hcab.per); } }
    pthread_mutex_unlock(&g_hcab_mx); return ok; }
static void hc_json(StBuf* s, double now) {
    static uint64_t pt[HC_N], pc[HC_N]; static double pnow, rc[HC_N], rms[HC_N];
    if (now - pnow >= 1.0) { double dt = pnow ? now - pnow : 0;
        for (int k = 0; k < HC_N; k++) { uint64_t t = g_hc[k].ticks, c = g_hc[k].calls; if (dt > 0) { rc[k] = (double)(c - pc[k]) / dt; rms[k] = (double)(t - pt[k]) / hc_freq() * 1e3 / dt; } pt[k] = t; pc[k] = c; }
        pnow = now; }
    // (no frames: a run that never starts or never ends is dropped after its length + 10 s)
    pthread_mutex_lock(&g_hcab_mx); if (g_hcab.state && now - g_hcab.t0 > g_hcab.secs + 10) { if (g_hcab.state == 2) hc_restore(); g_hcab.state = 0; nlog("[nhook] hook cost: measurement dropped (no frames)"); } pthread_mutex_unlock(&g_hcab_mx);
    double fps = 0, p50 = 0, p95, mx; nr_perf(&fps, &p50, &p95, &mx);
    sb_put(s, ",\"hooks\":{\"fps\":%.1f,\"frame_ms\":%.2f,\"list\":[", fps, p50);
    for (int k = 0; k < HC_N; k++) {
        double ms = rms[k] - (k == HC_DEL ? rms[HC_AIM] : 0); if (ms < 0) ms = 0;   // (the bullet hook's figure without the aim meter inside it)
        int on = hc_bits(k) ? nov_feat(__builtin_ctz(hc_bits(k))) : k == HC_OV ? nov_wanted() : !g_hc_off[k];
        double msf = fps > 0 ? ms / fps : 0, cost = -1;
        if (g_hc[k].crit && p50 > 0.5 && fps > 0) { double rest = p50 - msf; cost = rest > 0.1 ? 1000.0 / rest - 1000.0 / p50 : -1; }
        const HcRes* r = &g_hcres[k];
        sb_put(s, "%s{\"id\":\"%s\",\"name\":", k ? "," : "", g_hc[k].id); sb_str(s, g_hc[k].name);
        if (k == HC_SNAP) sb_put(s, ",\"reads\":%lu,\"read_bytes\":%lu,\"cached_reads\":%lu,\"copy_mb\":%.2f", g_snap_rd_n, g_snap_rd_b, g_snap_oc_hits, g_snap_oc_mb100 / 100.0);
        sb_put(s, ",\"switch\":%s", hc_bits(k) || k == HC_OV || k == HC_SNAP ? "false" : "true");
        sb_put(s, ",\"thread\":\"%s\",\"crit\":%s,\"on\":%s,\"calls_s\":%.1f,\"us_call\":%.2f,\"ms_s\":%.3f,\"ms_frame\":%.3f,\"pct\":%.2f,\"fps_cost\":%.2f,\"total\":%llu",
               g_hc[k].thread, g_hc[k].crit ? "true" : "false", on ? "true" : "false", rc[k], rc[k] > 0 ? ms * 1e3 / rc[k] : 0, ms, msf, p50 > 0 ? 100 * msf / p50 : 0, cost, (unsigned long long)g_hc[k].calls);
        if (r->have) sb_put(s, ",\"ab\":{\"on_fps\":%.2f,\"off_fps\":%.2f,\"loss\":%.2f,\"lo\":%.2f,\"hi\":%.2f,\"pairs\":%d,\"age\":%.0f,\"p95_on\":%.1f,\"p95_off\":%.1f,\"max_on\":%.1f,\"max_off\":%.1f}",
                           r->on_fps, r->off_fps, r->loss, r->lo, r->hi, r->pairs, now - r->when, r->p95_on, r->p95_off, r->max_on, r->max_off);
        sb_put(s, "}"); }
    pthread_mutex_lock(&g_hcab_mx);
    sb_put(s, "],\"measuring\":%s", g_hcab.state ? "{" : "null");
    if (g_hcab.state) sb_put(s, "\"id\":\"%s\",\"on\":%s,\"left\":%.0f,\"phases\":%d}", g_hc[g_hcab.k].id, g_hcab.on ? "true" : "false", g_hcab.secs - (now - g_hcab.t0), g_hcab.nph);
    pthread_mutex_unlock(&g_hcab_mx); sb_put(s, "}"); }

// ---- aim vs hits, per player (noverlay.c measures each shot): the last 64 errors (median shown), how many went through the
// target's hitboxes, and REGISTERED = the target's shield + health went down within 0.6 s (the server's decision, as it
// reaches this client) -- for shots through the hitboxes and, separately, for misses (damage after a visible miss: the
// server saw it differently). A burst at one target overlaps: a later bullet can be credited with an earlier one's damage.
static struct { uint32_t target; float pre; double t; int tr, on; } g_aimp[256]; static int g_naimp;
static void st_aim_tick(const StPlayer* pl, int np, double now, const uint8_t* tab, int hi) {   // (tab / hi: the snapshot's object table)
    static unsigned seen; unsigned n = __atomic_load_n(&g_nov_aimr_w, __ATOMIC_ACQUIRE); if (n - seen > NOV_AIMR_N) seen = n - NOV_AIMR_N;
    for (; seen != n; seen++) { const NovAimRes* q = &g_nov_aimr[seen % NOV_AIMR_N];
        for (int k = 0; k < np; k++) if (pl[k].unit == q->owner) { StTrack* tr = st_track(&pl[k]); if (!tr) break;
            tr->aim_err[tr->aim_ei++ & 63] = q->err; tr->aim_n++; if (q->on) { tr->aim_on++; tr->hits++; if (q->head) tr->aim_head++; }   // (a hit = through an enemy's hitbox)
            // the outlier signals (the page's Aimbot? column): the OPENING shot of an engagement (no aimed shot for 1 s before
            // it) on target -- a person usually needs a correction, a lock-on doesn't -- and shots at 12+ world units (~36 m)
            if (q->t - tr->aim_last > 1.0) { tr->aim_open_n++; tr->aim_open_on += q->on; } tr->aim_last = q->t;
            if (q->along >= 12.0f) { tr->aim_far_n++; tr->aim_far_on += q->on; }
            if (g_naimp < 256) { g_aimp[g_naimp].target = q->target; g_aimp[g_naimp].pre = q->pre; g_aimp[g_naimp].t = q->t; g_aimp[g_naimp].tr = (int)(tr - g_trk); g_aimp[g_naimp].on = q->on; g_naimp++; }
            break; } }
    for (int i = 0; i < g_naimp; i++) {
        uint32_t h = g_aimp[i].target; int ix = (int)(h & 0xFFFF); uint64_t d = ix < hi && *(const uint16_t*)(tab + ix * 0x18) == (uint16_t)(h >> 16) ? *(const uint64_t*)(tab + ix * 0x18 + 0x10) : 0; float hs[2] = {0, 0}; uint32_t fl = 0; int reg = -1;
        if (!d || !hb_read(d + 0xF4, hs, 8) || !hb_read(d + 0x110, &fl, 4)) { g_aimp[i] = g_aimp[--g_naimp]; i--; continue; }   // (gone: not judged)
        if (((fl >> 2) & 1) || hs[0] + hs[1] < g_aimp[i].pre - 0.005f) reg = 1;   // (dead / hurt since the shot)
        else if (now - g_aimp[i].t > 0.6) reg = 0;
        if (reg < 0) continue;
        int ti = g_aimp[i].tr; if (ti >= 0 && ti < g_ntrk) { StTrack* tr = &g_trk[ti]; if (g_aimp[i].on) { tr->aim_on_judged++; tr->aim_on_reg += reg; } else { tr->aim_off_judged++; tr->aim_off_reg += reg; } }
        g_aimp[i] = g_aimp[--g_naimp]; i--; }
}
static int st_aim_fcmp(const void* x, const void* y) { float a = *(const float*)x, b = *(const float*)y; return a < b ? -1 : a > b; }
static void st_aim_json(StBuf* s, const StTrack* t) {
    if (!t || !t->aim_n) return; int m = t->aim_n < 64 ? t->aim_n : 64; float e[64]; memcpy(e, t->aim_err, (size_t)m * 4); qsort(e, (size_t)m, 4, st_aim_fcmp);
    sb_put(s, ",\"aim\":{\"n\":%d,\"err\":%.2f,\"err75\":%.2f,\"on\":%.3f,\"head\":%.3f,\"reg\":%.3f,\"reg_n\":%d,\"miss_reg\":%.3f,\"miss_n\":%d,\"open_n\":%d,\"open_on\":%d,\"far_n\":%d,\"far_on\":%d}",
           t->aim_n, e[m / 2], e[(m * 3) / 4], (double)t->aim_on / t->aim_n, t->aim_on ? (double)t->aim_head / t->aim_on : 0.0,
           t->aim_on_judged ? (double)t->aim_on_reg / t->aim_on_judged : -1.0, t->aim_on_judged, t->aim_off_judged ? (double)t->aim_off_reg / t->aim_off_judged : -1.0, t->aim_off_judged,
           t->aim_open_n, t->aim_open_on, t->aim_far_n, t->aim_far_on);
}

// Who talks: the app's own voice report, every ~2 s (100 frames of 20 ms): "[voice] active true sending standard members 65 sent 0
// (0 bytes) received 60645 relayed 0 mixed 201 frames rms -24.8 dBFS; heard 0x<peer id>: 100 frames (0 concealed) rms -30.5 dBFS
// peak -11.2 dBFS, 0x<id>: ...". A sender counts as talking when it was heard above TALK_DBFS (open microphones send silence:
// -120 dBFS for whole reports, -112 / -90 dBFS hiss). The page's talking dot stays on for the report's ~2 s (it was a 0.5 s
// stamp per packet in the loader); forensic.c gets the frames as FXF_REPORT records (timing only, no audio).
#define TALK_DBFS (-60.0)
static struct { uint64_t id; double t; } g_talk[128]; static pthread_mutex_t g_talk_mx = PTHREAD_MUTEX_INITIALIZER;
static double st_talk_age(uint64_t id) { double now = mono_s(), a = 1e9; pthread_mutex_lock(&g_talk_mx);
    for (int i = 0; i < 128; i++) if (g_talk[i].id == id) { a = now - g_talk[i].t; break; } pthread_mutex_unlock(&g_talk_mx); return a; }
static uint64_t fx_now_ns(void); static void fx_voice_report(uint64_t t_start_ns, uint64_t sender, int frames);   // (forensic.c, below)
static void fx_voice_level(uint64_t t_start_ns, uint32_t window_ms, uint64_t sender, int frames, double db);
static volatile int g_fx_on; static void fx_voice(uint64_t sender, const uint8_t* payload, uint64_t len);
// The voice packets themselves (forensic audio, like the loader's voice_recv hook): a text hook on the app's Opus decode (opus-rs,
// 0.10.0 app 0x1003f1e7c: x0 decoder, x1 packet, x2 length; 0 = a lost frame being concealed). Its one caller (0x102152578, the
// voice mixer, once per sender per 20 ms) walks a hash map of senders: x26 = the entry's end, the peer id at x26 - 0x278 (the
// mixer reads it there for the volume table). The packet arrives as [kind 1 = Opus][Opus]; decode gets the Opus part. The
// archive's records are built in the loader's layout ([channel][sequence u64][0x01][Opus]: channel 0xFF = not known, sequence =
// our per-sender count) so voice.bin, the viewer and its .ogg files work unchanged. Checked before hooking: decode's first four
// instructions, the caller's bl / argument moves and its id load (another app build: no hook, "voice packets: not hooked").
static volatile double g_opus_last;   // the last packet (mono): the voice reports stop writing FXF_REPORT records while packets come
static volatile unsigned long g_opus_n;
static uint64_t g_opus_ret;   // the mixer's return address after its decode call (nhook.c voice_hook_install sets it)
static void nh_on_opus_body(uint64_t dec, const uint8_t* pkt, uint64_t len, uint64_t ent, uint64_t ret);
__attribute__((used)) void nh_on_opus(uint64_t dec, const uint8_t* pkt, uint64_t len, uint64_t ent, uint64_t ret) {   // (timed: nhcost.h)
    if (g_hc_off[HC_OPUS]) return; uint64_t t0 = hc_now(); nh_on_opus_body(dec, pkt, len, ent, ret); hc_add(HC_OPUS, t0); }
static void nh_on_opus_body(uint64_t dec, const uint8_t* pkt, uint64_t len, uint64_t ent, uint64_t ret) {
    (void)dec; if (ret != g_opus_ret || !len || len > 4000 || !pkt) return; uint64_t id = 0; if (rd(ent - 0x278, &id, 8) || !id) return;
    g_opus_n++; g_opus_last = mono_s(); if (!g_fx_on) return;
    static struct { uint64_t id, seq; } sq[256]; static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER; uint64_t seq = 0;
    pthread_mutex_lock(&mx); int k = -1, fr = -1; for (int i = 0; i < 256; i++) { if (sq[i].id == id) { k = i; break; } if (fr < 0 && !sq[i].id) fr = i; }
    if (k < 0) { k = fr >= 0 ? fr : (int)(id & 255); sq[k].id = id; sq[k].seq = 0; } seq = ++sq[k].seq; pthread_mutex_unlock(&mx);
    uint8_t b[10 + 4000]; b[0] = 0xFF; memcpy(b + 1, &seq, 8); b[9] = 1; if (rd((uint64_t)(uintptr_t)pkt, b + 10, (size_t)len)) return;
    fx_voice(id, b, len + 10);
}
static void st_voice_report(const char* l) {
    const char* h = strstr(l, "; heard "); if (!h) return; h += 8; double now = mono_s(); uint64_t tns = fx_now_ns();
    static double prev; double win = prev > 0 ? now - prev : 2.0; prev = now; if (win < 0.5 || win > 5) win = 2.0;   // (the report's window, for FXF_LEVEL)
    while ((h = strstr(h, "0x"))) { char* e; uint64_t id = strtoull(h + 2, &e, 16); if (e == h + 2 || *e != ':') { h += 2; continue; }
        int fr = (int)strtol(e + 1, &e, 10); const char* r = strstr(e, " rms "); const char* nx = strstr(e, ", 0x");
        if (!r || (nx && r > nx)) { h = e; continue; } double db = strtod(r + 5, 0); h = nx ? nx + 2 : e;
        static int vl = -1; if (vl < 0) vl = getenv("NHOOK_VOICE_LOG") != 0;   // NHOOK_VOICE_LOG=1: every sender of every report
        if (vl) nlog("[nhook] voice report: %016llx %d frames rms %.1f dBFS%s", (unsigned long long)id, fr, db, id && fr > 0 && db > TALK_DBFS ? " -> talking" : "");
        if (id && fr > 0) fx_voice_level(tns - (uint64_t)(win * 1e9), (uint32_t)(win * 1000), id, fr, db);   // (the archive splits open microphones on these)
        if (!id || fr <= 0 || db <= TALK_DBFS) continue;
        pthread_mutex_lock(&g_talk_mx); int k = -1, old = 0; for (int i = 0; i < 128; i++) { if (g_talk[i].id == id) { k = i; break; } if (g_talk[i].t < g_talk[old].t) old = i; }
        double gap = k >= 0 ? now - g_talk[k].t : 1e9;   // talked in the report before (~2 s ago): these frames continue that talk
        if (k < 0) { k = old; g_talk[k].id = id; } g_talk[k].t = now; pthread_mutex_unlock(&g_talk_mx);
        uint64_t dur = (uint64_t)fr * 20000000ull, back = gap < 3.0 ? (uint64_t)(gap * 1e9) : dur; if (back < dur) back = dur;
        if (mono_s() - g_opus_last > 5.0) fx_voice_report(tns - back, id, fr); }   // (packets coming: the archive has them already)
}

// ---- the forensic archive and its viewer (/forensics): the loader's shim/src/forensic.c, unchanged, with what it takes
// from stats.c / peload.c defined for the native app. Voice is not recorded yet (no voice hook in the native app): the
// archive keeps events, the roster and per-second packet sums. Docs: shim/docs/forensics.md.
static int g_st_layout = 1;                                // the session's extended layout (forensic.c fx_peer_id; nnet.c's shifts)
static uint8_t* hb_halo3_base(void) { return (uint8_t*)(uintptr_t)g_img; }
static char g_cmdlineA[1100];                              // "join?server=<addr>" for a --join-server launch (forensic.c fx_join_server)
static int st_send(int fd, const char* b, size_t n);
#define NHOOK_NATIVE 1   // (forensic.c: the native build -- its writer thread at a low priority)
#include "../src/forensic.c"
// the last second's packet records summed per (direction, type, peer) -> packets.jsonl (pktlog.c pk_forensic_tick)
static void pk_forensic_tick(void) {
    static uint32_t last; uint32_t top = __atomic_load_n(&g_pk_seq, __ATOMIC_ACQUIRE); if (!last) { last = top; return; }
    if (!g_pk_on || !g_fx_on || !g_fxr.key[0]) { last = top; return; }
    g_pk_want_rdns = 1;                                        // (the writer thread's own: names into the archive)
    uint32_t lo = last + 1, lost = 0; if (top > last + PK_N - 64) { lost = top - last - (PK_N - 64); lo = top - (PK_N - 64) + 1; } last = top;
    static PkAgg a[1024]; int na = 0; PkEv e; for (uint32_t q = lo; q <= top; q++) if (pk_get(q, &e)) na = pk_agg_add(a, na, 1024, &e); else lost++;
    for (int i = 0; i < na; i++) { StBuf b = {0}; pk_agg_json(&b, &a[i], 1, 0); if (b.b) fx_push(FXI_PACKETS, b.b, b.n, 0, 0); free(b.b); }
    if (lost) { char l[80]; int k = snprintf(l, sizeof l, "{\"t\":%.3f,\"lost\":%u}", st_wall(), lost); fx_push(FXI_PACKETS, l, (size_t)k, 0, 0); }
}

// ---- the snapshot (stats.c st_snapshot's shape) ---------------------------------------------------------------------
static void st_snapshot(StBuf* s) {
    static const char* teams[8] = {"red", "blue", "green", "orange", "purple", "gold", "brown", "pink"};
    chat_log_tick();
    double now = mono_s(), t0 = g_gameplay_t0;
    sb_put(s, "{\"time\":%.2f,\"in_match\":%s,\"inst\":%d,\"native\":true", t0 ? now - t0 : 0.0, t0 ? "true" : "false", (int)getpid());
    { pthread_mutex_lock(&g_ev_mx); uint32_t fs = g_ev_seq; pthread_mutex_unlock(&g_ev_mx); sb_put(s, ",\"feed_seq\":%u", fs); }
    { char rb[1200]; int video_ok = g_nr_last > 0 && now - g_nr_last < 2.0; if (rec_status_json(rb, sizeof rb, video_ok) > 0) sb_put(s, ",\"rec\":%s", rb); }
    { double fps, p50, p95, mx; int nobj = 0; uint64_t oa = hb_object_array(); uint8_t oh[0x60]; if (oa && hb_read(oa, oh, sizeof oh)) nobj = *(int32_t*)(oh + 0x40);   // (data array: +0x40 count in use)
      if (nr_perf(&fps, &p50, &p95, &mx)) {
          sb_put(s, ",\"perf\":{\"fps\":%.1f,\"window_fps\":%.1f,\"frame_ms\":[%.1f,%.1f,%.1f],\"draws\":0,\"objects\":%d,\"in_view\":%d,\"bipeds\":0,\"vehicles\":0,\"projectiles\":0,"
                    "\"present_to_gpu_ms\":-1,\"present_to_window_ms\":-1,\"input_to_screen_ms\":-1,\"input_to_window_ms\":-1,\"input_to_present_ms\":-1,\"inputs\":0,\"refresh_hz\":0,\"cpu_pct\":0,\"native\":true}",
                 fps, fps, p50, p95, mx, nobj, nobj);
          static double tl; if (now - tl >= 10) { tl = now; nlog("[nhook] [perf] %.0f fps, frame %.1f / %.1f / %.1f ms (median / 95%% / max), %d players, %d objects, map %s%s; object_delete %lu, bullets %u, aimed %lu / measured %lu, voice packets %lu, camera %s%s", fps, p50, p95, mx, np_for_perf, nobj, g_st_curmap, t0 ? "" : " (not in a match)", g_nov_del_calls, g_nov_imp_n, g_nov_aim_n, g_nov_aim_done, g_opus_n,
                      g_nov_cam_src == 1 ? "view constants" : g_nov_cam_src == 2 ? "observer" : "-",
                      getenv("NHOOK_TEXTHOOK_TEST") ? ({ static char tb[300]; extern volatile unsigned long g_tht_n[8]; extern uint32_t g_tht_rva[8]; int q = snprintf(tb, sizeof tb, ", text hooks");
                          for (int i = 0; i < 8; i++) if (g_tht_rva[i]) q += snprintf(tb + q, sizeof tb - (size_t)q, " %x:%lu", g_tht_rva[i], g_tht_n[i]); tb; }) : ""); } }
      else sb_put(s, ",\"perf\":null"); }
    hc_json(s, now);
    sb_put(s, ",\"latch\":{\"mode\":\"off\",\"applying\":false,\"deg_per_count\":0,\"r2\":0,\"input_age_ms\":0,\"moving_frames\":0,\"last_deg\":0,\"in_vehicle_or_dead\":false}");
    sb_put(s, ",\"autovote\":{\"calls\":\"off\",\"map\":\"off\"}");
    char m[64] = "";
    { char mp[1024] = ""; hb_current_map(mp, sizeof mp);
      const char* b = strrchr(mp, '/'); b = b ? b + 1 : mp; snprintf(m, sizeof m, "%s", b); char* dot = strrchr(m, '.'); if (dot) *dot = 0;
      sb_put(s, ",\"map\":"); sb_str(s, m); snprintf(g_st_curmap, sizeof g_st_curmap, "%s", m);
      if (__atomic_exchange_n(&g_match_pending, 0, __ATOMIC_ACQ_REL)) st_event_pos(0, "game", m, 0, 0, -1, -1, 0, "Match started: %s", g_loading[0] ? g_loading : m[0] ? m : "?");
      if (t0) st_maptop_ensure(mp, m); }
    sb_put(s, ",\"zone_show\":%d,\"weak_show\":%d,\"overlay\":{", nz_show(), g_nov_wk); nov_json(s);
    sb_put(s, ",\"teleport\":false,\"host\":false,\"tp_forward\":false,\"controls\":%s,\"spar\":false,\"spar_aim\":-1.0,\"spar_available\":false,"
              "\"bounds_off\":null,\"invwalls_off\":null,\"mic\":\"off\",\"mic_in_use\":false,\"speed_idx\":-1,\"quality\":\"\"", st_controls_ok() ? "true" : "false");
    sb_put(s, ",\"spots\":[]}");
    if (strcmp(m, g_trk_map)) { snprintf(g_trk_map, sizeof g_trk_map, "%s", m); g_ntrk = 0; g_nbip = 0; sb_reset(); }   // a new map: new match stats
    pthread_mutex_lock(&g_hm_mx); snprintf(g_hm_cur, sizeof g_hm_cur, "%s", m); if (g_hm.dirty && now - g_hm.saved > 30) { hm_save(); g_hm.saved = now; } pthread_mutex_unlock(&g_hm_mx);
    static uint8_t tab[HB_MAXOBJ * 0x18]; int hi = 0; uint64_t arr = hb_object_array(); uint8_t hdr[0x60];
    if (arr && hb_read(arr, hdr, sizeof hdr) && *(uint32_t*)(hdr + 0x2c) == 0x64407440 && *(int32_t*)(hdr + 0x24) == 0x18) {
        hi = *(int32_t*)(hdr + 0x3c);
        if (t_rd_prof) { static double tl; if (now - tl > 10) { tl = now; uint64_t lo = ~0ull, hp = 0; int nn = 0; static uint8_t tb2[HB_MAXOBJ * 0x18]; int h2 = hi < HB_MAXOBJ ? hi : HB_MAXOBJ;
            if (hb_read(*(uint64_t*)(hdr + 0x48), tb2, (size_t)h2 * 0x18)) for (int i = 0; i < h2; i++) { uint64_t q = *(uint64_t*)(tb2 + i * 0x18 + 0x10); if (!*(uint16_t*)(tb2 + i * 0x18) || !q) continue; nn++; if (q < lo) lo = q; if (q > hp) hp = q; }
            nlog("[nhook] [rdprof] object data: %d objects between 0x%llx and 0x%llx (%.1f MB)", nn, (unsigned long long)lo, (unsigned long long)hp, nn ? (hp - lo) / 1048576.0 : 0.0); } }
        if (hi < 0 || hi > HB_MAXOBJ || hi > *(int32_t*)(hdr + 0x20) || !hb_read(*(uint64_t*)(hdr + 0x48), tab, (size_t)hi * 0x18)) hi = 0;
    }
    #define ST_DATUM(h) ((((h) & 0xFFFF) < (uint32_t)hi && (h) != 0xFFFFFFFFu && *(uint16_t*)(tab + ((h) & 0xFFFF) * 0x18) == (uint16_t)((h) >> 16)) ? *(uint64_t*)(tab + ((h) & 0xFFFF) * 0x18 + 0x10) : 0)
    #define ST_TYPE(h) (tab[((h) & 0xFFFF) * 0x18 + 3])
    StPlayer pl[256]; int np = st_players(pl, 256); np_for_perf = np;
    if (g_app_playing && !g_gameplay_t0 && np > 0 && hi > 0) { g_gameplay_t0 = now; t0 = now; g_match_pending = 1; }   // the server's next map
    { int ns = 0, any = 0; static struct { uint32_t unit; int8_t side; } tmp[256];   // Infection's sides for the overlay's team filters
      for (int k = 0; k < np && ns < 256; k++) { int z = st_is_zombie(pl[k].rec); if (z >= 0) any = 1; tmp[ns].unit = pl[k].unit; tmp[ns].side = (int8_t)z; ns++; }
      g_nov_nside = 0; memcpy(g_nov_side, tmp, sizeof tmp[0] * (size_t)ns); g_nov_nside = ns; g_nov_sides_on = any; }
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
    static StSess ss[128]; int nss = 0;
    { uint32_t sst = 0; nss = st_session(ss, 128, &sst); int hostplayer = 0;
      sb_put(s, ",\"session\":["); for (int k = 0; k < nss; k++) { sb_put(s, "%s{\"name\":", k ? "," : ""); sb_str(s, ss[k].name);
          sb_put(s, ",\"index\":%d,\"peer\":%d,\"host\":%s,\"voice\":\"%016llx\",\"muted\":%s,\"talking\":%s", ss[k].idx, ss[k].peer, ss[k].host ? "true" : "false", (unsigned long long)ss[k].voice,
                 ss[k].muted ? "true" : "false", st_talk_age(ss[k].voice) < 2.5 ? "true" : "false");
          { char ad[64]; if (pk_addr_of(ss[k].voice, ad, sizeof ad)) { sb_put(s, ",\"ip\":"); sb_str(s, ad); } } sb_put(s, "}");
          if (ss[k].host) hostplayer = 1; }
      sb_put(s, "],\"session_state\":%u,\"host_peer\":%d,\"local_peer\":%d,\"dedicated\":%s,\"mute\":%s", sst, g_st_hostpeer, g_st_localpeer, g_st_hostpeer >= 0 && nss && !hostplayer ? "true" : "false",
             g_set_muted && st_controls_ok() ? "true" : "false");
      int mx = st_max_players(); sb_put(s, ",\"slots\":{\"used\":%d", nss ? nss : np); if (mx) sb_put(s, ",\"max\":%d,\"src\":\"session\"}", mx); else sb_put(s, ",\"max\":null}");
      // addresses: the page is on 127.0.0.1 only (nothing to hide); "host" = this app hosts, "client" = it joined g_srv_addr
      { int host = g_st_localpeer >= 0 && g_st_hostpeer == g_st_localpeer; sb_put(s, ",\"addr_role\":\"%s\"", host ? "host" : nss || g_srv_addr[0] ? "client" : "none");
        if (!host && g_srv_addr[0]) { sb_put(s, ",\"server_addr\":"); sb_str(s, g_srv_addr); } } }
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
        if (pl[k].slot >= 0 && pl[k].slot < 128) { SbNow* w = &g_sbnow[pl[k].slot]; snprintf(w->name, sizeof w->name, "%s", pl[k].name); w->team = team; w->have = have && !dead; w->dead = dead; w->how[0] = 0; if (have) memcpy(w->cur, o, 12);
            if (have && !dead) { uint32_t parent = *(const uint32_t*)(d + 0x10); uint64_t vd = parent != 0xFFFFFFFFu ? ST_DATUM(parent) : 0; uint32_t df = 0;
                if (vd && hb_read(vd, &df, 4) && *(const int16_t*)(d + 0x24E) == 0) { snprintf(w->how, sizeof w->how, "%s", hb_tag_name(df)); st_nice_case(w->how); }   // the driver: a splatter is possible
                else { int ws = *(const int8_t*)(d + 0x262); const uint32_t* wp = (const uint32_t*)(d + 0x268); uint64_t wd = (ws >= 0 && ws < 4) ? ST_DATUM(wp[ws]) : 0;
                       if (wd && hb_read(wd, &df, 4)) { snprintf(w->how, sizeof w->how, "%s", hb_tag_name(df)); st_nice_case(w->how); } } } }
        int you = g_nick[0] && !strcmp(pl[k].name, g_nick);
        char pip[64] = "", prd[128] = ""; for (int q = 0; q < nss; q++) if (!strcmp(ss[q].name, pl[k].name)) { if (pk_addr_of(ss[q].voice, pip, sizeof pip)) pk_rdns_str(pip, prd, sizeof prd); break; }
        sb_put(s, "%s{\"name\":", first ? "" : ","); first = 0; sb_str(s, pl[k].name); sb_put(s, ",\"slot\":%d", pl[k].slot);
        if (pip[0]) { sb_put(s, ",\"ip\":"); sb_str(s, pip); } if (prd[0]) { sb_put(s, ",\"rdns\":"); sb_str(s, prd); }
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
        st_aim_json(s, t);
        if (pl[k].slot >= 0 && pl[k].slot < 128 && g_rs_have) { int any = 0;   // this match's medals (the game results' counts)
            for (int m = 0; m < 112; m++) { int c = g_rs_cur[pl[k].slot][RS_MEDAL + m] & 0x7fff; if (!c || m == 74 || m == 86 || (m >= 55 && m <= 57)) continue;
                sb_put(s, "%s", any ? "," : ",\"medals\":{"); any = 1; sb_str(s, g_medal[m]); sb_put(s, ":%d", c); }
            if (any) sb_put(s, "}"); }
        static int noaim = -1; if (noaim < 0) noaim = getenv("NHOOK_NO_AIM") != 0;   // hits: shots through an enemy's hitbox (the aim meter); without it the old estimate
        int hits = t ? (noaim ? t->hits_sphere : t->hits) : 0;
        if (t) sb_put(s, ",\"deaths\":%d,\"hits\":%d,\"accuracy\":%.3f,\"shots\":%d,\"life\":%.1f,\"longest_life\":%.1f,\"distance\":%.1f,\"top_speed\":%.2f", sbok ? sbs[SB_DEATHS] : t->deaths, hits, t->shots ? (double)hits / t->shots : 0.0, t->shots, t->alive ? now - t->life_start : 0.0,
                      t->longest > (t->alive ? now - t->life_start : 0) ? t->longest : (t->alive ? now - t->life_start : 0), t->dist, t->top_speed);
        if (g_st_var_ok) st_player_traits(s, pl[k].rec);
        sb_put(s, "}");
    }
    if (t0) { rs_tick(pl, np, now); sb_tick(pl, np, now); }
    { static int lg = -1; if (lg < 0) lg = getenv("NHOOK_RESULTS_LOG") != 0;   // game results: what changes (diagnostic)
      if (lg && g_res_base && g_res_vslog > 0 && g_res_vslog <= 8) {
        static uint16_t prev[128][0x263]; static uint8_t have[128]; uint16_t cur[0x263]; int rowl = 1 << g_res_vslog;
        for (int k = 0; k < np; k++) { int sl = pl[k].slot; if (sl < 0 || sl >= 128 || sl >= rowl) continue;
            if (rd(g_res_base + (uint64_t)g_res_stats + (uint64_t)sl * 0x4c6, cur, sizeof cur)) continue;
            if (have[sl]) { char b[700]; int w = 0;
                for (int i = 0; i < 0x263 && w < 600; i++) if ((cur[i] & 0x7fff) != (prev[sl][i] & 0x7fff)) w += snprintf(b + w, sizeof b - (size_t)w, " %d:%d->%d", i, prev[sl][i] & 0x7fff, cur[i] & 0x7fff);
                if (w) nlog("[nhook] [results] %s (slot %d) stats%s", pl[k].name, sl, b); }
            memcpy(prev[sl], cur, sizeof cur); have[sl] = 1; }
        static uint32_t vprev[128 * 128]; static int vhave; static uint32_t vcur[128 * 128]; size_t vn = (size_t)rowl * (size_t)rowl;
        if (vn <= 128 * 128 && !rd(g_res_base + (uint64_t)g_res_vs, vcur, vn * 4)) {
            if (vhave) for (size_t i = 0; i < vn; i++) if (vcur[i] != vprev[i]) nlog("[nhook] [results] versus [%zu][%zu]: %08x -> %08x", i / (size_t)rowl, i % (size_t)rowl, vprev[i], vcur[i]);
            memcpy(vprev, vcur, vn * 4); vhave = 1; } } }
    { static unsigned seen; unsigned n = __atomic_load_n(&g_nov_shot_n, __ATOMIC_ACQUIRE); if (n - seen > 8192) seen = n - 8192;   // shots / hits per player (object_delete)
      for (; seen != n; seen++) { const NovShot* q = &g_nov_shot[seen & 8191];
          for (int k = 0; k < np; k++) if (pl[k].unit == q->owner) { StTrack* tr = st_track(&pl[k]); if (tr) { tr->shots++; if (q->victim != 0xFFFFFFFFu) tr->hits_sphere++; } break; } } }
    st_aim_tick(pl, np, now, tab, hi);                                  // aim vs hits (noverlay.c nov_aim_shot / nov_aim_tick)
    st_spawns(tab, hi, pl, np, m, now);                        // spawns / pickups -> the feed, the heatmap's spawn points, "items"
    // the forensic archive: roster changes, session boundaries. Its map: not the loading screen's scenery (130_epilogue, while
    // joining and for the first seconds of a match until hb_current_map's 2 s recheck): a session that starts then is named
    // after the server instead (a mid-game join was "130_epilogue" every time, 2026-11-07)
    fx_roster_tick(t0 ? (now - t0 > 8 ? m : "") : strcmp(m, "130_epilogue") ? m : "", ss, nss, pl, np);
    if (getenv("NHOOK_CAM_LOG")) { static double tl; uint64_t blk = tls_block(), ob = 0; float r[40];   // the observer camera (local user 0)
        if (now - tl > 2 && blk && hb_read(blk + 0x578, &ob, 8) && ob && hb_read(ob + 0x11c, r, sizeof r)) { tl = now; char b[700]; int k = 0;
            for (int i = 0; i < 40; i++) k += snprintf(b + k, sizeof b - (size_t)k, " %d:%.3f", i * 4, r[i]);
            nlog("[nhook] [cam] observer 0x%llx result%s", (unsigned long long)ob, b);
            for (int q = 0; q < np; q++) { uint64_t p = ST_DATUM(pl[q].unit); float c[4]; if (p && hb_read(p + 0x1c, c, 16)) nlog("[nhook] [cam]   %s centre %.3f %.3f %.3f r %.3f", pl[q].name, c[0], c[1], c[2], c[3]); } } }
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
    // spawned weapons / equipment / vehicles / power-ups as they are now (stats.c's "items" and "deployed")
    sb_put(s, "],\"items\":["); first = 1;
    for (int i = 0; i < hi && i < HB_MAXOBJ; i++) { if (!g_wt[i].live || (g_wt[i].st != 1 && g_wt[i].st != 2) || g_wt[i].kind < 0) continue; int kd = g_wt[i].kind;
        const float* c = g_wt[i].cur; if (!isfinite(c[0]) || !isfinite(c[1]) || !isfinite(c[2])) continue;
        sb_put(s, "%s[%d,%d,%d,%.2f,%.2f,%.2f,%d", first ? "" : ",", g_wt[i].sp, kd == 1 ? 1 : kd == 2 ? 3 : g_wt[i].type == 3 ? 2 : 0, g_wt[i].state, c[0], c[1], c[2], g_wt[i].holder); first = 0;
        if (g_wt[i].sp < 0) { sb_put(s, ","); sb_str(s, st_wclass(g_wt[i].def, g_wt[i].type)->nm); }
        sb_put(s, "]"); }
    sb_put(s, "],\"deployed\":["); first = 1;
    if (g_gameplay_t0) for (int i = 0; i < hi && i < HB_MAXOBJ; i++) { const uint8_t* e = tab + i * 0x18; if (!*(const uint16_t*)e || e[3] != 3) continue; uint64_t p = *(const uint64_t*)(e + 0x10); uint8_t d[0x5c];
        if (!p || !hb_read(p, d, sizeof d) || *(const uint32_t*)(d + 0x10) != 0xFFFFFFFFu) continue;
        const StWcls* c = st_wclass(*(const uint32_t*)d, 3); if (!c->dep) continue; const float* o = (const float*)(d + 0x50); if (!isfinite(o[0]) || !isfinite(o[1])) continue;
        sb_put(s, "%s[%.2f,%.2f,%.2f,", first ? "" : ",", o[0], o[1], o[2]); first = 0; sb_str(s, c->nm); sb_put(s, "]"); }
    sb_put(s, "]}");
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
        if (g_hc_off[HC_SNAP]) { usleep(100000); continue; }   // (the hook cost measurement: snapshots paused)
        static int prof = -1; if (prof < 0) prof = getenv("NHOOK_RD_PROF") != 0; t_rd_prof = prof;
        unsigned long n0 = t_rd_n, b0 = t_rd_b, h0 = t_oc_hits; uint64_t t0 = hc_now(); StBuf s = {0};
        t_tls_memo = 1; t_sb_memo = 1; st_ocache_fill(); st_snapshot(&s); t_noc = 0; t_tls_memo = 0; t_sb_memo = 0; hc_add(HC_SNAP, t0);
        g_snap_rd_n = t_rd_n - n0; g_snap_rd_b = t_rd_b - b0; g_snap_oc_hits = t_oc_hits - h0;
        if (prof) { static double tl; double nw = mono_s(); if (nw - tl > 10) { tl = nw; Dl_info di; uintptr_t base = 0; if (dladdr((void*)&st_snapshot, &di)) base = (uintptr_t)di.dli_fbase;
            nlog("[nhook] [rdprof] the snapshot: %lu system calls, %lu bytes a run (%lu reads answered from the %.1f MB object copy); sites over 10 s (nhook.dylib offsets, atos -o shim/build/nhook.dylib -l 0x0):", g_snap_rd_n, g_snap_rd_b, g_snap_oc_hits, g_snap_oc_mb100 / 100.0);
            for (int i = 0; i < g_rd_nsite; i++) for (int j = i + 1; j < g_rd_nsite; j++) if (g_rd_site[j].n > g_rd_site[i].n) { __typeof__(g_rd_site[0]) x = g_rd_site[i]; g_rd_site[i] = g_rd_site[j]; g_rd_site[j] = x; }
            for (int i = 0; i < g_rd_nsite && i < 14; i++) nlog("[nhook] [rdprof]   0x%lx: %lu reads, %lu bytes", (unsigned long)(g_rd_site[i].site - base), g_rd_site[i].n, g_rd_site[i].b);
            g_rd_nsite = 0; } }
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
static int st_origin_ok(const char* hdrs) {               // no Origin (curl) or the page's own
    char o[300], hv[256]; if (!st_hdr(hdrs, "Origin", o, sizeof o)) return 1; if (!st_hdr(hdrs, "Host", hv, sizeof hv)) return 0;
    char want[300]; snprintf(want, sizeof want, "http://%s", hv); return !strcasecmp(o, want); }
static int st_control(const char* act, const char* qs) {
    if (nov_control(act, qs)) return 1;                       // the overlay's buttons and panel
    if (!strcmp(act, "capture")) { extern volatile int g_nr_cap_depth; if (strstr(qs, "depth=1")) g_nr_cap_depth = 1;
        __atomic_store_n(&g_nr_cap_req, strstr(qs, "overlay=1") ? 2 : 1, __ATOMIC_RELEASE); nlog("[nhook] page: capturing the next frame"); return 1; }
    if (!strcmp(act, "rec")) {                                // secs=<n>&mode=av|audio (rec.m; the picture = the app's frame, scaled)
        const char* v = strstr(qs, "secs="); double secs = v ? atof(v + 5) : 30; const char* m = strstr(qs, "mode="); int ao = m && !strncmp(m + 5, "audio", 5);
        int live = g_nr_last > 0 && mono_s() - g_nr_last < 2.0; char err[200];
        return rec_start(ao, secs, g_st_curmap[0] ? g_st_curmap : "game", ao || !live ? 0 : g_nr_w, ao || !live ? 0 : g_nr_h, err, sizeof err); }
    if (!strcmp(act, "recstop")) { rec_stop(); return 1; }
    if (!strcmp(act, "forensic")) return fx_control(qs);       // archive / voice audio / consent (forensic.c)
    if (!strcmp(act, "hookab")) return hc_control(qs);           // the hook cost meter's on/off measurement
    if (!strcmp(act, "hookset")) return hc_set(qs);               // ... and its per-hook switches
    if (!strcmp(act, "mute") || !strcmp(act, "unmute")) { voice_init(); const char* v = strstr(qs, "voice="); unsigned long long id = v ? strtoull(v + 6, 0, 16) : 0;
        if (!g_set_muted || !id) return 0; g_set_muted(id, !strcmp(act, "mute")); uint64_t mu[64];
        nlog("[nhook] page: %s %016llx (the app's mute list now holds %d)", act, id, muted_list(mu, 64)); return 1; }
    if (!strcmp(act, "muteall") || !strcmp(act, "unmuteall")) { voice_init(); if (!g_set_muted) return 0; static StSess ss2[128]; int n = st_session(ss2, 128, 0), on = !strcmp(act, "muteall"), k = 0;
        for (int i = 0; i < n; i++) if (ss2[i].voice && ss2[i].muted != on && !(g_st_localpeer >= 0 && ss2[i].peer == g_st_localpeer)) { g_set_muted(ss2[i].voice, on); k++; }
        nlog("[nhook] page: %s (%d players)", act, k); return 1; }
    return 0;
}
// GET /recordings/<file>: a clip from ~/Movies/Reclaimer (stats.c st_rec_file: Range requests answered)
static void st_rec_file(int fd, const char* req, const char* name) {
    char fp[1100], h[512]; struct stat st; int f = rec_file_path(name, fp, sizeof fp) ? open(fp, O_RDONLY) : -1;
    if (f < 0 || fstat(f, &st)) { const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd, nf, strlen(nf)); if (f >= 0) close(f); return; }
    long long a = 0, b = (long long)st.st_size - 1; int part = 0; const char* rg = strcasestr(req, "\r\nRange: bytes=");
    if (rg && st.st_size > 0) { long long x = -1, y = -1; int c = sscanf(rg + 15, "%lld-%lld", &x, &y); if (c >= 1 && x >= 0 && x < st.st_size) { a = x; if (c == 2 && y >= a && y < st.st_size) b = y; part = 1; } }
    int k = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n", part ? "206 Partial Content" : "200 OK", strstr(name, ".m4a") ? "audio/mp4" : "video/mp4", st.st_size ? b - a + 1 : 0);
    if (part) k += snprintf(h + k, sizeof h - (size_t)k, "Content-Range: bytes %lld-%lld/%lld\r\n", a, b, (long long)st.st_size);
    k += snprintf(h + k, sizeof h - (size_t)k, "Content-Disposition: inline; filename=\"%s\"\r\nConnection: close\r\n\r\n", name);
    if (st_send(fd, h, (size_t)k) && st.st_size) { char* buf = malloc(1 << 18); long long left = b - a + 1, off = a;
        while (buf && left > 0) { ssize_t n = pread(f, buf, left < (1 << 18) ? (size_t)left : (size_t)(1 << 18), off); if (n <= 0 || !st_send(fd, buf, (size_t)n)) break; off += n; left -= n; }
        free(buf); }
    close(f);
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
    if (!strcmp(path, "/control")) {                        // POST /control?do=<action> (stats.c's guard: header + Origin)
        char act[32] = "", hv[64]; const char* dd = strstr(qs, "do="); if (dd) sscanf(dd + 3, "%31[a-z]", act);
        int hdr = st_hdr(req, "X-Reclaimer-Control", hv, sizeof hv) != 0 && st_origin_ok(req);
        int code = !strcmp(meth, "POST") && hdr ? (st_controls_ok() ? (st_control(act, qs) ? 200 : 400) : 403) : 403;
        const char* body = code == 200 ? "{\"ok\":true}" : code == 400 ? "{\"ok\":false,\"error\":\"unknown, disabled or not in the native app yet\"}" : !st_controls_ok() ? "{\"ok\":false,\"error\":\"controls are off\"}" : "{\"ok\":false,\"error\":\"needs POST with an X-Reclaimer-Control header\"}";
        char h[400]; int k = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", code, code == 200 ? "OK" : code == 400 ? "Bad Request" : "Forbidden", strlen(body), body);
        st_send(fd, h, (size_t)k);
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
    else if (!strcmp(path, "/recordings.json")) { size_t ln = 0; char* j = rec_list_json(&ln); st_reply(fd, "application/json", j ? j : "[]", j ? ln : 2, 0); free(j); }
    else if (!strncmp(path, "/recordings/", 12)) st_rec_file(fd, req, path + 12);
    else if (!strncmp(path, "/forensics", 10) && (!path[10] || path[10] == '/')) fx_http(fd, path, qs, req);
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
    { pthread_t t; nh_thread(&t, st_snap_thread, 0, QOS_CLASS_UTILITY); }
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
        pthread_t c; if (nh_thread(&c, st_conn, (void*)(intptr_t)fd, QOS_CLASS_UTILITY)) close(fd);
    }
    return 0;
}
static void nstats_start(void) {
    const char* e = getenv("NHOOK_STATS_HTTP"); if (e) g_st_port = atoi(e); if (g_st_port <= 0 || g_st_port > 65535) return;
    Dl_info di; if (dladdr((void*)nstats_start, &di) && di.dli_fname) {   // the repo: <repo>/shim/build/nhook.dylib
        snprintf(g_repo, sizeof g_repo, "%s", di.dli_fname); for (int k = 0; k < 3; k++) { char* sl = strrchr(g_repo, '/'); if (sl) *sl = 0; } }
    nick_load();
    { extern char*** _NSGetArgv(void); extern int* _NSGetArgc(void); char** av = *_NSGetArgv(); int ac = *_NSGetArgc();
      for (int i = 1; i + 1 < ac; i++) if (!strcmp(av[i], "--join-server")) snprintf(g_cmdlineA, sizeof g_cmdlineA, "join?server=%s", av[i + 1]);
      for (int i = 1; i < ac; i++) if (strstr(av[i], "join?server=")) snprintf(g_cmdlineA, sizeof g_cmdlineA, "%s", av[i]); } { const char* pe = getenv("NHOOK_PKTLOG"); if (pe && *pe) g_pk_on = atoi(pe) != 0; }
    pthread_t t; nh_thread(&t, st_listen_thread, 0, QOS_CLASS_UTILITY);
}
