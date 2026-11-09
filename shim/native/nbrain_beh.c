// nbrain_beh.c -- the walker's BEHAVIOURS (2026-11-09, brain stage 2; the user: "I feel like native/nbot_* should be its own kernel or
// something. it looks like a pile of if then statements now"). Each behaviour looks at the moment (World) and BIDS for the legs in the
// locomotion arbiter (nbot.c arb_*): a score (a live knob, NHOOK_U_*) and, for a path, a point. Side effects a behaviour owns stay with
// it (the boarding honk answer, the loose ball's act tap). The step (nbot.c bot_step_body) builds the World, asks every behaviour, lets
// the arbiter choose, and walks the winner. Included by nbot.c before bot_step_body; part of the reloadable brain (nbrain.dylib).

typedef struct {
    double now; const NovObj* objs; int n, me; const NovObj* o; const float* cam;
    int engaged, fight; float tdist;   // (the fighter's view this step: a target in reach and sight, the fighter on, its distance)
} World;

// BOARD -- a teammate drives a vehicle with a seat free (gun_board): go to the seat; a teammate's honk is answered in chat
static void beh_board(const World* W, Arb* a, float* gbp, float* gface, float* gbd, int* gun_go) {
    uint32_t ghog = 0xFFFFFFFFu; *gun_go = !g_bot_shadow && gun_board(W->now, W->objs, W->n, W->me, gbp, gface, gbd, &ghog);
    static int was; static double thonk_seen;
    if (*gun_go) { if (!was) nlog("[nhook] [gunner] my teammate is in a vehicle with a seat free: going to it (%.1f units)", *gbd);
        g_bot_pick_on = 0;   // (no detour for a weapon)
        double ha = honk_age(); if (ha < 15.0 && wall_s() - ha > thonk_seen + 1.0) { thonk_seen = wall_s() - ha;   // (a new honk: answer it)
            nlog("[nhook] [gunner] my teammate honked: coming (%.1f units)", *gbd); char* m = malloc(96); if (m) { snprintf(m, 96, "%s: coming!", g_nick[0] ? g_nick : "walker");
                pthread_t th; if (pthread_create(&th, 0, camp_say_thread, m) == 0) pthread_detach(th); else free(m); } }
        float u = 95; lk_over(&u, "NHOOK_U_BOARD"); arb_bid(a, ARB_BOARD, u, gbp, "board the leader's vehicle"); }
    else if (was) nlog("[nhook] [gunner] no teammate's vehicle to ride any more");
    was = *gun_go; }

// FOLLOW -- the WINGMAN (02:20, the user: "cohesion should be the goal for the moment. always be a loyal wingman"): the leader is the
// player NHOOK_BOT_FOLLOW names (a friend in any game type), else the nearest human teammate, else the teammate walker with the lowest
// unit handle. (02:10: "it should always find me"; 02:25: "find me at all costs"; 05:55: "super needy following me like a puppy" -> the
// LEASH: NHOOK_BOT_LEASH 15 units -- beyond it the bid is NHOOK_U_FOLLOW_FAR 90, over the fight's 60; inside it, out of reach of
// WING_START 8 until within WING_STOP 4, NHOOK_U_FOLLOW_NEAR 20 -- under the fight and a weapon fetch.) NHOOK_BOT_NOWING=1 = off
static void beh_follow(const World* W, Arb* a) {
    const NovObj* objs = W->objs; const NovObj* o = W->o; int n = W->n, me = W->me;
    static int nw = -1; if (nw < 0) nw = getenv("NHOOK_BOT_NOWING") != 0; static int was;
    int wing = 0; float lp[3] = {0, 0, 0};
    float leash = 15, wstart = 8, wstop = 4; lk_over(&leash, "NHOOK_BOT_LEASH"); lk_over(&wstart, "NHOOK_BOT_WING_START"); lk_over(&wstop, "NHOOK_BOT_WING_STOP");
    int fol_far = 0;
    // (2026-11-09 10:50, the user: "it does not need to focus on finding me in the moment"): NHOOK_BOT_NOWING is a live knob too
    //  (1 = no following / closing up; the followed player stays a friend)
    float nwk = (float)nw; lk_over(&nwk, "NHOOK_BOT_NOWING");
    if (!(nwk > 0) && !g_bot_shadow) { int li = -1; float bd = 1e30f; int human = 0, fol = 0;
        for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (i == me || t->type || t->dead || bot_enemy(o, t)) continue; int hu = t->handle == g_follow_unit;
            if (hu) { float fx = t->c[0] - o->c[0], fy = t->c[1] - o->c[1]; li = i; bd = fx * fx + fy * fy; human = 1; fol = 1; break; }
            for (int k = 0; k < g_human_n && k < 8; k++) if (g_human_unit[k] == t->handle) hu = 1;
            float dx = t->c[0] - o->c[0], dy = t->c[1] - o->c[1], d = dx * dx + dy * dy;
            if (hu && (!human || d < bd)) { human = 1; bd = d; li = i; }
            else if (!human && !hu && t->handle < o->handle && (li < 0 || t->handle < objs[li].handle)) { li = i; bd = d; } }
        if (li >= 0 && bd > (fol ? (was ? wstop * wstop : wstart * wstart) : (was ? 16.0f : 36.0f))) { wing = 1; memcpy(lp, objs[li].c, 12);
            static double tlog; if (!was && W->now - tlog > 10.0) { tlog = W->now; nlog("[nhook] [wing] %.0f units from my %s: closing up", sqrtf(bd), human ? "human teammate" : "lead walker"); } }
        if (li >= 0 && fol && bd > leash * leash) fol_far = 1; }
    was = wing; g_fol_go = wing && fol_far;
    if (wing) { float u_far = 90, u_near = 20; lk_over(&u_far, "NHOOK_U_FOLLOW_FAR"); lk_over(&u_near, "NHOOK_U_FOLLOW_NEAR");
        arb_bid(a, ARB_FOLLOW, fol_far ? u_far : u_near, lp, fol_far ? "follow (beyond the leash)" : "close up on the leader"); } }

// BALL -- oddball: loose -> fetch it (act tapped on it, never beside a vehicle) 75; carried by me -> 12 units away from the nearest enemy
// 80 (no shots, melee in reach: the fire section); an enemy's -> hunt the carrier 50; a teammate's -> escort 30. NHOOK_BOT_NOBALL=1 off
static void beh_ball(const World* W, Arb* a, int gun_go, int* ob_carry, float* ob_d) {
    const NovObj* objs = W->objs; const NovObj* o = W->o; int n = W->n, me = W->me; double now = W->now;
    static int nb = -1; if (nb < 0) nb = getenv("NHOOK_BOT_NOBALL") != 0;
    *ob_carry = 0; *ob_d = 1e9f;
    if (nb || gun_go || g_bot_shadow) return;
    ob_scan(now, objs, n, me); *ob_carry = g_ob_state == 2 || (g_inv.held && ob_is_ball(g_inv.held));
    float dx = g_ob_pos[0] - o->c[0], dy = g_ob_pos[1] - o->c[1]; *ob_d = sqrtf(dx * dx + dy * dy); static double tact;
    if (*ob_carry) { int ei = -1; float ed = 1e30f;
        for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (i == me || t->type || t->dead || !bot_enemy(o, t)) continue; float ex = t->c[0] - o->c[0], ey = t->c[1] - o->c[1], e2 = ex * ex + ey * ey; if (e2 < ed) { ed = e2; ei = i; } }
        if (ei >= 0) { float ex = o->c[0] - objs[ei].c[0], ey = o->c[1] - objs[ei].c[1], el = sqrtf(ex * ex + ey * ey) + 1e-3f;
            float ap[3] = {o->c[0] + ex / el * 12.0f, o->c[1] + ey / el * 12.0f, o->c[2]}; float u = 80; lk_over(&u, "NHOOK_U_BALL_RUN"); arb_bid(a, ARB_BALL, u, ap, "run with the ball"); }
        g_bot_pick_on = 0; }
    else if (g_ob_state == 1) { { float u = 75; lk_over(&u, "NHOOK_U_BALL_FETCH"); arb_bid(a, ARB_BALL, u, g_ob_pos, "fetch the loose ball"); }
        if (*ob_d < 1.5f && fabsf(g_ob_pos[2] - o->c[2]) < 1.6f && now - tact > 0.8 && !gun_near_veh(o->c, 0xFFFFFFFFu)) { tact = now; g_bot_act_until = now + 0.15; } }
    else if (g_ob_state == 4) { float u = 50; lk_over(&u, "NHOOK_U_BALL_HUNT"); arb_bid(a, ARB_BALL, u, g_ob_pos, "hunt the ball carrier"); }
    else if (g_ob_state == 3 && *ob_d > 5.0f) { float u = 30; lk_over(&u, "NHOOK_U_BALL_ESCORT"); arb_bid(a, ARB_BALL, u, g_ob_pos, "escort the ball carrier"); } }

// FETCH a better weapon (the pick-up scan chose it: g_bot_pick_on) 45; FIGHT -- an enemy in reach and in sight, the fighter's moves 60
// (not while carrying the ball beyond melee reach); HOLD the high ground over an enemy below 65; HUNT -- the goal draw, always, 10
// (07:20: the fetch bid scales with the upgrade: NHOOK_U_FETCH 45 + NHOOK_U_FETCH_GAIN 6 a rank better, - 1 a unit beyond 10 -- an assault
//  rifle -> a battle rifle close by bids ~81, over the fight's 60; a small upgrade far off loses to it)
// (09:30: standing on weapons mid-fight -- "moved 0.00 u/s" for 10 s of fighting while the fetch held the legs: with an enemy within 10
//  units in the fight the fetch bids only NHOOK_U_FETCH_HOT (40, under the fight's 60) unless the weapon is within 2 units -- grabbed in
//  passing -- or the walker is DRY)
static void beh_fetch(const World* W, Arb* a) { if (g_bot_pick_on) { float u = 45, g = 6, hot = 40; lk_over(&u, "NHOOK_U_FETCH"); lk_over(&g, "NHOOK_U_FETCH_GAIN"); lk_over(&hot, "NHOOK_U_FETCH_HOT");
    float bid = u + g * (float)g_pick_gain - fmaxf(0.0f, g_pick_d - 10.0f);
    if (W->engaged && W->tdist < 10.0f && g_pick_d > 2.0f && !g_bot_dry) bid = fminf(bid, hot);
    arb_bid(a, ARB_FETCH, bid, g_bot_pick_pos, "fetch a better weapon"); } }
static void beh_fight(const World* W, Arb* a, int ob_carry) { if (W->engaged && !(ob_carry && W->tdist > 2.5f)) { float u = 60, uo = 35; lk_over(&u, "NHOOK_U_FIGHT"); lk_over(&uo, "NHOOK_U_FIGHT_OUTRANGED");
    arb_bid(a, ARB_FIGHT, g_bot_outranged ? uo : u, 0, g_bot_outranged ? "fight (outranged)" : "fight"); } }   // (outranged: under a weapon fetch's 45)
static void beh_hold(const World* W, Arb* a) { if (W->fight && W->now - g_bot_hold_high < 0.3) { float u = 65; lk_over(&u, "NHOOK_U_HOLD"); arb_bid(a, ARB_HOLD, u, 0, "hold the high ground"); } }
static void beh_hunt(const World* W, Arb* a) { (void)W; float u = 10; lk_over(&u, "NHOOK_U_HUNT"); arb_bid(a, ARB_HUNT, u, 0, "hunt"); }

// RECHARGE (2026-11-09 13:10; the death notes: a death every ~18 s, mostly close fights lost on the last sliver of health -- and after a
// fight the walker went straight on hunting with its shield broken): the shield under NHOOK_BOT_RECHARGE_SH (0.6) with no enemy in sight
// within 20 units (g_eng_close) -> 6 units away from the nearest enemy, for up to 6 s or until the shield is back over 0.95, bidding
// NHOOK_U_RECHARGE (65: over the fight's 60 for a target far off / hidden -- a fight in sight within 20 units is not bid against: the
// recharge waits). NHOOK_U_RECHARGE=0 off
// BREAK OFF (14:20; the last 15 deaths: shield 0 at death but health a median 0.87 -- once the shield pops a headshot kills, and 9 of 15
// had 2+ enemies aiming): the walker's shield under 0.15, the target in sight 3+ units away and healthier than it -> back the way it came
// (its position ~2.5 s ago: usually out of that line of fire), still shooting, for up to 3 s or until the shield is over 0.6, bidding
// NHOOK_U_BREAK (70, over the fight's 60). NHOOK_U_BREAK=0 off
static void beh_break(const World* W, Arb* a) {
    const NovObj* o = W->o; static float hist[8][3]; static double th, t0; static int nh;
    if (W->now - th > 0.5) { th = W->now; memmove(hist[1], hist[0], sizeof(float) * 21); memcpy(hist[0], o->c, 12); if (nh < 8) nh++; }
    float u = 70; lk_over(&u, "NHOOK_U_BREAK"); if (u <= 0) return;
    if (o->shield > 0.6f) { t0 = 0; return; }
    int start = o->shield < 0.15f && g_ftg_vis && g_ftg_d > 3.0f && g_ftg_d < 25.0f && g_ftg_v > o->shield + o->health + 0.2f;
    if (t0 <= 0 && !start) return;
    if (t0 <= 0) { t0 = W->now; if (bot_log()) nlog("[nhook] [break] shield %.2f, the target healthier (%.2f) at %.0f units: breaking off the way I came", o->shield, g_ftg_v, g_ftg_d); }
    if (W->now - t0 > 3.0 || nh < 6) return;
    const float* b = hist[5]; float dx = b[0] - o->c[0], dy = b[1] - o->c[1]; if (dx * dx + dy * dy < 4.0f) return;   // (came from under 2 units: nowhere to go back to)
    arb_bid(a, ARB_RECHARGE, u, b, "break off"); }
// COVER (2026-11-09 11:00, the user: "the walker is aware of when its shield is getting decimated too eh? and like when it should try
// to take cover near by while continuing to shoot and defend itself aggressively?"): the shield down NHOOK_BOT_COVER_DROP (0.35) within
// 1.5 s and under NHOOK_BOT_COVER_SH (0.5), the target in sight -> up to 8 floor spots 3-6 units around (sideways first, never toward
// the attacker; its own floor part, within 1.5 units of its level) are tested from the attacker's eyes to where the walker's head
// would be (bot_cov_ray: the line-of-sight channel). The first spot the attacker cannot see wins: the legs go there, bidding
// NHOOK_U_COVER (80: over the break-off's 70 and the fight) while the aim keeps tracking and shooting; it stays until the shield is
// back over NHOOK_BOT_COVER_UNTIL (0.9) or 6 s, or until it is hit hard there again (a new search, at most one a second). No hidden
// spot -> the break-off as before. NHOOK_U_COVER=0 off. [cover] lines
static void beh_cover(const World* W, Arb* a) {
    const NovObj* o = W->o; static float sh[16]; static double tsh; static int nsh;
    if (W->now - tsh > 0.1) { tsh = W->now; memmove(sh + 1, sh, sizeof(float) * 15); sh[0] = o->shield; if (nsh < 16) nsh++; }
    float u = 80, drop = 0.35f, lim = 0.5f, until = 0.9f; lk_over(&u, "NHOOK_U_COVER"); lk_over(&drop, "NHOOK_BOT_COVER_DROP"); lk_over(&lim, "NHOOK_BOT_COVER_SH"); lk_over(&until, "NHOOK_BOT_COVER_UNTIL");
    static int st; static double t0, tsearch; static float spot[3], fz[8];
    if (u <= 0 || !g_nav.ok) { st = 0; g_cov_n = 0; return; }
    float mx = o->shield; for (int i = 0; i < nsh; i++) if (sh[i] > mx) mx = sh[i];
    int hit = o->shield >= 0 && o->shield < lim && mx - o->shield >= drop;
    // (11:35: 28 of 40 deaths "outnumbered" -- 2-4 enemies aiming at it: with NHOOK_BOT_COVER_NAIM (2, live) or more aiming and the shield
    //  starting to go, look for cover at once instead of waiting for the big drop)
    { float cn = 2; lk_over(&cn, "NHOOK_BOT_COVER_NAIM"); if (cn > 0 && g_naim_me >= (int)cn && o->shield < 0.95f && mx - o->shield >= 0.1f) hit = 1; }
    if (st == 2) {
        float dx = spot[0] - o->c[0], dy = spot[1] - o->c[1]; int there = dx * dx + dy * dy < 1.0f;
        if (o->shield >= until || W->now - t0 > 6.0) { st = 0; if (bot_log()) nlog("[nhook] [cover] out after %.1f s, shield %.2f", W->now - t0, o->shield); return; }
        if (!(there && hit && W->now - tsearch > 1.0)) { arb_bid(a, ARB_RECHARGE, u, spot, "take cover"); return; }
        st = 0; if (bot_log()) nlog("[nhook] [cover] hit hard in cover (shield %.2f): looking again", o->shield); }
    if (st == 1) {
        int pend = 0;
        for (int k = 0; k < g_cov_n; k++) { int r = los_get(cov_tag(k), W->now);
            if (r == 0) { memcpy(spot, g_cov_c[k], 12); spot[2] = fz[k] + 0.1f; st = 2; t0 = W->now; g_cov_n = 0;
                if (bot_log()) nlog("[nhook] [cover] shield %.2f: spot %d of the candidates hidden from %08x, %.1f units away -- going", o->shield, k, g_cov_from, sqrtf((spot[0] - o->c[0]) * (spot[0] - o->c[0]) + (spot[1] - o->c[1]) * (spot[1] - o->c[1])));
                arb_bid(a, ARB_RECHARGE, u, spot, "take cover"); return; }
            if (r < 0) pend++; }
        if ((g_cov_k >= g_cov_n && !pend) || W->now - tsearch > 1.5) { if (bot_log()) nlog("[nhook] [cover] none of %d spots hidden from %08x: fighting on", g_cov_n, g_cov_from); st = 0; g_cov_n = 0; }
        return; }
    if (!hit || !g_ftg_vis || !g_ftg_h || W->now - tsearch < 1.0) return;
    int cur = nav_node_at(o->c); if (cur < 0) return; float cp[3]; nav_pos(cur, cp); int comp = g_nav.nd[cur].comp; float hz = o->c[2] - cp[2] + 0.55f * o->r;
    float ax = o->c[0] - g_ftg_c[0], ay = o->c[1] - g_ftg_c[1], al = sqrtf(ax * ax + ay * ay); if (al < 0.5f) return; ax /= al; ay /= al;
    static const float cand[8][2] = {{90, 3}, {-90, 3}, {45, 3.5f}, {-45, 3.5f}, {0, 4}, {90, 5}, {-90, 5}, {0, 6}};
    int nc = 0, used[8];
    for (int i = 0; i < 8 && nc < 8; i++) { float an = cand[i][0] / 57.2958f, r = cand[i][1], c = cosf(an), sn = sinf(an);
        float p[3] = {o->c[0] + (ax * c - ay * sn) * r, o->c[1] + (ax * sn + ay * c) * r, o->c[2] + 0.5f}; int nd = nav_node_at(p); if (nd < 0 || nd == cur || g_nav.nd[nd].comp != comp) continue;
        float np[3]; nav_pos(nd, np); if (fabsf(np[2] - cp[2]) > 1.5f) continue; int dup = 0; for (int j = 0; j < nc; j++) if (used[j] == nd) dup = 1; if (dup) continue;
        used[nc] = nd; fz[nc] = np[2]; g_cov_c[nc][0] = np[0]; g_cov_c[nc][1] = np[1]; g_cov_c[nc][2] = np[2] + hz; nc++; }
    if (!nc) return;
    g_cov_gen++; g_cov_n = nc; g_cov_k = 0; g_cov_from = g_ftg_h; st = 1; tsearch = W->now;
    if (bot_log()) nlog("[nhook] [cover] shield %.2f (from %.2f in 1.5 s), %08x at %.0f units: testing %d spots", o->shield, mx, g_ftg_h, al, nc); }
static void beh_recharge(const World* W, Arb* a) {
    const NovObj* o = W->o; static double t0; float u = 65, lim = 0.6f; lk_over(&u, "NHOOK_U_RECHARGE"); lk_over(&lim, "NHOOK_BOT_RECHARGE_SH");
    if (u <= 0 || o->shield >= 0.95f) { t0 = 0; return; }
    if (g_eng_close) return;
    if (o->shield >= lim && t0 <= 0) return;
    if (t0 <= 0) { t0 = W->now; if (bot_log()) nlog("[nhook] [recharge] shield %.2f, nobody in sight: backing off to recharge", o->shield); }
    if (W->now - t0 > 6) return;
    int ei = -1; float ed = 1e30f;
    for (int i = 0; i < W->n; i++) { const NovObj* t = &W->objs[i]; if (i == W->me || t->type || t->dead || !bot_enemy(o, t)) continue; float dx = t->c[0] - o->c[0], dy = t->c[1] - o->c[1], d = dx * dx + dy * dy; if (d < ed) { ed = d; ei = i; } }
    if (ei < 0) return;
    float dx = o->c[0] - W->objs[ei].c[0], dy = o->c[1] - W->objs[ei].c[1], dl = sqrtf(dx * dx + dy * dy) + 1e-3f;
    float pt[3] = {o->c[0] + dx / dl * 6.0f, o->c[1] + dy / dl * 6.0f, o->c[2]};
    arb_bid(a, ARB_RECHARGE, u, pt, "recharge the shield"); }
