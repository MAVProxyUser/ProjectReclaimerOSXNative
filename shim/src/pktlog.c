// pktlog.c — packet log: the TYPE of what this process sends and receives (never the content), for the stats page's
// Packets window (GET /packets.json) and the forensic archive (packets.jsonl). Included by peload.c after forensic.c.
// Guide: shim/docs/forensics.md "Packets".
//
// Two layers:
//   wire  every UDP datagram on the loader's sockets (sendto / recvfrom / WSASendMsg / WSARecvMsg) and every TCP
//         connection (connect, close, bytes): direction, local port, remote address (+ the host name it was looked up
//         by), size, and what the datagram's first bytes say without decrypting anything:
//           QUIC (RFC 9000 17.2 / 17.3; the game's own traffic, encrypted): a long header's packet type (Initial,
//             0-RTT, Handshake, Retry) and version, a version negotiation, or a short header ("1-RTT", the connected
//             state; its spin bit is the one flag not hidden by header protection);
//           a plaintext tag (four characters such as MCH1 / RR-R: hole punching, relays);
//           anything else: "udp".
//   game  (opt-in: PELOAD_PKTLOG_GAME=1 or the page's switch) halo3's own network message types by name (ping,
//         join-request, membership-update, player-add ... 46 of them), as halo3 encodes one to send it or decodes one it
//         received: its message table (halo3+0x46b6e98 -> 46 x 0x40-byte entries {valid, name, flags, size, max size,
//         encode, decode, ...}) has its encode / decode pointers swapped for logging thunks that jump on to the
//         originals. Data only, no code is patched; installed and removed at the engine's frame safe point. halo3 only
//         knows the peer deeper down, so game rows have a direction but no address.
// The hooks write one 64-byte record into a ring (16384) and return; readers (the page, the archive's writer thread)
// copy from it. PELOAD_PKTLOG=0 turns everything off (default: on whenever the stats page is).
#include <netdb.h>

enum { PK_WIRE=0, PK_GAME=1, PK_N=16384 };   // (PK_TX / PK_RX: peload.c, beside the forward declarations)
typedef struct { volatile uint32_t seq; uint8_t layer, dir, fam, flags; uint16_t lport, rport; uint32_t len; double t; uint8_t addr[16]; char type[24]; } PkEv;
_Static_assert(sizeof(PkEv)==64,"one cache line per record");
static PkEv g_pk[PK_N]; static volatile uint32_t g_pk_seq;   // records 1.. (seq 0 = never written)
static int g_pk_on_v=-1;
static int pk_on(void){ if(g_pk_on_v<0){ const char*e=getenv("PELOAD_PKTLOG"); g_pk_on_v=e&&*e?atoi(e)!=0:stats_on(); } return g_pk_on_v; }
static void pk_bench(void);
// a record: reserve its sequence number, mark the slot as being written (seq 0), fill it, publish (seq). A reader copies
// a slot only when its seq is the one it wants before AND after the copy (else a writer came round: skipped).
static PkEv* pk_new(uint32_t*sq){ uint32_t s=__atomic_add_fetch(&g_pk_seq,1,__ATOMIC_RELAXED); PkEv*e=&g_pk[s%PK_N]; __atomic_store_n(&e->seq,0,__ATOMIC_RELEASE); *sq=s; return e; }
static void pk_done(PkEv*e,uint32_t s){ __atomic_store_n(&e->seq,s,__ATOMIC_RELEASE); }

// ---- names: the host names addresses were looked up by (getaddrinfo / GetAddrInfoW) ----
static struct { uint8_t fam, a[16]; char name[48]; } g_pk_dns[256]; static int g_pk_ndns; static pthread_mutex_t g_pk_dnsmx=PTHREAD_MUTEX_INITIALIZER;
static void pk_dns(const char*name,const struct addrinfo*ai){
    if(!pk_on() || !name || !*name || !ai) return; pthread_mutex_lock(&g_pk_dnsmx);
    for(const struct addrinfo*p=ai;p;p=p->ai_next){ uint8_t a[16]={0}, fam=0;
        if(p->ai_family==AF_INET){ memcpy(a,&((struct sockaddr_in*)p->ai_addr)->sin_addr,4); fam=4; }
        else if(p->ai_family==AF_INET6){ memcpy(a,&((struct sockaddr_in6*)p->ai_addr)->sin6_addr,16); fam=6; } else continue;
        int k=0; for(;k<g_pk_ndns;k++) if(g_pk_dns[k].fam==fam && !memcmp(g_pk_dns[k].a,a,16)) break;
        if(k==g_pk_ndns){ if(g_pk_ndns<256) g_pk_ndns++; else k=(int)(st_wall()*7)%256; }
        g_pk_dns[k].fam=fam; memcpy(g_pk_dns[k].a,a,16); snprintf(g_pk_dns[k].name,sizeof g_pk_dns[k].name,"%s",name); }
    pthread_mutex_unlock(&g_pk_dnsmx); }
static void pk_host(const PkEv*e,char*o,size_t on){ o[0]=0; if(!e->fam) return; pthread_mutex_lock(&g_pk_dnsmx);
    for(int k=0;k<g_pk_ndns;k++) if(g_pk_dns[k].fam==e->fam && !memcmp(g_pk_dns[k].a,e->addr,16)){ snprintf(o,on,"%s",g_pk_dns[k].name); break; }
    pthread_mutex_unlock(&g_pk_dnsmx); }
static void pk_peer(const PkEv*e,char*o,size_t on){ o[0]=0; if(!e->fam) return; char ip[64];
    inet_ntop(e->fam==6?AF_INET6:AF_INET,e->addr,ip,sizeof ip); snprintf(o,on,e->fam==6?"[%s]:%u":"%s:%u",ip,e->rport); }

// ---- wire layer ----
// flags: SPIN = a short header's spin bit (the one bit header protection leaves visible); GREASED = the fixed bit is 0:
// both ends offered grease_quic_bit (RFC 9287; the client's QUIC library does), so it carries no meaning
enum { PKF_SPIN=1, PKF_GREASED=2 };
static void pk_cpy(char*type,const char*s){ size_t n=strlen(s); if(n>23) n=23; memcpy(type,s,n); type[n]=0; }   // (no snprintf per datagram)
static void pk_classify(const uint8_t*b,long n,char*type,uint8_t*fl){
    *fl=0; if(n<=0){ pk_cpy(type,"empty"); return; }
    #define PK_TC(c) (((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='-')
    if(n>=4 && b[0]>='A'&&b[0]<='Z' && b[1]>='A'&&b[1]<='Z' && PK_TC(b[2]) && PK_TC(b[3])){ memcpy(type,"tag ",4); memcpy(type+4,b,4); type[8]=0; return; }
    #undef PK_TC
    uint8_t b0=b[0];
    if(b0&0x80){   // long header: form 1, fixed 1, type (2 bits), 4 protected bits, then the version
        if(n<7){ pk_cpy(type,"quic-long?"); return; }
        uint32_t v=((uint32_t)b[1]<<24)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<8)|b[4]; int t=(b0>>4)&3;
        if(!v){ pk_cpy(type,"quic-version-neg"); return; }   // (its other bits are arbitrary)
        if(!(b0&0x40)) *fl|=PKF_GREASED;
        static const char*const v1[4]={"quic-initial","quic-0-rtt","quic-handshake","quic-retry"}, *const v2[4]={"quic2-retry","quic2-initial","quic2-0-rtt","quic2-handshake"};
        if(v==1) pk_cpy(type,v1[t]); else if(v==0x6b3343cf) pk_cpy(type,v2[t]); else snprintf(type,24,"%s v%08x",v1[t],v);   // (other versions: rare)
        return; }
    // short header: form 0, then the fixed bit (greased: either), the spin bit, then header-protected bits. Every UDP flow of
    // this client is QUIC; the only plaintext ones carry the tags above
    pk_cpy(type,"quic-1rtt"); if(!(b0&0x40)) *fl|=PKF_GREASED; if(b0&0x20) *fl|=PKF_SPIN; }
static void pk_fill_addr(PkEv*e,const struct sockaddr_storage*ss){ e->fam=0; e->rport=0; memset(e->addr,0,16); if(!ss) return;
    if(ss->ss_family==AF_INET){ const struct sockaddr_in*si=(const void*)ss; memcpy(e->addr,&si->sin_addr,4); e->rport=ntohs(si->sin_port); e->fam=4; }
    else if(ss->ss_family==AF_INET6){ const struct sockaddr_in6*si=(const void*)ss; memcpy(e->addr,&si->sin6_addr,16); e->rport=ntohs(si->sin6_port); e->fam=6; } }
enum { PK_NFD=4096 };
static uint16_t g_pk_lport[PK_NFD];   // the fd's local port (asked once; 0 = not asked)
static uint16_t pk_lport(int fd){ if(fd<0 || fd>=PK_NFD) return 0; uint16_t p=g_pk_lport[fd];
    if(!p){ struct sockaddr_storage me; socklen_t ml=sizeof me; if(!getsockname(fd,(struct sockaddr*)&me,&ml)) p=ntohs(me.ss_family==AF_INET6?((struct sockaddr_in6*)&me)->sin6_port:((struct sockaddr_in*)&me)->sin_port);
        g_pk_lport[fd]=p?p:1; } return p==1?0:p; }
static void pk_wire(int dir,int fd,const void*ssv,const void*buf,long n){
    if(!pk_on() || n<0) return; const struct sockaddr_storage*ss=ssv;
    uint32_t s; PkEv*e=pk_new(&s);
    e->layer=PK_WIRE; e->dir=(uint8_t)dir; e->len=(uint32_t)n; e->t=st_wall(); e->lport=pk_lport(fd); pk_fill_addr(e,ss);
    pk_classify((const uint8_t*)buf,n,e->type,&e->flags); pk_done(e,s); }
// TCP: connect / close records, bytes counted per socket in between (each send / recv is not a record)
static struct { uint64_t tx, rx; struct sockaddr_storage ss; uint8_t on; } g_pk_tcp[PK_NFD];
static void pk_tcp_rec(int fd,const char*type,uint32_t len){ uint32_t s; PkEv*e=pk_new(&s);
    e->layer=PK_WIRE; e->dir=PK_TX; e->len=len; e->t=st_wall(); e->lport=pk_lport(fd); e->flags=0; pk_fill_addr(e,&g_pk_tcp[fd].ss); pk_cpy(e->type,type); pk_done(e,s); }
static void pk_tcp_connect(int fd,const void*ss){ if(!pk_on() || fd<0 || fd>=PK_NFD) return; g_pk_tcp[fd].tx=g_pk_tcp[fd].rx=0; memcpy(&g_pk_tcp[fd].ss,ss,sizeof(struct sockaddr_storage));
    g_pk_tcp[fd].on=1; g_pk_lport[fd]=0; pk_tcp_rec(fd,"tcp-connect",0); }
static void pk_tcp_bytes(int fd,int dir,long n){ if(fd<0 || fd>=PK_NFD || n<=0 || !g_pk_tcp[fd].on) return; if(dir==PK_TX) g_pk_tcp[fd].tx+=(uint64_t)n; else g_pk_tcp[fd].rx+=(uint64_t)n; }
static void pk_sock_new(int fd){ if(fd>=0 && fd<PK_NFD){ g_pk_lport[fd]=0; g_pk_tcp[fd].on=0; } }   // a new socket on a reused fd
static void pk_close(int fd){ if(fd<0 || fd>=PK_NFD) return;
    if(g_pk_tcp[fd].on && pk_on()) pk_tcp_rec(fd,"tcp-close",(uint32_t)(g_pk_tcp[fd].tx+g_pk_tcp[fd].rx));
    g_pk_tcp[fd].on=0; g_pk_lport[fd]=0; }

// ---- game layer: halo3's message table ----
enum { PKG_MAX=64, PKG_RVA_TABLE=0x46b6e98 };
static uint8_t* g_pkg_tbl; static int g_pkg_n, g_pkg_installed; static volatile int g_pkg_want=-1;
static void* g_pkg_orig[PKG_MAX][2]; static uint8_t* g_pkg_thunk[PKG_MAX][2]; static uint64_t* g_pkg_tgt[PKG_MAX][2];   // tgt: the thunk's jump target qword
static char g_pkg_name[PKG_MAX][32]; static uint32_t g_pkg_flags[PKG_MAX];
static uint64_t g_pkg_count[PKG_MAX][2];
static uint8_t* g_pkg_pool; static size_t g_pkg_off;
static void __attribute__((ms_abi)) pk_game_ev(uint32_t id,int64_t size){
    int ty=(int)(id>>1), dir=(int)(id&1); if(ty>=PKG_MAX) return; __atomic_fetch_add(&g_pkg_count[ty][dir],1,__ATOMIC_RELAXED);
    uint32_t s; PkEv*e=pk_new(&s);
    e->layer=PK_GAME; e->dir=(uint8_t)dir; e->len=(uint32_t)(size>0?size:0); e->t=st_wall(); e->lport=0; e->fam=0; e->rport=0; memset(e->addr,0,16);
    e->flags=(uint8_t)g_pkg_flags[ty]; pk_cpy(e->type,g_pkg_name[ty]); pk_done(e,s); }
// thunk: save rcx/rdx/r8/r9/rax/r10/r11 + xmm0-5 (as stats.c's entry hooks), pk_game_ev(id, original rdx = the message
// storage size), restore, jmp [rip] original -- the original runs with the caller's own stack and registers. The jump's
// target is the qword right after it: a re-wrap (the table was rebuilt) rewrites that qword and reuses the thunk.
static uint8_t* pkg_thunk(uint32_t id,void*orig,uint64_t**tgt){
    if(!g_pkg_pool){ g_pkg_pool=mmap(0,1<<16,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(g_pkg_pool==MAP_FAILED){ g_pkg_pool=0; return 0; } }
    if(g_pkg_off+192>(1u<<16)) return 0; uint8_t*t=g_pkg_pool+g_pkg_off, *w=t;
    static const uint8_t save[]={0x51,0x52,0x41,0x50,0x41,0x51,0x50,0x41,0x52,0x41,0x53, 0x48,0x81,0xEC,0x80,0,0,0,
        0xF3,0x0F,0x7F,0x44,0x24,0x20, 0xF3,0x0F,0x7F,0x4C,0x24,0x30, 0xF3,0x0F,0x7F,0x54,0x24,0x40, 0xF3,0x0F,0x7F,0x5C,0x24,0x50, 0xF3,0x0F,0x7F,0x64,0x24,0x60, 0xF3,0x0F,0x7F,0x6C,0x24,0x70};
    static const uint8_t rest[]={0xF3,0x0F,0x6F,0x44,0x24,0x20, 0xF3,0x0F,0x6F,0x4C,0x24,0x30, 0xF3,0x0F,0x6F,0x54,0x24,0x40, 0xF3,0x0F,0x6F,0x5C,0x24,0x50, 0xF3,0x0F,0x6F,0x64,0x24,0x60, 0xF3,0x0F,0x6F,0x6C,0x24,0x70,
        0x48,0x81,0xC4,0x80,0,0,0, 0x41,0x5B,0x41,0x5A,0x58,0x41,0x59,0x41,0x58,0x5A,0x59};
    memcpy(w,save,sizeof save); w+=sizeof save;
    *w++=0xB9; memcpy(w,&id,4); w+=4;                                                                   // mov ecx, id
    static const uint8_t ldrdx[8]={0x48,0x8B,0x94,0x24,0xA8,0,0,0}; memcpy(w,ldrdx,8); w+=8;            // mov rdx, [rsp+0xa8] (saved rdx)
    *w++=0x48; *w++=0xB8; uint64_t fn=(uint64_t)(uintptr_t)pk_game_ev; memcpy(w,&fn,8); w+=8; *w++=0xFF; *w++=0xD0;   // mov rax, pk_game_ev; call rax
    memcpy(w,rest,sizeof rest); w+=sizeof rest;
    *w++=0xFF; *w++=0x25; *w++=0; *w++=0; *w++=0; *w++=0; uint64_t o=(uint64_t)(uintptr_t)orig; *tgt=(uint64_t*)w; memcpy(w,&o,8); w+=8;   // jmp [rip+0] orig
    sys_icache_invalidate(t,(size_t)(w-t)); g_pkg_off+=((size_t)(w-t)+15)&~(size_t)15; return t; }
static int pkg_table(uint8_t**out){   // halo3's message table, checked by its first entries' names
    uint8_t*h3=hb_halo3_base(); uint64_t tb=0; if(!h3 || !hb_read((uint64_t)(uintptr_t)(h3+PKG_RVA_TABLE),&tb,8) || !tb) return 0;
    uint8_t e0[0x40], e1[0x40]; uint64_t np=0; char nm[8]={0};
    if(!hb_read(tb,e0,0x40) || !hb_read(tb+0x40,e1,0x40) || e0[0]!=1 || e1[0]!=1) return 0;
    memcpy(&np,e0+8,8); if(!np || !hb_read(np,nm,5) || memcmp(nm,"ping",5)) return 0;
    memcpy(&np,e1+8,8); if(!np || !hb_read(np,nm,5) || memcmp(nm,"pong",5)) return 0;
    *out=(uint8_t*)(uintptr_t)tb; return 1; }
static int pkg_is_thunk(void*p){ return g_pkg_pool && (uint8_t*)p>=g_pkg_pool && (uint8_t*)p<g_pkg_pool+(1u<<16); }
static void pkg_install(void){
    uint8_t*tb; if(!pkg_table(&tb)){ ws("  [pktlog] game layer: halo3's message table not found (another halo3 build?): off\n"); g_pkg_want=0; return; }
    g_pkg_tbl=tb; int n=0, w=0;
    for(int i=0;i<46 && i<PKG_MAX;i++){ uint8_t*e=tb+(size_t)i*0x40; if(e[0]!=1) continue; n=i+1;
        const char*nm=*(const char**)(e+8); char b[32]=""; if(nm) hb_read((uint64_t)(uintptr_t)nm,b,31); b[31]=0; snprintf(g_pkg_name[i],32,"%s",b[0]?b:"?");
        g_pkg_flags[i]=*(uint32_t*)(e+0x10);
        for(int d=0;d<2;d++){ void**slot=(void**)(e+0x20+8*d); void*cur=*slot; if(!cur || pkg_is_thunk(cur)) continue;
            if(!g_pkg_thunk[i][d]) g_pkg_thunk[i][d]=pkg_thunk((uint32_t)(i*2+(d?PK_RX:PK_TX)),cur,&g_pkg_tgt[i][d]);   // encode = send, decode = receive
            else if(g_pkg_orig[i][d]!=cur) __atomic_store_n(g_pkg_tgt[i][d],(uint64_t)(uintptr_t)cur,__ATOMIC_RELEASE);
            g_pkg_orig[i][d]=cur;
            if(g_pkg_thunk[i][d]){ __atomic_store_n(slot,(void*)g_pkg_thunk[i][d],__ATOMIC_RELEASE); w++; } } }
    g_pkg_n=n; g_pkg_installed=1;
    char m[160]; int k=snprintf(m,sizeof m,"  [pktlog] game layer on: %d of halo3's message encode / decode functions wrapped (%d types)\n",w,n); (void)!write(2,m,(size_t)k); }
static void pkg_uninstall(void){ if(!g_pkg_installed) return; uint8_t*tb;
    if(pkg_table(&tb) && tb==g_pkg_tbl) for(int i=0;i<g_pkg_n;i++) for(int d=0;d<2;d++){ void**slot=(void**)(tb+(size_t)i*0x40+0x20+8*d);
        if(g_pkg_thunk[i][d] && *slot==(void*)g_pkg_thunk[i][d]) __atomic_store_n(slot,g_pkg_orig[i][d],__ATOMIC_RELEASE); }
    g_pkg_installed=0; ws("  [pktlog] game layer off: halo3's message table restored\n"); }
// the engine's frame safe point (peload.c hb_frame_hook): switch the game layer as asked; once a second, re-wrap entries
// that changed (the table is rebuilt when the network restarts)
static void pk_game_tick(void){
    if(g_pkg_want<0){ const char*e=getenv("PELOAD_PKTLOG_GAME"); g_pkg_want=e&&atoi(e)?1:0; pk_bench(); }
    if(!pk_on()) return; int want=g_pkg_want;
    if(want && !g_pkg_installed) pkg_install(); else if(!want && g_pkg_installed) pkg_uninstall();
    else if(want){ static double last; double now=st_wall(); if(now-last>1.0){ last=now; uint8_t*tb; int redo=0;
            if(!pkg_table(&tb)) return; if(tb!=g_pkg_tbl) redo=1;
            for(int i=0;i<g_pkg_n && !redo;i++) for(int d=0;d<2;d++){ void*cur=*(void**)(tb+(size_t)i*0x40+0x20+8*d); if(cur && !pkg_is_thunk(cur)) redo=1; }
            if(redo){ g_pkg_installed=0; pkg_install(); } } } }

// PELOAD_PKTLOG_BENCH=1: the cost of one datagram's record, measured at start (1M records of a 1-RTT datagram), then
// the ring is cleared. "Does the log slow the game down?" in numbers: a sendto / recvfrom itself takes microseconds.
static void pk_bench(void){ static int done; if(done || !getenv("PELOAD_PKTLOG_BENCH") || !pk_on()) return; done=1;
    uint8_t d[64]={0x43,1,2,3,4,5,6,7}; struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(49190); a.sin_addr.s_addr=htonl(0x7f000001);
    struct sockaddr_storage ss; memset(&ss,0,sizeof ss); memcpy(&ss,&a,sizeof a); int n=1000000;
    double t0=st_wall(); for(int i=0;i<n;i++) pk_wire(i&1,-1,&ss,d,48); double t1=st_wall();
    memset(g_pk,0,sizeof g_pk); __atomic_store_n(&g_pk_seq,0,__ATOMIC_RELEASE);
    char m[120]; int k=snprintf(m,sizeof m,"  [pktlog] bench: %.0f ns per datagram record (%d records)\n",(t1-t0)*1e9/n,n); (void)!write(2,m,(size_t)k); }

// ---- readers ----
static const char* pk_dirs(int d){ return d==PK_RX?"rx":"tx"; }
static void pk_ev_json(StBuf*s,const PkEv*e,uint32_t seq,int first){ char peer[80], host[48]; pk_peer(e,peer,sizeof peer); pk_host(e,host,sizeof host);
    sb_put(s,"%s{\"seq\":%u,\"t\":%.4f,\"layer\":\"%s\",\"dir\":\"%s\",\"type\":",first?"":",",seq,e->t,e->layer==PK_GAME?"game":"wire",pk_dirs(e->dir)); sb_str(s,e->type);
    sb_put(s,",\"len\":%u,\"lport\":%u,\"flags\":%u",e->len,e->lport,e->flags);
    if(peer[0]){ sb_put(s,",\"peer\":"); sb_str(s,peer); } if(host[0]){ sb_put(s,",\"host\":"); sb_str(s,host); } sb_put(s,"}"); }
static int pk_get(uint32_t seq,PkEv*out){ const PkEv*e=&g_pk[seq%PK_N]; if(__atomic_load_n(&e->seq,__ATOMIC_ACQUIRE)!=seq) return 0;
    memcpy(out,(const void*)e,sizeof *out); return __atomic_load_n(&e->seq,__ATOMIC_ACQUIRE)==seq; }   // (overwritten meanwhile: skip)
typedef struct { uint8_t layer, dir, fam; uint8_t addr[16]; uint16_t rport; char type[24]; uint64_t n, bytes; double t0, t1; } PkAgg;
static int pk_agg_add(PkAgg*a,int na,int cap,const PkEv*e){
    for(int i=0;i<na;i++){ PkAgg*x=&a[i]; if(x->layer==e->layer && x->dir==e->dir && x->fam==e->fam && x->rport==e->rport && !memcmp(x->addr,e->addr,16) && !strcmp(x->type,e->type)){
            x->n++; x->bytes+=e->len; if(e->t>x->t1) x->t1=e->t; return na; } }
    if(na>=cap) return na; PkAgg*x=&a[na]; x->layer=e->layer; x->dir=e->dir; x->fam=e->fam; x->rport=e->rport; memcpy(x->addr,e->addr,16); snprintf(x->type,24,"%s",e->type);
    x->n=1; x->bytes=e->len; x->t0=x->t1=e->t; return na+1; }
static void pk_agg_json(StBuf*s,const PkAgg*x,int first,double secs){ PkEv e; memset(&e,0,sizeof e); e.fam=x->fam; memcpy(e.addr,x->addr,16); e.rport=x->rport;
    char peer[80], host[48]; pk_peer(&e,peer,sizeof peer); pk_host(&e,host,sizeof host);
    sb_put(s,"%s{\"layer\":\"%s\",\"dir\":\"%s\",\"type\":",first?"":",",x->layer==PK_GAME?"game":"wire",pk_dirs(x->dir)); sb_str(s,x->type);
    sb_put(s,",\"n\":%llu,\"bytes\":%llu",(unsigned long long)x->n,(unsigned long long)x->bytes);
    if(secs>0) sb_put(s,",\"per_s\":%.2f,\"bytes_per_s\":%.0f",x->n/secs,x->bytes/secs); else sb_put(s,",\"t0\":%.3f,\"t1\":%.3f",x->t0,x->t1);
    if(peer[0]){ sb_put(s,",\"peer\":"); sb_str(s,peer); } if(host[0]){ sb_put(s,",\"host\":"); sb_str(s,host); } sb_put(s,"}"); }
// GET /packets.json?since=<seq>&max=<n>: the records after since (the newest max, default 400) + the last 10 s summed per
// (layer, direction, type, peer)
static void pk_json(StBuf*s,const char*qs){
    const char*v=strstr(qs,"since="); uint32_t since=v?(uint32_t)strtoul(v+6,0,10):0; v=strstr(qs,"max="); int mx=v?atoi(v+4):400; if(mx<1) mx=1; if(mx>4000) mx=4000;
    uint32_t top=__atomic_load_n(&g_pk_seq,__ATOMIC_ACQUIRE); uint32_t lo=top>PK_N-64?top-(PK_N-64):1; if(since+1>lo) lo=since+1; if(top>=lo && top-lo+1>(uint32_t)mx) lo=top-(uint32_t)mx+1;
    sb_put(s,"{\"seq\":%u,\"on\":%s,\"game\":{\"want\":%s,\"on\":%s,\"types\":%d},\"events\":[",top,pk_on()?"true":"false",g_pkg_want>0?"true":"false",g_pkg_installed?"true":"false",g_pkg_n);
    int f=1; PkEv e; for(uint32_t q=lo;q<=top && top;q++) if(pk_get(q,&e)){ pk_ev_json(s,&e,q,f); f=0; }
    // the last 10 s (or what the ring still holds: a busy server writes more than 16384 records in 10 s)
    double now=st_wall(), oldest=now; static PkAgg a[512]; int na=0; uint32_t lo2=top>PK_N-64?top-(PK_N-64):1;
    for(uint32_t q=top;q>=lo2 && q;q--){ if(!pk_get(q,&e)) continue; if(now-e.t>10.0) break; if(e.t<oldest) oldest=e.t; na=pk_agg_add(a,na,512,&e); }
    double win=now-oldest; if(win<1.0) win=1.0; if(win>10.0) win=10.0;
    sb_put(s,"],\"summary_s\":%.1f,\"summary\":[",win); for(int i=0;i<na;i++) pk_agg_json(s,&a[i],!i,win);
    sb_put(s,"],\"game_counts\":["); for(int i=0,ff=1;i<g_pkg_n;i++){ if(!g_pkg_count[i][0] && !g_pkg_count[i][1]) continue;
        sb_put(s,"%s{\"type\":",ff?"":","); ff=0; sb_str(s,g_pkg_name[i]); sb_put(s,",\"tx\":%llu,\"rx\":%llu,\"flags\":%u}",(unsigned long long)g_pkg_count[i][0],(unsigned long long)g_pkg_count[i][1],g_pkg_flags[i]); }
    sb_put(s,"]}"); }
static int pk_control(const char*qs){   // POST /control?do=pktgame&on=0|1
    const char*v=strstr(qs,"on="); if(!v) return 0; g_pkg_want=atoi(v+3)?1:0;
    char m[100]; int k=snprintf(m,sizeof m,"  [pktlog] page: game layer %s (switches at the next engine frame)\n",g_pkg_want?"on":"off"); (void)!write(2,m,(size_t)k); return 1; }

// ---- the forensic archive: once a second, the second's records summed per (layer, direction, type, peer) -> packets.jsonl
// (only while the archive is on and we are in a server's session: menu time is not archived)
static void pk_forensic_tick(void){
    static uint32_t last; uint32_t top=__atomic_load_n(&g_pk_seq,__ATOMIC_ACQUIRE); if(!last){ last=top; return; }
    if(!pk_on() || !g_fx_on || !g_fxr.key[0]){ last=top; return; }
    uint32_t lo=last+1, lost=0; if(top>last+PK_N-64){ lost=top-last-(PK_N-64); lo=top-(PK_N-64)+1; } last=top;
    static PkAgg a[1024]; int na=0; PkEv e; for(uint32_t q=lo;q<=top;q++) if(pk_get(q,&e)) na=pk_agg_add(a,na,1024,&e); else lost++;
    for(int i=0;i<na;i++){ StBuf b={0}; pk_agg_json(&b,&a[i],1,0); if(b.b) fx_push(FXI_PACKETS,b.b,b.n,0,0); free(b.b); }
    if(lost){ char l[80]; int k=snprintf(l,sizeof l,"{\"t\":%.3f,\"lost\":%u}",st_wall(),lost); fx_push(FXI_PACKETS,l,(size_t)k,0,0); } }
