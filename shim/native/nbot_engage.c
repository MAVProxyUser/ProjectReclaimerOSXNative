// nbot_engage.c -- the fighter's tactics from how real players fight (included by nbot.c; 2026-11-08 midday, the user: "only AFTER
// you've learned how players engage in fights ...", "don't just limit the logic to single player engagement ... they may get shot
// by someone else while fighting 1:1 and change focus"). The numbers come from shim/tools/engage-model.py over the engagement
// library (shim/tools/engage-extract.py: duels and the multi-player records of the recorded public matches), file
// ReclaimerMac/tracks/engage-model.json (NHOOK_BOT_ENGAGE=<file>, =0 = off: the old nearest-enemy fighter):
//   awareness   an enemy is known when it is in view (within EG_VIEW deg of the aim), it attacked us (aimed at us while we were
//               hurt in the last 1.5 s), it is within 5 units, or it was our target in the last 3 s -- players are caught looking
//               away half the time (and then win 7% of their duels): the walker does not see through the back of its head
//   choice      a new target = the known enemy within 45 units with the best score w . features, the players' own choice rule
//               fitted as a conditional logit (log distance, how far off the aim, attacked me, shield + health, previous target,
//               a teammate on it, just hurt, height); plus +1.5 when the line of sight is clear, -1.5 when blocked (not in the
//               data: they attacked whom they saw). The target is KEPT while it lives and was seen in the last second.
//   second      fighting B for a second or more, a NEW enemy C attacks: the walker turns to C with the players' odds (in view /
//               behind) after their delay (median ~1.75 s, drawn 0.5..1.5x), decided once per attacker
//   distance    the band players attack from with the walker's weapon class (25%..75%), when the class has 20+ duels
//   movement    2-second pieces of real fights (the attacker's speed across / toward the other every 0.1 s) by weapon class and
//               distance band, played back as the throttle, with a pull back into the band
#define EG_VIEW 75.0f
static double g_gl_kill_t;   // the last time a target died while the walker fought it (nbot_goals.c: a life with a kill)
static int rd_contact(const NovObj* me, const NovObj* t);   // (nbot_goals.c: on the walker's radar)
typedef struct { char grp[24]; float lat[20], rad[20]; char mine; } EgPiece;   // mine: from this map's own model
static struct { int loaded, on, npiece, nmine; float w[8]; int has_w; struct { char cls[12]; float p25, med, p75; int n; } band[10]; int nband;
                float p_view, p_behind, p_mid, turn_s; EgPiece* lib; int gren_ok; float gren_every, gren_d0, gren_d1;
                float rd_turn, rd_turn_s, p_killer, p_spot, it_near, it_mid, it_far;
                float os_pen;   // (behaviour.powerups: the logit the attacker loses against an overshield; target choice)
                float sw_d[6][6]; int sw_n[6][6];   // (behaviour.inventory.by_pair: the players' switches class -> class, the nearest enemy's median distance)
                float tw_spacing, tw_within15, p_join, join_s, p_aid, aid_s;   // (teamwork in use: tw_pick() from twset by the game's size)
                float twset[3][6]; int twhas[3]; } g_eg;   // [0] all team games, [1] 12 players or fewer, [2] more
static volatile uint32_t g_tw_mate; static volatile double g_tw_until;   // the teammate the walker works with right now (aid / join), until
static volatile uint32_t g_tw_aid_mate;                                   // a teammate to go to (its attacker not known to us): nbot_goals.c   // (behaviour: radar, revenge, items)
static const char* eg_find(const char* txt, const char* key, const char* end) {   // "key": within [txt, end)
    char k[64]; snprintf(k, sizeof k, "\"%s\":", key); const char* p = strstr(txt, k); if (!p || (end && p >= end)) return 0;
    p += strlen(k); while (*p == ' ' || *p == '\n' || *p == '\t') p++; return p; }   // (whitespace after the colon: a pretty-printed model too)
static float eg_num(const char* txt, const char* key, const char* end, float d) {
    const char* p = eg_find(txt, key, end); if (!p) return d; while (*p == ' ') p++; if (*p == '[') p++; if (!strncmp(p, "null", 4)) return d; return strtof(p, 0); }
static const char* eg_close(const char* p) {   // the end of the {...} / [...] that starts at p
    int dep = 0; for (; *p; p++) { if (*p == '{' || *p == '[') dep++; else if (*p == '}' || *p == ']') { if (--dep == 0) return p + 1; } } return p; }
static int eg_arr(const char* p, float* out, int max) {   // [a, b, null, ...] -> values (null = 0)
    int n = 0; if (!p) return 0; while (*p == ' ') p++; if (*p != '[') return 0; p++;
    while (*p && *p != ']' && n < max) { while (*p == ' ' || *p == ',') p++; if (!strncmp(p, "null", 4)) { out[n++] = 0; p += 4; continue; } char* e; float v = strtof(p, &e); if (e == p) break; out[n++] = v; p = e; }
    return n; }
// eg_parse: one model's text [txt, end) into g_eg. overlay = 0: the aggregate (defaults first); 1: a map's (or a map x game type's)
// numbers replace the aggregate's only where they are there (target weights when fitted, distance bands by class, the
// second-attacker odds, grenades, behaviour); its movement pieces are added marked "mine" (eg_move prefers them).
static void eg_parse(const char* txt, const char* end, int overlay, int mine) {
    static const char* const fk[8] = {"log_dist", "off_aim", "attacked_me", "vitality", "previous_target", "teammate_on_it", "just_hurt", "height"};
    if (!overlay) { g_eg.p_view = 0.33f; g_eg.p_behind = 0.07f; g_eg.turn_s = 1.75f;
        g_eg.os_pen = -1.0f; g_eg.rd_turn = 0.28f; g_eg.rd_turn_s = 1.24f; g_eg.p_killer = 0.05f; g_eg.p_spot = 0.08f; g_eg.it_near = 0.03f; g_eg.it_mid = 0.11f; g_eg.it_far = 0.05f;
        g_eg.tw_spacing = 10.0f; g_eg.tw_within15 = 0.5f; g_eg.p_join = 0.3f; g_eg.join_s = 1.5f; g_eg.p_aid = 0.3f; g_eg.aid_s = 1.5f; }
    const char* tw = eg_find(txt, "target", end);
    if (tw && *tw == '{') { const char* te = eg_close(tw); for (int i = 0; i < 8; i++) g_eg.w[i] = eg_num(tw, fk[i], te, overlay ? g_eg.w[i] : 0); g_eg.has_w = 1; }
    const char* db = eg_find(txt, "distance_by_weapon", end);
    if (db && *db == '{') { const char* de = eg_close(db);
        static const char* const cl[] = {"precision", "auto", "shotgun", "sniper", "rocket", "melee", "other", "none"};
        for (int i = 0; i < 8; i++) { const char* c = eg_find(db, cl[i], de); if (!c) continue; const char* ce = eg_close(c); int k = -1;
            for (int j = 0; j < g_eg.nband; j++) if (!strcmp(g_eg.band[j].cls, cl[i])) k = j;
            if (k < 0) { if (g_eg.nband >= 10) continue; k = g_eg.nband++; }
            int n = (int)eg_num(c, "n", ce, 0); if (overlay && n < 20) continue;   // (a map's band only from 20+ of its duels)
            snprintf(g_eg.band[k].cls, sizeof g_eg.band[0].cls, "%s", cl[i]); g_eg.band[k].p25 = eg_num(c, "p25", ce, 0); g_eg.band[k].med = eg_num(c, "median", ce, 0);
            g_eg.band[k].p75 = eg_num(c, "p75", ce, 0); g_eg.band[k].n = n; } }
    const char* sa = eg_find(txt, "second_attacker", end);
    if (sa && *sa == '{') { const char* se = eg_close(sa); g_eg.p_view = eg_num(sa, "turned_if_new_one_in_view", se, g_eg.p_view); g_eg.p_behind = eg_num(sa, "turned_if_new_one_behind", se, g_eg.p_behind);
        g_eg.turn_s = eg_num(sa, "turn_s_median", se, g_eg.turn_s); }
    g_eg.p_mid = (g_eg.p_view + g_eg.p_behind) / 2;
    // grenades (recordings since 12:00 carry the counts): the players' throws per minute of fighting and their distance band
    const char* gr = eg_find(txt, "grenades", end);
    if (gr && *gr == '{') { const char* ge = eg_close(gr); float rate = eg_num(gr, "throws_per_fight_min", ge, 0); int nd = (int)eg_num(gr, "duels", ge, 0);
        float d0 = eg_num(gr, "throw_dist_p25", ge, 0), d1 = eg_num(gr, "throw_dist_p75", ge, 0);
        if (nd >= 50 && rate > 0.05f && d1 > d0) { g_eg.gren_ok = 1; g_eg.gren_every = 60.0f / rate; g_eg.gren_d0 = d0; g_eg.gren_d1 = d1; } }
    // behaviour (engage-model.py: the players' radar turns, revenge, power items) -> the walker's radar look and goal draw
    const char* bh = eg_find(txt, "behaviour", end);
    if (bh && *bh == '{') { const char* be = eg_close(bh); const char* rd = eg_find(bh, "radar", be), * rv = eg_find(bh, "revenge", be), * it = eg_find(bh, "items", be), * pu = eg_find(bh, "powerups", be), * iv = eg_find(bh, "inventory", be);
        if (iv && iv < be) { const char* ie2 = eg_close(iv); const char* bp = eg_find(iv, "by_pair", ie2);
            static const char* const cl6[6] = {"precision", "auto", "sniper", "shotgun", "rocket", "melee"};   // (nbot.c wz_cls's order)
            if (bp && bp < ie2) { const char* bpe = eg_close(bp);
                for (int a = 0; a < 6; a++) for (int b = 0; b < 6; b++) { if (a == b) continue; char k[32]; snprintf(k, sizeof k, "%s>%s", cl6[a], cl6[b]); const char* q = eg_find(bp, k, bpe);
                    if (!q || q >= bpe) continue; const char* qe = eg_close(q); int n = (int)eg_num(q, "n", qe, 0); float d = eg_num(q, "dist_median", qe, -1);
                    if (n > 0 && d > 0) { g_eg.sw_n[a][b] = n; g_eg.sw_d[a][b] = d; } } } }
        if (pu && pu < be) { const char* pe = eg_close(pu); float wn = eg_num(pu, "attacker_wins_normal", pe, -1), wv = eg_num(pu, "attacker_wins_vs_overshield", pe, -1), nv = eg_num(pu, "duels_vs_overshield", pe, 0);
            if (wn > 0.02f && wn < 0.98f && wv > 0.02f && wv < 0.98f && nv >= 20) g_eg.os_pen = logf(wv / (1 - wv)) - logf(wn / (1 - wn)); }
        if (rd && rd < be) { const char* re = eg_close(rd); g_eg.rd_turn = eg_num(rd, "turn_moving_in_range", re, g_eg.rd_turn); g_eg.rd_turn_s = eg_num(rd, "turn_s_median", re, g_eg.rd_turn_s); }
        if (rv && rv < be) { const char* ve = eg_close(rv); float nk = eg_num(rv, "next_target_killer", ve, -1), ch = eg_num(rv, "chance", ve, -1), bs = eg_num(rv, "back_at_death_spot", ve, -1), bb = eg_num(rv, "baseline_spot", ve, -1);
            // the players' EXCESS over chance / the baseline spot, at least 5% / 8% (the user saw it; FFA data shows little of it yet)
            if (nk >= 0 && ch >= 0) g_eg.p_killer = fmaxf(0.05f, nk - ch); if (bs >= 0 && bb >= 0) g_eg.p_spot = fmaxf(0.08f, bs - bb); }
        if (it && it < be) { const char* ie = eg_close(it);
            float a = eg_num(it, "heading_in_2_10", ie, -1), a0 = eg_num(it, "baseline_2_10", ie, -1), b = eg_num(it, "heading_in_10_20", ie, -1), b0 = eg_num(it, "baseline_10_20", ie, -1), c = eg_num(it, "heading_in_20_40", ie, -1), c0 = eg_num(it, "baseline_20_40", ie, -1);
            // per goal draw: twice the players' extra heading-in share at that distance (a draw lasts 6-15 s; they decide all the time)
            if (a >= 0 && a0 >= 0) g_eg.it_near = fmaxf(0.03f, 2 * (a - a0)); if (b >= 0 && b0 >= 0) g_eg.it_mid = fmaxf(0.03f, 2 * (b - b0)); if (c >= 0 && c0 >= 0) g_eg.it_far = fmaxf(0.03f, 2 * (c - c0)); } }
    // teamwork (engage-model.py behaviour.teamwork, from team games): the spacing to the nearest teammate, how often one is within
    // 15 units, the odds and delays of joining a teammate's fight and of coming to a teammate's aid
    static const char* const twk[3] = {"teamwork", "teamwork_small", "teamwork_large"};   // (by game size: 30-vs-30 is no rule for a 2v2)
    for (int k = 0; k < 3; k++) { const char* tm = eg_find(txt, twk[k], end); if (!(tm && *tm == '{')) continue; const char* tme = eg_close(tm); float* v = g_eg.twset[k];
        if (!g_eg.twhas[k]) { v[0] = 10.0f; v[1] = 0.5f; v[2] = 0.3f; v[3] = 1.5f; v[4] = 0.3f; v[5] = 1.5f; }
        v[0] = eg_num(tm, "near_mate_median", tme, v[0]); v[1] = eg_num(tm, "within15", tme, v[1]); v[2] = eg_num(tm, "joined", tme, v[2]);
        v[3] = eg_num(tm, "join_s_median", tme, v[3]); v[4] = eg_num(tm, "aided", tme, v[4]); v[5] = eg_num(tm, "aid_s_median", tme, v[5]); g_eg.twhas[k] = 1; }
    const char* lb = eg_find(txt, "library", end);
    if (lb && *lb == '{') { const char* le = eg_close(lb); if (!g_eg.lib) g_eg.lib = calloc(2400, sizeof(EgPiece));
        for (const char* p = lb + 1; g_eg.lib && p < le && g_eg.npiece < 2400; ) {
            const char* q = strchr(p, '"'); if (!q || q >= le) break; const char* q2 = strchr(q + 1, '"'); if (!q2) break;
            char grp[24]; snprintf(grp, sizeof grp, "%.*s", (int)(q2 - q - 1), q + 1); const char* arr = strchr(q2, '['); if (!arr || arr >= le) break; const char* ae = eg_close(arr);
            for (const char* o = strchr(arr, '{'); o && o < ae && g_eg.npiece < 2400; o = strchr(o, '{')) { const char* oe = eg_close(o); EgPiece* pc = &g_eg.lib[g_eg.npiece];
                if (eg_arr(eg_find(o, "lat", oe), pc->lat, 20) == 20 && eg_arr(eg_find(o, "rad", oe), pc->rad, 20) == 20) { snprintf(pc->grp, sizeof pc->grp, "%s", grp); pc->mine = (char)mine; g_eg.npiece++; if (mine) g_eg.nmine++; }
                o = oe; }
            p = ae; } }
}
static char* eg_read(const char* path, long* len) {
    FILE* f = fopen(path, "r"); if (!f) return 0; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n <= 0 || n > (32 << 20)) { fclose(f); return 0; }
    char* txt = malloc((size_t)n + 1); if (!txt) { fclose(f); return 0; } size_t got = fread(txt, 1, (size_t)n, f); fclose(f); txt[got] = 0; *len = (long)got; return txt; }
// The models for THIS map and game type (2026-11-08 14:00, the user: "the walkers should be able to adapt learned logic for each
// map"): the aggregate (tracks/engage-model.json), overlaid by tracks/maps/<map>.engage.json, overlaid by its "by_class" section
// for the game being played (slayer / ctf / other, from the variant's engine) -- reloaded whenever the map or the game changes.
static char g_eg_key[128];
static void eg_load_for(const char* map, const char* engine) {
    const char* cls = !engine || !engine[0] ? "" : strstr(engine, "Capture") ? "ctf" : strstr(engine, "Slayer") ? "slayer" : strstr(engine, "Infection") ? "infection" : "other";
    char key[128]; snprintf(key, sizeof key, "%s|%s", map ? map : "", cls); if (g_eg.loaded && !strcmp(key, g_eg_key)) return;
    snprintf(g_eg_key, sizeof g_eg_key, "%s", key); free(g_eg.lib); memset(&g_eg, 0, sizeof g_eg); g_eg.loaded = 1;
    const char* e = getenv("NHOOK_BOT_ENGAGE"), * h = getenv("HOME"); if (e && !strcmp(e, "0")) { nlog("[nhook] [fight] engagement model off (NHOOK_BOT_ENGAGE=0)"); return; }
    char path[1024]; if (e && *e) snprintf(path, sizeof path, "%s", e); else snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/tracks/engage-model.json", h ? h : "");
    long n = 0; char* txt = eg_read(path, &n); if (!txt) { nlog("[nhook] [fight] no engagement model at %s: the old fighter", path); return; }
    eg_parse(txt, txt + n, 0, 0); free(txt);
    int map_ok = 0, cls_ok = 0;
    if (map && map[0] && !getenv("NHOOK_BOT_NO_MAPMODEL")) { char mp[1200]; snprintf(mp, sizeof mp, "%.*s/maps/%s.engage.json", (int)(strrchr(path, '/') ? strrchr(path, '/') - path : 0), path, map);
        long mn = 0; char* mt = eg_read(mp, &mn);
        if (mt) { const char* bc = eg_find(mt, "by_class", 0); eg_parse(mt, bc ? bc : mt + mn, 1, 1); map_ok = 1;
            if (bc && *bc == '{' && cls[0]) { const char* be = eg_close(bc), * c = eg_find(bc, cls, be); if (c && *c == '{') { eg_parse(c, eg_close(c), 1, 1); cls_ok = 1; } }
            free(mt); } }
    g_eg.on = g_eg.has_w;
    nlog("[nhook] [fight] engagement model for %s (%s): aggregate%s%s; target weights %s (off aim %.2f, log distance %.2f, attacked me %.2f, just hurt %.2f), %d weapon bands, second attacker %.0f%% in view / %.0f%% behind after %.2f s, %d movement pieces (%d of this map), grenades %s (every %.0f s of fighting at %.0f..%.0f units)",
         map && map[0] ? map : "?", cls[0] ? cls : "?", map_ok ? " + this map" : " (no model for this map yet)", cls_ok ? " + this map's game type" : "",
         g_eg.has_w ? "loaded" : "MISSING", g_eg.w[1], g_eg.w[0], g_eg.w[2], g_eg.w[6], g_eg.nband, g_eg.p_view * 100, g_eg.p_behind * 100, g_eg.turn_s, g_eg.npiece, g_eg.nmine,
         g_eg.gren_ok ? "from the players" : "the tactics' own", g_eg.gren_every, g_eg.gren_d0, g_eg.gren_d1);
    nlog("[nhook] [fight] behaviour: radar look %.0f%% after %.2f s; after a respawn: the killer %.0f%%, the death spot %.0f%%; power items per draw %.0f%% / %.0f%% / %.0f%% (2-10 / 10-20 / 20-40 units)",
         g_eg.rd_turn * 100, g_eg.rd_turn_s, g_eg.p_killer * 100, g_eg.p_spot * 100, g_eg.it_near * 100, g_eg.it_mid * 100, g_eg.it_far * 100);
    nlog("[nhook] [fight] teamwork (team games): a teammate %.1f units away (median), within 15 units %.0f%% of the time; joins a teammate's fight %.0f%% after %.2f s; comes to a teammate's aid %.0f%% after %.2f s",
         g_eg.tw_spacing, g_eg.tw_within15 * 100, g_eg.p_join * 100, g_eg.join_s, g_eg.p_aid * 100, g_eg.aid_s);
}
static void eg_load(void) { eg_load_for("", ""); }   // (the aggregate only: the offline test, the first fight tick before the map is known)
static void tw_pick(int players) {   // the teamwork numbers for a game of this size (small set, else large, else all team games, else defaults)
    int k = players <= 12 ? (g_eg.twhas[1] ? 1 : g_eg.twhas[0] ? 0 : -1) : (g_eg.twhas[2] ? 2 : g_eg.twhas[0] ? 0 : -1);
    if (k < 0) return; const float* v = g_eg.twset[k];
    g_eg.tw_spacing = v[0]; g_eg.tw_within15 = v[1]; g_eg.p_join = v[2]; g_eg.join_s = v[3]; g_eg.p_aid = v[4]; g_eg.aid_s = v[5]; }
// a weapon / item name -> the matching key: lowercase, spaces as underscores (2026-11-08: natively hb_tag_name gives the tag's
// last path part in CAPITALS with spaces -- "ASSAULT RIFLE" -- so every lookup below written for "assault_rifle" missed: every
// weapon was class "other" (no per-class distance band), no pickup ever ranked better, no power weapon was seen as one)
static const char* bot_tagkey(const char* in, char* out, size_t n) {
    size_t j = 0; for (; in && in[j] && j + 1 < n; j++) { char c = in[j]; out[j] = c == ' ' ? '_' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } out[j] = 0; return out; }
static const char* eg_wcls(const char* tag0) {   // the walker's weapon (a tag path or name) -> engage-model.py's weapon classes
    if (!tag0) return "none"; char tb[96]; const char* tag = bot_tagkey(tag0, tb, sizeof tb);
    static const struct { const char* k; const char* c; } m[] = {{"sniper", "sniper"}, {"beam_rifle", "sniper"}, {"rocket", "rocket"}, {"spartan_laser", "rocket"}, {"fuel_rod", "rocket"},
        {"missile", "rocket"}, {"shotgun", "shotgun"}, {"mauler", "shotgun"}, {"energy_sword", "melee"}, {"energy_blade", "melee"}, {"hammer", "melee"}, {"battle_rifle", "precision"},
        {"carbine", "precision"}, {"magnum", "precision"}, {"dmr", "precision"}, {"assault_rifle", "auto"}, {"smg", "auto"}, {"plasma_rifle", "auto"}, {"spike_rifle", "auto"},
        {"needler", "auto"}, {"brute_shot", "auto"}, {"machinegun", "auto"}, {"plasma_pistol", "auto"}, {"plasma_cannon", "auto"}, {"sentinel", "auto"}, {0, 0}};
    for (int i = 0; m[i].k; i++) if (strstr(tag, m[i].k)) return m[i].c; return "other"; }
static int eg_band(const char* cls, float* p25, float* p75) {   // the players' attack distances with this class (20+ duels)
    for (int i = 0; i < g_eg.nband; i++) if (!strcmp(g_eg.band[i].cls, cls) && g_eg.band[i].n >= 20 && g_eg.band[i].p75 > g_eg.band[i].p25) { *p25 = g_eg.band[i].p25; *p75 = g_eg.band[i].p75; return 1; }
    return 0; }
// per-unit memory: shield + health a moment ago (just hurt), its last drop, whether it aimed at us while we were hurt
typedef struct { uint32_t h; float vit; double hurt_t, att_t, seen_tgt, asked_t, attl_t; int asked; } EgUnit;   // (attl_t: it shot the walker's leader)
// COACHED UP (2026-11-08, the user after beating two walkers 17-4: "shooting first" -- match-study.py: shot at first, the walkers
// fired back after a median 4.4-5.5 s (the user 1.1 s) and 14-22 times never; with a second attacker on them they "stayed on the
// target" 19 of 22 times -- the players' own odds: 33% in view, 7% behind, after ~1.75 s): a second attacker hurting the walker
// while its current target is not shooting at it wins (asked again every second while it keeps hitting), the turn takes half the
// players' delay (NHOOK_BOT_TURNX, default 0.5), and a new target that is shooting at the walker gets NHOOK_BOT_ATTBONUS (6) on
// its score -- enough to outweigh being behind (off aim -7.74 at 180 deg). NHOOK_BOT_HUMANLIKE=1 = the players' rules as before.
static int eg_humanlike(void) { static int v = -1; if (v < 0) v = getenv("NHOOK_BOT_HUMANLIKE") != 0; return v; }
static float eg_knob(const char* n, float d) { float v; if (lk_get(n, &v)) return v; const char* e = getenv(n); return e ? strtof(e, 0) : d; }   // (live knobs first)
static EgUnit g_egu[128]; static int g_negu;
static volatile uint32_t g_eg_fast;   // a target taken on purpose (a second attacker after the players' delay): turned to at full speed
static EgUnit* eg_unit(uint32_t h) { for (int i = 0; i < g_negu; i++) if (g_egu[i].h == h) return &g_egu[i];
    if (g_negu < 128) { EgUnit* u = &g_egu[g_negu++]; memset(u, 0, sizeof *u); u->h = h; u->vit = -1; return u; }
    EgUnit* u = &g_egu[arc4random_uniform(128)]; memset(u, 0, sizeof *u); u->h = h; u->vit = -1; return u; }
static int eg_aim_at(const NovObj* t, const float* p, float deg) {   // t's unit aim (+0x1a0) within deg of the line to p
    float av[3]; if (!t->p || !hb_read(t->p + 0x1a0, av, 12)) return 0; float dx = p[0] - t->c[0], dy = p[1] - t->c[1];
    float a1 = atan2f(av[1], av[0]), a2 = atan2f(dy, dx), d = fabsf(bot_wrap(a1 - a2)) * 57.2958f; return d < deg; }
// The choice: returns the index in objs of the target (-1 = nobody known), keeps / switches as the players do.
static int eg_choose(const NovObj* objs, int n, int me, const float* cam, float yaw0, double now, int (*vis_of)(uint32_t, double)) {
    const NovObj* o = &objs[me]; static uint32_t cur; static double cur_t0, cur_seen, sw_at; static uint32_t sw_to, prev; static double prev_t; static float myvit = -1; static double my_hurt;
    float mv = o->shield + o->health; if (myvit >= 0 && mv < myvit - 0.02f) my_hurt = now; myvit = mv;
    int ci = -1;
    // (2026-11-09, the user: "auto target anyone shooting me. this will hone the walkers skills immensely"): the LEADER the walker
    // shadows (NHOOK_BOT_FOLLOW) -- when it loses shield / health, an enemy aiming at it within 12 deg is ITS attacker: counted as
    // attacking the walker too (known, the attacker bonus, the switch to it) and marked attl_t for a bonus of its own
    const NovObj* L = 0; if (g_follow_unit != 0xFFFFFFFFu) for (int i = 0; i < n; i++) if (objs[i].handle == g_follow_unit && !objs[i].dead && !objs[i].type) { L = &objs[i]; break; }
    static float lvit = -1; static double lead_hurt; if (L) { float lv = L->shield + L->health; if (lvit >= 0 && lv < lvit - 0.02f) lead_hurt = now; lvit = lv; } else lvit = -1;
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || i == me) continue; EgUnit* u = eg_unit(t->handle);
        float v = t->dead ? 0 : t->shield + t->health; if (u->vit >= 0 && v < u->vit - 0.02f) u->hurt_t = now; u->vit = v;
        if (!t->dead && bot_enemy(o, t) && now - my_hurt < 1.5 && eg_aim_at(t, o->c, 12)) { if (now - u->att_t > 1.5) u->asked = 0; u->att_t = now; }
        if (L && t != L && !t->dead && bot_enemy(o, t) && now - lead_hurt < 1.5 && eg_aim_at(t, L->c, 12)) {
            if (now - u->att_t > 1.5) u->asked = 0; u->att_t = now;
            if (now - u->attl_t > 3.0 && bot_log()) nlog("[nhook] [wing] %08x is shooting my leader: on it", t->handle); u->attl_t = now; }
        if (t->handle == cur) ci = i; }
    if (ci >= 0 && objs[ci].dead && now - cur_seen < 2.0) g_gl_kill_t = now;   // (it died while we fought it)
    if (ci >= 0 && (objs[ci].dead || !bot_enemy(o, &objs[ci]))) ci = -1;
    if (ci >= 0 && vis_of(cur, now) == 1) cur_seen = now;
    // a second attacker while fighting the current target for a second or more: turn to it with the players' odds, once per attacker
    if (ci >= 0 && now - cur_t0 > 1.0) for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || t->handle == cur || !bot_enemy(o, t)) continue;
        EgUnit* u = eg_unit(t->handle); if (now - u->att_t > 0.2) continue;
        int hl = eg_humanlike(); if (u->asked && (hl || now - u->asked_t < 1.0)) continue; u->asked = 1; u->asked_t = now;
        float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], off = fabsf(bot_wrap(atan2f(dy, dx) - yaw0)) * 57.2958f;
        float p = off < 45 ? g_eg.p_view : off >= 90 ? g_eg.p_behind : g_eg.p_mid;
        int cur_att = 0; if (!hl) { EgUnit* cu = eg_unit(cur); cur_att = now - cu->att_t < 1.0; p = cur_att ? fmaxf(p, 0.5f) : 1.0f; }   // (coached: see EgUnit)
        static float tx = -1; if (tx < 0) tx = hl ? 1.0f : eg_knob("NHOOK_BOT_TURNX", 0.5f);
        if (bot_rand01() < p) { sw_to = t->handle; sw_at = now + tx * g_eg.turn_s * (0.5f + bot_rand01()); if (bot_log()) nlog("[nhook] [fight] second attacker %08x (%.0f deg off%s): turning to it in %.1f s", t->handle, off, cur_att ? ", the target shoots too" : "", sw_at - now); }
        else if (bot_log()) nlog("[nhook] [fight] second attacker %08x (%.0f deg off): staying on the target", t->handle, off); }
    // TEAMWORK (team games; the user: "cooperate with other team mates toward a common offense, or if one comes to your aid, ensure
    // you are working together for your and his collective defense as a unit"): with no target, a teammate within 25 units under
    // fire (hurt in the last second while an enemy aims at it) -> come to its aid with the players' odds after their delay (its
    // attacker becomes the target when we know it, else we go to the teammate); a teammate shooting an enemy (aim within 8 deg) ->
    // join on the same enemy with the players' odds and delay. Decided once per (teammate, enemy) per 5 s; the teammate is then
    // the walker's unit partner for 12 s (nbot_goals.c keeps them together).
    { int units = 0; for (int i = 0; i < n; i++) if (!objs[i].type && !objs[i].dead) units++; tw_pick(units); }   // (this game's size)
    if (ci < 0 || now - cur_seen >= 1.0) {
        static struct { uint32_t m, e; double t; } asked[64]; static int na;
        for (int i = 0; i < n && !sw_to; i++) { const NovObj* m = &objs[i]; if (m->type || m->dead || i == me || bot_enemy(o, m)) continue;
            float mx = m->c[0] - o->c[0], my = m->c[1] - o->c[1]; if (mx * mx + my * my > 625) continue;
            EgUnit* mu = eg_unit(m->handle); int hurt = now - mu->hurt_t < 1.0;
            for (int j = 0; j < n; j++) { const NovObj* e = &objs[j]; if (e->type || e->dead || j == me || !bot_enemy(o, e)) continue;
                int aid = hurt && eg_aim_at(e, m->c, 12), join = !aid && eg_aim_at(m, e->c, 8); if (!aid && !join) continue;
                int k = -1; for (int q = 0; q < na; q++) if (asked[q].m == m->handle && asked[q].e == e->handle) { k = q; break; }
                if (k >= 0 && now - asked[k].t < 5.0) continue;
                if (k < 0) { k = na < 64 ? na++ : (int)arc4random_uniform(64); asked[k].m = m->handle; asked[k].e = e->handle; } asked[k].t = now;
                if (bot_rand01() >= (aid ? g_eg.p_aid : g_eg.p_join)) continue;
                float dx = e->c[0] - cam[0], dy = e->c[1] - cam[1], off = fabsf(bot_wrap(atan2f(dy, dx) - yaw0)) * 57.2958f;
                int known = off < EG_VIEW || rd_contact(o, e) || vis_of(e->handle, now) == 1;
                g_tw_mate = m->handle; g_tw_until = now + 12.0;
                if (known) { sw_to = e->handle; sw_at = now + (aid ? g_eg.aid_s : g_eg.join_s) * (0.5f + bot_rand01()); }
                else if (aid) g_tw_aid_mate = m->handle;
                if (bot_log()) nlog("[nhook] [team] %s teammate %08x %s %08x: %s", aid ? "aid:" : "join:", m->handle, aid ? "attacked by" : "fighting", e->handle,
                                    known ? "taking its enemy" : aid ? "going to the teammate" : "(enemy not known: staying)");
                break; } } }
    if (sw_to && now >= sw_at) { for (int i = 0; i < n; i++) if (objs[i].handle == sw_to && !objs[i].dead && i != me) { prev = cur; prev_t = now; cur = sw_to; cur_t0 = now; cur_seen = now; ci = i; g_eg_fast = sw_to; break; } sw_to = 0; }
    // ENGAGE (2026-11-08, the user: "walkers should never just walk past an enemy without predictive grenade lay some shots, or a beat
    // down! engage!"): a VISIBLE enemy within 10 units is taken at once -- unless the current target is itself visible within 10 --
    // instead of the drawn choice keeping a far / hidden one. NHOOK_BOT_NOENGAGE=1 = off.
    { static int ne = -1; if (ne < 0) ne = getenv("NHOOK_BOT_NOENGAGE") != 0;
      if (!ne && !(ci >= 0 && vis_of(cur, now) == 1 && ({ float ex = objs[ci].c[0] - cam[0], ey = objs[ci].c[1] - cam[1], ez = objs[ci].c[2] - cam[2]; ex * ex + ey * ey + ez * ez < 100.0f; }))) {
          int bi = -1; float bd = 100.0f;
          for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t) || vis_of(t->handle, now) != 1) continue;
              float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; bi = i; } }
          if (bi >= 0 && objs[bi].handle != cur) { if (cur) { prev = cur; prev_t = now; } cur = objs[bi].handle; cur_t0 = now; cur_seen = now; g_eg_fast = cur;
              if (bot_log()) nlog("[nhook] [fight] engage: %08x in sight at %.1f units -- taken", cur, sqrtf(bd)); return bi; } } }
    if (ci >= 0 && now - cur_seen < 1.0) return ci;   // (kept: alive and seen in the last second)
    // a new target: the players' choice rule over the KNOWN enemies within 45 units -- DRAWN from its probabilities (exp(score) /
    // sum), not the best score: the best-score walker was more predictable than the players (the rule named its target 72% of the
    // time, the players' own 39%: engage-compare.py, 2026-11-08 12:10). NHOOK_BOT_PICK=best = the old argmax.
    static int pickbest = -1; if (pickbest < 0) { const char* e = getenv("NHOOK_BOT_PICK"); pickbest = e && !strcmp(e, "best"); }
    int bi = -1; float bs = -1e30f; int ci2[64]; float cs2[64]; int nc2 = 0;
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
        float dx = t->c[0] - cam[0], dy = t->c[1] - cam[1], dz = t->c[2] - cam[2], d = sqrtf(dx * dx + dy * dy + dz * dz); if (d > 45) continue;
        EgUnit* u = eg_unit(t->handle); float off = fabsf(bot_wrap(atan2f(dy, dx) - yaw0)) * 57.2958f; int att = now - u->att_t < 1.5, pv = t->handle == prev && now - prev_t < 3.0;
        int carrier = g_ob_enemy_carrier && t->handle == g_ob_enemy_carrier;   // (oddball: the carrier is marked on every player's screen)
        if (!(off < EG_VIEW || att || d < 5 || pv || t->handle == cur || rd_contact(o, t) || carrier)) continue;   // (unknown: behind us, quiet and off the radar)
        // active camo (+0x360): not seen in view beyond 8 units -- known only when it attacked us, is on the radar (moving) or close
        if (t->camo > 0.3f && !(att || d < 8 || pv || t->handle == cur || rd_contact(o, t))) continue;
        int mate = 0; for (int k = 0; k < n && !mate; k++) { const NovObj* m = &objs[k]; if (m->type || m->dead || k == me || bot_enemy(o, m)) continue;   /* (a teammate: free-for-all has none) */ mate = eg_aim_at(m, t->c, 8); }
        float f[8] = {logf(d > 0.5f ? d : 0.5f), off / 180.0f, (float)att, fminf(1.5f, (t->shield + t->health) / 2), (float)pv, (float)mate, (float)(now - u->hurt_t < 1.0), (t->c[2] - o->c[2]) / 5.0f};
        float s = 0; for (int k = 0; k < 8; k++) s += g_eg.w[k] * f[k];
        int vs = vis_of(t->handle, now); s += vs == 1 ? 1.5f : vs == 0 ? -1.5f : 0;
        // an overshielded enemy (shield above 1): the players' attackers win 54% against one, 77% otherwise (engage-model.json
        // behaviour.powerups) -- half that logit as a penalty unless we have one too (choose the fight we would win)
        if (t->shield > 1.05f && o->shield <= 1.05f) s += 0.5f * g_eg.os_pen;
        { float ab = eg_humanlike() ? 0.0f : eg_knob("NHOOK_BOT_ATTBONUS", 6.0f); if (att) s += ab; }   // (coached: the one shooting at us)
        { float cb = eg_knob("NHOOK_BOT_BALLBONUS", 8.0f); if (carrier) s += cb; }   // (oddball: the enemy carrying the ball first)
        if (L) {   // (helping the leader: its attacker +4 more, its own target +5, anyone within 8 units of it +3)
            if (now - u->attl_t < 1.5) s += eg_knob("NHOOK_BOT_LEADATT", 4.0f);
            float lx = t->c[0] - L->c[0], ly = t->c[1] - L->c[1], ld = sqrtf(lx * lx + ly * ly);
            if (ld < 40.0f && eg_aim_at(L, t->c, 6)) s += eg_knob("NHOOK_BOT_LEADTGT", 5.0f);
            if (ld < 8.0f) s += eg_knob("NHOOK_BOT_LEADNEAR", 3.0f); }
        if (nc2 < 64) { ci2[nc2] = i; cs2[nc2] = s; nc2++; }
        if (s > bs) { bs = s; bi = i; } }
    if (!pickbest && nc2 > 1) { float sum = 0; for (int k = 0; k < nc2; k++) { cs2[k] = expf(cs2[k] - bs); sum += cs2[k]; }
        float r = bot_rand01() * sum; for (int k = 0; k < nc2; k++) { r -= cs2[k]; if (r <= 0) { bi = ci2[k]; break; } } }
    if (bi >= 0 && objs[bi].handle != cur) { if (cur) { prev = cur; prev_t = now; } cur = objs[bi].handle; cur_t0 = now; cur_seen = now;
        if (bot_log()) nlog("[nhook] [fight] new target %08x (score %.2f)", cur, bs); }
    else if (bi < 0 && ci >= 0) return ci;   // (nobody else known: stay on the hidden one -- the walker hunts it)
    return bi; }
// movement: a piece of a real fight by weapon class / distance band, played at 10 rows a second
static int eg_move(const char* cls, float dist, double now, float* lat, float* rad) {
    static int cur = -1; static double t0; if (!g_eg.npiece) return 0;
    if (cur < 0 || now - t0 >= 2.0) { const char* band = dist < 6 ? "close" : dist < 15 ? "mid" : "far"; char g[24]; snprintf(g, sizeof g, "%s/%s", cls, band);
        static int cand[2400]; int nc = 0;
        for (int i = 0; i < g_eg.npiece; i++) if (g_eg.lib[i].mine && !strcmp(g_eg.lib[i].grp, g)) cand[nc++] = i;   // (this map's own pieces first)
        if (!nc) for (int i = 0; i < g_eg.npiece; i++) if (!strcmp(g_eg.lib[i].grp, g)) cand[nc++] = i;
        if (!nc) { snprintf(g, sizeof g, "/%s", band); for (int i = 0; i < g_eg.npiece; i++) if (strstr(g_eg.lib[i].grp, g)) cand[nc++] = i; }   // (any weapon at this distance)
        if (!nc) return 0; cur = cand[arc4random_uniform((uint32_t)nc)]; t0 = now; }
    int k = (int)((now - t0) * 10); if (k > 19) k = 19; *lat = g_eg.lib[cur].lat[k]; *rad = g_eg.lib[cur].rad[k]; return 1; }
