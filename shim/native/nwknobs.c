// nwknobs.c -- the stats page's Walker tab (2026-11-09, the user: "add a walker settings page to the status window as a tab please so
// we can adjust it on the fly"; included by nstats.c after nbot.c). The walkers' LIVE knob files -- nbot.c lk_tick reads
// <ReclaimerMac>/battles/live-knobs/<nickname>.json within a second of a change, no restart, no rejoin -- listed and edited from the
// page. Two folders, both found under the user's REAL home (the user database, not $HOME: the public walker runs with a HOME of its
// own, walker-public.sh):
//   test  = ReclaimerMac/battles/live-knobs                                   (local battle walkers, by nickname)
//   pub   = ReclaimerMac/native/public-walker/home/Library/Application Support/ReclaimerMac/battles/live-knobs   (the public walker)
// GET  /walker.json -> {"self":<this game's own file id or null>,"files":[{"id":"pub/<nick>","nick":..,"where":"pub"|"test","mine":..,
//      "knobs":{"NHOOK_...":value,...}}]}
// POST /control?do=wknob&f=<id>&set=NAME:VALUE,NAME:VALUE&del=NAME,NAME|all  (names NHOOK_[A-Z0-9_]+; the file is rewritten whole,
//      through a temporary file + rename, so the walker never reads half of it). A knob taken out goes back to the launch value.
#include <pwd.h>
#include <dirent.h>

static const char* wk_home(void) {   // (the user's real home)
    // (NHOOK_WKNOBS_HOME: another home, for shim/tools/wknobs-test.sh)
    static char h[512]; if (!h[0] && getenv("NHOOK_WKNOBS_HOME") && *getenv("NHOOK_WKNOBS_HOME")) snprintf(h, sizeof h, "%s", getenv("NHOOK_WKNOBS_HOME"));
    if (!h[0]) { struct passwd* pw = getpwuid(getuid()); snprintf(h, sizeof h, "%s", pw && pw->pw_dir ? pw->pw_dir : (getenv("HOME") ? getenv("HOME") : "/tmp")); }
    return h; }
static void wk_dir(int pub, char* out, size_t n) {
    if (pub) snprintf(out, n, "%s/Library/Application Support/ReclaimerMac/native/public-walker/home/Library/Application Support/ReclaimerMac/battles/live-knobs", wk_home());
    else snprintf(out, n, "%s/Library/Application Support/ReclaimerMac/battles/live-knobs", wk_home()); }
static int wk_nick_ok(const char* s) {   // (a file name: letters, digits, space _ - . ; not starting with '.')
    size_t n = strlen(s); if (!n || n > 32 || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) { char c = s[i]; if (!(isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.')) return 0; }
    return 1; }
static int wk_name_ok(const char* s) {
    if (strncmp(s, "NHOOK_", 6)) return 0; size_t n = strlen(s); if (n > 46) return 0;
    for (size_t i = 6; i < n; i++) if (!(isupper((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_')) return 0;
    return n > 6; }
static int wk_path(const char* id, char* out, size_t n) {   // "pub/<nick>" | "test/<nick>" -> the file
    int pub = !strncmp(id, "pub/", 4); if (!pub && strncmp(id, "test/", 5)) return 0;
    const char* nick = id + (pub ? 4 : 5); if (!wk_nick_ok(nick)) return 0;
    char d[1024]; wk_dir(pub, d, sizeof d); snprintf(out, n, "%s/%s.json", d, nick); return 1; }
static int wk_read(const char* path, LiveKnob* k, int max) {   // (lk_tick's parse: every "NAME": number pair)
    FILE* f = fopen(path, "r"); if (!f) return -1; char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    int nk = 0; const char* q = b;
    while (nk < max && (q = strchr(q, '"'))) { const char* e = strchr(q + 1, '"'); if (!e) break; size_t L = (size_t)(e - q - 1);
        const char* c = e + 1; while (*c == ' ' || *c == ':' || *c == '\t') c++; char* end; float v = strtof(c, &end);
        if (end != c && L > 0 && L < sizeof k[0].n) { memcpy(k[nk].n, q + 1, L); k[nk].n[L] = 0; k[nk].v = v; nk++; }
        q = end != c ? end : e + 1; }
    return nk; }
static int wk_write(const char* path, const LiveKnob* k, int nk) {
    char dir[1100]; snprintf(dir, sizeof dir, "%s", path); char* sl = strrchr(dir, '/'); if (!sl) return 0; *sl = 0;
    { char p[1100]; snprintf(p, sizeof p, "%s", dir); for (char* s2 = p + 1; *s2; s2++) if (*s2 == '/') { *s2 = 0; mkdir(p, 0755); *s2 = '/'; } mkdir(p, 0755); }
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.page-%d.tmp", path, (int)getpid()); FILE* f = fopen(tmp, "w"); if (!f) return 0;
    fputc('{', f); for (int i = 0; i < nk; i++) fprintf(f, "%s\"%s\": %.6g", i ? ", " : "", k[i].n, (double)k[i].v); fputs("}\n", f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1; }
// the last <max> JSON lines of a file (our own: battles/follow/why-<nick>.jsonl, tuner-<nick>.jsonl) as a JSON array
static void wk_tail_json(StBuf* b, const char* path, int max) {
    sb_put(b, "["); FILE* f = fopen(path, "r"); if (!f) { sb_put(b, "]"); return; }
    static char buf[65536]; fseek(f, 0, SEEK_END); long sz = ftell(f); long from = sz > (long)sizeof buf - 1 ? sz - (long)sizeof buf + 1 : 0; fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char* lines[256]; int nl = 0; char* p = buf; if (from > 0) { char* nlp = strchr(p, '\n'); p = nlp ? nlp + 1 : p + n; }
    while (*p) { char* e = strchr(p, '\n'); if (e) *e = 0; size_t L = strlen(p); int ok = L > 1 && p[0] == '{' && p[L - 1] == '}';
        for (size_t i = 0; ok && i < L; i++) if ((unsigned char)p[i] < 0x20) ok = 0;
        if (ok) { if (nl == 256) { memmove(lines, lines + 1, sizeof lines[0] * 255); nl = 255; } lines[nl++] = p; }
        if (!e) break; p = e + 1; }
    int k0 = nl > max ? nl - max : 0; for (int i = k0; i < nl; i++) sb_put(b, "%s%s", i > k0 ? "," : "", lines[i]);
    sb_put(b, "]"); }
static void wk_file_json(StBuf* b, const char* id, const char* nick, int pub, const char* path, int mine, int* first) {
    LiveKnob k[64]; int nk = wk_read(path, k, 64);
    sb_put(b, "%s{\"id\":", *first ? "" : ","); sb_str(b, id); sb_put(b, ",\"nick\":"); sb_str(b, nick);
    sb_put(b, ",\"where\":\"%s\",\"mine\":%s,\"exists\":%s,\"knobs\":{", pub ? "pub" : "test", mine ? "true" : "false", nk >= 0 ? "true" : "false");
    for (int i = 0; i < nk; i++) { sb_put(b, "%s", i ? "," : ""); sb_str(b, k[i].n); sb_put(b, ":%.6g", isfinite(k[i].v) ? (double)k[i].v : 0.0); }
    // (the death review and the tuner's steps: nbot.c why_note, shim/tools/death-tuner.py)
    char fp[1100]; snprintf(fp, sizeof fp, "%s/Library/Application Support/ReclaimerMac/battles/follow/why-%s.jsonl", wk_home(), nick);
    sb_put(b, "},\"why\":"); wk_tail_json(b, fp, 60);
    snprintf(fp, sizeof fp, "%s/Library/Application Support/ReclaimerMac/battles/follow/tuner-%s.jsonl", wk_home(), nick);
    sb_put(b, ",\"tune\":"); wk_tail_json(b, fp, 16); sb_put(b, "}"); *first = 0; }
static const char* wk_pub_nick(void) {   // (the public walker's nickname: its profile's identity.nickname -- nothing else is read from it)
    static char nk[64]; static int done; if (done) return nk; done = 1;
    char p[1100]; snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/native/public-walker/home/Library/Application Support/ProjectReclaimer/profile.json", wk_home());
    FILE* f = fopen(p, "r"); if (!f) return nk; char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    const char* q = strstr(buf, "\"nickname\""); if (q) q = strchr(q + 10, ':'); if (q) q = strchr(q, '"');
    if (q) { const char* e = strchr(q + 1, '"'); if (e && e - q - 1 < (long)sizeof nk) { memcpy(nk, q + 1, (size_t)(e - q - 1)); nk[e - q - 1] = 0; } }
    if (!wk_nick_ok(nk)) nk[0] = 0; return nk; }
static void wk_json(StBuf* b) {
    char own[1100] = ""; if (g_bot_started) { char* r = realpath(lk_path(), 0); snprintf(own, sizeof own, "%s", r ? r : lk_path()); free(r); }
    // (the public walker's home links ReclaimerMac back to the user's -- 2026-11-09: one folder seen twice, every file listed twice:
    //  then the folder is listed once, and the public walker is told apart by its nickname)
    char d0[1024], d1[1024], rd[2][1100]; wk_dir(0, d0, sizeof d0); wk_dir(1, d1, sizeof d1);
    { char* r = realpath(d0, 0); snprintf(rd[0], sizeof rd[0], "%s", r ? r : d0); free(r); r = realpath(d1, 0); snprintf(rd[1], sizeof rd[1], "%s", r ? r : d1); free(r); }
    int same = !strcmp(rd[0], rd[1]); const char* pn = wk_pub_nick();
    char selfid[80] = ""; int first = 1; sb_put(b, "{\"files\":[");
    for (int pub = 1; pub >= 0; pub--) { if (pub && same) continue;
        const char* d = pub ? d1 : d0; DIR* dp = opendir(d); int cnt = 0;
        while (dp && cnt < 32) { struct dirent* de = readdir(dp); if (!de) break; size_t L = strlen(de->d_name);
            if (L < 6 || strcmp(de->d_name + L - 5, ".json")) continue;
            char nick[64]; snprintf(nick, sizeof nick, "%.*s", (int)(L - 5), de->d_name); if (!wk_nick_ok(nick)) continue;
            char id[80], path[1200]; snprintf(id, sizeof id, "%s/%s", pub ? "pub" : "test", nick); snprintf(path, sizeof path, "%s/%s", d, de->d_name);
            char* r = realpath(path, 0); int mine = own[0] && r && !strcmp(r, own); free(r); if (mine) snprintf(selfid, sizeof selfid, "%s", id);
            wk_file_json(b, id, nick, pub || (pn[0] && !strcmp(nick, pn)), path, mine, &first); cnt++; }
        if (dp) closedir(dp); }
    // (this game is a walker with no file yet: its own entry, created by the first change)
    if (own[0] && !selfid[0] && g_nick[0] && wk_nick_ok(g_nick)) for (int pub = 1; pub >= 0; pub--) { size_t dl = strlen(rd[pub]);
        if (!(pub && same) && !strncmp(own, rd[pub], dl) && own[dl] == '/') { snprintf(selfid, sizeof selfid, "%s/%s", pub ? "pub" : "test", g_nick);
            wk_file_json(b, selfid, g_nick, pub || (pn[0] && !strcmp(g_nick, pn)), own, 1, &first); break; } }
    sb_put(b, "],\"self\":"); if (selfid[0]) sb_str(b, selfid); else sb_put(b, "null"); sb_put(b, "}"); }
static int wk_qs(const char* qs, const char* key, char* out, size_t n) {   // one query parameter, %XX and '+' decoded
    size_t kl = strlen(key); const char* p = qs;
    while (p && *p) { const char* amp = strchr(p, '&'); size_t L = amp ? (size_t)(amp - p) : strlen(p);
        if (L > kl && !strncmp(p, key, kl) && p[kl] == '=') { size_t o = 0; for (size_t i = kl + 1; i < L && o + 1 < n; i++) { char c = p[i];
                if (c == '+') c = ' '; else if (c == '%' && i + 2 < L + 1 && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) { char h[3] = {p[i + 1], p[i + 2], 0}; c = (char)strtol(h, 0, 16); i += 2; }
                out[o++] = c; }
            out[o] = 0; return 1; }
        p = amp ? amp + 1 : 0; }
    return 0; }
static int wk_control(const char* qs) {
    char id[80], path[1200], set[700] = "", del[700] = ""; if (!wk_qs(qs, "f", id, sizeof id) || !wk_path(id, path, sizeof path)) return 0;
    wk_qs(qs, "set", set, sizeof set); wk_qs(qs, "del", del, sizeof del); if (!set[0] && !del[0]) return 0;
    LiveKnob k[64]; int nk = wk_read(path, k, 64); if (nk < 0) nk = 0;
    if (!strcmp(del, "all")) nk = 0;
    else for (char* t = strtok(del, ","); t; t = strtok(0, ",")) for (int i = 0; i < nk; i++) if (!strcmp(k[i].n, t)) { k[i] = k[--nk]; break; }
    for (char* t = set; *t; ) { char* e = strchr(t, ','); if (e) *e = 0; char* c = strchr(t, ':');
        if (c) { *c = 0; char* end; float v = strtof(c + 1, &end);
            if (!wk_name_ok(t) || end == c + 1 || !isfinite(v) || fabsf(v) > 1e6f) return 0;
            int i = 0; while (i < nk && strcmp(k[i].n, t)) i++;
            if (i == nk) { if (nk >= 64) return 0; snprintf(k[nk].n, sizeof k[nk].n, "%s", t); nk++; }
            k[i].v = v; }
        if (!e) break; t = e + 1; }
    if (!wk_write(path, k, nk)) return 0;
    nlog("[nhook] page: walker knobs %s -> %d in the file", id, nk);
    return 1; }
