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
static const char* const gl_kn[] = {"none", "enemy", "revenge", "death spot", "intercept", "item", "roam"};
enum { GK_NONE, GK_ENEMY, GK_REVENGE, GK_SPOT, GK_ICPT, GK_ITEM, GK_ROAM };
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
    // revenge, the first GL_REVENGE_S after a respawn: the most recent death only
    if (g_ngld && now - g_gl_respawn_t < GL_REVENGE_S && g_gld[g_ngld - 1].t < g_gl_respawn_t + 0.5) { const GlDeath* dd = &g_gld[g_ngld - 1]; float ip[3];
        const GlSeen* ks = dd->killer ? gl_seen(dd->killer) : 0; int kalive = 0; for (int i = 0; i < n; i++) if (objs[i].handle == dd->killer && !objs[i].dead) kalive = 1;
        if (ks && kalive && now - ks->t < 8.0) GL_ADD(GK_REVENGE, ks->pos, dd->killer, GL_W_REVENGE, "killer %08x", dd->killer);
        if (gl_power_spawn_near(dd->pos, 8.0f, ip)) { float lp[3]; if (gl_lane_from(ip, 10, 25, lp)) GL_ADD(GK_ICPT, lp, dd->killer, GL_W_ICPT, "the item's lane (%.0f,%.0f)", ip[0], ip[1]); }
        GL_ADD(GK_SPOT, dd->pos, dd->killer, GL_W_SPOT, "where it died"); }
    // items: power-ups and power weapons lying at their spawn points (nspawns.c's live table)
    for (int i = 0; i < HB_MAXOBJ && nc < 90; i++) { if (!g_wt[i].live || g_wt[i].state != 0) continue; int k = g_wt[i].kind; float val = 0;
        if (k == 2) val = 2.5f; else if (k == 0) { const char* tg = hb_tag_name(g_wt[i].def); if (gl_power_tag(tg)) val = 2.0f; } if (val <= 0) continue;
        float p[3] = {g_wt[i].cur[0], g_wt[i].cur[1], g_wt[i].cur[2]}, dx = p[0] - o->c[0], dy = p[1] - o->c[1], d = sqrtf(dx * dx + dy * dy);
        if (d > 70) continue; GL_ADD(GK_ITEM, p, 0, val * expf(-d / 30.0f), "%s", k == 2 ? "a power-up" : "a power weapon"); }
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
    for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (t->type || t->dead || i == me || !bot_enemy(o, t)) continue;
        int rd = rd_contact(o, t), vs = vis_of(t->handle, now) == 1; if (rd || vs) gl_note(t->handle, t->c, now, !vs); }
    if (!g_gl.kind) gl_pick(now, objs, n, me);
    else {
        float dx = g_gl.pos[0] - o->c[0], dy = g_gl.pos[1] - o->c[1];
        if (dx * dx + dy * dy < 4.0f) { gl_drop(now, "reached"); gl_pick(now, objs, n, me); }
        else if (now > g_gl.until) { gl_drop(now, "held long enough"); gl_pick(now, objs, n, me); }
        else if (g_gl.kind == GK_ENEMY || g_gl.kind == GK_REVENGE) {   // (follow the enemy's newest known position; gone or dead: drop)
            const GlSeen* s = gl_seen(g_gl.who); int alive = 0; for (int i = 0; i < n; i++) if (objs[i].handle == g_gl.who && !objs[i].dead) alive = 1;
            if (!alive) { gl_drop(now, "it died"); gl_pick(now, objs, n, me); } else if (s) memcpy(g_gl.pos, s->pos, 12); }
        else if (g_gl.kind == GK_ITEM) { int still = 0; for (int i = 0; i < HB_MAXOBJ; i++) if (g_wt[i].live && g_wt[i].state == 0) { float ex = g_wt[i].cur[0] - g_gl.pos[0], ey = g_wt[i].cur[1] - g_gl.pos[1]; if (ex * ex + ey * ey < 4) { still = 1; break; } }
            if (!still) { gl_drop(now, "taken"); gl_pick(now, objs, n, me); } } }
    return g_gl.kind != GK_NONE; }
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
