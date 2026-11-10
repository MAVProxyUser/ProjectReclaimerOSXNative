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
    snprintf(t, n, "%s/../../re-tools/h3nav.py", d);
    // (2026-11-09: a brain runs from a COPY in $TMPDIR (hot swaps) -- the path above then led nowhere and a new map got no walking graph,
    //  "re-tools/h3nav.py not found" on hugetowers. Then the checkout the body came from: DYLD_INSERT_LIBRARIES (shim/native.sh injects
    //  <checkout>/shim/build/nhook.dylib), else NHOOK_STATS_PAGE (<checkout>/shim/src/stats_page.html))
    if (!access(t, R_OK)) return;
    const char* e = getenv("DYLD_INSERT_LIBRARIES"); char b[1024] = "";
    if (e && *e) { snprintf(b, sizeof b, "%s", e); char* c = strchr(b, ':'); if (c) *c = 0; char* q = strstr(b, "/shim/build/"); if (q) *q = 0; else b[0] = 0; }
    if (!b[0] && (e = getenv("NHOOK_STATS_PAGE")) && *e) { snprintf(b, sizeof b, "%s", e); char* q = strstr(b, "/shim/src/"); if (q) *q = 0; else b[0] = 0; }
    if (b[0]) snprintf(t, n, "%s/re-tools/h3nav.py", b); }

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
// (2026-11-09, the user: "brain updates should be seamless to the walker even if it is in the middle of an active firefight. it should
// never pause movement etc. the brain should always be ready for a hot takeover"): g_brain_warm = this copy of the brain is warming up
// beside the running one (nh_brain_warm): it reads the same world and decides, but writes nothing -- no log lines, no files, no outputs
static int g_brain_warm;
// SHIELDLESS GAMES (2026-11-09: SWAT on the new approved server -- no shields, a headshot kills): g_noshield = no live player has shown
// a shield above 0.05 for 20 s of this map (set in bot_step_body). The rules that read the walker's own shield as "how hurt am I" then
// stand down -- otherwise a permanent 0 meant: back off inside every fight (fight movement), back off to recharge, break off, and every
// death "entered hurt" (the tuner stepping recharge settings for nothing). NHOOK_BOT_NOSHIELD=1 / 0 forces it on / off (live)
static int g_noshield;
// (2026-11-09) the walker's own kills / deaths in THIS match from the scoreboard (the game results, nstats.c g_rs_cur; -1 = not known):
// the three-minute review counted "a target that died while I fought it" as a kill -- 17 "kills" in a UK match the scoreboard put at 3
static int g_my_kills = -1, g_my_deaths = -1;
static double g_nav_ok_t;   // (2026-11-09) the last time the planner found a path (the unstick: no path anywhere for 45 s = stuck too)
// INFECTION (2026-11-09, the user: "yes" to an Infection profile -- the walker went in with its normal fight: as a zombie the caution of
// a bad window HALVED its charge, as a human it fought alone): g_inf_me = its side, from the sides the body hands the brain (NhIO v3):
// 1 a zombie, 0 a human, -1 not Infection. A zombie hunts the nearest human (the lonely ones first), charges straight in with a light
// weave whatever the caution / crowd / shield say, no cover / recharge / break-off. A human goes to the biggest group of humans near it
// and holds there (kiting swords / hammers, NHOOK_BOT_KITE, as before). Live knobs NHOOK_INF (1; 0 = off), NHOOK_INF_ZWEAVE (0.35),
// NHOOK_INF_GROUP (8 units: a group), NHOOK_INF_HOLD (4 units: close enough to hold)
static int g_inf_me = -1; static double g_inf_hold_t;
static int bot_log(void) { static int l = -1; if (l < 0) l = getenv("NHOOK_SPAR_LOG") ? 1 : 0; return l && !g_brain_warm; }

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
    // (2026-11-09: the change time to the nanosecond -- the Walker tab can write twice in one second with the same file size)
    static struct timespec mt; static off_t sz; static int had; struct stat st; const char* p = lk_path();
    if (stat(p, &st) != 0) { if (had) { had = 0; g_nlk = 0; mt.tv_sec = 0; mt.tv_nsec = 0; nlog("[nhook] [knobs] %s gone: the launch values again", p); } return; }
    if (had && st.st_mtimespec.tv_sec == mt.tv_sec && st.st_mtimespec.tv_nsec == mt.tv_nsec && st.st_size == sz) return;
    FILE* f = fopen(p, "r"); if (!f) return; char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    LiveKnob k[64]; int nk = 0; const char* q = b; char list[600]; int lq = 0; list[0] = 0;
    while (nk < 64 && (q = strchr(q, '"'))) { const char* e = strchr(q + 1, '"'); if (!e) break; size_t L = (size_t)(e - q - 1);
        const char* c = e + 1; while (*c == ' ' || *c == ':' || *c == '\t') c++; char* end; float v = strtof(c, &end);
        if (end != c && L > 0 && L < sizeof k[0].n) { memcpy(k[nk].n, q + 1, L); k[nk].n[L] = 0; k[nk].v = v;
            if (lq < (int)sizeof list - 60) lq += snprintf(list + lq, sizeof list - (size_t)lq, "%s%s=%.3g", nk ? " " : "", k[nk].n + (strncmp(k[nk].n, "NHOOK_BOT_", 10) ? 0 : 10), v); nk++; }
        q = end != c ? end : e + 1; }
    memcpy(g_lk, k, sizeof k); g_nlk = nk; mt = st.st_mtimespec; sz = st.st_size; had = 1;
    nlog("[nhook] [knobs] live from %s: %s", p, nk ? list : "(none)"); }
static int lk_get(const char* n, float* v) { for (int i = 0; i < g_nlk; i++) if (!strcmp(g_lk[i].n, n)) { *v = g_lk[i].v; return 1; } return 0; }
// LESSONS (2026-11-09 11:28, the user: "i feel like the *lessons* in 'what the game is learning' are not applied to the walkers kernel"):
// shim/tools/learnings.py writes the settings its lessons call for to tracks/walker-lessons.json (strafe / jump by role, push, the
// high ground, shields up, the sword's distance ...); they apply UNDER the live knob file -- a value set on the Walker tab wins -- and
// over the launch values. Re-read within 2 s of a change; NHOOK_BOT_LESSONS=0 (environment or live knob) = off. [lessons] lines
static LiveKnob g_ls[64]; static int g_nls;
static char g_wr[40][40]; static int g_nwr;   // ("_wrank": the players' weapons best first, tag-style keys -- bot_wpn_rank)
static void ls_tick(void) {
    static double t; double now = mono_s(); if (now - t < 2.0) return; t = now;
    static struct timespec mt; static int had; struct stat st; char p[1100]; const char* h = getenv("HOME");
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/walker-lessons.json", h ? h : "/tmp");
    if (stat(p, &st) != 0) { if (had) { had = 0; g_nls = 0; } return; }
    if (had && st.st_mtimespec.tv_sec == mt.tv_sec && st.st_mtimespec.tv_nsec == mt.tv_nsec) return;
    FILE* f = fopen(p, "r"); if (!f) return; char b[16384]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    LiveKnob k[64]; int nk = 0; const char* q = b; char list[700]; int lq = 0; list[0] = 0;
    while (nk < 64 && (q = strchr(q, '"'))) { const char* e = strchr(q + 1, '"'); if (!e) break; size_t L = (size_t)(e - q - 1);
        const char* c = e + 1; while (*c == ' ' || *c == ':' || *c == '\t') c++; char* end; float v = strtof(c, &end);
        if (end != c && L > 6 && L < sizeof k[0].n && !strncmp(q + 1, "NHOOK_", 6)) { memcpy(k[nk].n, q + 1, L); k[nk].n[L] = 0; k[nk].v = v;
            if (lq < (int)sizeof list - 60) lq += snprintf(list + lq, sizeof list - (size_t)lq, "%s%s=%.3g", nk ? " " : "", k[nk].n + 6, v); nk++; }
        q = end != c ? end : e + 1; }
    memcpy(g_ls, k, sizeof k); g_nls = nk; mt = st.st_mtimespec; had = 1;
    { int nw = 0; const char* w = strstr(b, "\"_wrank\""); const char* end = w ? strchr(w, ']') : 0; w = w ? strchr(w, '[') : 0;
      while (w && end && nw < 40) { const char* a = strchr(w, '"'); if (!a || a > end) break; const char* z = strchr(a + 1, '"'); if (!z || z > end) break;
          size_t L = (size_t)(z - a - 1); if (L > 0 && L < sizeof g_wr[0]) { memcpy(g_wr[nw], a + 1, L); g_wr[nw][L] = 0; nw++; } w = z + 1; }
      g_nwr = nw; }
    nlog("[nhook] [lessons] from the players' data (%s): %s; %d weapons ranked%s%s", p, nk ? list : "(none)", g_nwr, g_nwr ? ", best " : "", g_nwr ? g_wr[0] : ""); }
static int ls_get(const char* n, float* v) {
    static int off = -1; if (off < 0) { const char* e = getenv("NHOOK_BOT_LESSONS"); off = e && !strcmp(e, "0"); }
    float on = 1; if (off || (lk_get("NHOOK_BOT_LESSONS", &on) && on <= 0)) return 0;
    for (int i = 0; i < g_nls; i++) if (!strcmp(g_ls[i].n, n)) { *v = g_ls[i].v; return 1; } return 0; }
static void lk_over(float* v, const char* n) { float x; if (lk_get(n, &x)) *v = x; else if (ls_get(n, &x)) *v = x; }

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
    // (06:55: the walker is to mimic the BEST players -- walker-model.py --top writes tracks/walker-model-top.json, the top fifth of each
    //  recorded match by K/D (reaction 0.85 s vs 0.94 for everyone, strafe switch 0.74 vs 0.81 s); its fight numbers are read first
    //  when present (the maps' lanes still come from everyone's model). NHOOK_BOT_MODEL_TOP=0 (live) = the everyone model)
    { float mt = 1; lk_over(&mt, "NHOOK_BOT_MODEL_TOP"); char tp[1024]; struct stat st2;
      snprintf(tp, sizeof tp, "%s/Library/Application Support/ReclaimerMac/tracks/walker-model-top.json", h ? h : "");
      if (!(e && *e) && mt > 0 && stat(tp, &st2) == 0) snprintf(path, sizeof path, "%s", tp); }
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
static double g_escape_until, g_escape_t; static float g_escape_dir; static double g_act_last; static int g_fetch_abort;   // (14:50: the wedged-spot escape)
static double g_fjump_until; static int g_bot_camo_hold;   // (a jump in a fight: NHOOK_BOT_FJUMP)
static int bot_fcmp(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
// LANES LEARNED LIVE (2026-11-09, the user: "lanes for the new map should come quick. learn them with each 3 minute brain update"):
// the lanes files of a map -- the real heatmaps folder's and the walker's own (NHOOK_HEATMAP_DIR, where its body saves the match it is
// in every 30 s: every player's moves it sees) -- summed into one grid; mixed into g_nav.pop (with the model's heat, g_bm_heat) and the
// goal lanes (g_gll) by bm_lanes_mix; bm_lanes_refresh reads them on a side thread at each revision, bm_lanes_apply mixes them in on
// the walker's thread at its next step (the planner's arrays are only touched there)
static float* g_bm_heat; static int g_bm_heat_n;
static int bm_lanes_read(const char* map, LnGrid* lg) {
    const char* ld = getenv("NHOOK_BOT_LANES"), * h = getenv("HOME"), * hd = getenv("NHOOK_HEATMAP_DIR"); if (ld && !strcmp(ld, "0")) return 0;
    char p1[1100], p2[1100] = ""; int n = 0;
    if (ld && *ld) snprintf(p1, sizeof p1, "%s/%s.lanes", ld, map); else snprintf(p1, sizeof p1, "%s/Library/Application Support/ReclaimerMac/heatmaps/%s.lanes", h ? h : "", map);
    if (hd && *hd) snprintf(p2, sizeof p2, "%s/%s.lanes", hd, map);
    const char* ps[2] = {p1, p2};
    for (int k = 0; k < 2; k++) { if (!ps[k][0] || (k == 1 && !strcmp(p1, p2))) continue;
        { char rp1[PATH_MAX], rp2[PATH_MAX]; if (k == 1 && realpath(p1, rp1) && realpath(p2, rp2) && !strcmp(rp1, rp2)) continue; }
        LnSrc* src = 0; uint32_t ns = 0; double since = 0; if (ln_read(ps[k], lg, &src, &ns, &since)) n++; free(src); }
    return n && lg->used ? n : 0; }
static int bm_lanes_mix(LnGrid* lg, float* pop) {   // lanes -> pop (each node the larger of its heat and lanes) + the goal lanes; -> lane cells
    int lcells = 0; float* lpop = calloc((size_t)g_nav.n, sizeof(float));
    for (uint32_t i = 0; lpop && i < lg->cap; i++) { const LnCell* c = &lg->c[i]; if (!(c->foot > 0)) continue; float x = (c->x + 0.5f) * LN_CELL, y = (c->y + 0.5f) * LN_CELL;
        int cx = (int)floorf((x - g_nav.x0) / g_nav.cell), cy = (int)floorf((y - g_nav.y0) / g_nav.cell); if (cx < 0 || cy < 0 || cx >= g_nav.w || cy >= g_nav.h) continue;
        int cc = cy * g_nav.w + cx; for (uint32_t k = g_nav.first[cc]; k < g_nav.first[cc + 1]; k++) lpop[k] += c->foot; lcells++; }
    { int ng = 0; float* fv = malloc(sizeof(float) * (lg->used + 1)); for (uint32_t i = 0; fv && i < lg->cap; i++) if (lg->c[i].foot > 0) fv[ng++] = lg->c[i].foot;   // (the busier half: goal material)
      float med = 0; if (fv && ng) { qsort(fv, (size_t)ng, sizeof(float), bot_fcmp); med = fv[ng / 2]; } free(fv);
      GlLane* gl2 = malloc(sizeof(GlLane) * (size_t)(ng + 1)); int k3 = 0; float ws2 = 0;
      for (uint32_t i = 0; gl2 && i < lg->cap; i++) if (lg->c[i].foot > med && lg->c[i].foot > 1) { gl2[k3].x = (lg->c[i].x + 0.5f) * LN_CELL; gl2[k3].y = (lg->c[i].y + 0.5f) * LN_CELL; gl2[k3].w = logf(1 + lg->c[i].foot); ws2 += gl2[k3].w; k3++; }
      GlLane* old2 = g_gll; g_gll = gl2; g_ngll = k3; g_gll_wsum = ws2; free(old2); }
    if (lpop) { int nz = 0; for (int i = 0; i < g_nav.n; i++) if (lpop[i] > 0) nz++;
        float* v = nz ? malloc(sizeof(float) * (size_t)nz) : 0; int k2 = 0; for (int i = 0; v && i < g_nav.n; i++) if (lpop[i] > 0) v[k2++] = lpop[i];
        float q99 = 0; if (v && k2) { qsort(v, (size_t)k2, sizeof(float), bot_fcmp); q99 = v[(k2 - 1) * 99 / 100]; }   /* (the 99th percentile) */
        free(v);
        if (q99 > 0) for (int i = 0; i < g_nav.n; i++) { float lv = lpop[i] > 0 ? fminf(1.0f, logf(1 + lpop[i]) / logf(1 + q99)) : 0; if (lv > pop[i]) pop[i] = lv; }
        free(lpop); }
    return lcells; }
static LnGrid* volatile g_lr_ready; static volatile int g_lr_busy; static char g_lr_map[64];
static void* bm_lr_thread(void* a) { (void)a; LnGrid* g = calloc(1, sizeof *g); if (g && !bm_lanes_read(g_lr_map, g)) { ln_free(g); free(g); g = 0; }
    if (!g) { g_lr_busy = 0; return 0; } __atomic_store_n(&g_lr_ready, g, __ATOMIC_RELEASE); return 0; }
static void bm_lanes_refresh(const char* map) {   // (a revision) the lanes files read again on a side thread
    if (g_lr_busy || !g_nav.ok || !map || !map[0]) return; g_lr_busy = 1; snprintf(g_lr_map, sizeof g_lr_map, "%s", map);
    pthread_t th; if (pthread_create(&th, 0, bm_lr_thread, 0)) { g_lr_busy = 0; return; } pthread_detach(th); }
static void bm_lanes_apply(void) {   // (every step, the walker's thread) a refreshed grid -> mixed in
    LnGrid* g = __atomic_exchange_n(&g_lr_ready, (LnGrid*)0, __ATOMIC_ACQ_REL); if (!g) return; g_lr_busy = 0;
    if (!strcmp(g_lr_map, g_st_curmap) && g_nav.ok && g_bm_heat && g_bm_heat_n == g_nav.n) {
        double t0 = mono_s(); float* pop = malloc(sizeof(float) * (size_t)g_nav.n);
        if (pop) { memcpy(pop, g_bm_heat, sizeof(float) * (size_t)g_nav.n); int lc = bm_lanes_mix(g, pop); float* old = g_nav.pop; g_nav.pop = pop; free(old);
            double tot = 0; for (uint32_t i = 0; i < g->cap; i++) tot += g->c[i].foot;
            if (bot_log()) nlog("[nhook] [brain] lanes relearned for %s: %d lane cells, %.1f km of players' moves (this match's included), mixed in %.0f ms", g_st_curmap, lc, tot * LN_CELL * 3.048 / 1000.0, (mono_s() - t0) * 1000); } }
    ln_free(g); free(g); }
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
    // (2026-11-09: + the walker's OWN heatmaps folder, NHOOK_HEATMAP_DIR -- where its body saves the lanes of the match it is in every
    //  30 s; and the heat part kept apart, g_bm_heat, so every three-minute revision mixes the newest lanes in again: bm_lanes_refresh)
    { free(g_bm_heat); g_bm_heat = malloc(sizeof(float) * (size_t)g_nav.n); if (g_bm_heat) memcpy(g_bm_heat, pop, sizeof(float) * (size_t)g_nav.n); g_bm_heat_n = g_nav.n; }
    int lcells = 0; { LnGrid lg = {0}; if (bm_lanes_read(map, &lg)) lcells = bm_lanes_mix(&lg, pop); ln_free(&lg); }
    if (mx <= 0 && !lcells) { free(pop); nlog("[nhook] [fight] no heat or lanes for %s: shortest paths", map); return; }
    float* old = g_nav.pop; g_nav.pop = pop; free(old); nlog("[nhook] [fight] %s: player lanes from %d heat cells and %d lane cells (A* prefers them)", map, cells, lcells); }
static int inf_group_goal(const NovObj* objs, int n, int me, const float* pos, int* goal, double now) {   // -> 0 not handled, 1 a goal, 2 hold here
    float gr = 8, hd = 4; lk_over(&gr, "NHOOK_INF_GROUP"); lk_over(&hd, "NHOOK_INF_HOLD");
    int bi = -1, bn = 0; float bd = 1e30f;
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || nov_side_of(t->handle) != 0) continue;
        float dx = t->c[0] - pos[0], dy = t->c[1] - pos[1], d = sqrtf(dx * dx + dy * dy); if (d > 80) continue;
        int nb = 1; for (int j = 0; j < n; j++) { const NovObj* u = &objs[j]; if (j == i || j == me || u->type || u->dead || nov_side_of(u->handle) != 0) continue;
            float ex = u->c[0] - t->c[0], ey = u->c[1] - t->c[1]; if (ex * ex + ey * ey < gr * gr) nb++; }
        if (nb > bn || (nb == bn && d < bd)) { bn = nb; bd = d; bi = i; } }
    if (bi < 0) return 0;
    if (bd <= hd) { g_inf_hold_t = now; return 2; }
    float tp[3] = {objs[bi].c[0], objs[bi].c[1], objs[bi].c[2] - 0.35f}; int gn = nav_node_at(tp); if (gn < 0) return 0; *goal = gn;
    static double tl; if (bot_log() && now - tl > 15) { tl = now; nlog("[nhook] [inf] a human: to the group of %d humans at (%.0f, %.0f), %.0f units away", bn, objs[bi].c[0], objs[bi].c[1], bd); }
    return 1; }
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
                 int ammo, ammo_max, other_ammo, reserve; } BotInv;   // (reserve: rounds not loaded, the field before the learnt magazine; -1 unknown)   // (ammo: rounds loaded, -1 unknown; ammo_max: the most seen in that weapon)
static BotInv g_inv;
// ODDBALL (2026-11-09, the user: "the walkers will have to learn to run with the oddball and melee only and obtain the oddball from other
// players with whatever weapon given"): the ball's state, read every 0.3 s (ob_scan): 0 none seen, 1 loose, 2 I carry it, 3 a teammate
// does, 4 an enemy does (its carrier: the fighter's first target, nbot_engage.c -- the game marks the carrier on every screen)
static volatile uint32_t g_ob_enemy_carrier; static int g_ob_state; static float g_ob_pos[3];
#include "nbot_engage.c"   // the fighter's tactics from the players' fights (engage-model.json: awareness, target choice, movement)
#include "nbot_goals.c"    // radar, memory, own deaths, and where to go (the goal draw)
// THE HANDOFF (2026-11-09, the hot takeover; the user: "brain updates should be seamless to the walker even if it is in the middle of an
// active firefight"): the first mid-match swap (15:27) did not stall, but the new brain had chosen its own target and goal while it
// warmed up -- at the swap it turned 15-19 deg to another enemy and walked to another goal. Now the running brain publishes its intent
// every step -- its target (and since when) and its goal -- in a small shared-memory block of this process (shm "/nhbrain-<pid>",
// a seqlock: odd while written); a brain warming up beside it adopts them every warm step, so at the swap it fights the same enemy and
// walks to the same goal, and from then on decides for itself. Layout HAND_VER; another layout is ignored (a cold start of intent).
typedef struct { uint32_t magic, ver; volatile uint32_t seq; uint32_t target; double target_t0; int gk; float gpos[3]; uint32_t gwho; double gt0, guntil; char gwhat[40]; } BrainHand;
#define HAND_MAGIC 0x4e48424eu
#define HAND_VER 1
static BrainHand* hand_get(void) {
    static BrainHand* h; static int tried; if (h || tried) return h; tried = 1;
    char nm[32]; snprintf(nm, sizeof nm, "/nhbrain-%d", (int)getpid()); int fd = shm_open(nm, O_RDWR | O_CREAT, 0600); if (fd < 0) return 0;
    struct stat st; if (!fstat(fd, &st) && st.st_size < 4096 && ftruncate(fd, 4096)) { close(fd); return 0; }
    void* p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd); if (p == MAP_FAILED) return 0; h = (BrainHand*)p; return h; }
static void hand_publish(void) {   // (the running brain, every step)
    BrainHand* h = hand_get(); if (!h) return; uint32_t q = h->seq; if (q & 1) q++;
    __atomic_store_n(&h->seq, q + 1, __ATOMIC_RELEASE);
    h->magic = HAND_MAGIC; h->ver = HAND_VER; h->target = g_eg_cur; h->target_t0 = g_eg_cur_t0;
    h->gk = g_gl.kind; memcpy(h->gpos, g_gl.pos, 12); h->gwho = g_gl.who; h->gt0 = g_gl.t0; h->guntil = g_gl.until; memcpy(h->gwhat, g_gl.what, sizeof h->gwhat);
    __atomic_store_n(&h->seq, q + 2, __ATOMIC_RELEASE); }
static void hand_adopt(void) {   // (a warming brain, every warm step)
    BrainHand* h = hand_get(); if (!h) return; BrainHand c;
    for (int tries = 0; tries < 4; tries++) { uint32_t a = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE); if (a & 1) continue;
        memcpy(&c, h, sizeof c); if (__atomic_load_n(&h->seq, __ATOMIC_ACQUIRE) != a) continue;
        if (c.magic != HAND_MAGIC || c.ver != HAND_VER || !a) return;
        g_eg_adopt = c.target; g_eg_adopt_t0 = c.target_t0; g_eg_adopt_on = 1;
        g_gl.kind = c.gk; memcpy(g_gl.pos, c.gpos, 12); g_gl.who = c.gwho; g_gl.t0 = c.gt0; g_gl.until = c.guntil; memcpy(g_gl.what, c.gwhat, sizeof g_gl.what); g_gl.what[sizeof g_gl.what - 1] = 0;
        return; } }

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
// (11:33: with the lessons' "_wrank" -- the players' K/D x kill rate per weapon -- the rank is its place there; weapons not in it rank
//  after, by the fixed list. NHOOK_BOT_WRANK_DATA=0 (live) = the fixed list only)
static int bot_wpn_rank_fixed(const char* n0);
static int bot_wpn_rank(const char* n0) {
    float on = 1; lk_over(&on, "NHOOK_BOT_WRANK_DATA");
    if (n0 && g_nwr > 0 && on > 0) { char nb[96]; const char* n = bot_tagkey(n0, nb, sizeof nb); for (int i = 0; i < g_nwr; i++) if (strstr(n, g_wr[i])) return i;
        return g_nwr + bot_wpn_rank_fixed(n0); }
    return bot_wpn_rank_fixed(n0); }
static int bot_wpn_rank_fixed(const char* n0) { static const char* const pref[] = {"rocket_launcher", "spartan_laser", "sniper_rifle", "beam_rifle", "battle_rifle", "covenant_carbine",
    "shotgun", "excavator", "flak_cannon", "energy_sword", "gravity_hammer", "brute_shot", "assault_rifle", "spike_rifle", "smg", "plasma_rifle", "needler", "magnum", "plasma_pistol", 0};   /* (10:35: the rest ranked -- unknown = 50 made a Spiker / SMG "worse than anything") */   /* (08:15: excavator = the Mauler, flak_cannon = the Fuel Rod: Halo 3's tag names) */ if (!n0) return 99; char nb[96]; const char* n = bot_tagkey(n0, nb, sizeof nb); for (int i = 0; pref[i]; i++) if (strstr(n, pref[i])) return i; return 50; }
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
static int g_bot_outranged;   // (11:50: the target out-ranges the held weapon from beyond its band: the fight bids low)
static double g_kite_t;       // (13:00: the last step that kited a sword / hammer: no melee into it)
static double g_bot_click_until;   // (11:40: the trigger pulled on an empty magazine -- the game reloads by itself, the "click")
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
// THE WEAPON-SWITCH BUTTON (2026-11-09 10:50): the packed packet's +0x2c bit 1 came with the user's switches but, sent by the walker,
// rarely switched ("EMPTY BRUTE SHOT: switching" again and again) -- like fire, whose packet bit alone does nothing, the switch is
// probably a CONTROL RECORD button (+0xC8, the word the walker's fire bit 19 goes into) and the packet bit its consequence. Out of a
// fight, with two weapons, the brain presses one candidate record bit for 0.1 s (never jump 1, fire 19 / 21, grenade 30) and looks
// 0.7 s later whether the weapon slot (+0x262) changed; a bit that switches twice is THE switch, saved to native/switch-bit.txt and
// read back by every later brain. [swscan] lines. NHOOK_BOT_SWSCAN=0 (live) = off.
static int g_swbit = -1; static uint64_t g_swscan_bits; static double g_swrec_until; static int g_eng_close; static int g_swscan_busy;
// (cover, 2026-11-09: the fight target's handle and where it is -- the attacker the cover is tested against)
static uint32_t g_ftg_h; static float g_ftg_c[3], g_ftg_r;
static int g_naim_me;   // (enemies within 40 units aiming at the walker, last step: cover when outnumbered, the lull reload)
// COVER SEARCH (beh_cover, nbrain_beh.c): candidate spots tested from the attacker's eyes on the line-of-sight channel -- bot_cov_ray
// takes 2 of every 3 ray slots while a search runs; each answer is filed under the tag 0xC0E00000 | generation << 4 | k
static float g_cov_c[8][3]; static int g_cov_n, g_cov_k, g_cov_gen; static uint32_t g_cov_from; static int g_cov_slot;
static uint32_t cov_tag(int k) { return 0xC0E00000u | ((uint32_t)(g_cov_gen & 0xFF) << 4) | (uint32_t)k; }
static float g_ftg_v = 2, g_ftg_d = 99; static int g_ftg_vis;   // (14:20: the fight's target -- its shield + health, distance, in sight: the break-off rule)   // (busy: a candidate pressed / watched -- no other switch then)
static const char* swbit_path(void) { static char p[1024]; if (!p[0]) { const char* h = getenv("HOME"); snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/native/switch-bit.txt", h ? h : ""); } return p; }
static void swscan_tick(double now, const NovObj* o, int engaged) {
    g_swscan_bits = 0;
    { static int rd0; if (!rd0) { rd0 = 1; FILE* f = fopen(swbit_path(), "r"); if (f) { int b = -1; if (fscanf(f, "%d", &b) == 1 && b >= 0 && b < 64) { g_swbit = b; nlog("[nhook] [swscan] the switch is control-record bit %d (learned before)", b); } fclose(f); } } }
    if (g_swbit >= 0) return; float on = 1; lk_over(&on, "NHOOK_BOT_SWSCAN"); if (on <= 0) return;
    static int cand = -1, hits[64], tries[64], slot0, loaded; static double tpress, tnext;
    // (11:30: single "hits" were noise -- the slot also changes from late packet pulses and pick-ups: the counts now persist across brains
    //  in native/switch-scan.txt and a bit is adopted at 3+ hits with a 75%+ hit rate; the real button switches every time)
    char sp[1100]; snprintf(sp, sizeof sp, "%s.scan", swbit_path());
    if (!loaded) { loaded = 1; FILE* f = fopen(sp, "r"); if (f) { int b, t, h; while (fscanf(f, "%d %d %d", &b, &t, &h) == 3) if (b >= 0 && b < 64) { tries[b] = t; hits[b] = h; } fclose(f); }
        int best = -1; for (int b = 0; b < 64; b++) if (hits[b] >= 3 && hits[b] * 100 >= tries[b] * 55 && (best < 0 || hits[b] * tries[best] > hits[best] * tries[b])) best = b;
        if (best >= 0) { g_swbit = best; FILE* g = fopen(swbit_path(), "w"); if (g) { fprintf(g, "%d\n", best); fclose(g); } nlog("[nhook] [swscan] LEARNED (from the saved counts): the weapon switch is control-record bit %d (%d of %d)", best, hits[best], tries[best]); return; } }
    g_swscan_busy = tpress > 0;
    if (tpress > 0) {
        if (now - tpress < 0.1) { g_swscan_bits = 1ull << cand; return; }
        if (now - tpress < 0.8) return;
        tries[cand]++; int changed = o->wslot != slot0; if (changed) { hits[cand]++; nlog("[nhook] [swscan] bit %d changed the weapon slot (%d -> %d): %d of %d", cand, slot0, o->wslot, hits[cand], tries[cand]); }
        tpress = 0; tnext = now + 0.3;
        if (!g_brain_warm) { FILE* f = fopen(sp, "w"); if (f) { for (int b = 0; b < 64; b++) if (tries[b]) fprintf(f, "%d %d %d\n", b, tries[b], hits[b]); fclose(f); } }
        // (11:50: 75% -> 55% -- a press can fall in a reload / firing animation; the in-use rate is logged)
        if (hits[cand] >= 3 && hits[cand] * 100 >= tries[cand] * 55) { g_swbit = cand; FILE* f = fopen(swbit_path(), "w"); if (f) { fprintf(f, "%d\n", cand); fclose(f); } nlog("[nhook] [swscan] LEARNED: the weapon switch is control-record bit %d", cand); return; }
        if (!changed || tries[cand] >= 6) cand = -1; }   // (a hit: the same bit again, to confirm -- up to 6 tries in a row)
    if (engaged || !g_inv.has_other || now < tnext || o->seat >= 0 || o->parent != 0xFFFFFFFFu) return;
    if (cand < 0) { static int next = 0; for (int k = 0; k < 64; k++) { int b = (next + k) % 64; if (b == 1 || b == 19 || b == 21 || b == 30) continue;
            if (tries[b] >= 3 && hits[b] * 2 < tries[b]) continue;   // (3+ tries switching under half the time: not the button)
            cand = b; next = b + 1; break; } }
    if (cand < 0) return;
    if (now - g_bot_switch_until < 1.0 || now - g_swrec_until < 1.0) return;   // (a switch just went out: wait until it has settled)
    slot0 = o->wslot; tpress = now; g_swscan_busy = 1; g_swscan_bits = 1ull << cand; }
static void bot_switch_press(double now) { if (g_swscan_busy) return; if (g_swbit >= 0) g_swrec_until = now + 0.1; else { float swp = 0.03f; lk_over(&swp, "NHOOK_BOT_SWPULSE"); g_bot_switch_until = now + swp; } }   // (the learnt record bit, else the packet pulse)
static int bot_ammo_of(uint64_t wd, uint32_t def, int* maxv) {   // rounds loaded (-1 unknown), and the most seen for that weapon definition
    static struct { uint32_t def; int max, nv; int16_t v[3]; } mx[64]; static int nmx; int off = bot_ammo_off(); int16_t a = -1;
    if (g_ammo_learned >= 0) { if (!wd || !hb_read(wd + g_ammo_learned, &a, 2) || a < 0) { if (maxv) *maxv = 0; return -1; }   // (learned: ammo_learn above)
        int k = -1; for (int i = 0; i < nmx; i++) if (mx[i].def == def) { k = i; break; }
        if (k < 0) { k = nmx < 64 ? nmx++ : 63; mx[k].def = def; mx[k].max = 0; mx[k].nv = 0; } if (a > mx[k].max) mx[k].max = a;
        if (mx[k].max <= 0) { if (maxv) *maxv = 0; return -1; }   // (13:15: never above 0 for this weapon = no magazine there -- the plasma rifle's battery: "DRY: PLASMA RIFLE" was false)
        if (maxv) *maxv = mx[k].max; return a; }
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
        if (q == o->wslot) { v->held = nm; v->held_c = eg_wcls(nm); v->held_rank = bot_wpn_rank(nm); v->ammo = bot_ammo_of(wd, df, &v->ammo_max);
            v->reserve = -1; if (g_ammo_learned >= 0) { int16_t rv = -1; if (hb_read(wd + g_ammo_learned - 2, &rv, 2) && rv >= 0 && rv < 2000) v->reserve = rv; } }
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
    static const char* const k[] = {"HAMMER", "SWORD", "ENERGY BLADE", "SNIPER", "BEAM RIFLE", "MAGNUM", "ROCKET", "BRUTE SHOT", "FUEL ROD", "PLASMA PISTOL", 0};   /* (14:05: the plasma pistol held = an overcharge held, and no switch away from it: 0 of 32) */
    if (!w) return 0; for (int i = 0; k[i]; i++) if (strstr(w, k[i])) return 1; return 0; }
static int g_fg_n, g_fg_hid, g_fg_react, g_fg_aim, g_fg_camo, g_fg_head, g_fg_ok, g_fg_trig, g_fg_vis;   // ([firegate] counters, 07:39)
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
enum { ARB_HUNT, ARB_FIGHT, ARB_FOLLOW, ARB_FETCH, ARB_BALL, ARB_BOARD, ARB_HOLD, ARB_RECHARGE, ARB_N };
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
typedef struct { uint32_t tg; float td; int tvis; int naim; uint32_t ah[4]; float ad[4]; const char* aw[4]; float sh, hp; const char* w; int am; } DeathSnap;
static void* g_death_snap;
// WHY DID THEY BEAT ME (2026-11-09 11:12, the user: "walker needs to focus hard on its K/D spread and tweak settings until it gets
// better, I feel like a per death why did the other player beat me calculation could be interesting, weighting future growth"): every
// step the duels of this life -- per enemy: when it came into sight, when it started aiming at the walker, when the walker started
// firing at it (and its shield + health then), its shield + health when last seen -- and the walker's own damage episode (the first
// hit after 3 s without one, the shield before it). At a death the likely killer (the target if it aimed at the walker, else the
// nearest enemy aiming at it) and ONE cause, in this order:
//   empty        the magazine empty / dry
//   outnumbered  2+ enemies aiming at it
//   unseen       the killer hurt it before it was ever in sight (flanked / from behind)
//   point blank  under 2.5 units against a shotgun / sword / hammer
//   traded       the killer left with under 0.25 shield + health: nearly won
//   entered hurt the shield under 0.5 when the fatal damage began
//   slow start   it fired back 0.5+ s after the damage began / the killer came into sight, or never
//   outranged    its weapon well outside its range band at that distance, the killer's inside its own
//   outshot      an even start, lost the exchange
// -> "[why]" line + a JSON line in battles/follow/why-<nickname>.jsonl (shim/tools/death-tuner.py turns the causes into setting steps)
typedef struct { uint32_t h; double vis_t, vis_last, aim_t, aim_last, fire_t, fire_last, vl_t; float vit0, vl; } DuelE;
static DuelE g_du[12]; static int g_ndu; static double g_dmg0_t, g_dmg_last_t; static float g_dmg_sh0 = 1, g_du_vprev = -1;
static DuelE* du_get(uint32_t h, int add) {
    for (int i = 0; i < g_ndu; i++) if (g_du[i].h == h) return &g_du[i];
    if (!add) return 0; int k = g_ndu < 12 ? g_ndu++ : 0;
    if (g_ndu == 12) { double old = 1e30; for (int i = 0; i < 12; i++) { double t = g_du[i].vis_last > g_du[i].aim_last ? g_du[i].vis_last : g_du[i].aim_last; if (t < old) { old = t; k = i; } } }
    memset(&g_du[k], 0, sizeof g_du[k]); g_du[k].h = h; g_du[k].vit0 = -1; return &g_du[k]; }
// (11:30, the lessons "Jumping under fire" / "Jumping while attacking" / "Strafe ... ": the role in the duel with h -- 1 attacking (it
//  fired at h before h aimed at it), -1 defending (h aimed at it first), 0 not known)
static int du_role(uint32_t h) { const DuelE* e = h ? du_get(h, 0) : 0; if (!e) return 0;
    if (e->fire_t > 0 && (e->aim_t <= 0 || e->fire_t <= e->aim_t + 0.1)) return 1;
    if (e->aim_t > 0 && (e->fire_t <= 0 || e->aim_t < e->fire_t - 0.1)) return -1;
    return 0; }
static void du_tick(double now, const NovObj* objs, int n, int me, uint32_t tgt, int trig) {
    // (called only while alive: why_note clears the table at a death)
    if (me < 0) return; const NovObj* o = &objs[me]; static float sprev = 1; if (o->dead) return;
    float v = o->shield + o->health;
    if (g_du_vprev >= 0 && v < g_du_vprev - 0.01f) { if (now - g_dmg_last_t > 3.0) { g_dmg0_t = now; g_dmg_sh0 = sprev; } g_dmg_last_t = now; }
    g_du_vprev = v; sprev = o->shield;
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
        float dx = o->c[0] - t->c[0], dy = o->c[1] - t->c[1], d = sqrtf(dx * dx + dy * dy); if (d > 60 || d < 0.2f) continue;
        int vis = los_get(t->handle, now) == 1; float ay = atan2f(t->aim[1], t->aim[0]); int aiming = d < 40 && (cosf(ay) * dx + sinf(ay) * dy) / d > 0.978f;
        if (!vis && !aiming) continue; DuelE* e = du_get(t->handle, 1);
        if (vis) { if (now - e->vis_last > 3.0) e->vis_t = now; e->vis_last = now; e->vl = t->shield + t->health; e->vl_t = now; }
        if (aiming) { if (now - e->aim_last > 3.0) e->aim_t = now; e->aim_last = now; } }
    if (trig && tgt) { DuelE* e = du_get(tgt, 1); if (now - e->fire_last > 3.0) { e->fire_t = now; e->vit0 = now - e->vl_t < 0.5 ? e->vl : -1; } e->fire_last = now; } }
static void why_note(double now, const DeathSnap* d) {
    uint32_t k = 0; const char* kw = "?"; float kd = 0;
    for (int i = 0; i < d->naim; i++) if (d->ah[i] == d->tg) { k = d->tg; kw = d->aw[i]; kd = d->ad[i]; }
    if (!k && d->naim) { k = d->ah[0]; kw = d->aw[0]; kd = d->ad[0]; for (int i = 1; i < d->naim; i++) if (d->ad[i] < kd) { k = d->ah[i]; kw = d->aw[i]; kd = d->ad[i]; } }
    if (!k && d->tg) { k = d->tg; kd = d->td; }
    const DuelE* e = k ? du_get(k, 0) : 0; double d0 = g_dmg0_t > 0 ? g_dmg0_t : now;
    int seen_first = e && e->vis_t > 0 && e->vis_t <= d0 + 0.3;
    double from = e && e->vis_t > d0 ? e->vis_t : d0; float gap = e && e->fire_t >= from - 3.0 && e->fire_last >= from ? (float)(e->fire_t > from ? e->fire_t - from : 0) : -1;
    float kl = e && now - e->vl_t < 1.5 ? e->vl : -1, dmg = e && e->vit0 >= 0 && kl >= 0 ? e->vit0 - kl : -1;
    const char* mc = g_inv.held_c ? g_inv.held_c : "?"; float a1 = 0, b1 = 0, a2 = 0, b2 = 0; int mb = eg_band(mc, &a1, &b1), kb = eg_band(kw, &a2, &b2);
    const char* band = !mb ? "?" : kd < a1 * 0.7f ? "too close" : kd > b1 * 1.3f ? "too far" : "in range"; int kin = kb && kd >= a2 * 0.85f && kd <= b2 * 1.15f;
    const char* cause; char det[200];
    if (d->am == 0 || g_bot_dry) { cause = "empty"; snprintf(det, sizeof det, "%s with %d rounds", d->w ? d->w : "-", d->am); }
    else if (d->naim >= 2) { cause = "outnumbered"; snprintf(det, sizeof det, "%d enemies aiming at me", d->naim); }
    else if (k && !seen_first && d->tg != k) { cause = "unseen"; snprintf(det, sizeof det, "it hurt me before I ever saw it (my target was %08x)", d->tg); }
    else if (kd < 2.5f && (!strcmp(kw, "shotgun") || !strcmp(kw, "melee"))) { cause = "point blank"; snprintf(det, sizeof det, "%s at %.1f units", kw, kd); }
    else if (kl >= 0 && kl < 0.25f) { cause = "traded"; snprintf(det, sizeof det, "it was left with %.2f shield + health", kl); }
    else if (!g_noshield && g_dmg_sh0 < 0.5f) { cause = "entered hurt"; snprintf(det, sizeof det, "my shield was %.2f when the fatal damage began", g_dmg_sh0); }
    else if (k && k != d->tg && gap < 0 && seen_first) { cause = "ignored attacker"; snprintf(det, sizeof det, "it was in sight and shooting me; I stayed on %08x", d->tg); }
    else if (gap < 0 || gap > 0.5f) { cause = "slow start"; if (gap < 0) snprintf(det, sizeof det, "I never fired at it"); else snprintf(det, sizeof det, "I fired back %.2f s after it hurt me / came into sight", gap); }
    else if (strcmp(band, "in range") && strcmp(band, "?") && kin) { cause = "outranged"; snprintf(det, sizeof det, "my %s %s at %.0f units, its %s in range", mc, band, kd, kw); }
    else { cause = "outshot"; snprintf(det, sizeof det, "even start (fired back in %.2f s), it kept %.2f, I took %.2f off it", gap, kl, dmg); }
    if (bot_log()) nlog("[nhook] [why] died to %08x (%s, %.0f u): %s -- %s", k, kw, kd, cause, det);
    char path[1100]; const char* h = getenv("HOME"); snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/battles/follow/why-%s.jsonl", h ? h : "/tmp", g_nick[0] ? g_nick : "walker");
    if (g_brain_warm) return;
    FILE* f = fopen(path, "a"); if (!f) return;
    fprintf(f, "{\"t\": %.0f, \"map\": \"%s\", \"killer\": \"%08x\", \"cause\": \"%s\", \"detail\": \"%s\", \"dist\": %.1f, \"naim\": %d, \"my_w\": \"%s\", \"killer_w\": \"%s\", \"band\": \"%s\", \"sh0\": %.2f, \"dur\": %.1f, \"gap\": %.2f, \"k_left\": %.2f, \"dmg\": %.2f, \"ammo\": %d, \"seen_first\": %d}\n",
            (double)time(0), g_st_curmap, k, cause, det, kd, d->naim, mc, kw, band, g_dmg_sh0, now - d0, gap, kl, dmg, d->am, seen_first);
    fclose(f); }
static void why_reset(void) { g_ndu = 0; g_dmg0_t = 0; g_dmg_last_t = 0; g_du_vprev = -1; g_dmg_sh0 = 1; }
static void death_note(double now) {
    const DeathSnap* d = (const DeathSnap*)g_death_snap; static int nd, nother, nnoaim, nfar; static float dsum; why_note(now, d); why_reset();
    int other = d->naim > 0; for (int k = 0; k < d->naim; k++) if (d->ah[k] == d->tg) other = 0;
    char b[300]; int q = 0; for (int k = 0; k < d->naim; k++) q += snprintf(b + q, sizeof b - (size_t)q, " %08x %.0f u (%s)%s", d->ah[k], d->ad[k], d->aw[k], d->ah[k] == d->tg ? " = my target" : "");
    if (bot_log()) nlog("[nhook] [death] fighting %08x at %.0f u (%s); aiming at me:%s; my shield %.2f health %.2f, %s %d rounds", d->tg, d->td, d->tg ? (d->tvis ? "in sight" : "hidden") : "no target", d->naim ? b : " nobody seen", d->sh, d->hp, d->w ? d->w : "-", d->am);
    nd++; if (other) nother++; if (!d->naim) nnoaim++; if (d->naim && d->ad[0] > 20) nfar++; if (d->naim) dsum += d->ad[0];
    if (nd % 8 == 0 && bot_log()) nlog("[nhook] [deaths] %d deaths: the likely killer was NOT my target %d times, nobody aiming at me seen %d, killer 20+ units away %d; mean killer distance %.0f u",
                                     nd, nother, nnoaim, nfar, nd - nnoaim > 0 ? dsum / (nd - nnoaim) : 0.0f); }
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
static float g_bot_head_c[3];      // (... and its centre: the fire gate's hit test -- where it WILL be, the lead below)
static float g_bot_body_c[3];      // (the target's centre where it will be: the hit test's body column)
// the distance from the line cam + s * dv (s > 0) to the point P; behind the camera = far
static float ray_pd(const float* cam, const float* dv, const float* P) {
    float wx = P[0] - cam[0], wy = P[1] - cam[1], wz = P[2] - cam[2], s = wx * dv[0] + wy * dv[1] + wz * dv[2];
    return s <= 0 ? 1e9f : sqrtf(fmaxf(0.0f, wx * wx + wy * wy + wz * wz - s * s)); }
#include "nbrain_beh.c"   // the behaviours: each bids for the legs (brain stage 2)
#include "npolicy.c"     // the learnt movement policy (policy-train.py on the GPU -> policy.json)
static void bot_models_tick(void) {   // (live knobs: the models re-read when their files change -- every 10 s a look at the files' times)
    static double t; double now = mono_s(); if (now - t < 10.0) return; t = now; const char* h = getenv("HOME"); char p[1100]; struct stat st;
    static time_t m1, m2, m3, m4, m5; static float mt_last = -1; float mt = 1; lk_over(&mt, "NHOOK_BOT_MODEL_TOP");
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/walker-model.json", h ? h : "");
    if (!getenv("NHOOK_BOT_MODEL") && stat(p, &st) == 0) { if (m1 && st.st_mtime != m1) { g_bm_done = 0; bm_load(); nlog("[nhook] [knobs] walker-model.json changed: re-read"); } m1 = st.st_mtime; }
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/walker-model-top.json", h ? h : "");
    if (!getenv("NHOOK_BOT_MODEL") && stat(p, &st) == 0) { if ((m5 && st.st_mtime != m5) || (mt_last >= 0 && (mt > 0) != (mt_last > 0))) { g_bm_done = 0; bm_load(); nlog("[nhook] [knobs] walker-model-top.json changed / switched: re-read"); } m5 = st.st_mtime; }
    mt_last = mt;
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/engage-model.json", h ? h : "");
    if (stat(p, &st) == 0) { if (m2 && st.st_mtime != m2) { g_eg_key[0] = 0; nlog("[nhook] [knobs] engage-model.json changed: re-read at the next step"); } m2 = st.st_mtime; }
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/maps", h ? h : "");
    if (stat(p, &st) == 0) { if (m3 && st.st_mtime != m3) { g_eg_key[0] = 0; nlog("[nhook] [knobs] the per-map models changed: re-read at the next step"); } m3 = st.st_mtime; }
    snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/tracks/melee.json", h ? h : "");
    if (stat(p, &st) == 0) { if (m4 && st.st_mtime != m4) { g_mel.loaded = 0; mel_load(); nlog("[nhook] [knobs] melee.json changed: re-read"); } m4 = st.st_mtime; } }
static void bot_step_body(void) {
    double now = mono_s(); int spar = g_bot_spar, mode = bot_walk_ok() ? nav_mode() : 0;
    lk_tick(); ls_tick(); bot_models_tick();   // (live knobs, the lessons and models: no restart)
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
    brain_review(now);   // (nbot_goals.c: every 3 minutes, its own deaths / kills -> caution, the paths around its death spots)
    bm_lanes_apply();   // (lanes reread by a revision: mixed into the planner here, on the walker's own thread)
    { static char nsm[64]; static double tfirst, tsh;
      if (strcmp(nsm, g_st_curmap)) { snprintf(nsm, sizeof nsm, "%s", g_st_curmap); tfirst = 0; tsh = 0; }
      int any = 0; for (int i = 0; i < n; i++) if (objs[i].type == 0 && !objs[i].dead) { any++; if (objs[i].shield > 0.05f) tsh = now; }
      if (any && tfirst <= 0) tfirst = now;
      // (16:00, the user: "the brain will need to adapt quickly in context cuz this is a fast paced game type": 4+ live players and
      //  none with a shield for 5 s is enough -- every player spawns with a full one in a game that has them)
      int ns = tfirst > 0 && now - (tsh > tfirst ? tsh : tfirst) > (any >= 4 ? 5.0 : 20.0);
      float fo = -1; lk_over(&fo, "NHOOK_BOT_NOSHIELD"); if (fo >= 0) ns = fo > 0.5f;
      if (ns != g_noshield && bot_log()) nlog("[nhook] [bot] %s", ns ? "no shields in this game (SWAT: a headshot kills): the shield rules stand down; the SWAT profile is on -- reacting x0.5, turning x1.5 / x1.3, aim noise x0.6, autos aim at the head too, the trigger only when the aim line goes through the HEAD, pressing x0.5" : "shields are back: the shield rules are on again, the SWAT profile off");
      g_noshield = ns; }
    { float io = 1; lk_over(&io, "NHOOK_INF"); int im = (io > 0 && g_nov_sides_on && me >= 0) ? nov_side_of(objs[me].handle) : -1;
      static int said; if (bot_log() && (im != g_inf_me || !said)) { said = 1; nlog("[nhook] [inf] %s", im == 1 ? "I'm a ZOMBIE now: hunting the nearest human, the lonely ones first, charging straight in -- no cover, no backing off" :
                                                          im == 0 ? "I'm a HUMAN: to the biggest group of humans near me, holding there, kiting swords and hammers" : "not Infection (or not known): the normal fight"); }
      g_inf_me = im; }
    // seated = a seat index OR a parent (02:20: on the turret the walker went on fighting on foot -- "stuck" in one spot -- after a burst:
    //  the seat field alone missed it). The state is logged at each change with every handle it is read from
    int seated = me >= 0 && !objs[me].dead && (objs[me].seat >= 0 || objs[me].parent != 0xFFFFFFFFu); if (seated) g_bot_seat_t = now;
    { static int was = -1; if (me >= 0 && seated != was) {
        if (was == 1 && !seated && g_gun_exit_bit_last >= 0 && now - g_gun_exit_bit_t < 1.4) {   // (out within a try of a record bit: that bit)
            char ep[1024]; snprintf(ep, sizeof ep, "%s/Library/Application Support/ReclaimerMac/battles/exit-bit.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
            FILE* f = g_brain_warm ? 0 : fopen(ep, "w"); if (f) { fprintf(f, "%d\n", g_gun_exit_bit_last); fclose(f); }
            nlog("[nhook] [gunner] OUT of the driver's seat right after control-record bit %d: kept as the exit (%s)", g_gun_exit_bit_last, ep); }
        was = seated; gun_vlist(now); const NovObj* pv = gun_v(objs[me].parent);
        nlog("[nhook] [gunner] seated: %s (my unit %08x, the record's %08x, the player's %08x; seat %d, parent %08x %s)", seated ? "yes" : "no", objs[me].handle, meh, g_my_unit,
             objs[me].seat, objs[me].parent, pv ? hb_tag_name(pv->def) : "-"); } }
    honk_scan(now, objs, n, me);   // (02:35: finding the field a teammate's honk changes, above)
    nade_check(now, objs, n);   // (the last grenade's outcome, above)
    if (seated && gun_on() && !g_bot_shadow && gun_seated(now, objs, n, me, cam, yaw0, pitch0)) { plen = 0; g_bot_err = -1; return; }   // (the gunner, above)
    { static int alive_was = 1; int alive_now = me >= 0 && !objs[me].dead; if (alive_was && !alive_now && me >= 0 && g_death_snap) death_note(now); alive_was = me >= 0 ? alive_now : alive_was; }
    if (me < 0 || objs[me].dead || seated) { g_bot_out = 0; g_nav_act = 0; plen = 0; g_bot_err = -1;   // dead, respawning or in a vehicle
        static char w[96]; snprintf(w, sizeof w, me < 0 ? "the record's unit %08x is none of the %d units seen" : objs[me].dead ? "dead" : "in a vehicle", meh, n); bot_idle(me < 0 ? w : objs[me].dead ? "dead" : "in a vehicle"); return; }
    const NovObj* o = &objs[me]; int out = 0; float yaw = yaw0, pitch = pitch0; g_bot_unit_p = o->p;
    { static int noinv = -1; if (noinv < 0) noinv = getenv("NHOOK_BOT_NOINV") != 0; if (!noinv) { ammo_learn(now, o); bot_inv(o, &g_inv); } else memset(&g_inv, 0, sizeof g_inv);
      swscan_tick(now, o, g_eng_close);   // (the switch-bit scan: no enemy in sight within 20 units -- g_eng_close, set by the fight section)   // (NHOOK_BOT_NOINV=1: blind to it, the old walker)
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
        // (14:15: FINISH THE WEAK -- 1 kill / 9 deaths / 5 ASSISTS in a 14-player Pit: its damage went to kills other players took. An
        //  enemy in sight within NHOOK_BOT_FINISH_R (20) units with shield + health under NHOOK_BOT_FINISH_V (0.5) becomes the target --
        //  the weakest one; the one at arm's length below still comes first)
        if (g_bot_fighting && bi >= 0) { float fr = 20, fv = 0.5f; lk_over(&fr, "NHOOK_BOT_FINISH_R"); lk_over(&fv, "NHOOK_BOT_FINISH_V");
            int wi = -1; float wv = fv; for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
                float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; float v = t->shield + t->health;
                if (d < fr * fr && v < wv && los_get(t->handle, now) == 1) { wv = v; wi = i; } }
            if (wi >= 0 && wi != bi) { const NovObj* wt = &objs[wi]; float dx = wt->c[0] - cam[0], dy = wt->c[1] - cam[1], dz = wt->c[2] - cam[2];
                static double tl; if (bot_log() && now - tl > 2) { tl = now; nlog("[nhook] [fight] %08x is nearly dead (%.2f): finishing it", wt->handle, wv); }
                bi = wi; bd = dx * dx + dy * dy + dz * dz; } }
        // (12:55: THE ONE AT ARM'S LENGTH -- "[death] fighting 80d30114 at 9 u ... aiming at me: 80e60071 1 u": an enemy within
        //  NHOOK_BOT_NEAR_THREAT (3) units, and closer than half the chosen target, becomes the target at once, whatever the choice rule)
        if (g_bot_fighting && bi >= 0) { float nt = 3; lk_over(&nt, "NHOOK_BOT_NEAR_THREAT"); int ni = -1; float nd = nt * nt;
            for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
                float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; if (d < nd && fabsf(dz) < 2.0f) { nd = d; ni = i; } }
            if (ni >= 0 && ni != bi && nd * 4 < bd) { static double tl; if (bot_log() && now - tl > 2) { tl = now; nlog("[nhook] [fight] %08x at %.1f units: the one at arm's length first (was %08x at %.1f)", objs[ni].handle, sqrtf(nd), objs[bi].handle, sqrtf(bd)); }
                bi = ni; bd = nd; } }
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
            // (12:40: the HEAD only with a precision weapon or a sniper -- Halo 3's assault rifle, SMG, plasma rifle and co. do no extra
            //  damage there and the head is the smallest part: they aim at the chest, NHOOK_BOT_AIM_BODY (0.2) x the radius over the centre)
            int prec = g_inv.held_c && (!strcmp(g_inv.held_c, "precision") || !strcmp(g_inv.held_c, "sniper"));
            // (2026-11-09 11:00, the user: "you should not be firing at a static height, rather attempting to track heads with the aiming
            //  reticule just like any real human would"): the HEAD HITBOX (bot_head: the skeleton's head node, crouching / leaning /
            //  jumping) with every weapon -- NHOOK_BOT_HEAD_ALL (live, 1); 0 = the old chest height for the automatic weapons
            float hall = 1; lk_over(&hall, "NHOOK_BOT_HEAD_ALL");
            // THE HEAD'S PATH (11:24, the user: "classic inability to lead a shot, and predict the path of the head"): the shot meets
            //  the head where it will BE -- the head node's own velocity (smoothed from its positions every 0.06 s+: crouching, jumping,
            //  strafing; the body's velocity before there is one) x the lead time: NHOOK_BOT_LEAD_LAT (0.08 s: the host registers a
            //  client's bullets with no lag compensation, and the target as drawn is already behind) + the bullet's flight (distance /
            //  the weapon class's speed x NHOOK_BOT_PROJ_SPEED) + one bot step, all x NHOOK_BOT_LEAD_X (1; death-tuner.py tunes it on
            //  the server's registered accuracy). The fire gate's hit test uses the same predicted head and body
            // (2026-11-09: the latency part follows the joined server's measured round trip: g_net_rtt_ms x NHOOK_BOT_LEAD_RTT (1.25 /
            //  1000 -- 0.08 s at the 64 ms where 0.08 was tuned; 0.18 s at 145 ms, where the fixed 0.08 left the walker at 5-12% accuracy
            //  against the lobby's 17-23%); a NHOOK_BOT_LEAD_LAT in the live file still overrides it)
            float tl = 0; { const char* hc2 = g_inv.held_c ? g_inv.held_c : ""; float lat = 0.08f, lx = 1, psx = 1, lrt = 1.25f; lk_over(&lrt, "NHOOK_BOT_LEAD_RTT");
                if (g_net_rtt_ms > 1.0f && lrt > 0) lat = g_net_rtt_ms / 1000.0f * lrt;
                lk_over(&lat, "NHOOK_BOT_LEAD_LAT"); lk_over(&lx, "NHOOK_BOT_LEAD_X"); lk_over(&psx, "NHOOK_BOT_PROJ_SPEED");
                float spd = (!strcmp(hc2, "sniper") ? 2000.0f : !strcmp(hc2, "precision") ? 400.0f : !strcmp(hc2, "auto") ? 300.0f : !strcmp(hc2, "shotgun") ? 250.0f : !strcmp(hc2, "rocket") ? 40.0f : 300.0f) * (psx > 0.05f ? psx : 0.05f);
                float dd2 = sqrtf((t->c[0] - cam[0]) * (t->c[0] - cam[0]) + (t->c[1] - cam[1]) * (t->c[1] - cam[1]) + (t->c[2] - cam[2]) * (t->c[2] - cam[2]));
                tl = (lat + dd2 / spd + 0.033f) * lx; if (tl < 0) tl = 0; if (tl > 0.8f) tl = 0.8f; }
            static uint32_t hv_h; static float hv_p[3], hv_v[3]; static double hv_t; static int hv_ok;
            g_bot_body_c[0] = t->c[0] + t->vel[0] * tl; g_bot_body_c[1] = t->c[1] + t->vel[1] * tl; g_bot_body_c[2] = t->c[2] + t->vel[2] * tl;
            if (!prec && !(hall > 0)) { float ab = 0.2f; lk_over(&ab, "NHOOK_BOT_AIM_BODY"); ap[2] = t->c[2] + ab * t->r; }
            // (11:20: automatic weapons track the head but aim at the NECK, NHOOK_BOT_HEAD_DZ_AUTO (-1.5 head radii, live): spread / drift
            //  upward still hits the head, downward the chest -- aimed at the head itself every shot drifting up missed everything)
            else if (ha_on > 0 && bot_head(t, now, hp)) { float dz = hdz; if (!prec && !g_noshield) { dz = -1.5f; lk_over(&dz, "NHOOK_BOT_HEAD_DZ_AUTO"); }
                if (t->handle != hv_h || now - hv_t > 0.4) { hv_h = t->handle; memcpy(hv_p, hp, 12); memcpy(hv_v, t->vel, 12); hv_t = now; hv_ok = 0; }
                else if (now - hv_t >= 0.06) { float dtt = (float)(now - hv_t), a = hv_ok ? 0.5f : 1.0f;
                    // (11:55: horizontally the engine's own body velocity -- the head node sways with the animation, and x a 0.15-0.25 s lead
                    //  at 20-30 units that sway shook the aim: on Valhalla the BR's aim line missed the target 29-36% of the steps; the head's
                    //  own velocity only vertically -- crouching, jumping -- smoothed harder)
                    { float v = (hp[2] - hv_p[2]) / dtt; if (v > 12) v = 12; if (v < -12) v = -12; hv_v[2] += (hv_ok ? 0.3f : 1.0f) * (v - hv_v[2]); }
                    hv_v[0] = t->vel[0]; hv_v[1] = t->vel[1]; (void)a;
                    memcpy(hv_p, hp, 12); hv_t = now; hv_ok = 1; }
                float ph[3] = {hp[0] + hv_v[0] * tl, hp[1] + hv_v[1] * tl, hp[2] + hv_v[2] * tl};
                ap[0] = ph[0]; ap[1] = ph[1]; ap[2] = ph[2] + dz * hp[3]; g_bot_head_r = hp[3]; memcpy(g_bot_head_c, ph, 12); }
            if (g_bot_head_r < 0) { ap[0] += t->vel[0] * tl; ap[1] += t->vel[1] * tl; ap[2] += t->vel[2] * tl; }
            // LEAD (10:25): Halo 3 registers a client's bullets on the HOST, no lag compensation -- a third of the walker's shots through
            // a hitbox registered no damage (aim reg 0.67): the aim point leads the target by its velocity (units a second) x
            // NHOOK_BOT_AIMLEAD s (live, 0.06; follow-study.py hones it on registered hits a shot)
            { float lead = 0.0f; lk_over(&lead, "NHOOK_BOT_AIMLEAD"); ap[0] += t->vel[0] * lead; ap[1] += t->vel[1] * lead; ap[2] += t->vel[2] * lead * 0.5f; }
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
    static int fight = -1; static float skill_0 = 0.5f, kdist_0 = 1, kstrafe_0 = 1, kswitch_0 = 1, fextra_0 = 2, retreat_0 = 0.35f, gren_0 = 9, backoff_0 = 0.15f, press_0 = 0.4f;
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
    // (the three-minute review, nbot_goals.c brain_review: a bad window -> less pressing into the target -- at most NHOOK_BOT_CAUTION_PRESS
    //  (0.25, live) less since 2026-11-09: at 0.6 a cautious walker hung back against the user in a 4-player Pit -- "not aggressive")
    { float cp = 0.25f; lk_over(&cp, "NHOOK_BOT_CAUTION_PRESS"); press *= 1.0f - cp * rv_caution(); }
    if (g_noshield) { float sp = 0.5f; lk_over(&sp, "NHOOK_SWAT_PRESS"); press *= sp; }
    int engaged = 0; float ftx = 0, fty = 0; uint32_t fbtn = 0; g_bot_fighting = fight && spar; g_eng_close = 0; g_ftg_vis = 0;   // (set again below when an enemy is in sight within 20 units)
    if (fight) { eg_load_for(g_st_curmap, g_trk_mode); wz_load(g_st_curmap); }   // (the models for this map and game: reloaded when either changes; the weapon zones)
    if (fight && spar && have_tg) {
        static uint32_t cur; static double tacq, tvis, tswitch, tmelee, tgren, tlos, tlast; static float ox, oy, side = 1;
        double dt = tlast ? now - tlast : 0.033; if (dt > 0.2) dt = 0.2; tlast = now;
        if (ftg.handle != cur) { cur = ftg.handle; tacq = now; tvis = 0;
            ox = (bot_rand01() - 0.5f) * 0.15f; oy = (bot_rand01() - 0.5f) * 0.08f;
            if (g_eg_fast && g_eg_fast == cur) { tacq = now - 30; g_eg_fast = 0; } }   // (a second attacker: the decision delay was its reaction -- turn at full speed)
        { static double tput; if (g_los_t > tput && g_los_req == g_los_done) { tput = g_los_t; los_put(g_los_res_tgt, g_los_res, g_los_t); } }   // (the last answer -> the cache)
        // (cover, 11:45: a search's rays get 2 slots of 3 -- the fight's own sight test keeps every third, fresher than its 0.5 s
        //  cache. It sat in the gunner's scheduler until 11:45 and was never asked on foot: 0 of 89 searches had an answer)
        if (now - tlos > 0.07 && g_los_req == g_los_done && g_cov_n > 0 && g_cov_k < g_cov_n && (++g_cov_slot % 3) != 0) {
            const NovObj* ca = 0; for (int i = 0; i < n; i++) if (objs[i].handle == g_cov_from && !objs[i].dead) { ca = &objs[i]; break; }
            if (!ca) g_cov_k = g_cov_n;
            else { int k = g_cov_k++; tlos = now; g_los_p[0] = ca->c[0]; g_los_p[1] = ca->c[1]; g_los_p[2] = ca->c[2] + 0.5f * ca->r;
                g_los_v[0] = g_cov_c[k][0] - g_los_p[0]; g_los_v[1] = g_cov_c[k][1] - g_los_p[1]; g_los_v[2] = g_cov_c[k][2] - g_los_p[2];
                g_los_ign[0] = o->handle; g_los_ign[1] = ca->handle; g_los_tgt = cov_tag(k); g_los_req++; } }
        if (now - tlos > 0.07 && g_los_req == g_los_done) { tlos = now;   // ask for the line to one enemy's chest: the target every other time, else the next of the 3 nearest
            static int rr; const NovObj* q = &ftg; rr++;
            if (rr & 1) { int cand[3] = {-1, -1, -1}; float cd[3] = {1e30f, 1e30f, 1e30f};
                for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t) || t->handle == ftg.handle) continue;
                    float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz;
                    for (int k = 0; k < 3; k++) if (d < cd[k]) { for (int m = 2; m > k; m--) { cd[m] = cd[m - 1]; cand[m] = cand[m - 1]; } cd[k] = d; cand[k] = i; break; } }
                int k = (rr >> 1) % 3; if (cand[k] >= 0 && cd[k] < 1600.0f) q = &objs[cand[k]]; }
            // (12:30: the ray goes to the HEAD -- the walker aims there, and waist-high cover hid a chest-high ray: targets within 3
            //  units read "hidden" 26% of the time, 3-6 units 35%, while they shot the walker dead. Its head hitbox (bot_head), else
            //  0.6 x the radius above the centre)
            float lz = q->c[2] + 0.6f * q->r, lx = q->c[0], ly = q->c[1]; { float hp[4]; if (bot_head(q, now, hp)) { lx = hp[0]; ly = hp[1]; lz = hp[2]; } }
            g_los_p[0] = cam[0]; g_los_p[1] = cam[1]; g_los_p[2] = cam[2]; g_los_v[0] = lx - cam[0]; g_los_v[1] = ly - cam[1]; g_los_v[2] = lz - cam[2];
            g_los_ign[0] = meh; g_los_ign[1] = q->handle; g_los_tgt = q->handle; g_los_req++; }
        int vis = los_get(ftg.handle, now); if (vis < 0) vis = tdist < 6 ? 1 : 0;
        // (12:50: ARM'S LENGTH = SEEN -- within NHOOK_BOT_SEE_NEAR (2.5) units a target is in reach whatever the ray says: a quarter of
        //  the targets within 3 units read "hidden" and the melee waits for "in sight"; 9 close-range deaths against 3 melees)
        { float sn = 2.5f; lk_over(&sn, "NHOOK_BOT_SEE_NEAR"); if (!vis && tdist < sn && fabsf(ftg.c[2] - o->c[2]) < 1.5f) vis = 1; }
        // (12:30: SHOT AT = SEEN -- the target aiming within 12 deg at the walker while the walker was hurt in the last 0.5 s, within 25
        //  units: it can be shot back; "[death] fighting ... at 5 u (hidden); aiming at me: ... = my target")
        { static float vit0 = -1; static double thurt2; float vv = o->shield + o->health; if (vit0 >= 0 && vv < vit0 - 0.01f) thurt2 = now; vit0 = vv;
          if (!vis && tdist < 25 && now - thurt2 < 0.5) { float dx = o->c[0] - ftg.c[0], dy = o->c[1] - ftg.c[1], dd = sqrtf(dx * dx + dy * dy) + 1e-3f, ay = atan2f(ftg.aim[1], ftg.aim[0]);
              if ((cosf(ay) * dx + sinf(ay) * dy) / dd > 0.978f) { vis = 1; static double tl; if (bot_log() && now - tl > 3) { tl = now; nlog("[nhook] [fight] %08x is shooting me at %.1f units: seen (the ray said hidden)", ftg.handle, tdist); } } } }
        if (vis) tvis = now;
        int seen = tvis && now - tvis < 0.6;
        // (2026-11-09 10:50, the user: "it should always be shooting aggressively. crank that dial to max"): NHOOK_BOT_FIRE_LINGER (s, live,
        //  0 = off) keeps the trigger allowed that long after the target went behind cover -- shots at where it is, through a doorway /
        //  over a ledge it just left; the aim already follows a target for 0.6 s after it was last seen ("seen")
        float flinger = 0; lk_over(&flinger, "NHOOK_BOT_FIRE_LINGER"); if (flinger > 0.6f) flinger = 0.6f;
        int fvis = vis || (flinger > 0 && tvis > 0 && now - tvis < flinger);
        // (2026-11-08 walker-compare: walkers reacted in 0.42 s and aimed 2.8 deg off against the players' 1.08 s and 4.1 deg: the
        //  factors below bring skill 0.5 to the players' numbers -- skill 1 is still faster and steadier than they are)
        static float rk0 = -1, t0r0 = -1; if (rk0 < 0) { const char* e = getenv("NHOOK_BOT_REACT"); rk0 = e ? (float)atof(e) : 1.6f; e = getenv("NHOOK_BOT_TURN0"); t0r0 = e ? (float)atof(e) : 0.73f; }
        float rk = rk0, t0r = t0r0; lk_over(&rk, "NHOOK_BOT_REACT"); lk_over(&t0r, "NHOOK_BOT_TURN0");   // (live knobs, above)
        // THE SWAT PROFILE (2026-11-09 16:00, the user: "the swat gametype is headshots only btw... no shields... one shot. so you gonna
        //  have to kick up the accuracy, speed to target, etc. or walker is gonna get destroyed. the brain will need to adapt quickly in
        //  context"): with no shields in the game (g_noshield) the first shot on a head wins -- react NHOOK_SWAT_REACT (0.5) x sooner,
        //  turn to the target NHOOK_SWAT_TURN0 (1.5) x faster before the reaction and NHOOK_SWAT_TURN (1.3) x after, aim noise
        //  NHOOK_SWAT_NOISE (0.6) x, every weapon at the head, the trigger only on the head (NHOOK_SWAT_HEADONLY 1), pressing
        //  NHOOK_SWAT_PRESS (0.5) x (one shot kills: no running into the open). All live; off again the moment a shield shows
        if (g_noshield) { float a = 0.5f, b = 1.5f; lk_over(&a, "NHOOK_SWAT_REACT"); lk_over(&b, "NHOOK_SWAT_TURN0"); rk *= a; t0r *= b; }
        // (07:50: walker-model's "reaction" = 30 deg -> 5 deg of aim, so the turn rate BEFORE the reaction time decides it: 1.5 rad/s
        //  measured 0.49-0.53 s against the players' 1.08 whatever the delay; NHOOK_BOT_TURN0 = that rate, NHOOK_BOT_REACT the delay factor)
        // NHOOK_BOT_NOISE: x the aim noise (2026-11-08 12:15: on Guardian the players' horizontal aim error while attacking is 6.8 deg,
        // the walkers' 3.8 -- lag and moving targets the walkers do not have; engage-compare.py)
        static float nk0 = -1; if (nk0 < 0) { const char* e = getenv("NHOOK_BOT_NOISE"); nk0 = e ? strtof(e, 0) : 1.0f; }
        float nk = nk0; lk_over(&nk, "NHOOK_BOT_NOISE");
        if (g_noshield) { float c = 0.6f; lk_over(&c, "NHOOK_SWAT_NOISE"); nk *= c; }
        float react = g_bm.reaction * (1.25f - skill) * rk, noise = g_bm.aim_err * (1.25f - skill) * 1.6f * nk / 57.2958f;
        // (13:35: steadier with a precision weapon -- battle-rifle duels at 14 units lost on the headshot race; the head is about +-0.3
        //  deg there and the wander +-0.6: x NHOOK_BOT_NOISE_PREC (0.7, live) with a precision weapon / sniper)
        if (g_inv.held_c && (!strcmp(g_inv.held_c, "precision") || !strcmp(g_inv.held_c, "sniper"))) { float np = 0.7f; lk_over(&np, "NHOOK_BOT_NOISE_PREC"); noise *= np; }
        if (seen) {
            // aim: OU noise around the exact angles, turning at most 1.5 rad/s before the reaction time is up, 7 rad/s after
            float th = 0.45f, k = (float)dt / th, sg = noise * sqrtf(2.0f * (float)dt / th);
            ox += -ox * k + sg * bot_gauss(); oy += -oy * k + 0.5f * sg * bot_gauss();
            float turn = 7.0f; lk_over(&turn, "NHOOK_BOT_TURN");   // (07:10: the turn rate once reacting -- "increase look sensitivity as you level up": the trainer raises it)
            if (g_noshield) { float e = 1.3f; lk_over(&e, "NHOOK_SWAT_TURN"); turn *= e; }
            float dy = bot_wrap(tyaw + ox - yaw0), dp = (tpitch + oy) - pitch0, mx = (float)dt * (now - tacq < react ? t0r : turn);
            yaw = yaw0 + (dy > mx ? mx : dy < -mx ? -mx : dy); pitch = pitch0 + (dp > mx ? mx : dp < -mx ? -mx : dp); out |= 1;
            float ey = bot_wrap(tyaw - yaw0), ep = tpitch - pitch0, err = sqrtf(ey * ey + ep * ep) * 57.2958f, size = atanf(0.4f / (tdist > 0.5f ? tdist : 0.5f)) * 57.2958f;
            // (2026-11-09 10:54, the user: "it should ALWAYS be *allowed* to fire IF it can hit an enemy in range of the weapon in its
            //  hand. it should never NOT be allowed to fire"): the trigger no longer waits out the reaction delay -- that delay still
            //  slows the first turn onto a new target (TURN0); NHOOK_BOT_FIRE_REACT=1 (live) = the old wait. The reach is the held
            //  weapon's -- sniper 120, precision / rockets 60, automatic and others 40, shotgun 12, sword / hammer 4 units -- x
            //  NHOOK_BOT_FIRE_RANGE (1, live); it was 40 for every weapon
            float fwr = 0, frm = 1; lk_over(&fwr, "NHOOK_BOT_FIRE_REACT"); lk_over(&frm, "NHOOK_BOT_FIRE_RANGE");
            const char* hcl = g_inv.held_c ? g_inv.held_c : "";
            // (2026-11-09, the user: "did dumb things like shoot at me with a shotgun from REAL far away": the shotgun's reach was 12 units;
            //  a Halo 3 shotgun kills inside ~5. NHOOK_BOT_REACH_SHOTGUN (6, live): beyond it the walker closes in instead of firing)
            float rsg = 6.0f; lk_over(&rsg, "NHOOK_BOT_REACH_SHOTGUN");
            float reach = frm * (!strcmp(hcl, "sniper") ? 120.0f : (!strcmp(hcl, "precision") || !strcmp(hcl, "rocket")) ? 60.0f : !strcmp(hcl, "shotgun") ? rsg : !strcmp(hcl, "melee") ? 4.0f : 40.0f);
            float cone = (size > 1.2f ? size : 1.2f) + fextra;
            // (11:20, the user: "a quick scoreboard glance immediately shows we miss way more shots than the competition" -- 54% of its
            //  shots on target against the best players' 64-84%, misses a median 1.1 deg off): with the head known, CAN HIT = the aim line
            //  passes through the head (its radius x 1.1 + NHOOK_BOT_FIRE_PAD 0.03 units) or the body under it (a column 0.22 units wide
            //  from the head down to 0.7 radii under the centre, + the pad) -- not a cone around the aim point, which let every shot
            //  drifting over the head go. NHOOK_BOT_FIRE_HITTEST=0 (live) = the cone
            int can_hit = err < cone; float fht = 1; lk_over(&fht, "NHOOK_BOT_FIRE_HITTEST");
            // (12:05: on Valhalla, battle rifles at 20-30 units, the aim line was off the predicted target 17-46% of the steps: the body
            //  column is ~0.6 deg wide there, while Halo 3's bullet magnetism pulls shots in from a little off -- the pad grows with the
            //  distance by NHOOK_BOT_FIRE_PAD_DEG (0.5 deg, live))
            if (fht > 0 && g_bot_head_r > 0) { float pad = 0.03f, pdeg = 0.5f; lk_over(&pad, "NHOOK_BOT_FIRE_PAD"); lk_over(&pdeg, "NHOOK_BOT_FIRE_PAD_DEG");
                pad += tanf(pdeg / 57.2958f) * tdist;
                float dv[3] = {cosf(pitch0) * cosf(yaw0), cosf(pitch0) * sinf(yaw0), sinf(pitch0)};
                can_hit = ray_pd(cam, dv, g_bot_head_c) < g_bot_head_r * 1.1f + pad;
                float top = g_bot_head_c[2] - g_bot_head_r, bot = g_bot_body_c[2] - 0.7f * ftg.r;
                float sho = 1; lk_over(&sho, "NHOOK_SWAT_HEADONLY"); int body_ok = !(g_noshield && sho > 0);
                for (int k = 0; k <= 8 && body_ok && !can_hit && top > bot; k++) { float f = (float)k / 8.0f;
                    float P[3] = {g_bot_body_c[0] + (g_bot_head_c[0] - g_bot_body_c[0]) * f, g_bot_body_c[1] + (g_bot_head_c[1] - g_bot_body_c[1]) * f, bot + (top - bot) * f};
                    if (ray_pd(cam, dv, P) < 0.22f + pad) can_hit = 1; } }
            g_bot_err = err; g_bot_fire = fvis && (fwr <= 0 || now - tacq > react) && can_hit && tdist < reach;
            // (07:35, the user: "hitting the critical hitbox is key. the game reticule has a red dot in some cases when it's a head shot,
            //  you should tune to hit them when possible. headshots being main goal"): with a PRECISION weapon (battle rifle, carbine,
            //  magnum, sniper -- headshots kill faster) the trigger waits for the aim to sit on the HEAD: within its angular radius
            //  (0.15 units) x NHOOK_BOT_HEADGATE (1.0) + 0.3 deg. The automatic weapons keep the body gate (a headshot adds nothing there)
            // (11:00: the gate only for the KILL shot -- shields still up, a body hit strips them as well as a head hit, and the gate at
            //  20+ units (about the aim's own spread) halved the battle rifle's rate: it lost the precision duels -- 5 of 12 deaths to
            //  battle-rifle headshots. Shields down (< NHOOK_BOT_HEADGATE_SH 0.1): the head gate. The aim point stays the head throughout)
            float hgs = 0.1f; lk_over(&hgs, "NHOOK_BOT_HEADGATE_SH");
            if (g_bot_fire && ftg.shield < hgs && g_inv.held_c && (!strcmp(g_inv.held_c, "precision") || !strcmp(g_inv.held_c, "sniper"))) {
                float hg = 1.0f; lk_over(&hg, "NHOOK_BOT_HEADGATE"); float hs = atanf(0.15f / (tdist > 0.5f ? tdist : 0.5f)) * 57.2958f;   // (the width stays 0.15 units -- the head hitbox's mean radius, ~0.07, would halve it -- but the error is now measured from the real head: bot_head)
                if (err > hs * hg + 0.3f) g_bot_fire = 0; }
            { static float mv = -1; static double thurt; float v2 = o->shield + o->health; if (mv >= 0 && v2 < mv - 0.02f) thurt = now; mv = v2;
              // under active camo: no shot beyond the near edge of the weapon's band (+2 units) unless we were hit in the last 2 s --
              // firing gives camo away; walk in instead (below)
              float a = 0, b = 0; if (g_inv.camo && now - thurt > 2.0 && eg_band(g_inv.held_c, &a, &b) && tdist > a * kdist + 2.0f) { g_bot_fire = 0; g_bot_camo_hold = 1; } else g_bot_camo_hold = 0; }
            // (07:44: the fire slack NHOOK_BOT_FIRE_EXTRA 1 -> 2 deg: [firegate] said "aim off" held 30-60% of the steps with a visible target;
            //  the walkers' aim error (3.8 deg median) is the best players' (4.1), who shoot at it and let Halo's aim assist / bullet
            //  magnetism pull the rounds in: 1.6 shots/s vs their 3.85 while aimed at a target)
            // (07:39: [firegate] -- the battle recordings: aimed at a target (within 10 deg for 0.3 s+) the walkers fired 1.6 shots/s,
            //  the best players 3.85. Every 20 s: of the steps with a target within 40 units, the share hidden / still reacting / aim off /
            //  held by the head gate / camo / fire allowed, and of those the share the trigger was really down (burst / pulse / switch))
            // (10:58: counted over the weapon's reach; "allowed" is now a share of the steps the walker could SEE the target -- hidden =
            //  it cannot hit it; "aim turning" = still swinging onto it, outside the fire slack)
            if (tdist < reach) { g_fg_n++; if (!fvis) g_fg_hid++; else { g_fg_vis++; if (fwr > 0 && now - tacq <= react) g_fg_react++; else if (!can_hit) g_fg_aim++;
                else if (g_bot_camo_hold) g_fg_camo++; else if (!g_bot_fire) g_fg_head++; else g_fg_ok++; } }
            // the other weapon when the target is outside the held one's band and well inside the other's (vpad Y held 0.15 s,
            // 3 s between switches; not in the first 0.3 s of a target)
            { static double tsw; if (g_vpad_on && g_inv.has_other && now - tsw > 3.0 && now - tacq > 0.3) {
                  float oh = bot_band_off(g_inv.held_c, tdist, kdist), oo = bot_band_off(g_inv.other_c, tdist, kdist);
                  int hi = wz_ci(g_inv.held_c), oi = wz_ci(g_inv.other_c), zt = 0, zsw = 0, zkeep = 0; float eh = 0, eo = 0;
                  if (g_wz.n && hi >= 0 && oi >= 0) { eh = wz_eff(hi, ftg.c, &zt); eo = wz_eff(oi, ftg.c, 0); if (zt >= 8) { zsw = eo > 1.5f * eh && eo > 1.2f && g_inv.other_ammo != 0; zkeep = eh > 1.5f * eo; } }
                  // (2026-11-09 11:15, the user: "select best weapon in inventory should be a setting too"): the range / zone / players'
                  //  switch in a fight is NHOOK_BOT_FIGHT_SWITCH (live, 1; 0 = keep the weapon in hand -- the empty-magazine switch stays)
                  float fsw = 1; lk_over(&fsw, "NHOOK_BOT_FIGHT_SWITCH"); if (!(fsw > 0)) zsw = 0;
                  if (g_inv.ammo == 0 && g_inv.other_ammo != 0) { tsw = now; if (bot_log()) nlog("[nhook] [inv] magazine empty: switching to %s", g_inv.other); }   // (switching beats reloading mid-fight)
                  else if (zsw) { tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s: around the target (%.0f, %.0f) players kill with a %s %.1fx its carried share, a %s %.1fx (%d kills)",
                                                                g_inv.other, ftg.c[0], ftg.c[1], g_inv.other_c, eo, g_inv.held_c, eh, zt); }
                  // the players' own switch for this pair (behaviour.inventory, 20+ switches): toward the shorter-range class when the
                  // nearest enemy is closer than their median distance, toward the longer one when farther
                  else if (fsw > 0 && !zkeep && hi >= 0 && oi >= 0 && g_eg.sw_n[hi][oi] >= 20 && g_inv.other_ammo != 0 && ({ float mh = 0, mo = 0;
                           for (int q = 0; q < g_eg.nband; q++) { if (!strcmp(g_eg.band[q].cls, g_inv.held_c)) mh = g_eg.band[q].med; if (!strcmp(g_eg.band[q].cls, g_inv.other_c)) mo = g_eg.band[q].med; }
                           mh > 0 && mo > 0 && (mo < mh ? tdist < g_eg.sw_d[hi][oi] : tdist > g_eg.sw_d[hi][oi]); })) {
                      tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s at %.1f units: players switch %s -> %s at %.1f (median of %d)", g_inv.other, tdist, g_inv.held_c, g_inv.other_c, g_eg.sw_d[hi][oi], g_eg.sw_n[hi][oi]); }
                  else if (fsw > 0 && !zkeep && !(hi >= 0 && oi >= 0 && g_eg.sw_n[hi][oi] >= 20) && oh > 0.3f && oo >= 0 && oo < oh * 0.5f) { tsw = now; if (bot_log()) nlog("[nhook] [inv] switching to %s (%s) for %.1f units (the %s band is %.0f%% off)", g_inv.other, g_inv.other_c, tdist, g_inv.held_c, oh * 100); } }
              { static float sh = -1; if (sh < 0) { const char* e = getenv("NHOOK_BOT_SWITCH_HOLD"); sh = e ? strtof(e, 0) : 0.4f; }   // (0.15 s never registered: see nvpad.m's read counters)
                if (now - tsw < sh && g_inv.other_ammo != 0) { fbtn |= 8; bot_switch_press(tsw); } } }   // (the switch bit: ONE packet -- 08:55: held 0.15 s it never switched; an impulse per packet?)
            // B: melee -- the players' range (+ 0.25: positions are a tick or two old), held 0.12 s, at most every 0.6 s
            { static int nomel = -1; if (nomel < 0) nomel = getenv("NHOOK_BOT_NOMELEE") != 0;
              if (nomel) { if (tdist < 1.8f && now - tmelee > 0.7) { tmelee = now; fbtn |= 2; } }   // (the old: B, one step)
              else { mel_load(); int mcls = g_inv.held_c && !strcmp(g_inv.held_c, "melee");
                  float mr = (mcls ? (g_inv.held && strstr(g_inv.held, "HAMMER") ? g_mel.hammer : g_mel.sword) : g_mel.range) + 0.25f;
                  // (12:15: the MELEE RACE -- the death notes: killed at 0-3 units with the walker's shield gone and health 0.8-1.0: the
                  //  first melee wins once a shield is down, and the lunge reaches past the players' median landing distance. Within
                  //  NHOOK_BOT_MELEE_RACE (1.8) units with either shield under 0.15: melee now)
                  float mrace = 1.8f; lk_over(&mrace, "NHOOK_BOT_MELEE_RACE"); if (!mcls && (o->shield < 0.15f || ftg.shield < 0.15f) && mrace > mr) mr = mrace;
                  // (13:20: THE LUNGE, measured -- 10 recorded melee kills: the killer's lunge began at a median 2.24 units (p25 1.78), the
                  //  victim's shield then a median 0.57, the killer's full. Within NHOOK_BOT_MELEE_LUNGE (2.0) units with the target's shield
                  //  under NHOOK_BOT_MELEE_LUNGE_SH (0.7): melee)
                  { float ml = 2.0f, mls = 0.7f; lk_over(&ml, "NHOOK_BOT_MELEE_LUNGE"); lk_over(&mls, "NHOOK_BOT_MELEE_LUNGE_SH"); if (!mcls && ftg.shield < mls && ml > mr) mr = ml; }
                  if (vis && tdist < mr && now - tmelee > 0.6 && now - tacq > 0.15 && now - g_kite_t > 0.5) { tmelee = now;   // (not into a sword / hammer: g_kite_t)
                      if (bot_log()) nlog("[nhook] [melee] %s at %.2f units (target shield %.2f: %s), holding %s", mcls ? "swing" : "melee", tdist, ftg.shield, ftg.shield < 0.05f ? "beatdown" : "full", g_inv.held ? g_inv.held : "-"); }
                  static float mh = -1; if (mh < 0) { const char* e = getenv("NHOOK_BOT_MELEE_HOLD"); mh = e ? strtof(e, 0) : 0.3f; }
                  if (now - tmelee < mh) { fbtn |= bot_meleepad(); g_bot_melee_until = tmelee + mh; } }
              g_bot_pull = !nomel && bot_pull_weapon(g_inv.held);
              // (11:57-12:00: the battle rifle HELD sprays -- an A/B in a BR Valhalla match, 90 s each (shim/tools/fire-ab.sh): pulled
              //  136 shots, 8.5 a second of trigger, accuracy 61.8%; held 159 shots, 9.7 a second, accuracy 22.6% (the top players there
              //  52-56%). The battle rifle / carbine / magnum are pulled at NHOOK_BOT_PULL_PER: NHOOK_BOT_PULL_PRECISION (live, 1; 0 = held))
              { float ppr = 1; lk_over(&ppr, "NHOOK_BOT_PULL_PRECISION"); if (ppr > 0 && g_inv.held_c && !strcmp(g_inv.held_c, "precision")) g_bot_pull = 1; } }
            { static float fj0 = -1; if (fj0 < 0) { const char* e = getenv("NHOOK_BOT_FJUMP"); fj0 = e ? strtof(e, 0) : 0.0f; } float fj = fj0; lk_over(&fj, "NHOOK_BOT_FJUMP");   // jumps a minute in a fight (walker-coach.py: the winners')
              // (11:30, the lessons: jumping helps a defender, not an attacker -- x NHOOK_BOT_FJUMP_DEF / _ATK by the role, 1 = as before)
              { int rl = du_role(ftg.handle); float jd = 1, ja = 1; lk_over(&jd, "NHOOK_BOT_FJUMP_DEF"); lk_over(&ja, "NHOOK_BOT_FJUMP_ATK"); fj *= rl < 0 ? jd : rl > 0 ? ja : 1.0f; }
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
                float r = tdist > p75 * kdist ? 0.9f : tdist < p25 * kdist ? -backoff - 0.2f : press; if (!g_noshield && o->shield < retreat && o->shield >= 0) r = -backoff;
                // OUTRANGED (11:50: today's best player sniped from 26 units while the walker's rifle pulled it across the open into the
                //  scope): the target 1.5x beyond the held weapon's band and holding a sniper / precision weapon -> no closing in, a full
                //  strafe, no long shots; the fight bids under a weapon fetch (g_bot_outranged: nbrain_beh.c beh_fight). NHOOK_BOT_OUTRANGE=0 off
                // (13:50: NHOOK_BOT_OUTRANGE is the factor now -- 1.2 x the band's far edge (1.5 missed a sniper at 14 units); 0 = off)
                { float orng = 1.2f; lk_over(&orng, "NHOOK_BOT_OUTRANGE"); g_bot_outranged = 0;
                  if (orng > 0 && tdist > orng * p75 * kdist && strcmp(wc, "sniper") && strcmp(wc, "precision") && strcmp(wc, "rocket")) {
                      uint32_t tw = ftg.wslot >= 0 && ftg.wslot < 4 ? ftg.weapons[ftg.wslot] : 0xFFFFFFFFu; uint64_t twd = nov_datum(tw); uint32_t twdef = 0;
                      const char* tcls = twd && hb_read(twd, &twdef, 4) ? eg_wcls(hb_tag_name(twdef)) : "none";
                      if (!strcmp(tcls, "sniper") || !strcmp(tcls, "precision")) { g_bot_outranged = 1; r = 0; g_bot_fire = 0;
                          static double tl; if (bot_log() && now - tl > 5) { tl = now; nlog("[nhook] [fight] outranged: a %s at %.0f units vs my %s -- no closing in, strafing, a better weapon first", tcls, tdist, wc); } } } }
                if (g_bot_camo_hold) r = 0.9f;   // (camo: walk in to the near edge of the band before the first shot)
                if (now > tswitch) { side = -side; tswitch = now + g_bm.strafe_switch * kswitch * (0.6f + 0.8f * bot_rand01()); }
                // (11:30, the lessons: attackers who strafe least win more, defenders who strafe hardest survive more -- the strafe x
                //  NHOOK_BOT_STRAFE_ATK / _DEF by the role in this duel)
                float ksr = kstrafe; { int rl = du_role(ftg.handle); float sa = 1, sd = 1; lk_over(&sa, "NHOOK_BOT_STRAFE_ATK"); lk_over(&sd, "NHOOK_BOT_STRAFE_DEF"); ksr *= rl > 0 ? sa : rl < 0 ? sd : 1.0f; }
                float lat = side * fminf(1.0f, ksr * (0.25f + g_bm.strafe_rms / run));
                if (g_eg.on && eg_move(wc, tdist, now, &el, &er)) {   // a real player's moves: across as recorded, toward / away pulled into the band
                    static float lsc0 = -1; if (lsc0 < 0) { const char* e = getenv("NHOOK_BOT_LIBSCALE"); lsc0 = e ? strtof(e, 0) : 1.0f; } float lsc = lsc0; lk_over(&lsc, "NHOOK_BOT_LIBSCALE");   // (the players' own moves: not the tactics' strafe scale)
                    lat = fmaxf(-1.0f, fminf(1.0f, lsc * el / run));
                    // pulled in when beyond the band, never pushed back inside it: the players close in at 1.3 units/s with shields up
                    // on Guardian (engage-compare.py; the push-back made the walkers hover at the band's near edge)
                    static float rsc0 = -1; if (rsc0 < 0) { const char* e = getenv("NHOOK_BOT_RADSCALE"); rsc0 = e ? strtof(e, 0) : 1.0f; } float rsc = rsc0; lk_over(&rsc, "NHOOK_BOT_RADSCALE");   // (walker-coach.py: x the replayed closing / backing off)
                    float pull = tdist > p75 * kdist ? 0.6f : 0.0f; r = fmaxf(-1.0f, fminf(1.0f, rsc * er / run + pull));
                    if (!g_noshield && o->shield < retreat && o->shield >= 0) r = -backoff;
                    if (g_bot_camo_hold) r = 0.9f; }
                // (07:48: PRESS IN with shields up -- the walkers' battle recordings: closing speed 0.02-0.03 u/s in their engagements; the
                //  best players' 0.78 (0.85 with shields up, 0.31 with them low: walker-model.py --top). Shield over 0.7 and the target
                //  beyond the near edge of the weapon's band: NHOOK_BOT_CLOSE (0.4 throttle, live; 0 = old) more toward it -- not when
                //  outranged, retreating or holding camo)
                { float cb = 0.4f; lk_over(&cb, "NHOOK_BOT_CLOSE");
                  if (cb > 0 && o->shield > 0.7f && tdist > p25 * kdist && !g_bot_outranged && !g_bot_camo_hold && fabsf(ftg.c[2] - o->c[2]) < 1.5f) r = fminf(1.0f, r + cb); }   /* (07:53: same level only -- toward an enemy below, the ledge check stopped it dead: "stopped" 62-76% of fight steps, 52% standing) */
                // KITE A SWORD / HAMMER (12:20: "[death] ... edb300a6 1 u (melee) = my target; my shield 1.00": a one-hit kill at arm's
                //  length): the target holding a melee-class weapon within NHOOK_BOT_KITE (5) units -> full speed back, a full strafe,
                //  shooting; after the policy and the rushes below it stays (g_kite)
                int g_kite = 0;
                { float kr = 5; lk_over(&kr, "NHOOK_BOT_KITE"); uint32_t tw = ftg.wslot >= 0 && ftg.wslot < 4 ? ftg.weapons[ftg.wslot] : 0xFFFFFFFFu; uint64_t twd = nov_datum(tw); uint32_t twdef = 0;
                  if (kr > 0 && tdist < kr && twd && hb_read(twd, &twdef, 4) && !strcmp(eg_wcls(hb_tag_name(twdef)), "melee") && !(g_inv.held_c && !strcmp(g_inv.held_c, "melee"))) {
                      g_kite = 1; static double tk; if (bot_log() && now - tk > 3) { tk = now; nlog("[nhook] [fight] a %s at %.1f units: kiting it", hb_tag_name(twdef), tdist); } } }
                // THE LEARNT POLICY (npolicy.c): the best players' moves for this moment, blended in by NHOOK_BOT_POLICY (live, 0 off);
                // the rush, arm's length and floor rules below still apply on top. [policy] every 30 s: how often it moved the walker
                { static double tpj, tpl; static int npol, nsteps, njump; float pw = 0; lk_over(&pw, "NHOOK_BOT_POLICY"); pol_tick(now); nsteps++;
                  if (pw > 0) { float pr, pl; int jp; if (pol_move(now, objs, n, me, o, &ftg, yaw0, &pr, &pl, &jp)) { npol++;
                      float m0 = hypotf(r, lat), m1 = hypotf(pr, pl);
                      r = (1 - pw) * r + pw * pr; lat = (1 - pw) * lat + pw * pl;
                      // (06:50: two moves that disagree cancel -- the blend's speed is the faster one's: a direction from both, never a dither
                      //  in place. NHOOK_BOT_POL_KEEPSPEED=0 = old)
                      { float ks = 1; lk_over(&ks, "NHOOK_BOT_POL_KEEPSPEED"); float mb = hypotf(r, lat), mt = fmaxf(m0, m1);
                        if (ks > 0 && mb > 0.05f && mb < mt) { r = fmaxf(-1, fminf(1, r * mt / mb)); lat = fmaxf(-1, fminf(1, lat * mt / mb)); } }
                      if (pw >= 0.5f && jp && now - tpj > 0.7) { tpj = now; g_fjump_until = now + 0.12; njump++; } } }
                  if (now - tpl > 30) { if (tpl > 0 && pw > 0 && bot_log()) nlog("[nhook] [policy] weight %.2f: moved %d of %d fight steps, %d jumps", pw, npol, nsteps, njump); tpl = now; npol = nsteps = njump = 0; } }
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
                // (13:00: a hammer closed from 4 to 0.9 units while the walker back-pedalled -- backwards is slow; across is full speed:
                //  the kite strafes at full speed, backs off at half, and never melees the swinger: g_kite_t blocks the melee below)
                if (g_kite) { r = -0.5f; lat = lat >= 0 ? 1.0f : -1.0f; g_kite_t = now; }
                // OUTNUMBERED (12:35: the death notes -- most deaths with 2-4 enemies aiming at the walker at once): 2+ enemies within 20
                // units aiming within 12 deg of it and its shield under NHOOK_BOT_CROWD_SH (0.5) -> full speed back with a full strafe,
                // still shooting: out of the crossfire. NHOOK_BOT_CROWD=0 (live) off
                { float cw = 1, csh = 0.5f; lk_over(&cw, "NHOOK_BOT_CROWD"); lk_over(&csh, "NHOOK_BOT_CROWD_SH");
                  if (cw > 0 && o->shield < csh) { int na = 0; for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
                          float dx = o->c[0] - t->c[0], dy = o->c[1] - t->c[1], d = sqrtf(dx * dx + dy * dy); if (d > 20 || d < 0.3f) continue; float ay = atan2f(t->aim[1], t->aim[0]); if ((cosf(ay) * dx + sinf(ay) * dy) / d > 0.978f) na++; }
                      if (na >= 2) { r = -1.0f; if (fabsf(lat) < 0.7f) lat = lat >= 0 ? 0.8f : -0.8f; static double tl; if (bot_log() && now - tl > 4) { tl = now; nlog("[nhook] [fight] outnumbered: %d enemies aiming at me, shield %.2f -- backing out of the crossfire", na, o->shield); } } } }
                // (06:58: the fight GAIN -- "[moves] ... moved 1.09 u/s, asked 1.19 u/s" in a local battle: not blocked, timid; the best
                //  players move 1.71 u/s (median) in fights. The fight's move is scaled by NHOOK_BOT_FIGHT_GAIN (1.4, live; 1 = old), each
                //  axis kept within -1..1, before the floor check below (which still keeps it on the ledge); a kite / crowd escape is full already)
                // INFECTION, a zombie: straight in, a light weave -- whatever the crowd, the caution or the shield said above
                if (g_inf_me == 1) { float zw = 0.35f; lk_over(&zw, "NHOOK_INF_ZWEAVE"); r = 1.0f; lat = fmod(now, 1.4) < 0.7 ? zw : -zw; g_kite = 0; }
                if (!g_kite) { float fg = 1.4f; lk_over(&fg, "NHOOK_BOT_FIGHT_GAIN"); if (fg > 0 && fg != 1.0f) { r = fmaxf(-1, fminf(1, r * fg)); lat = fmaxf(-1, fminf(1, lat * fg)); } }
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
                              if (!bot_floor_ok(pc)) { r = r < 0 ? 0 : r; float pd[3] = {o->c[0] + fx * r * 1.0f, o->c[1] + fy * r * 1.0f, o->c[2] - 0.35f}; if (!bot_floor_ok(pd)) { r = 0; ftries = 3;
                                  // (07:53: at a ledge, strafe ALONG it instead of standing: the strafe alone (either side) when its step has floor)
                                  float sl = fabsf(lat_in) > 0.2f ? fabsf(lat_in) : 0.6f;
                                  for (int sgn = 0; sgn < 2 && ftries == 3; sgn++) { float ls = (sgn ? -side : side) * sl; float pe[3] = {o->c[0] + lx * ls * 1.0f, o->c[1] + ly * ls * 1.0f, o->c[2] - 0.35f};
                                      if (bot_floor_ok(pe)) { lat = ls; ftries = 2; if (sgn) side = -side; } } } } } } } }
                // (08:45, the user: "[the best player] simply moves more nimble, and engages faster ... our walker does not move with that much
                //  *authority*": the recordings say the walker stands still 58% and strafes 0.0 u/s in fights. [moves] every 10 s of
                //  fighting: what the fight asked for, what the floor check let through, how fast the walker really moved across)
                { static double tm; static int ns, nask, nflip, nzero; static float vlat, vall, vask; ns++; vask += fminf(1.0f, hypotf(r, lat)) * (g_bm.run_speed > 0.5f ? g_bm.run_speed : 2.1f); if (fabsf(lat_in) > 0.2f || fabsf(r_in) > 0.2f) nask++; if (ftries == 1 || ftries == 2) nflip++; if (ftries == 3) nzero++;
                  float ex = ftg.c[0] - o->c[0], ey = ftg.c[1] - o->c[1], el2 = sqrtf(ex * ex + ey * ey) + 1e-3f; vlat += fabsf((o->vel[0] * -ey + o->vel[1] * ex) / el2) * 30.0f; vall += sqrtf(o->vel[0] * o->vel[0] + o->vel[1] * o->vel[1]) * 30.0f;
                  static float lp[3]; static double lt; static float dsum; static double dts; static float vraw;
                  if (lt > 0 && now - lt < 0.2) { float dx2 = o->c[0] - lp[0], dy2 = o->c[1] - lp[1]; dsum += sqrtf(dx2 * dx2 + dy2 * dy2); dts += now - lt; }
                  memcpy(lp, o->c, 12); lt = now; vraw += sqrtf(o->vel[0] * o->vel[0] + o->vel[1] * o->vel[1]);
                  if (now - tm > 10.0) { if (tm > 0 && ns > 20 && bot_log()) nlog("[nhook] [moves] %d fight steps: moves asked %.0f%%, floor check flipped %.0f%% / stopped %.0f%%; moved %.2f u/s by position (velocity field %.3f a step), across the enemy line %.0f%% of it; asked %.2f u/s",
                          ns, 100.0 * nask / ns, 100.0 * nflip / ns, 100.0 * nzero / ns, dts > 0 ? dsum / dts : 0.0, vraw / ns, vall > 0 ? 100.0 * vlat / vall : 0.0, vask / ns);
                      tm = now; ns = nask = nflip = nzero = 0; vlat = vall = vask = 0; dsum = 0; dts = 0; vraw = 0; } }
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
        g_eng_close = engaged && tdist < 20 && vis; g_ftg_v = ftg.shield + ftg.health; g_ftg_d = tdist; g_ftg_vis = vis;
        g_ftg_h = ftg.handle; memcpy(g_ftg_c, ftg.c, 12); g_ftg_r = ftg.r;
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
              // (10:10: "[still] 100% of 30 s ... fetch 79%": standing under a weapon on a ledge -- only weapons lying on walkable floor
              //  (a graph node within 1 unit across and 1 unit below / 0.6 above the weapon) are fetched)
              if (g_nav.ok) { float wp[3] = {w[i].c[0], w[i].c[1], w[i].c[2] - 0.3f}; int wn = nav_node_at(wp); float q[3]; if (wn < 0) continue; nav_pos(wn, q);
                  float hx = q[0] - w[i].c[0], hy = q[1] - w[i].c[1], hz = w[i].c[2] - q[2]; if (hx * hx + hy * hy > 1.0f || hz > 1.3f || hz < -0.6f) continue; }
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
      if (g_fetch_abort) { g_fetch_abort = 0; if (g_bot_pick_on) { g_bot_pick_on = 0; tried[(ntried++) & 31] = want_h; if (bot_log()) nlog("[nhook] [fight] the %s: the server put me back twice on the way -- left", g_pick_name); } }
      if (g_bot_pick_on) { float dx = g_bot_pick_pos[0] - o->c[0], dy = g_bot_pick_pos[1] - o->c[1], dz = g_bot_pick_pos[2] - o->c[2], pr = 1.3f; lk_over(&pr, "NHOOK_BOT_PICK_R");
          g_pick_d = sqrtf(dx * dx + dy * dy);
          if (dx * dx + dy * dy < 0.36f && fabsf(dz) >= 1.5f) { g_bot_pick_on = 0; tried[(ntried++) & 31] = want_h;   // (under / over it, out of reach: at once)
              if (bot_log()) nlog("[nhook] [fight] the %s is %.1f units %s: out of reach, left", g_pick_name, fabsf(dz), dz > 0 ? "above" : "below"); }
          else if (dx * dx + dy * dy < pr * pr && fabsf(dz) < 1.5f) { tpress = now; if (!tp0) { tp0 = now;
              if (bot_log()) nlog("[nhook] [fight] at the weapon (%.1f units): walking onto it, act held", sqrtf(dx * dx + dy * dy)); } } }
      uint32_t ppad = bot_pickpad();
      gun_vlist(now);
      // (08:15: standing ON a battle rifle with act held 2.5 s took nothing -- act was already down before the weapon became the action
      //  target, and a hold needs a press to begin: while at the weapon act is PULSED, 0.5 s down / 0.2 s up, a new press every 0.7 s)
      // (08:55: still nothing taken standing on it -- the +0x2c bits look like one-packet IMPULSES (a held act "seats and then
      //  unseats"; a held switch never switched): act now goes out for NHOOK_BOT_ACTPULSE (0.03 s, ~one packet) every 0.4 s at the weapon)
      // (09:40: every "could not take" had the weapon ~0.3 below the walker's centre -- on the ground -- the takes were at waist height:
      //  out of a fight the walker LOOKS at the weapon while it presses (the action target may need the view on it). NHOOK_BOT_PICKLOOK=0 off)
      if (tp0 > 0 && !engaged) { float pl = 1; lk_over(&pl, "NHOOK_BOT_PICKLOOK"); if (pl > 0) { float dx = g_bot_pick_pos[0] - cam[0], dy = g_bot_pick_pos[1] - cam[1], dz = g_bot_pick_pos[2] - cam[2];
          yaw = atan2f(dy, dx); pitch = atan2f(dz, sqrtf(dx * dx + dy * dy) + 1e-3f); out |= 1; } }
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
      // THE LULL RELOAD (2026-11-09 11:35: 5 of 40 deaths in a 37-player Pit match with the magazine EMPTY -- in a brawl the walker is
      //  never out of a fight, so the reload above never came): engaged, but its target out of sight for 1 s+ and nobody aiming at it,
      //  the magazine under NHOOK_BOT_RELOAD_LULL (0.45, live; 0 = off) -> reload now
      { static double tvz, tll; if (g_ftg_vis) tvz = now; float rl = 0.45f; lk_over(&rl, "NHOOK_BOT_RELOAD_LULL");
        if (rl > 0) rl += 0.25f * rv_caution();   // (the three-minute review: a bad window reloads sooner)
        if (rl > 0 && engaged && now - tvz > 1.0 && g_naim_me == 0 && g_inv.ammo >= 0 && g_inv.ammo_max > 2 && g_inv.ammo < rl * g_inv.ammo_max && g_inv.reserve != 0 && now - tll > 4.0 && now - trel > 4.0 && !g_bot_pick_on) {
            tll = now; trel = now; if (bot_log()) nlog("[nhook] [inv] reloading in a lull: %d of %d rounds, the target out of sight", g_inv.ammo, g_inv.ammo_max); } }
      // AFTER A KILL (2026-11-09, a 58-player Pit match: 104 deaths, 23 of the 91 reviewed on an EMPTY magazine, all mid-duel -- in a
      //  brawl the next enemy comes straight after a kill, so the after-the-fight and lull reloads never came): its target died, the
      //  magazine under NHOOK_BOT_RELOAD_KILL (0.6, live; 0 = off) of its most and nobody aiming at it -> reload now
      { static int nk0 = -1; if (nk0 < 0) nk0 = g_gl_nkill;
        if (g_gl_nkill != nk0) { nk0 = g_gl_nkill; float ak = 0.6f; lk_over(&ak, "NHOOK_BOT_RELOAD_KILL");
            if (ak > 0 && !noreload && g_naim_me == 0 && g_inv.ammo >= 0 && g_inv.ammo_max > 2 && g_inv.ammo < ak * g_inv.ammo_max && g_inv.reserve != 0 && now - trel > 2.0 && !g_bot_pick_on) {
                trel = now; if (bot_log()) nlog("[nhook] [inv] reloading after a kill: %d of %d rounds, nobody aiming at me", g_inv.ammo, g_inv.ammo_max); } } }
      // EMPTY (08:30, the user: "empty magazines sans reload are unacceptable btw. do better. find ammo, new weapon, or switch to
      // alternate"): with the magazine known (the learned field, ammo_learn), a 0 there -> the other weapon if it has rounds (switch
      // bit), else the reload tap at once, in a fight too; still 0 after 2 reloads = DRY (no reserve): the other weapon, else any
      // weapon on the floor is "better" (the pick-up scan, a high fetch bid) -- walking over the same weapon refills it
      { static double tswe; static int nrel;
        if (g_ammo_learned >= 0 && g_inv.ammo == 0 && !noreload && g_inv.held) {
            // (11:25: RELOAD FIRST when the reserve has rounds -- the act tap reloads every time, the switch is not yet reliable)
            { static double tclick; if (g_inv.reserve > 0 && now - tclick > 1.5) { tclick = now; g_bot_click_until = now + 0.12; } }   // (the click: an empty pull reloads)
            // (13:40: in a fight -- an enemy in sight within 20 units -- with the other weapon loaded: SWITCH first, it is faster than a
            //  reload; the learnt switch button takes ("3 of 3 took"). "[death] ... ASSAULT RIFLE 0 rounds" at 4 units)
            if (g_eng_close && g_swbit >= 0 && g_inv.has_other && g_inv.other_ammo != 0 && now - tswe > 1.5) { tswe = now; bot_switch_press(now); fbtn |= 8;
                if (bot_log()) nlog("[nhook] [inv] EMPTY %s in a fight: switching to %s (faster than a reload)", g_inv.held, g_inv.other); }
            else if (g_eng_close && g_swbit >= 0 && g_inv.has_other && g_inv.other_ammo != 0) ;
            else if (g_inv.reserve > 0 && now - trel > 2.5) { trel = now; nrel++; if (bot_log()) nlog("[nhook] [inv] EMPTY %s: reloading from %d in reserve (act tap + trigger click)", g_inv.held, g_inv.reserve); }
            else if (g_inv.reserve > 0) ;
            else if (g_inv.has_other && g_inv.other_ammo != 0 && now - tswe > 1.5) { tswe = now; bot_switch_press(now); fbtn |= 8;
                if (bot_log()) nlog("[nhook] [inv] EMPTY %s: switching to %s", g_inv.held, g_inv.other); }
            else if (now - trel > 2.5 && now - tswe > 1.0) { if (nrel >= 2) { if (!g_bot_dry && bot_log()) nlog("[nhook] [inv] DRY: %s still empty after %d reloads -- any weapon on the floor will do", g_inv.held, nrel); g_bot_dry = 1; }
                else { trel = now; nrel++; if (bot_log()) nlog("[nhook] [inv] EMPTY %s: reloading (try %d)", g_inv.held, nrel); } } }
        else if (g_inv.ammo > 0) { nrel = 0; g_bot_dry = 0; }
        // BEFORE DRY (2026-11-09, the same match: every empty-magazine death came mid-duel at ~5 units, the switch at 0 rounds too late):
        //  in a close fight with the magazine at NHOOK_BOT_DRYSW (0.2, live; 0 = off) of its most or less, the target still above half
        //  its vitality and the other weapon loaded -> switch now (faster than a reload); the rank / band switches then wait (no ping-pong).
        //  Not a shotgun / sword (its last round still kills there), not a weapon of 4 rounds or fewer
        { float dsw = 0.2f; lk_over(&dsw, "NHOOK_BOT_DRYSW");
          if (dsw > 0 && g_eng_close && !noreload && g_inv.held_c && strcmp(g_inv.held_c, "shotgun") && g_inv.ammo > 0 && g_inv.ammo_max > 4 && g_inv.ammo <= dsw * g_inv.ammo_max
              && g_ftg_v > 0.5f && g_swbit >= 0 && g_inv.has_other && g_inv.other_ammo != 0 && now - tswe > 1.5) {
              tswe = now + 3.0; bot_switch_press(now); fbtn |= 8;
              if (bot_log()) nlog("[nhook] [inv] %s down to %d of %d in a fight (the target at %.2f): switching to %s before it runs dry", g_inv.held, g_inv.ammo, g_inv.ammo_max, g_ftg_v, g_inv.other); } }
        // THE BETTER WEAPON IN HAND (09:20: "TOOK the BATTLE RIFLE ... holding ASSAULT RIFLE, other BATTLE RIFLE"): out of a fight, with the
        // other weapon 2+ ranks better and not known empty, one switch impulse (every 2 s until it is in hand)
        // (10:05: in a fight only when the target's distance is inside the other weapon's band -- it swapped the rifle for a shotgun mid-range)
        // (10:35: in a fight also only when the HELD weapon is out of its band -- else the band rule switched straight back: ping-pong)
        { static double tbt; float kdb = 1; lk_over(&kdb, "NHOOK_BOT_DIST"); float ob = engaged ? bot_band_off(g_inv.other_c, tdist, kdb) : 0, hbo = engaged ? bot_band_off(g_inv.held_c, tdist, kdb) : 1;
          // (12:25: "[death] fighting ... at 8 u ... EXCAVATOR 2 rounds": out of a fight a close-range gun (shotgun / Mauler / sword /
          //  hammer) is never put in hand by rank -- the range-band switch brings it out when an enemy is close)
          int close_gun = g_inv.other_c && (!strcmp(g_inv.other_c, "shotgun") || !strcmp(g_inv.other_c, "melee"));
          // (2026-11-09 11:15, the user: "select best weapon in inventory should be a setting too, and select best weapon for your hunt
          //  just the same"): NHOOK_BOT_BEST_WEAPON (live, 1; 0 = off) with NHOOK_BOT_BEST_RANKS (2) = how many ranks better the other
          //  weapon must be. THE HUNT'S WEAPON, NHOOK_BOT_HUNT_WEAPON (live, 1): out of a fight with a goal (an enemy's last position,
          //  a radar blip, revenge, an item, a lane), the weapon players kill with around the goal point (the zone model, 8+ kills
          //  there) goes in hand ahead of the meeting -- the other weapon NHOOK_BOT_HUNT_WEAPON_X (1.3) times as effective there; the
          //  one in hand that much better there is kept (the rank rule does not switch it back). Shotguns / swords count here too
          float bw = 1, brk = 2, hw = 1, hx = 1.3f; lk_over(&bw, "NHOOK_BOT_BEST_WEAPON"); lk_over(&brk, "NHOOK_BOT_BEST_RANKS"); lk_over(&hw, "NHOOK_BOT_HUNT_WEAPON"); lk_over(&hx, "NHOOK_BOT_HUNT_WEAPON_X");
          int hpick = 0, hkeep = 0, hzt = 0; float hhe = 0, hoe = 0;
          if (hw > 0 && !engaged && g_gl.kind && g_wz.n && g_inv.has_other && g_inv.other_ammo != 0) { int hi2 = wz_ci(g_inv.held_c), oi2 = wz_ci(g_inv.other_c);
              if (hi2 >= 0 && oi2 >= 0) { hhe = wz_eff(hi2, g_gl.pos, &hzt); hoe = wz_eff(oi2, g_gl.pos, 0); if (hzt >= 8) { hpick = hoe > hx * hhe && hoe > 1.0f; hkeep = hhe > hx * hoe; } } }
          int rpick = bw > 0 && g_inv.other_rank + (int)(brk + 0.5f) <= g_inv.held_rank;
          if (hpick && now - tbt > 3.0 && now - tswe > 1.0) { tbt = now; tswe = now; bot_switch_press(now); fbtn |= 8;
              if (bot_log()) nlog("[nhook] [inv] for the hunt (%s at %.0f, %.0f): switching to %s -- players kill with a %s %.1fx its carried share there, a %s %.1fx (%d kills)",
                                  gl_kn[g_gl.kind], g_gl.pos[0], g_gl.pos[1], g_inv.other, g_inv.other_c, hoe, g_inv.held_c, hhe, hzt); }
          else if (((!engaged && !close_gun && !hkeep) || (engaged && tdist > 3.0f && ob <= 0.05f && hbo != 0)) && g_inv.has_other && rpick && g_inv.other_ammo != 0 && now - tbt > 3.0 && now - tswe > 1.0) { tbt = now; tswe = now;   // (09:45: in fights too, beyond 3 units -- a 20-player FFA is nearly always "engaged")
              bot_switch_press(now); fbtn |= 8;
              if (bot_log()) nlog("[nhook] [inv] the better weapon in hand: switching from %s to %s%s", g_inv.held, g_inv.other, engaged ? " (the target is in its range)" : ""); } } }
      // (10:40: does a switch pulse take? 1 s after each new pulse: the weapon in hand changed or not -- [inv] switch pulses every 2 min)
      { static double tchk, tlast_until, trep; static const char* held0; static int ntry, nok;
        { double su = g_swbit >= 0 ? g_swrec_until : g_bot_switch_until; if (su != tlast_until && su > now) { tlast_until = su; tchk = now; held0 = g_inv.held; } }
        if (tchk > 0 && now - tchk > 1.0) { ntry++; if (g_inv.held != held0) nok++; tchk = 0; }
        if (now - trep > 120) { if (trep > 0 && ntry && bot_log()) { float swp = 0.03f; lk_over(&swp, "NHOOK_BOT_SWPULSE"); nlog("[nhook] [inv] switch presses (%s): %d of %d took", g_swbit >= 0 ? "the learnt record bit" : "packet pulses", nok, ntry); (void)swp; } trep = now; ntry = nok = 0; } }
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
    beh_fetch(&W, &arb); beh_fight(&W, &arb, ob_carry); beh_hold(&W, &arb); beh_break(&W, &arb); beh_cover(&W, &arb); beh_recharge(&W, &arb); beh_hunt(&W, &arb);
    // DEATH NOTES (12:05): what killed the walker -- each step remembers its target and the enemies aiming at it (within 12 deg, 40
    // units: the likely killer), its shield, weapon and rounds; at the death a [death] line, every 8 deaths a [deaths] summary: how
    // often the likely killer was NOT the target (awareness), at what distance, with what
    { static DeathSnap lastS; static int was_alive = 1, nd, nother; static float dsum;
      lastS.tg = have_tg ? ftg.handle : 0; lastS.td = have_tg ? tdist : 0; lastS.tvis = have_tg ? los_get(ftg.handle, now) == 1 : 0; lastS.naim = 0; lastS.sh = o->shield; lastS.hp = o->health; lastS.w = g_inv.held; lastS.am = g_inv.ammo;
      for (int i = 0; i < n && lastS.naim < 4; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue; float dx = o->c[0] - t->c[0], dy = o->c[1] - t->c[1], d = sqrtf(dx * dx + dy * dy); if (d > 40 || d < 0.2f) continue;
          float ay = atan2f(t->aim[1], t->aim[0]); if ((cosf(ay) * dx + sinf(ay) * dy) / d < 0.978f) continue;   // (cos 12 deg)
          uint32_t tw = t->wslot >= 0 && t->wslot < 4 ? t->weapons[t->wslot] : 0xFFFFFFFFu; uint64_t twd = nov_datum(tw); uint32_t twdef = 0;
          lastS.ah[lastS.naim] = t->handle; lastS.ad[lastS.naim] = d; lastS.aw[lastS.naim] = twd && hb_read(twd, &twdef, 4) ? eg_wcls(hb_tag_name(twdef)) : "?"; lastS.naim++; }
      (void)was_alive; (void)nd; (void)nother; (void)dsum; g_death_snap = &lastS; g_naim_me = lastS.naim;
      du_tick(now, objs, n, me, have_tg ? ftg.handle : 0, g_bot_trig); }
    { static float ol0 = -1; if (ol0 < 0) { const char* e = getenv("NHOOK_NAV_OFFLANE"); ol0 = e ? strtof(e, 0) : 0.0f; } float ol = ol0; lk_over(&ol, "NHOOK_NAV_OFFLANE"); g_nav_offlane = ol;
      float lp = 40; lk_over(&lp, "NHOOK_NAV_LIFTPEN"); g_nav_liftpen = lp;
      float bp = 25; lk_over(&bp, "NHOOK_NAV_BLOCKPEN"); g_nav_blkpen = bp; }   // (06:30: lifts only when nothing else goes there: a stuck at a lift's foot, the next node 5.5 units up, was the commonest)   // (15:30: paths along the players' lanes: src/nav.c)
    int arbw = arb_choose(&arb, now);
    // (09:55: the recordings: the walker moves under 0.3 u/s 53% of its life -- median 0.09 u/s, the players 0.7-1.2 -- while its fight moves
    //  measure 1.5 u/s: [still] every 30 s splits the still time by the legs' winner and the path state)
    { static double tl, tlast; static float lp2[3]; static double tot, still; static double by[ARB_N + 2]; double dts = tlast > 0 ? now - tlast : 0; tlast = now;
      if (dts > 0 && dts < 0.2) { float dx = o->c[0] - lp2[0], dy = o->c[1] - lp2[1]; float sp2 = sqrtf(dx * dx + dy * dy) / (float)dts; tot += dts;
          if (sp2 < 0.3f) { still += dts; int k = arbw; if ((arbw == ARB_HUNT || arbw == ARB_FIGHT) && plen < 2 && !engaged) k = ARB_N; else if (engaged) k = ARB_N + 1 > k ? (arbw == ARB_FIGHT ? ARB_FIGHT : k) : k; by[k] += dts; } }
      memcpy(lp2, o->c, 12);
      if (now - tl > 30) { if (tl > 0 && tot > 5 && bot_log()) { static const char* nm[ARB_N + 2] = {"hunt", "fight", "follow", "fetch", "ball", "board", "hold", "recharge", "no path (hunt)", "?"}; char b[400]; int q = 0;
              for (int k = 0; k < ARB_N + 2; k++) if (by[k] > 0.5) q += snprintf(b + q, sizeof b - (size_t)q, " %s %.0f%%,", nm[k], 100.0 * by[k] / tot);
              nlog("[nhook] [still] %.0f%% of %.0f s alive under 0.3 u/s:%s plan %d nodes, goal %d", 100.0 * still / tot, tot, q ? b : " -", plen, goal); }
          tl = now; tot = still = 0; memset(by, 0, sizeof by); } }
    if (arbw == ARB_FOLLOW || arbw == ARB_FETCH || arbw == ARB_BALL || arbw == ARB_RECHARGE || (arbw == ARB_BOARD && gbd > 4.0f)) {   // a path to the winner's point
        static float lastp[3]; static int lastw = -1; static double trp; const float* wp = arb.b[arbw].pt;
        mode = 3; memcpy(g_bot_pt, wp, 12); g_bot_pt[2] -= 0.35f;
        float dx = wp[0] - lastp[0], dy = wp[1] - lastp[1];
        if (arbw != lastw || dx * dx + dy * dy > 2.25f || now - trp > 1.0) { goal = -1; trp = now; memcpy(lastp, wp, 12); }
        lastw = arbw; }
    else { static int lastw2 = -1; if (arbw != lastw2) goal = -1; lastw2 = arbw; }   // (fight / hold / hunt: the goal draw's path; the fight's moves below)
    if (mode && g_nav.ok) {
        float pos[3] = {o->c[0], o->c[1], o->c[2] - 0.35f}; int cur = nav_node_at(pos);
        if (cur >= 0) {
            int need = plen == 0 || goal < 0 || now - tplan > (mode == 2 ? 1.0 : 3.0); int gres = 0;
            if (goal >= 0 && plen) { float gp[3]; nav_pos(goal, gp); if ((gp[0] - pos[0]) * (gp[0] - pos[0]) + (gp[1] - pos[1]) * (gp[1] - pos[1]) < 1.0f) { nav_say("  [nav] reached the goal\n"); goal = -1; need = 1;
                // (06:15: the goal draw handed the same goal back at once -- "path of 2 nodes ... reached the goal" in a loop, the walker
                //  standing still, read as "stuck" ~1 a second in local battles: a reached hunt goal is dropped from the draw)
                if (mode == 2 && g_eg.on && gl_on()) gl_drop(now, "reached"); } }
            if (mode == 2 && g_bot_pick_on) { int gn = nav_node_at(g_bot_pick_pos); if (gn >= 0) goal = gn; }   // (the fighter's pickup: walk to the weapon)
            else if (mode == 2 && g_inf_me == 1) {   // INFECTION, a zombie: the nearest human, one with other humans near it counted farther
                float bs = 1e30f; int bi = -1;
                for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || nov_side_of(t->handle) != 0) continue;
                    float dx = t->c[0] - pos[0], dy = t->c[1] - pos[1], d = sqrtf(dx * dx + dy * dy); int nb = 0;
                    for (int j = 0; j < n; j++) { const NovObj* u = &objs[j]; if (j == i || u->type || u->dead || nov_side_of(u->handle) != 0) continue; float ex = u->c[0] - t->c[0], ey = u->c[1] - t->c[1]; if (ex * ex + ey * ey < 36) nb++; }
                    float sc = d * (1 + 0.5f * nb); if (sc < bs) { bs = sc; bi = i; } }
                if (bi >= 0) { float tp[3] = {objs[bi].c[0], objs[bi].c[1], objs[bi].c[2] - 0.35f}; int gn = nav_node_at(tp); if (gn >= 0) goal = gn; } }
            else if (mode == 2 && g_inf_me == 0 && !g_bot_fighting && (gres = inf_group_goal(objs, n, me, pos, &goal, now)) != 0) { if (gres == 2) { goal = -1; plen = 0; } }   // INFECTION, a human: with the group
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
            // ISLANDS (2026-11-09, Hugegrass: the walker spawned on a part of the floor graph joined to the rest by nothing it knew -- 6 min
            //  stood at its base, every goal "no path"): 3 failed plans within 8 s -> 4 s walking straight at the last goal, hopping
            //  (the graph misses a drop, a ramp edge, a lift: the legs find it; nav-portals.py learns it from players afterwards)
            static int npf; static double npt0, npu; static float npto[3];
            if (goal >= 0 && need) { int g0 = goal; int np = nav_astar(cur, goal, path, 8192); tplan = now;
                if (np < 2) { if (now - npt0 > 8.0) { npt0 = now; npf = 0; }
                              // (17:40, the user: "clearly is not learning from teammate lanes" -- the walker hopped west toward its goal
                              //  while every teammate left the room by the teleporters 8 units east): the escape heads for the NEAREST
                              //  PLACE PLAYERS LEFT FROM -- a learned link's start (g_np_a, nav-portals.py) within 60 units, not in its own
                              //  spot -- else at the goal
                              if (++npf >= 3 && now > npu) { nav_pos(g0, npto); npu = now + 4.0; npf = 0; int via = -1; float bd = 3600.0f;
                                  for (int q = 0; q < g_np_n; q++) { float qp[3]; nav_pos(g_np_a[q], qp); float dx = qp[0] - pos[0], dy = qp[1] - pos[1], dd = dx * dx + dy * dy;
                                      if (dd > 0.5f && dd < bd && fabsf(qp[2] - pos[2]) < 4.0f) { bd = dd; via = q; } }
                                  if (via >= 0) nav_pos(g_np_a[via], npto);
                                  nav_say("  [nav] no path 3 times in 8 s from node %d: walking straight at (%.1f %.1f) for 4 s, hopping (%s)\n", cur, npto[0], npto[1],
                                          via >= 0 ? "where players left this spot from" : "an island of the floor graph: toward the goal"); }
                              if (mode != 2) { nav_say("  [nav] no path from node %d to %d: a new goal\n", cur, goal); goal = -1; }
                              else if (g_eg.on && gl_on()) { gl_drop(now, "no path"); goal = -1; }   // (the goal draw: another at once -- it used to stand there)
                              plen = 0; }
                else { nav_say("  [nav] path of %d nodes from (%.1f %.1f %.1f) to node %d\n", np, pos[0], pos[1], pos[2], goal); plen = np; pidx = 0; g_nav_ok_t = now; } }
            if (plen >= 2) {
                { float bd = 1e30f; int bi = pidx; for (int i = pidx; i < plen && i < pidx + 60; i++) { float p[3]; nav_pos(path[i], p); float d = (p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]); if (d < bd) { bd = d; bi = i; } }
                  pidx = bi; if (bd > 4.0f) tplan = 0; }   // more than 2 units off the path: replan
                { static double tpub; if (now - tpub > 0.25) { tpub = now; nav_publish(path, pidx, plen, goal); } }
                static int look = -1; if (look < 0) { const char* e = getenv("PELOAD_NAV_LOOK"); look = e ? atoi(e) : 6; if (look < 1) look = 1; }
                // (06:20: 601 of 610 stucks in a local battle were at path index 0 -- steering at the node 6 ahead cuts every corner into
                //  the wall at a path's start: for 3 s after a stuck the walker steers at the next node, NHOOK_BOT_LOOK_STUCK (1))
                float lks = 1; lk_over(&lks, "NHOOK_BOT_LOOK_STUCK"); int lk = now - tjump < 3.0 && tjump > 0 ? (int)lks : look; if (pidx < 2 && lk > 2) lk = 2; if (lk < 1) lk = 1;   /* (and 2 nodes at most for a path's first steps) */
                int ti = pidx + lk < plen ? pidx + lk : plen - 1; float tp[3]; nav_pos(path[ti], tp);
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
                  // (15:10: only while the legs FOLLOW the path -- in a fight (or holding) they belong to the fight, the walker strafes
                  //  instead of walking the path, and that read as "stuck" all fight long: in a local battle 300-400 "stuck" and 100+
                  //  "wedged" a walker in 5 minutes, each one jumping and the escape running the walker about mid-fight)
                  // (07:15: NO PROGRESS -- on The Pit a walker hovered around one spot for 41.7 s (the server's recording: 68% of its
                  //  time standing) while its own log said "moving": it slid back and forth along an obstacle, never under 0.35 units in
                  //  1.4 s, replanning paths of 111-145 nodes. Now also stuck when the path left to the SAME goal has not shrunk by 2 nodes
                  //  in NHOOK_BOT_NOPROG (4 s, live; 0 = off) of path walking)
                  // (07:22: SNAPPED BACK -- the hover spot on The Pit was the SERVER holding the walker: its log walked west to
                  //  (-19.2, -4.2) and the next step stood at (-15.6, -3.3) again, 3.6 units back -- something there blocks it on the
                  //  server only. A jump of over 1.5 units within 0.25 s while alive on foot is a snap-back: stuck at the server's spot,
                  //  so that step is blocked and the plan goes round. NHOOK_BOT_SNAPBACK=0 (live) = off)
                  static float sb_last[3]; static double sb_t; int snapped = 0; float sbk = 1; lk_over(&sbk, "NHOOK_BOT_SNAPBACK");
                  if (now < g_bot_act_until) g_act_last = now;   /* (08:01: for the snap-back line -- does the server hold a walker around a pickup?) */
                  // (07:27: and only BACK to where it stood in the last 5 s (within 1 unit) -- a man cannon (Valhalla's bases) also moves a
                  //  walker 1.5+ units in 0.25 s, forward)
                  // (08:01: 12 s of positions, not 5: a client that wandered off longer before the server put it back was missed --
                  //  30 s held on The Pit with 3 snaps seen)
                  { static float rp[120][3]; static double rt[120]; static int rn; static double trp;
                    float ddx = pos[0] - sb_last[0], ddy = pos[1] - sb_last[1];
                    if (sbk > 0 && sb_t > 0 && now - sb_t < 0.25 && ddx * ddx + ddy * ddy > 1.5f * 1.5f)
                        for (int i = 0; i < 120 && i < rn; i++) { int k = (rn - 1 - i) % 120; if (now - rt[k] > 12.0) break; float ex = rp[k][0] - pos[0], ey = rp[k][1] - pos[1], ez = rp[k][2] - pos[2];
                            if (now - rt[k] > 0.3 && ex * ex + ey * ey < 1.0f && fabsf(ez) < 1.0f) { snapped = 1; break; } }
                    memcpy(sb_last, pos, 12); sb_t = now;
                    if (now - trp >= 0.1) { trp = now; memcpy(rp[rn % 120], pos, 12); rt[rn % 120] = now; rn++; } }
                  static int pg_goal = -2, pg_best; static double pg_t; int noprog = 0; float npg = 4; lk_over(&npg, "NHOOK_BOT_NOPROG");
                  { int walking = !strafe && arbw != ARB_FIGHT && arbw != ARB_HOLD && !g_bot_fighting && plen > 0, rem = plen - pidx;
                    if (!walking || goal != pg_goal) { pg_goal = walking ? goal : -2; pg_best = rem; pg_t = now; }
                    else if (rem < pg_best - 2 || rem > pg_best + 4) { pg_best = rem; pg_t = now; }   /* (progress, or a new longer plan: a detour starts over) */
                    else if (npg > 0 && now - pg_t > npg) { noprog = 1; pg_t = now; pg_best = rem; } }
                  if (!strafe && arbw != ARB_FIGHT && arbw != ARB_HOLD && !(g_bot_fighting && tdist < 3.0f) && ((now - histt[j & 63] >= 1.4 && dx * dx + dy * dy < 0.35f * 0.35f) || noprog || snapped) && (now - tjump > 1.5 || snapped)) {
                      if (noprog) nav_say("  [nav] no progress for %.0f s at (%.1f %.1f %.1f): treated as stuck\n", npg, pos[0], pos[1], pos[2]);
                      if (snapped && bot_log()) nlog("[nhook] [nav] snapped back by the server to (%.1f %.1f %.1f): treated as stuck (act last pressed %.1f s ago; legs %s)", pos[0], pos[1], pos[2], g_act_last > 0 ? now - g_act_last : -1.0, g_arb_why);
                      // (08:06: EVERY snap-back of a Pit battle came while fetching a weapon -- 6 in 35 s toward one, act last pressed
                      //  12-47 s before, so not the act press -- and the fetch never gave up: on its own screen it kept getting closer.
                      //  Two snap-backs within 20 s while fetching drop that weapon (g_fetch_abort -> the pickup code's "tried" list))
                      if (snapped && arbw == ARB_FETCH) { static double tsf; static int nsf; if (now - tsf > 20.0) nsf = 0; tsf = now; if (++nsf >= 2) { g_fetch_abort = 1; nsf = 0; } }
                      // (01:10: fighting at arm's length is no progress by design, not stuck)
                      // (07:05: these statements sat AFTER that comment on its line -- commented out since 01:10: tjump never moved, so
                      //  "stuck" fired on EVERY step once a walker slowed (3 lines in one 0.1 s step), each adding +8 to 10 path nodes,
                      //  blocking steps and replanning; no back-off ever ran, no jump timing, "4 in 10 s" always true. 239-669 stucks a
                      //  battle came from this)
                      tjump = now; hh = 0; tback = now; bstr = (nav_rand() & 1) ? 0.3f : -0.3f;
                      nav_pos(path[pidx >= 4 ? pidx - 4 : 0], bk);
                      // (14:45: "stuck at" 188 times in 3000 log lines on Valhalla, ~50 at one spot: the +4 penalty never routed around it, and
                      //  the walker never jumped -- on rocks and lips a jump is what frees a player. Each stuck now jumps (0.25 s) and the next 10
                      //  path nodes cost NHOOK_BOT_STUCK_PEN (8, live) more)
                      { float sp = 8; lk_over(&sp, "NHOOK_BOT_STUCK_PEN"); for (int i = pidx; i < plen && i < pidx + 10; i++) g_nav.pen[path[i]] += sp; }
                      // (06:42: 2 of 3 stucks came at the FIRST step of a fresh path -- the replan from the spot chose the same step again
                      //  ("stuck at (-3.7 -18.5 19.0) ... path 0/141", 0/192, 0/164, 0/165 on Guardian). The step being pushed along
                      //  (the walker's node -> the path's next) is marked blocked in the walking graph (nav.c nav_block_step: +25, live
                      //  NHOOK_NAV_BLOCKPEN); blocked twice it is kept for the map (maptop/<map>.blocked.txt))
                      { int nx = pidx + 1 < plen ? path[pidx + 1] : -1, bs = 0, from = -1;
                        if (snapped && plen > 1) {   // (a snap-back: the blocked step leaves the spot the server held it at -- the path node nearest it, up to pidx)
                            int kb = 0; float kd = 1e30f; for (int i = 0; i <= pidx && i < plen - 1; i++) { float q[3]; nav_pos(path[i], q); float e2 = (q[0] - pos[0]) * (q[0] - pos[0]) + (q[1] - pos[1]) * (q[1] - pos[1]); if (e2 < kd) { kd = e2; kb = i; } }
                            nx = path[kb + 1]; if (cur < 0 || cur == nx) cur = path[kb];
                            // (07:29: the server held it whatever the first step: one blocked step a snap and the next plan went the same way
                            //  by a neighbouring step -- 33-44 s held at one spot on The Pit. The whole stretch the client walked before the
                            //  snap (path nodes kb+1 .. pidx+2) costs NHOOK_NAV_BLOCKPEN more for the session, so the replan turns away)
                            int np = 0; for (int i = kb + 1; i < plen && i <= pidx + 2; i++) { g_nav.pen[path[i]] += g_nav_blkpen; np++; }
                            nav_say("  [nav] snap-back: %d path nodes the server never let it walk cost +%.0f\n", np, g_nav_blkpen); }
                        if (nx >= 0 && cur >= 0 && cur != nx) { from = cur; bs = nav_block_step(cur, nx); }
                        if (!bs && nx >= 0) { from = path[pidx]; bs = nav_block_step(from, nx); }
                        if (bs) nav_say("  [nav] the step %d -> %d is blocked%s\n", from, nx, bs == 2 ? " (again: kept for this map)" : "");
                        // (06:48: one Guardian node had 5 of its 8 steps blocked -- the walker pinned there whatever way it pushed: a TRAP
                        //  (nav.c nav_trap, 3+ blocked; A* keeps out of it). Caught in one, the escape runs 2 s (random directions at full
                        //  throttle, jumps) -- only then: run on "4 stucks in 10 s" it fired all the time (15:20 entry). NHOOK_BOT_TRAP_ESC=0 off)
                        { float te = 2.0f; lk_over(&te, "NHOOK_BOT_TRAP_ESC"); if (te > 0 && from >= 0 && nav_trap(from) && now > g_escape_until + 2.0) { g_escape_until = now + te; g_escape_t = 0;
                            if (bot_log()) nlog("[nhook] [nav] trapped at (%.1f %.1f %.1f), node %d (%d of 8 ways blocked): escaping", pos[0], pos[1], pos[2], from, __builtin_popcount(g_nav_blk[from])); } } }
                      g_fjump_until = now + 0.25;
                      memmove(tstuck, tstuck + 1, 3 * sizeof(double)); tstuck[3] = now;
                      // (2026-11-08, the user: "why was 'second' walker stationary so long with the smg and assault rifle?": 24 s at a ledge on
                      //  Guardian's middle level, its path to a hidden enemy 17 units off dropping ~3 units -- creeping, "stuck", backing off,
                      //  replanning the same stretch: in the goal draw (mode 2) four stucks never dropped the goal. Now they do -- the draw
                      //  picks another at once)
                      // (14:50: WEDGED -- stuck 4 times at one spot ((60.8, -146.9) for minutes on Valhalla: 0 kills / 7 deaths in that window):
                      //  every graph node within 2.5 units costs NHOOK_BOT_WEDGE_PEN (60) more, and an ESCAPE runs 3 s: a random compass
                      //  direction at full throttle, a new one every 0.5 s, a jump every 0.6 s (g_escape_*, applied below the path walk)
                      // (15:20: OFF by default -- in local battles "stuck" fires ~1 a second whatever the walker does (an old problem: 402 in a
                      //  01:09 battle), so "4 in 10 s" was nearly always true and the escape ran most of the time, the penalties piling up;
                      //  NHOOK_BOT_WEDGE_PEN > 0 (live) turns it on)
                      if (now - tstuck[0] < 10.0) { float wp = 0; lk_over(&wp, "NHOOK_BOT_WEDGE_PEN"); if (wp > 0)
                          for (int i = 0; i < g_nav.n; i++) { float q[3]; nav_pos(i, q); float ddx = q[0] - pos[0], ddy = q[1] - pos[1], ddz = q[2] - pos[2]; if (ddx * ddx + ddy * ddy < 6.25f && fabsf(ddz) < 2.0f) g_nav.pen[i] += wp; }
                          if (wp > 0) { g_escape_until = now + 3.0; g_escape_t = 0; if (bot_log()) nlog("[nhook] [nav] wedged at (%.1f %.1f): escaping, the spot's nodes +%.0f", pos[0], pos[1], wp); } }
                      if (now - tstuck[0] < 10.0) { nav_say("  [nav] stuck 4 times at (%.1f %.1f %.1f): a new goal\n", pos[0], pos[1], pos[2]);
                          if (mode != 2) goal = -1; else if (g_eg.on && gl_on()) { gl_drop(now, "stuck 4 times"); goal = -1; plen = 0; }
                          memset(tstuck, 0, sizeof tstuck); }
                      else nav_say("  [nav] stuck at (%.1f %.1f %.1f): back off and replan (legs: %s, path %d/%d, goal %d)\n", pos[0], pos[1], pos[2], g_arb_why, pidx, plen, goal);
                      tplan = 0; } }
                if (now < g_escape_until) { if (now - g_escape_t > 0.5) { g_escape_t = now; g_escape_dir = bot_rand01() * 6.2831853f; }
                    float rel = bot_wrap(g_escape_dir - yaw); tx = cosf(rel); ty = sinf(rel); if (fmod(now, 0.6) < 0.15) g_fjump_until = now + 0.12; }
                else if (now - tback < 0.6) { float bw = atan2f(bk[1] - pos[1], bk[0] - pos[0]), rel = bot_wrap(bw - yaw);
                    if ((bk[0] - pos[0]) * (bk[0] - pos[0]) + (bk[1] - pos[1]) * (bk[1] - pos[1]) > 0.09f) { tx = cosf(rel) * 0.8f; ty = sinf(rel) * 0.8f + bstr; } else { tx = 0; ty = 0; } }
                out |= 2; g_nav_act = 1;
                if (now - tjump < 0.25 && g_bot_jump) { g_bot_btn = g_bot_jump; out |= 4; }
                if (now - tstat > 2.0) { tstat = now; nav_say("  [nav] at (%.1f %.1f %.1f) node %d, path %d/%d, next (%.1f %.1f %.1f) -> throttle (%.2f, %.2f)\n",
                    pos[0], pos[1], pos[2], cur, pidx, plen, tp[0], tp[1], tp[2], tx, ty); } }
            else if (now < npu && !g_bot_fighting) { float way = atan2f(npto[1] - pos[1], npto[0] - pos[0]);
                if (!(out & 1)) { float e = bot_wrap(way - yaw0), mx = 0.2f; yaw = yaw0 + (e > mx ? mx : e < -mx ? -mx : e); pitch = -0.08f; out |= 1; }
                float rel = bot_wrap(way - yaw); tx = cosf(rel); ty = sinf(rel); if (fmod(now, 1.2) < 0.15) g_fjump_until = now + 0.12;
                out |= 2; g_nav_act = 1; }
            else g_nav_act = 0; }
        else g_nav_act = 0; }
    if (arbw == ARB_BOARD && gbd <= 4.0f) {   // boarding: straight to the point behind the turret, facing the turret, act pulsed (0.4 s a second) close in
        float way = atan2f(gbp[1] - o->c[1], gbp[0] - o->c[0]), fy = atan2f(gface[1] - cam[1], gface[0] - cam[0]), fp = atan2f(gface[2] - cam[2], sqrtf((gface[0] - cam[0]) * (gface[0] - cam[0]) + (gface[1] - cam[1]) * (gface[1] - cam[1])));
        yaw = fy; pitch = fp; out |= 1; float rel = bot_wrap(way - yaw), sp = gbd > 1.0f ? 1.0f : gbd > 0.4f ? 0.45f : 0.15f; tx = cosf(rel) * sp; ty = sinf(rel) * sp; out |= 2;
        { static double ttap; if (g_gun_press_ok && now - ttap > 1.2) { ttap = now; g_bot_act_until = now + 0.15; } }   // (a TAP, then 1.2 s to see it seated: a held press seats and then unseats)
        { static double tl; if (bot_log() && now - tl > 1.0) { tl = now; nlog("[nhook] [gunner] boarding: %.1f units from the point behind the turret%s", gbd, g_gun_press_ok ? ", right behind it: pressing act" : ""); } } }
    else if (arbw == ARB_FETCH && g_pick_d < 1.3f && engaged && tdist < 10.0f) { tx = ftx; ty = fty; out |= 2; }   // (09:30: at the weapon in a fight: the fight's moves, act still pulsed -- never a still target)
    // (06:35: "going for EXCAVATOR (1.6 units ...)" then 30+ "stuck at (11.9 -11.8 18.1) (legs: fetch ...)" on Guardian: 1.6 units ACROSS
    //  but on another level, its path 27 nodes round -- the straight walk ran into the wall. Straight only on the same level (under 1 unit
    //  up / down) and when the path is no detour (at most 2 nodes a unit + 4); else the path's way)
    else if (arbw == ARB_FETCH && g_pick_d < 3.0f && fabsf(g_bot_pick_pos[2] - o->c[2]) < 1.0f && (plen == 0 || plen - pidx <= (int)(g_pick_d * 2.0f) + 4)) {   // (the last 3 units of a weapon fetch: straight at it -- it may lie off the walking graph)
        float way = atan2f(g_bot_pick_pos[1] - o->c[1], g_bot_pick_pos[0] - o->c[0]), rel = bot_wrap(way - yaw), sp = g_pick_d > 1.0f ? 1.0f : 0.45f;   /* (06:15: through it at half speed -- takes come walking over it (~0.8 units); standing still on it never took and read as stuck) */ tx = cosf(rel) * sp; ty = sinf(rel) * sp; out |= 2; }
    else if (arbw == ARB_FIGHT) { tx = ftx; ty = fty; out |= 2; }   // (the fighter's moves: the arbiter gave it the legs)
    else if (arbw == ARB_HOLD) {   // (levels apart: the enemy below -- hold the high ground)
        // (06:47: "[still] 73% of 30 s alive under 0.3 u/s: ... hold 58%" -- a statue on the ledge. The user, who won 3-0 from above, moved
        //  ~0.5 u/s there: the walker now sways side to side at NHOOK_BOT_HOLD_SWAY (0.3, live; 0 = old) throttle, turning back every
        //  0.9-1.3 s or where the floor 1 unit to that side is not its own level (never off the ledge it holds))
        tx = 0; ty = 0; out |= 2; float hs = 0.3f; lk_over(&hs, "NHOOK_BOT_HOLD_SWAY");
        if (hs > 0 && g_nav.ok) { static double tsw; static float sd = 1; if (now - tsw > 0.9 + 0.4 * bot_rand01()) { tsw = now; sd = -sd; }
            float me3[3] = {o->c[0], o->c[1], o->c[2] - 0.15f}; int mn = nav_node_at(me3); float mz[3] = {0, 0, o->c[2] - 0.35f}; if (mn >= 0) nav_pos(mn, mz);
            for (int tries = 0; tries < 2; tries++) { float side = yaw + sd * 1.5707963f, q[3] = {o->c[0] + cosf(side), o->c[1] + sinf(side), mz[2] + 0.2f}; int qn = nav_node_at(q); float qp[3];
                if (qn >= 0) { nav_pos(qn, qp); if ((qp[0] - q[0]) * (qp[0] - q[0]) + (qp[1] - q[1]) * (qp[1] - q[1]) < 0.36f && fabsf(qp[2] - mz[2]) < 0.5f) { ty = sd * hs; break; } }
                sd = -sd; tsw = now; } } }
    if (now < g_escape_until && arbw != ARB_FIGHT && arbw != ARB_HOLD) { float rel = bot_wrap(g_escape_dir - yaw); tx = cosf(rel); ty = sinf(rel); out |= 2; }   // (wedged: the escape beats the path moves -- never a fight's)
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
      // (10:50: NHOOK_BOT_HOSE is a live knob too -- 1 = the trigger stays held at every range, the walker settings page's "aggressive")
      { float hk = (float)hose; lk_over(&hk, "NHOOK_BOT_HOSE"); hose = hk > 0; }
      int burst_ok = 1;
      if (!hose && !g_bot_pull && g_inv.held_c && !strcmp(g_inv.held_c, "auto") && tdist >= 8.0f) {
          float duty = tdist < 15 ? 0.34f : tdist < 25 ? 0.22f : 0.15f, per = 0.25f / duty;
          burst_ok = fmod(now - g_bot_fire_t0, per) < 0.25; }
      // (10:50: NHOOK_BOT_PULL_PER, live: the pull weapons' trigger period, down for half of it -- 0.24 s = ~4 pulls a second; at least
      //  0.1 s, the bot steps every 33 ms. With the ball: melee only)
      float pper = 0.24f; lk_over(&pper, "NHOOK_BOT_PULL_PER"); if (pper < 0.1f) pper = 0.1f;
      int trig = !nofire && spar && g_bot_fire && burst_ok && (!g_bot_pull || fmod(now - g_bot_fire_t0, pper) < pper * 0.5) && !ob_carry; g_bot_trig = trig;
      { static double tfg; if (g_bot_fire && trig) g_fg_trig++;
        if (now - tfg > 20.0) { int nv = g_fg_vis > 0 ? g_fg_vis : 1;
            if (tfg > 0 && g_fg_n > 30 && bot_log()) nlog("[nhook] [firegate] %d steps with a target in its weapon's reach: hidden %.0f%% (no shot possible); of the %d it could see: fire allowed %.0f%%, aim line not through it (turning / off) %.0f%%, reaction wait %.0f%%, camo %.0f%%, head gate / other %.0f%% -- trigger down in %.0f%% of the allowed (held %s)",
              g_fg_n, 100.0 * g_fg_hid / g_fg_n, g_fg_vis, 100.0 * g_fg_ok / nv, 100.0 * g_fg_aim / nv, 100.0 * g_fg_react / nv, 100.0 * g_fg_camo / nv, 100.0 * g_fg_head / nv, g_fg_ok ? 100.0 * g_fg_trig / g_fg_ok : 0.0, g_inv.held ? g_inv.held : "-");
          tfg = now; g_fg_n = g_fg_hid = g_fg_react = g_fg_aim = g_fg_camo = g_fg_head = g_fg_ok = g_fg_trig = g_fg_vis = 0; } }
      // (13:25: the SPARTAN LASER fires after its charge, and every release starts the charge again -- the aim gate flickered the trigger:
      //  a death holding it at 8 units. With it in hand and the target in sight within 40 units the trigger stays down)
      if (!nofire && spar && have_tg && tdist < 40 && g_inv.held && strstr(g_inv.held, "SPARTAN LASER") && los_get(ftg.handle, now) == 1) { trig = 1; g_bot_trig = 1; }
      { static double tobm; if (ob_carry && have_tg && tdist < 1.8f && now - tobm > 0.6) { tobm = now; g_bot_melee_until = now + 0.12; } }
      // (14:05: the trigger is let go around a switch press -- "switching from PLASMA PISTOL to ASSAULT RIFLE" 32 times with the trigger
      //  held, none took; the switches that took came with the trigger up)
      if (g_swbit >= 0 && now < g_swrec_until + 0.15) { trig = 0; g_bot_trig = 0; }
      uint64_t want = (now < g_bot_click_until && !(g_swbit >= 0 && now < g_swrec_until + 0.15) ? fb : 0) | g_swscan_bits | (g_swbit >= 0 && now < g_swrec_until ? (1ull << g_swbit) : 0) | (now < g_bot_melee_until ? bot_meleebits() : 0) | (trig ? fb : 0) | ((fbtn & 0x10000) ? gb : 0) | ((fbtn & 0x20000) ? pkb : 0) | (now < g_fjump_until ? g_bot_jump : 0);   // (fbtn bit 16: the grenade pulse, 17: pick up)
      if (want) { g_bot_btn = ((out & 4) ? g_bot_btn : 0) | want; out |= 4; } else if (!(out & 4)) g_bot_btn = 0; }
    if (diag) { uint64_t b = 0; int d = bot_diag(&tx, &ty, &b, now); out |= d; if (d & 4) g_bot_btn = b; else if (!(out & 4)) g_bot_btn = 0;
        static double tl; if (now - tl > 0.25) { tl = now; const float* v = o->vel;
            nlog("[nhook] [bot] test: pos (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) yaw %.1f throttle (%.2f, %.2f) buttons 0x%llx; shots %u deletes %lu", o->c[0], o->c[1], o->c[2], v[0], v[1], v[2],
                 yaw0 * 57.2958f, tx, ty, (d & 4) ? (unsigned long long)b : 0ull, g_nov_shot_n, g_nov_del_calls); } }
    { static int fo = -1; static float fy, fp; static int hp; if (fo < 0) { const char* e = getenv("NHOOK_BOT_FACE"); int k = e ? sscanf(e, "%f,%f", &fy, &fp) : 0; fo = k >= 1; hp = k == 2; }
      if (fo) { yaw = fy / 57.29578f; if (hp) pitch = fp / 57.29578f; out |= 1; } }   // NHOOK_BOT_FACE (above)
    if (g_bot_shadow) { g_sh_yaw = yaw; g_sh_pitch = pitch; g_sh_aim_ok = 1; shadow_log(now, o, have_tg ? &ftg : 0, tdist, engaged, yaw, pitch, tx, ty, yaw0, pitch0, rec); g_bot_out = 0; g_bot_btn = 0; return; }   // (decided, never published)
    // (07:00: the step trace -- the server saw walkers stand 15-50% of their time near enemies while their own [still] said ~10%, and
    //  "stuck" fired every ~1.5 s of path walking: moving or not? NHOOK_BOT_STEPLOG=<s> logs every 0.1 s for that many seconds of
    //  gameplay: position, velocity, view / move yaw, throttle, the legs)
    { static float sl = -1; if (sl < 0) { const char* e = getenv("NHOOK_BOT_STEPLOG"); sl = e ? strtof(e, 0) : 0; } static double tsl;
      if (sl > 0 && g_gameplay_t0 > 0 && now - g_gameplay_t0 < sl && now - tsl >= 0.1) { tsl = now;
          nlog("[step] t %.2f pos %.2f %.2f %.2f vel %.2f %.2f yaw %.0f->%.0f thr %.2f %.2f out %d legs %s fight %d", now - g_gameplay_t0, o->c[0], o->c[1], o->c[2],
               o->vel[0], o->vel[1], yaw0 * 57.2958f, yaw * 57.2958f, tx, ty, out, g_arb_why, (int)g_bot_fighting); } }
    // STUCK TOO LONG (2026-11-09 17:20, the user: "if stuck too long look straight down and throw grenades and intentionally stand on
    //  them" -- on Hugegrass the walker spent 6+ minutes alive in a spawn spot no path led out of): alive, not fighting for 15 s, and every
    //  position of the last NHOOK_BOT_UNSTICK_S (45 s, live; 0 = off) within 6 units of where that stretch began -> look straight down,
    //  stand still, a grenade at its feet every 1.5 s (the left trigger) until it dies and respawns somewhere else. Never with a
    //  teammate within 10 units (friendly fire: a betrayal) or without a grenade
    { static double tfight, tsam, tdo, tsay; static float ring[128][3]; static double rtm[128]; static int rn;
      float us = 45; lk_over(&us, "NHOOK_BOT_UNSTICK_S");
      int alive_me = me >= 0 && !objs[me].dead;
      if (!alive_me) rn = 0;
      static double alive_t0; if (!alive_me) alive_t0 = 0; else if (alive_t0 <= 0) { alive_t0 = now; g_nav_ok_t = now; }
      if ((engaged || g_bot_fighting) && tdist < 40.0f) tfight = now;
      if (alive_me && now - tsam >= 1.0) { tsam = now; memcpy(ring[rn & 127], o->c, 12); rtm[rn & 127] = now; rn++; }
      int stuck = 0;
      if (us > 0 && alive_me && now - tfight > 15.0 && rn > (int)us + 1 && (int)us < 120) {
          int i0 = rn - (int)us; float* a0 = ring[i0 & 127]; stuck = 1;
          for (int i = i0; i < rn; i++) { float dx = ring[i & 127][0] - a0[0], dy = ring[i & 127][1] - a0[1]; if (dx * dx + dy * dy > 36.0f) { stuck = 0; break; } } }
      // (17:35: on the Hugegrass island the escape hopped it back and forth over 6 units, so it never counted as standing still: no path
      //  found to anywhere for the same time counts as stuck too)
      if (now - g_inf_hold_t < 3.0) stuck = 0;
      if (!stuck && us > 0 && alive_me && g_nav.ok && now - g_inf_hold_t > 3.0 && now - tfight > 15.0 && alive_t0 > 0 && now - alive_t0 > us && now - g_nav_ok_t > us) stuck = 1;   /* (only with a walking graph: none = no plan at all, not stuck) */
      if (stuck) { int mate = 0; for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || bot_enemy(o, t)) continue;
              float dx = t->c[0] - o->c[0], dy = t->c[1] - o->c[1], dz = t->c[2] - o->c[2]; if (dx * dx + dy * dy + dz * dz < 100.0f) { mate = 1; break; } }
          int nades = g_inv.frag + g_inv.plasma > 0 || g_inv.gren > 0;
          if (!mate && nades) { pitch = -1.45f; out |= 1 | 2; tx = 0; ty = 0; g_bot_trig = 0;
              if (now - tdo > 1.5) tdo = now;
              if (now - tdo < 0.15) fbtn |= 0x10000;
              if (now - tsay > 20.0 && bot_log()) { tsay = now; nlog("[nhook] [unstick] stuck %.0f s within 6 units of (%.1f, %.1f): looking straight down, a grenade at my feet -- a respawn elsewhere", us, o->c[0], o->c[1]); } }
          else if (now - tsay > 20.0 && bot_log()) { tsay = now; nlog("[nhook] [unstick] stuck %.0f s at (%.1f, %.1f) but %s: staying", us, o->c[0], o->c[1], mate ? "a teammate is within 10 units" : "no grenade"); } } }
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
#define NHIO_VERSION 3   // (2: my_kills / my_deaths / rtt_ms -- the scoreboard and the ping; 3: Infection's sides, 2026-11-09)
typedef struct {
    int version;
    // body -> brain
    uint32_t my_unit, follow_unit, human_unit[8]; int human_n; char srv_addr[96], nick[64], st_curmap[64], trk_mode[48], veh[64];
    double gameplay_t0; int rd_mode, rd_range, vpad_on, spar, nav_mode_rt, ht_my_slot, shadow; float pt[3]; uint64_t jump;
    uint64_t testbtn; double testbtn_until; uint32_t testpad; double testpad_until;
    int los_done, los_res; uint32_t los_res_tgt; double los_t; uint64_t inp_b8_acc;
    int my_kills, my_deaths; float rtt_ms;
    int sides_on, nside; uint32_t side_unit[128]; int8_t side_val[128];   // (Infection: zombie 1 / human 0 per unit -- the brain's friend or enemy)
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
    io->my_kills = io->my_deaths = -1; io->rtt_ms = g_net_rtt_ms;
    { int n = g_nov_nside; if (n > 128) n = 128; io->nside = n; io->sides_on = g_nov_sides_on; for (int i = 0; i < n; i++) { io->side_unit[i] = g_nov_side[i].unit; io->side_val[i] = g_nov_side[i].side; } }
    { int sl = g_ht_my_slot; if (g_rs_have && sl >= 0 && sl < 128) { io->my_kills = g_rs_cur[sl][RS_KILLS] & 0x7fff; io->my_deaths = g_rs_cur[sl][RS_DEATHS] & 0x7fff; } }
    io->los_req = g_los_req; }   // (the brain keeps counting from the body's)
static void io_in_apply(const NhIO* io) {   // (the brain) the body's state -> this copy's globals
    g_my_unit = io->my_unit; g_follow_unit = io->follow_unit; g_human_n = io->human_n; for (int i = 0; i < 8; i++) g_human_unit[i] = io->human_unit[i];
    memcpy((void*)g_srv_addr, io->srv_addr, sizeof io->srv_addr); memcpy(g_nick, io->nick, sizeof io->nick); memcpy(g_st_curmap, io->st_curmap, sizeof io->st_curmap);
    memcpy(g_trk_mode, io->trk_mode, sizeof io->trk_mode); memcpy(g_bot_veh, io->veh, sizeof io->veh); memcpy(g_bot_pt, io->pt, sizeof io->pt);
    g_gameplay_t0 = io->gameplay_t0; g_rd_mode = io->rd_mode; g_rd_range = io->rd_range; g_vpad_on = io->vpad_on; g_bot_spar = io->spar; g_nav_mode_rt = io->nav_mode_rt;
    g_ht_my_slot = io->ht_my_slot; g_bot_shadow = io->shadow; g_bot_jump = io->jump; g_bot_testbtn = io->testbtn; g_bot_testbtn_until = io->testbtn_until;
    g_bot_testpad = io->testpad; g_bot_testpad_until = io->testpad_until; g_los_done = io->los_done; g_los_res = io->los_res; g_los_res_tgt = io->los_res_tgt; g_los_t = io->los_t;
    g_inp_b8_acc = io->inp_b8_acc; g_los_req = io->los_req; g_my_kills = io->my_kills; g_my_deaths = io->my_deaths; g_net_rtt_ms = io->rtt_ms;
    { int n = io->nside > 128 ? 128 : io->nside; g_nov_nside = 0; for (int i = 0; i < n; i++) { g_nov_side[i].unit = io->side_unit[i]; g_nov_side[i].side = io->side_val[i]; } g_nov_nside = n; g_nov_sides_on = io->sides_on; } }
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
    { static int was_warm, noted; if (was_warm && !g_brain_warm) nlog("[nhook] [brain] code built %s %s took over (warm: the world, its maps and models already loaded)", __DATE__, __TIME__);
      // (2026-11-09, the user: "how come the #brain log doesn't indicate a new brain loaded any more?" -- the Brain tab read the build from
      //  the walker log's last 3 MB, which path lines filled): every brain that takes over writes one line to battles/follow/brain-<nick>.jsonl
      //  -- when, its build, warm or cold, its lesson -- the tab lists them
      if (!g_brain_warm && !noted && g_nick[0]) { noted = 1; char path[1100]; const char* h = getenv("HOME");
          snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/battles/follow/brain-%s.jsonl", h ? h : "/tmp", g_nick);
          FILE* f = fopen(path, "a");
          if (f) { const char* les = "";
#ifdef NH_BRAIN_LESSON
              les = NH_BRAIN_LESSON;
#endif
              fprintf(f, "{\"t\": %.0f, \"built\": \"%s %s\", \"warm\": %d, \"map\": \"", (double)time(0), __DATE__, __TIME__, was_warm);
              for (const char* c = g_st_curmap; *c; c++) if (*c != '"' && *c != '\\') fputc(*c, f);
              fputs("\", \"lesson\": \"", f);
              for (const char* c = les; *c; c++) { if (*c == '"' || *c == '\\') fputc('\\', f); if ((unsigned char)*c >= 0x20) fputc(*c, f); }
              fputs("\"}\n", f); fclose(f); } }
      was_warm = g_brain_warm; }
    g_nav_quiet = g_brain_warm;
    static int told; if (!told) { told = 1; nlog("[nhook] [brain] code built %s %s is %s", __DATE__, __TIME__, g_brain_warm ? "warming up beside the running brain" : "stepping");
#ifdef NH_BRAIN_LESSON
        if (NH_BRAIN_LESSON[0]) nlog("[nhook] [brain] lesson: %s", NH_BRAIN_LESSON);
#endif
    }
    io_in_apply(io);
    { static char hmmap[64]; if (strcmp(hmmap, g_st_curmap)) { snprintf(hmmap, sizeof hmmap, "%s", g_st_curmap);   // (the death map of the map being played: the
          pthread_mutex_lock(&g_hm_mx); hm_switch(g_st_curmap); pthread_mutex_unlock(&g_hm_mx);                     //  brain's own copy -- nav_danger_update avoids its hot spots)
          nlog("[nhook] [brain] death map for %s: %u deaths", g_st_curmap[0] ? g_st_curmap : "-", (unsigned)g_hm.nd); } }
    if (g_brain_warm) hand_adopt();
    bot_step(); io_out_pack(io);
    if (!g_brain_warm) hand_publish();
    return 1; }
// THE HOT TAKEOVER: the body steps a new brain here, on its own thread, beside the running one, until the new one's steps are cheap
// (its nav graph, death map, map tags, models loaded; its target, aim and path caught up with the world); then it swaps between
// two steps. Nothing this copy decides here leaves it (the body throws the outputs away; g_brain_warm silences logs and files)
__attribute__((visibility("default"))) int nh_brain_warm(NhIO* io) { g_brain_warm = 1; int r = nh_brain_step(io); g_brain_warm = 0; return r; }
// (2026-11-09, the user: "we are all laggy now. close everything, fix the memory leak": a hot swap left the old brain's memory behind --
//  its walking graph, lanes, the brain's copy of the death map, the learned links -- and the map files its hitbox reader OPENED, with their
//  tag tables (an open file per map per brain, never closed). The body calls this on the brain it replaced, 5 s after the takeover; that
//  brain never steps again)
__attribute__((visibility("default"))) void nh_brain_free(void) {
    free(g_nav.pop); g_nav.pop = 0; nav_free(); free(g_np_a); free(g_np_b); free(g_np_c); g_np_a = g_np_b = 0; g_np_c = 0; g_np_n = 0;
    pthread_mutex_lock(&g_hm_mx); free(g_hm.cells); free(g_hm.d); free(g_hm.sp); memset(&g_hm, 0, sizeof g_hm); pthread_mutex_unlock(&g_hm_mx);
    free(g_bm_heat); g_bm_heat = 0; g_bm_heat_n = 0; free(g_gll); g_gll = 0; g_ngll = 0;
    int nf = 0; pthread_mutex_lock(&g_hbm_mx);
    for (int i = 0; i < g_hbm_n; i++) { HbMap* m = &g_hbm[i]; if (m->fd >= 0) { close(m->fd); nf++; } free(m->tp); free(m->salt); free(m->grp); free(m->names); free(m->noff); memset(m, 0, sizeof *m); m->fd = -1; }
    g_hbm_n = 0; pthread_mutex_unlock(&g_hbm_mx);
    nlog("[nhook] [brain] the replaced brain (built %s %s) let go of its memory: walking graph, lanes, links, death map, %d map files closed", __DATE__, __TIME__, nf); }
static void* bot_thread(void* p) { (void)p; return 0; }   // (never started here: the body's thread calls nh_brain_step)
#else
typedef int (*NhBrainStep)(NhIO*);
static NhBrainStep g_brain; static int g_brain_n;
// THE HOT TAKEOVER (2026-11-09, the user: "brain updates should be seamless to the walker even if it is in the middle of an active
// firefight. it should never pause movement etc. the brain should always be ready for a hot takeover"). Before, the bot thread itself
// copied + dlopen'ed a new nbrain.dylib and stepped it at once: the new copy loaded its nav graph (91k nodes on Valhalla), the map's
// tags, models and death map inside its first steps on the walker's own thread -- the legs and aim held their last values meanwhile --
// and then started from nothing (a 31-unit fight went from FIRING at 0.8 deg to "hunting" 15 deg off). Now a separate thread (shim-brain-warm)
// copies and loads it, then steps it (nh_brain_warm: outputs thrown away, no logs / files) with the same inputs every 33 ms while
// the running brain keeps driving, until it has stepped NHOOK_BRAIN_WARM_STEPS (30, ~1 s) and its last 3 steps each took under 4 ms
// (everything loaded, its target / aim / path caught up), at most NHOOK_BRAIN_WARM_MAX (20 s); then the bot thread swaps it in
// between two steps. A brain without nh_brain_warm (older builds) is swapped in at once, as before. NHOOK_BRAIN_COLD=1 = the old way.
static NhBrainStep g_brain_next; static volatile int g_brain_busy; static char g_brain_cp[1200];
typedef void (*NhBrainFree)(void); static NhBrainFree g_brain_next_free, g_brain_cur_free, g_brain_old_free; static double g_brain_free_at;   // (the replaced brain's memory, 5 s after)
static double proc_footprint_mb(void) { task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vc) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1; }
static void* brain_warm_thread(void* arg) {   // (shim-brain-warm) load g_brain_cp, warm it, hand it to the bot thread
    (void)arg; pthread_setname_np("shim-brain-warm");
    void* h = dlopen(g_brain_cp, RTLD_NOW | RTLD_LOCAL);
    if (!h) { nlog("[nhook] [brain] %s not loaded: %s", g_brain_cp, dlerror()); g_brain_busy = 0; return 0; }
    int (*ver)(void) = (int (*)(void))dlsym(h, "nh_brain_version"); NhBrainStep f = (NhBrainStep)dlsym(h, "nh_brain_step"), w = (NhBrainStep)dlsym(h, "nh_brain_warm");
    g_brain_next_free = (NhBrainFree)dlsym(h, "nh_brain_free");
    if (!ver || !f || ver() != NHIO_VERSION) { nlog("[nhook] [brain] %s refused: layout %d, this body's %d", g_brain_cp, ver ? ver() : -1, NHIO_VERSION); g_brain_busy = 0; return 0; }
    static int cold = -1; if (cold < 0) cold = getenv("NHOOK_BRAIN_COLD") && strcmp(getenv("NHOOK_BRAIN_COLD"), "0");
    double t0 = mono_s(); int n = 0, fast = 0; double slow = 0;
    if (w && !cold) {
        int need = 30; const char* e = getenv("NHOOK_BRAIN_WARM_STEPS"); if (e) need = atoi(e);
        double mx = 20; e = getenv("NHOOK_BRAIN_WARM_MAX"); if (e) mx = atof(e);
        while (mono_s() - t0 < mx) {
            NhIO io; memset(&io, 0, sizeof io); io_in_pack(&io);
            double a = mono_s(); w(&io); double dt = mono_s() - a; n++; if (dt > slow) slow = dt;
            fast = dt < 0.004 ? fast + 1 : 0;
            if (n >= need && fast >= 3) break;
            usleep(33000); } }
    nlog("[nhook] [brain] #%d ready: %s (%d warm-up steps in %.1f s beside the running brain, the slowest %.0f ms) -- taking over at the next step",
         g_brain_n + 1, w && !cold ? "warm" : "cold (no warm-up)", n, mono_s() - t0, slow * 1000);
    __atomic_store_n(&g_brain_next, f, __ATOMIC_RELEASE); return 0; }
static void brain_tick(void) {   // (the body, every 2 s) a newer nbrain.dylib -> a fresh copy, warmed beside the running brain (above)
    static double t; double now = mono_s(); if (now - t < 2.0) return; t = now;
    static int off = -1; if (off < 0) off = getenv("NHOOK_NO_BRAIN") != 0; if (off) return;
    if (g_brain_busy) return;
    static char path[1100]; if (!path[0]) { const char* e = getenv("NHOOK_BRAIN"); Dl_info di;
        if (e && *e) snprintf(path, sizeof path, "%s", e);
        else if (dladdr((void*)brain_tick, &di) && di.dli_fname) { snprintf(path, sizeof path, "%s", di.dli_fname); char* sl = strrchr(path, '/'); if (sl) snprintf(sl + 1, sizeof path - (size_t)(sl + 1 - path), "nbrain.dylib"); }
        else return; }
    struct stat st; if (stat(path, &st)) return; static struct timespec mt; static off_t sz;
    if (st.st_mtimespec.tv_sec == mt.tv_sec && st.st_mtimespec.tv_nsec == mt.tv_nsec && st.st_size == sz) return;
    mt = st.st_mtimespec; sz = st.st_size;
    static int nload; nload++;
    snprintf(g_brain_cp, sizeof g_brain_cp, "%s/nbrain-%d-%d.dylib", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid(), nload);
    { int in = open(path, O_RDONLY), outf = open(g_brain_cp, O_WRONLY | O_CREAT | O_TRUNC, 0755); char b[65536]; ssize_t r; int ok = in >= 0 && outf >= 0;
      while (ok && (r = read(in, b, sizeof b)) > 0) if (write(outf, b, (size_t)r) != r) ok = 0;
      if (in >= 0) close(in); if (outf >= 0) close(outf); if (!ok) { nlog("[nhook] [brain] could not copy %s", path); return; } }
    g_brain_busy = 1; pthread_t th;
    if (pthread_create(&th, 0, brain_warm_thread, 0)) { g_brain_busy = 0; return; }
    pthread_detach(th); nlog("[nhook] [brain] a new build in %s: warming it up beside the running brain", path); }
static void* bot_thread(void* p) { (void)p; pthread_setname_np("shim-bot");
    for (;;) { usleep(33000); brain_tick();
        NhBrainStep nx = __atomic_exchange_n(&g_brain_next, (NhBrainStep)0, __ATOMIC_ACQ_REL);
        if (nx) { if (g_brain_old_free) { NhBrainFree fr = g_brain_old_free; g_brain_old_free = 0; fr(); }   /* (a brain two swaps back: free it now) */
            if (g_brain_cur_free) { g_brain_old_free = g_brain_cur_free; g_brain_free_at = mono_s() + 5.0; }
            g_brain = nx; g_brain_cur_free = g_brain_next_free; g_brain_n++; g_brain_busy = 0;
            nlog("[nhook] [brain] #%d took over at this step (the old one stepped until the last); process footprint %.0f MB", g_brain_n, proc_footprint_mb()); }
        if (g_brain_old_free && mono_s() > g_brain_free_at) { NhBrainFree fr = g_brain_old_free; g_brain_old_free = 0; double a = proc_footprint_mb(); fr();
            nlog("[nhook] [brain] footprint %.0f -> %.0f MB after the replaced brain let go", a, proc_footprint_mb()); }
        if (g_brain) { NhIO io; memset(&io, 0, sizeof io); io_in_pack(&io); if (g_brain(&io)) io_out_apply(&io); else { bot_step(); hand_publish(); } }
        else { bot_step(); hand_publish(); } }
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
