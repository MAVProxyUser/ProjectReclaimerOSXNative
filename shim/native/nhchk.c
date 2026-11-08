// The hooks check's report (nhchk.h has the registry; included by nstats.c after nbot.c): preflight of the hooks that install
// on first use, the verdict per item, GET /hooks.json, the snapshot's "hooks" summary, the [hooks] log lines.
//
// Verdicts: ok (installed / found, and it fired -- or it is not a thing that fires), FAILED (could not be installed / found:
// why says what was expected), silent (installed, the thing it waits for happened, no call yet), waiting (what it waits for
// has not happened in this run), off (a switch turned it off), not expected (a diagnostic hook that the app never calls).

// ---- preflight: would a first-use hook install? (the installers' own shape checks; nothing is written)
static void hk_text_preflight(const char* name, uint64_t f, const char* what) {
    int id = hk_named(name); if (id < 0 || g_hk[id].state == HK_OK) return; uint32_t w[4];
    if (!f) { hk_state(id, HK_FAIL, 0, "%s is not the app's own code in this build (no `b` in its code-table entry)", what); return; }
    if (rd(f, w, 16)) { hk_state(id, HK_FAIL, f, "%s at 0x%llx is unreadable", what, (unsigned long long)f); return; }
    for (int i = 0; i < 4; i++) if (insn_pcrel(w[i])) { hk_state(id, HK_FAIL, f, "%s at 0x%llx: instruction %d (%08x) is PC-relative", what, (unsigned long long)f, i, w[i]); return; }
    hk_state(id, HK_LAZY, f, "installs on first use (%s at 0x%llx checks out)", what, (unsigned long long)f); }
static void hk_body_preflight(const char* name, uint32_t rva) {
    int id = hk_named(name); if (id < 0 || g_hk[id].state == HK_OK) return; uint64_t b = body_of(rva);
    if (b) hk_state(id, HK_LAZY, b, "installs on first use (halo3+0x%x's lifted body at 0x%llx checks out)", rva, (unsigned long long)b);
    else hk_state(id, HK_FAIL, 0, "halo3+0x%x has no lifted body of the known shape in this build", rva); }
static void hk_preflight(void) {
    if (getenv("NHOOK_NO_VIEWHOOK")) hk_state(hk_named("view constants"), HK_OFF, 0, "NHOOK_NO_VIEWHOOK");
    else hk_text_preflight("view constants", branch_target_of(0x2af478), "halo3+0x2af478 (set shader parameter)");
    hk_text_preflight("player control (sparring aim / walker)", branch_target_of(0xf7178), "halo3+0xf7178 (the player-control reader)");
    hk_body_preflight("game tick (host tools)", 0xeefa8);
    { int id = hk_named("frame (host tools)"); uint64_t f = APP(frame_fn); uint32_t w[4];
      if (g_hk[id].state != HK_OK) { if (!f) hk_state(id, HK_FAIL, 0, "frame_fn is not in this build's table");
          else if (rd(f, w, 16) || memcmp(w, g_ht_frame_words, 16)) hk_state(id, HK_FAIL, f, "0x%llx does not start sub sp, sp, #0x40 ; stp x22, x21 ; stp x20, x19 ; stp x29, x30", (unsigned long long)f);
          else hk_text_preflight("frame (host tools)", f, "the app's per-frame function"); } }
    hk_text_preflight("kill volume test", branch_target_of(0x1b8e00), "halo3+0x1b8e00 (kill volume / safe zone test)");
    hk_text_preflight("soft ceiling test", branch_target_of(0x1bac00), "halo3+0x1bac00 (soft ceiling test)");
    hk_body_preflight("shape builder (invisible walls)", 0x21b4d8);
    // the interposes are bound by dyld at load (the __interpose section): installed if this code runs at all
    static const char* const ip[] = {"log tap (write)", "sockets (send / receive)", "AudioUnitRender (mic)", "AudioUnitSetProperty"};
    for (size_t k = 0; k < sizeof ip / sizeof ip[0]; k++) { int id = hk_named(ip[k]); if (g_hk[id].state == HK_PENDING) hk_state(id, HK_OK, 0, "interposed by dyld (__DATA,__interpose)"); }
    // log lines and readers have no install step: "ok" once they fire
}

// ---- what each item waits for, as far as this run knows
static volatile double g_hk_match_s;          // seconds spent in matches (st_snapshot, 10 a second)
static int hk_when_happened(const HkRec* r) {
    const char* w = r->when;
    if (!strcmp(w, "always")) return 1;
    if (!strcmp(w, "engine")) return g_img != 0;
    if (!strcmp(w, "match")) return g_hk_match_s >= 15;
    if (!strcmp(w, "shots")) return g_hk_match_s >= 120;          // (a public match has shots in two minutes; a test fires on purpose)
    if (!strcmp(w, "online")) return g_srv_addr[0] && g_hk_match_s >= 15;
    if (!strcmp(w, "voice")) { HK_ONCE(v, "line: [voice] active"); return v >= 0 && g_hk[v].fires > 0; }   // the app itself reported a talker
    if (!strcmp(w, "overlay")) return g_view_wanted != 0;
    if (!strcmp(w, "bot")) return g_bot_out != 0;
    if (!strcmp(w, "host")) return g_ht_tick_ok > 0 || g_ht_bnd_ok > 0 || g_ht_inv_ok > 0;
    return 0;                                                       // chat / mic / never: only its own fires tell
}
static const char* hk_verdict(const HkRec* r) {
    int st = r->state;
    if (st == HK_FAIL) return "FAILED";
    if (st == HK_OFF) return "off";
    if (!strcmp(r->when, "never")) return r->fires ? "ok" : "not expected";
    if (!strcmp(r->when, "setup")) return st == HK_OK ? "ok" : "waiting";
    if (r->fires) return "ok";
    if (!strcmp(r->kind, "address") || !strcmp(r->kind, "app code") || !strcmp(r->kind, "app data")) return st == HK_OK ? "ok" : hk_when_happened(r) ? "silent" : "waiting";
    if (st == HK_LAZY) return hk_when_happened(r) ? "silent" : "waiting";
    return hk_when_happened(r) ? "silent" : "waiting";
}
static void hk_counts(int* ok, int* bad, int* silent, int* wait, int* off) {
    *ok = *bad = *silent = *wait = *off = 0; int n = __atomic_load_n(&g_nhk, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) { const char* v = hk_verdict(&g_hk[i]);
        if (!strcmp(v, "ok")) (*ok)++; else if (!strcmp(v, "FAILED")) (*bad)++; else if (!strcmp(v, "silent")) (*silent)++; else if (!strcmp(v, "off")) (*off)++; else (*wait)++; } }
static void hk_json(StBuf* s) {
    int ok, bad, si, wa, of; hk_counts(&ok, &bad, &si, &wa, &of); double now = (double)hk_ticks() / hk_freq();
    sb_put(s, "{\"app\":{\"version\":"); sb_str(s, g_app_ver); sb_put(s, ",\"uuid\":"); sb_str(s, g_app_uuid);
    sb_put(s, ",\"known\":%s,\"slide\":\"0x%llx\",\"image\":\"0x%llx\"},\"match_seconds\":%.0f,", g_app_known ? "true" : "false", (unsigned long long)g_app_slide, (unsigned long long)g_img, g_hk_match_s);
    sb_put(s, "\"summary\":{\"ok\":%d,\"failed\":%d,\"silent\":%d,\"waiting\":%d,\"off\":%d},\"items\":[", ok, bad, si, wa, of);
    int n = __atomic_load_n(&g_nhk, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) { const HkRec* r = &g_hk[i]; if (i) sb_put(s, ",");
        sb_put(s, "{\"name\":"); sb_str(s, r->name); sb_put(s, ",\"feature\":"); sb_str(s, r->feature); sb_put(s, ",\"kind\":"); sb_str(s, r->kind[0] ? r->kind : "diagnostic");
        sb_put(s, ",\"when\":"); sb_str(s, r->when); sb_put(s, ",\"state\":"); sb_str(s, hk_state_name(r->state)); sb_put(s, ",\"verdict\":"); sb_str(s, hk_verdict(r));
        sb_put(s, ",\"why\":"); sb_str(s, r->why); sb_put(s, ",\"at\":\"0x%llx\",\"fires\":%llu,\"last_s\":", (unsigned long long)r->at, (unsigned long long)r->fires);
        if (r->fires && r->last) sb_put(s, "%.1f}", now - (double)r->last / hk_freq()); else sb_put(s, "null}"); }
    sb_put(s, "]}"); }
static void hk_snapshot_json(StBuf* s) {   // (the stats snapshot: the page's Hooks button colour)
    int ok, bad, si, wa, of; hk_counts(&ok, &bad, &si, &wa, &of);
    sb_put(s, "\"hookcheck\":{\"app\":"); sb_str(s, g_app_ver); sb_put(s, ",\"known\":%s,\"ok\":%d,\"failed\":%d,\"silent\":%d,\"waiting\":%d,\"off\":%d}", g_app_known ? "true" : "false", ok, bad, si, wa, of); }
static void hk_log_summary(const char* when) {
    int ok, bad, si, wa, of; hk_counts(&ok, &bad, &si, &wa, &of);
    nlog("[hooks] %s (app %s): %d ok, %d FAILED, %d silent, %d waiting, %d off -- GET /hooks.json", when, g_app_ver, ok, bad, si, wa, of);
    int n = __atomic_load_n(&g_nhk, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) { const HkRec* r = &g_hk[i]; const char* v = hk_verdict(r);
        if (!strcmp(v, "FAILED") || !strcmp(v, "silent")) nlog("[hooks]   %-7s %s (%s): %s", v, r->name, r->feature, r->why[0] ? r->why : "installed, no call yet"); } }
// from the stats snapshot (10 a second): match time, the summaries (90 s after the engine came up, 60 s into each match)
static void hk_tick(double dt) {
    if (g_gameplay_t0) g_hk_match_s += dt;
    static double up; static int said_up, said_match; static double last_t0;
    if (g_img) up += dt;
    if (!said_up && up >= 90) { said_up = 1; hk_log_summary("90 s after the engine started"); }
    if (g_gameplay_t0 != last_t0) { last_t0 = g_gameplay_t0; said_match = 0; }
    if (g_gameplay_t0 && !said_match && mono_s() - g_gameplay_t0 >= 60) { said_match = 1; hk_log_summary("60 s into the match"); }
}
