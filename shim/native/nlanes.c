// nlanes.c -- the lanes players use (included by nstats.c after the heatmap; 2026-11-08, the user: "start mapping 'lanes' that
// players usually use and make a 'lane' option for the heat map, so you can select lanes, or deaths. I'd expect the lanes to
// start in essence carving a visible rut the more they are used across the map").
//
// Every stats snapshot (~10 a second) each live player's move since the previous one -- the same body, at most LN_MAXDT apart,
// longer than 0.02 units, slower than LN_MAXSPEED (respawns and teleports are no lane) -- is walked through the 0.5-unit grid
// (Amanatides & Woo) and each cell it crosses gets the length inside it, in cells: a cell's count is how many times players
// crossed it ("passes"; a diagonal crossing counts up to 1.4). Standing still adds nothing: the footprint heatmap counts time,
// this counts traffic. On foot and in a vehicle are kept apart (vehicles: the seated player's moves, aircraft included), and the
// on-foot cells also sum the direction of travel (the page's hover: "mostly heading north-east" / "both ways").
//
// Per map, every match ever seen: <heatmaps folder>/<map>.lanes (RLN1, ln_read below). Saved every 30 s and at exit as a MERGE:
// under an flock on <map>.lanes.lock, read the file, add what came since the last save, write a temporary file, rename -- so
// shim/tools/lanes-import.py (the recorded matches, ReclaimerMac/tracks) and a second game add to it instead of overwriting each
// other. The file also lists the recordings already imported. This match's lanes are kept in memory only.
// GET /lanes.json?scope=all|match (the page's heatmap "Lanes"); the snapshot says "lanes_live": true (match-recorder.py marks its
// recordings with it, so lanes-import.py never counts a match twice). NHOOK_LANES=0 = off.
#include <sys/file.h>
#define LN_CELL 0.5f
#define LN_MAXDT 0.6
#define LN_MAXSPEED 40.0f
typedef struct { int32_t x, y; float foot, veh, fx, fy; } LnCell;   // foot / veh: passes; fx, fy: on-foot travel direction sums (passes)
_Static_assert(sizeof(LnCell) == 24, "lane cell");
typedef struct { LnCell* c; uint32_t cap, used; } LnGrid;
typedef char LnSrc[64];
static struct { char map[64]; LnGrid all, add, match; LnSrc* src; uint32_t nsrc; double since, saved, match_t0; int loaded; } g_ln;
static pthread_mutex_t g_ln_mx = PTHREAD_MUTEX_INITIALIZER;
static int ln_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("NHOOK_LANES"); on = !(e && !strcmp(e, "0")); } return on; }
// (2026-11-08: a game on a LOOPBACK server is our local test server -- walkers, tests; the user's own client joined the walkers
//  once with the real profile and wore 3.9 km of walker moves into Guardian's real lanes: not recorded there; NHOOK_LANES_LOCAL=1)
static int ln_here(void) { static int loc = -1; if (loc < 0) loc = getenv("NHOOK_LANES_LOCAL") != 0; return loc || strncmp(g_srv_addr, "127.", 4) != 0; }

static void ln_free(LnGrid* g) { free(g->c); memset(g, 0, sizeof *g); }
static void ln_add(LnGrid* g, int32_t x, int32_t y, float foot, float veh, float fx, float fy) {
    if (g->used * 10 >= g->cap * 7) {
        uint32_t oc = g->cap, nc = oc ? oc * 2 : 16384; LnCell* nt = calloc(nc, sizeof *nt), *ot = g->c; if (!nt) return;
        g->c = nt; g->cap = nc; g->used = 0;
        for (uint32_t i = 0; i < oc; i++) if (ot[i].foot > 0 || ot[i].veh > 0) ln_add(g, ot[i].x, ot[i].y, ot[i].foot, ot[i].veh, ot[i].fx, ot[i].fy);
        free(ot);
    }
    uint32_t m = g->cap - 1, i = ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & m;
    for (;; i = (i + 1) & m) { LnCell* c = &g->c[i];
        if (!(c->foot > 0 || c->veh > 0)) { c->x = x; c->y = y; c->foot = foot; c->veh = veh; c->fx = fx; c->fy = fy; g->used++; return; }
        if (c->x == x && c->y == y) { c->foot += foot; c->veh += veh; c->fx += fx; c->fy += fy; return; } }
}
static void ln_path(const char* map, const char* ext, char* out, size_t n) {   // beside the heatmap: <dir>/<map><ext>
    char p[1024]; hm_path(map, p, sizeof p); size_t l = strlen(p); if (l > 4 && !strcmp(p + l - 4, ".bin")) p[l - 4] = 0; snprintf(out, n, "%s%s", p, ext); }
// RLN1: "RLN1", f32 cell, u32 cells, u32 sources, f64 since (unix time of the first live sample, 0 = none), u32 reserved[4],
// then the cells (LnCell), then the sources (64-byte names of the recordings lanes-import.py added)
static int ln_read(const char* p, LnGrid* g, LnSrc** src, uint32_t* nsrc, double* since) {
    FILE* f = fopen(p, "rb"); if (!f) return 0; char mg[4]; float cell = 0; uint32_t nc = 0, ns = 0, rs[4]; double sn = 0; int ok = 0;
    if (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "RLN1", 4) && fread(&cell, 4, 1, f) == 1 && cell == LN_CELL && fread(&nc, 4, 1, f) == 1 && fread(&ns, 4, 1, f) == 1
        && fread(&sn, 8, 1, f) == 1 && fread(rs, 4, 4, f) == 4 && nc < (1u << 24) && ns < (1u << 20)) {
        ok = 1; LnCell c;
        for (uint32_t i = 0; i < nc; i++) { if (fread(&c, sizeof c, 1, f) != 1) { ok = 0; break; } if (c.foot > 0 || c.veh > 0) ln_add(g, c.x, c.y, c.foot, c.veh, c.fx, c.fy); }
        LnSrc* s = ns ? calloc(ns, sizeof *s) : 0; uint32_t got = s ? (uint32_t)fread(s, sizeof *s, ns, f) : 0;
        for (uint32_t i = 0; i < got; i++) s[i][63] = 0;
        *src = s; *nsrc = got; *since = sn;
    }
    fclose(f); return ok;
}
static int ln_write(const char* p, const LnGrid* g, const LnSrc* src, uint32_t nsrc, double since) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp%d", p, (int)getpid()); FILE* f = fopen(tmp, "wb"); if (!f) return 0;
    float cell = LN_CELL; uint32_t rs[4] = {0}; fwrite("RLN1", 1, 4, f); fwrite(&cell, 4, 1, f); fwrite(&g->used, 4, 1, f); fwrite(&nsrc, 4, 1, f); fwrite(&since, 8, 1, f); fwrite(rs, 4, 4, f);
    for (uint32_t i = 0; i < g->cap; i++) if (g->c[i].foot > 0 || g->c[i].veh > 0) fwrite(&g->c[i], sizeof(LnCell), 1, f);
    if (nsrc) fwrite(src, sizeof(LnSrc), nsrc, f);
    int ok = !ferror(f); if (fclose(f)) ok = 0; if (ok) ok = !rename(tmp, p); if (!ok) unlink(tmp); return ok;
}
static int ln_lock(const char* map, int how) {   // the file's lock (lanes-import.py takes the same one)
    char lp[1100]; ln_path(map, ".lanes.lock", lp, sizeof lp); int fd = open(lp, O_RDWR | O_CREAT, 0644); if (fd < 0) return -1;
    if (flock(fd, how) != 0) { close(fd); return -1; } return fd; }
static void ln_unlock(int fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
static void ln_save(void) {   // (g_ln_mx held) the merge: disk + what came since the last save
    if (!g_ln.map[0] || !g_ln.add.used) return;
    char p[1100]; ln_path(g_ln.map, ".lanes", p, sizeof p);
    { char dir[1100]; snprintf(dir, sizeof dir, "%s", p); char* sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdir(dir, 0755); } }
    int lk = ln_lock(g_ln.map, LOCK_EX); LnGrid t = {0}; LnSrc* src = 0; uint32_t ns = 0; double since = 0;
    ln_read(p, &t, &src, &ns, &since);
    for (uint32_t i = 0; i < g_ln.add.cap; i++) { const LnCell* c = &g_ln.add.c[i]; if (c->foot > 0 || c->veh > 0) ln_add(&t, c->x, c->y, c->foot, c->veh, c->fx, c->fy); }
    if (!since) since = g_ln.since;
    if (ln_write(p, &t, src, ns, since)) {
        ln_free(&g_ln.all); g_ln.all = t; free(g_ln.src); g_ln.src = src; g_ln.nsrc = ns; g_ln.since = since; ln_free(&g_ln.add);   // (the import's cells show from now on)
    } else { ln_free(&t); free(src); nlog("[nhook] lanes: could not write %s (kept in memory, tried again in 30 s)", p); }
    ln_unlock(lk);
}
static void ln_switch(const char* map) {   // (g_ln_mx held)
    if (g_ln.loaded && !strcmp(map, g_ln.map)) return;
    ln_save(); ln_free(&g_ln.all); ln_free(&g_ln.add); ln_free(&g_ln.match); free(g_ln.src); memset(&g_ln, 0, sizeof g_ln);
    snprintf(g_ln.map, sizeof g_ln.map, "%s", map); g_ln.loaded = 1; g_ln.saved = mono_s(); if (!map[0]) return;
    char p[1100]; ln_path(map, ".lanes", p, sizeof p); int lk = ln_lock(map, LOCK_SH); ln_read(p, &g_ln.all, &g_ln.src, &g_ln.nsrc, &g_ln.since); ln_unlock(lk);
    double tot = 0; for (uint32_t i = 0; i < g_ln.all.cap; i++) tot += g_ln.all.c[i].foot;
    nlog("[nhook] lanes %s: %u cells, %.1f km on foot from earlier matches (%u recordings imported)", map, g_ln.all.used, tot * LN_CELL * 3.048 / 1000.0, g_ln.nsrc);
}
// the snapshot, once: the map being played (a menu keeps the last one), a new match = new match lanes, the save every 30 s
static void ln_tick(const char* map, double t0, double now) {
    if (!ln_on()) return;
    pthread_mutex_lock(&g_ln_mx);
    if (map[0] && t0) ln_switch(map);
    if (t0 && t0 != g_ln.match_t0) { ln_free(&g_ln.match); g_ln.match_t0 = t0; }
    if (now - g_ln.saved > 30) { ln_save(); g_ln.saved = now; }
    pthread_mutex_unlock(&g_ln_mx);
}
static void ln_cell3(int32_t x, int32_t y, float foot, float veh, float fx, float fy) {
    ln_add(&g_ln.all, x, y, foot, veh, fx, fy); ln_add(&g_ln.add, x, y, foot, veh, fx, fy); ln_add(&g_ln.match, x, y, foot, veh, fx, fy); }
// one player's move a -> b (world units) dt seconds apart; veh: seated in a vehicle
static void ln_move(const float* a, const float* b, double dt, int veh) {
    if (!ln_on() || !ln_here() || !(dt > 0) || dt > LN_MAXDT) return;
    float dx = b[0] - a[0], dy = b[1] - a[1], d = sqrtf(dx * dx + dy * dy);
    if (!(d > 0.02f) || d > 8.0f || d / (float)dt > LN_MAXSPEED || !isfinite(a[0]) || !isfinite(b[0])) return;
    pthread_mutex_lock(&g_ln_mx);
    if (!g_ln.loaded || !g_ln.map[0]) { pthread_mutex_unlock(&g_ln_mx); return; }
    if (!g_ln.since) g_ln.since = (double)time(0);
    // Amanatides & Woo in cell units: t runs 0..1 along the move; each cell gets (t_exit - t_enter) * length / cell
    float x0 = a[0] / LN_CELL, y0 = a[1] / LN_CELL, x1 = b[0] / LN_CELL, y1 = b[1] / LN_CELL, ux = dx / d, uy = dy / d, L = d / LN_CELL;
    int32_t cx = (int32_t)floorf(x0), cy = (int32_t)floorf(y0), ex = (int32_t)floorf(x1), ey = (int32_t)floorf(y1);
    int sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1; float vx = x1 - x0, vy = y1 - y0;
    float tdx = vx != 0 ? fabsf(1.0f / vx) : 1e30f, tdy = vy != 0 ? fabsf(1.0f / vy) : 1e30f;
    float tmx = vx != 0 ? ((sx > 0 ? (cx + 1 - x0) : (x0 - cx)) * tdx) : 1e30f, tmy = vy != 0 ? ((sy > 0 ? (cy + 1 - y0) : (y0 - cy)) * tdy) : 1e30f;
    float t = 0;
    for (int k = 0; k < 64; k++) {
        float tn = tmx < tmy ? tmx : tmy; int last = (cx == ex && cy == ey) || tn >= 1.0f; if (last) tn = 1.0f;
        float part = (tn - t) * L;
        if (part > 1e-4f) { if (veh) ln_cell3(cx, cy, 0, part, 0, 0); else ln_cell3(cx, cy, part, 0, ux * part, uy * part); }
        if (last) break; t = tn;
        if (tmx < tmy) { tmx += tdx; cx += sx; } else { tmy += tdy; cy += sy; }
    }
    pthread_mutex_unlock(&g_ln_mx);
}
static void ln_json(StBuf* s, int match) {
    pthread_mutex_lock(&g_ln_mx); const LnGrid* g = match ? &g_ln.match : &g_ln.all; double foot = 0, veh = 0;
    sb_put(s, "{\"map\":"); sb_str(s, g_ln.map); sb_put(s, ",\"cell\":%.2f,\"scope\":\"%s\",\"on\":%s,\"since\":%.0f,\"imported\":%u,\"cells\":[", LN_CELL, match ? "match" : "all", ln_on() ? "true" : "false", g_ln.since, g_ln.nsrc);
    int first = 1;   // x, y, foot, vehicle (tenths of a pass), flow x, y (tenths)
    for (uint32_t i = 0; i < g->cap; i++) { const LnCell* c = &g->c[i]; if (!(c->foot > 0 || c->veh > 0)) continue; foot += c->foot; veh += c->veh;
        sb_put(s, "%s%d,%d,%d,%d,%d,%d", first ? "" : ",", c->x, c->y, (int)lroundf(c->foot * 10), (int)lroundf(c->veh * 10), (int)lroundf(c->fx * 10), (int)lroundf(c->fy * 10)); first = 0; }
    sb_put(s, "],\"foot_km\":%.2f,\"vehicle_km\":%.2f}", foot * LN_CELL * 3.048 / 1000.0, veh * LN_CELL * 3.048 / 1000.0);
    pthread_mutex_unlock(&g_ln_mx);
}
static void ln_atexit(void) { if (pthread_mutex_trylock(&g_ln_mx) == 0) { ln_save(); pthread_mutex_unlock(&g_ln_mx); } }
