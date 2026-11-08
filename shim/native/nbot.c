// ---- The sparring partner's aim and the walker, natively (backlog N15 / N16, 2026-11-07; the loader's hitbox.c hb_spar_tick
// and nav.c). Included by nstats.c (one compilation unit with nhook.c).
//
// What drives the player: halo3's per-player control record. TLS block +0xC0 holds 4 records of 0x110 bytes; halo3+0xf7354 runs,
// for each local player i, the update 0xf66ac(player, i, ...) that fills record i from the input, then 0xf7178(i, &control)
// that copies it into the unit's control (static RE of halo3.dll, re-tools/tlsref.py 0xc0):
//   +0xC0 unit   +0xC8 buttons (u64: bit 0 crouch, bit 1 jump)   +0xD0 flags   +0xD4 yaw   +0xD8 pitch (radians: the view direction the stick turns)
//   +0xDC / +0xE0 throttle (forward, left; 1 = full)
// In the native app both are the app's OWN code (their code-table entries are a `b` into its text: it replaced halo3's player
// control with its own input), so a text hook at the entry of the app's 0xf7178 (x0 = the guest context: ctx[1] = rcx = the local
// player index) writes record 0's yaw / pitch (the sparring aim: the nearest enemy's head; the walker: the way along its path)
// and throttle (the walker) just before it copies them. (A body hook after a lifted 0xf66ac is the fallback; 0.10.0 has none.) The app's own aim request (aim_at_nearest_player, 0x102734f00)
// writes the same two angles once; they reach the server like a turn of the stick.
//
// The walker drives only where this client's moves count: a local game, or one it hosts (g_srv_addr empty). A JOINED client's
// movement goes to the server from the app's own input layer (its IOHID keyboards and game controllers), not from this record:
// the throttle moved only the client's own prediction and the server pulled it back to where the server kept it (still at its
// spawn: our server's page, 2026-11-07); key events sent to the app's window did not move it either. The aim angles DO reach the
// server (its page showed the partner's aim on the target to 0.1 deg), so the sparring partner on a server aims, standing still.
//
// Only in a TEST client (the loader's rule: in a player's own client the bot would take over their controls): the app must run
// with a test profile (RECLAIMER_PROFILE_DIR, which shim/native.sh sets unless NATIVE_REAL=1 -- the app's test profile also
// refuses public servers by itself), and in a local game or on a server at a loopback / private address. NHOOK_NO_BOT=1: never.
// Switches: NHOOK_SPAR=1 and NHOOK_NAV=roam|enemy|x,y,z|vehicle:<name> at launch, the page's "Sparring aim" button and "Walk"
// list (POST do=spar&on=, do=walk&mode=off|roam|enemy). NHOOK_SPAR_LOG=1 / PELOAD_NAV_LOG=1 log what they do. The walking graph
// is the loader's (re-tools/h3nav.py -> ReclaimerMac/maptop/<map>.nav, made in the background the first time a map is played).
#include <signal.h>
#define NAV_CORE 1
#define NAV_TOOL_PATH_FN nbot_tool_path
static void nbot_tool_path(char* t, size_t n);
#include "../src/nav.c"

static void nbot_tool_path(char* t, size_t n) {   // re-tools/h3nav.py beside the dylib's checkout / bundle (shim/build/nhook.dylib)
    Dl_info di; t[0] = 0; if (!dladdr((void*)nbot_tool_path, &di) || !di.dli_fname) return;
    char d[1024]; snprintf(d, sizeof d, "%s", di.dli_fname); char* sl = strrchr(d, '/'); if (!sl) return; *sl = 0;
    snprintf(t, n, "%s/../../re-tools/h3nav.py", d); }

static volatile int g_bot_spar, g_bot_started;           // the sparring aim on
static float g_bot_pt[3]; static char g_bot_veh[64];    // walker targets (point / vehicle name)
static volatile int g_bot_out;                          // what the hook writes: 1 yaw / pitch, 2 throttle, 4 buttons
static volatile float g_bot_yaw, g_bot_pitch, g_bot_tx, g_bot_ty; static volatile uint64_t g_bot_btn;
static volatile float g_bot_err = -1;                   // the aim's error toward its target (degrees; the page's "aim off by")
static volatile unsigned long g_bot_writes;
static int g_bot_hook = -1;                             // the body hook: -1 not tried, 0 failed, 1 in
static uint64_t g_bot_jump = 0x2;                       // record +0xC8: bit 1 jump, bit 0 crouch (NHOOK_BOT_BTNSCAN, 2026-11-07);
                                                        // NHOOK_BOT_JUMP=<hex mask> another, 0 = no jumping
static int bot_log(void) { static int l = -1; if (l < 0) l = getenv("NHOOK_SPAR_LOG") ? 1 : 0; return l; }

static int bot_private_addr(const char* a) {   // "ip:port" at a loopback / private address
    if (!a[0]) return 1;   // (no server: a local game)
    unsigned b[4]; if (sscanf(a, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4)
        return b[0] == 127 || b[0] == 10 || (b[0] == 192 && b[1] == 168) || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 169 && b[1] == 254);
    return !strncmp(a, "[::1]", 5) || !strncmp(a, "localhost", 9); }
static int bot_allowed(void);
static int bot_walk_ok(void) { return bot_allowed() && !g_srv_addr[0]; }   // (the walker: this client's moves count -- see above)
static int bot_allowed(void) {
    static int t = -1; if (t < 0) { const char* p = getenv("RECLAIMER_PROFILE_DIR"); t = p && *p && !getenv("NHOOK_NO_BOT"); }
    return t && bot_private_addr(g_srv_addr); }

// halo3's control record of local player 0 (engine thread)
static void bot_write(uint32_t idx) {   // (engine thread) record 0, from what bot_step last decided
    if (idx != 0 || g_hc_off[HC_BOT]) return; int o = g_bot_out; if (!o) return;
    uint64_t t0 = hc_now(); uint64_t blk = tls_block(); if (!blk) return; uint8_t* r = *(uint8_t**)(uintptr_t)(blk + 0xC0); if (!r) return;
    if (o & 1) { *(float*)(r + 0xD4) = g_bot_yaw; *(float*)(r + 0xD8) = g_bot_pitch; }
    if (o & 2) { *(float*)(r + 0xDC) = g_bot_tx; *(float*)(r + 0xE0) = g_bot_ty; }
    if (o & 4) *(uint64_t*)(r + 0xC8) |= g_bot_btn;
    g_bot_writes++; hc_add(HC_BOT, t0); }
static void bot_on_reader(uint64_t* ctx) { bot_write((uint32_t)ctx[1]); }   // text hook: the app's 0xf7178(index, &control) entry
static void bot_on_update(uint64_t* a) { bot_write((uint32_t)a[3]); }        // body hook after a lifted 0xf66ac(player, index, ...)
static int bot_hook_install(void) {
    if (g_bot_hook >= 0) return g_bot_hook;
    uint64_t t = branch_target_of(0xf7178);
    g_bot_hook = t ? !text_hook("player control (sparring aim / walker)", t, bot_on_reader) : !body_hook_after("player control (sparring aim / walker)", 0xf66ac, bot_on_update);
    if (!g_bot_hook) nlog("[nhook] [bot] halo3+0xf66ac could not be hooked: no sparring aim / walker"); return g_bot_hook; }

// units (players' bipeds) from the object table: noverlay.c's collector in its units-only mode, under its lock
static int bot_units(NovObj* o, int max) {
    pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 1; int n = nov_collect(o, max); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx);
    return n; }
static int bot_enemy(const NovObj* me, const NovObj* t) {   // the overlay's friend-or-foe (Infection sides, else teams; FFA: everyone)
    if (g_nov_sides_on) { int a = nov_side_of(me->handle), b = nov_side_of(t->handle); if (a >= 0 && b >= 0) return a != b; }
    return me->team < 0 || t->team != me->team; }
static float bot_wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

static void bot_nav_json(StBuf* s) { nav_json(s); }   // the snapshot's "nav" (the page draws the walker's path and goal)

// Diagnostics for re-finding the record's fields after an app update (test clients): NHOOK_BOT_THROTTLE=x,y holds that throttle;
// NHOOK_BOT_BTNSCAN=1 presses record +0xC8 bit k (k = 0..31, one a second, 0.25 s each) -- both log position, velocity and yaw
// every 0.25 s ("[bot] test: ..."): a jump shows as an upward velocity.
static int bot_diag(float* tx, float* ty, uint64_t* btn, double now) {
    static int on = -1; static float fx, fy; static double t0; if (on < 0) { const char* e = getenv("NHOOK_BOT_THROTTLE"); on = 0;
        if (e && sscanf(e, "%f,%f", &fx, &fy) == 2) on |= 1; if (getenv("NHOOK_BOT_BTNSCAN")) on |= 2; }
    if (!on) return 0; if (!t0) t0 = now; int r = 0;
    if (on & 1) { *tx = fx; *ty = fy; r |= 2; }
    if (on & 2) { double t = now - t0 - 3.0; int k = (int)t; if (t >= 0 && k < 32 && t - k < 0.25) { *btn = 1ull << k; r |= 4; } }
    return r; }
static void bot_step(void) {
    double now = mono_s(); int spar = g_bot_spar, mode = bot_walk_ok() ? nav_mode() : 0;
    static int diag = -1; if (diag < 0) diag = getenv("NHOOK_BOT_THROTTLE") || getenv("NHOOK_BOT_BTNSCAN");
    if ((!spar && !mode && !diag) || !bot_allowed() || !g_gameplay_t0 || now - g_gameplay_t0 < 3.0) { g_bot_out = 0; g_nav_act = 0; g_bot_err = -1; return; }
    if (!bot_hook_install()) { g_bot_out = 0; return; }
    if (mode) { nav_ensure(); nav_danger_update(now); }
    uint64_t blk = tls_block(), rec = 0; uint32_t meh = 0xFFFFFFFFu; float yaw0 = 0, pitch0 = 0;
    if (!blk || !hb_read(blk + 0xC0, &rec, 8) || !rec || !hb_read(rec + 0xC0, &meh, 4) || !hb_read(rec + 0xD4, &yaw0, 4) || !hb_read(rec + 0xD8, &pitch0, 4)) { g_bot_out = 0; return; }
    float cam[3], fwd[3]; if (!nov_observer(cam, fwd)) { g_bot_out = 0; return; }
    static NovObj objs[1024]; int n = bot_units(objs, 1024), me = -1;
    for (int i = 0; i < n; i++) if (objs[i].type == 0 && objs[i].handle == meh) { me = i; break; }
    static int32_t path[8192]; static int plen, pidx, goal = -1; static double tplan, tstat, tjump, tback, tstuck[4]; static float bstr; static float hist[64][3]; static double histt[64]; static int hh;
    if (me < 0 || objs[me].dead || objs[me].seat >= 0) { g_bot_out = 0; g_nav_act = 0; plen = 0; g_bot_err = -1; return; }   // dead, respawning or in a vehicle
    const NovObj* o = &objs[me]; int out = 0; float yaw = yaw0, pitch = pitch0;
    // the sparring aim: the nearest live enemy's head (the top of its bounding sphere's core, as the loader aimed), from the camera
    if (spar) {
        float bd = 1e30f; int bi = -1;
        for (int pass = 0; pass < 2 && bi < 0; pass++) for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me) continue;
            if (!pass && !bot_enemy(o, t)) continue; float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; bi = i; } }
        if (bi >= 0) { const NovObj* t = &objs[bi]; float d[3] = {t->c[0] - cam[0], t->c[1] - cam[1], t->c[2] + 0.55f * t->r - cam[2]}; float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
            if (h > 0.01f) { yaw = atan2f(d[1], d[0]); pitch = atan2f(d[2], h); out |= 1;
                // the error against the record's angles (the aim the engine used last tick: the lag behind a moving target). Not
                // against the camera's forward: halo3's first-person camera looks 4.9 deg above the aim the record sets (the app's
                // own aim request writes the same uncorrected pitch and its shots hit: native-aim-test.sh)
                float fh = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]), ey = bot_wrap(yaw - yaw0), ep = pitch - pitch0;
                g_bot_err = sqrtf(ey * ey + ep * ep) * 57.2958f;
                if (bot_log()) { static double tl; if (now - tl > 1.0) { tl = now; nlog("[nhook] [spar] target %08x at %.1f units, aim error %.1f deg (yaw %+.1f, pitch %+.1f; wrote %.1f / %.1f, view %.1f / %.1f)", t->handle, sqrtf(bd), g_bot_err, ey * 57.2958f, ep * 57.2958f,
                    yaw * 57.2958f, pitch * 57.2958f, atan2f(fwd[1], fwd[0]) * 57.2958f, atan2f(fwd[2], fh) * 57.2958f); } } } }
        else g_bot_err = -1; }
    else g_bot_err = -1;
    // the walker (nav.c's graph): the path's way PELOAD_NAV_LOOK nodes ahead; with the aim on, the throttle walks it in any
    // direction relative to the view
    float tx = 0, ty = 0;
    if (mode && g_nav.ok) {
        float pos[3] = {o->c[0], o->c[1], o->c[2] - 0.35f}; int cur = nav_node_at(pos);
        if (cur >= 0) {
            int need = plen == 0 || goal < 0 || now - tplan > (mode == 2 ? 1.0 : 3.0);
            if (goal >= 0 && plen) { float gp[3]; nav_pos(goal, gp); if ((gp[0] - pos[0]) * (gp[0] - pos[0]) + (gp[1] - pos[1]) * (gp[1] - pos[1]) < 1.0f) { nav_say("  [nav] reached the goal\n"); goal = -1; need = 1; } }
            if (mode == 2) { float bd = 1e30f; int bi = -1; for (int pass = 0; pass < 2 && bi < 0; pass++) for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me) continue;
                    if (!pass && !bot_enemy(o, t)) continue; float dx = t->c[0] - pos[0], dy = t->c[1] - pos[1], d = dx * dx + dy * dy; if (d < bd) { bd = d; bi = i; } }
                if (bi >= 0) { float tp[3] = {objs[bi].c[0], objs[bi].c[1], objs[bi].c[2] - 0.35f}; int gn = nav_node_at(tp); if (gn >= 0) goal = gn; } }
            else if (mode == 3 && goal < 0) goal = nav_node_at(g_bot_pt);
            else if (mode == 4 && (goal < 0 || need)) {   // the nearest vehicle whose tag name has g_bot_veh: stand beside it, on our side
                static NovObj veh[2048]; pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 2; int nv = nov_collect(veh, 2048); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx);
                float bd = 1e30f; int bi = -1; for (int i = 0; i < nv; i++) { const NovObj* t = &veh[i]; if ((t->type != 1 && t->type != 12) || t->dead || !nav_ci_has(hb_tag_name(t->def), g_bot_veh)) continue;
                    float dx = t->c[0] - pos[0], dy = t->c[1] - pos[1], dz = t->c[2] - pos[2], d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; bi = i; } }
                if (bi >= 0) { const NovObj* t = &veh[bi]; float dx = t->c[0] - pos[0], dy = t->c[1] - pos[1], d = sqrtf(dx * dx + dy * dy) + 1e-3f, back = t->r + 1.5f;
                    float tp[3] = {t->c[0] - dx / d * back, t->c[1] - dy / d * back, t->c[2]}; int gn = nav_node_at(tp); if (gn >= 0) goal = gn; } }
            else if (mode == 1 && goal < 0) goal = nav_pick_roam(cur);
            if (goal >= 0 && need) { int np = nav_astar(cur, goal, path, 8192); tplan = now;
                if (np < 2) { if (mode != 2) { nav_say("  [nav] no path from node %d to %d: a new goal\n", cur, goal); goal = -1; } plen = 0; }
                else { nav_say("  [nav] path of %d nodes from (%.1f %.1f %.1f) to node %d\n", np, pos[0], pos[1], pos[2], goal); plen = np; pidx = 0; } }
            if (plen >= 2) {
                { float bd = 1e30f; int bi = pidx; for (int i = pidx; i < plen && i < pidx + 60; i++) { float p[3]; nav_pos(path[i], p); float d = (p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]); if (d < bd) { bd = d; bi = i; } }
                  pidx = bi; if (bd > 4.0f) tplan = 0; }   // more than 2 units off the path: replan
                { static double tpub; if (now - tpub > 0.25) { tpub = now; nav_publish(path, pidx, plen, goal); } }
                static int look = -1; if (look < 0) { const char* e = getenv("PELOAD_NAV_LOOK"); look = e ? atoi(e) : 6; if (look < 1) look = 1; }
                int ti = pidx + look < plen ? pidx + look : plen - 1; float tp[3]; nav_pos(path[ti], tp);
                float way = atan2f(tp[1] - pos[1], tp[0] - pos[0]);
                if (!(out & 1)) {   // no aim: turn the view along the path (at most ~360 deg/s, a little downward) and walk forward
                    float e = bot_wrap(way - yaw0), mx = 0.2f; yaw = yaw0 + (e > mx ? mx : e < -mx ? -mx : e); pitch = -0.08f; out |= 1;
                    float rel = bot_wrap(way - yaw); tx = cosf(rel); ty = sinf(rel); if (fabsf(rel) > 1.05f) { tx *= 0.25f; ty *= 0.25f; } }
                else { float rel = bot_wrap(way - yaw); tx = cosf(rel); ty = sinf(rel); }   // walk the way whatever the view
                int strafe = 0;
                if (mode == 2) { static float nearr = -1; if (nearr < 0) { const char* e = getenv("PELOAD_NAV_NEAR"); nearr = e ? atof(e) : 4.0f; } float gp[3]; nav_pos(goal, gp);
                    if ((gp[0] - pos[0]) * (gp[0] - pos[0]) + (gp[1] - pos[1]) * (gp[1] - pos[1]) < nearr * nearr) { float side = fmod(now, 3.0) < 1.5 ? 0.8f : -0.8f; tx = 0; ty = side; strafe = 1; } }   // near: strafe
                // stuck: under 0.35 units in 1.5 s while walking -> back off, jump, raise the cost of the next stretch, replan; four
                // times in 10 s: a new goal. Not while strafing beside the enemy (side to side is no progress by design: the loader's
                // walker took it for stuck and backed off blind -- off a ledge into Guardian's kill volume, 2026-11-07). The back-off
                // walks toward a path node already walked (floor by construction), not straight back.
                if (strafe) hh = 0;
                hist[hh & 63][0] = pos[0]; hist[hh & 63][1] = pos[1]; hist[hh & 63][2] = pos[2]; histt[hh & 63] = now; hh++;
                static float bk[3]; { int j = hh - 1; while (j > 0 && hh - j < 63 && now - histt[(j - 1) & 63] <= 1.5) j--; float dx = pos[0] - hist[j & 63][0], dy = pos[1] - hist[j & 63][1];
                  if (!strafe && now - histt[j & 63] >= 1.4 && dx * dx + dy * dy < 0.35f * 0.35f && now - tjump > 1.5) { tjump = now; hh = 0; tback = now; bstr = (nav_rand() & 1) ? 0.3f : -0.3f;
                      nav_pos(path[pidx >= 4 ? pidx - 4 : 0], bk);
                      for (int i = pidx; i < plen && i < pidx + 10; i++) g_nav.pen[path[i]] += 4.0f;
                      memmove(tstuck, tstuck + 1, 3 * sizeof(double)); tstuck[3] = now;
                      if (now - tstuck[0] < 10.0) { nav_say("  [nav] stuck 4 times at (%.1f %.1f %.1f): a new goal\n", pos[0], pos[1], pos[2]); if (mode != 2) goal = -1; memset(tstuck, 0, sizeof tstuck); }
                      else nav_say("  [nav] stuck at (%.1f %.1f %.1f): back off and replan\n", pos[0], pos[1], pos[2]);
                      tplan = 0; } }
                if (now - tback < 0.6) { float bw = atan2f(bk[1] - pos[1], bk[0] - pos[0]), rel = bot_wrap(bw - yaw);
                    if ((bk[0] - pos[0]) * (bk[0] - pos[0]) + (bk[1] - pos[1]) * (bk[1] - pos[1]) > 0.09f) { tx = cosf(rel) * 0.8f; ty = sinf(rel) * 0.8f + bstr; } else { tx = 0; ty = 0; } }
                out |= 2; g_nav_act = 1;
                if (now - tjump < 0.25 && g_bot_jump) { g_bot_btn = g_bot_jump; out |= 4; }
                if (now - tstat > 2.0) { tstat = now; nav_say("  [nav] at (%.1f %.1f %.1f) node %d, path %d/%d, next (%.1f %.1f %.1f) -> throttle (%.2f, %.2f)\n",
                    pos[0], pos[1], pos[2], cur, pidx, plen, tp[0], tp[1], tp[2], tx, ty); } }
            else g_nav_act = 0; }
        else g_nav_act = 0; }
    if (diag) { uint64_t b = 0; int d = bot_diag(&tx, &ty, &b, now); out |= d; if (d & 4) g_bot_btn = b; else if (!(out & 4)) g_bot_btn = 0;
        static double tl; if (now - tl > 0.25) { tl = now; const float* v = o->vel;
            nlog("[nhook] [bot] test: pos (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) yaw %.1f throttle (%.2f, %.2f) buttons 0x%llx", o->c[0], o->c[1], o->c[2], v[0], v[1], v[2],
                 yaw0 * 57.2958f, tx, ty, (d & 4) ? (unsigned long long)b : 0ull); } }
    g_bot_yaw = yaw; g_bot_pitch = pitch; g_bot_tx = tx; g_bot_ty = ty; g_bot_out = out;
}
static void* bot_thread(void* p) { (void)p; pthread_setname_np("shim-bot"); for (;;) { usleep(33000); bot_step(); } return 0; }
static void bot_start(void) {   // first use: the thread (also from the launch switches)
    if (__atomic_exchange_n(&g_bot_started, 1, __ATOMIC_ACQ_REL)) return; pthread_t th; pthread_create(&th, 0, bot_thread, 0); pthread_detach(th); }
static int bot_parse_nav(const char* v) {   // -> mode (0 off, 1 roam, 2 enemy, 3 point, 4 vehicle), sets the target
    if (!v || !*v || !strcasecmp(v, "off") || !strcmp(v, "0")) return 0;
    if (!strcasecmp(v, "roam")) return 1; if (!strcasecmp(v, "enemy")) return 2;
    if (!strncasecmp(v, "vehicle:", 8)) { snprintf(g_bot_veh, sizeof g_bot_veh, "%s", v + 8); return 4; }
    float p[3]; if (sscanf(v, "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) { memcpy(g_bot_pt, p, 12); return 3; }
    return -1; }
static void bot_init(void) {   // launch switches (nstats_start)
    const char* j = getenv("NHOOK_BOT_JUMP"); if (j) g_bot_jump = strtoull(j, 0, 16);
    if (getenv("NHOOK_SPAR") && strcmp(getenv("NHOOK_SPAR"), "0")) g_bot_spar = 1;
    int m = bot_parse_nav(getenv("NHOOK_NAV")); if (m > 0) g_nav_mode_rt = m; else g_nav_mode_rt = 0;
    if (g_bot_spar || g_nav_mode_rt > 0 || getenv("NHOOK_BOT_THROTTLE") || getenv("NHOOK_BOT_BTNSCAN")) bot_start(); }
static int bot_control(const char* act, const char* qs) {   // POST do=spar&on=0|1, do=walk&mode=off|roam|enemy|x,y,z|vehicle:<name>
    if (!bot_allowed()) return 0;
    if (!strcmp(act, "walk") && !bot_walk_ok()) return 0;   // (a joined client: the server decides its moves)
    if (!strcmp(act, "spar")) { const char* v = strstr(qs, "on="); if (!v) return 0; g_bot_spar = atoi(v + 3) != 0; if (g_bot_spar) bot_start(); return 1; }
    const char* v = strstr(qs, "mode="); if (!v) return 0; char m[96]; size_t k = 0; v += 5; while (*v && *v != '&' && k + 1 < sizeof m) { m[k++] = *v == '+' ? ' ' : *v; v++; } m[k] = 0;
    { char d[96]; size_t a = 0; for (size_t i = 0; m[i] && a + 1 < sizeof d; i++) { if (m[i] == '%' && m[i + 1] && m[i + 2]) { char h[3] = {m[i + 1], m[i + 2], 0}; d[a++] = (char)strtol(h, 0, 16); i += 2; } else d[a++] = m[i]; } d[a] = 0; memcpy(m, d, a + 1); }
    int md = bot_parse_nav(m); if (md < 0) return 0; g_nav_mode_rt = md; if (md) bot_start(); return 1; }
static void bot_overlay_json(StBuf* s) {   // the snapshot's overlay keys for the page's "Sparring aim" button and "Walk" list
    static const char* const mn[5] = {"off", "roam", "enemy", "point", "vehicle"}; int m = g_nav_mode_rt; int ok = bot_allowed();
    sb_put(s, ",\"spar\":%s,\"spar_aim\":%.1f,\"spar_available\":%s,\"walk\":\"%s\",\"walk_available\":%s", g_bot_spar ? "true" : "false",
           g_bot_spar && g_bot_err >= 0 ? (double)g_bot_err : -1.0, ok ? "true" : "false", m >= 0 && m < 5 ? mn[m] : "off", bot_walk_ok() ? "true" : "false"); }

// ---- the page's mic switch (backlog N17, 2026-11-07; the loader's do=mic, wasapi.c): the app records its microphone with an
// AUHAL unit whose input callback pulls the samples with AudioUnitRender(unit, ..., bus 1, ...) -- nhook.c my_AudioUnitRender
// zeroes them while muted, so the app's voice hears silence (nothing goes out, open microphone or talk key). The button shows
// once the app has captured in the last 30 s, or while muted. NHOOK_MIC_MUTED=1 starts muted.
static volatile int g_mic_muted = -1; static volatile double g_mic_t;
static int mic_muted(void) { if (g_mic_muted < 0) g_mic_muted = getenv("NHOOK_MIC_MUTED") && strcmp(getenv("NHOOK_MIC_MUTED"), "0") ? 1 : 0; return g_mic_muted; }
static void mic_json(StBuf* s) { double now = mono_s(), t = g_mic_t; int m = mic_muted();
    sb_put(s, ",\"mic\":\"%s\",\"mic_in_use\":%s", m ? "muted" : t > 0 && now - t < 30.0 ? "live" : "off", t > 0 && now - t < 1.5 ? "true" : "false"); }
