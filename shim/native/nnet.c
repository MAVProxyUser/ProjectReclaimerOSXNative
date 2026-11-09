// nnet.c -- the NATIVE app's network side for the stats page (included by nstats.c): the network session (everyone
// connected, also in the lobby; the "Connected" list, the host badge, player slots) and the packet log (the TYPE of every
// UDP datagram / TCP connection, never the content: the loader's pktlog.c wire layer, fed by the interposed socket calls
// in nhook.c). Layout sources: the app's own log lines ([c1] network sessions moved to ..., [capacity] session.*_shift).

// ---- the network session (shim/docs/reclaimer-voice-mute.md section 4; the same extended layout as the 0.9 client) ----
// sess = *(u64*)(halo3+0x1e250d0) (the active session, inside the app's moved session storage). The app moves the peer and
// player records to the end of each session: peers sess+0x58+peers_shift (0x1381c0), players sess+0x1270+players_shift
// (0x1493c0), as Reclaimer 0.9 did; the valid masks stay at sess+0x54 (peers) / sess+0x126c (players), 16 bytes each.
//   player i: +0x04 u64 identifier (0 = none), +0x0c i32 peer index, +0x98 UTF-16 name[16]
//   peer p:   +0x00 u64 peer id (the voice id), +0x08 u32 state
//   sess+0x67b70 state (4..9 = up), +0x38 membership update number (-1 = none), +0x40 host peer, +0x3dc4 our peer,
//   +0x4194 bit 0 -> +0x419c the session's maximum player count
typedef struct { char name[64]; int idx, peer; uint64_t voice; int muted, host; } StSess;
static volatile uint64_t g_ses_store;                       // "[c1] network sessions moved to 0x..." (0 = not seen)
static volatile int64_t g_ses_peers_shift = 0x138168, g_ses_players_shift = 0x148150;
static int g_st_hostpeer = -1, g_st_localpeer = -1; static volatile uint64_t g_st_host_id;   // (host_id: the host peer's id -- a dedicated server's, which is no player)
static uint64_t st_sess_ptr(void) {
    uint64_t sess = 0; if (!g_img || !hb_read(g_img + 0x1e250d0, &sess, 8) || sess < 0x10000) return 0;
    return sess;
}
static int muted_list(uint64_t* out, int max);
static int st_session(StSess* out, int max, uint32_t* state) {
    uint64_t sess = st_sess_ptr(); if (!sess) return 0;
    uint32_t st = 0; hb_read(sess + 0x67b70, &st, 4); if (state) *state = st;
    { int32_t upd = -1, hp = -1, lp = -1;
      if (hb_read(sess + 0x38, &upd, 4) && upd != -1 && st >= 4 && st <= 9 && hb_read(sess + 0x40, &hp, 4) && hb_read(sess + 0x3dc4, &lp, 4)
          && hp >= -1 && hp < 128 && lp >= -1 && lp < 128) { g_st_hostpeer = hp; g_st_localpeer = lp; }
      else { g_st_hostpeer = -1; g_st_localpeer = -1; } }
    uint8_t pm[16] = {0}, qm[16] = {0};
    if (!hb_read(sess + 0x126c, pm, 16) || !hb_read(sess + 0x54, qm, 16)) return 0;
    { uint64_t hid = 0; if (g_st_hostpeer >= 0 && ((qm[g_st_hostpeer >> 3] >> (g_st_hostpeer & 7)) & 1) && hb_read(sess + 0x58 + (uint64_t)g_ses_peers_shift + (uint64_t)g_st_hostpeer * 0x110, &hid, 8)) g_st_host_id = hid; }
    uint64_t pbase = sess + 0x1270 + (uint64_t)g_ses_players_shift, qbase = sess + 0x58 + (uint64_t)g_ses_peers_shift; int n = 0;
    for (int i = 0; i < 128 && n < max; i++) {
        if (!((pm[i >> 3] >> (i & 7)) & 1)) continue;
        uint8_t r[0xc8]; if (!hb_read(pbase + (uint64_t)i * 0xc8, r, sizeof r) || !*(uint64_t*)(r + 4)) continue;
        int pi = *(int32_t*)(r + 0xc); if (pi < 0 || pi >= 128 || !((qm[pi >> 3] >> (pi & 7)) & 1)) continue;
        uint8_t q[0x10]; if (!hb_read(qbase + (uint64_t)pi * 0x110, q, sizeof q)) continue;
        StSess* e = &out[n]; e->idx = i; e->peer = pi; e->voice = *(uint64_t*)q; e->muted = 0; e->host = g_st_hostpeer >= 0 && pi == g_st_hostpeer;
        int j = 0, ok = 1; const uint16_t* w = (const uint16_t*)(r + 0x98);
        for (int c = 0; c < 16 && w[c] && j < 60; c++) { uint16_t u = w[c]; if (u < 0x20) { ok = 0; break; }
            if (u < 0x80) e->name[j++] = (char)u; else if (u < 0x800) { e->name[j++] = (char)(0xC0 | (u >> 6)); e->name[j++] = (char)(0x80 | (u & 63)); }
            else { e->name[j++] = (char)(0xE0 | (u >> 12)); e->name[j++] = (char)(0x80 | ((u >> 6) & 63)); e->name[j++] = (char)(0x80 | (u & 63)); } }
        if (!ok || !j) continue; e->name[j] = 0; n++;
    }
    { static uint64_t mu[256]; int nm = muted_list(mu, 256); for (int i = 0; i < n; i++) for (int k = 0; k < nm; k++) if (mu[k] == out[i].voice) { out[i].muted = 1; break; } }
    if (n > 0) { HK_ONCE(h, "network session"); hk_fire(h); }
    return n;
}
// ---- voice mute (the app's own list: static Mutex<Vec<u64 peer id>> -- lock box +0, poison +8, cap +0x10, ptr +0x18, len +0x20;
// set_muted(id, bool) appends / removes under the lock; the scoreboard's own MUTE VOICE buttons use it (found from their
// strings, re-tools/machoref.py). 0.10.0: list 0x103f0a1e0, set_muted 0x102154ef0 (is_muted 0x10214db98); 0.10.1: 0x10409a728 /
// 0x102214604 -- napp_builds.h `muted` / `set_muted`). The ids are the session's peer ids (StSess.voice). set_muted is checked
// before use: its adrp / add pair must reach the list's lock.
static void (*g_set_muted)(uint64_t, int);
static int adrp_add_target_is(uint64_t f, int lim, uint64_t want) {   // an adrp + add (same register) in the function's first lim bytes reaching want
    uint32_t w[32]; if (lim > 128 || rd(f, w, (size_t)lim)) return 0;
    for (int i = 0; i + 1 < lim / 4; i++) { uint32_t a = w[i], b = w[i + 1];
        if ((a & 0x9F000000u) != 0x90000000u || (b & 0xFF800000u) != 0x91000000u || ((b >> 5) & 31) != (a & 31)) continue;
        int64_t imm = (int64_t)(((a >> 29) & 3) | (((a >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
        uint64_t pc = f + (uint64_t)i * 4, t = (pc & ~0xFFFull) + (uint64_t)(imm << 12) + ((b >> 10) & 0xFFF); if (t == want) return 1; }
    return 0;
}
static void voice_init(void) {
    static int done; if (done || !g_img) return; done = 1; uint64_t f = APP(set_muted), l = APP(muted); int h = hk_named("set_muted");
    if (f && l && adrp_add_target_is(f, 64, l)) { g_set_muted = (void (*)(uint64_t, int))(uintptr_t)f; hk_state(h, HK_OK, f, "set_muted at 0x%llx reaches the mute list 0x%llx", (unsigned long long)f, (unsigned long long)l);
        nlog("[nhook] voice: mute available (set_muted at 0x%llx)", (unsigned long long)f); }
    else { hk_state(h, HK_FAIL, f, f ? "set_muted at 0x%llx does not reach the mute list: page mutes off" : "not in this build's table: page mutes off", (unsigned long long)f);
        nlog("[nhook] voice: set_muted not where this build's table has it: page mutes off"); }
}
static int muted_list(uint64_t* out, int max) {   // the app's mute list (read without its lock: a racy copy is fine for showing it)
    voice_init(); if (!g_set_muted) return 0; uint64_t l = APP(muted), ptr = 0, len = 0;
    if (rd(l + 0x18, &ptr, 8) || rd(l + 0x20, &len, 8) || !ptr || len > 4096) return 0;
    int n = (int)(len < (uint64_t)max ? len : (uint64_t)max); if (n && rd(ptr, out, (size_t)n * 8)) return 0; return n;
}
static int st_max_players(void) {                          // the session's limit (host or joined), 0 = not known
    uint64_t sess = st_sess_ptr(); uint32_t st = 0, fl = 0; int32_t mx = 0;
    if (sess && hb_read(sess + 0x67b70, &st, 4) && st >= 4 && st <= 9 && hb_read(sess + 0x4194, &fl, 4) && (fl & 1) && hb_read(sess + 0x419c, &mx, 4) && mx >= 1 && mx <= 128) return mx;
    return 0;
}

// ---- the packet log (pktlog.c's wire layer) ---------------------------------------------------------------------------
#include <netdb.h>
enum { PK_TX = 0, PK_RX = 1, PK_WIRE = 0, PK_GAME = 1, PK_N = 16384, PK_NFD = 4096 };
typedef struct { volatile uint32_t seq; uint8_t layer, dir, fam, flags; uint16_t lport, rport; uint32_t len; double t; uint8_t addr[16]; char type[24]; } PkEv;
_Static_assert(sizeof(PkEv) == 64, "one cache line per record");
static PkEv g_pk[PK_N]; static volatile uint32_t g_pk_seq;
static int g_pk_on = 1;                                     // NHOOK_PKTLOG=0 off
static PkEv* pk_new(uint32_t* sq) { uint32_t s = __atomic_add_fetch(&g_pk_seq, 1, __ATOMIC_RELAXED); PkEv* e = &g_pk[s % PK_N]; __atomic_store_n(&e->seq, 0, __ATOMIC_RELEASE); *sq = s; return e; }
static void pk_done(PkEv* e, uint32_t s) { __atomic_store_n(&e->seq, s, __ATOMIC_RELEASE); }
static struct { uint8_t fam, a[16]; char name[48]; } g_pk_dns[256]; static int g_pk_ndns; static pthread_mutex_t g_pk_dnsmx = PTHREAD_MUTEX_INITIALIZER;
static void pk_dns(const char* name, const struct addrinfo* ai) {
    if (!g_pk_on || !name || !*name || !ai) return; pthread_mutex_lock(&g_pk_dnsmx);
    for (const struct addrinfo* p = ai; p; p = p->ai_next) { uint8_t a[16] = {0}, fam = 0;
        if (p->ai_family == AF_INET) { memcpy(a, &((struct sockaddr_in*)p->ai_addr)->sin_addr, 4); fam = 4; }
        else if (p->ai_family == AF_INET6) { memcpy(a, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, 16); fam = 6; } else continue;
        int k = 0; for (; k < g_pk_ndns; k++) if (g_pk_dns[k].fam == fam && !memcmp(g_pk_dns[k].a, a, 16)) break;
        if (k == g_pk_ndns) { if (g_pk_ndns < 256) g_pk_ndns++; else k = (int)(st_wall() * 7) % 256; }
        g_pk_dns[k].fam = fam; memcpy(g_pk_dns[k].a, a, 16); snprintf(g_pk_dns[k].name, sizeof g_pk_dns[k].name, "%s", name); }
    pthread_mutex_unlock(&g_pk_dnsmx);
}
static void pk_host(const PkEv* e, char* o, size_t on) { o[0] = 0; if (!e->fam) return; pthread_mutex_lock(&g_pk_dnsmx);
    for (int k = 0; k < g_pk_ndns; k++) if (g_pk_dns[k].fam == e->fam && !memcmp(g_pk_dns[k].a, e->addr, 16)) { snprintf(o, on, "%s", g_pk_dns[k].name); break; }
    pthread_mutex_unlock(&g_pk_dnsmx); }
// ---- who a peer is: the app logs each session peer's address ("[online] peer 0x<id> at <ip>:<port>", nst_on_line); the
// network session's peer records hold the same id (st_session .voice) -> the player's name
static struct { uint64_t id; char addr[64]; } g_pkp[512]; static int g_npkp; static pthread_mutex_t g_pkp_mx = PTHREAD_MUTEX_INITIALIZER;
static void pk_peer_note(uint64_t id, const char* addr) {
    pthread_mutex_lock(&g_pkp_mx); int k = 0; for (; k < g_npkp; k++) if (!strcmp(g_pkp[k].addr, addr)) break;
    if (k == g_npkp) { if (g_npkp < 512) g_npkp++; else k = (int)(id % 512); }
    g_pkp[k].id = id; snprintf(g_pkp[k].addr, sizeof g_pkp[k].addr, "%s", addr); pthread_mutex_unlock(&g_pkp_mx); }
static StSess g_pkn[128]; static int g_npkn; static double g_pkn_t; static pthread_mutex_t g_pkn_mx = PTHREAD_MUTEX_INITIALIZER;
static int pk_player(const char* addr, char* out, size_t on) {   // the player at an address, from the session (refreshed once a second)
    uint64_t id = 0; pthread_mutex_lock(&g_pkp_mx); for (int k = 0; k < g_npkp; k++) if (!strcmp(g_pkp[k].addr, addr)) { id = g_pkp[k].id; break; } pthread_mutex_unlock(&g_pkp_mx);
    if (!id) return 0; int ok = 0; pthread_mutex_lock(&g_pkn_mx);
    if (st_wall() - g_pkn_t > 1.0) { g_npkn = st_session(g_pkn, 128, 0); g_pkn_t = st_wall(); }
    for (int i = 0; i < g_npkn; i++) if (g_pkn[i].voice == id) { snprintf(out, on, "%s", g_pkn[i].name); ok = 1; break; }
    if (!ok && id == g_st_host_id) { snprintf(out, on, "server"); ok = 1; }   // (the host peer without a player: a dedicated server)
    pthread_mutex_unlock(&g_pkn_mx); return ok; }
// ---- reverse DNS (the Packets window's "look up names", ?rdns=1): each address's PTR name, once, on a resolver thread
typedef struct { uint8_t fam, a[16]; int st; char name[128]; } PkRd;   // st 0 queued, 1 named, 2 no name
static PkRd g_pkrd[2048]; static int g_npkrd; static pthread_mutex_t g_pkrd_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pkrd_cv = PTHREAD_COND_INITIALIZER;
static void* pk_rdns_thread(void* a) {
    (void)a; pthread_setname_np("nhook-rdns");
    for (;;) {
        pthread_mutex_lock(&g_pkrd_mx); int k = -1;
        while (k < 0) { for (int i = 0; i < g_npkrd; i++) if (!g_pkrd[i].st) { k = i; break; } if (k < 0) pthread_cond_wait(&g_pkrd_cv, &g_pkrd_mx); }
        PkRd q = g_pkrd[k]; pthread_mutex_unlock(&g_pkrd_mx);
        struct sockaddr_storage ss; memset(&ss, 0, sizeof ss); socklen_t sl;
        if (q.fam == 6) { struct sockaddr_in6* s6 = (void*)&ss; s6->sin6_family = AF_INET6; s6->sin6_len = sizeof *s6; memcpy(&s6->sin6_addr, q.a, 16); sl = sizeof *s6; }
        else { struct sockaddr_in* s4 = (void*)&ss; s4->sin_family = AF_INET; s4->sin_len = sizeof *s4; memcpy(&s4->sin_addr, q.a, 4); sl = sizeof *s4; }
        char host[NI_MAXHOST]; int r = getnameinfo((struct sockaddr*)&ss, sl, host, sizeof host, 0, 0, NI_NAMEREQD);
        pthread_mutex_lock(&g_pkrd_mx); g_pkrd[k].st = r ? 2 : 1; if (!r) snprintf(g_pkrd[k].name, sizeof g_pkrd[k].name, "%s", host); pthread_mutex_unlock(&g_pkrd_mx);
    }
    return 0;
}
static int pk_rdns(const PkEv* e, char* out, size_t on) {      // 1 = named, 2 = looked up: no name; unknown addresses are queued
    static int started; if (!started) { started = 1; pthread_t t; nh_thread(&t, pk_rdns_thread, 0, QOS_CLASS_BACKGROUND); }
    int got = 0; pthread_mutex_lock(&g_pkrd_mx); int k = 0;
    for (; k < g_npkrd; k++) if (g_pkrd[k].fam == e->fam && !memcmp(g_pkrd[k].a, e->addr, 16)) break;
    if (k == g_npkrd) { if (g_npkrd < 2048) { g_pkrd[k].fam = e->fam; memcpy(g_pkrd[k].a, e->addr, 16); g_pkrd[k].st = 0; g_npkrd++; pthread_cond_signal(&g_pkrd_cv); } }
    else if (g_pkrd[k].st == 1) { snprintf(out, on, "%s", g_pkrd[k].name); got = 1; }
    else if (g_pkrd[k].st == 2) got = 2;
    pthread_mutex_unlock(&g_pkrd_mx); return got; }
static int pk_addr_of(uint64_t id, char* out, size_t on) {   // a session peer id -> "ip:port" (the app's [online] peer lines)
    int ok = 0; pthread_mutex_lock(&g_pkp_mx); for (int k = 0; k < g_npkp; k++) if (g_pkp[k].id == id) { snprintf(out, on, "%s", g_pkp[k].addr); ok = 1; break; }
    pthread_mutex_unlock(&g_pkp_mx); return ok; }
static int pk_rdns(const PkEv* e, char* out, size_t on);
static int pk_rdns_str(const char* ipport, char* out, size_t on) {   // reverse DNS of "ip:port" / "[ip6]:port" (queued if unknown)
    char ip[64]; snprintf(ip, sizeof ip, "%s", ipport); char* c = strrchr(ip, ':'); if (c) *c = 0; char* q = ip; if (*q == '[') { q++; char* r = strchr(q, ']'); if (r) *r = 0; }
    PkEv e; memset(&e, 0, sizeof e);
    if (inet_pton(AF_INET, q, e.addr) == 1) e.fam = 4; else if (inet_pton(AF_INET6, q, e.addr) == 1) e.fam = 6; else return 0;
    return pk_rdns(&e, out, on); }
static __thread int g_pk_want_rdns;                          // this thread's records get reverse-DNS names (a /packets.json request with
                                                             // rdns=1; the forensic writer always: its packets.jsonl lines keep them)
static void pk_peer_json(StBuf* s, const PkEv* e) {
    char peer[80] = "", host[48], ip[64], who[64], rn[128];
    if (e->fam) { inet_ntop(e->fam == 6 ? AF_INET6 : AF_INET, e->addr, ip, sizeof ip); snprintf(peer, sizeof peer, e->fam == 6 ? "[%s]:%u" : "%s:%u", ip, e->rport); }
    pk_host(e, host, sizeof host);
    if (peer[0]) { sb_put(s, ",\"peer\":"); sb_str(s, peer); } if (host[0]) { sb_put(s, ",\"host\":"); sb_str(s, host); }
    if (peer[0] && pk_player(peer, who, sizeof who)) { sb_put(s, ",\"player\":"); sb_str(s, who); }
    int rr = e->fam && g_pk_want_rdns ? pk_rdns(e, rn, sizeof rn) : 0;
    if (rr == 1) { sb_put(s, ",\"rdns\":"); sb_str(s, rn); } else if (rr == 2) sb_put(s, ",\"rdns_none\":true"); }   // (the page: "no DNS name")
enum { PKF_SPIN = 1, PKF_GREASED = 2 };
static void pk_cpy(char* type, const char* s) { size_t n = strlen(s); if (n > 23) n = 23; memcpy(type, s, n); type[n] = 0; }
static void pk_classify(const uint8_t* b, long n, char* type, uint8_t* fl) {
    *fl = 0; if (n <= 0) { pk_cpy(type, "empty"); return; }
    #define PK_TC(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9') || (c) == '-')
    if (n >= 4 && b[0] >= 'A' && b[0] <= 'Z' && b[1] >= 'A' && b[1] <= 'Z' && PK_TC(b[2]) && PK_TC(b[3])) { memcpy(type, "tag ", 4); memcpy(type + 4, b, 4); type[8] = 0; return; }
    #undef PK_TC
    uint8_t b0 = b[0];
    if (b0 & 0x80) {
        if (n < 7) { pk_cpy(type, "quic-long?"); return; }
        uint32_t v = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4]; int t = (b0 >> 4) & 3;
        if (!v) { pk_cpy(type, "quic-version-neg"); return; }
        if (!(b0 & 0x40)) *fl |= PKF_GREASED;
        static const char* const v1[4] = {"quic-initial", "quic-0-rtt", "quic-handshake", "quic-retry"}, * const v2[4] = {"quic2-retry", "quic2-initial", "quic2-0-rtt", "quic2-handshake"};
        if (v == 1) pk_cpy(type, v1[t]); else if (v == 0x6b3343cf) pk_cpy(type, v2[t]); else snprintf(type, 24, "%s v%08x", v1[t], v);
        return;
    }
    pk_cpy(type, "quic-1rtt"); if (!(b0 & 0x40)) *fl |= PKF_GREASED; if (b0 & 0x20) *fl |= PKF_SPIN;
}
static void pk_fill_addr(PkEv* e, const struct sockaddr* sa) { e->fam = 0; e->rport = 0; memset(e->addr, 0, 16); if (!sa) return;
    if (sa->sa_family == AF_INET) { const struct sockaddr_in* si = (const void*)sa; memcpy(e->addr, &si->sin_addr, 4); e->rport = ntohs(si->sin_port); e->fam = 4; }
    else if (sa->sa_family == AF_INET6) { const struct sockaddr_in6* si = (const void*)sa;
        if (IN6_IS_ADDR_V4MAPPED(&si->sin6_addr)) { memcpy(e->addr, (const uint8_t*)&si->sin6_addr + 12, 4); e->fam = 4; }
        else { memcpy(e->addr, &si->sin6_addr, 16); e->fam = 6; }
        e->rport = ntohs(si->sin6_port); } }
static uint16_t g_pk_lport[PK_NFD];
static uint16_t pk_lport(int fd) { if (fd < 0 || fd >= PK_NFD) return 0; uint16_t p = g_pk_lport[fd];
    if (!p) { struct sockaddr_storage me; socklen_t ml = sizeof me; if (!getsockname(fd, (struct sockaddr*)&me, &ml)) p = ntohs(me.ss_family == AF_INET6 ? ((struct sockaddr_in6*)&me)->sin6_port : ((struct sockaddr_in*)&me)->sin_port);
        g_pk_lport[fd] = p ? p : 1; } return p == 1 ? 0 : p; }
static uint8_t g_pk_udp[PK_NFD];                            // 1 UDP, 2 TCP, 0 unknown / not a socket we log
static int pk_kind(int fd) {
    if (fd < 0 || fd >= PK_NFD) return 0; if (g_pk_udp[fd]) return g_pk_udp[fd];
    int t = 0; socklen_t tl = sizeof t; if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &t, &tl)) { g_pk_udp[fd] = 3; return 3; }
    struct sockaddr_storage me; socklen_t ml = sizeof me;   // (Unix sockets / pipes are not network traffic)
    if (getsockname(fd, (struct sockaddr*)&me, &ml) || (me.ss_family != AF_INET && me.ss_family != AF_INET6)) { g_pk_udp[fd] = 3; return 3; }
    g_pk_udp[fd] = t == SOCK_DGRAM ? 1 : t == SOCK_STREAM ? 2 : 3; return g_pk_udp[fd];
}
// NHOOK_UDP_HEX=<n> (debug: protocol RE, never on a normal run): the first n bytes of the first 3 datagrams each way per peer
// address (256 peers) go to the log in hex
static void pk_hex(int dir, const struct sockaddr* sa, const uint8_t* b, long n) {
    static int want = -1; if (want < 0) { const char* v = getenv("NHOOK_UDP_HEX"); want = v ? atoi(v) : 0; } if (want <= 0 || !sa) return;
    char a[64] = "?"; int port = 0;
    if (sa->sa_family == AF_INET) { inet_ntop(AF_INET, &((const struct sockaddr_in*)sa)->sin_addr, a, sizeof a); port = ntohs(((const struct sockaddr_in*)sa)->sin_port); }
    else if (sa->sa_family == AF_INET6) { inet_ntop(AF_INET6, &((const struct sockaddr_in6*)sa)->sin6_addr, a, sizeof a); port = ntohs(((const struct sockaddr_in6*)sa)->sin6_port); }
    static struct { char a[64]; int port; uint8_t c[2]; } seen[256]; static int ns; static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mx); int k = -1; for (int i = 0; i < ns; i++) if (seen[i].port == port && !strcmp(seen[i].a, a)) { k = i; break; }
    if (k < 0 && ns < 256) { k = ns++; snprintf(seen[k].a, sizeof seen[k].a, "%s", a); seen[k].port = port; seen[k].c[0] = seen[k].c[1] = 0; }
    int go = k >= 0 && seen[k].c[dir == PK_RX] < 3; if (go) seen[k].c[dir == PK_RX]++; pthread_mutex_unlock(&mx); if (!go) return;
    char h[2 * 512 + 1]; int m = (int)(n < want ? n : want); if (m > 512) m = 512; for (int i = 0; i < m; i++) snprintf(h + 2 * i, 3, "%02x", b[i]); h[2 * m] = 0;
    nlog("[nhook] udp %s %s:%d %ld bytes: %s", dir == PK_RX ? "rx" : "tx", a, port, n, h);
}
static void pk_wire_body(int dir, int fd, const struct sockaddr* sa, const void* buf, long n);
static void pk_wire(int dir, int fd, const struct sockaddr* sa, const void* buf, long n) {   // (timed: nhcost.h)
    { HK_ONCE(h, "sockets (send / receive)"); hk_fire(h); }
    if (g_hc_off[HC_SOCK]) return; uint64_t t0 = hc_now(); pk_wire_body(dir, fd, sa, buf, n); hc_add(HC_SOCK, t0); sm_add(SM_SOCK, t0); }
static void pk_wire_body(int dir, int fd, const struct sockaddr* sa, const void* buf, long n) {
    if (n > 0 && pk_kind(fd) == 1) pk_hex(dir, sa, (const uint8_t*)buf, n);
    if (!g_pk_on || n < 0 || pk_kind(fd) != 1) return;
    uint32_t s; PkEv* e = pk_new(&s);
    e->layer = PK_WIRE; e->dir = (uint8_t)dir; e->len = (uint32_t)n; e->t = st_wall(); e->lport = pk_lport(fd); pk_fill_addr(e, sa);
    if (!e->fam) { struct sockaddr_storage pe; socklen_t pl2 = sizeof pe; if (!getpeername(fd, (struct sockaddr*)&pe, &pl2)) pk_fill_addr(e, (struct sockaddr*)&pe); }
    pk_classify((const uint8_t*)buf, n, e->type, &e->flags); pk_done(e, s);
}
static struct { uint64_t tx, rx; struct sockaddr_storage ss; uint8_t on; } g_pk_tcp[PK_NFD];
static void pk_tcp_rec(int fd, const char* type, uint32_t len) { uint32_t s; PkEv* e = pk_new(&s);
    e->layer = PK_WIRE; e->dir = PK_TX; e->len = len; e->t = st_wall(); e->lport = pk_lport(fd); e->flags = 0; pk_fill_addr(e, (struct sockaddr*)&g_pk_tcp[fd].ss); pk_cpy(e->type, type); pk_done(e, s); }
static void pk_tcp_connect(int fd, const struct sockaddr* sa, socklen_t sl) {
    if (!g_pk_on || fd < 0 || fd >= PK_NFD || !sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) return;
    g_pk_udp[fd] = 0; g_pk_lport[fd] = 0; if (pk_kind(fd) != 2) return;
    g_pk_tcp[fd].tx = g_pk_tcp[fd].rx = 0; memset(&g_pk_tcp[fd].ss, 0, sizeof g_pk_tcp[fd].ss); memcpy(&g_pk_tcp[fd].ss, sa, sl < sizeof g_pk_tcp[fd].ss ? sl : sizeof g_pk_tcp[fd].ss);
    g_pk_tcp[fd].on = 1; pk_tcp_rec(fd, "tcp-connect", 0); }
static void pk_tcp_bytes(int fd, int dir, long n) { if (fd < 0 || fd >= PK_NFD || n <= 0 || !g_pk_tcp[fd].on) return; if (dir == PK_TX) g_pk_tcp[fd].tx += (uint64_t)n; else g_pk_tcp[fd].rx += (uint64_t)n; }
static void pk_close(int fd) { if (fd < 0 || fd >= PK_NFD) return;
    if (g_pk_tcp[fd].on && g_pk_on) pk_tcp_rec(fd, "tcp-close", (uint32_t)(g_pk_tcp[fd].tx + g_pk_tcp[fd].rx));
    g_pk_tcp[fd].on = 0; g_pk_lport[fd] = 0; g_pk_udp[fd] = 0; }
static void pk_ev_json(StBuf* s, const PkEv* e, uint32_t seq, int first) {
    sb_put(s, "%s{\"seq\":%u,\"t\":%.4f,\"layer\":\"%s\",\"dir\":\"%s\",\"type\":", first ? "" : ",", seq, e->t, e->layer == PK_GAME ? "game" : "wire", e->dir == PK_RX ? "rx" : "tx"); sb_str(s, e->type);
    sb_put(s, ",\"len\":%u,\"lport\":%u,\"flags\":%u", e->len, e->lport, e->flags); pk_peer_json(s, e); sb_put(s, "}"); }
static int pk_get(uint32_t seq, PkEv* out) { const PkEv* e = &g_pk[seq % PK_N]; if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) return 0;
    memcpy(out, (const void*)e, sizeof *out); return __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) == seq; }
typedef struct { uint8_t dir, fam, layer; uint8_t addr[16]; uint16_t rport; char type[24]; uint64_t n, bytes; double t0, t1; } PkAgg;
static int pk_agg_add(PkAgg* a, int na, int cap, const PkEv* e) {
    for (int i = 0; i < na; i++) { PkAgg* x = &a[i]; if (x->dir == e->dir && x->layer == e->layer && x->fam == e->fam && x->rport == e->rport && !memcmp(x->addr, e->addr, 16) && !strcmp(x->type, e->type)) {
            x->n++; x->bytes += e->len; if (e->t > x->t1) x->t1 = e->t; return na; } }
    if (na >= cap) return na; PkAgg* x = &a[na]; x->dir = e->dir; x->layer = e->layer; x->fam = e->fam; x->rport = e->rport; memcpy(x->addr, e->addr, 16); snprintf(x->type, 24, "%s", e->type);
    x->n = 1; x->bytes = e->len; x->t0 = x->t1 = e->t; return na + 1; }
static void pk_agg_json(StBuf* s, const PkAgg* x, int first, double secs) { PkEv e; memset(&e, 0, sizeof e); e.fam = x->fam; memcpy(e.addr, x->addr, 16); e.rport = x->rport;
    sb_put(s, "%s{\"layer\":\"%s\",\"dir\":\"%s\",\"type\":", first ? "" : ",", x->layer == PK_GAME ? "game" : "wire", x->dir == PK_RX ? "rx" : "tx"); sb_str(s, x->type);
    sb_put(s, ",\"n\":%llu,\"bytes\":%llu", (unsigned long long)x->n, (unsigned long long)x->bytes);
    if (secs > 0) sb_put(s, ",\"per_s\":%.2f,\"bytes_per_s\":%.0f", x->n / secs, x->bytes / secs); else sb_put(s, ",\"t0\":%.3f,\"t1\":%.3f", x->t0, x->t1);
    pk_peer_json(s, &e); sb_put(s, "}"); }
// ---- the game layer (opt-in, the page's "game" switch: POST do=pktgame&on=1; 2026-11-07, backlog N9): halo3's network message
// types by name as the app's halo3 encodes / decodes them. halo3's message table (halo3+0x46b6e98: 46 x 0x40 {valid, name,
// flags, size, max size, encode +0x20, decode +0x28}) is in the native app, and every encode / decode is a lifted halo3
// function (player-acknowledge's are the app's own code; synchronous-client-ready has none): the first switch-on hooks each
// distinct one (code-table entry hooks, permanent; the handlers do nothing while the switch is off). Only SESSION messages
// still run through them (2 minutes of a 59-player game: membership-update decoded ~150 times, the gateway's receive twice;
// channel send / receive, the synchronous-* messages and game-results never ran): the app carries the in-game traffic in
// its own protocol, so these records name session traffic only, with no peer (the channel functions that knew it don't run).
// A function several types share (0x74e30: leave-session, leave-acknowledge, session-disband ...) is named by all of them.
enum { PKG_NF = 96 };
typedef struct { uint32_t rva; uint8_t dir; char name[24], full[160]; volatile unsigned long n; } PkgFn;
static PkgFn g_pkgf[PKG_NF]; static int g_npkgf; static volatile int g_pkg_want, g_pkg_hooked; static pthread_mutex_t g_pkg_mx = PTHREAD_MUTEX_INITIALIZER;
static void pkg_hit(int k) {
    if (!g_pkg_want || k >= g_npkgf) return; PkgFn* f = &g_pkgf[k]; f->n++;
    uint32_t s; PkEv* e = pk_new(&s); e->layer = PK_GAME; e->dir = f->dir; e->fam = 0; e->len = 0; e->lport = e->rport = 0; e->flags = 0; e->t = st_wall();
    memset(e->addr, 0, 16); snprintf(e->type, sizeof e->type, "%s", f->name); pk_done(e, s); }
#define PKGH(k) static void pkgh##k(uint64_t* c) { (void)c; pkg_hit(k); }
#define PKGH8(b) PKGH(b##0) PKGH(b##1) PKGH(b##2) PKGH(b##3) PKGH(b##4) PKGH(b##5) PKGH(b##6) PKGH(b##7)
PKGH(0) PKGH(1) PKGH(2) PKGH(3) PKGH(4) PKGH(5) PKGH(6) PKGH(7) PKGH(8) PKGH(9) PKGH8(1) PKGH8(2) PKGH8(3) PKGH8(4) PKGH8(5) PKGH8(6) PKGH8(7) PKGH8(8) PKGH8(9)
PKGH(18) PKGH(19) PKGH(28) PKGH(29) PKGH(38) PKGH(39) PKGH(48) PKGH(49) PKGH(58) PKGH(59) PKGH(68) PKGH(69) PKGH(78) PKGH(79) PKGH(88) PKGH(89) PKGH(98) PKGH(99)
static nh_handler const g_pkgh[] = {pkgh0, pkgh1, pkgh2, pkgh3, pkgh4, pkgh5, pkgh6, pkgh7, pkgh8, pkgh9,
    pkgh10, pkgh11, pkgh12, pkgh13, pkgh14, pkgh15, pkgh16, pkgh17, pkgh18, pkgh19, pkgh20, pkgh21, pkgh22, pkgh23, pkgh24, pkgh25, pkgh26, pkgh27, pkgh28, pkgh29,
    pkgh30, pkgh31, pkgh32, pkgh33, pkgh34, pkgh35, pkgh36, pkgh37, pkgh38, pkgh39, pkgh40, pkgh41, pkgh42, pkgh43, pkgh44, pkgh45, pkgh46, pkgh47, pkgh48, pkgh49,
    pkgh50, pkgh51, pkgh52, pkgh53, pkgh54, pkgh55, pkgh56, pkgh57, pkgh58, pkgh59, pkgh60, pkgh61, pkgh62, pkgh63, pkgh64, pkgh65, pkgh66, pkgh67, pkgh68, pkgh69,
    pkgh70, pkgh71, pkgh72, pkgh73, pkgh74, pkgh75, pkgh76, pkgh77, pkgh78, pkgh79, pkgh80, pkgh81, pkgh82, pkgh83, pkgh84, pkgh85, pkgh86, pkgh87, pkgh88, pkgh89,
    pkgh90, pkgh91, pkgh92, pkgh93, pkgh94, pkgh95};
static int pkg_install(void) {   // (g_pkg_mx held) read the table and hook every distinct encode / decode: 1 = done
    uint64_t tb = 0; uint8_t e[0x40]; char nm[32]; uint64_t np = 0;
    if (!g_img || rd(g_img + 0x46b6e98, &tb, 8) || !tb || rd(tb, e, 0x40) || e[0] != 1) return 0;
    memcpy(&np, e + 8, 8); memset(nm, 0, sizeof nm); if (!np || rd(np, nm, 5) || memcmp(nm, "ping", 5)) return 0;
    g_npkgf = 0;
    for (int i = 0; i < 46; i++) { if (rd(tb + (uint64_t)i * 0x40, e, 0x40) || e[0] != 1) continue; memcpy(&np, e + 8, 8); memset(nm, 0, sizeof nm); if (np) rd(np, nm, 31);
        for (int d = 0; d < 2; d++) { uint64_t fp = 0; memcpy(&fp, e + 0x20 + 8 * d, 8); if (fp <= g_img || fp - g_img > 0x8000000) continue; uint32_t rva = (uint32_t)(fp - g_img);
            int k = -1; for (int j = 0; j < g_npkgf; j++) if (g_pkgf[j].rva == rva && g_pkgf[j].dir == (d ? PK_RX : PK_TX)) { k = j; break; }
            if (k < 0) { if (g_npkgf >= PKG_NF) continue; k = g_npkgf++; g_pkgf[k].rva = rva; g_pkgf[k].dir = (uint8_t)(d ? PK_RX : PK_TX); g_pkgf[k].full[0] = 0; }
            size_t fl = strlen(g_pkgf[k].full); snprintf(g_pkgf[k].full + fl, sizeof g_pkgf[k].full - fl, "%s%s", fl ? " / " : "", nm); } }
    int ok = 0, nf = g_npkgf;
    for (int k = 0; k < nf; k++) { PkgFn* f = &g_pkgf[k]; const char* sl = strchr(f->full, '/');
        snprintf(f->name, sizeof f->name, "%.*s%s", sl ? (int)(sl - f->full - 1) : 23, f->full, sl ? "+" : "");   // ("+": types share it)
        char hn[48]; snprintf(hn, sizeof hn, "message %s %s", f->name, f->dir == PK_RX ? "decode" : "encode");
        if (!hook_install(hn, f->rva, g_pkgh[k])) ok++; }
    nlog("[nhook] packets: game layer: %d of %d message encode / decode functions hooked (halo3's message table at 0x%llx)", ok, nf, (unsigned long long)tb);
    return 1; }
static int pk_control(const char* qs) {   // POST /control?do=pktgame&on=0|1
    const char* v = strstr(qs, "on="); if (!v) return 0; int on = atoi(v + 3) ? 1 : 0;
    pthread_mutex_lock(&g_pkg_mx); if (on && !g_pkg_hooked) g_pkg_hooked = pkg_install(); int ok = !on || g_pkg_hooked; if (ok) g_pkg_want = on; pthread_mutex_unlock(&g_pkg_mx);
    nlog("[nhook] packets: game layer %s", on ? (ok ? "on" : "not available (halo3's message table not found)") : "off"); return ok; }
// GET /packets.json?since=<seq>&max=<n>[&skip1rtt=1][&wire=0][&game=0] (pktlog.c's)
static void pk_json(StBuf* s, const char* qs) {
    const char* v = strstr(qs, "since="); uint32_t since = v ? (uint32_t)strtoul(v + 6, 0, 10) : 0; v = strstr(qs, "max="); int mx = v ? atoi(v + 4) : 400; if (mx < 1) mx = 1; if (mx > 4000) mx = 4000;
    int s1 = strstr(qs, "skip1rtt=1") != 0, nw = strstr(qs, "wire=0") != 0, ng = strstr(qs, "game=0") != 0; g_pk_want_rdns = strstr(qs, "rdns=1") != 0;
    uint32_t top = __atomic_load_n(&g_pk_seq, __ATOMIC_ACQUIRE); uint32_t lo = top > PK_N - 64 ? top - (PK_N - 64) : 1; if (since + 1 > lo) lo = since + 1;
    sb_put(s, "{\"seq\":%u,\"on\":%s,\"game\":{\"want\":%s,\"on\":%s,\"types\":%d,\"native\":\"session messages only (the app carries in-game traffic in its own protocol); no peer\"},\"events\":[",
           top, g_pk_on ? "true" : "false", g_pkg_want ? "true" : "false", g_pkg_want && g_pkg_hooked ? "true" : "false", g_npkgf);
    static uint32_t pick[4000]; int np = 0; PkEv e;
    for (uint32_t q = top; q >= lo && q && np < mx; q--) { if (!pk_get(q, &e)) continue; if (e.layer == PK_GAME ? ng : ((s1 && !strcmp(e.type, "quic-1rtt")) || nw)) continue; pick[np++] = q; }
    int f = 1; for (int i = np - 1; i >= 0; i--) if (pk_get(pick[i], &e)) { pk_ev_json(s, &e, pick[i], f); f = 0; }
    double now = st_wall(), oldest = now; static PkAgg a[512]; int na = 0; uint32_t lo2 = top > PK_N - 64 ? top - (PK_N - 64) : 1;
    for (uint32_t q = top; q >= lo2 && q; q--) { if (!pk_get(q, &e)) continue; if (now - e.t > 10.0) break; if (e.t < oldest) oldest = e.t; na = pk_agg_add(a, na, 512, &e); }
    double win = now - oldest; if (win < 1.0) win = 1.0; if (win > 10.0) win = 10.0;
    sb_put(s, "],\"summary_s\":%.1f,\"summary\":[", win); for (int i = 0; i < na; i++) pk_agg_json(s, &a[i], !i, win);
    sb_put(s, "],\"game_counts\":["); for (int i = 0, ff = 1; i < g_npkgf; i++) { if (!g_pkgf[i].n) continue;   // (per hooked function: its types, sends or receives)
        sb_put(s, "%s{\"type\":", ff ? "" : ","); ff = 0; sb_str(s, g_pkgf[i].full); sb_put(s, ",\"tx\":%lu,\"rx\":%lu,\"flags\":0}", g_pkgf[i].dir == PK_TX ? g_pkgf[i].n : 0, g_pkgf[i].dir == PK_RX ? g_pkgf[i].n : 0); }
    sb_put(s, "]}");
}
