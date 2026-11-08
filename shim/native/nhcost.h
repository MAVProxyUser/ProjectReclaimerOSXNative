// Hook cost meter (2026-11-07; user: "a *hook cost* meter for FPS: when a hook is enabled it should calculate any performance
// loss that occurs as a factor of it being enabled"). Included early by nhook.c (one compilation unit with nstats.c,
// noverlay.c, nnet.c).
//
// Every hook and overlay part times itself with the CPU's own counter (CNTVCT_EL0: 24 MHz on Apple silicon, read in ~1 ns)
// and counts its calls (hc_add). The stats snapshot turns that into calls a second, time per call, time per frame and the
// fps it would cost IF its thread sets the frame rate (nstats.c hc_json: an estimate). The page's "measure" button gives
// the real figure: that one hook (or overlay feature) switched off and on in turns while frames are counted per phase
// (nstats.c hc_ab_*, driven from every presented frame by nrender.m).
//
// Off switches: g_hc_off[k] makes a hook's handler return at once (the A/B measurement and nothing else sets them). The
// camera hook's stub filters ~700k calls a second in assembly before its C handler: that filter is not timed and stays
// while the handler is off (NHOOK_NO_VIEWHOOK=1 at launch removes the hook).
enum { HC_DEL, HC_AIM, HC_VIEW, HC_OV, HC_OV_HIT, HC_OV_VEH, HC_OV_TRC, HC_OV_INF, HC_OV_ITM, HC_OV_ZON, HC_AIMT, HC_OPUS, HC_SOCK, HC_SNAP, HC_BOT, HC_MIC, HC_N };
typedef struct { const char* id, * name, * thread; int crit; volatile uint64_t ticks, calls; } HcSlot;
static HcSlot g_hc[HC_N] = {
    [HC_DEL]    = {"bullets", "Bullet hook (object_delete): shots, hits, tracer records", "simulation", 1, 0, 0},
    [HC_AIM]    = {"aim", "Aim meter: target and pose per bullet", "simulation", 1, 0, 0},
    [HC_VIEW]   = {"camera", "Camera hook: the overlay's view (C handler; its filter not timed)", "render", 1, 0, 0},
    [HC_OV]     = {"overlay", "Overlay: objects and camera (with any feature on)", "render (present)", 1, 0, 0},
    [HC_OV_HIT] = {"ov:hitboxes", "Overlay: player hitboxes", "render (present)", 1, 0, 0},
    [HC_OV_VEH] = {"ov:vehicles", "Overlay: vehicles", "render (present)", 1, 0, 0},
    [HC_OV_TRC] = {"ov:tracers", "Overlay: tracers", "render (present)", 1, 0, 0},
    [HC_OV_INF] = {"ov:info", "Overlay: info labels", "render (present)", 1, 0, 0},
    [HC_OV_ITM] = {"ov:items", "Overlay: items", "render (present)", 1, 0, 0},
    [HC_OV_ZON] = {"ov:zones", "Overlay: zones", "render (present)", 1, 0, 0},
    [HC_AIMT]   = {"aimtick", "Aim meter: hitbox measurement", "render (present)", 1, 0, 0},
    [HC_OPUS]   = {"voice", "Voice hook (Opus decode): forensic audio", "voice", 0, 0, 0},
    [HC_SOCK]   = {"packets", "Socket hook: packet log", "network", 0, 0, 0},
    [HC_SNAP]   = {"stats", "Stats snapshot (10 a second)", "its own", 0, 0, 0},
    [HC_BOT]    = {"bot", "Sparring aim / walker: the player's controls (test clients)", "simulation", 1, 0, 0},
    [HC_MIC]    = {"mic", "Microphone switch (the app's capture)", "audio", 0, 0, 0} };
static volatile uint8_t g_hc_off[HC_N];
static inline uint64_t hc_now(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline void hc_add(int k, uint64_t t0) { uint64_t d = hc_now() - t0; __atomic_fetch_add(&g_hc[k].ticks, d, __ATOMIC_RELAXED); __atomic_fetch_add(&g_hc[k].calls, 1, __ATOMIC_RELAXED); }
static inline void hc_add_ticks(int k, uint64_t d) { if (d) __atomic_fetch_add(&g_hc[k].ticks, d, __ATOMIC_RELAXED); }
static double hc_freq(void) { static double f; if (!f) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); f = v ? (double)v : 24e6; } return f; }
