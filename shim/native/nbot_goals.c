// nbot_goals.c -- where the walker goes, and what it knows (included by nbot.c after nbot_engage.c; 2026-11-08 afternoon). The
// user, watching four walkers: "the walkers don't understand how the radar works, and that is critical", "a propensity to return to
// where you were just killed to seek out the person that just killed you", "players seek out spawning power weapons, and items like
// overshield and camo -- add that in to the route selection randomizer / solidifier", "[radar:] simply look at the radar's output,
// and check near players not every player", "revenge should be for most recent kills or if it was over a power item, revenge should
// seek the most common path taken from the item to intersect with the person probabilistically", "walkers should have some
// awareness of their own death heat map ... avoid dying over and over in the same exact heat zone", "sometimes walkers just stop.
// they should always be fluid, setting new lanes, and new goals to find players or collect objects".
//
// RADAR: halo3's motion tracker as the walker's own player sees it -- its resolved traits (player record +0x1FC: +0x18 mode 1 off,
//   2 allies only, 3 normal, 4 enhanced; +0x1A range 10/15/25/50/75/100/150 m; the stats snapshot publishes them: g_rd_*), only
//   enemies NEAR it (within the range, 1 world unit = 3.048 m), shown when moving faster than a crouch-walk (GL_RADAR_SPEED) or
//   in a vehicle, every one in range with "enhanced". A contact makes the enemy KNOWN (target choice, goals). (The engine's own
//   blip list is not found yet: its draw code takes the HUD block as an argument; this follows halo3's rules.)
// MEMORY: every enemy's last known position (seen with a clear line or on radar) and when.
// OWN DEATHS: where the walker died, who killed it (the enemy that last attacked it within 3 s, else the target it fought) and
//   whether that life got a kill (a target that died while the walker fought it). A death without one is FAILED: each failed death
//   raises the path cost of the floor within 5 units (+3 per death, A* goes around) and lowers goals inside the zone.
// GOALS (the "route selection randomizer / solidifier"): a weighted random draw among
//   enemy      a known enemy's last position (seen in the last 6 s or on radar now): weight GL_W_ENEMY x exp(-d / 25)
//   revenge    after a respawn (for GL_REVENGE_S): the last killer's known position, else where the walker died; when that death
//              was within 8 units of a power item's spawn point: a point 10..25 units out along one of the lanes players take
//              from that spawn, drawn by its traffic (the killer probably took the item and left by the usual way)
//   item       a power-up, or a power weapon lying at its spawn: weight by its value x exp(-d / 30) (power-ups are taken by walking
//              over them; a weapon is contested -- no control bit picks one up)
//   roam       a lane cell 10..40 units away drawn by its traffic (the lanes file: <heatmaps>/<map>.lanes)
//   then HELD for 6..15 s (or until reached / gone / no path: then the next draw at once) -- never standing without a goal.
//   Weights x (1 - 0.6 x the zone's failed deaths, at least 0.15). NHOOK_BOT_GOALS=0 = the old nearest-enemy hunt.
#include <ctype.h>
#define GL_RADAR_SPEED 1.2f
#define GL_REVENGE_S 25.0
#define GL_W_ENEMY 3.0f
#define GL_W_REVENGE 2.0f
#define GL_W_SPOT 1.0f
#define GL_W_ICPT 1.6f
#define GL_W_ROAM 1.0f
static volatile int g_rd_mode = 3, g_rd_range = 3; static volatile uint32_t g_bot_unit;   // (the snapshot: the walker's player's traits)
static float rd_range_wu(void) { static const float m[8] = {25, 10, 15, 25, 50, 75, 100, 150}; int r = g_rd_range; return m[r >= 0 && r < 8 ? r : 0] / 3.048f; }
static int rd_contact(const NovObj* me, const NovObj* t) {
    int mode = g_rd_mode; if (mode == 1 || mode == 2 || t->dead || !bot_enemy(me, t)) return 0;
    float dx = t->c[0] - me->c[0], dy = t->c[1] - me->c[1], R = rd_range_wu(); if (dx * dx + dy * dy > R * R) return 0;   // (near players only)
    if (mode == 4) return 1;
    float sp = sqrtf(t->vel[0] * t->vel[0] + t->vel[1] * t->vel[1]);   // (units a second: a running player ~2.25)
    return sp > GL_RADAR_SPEED || t->seat >= 0 || t->parent != 0xFFFFFFFFu; }
// THE ENGINE'S OWN MOTION TRACKER (found 2026-11-08 13:05 in halo3.dll; docs/native-client.md "The motion tracker"): the HUD draw
// for local user u (halo3+0x2d289c) calls halo3+0x2d349c "is the tracker shown" (user's player, chud flag, radar mode != off) and
// then the tracker's draw halo3+0x2f3904 with S = [TLS block + 0xF0] + u * 0x28c8 + 0x244: S+0xcbc = the range in world units
// (the draw: / 0.3281 = metres, scales switch at 10 / 75 / 150 m), S+0xcb4 = the newest of 10 history frames, frame f at S + 0xf0 +
// f * 0x104 = int count, then up to 16 blips of 16 bytes {float x, float y (the sensor plane), ?, u8 type +0xc (7 = drawn 3x:
// vehicles?), u8 size class +0xd, u8 colour +0xe} drawn by halo3+0x2f329c. rd_blips reads the newest frame of user 0;
// NHOOK_RADAR_LOG=1 logs it beside every enemy's true position in the walker's frame every 2 s (to fit x / y -> world: scale,
// rotation) -- until that is fitted, rd_contact (halo3's rules from the traits) decides.
typedef struct { float x, y, z; uint8_t type, size, color, pad; } RdBlip;
static int rd_blips(RdBlip* out, int max, float* range, int* frame) {
    uint64_t blk = tls_block(), chud = 0; if (!blk || !hb_read(blk + 0xF0, &chud, 8) || !chud) return -1;
    static uint8_t b[0xcc0]; if (!hb_read(chud + 0x244, b, sizeof b)) return -1;
    int cur = *(const int32_t*)(b + 0xcb4); if (cur < 0) return -1; cur %= 10; *frame = cur; *range = *(const float*)(b + 0xcbc);
    const uint8_t* f = b + 0xf0 + cur * 0x104; int n = *(const int32_t*)f; if (n < 0 || n > 16) return -1; if (n > max) n = max;
    for (int i = 0; i < n; i++) { const uint8_t* q = f + 4 + i * 16; memcpy(&out[i].x, q, 12); out[i].type = q[0xc]; out[i].size = q[0xd]; out[i].color = q[0xe]; }
    return n; }
static void rd_log(double now, const NovObj* objs, int n, int me) {
    static int on = -1; if (on < 0) on = getenv("NHOOK_RADAR_LOG") != 0; if (!on) return; static double tl; if (now - tl < 2.0) return; tl = now;
    RdBlip bl[16]; float rng = 0; int fr = -1, nb = rd_blips(bl, 16, &rng, &fr); const NovObj* o = &objs[me]; char line[1600]; int k = 0;
    float fx = 0, fy = 0; { float c[3], f[3]; if (nov_observer(c, f)) { fx = f[0]; fy = f[1]; } } float fl = sqrtf(fx * fx + fy * fy); if (fl > 1e-3f) { fx /= fl; fy /= fl; }
    k += snprintf(line + k, sizeof line - (size_t)k, "[nhook] [radar] frame %d range %.2f u, %d blips:", fr, rng, nb);
    for (int i = 0; i < nb && k < 1200; i++) k += snprintf(line + k, sizeof line - (size_t)k, " (%.3f %.3f %.3f t%d s%d c%d)", bl[i].x, bl[i].y, bl[i].z, bl[i].type, bl[i].size, bl[i].color);
    k += snprintf(line + k, sizeof line - (size_t)k, " | enemies (fwd, left, units):");
    for (int i = 0; i < n && k < 1550; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue; float dx = t->c[0] - o->c[0], dy = t->c[1] - o->c[1];
        if (dx * dx + dy * dy > 30 * 30) continue; float sp = sqrtf(t->vel[0] * t->vel[0] + t->vel[1] * t->vel[1]);
        k += snprintf(line + k, sizeof line - (size_t)k, " (%.1f %.1f v%.1f)", dx * fx + dy * fy, -dx * fy + dy * fx, sp); }
    nlog("%s", line); }
typedef struct { uint32_t h; float pos[3]; double t; int radar; } GlSeen;
static GlSeen g_gls[64]; static int g_ngls;
static void gl_note(uint32_t h, const float* p, double now, int radar) {
    GlSeen* s = 0; for (int i = 0; i < g_ngls; i++) if (g_gls[i].h == h) { s = &g_gls[i]; break; }
    if (!s) { if (g_ngls < 64) s = &g_gls[g_ngls++]; else { int o = 0; for (int i = 1; i < 64; i++) if (g_gls[i].t < g_gls[o].t) o = i; s = &g_gls[o]; } s->h = h; }
    memcpy(s->pos, p, 12); s->t = now; s->radar = radar; }
static const GlSeen* gl_seen(uint32_t h) { for (int i = 0; i < g_ngls; i++) if (g_gls[i].h == h) return &g_gls[i]; return 0; }
typedef struct { float pos[3]; double t; int failed; uint32_t killer; } GlDeath;
static GlDeath g_gld[64]; static int g_ngld; static double g_gl_respawn_t, g_gl_life_t0;   // (g_gl_kill_t: nbot_engage.c)
static float gl_zone(const float* p) {   // failed deaths within 6 units
    float z = 0; for (int i = 0; i < g_ngld; i++) { if (!g_gld[i].failed) continue; float dx = p[0] - g_gld[i].pos[0], dy = p[1] - g_gld[i].pos[1]; if (dx * dx + dy * dy < 36) z += 1; }
    return z; }
static void gl_pen_zone(const float* p) {   // A* goes around: +3 on every floor node within 5 units
    if (!g_nav.ok || !g_nav.pen) return; int r = (int)ceilf(5.0f / g_nav.cell);
    int cx0 = (int)floorf((p[0] - g_nav.x0) / g_nav.cell), cy0 = (int)floorf((p[1] - g_nav.y0) / g_nav.cell);
    for (int cy = cy0 - r; cy <= cy0 + r; cy++) for (int cx = cx0 - r; cx <= cx0 + r; cx++) { if (cx < 0 || cy < 0 || cx >= g_nav.w || cy >= g_nav.h) continue;
        int cc = cy * g_nav.w + cx; for (uint32_t k = g_nav.first[cc]; k < g_nav.first[cc + 1]; k++) { float q[3]; nav_pos((int)k, q); float dx = q[0] - p[0], dy = q[1] - p[1]; if (dx * dx + dy * dy < 25) g_nav.pen[k] += 3.0f; } } }
// the lanes as goal material: the busier half of the map's lane cells (x, y, passes), loaded by bm_pop
// (GlLane / g_gll: declared in nbot.c before bm_pop, which loads them)
static int gl_power_tag(const char* tag) {
    static const char* const pw[] = {"rocket", "sniper", "beam_rifle", "spartan_laser", "shotgun", "energy_sword", "hammer", "fuel_rod", "missile", "flamethrower", "machinegun_turret", "plasma_cannon", 0};
    if (!tag) return 0; for (int i = 0; pw[i]; i++) if (strstr(tag, pw[i])) return 1; return 0; }
// power items' spawn points (power-ups, power weapons) from the spawn tracker (nspawns.c: g_hm.sp)
static int gl_power_spawn_near(const float* p, float r, float* out) {
    int got = 0; float bd = r * r; pthread_mutex_lock(&g_hm_mx);
    for (uint32_t i = 0; i < g_hm.nsp; i++) { const HmSpawn* q = &g_hm.sp[i]; int pow = q->kind == 3 || (q->kind == 0 && gl_power_tag(q->name));
        if (!pow) { char lw[48]; snprintf(lw, sizeof lw, "%s", q->name); for (char* c = lw; *c; c++) *c = (char)((*c == ' ') ? '_' : tolower((unsigned char)*c)); pow = q->kind == 0 && gl_power_tag(lw); }
        if (!pow) continue; float dx = q->x - p[0], dy = q->y - p[1], d = dx * dx + dy * dy; if (d < bd) { bd = d; out[0] = q->x; out[1] = q->y; out[2] = q->z; got = 1; } }
    pthread_mutex_unlock(&g_hm_mx); return got; }
static struct { int kind; float pos[3]; uint32_t who; double t0, until; char what[40]; } g_gl;
static const char* const gl_kn[] = {"none", "enemy", "revenge", "death spot", "intercept", "item", "roam", "teammate", "aid"};
enum { GK_NONE, GK_ENEMY, GK_REVENGE, GK_SPOT, GK_ICPT, GK_ITEM, GK_ROAM, GK_MATE, GK_AID };
static int gl_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("NHOOK_BOT_GOALS"); on = !(e && !strcmp(e, "0")); } return on; }
static void gl_drop(double now, const char* why) { if (g_gl.kind && bot_log()) nlog("[nhook] [goal] %s (%s) dropped: %s", gl_kn[g_gl.kind], g_gl.what, why); g_gl.kind = GK_NONE; (void)now; }
// a lane point 10..25 units out from an item spawn, along the lanes leaving it, drawn by traffic
static int gl_lane_from(const float* from, float dmin, float dmax, float* out) {
    if (!g_ngll) return 0; float ws = 0; static int cand[4096]; static float cw[4096]; int nc = 0;
    for (int i = 0; i < g_ngll && nc < 4096; i++) { float dx = g_gll[i].x - from[0], dy = g_gll[i].y - from[1], d = sqrtf(dx * dx + dy * dy); if (d < dmin || d > dmax) continue;
        float w = g_gll[i].w * (1.0f - 0.6f * fminf(1.0f, gl_zone((float[3]){g_gll[i].x, g_gll[i].y, 0}) / 2.0f)); cand[nc] = i; cw[nc] = w; ws += w; nc++; }
    if (!nc || ws <= 0) return 0; float r = bot_rand01() * ws; int pick = cand[nc - 1]; for (int k = 0; k < nc; k++) { r -= cw[k]; if (r <= 0) { pick = cand[k]; break; } }
    out[0] = g_gll[pick].x; out[1] = g_gll[pick].y; out[2] = from[2]; return 1; }
// one draw among the goals now available
static void gl_pick(double now, const NovObj* objs, int n, int me) {
    const NovObj* o = &objs[me]; typedef struct { int kind; float pos[3]; uint32_t who; float w; char what[40]; } C; static C c[96]; int nc = 0;
    #define GL_ADD(K, P, WHO, W, ...) do { if (nc < 96 && (W) > 0) { c[nc].kind = K; memcpy(c[nc].pos, P, 12); c[nc].who = WHO; c[nc].w = (W) * fmaxf(0.15f, 1.0f - 0.6f * gl_zone(P)); snprintf(c[nc].what, sizeof c[nc].what, __VA_ARGS__); nc++; } } while (0)
    // enemies: known positions (seen in the last 6 s, on radar now)
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue; const GlSeen* s = gl_seen(t->handle); if (!s || now - s->t > 6.0) continue;
        float dx = s->pos[0] - o->c[0], dy = s->pos[1] - o->c[1], d = sqrtf(dx * dx + dy * dy); GL_ADD(GK_ENEMY, s->pos, t->handle, GL_W_ENEMY * expf(-d / 25.0f), "%08x %s", t->handle, s->radar ? "on radar" : "seen"); }
    // REVENGE: drawn once per respawn with the players' own odds (engage-model.json behaviour: their excess over chance) -- the most
    // recent death only; the killer's known position, else the death spot, or -- that death within 8 units of a power item's spawn
    // point -- a point 10..25 units out along a lane leaving the item, drawn by traffic (the killer took it and left the usual way)
    static double rev_drawn; if (g_ngld && now - g_gl_respawn_t < GL_REVENGE_S && g_gld[g_ngld - 1].t < g_gl_respawn_t + 0.5 && rev_drawn != g_gl_respawn_t) {
        rev_drawn = g_gl_respawn_t; const GlDeath* dd = &g_gld[g_ngld - 1]; float r = bot_rand01(), ip[3];
        const GlSeen* ks = dd->killer ? gl_seen(dd->killer) : 0; int kalive = 0; for (int i = 0; i < n; i++) if (objs[i].handle == dd->killer && !objs[i].dead) kalive = 1;
        int kind = 0; float gp[3]; char what[40] = "";
        if (r < g_eg.p_killer && ks && kalive) { kind = GK_REVENGE; memcpy(gp, ks->pos, 12); snprintf(what, sizeof what, "killer %08x", dd->killer); }
        else if (r < g_eg.p_killer + g_eg.p_spot) {
            if (gl_power_spawn_near(dd->pos, 8.0f, ip) && gl_lane_from(ip, 10, 25, gp)) { kind = GK_ICPT; snprintf(what, sizeof what, "the item's lane (%.0f,%.0f)", ip[0], ip[1]); }
            else { kind = GK_SPOT; memcpy(gp, dd->pos, 12); snprintf(what, sizeof what, "where it died"); } }
        if (kind) { g_gl.kind = kind; memcpy(g_gl.pos, gp, 12); g_gl.who = dd->killer; g_gl.t0 = now; g_gl.until = now + 10.0 + 10.0 * bot_rand01(); snprintf(g_gl.what, sizeof g_gl.what, "%s", what);
            if (bot_log()) nlog("[nhook] [goal] %s: %s (%.0f, %.0f) -- the respawn draw (killer %.0f%%, spot %.0f%%)", gl_kn[kind], what, gp[0], gp[1], g_eg.p_killer * 100, g_eg.p_spot * 100); return; } }
    // POWER ITEMS: power-ups and power weapons lying at their spawn points (nspawns.c's live table), taken with the players' extra
    // heading-in share at that distance (per draw)
    { int bi2 = -1; float bd2 = 1e30f; for (int i = 0; i < HB_MAXOBJ; i++) { if (!g_wt[i].live || g_wt[i].state != 0) continue; int k = g_wt[i].kind;
          if (!(k == 2 || (k == 0 && gl_power_tag(hb_tag_name(g_wt[i].def))))) continue; float dx = g_wt[i].cur[0] - o->c[0], dy = g_wt[i].cur[1] - o->c[1], d = dx * dx + dy * dy; if (d < bd2) { bd2 = d; bi2 = i; } }
      if (bi2 >= 0) { float d = sqrtf(bd2), p = d < 10 ? g_eg.it_near : d < 20 ? g_eg.it_mid : d < 40 ? g_eg.it_far : 0; if (g_wt[bi2].kind == 2) p *= 1.5f;   // (power-ups: walkers can take them)
          if (bot_rand01() < p) { g_gl.kind = GK_ITEM; memcpy(g_gl.pos, g_wt[bi2].cur, 12); g_gl.who = 0; g_gl.t0 = now; g_gl.until = now + 8.0 + 8.0 * bot_rand01();
              snprintf(g_gl.what, sizeof g_gl.what, "%s", g_wt[bi2].kind == 2 ? "a power-up" : "a power weapon");
              if (bot_log()) nlog("[nhook] [goal] item: %s %.0f units away (%.0f%% this draw)", g_gl.what, d, p * 100); return; } } }
    // TEAMMATES (team games): the nearest teammate, weighted by how often players keep one within 15 units -- the unit partner of an
    // aid / join (g_tw_mate) much more, while that lasts; reached at the players' spacing (gl_tick)
    { int mi = -1; float md = 1e30f; for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || bot_enemy(o, t)) continue;
          float dx = t->c[0] - o->c[0], dy = t->c[1] - o->c[1], d = dx * dx + dy * dy; if (t->handle == g_tw_mate && now < g_tw_until) d = -1; if (d < md) { md = d; mi = i; } }
      if (mi >= 0) { const NovObj* t = &objs[mi]; float dd = sqrtf(fmaxf(md, 0)); int unit = t->handle == g_tw_mate && now < g_tw_until;
          if (unit || dd > g_eg.tw_spacing) GL_ADD(GK_MATE, t->c, t->handle, unit ? 4.0f : 2.0f * g_eg.tw_within15, "teammate %08x%s", t->handle, unit ? " (our unit)" : ""); } }
    // roam: lane cells 10..40 units away, by traffic (a few draws)
    for (int k = 0; k < 4; k++) { float lp[3]; if (gl_lane_from(o->c, 10, 40, lp)) GL_ADD(GK_ROAM, lp, 0, GL_W_ROAM * 0.25f, "a lane (%.0f,%.0f)", lp[0], lp[1]); }
    if (!nc) { int cur = -1; float q[3] = {o->c[0], o->c[1], o->c[2] - 0.35f}; if (g_nav.ok) cur = nav_node_at(q); int gn = cur >= 0 ? nav_pick_roam(cur) : -1;   // (no lanes: the graph's own roam)
        if (gn >= 0) { float p[3]; nav_pos(gn, p); GL_ADD(GK_ROAM, p, 0, 1.0f, "a roam node"); } }
    #undef GL_ADD
    if (!nc) { g_gl.kind = GK_NONE; return; }
    float ws = 0; for (int i = 0; i < nc; i++) ws += c[i].w; float r = bot_rand01() * ws; int pick = nc - 1; for (int i = 0; i < nc; i++) { r -= c[i].w; if (r <= 0) { pick = i; break; } }
    g_gl.kind = c[pick].kind; memcpy(g_gl.pos, c[pick].pos, 12); g_gl.who = c[pick].who; g_gl.t0 = now; g_gl.until = now + 6.0 + 9.0 * bot_rand01();
    snprintf(g_gl.what, sizeof g_gl.what, "%s", c[pick].what);
    if (bot_log()) nlog("[nhook] [goal] %s: %s (%.0f, %.0f), %d choices, held up to %.0f s", gl_kn[g_gl.kind], g_gl.what, g_gl.pos[0], g_gl.pos[1], nc, g_gl.until - now); }
// every walker tick (alive, on foot): memory, deaths, the goal; returns 1 when a goal is set (g_gl.pos)
static int gl_tick(double now, const NovObj* objs, int n, int me, int (*vis_of)(uint32_t, double)) {
    const NovObj* o = &objs[me]; g_bot_unit = o->handle;
    rd_log(now, objs, n, me);   // (NHOOK_RADAR_LOG: the engine's tracker beside the truth)
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
        int rd = rd_contact(o, t), vs = vis_of(t->handle, now) == 1; if (rd || vs) gl_note(t->handle, t->c, now, !vs); }
    if (g_tw_aid_mate) { uint32_t m = g_tw_aid_mate; g_tw_aid_mate = 0;   // (nbot_engage.c: a teammate under fire, its attacker unknown to us: go to it)
        for (int i = 0; i < n; i++) if (objs[i].handle == m && !objs[i].dead) { g_gl.kind = GK_AID; memcpy(g_gl.pos, objs[i].c, 12); g_gl.who = m; g_gl.t0 = now; g_gl.until = now + 6.0;
            snprintf(g_gl.what, sizeof g_gl.what, "teammate %08x under fire", m); if (bot_log()) nlog("[nhook] [goal] aid: going to teammate %08x", m); break; } }
    if (!g_gl.kind) gl_pick(now, objs, n, me);
    else {
        float dx = g_gl.pos[0] - o->c[0], dy = g_gl.pos[1] - o->c[1];
        float rr = (g_gl.kind == GK_MATE || g_gl.kind == GK_AID) ? fmaxf(3.0f, 0.6f * g_eg.tw_spacing) : 2.0f;   // (a teammate: at the players' spacing)
        if (dx * dx + dy * dy < rr * rr) { gl_drop(now, "reached"); gl_pick(now, objs, n, me); }
        else if (now > g_gl.until) { gl_drop(now, "held long enough"); gl_pick(now, objs, n, me); }
        else if (g_gl.kind == GK_MATE || g_gl.kind == GK_AID) {   // (follow the teammate; dead: drop)
            int found = 0; for (int i = 0; i < n; i++) if (objs[i].handle == g_gl.who && !objs[i].dead) { memcpy(g_gl.pos, objs[i].c, 12); found = 1; break; }
            if (!found) { gl_drop(now, "the teammate died"); gl_pick(now, objs, n, me); } }
        else if (g_gl.kind == GK_ENEMY || g_gl.kind == GK_REVENGE) {   // (follow the enemy's newest known position; gone or dead: drop)
            const GlSeen* s = gl_seen(g_gl.who); int alive = 0; for (int i = 0; i < n; i++) if (objs[i].handle == g_gl.who && !objs[i].dead) alive = 1;
            if (!alive) { gl_drop(now, "it died"); gl_pick(now, objs, n, me); } else if (s) memcpy(g_gl.pos, s->pos, 12); }
        else if (g_gl.kind == GK_ITEM) { int still = 0; for (int i = 0; i < HB_MAXOBJ; i++) if (g_wt[i].live && g_wt[i].state == 0) { float ex = g_wt[i].cur[0] - g_gl.pos[0], ey = g_wt[i].cur[1] - g_gl.pos[1]; if (ex * ex + ey * ey < 4) { still = 1; break; } }
            if (!still) { gl_drop(now, "taken"); gl_pick(now, objs, n, me); } } }
    return g_gl.kind != GK_NONE; }
// RADAR LOOK: with no target, a new radar contact (an enemy moving within the range, unseen) makes the walker turn and look at it
// with the players' odds (engage-model behaviour: turned to an unseen moving enemy within the radar's range) after their delay
// (median x 0.5..1.5), for 1.5 s; returns 1 and the yaw to look at while it does
static int gl_radar_look(double now, const NovObj* objs, int n, int me, int have_target, float* yaw_out) {
    static uint32_t look_h; static double look_at, look_until; static struct { uint32_t h; double t; } asked[32]; static int na;
    const NovObj* o = &objs[me]; if (have_target) { look_h = 0; return 0; }
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !rd_contact(o, t)) continue;
        int k = -1; for (int j = 0; j < na; j++) if (asked[j].h == t->handle) { k = j; break; }
        if (k >= 0 && now - asked[k].t < 6.0) continue;   // (one decision per contact per 6 s)
        if (k < 0) { k = na < 32 ? na++ : (int)arc4random_uniform(32); asked[k].h = t->handle; } asked[k].t = now;
        if (!look_h && bot_rand01() < g_eg.rd_turn) { look_h = t->handle; look_at = now + g_eg.rd_turn_s * (0.5f + bot_rand01()); look_until = look_at + 1.5;
            if (bot_log()) nlog("[nhook] [goal] radar: %08x moving %.1f units away -- looking in %.1f s", t->handle, sqrtf((t->c[0] - o->c[0]) * (t->c[0] - o->c[0]) + (t->c[1] - o->c[1]) * (t->c[1] - o->c[1])), look_at - now); } }
    if (!look_h || now < look_at) return 0;
    if (now > look_until) { look_h = 0; return 0; }
    for (int i = 0; i < n; i++) if (objs[i].handle == look_h && !objs[i].dead) { *yaw_out = atan2f(objs[i].c[1] - o->c[1], objs[i].c[0] - o->c[0]); return 1; }
    look_h = 0; return 0; }
// life and death (called every tick with the walker's unit state): records deaths, respawns, kills
static void gl_life(double now, int alive, const float* pos, uint32_t target, int target_dead_now) {
    static int was = -1; static float last[3]; static uint32_t lastk;
    if (alive && pos) memcpy(last, pos, 12);
    if (alive && target) lastk = target;
    if (target_dead_now) g_gl_kill_t = now;
    if (was == 1 && !alive) {   // died
        uint32_t killer = 0; double best = 3.0; for (int i = 0; i < g_negu; i++) if (now - g_egu[i].att_t < best) { best = now - g_egu[i].att_t; killer = g_egu[i].h; }
        if (!killer) killer = lastk;
        int failed = !(g_gl_kill_t > g_gl_life_t0 && now - g_gl_kill_t < 20.0);
        GlDeath* d = g_ngld < 64 ? &g_gld[g_ngld++] : (memmove(g_gld, g_gld + 1, 63 * sizeof *g_gld), &g_gld[63]);
        memcpy(d->pos, last, 12); d->t = now; d->failed = failed; d->killer = killer;
        int nf = 0; for (int i = 0; i < g_ngld; i++) if (g_gld[i].failed) { float dx = g_gld[i].pos[0] - last[0], dy = g_gld[i].pos[1] - last[1]; if (dx * dx + dy * dy < 36) nf++; }
        if (failed) gl_pen_zone(last);
        if (bot_log()) nlog("[nhook] [goal] died at (%.1f, %.1f) killed by %08x, %s (%d failed deaths in this zone)", last[0], last[1], killer, failed ? "no kill this life" : "after a kill", nf);
        g_gl.kind = GK_NONE; }
    if (was == 0 && alive) { g_gl_respawn_t = now; g_gl_life_t0 = now; g_gl.kind = GK_NONE; }
    if (was < 0 && alive) g_gl_life_t0 = now;
    was = alive; }
