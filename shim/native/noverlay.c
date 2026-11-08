// noverlay.c -- the overlay for the NATIVE app (included by nstats.c; drawn by nrender.m into the frame the app presents):
// hitboxes (collision models posed by their node matrices: h3maps.c hb_geom), vehicles and their weak spots, player info
// labels, items lying on the map, projectile tracers. A port of the loader's shim/src/hitbox.c (hb_collect,
// hb_emit_hitbox, hb_draw_overlay, the stroke font), with two differences:
//   * the camera is halo3's own observer camera (local user 0): observer globals = TLS +0x578 (halo3+0x17e274 allocates
//     0xf50 bytes), 0x3d0 bytes per local user, the camera result at +0x11c (halo3+0x17eb55 returns it): +0x00 position,
//     +0x28 forward, +0x34 up, +0x40 field of view (radians; the width of a 16:9 view: Hor+), and the frame's own aspect.
//     The loader took the frame's view-projection from halo3's first world draw (VS cb0) instead.
//   * no "visible only" (it needs the frame's depth buffer, which the app keeps inside its Metal renderer) and no zones yet.
// Switches: NHOOK_HITBOXES / NHOOK_HB_TRACERS / NHOOK_HB_INFO / NHOOK_HB_ITEMS / NHOOK_HB_VEHICLES = 1 start a feature on
// (the stats page's buttons flip them), NHOOK_HB_TEAMS=all|enemies|friends, NHOOK_HB_TEXT_PX (label height, points).
enum { NOV_HIT, NOV_TRC, NOV_INF, NOV_ITM, NOV_VIS, NOV_ZON, NOV_VEH, NOV_N };
static const char* const g_nov_name[NOV_N] = {"hitboxes", "tracers", "info", "items", "visible", "zones", "vehicles"};
static volatile unsigned g_nov_on;                          // bit f = feature f on
static volatile int g_nov_team[NOV_N], g_nov_px[NOV_N];      // team filter (0 all, 1 enemies, 2 friends), label px
static volatile int g_nov_wk = 7;                            // weak-spot kinds shown: 1 lethal, 2 opens the driver, 4 disables
static volatile int g_nov_vis[NOV_N];                        // "visible only" per feature (hidden behind the world: needs the scene's depth)
static void nov_init(void) {
    static int done; if (done) return; done = 1;
    static const char* const env[NOV_N] = {"NHOOK_HITBOXES", "NHOOK_HB_TRACERS", "NHOOK_HB_INFO", "NHOOK_HB_ITEMS", 0, "NHOOK_HB_ZONES", "NHOOK_HB_VEHICLES"};
    unsigned on = 0; for (int f = 0; f < NOV_N; f++) { const char* e = env[f] ? getenv(env[f]) : 0; if (e && *e && strcmp(e, "0")) on |= 1u << f; }
    const char* t = getenv("NHOOK_HB_TEAMS"); int tm = t && !strcasecmp(t, "enemies") ? 1 : t && !strcasecmp(t, "friends") ? 2 : 0;
    const char* p = getenv("NHOOK_HB_TEXT_PX"); int px = p ? atoi(p) : 7; if (px < 4) px = 4; if (px > 60) px = 60;
    for (int f = 0; f < NOV_N; f++) { g_nov_team[f] = (f == NOV_ITM || f == NOV_VIS || f == NOV_ZON) ? -1 : tm; g_nov_px[f] = (f == NOV_INF || f == NOV_ITM || f == NOV_ZON) ? px : -1; }
    const char* w = getenv("NHOOK_HB_WEAK_SHOW"); if (w) g_nov_wk = (int)strtol(w, 0, 0);
    { const char* v = getenv("NHOOK_HB_VISIBLE"); int vb = v && *v && strcmp(v, "0"); for (int f = 0; f < NOV_N; f++) g_nov_vis[f] = vb; }
    g_nov_on = on;
}
static int nov_feat(int f) { nov_init(); return (g_nov_on >> f) & 1; }
int nov_wanted(void) { nov_init(); return (g_nov_on & ~(1u << NOV_VIS)) != 0; }   // (nrender.m: draw at all?)

// ---- the camera ---------------------------------------------------------------------------------------------------
// vp: 16 floats, rows 0 (clip x), 1 (clip y), 3 (clip w) used as the loader's view-projection rows; vp[12..14] the forward,
// vp[28..30] the camera position (hitbox.c's cb0 layout).
int nh_camera(float* rows16, float* pos3);   // nhook.c: the frame's own view-projection (shader parameter 0x27)
static int g_nov_cam_src;                    // 1 = the frame's own view constants, 2 = the observer camera (fallback)
static int nov_observer(float* pos, float* fwd) {   // local user 0's observer camera position / forward
    uint64_t blk = tls_block(), ob = 0; float r[0x34 / 4];
    if (!blk || !hb_read(blk + 0x578, &ob, 8) || !ob || !hb_read(ob + 0x11c, r, sizeof r)) return 0;
    memcpy(pos, r, 12); if (fwd) memcpy(fwd, r + 0x28 / 4, 12); if (isfinite(pos[0])) { HK_ONCE(h, "observer camera"); hk_fire(h); } return isfinite(pos[0]);
}
int nov_cam_is_player(const float* pos) {   // a captured world camera that sits at the player's view (not a shadow / reflection view)
    static float op[3]; static double t; double now = mono_s();
    if (now - t > 0.1) { float p2[3]; if (nov_observer(p2, 0)) memcpy(op, p2, 12); t = now; }
    float dx = pos[0] - op[0], dy = pos[1] - op[1], dz = pos[2] - op[2]; return dx * dx + dy * dy + dz * dz < 1.0f;
}
static int nov_camera(float* vp, float aspect) {
    { float rows[16], pos[3];   // the frame's own view-projection (exact, interpolated, Hor+ / FOV settings included)
      if (!getenv("NHOOK_OBSERVER_CAMERA") && nh_camera(rows, pos)) { memset(vp, 0, 32 * sizeof(float)); memcpy(vp, rows, 64); memcpy(vp + 28, pos, 12); g_nov_cam_src = 1; return 1; } }
    g_nov_cam_src = 2;
    uint64_t blk = tls_block(), ob = 0; float r[0x48 / 4];
    if (!blk || !hb_read(blk + 0x578, &ob, 8) || !ob || !hb_read(ob + 0x11c, r, sizeof r)) return 0;
    const float* pos = r, * fwd = r + 0x28 / 4, * up = r + 0x34 / 4; float hfov = r[0x40 / 4];
    if (!(hfov > 0.1f && hfov < 3.0f) || !isfinite(pos[0]) || fabsf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2] - 1) > 0.05f) return 0;
    float left[3] = {up[1] * fwd[2] - up[2] * fwd[1], up[2] * fwd[0] - up[0] * fwd[2], up[0] * fwd[1] - up[1] * fwd[0]};   // up x forward
    // Hor+: the field of view is the width of a 16:9 view; other aspects keep its height (a 3440x1440 screen sees more at
    // the sides). Spreading it over the whole width put everything off-centre 1.34x too far out on a 21:9 screen (user,
    // 2026-11-07: "quite a bit off and runs parallel with the player"; measured 1.37x against the game's name markers).
    float tv = tanf(hfov * 0.5f) / (16.0f / 9.0f), th = tv * (aspect > 0.1f ? aspect : 1.7778f);
    memset(vp, 0, 32 * sizeof(float));
    for (int a = 0; a < 3; a++) { vp[0 + a] = -left[a] / th; vp[4 + a] = up[a] / tv; vp[12 + a] = fwd[a]; }
    vp[3] = -(vp[0] * pos[0] + vp[1] * pos[1] + vp[2] * pos[2]); vp[7] = -(vp[4] * pos[0] + vp[5] * pos[1] + vp[6] * pos[2]);
    vp[15] = -(vp[12] * pos[0] + vp[13] * pos[1] + vp[14] * pos[2]);
    vp[28] = pos[0]; vp[29] = pos[1]; vp[30] = pos[2];
    return 1;
}

// ---- objects (hitbox.c hb_collect) --------------------------------------------------------------------------------
typedef struct { float c[3], r, o[3], vel[3]; int type; uint32_t handle; uint64_t p; uint32_t def; uint16_t nmsz, nmoff;
    float health, shield, camo; int team, seat, wslot, dead; uint32_t weapons[4], parent, owner; int8_t gren[4]; int rcount; uint16_t roff; } NovObj;
static uint8_t g_nov_tab[HB_MAXOBJ * 0x18]; static int g_nov_hi;
// nov_collect's static buffers are shared: the overlay (render thread) and the sparring aim / walker (nbot.c, its own thread)
// take this lock around it; g_nov_units_only (under it) = 1 bipeds only, 2 vehicles only, whatever the overlay switches say
static pthread_mutex_t g_nov_collect_mx = PTHREAD_MUTEX_INITIALIZER; static int g_nov_units_only;
static uint64_t nov_datum(uint32_t h) { int i = h & 0xFFFF; if (h == 0xFFFFFFFFu || i >= g_nov_hi) return 0; const uint8_t* e = g_nov_tab + i * 0x18;
    if (*(const uint16_t*)e != (uint16_t)(h >> 16)) return 0; return *(const uint64_t*)(e + 0x10); }
static int nov_collect(NovObj* out, int max) {
    uint64_t arr = hb_object_array(); uint8_t hdr[0x60]; if (!arr || !hb_read(arr, hdr, sizeof hdr) || *(uint32_t*)(hdr + 0x2c) != 0x64407440) return 0;
    int32_t maxc = *(int32_t*)(hdr + 0x20), esz = *(int32_t*)(hdr + 0x24), hi = *(int32_t*)(hdr + 0x3c); uint64_t data = *(uint64_t*)(hdr + 0x48);
    if (esz != 0x18 || hi <= 0 || hi > maxc || hi > HB_MAXOBJ || !hb_read(data, g_nov_tab, (size_t)hi * 0x18)) return 0; g_nov_hi = hi;
    int fh = nov_feat(NOV_HIT), ft = nov_feat(NOV_TRC), fi = nov_feat(NOV_INF), fw = nov_feat(NOV_ITM), fveh = nov_feat(NOV_VEH);
    int fb = fh || fi || (fveh && g_nov_team[NOV_VEH] > 0) || (ft && g_nov_team[NOV_TRC] > 0);   // (players only for a feature that draws or filters by them)
    int n = 0;
    for (int i = 0; i < hi && n < max; i++) {
        const uint8_t* e = g_nov_tab + i * 0x18; if (!*(const uint16_t*)e) continue; int t = e[3], unit = t == 0 || t == 1 || t == 12;
        static int dbg = -1; if (dbg < 0) dbg = getenv("NHOOK_OV_DEBUG") ? 1 : 0;   // calibration: scenery / machines / crates too
        int want = g_nov_units_only ? (g_nov_units_only == 1 ? t == 0 : g_nov_units_only == 3 ? t == 2 : (t == 1 || t == 12))   // (3: weapons, the walker's pickups)
                 : (t == 0 && fb) || ((t == 1 || t == 12) && fveh) || (t == 5 && ft) || ((t == 2 || t == 3) && fw) || (dbg && t >= 6 && t <= 10); if (!want) continue;
        uint64_t p = *(const uint64_t*)(e + 0x10); static uint8_t dh[0x380]; size_t rn = unit ? 0x380 : 0x140; if (!p || !hb_read(p, dh, rn)) continue;
        const float* od = (const float*)(dh + 0x1c); NovObj* o = &out[n]; memset(o, 0, sizeof *o);
        o->c[0] = od[0]; o->c[1] = od[1]; o->c[2] = od[2]; o->r = od[3]; memcpy(o->o, dh + 0x50, 12); memcpy(o->vel, dh + 0x74, 12);
        o->type = t; o->handle = ((uint32_t)*(const uint16_t*)e << 16) | (uint32_t)i; o->p = p; o->def = *(const uint32_t*)dh; o->parent = *(const uint32_t*)(dh + 0x10);
        o->nmsz = *(const uint16_t*)(dh + 0x136); o->nmoff = *(const uint16_t*)(dh + 0x138);
        { int16_t rs = *(const int16_t*)(dh + 0x13a); if ((*(const uint32_t*)(dh + 4) >> 11) & 1) rs -= 0x20; o->rcount = rs > 0 ? rs / 10 : 0; o->roff = *(const uint16_t*)(dh + 0x13c); }
        o->dead = (*(const uint32_t*)(dh + 0x110) >> 2) & 1; o->health = o->dead ? 0 : *(const float*)(dh + 0xF4); o->shield = o->dead ? 0 : *(const float*)(dh + 0xF8);
        if (unit) { o->team = *(const int32_t*)(dh + 0x174); o->seat = *(const int16_t*)(dh + 0x24E); o->wslot = *(const int8_t*)(dh + 0x262);
            memcpy(o->weapons, dh + 0x268, 16); memcpy(o->gren, dh + 0x294, 4); o->camo = *(const float*)(dh + 0x360); }
        o->owner = t == 5 ? *(const uint32_t*)(dh + 0xC0) : 0xFFFFFFFFu;
        if (!(o->r > 0.001f && o->r < 50.f) || o->c[0] != o->c[0]) continue; n++;
    }
    return n;
}
// aim vs hits: the rings (nov_aim_shot / nov_aim_tick below, nstats.c st_aim_tick)
typedef struct { volatile unsigned seq; double t; uint32_t owner, target, def; float s[3], dn[3], len, along, pre; uint16_t nmsz; uint8_t rcount, pad; uint8_t perm[32]; float mt[128 * 13]; } NovAimRec;
enum { NOV_AIM_N = 64, NOV_AIMR_N = 4096 };
static NovAimRec g_nov_aim[NOV_AIM_N]; static volatile unsigned g_nov_aim_w;     // the engine thread writes (one writer)
typedef struct { double t; uint32_t owner, target; float err, along, pre; uint8_t on, head; } NovAimRes;
static NovAimRes g_nov_aimr[NOV_AIMR_N]; static volatile unsigned g_nov_aimr_w;  // the render thread writes
static volatile unsigned long g_nov_aim_n, g_nov_aim_done;
static volatile int g_nov_sides_on;   // (Infection sides: nov_side_of below)
// Friend or enemy (every team filter: hitboxes, vehicles by their occupants, tracers by their shooter, info). Team games:
// the same team index. Infection: halo3 gives every player a team of their own (teams off), so the sides are zombie /
// human: g_nov_side, filled by the stats snapshot from each player's active traits (st_snapshot nov_sides). Free-for-all:
// everyone is an enemy. (Infection used to show "friends" = nobody, "enemies" = everyone; user, 2026-11-07.)
static int g_nov_myteam = -1, g_nov_myside = -1;
static struct { uint32_t unit; int8_t side; } g_nov_side[256]; static volatile int g_nov_nside;   // side 1 zombie, 0 human (g_nov_sides_on: above)
static int nov_side_of(uint32_t unit) { int n = g_nov_nside; for (int i = 0; i < n && i < 256; i++) if (g_nov_side[i].unit == unit) return g_nov_side[i].side; return -1; }
static int nov_friend(const NovObj* o) {
    if (o->type != 0) return 0;
    if (g_nov_sides_on) { int s2 = nov_side_of(o->handle); return g_nov_myside >= 0 && s2 >= 0 && s2 == g_nov_myside; }
    return g_nov_myteam >= 0 && o->team == g_nov_myteam; }
static int nov_bip_cls(const NovObj* o) { return nov_friend(o) ? 2 : 1; }
static int nov_team_ok(int mode, int cls) { return mode <= 0 || (cls & mode); }
static int nov_occ_cls(uint32_t veh, const NovObj* objs, int n) { int c = 0; for (int i = 0; i < n; i++) if (objs[i].type == 0 && objs[i].parent == veh) c |= nov_bip_cls(&objs[i]); return c; }
static int nov_owner_cls(uint32_t h, const NovObj* objs, int n) { if (h == 0xFFFFFFFFu) return 0;
    for (int i = 0; i < n; i++) if (objs[i].handle == h) return objs[i].type == 0 ? nov_bip_cls(&objs[i]) : nov_occ_cls(h, objs, n);
    return nov_occ_cls(h, objs, n); }

// ---- lines (hitbox.c: hb_emit_hitbox, the stroke font, hb_proj / hb_seg) -- vertices x, y (NDC), r, g, b, depth ------------
// depth = what the scene's depth buffer holds for this point if nothing is in front of it: Halo 3 is reverse-Z with no far
// plane and near 1/128 (view constants row 2), so depth = (1/128) / w; it is linear in screen space along a line. The shader
// (nrender.m) hides a pixel when the scene there is nearer (0.3 units of slack). 0 = always drawn (g_nov_occ off).
#define NOV_NEAR 0.0078125f
enum { NOV_VSTRIDE = 6 };
static int g_nov_occ;                         // the feature being drawn hides behind the world (its "visible only", zones always)
static float g_nov_label_d;                   // a label's depth (its anchor's), 0 = always drawn
static float nov_dep(float cw) { return g_nov_occ && cw > 0.01f ? NOV_NEAR / cw : 0.0f; }
static int nov_proj_w(const float* vp, const float* w, float* nx, float* ny, float* ow) {
    float cx = w[0] * vp[0] + w[1] * vp[1] + w[2] * vp[2] + vp[3], cy = w[0] * vp[4] + w[1] * vp[5] + w[2] * vp[6] + vp[7], cw = w[0] * vp[12] + w[1] * vp[13] + w[2] * vp[14] + vp[15];
    if (cw < 0.05f) return 0; *nx = cx / cw; *ny = cy / cw; if (ow) *ow = cw; return 1; }
static int nov_proj(const float* vp, const float* w, float* nx, float* ny) { float cw; int r = nov_proj_w(vp, w, nx, ny, &cw); if (r) g_nov_label_d = nov_dep(cw); return r; }
static void nov_vtx(float* d, float x, float y, const float* col, float dep) { d[0] = x; d[1] = y; d[2] = col[0]; d[3] = col[1]; d[4] = col[2]; d[5] = dep; }
static int nov_seg(float* v, int nv, int cap, const float* vp, const float* a, const float* b, const float* col) {
    float ax, ay, bx, by, aw, bw; if (nv + 2 > cap || !nov_proj_w(vp, a, &ax, &ay, &aw) || !nov_proj_w(vp, b, &bx, &by, &bw)) return nv; float* d = v + nv * NOV_VSTRIDE;
    nov_vtx(d, ax, ay, col, nov_dep(aw)); nov_vtx(d + NOV_VSTRIDE, bx, by, col, nov_dep(bw)); return nv + 2; }
static const char* nov_glyph(char c) {   // hitbox.c hb_glyph: segments "x1y1x2y2" on a 4x6 grid
    switch (c) {
    case '0': return "0040404646060600";  case '1': return "202626151030";  case '2': return "06464643430303000040";
    case '3': return "0646464040000343";  case '4': return "060303434640";  case '5': return "46060603034343404000";
    case '6': return "46060600004040434303";  case '7': return "06464620";  case '8': return "00404046460606000343";
    case '9': return "43030306064646404000";  case 'A': return "00040426264444400242";  case 'B': return "000606363645453333034342424131300000";
    case 'C': return "460606000040";  case 'D': return "000606363644444242303000";  case 'E': return "4606060000400333";
    case 'F': return "460606000333";  case 'G': return "46060600004040434323";  case 'H': return "000640460343";
    case 'I': return "064620260040";  case 'J': return "464040000002";  case 'K': return "000603460340";
    case 'L': return "06000040";  case 'M': return "0006062323464640";  case 'N': return "000606404046";
    case 'O': return "0040404646060600";  case 'P': return "0006064646434303";  case 'Q': return "00404046460606002241";
    case 'R': return "00060646464343031340";  case 'S': return "46060603034343404000";  case 'T': return "06462620";
    case 'U': return "060000404046";  case 'V': return "06202046";  case 'W': return "0600002323404046";
    case 'X': return "00460640";  case 'Y': return "062346232320";  case 'Z': return "064646000040";
    case '%': return "004605160616304131404131";  case ':': return "21222425";  case '-': return "0343";  case '.': return "2021";
    case '/': return "0046";  case '+': return "03432125";  case '?': return "05060646464444222221";  case '(': return "3616162020101034";
    default: return ""; } }
static float g_nov_gx, g_nov_gy, g_nov_W = 1920, g_nov_H = 1080, g_nov_scale = 1;   // NDC size of one font-grid unit; the frame in points
static void nov_text_size(int px) { if (px < 4) px = 4; if (px > 60) px = 60; float u = px / 6.0f; g_nov_gx = 2.0f * u / g_nov_W; g_nov_gy = 2.0f * u / g_nov_H; }
static int nov_text(float* v, int nv, int cap, float x, float y, const char* s, const float* col) {
    for (; *s; s++, x += 5 * g_nov_gx) { char ch = *s; if (ch >= 'a' && ch <= 'z') ch -= 32; const char* g = nov_glyph(ch);
        for (; g[0] && g[1] && g[2] && g[3] && nv + 2 <= cap; g += 4) { float* d = v + nv * NOV_VSTRIDE;   // (depth: the label's anchor, nov_proj)
            nov_vtx(d, x + (g[0] - '0') * g_nov_gx, y - (6 - (g[1] - '0')) * g_nov_gy, col, g_nov_label_d);
            nov_vtx(d + NOV_VSTRIDE, x + (g[2] - '0') * g_nov_gx, y - (6 - (g[3] - '0')) * g_nov_gy, col, g_nov_label_d); nv += 2; } }
    return nv; }
static int nov_hitbox(const NovObj* o, const float* vp, float* verts, int nv, int cap) {   // hitbox.c hb_emit_hitbox
    HbGeom* g = hb_geom(o->def); if (!g) return nv;
    int nn = o->nmsz / 52; if (o->nmsz % 52 || nn <= g->maxnode || nn > 128 || o->nmoff < 0x100) return nv;
    static float mt[128 * 13]; if (!hb_read(o->p + o->nmoff, mt, (size_t)nn * 52) || fabsf(mt[0] - 1.0f) > 0.1f) return nv;
    uint8_t sel[256]; memset(sel, 0, sizeof sel);
    if (g->nhreg > 0 && o->rcount > 0 && o->rcount <= 32) { uint8_t pb[32]; if (hb_read(o->p + o->roff + (uint64_t)o->rcount * 8, pb, (size_t)o->rcount))
        for (int i = 0; i < g->nhreg && i < o->rcount; i++) { int pi = pb[i]; if (pi >= g->hnperm[i]) pi = 0; int cr = g->hcreg[i], cp = g->hnperm[i] ? g->hcperm[i][pi] : 0; if (cr >= 0 && cr < 256 && cp >= 0) sel[cr] = (uint8_t)cp; } }
    int head = -1;
    if (o->type == 0 && g->nreg >= 2) { float zs[256] = {0}; int zc[256] = {0}; for (int s = 0; s < g->nseg; s++) { if (g->perm[s] != sel[g->reg[s]]) continue; zs[g->reg[s]] += mt[g->node[s] * 13 + 12]; zc[g->reg[s]]++; }
        float best = -1e30f; for (int r = 0; r < g->nreg; r++) if (zc[r] && zs[r] / zc[r] > best) { best = zs[r] / zc[r]; head = r; } }
    static const float cbody[3] = {1, 0.15f, 0.15f}, chead[3] = {1, 0.95f, 0.1f}, fbody[3] = {0.25f, 0.5f, 1}, fhead[3] = {0.6f, 0.95f, 1}, cveh[3] = {0.3f, 0.75f, 1},
        cwk[4][3] = {{0.3f, 0.75f, 1}, {1, 0.55f, 0.1f}, {1, 0.95f, 0.1f}, {1, 0.3f, 0.85f}};
    static const float cdead[3] = {0.45f, 0.45f, 0.45f};
    int fr = nov_friend(o); const float* cb = o->dead ? cdead : fr ? fbody : cbody, * ch = o->dead ? cdead : fr ? fhead : chead; int wkshow = g_nov_wk;
    for (int s = 0; s < g->nseg && nv + 2 <= cap; s++) { if (g->perm[s] != sel[g->reg[s]]) continue;
        const float* M = mt + g->node[s] * 13, * P = g->pts + s * 6; int wk = g->wk[s]; if (wk && !(wkshow & (wk == 2 ? 1 : wk == 1 ? 2 : 4))) wk = 0;
        const float* col = o->type == 0 ? (g->reg[s] == head ? ch : cb) : (wk < 4 ? cwk[wk] : cveh);
        float q[2][3]; int ok = 1;
        for (int e = 0; e < 2; e++) { const float* v = P + e * 3; float w[3];
            for (int a = 0; a < 3; a++) w[a] = M[10 + a] + M[0] * (v[0] * M[1 + a] + v[1] * M[4 + a] + v[2] * M[7 + a]);
            if (!nov_proj_w(vp, w, &q[e][0], &q[e][1], &q[e][2])) { ok = 0; break; } }
        if (!ok) continue; float* d = verts + nv * NOV_VSTRIDE;
        nov_vtx(d, q[0][0], q[0][1], col, nov_dep(q[0][2])); nov_vtx(d + NOV_VSTRIDE, q[1][0], q[1][1], col, nov_dep(q[1][2])); nv += 2; }
    return nv;
}
typedef struct { uint32_t h; int n; float p[32][3]; double seen; } NovTrail;
static NovTrail g_nov_trails[256];
static void nov_trail_add(uint32_t h, const float* pos, double now) {
    NovTrail* t = 0, * old = &g_nov_trails[0]; for (int i = 0; i < 256; i++) { if (g_nov_trails[i].h == h && g_nov_trails[i].n) { t = &g_nov_trails[i]; break; } if (g_nov_trails[i].seen < old->seen) old = &g_nov_trails[i]; }
    if (!t) { t = old; t->h = h; t->n = 0; }
    if (t->n) { const float* l = t->p[(t->n - 1) & 31]; float dx = pos[0] - l[0], dy = pos[1] - l[1], dz = pos[2] - l[2]; if (dx * dx + dy * dy + dz * dz < 1e-6f) { t->seen = now; return; } }
    memcpy(t->p[t->n & 31], pos, 12); t->n++; t->seen = now; }
// ---- bullets (hitbox.c hb_on_delete): halo3 resolves most bullets inside the tick that fires them, so they are never in
// the object table between frames; object_delete (halo3+0x342ed8, hooked at the wrapper its entry branch leads to) sees each
// one at deletion: +0x1c the impact point, +0x74 velocity, +0x1e8 the start, +0x1b0 distance travelled, +0xC0 the owner.
// The hook runs on the engine thread inside the lifted call: it reads that thread's own TLS directly (pointers checked).
typedef struct { volatile double t; uint32_t def, owner; float pos[3], vel[3], st[3], trav; } NovImp;
// shots and estimated hits (hitbox.c g_hbshot): every deleted projectile with its owner, and the biped whose bounding sphere
// holds its end point (the victim, -1 none) -- the stats snapshot counts them per player (shots, hits, accuracy)
typedef struct { uint32_t owner, victim, def; } NovShot;
static NovShot g_nov_shot[8192]; static volatile unsigned g_nov_shot_n;
static NovImp g_nov_imp[256]; static volatile unsigned g_nov_imp_n;
static volatile unsigned long g_nov_del_calls;
// ---- aim vs hits (2026-11-07; user: "a meter that tells how close to an accurate shot in a hitbox is being made ... a high
// deviation would indicate a trash player, or that lag was too high between players to land hits"). For every deleted bullet
// that flew straight (it travelled no more than its start-to-impact line: no grenades, no bounces) the enemy biped whose
// centre lies nearest its path -- within 12 degrees, in front of where it stopped, on foot, alive -- is its target; that
// biped's pose at this moment (node matrices, region permutations) and its shield + health go into a ring (nov_aim_shot, the
// engine thread). The render thread, which owns the hitbox geometry cache (h3maps.c hb_geom), measures how far the path
// passed from the target's real hitboxes (nov_aim_tick: per collision region and node, the centre and mean radius of its
// posed edges; 0 = through one), and the stats snapshot checks whether the server then took the target's shield / health
// down (st_aim_tick). Everything is as THIS client saw it: a remote player's target sits where this client draws it.
// NHOOK_NO_AIM=1 = off.
static void nov_aim_shot(const uint8_t* arr, const uint8_t* data, const uint8_t* p) {
    static int on = -1; if (on < 0) on = !getenv("NHOOK_NO_AIM"); if (!on) return;
    uint32_t own = *(const uint32_t*)(p + 0xC0); int hi = *(const int32_t*)(arr + 0x3c);
    if (own == 0xFFFFFFFFu || hi <= 0 || hi > 65536) return;
    int oi = own & 0xFFFF; if (oi >= hi) return; const uint8_t* oe = data + (size_t)oi * 0x18;
    if (oe[3] != 0 || *(const uint16_t*)oe != (uint16_t)(own >> 16)) return;   // a biped's shot (not a turret's)
    const uint8_t* op = *(uint8_t* const*)(oe + 0x10); if (!op) return;
    int oteam = *(const int32_t*)(op + 0x174), oside = g_nov_sides_on ? nov_side_of(own) : -1;
    const float* S = (const float*)(p + 0x1e8), * I = (const float*)(p + 0x1c); float d[3] = {I[0] - S[0], I[1] - S[1], I[2] - S[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), trav = *(const float*)(p + 0x1b0);
    if (!(len > 0.5f && len < 2000.f) || trav > len * 1.05f + 0.3f) return;
    float dn[3] = {d[0] / len, d[1] / len, d[2] / len}, best = 0.2126f, bal = 0; const uint8_t* tp = 0; uint32_t th = 0;   // (tan 12 degrees)
    for (int j = 0; j < hi; j++) { const uint8_t* b = data + (size_t)j * 0x18; if (b[3] != 0 || !*(const uint16_t*)b) continue;
        uint32_t bh = ((uint32_t)*(const uint16_t*)b << 16) | (uint32_t)j; if (bh == own) continue; const uint8_t* bp = *(uint8_t* const*)(b + 0x10); if (!bp) continue;
        if ((*(const uint32_t*)(bp + 0x110) >> 2) & 1) continue;                 // dead
        if (*(const uint32_t*)(bp + 0x10) != 0xFFFFFFFFu) continue;              // in a vehicle: the shot meets the vehicle
        if (oside >= 0) { int s2 = nov_side_of(bh); if (s2 >= 0 && s2 == oside) continue; }   // Infection: zombie / human
        else { int t2 = *(const int32_t*)(bp + 0x174); if (oteam >= 0 && t2 == oteam) continue; }
        const float* c = (const float*)(bp + 0x1c); float r = c[3]; if (!(r > 0.05f && r < 5)) continue;
        float v[3] = {c[0] - S[0], c[1] - S[1], c[2] - S[2]}, al = v[0] * dn[0] + v[1] * dn[1] + v[2] * dn[2]; if (al < 0.3f || al > len + r + 0.5f) continue;
        float px = v[0] - dn[0] * al, py = v[1] - dn[1] * al, pz = v[2] - dn[2] * al, q = sqrtf(px * px + py * py + pz * pz) / al;
        if (q < best) { best = q; tp = bp; th = bh; bal = al; } }
    if (!tp) return;
    uint16_t nmsz = *(const uint16_t*)(tp + 0x136), nmoff = *(const uint16_t*)(tp + 0x138); int nn = nmsz / 52; if (nmsz % 52 || nn < 1 || nn > 128 || nmoff < 0x100) return;
    unsigned w = g_nov_aim_w; NovAimRec* a = &g_nov_aim[w % NOV_AIM_N];
    __atomic_store_n(&a->seq, 0, __ATOMIC_RELEASE);                               // (being written: a reader skips it)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); a->t = ts.tv_sec + ts.tv_nsec * 1e-9;
    a->owner = own; a->target = th; a->def = *(const uint32_t*)tp; memcpy(a->s, S, 12); memcpy(a->dn, dn, 12); a->len = len; a->along = bal;
    a->pre = *(const float*)(tp + 0xF4) + *(const float*)(tp + 0xF8); a->nmsz = nmsz; memcpy(a->mt, tp + nmoff, (size_t)nn * 52);
    { int16_t rs = *(const int16_t*)(tp + 0x13a); if ((*(const uint32_t*)(tp + 4) >> 11) & 1) rs -= 0x20; int rc = rs > 0 ? rs / 10 : 0; uint16_t roff = *(const uint16_t*)(tp + 0x13c);
      a->rcount = (uint8_t)(rc > 32 ? 0 : rc); if (a->rcount) memcpy(a->perm, tp + roff + (size_t)a->rcount * 8, a->rcount); }
    __atomic_store_n(&a->seq, w + 1, __ATOMIC_RELEASE); __atomic_store_n(&g_nov_aim_w, w + 1, __ATOMIC_RELEASE); g_nov_aim_n++;
}
// ---- the bullet log (NHOOK_SHOTLOG=<file>, %p = this process's id; 2026-11-08, the user: "what can you tell me about the 'random
// bloom' on both the AR and the BR ... do they seem random? any disadvantages or flaws under certain situations?"): every deleted
// projectile with its owner's state at that moment -- the shooter's aim vector (unit +0x1a0: where it shoots), its velocity
// (+0x74) and the weapon in its hand, the projectile's start (+0x1e8), end (+0x1c) and distance travelled (+0x1b0), the estimated
// victim. halo3 resolves bullets inside the tick that fires them, so the owner's aim here is the aim the shot was fired with
// (within the tick's own update order). On our dedicated server this is the authoritative simulation; on a client it is that
// client's own view. Lines (shim/tools/bloom.py): t owner weapon projectile victim sx sy sz ex ey ez trav ax ay az vx vy vz
typedef struct { double t; uint32_t owner, wdef, pdef, victim; float s[3], e[3], trav, aim[3], vel[3]; } NovSlog;
static NovSlog g_slog[16384]; static volatile unsigned g_slog_w;
static int nov_slog_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("NHOOK_SHOTLOG"); on = e && *e; } return on; }
static void nov_slog_name(char* o, size_t n, uint32_t def) { snprintf(o, n, "%s", def ? hb_tag_name(def) : "-"); for (char* q = o; *q; q++) if (*q == ' ') *q = '_'; }
static void* nov_slog_thread(void* a) {
    (void)a; pthread_setname_np("shim-shotlog"); char path[1024]; const char* e = getenv("NHOOK_SHOTLOG"); size_t j = 0;
    for (const char* q = e; *q && j + 12 < sizeof path; q++) {   // (%p = the process id, %r = NHOOK_ROLE: "server" on our dedicated server, else "client")
        if (q[0] == '%' && q[1] == 'p') { j += (size_t)snprintf(path + j, sizeof path - j, "%d", getpid()); q++; }
        else if (q[0] == '%' && q[1] == 'r') { const char* ro = getenv("NHOOK_ROLE"); j += (size_t)snprintf(path + j, sizeof path - j, "%s", ro && *ro ? ro : "client"); q++; }
        else path[j++] = *q; } path[j] = 0;
    FILE* f = fopen(path, "a"); if (!f) { nlog("[nhook] [shotlog] cannot open %s", path); return 0; }
    nlog("[nhook] [shotlog] every bullet -> %s", path); unsigned r = 0;
    for (;;) { usleep(500000); unsigned w = __atomic_load_n(&g_slog_w, __ATOMIC_ACQUIRE); if (w - r > 16384) r = w - 16384;
        for (; r != w; r++) { const NovSlog* x = &g_slog[r & 16383]; char wn[48], pn[48]; nov_slog_name(wn, sizeof wn, x->wdef); nov_slog_name(pn, sizeof pn, x->pdef);
            fprintf(f, "%.4f %08x %s %s %08x %.3f %.3f %.3f %.3f %.3f %.3f %.2f %.5f %.5f %.5f %.3f %.3f %.3f\n", x->t, x->owner, wn, pn, x->victim, x->s[0], x->s[1], x->s[2],
                    x->e[0], x->e[1], x->e[2], x->trav, x->aim[0], x->aim[1], x->aim[2], x->vel[0], x->vel[1], x->vel[2]); }
        fflush(f); } return 0; }
static void nov_slog_add(const uint8_t* arr, const uint8_t* data, const uint8_t* p, uint32_t own, uint32_t vic) {
    static int started; if (!started) { started = 1; pthread_t th; if (!pthread_create(&th, 0, nov_slog_thread, 0)) pthread_detach(th); }
    int hi = *(const int32_t*)(arr + 0x3c); if (own == 0xFFFFFFFFu || hi <= 0 || hi > 65536) return; int oi = own & 0xFFFF; if (oi >= hi) return;
    const uint8_t* oe = data + (size_t)oi * 0x18; if (oe[3] != 0 || *(const uint16_t*)oe != (uint16_t)(own >> 16)) return;   // (a biped's bullet)
    const uint8_t* op = *(uint8_t* const*)(oe + 0x10); if (!op) return;
    unsigned k = __atomic_load_n(&g_slog_w, __ATOMIC_RELAXED); NovSlog* x = &g_slog[k & 16383];
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); x->t = ts.tv_sec + ts.tv_nsec * 1e-9; x->owner = own; x->victim = vic; x->pdef = *(const uint32_t*)p; x->wdef = 0;
    { int ws = *(const int8_t*)(op + 0x262); uint32_t wh = (ws >= 0 && ws < 4) ? ((const uint32_t*)(op + 0x268))[ws] : 0xFFFFFFFFu;
      if (wh != 0xFFFFFFFFu && (int)(wh & 0xFFFF) < hi) { const uint8_t* we = data + (size_t)(wh & 0xFFFF) * 0x18; const uint8_t* wp = *(uint8_t* const*)(we + 0x10); if (wp && *(const uint16_t*)we == (uint16_t)(wh >> 16)) x->wdef = *(const uint32_t*)wp; } }
    memcpy(x->s, p + 0x1e8, 12); memcpy(x->e, p + 0x1c, 12); memcpy(&x->trav, p + 0x1b0, 4); memcpy(x->aim, op + 0x1a0, 12); memcpy(x->vel, op + 0x74, 12);
    __atomic_store_n(&g_slog_w, k + 1, __ATOMIC_RELEASE); }
static void on_object_delete_body(uint64_t* ctx) {   // (x0: the guest context the app's own object_delete gets -- read through rd: checked)
    g_nov_del_calls++;
    uint64_t c2[2], tls = 0; static uint32_t idx = 0xFFFFFFFFu;
    if (!ctx || rd((uint64_t)(uintptr_t)ctx, c2, 16) || rd((uint64_t)(uintptr_t)ctx + 0x2d8, &tls, 8)) return;
    uint32_t h = (uint32_t)c2[1];
    if (h == 0xFFFFFFFFu || !tls || !g_img) return;
    if (idx == 0xFFFFFFFFu && rd(g_img + 0xa39f9c, &idx, 4)) return; if (idx >= 64) return;
    uint64_t blkv = 0; if (rd(tls + (uint64_t)idx * 8, &blkv, 8) || !blkv) return; uint8_t* blk = (uint8_t*)(uintptr_t)blkv;
    uint8_t* arr = *(uint8_t**)(blk + 0x38); if (!arr || *(uint32_t*)(arr + 0x2c) != 0x64407440 || memcmp(arr, "object", 7)) return;
    uint8_t* data = *(uint8_t**)(arr + 0x48); int i = h & 0xFFFF; if (!data || i >= *(int32_t*)(arr + 0x20)) return;
    uint8_t* e = data + (size_t)i * 0x18; if (e[3] != 5 || *(uint16_t*)e != (uint16_t)(h >> 16)) return;   // projectiles only
    uint8_t* p = *(uint8_t**)(e + 0x10); if (!p) return;
    { uint32_t own = *(uint32_t*)(p + 0xC0), vic = 0xFFFFFFFFu; const float* ip = (const float*)(p + 0x1c); int hi = *(int32_t*)(arr + 0x3c);
      static int noaim = -1; if (noaim < 0) noaim = getenv("NHOOK_NO_AIM") != 0;   // (hits come from the aim meter's hitbox test; this pass only without it --
      extern volatile unsigned long g_nr_frames;                                     //  or without frames: a dedicated server's aim meter never runs)
      if ((noaim || !g_nr_frames) && own != 0xFFFFFFFFu && hi > 0 && hi <= 65536) for (int j = 0; j < hi; j++) { uint8_t* b = data + (size_t)j * 0x18; if (b[3] != 0 || !*(uint16_t*)b) continue;
          uint32_t bh = ((uint32_t)*(uint16_t*)b << 16) | (uint32_t)j; if (bh == own) continue; uint8_t* bp = *(uint8_t**)(b + 0x10); if (!bp) continue;
          const float* c = (const float*)(bp + 0x1c); float r2 = c[3] + 0.1f, dx = ip[0] - c[0], dy = ip[1] - c[1], dz = ip[2] - c[2];
          if (r2 > 0 && r2 < 5 && dx * dx + dy * dy + dz * dz <= r2 * r2) { vic = bh; break; } }
      unsigned q = __atomic_load_n(&g_nov_shot_n, __ATOMIC_RELAXED) & 8191; g_nov_shot[q].owner = own; g_nov_shot[q].victim = vic; g_nov_shot[q].def = *(uint32_t*)p;
      __atomic_store_n(&g_nov_shot_n, g_nov_shot_n + 1, __ATOMIC_RELEASE);   // (one writer: the engine thread)
      if (nov_slog_on()) nov_slog_add(arr, data, p, own, vic); }
    if (!g_hc_off[HC_AIM]) { uint64_t t1 = hc_now(); nov_aim_shot(arr, data, p); hc_add(HC_AIM, t1); }
    if (!nov_feat(NOV_TRC)) return;   // (the tracer record: only while tracers are on)
    unsigned k = __atomic_fetch_add(&g_nov_imp_n, 1, __ATOMIC_RELAXED) & 255; NovImp* r = &g_nov_imp[k];
    r->def = *(uint32_t*)p; r->owner = *(uint32_t*)(p + 0xC0); memcpy(r->pos, p + 0x1c, 12); memcpy(r->vel, p + 0x74, 12); memcpy(r->st, p + 0x1e8, 12); memcpy(&r->trav, p + 0x1b0, 4);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double tv = ts.tv_sec + ts.tv_nsec * 1e-9; __atomic_store(&r->t, &tv, __ATOMIC_RELEASE);
}
static void nov_aim_tick_body(void) {   // nrender.m, every frame (the render thread owns hb_geom's cache)
    static unsigned r; static NovAimRec a; unsigned w = __atomic_load_n(&g_nov_aim_w, __ATOMIC_ACQUIRE); if (w - r > NOV_AIM_N) r = w - NOV_AIM_N;
    for (; r != w; r++) {
        NovAimRec* src = &g_nov_aim[r % NOV_AIM_N]; if (__atomic_load_n(&src->seq, __ATOMIC_ACQUIRE) != r + 1) continue;
        memcpy(&a, src, sizeof a); if (__atomic_load_n(&src->seq, __ATOMIC_ACQUIRE) != r + 1 || a.seq != r + 1) continue;   // (overwritten meanwhile)
        HbGeom* g = hb_geom(a.def); int nn = a.nmsz / 52; if (!g || nn <= g->maxnode || fabsf(a.mt[0] - 1.0f) > 0.1f) continue;
        uint8_t sel[256]; memset(sel, 0, sizeof sel);
        if (g->nhreg > 0 && a.rcount > 0) for (int i = 0; i < g->nhreg && i < a.rcount; i++) { int pi = a.perm[i]; if (pi >= g->hnperm[i]) pi = 0;
            int cr = g->hcreg[i], cp = g->hnperm[i] ? g->hcperm[i][pi] : 0; if (cr >= 0 && cr < 256 && cp >= 0) sel[cr] = (uint8_t)cp; }
        // per (region, node): the centre and the mean radius of its posed edge end points
        enum { NG = 64 }; int gk[NG], gn[NG], ng = 0; float gc[NG][3], gp[NG * 0 + 1][1]; (void)gp; static float pts[4096][3]; static int pg[4096]; int np2 = 0;
        float zs[256] = {0}; int zc[256] = {0};
        for (int s = 0; s < g->nseg && np2 + 2 <= 4096; s++) { if (g->perm[s] != sel[g->reg[s]]) continue;
            const float* M = a.mt + g->node[s] * 13, * P = g->pts + s * 6; int key = g->reg[s] * 256 + g->node[s], gi = -1;
            for (int q = 0; q < ng; q++) if (gk[q] == key) { gi = q; break; }
            if (gi < 0) { if (ng >= NG) continue; gi = ng++; gk[gi] = key; gn[gi] = 0; gc[gi][0] = gc[gi][1] = gc[gi][2] = 0; }
            for (int e = 0; e < 2; e++) { const float* v = P + e * 3; float* o = pts[np2];
                for (int k = 0; k < 3; k++) o[k] = M[10 + k] + M[0] * (v[0] * M[1 + k] + v[1] * M[4 + k] + v[2] * M[7 + k]);
                pg[np2++] = gi; gc[gi][0] += o[0]; gc[gi][1] += o[1]; gc[gi][2] += o[2]; gn[gi]++; }
            zs[g->reg[s]] += M[12]; zc[g->reg[s]]++; }
        if (!ng) continue;
        int head = -1; { float hb = -1e30f; for (int q = 0; q < g->nreg && q < 256; q++) if (zc[q] && zs[q] / zc[q] > hb) { hb = zs[q] / zc[q]; head = q; } }
        float rad[NG] = {0};
        for (int q = 0; q < ng; q++) { gc[q][0] /= gn[q]; gc[q][1] /= gn[q]; gc[q][2] /= gn[q]; }
        for (int i = 0; i < np2; i++) { const float* o = pts[i], * c = gc[pg[i]]; rad[pg[i]] += sqrtf((o[0] - c[0]) * (o[0] - c[0]) + (o[1] - c[1]) * (o[1] - c[1]) + (o[2] - c[2]) * (o[2] - c[2])); }
        float miss = 1e30f; int mg = -1;
        for (int q = 0; q < ng; q++) { rad[q] /= gn[q]; float v[3] = {gc[q][0] - a.s[0], gc[q][1] - a.s[1], gc[q][2] - a.s[2]}, t = v[0] * a.dn[0] + v[1] * a.dn[1] + v[2] * a.dn[2];
            if (t < 0) t = 0; if (t > a.len + 0.3f) t = a.len + 0.3f;
            float px = v[0] - a.dn[0] * t, py = v[1] - a.dn[1] * t, pz = v[2] - a.dn[2] * t, sf = sqrtf(px * px + py * py + pz * pz) - rad[q];
            if (sf < miss) { miss = sf; mg = q; } }
        if (mg < 0) continue;
        unsigned k = g_nov_aimr_w; NovAimRes* o = &g_nov_aimr[k % NOV_AIMR_N];
        o->t = a.t; o->owner = a.owner; o->target = a.target; o->along = a.along; o->pre = a.pre; o->on = miss <= 0; o->head = gk[mg] / 256 == head;
        o->err = miss <= 0 ? 0 : atan2f(miss, a.along > 0.3f ? a.along : 0.3f) * 57.29578f;
        { static int lg = -1; if (lg < 0) lg = getenv("NHOOK_AIM_LOG") != 0;   // NHOOK_AIM_LOG=1: each measured shot (aim test diagnostics, 2026-11-08)
          if (lg) { const float* c = gc[mg]; float v[3] = {c[0] - a.s[0], c[1] - a.s[1], c[2] - a.s[2]}, t = v[0] * a.dn[0] + v[1] * a.dn[1] + v[2] * a.dn[2];
              float cp[3] = {a.s[0] + a.dn[0] * t, a.s[1] + a.dn[1] * t, a.s[2] + a.dn[2] * t};
              nlog("[nhook] [aim] shot from (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) len %.2f | nearest part region %d node %d centre (%.2f %.2f %.2f) r %.2f, path passes (%.2f %.2f %.2f): miss %.2f u = %.2f deg%s | target root (%.2f %.2f %.2f), %d parts",
                   a.s[0], a.s[1], a.s[2], a.dn[0], a.dn[1], a.dn[2], a.len, gk[mg] / 256, gk[mg] % 256, c[0], c[1], c[2], rad[mg], cp[0], cp[1], cp[2], miss, o->err, miss <= 0 ? " ON" : "",
                   a.mt[10], a.mt[11], a.mt[12], ng); } }
        __atomic_store_n(&g_nov_aimr_w, k + 1, __ATOMIC_RELEASE); g_nov_aim_done++;
    }
}
void nov_aim_tick(void) { if (g_hc_off[HC_AIMT]) return; uint64_t t0 = hc_now(); nov_aim_tick_body(); hc_add(HC_AIMT, t0); }
static void on_object_delete(uint64_t* ctx) { if (g_hc_off[HC_DEL]) return; uint64_t t0 = hc_now(); on_object_delete_body(ctx); hc_add(HC_DEL, t0); }   // (timed: nhcost.h)
static const float g_nov_teamcol[8][3] = {{1, 0.3f, 0.25f}, {0.35f, 0.6f, 1}, {0.35f, 1, 0.35f}, {1, 0.6f, 0.15f}, {0.75f, 0.4f, 1}, {1, 0.9f, 0.2f}, {0.75f, 0.55f, 0.35f}, {1, 0.55f, 0.8f}};

// ---- zones (hitbox.c hbz_load / hbz_outline / hbz_draw, the loader's line mode): kill volumes (red) and safe zones (green)
// from the scenario's trigger volumes (scnr +0x258, kill list +0x484 / a kill index at +0x40, safe list +0x490), soft
// ceilings from each structure BSP's design tag (scnr +0x18 -> sddt +0x10: acceleration orange, soft kill pink, slip yellow)
// and invisible walls (maptop/<map>.inv.bin, re-tools/h3inv.py; light cyan) as outlines. Through walls: the per-pixel
// hiding needs the frame's depth buffer, which the app keeps to itself. Kinds = bits of g_hbz_show (the page's chips).
typedef struct { float c[8][3]; int kind; } NzBox;
typedef struct { float v[3][3]; int type, group; } NzTri;
typedef struct { float a[3], b[3]; int type; } NzSeg;
typedef struct { int32_t k[6]; int tri, e; } NzEdge;
static int nz_edge_cmp(const void* x, const void* y) { return memcmp(((const NzEdge*)x)->k, ((const NzEdge*)y)->k, sizeof(int32_t) * 6); }
static struct { char path[1024]; NzBox* box; int nbox; NzTri* tri; int ntri; NzSeg* seg; int nseg; } g_nz;
static volatile int g_hbz_show = -1;
static int nz_show(void) { if (g_hbz_show < 0) { const char* e = getenv("NHOOK_HB_ZONES_SHOW"); g_hbz_show = e ? (int)strtol(e, 0, 0) : 63; } return g_hbz_show; }
static void nz_outline(void) {
    free(g_nz.seg); g_nz.seg = 0; g_nz.nseg = 0; if (!g_nz.ntri) return;
    NzEdge* ed = malloc((size_t)g_nz.ntri * 3 * sizeof *ed); g_nz.seg = malloc((size_t)g_nz.ntri * 3 * sizeof *g_nz.seg); if (!ed || !g_nz.seg) { free(ed); return; }
    int ne = 0; for (int t = 0; t < g_nz.ntri; t++) for (int e = 0; e < 3; e++) { const float* p = g_nz.tri[t].v[e], * q = g_nz.tri[t].v[(e + 1) % 3]; int32_t a[3], b[3];
        for (int r = 0; r < 3; r++) { a[r] = (int32_t)lrintf(p[r] * 100); b[r] = (int32_t)lrintf(q[r] * 100); }
        int sw = memcmp(a, b, 12) > 0; NzEdge* x = &ed[ne++]; memcpy(x->k, sw ? b : a, 12); memcpy(x->k + 3, sw ? a : b, 12); x->tri = t; x->e = e; }
    qsort(ed, (size_t)ne, sizeof *ed, nz_edge_cmp);
    for (int i = 0; i < ne;) { int j = i + 1; while (j < ne && !memcmp(ed[j].k, ed[i].k, sizeof(int32_t) * 6)) j++;
        int interior = 0; for (int a2 = i; a2 < j && !interior; a2++) for (int b2 = a2 + 1; b2 < j; b2++) if (g_nz.tri[ed[a2].tri].group == g_nz.tri[ed[b2].tri].group) { interior = 1; break; }
        if (!interior) { const NzTri* z = &g_nz.tri[ed[i].tri]; NzSeg* sg = &g_nz.seg[g_nz.nseg++]; memcpy(sg->a, z->v[ed[i].e], 12); memcpy(sg->b, z->v[(ed[i].e + 1) % 3], 12); sg->type = z->type; }
        i = j; }
    free(ed);
}
static void nz_load(const char* path) {
    if (!strcmp(g_nz.path, path)) return; free(g_nz.box); free(g_nz.tri); free(g_nz.seg); memset(&g_nz, 0, sizeof g_nz); snprintf(g_nz.path, sizeof g_nz.path, "%s", path);
    HbMap* m = hbm_get(path); if (!m) return; int si = -1; for (int i = 0; i < m->ntags; i++) if (m->grp[i] == 'scnr') { si = i; break; }
    uint8_t d[0x4A0]; if (si < 0 || !hbm_read(m, (uint64_t)m->tp[si] << 2, d, sizeof d)) return;
    uint8_t* kl = 0, * sl = 0, * tv = 0; int nk = hbm_block(m, d, 0x484, 2, &kl, 4096), ns = hbm_block(m, d, 0x490, 2, &sl, 4096), nt = hbm_block(m, d, 0x258, 0x44, &tv, 4096);
    g_nz.box = calloc(nt > 0 ? nt : 1, sizeof(NzBox));
    for (int i = 0; i < nt && g_nz.box; i++) { const uint8_t* e = tv + i * 0x44; int kind = -1;
        for (int k = 0; k < nk; k++) if (*(int16_t*)(kl + k * 2) == i) kind = 0; if (kind < 0 && *(int16_t*)(e + 0x40) >= 0) kind = 0;
        if (kind < 0) for (int k = 0; k < ns; k++) if (*(int16_t*)(sl + k * 2) == i) kind = 1;
        if (kind < 0) continue; const float* f = (const float*)(e + 0x0C), * u = (const float*)(e + 0x18), * p = (const float*)(e + 0x24), * x = (const float*)(e + 0x30);
        float l[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]}; NzBox* b = &g_nz.box[g_nz.nbox++]; b->kind = kind;
        for (int c = 0; c < 8; c++) for (int r = 0; r < 3; r++) b->c[c][r] = p[r] + ((c & 1) ? x[0] * f[r] : 0) + ((c & 2) ? x[1] * l[r] : 0) + ((c & 4) ? x[2] * u[r] : 0); }
    free(kl); free(sl); free(tv);
    uint8_t* bs = 0; int nb = hbm_block(m, d, 0x18, 0x6C, &bs, 64), cap = 0;
    for (int b = 0; b < nb; b++) { uint32_t sd = *(uint32_t*)(bs + b * 0x6C + 0x10 + 12); if (sd == 0xFFFFFFFFu || (int)(sd & 0xFFFF) >= m->ntags) continue;
        uint8_t sm[0x40]; if (!hbm_read(m, (uint64_t)m->tp[sd & 0xFFFF] << 2, sm, sizeof sm)) continue;
        uint8_t* sc = 0; int nsc = hbm_block(m, sm, 0x10, 0x14, &sc, 256);
        for (int c = 0; c < nsc; c++) { int ty = *(int16_t*)(sc + c * 0x14 + 4); uint8_t* tr = 0; int ntr = hbm_block(m, sc + c * 0x14, 8, 0x44, &tr, 65536); static int grp; grp++;
            for (int t = 0; t < ntr && g_nz.ntri < (1 << 20); t++) { if (g_nz.ntri >= cap) { int nc = cap ? cap * 2 : 1024; NzTri* n2 = realloc(g_nz.tri, (size_t)nc * sizeof(NzTri)); if (!n2) break; g_nz.tri = n2; cap = nc; }
                NzTri* z = &g_nz.tri[g_nz.ntri++]; z->type = ty; z->group = grp; memcpy(z->v, tr + t * 0x44 + 0x20, 36); }
            free(tr); }
        free(sc); }
    free(bs);
    { const char* h = getenv("HOME"); const char* bn = strrchr(path, '/'); char nm[256]; snprintf(nm, sizeof nm, "%s", bn ? bn + 1 : path); char* dot = strrchr(nm, '.'); if (dot) *dot = 0;
      char ip[1400]; snprintf(ip, sizeof ip, "%s/Library/Application Support/ReclaimerMac/maptop/%s.inv.bin", h ? h : "", nm);
      FILE* f = fopen(ip, "rb"); uint8_t hd[8]; uint32_t n = 0;
      if (f && fread(hd, 1, 8, f) == 8 && !memcmp(hd, "RIW1", 4) && (n = *(uint32_t*)(hd + 4)) > 0 && n < 2000000) {
          NzTri* n2 = realloc(g_nz.tri, (size_t)(g_nz.ntri + n) * sizeof(NzTri));
          if (n2) { g_nz.tri = n2; float v[9]; for (uint32_t i = 0; i < n && fread(v, 4, 9, f) == 9; i++) { NzTri* z = &g_nz.tri[g_nz.ntri++]; z->type = 3; z->group = 1 << 30; memcpy(z->v, v, 36); } } }
      if (f) fclose(f); }
    nz_outline();
    int nkb = 0; for (int i = 0; i < g_nz.nbox; i++) nkb += g_nz.box[i].kind == 0;
    const char* bn = strrchr(path, '/'); nlog("[nhook] zones %s: %d kill volumes, %d safe zones, %d ceiling / invisible-wall triangles (%d outline edges)", bn ? bn + 1 : path, nkb, g_nz.nbox - nkb, g_nz.ntri, g_nz.nseg);
}
static int nov_seg_clip(float* v, int nv, int cap, const float* vp, const float* a, const float* b, const float* col) {   // hitbox.c hb_seg_clip
    float ca[3], cb[3]; for (int r = 0; r < 3; r++) { int i = r == 2 ? 12 : r * 4; ca[r] = a[0] * vp[i] + a[1] * vp[i + 1] + a[2] * vp[i + 2] + vp[i + 3]; cb[r] = b[0] * vp[i] + b[1] * vp[i + 1] + b[2] * vp[i + 2] + vp[i + 3]; }
    const float E = 0.05f; if (nv + 2 > cap || (ca[2] < E && cb[2] < E)) return nv;
    if (ca[2] < E) { float t = (E - ca[2]) / (cb[2] - ca[2]); for (int r = 0; r < 3; r++) ca[r] += t * (cb[r] - ca[r]); }
    else if (cb[2] < E) { float t = (E - cb[2]) / (ca[2] - cb[2]); for (int r = 0; r < 3; r++) cb[r] += t * (ca[r] - cb[r]); }
    float* d = v + nv * NOV_VSTRIDE; nov_vtx(d, ca[0] / ca[2], ca[1] / ca[2], col, nov_dep(ca[2])); nov_vtx(d + NOV_VSTRIDE, cb[0] / cb[2], cb[1] / cb[2], col, nov_dep(cb[2])); return nv + 2;
}
static int nz_draw(float* v, int nv, int cap, const float* vp) {
    char mp[1024]; if (!hb_current_map(mp, sizeof mp)) return nv; nz_load(mp);
    static const float ckill[3] = {1, 0.15f, 0.15f}, csafe[3] = {0.2f, 1, 0.35f}, csoft[4][3] = {{1, 0.6f, 0.1f}, {1, 0.25f, 0.65f}, {1, 0.92f, 0.2f}, {0.45f, 0.95f, 1}};
    const float* cam = vp + 28; int show = nz_show(); nov_text_size(g_nov_px[NOV_ZON] > 0 ? g_nov_px[NOV_ZON] : 7);
    for (int i = 0; i < g_nz.nbox; i++) { const NzBox* b = &g_nz.box[i]; if (!(show & (b->kind ? 2 : 1))) continue; const float* col = b->kind ? csafe : ckill;
        for (int c = 0; c < 8; c++) for (int bit = 1; bit < 8; bit <<= 1) if (!(c & bit)) nv = nov_seg_clip(v, nv, cap, vp, b->c[c], b->c[c | bit], col);
        float ctr[3] = {(b->c[0][0] + b->c[7][0]) / 2, (b->c[0][1] + b->c[7][1]) / 2, (b->c[0][2] + b->c[7][2]) / 2}, tx, ty, dx = ctr[0] - cam[0], dy = ctr[1] - cam[1], dz = ctr[2] - cam[2];
        if (dx * dx + dy * dy + dz * dz < 150.f * 150.f && nov_proj(vp, ctr, &tx, &ty)) { const char* t = b->kind ? "SAFE ZONE" : "KILL ZONE"; nv = nov_text(v, nv, cap, tx - strlen(t) * 2.5f * g_nov_gx, ty, t, col); } }
    for (int i = 0; i < g_nz.nseg; i++) { const NzSeg* z = &g_nz.seg[i]; int ty = z->type >= 0 && z->type < 4 ? z->type : 1; if (!(show & (4 << ty))) continue;
        nv = nov_seg_clip(v, nv, cap, vp, z->a, z->b, csoft[ty]); }
    return nv;
}

// The frame's overlay lines (nrender.m, the app's render thread, once per presented frame): w x h = the frame in pixels,
// scale = pixels per point. Returns the vertex count (pairs = lines), 0 = nothing to draw.
static pthread_mutex_t g_nov_mx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_hc_part[HC_N];   // this frame's ticks per overlay feature (nov_build_body)
#define HC_PART(k, stmt) do { uint64_t t_ = hc_now(); stmt; g_hc_part[k] += hc_now() - t_; } while (0)
static int nov_build_body(float* verts, int cap, int w, int h, float scale) {
    int want = nov_wanted(); g_view_wanted = want; if (want) nov_view_hook_want();   // (the camera hook: installed on first use, idle while off)
    if (!want || !g_gameplay_t0 || w < 16 || h < 16) return 0;
    float vp[32]; if (!nov_camera(vp, (float)w / (float)h)) return 0;
    pthread_mutex_lock(&g_nov_mx);
    g_nov_scale = scale > 0 ? scale : 1; g_nov_W = w / g_nov_scale; g_nov_H = h / g_nov_scale;
    int fh = nov_feat(NOV_HIT), ft = nov_feat(NOV_TRC), fi = nov_feat(NOV_INF), fw = nov_feat(NOV_ITM), fveh = nov_feat(NOV_VEH); double now = mono_s();
    static NovObj objs[2048]; pthread_mutex_lock(&g_nov_collect_mx); int n = nov_collect(objs, 2048), nv = 0; pthread_mutex_unlock(&g_nov_collect_mx); const float* cam = vp + 28;
    for (int i = 0; i < n; i++) { const NovObj* o = &objs[i]; if (o->type) continue; float dx = o->c[0] - cam[0], dy = o->c[1] - cam[1], dz = o->c[2] - cam[2];
        if (dx * dx + dy * dy + dz * dz < (o->r + 0.3f) * (o->r + 0.3f)) { g_nov_myteam = o->team; g_nov_myside = nov_side_of(o->handle); break; } }
    static const float cwhite[3] = {1, 1, 1}, cgrey[3] = {0.7f, 0.7f, 0.7f}, ctrace[3] = {1, 0.95f, 0.55f}, cvel[3] = {1, 0.5f, 0.1f}, citem[3] = {0.3f, 1, 0.45f}, cequip[3] = {0.3f, 0.9f, 1};
    for (int i = 0; i < n; i++) { const NovObj* o = &objs[i]; int veh = o->type == 1 || o->type == 12;
        float dx = o->c[0] - cam[0], dy = o->c[1] - cam[1], dz = o->c[2] - cam[2]; int self = dx * dx + dy * dy + dz * dz < (o->r + 0.3f) * (o->r + 0.3f);
        if (o->type == 5) { if (!ft || (g_nov_team[NOV_TRC] > 0 && !nov_team_ok(g_nov_team[NOV_TRC], nov_owner_cls(o->owner, objs, n)))) continue; nov_trail_add(o->handle, o->c, now); continue; }
        if (o->type == 2 || o->type == 3) { uint64_t ti_ = hc_now();
            if (fw && o->parent == 0xFFFFFFFFu) { float a[3] = {o->c[0] - 0.15f, o->c[1], o->c[2]}, b[3] = {o->c[0] + 0.15f, o->c[1], o->c[2]}, c2[3] = {o->c[0], o->c[1] - 0.15f, o->c[2]}, d2[3] = {o->c[0], o->c[1] + 0.15f, o->c[2]};
                g_nov_occ = g_nov_vis[NOV_ITM]; const float* col = o->type == 2 ? citem : cequip; nv = nov_seg(verts, nv, cap, vp, a, b, col); nv = nov_seg(verts, nv, cap, vp, c2, d2, col);
                float tx, ty; if (nov_proj(vp, o->c, &tx, &ty)) { nov_text_size(g_nov_px[NOV_ITM]); const char* nm = hb_tag_name(o->def); nv = nov_text(verts, nv, cap, tx - strlen(nm) * 2.5f * g_nov_gx, ty + 8 * g_nov_gy, nm, col); } }
            g_hc_part[HC_OV_ITM] += hc_now() - ti_; continue; }
        if (o->type >= 6 && o->type <= 10) {   // NHOOK_OV_DEBUG: an axis cross through the bounding centre, one radius each way
            static const float cdbg[3] = {1, 0.2f, 1};
            g_nov_occ = getenv("NHOOK_OV_DEBUG_OCC") != 0;
            for (int a = 0; a < 3; a++) { float p0[3] = {o->c[0], o->c[1], o->c[2]}, p1[3] = {o->c[0], o->c[1], o->c[2]}; p0[a] -= o->r; p1[a] += o->r; nv = nov_seg(verts, nv, cap, vp, p0, p1, cdbg); }
            continue; }
        if (self) continue;
        if (veh) { g_nov_occ = g_nov_vis[NOV_VEH]; if (fveh && (g_nov_team[NOV_VEH] <= 0 || nov_team_ok(g_nov_team[NOV_VEH], nov_occ_cls(o->handle, objs, n)))) HC_PART(HC_OV_VEH, nv = nov_hitbox(o, vp, verts, nv, cap)); continue; }
        if (o->type != 0) continue;
        int cls = nov_bip_cls(o);
        g_nov_occ = g_nov_vis[NOV_HIT];
        // dead bodies: no hitboxes (a body lying on the ground drew as red boxes: clutter; 2026-11-07), NHOOK_HB_DEAD=1 = grey
        static int deadon = -1; if (deadon < 0) deadon = getenv("NHOOK_HB_DEAD") && atoi(getenv("NHOOK_HB_DEAD"));
        if (fh && nov_team_ok(g_nov_team[NOV_HIT], cls) && (!o->dead || deadon)) HC_PART(HC_OV_HIT, nv = nov_hitbox(o, vp, verts, nv, cap));
        if (fi && nov_team_ok(g_nov_team[NOV_INF], cls)) { uint64_t tf_ = hc_now();
            g_nov_occ = g_nov_vis[NOV_INF]; float top[3] = {o->c[0], o->c[1], o->c[2] + o->r + 0.12f}, tx, ty; if (!nov_proj(vp, top, &tx, &ty)) continue;
            nov_text_size(g_nov_px[NOV_INF]);
            const float* tc = o->team >= 0 && o->team < 8 ? g_nov_teamcol[o->team] : cwhite;
            char l1[48], l2[80], l3[48]; l3[0] = 0;
            if (o->dead) snprintf(l1, sizeof l1, "DEAD"); else snprintf(l1, sizeof l1, "S%d H%d", (int)(o->shield * 100.f + 0.5f), (int)(o->health * 100.f + 0.5f));
            uint32_t wh = (o->wslot >= 0 && o->wslot < 4) ? o->weapons[o->wslot] : 0xFFFFFFFFu; uint64_t wd = nov_datum(wh); uint32_t wdef = 0; if (wd) hb_read(wd, &wdef, 4);
            { int k2 = snprintf(l2, sizeof l2, "%s", wd ? hb_tag_name(wdef) : "-");
              for (int q = 0; q < 4; q++) { if (q == o->wslot) continue; uint64_t od = nov_datum(o->weapons[q]); uint32_t odef = 0; if (!od || !hb_read(od, &odef, 4)) continue;
                  snprintf(l2 + k2, sizeof l2 - k2, " / %s", hb_tag_name(odef)); break; } }
            int g = o->gren[0] + o->gren[1] + o->gren[2] + o->gren[3], k3 = 0;
            if (g > 0) k3 += snprintf(l3 + k3, sizeof l3 - k3, "G%d ", g);
            if (o->camo > 0.05f) k3 += snprintf(l3 + k3, sizeof l3 - k3, "CAMO %d ", (int)(o->camo * 100.f + 0.5f));
            if (o->seat >= 0) k3 += snprintf(l3 + k3, sizeof l3 - k3, "VEH");
            float lh = 8 * g_nov_gy, y = ty + 3 * lh;
            nv = nov_text(verts, nv, cap, tx - strlen(l1) * 2.5f * g_nov_gx, y, l1, tc); y -= lh;
            nv = nov_text(verts, nv, cap, tx - strlen(l2) * 2.5f * g_nov_gx, y, l2, cwhite); y -= lh;
            if (l3[0]) nv = nov_text(verts, nv, cap, tx - strlen(l3) * 2.5f * g_nov_gx, y, l3, o->camo > 0.05f ? cequip : cgrey); g_hc_part[HC_OV_INF] += hc_now() - tf_; }
    }
    uint64_t tt_ = hc_now(); if (ft) { g_nov_occ = g_nov_vis[NOV_TRC];
        for (int t = 0; t < 256; t++) { NovTrail* tr = &g_nov_trails[t]; if (!tr->n) continue; if (now - tr->seen > 0.6) { tr->n = 0; continue; }
            int cnt = tr->n < 32 ? tr->n : 32; for (int k = 1; k < cnt; k++) nv = nov_seg(verts, nv, cap, vp, tr->p[(tr->n - 1 - k) & 31], tr->p[(tr->n - k) & 31], ctrace); }
        for (int i = 0; i < n; i++) { const NovObj* o = &objs[i]; if (o->type != 5) continue; float sp = sqrtf(o->vel[0] * o->vel[0] + o->vel[1] * o->vel[1] + o->vel[2] * o->vel[2]); if (sp < 1e-4f) continue;
            if (g_nov_team[NOV_TRC] > 0 && !nov_team_ok(g_nov_team[NOV_TRC], nov_owner_cls(o->owner, objs, n))) continue;
            float e[3] = {o->c[0] + o->vel[0] / sp * 1.5f, o->c[1] + o->vel[1] / sp * 1.5f, o->c[2] + o->vel[2] / sp * 1.5f}; nv = nov_seg(verts, nv, cap, vp, o->c, e, cvel); }
        unsigned nimp = __atomic_load_n(&g_nov_imp_n, __ATOMIC_ACQUIRE);   // bullets seen at deletion: start -> impact, the last 0.5 s
        for (unsigned k = 0; k < 256 && k < nimp; k++) { NovImp* r = &g_nov_imp[(nimp - 1 - k) & 255]; double t; __atomic_load(&r->t, &t, __ATOMIC_ACQUIRE); if (now - t > 0.5) continue;
            if (g_nov_team[NOV_TRC] > 0 && !nov_team_ok(g_nov_team[NOV_TRC], nov_owner_cls(r->owner, objs, n))) continue;
            float dx = r->pos[0] - r->st[0], dy = r->pos[1] - r->st[1], dz = r->pos[2] - r->st[2], dd = sqrtf(dx * dx + dy * dy + dz * dz), a[3] = {r->st[0], r->st[1], r->st[2]};
            if (!(r->trav > 0.0f && fabsf(dd - r->trav) < 0.25f * r->trav + 0.5f)) {   // start not recorded: 3 units back along the velocity
                float sp = sqrtf(r->vel[0] * r->vel[0] + r->vel[1] * r->vel[1] + r->vel[2] * r->vel[2]); memcpy(a, r->pos, 12); if (sp > 1e-5f) for (int q = 0; q < 3; q++) a[q] -= r->vel[q] / sp * 3.0f; }
            nv = nov_seg_clip(verts, nv, cap, vp, a, r->pos, ctrace);
            float x0[3] = {r->pos[0] - 0.05f, r->pos[1], r->pos[2]}, x1[3] = {r->pos[0] + 0.05f, r->pos[1], r->pos[2]}; nv = nov_seg_clip(verts, nv, cap, vp, x0, x1, cvel); }
    }
    g_hc_part[HC_OV_TRC] += hc_now() - tt_;
    if (nov_feat(NOV_ZON)) { g_nov_occ = 1; HC_PART(HC_OV_ZON, nv = nz_draw(verts, nv, cap, vp)); }   // zones: always hidden behind the world (the loader's)
    g_nov_occ = 0;
    pthread_mutex_unlock(&g_nov_mx);
    return nv;
}
void nov_idle(void) { g_view_wanted = 0; }   // nrender.m: a frame without the overlay (the camera hook's handler stays idle)
int nov_build(float* verts, int cap, int w, int h, float scale) {   // (timed: nhcost.h; a feature's own parts apart)
    uint64_t t0 = hc_now(); memset(g_hc_part, 0, sizeof g_hc_part); int nv = nov_build_body(verts, cap, w, h, scale); uint64_t all = hc_now() - t0, parts = 0;
    static const int fk[6] = {HC_OV_HIT, HC_OV_VEH, HC_OV_TRC, HC_OV_INF, HC_OV_ITM, HC_OV_ZON}, fb[6] = {NOV_HIT, NOV_VEH, NOV_TRC, NOV_INF, NOV_ITM, NOV_ZON};
    for (int i = 0; i < 6; i++) if (nov_feat(fb[i])) { hc_add_ticks(fk[i], g_hc_part[fk[i]]); __atomic_fetch_add(&g_hc[fk[i]].calls, 1, __ATOMIC_RELAXED); parts += g_hc_part[fk[i]]; }
    hc_add_ticks(HC_OV, all > parts ? all - parts : 0); __atomic_fetch_add(&g_hc[HC_OV].calls, 1, __ATOMIC_RELAXED);
    return nv;
}

// ---- the page's controls and the snapshot's overlay state (stats.c st_control / st_feat / st_overlay_features) -------
static int nov_control(const char* act, const char* qs) {
    nov_init();
    if (!strcmp(act, "visible")) { int v = !g_nov_vis[NOV_HIT]; for (int f = 0; f < NOV_N; f++) g_nov_vis[f] = v; nlog("[nhook] page: visible only %s (all)", v ? "on" : "off"); return 1; }
    for (int f = 0; f < NOV_N; f++) if (!strcmp(act, g_nov_name[f])) { if (f == NOV_VIS) return 0;
        __atomic_fetch_xor(&g_nov_on, 1u << f, __ATOMIC_ACQ_REL); nlog("[nhook] page: %s %s", g_nov_name[f], nov_feat(f) ? "on" : "off"); return 1; }
    if (!strcmp(act, "teams")) { int m = -1; for (int f = 0; f < NOV_N; f++) if (g_nov_team[f] >= 0) { if (m < 0) m = (g_nov_team[f] + 1) % 3; g_nov_team[f] = m; }
        nlog("[nhook] page: showing %s", m == 1 ? "enemies only" : m == 2 ? "friends only" : "all"); return 1; }
    if (!strcmp(act, "textup") || !strcmp(act, "textdown")) { int d = act[4] == 'u' ? 1 : -1; for (int f = 0; f < NOV_N; f++) if (g_nov_px[f] > 0) { int v = g_nov_px[f] + d; g_nov_px[f] = v < 4 ? 4 : v > 60 ? 60 : v; } return 1; }
    if (!strcmp(act, "zonetype")) { const char* v = strstr(qs, "t="); int b = v ? atoi(v + 2) : 0; if (b != 1 && b != 2 && b != 4 && b != 8 && b != 16 && b != 32) return 0;
        nz_show(); __atomic_fetch_xor(&g_hbz_show, b, __ATOMIC_ACQ_REL); return 1; }
    if (!strcmp(act, "weaktype")) { const char* v = strstr(qs, "t="); int b = v ? atoi(v + 2) : 0; if (b != 1 && b != 2 && b != 4) return 0; __atomic_fetch_xor(&g_nov_wk, b, __ATOMIC_ACQ_REL); return 1; }
    if (!strcmp(act, "feat")) {   // f=<a,b|all>[&on=][&team=][&px=] (vis: not in the native app)
        char b[160]; snprintf(b, sizeof b, "%s", qs); unsigned mask = 0; int on = -1, team = -1, px = -1, vis = -1;
        for (char* p = b; (p = strcasestr(p, "%2c")); ) { *p = ','; memmove(p + 1, p + 3, strlen(p + 3) + 1); }
        for (char* sv = 0, * t = strtok_r(b, "&", &sv); t; t = strtok_r(0, "&", &sv)) { char* v = strchr(t, '='); if (!v) continue; *v++ = 0;
            if (!strcmp(t, "f")) { for (char* s2 = 0, * w = strtok_r(v, ",", &s2); w; w = strtok_r(0, ",", &s2)) { int f = -1;
                    for (int q = 0; q < NOV_N; q++) if (!strcmp(w, g_nov_name[q])) f = q;
                    if (!strcmp(w, "all")) mask |= ((1u << NOV_N) - 1) & ~(1u << NOV_VIS); else if (f < 0 || f == NOV_VIS) return 0; else mask |= 1u << f; } }
            else if (!strcmp(t, "on")) { if (strcmp(v, "0") && strcmp(v, "1")) return 0; on = v[0] - '0'; }
            else if (!strcmp(t, "vis")) { if (strcmp(v, "0") && strcmp(v, "1")) return 0; vis = v[0] - '0'; }
            else if (!strcmp(t, "team")) { team = !strcmp(v, "all") ? 0 : !strcmp(v, "enemies") ? 1 : !strcmp(v, "friends") ? 2 : -2; if (team < 0) return 0; }
            else if (!strcmp(t, "px")) { char* e; long x = strtol(v, &e, 10); if (*e || x < 4 || x > 60) return 0; px = (int)x; } }
        if (!mask) return 0;
        for (int f = 0; f < NOV_N; f++) { if (!((mask >> f) & 1)) continue;
            if (on >= 0 && nov_feat(f) != on) __atomic_fetch_xor(&g_nov_on, 1u << f, __ATOMIC_ACQ_REL);
            if (team >= 0 && g_nov_team[f] >= 0) g_nov_team[f] = team; if (px > 0 && g_nov_px[f] > 0) g_nov_px[f] = px; if (vis >= 0 && f != NOV_ZON) g_nov_vis[f] = vis; }
        return 1; }
    return 0;
}
static void nov_json(StBuf* s) {   // the snapshot's overlay keys (the stats page's buttons)
    nov_init(); static const char* const tm[3] = {"all", "enemies", "friends"}; int t0 = g_nov_team[NOV_HIT];
    sb_put(s, "\"vehicles\":%s,\"zones\":%s,\"hitboxes\":%s,\"tracers\":%s,\"info\":%s,\"items\":%s,\"visible\":%s,\"teams\":\"%s\",\"text_px\":%d",
        nov_feat(NOV_VEH) ? "true" : "false", nov_feat(NOV_ZON) ? "true" : "false", nov_feat(NOV_HIT) ? "true" : "false", nov_feat(NOV_TRC) ? "true" : "false", nov_feat(NOV_INF) ? "true" : "false",
        nov_feat(NOV_ITM) ? "true" : "false", g_nov_vis[NOV_HIT] ? "true" : "false", tm[t0 >= 0 && t0 < 3 ? t0 : 0], g_nov_px[NOV_INF] > 0 ? g_nov_px[NOV_INF] : 7);
    static const int ord[6] = {NOV_HIT, NOV_VEH, NOV_TRC, NOV_INF, NOV_ITM, NOV_ZON};
    sb_put(s, ",\"features\":{");
    for (int i = 0; i < 6; i++) { int f = ord[i]; char px[16]; snprintf(px, sizeof px, "%d", g_nov_px[f]);
        sb_put(s, "%s\"%s\":{\"on\":%s,\"team\":%s%s%s,\"vis\":%s,\"px\":%s}", i ? "," : "", g_nov_name[f], nov_feat(f) ? "true" : "false",
            g_nov_team[f] >= 0 ? "\"" : "", g_nov_team[f] >= 0 ? tm[g_nov_team[f] % 3] : "null", g_nov_team[f] >= 0 ? "\"" : "", f == NOV_ZON ? "null" : g_nov_vis[f] ? "true" : "false", g_nov_px[f] > 0 ? px : "null"); }
    sb_put(s, "}");
}
