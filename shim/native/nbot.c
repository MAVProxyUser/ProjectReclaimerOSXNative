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
// do=botpad&b=<hex>&secs= (2026-11-08, shim/tools/melee-scan.sh): virtual-pad buttons held for a time (nvpad.m bits: 1 A, 2 B, 4 X,
// 8 Y, 0x10 LB, 0x20 RB, 0x40 / 0x80 stick clicks, 0x400.. d-pad; 0x40000000 the left trigger, 0x80000000 the right trigger)
static volatile uint32_t g_bot_testpad; static volatile double g_bot_testpad_until;
static int g_bot_pick_on; static float g_bot_pick_pos[3];   // a weapon on the floor the walker walks to (the fighter's pickups)
static int g_pick_gain; static float g_pick_d;               // (how many ranks better it is, how far: the fetch bid scales with them)
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
void nh_vpad_btn_reads(unsigned long* out4);   // nvpad.m: the app's reads of B, Y, RB, the right trigger
static volatile int g_vpad_on; static int g_pad_written;
// SHADOW MODE (2026-11-09, the user: "I need it to learn in a REAL scenario now" -- a walker in a public match was declined: an
// automated player with memory-read targeting against people who never agreed to face one): NHOOK_SHADOW=1 runs the walker's whole
// brain (fighter, goals, nav) inside the PLAYER's own game, any server, but publishes NOTHING -- g_bot_out stays 0 (bot_write and the
// packet hook write only from it), the virtual pad is never set. Every 0.1 s its decision and what the player actually did go to
// tracks/shadow/<time>_<map>.jsonl (shim/tools/shadow-study.py compares them); the stats snapshot's "shadow" shows its target live.
static int g_bot_shadow, g_sh_aim_ok; static float g_sh_yaw, g_sh_pitch; static volatile uint32_t g_sh_tgt; static volatile float g_sh_d; static volatile int g_sh_fire; static volatile double g_sh_t;
static void bot_pad(float lx, float ly, float rt, uint32_t b) {   // b: nvpad.m's button bits; bit 30 = the left trigger (a grenade)
    if (!g_vpad_on || g_bot_shadow) return; nh_vpad_set(lx, ly, 0, 0, (b & 0x40000000u) ? 1.0f : 0.0f, rt, b & 0xFFFFu); g_pad_written = 1; }
static int bot_walk_ok(void) { static int nopkt = -1; if (nopkt < 0) nopkt = getenv("NHOOK_BOT_NOPKT") != 0;   // (the walker: this client's moves count:
    return (bot_allowed() || g_bot_shadow) && (!g_srv_addr[0] || g_vpad_on || !nopkt); }                                            //  a local game, or the control packet hook)
// (2026-11-09) A PUBLIC server whose OWNER agreed (the user asked its host first). NHOOK_BOT_ALLOW_SERVER=<ip:port> unlocks exactly that
// address, and only for an identity whose nickname says what it is (starts "AIWalker"); a test profile still never plays anything
// public. Anything else: off. (No server's address or host is kept in the sources.)
static int bot_owner_ok(void) {
    static const char* e = (const char*)1; if (e == (const char*)1) e = getenv("NHOOK_BOT_ALLOW_SERVER");
    int ok = e && *e && g_srv_addr[0] && !strcmp(e, g_srv_addr) && !strncmp(g_nick, "AIWalker", 8) && !getenv("NHOOK_NO_BOT");
    static int told = -1; if (ok != told) { told = ok; if (e && *e) nlog("[nhook] [bot] owner-approved server %s: %s (joined %s, nickname %s)", e, ok ? "the walker plays" : "not this one",
                                                                          g_srv_addr[0] ? g_srv_addr : "-", !strncmp(g_nick, "AIWalker", 8) ? "discloses" : "does NOT disclose"); }
    return ok; }
static int bot_allowed(void) {
    static int t = -1; if (t < 0) { const char* p = getenv("RECLAIMER_PROFILE_DIR"); t = p && *p && !getenv("NHOOK_NO_BOT"); }
    return (t && bot_private_addr(g_srv_addr)) || bot_owner_ok(); }

// LIVE KNOBS (2026-11-09, the user: "fix the walker code so it can do a hot launch of its learning model and not lose connection"):
// the walker's tunables come from <battles>/live-knobs/<nickname>.json (NHOOK_KNOB_FILE) while it holds them -- {"NHOOK_BOT_NOISE": 0.9,
// ...}, re-read within a second of a change -- else from the launch environment (a knob taken out of the file goes back to it). The
// models (walker-model.json, engage-model.json, melee.json) are re-read when their files change (bot_models_tick). No restart: the
// walker keeps its place in the game. [knobs] log lines.
typedef struct { char n[48]; float v; } LiveKnob; static LiveKnob g_lk[64]; static int g_nlk;
static const char* lk_path(void) { static char p[1100]; const char* e = getenv("NHOOK_KNOB_FILE");
    if (e && *e) return e;
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/battles/live-knobs/%s.json", getenv("HOME") ? getenv("HOME") : "/tmp", g_nick[0] ? g_nick : "walker"); return p; }
static void lk_tick(void) {
    static double t; double now = mono_s(); if (now - t < 1.0) return; t = now;
    static time_t mt; static off_t sz; static int had; struct stat st; const char* p = lk_path();
    if (stat(p, &st) != 0) { if (had) { had = 0; g_nlk = 0; mt = 0; nlog("[nhook] [knobs] %s gone: the launch values again", p); } return; }
    if (had && st.st_mtime == mt && st.st_size == sz) return;
    FILE* f = fopen(p, "r"); if (!f) return; char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    LiveKnob k[64]; int nk = 0; const char* q = b; char list[600]; int lq = 0; list[0] = 0;
    while (nk < 64 && (q = strchr(q, '"'))) { const char* e = strchr(q + 1, '"'); if (!e) break; size_t L = (size_t)(e - q - 1);
        const char* c = e + 1; while (*c == ' ' || *c == ':' || *c == '\t') c++; char* end; float v = strtof(c, &end);
        if (end != c && L > 0 && L < sizeof k[0].n) { memcpy(k[nk].n, q + 1, L); k[nk].n[L] = 0; k[nk].v = v;
            if (lq < (int)sizeof list - 60) lq += snprintf(list + lq, sizeof list - (size_t)lq, "%s%s=%.3g", nk ? " " : "", k[nk].n + (strncmp(k[nk].n, "NHOOK_BOT_", 10) ? 0 : 10), v); nk++; }
        q = end != c ? end : e + 1; }
    memcpy(g_lk, k, sizeof k); g_nlk = nk; mt = st.st_mtime; sz = st.st_size; had = 1;
    nlog("[nhook] [knobs] live from %s: %s", p, nk ? list : "(none)"); }
static int lk_get(const char* n, float* v) { for (int i = 0; i < g_nlk; i++) if (!strcmp(g_lk[i].n, n)) { *v = g_lk[i].v; return 1; } return 0; }
static void lk_over(float* v, const char* n) { float x; if (lk_get(n, &x)) *v = x; }

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
static int g_bm_done;   // (bot_models_tick clears it when walker-model.json changes)
static void bm_load(void) {
    if (g_bm_done) return; g_bm_done = 1; char path[1024]; const char* e = getenv("NHOOK_BOT_MODEL"), * h = getenv("HOME");
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
// do=botpkt&b8=<hex>&b2c=<hex>&secs= (2026-11-08, melee-scan.py --packet): those bits ORed into the packed record for a time --
// the packet carries only +8 bits 0-2, 11, 14 and +0x2c's 7 bits, so the melee bit is among them (no pad button meleed)
static volatile uint32_t g_bot_tpk8, g_bot_tpk2c; static volatile double g_bot_tpk_until;
// do=botwslot&v=<n>&secs= (2026-11-08): the walker's buttons never reach the game in a headless client (Y never switched weapons,
// an empty rifle went "click click click" past the user), but the control packet carries the weapon index from the local unit's
// +0x260 (net-authority.md, packet "A" +0x1a). This writes that byte on the walker's own unit just before each packet is built --
// the experiment: does the server switch the weapon? The bytes +0x258..+0x26f are logged once at the first request.
static volatile uint64_t g_bot_unit_p; static volatile int g_bot_wsel_v; static volatile double g_bot_wsel_until;
// MELEE BITS (2026-11-09, NHOOK_INPUT_LOG on the user's own game: one action switched on 0x21100800 in the packed button word 33 times
// in two rounds -- bit 11 (one of the bits the control packet carries) with 20 / 24 / 29 -- the best candidate for melee, the
// user's 5 melee kills a round; the virtual pad's buttons never reach a headless client). While the walker melees, those bits go into
// its control record (NHOOK_BOT_MELEEBITS, hex) and bit 11 straight into the packed record. NHOOK_BOT_MELEEBITS=0 = off.
static volatile double g_bot_melee_until;
// (2026-11-09 00:35: the input map labelled 0x21100800 IN CONTEXT -- 15 grenade throws follow it, it is the GRENADE THROW, not melee:
//  off by default; melee leaves no trace in the unit and is among the "nothing seen" bits still)
// (00:55: with more of the user's play -- jump 27 times after +8 bit 1, a throw 47 after bit 11 -- +8 bit 0 is the one packet bit
//  that left no trace yet came with play: MELEE's best candidate now; the extra byte's bit 0 came with 10 pick-ups (the ACT button:
//  pick up / reload), its bit 1 with weapon switches. The walker sends them in the packed record while it melees / picks up / reloads
//  / switches: NHOOK_BOT_MELEEBITS (+8, default 1), NHOOK_BOT_ACTBIT / NHOOK_BOT_SWITCHBIT (the extra byte, default 1 / 2; 0 = off))
// (01:10: MELEE IS +8 BIT 5 -- the user's two beatdowns in the Pit round came 0.2 s after bit 5 presses (round clock 232.3 / 381.3 s, his
//  log 211.0 / 360.0 s: the same 21.3 s offset both times), and his two other bit-5 presses were swings that missed; the walkers had
//  sent bit 0 and "meleed" every step at 0.4 units with nothing landing. Bit 5 goes where fire's bit 19 goes: the control record)
static uint64_t bot_meleebits(void) { static uint64_t v = 2; if (v == 2) { const char* e = getenv("NHOOK_BOT_MELEEBITS"); v = e ? strtoull(e, 0, 16) : 0x20; } return v; }
static uint8_t bot_actbit(void) { static int v = -1; if (v < 0) { const char* e = getenv("NHOOK_BOT_ACTBIT"); v = e ? (int)strtol(e, 0, 16) : 0x1; } return (uint8_t)v; }
static uint8_t bot_switchbit(void) { static int v = -1; if (v < 0) { const char* e = getenv("NHOOK_BOT_SWITCHBIT"); v = e ? (int)strtol(e, 0, 16) : 0x2; } return (uint8_t)v; }
static volatile double g_bot_act_until, g_bot_switch_until;   // (the act / switch bits held until)
// (02:20, the user: "third has got to stop jumping out of the turret": act pressed while seated is "get out" -- the boarding pulse that
//  seated it went on pressing. While seated the act bit only goes out inside a deliberate exit window)
static volatile double g_bot_seat_t, g_gun_exit_until;
// HONK (the user: "IF they do they MUST get back in, especially if the team mate honks"; then "honk detector should run on the walkers
// not me": nothing in the player's game). The walker's own detector is honk_scan below (finding the field first); honk.txt
// (NHOOK_HONK_FILE: a wall time) stays as a manual trigger for tests
static const char* honk_path(void) { static char p[1024]; if (!p[0]) { const char* e = getenv("NHOOK_HONK_FILE");
    if (e) snprintf(p, sizeof p, "%s", e); else snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/battles/honk.txt", getenv("HOME") ? getenv("HOME") : "/tmp"); } return p; }
static double wall_s(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }
static double honk_age(void) {   // seconds since the last honk (1e9: none)
    static double tr, last; double now = mono_s(); if (now - tr > 0.5) { tr = now; FILE* f = fopen(honk_path(), "r"); if (f) { double v = 0; if (fscanf(f, "%lf", &v) == 1) last = v; fclose(f); } }
    return last > 0 ? wall_s() - last : 1e9; }
// NHOOK_INPUT_LOG=1 (2026-11-08): what a REAL player's game puts in the packed record -- the button word (+8, a u64) and the extra
// byte (+0x2c) -- and the local unit's weapon slot bytes (+0x260 / +0x262) and grenade type (+0x298), logged whenever one changes
// ([input] lines, ms since the first). The walkers' buttons never reach the game in a headless client (melee, reload, weapon and
// grenade switches); the user meleeing / reloading / switching shows which bit or byte each one is. Read only.
static int inp_log_on(void) { static int v = -1; if (v < 0) v = getenv("NHOOK_INPUT_LOG") != 0; return v; }
// IN CONTEXT (2026-11-09, the user: "I'm sending walking, jumping, change grenade, you know the entire dang controller map. the server
// should see it all in context"): every bit that switches on is checked 0.6 s later against the player's own unit -- grenades fewer
// (a throw), the grenade type changed (+0x298), the weapon slot changed (+0x262: a switch), the weapons changed (+0x268 / +0x26c: a
// pick-up), risen 0.3+ units (a jump) -- tallied per bit; every 60 s an [input-map] line per bit. The unit by its handle straight from
// the object array (noverlay's table is not filled in a player's game).
static uint64_t inp_unit_ptr(uint32_t h) {
    uint64_t oa = hb_object_array(); uint8_t hdr[0x60]; if (!oa || h == 0xFFFFFFFFu || !hb_read(oa, hdr, sizeof hdr)) return 0;
    int i = h & 0xFFFF, hi = *(int32_t*)(hdr + 0x3c); if (i >= hi) return 0; uint64_t data = *(uint64_t*)(hdr + 0x48); uint8_t e[0x18];
    if (!data || !hb_read(data + (uint64_t)i * 0x18, e, sizeof e) || *(uint16_t*)e != (uint16_t)(h >> 16)) return 0; return *(uint64_t*)(e + 0x10); }
typedef struct { int ok, wslot, gtype, gsum; uint32_t w0, w1; float z; } InpUnit;
static InpUnit inp_unit(void) { InpUnit u; memset(&u, 0, sizeof u); uint64_t p = inp_unit_ptr(g_my_unit); uint8_t b[0x2a0];
    if (!p || !hb_read(p, b, sizeof b)) return u; u.ok = 1; u.wslot = (int8_t)b[0x262]; u.gtype = (int8_t)b[0x298]; u.w0 = *(uint32_t*)(b + 0x268); u.w1 = *(uint32_t*)(b + 0x26c);
    for (int k = 0; k < 4; k++) u.gsum += (int8_t)b[0x294 + k]; u.z = *(float*)(b + 0x58); return u; }
enum { IE_THROW, IE_GSWITCH, IE_WSWITCH, IE_PICKUP, IE_JUMP, IE_NONE, IE_N };
static const char* const g_ie_name[IE_N] = {"throw", "grenade switch", "weapon switch", "pick-up", "jump", "nothing seen"};
static void inp_context(uint64_t on8, uint8_t on2c) {
    static struct { double t; uint64_t b8; uint8_t b2c; InpUnit u; } q[32]; static int nq; static unsigned cnt[72][IE_N]; static double tsum;
    double now = mono_s();
    for (int i = 0; i < nq; ) { if (now - q[i].t < 0.6) { i++; continue; }
        InpUnit a = q[i].u, b = inp_unit(); int ef[IE_N] = {0}, any = 0;
        if (a.ok && b.ok) { if (b.gsum < a.gsum) ef[IE_THROW] = any = 1; if (b.gtype != a.gtype) ef[IE_GSWITCH] = any = 1;
            if (b.w0 != a.w0 || b.w1 != a.w1) ef[IE_PICKUP] = any = 1; else if (b.wslot != a.wslot) ef[IE_WSWITCH] = any = 1; if (b.z - a.z > 0.3f) ef[IE_JUMP] = any = 1; }
        if (!any) ef[IE_NONE] = 1;
        for (int k = 0; k < 64; k++) if (q[i].b8 >> k & 1) for (int e = 0; e < IE_N; e++) cnt[k][e] += (unsigned)ef[e];
        for (int k = 0; k < 8; k++) if (q[i].b2c >> k & 1) for (int e = 0; e < IE_N; e++) cnt[64 + k][e] += (unsigned)ef[e];
        q[i] = q[--nq]; }
    if ((on8 || on2c) && nq < 32) { q[nq].t = now; q[nq].b8 = on8; q[nq].b2c = on2c; q[nq].u = inp_unit(); nq++; }
    if (now - tsum >= 60.0) { tsum = now;
        for (int k = 0; k < 72; k++) { unsigned tot = 0; for (int e = 0; e < IE_N; e++) tot += cnt[k][e]; if (!tot) continue;
            char l[300]; int m = 0; for (int e = 0; e < IE_N; e++) if (cnt[k][e]) m += snprintf(l + m, sizeof l - (size_t)m, "%s%s %u", m ? ", " : "", g_ie_name[e], cnt[k][e]);
            nlog("[nhook] [input-map] %s bit %d: %s", k < 64 ? "+8" : "+0x2c", k < 64 ? k : k - 64, l); } } }
static volatile uint64_t g_inp_b8_acc;   // (shadow mode: the player's packed buttons since the last sample)
static void inp_log(const uint8_t* act) {
    static uint64_t p8 = ~0ull; static uint8_t p2c = 0xFF, pw0 = 0xFF, pw2 = 0xFF, pg = 0xFF; static double t0;
    uint64_t b8 = *(const uint64_t*)(act + 8); uint8_t b2c = act[0x2c], w0 = 0xFF, w2 = 0xFF, gr = 0xFF;
    __atomic_fetch_or(&g_inp_b8_acc, b8, __ATOMIC_RELAXED);
    inp_context(p8 == ~0ull ? 0 : b8 & ~p8, (uint8_t)(p2c == 0xFF ? 0 : b2c & ~p2c));   // (every packet: the pending checks too)
    { InpUnit u = inp_unit(); if (u.ok) { w2 = (uint8_t)u.wslot; gr = (uint8_t)u.gtype; uint64_t up = inp_unit_ptr(g_my_unit); uint8_t x; if (up && hb_read(up + 0x260, &x, 1)) w0 = x; } }
    if (b8 == p8 && b2c == p2c && w0 == pw0 && w2 == pw2 && gr == pg) return;
    double now = mono_s(); if (!t0) t0 = now;
    nlog("[nhook] [input] %8.0f ms: +8 %016llx (on %llx, off %llx) +0x2c %02x (on %02x, off %02x); unit +0x260 %d +0x262 %d grenade %d",
         (now - t0) * 1e3, (unsigned long long)b8, (unsigned long long)(p8 == ~0ull ? 0 : b8 & ~p8), (unsigned long long)(p8 == ~0ull ? 0 : p8 & ~b8),
         b2c, (uint8_t)(p2c == 0xFF ? 0 : b2c & ~p2c), (uint8_t)(p2c == 0xFF ? 0 : p2c & ~b2c), (int)(int8_t)w0, (int)(int8_t)w2, (int)(int8_t)gr);
    p8 = b8; p2c = b2c; pw0 = w0; pw2 = w2; pg = gr; }
static void bot_on_abuild(uint64_t* ctx) {
    uint8_t* act = (uint8_t*)(uintptr_t)ctx[2]; if (act && (inp_log_on() || g_bot_shadow)) inp_log(act);
    if (!act || !g_bot_out || g_hc_off[HC_BOT]) return; g_bot_pkt_n++;
    { static int told; if (!told) { told = 1; uint64_t blk = tls_block(), rec = 0; if (blk) hb_read(blk + 0xC0, &rec, 8);
        nlog("[nhook] [bot] control packet: the builder's record %p, the reader's record 0x%llx (record +0x%llx)", (void*)act, (unsigned long long)rec, rec ? (unsigned long long)((uint64_t)(uintptr_t)act - rec) : 0ull); } }
    int o = g_bot_out;
    if (o & 1) { *(float*)(act + 0x10) = g_bot_yaw; *(float*)(act + 0x14) = g_bot_pitch; }
    if (o & 2) { *(float*)(act + 0x18) = g_bot_tx; *(float*)(act + 0x1c) = g_bot_ty; }
    static int extra = -1; static uint32_t xb; if (extra < 0) { const char* e = getenv("NHOOK_BOT_PKTBTN"); extra = e != 0; xb = e ? (uint32_t)strtoul(e, 0, 16) : 0; }
    if (g_bot_wsel_until > 0 && mono_s() < g_bot_wsel_until && g_bot_unit_p) {
        uint8_t* u = (uint8_t*)(uintptr_t)g_bot_unit_p; static int told;
        if (!told) { told = 1; char h[80]; int q = 0; for (int i = 0x258; i < 0x270; i++) q += snprintf(h + q, sizeof h - (size_t)q, "%02x%s", u[i], (i & 3) == 3 ? " " : "");
            nlog("[nhook] [bot] weapon slot test: unit +0x258..+0x26f = %s (+0x262 the current slot %d)", h, (int)(int8_t)u[0x262]); }
        u[0x260] = (uint8_t)g_bot_wsel_v; }
    int tpk = g_bot_tpk_until > 0 && mono_s() < g_bot_tpk_until;
    uint32_t b8 = g_bot_pkt_btn8 | (extra ? xb : 0) | (tpk ? g_bot_tpk8 : 0) | (mono_s() < g_bot_melee_until ? (uint32_t)(bot_meleebits() & 0x4827u) : 0);   // (melee: the packet's bits of it)
    if (b8) *(uint64_t*)(act + 8) |= b8;
    double nw = mono_s(); uint32_t b2c = g_bot_pkt_btn2c | (tpk ? g_bot_tpk2c : 0) | (nw < g_bot_act_until && (nw - g_bot_seat_t > 0.6 || nw < g_gun_exit_until) ? bot_actbit() : 0) | (nw < g_bot_switch_until ? bot_switchbit() : 0);
    if (b2c) *(act + 0x2c) |= (uint8_t)b2c; }
#ifdef NH_BRAIN   // (the brain: the hooks are the body's, installed once)
int nhx_bot_hook_install(void);
static void pkt_hook_install(void) {}
static int bot_hook_install(void) { return nhx_bot_hook_install(); }
#define pkt_hook_install pkt_hook_install_body
#define bot_hook_install bot_hook_install_body
#endif
static void pkt_hook_install(void) {   // the control packet builder's hook: the walker's moves, and NHOOK_INPUT_LOG's reads
    static int done; if (done || getenv("NHOOK_BOT_NOPKT")) return; done = 1; uint64_t a = branch_target_of(0x50fb8);
    if (!a || text_hook("control packet (walker on a server)", a, bot_on_abuild)) nlog("[nhook] [bot] halo3+0x50fb8 could not be hooked: a joined walker's moves stay local");
    else if (inp_log_on()) nlog("[nhook] [input] logging the packed controls (NHOOK_INPUT_LOG)"); }
static int bot_hook_install(void) {
    if (g_bot_hook >= 0) return g_bot_hook;
    pkt_hook_install();
    uint64_t t = branch_target_of(0xf7178);
    g_bot_hook = t ? !text_hook("player control (sparring aim / walker)", t, bot_on_reader) : !body_hook_after("player control (sparring aim / walker)", 0xf66ac, bot_on_update);
    if (!g_bot_hook) nlog("[nhook] [bot] halo3+0xf66ac could not be hooked: no sparring aim / walker"); return g_bot_hook; }
#ifdef NH_BRAIN
#undef pkt_hook_install
#undef bot_hook_install
#else
__attribute__((visibility("default"))) int nhx_bot_hook_install(void) { return bot_hook_install(); }   // (the brain's way to it)
#endif

// units (players' bipeds) from the object table: noverlay.c's collector in its units-only mode, under its lock
static int bot_units(NovObj* o, int max) {
    pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 1; int n = nov_collect(o, max); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx);
    return n; }
static int bot_enemy(const NovObj* me, const NovObj* t) {   // the overlay's friend-or-foe (Infection sides, else teams; FFA: everyone)
    if (g_follow_unit != 0xFFFFFFFFu && t->handle == g_follow_unit) return 0;   // (NHOOK_BOT_FOLLOW: the player it shadows is its friend, FFA too)
    if (g_nov_sides_on) { int a = nov_side_of(me->handle), b = nov_side_of(t->handle); if (a >= 0 && b >= 0) return a != b; }
    return me->team < 0 || t->team != me->team; }
static float bot_wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

static void bot_nav_json(StBuf* s) { nav_json(s); }   // the snapshot's "nav" (the page draws the walker's path and goal)
// the walker's own inventory and power-ups (bot_inv below; the goal draw reads it)
typedef struct { const char* held; const char* other; const char* held_c; const char* other_c; int held_rank, other_rank, has_other, gren, frag, plasma, os, camo;
                 int ammo, ammo_max, other_ammo; } BotInv;   // (ammo: rounds loaded, -1 unknown; ammo_max: the most seen in that weapon)
static BotInv g_inv;
// ODDBALL (2026-11-09, the user: "the walkers will have to learn to run with the oddball and melee only and obtain the oddball from other
// players with whatever weapon given"): the ball's state, read every 0.3 s (ob_scan): 0 none seen, 1 loose, 2 I carry it, 3 a teammate
// does, 4 an enemy does (its carrier: the fighter's first target, nbot_engage.c -- the game marks the carrier on every screen)
static volatile uint32_t g_ob_enemy_carrier; static int g_ob_state; static float g_ob_pos[3];
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
static int bot_wpn_rank(const char* n0) { static const char* const pref[] = {"rocket_launcher", "spartan_laser", "sniper_rifle", "beam_rifle", "battle_rifle", "covenant_carbine",
    "shotgun", "excavator", "flak_cannon", "energy_sword", "gravity_hammer", "brute_shot", "assault_rifle", 0};   /* (08:15: excavator = the Mauler, flak_cannon = the Fuel Rod: Halo 3's tag names) */ if (!n0) return 99; char nb[96]; const char* n = bot_tagkey(n0, nb, sizeof nb); for (int i = 0; pref[i]; i++) if (strstr(n, pref[i])) return i; return 50; }
// ---- the walker's own inventory and power-ups (2026-11-08; the user: "the walker should be aware of its own inventory, and power up
// status as well"): the held and the other weapon (class + rank), grenades by type, an overshield (shield above 1), active camo
// (+0x360 above 0.05). The fighter switches to the other weapon when the target's distance is outside the held one's band and
// inside the other's (vpad Y), throws only with a grenade in hand, holds fire under camo until close; the goal draw skips power
// weapons with one in hand and goes for fights with an overshield / camo.
// the rounds-loaded field of a weapon object: ReclaimerMac/native/ammo.json {"loaded": off} (shim/tools/ammo-scan.py finds it from
// NHOOK_WPNDUMP dumps) or NHOOK_AMMO_OFF=<hex>; unknown = the walker reloads after fights only
static int bot_ammo_off(void) {
    static int off = -2; if (off != -2) return off; off = -1;
    const char* e = getenv("NHOOK_AMMO_OFF"); if (e && *e) { off = (int)strtol(e, 0, 16); return off; }
    char p[1024]; const char* h = getenv("HOME"); snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/native/ammo.json", h ? h : "");
    FILE* f = fopen(p, "r"); if (f) { char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; const char* k = strstr(b, "\"loaded\":"); if (k) off = atoi(k + 9); }
    if (off >= 0x300) off = -1; if (off >= 0) nlog("[nhook] [inv] rounds loaded at weapon object +0x%x", off); return off; }
// (08:10: in the public game the field read 0 for the whole life of a rifle and 129 now and then -- not a count there -- and the walker
//  took every 0 for an empty magazine: "empty magazine: a grenade at N units, then the beatdown" 111 times in one session, rushing
//  armed players to punch them. The field is TRUSTED for a weapon definition only once it has shown 3 different non-zero values (a
//  magazine counting down); until then the rounds are unknown (-1). NHOOK_BOT_AMMO_TRUST=0 = the old trust-at-once)
// LEARNING THE ROUNDS FIELDS (2026-11-09 08:30; the user: "empty magazines sans reload are unacceptable btw. do better. find ammo, new
// weapon, or switch to alternate. reload click noise is a signal in the game. so clearly it goes somewhere"): the rounds field the
// walker read (+0x1b4, ammo.json) is no count in this client -- 0 for a rifle's whole life, 129 now and then -- so it never knew it
// was empty and never reloaded in time. The brain watches its OWN held weapon object (its handle, never a sweep): every int16 field in
// +0x100..+0x3ff, 10 times a second; a field that steps DOWN by 1-6 while the trigger is held ("dec"), never while it is not ("bad"),
// and jumps UP on a reload / a fresh weapon ("inc") is the magazine. Adopted once dec >= 15, inc >= 1, bad <= dec / 4: from then the
// walker's rounds are that field (any weapon), and a 0 there is empty. [ammo] lines every 30 s give the best candidates.
// NHOOK_BOT_AMMO_LEARN=0 (live) = off.
static int g_ammo_learned = -1;
static int g_bot_trig;   // (the trigger held last step: defined with the fire section below)
static int g_bot_dry;    // (the magazine stays empty after reloads: no reserve -- the other weapon or any weapon on the floor)
static const char* ammo_learned_path(void) { static char p[1024]; if (!p[0]) { const char* h = getenv("HOME"); snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/native/ammo-learned.json", h ? h : ""); } return p; }
static void ammo_learn(double now, const NovObj* o) {
    float on = 1; lk_over(&on, "NHOOK_BOT_AMMO_LEARN"); if (on <= 0) return;
    { static int rd0; if (!rd0) { rd0 = 1; FILE* f = fopen(ammo_learned_path(), "r");   // (what an earlier brain / launch learned: ammo-learned.json)
        if (f) { char b[256]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; const char* k = strstr(b, "\"loaded\":"); int v = k ? atoi(k + 9) : -1;
            if (v >= 0x100 && v < 0x400 && g_ammo_learned < 0) { g_ammo_learned = v; nlog("[nhook] [ammo] the magazine is weapon object +0x%x (learned before: %s)", v, ammo_learned_path()); } } } }
    static double tl, tlog; if (now - tl < 0.1) return; tl = now;
    static struct { int dec, inc, bad; int16_t last; } al[0x180]; static uint64_t lwd; static int was_firing;
    uint64_t wd = (o->wslot >= 0 && o->wslot < 4) ? nov_datum(o->weapons[o->wslot]) : 0; if (!wd) { lwd = 0; return; }
    static uint8_t buf[0x400]; if (!hb_read(wd, buf, sizeof buf)) return;
    int same = wd == lwd, firing = g_bot_trig || was_firing; lwd = wd; was_firing = g_bot_trig;
    for (int i = 0; i < 0x180; i++) { int16_t v = *(const int16_t*)(buf + 0x100 + i * 2);
        if (same) { int d = (int)v - (int)al[i].last;
            if (d < 0 && d >= -6) { if (firing) al[i].dec++; else al[i].bad++; }
            else if (d > 0 && d <= 400) al[i].inc++;
            else if (d) al[i].bad++; }
        al[i].last = v; }
    if (now - tlog > 30) { tlog = now; int top[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 0x180; i++) { int sc = al[i].dec - 2 * al[i].bad; if (al[i].dec < 3) continue;
            for (int k = 0; k < 4; k++) if (top[k] < 0 || sc > al[top[k]].dec - 2 * al[top[k]].bad) { for (int q = 3; q > k; q--) top[q] = top[q - 1]; top[k] = i; break; } }
        char b[400]; int q = 0; for (int k = 0; k < 4 && top[k] >= 0; k++) { int i = top[k];
            q += snprintf(b + q, sizeof b - (size_t)q, " +0x%x = %d (down %d while firing, up %d, wrong %d);", 0x100 + i * 2, al[i].last, al[i].dec, al[i].inc, al[i].bad); }
        if (q) nlog("[nhook] [ammo] rounds-field candidates (held weapon):%s%s", b, g_ammo_learned >= 0 ? "" : " -- none adopted yet");
        if (g_ammo_learned < 0 && top[0] >= 0) { int i = top[0];
            if (al[i].dec >= 15 && al[i].inc >= 1 && al[i].bad * 4 <= al[i].dec) { g_ammo_learned = 0x100 + i * 2;
                nlog("[nhook] [ammo] LEARNED: the magazine is weapon object +0x%x (down %d times while firing, up %d, wrong %d)", g_ammo_learned, al[i].dec, al[i].inc, al[i].bad);
                FILE* f = fopen(ammo_learned_path(), "w"); if (f) { fprintf(f, "{\"loaded\": %d, \"hex\": \"0x%x\", \"down\": %d, \"up\": %d, \"wrong\": %d}\n", g_ammo_learned, g_ammo_learned, al[i].dec, al[i].inc, al[i].bad); fclose(f); } } }
        if (g_ammo_learned >= 0) { char b[200]; int q = 0; for (int off = g_ammo_learned - 8; off <= g_ammo_learned + 8; off += 2) q += snprintf(b + q, sizeof b - (size_t)q, " %+d:%d", off - g_ammo_learned, *(const int16_t*)(buf + off));
            nlog("[nhook] [ammo] around the magazine (+0x%x):%s", g_ammo_learned, b); } } }
static int bot_ammo_of(uint64_t wd, uint32_t def, int* maxv) {   // rounds loaded (-1 unknown), and the most seen for that weapon definition
    static struct { uint32_t def; int max, nv; int16_t v[3]; } mx[64]; static int nmx; int off = bot_ammo_off(); int16_t a = -1;
    if (g_ammo_learned >= 0) { if (!wd || !hb_read(wd + g_ammo_learned, &a, 2) || a < 0) { if (maxv) *maxv = 0; return -1; }   // (learned: ammo_learn above)
        int k = -1; for (int i = 0; i < nmx; i++) if (mx[i].def == def) { k = i; break; }
        if (k < 0) { k = nmx < 64 ? nmx++ : 63; mx[k].def = def; mx[k].max = 0; mx[k].nv = 0; } if (a > mx[k].max) mx[k].max = a; if (maxv) *maxv = mx[k].max; return a; }
    if (off < 0 || !wd || !hb_read(wd + off, &a, 2) || a < 0) { if (maxv) *maxv = 0; return -1; }
    int k = -1; for (int i = 0; i < nmx; i++) if (mx[i].def == def) { k = i; break; }
    if (k < 0) { k = nmx < 64 ? nmx++ : 63; mx[k].def = def; mx[k].max = 0; mx[k].nv = 0; } if (a > mx[k].max) mx[k].max = a;
    if (a > 0 && mx[k].nv < 3) { int seen = 0; for (int i = 0; i < mx[k].nv; i++) if (mx[k].v[i] == a) seen = 1; if (!seen) mx[k].v[mx[k].nv++] = a; }
    float need = 3; lk_over(&need, "NHOOK_BOT_AMMO_TRUST");
    if (mx[k].nv < (int)need) { if (maxv) *maxv = 0; return -1; }
    // (08:20: still "empty magazine ... then the beatdown": a trusted field can drop straight from 129 to 0. A 0 is EMPTY only after
    //  a countdown -- this weapon object's last non-zero value was a magazine's worth (<= 60) within the last 10 s; else unknown)
    { static struct { uint64_t wd; int v; double t; } lw[16]; static unsigned nlw; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double tn = ts.tv_sec + ts.tv_nsec * 1e-9;
      int j = -1; for (int i = 0; i < 16; i++) if (lw[i].wd == wd) { j = i; break; } if (j < 0) { j = (int)(nlw++ & 15); lw[j].wd = wd; lw[j].v = -1; lw[j].t = 0; }
      if (a > 0) { lw[j].v = a; lw[j].t = tn; }
      else if (!(lw[j].v > 0 && lw[j].v <= 60 && tn - lw[j].t < 10.0)) { if (maxv) *maxv = 0; return -1; } }
    if (maxv) *maxv = mx[k].max; return a; }
static void bot_inv(const NovObj* o, BotInv* v) {
    memset(v, 0, sizeof *v); v->held_c = v->other_c = "none"; v->held_rank = v->other_rank = 99; v->ammo = v->other_ammo = -1;
    for (int q = 0; q < 4; q++) { uint64_t wd = nov_datum(o->weapons[q]); uint32_t df = 0; if (!wd || !hb_read(wd, &df, 4)) continue; const char* nm = hb_tag_name(df);
        if (q == o->wslot) { v->held = nm; v->held_c = eg_wcls(nm); v->held_rank = bot_wpn_rank(nm); v->ammo = bot_ammo_of(wd, df, &v->ammo_max); }
        else if (!v->has_other) { v->other = nm; v->other_c = eg_wcls(nm); v->other_rank = bot_wpn_rank(nm); v->has_other = 1; v->other_ammo = bot_ammo_of(wd, df, 0); } }
    v->frag = o->gren[0]; v->plasma = o->gren[1]; for (int k = 0; k < 4; k++) if (o->gren[k] > 0) v->gren += o->gren[k];
    v->os = o->shield > 1.05f; v->camo = o->camo > 0.05f; }
// WEAPON ZONES (2026-11-08; the user: "walker should use its best weapon based on situation that can be derived from game
// observations of where kills happen with certain weapons. if opponent is in a certain zone and you have a certain high kill
// weapon for that zone and aren't using it switch to it"): tracks/maps/<map>.wzones.txt (shim/tools/weapon-kills.py, every
// recorded match on the map, Infection left out): the players' kills by the victim's 6-unit cell and the killer's weapon class,
// and each class's share of the weapons carried there. For a target's position the 3 x 3 cells around it give each class's
// kill share there over its carried share (wz_eff: above 1 = it kills there more than it is carried); in a fight the walker
// switches to its other weapon when that one's is 1.5x the held one's and above 1.2 (8+ kills around), and keeps a weapon the
// zone favours 1.5x even when the distance band would switch away. NHOOK_BOT_NOZONES=1 = off.
#define WZ_NC 6
static const char* const wz_cls[WZ_NC] = {"precision", "auto", "sniper", "shotgun", "rocket", "melee"};
typedef struct { int16_t cx, cy; uint16_t k[WZ_NC]; } WzCell;
static struct { char map[64]; float cell, held[WZ_NC]; int n; WzCell* c; } g_wz;
static int wz_ci(const char* cls) { for (int i = 0; cls && i < WZ_NC; i++) if (!strcmp(cls, wz_cls[i])) return i; return -1; }
static void wz_load(const char* map) {
    static int off = -1; if (off < 0) off = getenv("NHOOK_BOT_NOZONES") != 0; if (off || !map || !map[0] || !strcmp(map, g_wz.map)) return;
    snprintf(g_wz.map, sizeof g_wz.map, "%s", map); free(g_wz.c); g_wz.c = 0; g_wz.n = 0; g_wz.cell = 6; memset(g_wz.held, 0, sizeof g_wz.held);
    char p[1024]; const char* d = getenv("NHOOK_WZONES_DIR"), * h = getenv("HOME");
    if (d && *d) snprintf(p, sizeof p, "%s/%s.wzones.txt", d, map); else snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/maps/%s.wzones.txt", h ? h : "", map);
    FILE* f = fopen(p, "r"); if (!f) { nlog("[nhook] [inv] no weapon zones for %s", map); return; }
    char ln[2048]; int cap = 0;
    while (fgets(ln, sizeof ln, f)) {
        if (!strncmp(ln, "wzones", 6)) { float c = 0; if (sscanf(ln, "wzones %*d cell %f", &c) == 1 && c > 0) g_wz.cell = c; continue; }
        int held = !strncmp(ln, "held ", 5); WzCell z; memset(&z, 0, sizeof z); char* q = ln;
        if (!held) { int cx, cy, nn = 0; if (sscanf(ln, "%d %d%n", &cx, &cy, &nn) != 2) continue; z.cx = (int16_t)cx; z.cy = (int16_t)cy; q = ln + nn; } else q = ln + 5;
        for (char* t = strtok(q, " \n"); t; t = strtok(0, " \n")) { char* e = strchr(t, '='); if (!e) continue; *e = 0; int ci = wz_ci(t); if (ci < 0) continue;
            if (held) g_wz.held[ci] = strtof(e + 1, 0); else z.k[ci] = (uint16_t)atoi(e + 1); }
        if (!held) { if (g_wz.n >= cap) { cap = cap ? cap * 2 : 256; g_wz.c = realloc(g_wz.c, cap * sizeof *g_wz.c); } g_wz.c[g_wz.n++] = z; } }
    fclose(f); nlog("[nhook] [inv] weapon zones for %s: %d cells (cell %.0f u)", map, g_wz.n, g_wz.cell); }
static float wz_eff(int ci, const float* p, int* tot) {   // kills there with this class / all kills there, over its carried share
    int cx = (int)floorf(p[0] / g_wz.cell), cy = (int)floorf(p[1] / g_wz.cell); int kc = 0, kt = 0;
    for (int i = 0; i < g_wz.n; i++) { const WzCell* z = &g_wz.c[i]; if (abs(z->cx - cx) > 1 || abs(z->cy - cy) > 1) continue; kc += z->k[ci]; for (int j = 0; j < WZ_NC; j++) kt += z->k[j]; }
    if (tot) *tot = kt; return ((kc + 0.5f) / (kt + 0.5f * WZ_NC)) / fmaxf(0.03f, g_wz.held[ci]); }
static float bot_band_off(const char* cls, float d, float kd) {   // 0 inside the players' band for this weapon class, else how far outside (relative); -1 no band
    float a, b; if (!eg_band(cls, &a, &b)) return -1; a *= kd; b *= kd; return d < a ? (a - d) / a : d > b ? (d - b) / b : 0; }
// ---- melee (2026-11-08; the user, watching a walker hold a Gravity Hammer on an enemy at 0.6-1.3 units: "clearly you never trained
// on melees sooooo many opportunities for the walker to melee"). The walker HELD the right trigger the whole fight: the hammer, the
// sword and single-shot weapons act on a PULL, so it swung / fired once at most; and its melee button was down for one 33 ms step.
// Players' numbers (shim/tools/melee-model.py over 43 public matches -> tracks/melee.json, NHOOK_MELEE_FILE): 3,836 melee kills at
// a median 0.65 units (p75 0.97), the killer closing ~2.1 units in the last second (2.9 apart 1 s before); half of them finish a
// victim whose shield was already down (closing in from ~3.2 units, p75); sword kills within 1.28 (p75), hammer within 1.33.
// NHOOK_BOT_NOMELEE=1 = the old melee (1.8 units, one step, the trigger held).
typedef struct { float range, rush, sword, hammer, back; int loaded; } BotMelee;
static BotMelee g_mel = {0.97f, 3.24f, 1.28f, 1.33f, 3.31f, 0};   // (back: an enemy looking away is rushed from here -- players: 3.3 units)
static void mel_load(void) {
    if (g_mel.loaded) return; g_mel.loaded = 1; char p[1100]; const char* e = getenv("NHOOK_MELEE_FILE"), *h = getenv("HOME");
    if (e) snprintf(p, sizeof p, "%s", e); else snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/melee.json", h ? h : "/tmp");
    FILE* f = fopen(p, "r"); if (!f) { nlog("[nhook] [melee] no %s: the built-in numbers (range %.2f, rush from %.1f)", p, g_mel.range, g_mel.rush); return; }
    static char b[65536]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; const char* w = strstr(b, "\"walker\"");
    #define MELNUM(k, v) { const char* q = w ? strstr(w, "\"" k "\":") : 0; if (q) { float x = strtof(q + strlen(k) + 3, 0); if (x > 0.1f && x < 10) v = x; } }
    MELNUM("melee_range", g_mel.range) MELNUM("rush_from", g_mel.rush) MELNUM("sword_range", g_mel.sword) MELNUM("hammer_range", g_mel.hammer) MELNUM("back_rush_from", g_mel.back)
    #undef MELNUM
    nlog("[nhook] [melee] players' numbers: melee within %.2f units, beatdown rush from %.1f, a turned back from %.1f, sword %.2f, hammer %.2f", g_mel.range, g_mel.rush, g_mel.back, g_mel.sword, g_mel.hammer); }
// weapons that act on a trigger PULL (holding does not repeat): the trigger is pulsed (0.12 s down, 0.12 s up) while firing.
// (the bullet log shows the battle rifle and carbine firing on while held: bursts every 0.25-0.35 s)
static int bot_pull_weapon(const char* w) {
    static const char* const k[] = {"HAMMER", "SWORD", "ENERGY BLADE", "SNIPER", "BEAM RIFLE", "MAGNUM", "ROCKET", "BRUTE SHOT", "FUEL ROD", 0};
    if (!w) return 0; for (int i = 0; k[i]; i++) if (strstr(w, k[i])) return 1; return 0; }
static int g_bot_pull, g_bot_trig; static double g_bot_fire_t0;   // (the held weapon acts on a pull; when this string of fire began)
static double g_bot_hold_high, g_bot_hold_last;   // (levels apart: holding the high ground until; the last levels-apart step)
static int g_fol_go;   // (the leader is 5+ units away: its path beats the fight's moves)
// GRENADE OUTCOMES (2026-11-09, the user: "this is the first place to try to hone the predictive nades"): each throw notes its target and
// the target's shield + health; 1.6 s later "[nade] <target> at D units, lead L s, arc A rad: hit / missed / KILLED / target gone" --
// follow-study.py counts them per window and steps the live grenade-lead knobs (NHOOK_BOT_NADE_*) toward the best hit rate
static float g_nade_lead, g_nade_arc, g_nade_hp, g_nade_d; static double g_nade_t; static uint32_t g_nade_tgt; static int g_nade_pending;
static void nade_note(double now, const NovObj* t, float d) { g_nade_t = now; g_nade_tgt = t->handle; g_nade_d = d; g_nade_pending = 1;
    g_nade_hp = (t->shield > 0 ? t->shield : 0) + (t->health > 0 ? t->health : 0); }
static void nade_check(double now, const NovObj* objs, int n) {
    if (!g_nade_pending || now - g_nade_t < 1.6) return; g_nade_pending = 0; const NovObj* t = 0;
    for (int i = 0; i < n; i++) if (objs[i].handle == g_nade_tgt) { t = &objs[i]; break; }
    float hp = t && !t->dead ? (t->shield > 0 ? t->shield : 0) + (t->health > 0 ? t->health : 0) : 0;
    const char* r = !t ? "target gone" : t->dead ? "KILLED" : g_nade_hp - hp > 0.3f ? "hit" : "missed";
    nlog("[nhook] [nade] %08x at %.1f units, lead %.2f s, arc %.2f rad: %s (shield+health %.2f -> %.2f)", g_nade_tgt, g_nade_d, g_nade_lead, g_nade_arc, r, g_nade_hp, hp); }
static int g_bot_empty;   // (an empty magazine with the enemy in sight: grenade + rush for the beatdown)
// the virtual pad's melee and act / pick-up / reload buttons (nvpad.m bits: 1 B, 0x20 RB). 2026-11-08: the walkers pressed B to
// melee and held RB to pick up -- Halo 3's own layout -- but the app's layout for these profiles is Bumper Jumper's (its own
// descriptions: "LB jumps and RB melees ... B acts and reloads"): ten weapon changes in a melee battle followed a B press (none
// melee), the hammers were swapped for floor weapons, and RB "pick-ups" / reload taps were melees. NHOOK_BOT_MELEEPAD /
// NHOOK_BOT_PICKPAD = other buttons (hex bits) for another layout; NHOOK_BOT_NOMELEE=1 also brings back the old B melee.
static uint32_t bot_meleepad(void) { static uint32_t v = 1; if (v == 1) { const char* e = getenv("NHOOK_BOT_MELEEPAD"); v = e ? (uint32_t)strtoul(e, 0, 16) : 0x20; } return v; }
static uint32_t bot_pickpad(void) { static uint32_t v = 1; if (v == 1) { const char* e = getenv("NHOOK_BOT_PICKPAD"); v = e ? (uint32_t)strtoul(e, 0, 16) : 0x02; } return v; }
static void shadow_log(double now, const NovObj* o, const NovObj* t, float tdist, int engaged, float yaw, float pitch, float tx, float ty, float yaw0, float pitch0, uint64_t rec) {
    static FILE* f; static double tlast, t0seen; static int fail;
    if (now - tlast < 0.1 || fail) return; tlast = now;
    if (!f || g_gameplay_t0 != t0seen) { if (f) fclose(f); f = 0; t0seen = g_gameplay_t0; const char* h = getenv("HOME"); char d[1100], p[1300];
        snprintf(d, sizeof d, "%s/Library/Application Support/ReclaimerMac/tracks/shadow", h ? h : "/tmp"); mkdir(d, 0755);
        time_t tt = time(0); struct tm tm; localtime_r(&tt, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
        snprintf(p, sizeof p, "%s/%s_%s.jsonl", d, ts, g_st_curmap[0] ? g_st_curmap : "map"); f = fopen(p, "w"); if (!f) { fail = 1; return; }
        nlog("[nhook] [shadow] recording -> %s", p); }
    uint64_t btn = __atomic_exchange_n(&g_inp_b8_acc, 0, __ATOMIC_RELAXED); float rx = 0, ry = 0;   // (the buttons as packed since the last sample)
    if (rec) { hb_read(rec + 0xDC, &rx, 4); hb_read(rec + 0xE0, &ry, 4); hb_read(rec + 0xD4, &yaw0, 4); hb_read(rec + 0xD8, &pitch0, 4); }   // (the player's own aim)
    fprintf(f, "{\"t\":%.2f,\"pos\":[%.2f,%.2f,%.2f],\"w\":\"%s\",\"sh\":%.2f", now - g_gameplay_t0, o->c[0], o->c[1], o->c[2], g_inv.held_c ? g_inv.held_c : "", o->shield);
    if (t) fprintf(f, ",\"tg\":\"%08x\",\"tp\":[%.2f,%.2f,%.2f],\"tsh\":%.2f,\"d\":%.2f", t->handle, t->c[0], t->c[1], t->c[2], t->shield, tdist);
    fprintf(f, ",\"eng\":%d,\"fire\":%d,\"mel\":%d,\"empty\":%d,\"want\":[%.2f,%.2f,%.3f,%.3f],\"you\":[%.2f,%.2f,%.3f,%.3f,\"%llx\"]}\n",
            engaged, g_bot_trig, now < g_bot_melee_until, g_bot_empty, tx, ty, yaw, pitch, rx, ry, yaw0, pitch0, (unsigned long long)btn);
    { static double tf; if (now - tf > 2.0) { tf = now; fflush(f); } }
    g_sh_tgt = t ? t->handle : 0; g_sh_d = tdist; g_sh_fire = g_bot_trig; g_sh_t = now; }
static void bot_step_body(void);
static void bot_step(void) { g_pad_written = 0; bot_step_body(); if (g_vpad_on && !g_pad_written) nh_vpad_set(0, 0, 0, 0, 0, 0, 0); }   // (idle: the pad let go)
// CAMPING (2026-11-09, the user: "can you have the walkers send a chat message when they *decide* to camp, and wait for a user. just
// send a chat message that says i'm camping..."). Camping = the walker's OWN decision is to stand: the throttle it chose is under 0.1
// (a stuck walker is still trying to walk, so being stuck never counts; neither does a fight in sight, nor a step without a path) for
// 2 s -- holding the high ground over an enemy below is the main case. One line a spell, at most one every 30 s: "<nickname>: i'm
// camping..." as a [Server] line through our server's RCON say (the app's own player-chat send is not found yet), from a spawned
// `<this app> --rcon <addr> <password> say <text>` on its own thread (no DYLD_* passed on). NHOOK_BOT_CAMPCHAT=0 = off;
// NHOOK_CHAT_RCON=<host:port> (default 127.0.0.1:49190: our server), NHOOK_CHAT_RCON_PWFILE (default
// ReclaimerMac/native/test-dedicated/rcon-password.txt). Every decision is logged ([camp]) whether or not the line goes out.
static void* camp_say_thread(void* a) {
    char* msg = a; char exe[1024]; uint32_t n = sizeof exe; char pw[128] = "", pfd[1024]; const char* home = getenv("HOME") ? getenv("HOME") : "/tmp";
    if (!bot_private_addr(g_srv_addr)) { nlog("[nhook] [camp] \"%s\" not said: not our server (the console line is our server's)", msg); free(msg); return 0; }
    const char* pf = getenv("NHOOK_CHAT_RCON_PWFILE"); if (!pf) { snprintf(pfd, sizeof pfd, "%s/Library/Application Support/ReclaimerMac/native/test-dedicated/rcon-password.txt", home); pf = pfd; }
    FILE* f = fopen(pf, "r"); if (f) { if (fgets(pw, sizeof pw, f)) pw[strcspn(pw, "\r\n")] = 0; fclose(f); }
    if (!pw[0] || _NSGetExecutablePath(exe, &n) != 0) { nlog("[nhook] [camp] no RCON password file or no app path: \"%s\" stays in this log", msg); free(msg); return 0; }
    const char* addr = getenv("NHOOK_CHAT_RCON"); if (!addr) addr = "127.0.0.1:49190";
    char* argv[] = {exe, "--rcon", (char*)addr, pw, "say", msg, 0}; char he[1100]; snprintf(he, sizeof he, "HOME=%s", home);
    char* envp[] = {he, "PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0};
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0); posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid; int rc = posix_spawn(&pid, exe, &fa, 0, argv, envp); posix_spawn_file_actions_destroy(&fa);
    if (rc == 0) { int st = 0; waitpid(pid, &st, 0); nlog("[nhook] [camp] said \"%s\" (rcon exit %d)", msg, WIFEXITED(st) ? WEXITSTATUS(st) : -1); }
    else nlog("[nhook] [camp] could not run the app's --rcon: %s", strerror(rc));
    free(msg); return 0; }
static void bot_camp_tick(double now, float tx, float ty, int out, int engaged, int hold_high) {
    static int off = -1; if (off < 0) { const char* e = getenv("NHOOK_BOT_CAMPCHAT"); off = e && !strcmp(e, "0"); }
    static double tlast, tstill, tsaid = -1e9; static int spell;
    if (now - tlast > 0.5) { tstill = 0; spell = 0; }   // (steps skipped: dead, in the lobby)
    tlast = now;
    if (!((out & 2) && fabsf(tx) < 0.1f && fabsf(ty) < 0.1f && !engaged)) { tstill = 0; spell = 0; return; }
    if (tstill == 0) tstill = now;
    if (spell || now - tstill < 2.0) return;
    spell = 1; int say = !off && now - tsaid >= 30.0;
    nlog("[nhook] [camp] decided to camp (%s) for 2 s%s", hold_high ? "holding the high ground over an enemy below" : "standing by choice, no enemy in sight",
         say ? ": saying so" : off ? "" : " (said so under 30 s ago)");
    if (!say) return; tsaid = now;
    char* msg = malloc(160); if (!msg) return; snprintf(msg, 160, "%s: i'm camping...", g_nick[0] ? g_nick : "walker");
    pthread_t th; if (pthread_create(&th, 0, camp_say_thread, msg) == 0) pthread_detach(th); else free(msg); }
// THE GUNNER (2026-11-09, the user: "every time my guy is in a warthog, it must come find me and get into the turret gunner. I want it
// to train as my gunner as I drive it around after the other walkers"; then "third got into a banshee on its own... now it is stuck.
// have it get out, and ONLY get in *MY* warthog", "one of the walkers tried to board me, but jumped into the passenger seat"). The act
// bit (+0x2c bit 0) DOES enter vehicles: a pick-up hold next to a Banshee put Third in it, a walker's next to the user's Warthog put
// one in its passenger seat, and seated the walker went idle. Now:
//   - SEATED anywhere but its teammate's Warthog (or that Warthog's turret) a walker holds act to get out (0.8 s, again every 3 s);
//   - no pick-up hold / reload tap within 3.5 units of a vehicle, unless boarding that Warthog;
//   - a TEAMMATE driving a Warthog whose turret nobody mans: the walker walks there (the pathfinder, then straight in the last 4
//     units) to a point 1.3 units behind the turret, facing it, and pulses act (0.4 s of each second) -- the nearest seat entry from
//     behind is the gunner's. FFA has no teammates: it never boards an enemy's vehicle;
//   - ON THE TURRET (or in that Warthog's passenger seat): the nearest enemy, preferring one in sight of the camera (line of sight,
//     the turret ignored), its chest led by its velocity, the view turned at most 4 rad/s, the trigger held within 2.5 deg (or the
//     target's size) and 60 units. Out again when the teammate leaves the Warthog.
// NHOOK_BOT_GUNNER=0 = none of it (the old: seated = idle; pick-ups anywhere). [gunner] log lines.
static NovObj g_gv[256]; static int g_gvn; static double g_gvt; static int g_gun_press_ok;
static int g_gun_exit_bit_last = -1; static double g_gun_exit_bit_t;   // (the driver's-seat exit tries: the last record bit and when)   // (gun_board: right behind the turret -- act now)
static void gun_vlist(double now) { if (now - g_gvt < 0.2) return; g_gvt = now;
    pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 2; g_gvn = nov_collect(g_gv, 256); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx); }
static const NovObj* gun_v(uint32_t h) { if (h == 0xFFFFFFFFu) return 0; for (int i = 0; i < g_gvn; i++) if (g_gv[i].handle == h) return &g_gv[i]; return 0; }
static int gun_on(void) { static int v = -1; if (v < 0) { const char* e = getenv("NHOOK_BOT_GUNNER"); v = !(e && !strcmp(e, "0")); } return v; }
static int gun_near_veh(const float* c, uint32_t except) {   // a vehicle within 3.5 units across, 3 up / down (no act there)
    if (!gun_on()) return 0;
    for (int i = 0; i < g_gvn; i++) { const NovObj* v = &g_gv[i]; if (v->dead || v->handle == except || v->parent != 0xFFFFFFFFu) continue;
        float dx = v->c[0] - c[0], dy = v->c[1] - c[1], dz = v->c[2] - c[2];   // (to the HULL: a Wraith is wide -- a walker touching one was 3.5+ from its centre)
        if (sqrtf(dx * dx + dy * dy) - v->r < 2.0f && fabsf(dz) < 3.0f + 0.5f * v->r) return 1; }
    return 0; }
// the Warthog a TEAMMATE sits in (straight in it: driver / passenger -- not on its turret) and that Warthog's turret
static int gun_find(const NovObj* objs, int n, int me, const NovObj** wh, const NovObj** tu) {
    const NovObj* o = &objs[me];
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (i == me || t->type || t->dead || t->seat < 0 || bot_enemy(o, t)) continue;
        const NovObj* v = gun_v(t->parent); if (!v || v->parent != 0xFFFFFFFFu) continue; const char* vn = hb_tag_name(v->def);
        if (!nav_ci_has(vn, "warthog") && !nav_ci_has(vn, "wraith") && !nav_ci_has(vn, "mongoose")) continue;   // (02:20: "treat the wraith the same way if I jump in, or mongoose")
        const NovObj* q = 0; for (int k = 0; k < g_gvn; k++) if (g_gv[k].parent == v->handle) { q = &g_gv[k]; break; }
        *wh = v; *tu = q; return 1; }
    return 0; }
static int gun_seated(double now, const NovObj* objs, int n, int me, const float* cam, float yaw0, float pitch0) {   // -> 1: handled
    gun_vlist(now); const NovObj* o = &objs[me]; const NovObj* mv = gun_v(o->parent); const NovObj *wh = 0, *tu = 0;
    int served = gun_find(objs, n, me, &wh, &tu), mine = served && mv && ((tu && mv->handle == tu->handle) || mv->handle == wh->handle);
    static double texit, tlog, tmiss[4]; static int ex; double exd; g_bot_melee_until = 0; g_bot_switch_until = 0;
    // (01:58, the user: "third needs to understand turret vs passenger seat. he's in the wrong spot": the Warthog itself is the driver's
    //  and the passenger's; the gunner sits on the turret (the vehicle parented to it). In the passenger seat with the turret free: out
    //  and board again from behind -- after 4 such misses in a minute it stays a passenger)
    // NO DRIVING (02:45, the user: "walkers should not attempt to drive vehicles until they study the player driving corpus from historic
    //  games"; a walker's tap landed in a Wraith its teammate had just left -- the driver's seat): seat 0 of a vehicle with no parent is
    //  the driver's -- out at once, whoever's vehicle it is
    // (03:05: in a Wraith's driver's seat neither a tap nor a hold of the packet's act bit got a walker out (turret seats: they do).
    //  So the tries go round: the packet's act tap, its hold, then candidate CONTROL-RECORD bits (+0xC8, where fire / jump / melee work)
    //  held 0.5 s each -- the bit that ends the seat is logged, kept in <battles>/exit-bit.txt and tried first from then on)
    if (mv && mv->parent == 0xFFFFFFFFu && o->seat == 0) {
        static const int cand[] = {2, 3, 4, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 22, 23, 25, 26, 27, 28, 31}; enum { NC = sizeof cand / sizeof cand[0] };
        static int learned = -2; static int lastbit = -1; static double tlast; static char ep[1024];
        if (learned == -2) { snprintf(ep, sizeof ep, "%s/Library/Application Support/ReclaimerMac/battles/exit-bit.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
            FILE* f = fopen(ep, "r"); learned = -1; if (f) { if (fscanf(f, "%d", &learned) != 1) learned = -1; fclose(f); } }
        static uint64_t holdbits; static double hold_until;
        if (now - texit > 1.5) { texit = now; int step = ex++ % (2 + NC + (learned >= 0)); int bit = -1; double d2 = 0.25;
            if (learned >= 0 && step == 0) bit = learned; else { if (learned >= 0) step--;
                if (step == 0) d2 = 0.25; else if (step == 1) d2 = 1.2; else bit = cand[step - 2]; }
            if (bit < 0) { g_bot_act_until = now + d2; g_gun_exit_until = now + d2 + 0.1; holdbits = 0; }
            else { holdbits = 1ull << bit; hold_until = now + 0.5; }
            lastbit = bit; tlast = now;
            nlog("[nhook] [gunner] in the DRIVER's seat of %s: walkers do not drive -- getting out (%s%d%s)", hb_tag_name(mv->def), bit < 0 ? "packet act " : "record bit ", bit < 0 ? (int)(d2 * 1000) : bit, bit < 0 ? " ms" : ""); }
        g_gun_exit_bit_last = lastbit; g_gun_exit_bit_t = tlast;
        g_bot_yaw = yaw0; g_bot_pitch = pitch0; g_bot_btn = now < hold_until ? holdbits : 0; g_bot_fire = 0; g_bot_out = 1 | (now < hold_until ? 4 : 0); g_nav_act = 0; return 1; }
    int passenger = mine && tu && mv->handle == wh->handle, tfree = 1;
    if (passenger) for (int i = 0; i < n; i++) if (objs[i].type == 0 && !objs[i].dead && objs[i].parent == tu->handle) tfree = 0;
    // (an exit: a 0.25 s tap and a 1.2 s hold in turn -- Second's 0.8 s holds never got it out of a Banshee)
    if (passenger && tfree && now - texit > 3.0 && !(tmiss[0] > 0 && now - tmiss[0] < 60.0)) {
        memmove(tmiss, tmiss + 1, 3 * sizeof(double)); tmiss[3] = now; texit = now; exd = (++ex & 1) ? 0.25 : 1.2; g_bot_act_until = now + exd; g_gun_exit_until = now + exd + 0.1;
        nlog("[nhook] [gunner] in the PASSENGER seat with the turret free: getting out to board the turret from behind");
        g_bot_yaw = yaw0; g_bot_pitch = pitch0; g_bot_btn = 0; g_bot_fire = 0; g_bot_out = 1; g_nav_act = 0; return 1; }
    if (!mine) {
        if (now - texit > 3.0) { texit = now; exd = (++ex & 1) ? 0.25 : 1.2; g_bot_act_until = now + exd; g_gun_exit_until = now + exd + 0.1;
            nlog("[nhook] [gunner] in %s (seat %d): not my teammate's vehicle -- getting out (act %.2f s)", mv ? hb_tag_name(mv->def) : "a vehicle", o->seat, exd); }
        g_bot_yaw = yaw0; g_bot_pitch = pitch0; g_bot_btn = 0; g_bot_fire = 0; g_bot_out = 1; g_nav_act = 0; return 1; }
    g_bot_act_until = 0;
    // the target: the nearest enemy, one out of sight counting 4x as far
    int bi = -1; float bs = 1e30f;
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
        float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; if (d > 60.0f * 60.0f) continue;
        int v = los_get(t->handle, now); float hp = (t->shield > 0 ? t->shield : 0) + (t->health > 0 ? t->health : 0);   // (the easiest: in sight, near, hurt)
        float sc = d * (v == 0 ? 16.0f : 1.0f) * (0.5f + 0.25f * (hp < 2.0f ? hp : 2.0f)); if (sc < bs) { bs = sc; bi = i; } }
    { static double tput; if (g_los_t > tput && g_los_req == g_los_done) { tput = g_los_t; los_put(g_los_res_tgt, g_los_res, g_los_t); } }
    static double tlos; static int rr;
    if (now - tlos > 0.07 && g_los_req == g_los_done) {   // the line from the camera: the target every other time, else another enemy in turn
        const NovObj* q = bi >= 0 ? &objs[bi] : 0; rr++;
        if (!(rr & 1) || !q) { int cnt = 0; for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue; if (cnt++ == (rr >> 1) % 8) { q = t; break; } } }
        if (q) { tlos = now; g_los_p[0] = cam[0]; g_los_p[1] = cam[1]; g_los_p[2] = cam[2]; g_los_v[0] = q->c[0] - cam[0]; g_los_v[1] = q->c[1] - cam[1]; g_los_v[2] = q->c[2] + 0.3f * q->r - cam[2];
                 g_los_ign[0] = mv->handle; g_los_ign[1] = q->handle; g_los_tgt = q->handle; g_los_req++; } }
    static double tlast; double dt = tlast > 0 && now - tlast < 0.5 ? now - tlast : 0.033; tlast = now;
    float yaw = yaw0, pitch = pitch0; int fire = 0;
    if (bi >= 0) { const NovObj* t = &objs[bi]; float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], d = sqrtf(dx * dx + dy * dy + (t->c[2] - cam[2]) * (t->c[2] - cam[2]));
        float lt = d / 300.0f, px = t->c[0] + t->vel[0] * lt - cam[0], py = t->c[1] + t->vel[1] * lt - cam[1], pz = t->c[2] + 0.3f * t->r + t->vel[2] * lt - cam[2];   // (09:05: +0x74 is units a SECOND -- measured against position: x30 led 30x too far)
        float ty_ = atan2f(py, px), tp_ = atan2f(pz, sqrtf(px * px + py * py)), mx = (float)dt * 12.0f;   // (02:20: "increase look sensitivity for aiming while in a vehicle": 4 -> 12 rad/s)
        float dyw = bot_wrap(ty_ - yaw0), dpt = tp_ - pitch0; yaw = yaw0 + (dyw > mx ? mx : dyw < -mx ? -mx : dyw); pitch = pitch0 + (dpt > mx ? mx : dpt < -mx ? -mx : dpt);
        float err = sqrtf(bot_wrap(ty_ - yaw) * bot_wrap(ty_ - yaw) + (tp_ - pitch) * (tp_ - pitch)), size = atanf(t->r / (d > 0.5f ? d : 0.5f));
        int vis = los_get(t->handle, now); fire = vis != 0 && d < 60.0f && err < fmaxf(0.0436f, size);
        if (bot_log() && now - tlog > 2.0) { tlog = now; nlog("[nhook] [gunner] %s: target %08x at %.1f units, %s, aim error %.1f deg%s", tu && mv->handle == tu->handle ? "on the turret" : "in the Warthog",
                                                         t->handle, d, vis > 0 ? "in sight" : vis == 0 ? "hidden" : "sight unknown", err * 57.2958f, fire ? ", FIRING" : ""); } }
    else if (bot_log() && now - tlog > 4.0) { tlog = now; nlog("[nhook] [gunner] %s: no enemy within 60 units", tu && mv->handle == tu->handle ? "on the turret" : "in the Warthog"); }
    static uint64_t fb = 0; if (!fb) { const char* e = getenv("NHOOK_BOT_FIREBIT"); fb = e ? strtoull(e, 0, 16) : 0x80000ull; }
    g_bot_yaw = yaw; g_bot_pitch = pitch; g_bot_btn = fire ? fb : 0; g_bot_fire = fire; g_bot_trig = fire; g_bot_out = 1 | (fire ? 4 : 0); g_nav_act = 0; return 1; }
// boarding: a teammate's Warthog with its turret free -> 1, the point 1.3 units behind the turret (bpt), the turret (face), the distance
static int gun_board(double now, const NovObj* objs, int n, int me, float* bpt, float* face, float* bd, uint32_t* hog) {
    if (!gun_on()) return 0; gun_vlist(now); const NovObj *wh = 0, *tu = 0;
    if (!gun_find(objs, n, me, &wh, &tu)) return 0;
    if (tu) { for (int i = 0; i < n; i++) if (objs[i].type == 0 && !objs[i].dead && objs[i].parent == tu->handle && i != me) return 0; }   // (manned)
    else { int k = 0; for (int i = 0; i < n; i++) if (objs[i].type == 0 && !objs[i].dead && objs[i].parent == wh->handle && i != me) k++; if (k >= 2) return 0; tu = wh; }   // (no turret -- a Mongoose: its passenger seat, from behind)
    float f[3] = {1, 0, 0}; if (wh->p) hb_read(wh->p + 0x5c, f, 12); float fl = sqrtf(f[0] * f[0] + f[1] * f[1]); if (fl > 0.1f) { f[0] /= fl; f[1] /= fl; } else { f[0] = 1; f[1] = 0; }
    // (01:58: the act press anywhere within 1.5 units of a point 1.3 behind the turret reached the passenger door. Now two stages: well
    //  behind first (1.6 units straight back from the turret), then in to 0.6 behind it; act only there -- within 0.7 of it and behind
    //  the turret -- where the gunner's entry is the one in reach)
    //  Stages: beside / in front of it -> round the side first (1.8 units out, 1 back); behind -> 1.6 back; lined up there (0.9+ back,
    //  under 0.8 off the line) -> in to 0.6 back until it drifts off the line (under 0.15 back or 1.2 off)
    const float* c = objs[me].c; float rx = c[0] - tu->c[0], ry = c[1] - tu->c[1], along = rx * f[0] + ry * f[1], side = rx * -f[1] + ry * f[0];
    static int stage; if (stage == 0 && along < -0.9f && fabsf(side) < 0.8f) stage = 1; else if (stage == 1 && (along > -0.15f || fabsf(side) > 1.2f)) stage = 0;
    float bk = stage ? (tu == wh ? 0.8f : 0.6f) : 1.6f, sd = 0;
    if (!stage && along > -0.3f && fabsf(side) < 1.4f) { bk = 1.0f; sd = side >= 0 ? 1.8f : -1.8f; }   // (round the side, not through the hull)
    bpt[0] = tu->c[0] - f[0] * bk - f[1] * sd; bpt[1] = tu->c[1] - f[1] * bk + f[0] * sd; bpt[2] = wh->c[2]; memcpy(face, tu->c, 12); *hog = wh->handle;
    float dx = bpt[0] - c[0], dy = bpt[1] - c[1]; *bd = sqrtf(dx * dx + dy * dy);
    g_gun_press_ok = stage && *bd < 0.7f && along < -0.3f; return 1; }
// HONK SCAN (02:35): what a teammate's honk changes in ITS unit and its vehicle as THIS walker's game sees them is not known. While a
// teammate drives (sits straight in a vehicle), the walker keeps the driver's unit datum (0x380 bytes) and the vehicle's and logs each
// byte that leaves its value and returns within 1.5 s (a pulse) -- bytes pulsing more than 10 times a minute (animation, timers) are
// skipped -> "[honkscan] unit|vehicle +0xNNN a -> b, back after N ms"; the player's own [input] bit-5 presses give the honk times to
// match. Then the honk field replaces the scan. NHOOK_HONK_SCAN=0 = off.
static void honk_scan(double now, const NovObj* objs, int n, int me) {
    static int off = -1; if (off < 0) { const char* e = getenv("NHOOK_HONK_SCAN"); off = e && !strcmp(e, "0"); } if (off || me < 0) return;
    static uint32_t hu = 0xFFFFFFFFu, hv = 0xFFFFFFFFu; static uint8_t base[2][0x380], away[2][0x380]; static double taway[2][0x380], pul[2][0x380][10]; static int have;
    const NovObj* o = &objs[me]; const NovObj* drv = 0; const NovObj* veh = 0; gun_vlist(now);
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (i == me || t->type || t->dead || t->parent == 0xFFFFFFFFu || bot_enemy(o, t)) continue;
        const NovObj* v = gun_v(t->parent); if (v && v->parent == 0xFFFFFFFFu) { drv = t; veh = v; break; } }
    if (!drv) { hu = hv = 0xFFFFFFFFu; have = 0; return; }
    uint8_t cur[2][0x380]; if (!hb_read(drv->p, cur[0], 0x380) || !hb_read(veh->p, cur[1], 0x380)) return;
    if (drv->handle != hu || veh->handle != hv || !have) { hu = drv->handle; hv = veh->handle; memcpy(base, cur, sizeof base); memset(away, 0, sizeof away); memset(pul, 0, sizeof pul); have = 1;
        nlog("[nhook] [honkscan] watching my teammate %08x in %s %08x: honk a few times", hu, hb_tag_name(veh->def), hv); return; }
    for (int w = 0; w < 2; w++) for (int i = 0; i < 0x380; i++) {
        if (cur[w][i] != base[w][i]) { if (!away[w][i]) { away[w][i] = 1; taway[w][i] = now; } else if (now - taway[w][i] > 1.5) { base[w][i] = cur[w][i]; away[w][i] = 0; } continue; }
        if (!away[w][i]) continue; away[w][i] = 0; double dur = now - taway[w][i];
        int recent = 0; for (int k = 0; k < 10; k++) if (now - pul[w][i][k] < 60.0) recent++;
        memmove(pul[w][i], pul[w][i] + 1, 9 * sizeof(double)); pul[w][i][9] = now;
        if (recent < 10) nlog("[nhook] [honkscan] %s +0x%03x pulsed for %.0f ms (wall %.2f)", w ? "vehicle" : "unit", i, dur * 1e3, wall_s()); }
}
static int ob_is_ball(const char* nm) { return nm && (nav_ci_has(nm, "ball") || nav_ci_has(nm, "skull")); }
static void ob_scan(double now, const NovObj* objs, int n, int me) {
    static double t; if (now - t < 0.3) return; t = now;
    static NovObj w[512]; pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 3; int nw = nov_collect(w, 512); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx);
    const NovObj* o = &objs[me]; int st = 0; uint32_t car = 0; float bd = 1e30f;
    for (int i = 0; i < nw; i++) { if (!ob_is_ball(hb_tag_name(w[i].def))) continue;   // (two balls: the nearest)
        int s2 = 1; uint32_t c2 = 0; float p[3]; memcpy(p, w[i].c, 12);
        if (w[i].parent != 0xFFFFFFFFu) { c2 = w[i].parent; if (c2 == o->handle) { s2 = 2; memcpy(p, o->c, 12); }
            else for (int k = 0; k < n; k++) if (objs[k].handle == c2) { s2 = bot_enemy(o, &objs[k]) ? 4 : 3; memcpy(p, objs[k].c, 12); break; } }
        float dx = p[0] - o->c[0], dy = p[1] - o->c[1], d = s2 == 2 ? -1 : dx * dx + dy * dy;
        if (d < bd) { bd = d; st = s2; car = c2; memcpy(g_ob_pos, p, 12); } }
    g_ob_enemy_carrier = st == 4 ? car : 0;
    if (st != g_ob_state && bot_log()) nlog("[nhook] [ball] %s", st == 0 ? "no ball seen" : st == 1 ? "the ball is loose: going for it" : st == 2 ? "I carry the ball: running, melee only"
                                            : st == 3 ? "a teammate carries the ball: escorting" : "an enemy carries the ball: hunting the carrier");
    g_ob_state = st; }
// ---- THE LOCOMOTION ARBITER (2026-11-09, brain stage 1; the user: "I feel like native/nbot_* should be its own kernel or something. it
// looks like a pile of if then statements now" -- yes, start on it). Six blocks used to write the walker's path one after another
// (boarding, the wingman, the ball, the weapon fetch, the fight's moves, levels apart) and the last writer won: the puppy (following
// beat fighting) and the AR forever (following overwrote the fetch) both came from that. Now each BIDS for the legs -- a score and,
// for a path, a point -- and the highest bid moves the walker; aim and buttons keep their own code for now. Scores are live knobs:
// NHOOK_U_BOARD 95, _FOLLOW_FAR 90 (the leader beyond the leash), _BALL_RUN 80, _BALL_FETCH 75, _HOLD 65, _FIGHT 60, _BALL_HUNT 50,
// _FETCH 45 (a better weapon), _BALL_ESCORT 30, _FOLLOW_NEAR 20 (closing up inside the leash), _HUNT 10 (the goal draw, always).
// A change of winner: "[arb] <winner> <score> > <others>" (at most twice a second).
enum { ARB_HUNT, ARB_FIGHT, ARB_FOLLOW, ARB_FETCH, ARB_BALL, ARB_BOARD, ARB_HOLD, ARB_N };
typedef struct { struct { float score, pt[3]; const char* why; int on; } b[ARB_N]; } Arb;
static void arb_reset(Arb* a) { memset(a, 0, sizeof *a); }
static void arb_bid(Arb* a, int k, float score, const float* pt, const char* why) {
    if (a->b[k].on && a->b[k].score >= score) return;   // (one bid per behaviour: its best)
    a->b[k].on = 1; a->b[k].score = score; a->b[k].why = why; if (pt) memcpy(a->b[k].pt, pt, 12); }
static const char* g_arb_why = "-";   // (the last winner's reason: diagnostics)
static char g_pick_name[64];         // (the weapon being fetched: diagnostics)
static int arb_choose(Arb* a, double now) {
    int w = ARB_HUNT; float ws = -1e30f; for (int k = 0; k < ARB_N; k++) if (a->b[k].on && a->b[k].score > ws) { ws = a->b[k].score; w = k; }
    g_arb_why = a->b[w].why ? a->b[w].why : "?";
    static int last = -1; static double tl;
    if (w != last && now - tl > 0.5) { tl = now; last = w; char b[300]; int q = snprintf(b, sizeof b, "[nhook] [arb] %s %.0f", a->b[w].why ? a->b[w].why : "?", ws);
        for (int k = 0; k < ARB_N && q < (int)sizeof b - 40; k++) if (k != w && a->b[k].on) q += snprintf(b + q, sizeof b - (size_t)q, " > %s %.0f", a->b[k].why, a->b[k].score);
        nlog("%s", b); }
    return w; }
// The target's HEAD (2026-11-09 07:55; the user: "you can see all I can see for aim, tune, etc re: hitbox. don't make me a crutch in any
// way"): the head region of the target's collision model posed by its node matrices -- the same geometry and the same "head" (the region
// whose nodes sit highest) the aim meter judges every shot with (noverlay.c nov_aim_tick_body) -- its centre and mean radius. The fighter
// aims there, NHOOK_BOT_HEAD_DZ head radii above it (live, 0; follow-study.py hones it on head hits a shot), instead of a guessed height
// over the bounding sphere (NHOOK_BOT_AIM_HEAD, still the fallback). The offset from the object's centre is kept 50 ms per target and
// added to its fresh centre. BRAIN only: there the bot thread owns its own hb_geom cache (the brain reads the map the body names); in the
// body hb_geom belongs to the render / snapshot thread. NHOOK_BOT_HEADAIM=0 (live) = the old height.
static int bot_head(const NovObj* t, double now, float* out) {   // -> 1: out = head centre xyz + radius
#ifndef NH_BRAIN
    (void)t; (void)now; (void)out; return 0;
#else
    static uint32_t ch = 0xFFFFFFFFu; static double ct; static float co[4]; static int cok;
    if (t->handle != ch || now - ct > 0.05) { ch = t->handle; ct = now; cok = 0;
        HbGeom* g = t->def ? hb_geom(t->def) : 0; int nn = t->nmsz / 52;
        static float mt[128 * 13];
        if (g && !(t->nmsz % 52) && nn >= 1 && nn <= 128 && nn > g->maxnode && t->nmoff >= 0x100 && hb_read(t->p + t->nmoff, mt, (size_t)nn * 52) && fabsf(mt[0] - 1.0f) <= 0.1f) {
            uint8_t perm[32] = {0}; int rc = t->rcount > 0 && t->rcount <= 32 ? t->rcount : 0; if (rc && !hb_read(t->p + t->roff + (size_t)rc * 8, perm, (size_t)rc)) rc = 0;
            uint8_t sel[256]; memset(sel, 0, sizeof sel);
            if (g->nhreg > 0 && rc > 0) for (int i = 0; i < g->nhreg && i < rc; i++) { int pi = perm[i]; if (pi >= g->hnperm[i]) pi = 0;
                int cr = g->hcreg[i], cp = g->hnperm[i] ? g->hcperm[i][pi] : 0; if (cr >= 0 && cr < 256 && cp >= 0) sel[cr] = (uint8_t)cp; }
            float zs[256] = {0}; int zc[256] = {0};
            for (int q = 0; q < g->nseg; q++) { if (g->perm[q] != sel[g->reg[q]]) continue; zs[g->reg[q]] += mt[g->node[q] * 13 + 12]; zc[g->reg[q]]++; }
            int head = -1; float hb = -1e30f; for (int q = 0; q < g->nreg && q < 256; q++) if (zc[q] && zs[q] / zc[q] > hb) { hb = zs[q] / zc[q]; head = q; }
            static float pts[2048][3]; int np = 0; float c[3] = {0, 0, 0};
            if (head >= 0) for (int q = 0; q < g->nseg && np + 2 <= 2048; q++) { if (g->reg[q] != head || g->perm[q] != sel[head]) continue;
                const float* M = mt + g->node[q] * 13, * P = g->pts + q * 6;
                for (int e = 0; e < 2; e++) { const float* v = P + e * 3; float* w = pts[np++];
                    for (int k = 0; k < 3; k++) w[k] = M[10 + k] + M[0] * (v[0] * M[1 + k] + v[1] * M[4 + k] + v[2] * M[7 + k]);
                    c[0] += w[0]; c[1] += w[1]; c[2] += w[2]; } }
            if (np >= 2) { c[0] /= np; c[1] /= np; c[2] /= np; float r = 0;
                for (int i = 0; i < np; i++) r += sqrtf((pts[i][0] - c[0]) * (pts[i][0] - c[0]) + (pts[i][1] - c[1]) * (pts[i][1] - c[1]) + (pts[i][2] - c[2]) * (pts[i][2] - c[2]));
                r /= np; float ox = c[0] - t->c[0], oy = c[1] - t->c[1], oz = c[2] - t->c[2];
                if (r > 0.02f && r < 0.6f && ox * ox + oy * oy + oz * oz < 4.0f * t->r * t->r) { co[0] = ox; co[1] = oy; co[2] = oz; co[3] = r; cok = 1; } }
            static int told; if (!told && cok) { told = 1; nlog("[nhook] [aim] aiming at the head hitbox: region %d, %.2f units above the centre, radius %.2f (%d edge points)", head, co[2], co[3], np); } }
        if (!cok) { static double tw; if (now - tw > 30) { tw = now; char mp[1024] = ""; hb_current_map(mp, sizeof mp);
            nlog("[nhook] [aim] no head hitbox for %08x (definition %08x): geometry %s, nodes %d (size %u at +0x%x), regions %d; map \"%s\"", t->handle, t->def,
                 g ? "found" : "NOT found", nn, (unsigned)t->nmsz, (unsigned)t->nmoff, t->rcount, mp); } } }
    if (!cok) return 0;
    out[0] = t->c[0] + co[0]; out[1] = t->c[1] + co[1]; out[2] = t->c[2] + co[2]; out[3] = co[3]; return 1;
#endif
}
static float g_bot_head_r = -1;   // (the current target's head radius, -1 = not known)
#include "nbrain_beh.c"   // the behaviours: each bids for the legs (brain stage 2)
static void bot_models_tick(void) {   // (live knobs: the models re-read when their files change -- every 10 s a look at the files' times)
    static double t; double now = mono_s(); if (now - t < 10.0) return; t = now; const char* h = getenv("HOME"); char p[1100]; struct stat st;
    static time_t m1, m2, m3, m4;
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/walker-model.json", h ? h : "");
    if (!getenv("NHOOK_BOT_MODEL") && stat(p, &st) == 0) { if (m1 && st.st_mtime != m1) { g_bm_done = 0; bm_load(); nlog("[nhook] [knobs] walker-model.json changed: re-read"); } m1 = st.st_mtime; }
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/engage-model.json", h ? h : "");
    if (stat(p, &st) == 0) { if (m2 && st.st_mtime != m2) { g_eg_key[0] = 0; nlog("[nhook] [knobs] engage-model.json changed: re-read at the next step"); } m2 = st.st_mtime; }
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/maps", h ? h : "");
    if (stat(p, &st) == 0) { if (m3 && st.st_mtime != m3) { g_eg_key[0] = 0; nlog("[nhook] [knobs] the per-map models changed: re-read at the next step"); } m3 = st.st_mtime; }
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/melee.json", h ? h : "");
    if (stat(p, &st) == 0) { if (m4 && st.st_mtime != m4) { g_mel.loaded = 0; mel_load(); nlog("[nhook] [knobs] melee.json changed: re-read"); } m4 = st.st_mtime; } }
static void bot_step_body(void) {
    double now = mono_s(); int spar = g_bot_spar, mode = bot_walk_ok() ? nav_mode() : 0;
    lk_tick(); bot_models_tick();   // (live knobs and models: no restart)
    static int diag0 = -1; if (diag0 < 0) diag0 = getenv("NHOOK_BOT_THROTTLE") || getenv("NHOOK_BOT_BTNSCAN") || getenv("NHOOK_BOT_FACE") || getenv("NHOOK_BOT_HOLDBTN");
    int testb = g_bot_testbtn && now < g_bot_testbtn_until, diag = diag0 || testb;
    if ((!spar && !mode && !diag) || !(bot_allowed() || g_bot_shadow) || !g_gameplay_t0 || now - g_gameplay_t0 < 3.0) { g_bot_out = 0; g_nav_act = 0; g_bot_err = -1;
        bot_idle(!spar && !mode && !diag ? "nothing on" : !bot_allowed() ? "not allowed here" : !g_gameplay_t0 ? "not in a match" : "match just started"); return; }
    if (!bot_hook_install()) { g_bot_out = 0; return; }
    if (mode) { nav_ensure(); nav_danger_update(now); if (g_nav.ok) bm_pop(g_st_curmap); }
    uint64_t blk = tls_block(), rec = 0; uint32_t meh = 0xFFFFFFFFu; float yaw0 = 0, pitch0 = 0;
    if (!blk || !hb_read(blk + 0xC0, &rec, 8) || !rec || !hb_read(rec + 0xC0, &meh, 4) || !hb_read(rec + 0xD4, &yaw0, 4) || !hb_read(rec + 0xD8, &pitch0, 4)) { g_bot_out = 0; bot_idle("no control record (TLS +0xC0)"); return; }
    float cam[3], fwd[3]; if (!nov_observer(cam, fwd)) { g_bot_out = 0; bot_idle("no observer camera"); return; }
    if (g_bot_shadow && g_sh_aim_ok) { yaw0 = g_sh_yaw; pitch0 = g_sh_pitch; }   // (shadow: the walker turns its OWN view; the player's is logged apart)
    static NovObj objs[1024]; int n = bot_units(objs, 1024), me = -1;
    for (int i = 0; i < n; i++) if (objs[i].type == 0 && objs[i].handle == meh) { me = i; break; }
    // (2026-11-08, the user's match against two walkers: a walker stood still 21% of its alive time, in spells of 14-17 s, logging
    //  "the record's unit ... is none of the units seen" ~20 times: after a respawn the control record's unit can stay stale. Then
    //  the player's own unit -- the players table, by the profile's nickname (nstats.c g_my_unit) -- is the walker's body.
    //  NHOOK_BOT_RECUNIT=1 = the record only, the old)
    { static int ro = -1; if (ro < 0) ro = getenv("NHOOK_BOT_RECUNIT") != 0;
      if (me < 0 && !ro && g_my_unit != 0xFFFFFFFFu) for (int i = 0; i < n; i++) if (objs[i].type == 0 && objs[i].handle == g_my_unit) { me = i;
          static double tl; if (bot_log() && now - tl > 10) { tl = now; nlog("[nhook] [spar] the control record's unit %08x is stale: the player's unit %08x drives", meh, g_my_unit); } break; } }
    static int32_t path[8192]; static int plen, pidx, goal = -1; static double tplan, tstat, tjump, tback, tstuck[4]; static float bstr; static float hist[64][3]; static double histt[64]; static int hh;
    gl_life(now, me >= 0 && !objs[me].dead, me >= 0 ? objs[me].c : 0, 0, 0);   // (nbot_goals.c: deaths, respawns, failed zones)
    // seated = a seat index OR a parent (02:20: on the turret the walker went on fighting on foot -- "stuck" in one spot -- after a burst:
    //  the seat field alone missed it). The state is logged at each change with every handle it is read from
    int seated = me >= 0 && !objs[me].dead && (objs[me].seat >= 0 || objs[me].parent != 0xFFFFFFFFu); if (seated) g_bot_seat_t = now;
    { static int was = -1; if (me >= 0 && seated != was) {
        if (was == 1 && !seated && g_gun_exit_bit_last >= 0 && now - g_gun_exit_bit_t < 1.4) {   // (out within a try of a record bit: that bit)
            char ep[1024]; snprintf(ep, sizeof ep, "%s/Library/Application Support/ReclaimerMac/battles/exit-bit.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
            FILE* f = fopen(ep, "w"); if (f) { fprintf(f, "%d\n", g_gun_exit_bit_last); fclose(f); }
            nlog("[nhook] [gunner] OUT of the driver's seat right after control-record bit %d: kept as the exit (%s)", g_gun_exit_bit_last, ep); }
        was = seated; gun_vlist(now); const NovObj* pv = gun_v(objs[me].parent);
        nlog("[nhook] [gunner] seated: %s (my unit %08x, the record's %08x, the player's %08x; seat %d, parent %08x %s)", seated ? "yes" : "no", objs[me].handle, meh, g_my_unit,
             objs[me].seat, objs[me].parent, pv ? hb_tag_name(pv->def) : "-"); } }
    honk_scan(now, objs, n, me);   // (02:35: finding the field a teammate's honk changes, above)
    nade_check(now, objs, n);   // (the last grenade's outcome, above)
    if (seated && gun_on() && !g_bot_shadow && gun_seated(now, objs, n, me, cam, yaw0, pitch0)) { plen = 0; g_bot_err = -1; return; }   // (the gunner, above)
    if (me < 0 || objs[me].dead || seated) { g_bot_out = 0; g_nav_act = 0; plen = 0; g_bot_err = -1;   // dead, respawning or in a vehicle
        static char w[96]; snprintf(w, sizeof w, me < 0 ? "the record's unit %08x is none of the %d units seen" : objs[me].dead ? "dead" : "in a vehicle", meh, n); bot_idle(me < 0 ? w : objs[me].dead ? "dead" : "in a vehicle"); return; }
    const NovObj* o = &objs[me]; int out = 0; float yaw = yaw0, pitch = pitch0; g_bot_unit_p = o->p;
    { static int noinv = -1; if (noinv < 0) noinv = getenv("NHOOK_BOT_NOINV") != 0; if (!noinv) { ammo_learn(now, o); bot_inv(o, &g_inv); } else memset(&g_inv, 0, sizeof g_inv);   // (NHOOK_BOT_NOINV=1: blind to it, the old walker)
      if (bot_log()) { static BotInv last; static double tl; if (now - tl > 5.0 || last.held != g_inv.held || last.os != g_inv.os || last.camo != g_inv.camo || last.gren != g_inv.gren) { tl = now; last = g_inv;
          nlog("[nhook] [inv] holding %s (%s, %d rounds), other %s (%s); grenades %d (frag %d, plasma %d)%s%s", g_inv.held ? g_inv.held : "-", g_inv.held_c, g_inv.ammo, g_inv.other ? g_inv.other : "-", g_inv.other_c,
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
        // (2026-11-09, the user: "I feel like it sucks at hitting the head hitbox": the user's client measured 120 of its shots -- median
        //  error 0.11 deg, 47.5% through a hitbox, only 28% of those the head: aimed at the neck. NHOOK_BOT_AIM_HEAD = the aim point's
        //  height over the bounding sphere's centre, x its radius (was 0.55; a live knob: follow-study.py hones it on the head share))
        float aimh = 0.55f; lk_over(&aimh, "NHOOK_BOT_AIM_HEAD");
        float ha_on = 1, hdz = 0; lk_over(&ha_on, "NHOOK_BOT_HEADAIM"); lk_over(&hdz, "NHOOK_BOT_HEAD_DZ"); g_bot_head_r = -1;
        if (bi >= 0) { const NovObj* t = &objs[bi]; float hp[4], ap[3] = {t->c[0], t->c[1], t->c[2] + aimh * t->r};
            if (ha_on > 0 && bot_head(t, now, hp)) { ap[0] = hp[0]; ap[1] = hp[1]; ap[2] = hp[2] + hdz * hp[3]; g_bot_head_r = hp[3]; }   // (the head hitbox: bot_head above)
            float d[3] = {ap[0] - cam[0], ap[1] - cam[1], ap[2] - cam[2]}; float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
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
    static int fight = -1; static float skill_0 = 0.5f, kdist_0 = 1, kstrafe_0 = 1, kswitch_0 = 1, fextra_0 = 1, retreat_0 = 0.35f, gren_0 = 9, backoff_0 = 0.15f, press_0 = 0.4f;
    if (fight < 0) { const char* e = getenv("NHOOK_BOT_FIGHT"); fight = e && strcmp(e, "0");
        #define BKNOB(v, n) if (getenv(n)) v = strtof(getenv(n), 0)
        BKNOB(skill_0, "NHOOK_BOT_SKILL"); BKNOB(kdist_0, "NHOOK_BOT_DIST"); BKNOB(kstrafe_0, "NHOOK_BOT_STRAFE"); BKNOB(kswitch_0, "NHOOK_BOT_SWITCH");
        BKNOB(fextra_0, "NHOOK_BOT_FIRE_EXTRA"); BKNOB(retreat_0, "NHOOK_BOT_RETREAT"); BKNOB(gren_0, "NHOOK_BOT_GREN"); BKNOB(backoff_0, "NHOOK_BOT_BACKOFF"); BKNOB(press_0, "NHOOK_BOT_PRESS");
        #undef BKNOB
        if (fight) { bm_load(); eg_load(); nlog("[nhook] [fight] on: skill %.2f, distance x%.2f, strafe x%.2f, switch x%.2f, fire slack %.1f deg, back off below shield %.2f (%.2f), press %.2f, grenades every %.0f s",
                                     skill_0, kdist_0, kstrafe_0, kswitch_0, fextra_0, retreat_0, backoff_0, press_0, gren_0); } }
    float skill = skill_0, kdist = kdist_0, kstrafe = kstrafe_0, kswitch = kswitch_0, fextra = fextra_0, retreat = retreat_0, gren = gren_0, backoff = backoff_0, press = press_0;   // (live knobs)
    lk_over(&skill, "NHOOK_BOT_SKILL"); lk_over(&kdist, "NHOOK_BOT_DIST"); lk_over(&kstrafe, "NHOOK_BOT_STRAFE"); lk_over(&kswitch, "NHOOK_BOT_SWITCH"); lk_over(&fextra, "NHOOK_BOT_FIRE_EXTRA");
    lk_over(&retreat, "NHOOK_BOT_RETREAT"); lk_over(&gren, "NHOOK_BOT_GREN"); lk_over(&backoff, "NHOOK_BOT_BACKOFF"); lk_over(&press, "NHOOK_BOT_PRESS");
    int engaged = 0; float ftx = 0, fty = 0; uint32_t fbtn = 0; g_bot_fighting = fight && spar;
    if (fight) { eg_load_for(g_st_curmap, g_trk_mode); wz_load(g_st_curmap); }   // (the models for this map and game: reloaded when either changes; the weapon zones)
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
        static float rk0 = -1, t0r0 = -1; if (rk0 < 0) { const char* e = getenv("NHOOK_BOT_REACT"); rk0 = e ? (float)atof(e) : 1.6f; e = getenv("NHOOK_BOT_TURN0"); t0r0 = e ? (float)atof(e) : 0.73f; }
        float rk = rk0, t0r = t0r0; lk_over(&rk, "NHOOK_BOT_REACT"); lk_over(&t0r, "NHOOK_BOT_TURN0");   // (live knobs, above)
        // (07:50: walker-model's "reaction" = 30 deg -> 5 deg of aim, so the turn rate BEFORE the reaction time decides it: 1.5 rad/s
        //  measured 0.49-0.53 s against the players' 1.08 whatever the delay; NHOOK_BOT_TURN0 = that rate, NHOOK_BOT_REACT the delay factor)
        // NHOOK_BOT_NOISE: x the aim noise (2026-11-08 12:15: on Guardian the players' horizontal aim error while attacking is 6.8 deg,
        // the walkers' 3.8 -- lag and moving targets the walkers do not have; engage-compare.py)
        static float nk0 = -1; if (nk0 < 0) { const char* e = getenv("NHOOK_BOT_NOISE"); nk0 = e ? strtof(e, 0) : 1.0f; }
        float nk = nk0; lk_over(&nk, "NHOOK_BOT_NOISE");
        float react = g_bm.reaction * (1.25f - skill) * rk, noise = g_bm.aim_err * (1.25f - skill) * 1.6f * nk / 57.2958f;
        if (seen) {
            // aim: OU noise around the exact angles, turning at most 1.5 rad/s before the reaction time is up, 7 rad/s after
            float th = 0.45f, k = (float)dt / th, sg = noise * sqrtf(2.0f * (float)dt / th);
            ox += -ox * k + sg * bot_gauss(); oy += -oy * k + 0.5f * sg * bot_gauss();
            float turn = 7.0f; lk_over(&turn, "NHOOK_BOT_TURN");   // (07:10: the turn rate once reacting -- "increase look sensitivity as you level up": the trainer raises it)
            float dy = bot_wrap(tyaw + ox - yaw0), dp = (tpitch + oy) - pitch0, mx = (float)dt * (now - tacq < react ? t0r : turn);
            yaw = yaw0 + (dy > mx ? mx : dy < -mx ? -mx : dy); pitch = pitch0 + (dp > mx ? mx : dp < -mx ? -mx : dp); out |= 1;
            float ey = bot_wrap(tyaw - yaw0), ep = tpitch - pitch0, err = sqrtf(ey * ey + ep * ep) * 57.2958f, size = atanf(0.4f / (tdist > 0.5f ? tdist : 0.5f)) * 57.2958f;
            g_bot_err = err; g_bot_fire = vis && now - tacq > react && err < (size > 1.2f ? size : 1.2f) + fextra && tdist < 40;
            // (07:35, the user: "hitting the critical hitbox is key. the game reticule has a red dot in some cases when it's a head shot,
            //  you should tune to hit them when possible. headshots being main goal"): with a PRECISION weapon (battle rifle, carbine,
            //  magnum, sniper -- headshots kill faster) the trigger waits for the aim to sit on the HEAD: within its angular radius
            //  (0.15 units) x NHOOK_BOT_HEADGATE (1.0) + 0.3 deg. The automatic weapons keep the body gate (a headshot adds nothing there)
            if (g_bot_fire && g_inv.held_c && (!strcmp(g_inv.held_c, "precision") || !strcmp(g_inv.held_c, "sniper"))) {
                float hg = 1.0f; lk_over(&hg, "NHOOK_BOT_HEADGATE"); float hs = atanf(0.15f / (tdist > 0.5f ? tdist : 0.5f)) * 57.2958f;   // (the width stays 0.15 units -- the head hitbox's mean radius, ~0.07, would halve it -- but the error is now measured from the real head: bot_head)
                if (err > hs * hg + 0.3f) g_bot_fire = 0; }
            { static float mv = -1; static double thurt; float v2 = o->shield + o->health; if (mv >= 0 && v2 < mv - 0.02f) thurt = now; mv = v2;
              // under active camo: no shot beyond the near edge of the weapon's band (+2 units) unless we were hit in the last 2 s --
              // firing gives camo away; walk in instead (below)
              float a = 0, b = 0; if (g_inv.camo && now - thurt > 2.0 && eg_band(g_inv.held_c, &a, &b) && tdist > a * kdist + 2.0f) { g_bot_fire = 0; g_bot_camo_hold = 1; } else g_bot_camo_hold = 0; }
            // the other weapon when the target is outside the held one's band and well inside the other's (vpad Y held 0.15 s,
            // 3 s between switches; not in the first 0.3 s of a target)
            { static double tsw; if (g_vpad_on && g_inv.has_other && now - tsw > 3.0 && now - tacq > 0.3) {
                  float oh = bot_band_off(g_inv.held_c, tdist, kdist), oo = bot_band_off(g_inv.other_c, tdist, kdist);
                  int hi = wz_ci(g_inv.held_c), oi = wz_ci(g_inv.other_c), zt = 0, zsw = 0, zkeep = 0; float eh = 0, eo = 0;
                  if (g_wz.n && hi >= 0 && oi >= 0) { eh = wz_eff(hi, ftg.c, &zt); eo = wz_eff(oi, ftg.c, 0); if (zt >= 8) { zsw = eo > 1.5f * eh && eo > 1.2f && g_inv.other_ammo != 0; zkeep = eh > 1.5f * eo; } }
                  if (g_inv.ammo == 0 && g_inv.other_ammo != 0) { tsw = now; if (bot_log()) nlog("[nhook] [inv] magazine empty: switching to %s", g_inv.other); }   // (switching beats reloading mid-fight)
                  else if (zsw) { tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s: around the target (%.0f, %.0f) players kill with a %s %.1fx its carried share, a %s %.1fx (%d kills)",
                                                                g_inv.other, ftg.c[0], ftg.c[1], g_inv.other_c, eo, g_inv.held_c, eh, zt); }
                  // the players' own switch for this pair (behaviour.inventory, 20+ switches): toward the shorter-range class when the
                  // nearest enemy is closer than their median distance, toward the longer one when farther
                  else if (!zkeep && hi >= 0 && oi >= 0 && g_eg.sw_n[hi][oi] >= 20 && g_inv.other_ammo != 0 && ({ float mh = 0, mo = 0;
                           for (int q = 0; q < g_eg.nband; q++) { if (!strcmp(g_eg.band[q].cls, g_inv.held_c)) mh = g_eg.band[q].med; if (!strcmp(g_eg.band[q].cls, g_inv.other_c)) mo = g_eg.band[q].med; }
                           mh > 0 && mo > 0 && (mo < mh ? tdist < g_eg.sw_d[hi][oi] : tdist > g_eg.sw_d[hi][oi]); })) {
                      tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s at %.1f units: players switch %s -> %s at %.1f (median of %d)", g_inv.other, tdist, g_inv.held_c, g_inv.other_c, g_eg.sw_d[hi][oi], g_eg.sw_n[hi][oi]); }
                  else if (!zkeep && !(hi >= 0 && oi >= 0 && g_eg.sw_n[hi][oi] >= 20) && oh > 0.3f && oo >= 0 && oo < oh * 0.5f) { tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s (%s) for %.1f units (the %s band is %.0f%% off)", g_inv.other, g_inv.other_c, tdist, g_inv.held_c, oh * 100); } }
              { static float sh = -1; if (sh < 0) { const char* e = getenv("NHOOK_BOT_SWITCH_HOLD"); sh = e ? strtof(e, 0) : 0.4f; }   // (0.15 s never registered: see nvpad.m's read counters)
                if (now - tsw < sh) { fbtn |= 8; float swp = 0.03f; lk_over(&swp, "NHOOK_BOT_SWPULSE"); g_bot_switch_until = tsw + swp; } } }   // (the switch bit: ONE packet -- 08:55: held 0.15 s it never switched; an impulse per packet?)
            // B: melee -- the players' range (+ 0.25: positions are a tick or two old), held 0.12 s, at most every 0.6 s
            { static int nomel = -1; if (nomel < 0) nomel = getenv("NHOOK_BOT_NOMELEE") != 0;
              if (nomel) { if (tdist < 1.8f && now - tmelee > 0.7) { tmelee = now; fbtn |= 2; } }   // (the old: B, one step)
              else { mel_load(); int mcls = g_inv.held_c && !strcmp(g_inv.held_c, "melee");
                  float mr = (mcls ? (g_inv.held && strstr(g_inv.held, "HAMMER") ? g_mel.hammer : g_mel.sword) : g_mel.range) + 0.25f;
                  if (vis && tdist < mr && now - tmelee > 0.6 && now - tacq > 0.15) { tmelee = now;
                      if (bot_log()) nlog("[nhook] [melee] %s at %.2f units (target shield %.2f: %s), holding %s", mcls ? "swing" : "melee", tdist, ftg.shield, ftg.shield < 0.05f ? "beatdown" : "full", g_inv.held ? g_inv.held : "-"); }
                  static float mh = -1; if (mh < 0) { const char* e = getenv("NHOOK_BOT_MELEE_HOLD"); mh = e ? strtof(e, 0) : 0.3f; }
                  if (now - tmelee < mh) { fbtn |= bot_meleepad(); g_bot_melee_until = tmelee + mh; } }
              g_bot_pull = !nomel && bot_pull_weapon(g_inv.held); }
            { static float fj0 = -1; if (fj0 < 0) { const char* e = getenv("NHOOK_BOT_FJUMP"); fj0 = e ? strtof(e, 0) : 0.0f; } float fj = fj0; lk_over(&fj, "NHOOK_BOT_FJUMP");   // jumps a minute in a fight (walker-coach.py: the winners')
              if (fj > 0 && bot_rand01() < fj * (float)dt / 60.0f) g_fjump_until = now + 0.25; }
            { float gd0 = 6, gd1 = 16; static int gset = -1; static double gnext; if (gset < 0) gset = getenv("NHOOK_BOT_GREN") != 0;   // (the players' band and rate unless NHOOK_BOT_GREN is set)
              int pl = g_eg.on && g_eg.gren_ok; if (pl) { gd0 = g_eg.gren_d0; gd1 = g_eg.gren_d1; }
              // the next throw's time is drawn ONCE per throw (it was drawn every tick, so the shortest draw won: throws came early)
              static float gx0 = -1; if (gx0 < 0) { const char* e = getenv("NHOOK_BOT_GRENX"); gx0 = e ? strtof(e, 0) : 1.0f; } float gx = gx0; lk_over(&gx, "NHOOK_BOT_GRENX");   // (walker-coach.py: x the time between grenades)
              if (!gnext) gnext = now + gx * (pl && !gset ? g_eg.gren_every * (0.5f + bot_rand01()) : gren + 6 * bot_rand01());
              if ((pl || gren > 0) && (g_inv.gren > 0 || !g_inv.held) && tdist > gd0 && tdist < gd1 && vis && now > gnext && now - tacq > react) { tgren = now; nade_note(now, &ftg, tdist);
                  gnext = now + gx * (pl && !gset ? g_eg.gren_every * (0.5f + bot_rand01()) : gren + 6 * bot_rand01()); } }
            // OUT OF AMMO (2026-11-08, the user: a walker went "click click click" right past him -- "never just walk past an enemy
            // without predictive grenade lay some shots, or a beat down! engage!"): an empty magazine (the ammo field) with the enemy
            // in sight -> a grenade at once (3-16 units, if any) and the rush below for the beatdown. NHOOK_BOT_NOEMPTY=1 = off.
            { static int ne = -1; if (ne < 0) ne = getenv("NHOOK_BOT_NOEMPTY") != 0;
              // (2026-11-09: the found "rounds loaded" field reads 0 for a fresh rifle in a player's game and 129 in the walkers' --
              //  not a magazine: "empty" only when the weapon has shown 8+ rounds there before)
              g_bot_empty = !ne && g_inv.ammo == 0 && g_inv.ammo_max >= 2 && g_inv.held && strcmp(g_inv.held_c ? g_inv.held_c : "", "melee") && (g_ammo_learned < 0 || g_bot_dry);   // (08:30: with the magazine known, only when DRY -- else it reloads / switches)
              if (g_bot_empty && vis && g_inv.gren > 0 && tdist > 3 && tdist < 16 && now - tgren > 1.2) { tgren = now; nade_note(now, &ftg, tdist);
                  if (bot_log()) nlog("[nhook] [fight] empty magazine: a grenade at %.1f units, then the beatdown", tdist); } }
            if (now - tgren < 0.15) { fbtn |= 0x10000;                                                                     // (left trigger: a grenade)
                // the PREDICTIVE throw (same request): aimed where the target will be -- its velocity (units a second; 09:05: it was taken as a tick, x30) times
                // the flight (0.4 s + distance / 10) -- and a little up for the arc (0.025 rad a unit, at most 0.4). NHOOK_BOT_NOLEAD=1 = off.
                static int nl = -1; if (nl < 0) nl = getenv("NHOOK_BOT_NOLEAD") != 0;
                if (!nl) { float n0 = 0.4f, npu = 0.1f, narc = 0.025f, nmax = 0.4f;   // (live knobs: NHOOK_BOT_NADE_T0 / _TPU s a unit / _ARC rad a unit / _ARCMAX -- follow-study.py hones them)
                    lk_over(&n0, "NHOOK_BOT_NADE_T0"); lk_over(&npu, "NHOOK_BOT_NADE_TPU"); lk_over(&narc, "NHOOK_BOT_NADE_ARC"); lk_over(&nmax, "NHOOK_BOT_NADE_ARCMAX");
                    float lt = n0 + tdist * npu, px = ftg.c[0] + ftg.vel[0] * lt - cam[0], py = ftg.c[1] + ftg.vel[1] * lt - cam[1], pz = ftg.c[2] - cam[2];   // (09:05: the velocity is units a SECOND: the old x30 threw 30x too far ahead of a moving target)
                    g_nade_lead = lt; g_nade_arc = fminf(nmax, narc * tdist);
                    float ly = atan2f(py, px), lp = atan2f(pz, sqrtf(px * px + py * py)) + fminf(nmax, narc * tdist), mxr = (float)dt * 7.0f;
                    float dyw = bot_wrap(ly - yaw0), dpt = lp - pitch0; yaw = yaw0 + (dyw > mxr ? mxr : dyw < -mxr ? -mxr : dyw); pitch = pitch0 + (dpt > mxr ? mxr : dpt < -mxr ? -mxr : dpt); out |= 1; } }
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
                    static float lsc0 = -1; if (lsc0 < 0) { const char* e = getenv("NHOOK_BOT_LIBSCALE"); lsc0 = e ? strtof(e, 0) : 1.0f; } float lsc = lsc0; lk_over(&lsc, "NHOOK_BOT_LIBSCALE");   // (the players' own moves: not the tactics' strafe scale)
                    lat = fmaxf(-1.0f, fminf(1.0f, lsc * el / run));
                    // pulled in when beyond the band, never pushed back inside it: the players close in at 1.3 units/s with shields up
                    // on Guardian (engage-compare.py; the push-back made the walkers hover at the band's near edge)
                    static float rsc0 = -1; if (rsc0 < 0) { const char* e = getenv("NHOOK_BOT_RADSCALE"); rsc0 = e ? strtof(e, 0) : 1.0f; } float rsc = rsc0; lk_over(&rsc, "NHOOK_BOT_RADSCALE");   // (walker-coach.py: x the replayed closing / backing off)
                    float pull = tdist > p75 * kdist ? 0.6f : 0.0f; r = fmaxf(-1.0f, fminf(1.0f, rsc * er / run + pull));
                    if (o->shield < retreat && o->shield >= 0) r = -backoff;
                    if (g_bot_camo_hold) r = 0.9f; }
                // the beatdown rush (players: half of the melee kills finish a victim whose shield is down, closing in from ~3.2 units) and a
                // sword / hammer closing in to its swing range (melee.json; not with a rocket launcher in hand)
                { static int nm = -1; if (nm < 0) nm = getenv("NHOOK_BOT_NOMELEE") != 0; int mcls = g_inv.held_c && !strcmp(g_inv.held_c, "melee");
                  // (2026-11-08, the user: "melee more when it is in close to an opponent! especially if the opponent's back is to it":
                  //  a target LOOKING AWAY -- its aim more than 110 deg off the line to the walker -- within the players' back-rush
                  //  distance is run at straight; players take that chance at arm's length only 20% of the time. Shots go on: the
                  //  walker's melee does not land on a server yet, backlog N26. NHOOK_BOT_NOBACK=1 = off)
                  static int nb = -1; if (nb < 0) nb = getenv("NHOOK_BOT_NOBACK") != 0;
                  float ax = ftg.aim[0], ay = ftg.aim[1], al = sqrtf(ax * ax + ay * ay), vx = o->c[0] - ftg.c[0], vy = o->c[1] - ftg.c[1], vl = sqrtf(vx * vx + vy * vy);
                  int back = !nb && al > 0.1f && vl > 0.01f && (ax * vx + ay * vy) / (al * vl) < -0.342f;   // (cos 110 deg)
                  if (back && tdist < g_mel.back) { static double tb; if (bot_log() && now - tb > 2.0) { tb = now; nlog("[nhook] [melee] its back is turned at %.1f units: going in", tdist); } }
                  if (!nm && vis && ((back && tdist < g_mel.back) || (g_bot_empty && tdist < 12.0f) || (ftg.shield < 0.05f && ftg.health > 0 && tdist < g_mel.rush && !(g_inv.held_c && !strcmp(g_inv.held_c, "rocket"))) || (mcls && tdist < 10.0f))) { r = 0.9f; lat *= back ? 0.0f : 0.3f; } }
                // ARM'S LENGTH (01:10, the user: "if i stand still they'll try to walk over me and get stuck jumping not shooting or meleeing"):
                // the rush above drove on at full throttle inside a unit -- onto the enemy's head (Second 0.36 units across, 0.4-1.1 above,
                // bouncing). Within a unit across: no closer (circle); on top of it (0.5+ above, 0.8 across): step off. NHOOK_BOT_NOARM=1 = old
                { static int na = -1; if (na < 0) na = getenv("NHOOK_BOT_NOARM") != 0;
                  float hx = ftg.c[0] - o->c[0], hy = ftg.c[1] - o->c[1], hd = sqrtf(hx * hx + hy * hy), dz = o->c[2] - ftg.c[2];
                  if (!na && hd < 0.8f && dz > 0.5f) { r = -0.7f; lat = lat >= 0 ? 0.5f : -0.5f; }
                  else if (!na && hd < 1.0f && r > 0) { r = 0; if (fabsf(lat) < 0.3f) lat = lat >= 0 ? 0.4f : -0.4f; } }
                // never off the floor: test where this step leads (1 unit along the move); flip the strafe, then drop the back-off
                float fx = cosf(yaw0), fy = sinf(yaw0), lx = -fy, ly = fx; float lat_in = lat, r_in = r; int ftries = 0;
                // (09:10: the check flipped the strafe on 23-60% of fight steps -- a strafe reversing every few steps is a jitter in
                //  place: the recordings' "strafes 0.0 u/s", nothing of the best player's committed moves. Now: no check where the graph has no
                //  floor under the walker itself (too sparse to judge), 1.0 unit ahead, and a flip only after NHOOK_BOT_FLOORN (2)
                //  failing steps in a row: a strafe is committed to. NHOOK_BOT_FLOORN=1 = the old twitchy check)
                { static double thold; float me3[3] = {o->c[0], o->c[1], o->c[2] - 0.35f};
                  // (09:15: a flip after two failing steps still flipped on 31% of steps by walls: both sides are looked at now -- the
                  //  strafe keeps its side while that side has floor 1 unit off, else takes the other side if it has floor and holds it
                  //  0.6 s (no flip back meanwhile: a side that fails then only drops the strafe), else no strafe; then the throttle)
                  if (bot_floor_ok(me3)) {
                      float pa[3] = {o->c[0] + (fx * r + lx * lat) * 1.0f, o->c[1] + (fy * r + ly * lat) * 1.0f, o->c[2] - 0.35f};
                      if (!bot_floor_ok(pa)) { float pb2[3] = {o->c[0] + (fx * r - lx * lat) * 1.0f, o->c[1] + (fy * r - ly * lat) * 1.0f, o->c[2] - 0.35f};
                          if (now > thold && fabsf(lat) > 0.05f && bot_floor_ok(pb2)) { side = -side; lat = -lat; thold = now + 0.6; ftries = 1; }
                          else { float pc[3] = {o->c[0] + fx * r * 1.0f, o->c[1] + fy * r * 1.0f, o->c[2] - 0.35f}; lat = 0; ftries = 2;
                              if (!bot_floor_ok(pc)) { r = r < 0 ? 0 : r; float pd[3] = {o->c[0] + fx * r * 1.0f, o->c[1] + fy * r * 1.0f, o->c[2] - 0.35f}; if (!bot_floor_ok(pd)) { r = 0; ftries = 3; } } } } } }
                // (08:45, the user: "[the best player] simply moves more nimble, and engages faster ... our walker does not move with that much
                //  *authority*": the recordings say the walker stands still 58% and strafes 0.0 u/s in fights. [moves] every 10 s of
                //  fighting: what the fight asked for, what the floor check let through, how fast the walker really moved across)
                { static double tm; static int ns, nask, nflip, nzero; static float vlat, vall; ns++; if (fabsf(lat_in) > 0.2f || fabsf(r_in) > 0.2f) nask++; if (ftries == 1 || ftries == 2) nflip++; if (ftries == 3) nzero++;
                  float ex = ftg.c[0] - o->c[0], ey = ftg.c[1] - o->c[1], el2 = sqrtf(ex * ex + ey * ey) + 1e-3f; vlat += fabsf((o->vel[0] * -ey + o->vel[1] * ex) / el2) * 30.0f; vall += sqrtf(o->vel[0] * o->vel[0] + o->vel[1] * o->vel[1]) * 30.0f;
                  static float lp[3]; static double lt; static float dsum; static double dts; static float vraw;
                  if (lt > 0 && now - lt < 0.2) { float dx2 = o->c[0] - lp[0], dy2 = o->c[1] - lp[1]; dsum += sqrtf(dx2 * dx2 + dy2 * dy2); dts += now - lt; }
                  memcpy(lp, o->c, 12); lt = now; vraw += sqrtf(o->vel[0] * o->vel[0] + o->vel[1] * o->vel[1]);
                  if (now - tm > 10.0) { if (tm > 0 && ns > 20 && bot_log()) nlog("[nhook] [moves] %d fight steps: moves asked %.0f%%, floor check flipped %.0f%% / stopped %.0f%%; moved %.2f u/s by position (velocity field %.3f a step), across the enemy line %.0f%% of it",
                          ns, 100.0 * nask / ns, 100.0 * nflip / ns, 100.0 * nzero / ns, dts > 0 ? dsum / dts : 0.0, vraw / ns, vall > 0 ? 100.0 * vlat / vall : 0.0);
                      tm = now; ns = nask = nflip = nzero = 0; vlat = vall = 0; dsum = 0; dts = 0; vraw = 0; } }
                ftx = r; fty = lat; } }
        else { g_bot_fire = 0;
            // LEVELS APART (2026-11-08, the user: "enemy radar on multi level maps. like if you see someone on radar and then in turn
            // don't directly see them they are above or below you. the engagement struggled with that"). match-study.py on that
            // round: within 12 units across and over 2 units above / below, the walkers kept moving (1.3-1.7 u/s), never reached
            // the other's level and won 3 / lost 6; the user stood (0.5 u/s) and won 3 from above, lost none. A known enemy hidden
            // on another level: the walker aims at it (up / down, at most 3 rad/s: where it will show), and with the enemy BELOW
            // it holds its ground up to 8 s (g_bot_hold_high, the move override below) -- the high ground wins. NHOOK_BOT_NOLEVELS=1 = off.
            static int nolv = -1; if (nolv < 0) nolv = getenv("NHOOK_BOT_NOLEVELS") != 0;
            float lz = ftg.c[2] - o->c[2], lh = hypotf(ftg.c[0] - o->c[0], ftg.c[1] - o->c[1]);
            if (!nolv && fabsf(lz) > 2.0f && lh < 12.0f) {
                float mxr = (float)dt * 3.0f, dyw = bot_wrap(tyaw - yaw0), dpt = tpitch - pitch0;
                yaw = yaw0 + (dyw > mxr ? mxr : dyw < -mxr ? -mxr : dyw); pitch = pitch0 + (dpt > mxr ? mxr : dpt < -mxr ? -mxr : dpt); out |= 1;
                static uint32_t lv_tgt; static double lv_t0;
                if (ftg.handle != lv_tgt || now - g_bot_hold_last > 2.0) { lv_tgt = ftg.handle; lv_t0 = now; }
                if (lz < -2.0f && now - lv_t0 < 8.0) g_bot_hold_high = now;
                g_bot_hold_last = now;
                if (bot_log()) { static double tl; if (now - tl > 2.0) { tl = now; nlog("[nhook] [fight] levels apart: %08x %.1f units %s, %.1f across -- %s", ftg.handle, fabsf(lz), lz > 0 ? "above" : "below", lh,
                    lz < -2.0f && now - lv_t0 < 8.0 ? "holding the high ground, aimed down" : lz > 0 ? "aimed up, the path goes on" : "aimed down, hunting again"); } } }
            else if (tdist > 8) { yaw = yaw0; pitch = pitch0; out &= ~1; } }   // (out of sight: no tracking through walls -- look where it walks)
        if (bot_log() && g_vpad_on) { static double tr; static unsigned long pr[4]; static int ps, pc; if (now - tr > 10.0) { unsigned long r[4]; nh_vpad_btn_reads(r); int sr = nh_vpad_stick_reads(), cr = nh_vpad_reads();
              if (tr > 0) nlog("[nhook] [vpad] the app read in %.0f s: controllers %d, left stick %d, B %lu, Y %lu, RB %lu, right trigger %lu", now - tr, cr - pc, sr - ps, r[0] - pr[0], r[1] - pr[1], r[2] - pr[2], r[3] - pr[3]);
              tr = now; memcpy(pr, r, sizeof pr); ps = sr; pc = cr; } }
        if (bot_log()) { static double tl; if (now - tl > 1.0) { tl = now; nlog("[nhook] [fight] target %08x %.1f u %s, err %.1f deg, %s%s, shield %.2f", ftg.handle, tdist, vis ? "in sight" : "hidden", g_bot_err, engaged ? "engaged" : "hunting", g_bot_fire ? ", FIRING" : "", o->shield); } }
    }
    // ---- pickups (NHOOK_BOT_PICKBIT=<hex>: the control record's pick-up bit, shim/tools/native-pickup-scan.sh finds it): out of a
    // fight, a better weapon (bot_wpn_rank) lying within 12 units and 2.5 of height is walked to and picked up (the bit held 0.6 s);
    // each weapon is tried once
    { static int pb = -1; static uint64_t pbit; if (pb < 0) { const char* e = getenv("NHOOK_BOT_PICKBIT"); pbit = e ? strtoull(e, 0, 16) : 0; pb = pbit != 0; }
      static double tscan, tpress; static uint32_t tried[32]; static int ntried; static uint32_t want_h;
      if (pb == 0) pb = 2;   // (07:25: the pick-up is the packet's ACT bit now -- it needs no virtual controller; the public walker has none, and with
                             //  "pb" tied to it no fetch had ever started: "ALWAYS uses a garbage AR")
      { static int np = -1; if (np < 0) np = getenv("NHOOK_BOT_NOPICK") != 0; if (np) pb = 0; }   // (NHOOK_BOT_NOPICK=1: keep the start weapons -- melee-battle.sh)
      // (2026-11-08: a walker stood the whole battle "going for BATTLE RIFLE (8.1 units)" -- a weapon it could not reach was never
      //  given up: a target that brings no progress (0.5 units closer) in 6 s is now marked tried. The tried list is 32 entries
      //  long: its check read past the end once 32 had been tried)
      static double t_prog; static float d_prog;
      // (07:20: the scan runs during fights too -- in a 24-player game the walker is nearly always "engaged": it never fetched; the arbiter weighs it)
      if (pb && fight && now - tscan > 1.0) { tscan = now; static NovObj w[512]; pthread_mutex_lock(&g_nov_collect_mx); g_nov_units_only = 3; int nw = nov_collect(w, 512); g_nov_units_only = 0; pthread_mutex_unlock(&g_nov_collect_mx);
          int cur = 99; uint32_t held = o->wslot >= 0 && o->wslot < 4 ? o->weapons[o->wslot] : 0xFFFFFFFFu;
          for (int i = 0; i < nw; i++) if (w[i].handle == held) { cur = bot_wpn_rank(hb_tag_name(w[i].def)); break; }
          if (g_bot_dry) cur = 99;   // (dry: any weapon -- the same kind refills it)
          // (07:10: in a public game no better weapon ever lay within 12 units out of a fight -- "ALWAYS uses a garbage AR": POWER weapons
          //  (rocket .. shotgun, rank <= 6) are fetched from NHOOK_BOT_FETCH_FAR (30) units, the rest from 12; a better rank counts as
          //  nearer (distance x (1 + 0.15 x rank)), so a battle rifle 20 units off beats a carbine at 15)
          float far = 30.0f; lk_over(&far, "NHOOK_BOT_FETCH_FAR"); float bd = 1e30f, bdd = 0; int bi = -1;
          for (int i = 0; i < nw; i++) { if (w[i].parent != 0xFFFFFFFFu) continue; int rk = bot_wpn_rank(hb_tag_name(w[i].def)); if (rk >= cur) continue;
              int t2 = 0; for (int k = 0; k < ntried && k < 32; k++) if (tried[k] == w[i].handle) t2 = 1; if (t2) continue;
              float dx = w[i].c[0] - o->c[0], dy = w[i].c[1] - o->c[1], dz = w[i].c[2] - o->c[2], d = dx * dx + dy * dy, lim = rk <= 6 ? far * far : 144.0f;
              float sc = sqrtf(d) * (1.0f + 0.15f * (float)rk); if (d < lim && fabsf(dz) < 4.0f && sc < bd) { bd = sc; bdd = d; bi = i; } }
          bd = bdd;
          if (bi >= 0) { float d = sqrtf(bd);
              if (w[bi].handle != want_h || !g_bot_pick_on) { t_prog = now; d_prog = d; } else if (d < d_prog - 0.5f) { d_prog = d; t_prog = now; }
              if (w[bi].handle == want_h && g_bot_pick_on && now - t_prog > 6.0) { g_bot_pick_on = 0; tried[(ntried++) & 31] = want_h;
                  if (bot_log()) nlog("[nhook] [fight] gave up on %s (%.1f units): no closer in 6 s", hb_tag_name(w[bi].def), d); }
              else { if (!g_bot_pick_on || want_h != w[bi].handle) { if (bot_log()) nlog("[nhook] [fight] going for %s (%.1f units, rank %d over %d)", hb_tag_name(w[bi].def), d, bot_wpn_rank(hb_tag_name(w[bi].def)), cur); }
                     g_bot_pick_on = 1; memcpy(g_bot_pick_pos, w[bi].c, 12); want_h = w[bi].handle; snprintf(g_pick_name, sizeof g_pick_name, "%s", hb_tag_name(w[bi].def)); g_pick_gain = cur - bot_wpn_rank(hb_tag_name(w[bi].def)); g_pick_d = d; } }
          else g_bot_pick_on = 0; }
      // (07:30: "gave up on BATTLE RIFLE (2.1 units): no closer in 6 s" -- the path ends at the graph's floor node, a weapon on a rack /
      //  ledge lies off it; the press needed 0.9 units. Now within NHOOK_BOT_PICK_R (1.3) across and 1.5 up / down; the last 3 units are
      //  walked straight at it (the step's FETCH branch below))
      // (08:05: "at the weapon (1.3 units): holding act to take it" 8 times, ONE battle rifle in hand: the press began 1.3 units off and the
      //  fetch ended with it -- the walker turned back to the fight before touching the weapon. Now the fetch goes on while act is held:
      //  the walk ends ON the weapon (the step's FETCH branch), act held until the weapon is in one of its slots ("took"), or
      //  NHOOK_BOT_PICK_HOLD (2.5) s pass ("could not take": tried). With a free slot walking over it picks it up by itself)
      static double tp0; float phold = 2.5f; lk_over(&phold, "NHOOK_BOT_PICK_HOLD");
      if (tp0 > 0 && now - tp0 > phold + 1.0) tp0 = 0;   // (a press that outlived its hold: the walker died / respawned meanwhile)
      // (09:00: a "could not take the BATTLE RIFLE" walker was holding one a moment later -- in hand the weapon is another object
      //  handle than on the floor: taken = its NAME is in a slot now)
      if (tp0 > 0) { int got = 0; for (int k = 0; k < 4; k++) if (o->weapons[k] == want_h) got = 1;
          if ((g_inv.held && !strcmp(g_inv.held, g_pick_name)) || (g_inv.other && !strcmp(g_inv.other, g_pick_name))) got = 1;
          if (got || !g_bot_pick_on || now - tp0 > phold) { if (bot_log()) nlog("[nhook] [fight] %s the %s after %.1f s of act (%.2f units across, %+.2f up; the legs: %s; holding %s, other %s)", got ? "TOOK" : !g_bot_pick_on ? "left" : "could not take", g_pick_name, now - tp0,
                  g_pick_d, g_bot_pick_pos[2] - o->c[2], g_arb_why, g_inv.held ? g_inv.held : "-", g_inv.other ? g_inv.other : "-");
              if (!got && g_bot_pick_on) { g_bot_pick_on = 0; tried[(ntried++) & 31] = want_h; } if (got) g_bot_pick_on = 0; tp0 = 0; } }
      if (g_bot_pick_on) { float dx = g_bot_pick_pos[0] - o->c[0], dy = g_bot_pick_pos[1] - o->c[1], dz = g_bot_pick_pos[2] - o->c[2], pr = 1.3f; lk_over(&pr, "NHOOK_BOT_PICK_R");
          g_pick_d = sqrtf(dx * dx + dy * dy);
          if (dx * dx + dy * dy < pr * pr && fabsf(dz) < 1.5f) { tpress = now; if (!tp0) { tp0 = now;
              if (bot_log()) nlog("[nhook] [fight] at the weapon (%.1f units): walking onto it, act held", sqrtf(dx * dx + dy * dy)); } } }
      uint32_t ppad = bot_pickpad();
      gun_vlist(now);
      // (08:15: standing ON a battle rifle with act held 2.5 s took nothing -- act was already down before the weapon became the action
      //  target, and a hold needs a press to begin: while at the weapon act is PULSED, 0.5 s down / 0.2 s up, a new press every 0.7 s)
      // (08:55: still nothing taken standing on it -- the +0x2c bits look like one-packet IMPULSES (a held act "seats and then
      //  unseats"; a held switch never switched): act now goes out for NHOOK_BOT_ACTPULSE (0.03 s, ~one packet) every 0.4 s at the weapon)
      if (pb && tp0 > 0 && !gun_near_veh(o->c, 0xFFFFFFFFu)) { float ap = 0.03f; lk_over(&ap, "NHOOK_BOT_ACTPULSE"); static double tap;
          if (now - tap > 0.4) { tap = now; g_bot_act_until = now + ap; } }
      else if (pb && now - tpress < 0.6 && !gun_near_veh(o->c, 0xFFFFFFFFu)) g_bot_act_until = tpress + 0.6;   // (the act bit held: pick up -- never beside a vehicle: it boards it)
      if (pb && now - tpress < 0.6) fbtn |= 0x20000 | (g_vpad_on ? ppad : 0); }   // (fbtn bit 17: hold the pick-up bit; mapped below; the pad's button directly)
    // reload after a fight (the players' habit; no ammo count is read yet): the pad's reload button (NHOOK_BOT_PICKPAD, a TAP; a hold
    // picks up) 0.4-1.0 s after a fight in which the walker fired ends, unless it is walking to a weapon
    { static double tend, trel, tfired; static int was; if (g_bot_fire) tfired = now;
      if (was && !engaged) tend = now + 0.4 + 0.6 * bot_rand01(); if (engaged) tend = 0; was = engaged;
      // (2026-11-08: not with a sword / hammer -- nothing to reload, and the tap then picks up the weapon on the floor: melee test
      //  walkers swapped their hammers for plasma rifles / SMGs after every fight)
      int noreload = g_inv.held_c && !strcmp(g_inv.held_c, "melee");
      if (fight && tend && now > tend) { tend = 0; if (!noreload && now - tfired < 8.0 && !g_bot_pick_on && (g_inv.ammo < 0 || g_inv.ammo < g_inv.ammo_max)) { trel = now; if (bot_log()) nlog("[nhook] [inv] reloading after the fight"); } }
      { static double tlow; if (fight && !engaged && g_inv.ammo >= 0 && g_inv.ammo_max > 2 && g_inv.ammo < 0.6f * g_inv.ammo_max && now - tlow > 4.0 && now - trel > 4.0 && !g_bot_pick_on) {
            tlow = now; trel = now; if (bot_log()) nlog("[nhook] [inv] reloading: %d of %d rounds", g_inv.ammo, g_inv.ammo_max); } }
      // EMPTY (08:30, the user: "empty magazines sans reload are unacceptable btw. do better. find ammo, new weapon, or switch to
      // alternate"): with the magazine known (the learned field, ammo_learn), a 0 there -> the other weapon if it has rounds (switch
      // bit), else the reload tap at once, in a fight too; still 0 after 2 reloads = DRY (no reserve): the other weapon, else any
      // weapon on the floor is "better" (the pick-up scan, a high fetch bid) -- walking over the same weapon refills it
      { static double tswe; static int nrel;
        if (g_ammo_learned >= 0 && g_inv.ammo == 0 && !noreload && g_inv.held) {
            if (g_inv.has_other && g_inv.other_ammo != 0 && now - tswe > 1.5) { tswe = now; float swp = 0.03f; lk_over(&swp, "NHOOK_BOT_SWPULSE"); g_bot_switch_until = now + swp; fbtn |= 8;
                if (bot_log()) nlog("[nhook] [inv] EMPTY %s: switching to %s", g_inv.held, g_inv.other); }
            else if (now - trel > 2.5 && now - tswe > 1.0) { if (nrel >= 2) { if (!g_bot_dry && bot_log()) nlog("[nhook] [inv] DRY: %s still empty after %d reloads -- any weapon on the floor will do", g_inv.held, nrel); g_bot_dry = 1; }
                else { trel = now; nrel++; if (bot_log()) nlog("[nhook] [inv] EMPTY %s: reloading (try %d)", g_inv.held, nrel); } } }
        else if (g_inv.ammo > 0) { nrel = 0; g_bot_dry = 0; }
        // THE BETTER WEAPON IN HAND (09:20: "TOOK the BATTLE RIFLE ... holding ASSAULT RIFLE, other BATTLE RIFLE"): out of a fight, with the
        // other weapon 2+ ranks better and not known empty, one switch impulse (every 2 s until it is in hand)
        { static double tbt; if (!engaged && g_inv.has_other && g_inv.other_rank + 2 <= g_inv.held_rank && g_inv.other_ammo != 0 && now - tbt > 2.0 && now - tswe > 1.0) { tbt = now; tswe = now;
              float swp = 0.03f; lk_over(&swp, "NHOOK_BOT_SWPULSE"); g_bot_switch_until = now + swp; fbtn |= 8;
              if (bot_log()) nlog("[nhook] [inv] the better weapon in hand: switching from %s to %s", g_inv.held, g_inv.other); } } }
      uint32_t rpad = bot_pickpad();
      if (now - trel < 0.1) { fbtn |= rpad; if (!gun_near_veh(o->c, 0xFFFFFFFFu)) g_bot_act_until = trel + 0.1; } }   // (the act bit tapped: reload -- not beside a vehicle)
    // the walker (nav.c's graph): the path's way PELOAD_NAV_LOOK nodes ahead; with the aim on, the throttle walks it in any
    // direction relative to the view
    float tx = 0, ty = 0;
    // the behaviours bid for the legs (nbrain_beh.c), the arbiter chooses (arb_*)
    Arb arb; arb_reset(&arb);
    World W = {now, objs, n, me, o, cam, engaged, fight, tdist};
    float gbp[3] = {0, 0, 0}, gface[3] = {0, 0, 0}, gbd = 1e9f; int gun_go = 0; beh_board(&W, &arb, gbp, gface, &gbd, &gun_go);
    beh_follow(&W, &arb);
    int ob_carry = 0; float ob_d = 1e9f; beh_ball(&W, &arb, gun_go, &ob_carry, &ob_d);
    beh_fetch(&W, &arb); beh_fight(&W, &arb, ob_carry); beh_hold(&W, &arb); beh_hunt(&W, &arb);
    int arbw = arb_choose(&arb, now);
    if (arbw == ARB_FOLLOW || arbw == ARB_FETCH || arbw == ARB_BALL || (arbw == ARB_BOARD && gbd > 4.0f)) {   // a path to the winner's point
        static float lastp[3]; static int lastw = -1; static double trp; const float* wp = arb.b[arbw].pt;
        mode = 3; memcpy(g_bot_pt, wp, 12); g_bot_pt[2] -= 0.35f;
        float dx = wp[0] - lastp[0], dy = wp[1] - lastp[1];
        if (arbw != lastw || dx * dx + dy * dy > 2.25f || now - trp > 1.0) { goal = -1; trp = now; memcpy(lastp, wp, 12); }
        lastw = arbw; }
    else { static int lastw2 = -1; if (arbw != lastw2) goal = -1; lastw2 = arbw; }   // (fight / hold / hunt: the goal draw's path; the fight's moves below)
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
                  if (!strafe && !(g_bot_fighting && tdist < 3.0f) && now - histt[j & 63] >= 1.4 && dx * dx + dy * dy < 0.35f * 0.35f && now - tjump > 1.5) {   // (01:10: fighting at arm's length is no progress by design, not stuck) tjump = now; hh = 0; tback = now; bstr = (nav_rand() & 1) ? 0.3f : -0.3f;
                      nav_pos(path[pidx >= 4 ? pidx - 4 : 0], bk);
                      for (int i = pidx; i < plen && i < pidx + 10; i++) g_nav.pen[path[i]] += 4.0f;
                      memmove(tstuck, tstuck + 1, 3 * sizeof(double)); tstuck[3] = now;
                      // (2026-11-08, the user: "why was 'second' walker stationary so long with the smg and assault rifle?": 24 s at a ledge on
                      //  Guardian's middle level, its path to a hidden enemy 17 units off dropping ~3 units -- creeping, "stuck", backing off,
                      //  replanning the same stretch: in the goal draw (mode 2) four stucks never dropped the goal. Now they do -- the draw
                      //  picks another at once)
                      if (now - tstuck[0] < 10.0) { nav_say("  [nav] stuck 4 times at (%.1f %.1f %.1f): a new goal\n", pos[0], pos[1], pos[2]);
                          if (mode != 2) goal = -1; else if (g_eg.on && gl_on()) { gl_drop(now, "stuck 4 times"); goal = -1; plen = 0; }
                          memset(tstuck, 0, sizeof tstuck); }
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
    if (arbw == ARB_BOARD && gbd <= 4.0f) {   // boarding: straight to the point behind the turret, facing the turret, act pulsed (0.4 s a second) close in
        float way = atan2f(gbp[1] - o->c[1], gbp[0] - o->c[0]), fy = atan2f(gface[1] - cam[1], gface[0] - cam[0]), fp = atan2f(gface[2] - cam[2], sqrtf((gface[0] - cam[0]) * (gface[0] - cam[0]) + (gface[1] - cam[1]) * (gface[1] - cam[1])));
        yaw = fy; pitch = fp; out |= 1; float rel = bot_wrap(way - yaw), sp = gbd > 1.0f ? 1.0f : gbd > 0.4f ? 0.45f : 0.15f; tx = cosf(rel) * sp; ty = sinf(rel) * sp; out |= 2;
        { static double ttap; if (g_gun_press_ok && now - ttap > 1.2) { ttap = now; g_bot_act_until = now + 0.15; } }   // (a TAP, then 1.2 s to see it seated: a held press seats and then unseats)
        { static double tl; if (bot_log() && now - tl > 1.0) { tl = now; nlog("[nhook] [gunner] boarding: %.1f units from the point behind the turret%s", gbd, g_gun_press_ok ? ", right behind it: pressing act" : ""); } } }
    else if (arbw == ARB_FETCH && g_pick_d < 3.0f) {   // (the last 3 units of a weapon fetch: straight at it -- it may lie off the walking graph)
        float way = atan2f(g_bot_pick_pos[1] - o->c[1], g_bot_pick_pos[0] - o->c[0]), rel = bot_wrap(way - yaw), sp = g_pick_d > 1.0f ? 1.0f : g_pick_d > 0.3f ? 0.45f : 0.0f;   /* (08:05: onto it, then still) */ tx = cosf(rel) * sp; ty = sinf(rel) * sp; out |= 2; }
    else if (arbw == ARB_FIGHT) { tx = ftx; ty = fty; out |= 2; }   // (the fighter's moves: the arbiter gave it the legs)
    else if (arbw == ARB_HOLD) { tx = 0; ty = 0; out |= 2; }   // (levels apart: the enemy below -- hold the high ground)
    if (testb) { g_bot_btn = ((out & 4) ? g_bot_btn : 0) | g_bot_testbtn; out |= 4; }   // (do=botbtn: held until its time is up)
    // fire: the control record's bit 19 (+0xC8) -- the local unit fires, and the control packet carries the weapon's firing state
    // to the server (halo3+0x5452c reads it from the local weapon: a packet bit of its own does nothing; 2026-11-08 scan: bits 19
    // and 21 fire, 1 jumps). NHOOK_BOT_FIREBIT=<hex> = another bit.
    { static uint64_t fb = 0; if (!fb) { const char* e = getenv("NHOOK_BOT_FIREBIT"); fb = e ? strtoull(e, 0, 16) : 0x80000ull; }
      static uint64_t gb = 0; if (!gb) { const char* e = getenv("NHOOK_BOT_GRENBIT"); gb = e ? strtoull(e, 0, 16) : 0x40000000ull; }
      static uint64_t pkb = 1; if (pkb == 1) { const char* e = getenv("NHOOK_BOT_PICKBIT"); pkb = e ? strtoull(e, 0, 16) : 0; }
      // (a weapon that acts on a pull -- hammer, sword, sniper, magnum, rockets: the trigger pulsed 0.12 s down / 0.12 s up while firing)
      { static int was; if (spar && g_bot_fire && !was) g_bot_fire_t0 = now; was = spar && g_bot_fire; }
      static int nofire = -1; if (nofire < 0) { const char* e = getenv("NHOOK_BOT_FIRE"); nofire = e && !strcmp(e, "0"); }   // (=0: never pull the trigger -- melee-battle.sh MELEE_ONLY)
      // automatic weapons at range fire in BURSTS (2026-11-08, the user: a walker "shanking shots due to the random bloom on the AR,
      // spreading out at range ... lack of weapon proficiency"; shim/tools/fire-rhythm.py: aiming at an enemy with an automatic
      // weapon, the top-quarter players fire 9.0 shots/s within 8 units, 3.1 at 8-15, 2.0 at 15-25, 1.4 beyond -- the walkers 5.2,
      // 5.7 (!), 1.6): beyond 8 units 0.25 s bursts with a pause that keeps the players' share of their close-range rate (8-15: 34%,
      // 15-25: 22%, 25+: 15%); the spread settles in the pauses. Close in the trigger stays held. NHOOK_BOT_HOSE=1 = always held.
      static int hose = -1; if (hose < 0) hose = getenv("NHOOK_BOT_HOSE") != 0;
      int burst_ok = 1;
      if (!hose && !g_bot_pull && g_inv.held_c && !strcmp(g_inv.held_c, "auto") && tdist >= 8.0f) {
          float duty = tdist < 15 ? 0.34f : tdist < 25 ? 0.22f : 0.15f, per = 0.25f / duty;
          burst_ok = fmod(now - g_bot_fire_t0, per) < 0.25; }
      int trig = !nofire && spar && g_bot_fire && burst_ok && (!g_bot_pull || fmod(now - g_bot_fire_t0, 0.24) < 0.12) && !ob_carry; g_bot_trig = trig;   // (with the ball: melee only)
      { static double tobm; if (ob_carry && have_tg && tdist < 1.8f && now - tobm > 0.6) { tobm = now; g_bot_melee_until = now + 0.12; } }
      uint64_t want = (now < g_bot_melee_until ? bot_meleebits() : 0) | (trig ? fb : 0) | ((fbtn & 0x10000) ? gb : 0) | ((fbtn & 0x20000) ? pkb : 0) | (now < g_fjump_until ? g_bot_jump : 0);   // (fbtn bit 16: the grenade pulse, 17: pick up)
      if (want) { g_bot_btn = ((out & 4) ? g_bot_btn : 0) | want; out |= 4; } else if (!(out & 4)) g_bot_btn = 0; }
    if (diag) { uint64_t b = 0; int d = bot_diag(&tx, &ty, &b, now); out |= d; if (d & 4) g_bot_btn = b; else if (!(out & 4)) g_bot_btn = 0;
        static double tl; if (now - tl > 0.25) { tl = now; const float* v = o->vel;
            nlog("[nhook] [bot] test: pos (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) yaw %.1f throttle (%.2f, %.2f) buttons 0x%llx; shots %u deletes %lu", o->c[0], o->c[1], o->c[2], v[0], v[1], v[2],
                 yaw0 * 57.2958f, tx, ty, (d & 4) ? (unsigned long long)b : 0ull, g_nov_shot_n, g_nov_del_calls); } }
    { static int fo = -1; static float fy, fp; static int hp; if (fo < 0) { const char* e = getenv("NHOOK_BOT_FACE"); int k = e ? sscanf(e, "%f,%f", &fy, &fp) : 0; fo = k >= 1; hp = k == 2; }
      if (fo) { yaw = fy / 57.29578f; if (hp) pitch = fp / 57.29578f; out |= 1; } }   // NHOOK_BOT_FACE (above)
    if (g_bot_shadow) { g_sh_yaw = yaw; g_sh_pitch = pitch; g_sh_aim_ok = 1; shadow_log(now, o, have_tg ? &ftg : 0, tdist, engaged, yaw, pitch, tx, ty, yaw0, pitch0, rec); g_bot_out = 0; g_bot_btn = 0; return; }   // (decided, never published)
    g_bot_yaw = yaw; g_bot_pitch = pitch; g_bot_tx = tx; g_bot_ty = ty; g_bot_out = out;
    bot_camp_tick(now, tx, ty, out, engaged, fight && now - g_bot_hold_high < 0.3);   // (camping: a chat line, above)
    // the virtual controller: left stick = the throttle (y forward, x right = -left), A = jump, right trigger = fire
    { uint32_t tp = g_bot_testpad && now < g_bot_testpad_until ? g_bot_testpad : 0;   // (do=botpad)
      bot_pad(-ty, tx, (g_bot_trig || (tp & 0x80000000u)) ? 1.0f : 0.0f, (((out & 4) && (g_bot_btn & g_bot_jump)) ? 1u : 0u) | (fbtn & 0xFFFF) | ((fbtn & 0x10000) ? 0x40000000u : 0) | (tp & 0x4000FFFFu)); }
    if (!(spar && g_bot_err >= 0)) g_bot_fire = 0;
}
// ---- THE BRAIN AS A RELOADABLE MODULE (2026-11-09, the user: "I want those changes to be loaded into the running bot without a restart
// or rejoin"). nbrain.dylib is this same code built with -DNH_BRAIN (build.sh): no start-up, no hooks, no interposing; the engine
// context, the current map and its tag names and the hooks come from the body (nhx_* exports). Every step the body hands it the game's
// state (NhIO inputs: who it is, its leader, the server, the match, line-of-sight answers, test controls) and takes back what the hooks
// write (NhIO outputs: aim, throttle, buttons, the act / melee / switch windows, line-of-sight requests). The body looks at
// nbrain.dylib (beside nhook.dylib, or NHOOK_BRAIN) every 2 s: a newer one is copied to a fresh file (dlopen caches by path), loaded,
// its layout version checked, and from the next step it is the walker -- the game, its connection and its place never stop. A brain's
// own memory (path, goal, models, live knobs) starts again at a load. Old brains are never unloaded (threads they started -- the
// walking-graph builder's waiter, chat lines -- may still run in them). NHOOK_NO_BRAIN=1 = the built-in walker only. [brain] lines.
#define NHIO_VERSION 1
typedef struct {
    int version;
    // body -> brain
    uint32_t my_unit, follow_unit, human_unit[8]; int human_n; char srv_addr[96], nick[64], st_curmap[64], trk_mode[48], veh[64];
    double gameplay_t0; int rd_mode, rd_range, vpad_on, spar, nav_mode_rt, ht_my_slot, shadow; float pt[3]; uint64_t jump;
    uint64_t testbtn; double testbtn_until; uint32_t testpad; double testpad_until;
    int los_done, los_res; uint32_t los_res_tgt; double los_t; uint64_t inp_b8_acc;
    // brain -> body
    int out; float yaw, pitch, tx, ty; uint64_t btn; int fire, fighting, trig; float err; uint64_t unit_p;
    double melee_until, act_until, switch_until, seat_t, gun_exit_until;
    int los_req; float los_p[3], los_v[3]; uint32_t los_ign[2], los_tgt; int nav_act; uint32_t bot_unit;
} NhIO;
static void io_in_pack(NhIO* io) {   // (the body) its state -> the brain
    io->version = NHIO_VERSION; io->my_unit = g_my_unit; io->follow_unit = g_follow_unit; io->human_n = g_human_n; for (int i = 0; i < 8; i++) io->human_unit[i] = g_human_unit[i];
    memcpy(io->srv_addr, g_srv_addr, sizeof io->srv_addr); memcpy(io->nick, g_nick, sizeof io->nick); memcpy(io->st_curmap, g_st_curmap, sizeof io->st_curmap);
    memcpy(io->trk_mode, g_trk_mode, sizeof io->trk_mode); memcpy(io->veh, g_bot_veh, sizeof io->veh); memcpy(io->pt, g_bot_pt, sizeof io->pt);
    io->gameplay_t0 = g_gameplay_t0; io->rd_mode = g_rd_mode; io->rd_range = g_rd_range; io->vpad_on = g_vpad_on; io->spar = g_bot_spar; io->nav_mode_rt = g_nav_mode_rt;
    io->ht_my_slot = g_ht_my_slot; io->shadow = g_bot_shadow; io->jump = g_bot_jump; io->testbtn = g_bot_testbtn; io->testbtn_until = g_bot_testbtn_until;
    io->testpad = g_bot_testpad; io->testpad_until = g_bot_testpad_until; io->los_done = g_los_done; io->los_res = g_los_res; io->los_res_tgt = g_los_res_tgt; io->los_t = g_los_t;
    io->inp_b8_acc = g_inp_b8_acc;
    io->los_req = g_los_req; }   // (the brain keeps counting from the body's)
static void io_in_apply(const NhIO* io) {   // (the brain) the body's state -> this copy's globals
    g_my_unit = io->my_unit; g_follow_unit = io->follow_unit; g_human_n = io->human_n; for (int i = 0; i < 8; i++) g_human_unit[i] = io->human_unit[i];
    memcpy((void*)g_srv_addr, io->srv_addr, sizeof io->srv_addr); memcpy(g_nick, io->nick, sizeof io->nick); memcpy(g_st_curmap, io->st_curmap, sizeof io->st_curmap);
    memcpy(g_trk_mode, io->trk_mode, sizeof io->trk_mode); memcpy(g_bot_veh, io->veh, sizeof io->veh); memcpy(g_bot_pt, io->pt, sizeof io->pt);
    g_gameplay_t0 = io->gameplay_t0; g_rd_mode = io->rd_mode; g_rd_range = io->rd_range; g_vpad_on = io->vpad_on; g_bot_spar = io->spar; g_nav_mode_rt = io->nav_mode_rt;
    g_ht_my_slot = io->ht_my_slot; g_bot_shadow = io->shadow; g_bot_jump = io->jump; g_bot_testbtn = io->testbtn; g_bot_testbtn_until = io->testbtn_until;
    g_bot_testpad = io->testpad; g_bot_testpad_until = io->testpad_until; g_los_done = io->los_done; g_los_res = io->los_res; g_los_res_tgt = io->los_res_tgt; g_los_t = io->los_t;
    g_inp_b8_acc = io->inp_b8_acc; g_los_req = io->los_req; }
static void io_out_pack(NhIO* io) {   // (the brain) what the hooks write
    io->out = g_bot_out; io->yaw = g_bot_yaw; io->pitch = g_bot_pitch; io->tx = g_bot_tx; io->ty = g_bot_ty; io->btn = g_bot_btn; io->fire = g_bot_fire; io->fighting = g_bot_fighting;
    io->trig = g_bot_trig; io->err = g_bot_err; io->unit_p = g_bot_unit_p; io->melee_until = g_bot_melee_until; io->act_until = g_bot_act_until; io->switch_until = g_bot_switch_until;
    io->seat_t = g_bot_seat_t; io->gun_exit_until = g_gun_exit_until; io->los_req = g_los_req; memcpy(io->los_p, g_los_p, sizeof io->los_p); memcpy(io->los_v, g_los_v, sizeof io->los_v);
    io->los_ign[0] = g_los_ign[0]; io->los_ign[1] = g_los_ign[1]; io->los_tgt = g_los_tgt; io->nav_act = g_nav_act; io->bot_unit = g_bot_unit; }
static void io_out_apply(const NhIO* io) {   // (the body) -> the hooks' globals (the line-of-sight request last: its fields first)
    g_bot_yaw = io->yaw; g_bot_pitch = io->pitch; g_bot_tx = io->tx; g_bot_ty = io->ty; g_bot_btn = io->btn; g_bot_fire = io->fire; g_bot_fighting = io->fighting; g_bot_trig = io->trig;
    g_bot_err = io->err; g_bot_unit_p = io->unit_p; g_bot_melee_until = io->melee_until; g_bot_act_until = io->act_until; g_bot_switch_until = io->switch_until;
    g_bot_seat_t = io->seat_t; g_gun_exit_until = io->gun_exit_until; g_nav_act = io->nav_act; g_bot_unit = io->bot_unit;
    if (io->los_req != g_los_req) { memcpy(g_los_p, io->los_p, sizeof g_los_p); memcpy(g_los_v, io->los_v, sizeof g_los_v); g_los_ign[0] = io->los_ign[0]; g_los_ign[1] = io->los_ign[1];
        g_los_tgt = io->los_tgt; __atomic_store_n(&g_los_req, io->los_req, __ATOMIC_RELEASE); }
    g_bot_out = io->out; }
#ifdef NH_BRAIN
__attribute__((visibility("default"))) int nh_brain_version(void) { return NHIO_VERSION; }
__attribute__((visibility("default"))) int nh_brain_step(NhIO* io) { if (io->version != NHIO_VERSION) return 0;
    static int told; if (!told) { told = 1; nlog("[nhook] [brain] code built %s %s is stepping", __DATE__, __TIME__);
#ifdef NH_BRAIN_LESSON
        if (NH_BRAIN_LESSON[0]) nlog("[nhook] [brain] lesson: %s", NH_BRAIN_LESSON);
#endif
    }
    io_in_apply(io);
    { static char hmmap[64]; if (strcmp(hmmap, g_st_curmap)) { snprintf(hmmap, sizeof hmmap, "%s", g_st_curmap);   // (the death map of the map being played: the
          pthread_mutex_lock(&g_hm_mx); hm_switch(g_st_curmap); pthread_mutex_unlock(&g_hm_mx);                     //  brain's own copy -- nav_danger_update avoids its hot spots)
          nlog("[nhook] [brain] death map for %s: %u deaths", g_st_curmap[0] ? g_st_curmap : "-", (unsigned)g_hm.nd); } }
    bot_step(); io_out_pack(io); return 1; }
static void* bot_thread(void* p) { (void)p; return 0; }   // (never started here: the body's thread calls nh_brain_step)
#else
typedef int (*NhBrainStep)(NhIO*);
static NhBrainStep g_brain; static int g_brain_n;
static void brain_tick(void) {   // (the body, every 2 s) a newer nbrain.dylib -> loaded from a fresh copy and swapped in
    static double t; double now = mono_s(); if (now - t < 2.0) return; t = now;
    static int off = -1; if (off < 0) off = getenv("NHOOK_NO_BRAIN") != 0; if (off) return;
    static char path[1100]; if (!path[0]) { const char* e = getenv("NHOOK_BRAIN"); Dl_info di;
        if (e && *e) snprintf(path, sizeof path, "%s", e);
        else if (dladdr((void*)brain_tick, &di) && di.dli_fname) { snprintf(path, sizeof path, "%s", di.dli_fname); char* sl = strrchr(path, '/'); if (sl) snprintf(sl + 1, sizeof path - (size_t)(sl + 1 - path), "nbrain.dylib"); }
        else return; }
    struct stat st; if (stat(path, &st)) return; static struct timespec mt; static off_t sz;
    if (st.st_mtimespec.tv_sec == mt.tv_sec && st.st_mtimespec.tv_nsec == mt.tv_nsec && st.st_size == sz) return;
    mt = st.st_mtimespec; sz = st.st_size;
    char cp[1200]; snprintf(cp, sizeof cp, "%s/nbrain-%d-%d.dylib", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid(), g_brain_n + 1);
    { int in = open(path, O_RDONLY), outf = open(cp, O_WRONLY | O_CREAT | O_TRUNC, 0755); char b[65536]; ssize_t r; int ok = in >= 0 && outf >= 0;
      while (ok && (r = read(in, b, sizeof b)) > 0) if (write(outf, b, (size_t)r) != r) ok = 0;
      if (in >= 0) close(in); if (outf >= 0) close(outf); if (!ok) { nlog("[nhook] [brain] could not copy %s", path); return; } }
    void* h = dlopen(cp, RTLD_NOW | RTLD_LOCAL); if (!h) { nlog("[nhook] [brain] %s not loaded: %s", path, dlerror()); return; }
    int (*ver)(void) = (int (*)(void))dlsym(h, "nh_brain_version"); NhBrainStep f = (NhBrainStep)dlsym(h, "nh_brain_step");
    if (!ver || !f || ver() != NHIO_VERSION) { nlog("[nhook] [brain] %s refused: layout %d, this body's %d", path, ver ? ver() : -1, NHIO_VERSION); return; }
    g_brain = f; g_brain_n++; nlog("[nhook] [brain] #%d loaded from %s: the walker runs it from the next step (no restart)", g_brain_n, path); }
static void* bot_thread(void* p) { (void)p; pthread_setname_np("shim-bot");
    for (;;) { usleep(33000); brain_tick();
        if (g_brain) { NhIO io; memset(&io, 0, sizeof io); io_in_pack(&io); if (g_brain(&io)) io_out_apply(&io); else bot_step(); }
        else bot_step(); }
    return 0; }
#endif
static void bot_start(void) {   // first use: the thread (also from the launch switches)
    if (__atomic_exchange_n(&g_bot_started, 1, __ATOMIC_ACQ_REL)) return; pthread_t th; pthread_create(&th, 0, bot_thread, 0); pthread_detach(th); }
static int bot_parse_nav(const char* v) {   // -> mode (0 off, 1 roam, 2 enemy, 3 point, 4 vehicle), sets the target
    if (!v || !*v || !strcasecmp(v, "off") || !strcmp(v, "0")) return 0;
    if (!strcasecmp(v, "roam")) return 1; if (!strcasecmp(v, "enemy")) return 2;
    if (!strncasecmp(v, "vehicle:", 8)) { snprintf(g_bot_veh, sizeof g_bot_veh, "%s", v + 8); return 4; }
    float p[3]; if (sscanf(v, "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) { memcpy(g_bot_pt, p, 12); return 3; }
    return -1; }
static void bot_init(void) {   // launch switches (nstats_start)
    if (getenv("NHOOK_SHADOW") && strcmp(getenv("NHOOK_SHADOW"), "0")) { g_bot_shadow = 1;   // (the walker's brain on: fighter, goals, nav -- outputs never)
        setenv("NHOOK_SPAR", "1", 0); setenv("NHOOK_NAV", "enemy", 0); setenv("NHOOK_BOT_FIGHT", "1", 0); setenv("NHOOK_BOT_FIRE", "1", 0); setenv("NHOOK_SPAR_LOG", "1", 0);
        nlog("[nhook] [shadow] the walker rides along: it decides every step, publishes nothing; decisions -> tracks/shadow/"); }
    const char* j = getenv("NHOOK_BOT_JUMP"); if (j) g_bot_jump = strtoull(j, 0, 16);
    if (getenv("NHOOK_SPAR") && strcmp(getenv("NHOOK_SPAR"), "0")) g_bot_spar = 1;
    int m = bot_parse_nav(getenv("NHOOK_NAV")); if (m > 0) g_nav_mode_rt = m; else g_nav_mode_rt = 0;
    if (g_bot_spar || g_nav_mode_rt > 0 || getenv("NHOOK_BOT_THROTTLE") || getenv("NHOOK_BOT_BTNSCAN") || getenv("NHOOK_BOT_FACE") || getenv("NHOOK_BOT_HOLDBTN")) bot_start(); }
static int bot_control(const char* act, const char* qs) {   // POST do=spar&on=0|1, do=walk&mode=off|roam|enemy|x,y,z|vehicle:<name>, do=botbtn&bits=<hex>&secs=
    if (!bot_allowed()) return 0;
    if (!strcmp(act, "botwslot")) { const char* v = strstr(qs, "v="), * t = strstr(qs, "secs="); if (!v) return 0;
        g_bot_wsel_v = atoi(v + 2); g_bot_wsel_until = mono_s() + (t ? strtod(t + 5, 0) : 1.0); return 1; }
    if (!strcmp(act, "botpkt")) { const char* a = strstr(qs, "b8="), * c = strstr(qs, "b2c="), * t = strstr(qs, "secs=");
        g_bot_tpk8 = a ? (uint32_t)strtoul(a + 3, 0, 16) : 0; g_bot_tpk2c = c ? (uint32_t)strtoul(c + 4, 0, 16) : 0;
        g_bot_tpk_until = mono_s() + (t ? strtod(t + 5, 0) : 0.3); bot_start(); return 1; }
    if (!strcmp(act, "botpad")) { const char* b = strstr(qs, "b="), * t = strstr(qs, "secs="); if (!b) return 0;
        g_bot_testpad = (uint32_t)strtoul(b + 2, 0, 16); g_bot_testpad_until = mono_s() + (t ? strtod(t + 5, 0) : 0.3); bot_start(); return 1; }
    if (!strcmp(act, "botbtn")) { const char* b = strstr(qs, "bits="), * t = strstr(qs, "secs="); if (!b) return 0;
        g_bot_testbtn = strtoull(b + 5, 0, 16); g_bot_testbtn_until = mono_s() + (t ? strtod(t + 5, 0) : 1.0); bot_start(); return 1; }
    if (!strcmp(act, "walk") && !bot_walk_ok()) return 0;   // (a joined client: the server decides its moves)
    if (!strcmp(act, "spar")) { const char* v = strstr(qs, "on="); if (!v) return 0; g_bot_spar = atoi(v + 3) != 0; if (g_bot_spar) bot_start(); return 1; }
    const char* v = strstr(qs, "mode="); if (!v) return 0; char m[96]; size_t k = 0; v += 5; while (*v && *v != '&' && k + 1 < sizeof m) { m[k++] = *v == '+' ? ' ' : *v; v++; } m[k] = 0;
    { char d[96]; size_t a = 0; for (size_t i = 0; m[i] && a + 1 < sizeof d; i++) { if (m[i] == '%' && m[i + 1] && m[i + 2]) { char h[3] = {m[i + 1], m[i + 2], 0}; d[a++] = (char)strtol(h, 0, 16); i += 2; } else d[a++] = m[i]; } d[a] = 0; memcpy(m, d, a + 1); }
    int md = bot_parse_nav(m); if (md < 0) return 0; g_nav_mode_rt = md; if (md) bot_start(); return 1; }
static void bot_overlay_json(StBuf* s) {   // the snapshot's overlay keys for the page's "Sparring aim" button and "Walk" list
    static const char* const mn[5] = {"off", "roam", "enemy", "point", "vehicle"}; int m = g_nav_mode_rt; int ok = bot_allowed();
    if (g_bot_shadow) sb_put(s, ",\"shadow\":{\"target\":\"%08x\",\"distance\":%.1f,\"fire\":%s,\"fresh\":%s}", g_sh_tgt, g_sh_d, g_sh_fire ? "true" : "false", mono_s() - g_sh_t < 1.0 ? "true" : "false");
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
