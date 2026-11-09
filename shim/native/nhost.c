// ---- host tools in the native app (2026-11-07; user: "port the host tools too"): teleport (next to a player, to a point, by
// an offset, the map spots), map boundaries off, invisible walls off, movement speed -- the loader's hitbox.c / stats.c tools,
// for a game this app HOSTS (a local match, or our own dedicated server: shim/tools/native-dedicated.sh). Included by nstats.c.
//
// The loader patched halo3's x86 code; the native app runs halo3 recompiled, so instead (every point checked live with
// NHOOK_RVA_KIND / NHOOK_COUNT / the census, 2026-11-07):
//   teleport   halo3's own teleport (halo3+0x259410: object, position*, forward*, up*, bool; lifted) called with nh_call
//              (nhook.c) from a text hook on the app's per-frame function (0.10.0 0x1029ed000: the top of the engine
//              thread's frame loop -- a sample of the engine thread shows halo3's thread loop calling it once a frame, and it
//              hands off to the renderer or runs the game; x0 = the guest context), inside halo3's game-state lock
//              (halo3+0x12c948 / 0x12c9a0), as the loader did from main_loop_body (which the app no longer calls: the app
//              calls halo3's tick functions directly, never through the code table, so no entry hook sees them; a first try
//              hooked halo3+0x34674c, which looked once-a-tick in the census but is the OBJECT LOOKUP, called from
//              anywhere on several threads: the teleport crashed there). Out of a vehicle
//              first (halo3+0x3a3c4c), the destination checked inside the map structure at knee and head height
//              (scenario_location_from_point, halo3+0x9bc78), |coord| < 30000 (past 32768 the host deletes the unit).
//   boundaries the kill-volume / safe-zone test (halo3+0x1b8e00) and the player soft-ceiling test (halo3+0x1bac00, which
//              biped_update calls only for a player's biped: the loader's 0x37b734 patch skipped that call) are the APP's own
//              code now: skip-capable text hooks (stub.S _nh_tps) answer "in no kill volume, inside a safe zone" and "no
//              soft ceiling" -- the loader's own replacement semantics -- while the switch is off. (The app's versions use
//              the lifted convention: x0 = the guest context, the result to +0x00, the return address popped: checked in
//              its disassembly.) Slip surfaces (the loader's third patch, 0x219c1f) are not covered.
//   invisible  the Havok polygon-shape builder (halo3+0x21b4d8, lifted; built on demand: getChildShape ran 289,915 times in a
//   walls      game) gets an around-hook (stub.S _nh_tpls): the original runs, then a surface flagged invisible (flag 2) has
//              its vertices moved to z = -20000 -- the loader's exit patch. Shape = rcx (also the result), the BSP reference
//              {small*, large*} = *r9, the surface index = the 9th argument (guest stack +0x48 at the entry), which the
//              builder also stores at shape +0x28 (read from there).
//   speed      the game variant's base player speed trait (game_globals +0x1608 +0x134 +0x10), as the loader.
// Hooks are installed on first use (a switch or a teleport) and stay; their handlers do nothing while the switches are on.
// Host only (game_globals +0x11: 5 host / 1 local, or this machine is the session's host peer).

// the app's per-frame function: napp_builds.h frame_fn (0.10.0 0x1029ed000, 0.10.1 0x102ac23d8; its prologue checked below)
static const uint32_t g_ht_frame_words[4] = {0xd10103ffu, 0xa90157f6u, 0xa9024ff4u, 0xa9037bfdu};   // sub sp ; stp ; stp ; stp
static volatile int g_ht_bnd_off, g_ht_inv_off, g_ht_tick_ok = -1, g_ht_bnd_ok = -1, g_ht_inv_ok = -1;
static volatile unsigned long g_ht_bnd_skips, g_ht_inv_hidden, g_ht_ticks, g_ht_frames;
static volatile int g_ht_my_slot = -1;                     // the local player's slot (st_snapshot: the profile's nickname)
static volatile uint32_t g_my_unit = 0xFFFFFFFFu;
static volatile uint32_t g_human_unit[8]; static volatile int g_human_n;
static volatile uint32_t g_follow_unit = 0xFFFFFFFFu;   // NHOOK_BOT_FOLLOW=<player name>: the walker's leader and friend (st_snapshot)   // the players that are no walker (by name): nbot.c's wingman follows them (st_snapshot)          // ... and its unit (nbot.c: when the control record's unit is stale)
typedef struct { int kind, slot, who; float v[3]; char label[64]; } HtReq;   // kind 1 next to `slot`, 2 to v (who), 3 by v, 4 floor probe at v
static HtReq g_ht_req; static volatile int g_ht_pending; static pthread_mutex_t g_ht_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_ht_msg[200]; static double g_ht_msg_t;
static void ht_say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_ht_msg, sizeof g_ht_msg, fmt, ap); va_end(ap); g_ht_msg_t = st_wall(); nlog("[nhook] [host] %s", g_ht_msg); }
static int ht_is_host(void) {
    uint64_t blk = tls_block(), gg = 0; uint8_t st = 0;
    if (blk && hb_read(blk + 0x48, &gg, 8) && gg && hb_read(gg + 0x11, &st, 1) && (st == 5 || st == 1)) return 1;
    return g_st_hostpeer >= 0 && g_st_hostpeer == g_st_localpeer;
}
// ---- the engine thread: objects by handle (direct reads: this thread owns them)
static uint8_t* ht_datum(uint64_t blk, uint32_t h, int* type) {
    uint8_t* arr = *(uint8_t**)(uintptr_t)(blk + 0x38); if (!arr || *(uint32_t*)(arr + 0x2c) != 0x64407440 || h == 0xFFFFFFFFu) return 0;
    uint8_t* data = *(uint8_t**)(arr + 0x48); int i = h & 0xFFFF; if (!data || i >= *(int32_t*)(arr + 0x3c)) return 0;
    uint8_t* e = data + (size_t)i * 0x18; if (*(uint16_t*)e != (uint16_t)(h >> 16)) return 0; if (type) *type = e[3]; return *(uint8_t**)(e + 0x10);
}
static uint32_t ht_unit_of(uint64_t blk, int slot) {
    uint8_t* parr = *(uint8_t**)(uintptr_t)(blk + 0x10); if (!parr || *(uint32_t*)(parr + 0x2c) != 0x64407440 || slot < 0) return 0xFFFFFFFFu;
    int32_t es = *(int32_t*)(parr + 0x24), ph = *(int32_t*)(parr + 0x3c); uint8_t* pd = *(uint8_t**)(parr + 0x48);
    if (!pd || slot >= ph || es < 0x30) return 0xFFFFFFFFu; uint8_t* rec = pd + (size_t)slot * (size_t)es; if (!*(uint16_t*)rec) return 0xFFFFFFFFu;
    return *(uint32_t*)(rec + 0x28);
}
static int ht_inside(uint64_t* ctx, const float* p, float dz) {   // scenario_location_from_point: inside the map structure?
    static uint16_t loc; static float q[3]; loc = 0xFFFF; q[0] = p[0]; q[1] = p[1]; q[2] = p[2] + dz;
    uint64_t a[2] = {(uint64_t)(uintptr_t)&loc, (uint64_t)(uintptr_t)q}; nh_call(ctx, 0x9bc78, 2, a); return loc != 0xFFFF; }
// A floor under a point: halo3's collision ray (halo3+0x1fd748: bool (u64 flags, bool, point*, vector*, ignore, ignore, ignore,
// result*); the vector is the whole ray) straight down, as halo3's own "put it on the ground below" (0x259c7c) calls it: its
// flags = the runtime default at halo3+0x20ad508 with 0x24000 set, no object ignored (-1 x3), the result's +0x18 / +0x20 zeroed and
// its +0x28 word -1 first; the hit point at result +0x08. 1 = a hit (*hz its z), 0 = none, -1 = no call possible.
static int ht_floor(uint64_t* ctx, const float* p, float up, float depth, float* hz) {
    static float P[3], V[3]; static uint8_t res[0x200]; uint64_t fl = 0; if (rd(g_img + 0x20ad508, &fl, 8)) return -1;
    P[0] = p[0]; P[1] = p[1]; P[2] = p[2] + up; V[0] = 0; V[1] = 0; V[2] = -depth; memset(res, 0, sizeof res); *(uint16_t*)(res + 0x28) = 0xFFFF;
    uint64_t a[8] = {(fl & 0xFFFFFFFF00000000ull) | (uint64_t)((uint32_t)fl | 0x24000u), 0, (uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)V,
                     0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, (uint64_t)(uintptr_t)res};
    int hit = (int)(nh_call(ctx, 0x1fd748, 8, a) & 0xff) != 0; if (hit && hz) *hz = *(float*)(res + 0x10); return hit; }
// halo3's own network layer (its simulation entity table, TLS block +0x240) does not exist in the native app -- the app has
// its own netcode -- but game_globals +0x11 still says 5 (host), so halo3's "mark this object for a network update"
// (halo3+0x1ec3c, called by teleport, disconnect-from-map, vehicle exit ...) reads that NULL table: signal 11 at 0x48
// (2026-11-07). For the call, halo3 is told it runs a local game (1), which skips that step, and host (5) comes back right
// after; the game-state lock is held. NHOOK_HOST_KEEPMODE=1 = no switch (the crash).
static uint64_t ht_call(uint64_t* ctx, uint64_t blk, uint32_t rva, int n, const uint64_t* a) {
    uint64_t gg = *(uint64_t*)(uintptr_t)(blk + 0x48), se = *(uint64_t*)(uintptr_t)(blk + 0x240);
    volatile uint8_t* mode = gg ? (volatile uint8_t*)(uintptr_t)(gg + 0x11) : 0; uint8_t old = mode ? *mode : 0;
    int sw = mode && old == 5 && !se && !getenv("NHOOK_HOST_KEEPMODE"); if (sw) *mode = 1;
    uint64_t r = nh_call(ctx, rva, n, a);
    if (sw) *mode = old; return r; }
static void ht_teleport(uint64_t* ctx, uint64_t blk, uint32_t unit, const float* pos, const float* fwd) {
    static float P[3], F[3], U[3] = {0, 0, 1}; memcpy(P, pos, 12); memcpy(F, fwd, 12);
    uint64_t a[5] = {unit, (uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)F, (uint64_t)(uintptr_t)U, 1}; ht_call(ctx, blk, 0x259410, 5, a); }
static void ht_do(uint64_t* ctx, HtReq* r) {
    uint64_t tls = ctx[0x2d8 / 8], blk = 0; uint32_t idx = 0;
    if (!tls || rd(g_img + 0xa39f9c, &idx, 4) || idx >= 64 || rd(tls + (uint64_t)idx * 8, &blk, 8) || !blk) { ht_say("teleport: no game state on this thread"); return; }
    { uint64_t gg = *(uint64_t*)(uintptr_t)(blk + 0x48); uint8_t st = gg ? *(uint8_t*)(uintptr_t)(gg + 0x11) : 0;
      if (st != 5 && st != 1 && !(g_st_hostpeer >= 0 && g_st_hostpeer == g_st_localpeer)) { ht_say("teleport refused: this game is hosted elsewhere (only the host moves players)"); return; } }
    if (r->kind == 4) {   // do=floorprobe (a diagnostic): "inside the map's open space" at depths around v -- what the floor check sees
        static const float dz[9] = {1.2f, 0.3f, 0.0f, -0.3f, -0.45f, -0.6f, -0.8f, -1.1f, -1.5f}; char o[180]; int k = 0;
        for (int i = 0; i < 9; i++) k += snprintf(o + k, sizeof o - (size_t)k, " %+.2f:%s", dz[i], ht_inside(ctx, r->v, dz[i]) ? "open" : "solid");
        float hz = 0; int h = ht_floor(ctx, r->v, 0.3f, 1.8f, &hz);
        ht_say("floor probe at (%.2f, %.2f, %.2f): ray down %s;%s", r->v[0], r->v[1], r->v[2], h > 0 ? "hit" : h == 0 ? "no floor within 1.5" : "unavailable", o);
        if (h > 0) { size_t L = strlen(g_ht_msg); snprintf(g_ht_msg + L, sizeof g_ht_msg - L, " (floor z %.2f)", hz); } return; }
    int who = r->who >= 0 ? r->who : g_ht_my_slot; if (who < 0) { ht_say("teleport: this app has no player of its own here (a dedicated server): add &slot=<player> to say whom to move"); return; }
    uint32_t me = ht_unit_of(blk, who); int ty = -1;
    uint8_t* mp = ht_datum(blk, me, &ty); if (!mp || ty != 0) { ht_say("teleport: player %d has no body right now", who); return; }
    if (getenv("NHOOK_HOST_PROBE")) { uint64_t a[2] = {me, 0x1003}; uint64_t g = nh_call(ctx, 0x34674c, 2, a); nlog("[nhook] [host] probe: halo3 object lookup of %08x -> 0x%llx, ours 0x%llx", me, (unsigned long long)g, (unsigned long long)(uintptr_t)mp);
        uint64_t gg = *(uint64_t*)(uintptr_t)(blk + 0x48), se = *(uint64_t*)(uintptr_t)(blk + 0x240); uint64_t nop[1] = {0}; uint64_t pb = nh_call(ctx, 0xf01a0, 0, nop);
        nlog("[nhook] [host] probe: game_globals+0x11 %u, TLS+0x240 (simulation entities) 0x%llx, its +0x48 0x%llx, 0xf01a0() -> %llu, object +0xcc %08x",
             gg ? *(uint8_t*)(uintptr_t)(gg + 0x11) : 255, (unsigned long long)se, se ? (unsigned long long)*(uint64_t*)(uintptr_t)(se + 0x48) : 0ull, (unsigned long long)(pb & 0xff), *(uint32_t*)(mp + 0xcc)); if (r->kind == 3 && r->v[0] == 0) return; }
    if ((*(uint32_t*)(mp + 0x110) >> 2) & 1) { ht_say("teleport: that player is dead"); return; }
    if (*(uint32_t*)(mp + 0x10) != 0xFFFFFFFFu || *(int16_t*)(mp + 0x24E) != -1) {   // in a vehicle: out first, the move on a later tick
        static int tries; if (tries++ < 3) { uint64_t a[2] = {me, 0}; ht_call(ctx, blk, 0x3a3c4c, 2, a); pthread_mutex_lock(&g_ht_mx); if (!g_ht_pending) { g_ht_req = *r; g_ht_pending = 1; } pthread_mutex_unlock(&g_ht_mx); ht_say("teleport: leaving the vehicle first"); }
        else { tries = 0; ht_say("teleport: could not leave the vehicle"); } return; }
    float pos[3], fwd[3]; const float* mo = (const float*)(mp + 0x50), * mf = (const float*)(mp + 0x5c);
    if (r->kind == 1) {   // next to another player: right, left, behind, close behind -- inside the structure at knee and head height
        uint32_t tu = ht_unit_of(blk, r->slot); uint8_t* tp = ht_datum(blk, tu, &ty); if (!tp || ty != 0 || tu == me) { ht_say("teleport: player %d has no body right now", r->slot); return; }
        const float* to = (const float*)(tp + 0x50), * tf = (const float*)(tp + 0x5c); float fx = tf[0], fy = tf[1], fl = sqrtf(fx * fx + fy * fy); if (fl < 1e-4f) { fx = 1; fy = 0; } else { fx /= fl; fy /= fl; }
        // A spot also needs a floor: "right of" a player at a ledge was open air -- a player put there fell into Guardian's kill
        // volume (2026-11-07, our server). Floor = halo3's collision ray from 0.3 above the spot hits something within 1.5 below
        // it (ht_floor; "solid below the floor" was no test: open floors and thin walkways have open space under them). The
        // first spot with a floor wins, else the first open one as before. NHOOK_HOST_NOFLOOR=1 = no floor check (old).
        const float cand[4][2] = {{fy, -fx}, {-fy, fx}, {-fx, -fy}, {-0.35f * fx, -0.35f * fy}}; static const char* const cn[4] = {"right", "left", "behind", "close behind"}; int ok = -1, open1 = -1;
        static int nofloor = -1; if (nofloor < 0) nofloor = getenv("NHOOK_HOST_NOFLOOR") != 0; int lg = getenv("NHOOK_HOST_LOG") != 0;
        for (int c = 0; c < 4 && (ok < 0 || lg); c++) { pos[0] = to[0] + cand[c][0]; pos[1] = to[1] + cand[c][1]; pos[2] = to[2] + 0.1f;
            if (fabsf(pos[0]) > 30000 || fabsf(pos[1]) > 30000 || fabsf(pos[2]) > 30000) continue; if (!ht_inside(ctx, pos, 0.3f) || !ht_inside(ctx, pos, 1.2f)) continue;
            float fz = 0; int fh = nofloor ? 1 : ht_floor(ctx, pos, 0.3f, 1.8f, &fz), floor = fh != 0;   // (-1: no ray possible -> as before)
            if (lg) nlog("[nhook] [host] next to: %s (%.2f, %.2f, %.2f) open, floor below: %s (z %.2f)", cn[c], pos[0], pos[1], pos[2], fh > 0 ? "yes" : fh == 0 ? "no" : "not known", fz);
            if (open1 < 0) open1 = c; if (floor && ok < 0) ok = c; }
        if (ok < 0) ok = open1;
        if (ok < 0) { ht_say("teleport: no open spot next to player %d", r->slot); return; }
        pos[0] = to[0] + cand[ok][0]; pos[1] = to[1] + cand[ok][1]; pos[2] = to[2] + 0.1f;
        fwd[0] = to[0] - pos[0]; fwd[1] = to[1] - pos[1]; fwd[2] = 0; float l = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]); if (l > 1e-4f) { fwd[0] /= l; fwd[1] /= l; } else { fwd[0] = fx; fwd[1] = fy; }
        ht_teleport(ctx, blk, me, pos, fwd); ht_say("moved player %d next to player %d (%s) at (%.1f, %.1f, %.1f)", who, r->slot, cn[ok], pos[0], pos[1], pos[2]); return; }
    if (r->kind == 2) memcpy(pos, r->v, 12); else { pos[0] = mo[0] + r->v[0]; pos[1] = mo[1] + r->v[1]; pos[2] = mo[2] + r->v[2]; }
    if (fabsf(pos[0]) > 30000 || fabsf(pos[1]) > 30000 || fabsf(pos[2]) > 30000) { ht_say("teleport: (%.0f, %.0f, %.0f) is too far out", pos[0], pos[1], pos[2]); return; }
    if (!ht_inside(ctx, pos, 0.3f)) { ht_say("teleport: (%.1f, %.1f, %.1f) is outside the map structure", pos[0], pos[1], pos[2]); return; }
    memcpy(fwd, mf, 12); ht_teleport(ctx, blk, me, pos, fwd);
    ht_say("moved player %d to (%.1f, %.1f, %.1f)%s%s", who, pos[0], pos[1], pos[2], r->label[0] ? ": " : "", r->label);
}
// Where a queued request runs (2026-11-07): after halo3's game tick runner (halo3+0xeefa8, a body hook: called once per main-loop
// pass from halo3's main loop 0xb2538, on the engine thread, inside the game-state lock) -- a dedicated server runs only this;
// and, as a fallback, at the entry of the app's per-frame function (0x1029ed000, called from halo3's render loop 0xb19bc: a
// client only). The game-state lock is recursion-aware (a thread that holds it gets token 0; unlock(0) does nothing).
static void ht_on_frame(uint64_t* ctx) {
    if (!__atomic_load_n(&g_ht_pending, __ATOMIC_ACQUIRE)) return;
    HtReq r; pthread_mutex_lock(&g_ht_mx); r = g_ht_req; g_ht_pending = 0; pthread_mutex_unlock(&g_ht_mx);
    static int32_t tok; tok = (int32_t)nh_call(ctx, 0x12c948, 0, 0);          // halo3's game-state lock (what it holds around every tick)
    ht_do(ctx, &r);
    uint64_t a[1] = {(uint64_t)(uintptr_t)&tok}; nh_call(ctx, 0x12c9a0, 1, a); }
static void ht_on_frame_entry(uint64_t* ctx) { g_ht_frames++; ht_on_frame(ctx); }
static void ht_on_tick(uint64_t* a) {   // (body hook: a[0] = the context, a[1] = the guest rsp at the call -- the context's own rsp
    g_ht_ticks++; if (!__atomic_load_n(&g_ht_pending, __ATOMIC_ACQUIRE)) return;   //  slot can be stale inside lifted code)
    uint64_t* ctx = (uint64_t*)(uintptr_t)a[0]; uint64_t sp = ctx[4]; ctx[4] = a[1]; ht_on_frame(ctx); ctx[4] = sp; }
// ---- boundaries: the app's kill-volume / safe-zone test and the player soft-ceiling test, answered while the switch is off
static int ht_kill_test(uint64_t* ctx) {   // halo3+0x1b8e00(object, u8* out_in_safe_zone) -> in a kill volume?
    if (!g_ht_bnd_off) return 0; uint64_t out = ctx[2]; if (out) *(volatile uint8_t*)(uintptr_t)out = 1;
    ctx[0] = 0; ctx[4] += 8; g_ht_bnd_skips++; return 1; }
static int ht_soft_ceiling(uint64_t* ctx) {   // halo3+0x1bac00(biped, position*): a player's soft ceilings
    if (!g_ht_bnd_off) return 0; ctx[0] = 0; ctx[4] += 8; return 1; }
// ---- invisible walls: the shape builder, around
// A body hook (nhook.c body_hook_after): halo3 calls the builder directly from lifted code, which the code-table entry hook
// never saw (0 calls in a live test, 2026-11-07). a[2] = rcx = the shape (also the result), a[5] = r9 -> the BSP reference
// {small*, large*}; the surface index is the 9th argument ([rbp+0x70] in the x86: `movsxd r14,[rbp+0x70]`), which the builder
// stores at shape +0x28 -- read there (a[9], the 8th argument, was the wrong one: no wall was ever hidden, 2026-11-07).
static volatile unsigned long g_ht_inv_calls;
static void ht_shape(uint64_t* a) {
    g_ht_inv_calls++; if (!g_ht_inv_off) return;
    uint64_t shape = a[2], refp = a[5]; int32_t sidx = shape ? *(int32_t*)(uintptr_t)(shape + 0x28) : -1;
    uint64_t tb = 0; if (!shape || !refp || sidx < 0 || rd(g_img + 0x1fcf4c8, &tb, 8) || !tb) return;
    uint64_t small = *(uint64_t*)(uintptr_t)refp, large = *(uint64_t*)(uintptr_t)(refp + 8); int inv = 0;
    if (small) { uint32_t sa = *(uint32_t*)(uintptr_t)(small + 0x40); if (sa) inv = (*(uint8_t*)(uintptr_t)(tb + (uint64_t)sa * 4 + (uint64_t)sidx * 12 + 0xa) & 2) != 0; }
    else if (large) { uint32_t sa = *(uint32_t*)(uintptr_t)(large + 0x40); if (sa) inv = (*(uint8_t*)(uintptr_t)(tb + (uint64_t)sa * 4 + (uint64_t)sidx * 16 + 0xe) & 2) != 0; }
    if (inv) { int n = *(uint8_t*)(uintptr_t)(shape + 0x2e); if (n > 8) n = 8; for (int i = 0; i < n; i++) *(float*)(uintptr_t)(shape + 0x48 + 16 * (uint64_t)i) = -20000.0f; g_ht_inv_hidden++; } }
// ---- installing (on first use, from the HTTP thread)
static int ht_tick_install(void) {
    if (g_ht_tick_ok >= 0) return g_ht_tick_ok; uint64_t f = APP(frame_fn); uint32_t w[4];
    int tick = !getenv("NHOOK_HOST_NOTICK") && !body_hook_after("game tick (host tools)", 0xeefa8, ht_on_tick);
    int frame = !getenv("NHOOK_HOST_NOFRAME") && g_img && f && !rd(f, w, 16) && !memcmp(w, g_ht_frame_words, 16) && !text_hook("frame (host tools)", f, ht_on_frame_entry);
    g_ht_tick_ok = tick || frame;
    if (!g_ht_tick_ok) ht_say("teleport unavailable: neither halo3's game tick nor the app's frame function could be hooked"); return g_ht_tick_ok; }
static int ht_bnd_install(void) {
    if (g_ht_bnd_ok >= 0) return g_ht_bnd_ok;
    uint64_t k = branch_target_of(0x1b8e00), c = branch_target_of(0x1bac00);
    g_ht_bnd_ok = k && c && !text_hook_x("kill volume test", k, (void*)ht_kill_test, 0, 0, 1, 0) && !text_hook_x("soft ceiling test", c, (void*)ht_soft_ceiling, 0, 0, 1, 0);
    if (!g_ht_bnd_ok) ht_say("boundaries switch unavailable: halo3+0x1b8e00 / 0x1bac00 are not the app's own code here"); return g_ht_bnd_ok; }
static int ht_inv_install(void) {
    if (g_ht_inv_ok >= 0) return g_ht_inv_ok;
    g_ht_inv_ok = !body_hook_after("shape builder (invisible walls)", 0x21b4d8, ht_shape);
    if (!g_ht_inv_ok) ht_say("invisible walls switch unavailable: halo3+0x21b4d8 could not be hooked"); return g_ht_inv_ok; }
// ---- speed (the variant's base player trait, as stats.c)
static const char* const g_ht_tl_speed[] = {"Unchanged", "25%", "50%", "75%", "90%", "100%", "110%", "125%", "150%", "200%", "300%"};
static int ht_speed_get(void) { uint64_t blk = tls_block(), gg = 0; uint8_t mode = 0, v = 0;
    if (!blk || !hb_read(blk + 0x48, &gg, 8) || !gg || !hb_read(gg + 0x10, &mode, 1) || mode != 2 || !hb_read(gg + 0x1608 + 0x134 + 0x10, &v, 1)) return -1; return v; }
static int ht_speed_set(int v) { uint64_t blk = tls_block(), gg = 0; uint8_t mode = 0;
    if (v < 0 || v >= 11 || !blk || !hb_read(blk + 0x48, &gg, 8) || !gg || !hb_read(gg + 0x10, &mode, 1) || mode != 2) return 0;
    *(volatile uint8_t*)(uintptr_t)(gg + 0x1608 + 0x134 + 0x10) = (uint8_t)v; ht_say("movement speed for everyone: %s", g_ht_tl_speed[v]); return 1; }
// ---- the map spots past the boundaries (re-tools/h3spots.py -> maptop/<map>.spots.json; stats.c st_dyn_load)
typedef struct { char id[16], label[80]; float x, y, z; } HtSpot;
static HtSpot g_ht_spots[24]; static int g_ht_nspots; static char g_ht_spotmap[96]; static double g_ht_spottry;
static void ht_spots_load(const char* map) {
    if (!strcmp(map, g_ht_spotmap) && (g_ht_nspots || mono_s() - g_ht_spottry < 10)) return;
    snprintf(g_ht_spotmap, sizeof g_ht_spotmap, "%s", map); g_ht_spottry = mono_s(); g_ht_nspots = 0;
    const char* home = getenv("HOME"); char fp[1024]; snprintf(fp, sizeof fp, "%s/Library/Application Support/ReclaimerMac/maptop/%s.spots.json", home ? home : "/tmp", map);
    FILE* f = fopen(fp, "r"); if (!f) return; static char buf[32768]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    const char* p = strstr(buf, "\"spots\""); if (!p) return; const char* end_arr = strchr(p, ']'); if (!end_arr) return;
    while (g_ht_nspots < 24 && (p = strchr(p, '{')) && p < end_arr) { const char* e = strchr(p, '}'); if (!e) break; HtSpot* d = &g_ht_spots[g_ht_nspots]; memset(d, 0, sizeof *d);
        #define HTF(key) ({ const char* q_ = strstr(p, "\"" key "\":"); q_ && q_ < e ? q_ + strlen(key) + 3 : (const char*)0; })
        const char* v; int ok = 1;
        if ((v = HTF("id"))) { v = strchr(v, '"'); if (v) sscanf(v + 1, "%15[^\"]", d->id); } else ok = 0;
        if ((v = HTF("label"))) { v = strchr(v, '"'); if (v) sscanf(v + 1, "%63[^\"]", d->label); } else ok = 0;
        if ((v = HTF("x"))) d->x = strtof(v, 0); else ok = 0; if ((v = HTF("y"))) d->y = strtof(v, 0); else ok = 0; if ((v = HTF("z"))) d->z = strtof(v, 0); else ok = 0;
        { const char* vf = HTF("verified"); if (!(vf && !strncmp(vf + (*vf == ' '), "true", 4))) { size_t L = strlen(d->label); snprintf(d->label + L, sizeof d->label - L, " (not checked yet)"); } }
        #undef HTF
        if (ok && d->id[0]) g_ht_nspots++; p = e + 1; } }
// ---- the snapshot's overlay fields and the page's controls
static void ht_overlay_json(StBuf* s, const char* map) {
    int host = ht_is_host(); ht_spots_load(map);
    sb_put(s, ",\"teleport\":%s,\"host\":%s,\"tp_forward\":false", host && g_ht_tick_ok != 0 ? "true" : "false", host ? "true" : "false");
    if (host) sb_put(s, ",\"bounds_off\":%s,\"invwalls_off\":%s,\"speed_idx\":%d", g_ht_bnd_off ? "true" : "false", g_ht_inv_off ? "true" : "false", ht_speed_get());
    else sb_put(s, ",\"bounds_off\":null,\"invwalls_off\":null,\"speed_idx\":-1");
    sb_put(s, ",\"host_msg\":"); sb_str(s, st_wall() - g_ht_msg_t < 30 ? g_ht_msg : "");
    sb_put(s, ",\"host_stats\":{\"ticks\":%lu,\"frames\":%lu,\"boundary_skips\":%lu,\"shapes_built\":%lu,\"walls_hidden\":%lu}", g_ht_ticks, g_ht_frames, g_ht_bnd_skips, g_ht_inv_calls, g_ht_inv_hidden);
    sb_put(s, ",\"spots\":["); for (int i = 0; i < g_ht_nspots && host; i++) { sb_put(s, "%s{\"id\":", i ? "," : ""); sb_str(s, g_ht_spots[i].id); sb_put(s, ",\"label\":"); sb_str(s, g_ht_spots[i].label);
        sb_put(s, ",\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}", g_ht_spots[i].x, g_ht_spots[i].y, g_ht_spots[i].z); }
    sb_put(s, "]");
}
static float ht_qf(const char* qs, const char* key, float dflt) { char k[16]; snprintf(k, sizeof k, "%s=", key); const char* v = strstr(qs, k); return v ? strtof(v + strlen(k), 0) : dflt; }
static int ht_qi(const char* qs, const char* key, int dflt) { char k[16]; snprintf(k, sizeof k, "%s=", key); const char* v = strstr(qs, k); return v ? atoi(v + strlen(k)) : dflt; }
static int ht_queue(const HtReq* r) { if (!ht_tick_install()) return 0; pthread_mutex_lock(&g_ht_mx); g_ht_req = *r; __atomic_store_n(&g_ht_pending, 1, __ATOMIC_RELEASE); pthread_mutex_unlock(&g_ht_mx); return 1; }
static int ht_control(const char* act, const char* qs) {   // 1 = handled (ok), 0 = refused, -1 = not a host tool
    int tp = !strcmp(act, "tpnext") || !strcmp(act, "tpto") || !strcmp(act, "tpby") || !strcmp(act, "explore") || !strcmp(act, "floorprobe");
    if (!tp && strcmp(act, "bounds") && strcmp(act, "invwalls") && strcmp(act, "speed")) return -1;
    if (!ht_is_host()) { ht_say("%s refused: only the host of a game can do that (this app is a client here)", act); return 0; }
    HtReq r; memset(&r, 0, sizeof r); r.who = -1;
    if (!strcmp(act, "tpnext")) { r.kind = 1; r.slot = ht_qi(qs, "slot", -1); r.who = ht_qi(qs, "who", -1); return r.slot >= 0 && ht_queue(&r); }
    if (!strcmp(act, "tpto")) { r.kind = 2; r.v[0] = ht_qf(qs, "x", NAN); r.v[1] = ht_qf(qs, "y", NAN); r.v[2] = ht_qf(qs, "z", NAN); r.who = ht_qi(qs, "slot", -1);
        return isfinite(r.v[0]) && isfinite(r.v[1]) && isfinite(r.v[2]) && ht_queue(&r); }
    if (!strcmp(act, "floorprobe")) { r.kind = 4; r.v[0] = ht_qf(qs, "x", NAN); r.v[1] = ht_qf(qs, "y", NAN); r.v[2] = ht_qf(qs, "z", NAN);
        return isfinite(r.v[0]) && isfinite(r.v[1]) && isfinite(r.v[2]) && ht_queue(&r); }
    if (!strcmp(act, "tpby")) { r.kind = 3; r.v[0] = ht_qf(qs, "dx", 0); r.v[1] = ht_qf(qs, "dy", 0); r.v[2] = ht_qf(qs, "dz", 0); r.who = ht_qi(qs, "slot", -1); return ht_queue(&r); }
    if (!strcmp(act, "explore")) {   // a spot past the boundaries: the boundaries go off first, then the move
        const char* v = strstr(qs, "spot="); char id[24] = ""; if (v) sscanf(v + 5, "%23[^&]", id);
        for (int i = 0; i < g_ht_nspots; i++) if (!strcmp(id, g_ht_spots[i].id)) { if (ht_bnd_install()) g_ht_bnd_off = 1;
            r.kind = 2; r.v[0] = g_ht_spots[i].x; r.v[1] = g_ht_spots[i].y; r.v[2] = g_ht_spots[i].z + 0.3f; snprintf(r.label, sizeof r.label, "%s", g_ht_spots[i].label); r.who = ht_qi(qs, "slot", -1); return ht_queue(&r); }
        return 0; }
    if (!strcmp(act, "bounds")) { int off = ht_qi(qs, "off", -1); if (off < 0 || !ht_bnd_install()) return 0; g_ht_bnd_off = off ? 1 : 0;
        ht_say("map boundaries %s", off ? "OFF (kill volumes, safe zones and player soft ceilings skipped)" : "back on"); return 1; }
    if (!strcmp(act, "invwalls")) { int off = ht_qi(qs, "off", -1); if (off < 0 || !ht_inv_install()) return 0; g_ht_inv_off = off ? 1 : 0;
        ht_say("invisible walls %s", off ? "OFF (player clip no longer collides)" : "back on"); return 1; }
    if (!strcmp(act, "speed")) return ht_speed_set(ht_qi(qs, "v", -1));
    return 0;
}
