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
static int g_st_hostpeer = -1, g_st_localpeer = -1;
static uint64_t st_sess_ptr(void) {
    uint64_t sess = 0; if (!g_img || !hb_read(g_img + 0x1e250d0, &sess, 8) || sess < 0x10000) return 0;
    return sess;
}
static int st_session(StSess* out, int max, uint32_t* state) {
    uint64_t sess = st_sess_ptr(); if (!sess) return 0;
    uint32_t st = 0; hb_read(sess + 0x67b70, &st, 4); if (state) *state = st;
    { int32_t upd = -1, hp = -1, lp = -1;
      if (hb_read(sess + 0x38, &upd, 4) && upd != -1 && st >= 4 && st <= 9 && hb_read(sess + 0x40, &hp, 4) && hb_read(sess + 0x3dc4, &lp, 4)
          && hp >= -1 && hp < 128 && lp >= -1 && lp < 128) { g_st_hostpeer = hp; g_st_localpeer = lp; }
      else { g_st_hostpeer = -1; g_st_localpeer = -1; } }
    uint8_t pm[16] = {0}, qm[16] = {0};
    if (!hb_read(sess + 0x126c, pm, 16) || !hb_read(sess + 0x54, qm, 16)) return 0;
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
    return n;
}
static int st_max_players(void) {                          // the session's limit (host or joined), 0 = not known
    uint64_t sess = st_sess_ptr(); uint32_t st = 0, fl = 0; int32_t mx = 0;
    if (sess && hb_read(sess + 0x67b70, &st, 4) && st >= 4 && st <= 9 && hb_read(sess + 0x4194, &fl, 4) && (fl & 1) && hb_read(sess + 0x419c, &mx, 4) && mx >= 1 && mx <= 128) return mx;
    return 0;
}

// ---- the packet log (pktlog.c's wire layer) ---------------------------------------------------------------------------
#include <netdb.h>
enum { PK_TX = 0, PK_RX = 1, PK_WIRE = 0, PK_N = 16384, PK_NFD = 4096 };
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
static void pk_peer_json(StBuf* s, const PkEv* e) {
    char peer[80] = "", host[48], ip[64];
    if (e->fam) { inet_ntop(e->fam == 6 ? AF_INET6 : AF_INET, e->addr, ip, sizeof ip); snprintf(peer, sizeof peer, e->fam == 6 ? "[%s]:%u" : "%s:%u", ip, e->rport); }
    pk_host(e, host, sizeof host);
    if (peer[0]) { sb_put(s, ",\"peer\":"); sb_str(s, peer); } if (host[0]) { sb_put(s, ",\"host\":"); sb_str(s, host); } }
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
static void pk_wire(int dir, int fd, const struct sockaddr* sa, const void* buf, long n) {
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
    sb_put(s, "%s{\"seq\":%u,\"t\":%.4f,\"layer\":\"wire\",\"dir\":\"%s\",\"type\":", first ? "" : ",", seq, e->t, e->dir == PK_RX ? "rx" : "tx"); sb_str(s, e->type);
    sb_put(s, ",\"len\":%u,\"lport\":%u,\"flags\":%u", e->len, e->lport, e->flags); pk_peer_json(s, e); sb_put(s, "}"); }
static int pk_get(uint32_t seq, PkEv* out) { const PkEv* e = &g_pk[seq % PK_N]; if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != seq) return 0;
    memcpy(out, (const void*)e, sizeof *out); return __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) == seq; }
typedef struct { uint8_t dir, fam; uint8_t addr[16]; uint16_t rport; char type[24]; uint64_t n, bytes; double t0, t1; } PkAgg;
static int pk_agg_add(PkAgg* a, int na, int cap, const PkEv* e) {
    for (int i = 0; i < na; i++) { PkAgg* x = &a[i]; if (x->dir == e->dir && x->fam == e->fam && x->rport == e->rport && !memcmp(x->addr, e->addr, 16) && !strcmp(x->type, e->type)) {
            x->n++; x->bytes += e->len; if (e->t > x->t1) x->t1 = e->t; return na; } }
    if (na >= cap) return na; PkAgg* x = &a[na]; x->dir = e->dir; x->fam = e->fam; x->rport = e->rport; memcpy(x->addr, e->addr, 16); snprintf(x->type, 24, "%s", e->type);
    x->n = 1; x->bytes = e->len; x->t0 = x->t1 = e->t; return na + 1; }
static void pk_agg_json(StBuf* s, const PkAgg* x, int first, double secs) { PkEv e; memset(&e, 0, sizeof e); e.fam = x->fam; memcpy(e.addr, x->addr, 16); e.rport = x->rport;
    sb_put(s, "%s{\"layer\":\"wire\",\"dir\":\"%s\",\"type\":", first ? "" : ",", x->dir == PK_RX ? "rx" : "tx"); sb_str(s, x->type);
    sb_put(s, ",\"n\":%llu,\"bytes\":%llu", (unsigned long long)x->n, (unsigned long long)x->bytes);
    if (secs > 0) sb_put(s, ",\"per_s\":%.2f,\"bytes_per_s\":%.0f", x->n / secs, x->bytes / secs); else sb_put(s, ",\"t0\":%.3f,\"t1\":%.3f", x->t0, x->t1);
    pk_peer_json(s, &e); sb_put(s, "}"); }
// GET /packets.json?since=<seq>&max=<n>[&skip1rtt=1][&wire=0][&game=0] (pktlog.c's; the game layer is not in the native app)
static void pk_json(StBuf* s, const char* qs) {
    const char* v = strstr(qs, "since="); uint32_t since = v ? (uint32_t)strtoul(v + 6, 0, 10) : 0; v = strstr(qs, "max="); int mx = v ? atoi(v + 4) : 400; if (mx < 1) mx = 1; if (mx > 4000) mx = 4000;
    int s1 = strstr(qs, "skip1rtt=1") != 0, nw = strstr(qs, "wire=0") != 0;
    uint32_t top = __atomic_load_n(&g_pk_seq, __ATOMIC_ACQUIRE); uint32_t lo = top > PK_N - 64 ? top - (PK_N - 64) : 1; if (since + 1 > lo) lo = since + 1;
    sb_put(s, "{\"seq\":%u,\"on\":%s,\"game\":{\"want\":false,\"on\":false,\"types\":0,\"native\":\"not in the native app yet\"},\"events\":[", top, g_pk_on ? "true" : "false");
    static uint32_t pick[4000]; int np = 0; PkEv e;
    for (uint32_t q = top; q >= lo && q && np < mx; q--) { if (!pk_get(q, &e)) continue; if ((s1 && !strcmp(e.type, "quic-1rtt")) || nw) continue; pick[np++] = q; }
    int f = 1; for (int i = np - 1; i >= 0; i--) if (pk_get(pick[i], &e)) { pk_ev_json(s, &e, pick[i], f); f = 0; }
    double now = st_wall(), oldest = now; static PkAgg a[512]; int na = 0; uint32_t lo2 = top > PK_N - 64 ? top - (PK_N - 64) : 1;
    for (uint32_t q = top; q >= lo2 && q; q--) { if (!pk_get(q, &e)) continue; if (now - e.t > 10.0) break; if (e.t < oldest) oldest = e.t; na = pk_agg_add(a, na, 512, &e); }
    double win = now - oldest; if (win < 1.0) win = 1.0; if (win > 10.0) win = 10.0;
    sb_put(s, "],\"summary_s\":%.1f,\"summary\":[", win); for (int i = 0; i < na; i++) pk_agg_json(s, &a[i], !i, win);
    sb_put(s, "],\"game_counts\":[]}");
}
