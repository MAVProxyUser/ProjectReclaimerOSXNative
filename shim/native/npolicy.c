// npolicy.c -- the walker's learnt MOVEMENT POLICY (2026-11-09; the user: "[the best player] simply moves more nimble, and engages
// faster ... our walker does not move with that much *authority* and confidence"; "M4 has AI cores... use them for walker logic").
// shim/tools/policy-train.py learns, on the GPU, how the best players of each recorded match move in a fight -- their next move toward /
// across the enemy line and their jumps, from what they saw -- and writes ReclaimerMac/battles/follow/policy.json. This file reads it
// whenever it changes (no restart), builds the same inputs from the walker's own state 10 times a second (the recordings' rate) and
// runs the network (2 hidden layers of 128; ~20k multiply-adds, microseconds on a CPU core -- a Neural Engine call would cost more
// in dispatch than the network does). The fight blends the policy's move with its own by NHOOK_BOT_POLICY (live, 0 = off, 1 = the
// policy alone); its jumps go out at NHOOK_BOT_POLICY >= 0.5. Included by nbot.c (brain code).

enum { POL_MAXIN = 64, POL_MAXH = 256, POL_MAXL = 4 };
static struct { int ok, nin, nl, rows[POL_MAXL], cols[POL_MAXL]; float mean[POL_MAXIN], sd[POL_MAXIN]; float* W[POL_MAXL]; float* b[POL_MAXL]; char trained[32]; float val, still, rep; } g_pol;

// a JSON number array (nested brackets flattened) starting at the first '[' at or after p; -> count read (at most max), *end past it
static int pol_floats(const char* p, float* out, int max, const char** end) {
    p = strchr(p, '['); if (!p) return -1; int depth = 0, n = 0;
    for (; *p; p++) { if (*p == '[') depth++; else if (*p == ']') { if (--depth == 0) { p++; break; } }
        else if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') { char* e; float v = strtof(p, &e); if (e == p) continue; if (n < max) out[n] = v; n++; p = e - 1; } }
    if (end) *end = p; return n; }
static void pol_tick(double now) {
    static double tc; static time_t mt; if (now - tc < 2.0) return; tc = now;
    char path[1024]; const char* h = getenv("HOME"); snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/battles/follow/policy.json", h ? h : "");
    struct stat st; if (stat(path, &st) || st.st_mtime == mt) return; mt = st.st_mtime;
    FILE* f = fopen(path, "r"); if (!f) return; char* buf = malloc((size_t)st.st_size + 1); if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f); fclose(f); buf[n] = 0;
    float mean[POL_MAXIN], sd[POL_MAXIN]; const char* p = strstr(buf, "\"mean\""), * q; int nin = p ? pol_floats(p, mean, POL_MAXIN, &q) : -1;
    p = strstr(buf, "\"std\""); int ns = p ? pol_floats(p, sd, POL_MAXIN, &q) : -1;
    const char* L = strstr(buf, "\"layers\""); int nl = 0, prev = nin, bad = nin <= 0 || nin > POL_MAXIN || ns != nin || !L; float* W[POL_MAXL] = {0}; float* B[POL_MAXL] = {0}; int rows[POL_MAXL], cols[POL_MAXL];
    while (!bad && nl < POL_MAXL) { const char* w = strstr(L, "\"W\""); if (!w) break; const char* bb = strstr(w, "\"b\""); if (!bb) { bad = 1; break; }
        int cap = POL_MAXH * POL_MAXH; float* wv = malloc(sizeof(float) * (size_t)cap); const char* e; int nw = pol_floats(w, wv, cap, &e);
        float* bv = malloc(sizeof(float) * POL_MAXH); int nb = pol_floats(bb, bv, POL_MAXH, &e);
        if (nw <= 0 || nb <= 0 || nb > POL_MAXH || nw != nb * prev) { free(wv); free(bv); bad = 1; break; }
        W[nl] = wv; B[nl] = bv; rows[nl] = nb; cols[nl] = prev; prev = nb; nl++; L = e; }
    if (bad || nl < 2 || rows[nl - 1] != 3) { nlog("[nhook] [policy] %s: not a policy this walker can run (inputs %d, layers %d)", path, nin, nl); for (int i = 0; i < nl; i++) { free(W[i]); free(B[i]); } free(buf); return; }
    for (int i = 0; i < g_pol.nl; i++) { free(g_pol.W[i]); free(g_pol.b[i]); }
    g_pol.nin = nin; g_pol.nl = nl; memcpy(g_pol.mean, mean, sizeof mean); for (int i = 0; i < nin; i++) g_pol.sd[i] = sd[i] > 1e-6f ? sd[i] : 1;
    for (int i = 0; i < nl; i++) { g_pol.W[i] = W[i]; g_pol.b[i] = B[i]; g_pol.rows[i] = rows[i]; g_pol.cols[i] = cols[i]; }
    { const char* t = strstr(buf, "\"trained\""); g_pol.trained[0] = 0; if (t && (t = strchr(t + 9, '"'))) { int k = 0; for (t++; *t && *t != '"' && k < 31; t++) g_pol.trained[k++] = *t; g_pol.trained[k] = 0; } }
    { const char* t; g_pol.val = (t = strstr(buf, "\"val_move_err\"")) ? strtof(t + 15, 0) : 0; g_pol.still = (t = strstr(buf, "\"val_still_err\"")) ? strtof(t + 16, 0) : 0; g_pol.rep = (t = strstr(buf, "\"val_repeat_err\"")) ? strtof(t + 17, 0) : 0; }
    g_pol.ok = 1; free(buf);
    nlog("[nhook] [policy] loaded (trained %s): %d inputs, %d layers; validation move error %.3f vs standing still %.3f, repeating the last move %.3f", g_pol.trained, nin, nl, g_pol.val, g_pol.still, g_pol.rep); }
static void pol_run(const float* x, float* o) {   // -> o[3]
    static float a[POL_MAXH], c[POL_MAXH]; int n = g_pol.nin; for (int i = 0; i < n; i++) a[i] = (x[i] - g_pol.mean[i]) / g_pol.sd[i];
    for (int l = 0; l < g_pol.nl; l++) { const float* W = g_pol.W[l]; int R = g_pol.rows[l], C = g_pol.cols[l];
        for (int r = 0; r < R; r++) { float s = g_pol.b[l][r]; const float* w = W + (size_t)r * C; for (int k = 0; k < C; k++) s += w[k] * a[k]; c[r] = l + 1 < g_pol.nl ? s / (1.0f + expf(-s)) : s; }
        memcpy(a, c, sizeof(float) * (size_t)R); }
    o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
static int pol_cls(const char* c) { static const char* const k[] = {"auto", "precision", "shotgun", "sniper", "rocket", "melee", "other"}; for (int i = 0; i < 7; i++) if (c && !strcmp(c, k[i])) return i; return 6; }
// the policy's move in the walker's view frame (*r toward, *lat left; throttle -1..1) and its jump wish; -> 1 when it ran
static int pol_move(double now, const NovObj* objs, int n, int me, const NovObj* o, const NovObj* E, float yaw0, float* r, float* lat, int* jump) {
    if (!g_pol.ok || g_pol.nin != 27) return 0;
    static double ts; static float hist[3][3]; static uint32_t eh; static float eprev[3], ecur[3]; static float out[3]; static int have;
    if (now - ts >= 0.1) { ts = now; memmove(hist[1], hist[0], sizeof(float) * 6); memcpy(hist[0], o->c, 12);
        if (E->handle != eh) { eh = E->handle; memcpy(eprev, E->c, 12); } else memcpy(eprev, ecur, 12); memcpy(ecur, E->c, 12); have++;
        if (have >= 3) {
            float dx = E->c[0] - o->c[0], dy = E->c[1] - o->c[1], dE = sqrtf(dx * dx + dy * dy); if (dE < 0.3f) return 0;
            float ux = dx / dE, uy = dy / dE, vx = -uy, vy = ux; float x[27];
            #define UV(ax, ay, U, V) do { U = 10.0f * ((ax) * ux + (ay) * uy); V = 10.0f * ((ax) * vx + (ay) * vy); } while (0)
            float m1u, m1v, m2u, m2v, e1u, e1v; UV(hist[0][0] - hist[1][0], hist[0][1] - hist[1][1], m1u, m1v); UV(hist[1][0] - hist[2][0], hist[1][1] - hist[2][1], m2u, m2v);
            UV(ecur[0] - eprev[0], ecur[1] - eprev[1], e1u, e1v);
            float ey = atan2f(E->aim[1], E->aim[0]), ef = cosf(ey) * (o->c[0] - E->c[0]) / dE + sinf(ey) * (o->c[1] - E->c[1]) / dE;
            float vc = cosf(yaw0) * ux + sinf(yaw0) * uy, vs = cosf(yaw0) * vx + sinf(yaw0) * vy;
            float d2 = 30, c2 = 0, s2 = 0; int n10 = 0;
            for (int i = 0; i < n; i++) { const NovObj* t = &objs[i]; if (i == me || t->type || t->dead || !bot_enemy(o, t)) continue; float tx = t->c[0] - o->c[0], ty = t->c[1] - o->c[1], d = sqrtf(tx * tx + ty * ty);
                if (d < 10) n10++; if (t->handle != E->handle && d < d2) { d2 = d; c2 = (tx * ux + ty * uy) / fmaxf(d, 1e-3f); s2 = (tx * vx + ty * vy) / fmaxf(d, 1e-3f); } }
            float f0[20] = {dE / 10, E->c[2] - o->c[2], o->shield, o->health, E->shield, E->health, ef, vc, vs, m1u, m1v, m2u, m2v, e1u, e1v, d2 / 10, c2, s2, n10 / 4.0f, 10.0f * (hist[0][2] - hist[1][2])};
            memcpy(x, f0, sizeof f0); for (int k = 0; k < 7; k++) x[20 + k] = 0; x[20 + pol_cls(g_inv.held_c)] = 1;
            pol_run(x, out);
            // (the world move -> the view frame)
            float wx = out[0] * ux + out[1] * vx, wy = out[0] * uy + out[1] * vy, fx = cosf(yaw0), fy = sinf(yaw0), run = g_bm.run_speed > 0.5f ? g_bm.run_speed : 2.1f;
            out[0] = fmaxf(-1, fminf(1, (wx * fx + wy * fy) / run)); out[1] = fmaxf(-1, fminf(1, (wx * -fy + wy * fx) / run));
            #undef UV
        } }
    if (have < 3) return 0;
    *r = out[0]; *lat = out[1]; *jump = out[2] > 0.0f; return 1; }
