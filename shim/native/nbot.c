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
static volatile float g_bot_yaw, g_bot_pitch, g_bot_tx, g_bot_ty; static volatile uint64_t g_bot_btn; static volatile int g_bot_fire, g_bot_fighting;
static volatile uint64_t g_bot_testbtn; static volatile double g_bot_testbtn_until;   // do=botbtn (button scans, test identities)
static int g_bot_pick_on; static float g_bot_pick_pos[3];   // a weapon on the floor the walker walks to (the fighter's pickups)
// line-of-sight results per enemy (the fighter checks the nearest few in turn and prefers one it can see)
typedef struct { uint32_t h; int res; double t; } LosEnt; static LosEnt g_lost[16];
static int los_get(uint32_t h, double now) { for (int i = 0; i < 16; i++) if (g_lost[i].h == h && now - g_lost[i].t < 0.5) return g_lost[i].res; return -1; }
static void los_put(uint32_t h, int res, double t) { int k = 0; for (int i = 0; i < 16; i++) { if (g_lost[i].h == h) { k = i; break; } if (g_lost[i].t < g_lost[k].t) k = i; } g_lost[k].h = h; g_lost[k].res = res; g_lost[k].t = t; }
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
// the virtual game controller (nvpad.m, NHOOK_VPAD=1, 2026-11-08): with it a JOINED client's moves reach the server too (the app
// sends what its controllers say), so the walker may walk there; it also fires (the right trigger) with NHOOK_BOT_FIRE=1
int nh_vpad_install(void); void nh_vpad_set(float lx, float ly, float rx, float ry, float lt, float rt, uint32_t b); int nh_vpad_reads(void); int nh_vpad_stick_reads(void);
static volatile int g_vpad_on; static int g_pad_written;
static void bot_pad(float lx, float ly, float rt, uint32_t b) {   // b: nvpad.m's button bits; bit 30 = the left trigger (a grenade)
    if (!g_vpad_on) return; nh_vpad_set(lx, ly, 0, 0, (b & 0x40000000u) ? 1.0f : 0.0f, rt, b & 0xFFFFu); g_pad_written = 1; }
static int bot_walk_ok(void) { static int nopkt = -1; if (nopkt < 0) nopkt = getenv("NHOOK_BOT_NOPKT") != 0;   // (the walker: this client's moves count:
    return bot_allowed() && (!g_srv_addr[0] || g_vpad_on || !nopkt); }                                            //  a local game, or the control packet hook)
static int bot_allowed(void) {
    static int t = -1; if (t < 0) { const char* p = getenv("RECLAIMER_PROFILE_DIR"); t = p && *p && !getenv("NHOOK_NO_BOT"); }
    return t && bot_private_addr(g_srv_addr); }

// halo3's control record of local player 0 (engine thread)
// ---- the fighter (2026-11-08, NHOOK_BOT_FIGHT=1; the user: "make 3-4 walkers have a battle ... learn to mimic actual players"):
// the walker's numbers come from real players (shim/tools/walker-model.py over match-recorder.py's recordings: NHOOK_BOT_MODEL,
// default ReclaimerMac/tracks/walker-model.json): the distances they fight at, how they strafe and switch, how they back off on
// low shields, how fast they get on target and how far off they aim. NHOOK_BOT_SKILL=0..1 (default 0.5) scales the reaction
// delay and the aim noise (1 = faster and steadier than the players measured).
typedef struct { float dist_p25, dist_med, dist_p75, strafe_switch, aim_err, reaction, run_speed, strafe_rms; int loaded; } BotModel;
static BotModel g_bm = {5.8f, 12.5f, 15.7f, 1.1f, 4.0f, 1.0f, 2.1f, 0.9f, 0};
static float bm_num(const char* txt, const char* key, float d) { char k[64]; snprintf(k, sizeof k, "\"%s\":", key); const char* p = strstr(txt, k);
    if (!p) return d; p += strlen(k); while (*p == ' ') p++; if (!strncmp(p, "null", 4)) return d; return strtof(p, 0); }
static void bm_load(void) {
    static int done; if (done) return; done = 1; char path[1024]; const char* e = getenv("NHOOK_BOT_MODEL"), * h = getenv("HOME");
    if (e && *e) snprintf(path, sizeof path, "%s", e); else snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/tracks/walker-model.json", h ? h : "");
    FILE* f = fopen(path, "r"); if (!f) { nlog("[nhook] [fight] no model at %s: built-in numbers", path); return; }
    static char txt[1 << 16]; size_t n = fread(txt, 1, sizeof txt - 1, f); fclose(f); txt[n] = 0;
    g_bm.dist_p25 = bm_num(txt, "dist_p25", g_bm.dist_p25); g_bm.dist_med = bm_num(txt, "dist_median", g_bm.dist_med); g_bm.dist_p75 = bm_num(txt, "dist_p75", g_bm.dist_p75);
    g_bm.strafe_switch = bm_num(txt, "strafe_switch_s", g_bm.strafe_switch); g_bm.aim_err = bm_num(txt, "aim_err_median", g_bm.aim_err);
    g_bm.reaction = bm_num(txt, "reaction_s", g_bm.reaction); g_bm.run_speed = bm_num(txt, "speed_median", g_bm.run_speed); g_bm.strafe_rms = bm_num(txt, "strafe_rms", g_bm.strafe_rms);
    g_bm.loaded = 1; nlog("[nhook] [fight] model %s: fights at %.1f..%.1f units, strafe switch %.2f s, aim error %.1f deg, reaction %.2f s",
                          path, g_bm.dist_p25, g_bm.dist_p75, g_bm.strafe_switch, g_bm.aim_err, g_bm.reaction); }
// line of sight: the walker thread asks (eye, target, the two units to ignore), the player-control hook (the engine thread)
// casts halo3's collision ray (nhost.c ht_floor's call, halo3+0x1fd748) and answers; 1 = clear, 0 = blocked
static volatile int g_los_req, g_los_done, g_los_res = -1; static float g_los_p[3], g_los_v[3]; static uint32_t g_los_ign[2], g_los_tgt, g_los_res_tgt; static volatile double g_los_t;
static void bot_los_do(uint64_t* ctx) {
    if (g_los_req == g_los_done) return; int q = g_los_req; static float P[3], V[3]; static uint8_t res[0x200]; uint64_t fl = 0;
    if (!g_img || rd(g_img + 0x20ad508, &fl, 8)) { g_los_done = q; return; }
    memcpy(P, g_los_p, 12); memcpy(V, g_los_v, 12); memset(res, 0, sizeof res); *(uint16_t*)(res + 0x28) = 0xFFFF;
    uint64_t a[8] = {(fl & 0xFFFFFFFF00000000ull) | (uint64_t)((uint32_t)fl | 0x24000u), 0, (uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)V,
                     g_los_ign[0], g_los_ign[1], 0xFFFFFFFFu, (uint64_t)(uintptr_t)res};
    int hit = (int)(nh_call(ctx, 0x1fd748, 8, a) & 0xff) != 0; float L = sqrtf(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]), d = L;
    if (hit) { const float* h = (const float*)(res + 0x08); float dx = h[0] - P[0], dy = h[1] - P[1], dz = h[2] - P[2]; d = sqrtf(dx * dx + dy * dy + dz * dz); }
    g_los_res = !hit || d > L - 0.5f; g_los_res_tgt = g_los_tgt; g_los_t = mono_s(); g_los_done = q; }
typedef struct { float x, y, w; } GlLane; static GlLane* g_gll; static int g_ngll; static float g_gll_wsum;   // the lanes as goal material (nbot_goals.c)
static double g_fjump_until; static int g_bot_camo_hold;   // (a jump in a fight: NHOOK_BOT_FJUMP)
static int bot_fcmp(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
// the model's heat for this map -> g_nav.pop (how much real players use each floor node: A* prefers their lanes); once per map
static void bm_pop(const char* map) {
    static char done[64]; if (!g_nav.ok || !map || !map[0] || !strcmp(done, map)) return; snprintf(done, sizeof done, "%s", map);
    char path[1024]; const char* e = getenv("NHOOK_BOT_MODEL"), * h = getenv("HOME");
    if (e && *e) snprintf(path, sizeof path, "%s", e); else snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/tracks/walker-model.json", h ? h : "");
    FILE* f = fopen(path, "r"); if (!f) return; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n <= 0 || n > (64 << 20)) { fclose(f); return; }
    char* txt = malloc((size_t)n + 1); if (!txt) { fclose(f); return; } size_t got = fread(txt, 1, (size_t)n, f); fclose(f); txt[got] = 0;
    char key[96]; snprintf(key, sizeof key, "\"%s\":", map); char* m = strstr(txt, "\"maps\""); m = m ? strstr(m, key) : 0; char* hp = m ? strstr(m, "\"heat\":") : 0;
    float* pop = calloc((size_t)g_nav.n, sizeof(float)); int cells = 0; float mx = 0; char* q = hp ? strchr(hp, '[') + 1 : 0;
    while (q && *q) { while (*q == ' ' || *q == '\n' || *q == ',') q++; if (*q != '[') break; float x, y, z, c; if (sscanf(q, "[%f,%f,%f,%f]", &x, &y, &z, &c) != 4 && sscanf(q, "[%f, %f, %f, %f]", &x, &y, &z, &c) != 4) break;
        if (z < -9000) {   // a footprint heatmap cell (no height): every floor node under its 2 x 2 units
            for (float ox = -0.75f; ox <= 0.8f; ox += 0.75f) for (float oy = -0.75f; oy <= 0.8f; oy += 0.75f) {
                int cx = (int)floorf((x + ox - g_nav.x0) / g_nav.cell), cy = (int)floorf((y + oy - g_nav.y0) / g_nav.cell); if (cx < 0 || cy < 0 || cx >= g_nav.w || cy >= g_nav.h) continue;
                int cc = cy * g_nav.w + cx; for (uint32_t i = g_nav.first[cc]; i < g_nav.first[cc + 1]; i++) { pop[i] += c / 9.0f; if (pop[i] > mx) mx = pop[i]; } } }
        else { float pz[3] = {x, y, z + 0.5f}; int nd = nav_node_at(pz); if (nd >= 0) { pop[nd] += c; if (pop[nd] > mx) mx = pop[nd]; } }   /* (walker-model.py heat: x, y, z, samples) */
        cells++; q = strchr(q, ']'); if (q) q++; }
    free(txt);
    if (mx > 0) for (int i = 0; i < g_nav.n; i++) pop[i] = pop[i] > 0 ? logf(1 + pop[i]) / logf(1 + mx) : 0;
    // the LANES (nlanes.c, 2026-11-08): how many times players crossed each 0.5-unit cell, from the real heatmaps folder (test runs
    // record into their own; NHOOK_BOT_LANES=<folder>, =0 = heat only): every floor node under a cell gets its on-foot passes,
    // log-scaled to 1 at the 99th percentile; a node keeps the larger of heat and lanes (routes beat places players stand)
    int lcells = 0; { const char* ld = getenv("NHOOK_BOT_LANES"); char lp[1100];
      if (!(ld && !strcmp(ld, "0"))) {
        if (ld && *ld) snprintf(lp, sizeof lp, "%s/%s.lanes", ld, map); else snprintf(lp, sizeof lp, "%s/Library/Application Support/ReclaimerMac/heatmaps/%s.lanes", h ? h : "", map);
        LnGrid lg = {0}; LnSrc* src = 0; uint32_t ns = 0; double since = 0;
        if (ln_read(lp, &lg, &src, &ns, &since) && lg.used) {
            float* lpop = calloc((size_t)g_nav.n, sizeof(float));
            for (uint32_t i = 0; lpop && i < lg.cap; i++) { const LnCell* c = &lg.c[i]; if (!(c->foot > 0)) continue; float x = (c->x + 0.5f) * LN_CELL, y = (c->y + 0.5f) * LN_CELL;
                int cx = (int)floorf((x - g_nav.x0) / g_nav.cell), cy = (int)floorf((y - g_nav.y0) / g_nav.cell); if (cx < 0 || cy < 0 || cx >= g_nav.w || cy >= g_nav.h) continue;
                int cc = cy * g_nav.w + cx; for (uint32_t k = g_nav.first[cc]; k < g_nav.first[cc + 1]; k++) lpop[k] += c->foot; lcells++; }
            { int ng = 0; float* fv = malloc(sizeof(float) * (lg.used + 1)); for (uint32_t i = 0; fv && i < lg.cap; i++) if (lg.c[i].foot > 0) fv[ng++] = lg.c[i].foot;   // (the busier half: goal material)
              float med = 0; if (fv && ng) { qsort(fv, (size_t)ng, sizeof(float), bot_fcmp); med = fv[ng / 2]; } free(fv);
              GlLane* gl2 = malloc(sizeof(GlLane) * (size_t)(ng + 1)); int k3 = 0; float ws2 = 0;
              for (uint32_t i = 0; gl2 && i < lg.cap; i++) if (lg.c[i].foot > med && lg.c[i].foot > 1) { gl2[k3].x = (lg.c[i].x + 0.5f) * LN_CELL; gl2[k3].y = (lg.c[i].y + 0.5f) * LN_CELL; gl2[k3].w = logf(1 + lg.c[i].foot); ws2 += gl2[k3].w; k3++; }
              GlLane* old2 = g_gll; g_gll = gl2; g_ngll = k3; g_gll_wsum = ws2; free(old2); }
            if (lpop) { int nz = 0; for (int i = 0; i < g_nav.n; i++) if (lpop[i] > 0) nz++;
                float* v = nz ? malloc(sizeof(float) * (size_t)nz) : 0; int k2 = 0; for (int i = 0; v && i < g_nav.n; i++) if (lpop[i] > 0) v[k2++] = lpop[i];
                float q99 = 0; if (v && k2) { qsort(v, (size_t)k2, sizeof(float), bot_fcmp); q99 = v[(k2 - 1) * 99 / 100]; }   // (the 99th percentile)
                free(v);
                if (q99 > 0) for (int i = 0; i < g_nav.n; i++) { float lv = lpop[i] > 0 ? fminf(1.0f, logf(1 + lpop[i]) / logf(1 + q99)) : 0; if (lv > pop[i]) pop[i] = lv; }
                free(lpop); }
        }
        ln_free(&lg); free(src); } }
    if (mx <= 0 && !lcells) { free(pop); nlog("[nhook] [fight] no heat or lanes for %s: shortest paths", map); return; }
    float* old = g_nav.pop; g_nav.pop = pop; free(old); nlog("[nhook] [fight] %s: player lanes from %d heat cells and %d lane cells (A* prefers them)", map, cells, lcells); }
static int bot_floor_ok(const float* p) {   // a walkable floor node within ~1 unit (fighting moves leave the path: never off a ledge)
    if (!g_nav.ok) return 1; int n = nav_node_at(p); if (n < 0) return 0; float q[3]; nav_pos(n, q);
    return (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) < 1.44f && fabsf(q[2] - p[2]) < 1.2f; }
static float bot_rand01(void) { return (float)arc4random_uniform(1000000) / 1000000.0f; }   // (per process: walkers differ)
static float bot_gauss(void) { float u = bot_rand01() + 1e-6f, v = bot_rand01(); return sqrtf(-2 * logf(u)) * cosf(6.2831853f * v); }
static void bot_write(uint32_t idx) {   // (engine thread) record 0, from what bot_step last decided
    if (idx != 0 || g_hc_off[HC_BOT]) return; int o = g_bot_out; if (!o) return;
    uint64_t t0 = hc_now(); uint64_t blk = tls_block(); if (!blk) return; uint8_t* r = *(uint8_t**)(uintptr_t)(blk + 0xC0); if (!r) return;
    if (o & 1) { *(float*)(r + 0xD4) = g_bot_yaw; *(float*)(r + 0xD8) = g_bot_pitch; }
    if (o & 2) { *(float*)(r + 0xDC) = g_bot_tx; *(float*)(r + 0xE0) = g_bot_ty; }
    if (o & 4) *(uint64_t*)(r + 0xC8) |= g_bot_btn;
    g_bot_writes++; hc_add(HC_BOT, t0); }
static void bot_on_reader(uint64_t* ctx) { if ((uint32_t)ctx[1] == 0 && g_los_req != g_los_done) bot_los_do(ctx); bot_write((uint32_t)ctx[1]); }   // text hook: the app's 0xf7178(index, &control) entry
static void bot_on_update(uint64_t* a) { bot_write((uint32_t)a[3]); }        // body hook after a lifted 0xf66ac(player, index, ...)
// The control packet (2026-11-08): a client sends the server its controls as packet "A" (net-authority.md section 1), built by
// halo3+0x50fb8(sim player, control record, 1, A out) -- the app's own code -- from the control record: +0x10 / +0x14 yaw / pitch,
// +0x18 / +0x1c throttle forward / left, +8 button bits (A bits 0-2 = bits 0-2, 3 = bit 11, 4 = bit 14), +0x2c 7 more bits. The
// app's input layer refills throttle and buttons from its devices every tick before that, and the reader hook below runs AFTER it:
// so only the aim (which the input layer only nudges) reached the server, a joined walker's moves never did. A text hook BEFORE
// the builder writes the walker's values into the record it is about to pack: the server gets them like a player's input.
// NHOOK_BOT_NOPKT=1 = off (the old: moves local only). NHOOK_BOT_PKTBTN=<hex> ORs bits into record +8 (tests).
static volatile unsigned long g_bot_pkt_n; static volatile uint32_t g_bot_pkt_btn8, g_bot_pkt_btn2c;
static void bot_on_abuild(uint64_t* ctx) {
    uint8_t* act = (uint8_t*)(uintptr_t)ctx[2]; if (!act || !g_bot_out || g_hc_off[HC_BOT]) return; g_bot_pkt_n++;
    { static int told; if (!told) { told = 1; uint64_t blk = tls_block(), rec = 0; if (blk) hb_read(blk + 0xC0, &rec, 8);
        nlog("[nhook] [bot] control packet: the builder's record %p, the reader's record 0x%llx (record +0x%llx)", (void*)act, (unsigned long long)rec, rec ? (unsigned long long)((uint64_t)(uintptr_t)act - rec) : 0ull); } }
    int o = g_bot_out;
    if (o & 1) { *(float*)(act + 0x10) = g_bot_yaw; *(float*)(act + 0x14) = g_bot_pitch; }
    if (o & 2) { *(float*)(act + 0x18) = g_bot_tx; *(float*)(act + 0x1c) = g_bot_ty; }
    static int extra = -1; static uint32_t xb; if (extra < 0) { const char* e = getenv("NHOOK_BOT_PKTBTN"); extra = e != 0; xb = e ? (uint32_t)strtoul(e, 0, 16) : 0; }
    uint32_t b8 = g_bot_pkt_btn8 | (extra ? xb : 0); if (b8) *(uint64_t*)(act + 8) |= b8;
    if (g_bot_pkt_btn2c) *(act + 0x2c) |= (uint8_t)g_bot_pkt_btn2c; }
static int bot_hook_install(void) {
    if (g_bot_hook >= 0) return g_bot_hook;
    { static int done; if (!done && !getenv("NHOOK_BOT_NOPKT")) { done = 1; uint64_t a = branch_target_of(0x50fb8);
        if (!a || text_hook("control packet (walker on a server)", a, bot_on_abuild)) nlog("[nhook] [bot] halo3+0x50fb8 could not be hooked: a joined walker's moves stay local"); } }
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
// the walker's own inventory and power-ups (bot_inv below; the goal draw reads it)
typedef struct { const char* held; const char* other; const char* held_c; const char* other_c; int held_rank, other_rank, has_other, gren, frag, plasma, os, camo; } BotInv;
static BotInv g_inv;
#include "nbot_engage.c"   // the fighter's tactics from the players' fights (engage-model.json: awareness, target choice, movement)
#include "nbot_goals.c"    // radar, memory, own deaths, and where to go (the goal draw)

// Diagnostics for re-finding the record's fields after an app update (test clients): NHOOK_BOT_THROTTLE=x,y holds that throttle;
// NHOOK_BOT_BTNSCAN=1 presses record +0xC8 bit k (k = 0..31, one a second, 0.25 s each) -- both log position, velocity and yaw
// every 0.25 s ("[bot] test: ..."): a jump shows as an upward velocity. NHOOK_BOT_FACE=<yaw deg>[,<pitch deg>] holds that facing
// (2026-11-08: shim/tools/native-aim-test.sh faces a known open direction, so the app's practice target lands on open floor).
static int bot_diag(float* tx, float* ty, uint64_t* btn, double now) {
    static int on = -1; static float fx, fy; static double t0; if (on < 0) { const char* e = getenv("NHOOK_BOT_THROTTLE"); on = 0;
        if (e && sscanf(e, "%f,%f", &fx, &fy) == 2) on |= 1; if (getenv("NHOOK_BOT_BTNSCAN")) on |= 2; if (getenv("NHOOK_BOT_HOLDBTN")) on |= 4; }
    if (!on) return 0; if (!t0) t0 = now; int r = 0;
    if (on & 1) { *tx = fx; *ty = fy; r |= 2; }
    if (on & 2) { double t = now - t0 - 3.0; int k = (int)t; if (t >= 0 && k < 32 && t - k < 0.25) { *btn = 1ull << k; r |= 4; } }
    { static int hold = -1; static uint64_t hb; if (hold < 0) { const char* e = getenv("NHOOK_BOT_HOLDBTN"); hold = e != 0; hb = e ? strtoull(e, 0, 16) : 0; }   // NHOOK_BOT_HOLDBTN=<hex>: held (tests)
      if (hold && now - t0 > 3.0) { *btn |= hb; r |= 4; } }
    return r; }
static void bot_idle(const char* why) {   // NHOOK_SPAR_LOG: why the aim / walker did nothing (every 5 s at most; 2026-11-08, 0.10.1)
    static double tl; static const char* last; double now = mono_s(); if (!bot_log() || (why == last && now - tl < 5)) return; tl = now; last = why; nlog("[nhook] [spar] idle: %s", why); }
// weapons by preference, best first (what the recorded players carried besides the assault rifle -- the Carbine 44% of the time --
// and the power weapons); a weapon not listed ranks below all of these
static int bot_wpn_rank(const char* n) { static const char* const pref[] = {"rocket_launcher", "spartan_laser", "sniper_rifle", "beam_rifle", "battle_rifle", "covenant_carbine",
    "shotgun", "energy_sword", "gravity_hammer", "brute_shot", "assault_rifle", 0}; if (!n) return 99; for (int i = 0; pref[i]; i++) if (strstr(n, pref[i])) return i; return 50; }
// ---- the walker's own inventory and power-ups (2026-11-08; the user: "the walker should be aware of its own inventory, and power up
// status as well"): the held and the other weapon (class + rank), grenades by type, an overshield (shield above 1), active camo
// (+0x360 above 0.05). The fighter switches to the other weapon when the target's distance is outside the held one's band and
// inside the other's (vpad Y), throws only with a grenade in hand, holds fire under camo until close; the goal draw skips power
// weapons with one in hand and goes for fights with an overshield / camo.
static void bot_inv(const NovObj* o, BotInv* v) {
    memset(v, 0, sizeof *v); v->held_c = v->other_c = "none"; v->held_rank = v->other_rank = 99;
    for (int q = 0; q < 4; q++) { uint64_t wd = nov_datum(o->weapons[q]); uint32_t df = 0; if (!wd || !hb_read(wd, &df, 4)) continue; const char* nm = hb_tag_name(df);
        if (q == o->wslot) { v->held = nm; v->held_c = eg_wcls(nm); v->held_rank = bot_wpn_rank(nm); }
        else if (!v->has_other) { v->other = nm; v->other_c = eg_wcls(nm); v->other_rank = bot_wpn_rank(nm); v->has_other = 1; } }
    v->frag = o->gren[0]; v->plasma = o->gren[1]; for (int k = 0; k < 4; k++) if (o->gren[k] > 0) v->gren += o->gren[k];
    v->os = o->shield > 1.05f; v->camo = o->camo > 0.05f; }
static float bot_band_off(const char* cls, float d, float kd) {   // 0 inside the players' band for this weapon class, else how far outside (relative); -1 no band
    float a, b; if (!eg_band(cls, &a, &b)) return -1; a *= kd; b *= kd; return d < a ? (a - d) / a : d > b ? (d - b) / b : 0; }
static void bot_step_body(void);
static void bot_step(void) { g_pad_written = 0; bot_step_body(); if (g_vpad_on && !g_pad_written) nh_vpad_set(0, 0, 0, 0, 0, 0, 0); }   // (idle: the pad let go)
static void bot_step_body(void) {
    double now = mono_s(); int spar = g_bot_spar, mode = bot_walk_ok() ? nav_mode() : 0;
    static int diag0 = -1; if (diag0 < 0) diag0 = getenv("NHOOK_BOT_THROTTLE") || getenv("NHOOK_BOT_BTNSCAN") || getenv("NHOOK_BOT_FACE") || getenv("NHOOK_BOT_HOLDBTN");
    int testb = g_bot_testbtn && now < g_bot_testbtn_until, diag = diag0 || testb;
    if ((!spar && !mode && !diag) || !bot_allowed() || !g_gameplay_t0 || now - g_gameplay_t0 < 3.0) { g_bot_out = 0; g_nav_act = 0; g_bot_err = -1;
        bot_idle(!spar && !mode && !diag ? "nothing on" : !bot_allowed() ? "not allowed here" : !g_gameplay_t0 ? "not in a match" : "match just started"); return; }
    if (!bot_hook_install()) { g_bot_out = 0; return; }
    if (mode) { nav_ensure(); nav_danger_update(now); if (g_nav.ok) bm_pop(g_st_curmap); }
    uint64_t blk = tls_block(), rec = 0; uint32_t meh = 0xFFFFFFFFu; float yaw0 = 0, pitch0 = 0;
    if (!blk || !hb_read(blk + 0xC0, &rec, 8) || !rec || !hb_read(rec + 0xC0, &meh, 4) || !hb_read(rec + 0xD4, &yaw0, 4) || !hb_read(rec + 0xD8, &pitch0, 4)) { g_bot_out = 0; bot_idle("no control record (TLS +0xC0)"); return; }
    float cam[3], fwd[3]; if (!nov_observer(cam, fwd)) { g_bot_out = 0; bot_idle("no observer camera"); return; }
    static NovObj objs[1024]; int n = bot_units(objs, 1024), me = -1;
    for (int i = 0; i < n; i++) if (objs[i].type == 0 && objs[i].handle == meh) { me = i; break; }
    static int32_t path[8192]; static int plen, pidx, goal = -1; static double tplan, tstat, tjump, tback, tstuck[4]; static float bstr; static float hist[64][3]; static double histt[64]; static int hh;
    gl_life(now, me >= 0 && !objs[me].dead, me >= 0 ? objs[me].c : 0, 0, 0);   // (nbot_goals.c: deaths, respawns, failed zones)
    if (me < 0 || objs[me].dead || objs[me].seat >= 0) { g_bot_out = 0; g_nav_act = 0; plen = 0; g_bot_err = -1;   // dead, respawning or in a vehicle
        static char w[96]; snprintf(w, sizeof w, me < 0 ? "the record's unit %08x is none of the %d units seen" : objs[me].dead ? "dead" : "in a vehicle", meh, n); bot_idle(me < 0 ? w : objs[me].dead ? "dead" : "in a vehicle"); return; }
    const NovObj* o = &objs[me]; int out = 0; float yaw = yaw0, pitch = pitch0;
    { static int noinv = -1; if (noinv < 0) noinv = getenv("NHOOK_BOT_NOINV") != 0; if (!noinv) bot_inv(o, &g_inv); else memset(&g_inv, 0, sizeof g_inv);   // (NHOOK_BOT_NOINV=1: blind to it, the old walker)
      if (bot_log()) { static BotInv last; static double tl; if (now - tl > 5.0 || last.held != g_inv.held || last.os != g_inv.os || last.camo != g_inv.camo || last.gren != g_inv.gren) { tl = now; last = g_inv;
          nlog("[nhook] [inv] holding %s (%s), other %s (%s); grenades %d (frag %d, plasma %d)%s%s", g_inv.held ? g_inv.held : "-", g_inv.held_c, g_inv.other ? g_inv.other : "-", g_inv.other_c,
               g_inv.gren, g_inv.frag, g_inv.plasma, g_inv.os ? ", OVERSHIELD" : "", g_inv.camo ? ", CAMO" : ""); } } }
    NovObj ftg; int have_tg = 0; float tdist = 0, tyaw = 0, tpitch = 0;   // the spar target (the fighter's too) and the exact angles to it
    // the sparring aim: the nearest live enemy's head (the top of its bounding sphere's core, as the loader aimed), from the camera
    if (spar) {
        float bd = 1e30f; int bi = -1;
        if (g_bot_fighting && g_eg.on) {   // the players' awareness and choice rule (nbot_engage.c)
            bi = eg_choose(objs, n, me, cam, atan2f(fwd[1], fwd[0]), now, los_get);
            if (bi >= 0) { float dx = objs[bi].c[0] - cam[0], dy = objs[bi].c[1] - cam[1], dz = objs[bi].c[2] - cam[2]; bd = dx * dx + dy * dy + dz * dz; } }
        else for (int pass = 0; pass < 2 && bi < 0; pass++) for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me) continue;
            if (!pass && !bot_enemy(o, t)) continue; float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; bi = i; } }
        if (g_bot_fighting && !g_eg.on && bi >= 0 && los_get(objs[bi].handle, now) != 1) {   // the fighter: the nearest enemy in sight (within 40 units) beats a nearer hidden one
            float vd = 1600.0f; int vi = -1; for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
                float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; if (d < vd && los_get(t->handle, now) == 1) { vd = d; vi = i; } }
            if (vi >= 0) { bi = vi; bd = vd; } }
        if (bi >= 0) { const NovObj* t = &objs[bi]; float d[3] = {t->c[0] - cam[0], t->c[1] - cam[1], t->c[2] + 0.55f * t->r - cam[2]}; float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
            if (h > 0.01f) { yaw = atan2f(d[1], d[0]); pitch = atan2f(d[2], h); out |= 1; ftg = *t; have_tg = 1; tdist = sqrtf(bd); tyaw = yaw; tpitch = pitch;
                // the error against the record's angles (the aim the engine used last tick: the lag behind a moving target). Not
                // against the camera's forward: halo3's first-person camera looks 4.9 deg above the aim the record sets (the app's
                // own aim request writes the same uncorrected pitch and its shots hit: native-aim-test.sh)
                float fh = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]), ey = bot_wrap(yaw - yaw0), ep = pitch - pitch0;
                g_bot_err = sqrtf(ey * ey + ep * ep) * 57.2958f;
                { static int fire = -1; static float fdeg = 3, frange = 35; if (fire < 0) { const char* e = getenv("NHOOK_BOT_FIRE"); fire = e && strcmp(e, "0"); if (getenv("NHOOK_BOT_FIRE_DEG")) fdeg = strtof(getenv("NHOOK_BOT_FIRE_DEG"), 0); if (getenv("NHOOK_BOT_FIRE_RANGE")) frange = strtof(getenv("NHOOK_BOT_FIRE_RANGE"), 0); }
                  g_bot_fire = fire && g_bot_err < fdeg && sqrtf(bd) < frange; }
                if (bot_log()) { static double tl; if (now - tl > 1.0) { tl = now; nlog("[nhook] [spar] target %08x at %.1f units, aim error %.1f deg (yaw %+.1f, pitch %+.1f; wrote %.1f / %.1f, view %.1f / %.1f)", t->handle, sqrtf(bd), g_bot_err, ey * 57.2958f, ep * 57.2958f,
                    yaw * 57.2958f, pitch * 57.2958f, atan2f(fwd[1], fwd[0]) * 57.2958f, atan2f(fwd[2], fh) * 57.2958f); } } } }
        else g_bot_err = -1; }
    else g_bot_err = -1;
    // ---- the fighter (NHOOK_BOT_FIGHT=1): aim with a reaction delay, a turn-rate limit and drifting noise; fire only with a clear
    // line; in reach keep the players' fighting distance, strafe with their switch timing, back off on low shields; grenades at mid
    // range, melee up close. Out of sight / reach the walker below hunts (NHOOK_NAV=enemy).
    // tunables (shim/tools/walker-train.py evolves them): NHOOK_BOT_SKILL 0..1 (reaction + aim noise), NHOOK_BOT_DIST (x the players'
    // fighting distances), NHOOK_BOT_STRAFE (x strafe strength), NHOOK_BOT_SWITCH (x the strafe switch time), NHOOK_BOT_FIRE_EXTRA
    // (deg of aim slack when firing), NHOOK_BOT_RETREAT (back off below this shield), NHOOK_BOT_GREN (s between grenades)
    static int fight = -1; static float skill = 0.5f, kdist = 1, kstrafe = 1, kswitch = 1, fextra = 1, retreat = 0.35f, gren = 9, backoff = 0.15f, press = 0.4f;
    if (fight < 0) { const char* e = getenv("NHOOK_BOT_FIGHT"); fight = e && strcmp(e, "0");
        #define BKNOB(v, n) if (getenv(n)) v = strtof(getenv(n), 0)
        BKNOB(skill, "NHOOK_BOT_SKILL"); BKNOB(kdist, "NHOOK_BOT_DIST"); BKNOB(kstrafe, "NHOOK_BOT_STRAFE"); BKNOB(kswitch, "NHOOK_BOT_SWITCH");
        BKNOB(fextra, "NHOOK_BOT_FIRE_EXTRA"); BKNOB(retreat, "NHOOK_BOT_RETREAT"); BKNOB(gren, "NHOOK_BOT_GREN"); BKNOB(backoff, "NHOOK_BOT_BACKOFF"); BKNOB(press, "NHOOK_BOT_PRESS");
        #undef BKNOB
        if (fight) { bm_load(); eg_load(); nlog("[nhook] [fight] on: skill %.2f, distance x%.2f, strafe x%.2f, switch x%.2f, fire slack %.1f deg, back off below shield %.2f (%.2f), press %.2f, grenades every %.0f s",
                                     skill, kdist, kstrafe, kswitch, fextra, retreat, backoff, press, gren); } }
    int engaged = 0; float ftx = 0, fty = 0; uint32_t fbtn = 0; g_bot_fighting = fight && spar;
    if (fight) eg_load_for(g_st_curmap, g_trk_mode);   // (the models for this map and game: reloaded when either changes)
    if (fight && spar && have_tg) {
        static uint32_t cur; static double tacq, tvis, tswitch, tmelee, tgren, tlos, tlast; static float ox, oy, side = 1;
        double dt = tlast ? now - tlast : 0.033; if (dt > 0.2) dt = 0.2; tlast = now;
        if (ftg.handle != cur) { cur = ftg.handle; tacq = now; tvis = 0;
            ox = (bot_rand01() - 0.5f) * 0.15f; oy = (bot_rand01() - 0.5f) * 0.08f;
            if (g_eg_fast && g_eg_fast == cur) { tacq = now - 30; g_eg_fast = 0; } }   // (a second attacker: the decision delay was its reaction -- turn at full speed)
        { static double tput; if (g_los_t > tput && g_los_req == g_los_done) { tput = g_los_t; los_put(g_los_res_tgt, g_los_res, g_los_t); } }   // (the last answer -> the cache)
        if (now - tlos > 0.07 && g_los_req == g_los_done) { tlos = now;   // ask for the line to one enemy's chest: the target every other time, else the next of the 3 nearest
            static int rr; const NovObj* q = &ftg; rr++;
            if (rr & 1) { int cand[3] = {-1, -1, -1}; float cd[3] = {1e30f, 1e30f, 1e30f};
                for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t) || t->handle == ftg.handle) continue;
                    float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz;
                    for (int k = 0; k < 3; k++) if (d < cd[k]) { for (int m = 2; m > k; m--) { cd[m] = cd[m - 1]; cand[m] = cand[m - 1]; } cd[k] = d; cand[k] = i; break; } }
                int k = (rr >> 1) % 3; if (cand[k] >= 0 && cd[k] < 1600.0f) q = &objs[cand[k]]; }
            g_los_p[0] = cam[0]; g_los_p[1] = cam[1]; g_los_p[2] = cam[2]; g_los_v[0] = q->c[0] - cam[0]; g_los_v[1] = q->c[1] - cam[1]; g_los_v[2] = q->c[2] + 0.3f * q->r - cam[2];
            g_los_ign[0] = meh; g_los_ign[1] = q->handle; g_los_tgt = q->handle; g_los_req++; }
        int vis = los_get(ftg.handle, now); if (vis < 0) vis = tdist < 6 ? 1 : 0;
        if (vis) tvis = now;
        int seen = tvis && now - tvis < 0.6;
        // (2026-11-08 walker-compare: walkers reacted in 0.42 s and aimed 2.8 deg off against the players' 1.08 s and 4.1 deg: the
        //  factors below bring skill 0.5 to the players' numbers -- skill 1 is still faster and steadier than they are)
        static float rk = -1, t0r = -1; if (rk < 0) { const char* e = getenv("NHOOK_BOT_REACT"); rk = e ? (float)atof(e) : 1.6f; e = getenv("NHOOK_BOT_TURN0"); t0r = e ? (float)atof(e) : 0.73f; }
        // (07:50: walker-model's "reaction" = 30 deg -> 5 deg of aim, so the turn rate BEFORE the reaction time decides it: 1.5 rad/s
        //  measured 0.49-0.53 s against the players' 1.08 whatever the delay; NHOOK_BOT_TURN0 = that rate, NHOOK_BOT_REACT the delay factor)
        // NHOOK_BOT_NOISE: x the aim noise (2026-11-08 12:15: on Guardian the players' horizontal aim error while attacking is 6.8 deg,
        // the walkers' 3.8 -- lag and moving targets the walkers do not have; engage-compare.py)
        static float nk = -1; if (nk < 0) { const char* e = getenv("NHOOK_BOT_NOISE"); nk = e ? strtof(e, 0) : 1.0f; }
        float react = g_bm.reaction * (1.25f - skill) * rk, noise = g_bm.aim_err * (1.25f - skill) * 1.6f * nk / 57.2958f;
        if (seen) {
            // aim: OU noise around the exact angles, turning at most 1.5 rad/s before the reaction time is up, 7 rad/s after
            float th = 0.45f, k = (float)dt / th, sg = noise * sqrtf(2.0f * (float)dt / th);
            ox += -ox * k + sg * bot_gauss(); oy += -oy * k + 0.5f * sg * bot_gauss();
            float dy = bot_wrap(tyaw + ox - yaw0), dp = (tpitch + oy) - pitch0, mx = (float)dt * (now - tacq < react ? t0r : 7.0f);
            yaw = yaw0 + (dy > mx ? mx : dy < -mx ? -mx : dy); pitch = pitch0 + (dp > mx ? mx : dp < -mx ? -mx : dp); out |= 1;
            float ey = bot_wrap(tyaw - yaw0), ep = tpitch - pitch0, err = sqrtf(ey * ey + ep * ep) * 57.2958f, size = atanf(0.4f / (tdist > 0.5f ? tdist : 0.5f)) * 57.2958f;
            g_bot_err = err; g_bot_fire = vis && now - tacq > react && err < (size > 1.2f ? size : 1.2f) + fextra && tdist < 40;
            { static float mv = -1; static double thurt; float v2 = o->shield + o->health; if (mv >= 0 && v2 < mv - 0.02f) thurt = now; mv = v2;
              // under active camo: no shot beyond the near edge of the weapon's band (+2 units) unless we were hit in the last 2 s --
              // firing gives camo away; walk in instead (below)
              float a = 0, b = 0; if (g_inv.camo && now - thurt > 2.0 && eg_band(g_inv.held_c, &a, &b) && tdist > a * kdist + 2.0f) { g_bot_fire = 0; g_bot_camo_hold = 1; } else g_bot_camo_hold = 0; }
            // the other weapon when the target is outside the held one's band and well inside the other's (vpad Y held 0.15 s,
            // 3 s between switches; not in the first 0.3 s of a target)
            { static double tsw; if (g_vpad_on && g_inv.has_other && now - tsw > 3.0 && now - tacq > 0.3) {
                  float oh = bot_band_off(g_inv.held_c, tdist, kdist), oo = bot_band_off(g_inv.other_c, tdist, kdist);
                  if (oh > 0.3f && oo >= 0 && oo < oh * 0.5f) { tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s (%s) for %.1f units (the %s band is %.0f%% off)", g_inv.other, g_inv.other_c, tdist, g_inv.held_c, oh * 100); } }
              if (now - tsw < 0.15) fbtn |= 8; }
            if (tdist < 1.8f && now - tmelee > 0.7) { tmelee = now; fbtn |= 2; }                                            // B: melee
            { static float fj = -1; if (fj < 0) { const char* e = getenv("NHOOK_BOT_FJUMP"); fj = e ? strtof(e, 0) : 0.0f; }   // jumps a minute in a fight (walker-coach.py: the winners')
              if (fj > 0 && bot_rand01() < fj * (float)dt / 60.0f) g_fjump_until = now + 0.25; }
            { float gd0 = 6, gd1 = 16; static int gset = -1; static double gnext; if (gset < 0) gset = getenv("NHOOK_BOT_GREN") != 0;   // (the players' band and rate unless NHOOK_BOT_GREN is set)
              int pl = g_eg.on && g_eg.gren_ok; if (pl) { gd0 = g_eg.gren_d0; gd1 = g_eg.gren_d1; }
              // the next throw's time is drawn ONCE per throw (it was drawn every tick, so the shortest draw won: throws came early)
              static float gx = -1; if (gx < 0) { const char* e = getenv("NHOOK_BOT_GRENX"); gx = e ? strtof(e, 0) : 1.0f; }   // (walker-coach.py: x the time between grenades)
              if (!gnext) gnext = now + gx * (pl && !gset ? g_eg.gren_every * (0.5f + bot_rand01()) : gren + 6 * bot_rand01());
              if ((pl || gren > 0) && (g_inv.gren > 0 || !g_inv.held) && tdist > gd0 && tdist < gd1 && vis && now > gnext && now - tacq > react) { tgren = now;
                  gnext = now + gx * (pl && !gset ? g_eg.gren_every * (0.5f + bot_rand01()) : gren + 6 * bot_rand01()); } }
            if (now - tgren < 0.15) fbtn |= 0x10000;                                                                       // (left trigger: a grenade)
            if (tdist < 30) { engaged = 1;
                // closing in: the players pressed in at ~0.8 units/s with shields up and only stopped (-0.03) on low shields; the walkers
                // had backed off hard (-0.37): NHOOK_BOT_PRESS (throttle inside the distance band), NHOOK_BOT_BACKOFF (on low shields / too close)
                // (engage-model.json: the band players attack from with this weapon class, and pieces of their real fights as the moves)
                float p25 = g_bm.dist_p25, p75 = g_bm.dist_p75, run = g_bm.run_speed > 0.5f ? g_bm.run_speed : 2.1f, el = 0, er = 0; const char* wc = "none";
                if (g_eg.on) { uint64_t wd = (o->wslot >= 0 && o->wslot < 4) ? nov_datum(o->weapons[o->wslot]) : 0; uint32_t wdef = 0;
                    if (wd && hb_read(wd, &wdef, 4)) wc = eg_wcls(hb_tag_name(wdef)); float a2, b2; if (eg_band(wc, &a2, &b2)) { p25 = a2; p75 = b2; } }
                float r = tdist > p75 * kdist ? 0.9f : tdist < p25 * kdist ? -backoff - 0.2f : press; if (o->shield < retreat && o->shield >= 0) r = -backoff;
                if (g_bot_camo_hold) r = 0.9f;   // (camo: walk in to the near edge of the band before the first shot)
                if (now > tswitch) { side = -side; tswitch = now + g_bm.strafe_switch * kswitch * (0.6f + 0.8f * bot_rand01()); }
                float lat = side * fminf(1.0f, kstrafe * (0.25f + g_bm.strafe_rms / run));
                if (g_eg.on && eg_move(wc, tdist, now, &el, &er)) {   // a real player's moves: across as recorded, toward / away pulled into the band
                    static float lsc = -1; if (lsc < 0) { const char* e = getenv("NHOOK_BOT_LIBSCALE"); lsc = e ? strtof(e, 0) : 1.0f; }   // (the players' own moves: not the tactics' strafe scale)
                    lat = fmaxf(-1.0f, fminf(1.0f, lsc * el / run));
                    // pulled in when beyond the band, never pushed back inside it: the players close in at 1.3 units/s with shields up
                    // on Guardian (engage-compare.py; the push-back made the walkers hover at the band's near edge)
                    static float rsc = -1; if (rsc < 0) { const char* e = getenv("NHOOK_BOT_RADSCALE"); rsc = e ? strtof(e, 0) : 1.0f; }   // (walker-coach.py: x the replayed closing / backing off)
                    float pull = tdist > p75 * kdist ? 0.6f : 0.0f; r = fmaxf(-1.0f, fminf(1.0f, rsc * er / run + pull));
                    if (o->shield < retreat && o->shield >= 0) r = -backoff;
                    if (g_bot_camo_hold) r = 0.9f; }
                // never off the floor: test where this step leads (1 unit along the move); flip the strafe, then drop the back-off
                float fx = cosf(yaw0), fy = sinf(yaw0), lx = -fy, ly = fx;
                for (int tries = 0; tries < 3; tries++) { float p2[3] = {o->c[0] + (fx * r + lx * lat) * 1.2f, o->c[1] + (fy * r + ly * lat) * 1.2f, o->c[2] - 0.35f};
                    if (bot_floor_ok(p2)) break; if (tries == 0) { side = -side; lat = -lat; } else if (tries == 1) r = r < 0 ? 0 : r; else { lat = 0; r = 0; } }
                ftx = r; fty = lat; } }
        else { g_bot_fire = 0; if (tdist > 8) { yaw = yaw0; pitch = pitch0; out &= ~1; } }   // (out of sight: no tracking through walls -- look where it walks)
        if (bot_log()) { static double tl; if (now - tl > 1.0) { tl = now; nlog("[nhook] [fight] target %08x %.1f u %s, err %.1f deg, %s%s, shield %.2f", ftg.handle, tdist, vis ? "in sight" : "hidden", g_bot_err, engaged ? "engaged" : "hunting", g_bot_fire ? ", FIRING" : "", o->shield); } }
    }
    // ---- pickups (NHOOK_BOT_PICKBIT=<hex>: the control record's pick-up bit, shim/tools/native-pickup-scan.sh finds it): out of a
    // fight, a better weapon (bot_wpn_rank) lying within 12 units and 2.5 of height is walked to and picked up (the bit held 0.6 s);
    // each weapon is tried once
    { static int pb = -1; static uint64_t pbit; if (pb < 0) { const char* e = getenv("NHOOK_BOT_PICKBIT"); pbit = e ? strtoull(e, 0, 16) : 0; pb = pbit != 0; }
      static double tscan, tpress; static uint32_t tried[32]; static int ntried; static uint32_t want_h;
      if (pb == 0 && g_vpad_on) pb = 2;   // (on the virtual controller: hold its pick-up button, NHOOK_BOT_PICKPAD, default RB)
      if (pb && fight && !engaged && now - tscan > 1.0) { tscan = now; static NovObj w[512]; pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 3; int nw = nov_collect(w, 512); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx);
          int cur = 99; uint32_t held = o->wslot >= 0 && o->wslot < 4 ? o->weapons[o->wslot] : 0xFFFFFFFFu;
          for (int i = 0; i < nw; i++) if (w[i].handle == held) { cur = bot_wpn_rank(hb_tag_name(w[i].def)); break; }
          float bd = 144.0f; int bi = -1;
          for (int i = 0; i < nw; i++) { if (w[i].parent != 0xFFFFFFFFu) continue; int rk = bot_wpn_rank(hb_tag_name(w[i].def)); if (rk >= cur) continue;
              int t2 = 0; for (int k = 0; k < ntried; k++) if (tried[k] == w[i].handle) t2 = 1; if (t2) continue;
              float dx = w[i].c[0] - o->c[0], dy = w[i].c[1] - o->c[1], dz = w[i].c[2] - o->c[2], d = dx * dx + dy * dy; if (d < bd && fabsf(dz) < 2.5f) { bd = d; bi = i; } }
          if (bi >= 0) { g_bot_pick_on = 1; memcpy(g_bot_pick_pos, w[bi].c, 12); want_h = w[bi].handle; if (bot_log()) nlog("[nhook] [fight] going for %s (%.1f units)", hb_tag_name(w[bi].def), sqrtf(bd)); }
          else g_bot_pick_on = 0; }
      if (engaged) g_bot_pick_on = 0;
      if (g_bot_pick_on) { float dx = g_bot_pick_pos[0] - o->c[0], dy = g_bot_pick_pos[1] - o->c[1];
          if (dx * dx + dy * dy < 0.8f) { tpress = now; g_bot_pick_on = 0; if (ntried < 32) tried[ntried++] = want_h; else tried[(ntried++) & 31] = want_h; } }
      static uint32_t ppad = 1; if (ppad == 1) { const char* e = getenv("NHOOK_BOT_PICKPAD"); ppad = e ? (uint32_t)strtoul(e, 0, 16) : 0x20; }
      if (pb && now - tpress < 0.6) fbtn |= 0x20000 | (g_vpad_on ? ppad : 0); }   // (fbtn bit 17: hold the pick-up bit; mapped below; the pad's button directly)
    // reload after a fight (the players' habit; no ammo count is read yet): the pad's reload button (NHOOK_BOT_PICKPAD, a TAP; a hold
    // picks up) 0.4-1.0 s after a fight in which the walker fired ends, unless it is walking to a weapon
    { static double tend, trel, tfired; static int was; if (g_bot_fire) tfired = now;
      if (was && !engaged) tend = now + 0.4 + 0.6 * bot_rand01(); if (engaged) tend = 0; was = engaged;
      if (fight && g_vpad_on && tend && now > tend) { tend = 0; if (now - tfired < 8.0 && !g_bot_pick_on) { trel = now; if (bot_log()) nlog("[nhook] [inv] reloading after the fight"); } }
      static uint32_t rpad = 1; if (rpad == 1) { const char* e = getenv("NHOOK_BOT_PICKPAD"); rpad = e ? (uint32_t)strtoul(e, 0, 16) : 0x20; }
      if (now - trel < 0.1) fbtn |= rpad; }
    // the walker (nav.c's graph): the path's way PELOAD_NAV_LOOK nodes ahead; with the aim on, the throttle walks it in any
    // direction relative to the view
    float tx = 0, ty = 0;
    if (mode && g_nav.ok) {
        float pos[3] = {o->c[0], o->c[1], o->c[2] - 0.35f}; int cur = nav_node_at(pos);
        if (cur >= 0) {
            int need = plen == 0 || goal < 0 || now - tplan > (mode == 2 ? 1.0 : 3.0);
            if (goal >= 0 && plen) { float gp[3]; nav_pos(goal, gp); if ((gp[0] - pos[0]) * (gp[0] - pos[0]) + (gp[1] - pos[1]) * (gp[1] - pos[1]) < 1.0f) { nav_say("  [nav] reached the goal\n"); goal = -1; need = 1; } }
            if (mode == 2 && g_bot_pick_on) { int gn = nav_node_at(g_bot_pick_pos); if (gn >= 0) goal = gn; }   // (the fighter's pickup: walk to the weapon)
            else if (mode == 2 && g_eg.on && gl_on()) {   // the goal draw (nbot_goals.c): known enemies, revenge, items, lanes -- never none
                if (gl_tick(now, objs, n, me, los_get)) { float gp[3] = {g_gl.pos[0], g_gl.pos[1], g_gl.kind == GK_ENEMY || g_gl.kind == GK_REVENGE || g_gl.kind == GK_ITEM || g_gl.kind == GK_MATE || g_gl.kind == GK_AID ? g_gl.pos[2] - 0.35f : pos[2]};
                    int gn = nav_node_at(gp); if (gn < 0) { gl_drop(now, "no floor there"); } else if (gn != goal) { goal = gn; need = 1; } } }
            else if (mode == 2) { float bd = 1e30f; int bi = -1; for (int pass = 0; pass < 2 && bi < 0; pass++) for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me) continue;
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
                if (np < 2) { if (mode != 2) { nav_say("  [nav] no path from node %d to %d: a new goal\n", cur, goal); goal = -1; }
                              else if (g_eg.on && gl_on()) { gl_drop(now, "no path"); goal = -1; }   // (the goal draw: another at once -- it used to stand there)
                              plen = 0; }
                else { nav_say("  [nav] path of %d nodes from (%.1f %.1f %.1f) to node %d\n", np, pos[0], pos[1], pos[2], goal); plen = np; pidx = 0; } }
            if (plen >= 2) {
                { float bd = 1e30f; int bi = pidx; for (int i = pidx; i < plen && i < pidx + 60; i++) { float p[3]; nav_pos(path[i], p); float d = (p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]); if (d < bd) { bd = d; bi = i; } }
                  pidx = bi; if (bd > 4.0f) tplan = 0; }   // more than 2 units off the path: replan
                { static double tpub; if (now - tpub > 0.25) { tpub = now; nav_publish(path, pidx, plen, goal); } }
                static int look = -1; if (look < 0) { const char* e = getenv("PELOAD_NAV_LOOK"); look = e ? atoi(e) : 6; if (look < 1) look = 1; }
                int ti = pidx + look < plen ? pidx + look : plen - 1; float tp[3]; nav_pos(path[ti], tp);
                float way = atan2f(tp[1] - pos[1], tp[0] - pos[0]);
                float rlook; if (!(out & 1) && g_eg.on && gl_on() && gl_radar_look(now, objs, n, me, have_tg && engaged, &rlook)) {   // (a radar contact: look at it, keep walking the path)
                    float e = bot_wrap(rlook - yaw0), mx = 0.2f; yaw = yaw0 + (e > mx ? mx : e < -mx ? -mx : e); pitch = -0.05f; out |= 1; }
                if (!(out & 1)) {   // no aim: turn the view along the path (at most ~360 deg/s, a little downward) and walk forward
                    float e = bot_wrap(way - yaw0), mx = 0.2f; yaw = yaw0 + (e > mx ? mx : e < -mx ? -mx : e); pitch = -0.08f; out |= 1;
                    float rel = bot_wrap(way - yaw); tx = cosf(rel); ty = sinf(rel); if (fabsf(rel) > 1.05f) { tx *= 0.25f; ty *= 0.25f; } }
                else { float rel = bot_wrap(way - yaw); tx = cosf(rel); ty = sinf(rel); }   // walk the way whatever the view
                int strafe = 0;
                if (mode == 2 && !g_bot_fighting) { static float nearr = -1; if (nearr < 0) { const char* e = getenv("PELOAD_NAV_NEAR"); nearr = e ? atof(e) : 4.0f; } float gp[3]; nav_pos(goal, gp);
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
    if (engaged) { tx = ftx; ty = fty; out |= 2; }   // (the fighter's moves in reach override the walker's path)
    if (testb) { g_bot_btn = ((out & 4) ? g_bot_btn : 0) | g_bot_testbtn; out |= 4; }   // (do=botbtn: held until its time is up)
    // fire: the control record's bit 19 (+0xC8) -- the local unit fires, and the control packet carries the weapon's firing state
    // to the server (halo3+0x5452c reads it from the local weapon: a packet bit of its own does nothing; 2026-11-08 scan: bits 19
    // and 21 fire, 1 jumps). NHOOK_BOT_FIREBIT=<hex> = another bit.
    { static uint64_t fb = 0; if (!fb) { const char* e = getenv("NHOOK_BOT_FIREBIT"); fb = e ? strtoull(e, 0, 16) : 0x80000ull; }
      static uint64_t gb = 0; if (!gb) { const char* e = getenv("NHOOK_BOT_GRENBIT"); gb = e ? strtoull(e, 0, 16) : 0x40000000ull; }
      static uint64_t pkb = 1; if (pkb == 1) { const char* e = getenv("NHOOK_BOT_PICKBIT"); pkb = e ? strtoull(e, 0, 16) : 0; }
      uint64_t want = (spar && g_bot_fire ? fb : 0) | ((fbtn & 0x10000) ? gb : 0) | ((fbtn & 0x20000) ? pkb : 0) | (now < g_fjump_until ? g_bot_jump : 0);   // (fbtn bit 16: the grenade pulse, 17: pick up)
      if (want) { g_bot_btn = ((out & 4) ? g_bot_btn : 0) | want; out |= 4; } else if (!(out & 4)) g_bot_btn = 0; }
    if (diag) { uint64_t b = 0; int d = bot_diag(&tx, &ty, &b, now); out |= d; if (d & 4) g_bot_btn = b; else if (!(out & 4)) g_bot_btn = 0;
        static double tl; if (now - tl > 0.25) { tl = now; const float* v = o->vel;
            nlog("[nhook] [bot] test: pos (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) yaw %.1f throttle (%.2f, %.2f) buttons 0x%llx; shots %u deletes %lu", o->c[0], o->c[1], o->c[2], v[0], v[1], v[2],
                 yaw0 * 57.2958f, tx, ty, (d & 4) ? (unsigned long long)b : 0ull, g_nov_shot_n, g_nov_del_calls); } }
    { static int fo = -1; static float fy, fp; static int hp; if (fo < 0) { const char* e = getenv("NHOOK_BOT_FACE"); int k = e ? sscanf(e, "%f,%f", &fy, &fp) : 0; fo = k >= 1; hp = k == 2; }
      if (fo) { yaw = fy / 57.29578f; if (hp) pitch = fp / 57.29578f; out |= 1; } }   // NHOOK_BOT_FACE (above)
    g_bot_yaw = yaw; g_bot_pitch = pitch; g_bot_tx = tx; g_bot_ty = ty; g_bot_out = out;
    // the virtual controller: left stick = the throttle (y forward, x right = -left), A = jump, right trigger = fire
    bot_pad(-ty, tx, (spar && g_bot_fire) ? 1.0f : 0.0f, (((out & 4) && (g_bot_btn & g_bot_jump)) ? 1u : 0u) | (fbtn & 0xFFFF) | ((fbtn & 0x10000) ? 0x40000000u : 0));
    if (!(spar && g_bot_err >= 0)) g_bot_fire = 0;
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
    if (g_bot_spar || g_nav_mode_rt > 0 || getenv("NHOOK_BOT_THROTTLE") || getenv("NHOOK_BOT_BTNSCAN") || getenv("NHOOK_BOT_FACE") || getenv("NHOOK_BOT_HOLDBTN")) bot_start(); }
static int bot_control(const char* act, const char* qs) {   // POST do=spar&on=0|1, do=walk&mode=off|roam|enemy|x,y,z|vehicle:<name>, do=botbtn&bits=<hex>&secs=
    if (!bot_allowed()) return 0;
    if (!strcmp(act, "botbtn")) { const char* b = strstr(qs, "bits="), * t = strstr(qs, "secs="); if (!b) return 0;
        g_bot_testbtn = strtoull(b + 5, 0, 16); g_bot_testbtn_until = mono_s() + (t ? strtod(t + 5, 0) : 1.0); bot_start(); return 1; }
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
