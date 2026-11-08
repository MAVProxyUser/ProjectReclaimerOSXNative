// Hooks check (2026-11-08; the user, on client 0.10.1: "can you add a hooks check? like all the stuff you patch I assume you
// can detect IF on a new version the hook loaded properly or function. that could be an interesting self test almost").
// Included early by nhook.c (one compilation unit with nstats.c, nnet.c, nhost.c, nbot.c, noverlay.c; nrender.m reaches it
// through the exported nh_hk_* functions at the end).
//
// Every thing nhook depends on in the app registers here: the app build's address row (napp_builds.h), the code table, each
// hook (entry / body / text hooks), each interposed libc / AudioToolbox function, each swizzled Metal method, each fixed app
// address (the chat_log gate, set_muted), the app log lines the parsers need and the memory readers (players table, statborg,
// game results, session). Per item: the feature it serves, WHEN it is expected to fire (so a silent one can be judged), its
// state (installed / found, waiting for first use, off by a switch, FAILED + why) and how often it fired, the last time.
// Fire counts of hooks come from a trampoline in front of the handler (hk_wrap), not from the handlers themselves; everything
// else calls hk_fire(i) where it does its work. GET /hooks.json, the stats page's Hooks window, a [hooks] summary in the log
// (90 s after the engine started and at every match start), and shim/tools/native-hooks-test.sh (a local match that exercises
// them: PASS / FAIL per item). NHOOK_NO_HKWRAP=1 = hooks without the counting trampoline (fires then stay 0).
typedef struct { const char* name, * feature, * kind, * when; volatile int state; char why[200]; uint64_t at;
                 volatile uint64_t fires, last; void* h; } HkRec;
enum { HK_PENDING = 0, HK_OK = 1, HK_LAZY = 2, HK_OFF = 3, HK_FAIL = -1 };
// when: always (from the app's start) / engine (the engine image is up) / match (any local or online game) / shots (someone
// shoots) / online (a joined server) / voice (someone talks) / chat (a chat line) / mic (the app records) / overlay (an overlay
// feature is on) / menu (the app's menus opened) / bot (sparring aim or walker on) / host (a page host tool used) / setup (called once at start-up: installed
// is enough) / never (diagnostic: not expected natively)
#define HK_MAX 112
static HkRec g_hk[HK_MAX]; static volatile int g_nhk; static pthread_mutex_t g_hk_mx = PTHREAD_MUTEX_INITIALIZER;
static inline uint64_t hk_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
// the item's index (made on first use; the same name = the same item)
static int hk_id(const char* name, const char* feature, const char* kind, const char* when) {
    int n = __atomic_load_n(&g_nhk, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (!strcmp(g_hk[i].name, name)) return i;
    pthread_mutex_lock(&g_hk_mx); int i;
    for (i = 0; i < g_nhk; i++) if (!strcmp(g_hk[i].name, name)) break;
    if (i == g_nhk && i < HK_MAX) { HkRec* r = &g_hk[i]; r->name = strdup(name); r->feature = feature ? feature : ""; r->kind = kind ? kind : "";
        r->when = when ? when : "always"; __atomic_store_n(&g_nhk, i + 1, __ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_hk_mx); return i < HK_MAX ? i : -1; }
static void hk_state(int i, int st, uint64_t at, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void hk_state(int i, int st, uint64_t at, const char* fmt, ...) {
    if (i < 0) return; HkRec* r = &g_hk[i]; if (at) r->at = at;
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(r->why, sizeof r->why, fmt, ap); va_end(ap); }
    __atomic_store_n(&r->state, st, __ATOMIC_RELEASE); }
static inline void hk_fire(int i) { if (i < 0) return; __atomic_fetch_add(&g_hk[i].fires, 1, __ATOMIC_RELAXED); g_hk[i].last = hk_ticks(); }
// by name (a hook's install path knows its name; its feature / when come from the catalogue below)
typedef struct { const char* name, * feature, * kind, * when; } HkCat;
static const HkCat g_hk_cat[] = {
    // addresses and tables
    {"app build", "the per-build addresses below (napp_builds.h, re-tools/machoport.py)", "address", "always"},
    {"guest image", "every halo3 read (the app's '[host] linked recompiled engine, image at' line)", "address", "engine"},
    {"code table", "every halo3 hook: the guest -> host code table", "address", "engine"},
    {"halo3.dll image check", "the engine image is MCC's halo3.dll (section bytes compared)", "address", "engine"},
    // hooks
    {"engine_clock", "the engine thread's context: players, stats page, overlay, everything engine-side", "entry hook", "match"},
    {"display_event", "halo3's event display (the app never calls it: kills come from the statborg / game results)", "text hook", "never"},
    {"object_delete", "shots, hits, accuracy, tracers", "text hook", "shots"},
    {"opus decode (voice)", "forensic voice audio + the sender's channel (the app's voice mixer)", "text hook", "voice"},
    {"view constants", "the overlay's camera (installed when an overlay feature is first on)", "text hook", "overlay"},
    {"player control (sparring aim / walker)", "sparring aim, walker (test clients)", "text hook", "bot"},
    {"game tick (host tools)", "teleports / host requests on a server", "body hook", "host"},
    {"frame (host tools)", "teleports / host requests on a client (the app's per-frame function)", "text hook", "host"},
    {"kill volume test", "the Boundaries switch", "text hook", "host"},
    {"soft ceiling test", "the Boundaries switch", "text hook", "host"},
    {"shape builder (invisible walls)", "the Invisible walls switch", "body hook", "host"},
    // fixed app addresses / data
    {"chat_log gate", "chat lines in the feed and the forensic archive (the app's chat_log byte)", "app data", "chat"},
    {"set_muted", "the page's mute buttons (the app's own mute list)", "app code", "always"},
    {"voice mixer", "the voice hook's caller check + sender id / channel offsets", "app code", "voice"},
    // interposed functions
    {"log tap (write)", "every app log line the parsers below read", "interpose", "always"},
    {"sockets (send / receive)", "the packet log, peer addresses, the Packets window", "interpose", "online"},
    {"AudioUnitRender (mic)", "the page's mic switch (the app's capture, bus 1)", "interpose", "mic"},
    {"soundboard microphone", "the soundboard: a virtual microphone the app opens on a Mac without one (nmic.c), its samples", "app audio", "online"},
    {"AudioUnitSetProperty", "the app's audio units (voice volume probe)", "interpose", "always"},
    // swizzled Metal / AppKit
    {"presentDrawable", "overlay drawing, fps line, recording, Capture frame", "swizzle", "engine"},
    {"render target (fbo)", "the frame's colour target for the overlay / recording (set once with the window, often before the swizzle)", "swizzle", "setup"},
    {"depth attachment", "scene depth for zones / visible only", "swizzle", "engine"},
    // app log lines (nstats.c nst_on_line)
    {"line: [switch] game", "match state (Playing / Idle)", "log line", "match"},
    {"line: [loading] map", "the map name, map changes", "log line", "match"},
    {"line: [capacity] statborg", "kills / deaths / assists (where the app moved the statborg)", "log line", "engine"},
    {"line: [c1] game results", "exact kill feed, medals (halo3's game results)", "log line", "engine"},
    {"line: [c1] network sessions", "the Connected list (where the app moved the network session)", "log line", "engine"},
    {"line: [online] joining", "the joined server's address (host tools forward, bot's private-server check)", "log line", "online"},
    {"line: [online] peer", "who is at which address (Packets, look up names)", "log line", "online"},
    {"line: [voice] active", "who talks (page dot, forensic timing)", "log line", "voice"},
    {"line: [chat] line", "chat in the feed (needs the chat_log gate)", "log line", "chat"},
    {"line: [engine] HUD notice", "join / leave notices (roster)", "log line", "online"},
    {"line: [menus] open", "the app's menus are up (a --map test launch never opens them)", "log line", "menu"},
    // memory readers
    {"players table", "players, names, teams, positions (halo3 TLS +0x10)", "reader", "match"},
    {"statborg", "kills / deaths / assists per player", "reader", "match"},
    {"game results", "exact kill feed, per-weapon kills, medals", "reader", "match"},
    {"network session", "the Connected list (halo3 session)", "reader", "online"},
    {"observer camera", "the overlay's fallback camera (halo3 observer TLS +0x578; read only while the overlay is on)", "reader", "overlay"},
};
static const HkCat* hk_cat(const char* name) { for (size_t k = 0; k < sizeof g_hk_cat / sizeof g_hk_cat[0]; k++) if (!strcmp(g_hk_cat[k].name, name)) return &g_hk_cat[k]; return 0; }
static int hk_named(const char* name) { const HkCat* c = hk_cat(name); return hk_id(name, c ? c->feature : "", c ? c->kind : "", c ? c->when : "always"); }
static void hk_init(void) { for (size_t k = 0; k < sizeof g_hk_cat / sizeof g_hk_cat[0]; k++) hk_id(g_hk_cat[k].name, g_hk_cat[k].feature, g_hk_cat[k].kind, g_hk_cat[k].when); }
// cached ids for hot paths (set by hk_init's order: looked up once)
#define HK_ONCE(var, name) static int var = -2; if (var == -2) var = hk_named(name)
// ---- the counting trampolines: hk_wrap(name, h) -> a function that counts, then tail-calls h with x0..x7 unchanged ----------
typedef uint64_t (*hk_fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void* volatile g_hkt_h[48]; static int g_hkt_i[48]; static int g_nhkt;
#define HKT(n) static uint64_t hkt##n(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) { \
    hk_fire(g_hkt_i[n]); return ((hk_fn8)g_hkt_h[n])(a, b, c, d, e, f, g, h); }
HKT(0) HKT(1) HKT(2) HKT(3) HKT(4) HKT(5) HKT(6) HKT(7) HKT(8) HKT(9) HKT(10) HKT(11) HKT(12) HKT(13) HKT(14) HKT(15)
HKT(16) HKT(17) HKT(18) HKT(19) HKT(20) HKT(21) HKT(22) HKT(23) HKT(24) HKT(25) HKT(26) HKT(27) HKT(28) HKT(29) HKT(30) HKT(31)
HKT(32) HKT(33) HKT(34) HKT(35) HKT(36) HKT(37) HKT(38) HKT(39) HKT(40) HKT(41) HKT(42) HKT(43) HKT(44) HKT(45) HKT(46) HKT(47)
static void* const g_hkt[48] = {hkt0, hkt1, hkt2, hkt3, hkt4, hkt5, hkt6, hkt7, hkt8, hkt9, hkt10, hkt11, hkt12, hkt13, hkt14, hkt15,
    hkt16, hkt17, hkt18, hkt19, hkt20, hkt21, hkt22, hkt23, hkt24, hkt25, hkt26, hkt27, hkt28, hkt29, hkt30, hkt31,
    hkt32, hkt33, hkt34, hkt35, hkt36, hkt37, hkt38, hkt39, hkt40, hkt41, hkt42, hkt43, hkt44, hkt45, hkt46, hkt47};
// (handlers reached from stubs that call them as C functions -- not nh_opus_shim, which reads the stub's stack: no wrap)
static void* hk_wrap(int id, void* h) {
    static int off = -1; if (off < 0) off = getenv("NHOOK_NO_HKWRAP") != 0;
    if (off || id < 0) return h;
    pthread_mutex_lock(&g_hk_mx); int k = g_nhkt < 48 ? g_nhkt++ : -1; pthread_mutex_unlock(&g_hk_mx);
    if (k < 0) return h;
    g_hkt_i[k] = id; g_hkt_h[k] = h; __atomic_thread_fence(__ATOMIC_SEQ_CST); g_hk[id].h = h; return g_hkt[k]; }
static const char* hk_state_name(int s) { return s == HK_OK ? "ok" : s == HK_LAZY ? "on first use" : s == HK_OFF ? "off" : s == HK_FAIL ? "FAILED" : "not yet"; }
// the verdict for one item: ok (installed / found and, if its "when" happened, fired), silent (installed, its "when" happened,
// never fired), failed, off, waiting (its "when" has not come yet)
static double hk_freq(void) { static double f; if (!f) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); f = v ? (double)v : 24e6; } return f; }
// exported for nrender.m (another compilation unit)
__attribute__((visibility("default"))) int nh_hk_named(const char* name) { return hk_named(name); }
__attribute__((visibility("default"))) void nh_hk_fire(int i) { hk_fire(i); }
__attribute__((visibility("default"))) void nh_hk_state(int i, int st, uint64_t at, const char* why) { hk_state(i, st, at, "%s", why ? why : ""); }
