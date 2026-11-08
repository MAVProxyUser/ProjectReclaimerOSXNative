// ntracks.c -- every public match recorded for the walkers (included by nstats.c; 2026-11-08, the user: "I feel like any game played
// on a public server should be collecting stats for our walkers to hone themselves on in the future").
//
// While the app is in a match on a server that is not this Mac (the joined server's address is not 127.x), each stats snapshot
// (~10 a second) writes one line to <ReclaimerMac/tracks>/<date>_<map>_<server tag>.jsonl.gz, the format of
// shim/tools/match-recorder.py, so engage-extract.py / walker-model.py / lanes-import.py read it as they are:
//   header  {"start": unix, "map", "server": 8-hex tag of the address (FNV-1a), "mode": the game engine ("Slayer", "Capture the
//            Flag", "Infection" ...), "variant": its name, "lanes_live": the game counts lanes itself, "source": "nhook",
//            "you_plays": true (the local player is a real player here, not an observer: "you" stays -1)}
//   sample  {"t", "you": -1, "p": [[slot, team, alive, x, y, z, yaw, pitch, shield, health, weapon, vehicle, kills, deaths, shots,
//            hits, grenades], ...], "proj": 0}
//   event   {"t", "ev": kill / death / weapon / vehicle / powerup ..., "a", "b": slots (names are never written), "how", "est",
//            "verb", "pos"}
// A new file per match (a new map or a new match start); closed when the match ends or the app leaves. NO NAMES: a name only maps to
// its slot inside this process. NHOOK_TRACKS=0 = off (shim/tools/observe-public.sh runs its own recorder), NHOOK_TRACKS_DIR=<dir>.
#include <zlib.h>
static char g_trk_mode[48], g_trk_var[64];   // (st_variant: this snapshot's game engine and variant name)
static struct { gzFile f; char map[64], path[1024]; double t0, start; uint32_t ev_seq; int n, nev, rowopen; StBuf row; char names[128][64]; } g_tks;
static int trk_enabled(void) { static int on = -1; if (on < 0) { const char* e = getenv("NHOOK_TRACKS"); on = !(e && !strcmp(e, "0")); } return on; }
static int trk_public(void) { return g_srv_addr[0] && strncmp(g_srv_addr, "127.", 4) != 0; }
static void trk_close(const char* why) {
    if (!g_tks.f) return; gzclose(g_tks.f); g_tks.f = 0;
    nlog("[nhook] [tracks] %s: %d samples, %d events (%s) -> %s", g_tks.map, g_tks.n, g_tks.nev, why, strrchr(g_tks.path, '/') ? strrchr(g_tks.path, '/') + 1 : g_tks.path); }
static void trk_begin(double now, double t0, const char* map) {
    g_tks.rowopen = 0;
    if (!trk_enabled() || !trk_public() || !t0 || !map[0]) { if (g_tks.f) trk_close("match over"); return; }
    if (g_tks.f && (strcmp(map, g_tks.map) || t0 != g_tks.t0)) trk_close("a new match");
    if (!g_tks.f) {
        char dir[900]; const char* d = getenv("NHOOK_TRACKS_DIR"), * h = getenv("HOME");
        if (d && *d) snprintf(dir, sizeof dir, "%s", d); else snprintf(dir, sizeof dir, "%s/Library/Application Support/ReclaimerMac/tracks", h ? h : "/tmp");
        mkdir(dir, 0755);
        uint32_t fnv = 2166136261u; for (const char* c = g_srv_addr; *c; c++) { fnv ^= (uint8_t)*c; fnv *= 16777619u; }
        time_t tt = time(0); struct tm tm; localtime_r(&tt, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
        snprintf(g_tks.path, sizeof g_tks.path, "%s/%s_%s_%08x.jsonl.gz", dir, ts, map, fnv);
        g_tks.f = gzopen(g_tks.path, "wb6"); if (!g_tks.f) return;
        snprintf(g_tks.map, sizeof g_tks.map, "%s", map); g_tks.t0 = t0; g_tks.start = now; g_tks.n = g_tks.nev = 0; memset(g_tks.names, 0, sizeof g_tks.names);
        pthread_mutex_lock(&g_ev_mx); g_tks.ev_seq = g_ev_seq; pthread_mutex_unlock(&g_ev_mx);
        StBuf hb = {0}; sb_put(&hb, "{\"start\":%.3f,\"map\":", (double)tt); sb_str(&hb, map); sb_put(&hb, ",\"server\":\"%08x\",\"mode\":", fnv); sb_str(&hb, g_trk_mode);
        sb_put(&hb, ",\"variant\":"); sb_str(&hb, g_trk_var); sb_put(&hb, ",\"lanes_live\":%s,\"source\":\"nhook\",\"you_plays\":true}\n", ln_on() ? "true" : "false");
        gzwrite(g_tks.f, hb.b, (unsigned)hb.n); free(hb.b);
        nlog("[nhook] [tracks] recording %s (%s %s) -> %s", map, g_trk_mode, g_trk_var, strrchr(g_tks.path, '/') + 1); }
    g_tks.row.n = 0; sb_put(&g_tks.row, "{\"t\":%.2f,\"you\":-1,\"p\":[", now - g_tks.start); g_tks.rowopen = 1; }
static void trk_player(int slot, const char* name, int team, int alive, const float* p, float yaw, float pitch, float sh, float hp, const char* wpn, const char* veh,
                       int kills, int deaths, int shots, int hits, int gren) {
    if (!g_tks.rowopen || slot < 0 || slot >= 128) return;
    if (name) snprintf(g_tks.names[slot], sizeof g_tks.names[slot], "%s", name);
    StBuf* r = &g_tks.row; int first = r->n > 0 && r->b[r->n - 1] == '[';
    sb_put(r, "%s[%d,%d,%d,%.3f,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,", first ? "" : ",", slot, team, alive, alive ? p[0] : 0.0, alive ? p[1] : 0.0, alive ? p[2] : 0.0, yaw, pitch, sh, hp);
    sb_str(r, wpn ? wpn : ""); sb_put(r, ","); sb_str(r, veh ? veh : ""); sb_put(r, ",%d,%d,%d,%d,%d]", kills, deaths, shots, hits, gren); }
static int trk_slot(const char* name) { if (!name || !name[0]) return -1; for (int i = 0; i < 128; i++) if (g_tks.names[i][0] && !strcmp(g_tks.names[i], name)) return i; return -1; }
static void trk_end(void) {
    if (!g_tks.rowopen || !g_tks.f) return; g_tks.rowopen = 0;
    sb_put(&g_tks.row, "],\"proj\":0}\n"); gzwrite(g_tks.f, g_tks.row.b, (unsigned)g_tks.row.n); g_tks.n++;
    // the feed's new events (kills, deaths, spawns, pickups ...): names -> slots, then dropped
    static StEvt ev[64]; int ne = 0; pthread_mutex_lock(&g_ev_mx); uint32_t top = g_ev_seq;
    if (top - g_tks.ev_seq > ST_NEV) g_tks.ev_seq = top - ST_NEV;
    for (uint32_t q = g_tks.ev_seq + 1; q <= top && ne < 64; q++) { const StEvt* e = &g_ev[q % ST_NEV]; if (e->seq == q) ev[ne++] = *e; g_tks.ev_seq = q; }
    pthread_mutex_unlock(&g_ev_mx);
    for (int i = 0; i < ne; i++) { const StEvt* e = &ev[i];
        if (!strcmp(e->kind, "chat") || !strcmp(e->kind, "join") || !strcmp(e->kind, "leave") || !strcmp(e->kind, "vote") || !strcmp(e->kind, "log") || !strcmp(e->kind, "server") || !strcmp(e->kind, "notice")) continue;
        StBuf b = {0}; sb_put(&b, "{\"t\":%.2f,\"ev\":", mono_s() - g_tks.start); sb_str(&b, e->kind); sb_put(&b, ",\"a\":%d,\"b\":%d,\"how\":", trk_slot(e->a), trk_slot(e->b)); sb_str(&b, e->how);
        sb_put(&b, ",\"est\":%s,\"verb\":", (e->flags & 1) ? "true" : "false"); sb_str(&b, e->verb);
        if (e->haspos) sb_put(&b, ",\"pos\":[%.2f,%.2f,%.2f]", e->pos[0], e->pos[1], e->pos[2]);
        sb_put(&b, "}\n"); gzwrite(g_tks.f, b.b, (unsigned)b.n); free(b.b); g_tks.nev++; }
    if (!(g_tks.n % 600)) gzflush(g_tks.f, Z_SYNC_FLUSH);   // (every minute: a crash loses at most a minute)
}
static void trk_atexit(void) { trk_close("the app quit"); }
