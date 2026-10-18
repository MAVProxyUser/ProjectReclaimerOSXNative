// stats.c — live game state over HTTP (PELOAD_STATS_HTTP=<port>). Included by peload.c after hitbox.c.
// A loader thread snapshots the engine's object table 10 times a second -- the fields the overlay reads (hitbox.c,
// shim/docs/halo3-objects.md) -- and serves it on 127.0.0.1:<port> (PELOAD_STATS_ADDR=<ip> listens elsewhere):
//   /stats.json  the latest snapshot          /events  Server-Sent Events, one snapshot per 100 ms
//   /            a page that shows /events as a live table
// It is what this client's engine holds: Halo 3 replicates every player's biped (position, health, shield, camo,
// weapons) to every client. Off by default; independent of the overlay (F2-F6).
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int g_st_port=-1;
static int stats_on(void){ if(g_st_port<0){ const char*e=getenv("PELOAD_STATS_HTTP"); g_st_port=e?atoi(e):0; if(g_st_port==1) g_st_port=8765; if(g_st_port<0||g_st_port>65535) g_st_port=0; } return g_st_port>0; }
static pthread_mutex_t g_st_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_st_cv=PTHREAD_COND_INITIALIZER;
static char* g_st_json; static size_t g_st_len; static unsigned long g_st_seq;

typedef struct { char*b; size_t n, cap; } StBuf;
static void sb_put(StBuf*s,const char*fmt,...){ va_list ap; for(;;){ size_t room=s->cap-s->n; va_start(ap,fmt); int k=vsnprintf(s->b?s->b+s->n:0,room,fmt,ap); va_end(ap);
        if(k<0) return; if((size_t)k<room){ s->n+=(size_t)k; return; } size_t nc=(s->cap?s->cap*2:16384); while(nc<s->n+(size_t)k+1) nc*=2; char*nb=realloc(s->b,nc); if(!nb) return; s->b=nb; s->cap=nc; } }
static void sb_str(StBuf*s,const char*v){ sb_put(s,"\""); for(const unsigned char*p=(const unsigned char*)v; p&&*p; p++){
        if(*p=='"'||*p=='\\') sb_put(s,"\\%c",*p); else if(*p<0x20) sb_put(s,"\\u%04x",*p); else sb_put(s,"%c",*p); } sb_put(s,"\""); }
static void sb_f3(StBuf*s,const float*v){ sb_put(s,"[%.2f,%.2f,%.2f]",v[0],v[1],v[2]); }

// ---- event feed (GET /feed.json?since=<seq>; the snapshot carries "feed_seq"): what the game tells its players --
// joins and leaves, kills, votes, server notices, match start and end. Sources: the client's own log lines
// (st_feed_log, from NtWriteFile -- where Rust's std writes stdio --, WriteFile and WriteConsoleW on its standard handles),
// the chat hook, halo3's display_event / chud hooks, and the death tracker (st_snapshot: a death, and the player
// whose projectile ended inside that biped just before it as the estimated killer).
// kind: join leave kill death vote server game chat weapon vehicle powerup. a/b: the players (killer, victim; chat: sender), how: the weapon
// (chat: the channel). flags: 1 = estimated, 2 = we sent it (chat), 4 = a betrayal.
// pos: where it happened (spawns and power-up pickups: st_spawns sets t_st_evpos around the call), haspos = 1 then.
typedef struct { uint32_t seq; int flags, ta, tb, haspos; double t; float pos[3]; char kind[12], a[64], b[64], how[64], verb[24], text[320]; } StEvt;
static __thread const float*t_st_evpos;   // the next event's position (st_spawns), else 0
// forensic.c (included after this file; shim/docs/forensics.md) archives every event, voice packet and roster change.
// g_fx_on is the hooks' one check while it is off; t_st_chat_id hands it a chat line's sender peer id (st_on_chat).
static volatile int g_fx_on; static void fx_event(const StEvt*e); static __thread uint64_t t_st_chat_id;
static char g_st_curmap[64];   // the map the last snapshot saw
enum { ST_NEV=1024 };
static StEvt g_ev[ST_NEV]; static uint32_t g_ev_seq; static pthread_mutex_t g_ev_mx=PTHREAD_MUTEX_INITIALIZER;
static double st_wall(void){ struct timeval tv; gettimeofday(&tv,0); return tv.tv_sec+tv.tv_usec*1e-6; }
// at: the event's wall-clock time (0 = now)
static void st_eventv(double at,const char*kind,const char*a,const char*b,const char*how,const char*verb,int ta,int tb,int flags,const char*fmt,va_list ap){
    StEvt e; memset(&e,0,sizeof e); e.t=at>0?at:st_wall(); e.flags=flags; e.ta=ta; e.tb=tb; if(verb) snprintf(e.verb,sizeof e.verb,"%s",verb);
    if(t_st_evpos){ memcpy(e.pos,t_st_evpos,sizeof e.pos); e.haspos=1; }
    snprintf(e.kind,sizeof e.kind,"%s",kind); snprintf(e.a,sizeof e.a,"%s",a?a:""); snprintf(e.b,sizeof e.b,"%s",b?b:""); snprintf(e.how,sizeof e.how,"%s",how?how:"");
    vsnprintf(e.text,sizeof e.text,fmt,ap);
    pthread_mutex_lock(&g_ev_mx); e.seq=++g_ev_seq; g_ev[e.seq%ST_NEV]=e; pthread_mutex_unlock(&g_ev_mx);
    if(g_fx_on) fx_event(&e);   // the forensic archive (forensic.c)
    static int lg=-1; if(lg<0) lg=getenv("PELOAD_FEED_LOG")?1:0;   // PELOAD_FEED_LOG=1: also print each event
    if(lg){ char m[440]; int k=snprintf(m,sizeof m,"  [feed] %s: %s\n",e.kind,e.text); if(k>(int)sizeof m-1) k=(int)sizeof m-1; (void)!write(2,m,(size_t)k); } }
static void st_event(const char*kind,const char*a,const char*b,const char*how,int ta,int tb,int flags,const char*fmt,...){
    va_list ap; va_start(ap,fmt); st_eventv(0,kind,a,b,how,0,ta,tb,flags,fmt,ap); va_end(ap); }
static void st_event_at(double at,const char*kind,const char*a,const char*b,const char*how,int ta,int tb,int flags,const char*fmt,...){
    va_list ap; va_start(ap,fmt); st_eventv(at,kind,a,b,how,0,ta,tb,flags,fmt,ap); va_end(ap); }
static void st_event_verb(double at,const char*kind,const char*a,const char*b,const char*how,const char*verb,int ta,int tb,int flags,const char*fmt,...){
    va_list ap; va_start(ap,fmt); st_eventv(at,kind,a,b,how,verb,ta,tb,flags,fmt,ap); va_end(ap); }
// kills per player this match (every kill event, exact or estimated; betrayals and suicides don't count), by name;
// cleared with the per-match player tracking (a new map)
static pthread_mutex_t g_kc_mx=PTHREAD_MUTEX_INITIALIZER; static struct { char name[64]; int kills; } g_kc[256]; static int g_nkc;
static void st_kills_reset(void){ pthread_mutex_lock(&g_kc_mx); g_nkc=0; pthread_mutex_unlock(&g_kc_mx); }
static void st_kills_add(const char*a){ pthread_mutex_lock(&g_kc_mx); int i; for(i=0;i<g_nkc;i++) if(!strcmp(g_kc[i].name,a)) break;
    if(i==g_nkc && g_nkc<256){ snprintf(g_kc[i].name,sizeof g_kc[i].name,"%s",a); g_kc[i].kills=0; g_nkc++; } if(i<g_nkc) g_kc[i].kills++; pthread_mutex_unlock(&g_kc_mx); }
static int st_kills_of(const char*a){ int k=0; pthread_mutex_lock(&g_kc_mx); for(int i=0;i<g_nkc;i++) if(!strcmp(g_kc[i].name,a)){ k=g_kc[i].kills; break; } pthread_mutex_unlock(&g_kc_mx); return k; }
static void st_kill_at(double at,const char*a,const char*b,const char*how,const char*verb,int ta,int tb,int flags){   // "A <verb> B (how)"
    char hw[110]=""; if(how && how[0]) snprintf(hw,sizeof hw," (%s)",how);
    st_event_verb(at,"kill",a,b,how,verb,ta,tb,flags,"%s %s %s%s",a,verb,b,hw);
    if(a && a[0] && b && strcmp(a,b) && !(flags&4)) st_kills_add(a); }
static void st_feed_json(StBuf*s,uint32_t since){
    pthread_mutex_lock(&g_ev_mx); uint32_t hi=g_ev_seq, lo=hi>=ST_NEV?hi-ST_NEV+1:1; if(since>=lo && since<=hi) lo=since+1;
    sb_put(s,"{\"seq\":%u,\"events\":[",hi);
    for(uint32_t q=lo;q<=hi;q++){ const StEvt*e=&g_ev[q%ST_NEV];
        sb_put(s,"%s{\"seq\":%u,\"t\":%.3f,\"kind\":\"%s\",\"text\":",q>lo?",":"",e->seq,e->t,e->kind); sb_str(s,e->text);
        if(e->a[0]){ sb_put(s,",\"a\":"); sb_str(s,e->a); } if(e->b[0]){ sb_put(s,",\"b\":"); sb_str(s,e->b); } if(e->how[0]){ sb_put(s,",\"how\":"); sb_str(s,e->how); }
        if(e->ta>=0) sb_put(s,",\"ta\":%d",e->ta); if(e->tb>=0) sb_put(s,",\"tb\":%d",e->tb); if(e->flags&1) sb_put(s,",\"estimated\":true"); if(e->flags&2) sb_put(s,",\"mine\":true"); if(e->flags&4) sb_put(s,",\"betrayal\":true"); if(e->verb[0]){ sb_put(s,",\"verb\":"); sb_str(s,e->verb); }
        if(e->haspos) sb_put(s,",\"pos\":[%.2f,%.2f,%.2f]",e->pos[0],e->pos[1],e->pos[2]); sb_put(s,"}"); }
    sb_put(s,"]}"); pthread_mutex_unlock(&g_ev_mx); }
// Auto-vote (2026-10-18; static RE of the 0.9.18 client, shim/docs/halo3-event-feed.md "Votes"). Player-called votes (kick,
// end round / game, shuffle) are answered with the client's own keys, F5 = yes / F6 = no (keybindings vote_yes / vote_no:
// WM_KEYDOWN to the game window -> exe+0x3bec80 -> answer_vote exe+0x87f840; one answer per vote, only while the vote status
// is < 2 s old, only in the menu runtime): PELOAD_AUTOVOTE=yes|no (default off) presses 0.5 s after the server's "... called
// a vote to ..." line, and again (3 at most) until the client logs "Voted yes|no in vote N.". The lobby's map ballot ("VOTE FOR
// THE NEXT GAME", playlist servers) has no key: PELOAD_AUTOVOTE_MAP=1|2|3|none clicks that card 1, 2 and 3 s after "Menu page:
// Lobby" (client-area coordinates at 1280x720 from the layout code: cards at y 268 / 348 / 428, "none of the above" 484).
// Both switchable on the stats page: POST /control?do=autovote&calls=yes|no|off and/or &map=1|2|3|none|off.
static volatile int g_av_calls=-1, g_av_map=-2;   // calls: 0 off, 1 yes, 2 no; map: -1 off, 0..2 a card, 3 none of the above
static volatile double g_av_due; static int g_av_tries;
static void st_av_init(void){
    if(g_av_calls<0){ const char*e=getenv("PELOAD_AUTOVOTE"); g_av_calls=!e?0:!strcmp(e,"yes")?1:!strcmp(e,"no")?2:0; }
    if(g_av_map==-2){ const char*e=getenv("PELOAD_AUTOVOTE_MAP"); int v=e?atoi(e):0; g_av_map=!e?-1:!strcmp(e,"none")?3:(v>=1&&v<=3)?v-1:-1; } }
extern void wi_steps_async(const char*e);
static void st_av_called(void){ st_av_init(); if(!g_av_calls) return; g_av_tries=0; g_av_due=mono_s()+0.5; }
// Map ballot vote (2026-10-18; static RE of 0.9.15 .. 0.9.18): a joined client never shows the ballot screen under the loader
// (its loading splash covers the lobby), so clicks can't vote. The client's own cast_vote(const u128*round, u64 choice)
// (0.9.18 exe+0x8f9fe0; builds the lobby Vote message and sends it to the host) is called directly, with the round of the
// Ballot the host broadcasts every 250 ms -- cached in a Mutex<Option<Ballot>> (0.9.18 exe+0x27d1ce0: None = 0x8000000000000000
// in its first qword; round at size-0x20, then counts[4], seconds_left, vote_seconds). It must run on a guest thread (a TEB:
// its send path reaches thread locals), so a worker made with our CreateThread waits for requests. Confirmed when counts[choice]
// in the next broadcast goes up; else sent again (3 times at most; the host just overwrites a voter's choice).
static int sig_parse(const char*s,uint8_t*b,uint8_t*m,int cap); static int64_t sig_find(uint8_t*base,const uint8_t*b,const uint8_t*m,int n);   // (peload.c)
static void*g_bv_fn; static uint8_t*g_bv_cache; static uint64_t g_bv_size;
static pthread_mutex_t g_bv_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_bv_cv=PTHREAD_COND_INITIALIZER;
static uint8_t g_bv_round[16]; static uint64_t g_bv_choice; static int g_bv_req;
static uint32_t MSABI st_bv_worker(void*a){ (void)a;
    for(;;){ uint8_t round[16]; uint64_t ch; pthread_mutex_lock(&g_bv_mx); while(!g_bv_req) pthread_cond_wait(&g_bv_cv,&g_bv_mx);
        memcpy(round,g_bv_round,16); ch=g_bv_choice; g_bv_req=0; pthread_mutex_unlock(&g_bv_mx);
        ((void(MSABI*)(const void*,uint64_t))g_bv_fn)(round,ch);
        char m[80]; int k=snprintf(m,sizeof m,"  [autovote] map ballot: vote sent (choice %d)\n",(int)ch+1); (void)!write(2,m,k); }
    return 0; }
static void st_bv_install(void){
    uint8_t b[128],m[128]; int n=sig_parse("55 56 57 48 81 EC B0 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 28 FE FF FF FF 48 89 D6 48 89 CF B1 01 31 C0 F0 0F B0 0D ?? ?? ?? ?? 75 22",b,m,128);
    int64_t r=sig_find(g_base,b,m,n); const char*why=0; uint8_t*fn=0;
    if(r<0) why="cast_vote not found (0.9.15+ only)";
    else { fn=g_base+r; int ok=0;   // meaning: it builds Message::Vote (tag 0x80000000000000xx) and calls lobby_send
        for(uint8_t*q=fn;q<fn+0x120 && !ok;q++) if(q[0]==0x48&&q[1]==0xB8&&q[3]==0&&q[4]==0&&q[5]==0&&q[6]==0&&q[7]==0&&q[8]==0&&q[9]==0x80&&q[10]==0x48&&q[11]==0x89&&q[12]==0x45&&q[13]==0xA8) ok=1;
        if(!ok) why="cast_vote match has no Vote message (another build?)"; }
    if(!why){ n=sig_parse("31 C9 48 3B 0D ?? ?? ?? ?? 70 ?? 89 85 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 89 45 ?? 89 55 ?? 48 8D 4D ?? 48 89 FA 41 89 D8 E8 ?? ?? ?? ?? 90 81 FA 00 CA 9A 3B 0F 94 C1 48 83 F8 03 0F 92 C0",b,m,128);
        int64_t h=sig_find(g_base,b,m,n); if(h<0) why="ballot cache not found";
        else { uint8_t*hit=g_base+h; uint8_t*cache=hit+9+*(int32_t*)(hit+5), *recv=hit+0x18+*(int32_t*)(hit+0x14);
            uint64_t sz=(uint64_t)(recv-cache); if(sz<0x100 || sz>0x1000) why="ballot cache size implausible"; else { g_bv_fn=fn; g_bv_cache=cache; g_bv_size=sz; } } }
    char msg[200]; int k=why?snprintf(msg,sizeof msg,"  [stats] map ballot vote: %s\n",why)
        :snprintf(msg,sizeof msg,"  [stats] map ballot vote: cast_vote exe+0x%llx, ballot cache exe+0x%llx (0x%llx bytes)\n",(unsigned long long)(fn-g_base),(unsigned long long)(g_bv_cache-g_base),(unsigned long long)g_bv_size);
    (void)!write(2,msg,k);
    if(g_bv_fn){ uint32_t tid=0; i_CreateThread(0,0,(WinThreadProc)st_bv_worker,0,0,&tid); } }
static void st_bv_tick(double now){   // stats thread: a new open ballot -> queue our vote; confirm by its counts
    if(!g_bv_cache || g_av_map<0) return;
    static uint8_t voted[16]; static int tries; static double tsent; static uint8_t cnt0;
    uint64_t tag=0; if(!hb_read((uint64_t)(uintptr_t)g_bv_cache,&tag,8) || tag==0x8000000000000000ull) { tries=0; return; }
    uint8_t rd[0x20], rd2[0x20]; uint64_t off=g_bv_size-0x20;
    if(!hb_read((uint64_t)(uintptr_t)(g_bv_cache+off),rd,0x20) || !hb_read((uint64_t)(uintptr_t)(g_bv_cache+off),rd2,0x20) || memcmp(rd,rd2,16)) return;
    uint8_t*counts=rd+0x10; uint32_t left=*(uint32_t*)(rd+0x14); int ch=g_av_map;
    if(memcmp(voted,rd,16)){   // a new ballot
        if(!left) return;
        memcpy(voted,rd,16); tries=0; cnt0=counts[ch];
        if(g_bv_size==0x320){ char m[400]; int k=snprintf(m,sizeof m,"  [autovote] map ballot open (%u s):",left);   // 0.9.17+ Choice layout: map String at +0x38/+0x40
            for(int i=0;i<3;i++){ uint64_t mp=0,ml=0; char nm[40]=""; uint8_t*opt=g_bv_cache+0x18+i*0xf8;
                if(hb_read((uint64_t)(uintptr_t)(opt+0x38),&mp,8) && hb_read((uint64_t)(uintptr_t)(opt+0x40),&ml,8) && mp && ml<39) hb_read(mp,nm,ml);
                k+=snprintf(m+k,sizeof m-k," %d=%s",i+1,nm[0]?nm:"?"); }
            k+=snprintf(m+k,sizeof m-k," -> voting %s\n",ch==3?"none of the above":ch==0?"1":ch==1?"2":"3"); (void)!write(2,m,k); } }
    else if(tries && counts[ch]>cnt0) { if(tries<99){ char m[80]; int k=snprintf(m,sizeof m,"  [autovote] map ballot: counted (%u votes for it)\n",counts[ch]); (void)!write(2,m,k); } tries=99; return; }
    if(tries>=3 || !left || (tries && now-tsent<1.0)) return;
    pthread_mutex_lock(&g_bv_mx); memcpy(g_bv_round,rd,16); g_bv_choice=(uint64_t)ch; g_bv_req=1; pthread_cond_signal(&g_bv_cv); pthread_mutex_unlock(&g_bv_mx);
    tries++; tsent=now; }
static void st_av_lobby(void){}   // (the ballot is voted through cast_vote now, st_bv_tick)
static void st_av_tick(double now){   // stats thread, every snapshot (100 ms)
    st_bv_tick(now);
    double due=g_av_due; if(!due || now<due) return;
    if(!g_av_calls || g_av_tries>=3){ g_av_due=0; return; }
    g_av_tries++; g_av_due=now+1.5; wi_steps_async(g_av_calls==1?"k:60":"k:61");   // macOS key codes: F5 0x60, F6 0x61
    char m[80]; int k=snprintf(m,sizeof m,"  [autovote] vote: pressing %s (try %d)\n",g_av_calls==1?"F5 (yes)":"F6 (no)",g_av_tries); (void)!write(2,m,k); }
// One line of the client's log ("HH:MM:SS.mmm <text>"): the notices it also shows its player.
// PELOAD_FEED_MATCH=<text>[,<text>...] adds lines containing any of those texts (kind "log").
static void st_feed_line(const char*ln){
    if(!(ln[0]>='0'&&ln[0]<='9' && ln[2]==':' && ln[5]==':' && ln[8]=='.' && ln[12]==' ')) return;   // client log lines only
    const char*p=ln+13; size_t L=strlen(p);
    if(!strncmp(p,"Community roster: ",18)){ const char*r=p+18; size_t rl=strlen(r); char nm[64];
        if(rl>18 && !strcmp(r+rl-18," joined the server")){ snprintf(nm,sizeof nm,"%.*s",(int)(rl-18),r); st_event("join",nm,0,0,-1,-1,0,"%s joined the server",nm); }
        else if(rl>16 && !strcmp(r+rl-16," left the server")){ snprintf(nm,sizeof nm,"%.*s",(int)(rl-16),r); st_event("leave",nm,0,0,-1,-1,0,"%s left the server",nm); }
        else st_event("server",0,0,0,-1,-1,0,"%s",r);
        return; }
    if(strstr(p," called a vote to ") || !strncmp(p,"Vote ",5) || !strncmp(p,"Called a vote to ",17) || !strncmp(p,"Voted ",6)){
        if(!strncmp(p,"Voted ",6)) g_av_due=0; else if(strstr(p," called a vote to ")) st_av_called();
        st_event("vote",0,0,0,-1,-1,0,"%s",p); return; }
    if(!strncmp(p,"Menu page: Lobby",16)){ st_av_lobby(); return; }
    static const char*const srv[]={"Team shuffle:","Teams have been shuffled","Teams shuffled","You were kicked","Host roster lost","A player still joining",
        "Moderation:","The kick-vote target"};
    for(size_t i=0;i<sizeof srv/sizeof*srv;i++) if(!strncmp(p,srv[i],strlen(srv[i]))){ st_event("server",0,0,0,-1,-1,0,"%s",p); return; }
    if(!strncmp(p,"Carnage report opened",21)){ st_event("game",0,0,0,-1,-1,0,"Game over"); return; }
    if(!strncmp(p,"Loading screen complete",23)){ char mp[64]; snprintf(mp,sizeof mp,"%s",g_st_curmap); st_event("game",mp,0,0,-1,-1,0,"Match started: %s",mp); return; }
    static char mt[512]; static int mi=-1; if(mi<0){ const char*e=getenv("PELOAD_FEED_MATCH"); snprintf(mt,sizeof mt,"%s",e?e:""); mi=1; }
    if(mt[0] && L){ char w[512]; snprintf(w,sizeof w,"%s",mt); char*sv=0; for(char*tk=strtok_r(w,",",&sv); tk; tk=strtok_r(0,",",&sv)) if(*tk && strstr(p,tk)){ st_event("log",0,0,0,-1,-1,0,"%s",p); return; } } }
static void st_feed_log(int fd,const char*buf,size_t n){
    if(!stats_on() || (fd!=1 && fd!=2) || !buf) return;
    static char lb[2][1024]; static size_t ll[2]; static int skip[2]; static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; int w=fd==2;
    pthread_mutex_lock(&mx);
    for(size_t i=0;i<n;i++){ char c=buf[i];
        if(c=='\n'){ if(!skip[w] && ll[w]>13){ lb[w][ll[w]]=0; st_feed_line(lb[w]); } ll[w]=0; skip[w]=0; }
        else if(c=='\r') continue;
        else if(ll[w]<sizeof lb[w]-1) lb[w][ll[w]++]=c; else skip[w]=1; }   // longer lines (the shell's JSON) are skipped
    pthread_mutex_unlock(&mx); }

// The engine's players table (TLS block +0x10, a data array like the object table): +0x28 unit handle, +0x58 name.
typedef struct { uint32_t unit; int slot; char name[64]; uint64_t rec; } StPlayer;
static uint64_t g_st_tls;   // the engine thread's halo3 TLS block (st_players found it)
static int st_players(StPlayer*out,int max){
    uint8_t*h3=hb_halo3_base(); uint32_t idx=0; if(!h3 || !hb_read((uint64_t)(h3+0xa39f9c),&idx,4) || idx>=128) return 0;
    void*blks[1025]; int na=tls_blocks(idx,blks,1025);
    for(int q=0;q<na;q++){ uint64_t blk=(uint64_t)blks[q], parr=0, oarr=0; uint8_t hdr[0x60];
        if(!blk || !hb_read(blk+0x38,&oarr,8) || !oarr) continue;   // the engine thread's block (it also holds the object table)
        if(!hb_read(blk+0x10,&parr,8) || !parr || !hb_read(parr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) continue;
        int32_t es=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t pd=*(uint64_t*)(hdr+0x48); if(es<0x98 || es>0x10000 || hi<=0 || hi>256) continue;
        int n=0; for(int i=0;i<hi && n<max;i++){ uint8_t rec[0x98]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue;
            StPlayer*p=&out[n]; p->unit=*(uint32_t*)(rec+0x28); p->slot=i; p->rec=pd+(uint64_t)i*es; int j=0; const uint16_t*w=(const uint16_t*)(rec+0x58);
            for(int c=0;c<32 && w[c] && j<60;c++){ uint16_t u=w[c]; if(u<0x80) p->name[j++]=(char)u;
                else if(u<0x800){ p->name[j++]=(char)(0xC0|(u>>6)); p->name[j++]=(char)(0x80|(u&63)); }
                else { p->name[j++]=(char)(0xE0|(u>>12)); p->name[j++]=(char)(0x80|((u>>6)&63)); p->name[j++]=(char)(0x80|(u&63)); } }
            p->name[j]=0; n++; }
        g_st_tls=blk; return n; }
    return 0; }

// ---- game variant and player traits (layout: shim/docs/halo3-variant-traits.md) -------------------------------------
// The running variant is game_globals (TLS block +0x48) + 0x1608 (0x4C4 bytes), only while game_globals+0x10 == 2
// (multiplayer). A trait set is 0x1C bytes; 0 = "Unchanged" in a variant's sets (weapons: -2). Each player's record
// holds its resolved active traits at +0x1FC, recomputed every tick (base -> mode role -> powerup -> respawn traits).
// Names and value labels follow the playlist option keys (shim/docs/game-options.md).
static const char*const tl_res[]={"Unchanged","10%","50%","90%","100%","110%","150%","200%","300%","500%","1000%","2000%","Invulnerable","Invulnerable (incl. assassinations)"};
static const char*const tl_rech[]={"Unchanged","-25%","-10%","-5%","Disabled","50%","90%","100%","110%","200%"};
static const char*const tl_vamp[]={"Unchanged","Off","10%","25%","50%","100%"};
static const char*const tl_head[]={"Unchanged","Immune","Vulnerable"};
static const char*const tl_shield[]={"Unchanged","None","Normal","2x","3x","4x"};
static const char*const tl_gren[]={"Unchanged","Map default","None"};
static const char*const tl_dmg[]={"Unchanged","0%","25%","50%","75%","90%","100%","110%","125%","150%","200%","300%","Instant kill"};
static const char*const tl_regen[]={"Unchanged","On","Off","On, with firebombs"};
static const char*const tl_ammo[]={"Unchanged","Normal","Infinite","Bottomless clip"};
static const char*const tl_pick[]={"Unchanged","Allowed","Disabled"};
static const char*const tl_speed[]={"Unchanged","25%","50%","75%","90%","100%","110%","125%","150%","200%","300%"};
static const char*const tl_grav[]={"Unchanged","50%","75%","100%","150%","200%"};
static const char*const tl_veh[]={"Unchanged","None","Passenger only","Full use"};
static const char*const tl_camo[]={"Unchanged","Off","Poor","Good","Invisible"};
static const char*const tl_wp[]={"Unchanged","Off","Allies","Everyone"};
static const char*const tl_aura[]={"Unchanged","Off","Team color","Black","White"};
static const char*const tl_radar[]={"Unchanged","Off","Allies only","Normal","Enhanced"};
static const char*const tl_range[]={"Unchanged","10m","15m","25m","50m","75m","100m","150m"};
static const char*const tl_weap[]={"Battle rifle","Assault rifle","Plasma pistol","Spiker","SMG","Carbine","Energy sword","Magnum","Needler",
    "Plasma rifle","Rocket launcher","Shotgun","Sniper rifle","Brute shot","Unarmed","Beam rifle","Spartan laser","No weapon","Gravity hammer",
    "Mauler","Flamethrower","Missile pod","Sentinel beam","Fuel rod gun","Silenced SMG","Automag","Brute plasma rifle","Machine gun turret",
    "Plasma turret","Golf club"};
#define TL(a) a,(int)(sizeof a/sizeof*a)
typedef struct { const char*key; int off, sz; const char*const*lab; int nlab; int def; } StTraitF;   // sz: 1 u8, 2 u16, -1 i8 weapon, 0 colour
static const StTraitF g_trait_f[]={
    {"resistance",0x00,1,TL(tl_res),4},{"recharge",0x01,1,TL(tl_rech),7},{"vampirism",0x02,1,TL(tl_vamp),1},{"headshots",0x03,1,TL(tl_head),2},
    {"shields",0x04,1,TL(tl_shield),2},{"grenades",0x08,2,TL(tl_gren),1},{"primary_weapon",0x0A,-1,TL(tl_weap),-1},{"secondary_weapon",0x0B,-1,TL(tl_weap),-1},
    {"damage",0x0C,1,TL(tl_dmg),6},{"grenade_regeneration",0x0D,1,TL(tl_regen),2},{"ammo",0x0E,1,TL(tl_ammo),1},{"pickup",0x0F,1,TL(tl_pick),1},
    {"speed",0x10,1,TL(tl_speed),5},{"gravity",0x11,1,TL(tl_grav),3},{"vehicles",0x12,1,TL(tl_veh),3},{"camouflage",0x14,1,TL(tl_camo),1},
    {"waypoint",0x15,1,TL(tl_wp),2},{"aura",0x16,1,TL(tl_aura),1},{"color",0x17,0,0,0,1},{"radar",0x18,2,TL(tl_radar),3},{"radar_range",0x1A,2,TL(tl_range),3}};
// active=0: a variant's trait set, listing the fields it changes; active=1: a player's resolved traits, listing the
// fields that differ from Halo 3's defaults
static void st_traits(StBuf*s,const uint8_t*t,int active){
    sb_put(s,"{"); int first=1;
    for(size_t i=0;i<sizeof g_trait_f/sizeof*g_trait_f;i++){ const StTraitF*f=&g_trait_f[i]; int v; char buf[48]; const char*lab=0;
        if(f->sz==2) v=*(const uint16_t*)(t+f->off); else if(f->sz==-1) v=*(const int8_t*)(t+f->off); else v=t[f->off];
        if(f->sz==-1){ if(active?v==f->def:v==-2) continue;
            if(v==-3) lab="Random"; else if(v==-1) lab="Map default"; else if(v==-2) lab="Unchanged"; else if(v>=0 && v<f->nlab) lab=f->lab[v]; }
        else { if(active?v==f->def:v==0) continue;
            if(f->lab && v>=0 && v<f->nlab) lab=f->lab[v]; else if(!f->lab){ if(v==0) lab="Unchanged"; else if(v==1) lab="Off"; else { snprintf(buf,sizeof buf,"Color %d",v-2); lab=buf; } } }
        if(!lab){ snprintf(buf,sizeof buf,"%d",v); lab=buf; }
        sb_put(s,"%s\"%s\":",first?"":",",f->key); sb_str(s,lab); first=0; }
    sb_put(s,"}"); }
static uint8_t g_st_var[0x4C4]; static int g_st_var_ok;   // this snapshot's variant (for the players' powerup durations)
static void st_variant(StBuf*s){
    g_st_var_ok=0; uint64_t gg=0; uint8_t mode=0;
    if(!g_st_tls || !hb_read(g_st_tls+0x48,&gg,8) || !gg || !hb_read(gg+0x10,&mode,1) || mode!=2 || !hb_read(gg+0x1608,g_st_var,sizeof g_st_var)) return;
    const uint8_t*v=g_st_var; int32_t eng=*(const int32_t*)v; if(eng<1 || eng>12 || eng==11) return; g_st_var_ok=1;
    static const char*const en[13]={"","Capture the Flag","Slayer","Oddball","King of the Hill","Forge","VIP","Juggernaut","Territories","Assault","Infection","","Gun Game"};
    char nm[64]; int j=0; for(int c=0;c<16 && j<60;c++){ uint16_t u=*(const uint16_t*)(v+0x14+c*2); if(!u) break;
        if(u<0x80) nm[j++]=(char)u; else if(u<0x800){ nm[j++]=(char)(0xC0|(u>>6)); nm[j++]=(char)(0x80|(u&63)); } else { nm[j++]=(char)(0xE0|(u>>12)); nm[j++]=(char)(0x80|((u>>6)&63)); nm[j++]=(char)(0x80|(u&63)); } }
    nm[j]=0;
    static const char*const tc[3]={"Disabled","Enabled","Balancing only"}; uint16_t soc=*(const uint16_t*)(v+0x12C), tch=*(const uint16_t*)(v+0x12E); uint32_t mo=*(const uint32_t*)(v+0x130);
    sb_put(s,",\"variant\":{\"engine\":%d,\"mode\":",eng); sb_str(s,en[eng]); sb_put(s,",\"name\":"); sb_str(s,nm);
    // the raw variant (0x4C4 bytes): the page decodes every option from it with the client's option table
    { static const char hx[]="0123456789abcdef"; char raw[sizeof g_st_var*2+1]; for(size_t i=0;i<sizeof g_st_var;i++){ raw[2*i]=hx[g_st_var[i]>>4]; raw[2*i+1]=hx[g_st_var[i]&15]; }
      raw[sizeof g_st_var*2]=0; sb_put(s,",\"raw\":\"%s\"",raw); }
    sb_put(s,",\"rounds\":{\"teams\":%s,\"team_changing\":\"%s\",\"friendly_fire\":%s,\"betrayal_booting\":%s,\"reset_players\":%s,\"reset_map\":%s,\"minutes\":%d,\"limit\":%d,\"early_victory\":%d}",
        v[0x104]&1?"true":"false",tch<3?tc[tch]:"?",soc&1?"true":"false",soc&2?"true":"false",v[0x104]&2?"true":"false",v[0x104]&4?"true":"false",v[0x105],v[0x106],v[0x107]);
    sb_put(s,",\"respawn\":{\"lives\":%d,\"team_lives\":%d,\"seconds\":%d,\"suicide\":%d,\"betrayal\":%d,\"growth\":%d,\"traits_duration\":%d,\"synchronize\":%s,\"on_kills\":%s}",
        v[0x109],v[0x10A],v[0x10B],v[0x10C],v[0x10D],v[0x10E],v[0x10F],v[0x108]&2?"true":"false",v[0x108]&8?"true":"false");
    sb_put(s,",\"map\":{\"grenades\":\"%s\",\"indestructible_vehicles\":%s}",mo&1?"Map default":"None",mo&2?"true":"false");
    sb_put(s,",\"powerups\":{\"red_duration\":%d,\"blue_duration\":%d,\"yellow_duration\":%d}",v[0x1A8],v[0x1A9],v[0x1AA]);
    { int sw=0; switch(eng){ case 2: case 6: case 7: sw=0x1B4; break; case 3: case 4: sw=0x1B8; break; case 1: case 9: sw=0x1BC; break; case 12: sw=0x1BE; break; }
      if(sw) sb_put(s,",\"score_to_win\":%d",*(const int16_t*)(v+sw)); }
    if(eng==2){ static const char*const pk[12]={"assist","death","suicide","betrayal","leader_kill","elimination","assassination","headshot","beatdown","sticky","splatter","spree"};
        sb_put(s,",\"points\":{\"kill\":%d",*(const int16_t*)(v+0x1B6)); for(int k=0;k<12;k++) sb_put(s,",\"%s\":%d",pk[k],(int8_t)v[0x1B8+k]); sb_put(s,"}"); }
    // trait sets: the base ones, then this mode's role sets
    typedef struct { const char*n; int off; } TS; static const TS base[5]={{"player",0x134},{"respawn_traits",0x110},{"red_powerup",0x154},{"blue_powerup",0x170},{"yellow_powerup",0x18C}};
    TS role[8]; int nr=0;
    switch(eng){ case 1: role[nr++]=(TS){"flag_carrier",0x1C0}; break; case 2: role[nr++]=(TS){"leader",0x1C4}; break; case 3: role[nr++]=(TS){"ball_carrier",0x1C4}; break;
        case 4: role[nr++]=(TS){"hill_occupant",0x1C0}; break; case 5: role[nr++]=(TS){"editor",0x1B8}; break;
        case 6: role[nr++]=(TS){"vip_team",0x1C6}; role[nr++]=(TS){"vip_proximity",0x1E2}; role[nr++]=(TS){"vip",0x1FE}; break;
        case 7: role[nr++]=(TS){"juggernaut",0x1C4}; break; case 8: role[nr++]=(TS){"defender",0x1BC}; role[nr++]=(TS){"attacker",0x1D8}; break;
        case 9: role[nr++]=(TS){"bomb_carrier",0x1C8}; role[nr++]=(TS){"bomb_arming",0x1E4}; break;
        case 10: role[nr++]=(TS){"infected",0x1C0}; role[nr++]=(TS){"initial_infected",0x1DC}; role[nr++]=(TS){"safe_haven",0x1F8}; role[nr++]=(TS){"last_survivor",0x214}; break;
        case 12: role[nr++]=(TS){"final_tier",0x2D0}; role[nr++]=(TS){"leader",0x2EC}; break; }
    sb_put(s,",\"traits\":{"); for(int k=0;k<5;k++){ sb_put(s,"%s\"%s\":",k?",":"",base[k].n); st_traits(s,v+base[k].off,0); }
    for(int k=0;k<nr;k++){ sb_put(s,",\"%s\":",role[k].n); st_traits(s,v+role[k].off,0); }
    sb_put(s,"}}"); }
// A player's active traits and what is driving them right now (a powerup, respawn traits)
static void st_player_traits(StBuf*s,uint64_t rec){
    uint8_t r[0x228]; if(!rec || !hb_read(rec,r,sizeof r)) return;
    sb_put(s,",\"traits\":"); st_traits(s,r+0x1FC,1);
    sb_put(s,",\"effects\":["); int first=1;
    uint64_t t=0; int32_t now=0; float spt=0;
    if(g_st_tls && hb_read(g_st_tls+0xc8,&t,8) && t && hb_read(t+0xC,&now,4) && hb_read(t+0x8,&spt,4) && spt>0 && spt<1 && g_st_var_ok){
        static const char*const pn[3]={"red powerup","blue powerup","yellow powerup"}; int best=-1; int32_t bt=0;
        for(int k=0;k<3;k++){ int32_t tk=*(const int32_t*)(r+0x218+k*4); if(tk>0 && tk>bt){ bt=tk; best=k; } }
        if(best>=0){ double left=g_st_var[0x1A8+best]-(now-bt)*(double)spt; if(left>0){ sb_put(s,"{\"what\":\"%s\",\"left\":%.1f}",pn[best],left); first=0; } }
        int16_t rt=*(const int16_t*)(r+0x1D4); if(rt>0 && !r[0x1D6]){ sb_put(s,"%s{\"what\":\"respawn traits\",\"left\":%.1f}",first?"":",",rt*(double)spt); first=0; } }
    sb_put(s,"]"); }

static void st_tag(StBuf*s,uint32_t def){ sb_str(s,hb_tag_name(def)); }

// ---- the network session: everyone connected (also in the lobby, where the players table is empty) + voice mute -----
// (shim/docs/reclaimer-voice-mute.md) Halo 3's session membership: sess = *(u64*)(halo3+0x1e250d0). Reclaimer patches
// halo3 for 128 players and then keeps the records in an extended area (flag byte exe+0x27f7210 in 0.9.18):
//   player valid mask sess+0x126c (16 / 128 bits); player i at sess+0x1270+i*0xc8 (extended sess+0x1493c0+i*0xc8):
//     +0x04 u64 identifier (0 = none), +0x0c i32 peer index, +0x98 UTF-16 name[16]
//   peer valid mask sess+0x54 (17 / 128 bits); peer p at sess+0x58+p*0x110 (extended sess+0x1381c0+p*0x110):
//     +0x00 u64 peer id (= the client's voice id), +0x08 u32 state (8 = joined)
// The client's voice mute is one global Mutex<Vec<peer id>> changed by set_muted(id, bool) (Win64; exe+0x66bea0 in
// 0.9.18, found by signature); its own scoreboard Mute buttons use the same list, so both stay in sync. Mutes last
// until the client exits. The call runs on a guest thread: the halo3 main loop's frame hook (st_voice_jobs).
typedef struct { char name[64]; int idx, peer; uint64_t voice; int muted, host; } StSess;
static int g_st_hostpeer=-1, g_st_localpeer=-1;   // the session's host / our peer (sess+0x40 / +0x3dc4; -1 unknown)
static uint8_t* g_st_setmuted; static uint64_t g_st_mutedvec; static int g_st_layout=-1;   // -1 unknown, 0 normal, 1 extended
static int sig_parse(const char*s,uint8_t*b,uint8_t*m,int cap); static int64_t sig_find(uint8_t*base,const uint8_t*b,const uint8_t*m,int n);
static int entry_hook(uint8_t*site,const uint8_t*pro,int len,void*fn);   // peload.c
static int entry_hook_tail(uint8_t*site,const uint8_t*pro,int len,void*fn,const uint8_t*tail,int tail_len,int ret_r9);
// Who is talking: an entry hook on the client's voice packet handler (voice_recv(state, sender peer id, payload, len);
// exe+0x684080 in 0.9.18, by signature; its first 19 bytes are position-independent) stamps the sender's last packet.
// entry_hook passes rcx/rdx/r8/r9 untouched; the payload (guest memory) is [0] channel, [1..9] sequence, [9..] Opus.
static void fx_voice(uint64_t sender,const uint8_t*payload,uint64_t len); static int fx_control(const char*qs);   // forensic.c
static void fx_http(int fd,const char*path,const char*qs,const char*req); static void fx_roster_tick(const char*map,const StSess*ss,int nss,const StPlayer*pl,int np);
static pthread_mutex_t g_talk_mx=PTHREAD_MUTEX_INITIALIZER; static struct { uint64_t id; double t; } g_talk[128];
// PELOAD_VOICE_SRCSTAT: the client's UDP datagrams with a 4-byte "MC?1" / "LC?1" magic (voice and its session traffic,
// shim/docs/reclaimer-voice-mute.md) tallied per magic and source as they arrive in recvfrom (thread-independent): every
// 30 s, per magic, packets / distinct sources / the top source (masked unless it is the joined server).
void st_voice_wiretally(const char*buf,long n,uint32_t ip,uint16_t port){ (void)n;
    static int on=-1; if(on<0) on=getenv("PELOAD_VOICE_SRCSTAT")?1:0; if(!on) return;
    static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; static struct { char mg[5]; uint32_t ip; uint16_t port; unsigned long n; } t[128]; static int nt; static double tl;
    pthread_mutex_lock(&mx); int k=0; for(; k<nt; k++) if(!memcmp(t[k].mg,buf,4) && t[k].ip==ip && t[k].port==port) break;
    if(k==nt && nt<128){ memcpy(t[nt].mg,buf,4); t[nt].mg[4]=0; t[nt].ip=ip; t[nt].port=port; t[nt].n=0; nt++; } if(k<128) t[k].n++;
    double now=mono_s(); if(!tl) tl=now;
    if(now-tl>=30){ char srv[80]=""; { const char*p=strstr(g_cmdlineA,"join?server="); if(p){ p+=12; size_t j2=0; while(*p && *p!=':' && *p!=' ' && *p!='"' && j2+1<sizeof srv) srv[j2++]=*p++; srv[j2]=0; } }
        char done[16][5]; int nd=0;
        for(int a=0;a<nt;a++){ int seen=0; for(int q=0;q<nd;q++) if(!strcmp(done[q],t[a].mg)) seen=1; if(seen || nd>=16) continue; snprintf(done[nd++],5,"%s",t[a].mg);
            unsigned long tot=0; int ns=0, bi=a; for(int b=0;b<nt;b++) if(!strcmp(t[b].mg,t[a].mg)){ tot+=t[b].n; ns++; if(t[b].n>t[bi].n) bi=b; }
            char ips[32]; struct in_addr ia; ia.s_addr=t[bi].ip; inet_ntop(AF_INET,&ia,ips,sizeof ips); int server=srv[0] && !strcmp(srv,ips);
            if(!server){ char*d=strchr(ips,'.'); if(d){ d=strchr(d+1,'.'); if(d) snprintf(d,sizeof ips-(size_t)(d-ips),".x.x"); } }
            char m[300]; int kk=snprintf(m,sizeof m,"  [voice-wire] %s: %lu datagrams in 30 s from %d source(s); top %s:%u %.0f%%%s\n",t[a].mg,tot,ns,ips,t[bi].port,tot?100.0*t[bi].n/tot:0,server?" = the joined server":"");
            (void)!write(2,m,kk); }
        nt=0; tl=now; }
    pthread_mutex_unlock(&mx); }
// PELOAD_VOICE_SRCSTAT=1 (diagnostic, aggregate only): where voice datagrams come from -- the source address of the
// recvfrom that delivered each packet (same thread, just before voice_recv). Every 30 s: packets, distinct sources, the
// top source and its share; an address other than the joined server's is masked (players' addresses are not logged).
static void st_voice_srcstat(void){
    static int on=-1; if(on<0) on=getenv("PELOAD_VOICE_SRCSTAT")?1:0; if(!on) return;
    static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; static struct { uint32_t ip; uint16_t port; unsigned long n; } t[64]; static int nt; static unsigned long tot; static double tl;
    pthread_mutex_lock(&mx); tot++; int k=0; for(; k<nt; k++) if(t[k].ip==t_rx_ip && t[k].port==t_rx_port) break;
    if(k==nt && nt<64){ t[nt].ip=t_rx_ip; t[nt].port=t_rx_port; t[nt].n=0; nt++; } if(k<64) t[k].n++;
    double now=mono_s(); if(!tl) tl=now;
    if(now-tl>=30){ int bi=0; for(int q=1;q<nt;q++) if(t[q].n>t[bi].n) bi=q;
        char srv[80]=""; { const char*p=strstr(g_cmdlineA,"join?server="); if(p){ p+=12; size_t j2=0; while(*p && *p!=':' && *p!=' ' && *p!='"' && j2+1<sizeof srv) srv[j2++]=*p++; srv[j2]=0; } }
        char ips[32]="?"; struct in_addr a; a.s_addr=nt?t[bi].ip:0; inet_ntop(AF_INET,&a,ips,sizeof ips); int server=srv[0] && !strcmp(srv,ips);
        if(!server){ char*d=strchr(ips,'.'); if(d){ d=strchr(d+1,'.'); if(d) snprintf(d,sizeof ips-(size_t)(d-ips),".x.x"); } }
        char m[300]; int kk=snprintf(m,sizeof m,"  [voice] %lu packets in 30 s from %d source address(es); top %s:%u %.0f%%%s\n",tot,nt,ips,nt?t[bi].port:0,nt&&tot?100.0*t[bi].n/tot:0,server?" = the joined server (it relays voice)":"");
        (void)!write(2,m,kk); tot=0; nt=0; tl=now; }
    pthread_mutex_unlock(&mx); }
static void __attribute__((ms_abi)) st_on_voice(void*state,uint64_t sender,const uint8_t*payload,uint64_t len){ st_voice_srcstat(); (void)state; double now=mono_s(); int o=0;
    pthread_mutex_lock(&g_talk_mx); for(int i=0;i<128;i++){ if(g_talk[i].id==sender){ o=i; break; } if(g_talk[i].t<g_talk[o].t) o=i; }
    g_talk[o].id=sender; g_talk[o].t=now; pthread_mutex_unlock(&g_talk_mx);
    if(g_fx_on) fx_voice(sender,payload,len); }   // the forensic archive: every packet, before the client's mute / channel checks
static double st_talk_age(uint64_t id){ double t=0; pthread_mutex_lock(&g_talk_mx); for(int i=0;i<128;i++) if(g_talk[i].id==id){ t=g_talk[i].t; break; } pthread_mutex_unlock(&g_talk_mx);
    return t>0?mono_s()-t:1e9; }
// Chat (shim/docs/halo3-event-feed.md section 1): every line the client's text chat shows -- players' messages,
// [SERVER] lines, its own notices -- passes through one function before its chat panel draws it (exe+0xb89930 in
// 0.9.18, found by signature; the first 19 bytes are position-independent): push_history(state, Line*, mine,
// timestamp). It owns the line, so the hook only reads it. Line (0x40 bytes, Rust Strings = {cap, ptr, len}, UTF-8):
//   q0 >= 0: a player's message: +0x00 sender name, +0x18 text, +0x30 sender peer id, +0x3c channel (0 all, 1 team,
//            2 nearby);  q0 = 0x8000000000000001: a server line, String at +0x08;  q0 = 0x8000000000000000: a notice,
//            kind byte at +0x08. Chat never goes through halo3.
static int st_rstr(uint64_t at,char*out,size_t on){   // a Rust String {cap, ptr, len} at `at` -> NUL-terminated
    uint64_t f[3]; out[0]=0; if(!hb_read(at,f,sizeof f) || !f[1] || f[2]>4096) return 0;
    size_t n=f[2]<on-1?(size_t)f[2]:on-1; if(n && !hb_read(f[1],out,n)) return 0; out[n]=0;
    for(size_t i=0;i<n;i++) if((unsigned char)out[i]<0x20) out[i]=' '; return 1; }
static void __attribute__((ms_abi)) st_on_chat(void*state,const uint8_t*line,uint64_t mine){ (void)state;
    int64_t q0=0; if(!line || !hb_read((uint64_t)(uintptr_t)line,&q0,8)) return;
    char a[64], txt[600]; int fl=(mine&0xFF)?2:0;   // flags 2: we sent it
    if(q0>=0){ uint8_t ch=0; hb_read((uint64_t)(uintptr_t)line+0x3c,&ch,1);
        if(!st_rstr((uint64_t)(uintptr_t)line,a,sizeof a) || !st_rstr((uint64_t)(uintptr_t)line+0x18,txt,sizeof txt)) return;
        if(g_fx_on) hb_read((uint64_t)(uintptr_t)line+0x30,&t_st_chat_id,8);   // the sender's peer id, for the forensic archive
        st_event("chat",a,0,ch==1?"team":ch==2?"nearby":"",-1,-1,fl,"%s",txt); t_st_chat_id=0; }
    else if((uint64_t)q0==0x8000000000000001ull){ if(st_rstr((uint64_t)(uintptr_t)line+8,txt,sizeof txt)){ st_event("chat",0,0,"server",-1,-1,fl,"%s",txt);
            if(strstr(txt," called a vote to ")) st_av_called(); } }
    else if((uint64_t)q0==0x8000000000000000ull){ uint8_t k=0xFF; hb_read((uint64_t)(uintptr_t)line+8,&k,1);   // the client's own wording
        static const char*const nt[5]={"You are sending messages too quickly.","Team chat is only available in team games.",
            "Proximity chat is only available while alive in a game.","The host has turned text chat off.","Your message could not be delivered."};
        st_event("chat",0,0,"notice",-1,-1,fl,"%s",k<5?nt[k]:"(chat notice)"); } }
static void st_chat_install(void){
    uint8_t b[96],m[96]; int n=sig_parse("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 20 FE FF FF FF 80 B9 F8 00 00 00 00 0F 84",b,m,96);
    int64_t r=sig_find(g_base,b,m,n); int ok=r>=0 && entry_hook(g_base+r,b,19,(void*)st_on_chat);
    char msg[120]; int k=snprintf(msg,sizeof msg,"  [stats] chat: %s\n",ok?"hooked (event feed)":"unavailable (push_history not found)"); (void)!write(2,msg,k); }
// Runs at exe load (single-threaded, before the entry point), only with the stats page on.
// Player addresses (2026-10-18, backlog 49; static RE of the 0.9.18 client). A HOST -- a menu-hosted game or a dedicated
// server's worker -- keeps Mutex<Option<ModerationState>> for its kick / banip / RCON "players" commands: players Vec at L-8
// (ptr) / L (len) of 0x68-byte entries {+0 player ID String (64 hex), +0x18 nickname String, +0x48 u64 session peer id (=
// StSess.voice), +0x50 u32 join number, +0x54 IpAddr tag (0 V4, 1 V6), +0x55 octets (network order)}, kicked Vec at L+0x28 /
// L+0x30 of 0x30-byte entries (+0x28 peer id), Option None = 0x8000000000000000 at L-0xa0, the futex lock byte at L-0xa8.
// 0.9.18: L = exe+0x27cf790, accepted only when some `lock cmpxchg byte [rip+L-0xa8]` exists (other builds: off; the
// layout moved in 0.9.16 and before 0.9.2 there is no such table). Read without the lock: len/ptr, the entries, len/ptr
// again; kept only when unchanged and every entry looks right. A CLIENT knows no other player's address (the roster has
// none, voice is relayed): only the server's (= the host player's in a player-hosted game). Shown only while the stats page
// is on this Mac (PELOAD_STATS_ADDR loopback or unset) unless PELOAD_STATS_IPS=1; PELOAD_STATS_IPS=0 hides them always.
static uint8_t*g_st_modlen;
// the client's moderation table (bans + the players Vec with their addresses): its lock is the first `lock cmpxchg byte
// [rip+d]` before the code that formats "You are banned from this server. Reason: "; the players Vec {cap, ptr, len}
// ends at lock+0xa8 (L = its len). Same layout in v0.9.18 (lock exe+0x27cf6e8) and v0.9.20 (exe+0x29f50b8).
static void st_addr_install(void){
    if(!g_base) return; DOS*dh=(DOS*)g_base; OPT64*oh=(OPT64*)(g_base+dh->e_lfanew+sizeof(COFF)); uint32_t isz=oh->ImageSize;
    uint32_t pe=*(uint32_t*)(g_base+0x3c); uint16_t ns=*(uint16_t*)(g_base+pe+6), so=*(uint16_t*)(g_base+pe+20); uint8_t*st=g_base+pe+24+so;
    static const char needle[]="You are banned from this server. Reason: "; size_t nl=sizeof needle-1; uint8_t*str=0;
    for(int i=0;i<ns && !str;i++){ uint8_t*sh=st+i*40; uint32_t vs=*(uint32_t*)(sh+8), va=*(uint32_t*)(sh+12), ch=*(uint32_t*)(sh+36);
        if(ch&0x20000000) continue; for(uint32_t k=0;k+nl<=vs && va+k+nl<=isz;k++) if(g_base[va+k]=='Y' && !memcmp(g_base+va+k,needle,nl)){ str=g_base+va+k; break; } }
    uint8_t*lock=0;
    if(str) for(int i=0;i<ns && !lock;i++){ uint8_t*sh=st+i*40; uint32_t vs=*(uint32_t*)(sh+8), va=*(uint32_t*)(sh+12), ch=*(uint32_t*)(sh+36); if(!(ch&0x20000000)) continue;
        uint8_t*p=g_base+va; for(uint32_t k=0;k+7<=vs && !lock;k++){ if((p[k]!=0x48 && p[k]!=0x4C) || p[k+1]!=0x8D || (p[k+2]&0xC7)!=0x05) continue;
            uint8_t*t=p+k+7+*(int32_t*)(p+k+3); if(t<str-8 || t>str) continue;   // (the literal may start a byte or two earlier: Rust concatenates them)
            for(uint8_t*q=p+k; q>p+k-0x800 && q>p; q--) if(q[0]==0xF0 && q[1]==0x0F && q[2]==0xB0 && q[3]==0x0D && q[8]==0x75){ lock=q+8+*(int32_t*)(q+4); break; } } }
    if(lock && lock>g_base && lock+0xb0<g_base+isz) g_st_modlen=lock+0xa8;
    char m[160]; int k=snprintf(m,sizeof m,"  [stats] player addresses: %s\n",g_st_modlen?"host table found (shown while hosting)":"host table not found (another client build?): off"); (void)!write(2,m,k);
    if(g_st_modlen){ k=snprintf(m,sizeof m,"  [stats] player addresses: table lock exe+0x%llx\n",(unsigned long long)(lock-g_base)); (void)!write(2,m,k); } }
static int st_ips_ok(void){ static int v=-1; if(v<0){ const char*e=getenv("PELOAD_STATS_IPS"), *ad=getenv("PELOAD_STATS_ADDR");
        int local=!ad || !*ad || !strncmp(ad,"127.",4) || !strcmp(ad,"::1") || !strcmp(ad,"localhost"); v=e?atoi(e)!=0:local; } return v; }
typedef struct { uint64_t peer; char ip[48]; } StAddr;
static StAddr g_st_addr[256]; static int g_st_naddr, g_st_hosting;
static void st_addr_refresh(void){   // snapshot thread, once per snapshot
    g_st_hosting=0; if(!g_st_modlen || !st_ips_ok()) { g_st_naddr=0; return; }
    uint64_t opt=0, len=0, ptr=0, len2=0, ptr2=0, klen=0, kptr=0; uint8_t*L=g_st_modlen;
    if(!hb_read((uint64_t)(uintptr_t)(L-0xa0),&opt,8) || opt==0x8000000000000000ull){ g_st_naddr=0; return; }   // not hosting
    if(!hb_read((uint64_t)(uintptr_t)L,&len,8) || !hb_read((uint64_t)(uintptr_t)(L-8),&ptr,8) || len>256 || (len && !ptr)) return;
    hb_read((uint64_t)(uintptr_t)(L+0x30),&klen,8); hb_read((uint64_t)(uintptr_t)(L+0x28),&kptr,8); if(klen>256) klen=0;
    static uint8_t e[256*0x68], kk[256*0x30];
    if(len && !hb_read(ptr,e,(size_t)len*0x68)) return; if(klen && (!kptr || !hb_read(kptr,kk,(size_t)klen*0x30))) klen=0;
    if(!hb_read((uint64_t)(uintptr_t)L,&len2,8) || !hb_read((uint64_t)(uintptr_t)(L-8),&ptr2,8) || len2!=len || ptr2!=ptr) return;   // changed: next time
    StAddr out[256]; int n=0;
    for(uint64_t i=0;i<len;i++){ const uint8_t*x=e+i*0x68; uint8_t tag=x[0x54]; uint64_t peer=*(const uint64_t*)(x+0x48), idl=*(const uint64_t*)(x+0x10);
        if(tag>1 || idl!=64 || !peer) return;   // not what we expect: keep the last result
        int kicked=0; for(uint64_t q=0;q<klen;q++) if(*(const uint64_t*)(kk+q*0x30+0x28)==peer) kicked=1; if(kicked) continue;
        StAddr*a=&out[n++]; a->peer=peer;
        if(tag==0) inet_ntop(AF_INET,x+0x55,a->ip,sizeof a->ip);
        else { static const uint8_t v4m[12]={0,0,0,0,0,0,0,0,0,0,0xff,0xff}; if(!memcmp(x+0x55,v4m,12)) inet_ntop(AF_INET,x+0x55+12,a->ip,sizeof a->ip); else inet_ntop(AF_INET6,x+0x55,a->ip,sizeof a->ip); } }
    memcpy(g_st_addr,out,sizeof(StAddr)*(size_t)n); g_st_naddr=n; g_st_hosting=1; }
static const char* st_addr_of(uint64_t peer){ if(!peer) return 0; for(int i=0;i<g_st_naddr;i++) if(g_st_addr[i].peer==peer) return g_st_addr[i].ip; return 0; }
// Player slots (2026-10-18, backlog 53; static RE of releases 0.9.2 .. 0.9.18). A joined client keeps the server's
// max players from admission in Mutex<Option<Arc<HostedSession>>>: Arc pointer at L+8 (L = the futex lock byte), Arc+0x10
// = the private-join capability (first field 0x8000000000000000 = this process HOSTS instead), max players = u8 at
// Arc+off (0.9.18: slot exe+0x27dd668, off 0x22d; the server's own limit, the one in "This server is full (N players)").
// Found by the client's max_players() getter (signature below; its Arc clone function's lock gives L, its first
// `movzx esi,byte [rax+d32]` gives off). Hosting (or when that is not there): halo3's session parameter maximum player
// count, sess+0x419c when sess+0x4194 bit 0 (halo3 get_maximum_player_count halo3+0x11db4).
static uint8_t*g_st_hslot; static int g_st_mpoff;
static void st_slots_install(void){
    // the getter's prologue: v0.9.18 pushes rbp, rsi; v0.9.20 also rdi (the clone call moves one byte; max_players at
    // Arc+0x22d in 0.9.18, +0x233 in 0.9.20 -- both read from the getter itself)
    static const char*const sigs[2]={
        "55 56 48 81 EC ?? ?? 00 00 48 8D AC 24 80 00 00 00 48 C7 85 ?? ?? 00 00 FE FF FF FF E8 ?? ?? ?? ?? 48 85 C0 74 16 31 C9 48 3B 48 10 71 ?? F0 48 FF 08 75 08 48 89 C1 E8 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ?? B0 10",
        "55 56 57 48 81 EC ?? ?? 00 00 48 8D AC 24 80 00 00 00 48 C7 85 ?? ?? 00 00 FE FF FF FF E8 ?? ?? ?? ?? 48 85 C0 74 16 31 C9 48 3B 48 10 71 ?? F0 48 FF 08 75 08 48 89 C1 E8 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ??" };
    uint8_t b[128],m[128]; int64_t r=-1; for(int v=0;v<2 && r<0;v++){ int n=sig_parse(sigs[v],b,m,128); r=sig_find(g_base,b,m,n); }
    const char*why="getter not found (another client build?)";
    if(r>=0){ uint8_t*G=g_base+r, *C=0; static const uint8_t pro[13]={0x56,0x48,0x83,0xEC,0x20,0xB1,0x01,0x31,0xC0,0xF0,0x0F,0xB0,0x0D};
        for(uint8_t*q=G;q<G+0x30 && !C;q++) if(q[0]==0xE8 && q[5]==0x48 && q[6]==0x85 && q[7]==0xC0) C=q+5+*(int32_t*)(q+1);   // call clone; test rax,rax
        if(!C || memcmp(C,pro,13)) why="clone function has an unexpected shape";
        else { uint8_t*L=C+0x11+*(int32_t*)(C+0xd); int off=-1;
            for(uint8_t*q=G;q<G+0x100;q++) if(q[0]==0x0F && q[1]==0xB6 && q[2]==0xB0 && q[7]==0xF0 && q[8]==0x48 && q[9]==0xFF && q[10]==0x08){ off=*(int32_t*)(q+3); break; }
            if(off>0x18 && off<0x1000){ g_st_hslot=L+8; g_st_mpoff=off; why=0; } else why="max_players offset not found"; } }
    char msg[160]; int k=why?snprintf(msg,sizeof msg,"  [stats] player slots: %s -- the session's limit only\n",why)
        :snprintf(msg,sizeof msg,"  [stats] player slots: server max at exe+0x%llx +0x%x\n",(unsigned long long)(g_st_hslot-g_base),g_st_mpoff); (void)!write(2,msg,k); }
static int st_max_players(const char**src){   // the server's max players, 0 = not known
    *src=0; int hosting=0, ingame=!g_st_hslot;   // (without the slot global: the session's limit, whatever the state)
    if(g_st_hslot){ uint64_t a=0,a2=0,strong=0,first=0; uint8_t v=0; ingame=0;
        if(hb_read((uint64_t)(uintptr_t)g_st_hslot,&a,8) && a && hb_read(a,&strong,8) && strong>=1 && strong<100000 && hb_read(a+0x10,&first,8)){
            ingame=1; if(first==0x8000000000000000ull) hosting=1;
            else if(hb_read(a+(uint64_t)g_st_mpoff,&v,1) && hb_read((uint64_t)(uintptr_t)g_st_hslot,&a2,8) && a2==a && v>=1 && v<=128){ *src="server"; return v; } } }
    if(!ingame) return 0;   // the menu's own local session (limit 16) is not a game
    uint8_t*h3=hb_halo3_base(); uint64_t sess=0; uint32_t st=0, fl=0; int32_t mx=0;
    if(h3 && hb_read((uint64_t)(h3+0x1e250d0),&sess,8) && sess && hb_read(sess+0x67b70,&st,4) && st>=4 && st<=9 && hb_read(sess+0x4194,&fl,4) && (fl&1)
       && hb_read(sess+0x419c,&mx,4) && mx>=1 && mx<=128){ *src=hosting?"host":"session"; return mx; }
    return 0; }
static void st_voice_install(void){ if(!stats_on() || !g_base) return;
    uint8_t b[96],m[96]; int n; int64_t r; char msg[220]; int k;
    n=sig_parse("55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 89 CE B1 01 31 C0 F0 0F B0 0D ?? ?? ?? ?? 0F 85",b,m,96);
    r=sig_find(g_base,b,m,n); if(r>=0){ g_st_setmuted=g_base+r; g_st_mutedvec=(uint64_t)(g_st_setmuted+0x24+*(int32_t*)(g_st_setmuted+0x20)); }
    // a unique match can be the wrong function: in clients 0.8.16 .. 0.9.1 this shape is a Forge Mutex<Vec> setter
    // (shim/docs/patch-addresses.md). The real set_muted's lock byte is also taken by is_muted
    // (56 48 83 EC 20 B2 01 31 C0 F0 0F B0 15 <disp32>): no such function -> no mute.
    if(g_st_setmuted){ DOS*dh=(DOS*)g_base; OPT64*oh=(OPT64*)(g_base+dh->e_lfanew+sizeof(COFF)); uint8_t*lockb=g_st_setmuted+0x24+*(int32_t*)(g_st_setmuted+0x20);
        static const uint8_t im[13]={0x56,0x48,0x83,0xEC,0x20,0xB2,0x01,0x31,0xC0,0xF0,0x0F,0xB0,0x15}; int paired=0;
        for(uint8_t*q=g_base+0x1000; q+17<g_base+oh->ImageSize && !paired; q++) if(q[0]==0x56 && !memcmp(q,im,13) && q+17+*(int32_t*)(q+13)==lockb) paired=1;
        if(!paired){ g_st_setmuted=0; g_st_mutedvec=0; ws("  [stats] voice: the set_muted match has no is_muted partner (another client build?): mute off\n"); } }
    n=sig_parse("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 80 00 00 00 48 C7 45 78 FE FF FF FF 4D 89 CE 4C 89 C7 49 89 D7 48 89 CB",b,m,96);
    int64_t rv=sig_find(g_base,b,m,n); int hooked=rv>=0 && entry_hook(g_base+rv,b,19,(void*)st_on_voice);
    k=snprintf(msg,sizeof msg,"  [stats] voice: mute %s, talking indicator %s\n",g_st_setmuted?"available":"unavailable (set_muted not found)",hooked?"hooked":"unavailable (voice_recv not found)");
    (void)!write(2,msg,k); st_chat_install(); st_addr_install(); st_slots_install(); st_bv_install(); }
static void st_voice_init(void){ static int done; if(done || !g_base) return; done=1;
    // 0.9.18: set_muted at its known place -> the client's expanded-session flag is at its known place too; other
    // builds pick the layout by which one holds valid records
    if(g_st_setmuted==g_base+0x66bea0){ uint8_t x=0; if(hb_read((uint64_t)(g_base+0x27f7210),&x,1) && x<=1) g_st_layout=x; } }
static int st_session(StSess*out,int max,uint32_t*state){
    uint8_t*h3=hb_halo3_base(); if(!h3) return 0; st_voice_init();
    uint64_t sess=0; if(!hb_read((uint64_t)(h3+0x1e250d0),&sess,8) || !sess) return 0;
    uint32_t st=0; hb_read(sess+0x67b70,&st,4); if(state) *state=st;
    // host / local peer (reclaimer-voice-mute.md "Host peer"): valid once membership exists (update number != -1) and
    // the session is up (state 4..9); a dedicated server is host peer 0 with no player of its own
    { int32_t upd=-1, hp=-1, lp=-1; if(hb_read(sess+0x38,&upd,4) && upd!=-1 && st>=4 && st<=9 && hb_read(sess+0x40,&hp,4) && hb_read(sess+0x3dc4,&lp,4)){ g_st_hostpeer=hp; g_st_localpeer=lp; }
      else { g_st_hostpeer=-1; g_st_localpeer=-1; } }
    if(g_st_setmuted==g_base+0x66bea0){ uint8_t x=0; if(hb_read((uint64_t)(g_base+0x27f7210),&x,1) && x<=1) g_st_layout=x; }   // set once Reclaimer patches halo3
    uint64_t muted[256]; int nm=0; { uint64_t ptr=0, len=0; if(g_st_mutedvec && hb_read(g_st_mutedvec+0x10,&ptr,8) && hb_read(g_st_mutedvec+0x18,&len,8) && len<=256 && ptr>1 && len) if(hb_read(ptr,muted,len*8)) nm=(int)len; }
    int best=0;
    for(int lay=1;lay>=0;lay--){ if(g_st_layout>=0 && lay!=g_st_layout) continue;
        uint8_t pm[16]={0}, qm[16]={0}; if(!hb_read(sess+0x126c,pm,lay?16:4) || !hb_read(sess+0x54,qm,lay?16:4)) continue;
        int np=lay?128:16, nq=lay?128:17; uint64_t pbase=sess+(lay?0x1493c0:0x1270), qbase=sess+(lay?0x1381c0:0x58); int n=0;
        for(int i=0;i<np && n<max;i++){ if(!((pm[i>>3]>>(i&7))&1)) continue; uint8_t r[0xc8]; if(!hb_read(pbase+(uint64_t)i*0xc8,r,sizeof r) || !*(uint64_t*)(r+4)) continue;
            int pi=*(int32_t*)(r+0xc); if(pi<0 || pi>=nq || !((qm[pi>>3]>>(pi&7))&1)) continue;
            uint8_t q[0x10]; if(!hb_read(qbase+(uint64_t)pi*0x110,q,sizeof q)) continue;
            StSess*e=&out[n]; e->idx=i; e->peer=pi; e->voice=*(uint64_t*)q; e->muted=0; e->host=g_st_hostpeer>=0 && pi==g_st_hostpeer; for(int k=0;k<nm;k++) if(muted[k]==e->voice){ e->muted=1; break; }
            int j=0, ok=1; const uint16_t*w=(const uint16_t*)(r+0x98);
            for(int c=0;c<16 && w[c] && j<60;c++){ uint16_t u=w[c]; if(u<0x20){ ok=0; break; }
                if(u<0x80) e->name[j++]=(char)u; else if(u<0x800){ e->name[j++]=(char)(0xC0|(u>>6)); e->name[j++]=(char)(0x80|(u&63)); }
                else { e->name[j++]=(char)(0xE0|(u>>12)); e->name[j++]=(char)(0x80|((u>>6)&63)); e->name[j++]=(char)(0x80|(u&63)); } }
            if(!ok || !j) continue; e->name[j]=0; n++; }
        if(n){ if(g_st_layout<0) g_st_layout=lay; return n; } best=n; }
    return best; }
// mute requests from the page, run on the halo3 main loop (hb_frame_hook)
// players in the network session (d3dsubmit.c's auto mode switches the submission thread by it)
int st_player_count(void){ static StSess ss[128]; return st_session(ss,128,0); }

static pthread_mutex_t g_vq_mx=PTHREAD_MUTEX_INITIALIZER; static struct { uint64_t id; uint8_t on; } g_vq[256]; static int g_vqn; volatile int g_st_voice_pending;
static void st_voice_jobs(void){
    uint64_t id[256]; uint8_t on[256]; int n;
    pthread_mutex_lock(&g_vq_mx); n=g_vqn; for(int i=0;i<n;i++){ id[i]=g_vq[i].id; on[i]=g_vq[i].on; } g_vqn=0; __atomic_store_n(&g_st_voice_pending,0,__ATOMIC_RELEASE); pthread_mutex_unlock(&g_vq_mx);
    for(int i=0;i<n && g_st_setmuted;i++){ ((void(__attribute__((ms_abi))*)(uint64_t,uint8_t))g_st_setmuted)(id[i],on[i]);
        char m[96]; int k=snprintf(m,sizeof m,"  [stats] voice %016llx %s\n",(unsigned long long)id[i],on[i]?"muted":"unmuted"); (void)!write(2,m,k); } }
static int st_voice_request(uint64_t id,int on){ if(!g_st_setmuted || !id) return 0;
    pthread_mutex_lock(&g_vq_mx); int ok=g_vqn<256; if(ok){ g_vq[g_vqn].id=id; g_vq[g_vqn].on=(uint8_t)!!on; g_vqn++; __atomic_store_n(&g_st_voice_pending,1,__ATOMIC_RELEASE); } pthread_mutex_unlock(&g_vq_mx); return ok; }

// ---- death heatmap + footprint, per map, kept across matches -------------------------------------------------------
// footprint: how often a live player stood in each 0.5-unit cell (it draws the map's playable layout over time);
// deaths: where each player died. Saved to ~/Library/Application Support/ReclaimerMac/heatmaps/<map>.bin every 30 s.
#define HM_CELL 0.5f
typedef struct { int32_t x,y; uint32_t n; } HmCell;
typedef struct { float x,y,z; int32_t team, slot; char name[48]; uint32_t match; float t; } HmDeath;
static struct { char map[64]; HmCell*cells; uint32_t cap, used; HmDeath*d; uint32_t nd, dcap, match, maxmatch; int dirty; double saved, match_t0; } g_hm;
static pthread_mutex_t g_hm_mx=PTHREAD_MUTEX_INITIALIZER;
static void hm_path(const char*map,char*out,size_t n){ const char*h=getenv("HOME"); snprintf(out,n,"%s/Library/Application Support/ReclaimerMac/heatmaps/%s.bin",h?h:"/tmp",map); }
static void hm_cell_add(int32_t x,int32_t y,uint32_t n){
    if(g_hm.used*10>=g_hm.cap*7){ uint32_t oc=g_hm.cap, nc=oc?oc*2:65536; HmCell*nt=calloc(nc,sizeof *nt), *ot=g_hm.cells; if(!nt) return;
        g_hm.cells=nt; g_hm.cap=nc; g_hm.used=0; for(uint32_t i=0;i<oc;i++) if(ot[i].n) hm_cell_add(ot[i].x,ot[i].y,ot[i].n); free(ot); }
    uint32_t m=g_hm.cap-1, i=((uint32_t)x*73856093u ^ (uint32_t)y*19349663u)&m;
    for(;;i=(i+1)&m){ HmCell*c=&g_hm.cells[i]; if(!c->n){ c->x=x; c->y=y; c->n=n; g_hm.used++; return; } if(c->x==x && c->y==y){ c->n+=n; return; } } }
static void hm_save(void){ if(!g_hm.map[0] || !g_hm.dirty) return; char p[1024]; hm_path(g_hm.map,p,sizeof p);
    char dir[1024]; snprintf(dir,sizeof dir,"%s",p); char*sl=strrchr(dir,'/'); if(sl){ *sl=0; mkdir(dir,0755); }
    char tmp[1100]; snprintf(tmp,sizeof tmp,"%s.tmp",p); FILE*f=fopen(tmp,"wb"); if(!f) return;
    float cell=HM_CELL; fwrite("RHM2",1,4,f); fwrite(&cell,4,1,f); fwrite(&g_hm.used,4,1,f); fwrite(&g_hm.nd,4,1,f); fwrite(&g_hm.maxmatch,4,1,f);
    for(uint32_t i=0;i<g_hm.cap;i++) if(g_hm.cells[i].n) fwrite(&g_hm.cells[i],sizeof(HmCell),1,f);
    fwrite(g_hm.d,sizeof(HmDeath),g_hm.nd,f); int ok=!ferror(f); fclose(f); if(ok) rename(tmp,p); else unlink(tmp); g_hm.dirty=0; }
static void hm_switch(const char*map){   // (g_hm_mx held)
    if(!strcmp(map,g_hm.map)) return; hm_save();
    free(g_hm.cells); free(g_hm.d); memset(&g_hm,0,sizeof g_hm); snprintf(g_hm.map,sizeof g_hm.map,"%s",map); if(!map[0]) return;
    char p[1024]; hm_path(map,p,sizeof p); FILE*f=fopen(p,"rb"); uint32_t nc=0,nd=0;
    if(f){ char mg[4]; float cell=0; if(fread(mg,1,4,f)==4 && !memcmp(mg,"RHM2",4) && fread(&cell,4,1,f)==1 && cell==HM_CELL && fread(&nc,4,1,f)==1 && fread(&nd,4,1,f)==1 && fread(&g_hm.maxmatch,4,1,f)==1 && nc<(1u<<24) && nd<(1u<<22)){
            for(uint32_t i=0;i<nc;i++){ HmCell c; if(fread(&c,sizeof c,1,f)!=1) break; hm_cell_add(c.x,c.y,c.n); }
            g_hm.d=malloc((nd?nd:1)*sizeof(HmDeath)); g_hm.dcap=nd?nd:1; g_hm.nd=g_hm.d?(uint32_t)fread(g_hm.d,sizeof(HmDeath),nd,f):0; }
        fclose(f); }
    g_hm.match=++g_hm.maxmatch; g_hm.match_t0=mono_s(); g_hm.saved=mono_s(); g_hm.dirty=0;
    char m[1200]; int k=snprintf(m,sizeof m,"  [stats] heatmap %s: %u cells, %u deaths from earlier matches\n",map,g_hm.used,g_hm.nd); (void)!write(2,m,k); }
// the map being played: switched to when there is something to record (menus open campaign maps for scenery)
static char g_hm_cur[64];
static void hm_death(const float*pos,int team,int slot,const char*name){   // (g_hm_mx held)
    hm_switch(g_hm_cur);
    if(g_hm.nd>=g_hm.dcap){ uint32_t nc=g_hm.dcap?g_hm.dcap*2:256; HmDeath*nd=realloc(g_hm.d,nc*sizeof *nd); if(!nd) return; g_hm.d=nd; g_hm.dcap=nc; }
    HmDeath*d=&g_hm.d[g_hm.nd++]; memset(d,0,sizeof *d); d->x=pos[0]; d->y=pos[1]; d->z=pos[2]; d->team=team; d->slot=slot; snprintf(d->name,sizeof d->name,"%s",name?name:"");
    d->match=g_hm.match; d->t=(float)(mono_s()-g_hm.match_t0); g_hm.dirty=1; }
// ---- the top-down map picture behind the heatmap: made automatically for every map played (stock or Workshop) --------
// The first time a map without a picture is played, re-tools/h3bsp.py runs in the background (native python3 with numpy
// and Pillow) on the very .map file the game opened and writes ~/Library/Application Support/ReclaimerMac/maptop/<map>.png
// + .json (+ .z.bin); the page keeps asking until it is there. PELOAD_MAPTOP_TOOL = the h3bsp.py to run (default: the
// re-tools/ next to shim/build/, as in a checkout and in the release bundle); PELOAD_NO_MAPTOP=1 off. Log: <map>.log.
static volatile int g_mt_state;   // 0 none/unknown, 1 ready, 2 making, 3 failed, 4 no tool
static char g_mt_name[96], g_mt_png[1100], g_mt_js[1100];
static void* st_maptop_wait(void*a){ pid_t pid=(pid_t)(intptr_t)a; int st=0; while(waitpid(pid,&st,0)<0 && errno==EINTR){}
    g_mt_state=(!access(g_mt_png,R_OK) && !access(g_mt_js,R_OK))?1:3;   // judged by the files (another waiter may reap the child)
    char m[200]; int k=snprintf(m,sizeof m,"  [stats] map picture for %s: %s\n",g_mt_name,g_mt_state==1?"made":"FAILED (see its .log in maptop/)"); (void)!write(2,m,k); return 0; }
static void* st_reap_child(void*a){ int st=0; waitpid((pid_t)(intptr_t)a,&st,0); return 0; }   // (no zombie)
static void st_maptop_ensure(const char*mp,const char*name){
    static char last[96]; if(!name[0] || !mp[0] || !strcmp(last,name)) return; snprintf(last,sizeof last,"%s",name);
    snprintf(g_mt_name,sizeof g_mt_name,"%s",name);
    if(name[0]>='0'&&name[0]<='9') { g_mt_state=0; return; }                       // campaign levels, the menu: no picture
    if(!strcmp(name,"mainmenu")||!strcmp(name,"shared")||!strcmp(name,"campaign")){ g_mt_state=0; return; }
    const char*home=getenv("HOME"); char dir[1024], png[1100], js[1100], lg[1100];
    snprintf(dir,sizeof dir,"%s/Library/Application Support/ReclaimerMac/maptop",home?home:"/tmp");
    snprintf(png,sizeof png,"%s/%s.png",dir,name); snprintf(js,sizeof js,"%s/%s.json",dir,name); snprintf(lg,sizeof lg,"%s/%s.log",dir,name);
    snprintf(g_mt_png,sizeof g_mt_png,"%s",png); snprintf(g_mt_js,sizeof g_mt_js,"%s",js);
    // invisible walls for the zones overlay (re-tools/h3inv.py -> <map>.inv.bin, ~0.3 s; read by hitbox.c hbz_load the next
    // time the map is loaded there)
    { char inv[1100]; snprintf(inv,sizeof inv,"%s/%s.inv.bin",dir,name);
      if(access(inv,R_OK) && !getenv("PELOAD_NO_MAPTOP")){ char exe[1024], it[1100]; uint32_t es=sizeof exe; it[0]=0;
          if(!_NSGetExecutablePath(exe,&es)){ char*sl=strrchr(exe,'/'); if(sl){ *sl=0; snprintf(it,sizeof it,"%s/../../re-tools/h3inv.py",exe); } }
          if(it[0] && !access(it,R_OK)){ mkdir(dir,0755); char*av[]={(char*)"/usr/bin/arch",(char*)"-arm64",(char*)"python3",it,(char*)mp,inv,0}; extern char**environ;
              posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_addopen(&fa,0,"/dev/null",O_RDONLY,0);
              posix_spawn_file_actions_addopen(&fa,1,"/dev/null",O_WRONLY,0); posix_spawn_file_actions_adddup2(&fa,1,2);
              posix_spawnattr_t at; posix_spawnattr_init(&at); posix_spawnattr_setflags(&at,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_CLOEXEC_DEFAULT); posix_spawnattr_setpgroup(&at,0);
              pid_t pid=0; if(!posix_spawn(&pid,"/usr/bin/arch",&fa,&at,av,environ)){ pthread_t th; if(!pthread_create(&th,0,st_reap_child,(void*)(intptr_t)pid)) pthread_detach(th); }
              posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at); } } }
    if(!access(png,R_OK) && !access(js,R_OK)){ g_mt_state=1; return; }
    if(getenv("PELOAD_NO_MAPTOP")){ g_mt_state=4; return; }
    char tool[1100]; const char*e=getenv("PELOAD_MAPTOP_TOOL");
    if(e&&*e) snprintf(tool,sizeof tool,"%s",e);
    else { char exe[1024]; uint32_t es=sizeof exe; tool[0]=0; if(!_NSGetExecutablePath(exe,&es)){ char*sl=strrchr(exe,'/'); if(sl){ *sl=0; snprintf(tool,sizeof tool,"%s/../../re-tools/h3bsp.py",exe); } } }
    if(!tool[0] || access(tool,R_OK)){ g_mt_state=4; char m[1200]; int k=snprintf(m,sizeof m,"  [stats] no map picture for %s: %s not found\n",name,tool[0]?tool:"h3bsp.py"); (void)!write(2,m,k); return; }
    mkdir(dir,0755);
    // native python3 (the loader runs under Rosetta: a plain spawn would pick python's x86_64 slice, without numpy)
    char*argv[]={(char*)"/usr/bin/arch",(char*)"-arm64",(char*)"python3",tool,(char*)mp,png,(char*)"--size",(char*)"1024",0}; extern char**environ;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_addopen(&fa,0,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&fa,1,lg,O_WRONLY|O_CREAT|O_TRUNC,0644); posix_spawn_file_actions_adddup2(&fa,1,2);
    posix_spawnattr_t at; posix_spawnattr_init(&at); posix_spawnattr_setflags(&at,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_CLOEXEC_DEFAULT); posix_spawnattr_setpgroup(&at,0);   // only its stdio
    pid_t pid=0; int r=posix_spawn(&pid,"/usr/bin/arch",&fa,&at,argv,environ); posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if(r){ g_mt_state=3; return; }
    g_mt_state=2; { char m[1200]; int k=snprintf(m,sizeof m,"  [stats] making the map picture for %s (%s)\n",name,mp); (void)!write(2,m,k); }
    pthread_t th; pthread_create(&th,0,st_maptop_wait,(void*)(intptr_t)pid); pthread_detach(th); }
static void hm_json(StBuf*s){ pthread_mutex_lock(&g_hm_mx);
    { static const char*const mts[5]={"none","ready","making","failed","no tool"}; int st=g_mt_state; sb_put(s,"{\"maptop\":\"%s\",",strcmp(g_mt_name,g_hm.map)?"none":mts[st<0||st>4?0:st]); }
    sb_put(s,"\"map\":"); sb_str(s,g_hm.map); sb_put(s,",\"cell\":%.2f,\"match\":%u,\"cells\":[",HM_CELL,g_hm.match);
    int first=1; for(uint32_t i=0;i<g_hm.cap;i++) if(g_hm.cells[i].n){ sb_put(s,"%s%d,%d,%u",first?"":",",g_hm.cells[i].x,g_hm.cells[i].y,g_hm.cells[i].n); first=0; }
    sb_put(s,"],\"deaths\":["); for(uint32_t i=0;i<g_hm.nd;i++){ HmDeath*d=&g_hm.d[i]; sb_put(s,"%s[%.2f,%.2f,%.2f,%d,",i?",":"",d->x,d->y,d->z,d->team); sb_str(s,d->name); sb_put(s,",%u,%.0f,%d]",d->match,d->t,d->slot); }
    sb_put(s,"]}"); pthread_mutex_unlock(&g_hm_mx); }

// ---- per-player tracking: deaths, lives, distance, top speed, projectiles fired ----------------------------------
typedef struct { char name[64]; int slot, team, deaths, shots, hits, alive; uint32_t unit; double life_start, longest, dist; float pos[3], top_speed; } StTrack;
static StTrack g_trk[256]; static int g_ntrk; static char g_trk_map[64];
// keyed by player slot + name (names repeat: everyone who never set one is "Player")
static StTrack* st_track(const StPlayer*p){ for(int i=0;i<g_ntrk;i++) if(g_trk[i].slot==p->slot && !strcmp(g_trk[i].name,p->name)) return &g_trk[i];
    if(g_ntrk>=256) return 0; StTrack*t=&g_trk[g_ntrk++]; memset(t,0,sizeof *t); snprintf(t->name,sizeof t->name,"%s",p->name); t->slot=p->slot; t->team=-1; t->unit=0xFFFFFFFFu; return t; }
// bipeds seen last snapshot (death = a biped going from alive to dead), projectiles already counted
typedef struct { uint32_t h; int alive, ti; } StBip;
static StBip g_bip[2048]; static int g_nbip;
static struct { uint32_t h; double t; } g_proj[8192];

// Page controls (POST /control?do=<action>): the same switches as the function keys -- hitboxes (F2), tracers (F3),
// info (F4), items (F5), teams (F6: all -> enemies -> friends), teleport (F7, only with PELOAD_ALLOW_TELEPORT) and
// capture (F8). A POST with a custom header, which another web site's page cannot send to this port without a CORS
// preflight we never answer. PELOAD_STATS_READONLY=1 turns the controls off.
// this process hosts the game (halo3 game_globals +0x11: 5 = network host, 1 = local): only the host can teleport
static int st_is_host(void){ uint64_t gg=0; uint8_t st=0; return g_st_tls && hb_read(g_st_tls+0x48,&gg,8) && gg && hb_read(gg+0x11,&st,1) && (st==5||st==1); }
static int st_controls_ok(void){ static int ro=-1; if(ro<0){ const char*e=getenv("PELOAD_STATS_READONLY"); ro=e&&*e&&strcmp(e,"0")?1:0; } return !ro; }
// Host controls from a client of OUR host (PELOAD_TELEPORT_HOST = the host loader's stats address: our local dedicated server
// or a friend's game where both ends run this loader; never a public server, whose host has no such page): the request
// goes to the host's /control and the host does it. POST /control?<qs>; 1 = the host said ok.
static int st_forward(const char*hostport,const char*qs){
    char host[64]; int port=8765; if(sscanf(hostport,"%63[^:]:%d",host,&port)<1) return 0;
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return 0; struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons((uint16_t)port); a.sin_addr.s_addr=inet_addr(host);
    struct timeval tv={2,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv); setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    int ok=0; if(!connect(fd,(struct sockaddr*)&a,sizeof a)){ char rq[512]; int k=snprintf(rq,sizeof rq,"POST /control?%s HTTP/1.1\r\nHost: %s\r\nX-Reclaimer-Control: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",qs,hostport);
        if(k>0 && k<(int)sizeof rq && send(fd,rq,(size_t)k,0)==k){ char rs[256]; ssize_t n=recv(fd,rs,sizeof rs-1,0); if(n>0){ rs[n]=0; ok=strstr(rs,"\"ok\":true")!=0; } } }
    close(fd); char m[300]; int k=snprintf(m,sizeof m,"  [host] forwarded to the host %s: %s -> %s\n",hostport,qs,ok?"done":"refused / unreachable"); (void)!write(2,m,(size_t)k); return ok; }
static int st_forward_tp(const char*hostport,int slot,int who){ char q[96]; snprintf(q,sizeof q,"do=tpnext&slot=%d&who=%d",slot,who); return st_forward(hostport,q); }
static int g_st_fwd_speed=-1;   // the speed index last set on our host through it (-1: not since we started)
// Spots past the map's edges, tested alive with the boundaries off (shim/docs/map-boundaries.md)
typedef struct { const char*map, *id, *label; float x,y,z; } StSpot;
static const StSpot g_st_spots[]={
    {"riverworld","valhalla_cliff","Valhalla: east cliff",114.0f,-100.0f,22.6f},   // outside the safe zone (x > 106.3); ground 22.25: SLIP
                                                                                     //   terrain, walkable only with the slip patch (boundaries off)
    {"riverworld","valhalla_north","Valhalla: north ridge",44.0f,-24.0f,10.0f},    // past the north end (y > -31.9); ground 9.4, no soft ceiling
};
int hb_bounds_off(void); int hb_spar_available(void);
// ---- clip recording (rec.m; shim/docs/recording.md): POST /control?do=rec&secs=<n>&mode=av|audio, do=recstop; GET
// /recordings.json, /recordings/<file>; the snapshot's "rec". The picture is the window's (its size now); no window
// (PELOAD_HIDE_WINDOW) = sound only. The file is named after the map being played.
static int st_rec_start(const char*qs){
    const char*v=strstr(qs,"secs="); double secs=v?atof(v+5):30; const char*m=strstr(qs,"mode="); int ao=m && !strncmp(m+5,"audio",5);
    int r[4]={0,0,0,0}; if(!ao && !win_hidden()) rec_pic_rect(r);
    char map[64]; snprintf(map,sizeof map,"%s",g_st_curmap[0]?g_st_curmap:"game"); char err[200];
    return rec_start(ao,secs,map,r[2],r[3],err,sizeof err); }
// ---- the overlay panel (user, 2026-10-18): per-feature settings, hitbox.c g_hbs / hb_feat_set ------------------------
// POST /control?do=feat&f=<feature>[,<feature>...] (f= may repeat; f=all = every one) with any of on=0|1,
// team=all|enemies|friends, vis=0|1, px=4..60 -- each applied where the feature has that setting (the rest skip it).
static const char*const g_st_featname[HBF_N]={"hitboxes","tracers","info","items",0,"zones","vehicles"};   // (HBF_VISIBLE: a global)
static int st_feat(const char*qs){
    char b[160]; snprintf(b,sizeof b,"%s",qs); unsigned mask=0; int on=-1, team=-1, vis=-1, px=-1;
    for(char*p=b; (p=strcasestr(p,"%2c")); ) { *p=','; memmove(p+1,p+3,strlen(p+3)+1); }   // an encoded comma list
    for(char*sv=0,*t=strtok_r(b,"&",&sv); t; t=strtok_r(0,"&",&sv)){ char*v=strchr(t,'='); if(!v) continue; *v++=0;
        if(!strcmp(t,"f")){ for(char*s2=0,*w=strtok_r(v,",",&s2); w; w=strtok_r(0,",",&s2)){ int f=-1;
                for(int q=0;q<HBF_N;q++) if(g_st_featname[q] && !strcmp(w,g_st_featname[q])) f=q;
                if(!strcmp(w,"all")) mask|=((1u<<HBF_N)-1)&~(1u<<HBF_VISIBLE); else if(f<0) return 0; else mask|=1u<<f; } }
        else if(!strcmp(t,"on") || !strcmp(t,"vis")){ if(strcmp(v,"0") && strcmp(v,"1")) return 0; if(t[0]=='o') on=v[0]-'0'; else vis=v[0]-'0'; }
        else if(!strcmp(t,"team")){ team=!strcmp(v,"all")?0:!strcmp(v,"enemies")?1:!strcmp(v,"friends")?2:-2; if(team<0) return 0; }
        else if(!strcmp(t,"px")){ char*e; long x=strtol(v,&e,10); if(*e || x<4 || x>60) return 0; px=(int)x; } }
    if(!mask || (on<0 && team<0 && vis<0 && px<0)) return 0;
    int n=hb_feat_set(mask,on,team,vis,px);
    static const char*const tn[3]={"all","enemies","friends"}; char m[240]; int k=snprintf(m,sizeof m,"  [stats] page: overlay");   // (fits: <= ~130 chars)
    for(int f=0;f<HBF_N;f++) if((mask>>f)&1) k+=snprintf(m+k,sizeof m-k," %s",g_st_featname[f]);
    k+=snprintf(m+k,sizeof m-k,":"); if(on>=0) k+=snprintf(m+k,sizeof m-k," %s",on?"on":"off"); if(team>=0) k+=snprintf(m+k,sizeof m-k," show %s",tn[team]);
    if(vis>=0) k+=snprintf(m+k,sizeof m-k," %s",vis?"visible-only":"through-walls"); if(px>0) k+=snprintf(m+k,sizeof m-k," text %d px",px);
    k+=snprintf(m+k,sizeof m-k,"\n"); (void)!write(2,m,(size_t)k); return n>0; }
// the snapshot's overlay.features: {"hitboxes":{"on":..,"team":"all|enemies|friends","vis":..,"px":..},...}; null = that
// setting doesn't apply to the feature
static void st_overlay_features(StBuf*s){
    static const int ord[6]={HBF_HITBOX,HBF_VEH,HBF_TRACER,HBF_INFO,HBF_ITEMS,HBF_ZONES}; static const char*const tm[3]={"\"all\"","\"enemies\"","\"friends\""};
    hbs_sync(); sb_put(s,",\"features\":{");
    for(int i=0;i<6;i++){ int f=ord[i]; HbSet h=g_hbs[f]; char px[16]; snprintf(px,sizeof px,"%d",h.px);
        sb_put(s,"%s\"%s\":{\"on\":%s,\"team\":%s,\"vis\":%s,\"px\":%s}",i?",":"",g_st_featname[f],hb_feat(f)?"true":"false",
            h.team>=0&&h.team<3?tm[h.team]:"null",h.vis<0?"null":h.vis?"true":"false",h.px>0?px:"null"); }
    sb_put(s,"}"); }
static int st_control(const char*act,const char*qs){
    char m[96]; int k=0;
    static const char*const feat[HBF_N]={"hitboxes","tracers","info","items","visible","zones","vehicles"};
    for(int f=0;f<HBF_N;f++) if(!strcmp(act,feat[f])){ __atomic_fetch_xor(&g_hb_toggle,1<<f,__ATOMIC_ACQ_REL); hbs_sync();   // (visible: applies to all)
        k=snprintf(m,sizeof m,"  [stats] page: %s %s\n",feat[f],hb_feat(f)?"on":"off"); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"feat")) return st_feat(qs);   // the overlay panel: per-feature settings
    if(!strcmp(act,"weaktype")){   // one kind of vehicle weak spot on/off: t = 1 lethal, 2 opens the driver, 4 disables (gun / steering)
        const char*v=strstr(qs,"t="); int b=v?atoi(v+2):0; if(b!=1 && b!=2 && b!=4) return 0;
        extern volatile int g_hb_wkshow; extern int hb_wkshow(void); hb_wkshow(); __atomic_fetch_xor(&g_hb_wkshow,b,__ATOMIC_ACQ_REL);
        k=snprintf(m,sizeof m,"  [stats] page: weak-spot kinds now 0x%x\n",g_hb_wkshow); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"zonetype")){   // one kind of zone on/off: t = its bit (1 kill, 2 safe, 4 acceleration, 8 soft kill, 16 slip, 32 invisible walls)
        const char*v=strstr(qs,"t="); int b=v?atoi(v+2):0; if(b!=1&&b!=2&&b!=4&&b!=8&&b!=16&&b!=32) return 0;
        extern volatile int g_hbz_show; extern int hbz_show(void); hbz_show(); __atomic_fetch_xor(&g_hbz_show,b,__ATOMIC_ACQ_REL);
        k=snprintf(m,sizeof m,"  [stats] page: zone kinds now 0x%x\n",g_hbz_show); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"teams")){ static const char*const tm[3]={"all","enemies only","friends only"}; __atomic_fetch_add(&g_hb_teamcycle,1,__ATOMIC_ACQ_REL); hbs_sync();   // every feature
        k=snprintf(m,sizeof m,"  [stats] page: showing %s\n",tm[hb_team_mode()]); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"tpby")){   // move us by a delta (host; or a client with PELOAD_TELEPORT_CLIENT_TEST for the authority test)
        if(!getenv("PELOAD_ALLOW_TELEPORT") || (!st_is_host() && !getenv("PELOAD_TELEPORT_CLIENT_TEST"))) return 0;
        float d[3]={0,0,0}; const char*v; if((v=strstr(qs,"dx="))) d[0]=(float)atof(v+3); if((v=strstr(qs,"dy="))) d[1]=(float)atof(v+3); if((v=strstr(qs,"dz="))) d[2]=(float)atof(v+3);
        for(int i=0;i<3;i++) if(!(d[i]>=-50 && d[i]<=50)) return 0;
        extern volatile int g_hb_tp_by; extern float g_hb_tp_delta[3]; memcpy(g_hb_tp_delta,d,12); g_hb_tp_by=1; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); return 1; }
    // exploring spots past a map's edges (map-boundaries.md): boundaries off, then teleport there -- host only
    // a client of our host: the host switches them for everyone, and this client patches its own movement prediction too
    // (soft ceilings are simulated on the predicted biped; without it the client is pushed back until the host corrects it)
    if(!strcmp(act,"bounds") && !st_is_host()){ const char*hp=getenv("PELOAD_TELEPORT_HOST"), *v=strstr(qs,"off="); if(!hp || !v) return 0;
        int off=atoi(v+4)?1:0; char q[48]; snprintf(q,sizeof q,"do=bounds&off=%d",off); int ok=st_forward(hp,q);
        extern volatile int g_hb_bnd_req, g_hb_bnd_only, g_hb_bnd_client; g_hb_bnd_req=off; g_hb_bnd_only=1; g_hb_bnd_client=1; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE);
        return ok; }
    if(!strcmp(act,"explore") || !strcmp(act,"bounds")){
        if(!getenv("PELOAD_ALLOW_TELEPORT") || !st_is_host()) return 0; extern volatile int g_hb_bnd_req;
        if(!strcmp(act,"bounds")){ const char*v=strstr(qs,"off="); if(!v) return 0; extern volatile int g_hb_bnd_only; g_hb_bnd_req=atoi(v+4)?1:0; g_hb_bnd_only=1;
            __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); return 1; }
        const char*v=strstr(qs,"spot="); if(!v) return 0; const StSpot*sp=0; for(int i=0;i<(int)(sizeof g_st_spots/sizeof g_st_spots[0]);i++) if(!strncmp(v+5,g_st_spots[i].id,strlen(g_st_spots[i].id))) sp=&g_st_spots[i];
        if(!sp || strcmp(g_st_curmap,sp->map)) return 0;   // that spot is on another map
        extern volatile int g_hb_tp_to, g_hb_tp_to_who; extern float g_hb_tp_dest[3]; g_hb_bnd_req=1; g_hb_tp_dest[0]=sp->x; g_hb_tp_dest[1]=sp->y; g_hb_tp_dest[2]=sp->z; g_hb_tp_to_who=-1; g_hb_tp_to=1;
        __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); char m[120]; int k=snprintf(m,sizeof m,"  [explore] page: boundaries off, to %s\n",sp->label); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"tpto")){   // move a player (slot=, default us) to x,y,z -- host only (exploring, boundary tests)
        if(!st_is_host()){ const char*hp=getenv("PELOAD_TELEPORT_HOST"); if(!hp || strstr(qs,"slot=") || g_hb_local_slot<0) return 0;   // a client of our host: the host moves us
            const char*v; float d[3]; int got=0; if((v=strstr(qs,"x="))){ d[0]=(float)atof(v+2); got++; } if((v=strstr(qs,"y="))){ d[1]=(float)atof(v+2); got++; } if((v=strstr(qs,"z="))){ d[2]=(float)atof(v+2); got++; }
            if(got!=3) return 0; char q[160]; snprintf(q,sizeof q,"do=tpto&x=%.2f&y=%.2f&z=%.2f&slot=%d",d[0],d[1],d[2],g_hb_local_slot); return st_forward(hp,q); }
        if(!getenv("PELOAD_ALLOW_TELEPORT")) return 0; const char*v; float d[3]; int got=0;
        if((v=strstr(qs,"x="))){ d[0]=(float)atof(v+2); got++; } if((v=strstr(qs,"y="))){ d[1]=(float)atof(v+2); got++; } if((v=strstr(qs,"z="))){ d[2]=(float)atof(v+2); got++; }
        if(got!=3) return 0; for(int i=0;i<3;i++) if(!(d[i]>-30000 && d[i]<30000)) return 0;
        const char*w=strstr(qs,"slot="); int who=w?atoi(w+5):-1; if(who>255) return 0;
        extern volatile int g_hb_tp_to, g_hb_tp_to_who; extern float g_hb_tp_dest[3]; memcpy(g_hb_tp_dest,d,12); g_hb_tp_to_who=who; g_hb_tp_to=1;
        __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); return 1; }
    if(!strcmp(act,"tpnext")){ if(!getenv("PELOAD_ALLOW_TELEPORT")) return 0; const char*v=strstr(qs,"slot="); int sl=v?atoi(v+5):-1; if(sl<0 || sl>255) return 0;
        const char*w=strstr(qs,"who="); int who=w?atoi(w+4):-1; if(who>255) return 0;
        // a client can't move itself (the host snaps it back): forward to the host's loader (PELOAD_TELEPORT_HOST =
        // its stats address, in private games where both ends run this loader); the host moves our player
        if(!st_is_host()){ const char*hp=getenv("PELOAD_TELEPORT_HOST"); if(!hp || who>=0 || g_hb_local_slot<0) return 0; return st_forward_tp(hp,sl,g_hb_local_slot); }
        extern volatile int g_hb_tp_slot, g_hb_tp_who; g_hb_tp_who=who; g_hb_tp_slot=sl; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE);
        char m[100]; int k=snprintf(m,sizeof m,"  [teleport] page: %s next to player slot %d requested\n",who>=0?"move a client":"move us",sl); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"teleport")){ extern volatile int g_hb_tp_slot; g_hb_tp_slot=-1; if(!getenv("PELOAD_ALLOW_TELEPORT")) return 0; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); (void)!write(2,"  [teleport] page: requested\n",29); return 1; }
    if(!strcmp(act,"muteall") || !strcmp(act,"unmuteall")){ static StSess ss[128]; int n=st_session(ss,128,0), ok=0;
        for(int i=0;i<n;i++) if(ss[i].muted!=!strcmp(act,"muteall")) ok+=st_voice_request(ss[i].voice,!strcmp(act,"muteall"));
        char m[80]; int k=snprintf(m,sizeof m,"  [stats] page: %s (%d players)\n",act,ok); (void)!write(2,m,k); return n>0; }
    if(!strcmp(act,"mute") || !strcmp(act,"unmute")){ const char*v=strstr(qs,"voice="); unsigned long long id=v?strtoull(v+6,0,16):0;
        return st_voice_request(id,!strcmp(act,"mute")); }
    if(!strcmp(act,"textup") || !strcmp(act,"textdown")){ hbf_init(); int v=g_hb_text_px>0?g_hb_text_px:7; v+=!strcmp(act,"textup")?1:-1; if(v<4) v=4; if(v>60) v=60; g_hb_text_px=v; hbs_sync();   // every feature's labels
        char m[64]; int k=snprintf(m,sizeof m,"  [stats] page: label text %d px\n",v); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"facepoint")){ if(!getenv("PELOAD_INPUT_FACE")) return 0;   // test helper: the scripted pad turns toward x,y,z
        extern volatile float g_hb_facept[3]; extern volatile int g_hb_facept_on; const char*v; float d[3]; int got=0;
        if((v=strstr(qs,"x="))){ d[0]=(float)atof(v+2); got++; } if((v=strstr(qs,"y="))){ d[1]=(float)atof(v+2); got++; } if((v=strstr(qs,"z="))){ d[2]=(float)atof(v+2); got++; }
        if(got!=3){ g_hb_facept_on=0; return 1; } for(int i=0;i<3;i++) g_hb_facept[i]=d[i]; g_hb_facept_on=1; return 1; }
    if(!strcmp(act,"autovote")){ st_av_init(); const char*v; int ok=0;
        if((v=strstr(qs,"calls="))){ v+=6; if(!strncmp(v,"yes",3)) g_av_calls=1; else if(!strncmp(v,"no",2)) g_av_calls=2; else if(!strncmp(v,"off",3)) g_av_calls=0; else return 0; ok=1; }
        if((v=strstr(qs,"map="))){ v+=4; if(!strncmp(v,"none",4)) g_av_map=3; else if(!strncmp(v,"off",3)) g_av_map=-1; else if(*v>='1'&&*v<='3') g_av_map=*v-'1'; else return 0; ok=1; }
        k=snprintf(m,sizeof m,"  [stats] page: autovote calls %s, map %d\n",g_av_calls==1?"yes":g_av_calls==2?"no":"off",g_av_map); (void)!write(2,m,k); return ok; }
    if(!strcmp(act,"spar")){ const char*v=strstr(qs,"on="); if(!v) return 0; extern int hb_spar_available(void); if(!hb_spar_available()) return 0;   // the bot only
        extern volatile int g_spar_on; g_spar_on=atoi(v+3)?1:0;   // sparring partner aim
        char m[64]; int k=snprintf(m,sizeof m,"  [spar] page: sparring aim %s\n",g_spar_on?"on":"off"); (void)!write(2,m,k); return 1; }
    // Movement speed for everyone, in a game you host: the game variant's base player traits (game_globals +0x1608
    // +0x134, speed byte +0x10: tl_speed's index, 0 = unchanged, 5 = 100%). The host recomputes every player's traits
    // from the variant each tick and sends them to the clients (halo3-variant-traits.md), so joined players move at it.
    if(!strcmp(act,"speed")){ const char*v=strstr(qs,"v="); int sp=v?atoi(v+2):-1; if(sp<0 || sp>=(int)(sizeof tl_speed/sizeof*tl_speed)) return 0;
        if(!st_is_host()){ const char*hp=getenv("PELOAD_TELEPORT_HOST"); if(!hp) return 0; char q[32]; snprintf(q,sizeof q,"do=speed&v=%d",sp); int ok=st_forward(hp,q); if(ok) g_st_fwd_speed=sp; return ok; }
        uint64_t gg=0; uint8_t mode=0; if(!g_st_tls || !hb_read(g_st_tls+0x48,&gg,8) || !gg || !hb_read(gg+0x10,&mode,1) || mode!=2) return 0;
        *(volatile uint8_t*)(uintptr_t)(gg+0x1608+0x134+0x10)=(uint8_t)sp;
        char m[80]; int k=snprintf(m,sizeof m,"  [stats] page: movement speed %s (everyone)\n",tl_speed[sp]); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"mic")){ const char*v=strstr(qs,"on="); if(!v || !wa_mic_mode() || wa_mic_mode()==3) return 0; g_mic_muted=atoi(v+3)?0:1;   // the microphone (wasapi.c)
        char m[64]; int k=snprintf(m,sizeof m,"  [stats] page: microphone %s\n",g_mic_muted?"muted":"live"); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"submit")){ const char*v=strstr(qs,"on="); if(!v) return 0; g_sub_want=atoi(v+3)?1:0; return 1; }   // GL submission thread (d3dsubmit.c)
    if(!strcmp(act,"pixwho")){ extern void pixwho_request(int,int); const char*xv=strstr(qs,"x="), *yv=strstr(qs,"y="); pixwho_request(xv?atoi(xv+2):-1,yv?atoi(yv+2):-1);
        (void)!write(2,"  [pixwho] page: tracing the next frame's draws at one pixel\n",61); return 1; }
    if(!strcmp(act,"rtdump")){ extern volatile int g_rtd_req; __atomic_store_n(&g_rtd_req,1,__ATOMIC_RELEASE); (void)!write(2,"  [rtdump] page: dumping the next frame's render targets\n",57); return 1; }
    if(!strcmp(act,"capture")){ __atomic_store_n(&g_hb_cap_req,1,__ATOMIC_RELEASE); (void)!write(2,"  [capture] page: capturing the next frame\n",43); return 1; }
    // test hook: run PELOAD_AUTOCLICK steps now (s=<url-encoded steps>: clicks, k:<macOS keycode>, t:<text>, s:<secs>),
    // e.g. to find which key opens the client's text chat; only with PELOAD_STATS_UISTEPS=1 (a test client's page)
    if(!strcmp(act,"uisteps")){ if(!getenv("PELOAD_STATS_UISTEPS")) return 0; const char*v=strstr(qs,"s="); if(!v) return 0;
        static char steps[512]; size_t j=0; for(const char*q=v+2; *q && *q!='&' && j+1<sizeof steps; q++){
            if(*q=='%' && q[1] && q[2]){ char h[3]={q[1],q[2],0}; steps[j++]=(char)strtol(h,0,16); q+=2; } else steps[j++]=*q=='+'?' ':*q; } steps[j]=0;
        extern void wi_steps_async(const char*); wi_steps_async(steps); return 1; }
    if(!strcmp(act,"forensic")) return fx_control(qs);   // archive / voice audio / one-party consent (forensic.c)
    if(!strcmp(act,"rec")) return st_rec_start(qs);   // clip recording (rec.m): secs=<n>&mode=av|audio; why it refused: the snapshot's rec.last_error
    if(!strcmp(act,"recstop")){ rec_stop(); return 1; }
    return 0; }
// ---- the engine's own events (shim/docs/halo3-event-feed.md sections 2-3) -----------------------------------------
// display_event (halo3+0x11b0ec, event* in rcx) runs once for every event the HUD shows -- local, received from the
// host, kills with their damage type -- on the engine thread; chud_messaging_post (halo3+0x2d3f2c: user, UTF-16 text,
// context) puts each line in the lower-left feed. The hooks only copy into rings; the snapshot thread turns them into
// feed events (st_engine_events). A client gets the kills involving its team (all of them in two-team games, only its
// own in free-for-all); the death tracker's estimates fill in the rest.
typedef struct { uint8_t ev[0x2c]; double t; } StGe;
static StGe g_ge[512]; static volatile unsigned g_ge_n;            // one writer: the engine thread
typedef struct { uint16_t text[0x60]; int ctx, src, has_ev; uint8_t ev[0x2c]; double t; } StHud;   // src: 0 event, 1 string id, 2 hint, 3 client, 4 other
static StHud g_hud[256]; static volatile unsigned g_hud_n; static pthread_mutex_t g_hud_mx=PTHREAD_MUTEX_INITIALIZER;
static __thread uint8_t t_cur_ev[0x2c]; static __thread int t_cur_ev_ok;
static int g_st_evhook;   // display_event hooked: kills come from the engine
static void __attribute__((ms_abi)) st_on_display_event(const uint8_t*ev){
    if(!ev) return; memcpy(t_cur_ev,ev,0x2c); t_cur_ev_ok=1;
    unsigned q=g_ge_n; StGe*g=&g_ge[q&511]; memcpy(g->ev,ev,0x2c); g->t=st_wall(); __atomic_store_n(&g_ge_n,q+1,__ATOMIC_RELEASE); }
static void __attribute__((ms_abi)) st_on_chud(int user,const uint16_t*text,int ctx,uint64_t ret){ (void)user;
    if(!text) return; uint8_t*h3=hb_halo3_base(); int src=4;
    if(h3 && ret==(uint64_t)(uintptr_t)(h3+0x11b860)) src=0; else if(h3 && ret==(uint64_t)(uintptr_t)(h3+0x2d3f1b)) src=1;
    else if(h3 && ret==(uint64_t)(uintptr_t)(h3+0x2ddd18)) src=2; else if(g_base && ret>=(uint64_t)(uintptr_t)g_base && ret<(uint64_t)(uintptr_t)g_base+0x3000000) src=3;
    pthread_mutex_lock(&g_hud_mx); unsigned q=g_hud_n; StHud*h=&g_hud[q&255]; int i=0; for(; i<0x5F && text[i]; i++) h->text[i]=text[i]; h->text[i]=0;
    h->ctx=ctx; h->src=src; h->has_ev=src==0 && t_cur_ev_ok; if(h->has_ev) memcpy(h->ev,t_cur_ev,0x2c); h->t=st_wall();
    __atomic_store_n(&g_hud_n,q+1,__ATOMIC_RELEASE); pthread_mutex_unlock(&g_hud_mx); }
static void st_events_install(uint8_t*base){
    if(!stats_on() || getenv("PELOAD_NO_EVHOOK")) return;
    static const uint8_t dpro[17]={0x40,0x53,0x48,0x83,0xEC,0x40,0x41,0x83,0xC8,0xFF,0x48,0x8B,0xD9,0x44,0x39,0x41,0x0C};
    g_st_evhook=entry_hook(base+0x11b0ec,dpro,17,(void*)st_on_display_event);
    // chud_messaging_post: its prologue reads the TLS index rip-relative, so the trampoline replays it with absolute
    // addresses: sub rsp,0x28; cmp ecx,4; jge Lret; mov r9,&tls_index; mov r9d,[r9]; jmp 2d3f3c; Lret: add rsp,0x28; ret
    static const uint8_t cpro[16]={0x48,0x83,0xEC,0x28,0x83,0xF9,0x04,0x7D,0x36,0x44,0x8B,0x0D,0x60,0x60,0x76,0x00};
    uint8_t tail[48]; int n=0; static const uint8_t t0[]={0x48,0x83,0xEC,0x28,0x83,0xF9,0x04,0x7D,0x00};
    memcpy(tail,t0,sizeof t0); n=sizeof t0; int jge=n-1;
    tail[n++]=0x49; tail[n++]=0xB9; *(uint64_t*)(tail+n)=(uint64_t)(uintptr_t)(base+0xa39f9c); n+=8;   // mov r9,imm64
    tail[n++]=0x45; tail[n++]=0x8B; tail[n++]=0x09;                                                     // mov r9d,[r9]
    tail[n++]=0x49; tail[n++]=0xBB; *(uint64_t*)(tail+n)=(uint64_t)(uintptr_t)(base+0x2d3f3c); n+=8;   // mov r11,resume
    tail[n++]=0x41; tail[n++]=0xFF; tail[n++]=0xE3;                                                     // jmp r11
    tail[jge]=(uint8_t)(n-(jge+1));                                                                      // jge -> Lret
    tail[n++]=0x48; tail[n++]=0x83; tail[n++]=0xC4; tail[n++]=0x28; tail[n++]=0xC3;                     // Lret
    int ch=entry_hook_tail(base+0x2d3f2c,cpro,16,(void*)st_on_chud,tail,n,1);
    char m[160]; int k=snprintf(m,sizeof m,"  [stats] engine events: display_event %s, HUD feed %s\n",g_st_evhook?"hooked":"prologue differs",ch?"hooked":"prologue differs"); (void)!write(2,m,k); }
// damage_reporting_type -> weapon (halo3-event-feed.md 3.5; 44+ follow the projectile tags, not the client's table)
static const char*const g_dmg_name[63]={"Guardians","Falling","Collision","Melee","Explosion","Magnum","Plasma pistol","Needler","Mauler","SMG",
    "Plasma rifle","Battle rifle","Carbine","Shotgun","Sniper rifle","Beam rifle","Assault rifle","Spiker","Fuel rod gun","Missile pod",
    "Rocket launcher","Spartan laser","Brute shot","Flamethrower","Sentinel beam","Energy sword","Gravity hammer","Frag grenade","Plasma grenade","Spike grenade",
    "Firebomb","Flag","Bomb","Bomb explosion","Oddball","Machine gun turret","Plasma cannon","Plasma mortar","Plasma turret","Banshee",
    "Ghost","Mongoose","Scorpion","Scorpion gunner","Warthog","Warthog gunner","Gauss warthog","Wraith","Wraith gunner","Scorpion cannon",
    "Chopper","Hornet","Mantis","Prowler","Sentinel beam turret","Sentinel RPG","Teleporter","Trip mine","Elephant turret","Shade turret",
    "Silenced SMG","Automag","Brute plasma rifle"};
// Recent engine-reported deaths (victim name, time): an estimated death for the same player is dropped.
static struct { char name[64]; double t; } g_nat[64]; static int g_nat_i;
// Deaths the tracker saw, held 2 s for the engine's own kill event (it comes 0.15 s after the death on the host,
// plus the network to a client) before they go out as estimates.
typedef struct { char victim[64], killer[64], how[96]; int vteam, kteam; double t, wall; int used; } StPend;
static StPend g_pend[64];
static int st_nat_seen(const char*name,double wall){ for(int i=0;i<64;i++) if(g_nat[i].name[0] && !strcmp(g_nat[i].name,name) && fabs(g_nat[i].t-wall)<3.0) return 1; return 0; }
static void st_pending_death(const char*victim,int vteam,const char*killer,int kteam,const char*how){
    double wall=st_wall(); if(st_nat_seen(victim,wall)) return;
    int o=0; for(int i=0;i<64;i++){ if(!g_pend[i].used){ o=i; break; } if(g_pend[i].t<g_pend[o].t) o=i; }
    StPend*p=&g_pend[o]; memset(p,0,sizeof *p); p->used=1; p->t=mono_s(); p->wall=wall; p->vteam=vteam; p->kteam=kteam;
    snprintf(p->victim,sizeof p->victim,"%s",victim); if(killer) snprintf(p->killer,sizeof p->killer,"%s",killer); if(how) snprintf(p->how,sizeof p->how,"%s",how); }
static void st_pending_flush(double now){
    for(int i=0;i<64;i++){ StPend*p=&g_pend[i]; if(!p->used) continue;
        if(st_nat_seen(p->victim,p->wall)){ p->used=0; continue; }
        if(now-p->t<(g_st_evhook?2.0:0.0)) continue;
        if(p->killer[0]) st_kill_at(p->wall,p->killer,p->victim,p->how,"killed",p->kteam,p->vteam,1);
        else st_event_at(p->wall,"death",0,p->victim,0,-1,p->vteam,0,"%s died",p->victim);
        p->used=0; } }
static void st_u16_utf8(const uint16_t*w,char*o,size_t on){ size_t j=0;
    for(int i=0; w[i] && j+4<on; i++){ uint32_t c=w[i];
        if(c<0x80) o[j++]=(char)c; else if(c<0x800){ o[j++]=(char)(0xC0|(c>>6)); o[j++]=(char)(0x80|(c&63)); }
        else { o[j++]=(char)(0xE0|(c>>12)); o[j++]=(char)(0x80|((c>>6)&63)); o[j++]=(char)(0x80|(c&63)); } }
    o[j]=0; }
// a player handle from an event -> name (players table: index = low 16 bits, salt check)
static const char* st_ev_name(int32_t h,const StPlayer*pl,int np){ if(h==-1) return 0;
    for(int k=0;k<np;k++) if(pl[k].slot==(h&0xFFFF)){ uint16_t salt=0; if(hb_read(pl[k].rec,&salt,2) && salt==(uint16_t)((uint32_t)h>>16)) return pl[k].name; }
    return 0; }
static void st_sentence(char*o,size_t on,const char*tag);
// the vehicle a player (event handle) sits in, by its tag name ("Warthog"); 0 if on foot
static int st_player_vehicle(int32_t h,const StPlayer*pl,int np,char*out,size_t on){
    for(int k=0;k<np;k++) if(pl[k].slot==(h&0xFFFF)){ uint64_t arr=hb_object_array(); uint8_t hdr[0x60]; uint32_t u=pl[k].unit;
        if(!arr || u==0xFFFFFFFFu || !hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) return 0;
        uint64_t data=*(uint64_t*)(hdr+0x48); int hi=*(int32_t*)(hdr+0x3c);
        #define ST_ENT(hh,e) ((int)((hh)&0xFFFF)<hi && hb_read(data+(uint64_t)((hh)&0xFFFF)*0x18,e,0x18) && *(uint16_t*)(e)==(uint16_t)((hh)>>16))
        uint8_t e[0x18]; uint32_t parent=0xFFFFFFFFu, def=0; if(!ST_ENT(u,e) || !hb_read(*(uint64_t*)(e+0x10)+0x10,&parent,4) || parent==0xFFFFFFFFu) return 0;
        if(!ST_ENT(parent,e) || e[3]!=1 || !hb_read(*(uint64_t*)(e+0x10),&def,4)) return 0;
        #undef ST_ENT
        st_sentence(out,on,hb_tag_name(def)); return out[0]!=0; }
    return 0; }
static void st_engine_events(const StPlayer*pl,int np){
    static unsigned ge_seen, hud_seen;
    unsigned n=__atomic_load_n(&g_ge_n,__ATOMIC_ACQUIRE); if(n-ge_seen>512) ge_seen=n-512;
    for(;ge_seen!=n;ge_seen++){ const StGe*g=&g_ge[ge_seen&511]; const uint8_t*e=g->ev;
        int32_t type=*(const int32_t*)e, idx=*(const int32_t*)(e+4), cause=*(const int32_t*)(e+0x10), ct=*(const int32_t*)(e+0x14), effect=*(const int32_t*)(e+0x18), et=*(const int32_t*)(e+0x1c);
        { static int raw=-1; if(raw<0) raw=getenv("PELOAD_FEED_RAW")?1:0;   // PELOAD_FEED_RAW=1: every engine event, raw
          if(raw){ const char*an=st_ev_name(cause,pl,np), *bn=st_ev_name(effect,pl,np); char m[300];
              int k=snprintf(m,sizeof m,"  [feed-raw] type %d idx %#x aud %08x cause %08x(%s) ct %d effect %08x(%s) et %d qty %d dmg %02x %02x %02x %04x\n",type,idx,*(const int32_t*)(e+0xc),
                  cause,an?an:"-",ct,effect,bn?bn:"-",et,*(const int32_t*)(e+0x20),e[0x24],e[0x25],e[0x26],*(const uint16_t*)(e+0x28)); (void)!write(2,m,(size_t)k); } }
        if(!(e[0x25]&1)) continue;   // no kill info: its HUD line (if any) becomes the feed line
        int dmg=e[0x24]&0x3f, special=e[0x24]>>6, mods=((e[0x25]>>1)&0x7f)|((e[0x26]&1)<<7);
        const char*a=st_ev_name(cause,pl,np), *b=st_ev_name(effect,pl,np); if(!b) continue;
        // damage type 0 ("Guardians") with a killer: a weapon without a reporting type (modded maps' weapons) -- unnamed;
        // collision (a splatter): the vehicle the killer drives
        char how[96]; int k=snprintf(how,sizeof how,"%s",dmg==0 && a && cause!=effect ? "" : dmg<63?g_dmg_name[dmg]:"?");
        if(dmg==2 && a && cause!=effect){ char vn[64]; if(st_player_vehicle(cause,pl,np,vn,sizeof vn)) k=snprintf(how,sizeof how,"%s",vn); }
        // the general event says how: splatter, melee, sticky, sniper headshot, assassination (halo3-event-feed.md 3.6)
        const char*verb = type!=0 ? "killed" : idx==0x24 ? "splattered" : idx==0x25 ? "beat down" : idx==0x2a ? "stuck" : idx==0x2b ? "sniped" :
            idx==0x2c ? "assassinated" : idx==2 ? "betrayed" : "killed";
        #define ST_ADD(w) (k+=snprintf(how+k,sizeof how-(size_t)k,"%s%s",k?", ":"",w))
        if((mods&0x10) || special==1) ST_ADD("headshot");
        int vs=!strcmp(verb,"stuck"), vp=!strcmp(verb,"splattered"), vm=!strcmp(verb,"beat down"), va=!strcmp(verb,"assassinated");   // said by the verb
        if(mods&2 || special==2){ if(mods&8){ if(!vs) ST_ADD("stuck"); } else if(!va) ST_ADD("assassination"); } else if(mods&8 && !vs) ST_ADD("stuck");
        if((mods&4 || special==3) && dmg!=2 && !vp) ST_ADD("splatter");
        if(mods&1 && dmg!=3 && dmg!=25 && dmg!=26 && !vm) ST_ADD("melee");
        #undef ST_ADD

        if(type==0 && idx==0x23) st_event_at(g->t,"death",0,b,how,-1,et,0,"%s fell to their death",b);
        else if(cause==effect || (type==0 && idx==1)) st_event_at(g->t,"death",0,b,how,-1,et,0,how[0]?"%s killed themselves (%s)":"%s killed themselves",b,how);
        else if(!a) st_event_at(g->t,"death",0,b,how,-1,et,0,how[0]?"%s was killed (%s)":"%s was killed",b,how);
        else st_kill_at(g->t,a,b,how,verb,ct,et,type==0 && idx==2?4:0);
        snprintf(g_nat[g_nat_i].name,sizeof g_nat[0].name,"%s",b); g_nat[g_nat_i].t=g->t; g_nat_i=(g_nat_i+1)&63; }
    n=__atomic_load_n(&g_hud_n,__ATOMIC_ACQUIRE); if(n-hud_seen>256) hud_seen=n-256;
    static char last[256]; static double last_t;
    for(;hud_seen!=n;hud_seen++){ StHud h; pthread_mutex_lock(&g_hud_mx); h=g_hud[hud_seen&255]; pthread_mutex_unlock(&g_hud_mx);
        if(h.src==3) continue;   // the client's own roster lines: the log tap has them
        if(h.has_ev){ int32_t type=*(const int32_t*)h.ev, idx=*(const int32_t*)(h.ev+4);
            if(h.ev[0x25]&1) continue;                                          // kills: the structured event has them
            if(type==0 && (idx==0xb || idx==0xc || idx==0x19)) continue; }      // quit / joined / rejoined: the roster has them
        char txt[300]; st_u16_utf8(h.text,txt,sizeof txt); if(!txt[0]) continue;
        if(!strcmp(txt,last) && h.t-last_t<3.0) continue; snprintf(last,sizeof last,"%s",txt); last_t=h.t;
        st_event_at(h.t,"game",0,0,0,-1,-1,0,"%s",txt); } }
// Estimated hits (hitbox.c's object_delete hook: a projectile that ended inside a biped), the newest per biped, so a
// death can name the player whose shot ended in that body just before it -- an estimate: grenades, melee, splatters
// and environmental deaths have no projectile inside the body (then the feed says "died").
typedef struct { uint32_t victim, owner, def; double t; } StHit;
static StHit g_lasthit[256];
// tag names (hb_tag_name: "GAUSS BULLET") in sentence case: "Gauss bullet"
static void st_sentence(char*o,size_t on,const char*tag){ size_t j=0; if(!tag || !strcmp(tag,"?")){ o[0]=0; return; }
    for(const char*c=tag; *c && j+1<on; c++){ char ch=*c=='_'?' ':*c; o[j]=(char)(j==0?toupper((unsigned char)ch):tolower((unsigned char)ch)); j++; } o[j]=0; }
static void st_death_event(const char*victim,int vteam,uint32_t h,int np,const StPlayer*pl,const uint8_t*tab,int hi,double now){
    #define ST_OBJ(hh) ((int)((hh)&0xFFFF)<hi && (hh)!=0xFFFFFFFFu && *(const uint16_t*)(tab+((hh)&0xFFFF)*0x18)==(uint16_t)((hh)>>16) ? *(const uint64_t*)(tab+((hh)&0xFFFF)*0x18+0x10) : 0)
    const StHit*lh=&g_lasthit[(h*2654435761u)>>24]; const char*killer=0; int kteam=-1; char how[96]="";
    if(lh->victim==h && now-lh->t<1.5) for(int k=0;k<np;k++) if(pl[k].unit==lh->owner){ killer=pl[k].name;
        // what fired it: a grenade is named by its projectile; a killer seated in a vehicle or turret by that object;
        // otherwise the weapon in the killer's hands (the projectile's name when none of these is known)
        char proj[48]; st_sentence(proj,sizeof proj,hb_tag_name(lh->def)); uint8_t d[0x278]; uint64_t up=ST_OBJ(lh->owner);
        int have=up && hb_read(up,d,sizeof d); if(have) kteam=*(const int32_t*)(d+0x174);
        uint32_t parent=have?*(const uint32_t*)(d+0x10):0xFFFFFFFFu; uint64_t vp=ST_OBJ(parent); uint32_t vdef=0;
        if(strcasestr(proj,"grenade")) snprintf(how,sizeof how,"%s",proj);
        else if(vp && tab[(parent&0xFFFF)*0x18+3]==1 && hb_read(vp,&vdef,4)){ char vn[48]; st_sentence(vn,sizeof vn,hb_tag_name(vdef));
            snprintf(how,sizeof how,"%s",vn); }   // the seat's object: "Gauss", "Chaingun" (a Warthog's turrets), "Chopper"
        else if(have){ int ws=*(const int8_t*)(d+0x262); uint32_t wh=(ws>=0&&ws<4)?*(const uint32_t*)(d+0x268+ws*4):0xFFFFFFFFu; uint64_t wp=ST_OBJ(wh); uint32_t def=0;
            if(wp && hb_read(wp,&def,4)) st_sentence(how,sizeof how,hb_tag_name(def)); }
        if(!how[0]) snprintf(how,sizeof how,"%s",proj);
        break; }
    #undef ST_OBJ
    // a kill is an estimate (flag 1); a death without a recent hit is certain, its cause unknown
    st_pending_death(victim,vteam,killer,kteam,how); }
static void nav_json(StBuf*s);   // nav.c (PELOAD_NAV: the walker's goal and path)
// Spawns and pickups (backlog 47; user, 2026-10-18: "add weapon spawns to the timeline", then "add vehicle spawns and
// powerup spawns to the timeline too (include the x/y coordinates)") -> events of kind "weapon", "vehicle" and "powerup"
// (feed, forensic archive). A spawn's text ends "at (x, y)" and the event carries "pos":[x,y,z] (datum +0x50: world
// space for an object without a parent). Datum: +0x00 definition, +0x10 parent, +0x50 position; unit +0x174 team.
// A new object is a spawn once it has been seen with NO parent in two snapshots in a row (100 ms apart: the snapshot
// reads the object table while the engine runs, so it can catch an object between its creation and its attachment --
// a respawn loadout's weapon, a vehicle's turret). Which tags count (full tag path, st_wclass; checked on the 27 stock
// multiplayer .map files, re-tools/h3map.py):
//  - weapon: a weapon (object type 2) or an equipment (3) a player can pick up. Its first holder = "picked up" (a
//    player's unit, or the vehicle they sit in). A dropped weapon keeps its object (no event). Left out: grenades (a dying
//    player drops them as new objects) and DEPLOYED equipment -- the eqip tags under objects\equipment\ whose name does
//    not end in "_equipment" (bubbleshield, tripmine, regenerator, ...: what a used *_equipment item spawns; maps place
//    and players carry the *_equipment ones), gravlift_permanent included. More than 8 weapon spawns in one snapshot
//    (match start, or joining a game in progress) are one summary line.
//  - vehicle: type 1 without a parent, every one (turrets on a vehicle have it as their parent and a parent-relative
//    position: MORTAR, ANTI INFANTRY, CHAINGUN SNOW ...; the deployed auto turret, objects\equipment\autoturret, is left out).
//  - powerup: equipment under objects\multi\powerups\ (powerup_red / _blue / _yellow) or named powerup*, every one.
//    Named Overshield / Active camo / Custom power-up by colour (red / blue / yellow; GUESSED from Halo 3's names and the
//    variant's default durations 5 / 30 / 30 s). Taken = the player record's pickup tick for that colour changed (player
//    +0x218 / +0x21C / +0x220, halo3-variant-traits.md section 4): "<player> picked up <name>", the power-up = the
//    nearest one of that colour within 5 units (still there, or gone within 1.5 s). Nothing is guessed when no tick moves.
// PELOAD_SPAWN_LOG=1 prints each spawn candidate's fate (spawned, attached, left out, power-up gone without a pickup tick).
static struct { uint16_t salt; uint8_t live, st, type; int8_t kind, color; uint32_t def, parent; float pos[3]; } g_wt[8192];   // st: 1 spawned, 2 taken, 3 pending
static char g_wt_map[64];
static struct { char name[64]; int32_t tk[3]; } g_wpp[256];   // per player slot: the power-up pickup ticks last seen
static struct { int8_t color, taken; float pos[3]; double t; uint32_t def; } g_wpg[16]; static int g_nwpg;   // power-ups gone, not yet taken
typedef struct { uint32_t def; int8_t kind, color; char nm[40]; } StWcls;   // kind -1 left out, 0 weapon, 1 vehicle, 2 powerup
static StWcls g_stwc[256]; static int g_nstwc; static unsigned g_stwc_gen;
static const char*const g_wkind[3]={"weapon","vehicle","powerup"};
static const char*const g_wpup[3]={"Overshield","Active camo","Custom power-up"};
// the full tag path of a definition datum (as in the map file: lower case, backslashes), searched like hb_tag_name
static int st_tag_path(uint32_t def,char*out,size_t on){
    out[0]=0; char paths[HB_NPATHS+1][1024]; int np=0; hb_current_map(paths[0],1024); if(paths[0][0]) np=1;
    { pthread_mutex_lock(&g_hb_mapmx); for(int k=0;k<g_hb_nmappaths;k++) if(strcmp(g_hb_mappaths[k],paths[0])) memcpy(paths[np++],g_hb_mappaths[k],1024); pthread_mutex_unlock(&g_hb_mapmx); }
    for(int k=0;k<np;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || !m->names || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        snprintf(out,on,"%s",m->names+m->noff[ti]); return 1; }
    return 0; }
static const StWcls* st_wclass(uint32_t def,int type){
    if(g_stwc_gen!=g_hb_mapgen){ g_stwc_gen=g_hb_mapgen; g_nstwc=0; }   // (definition datums are per map)
    for(int i=0;i<g_nstwc;i++) if(g_stwc[i].def==def) return &g_stwc[i];
    static StWcls c; memset(&c,0,sizeof c); c.def=def; c.color=-1;
    char p[256]; int known=st_tag_path(def,p,sizeof p); for(char*q=p;*q;q++) *q=(char)tolower((unsigned char)*q);
    const char*b=strrchr(p,'\\'); b=b?b+1:p; size_t bl=strlen(b); int eq=!strncmp(p,"objects\\equipment\\",18), held=bl>10 && !strcmp(b+bl-10,"_equipment");
    if(type==1) c.kind=(eq || strstr(b,"camera"))?-1:1;   // not the deployed auto turret, nor security cameras (vehicle objects too: The Pit)
    else if(strstr(p,"grenade")) c.kind=-1;
    else if(strstr(p,"\\multi\\powerups\\") || !strncmp(b,"powerup",7)){ c.kind=2; c.color=strstr(b,"red")?0:strstr(b,"blue")?1:strstr(b,"yellow")?2:-1; }
    else if(type==3 && eq && !held) c.kind=-1;
    else c.kind=0;
    if(c.kind==2 && c.color>=0) snprintf(c.nm,sizeof c.nm,"%s",g_wpup[c.color]);
    else if(known){ char t[64]; snprintf(t,sizeof t,"%.*s",(int)(held?bl-10:bl),b); st_sentence(c.nm,sizeof c.nm,t); }
    if(!c.nm[0]){ const char*h=hb_tag_name(def); if(h && strstr(h,"GRENADE")) c.kind=-1; st_sentence(c.nm,sizeof c.nm,h); }
    if(!c.nm[0]) snprintf(c.nm,sizeof c.nm,"%s",type==1?"Unknown vehicle":"Unknown item");
    if(known && g_nstwc<256) g_stwc[g_nstwc++]=c;   // (an unknown path is looked up again: the map file may not be read yet)
    return &c; }
static int g_wlog=-1;
static void st_wgone(int i,double now){   // tracked object i is gone (or its slot was reused)
    if(g_wt[i].kind==2 && g_wt[i].st==1){ if(g_nwpg==16){ memmove(g_wpg,g_wpg+1,15*sizeof *g_wpg); g_nwpg=15; } int k=g_nwpg++;
        g_wpg[k].color=g_wt[i].color; g_wpg[k].taken=0; memcpy(g_wpg[k].pos,g_wt[i].pos,12); g_wpg[k].t=now; g_wpg[k].def=g_wt[i].def; } }
static void st_spawns(const uint8_t*tab,int hi,const StPlayer*pl,int np,const char*map,double now){
    if(g_wlog<0) g_wlog=getenv("PELOAD_SPAWN_LOG")?1:0;
    if(strcmp(map,g_wt_map)){ memset(g_wt,0,sizeof g_wt); memset(g_wpp,0,sizeof g_wpp); g_nwpg=0; snprintf(g_wt_map,sizeof g_wt_map,"%s",map); }
    if(!g_gameplay_t0 || !hi) return;
    char lm[300]; int conf[1024], nconf=0, nwc=0;
    for(int i=0;i<hi && i<8192;i++){ const uint8_t*e=tab+i*0x18; uint16_t salt=*(const uint16_t*)e; int t=e[3];
        int here=salt && (t==1 || t==2 || t==3);
        if(g_wt[i].live && (!here || g_wt[i].salt!=salt)){ st_wgone(i,now); g_wt[i].live=0; }
        if(!here) continue;
        uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0x5c]; if(!p || !hb_read(p,d,sizeof d)) continue;
        uint32_t def=*(const uint32_t*)d, par=*(const uint32_t*)(d+0x10);
        if(!g_wt[i].live){   // a new object: a spawn candidate when nothing holds it
            memset(&g_wt[i],0,sizeof g_wt[i]); g_wt[i].live=1; g_wt[i].salt=salt; g_wt[i].type=(uint8_t)t; g_wt[i].def=def; g_wt[i].parent=par; g_wt[i].kind=-1; g_wt[i].color=-1;
            if(par==0xFFFFFFFFu){ const StWcls*c=st_wclass(def,t);
                if(c->kind>=0){ g_wt[i].st=3; g_wt[i].kind=c->kind; g_wt[i].color=c->color; }
                else if(g_wlog && !strstr(c->nm,"renade")){ int k=snprintf(lm,sizeof lm,"  [spawn] left out: %s (object type %d, deployed equipment)\n",c->nm,t); (void)!write(2,lm,(size_t)k); } }
            continue; }
        if(g_wt[i].st==3){   // pending: still free a snapshot later = it spawned
            if(par!=0xFFFFFFFFu){ g_wt[i].st=0; g_wt[i].parent=par;
                if(g_wlog){ int k=snprintf(lm,sizeof lm,"  [spawn] attached right after creation, not a spawn: %s (parent %08x)\n",st_wclass(def,t)->nm,par); (void)!write(2,lm,(size_t)k); }
                continue; }
            g_wt[i].st=1; memcpy(g_wt[i].pos,d+0x50,12); g_wt[i].parent=par; if(nconf<1024){ conf[nconf++]=i; if(g_wt[i].kind==0) nwc++; } continue; }
        if(g_wt[i].st==1 && par==0xFFFFFFFFu) memcpy(g_wt[i].pos,d+0x50,12);   // where it lies now (a power-up: where it is taken)
        if(g_wt[i].st==1 && g_wt[i].kind==0 && par!=0xFFFFFFFFu){   // a weapon picked up (the first time)
            g_wt[i].st=2; uint32_t holder=par; const char*who=0; int team=-1;
            for(int pass=0;pass<2 && !who;pass++){
                for(int k=0;k<np;k++) if(pl[k].unit==holder){ who=pl[k].name; uint64_t ud=0; uint16_t hs=*(const uint16_t*)(tab+(holder&0xFFFF)*0x18);
                        if((holder&0xFFFF)<(uint32_t)hi && hs==(uint16_t)(holder>>16)) ud=*(const uint64_t*)(tab+(holder&0xFFFF)*0x18+0x10); if(ud) hb_read(ud+0x174,&team,4); break; }
                if(!who && (holder&0xFFFF)<(uint32_t)hi){ uint64_t hd=*(const uint64_t*)(tab+(holder&0xFFFF)*0x18+0x10); uint32_t up=0xFFFFFFFFu; if(hd && hb_read(hd+0x10,&up,4)) holder=up; else break; } }
            const char*nm=st_wclass(def,t)->nm; if(who) st_event_verb(0,"weapon",who,0,nm,"picked up",team,-1,0,"%s picked up %s",who,nm); }
        g_wt[i].parent=par; }
    // spawns confirmed in this snapshot: weapons in a burst = one line; vehicles and power-ups one by one
    if(nwc>8){ struct { char nm[40]; int n; } sum[24]; int ns=0;
        for(int q=0;q<nconf;q++){ int i=conf[q]; if(g_wt[i].kind!=0) continue; const char*nm=st_wclass(g_wt[i].def,g_wt[i].type)->nm; int k=0;
            for(; k<ns; k++) if(!strcmp(sum[k].nm,nm)) break; if(k==ns && ns<24){ snprintf(sum[ns].nm,sizeof sum[ns].nm,"%s",nm); sum[ns].n=0; ns++; } if(k<ns) sum[k].n++; }
        char txt[380]; int k=snprintf(txt,sizeof txt,"%d weapons and equipment spawned:",nwc);
        for(int q=0;q<ns && k<(int)sizeof txt-48;q++){ k+=snprintf(txt+k,sizeof txt-(size_t)k,"%s %s",q?",":"",sum[q].nm); if(sum[q].n>1) k+=snprintf(txt+k,sizeof txt-(size_t)k," x%d",sum[q].n); }
        st_event_verb(0,"weapon",0,0,"","spawned",-1,-1,0,"%s",txt); }
    for(int q=0;q<nconf;q++){ int i=conf[q], kd=g_wt[i].kind; if(kd==0 && nwc>8) continue; const char*nm=st_wclass(g_wt[i].def,g_wt[i].type)->nm; const float*ps=g_wt[i].pos;
        t_st_evpos=ps; st_event_verb(0,g_wkind[kd],0,0,nm,"spawned",-1,-1,0,"%s spawned at (%.1f, %.1f)",nm,ps[0],ps[1]); t_st_evpos=0; }
    // power-ups taken: a player's pickup tick for a colour moved (the first sight of a player only remembers it)
    for(int k=0;k<np;k++){ int32_t tk[3]; if(!pl[k].rec || !hb_read(pl[k].rec+0x218,tk,12)) continue; int s=pl[k].slot&255;
        if(strcmp(g_wpp[s].name,pl[k].name)){ snprintf(g_wpp[s].name,sizeof g_wpp[s].name,"%s",pl[k].name); memcpy(g_wpp[s].tk,tk,12); continue; }
        for(int c=0;c<3;c++){ if(tk[c]<=0 || tk[c]==g_wpp[s].tk[c]) continue;
            float me[3]={0,0,0}; int team=-1, mine=0; uint32_t h=pl[k].unit; if(h!=0xFFFFFFFFu && (int)(h&0xFFFF)<hi && *(const uint16_t*)(tab+(h&0xFFFF)*0x18)==(uint16_t)(h>>16)){
                uint64_t ud=*(const uint64_t*)(tab+(h&0xFFFF)*0x18+0x10); if(ud && hb_read(ud+0x1c,me,12)){ mine=1; hb_read(ud+0x174,&team,4); } }
            int bi=-1, bg=-1; float bd=25.0f;   // the nearest power-up of that colour within 5 units: one still there, or one gone in the last 1.5 s
            for(int i=0;i<hi && i<8192 && mine;i++) if(g_wt[i].live && g_wt[i].kind==2 && g_wt[i].st==1 && (g_wt[i].color==c || g_wt[i].color<0)){
                float dx=g_wt[i].pos[0]-me[0], dy=g_wt[i].pos[1]-me[1], dz=g_wt[i].pos[2]-me[2], dd=dx*dx+dy*dy+dz*dz; if(dd<bd){ bd=dd; bi=i; } }
            for(int g=0;g<g_nwpg && mine;g++) if(!g_wpg[g].taken && now-g_wpg[g].t<1.5 && (g_wpg[g].color==c || g_wpg[g].color<0)){
                float dx=g_wpg[g].pos[0]-me[0], dy=g_wpg[g].pos[1]-me[1], dz=g_wpg[g].pos[2]-me[2], dd=dx*dx+dy*dy+dz*dz; if(dd<bd){ bd=dd; bg=g; bi=-1; } }
            const char*nm=g_wpup[c]; const float*ps=0;
            if(bg>=0){ g_wpg[bg].taken=1; ps=g_wpg[bg].pos; nm=st_wclass(g_wpg[bg].def,3)->nm; }
            else if(bi>=0){ g_wt[bi].st=2; ps=g_wt[bi].pos; nm=st_wclass(g_wt[bi].def,3)->nm; }
            t_st_evpos=ps; st_event_verb(0,"powerup",pl[k].name,0,nm,"picked up",team,-1,0,"%s picked up %s",pl[k].name,nm); t_st_evpos=0; }
        memcpy(g_wpp[s].tk,tk,12); }
    // power-ups gone without anyone's pickup tick moving (PELOAD_SPAWN_LOG: is a power-up consumed some other way?)
    for(int g=0;g<g_nwpg;){ if(now-g_wpg[g].t<1.5){ g++; continue; }
        if(g_wlog && !g_wpg[g].taken){ int k=snprintf(lm,sizeof lm,"  [spawn] power-up gone without a pickup tick: %s at (%.1f, %.1f, %.1f)\n",st_wclass(g_wpg[g].def,3)->nm,g_wpg[g].pos[0],g_wpg[g].pos[1],g_wpg[g].pos[2]); (void)!write(2,lm,(size_t)k); }
        memmove(g_wpg+g,g_wpg+g+1,(size_t)(g_nwpg-g-1)*sizeof *g_wpg); g_nwpg--; }
    if(g_wlog) for(int q=0;q<nconf;q++){ int i=conf[q]; int k=snprintf(lm,sizeof lm,"  [spawn] %s spawned: %s at (%.2f, %.2f, %.2f)\n",g_wkind[g_wt[i].kind],st_wclass(g_wt[i].def,g_wt[i].type)->nm,g_wt[i].pos[0],g_wt[i].pos[1],g_wt[i].pos[2]); (void)!write(2,lm,(size_t)k); } }
static void st_snapshot(StBuf*s){
    static const char* teams[8]={"red","blue","green","orange","purple","gold","brown","pink"};
    extern double g_gameplay_t0; double now=mono_s();
    sb_put(s,"{\"time\":%.2f,\"in_match\":%s,\"inst\":%d",g_gameplay_t0?now-g_gameplay_t0:0.0,g_gameplay_t0?"true":"false",(int)getpid());   // inst: a new game -> open pages reload
    { pthread_mutex_lock(&g_ev_mx); uint32_t fs=g_ev_seq; pthread_mutex_unlock(&g_ev_mx); sb_put(s,",\"feed_seq\":%u",fs); }
    { char rj[1024]; rec_status_json(rj,sizeof rj,!win_hidden()); sb_put(s,",\"rec\":%s",rj); }   // clip recording (rec.m)
    { st_av_init(); st_av_tick(now); static const char*const mn[5]={"off","1","2","3","none"};
      sb_put(s,",\"autovote\":{\"calls\":\"%s\",\"map\":\"%s\"}",g_av_calls==1?"yes":g_av_calls==2?"no":"off",mn[g_av_map+1]); }
    char m[64]="";
    { char mp[1024]=""; hb_current_map(mp,sizeof mp);   // the map being played (halo3/maps or a Workshop mod's folder)
      const char*b=strrchr(mp,'/'); b=b?b+1:mp; snprintf(m,sizeof m,"%s",b); char*dot=strrchr(m,'.'); if(dot)*dot=0; sb_put(s,",\"map\":"); sb_str(s,m);
      if(g_gameplay_t0) st_maptop_ensure(mp,m); }   // not in the menu: its scene borrows other maps' objects
    { static int tp=-1; if(tp<0) tp=getenv("PELOAD_ALLOW_TELEPORT")?1:0; static const char*const tm[3]={"all","enemies","friends"};
      { extern int hbz_show(void), hb_wkshow(void); sb_put(s,",\"zone_show\":%d,\"weak_show\":%d",hbz_show(),hb_wkshow()); }
      sb_put(s,",\"overlay\":{\"vehicles\":%s,\"zones\":%s,\"hitboxes\":%s,\"tracers\":%s,\"info\":%s,\"items\":%s,\"visible\":%s,\"teams\":\"%s\",\"teleport\":%s,\"host\":%s,\"tp_forward\":%s,\"controls\":%s,\"text_px\":%d,\"spar\":%s,\"spar_aim\":%.1f,\"spar_available\":%s,\"bounds_off\":%s,\"mic\":\"%s\",\"mic_in_use\":%s",
          hb_feat(HBF_VEH)?"true":"false",hb_feat(HBF_ZONES)?"true":"false",hb_feat(HBF_HITBOX)?"true":"false",hb_feat(HBF_TRACER)?"true":"false",hb_feat(HBF_INFO)?"true":"false",hb_feat(HBF_ITEMS)?"true":"false",hb_feat(HBF_VISIBLE)?"true":"false",
          tm[hb_team_mode()],tp?"true":"false",st_is_host()?"true":"false",getenv("PELOAD_TELEPORT_HOST")?"true":"false",st_controls_ok()?"true":"false",g_hb_text_px>0?g_hb_text_px:7,hb_spar_enabled()?"true":"false",g_spar_ok?(double)g_spar_err:-1.0,hb_spar_available()?"true":"false",hb_bounds_off()==1?"true":hb_bounds_off()==0?"false":"null",
          !wa_mic_mode()||wa_mic_mode()==3?"off":wa_mic_muted()?"muted":wa_mic_mode()==2?"tone":"live",g_mic_streams>0?"true":"false");
      { int si=-1; uint64_t gg=0; uint8_t b=0; if(st_is_host()){ if(g_st_tls && hb_read(g_st_tls+0x48,&gg,8) && gg && hb_read(gg+0x1608+0x134+0x10,&b,1)) si=b; } else si=g_st_fwd_speed;
        sb_put(s,",\"speed_idx\":%d",si); }   // the movement speed index (tl_speed; 0 = the variant's own, 9 = 200%%), -1 unknown
      st_overlay_features(s);   // the overlay panel's per-feature settings (the keys above stay for older pages)
      sb_put(s,",\"spots\":["); int ns=0; for(int i=0;i<(int)(sizeof g_st_spots/sizeof g_st_spots[0]);i++) if(!strcmp(g_st_spots[i].map,m)){
          sb_put(s,"%s{\"id\":\"%s\",\"label\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}",ns++?",":"",g_st_spots[i].id,g_st_spots[i].label,g_st_spots[i].x,g_st_spots[i].y,g_st_spots[i].z); }
      sb_put(s,"]}"); snprintf(g_st_curmap,sizeof g_st_curmap,"%s",m); }
    if(strcmp(m,g_trk_map)){ memset(g_lasthit,0,sizeof g_lasthit); snprintf(g_trk_map,sizeof g_trk_map,"%s",m); g_ntrk=0; g_nbip=0; memset(g_proj,0,sizeof g_proj); g_hb_local_slot=-1; st_kills_reset(); }   // new map: new match stats
    pthread_mutex_lock(&g_hm_mx); snprintf(g_hm_cur,sizeof g_hm_cur,"%s",m); if(g_hm.dirty && now-g_hm.saved>30){ hm_save(); g_hm.saved=now; } pthread_mutex_unlock(&g_hm_mx);
    static uint8_t tab[8192*0x18]; int hi=0; uint64_t arr=hb_object_array(); uint8_t hdr[0x60];
    if(arr && hb_read(arr,hdr,sizeof hdr) && *(uint32_t*)(hdr+0x2c)==0x64407440 && *(int32_t*)(hdr+0x24)==0x18){
        hi=*(int32_t*)(hdr+0x3c); if(hi<0||hi>8192||hi>*(int32_t*)(hdr+0x20) || !hb_read(*(uint64_t*)(hdr+0x48),tab,(size_t)hi*0x18)) hi=0; }
    #define ST_DATUM(h) ((((h)&0xFFFF)<(uint32_t)hi && (h)!=0xFFFFFFFFu && *(uint16_t*)(tab+((h)&0xFFFF)*0x18)==(uint16_t)((h)>>16)) ? *(uint64_t*)(tab+((h)&0xFFFF)*0x18+0x10) : 0)
    #define ST_TYPE(h) (tab[((h)&0xFFFF)*0x18+3])
    StPlayer pl[256]; int np=st_players(pl,256);
    st_spawns(tab,hi,pl,np,m,now);   // weapon / equipment, vehicle and power-up spawns + pickups -> events
    float cam[3]={0}, fwd[3]={0}; int camok=0; { float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); if(!(s0&1) && g_hb_vpseq==s0 && (vp[28]||vp[29]||vp[30])){ memcpy(cam,vp+28,12);
        float l=sqrtf(vp[12]*vp[12]+vp[13]*vp[13]+vp[14]*vp[14]); if(l>0){ fwd[0]=vp[12]/l; fwd[1]=vp[13]/l; fwd[2]=vp[14]/l; camok=1; } } }
    // The local player (stats "you", the overlay's friend/enemy split). Halo 3's player mapping (TLS +0x110; layout in
    // shim/docs/halo3-variant-traits.md section 6): output user 0..3 -> player handle at +0xB8 -- what the HUD and
    // cameras use, right in third person, vehicles and while dead. Else a player whose machine index (+0x1A) is the
    // local machine (players globals TLS +0x40, +0x7C). Last resort: the biped the camera is inside / just ahead on
    // the view ray. The slot is kept while none of these answers.
    int mapped=-1;
    { uint64_t pm=0; int32_t ou[4]; if(g_st_tls && hb_read(g_st_tls+0x110,&pm,8) && pm && hb_read(pm+0xB8,ou,16))
          for(int u=0;u<4 && mapped<0;u++) if(ou[u]!=-1) for(int k=0;k<np;k++) if(pl[k].slot==(ou[u]&0xFFFF)){ mapped=pl[k].slot; break; }
      uint64_t pg=0; int32_t lm=-1;
      if(mapped<0 && g_st_tls && hb_read(g_st_tls+0x40,&pg,8) && pg && hb_read(pg+0x7C,&lm,4) && lm>=0)
          for(int k=0;k<np;k++){ int16_t mi=-1; if(hb_read(pl[k].rec+0x1A,&mi,2) && mi==lm){ mapped=pl[k].slot; break; } } }
    st_engine_events(pl,np); st_pending_flush(now);
    if(mapped>=0) g_hb_local_slot=mapped;
    else if(camok){ uint32_t best=0xFFFFFFFFu; float bd=1e9f; int inside=0;
        for(int k=0;k<np;k++){ uint32_t h=pl[k].unit; uint64_t p=ST_DATUM(h); float c[4]; if(!p || ST_TYPE(h)!=0 || !hb_read(p+0x1c,c,16)) continue;
            float v[3]={c[0]-cam[0],c[1]-cam[1],c[2]-cam[2]}, d2=v[0]*v[0]+v[1]*v[1]+v[2]*v[2], r=c[3];
            if(d2<(r+0.3f)*(r+0.3f)){ best=h; inside=1; break; }
            float a=v[0]*fwd[0]+v[1]*fwd[1]+v[2]*fwd[2], perp2=d2-a*a; if(a>0.3f && a<6.0f && perp2<(r+0.4f)*(r+0.4f) && a<bd){ bd=a; best=h; } }
        (void)inside; if(best!=0xFFFFFFFFu) for(int k=0;k<np;k++) if(pl[k].unit==best){ g_hb_local_slot=pl[k].slot; break; } }
    g_hb_local_unit=0xFFFFFFFFu; for(int k=0;k<np;k++) if(pl[k].slot==g_hb_local_slot){ g_hb_local_unit=pl[k].unit; break; }
    // shots: every projectile that ended since the last snapshot (hitbox.c's object_delete hook), with the biped it
    // hit if any; without the hook, count the projectiles seen in flight (misses most bullets)
    if(g_hb_delhook){ static unsigned seen; unsigned n=__atomic_load_n(&g_hbshot_n,__ATOMIC_ACQUIRE); if(n-seen>8192) seen=n-8192;
        for(;seen!=n;seen++){ const HbShot*r=&g_hbshot[seen&8191];
            if(r->victim!=0xFFFFFFFFu){ StHit*lh=&g_lasthit[(r->victim*2654435761u)>>24]; lh->victim=r->victim; lh->owner=r->owner; lh->def=r->def; lh->t=now; }
            for(int k=0;k<np;k++) if(pl[k].unit==r->owner){ StTrack*t=st_track(&pl[k]); if(t){ t->shots++; if(r->victim!=0xFFFFFFFFu) t->hits++; } break; } } }
    else for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=5) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint32_t own=0;
        uint32_t h=((uint32_t)*(const uint16_t*)e<<16)|(uint32_t)i; uint32_t slot=(h*2654435761u)>>19, j=slot; int known=0;
        for(int q=0;q<16;q++,j=(j+1)&8191){ if(g_proj[j].h==h && now-g_proj[j].t<60){ known=1; break; } if(!g_proj[j].h || now-g_proj[j].t>=60) break; }
        if(known) continue; g_proj[j].h=h; g_proj[j].t=now;
        if(p && hb_read(p+0xC0,&own,4)) for(int k=0;k<np;k++) if(pl[k].unit==own){ StTrack*t=st_track(&pl[k]); if(t) t->shots++; break; } }
    // deaths: every biped, alive -> dead (the body stays a while, so 10 Hz catches it); the player it belonged to.
    // Positions are the world-space bounding centre (+0x1c): the origin (+0x50) is vehicle-relative when seated.
    { StBip nb[2048]; int nn=0;
      for(int i=0;i<hi && nn<2048;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=0) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0x178]; if(!p || !hb_read(p,d,sizeof d)) continue;
          uint32_t h=((uint32_t)*(const uint16_t*)e<<16)|(uint32_t)i; int alive=!((*(const uint32_t*)(d+0x110)>>2)&1), ti=-1;
          for(int k=0;k<np;k++) if(pl[k].unit==h){ StTrack*t=st_track(&pl[k]); if(t) ti=(int)(t-g_trk); break; }
          int prev=-1; for(int b=0;b<g_nbip;b++) if(g_bip[b].h==h){ prev=b; break; }
          if(prev>=0){ if(ti<0) ti=g_bip[prev].ti;
              if(g_bip[prev].alive && !alive){ StTrack*t=ti>=0?&g_trk[ti]:0; int team=*(const int32_t*)(d+0x174);
                  if(t){ t->deaths++; t->alive=0; if(now-t->life_start>t->longest) t->longest=now-t->life_start; }
                  if(t) st_death_event(t->name,team,h,np,pl,tab,hi,now);
                  if(t || getenv("PELOAD_STATS_ALL_BIPEDS")){ pthread_mutex_lock(&g_hm_mx); hm_death((const float*)(d+0x1c),team,t?t->slot:-1,t?t->name:""); pthread_mutex_unlock(&g_hm_mx); } } }
          nb[nn].h=h; nb[nn].alive=alive; nb[nn].ti=ti; nn++; }
      memcpy(g_bip,nb,(size_t)nn*sizeof *nb); g_nbip=nn; }
    // players: the engine's player table (a player between lives has no live biped)
    st_variant(s);   // the game's options and trait sets (st_players found the TLS block)
    static StSess ss[128]; uint32_t sst=0; int nss=st_session(ss,128,&sst); st_addr_refresh();
    sb_put(s,",\"session\":["); for(int k=0;k<nss;k++){ sb_put(s,"%s{\"name\":",k?",":""); sb_str(s,ss[k].name);
        sb_put(s,",\"index\":%d,\"peer\":%d,\"host\":%s,\"voice\":\"%016llx\",\"muted\":%s,\"talking\":%s",ss[k].idx,ss[k].peer,ss[k].host?"true":"false",(unsigned long long)ss[k].voice,ss[k].muted?"true":"false",st_talk_age(ss[k].voice)<0.5?"true":"false");
        { const char*ip=st_addr_of(ss[k].voice); if(ip){ sb_put(s,",\"ip\":"); sb_str(s,ip); } } sb_put(s,"}"); }
    { int hostplayer=0; for(int k=0;k<nss;k++) if(ss[k].host) hostplayer=1;
      sb_put(s,"],\"session_state\":%u,\"host_peer\":%d,\"local_peer\":%d,\"dedicated\":%s,\"mute\":%s",sst,g_st_hostpeer,g_st_localpeer,
          g_st_hostpeer>=0 && nss && !hostplayer?"true":"false",g_st_setmuted&&st_controls_ok()?"true":"false"); }
    { const char*src=0; int mx=st_max_players(&src); sb_put(s,",\"slots\":{\"used\":%d",nss);
      if(mx){ sb_put(s,",\"max\":%d,\"src\":\"%s\"}",mx,src); } else sb_put(s,",\"max\":null}"); }
    // addresses: "host" = this client hosts (players' addresses from its moderation table), "client" = only the server's
    { char srv[96]=""; const char*p=strstr(g_cmdlineA,"join?server="); if(p){ p+=12; size_t j2=0; while(*p && *p!=' ' && *p!='"' && *p!='&' && j2+1<sizeof srv) srv[j2++]=*p++; srv[j2]=0; }
      int client=srv[0] || (nss && g_st_hostpeer>=0 && g_st_hostpeer!=g_st_localpeer);   // (the join link is on the menu process's command line, not always ours)
      sb_put(s,",\"addr_role\":\"%s\"",!st_ips_ok()?"hidden":g_st_hosting?"host":client?"client":"none");
      if(srv[0] && st_ips_ok() && !g_st_hosting){ sb_put(s,",\"server_addr\":"); sb_str(s,srv); } }
    sb_put(s,",\"players\":["); int first=1;
    for(int k=0;k<np;k++){ StTrack*t=st_track(&pl[k]); uint32_t h=pl[k].unit; uint64_t p=ST_DATUM(h); static uint8_t d[0x380];
        int have = p && ST_TYPE(h)==0 && hb_read(p,d,sizeof d); int dead = !have || ((*(const uint32_t*)(d+0x110)>>2)&1);
        // world-space centre (the origin at +0x50 is vehicle-relative when seated)
        const float*o=have?(const float*)(d+0x1c):(t?t->pos:(const float[3]){0,0,0}); float spd=0; int team=have?*(const int32_t*)(d+0x174):(t?t->team:-1);
        if(have){ const float*v=(const float*)(d+0x74); spd=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
        if(t){ if(have && !dead){ if(!t->alive || t->unit!=h){ t->alive=1; t->life_start=now; }
                   else { float dx=o[0]-t->pos[0],dy=o[1]-t->pos[1],dz=o[2]-t->pos[2],dd=sqrtf(dx*dx+dy*dy+dz*dz); if(dd<20) t->dist+=dd; }
                   if(spd>t->top_speed && spd<50) t->top_speed=spd; memcpy(t->pos,o,12);
                   if(g_gameplay_t0){ pthread_mutex_lock(&g_hm_mx); hm_switch(g_hm_cur); hm_cell_add((int32_t)floorf(o[0]/HM_CELL),(int32_t)floorf(o[1]/HM_CELL),1); g_hm.dirty=1; pthread_mutex_unlock(&g_hm_mx); } }
               else if(t->alive && !have){ t->alive=0; }   // biped gone without us seeing it die
               t->unit=h; if(team>=0) t->team=team; }
        int you=pl[k].slot==g_hb_local_slot;
        sb_put(s,"%s{\"name\":",first?"":","); first=0; sb_str(s,pl[k].name); sb_put(s,",\"slot\":%d",pl[k].slot);
        sb_put(s,",\"handle\":\"%08x\",\"team\":%d,\"team_name\":\"%s\",\"you\":%s,\"alive\":%s,\"health\":%.3f,\"shield\":%.3f,\"camo\":%.3f",
               h,team,(team>=0&&team<8)?teams[team]:"none",you?"true":"false",dead?"false":"true",dead?0:*(const float*)(d+0xF4),dead?0:*(const float*)(d+0xF8),have?*(const float*)(d+0x360):0);
        sb_put(s,",\"weapon\":");
        if(have && !dead){ int ws=*(const int8_t*)(d+0x262); const uint32_t*wp=(const uint32_t*)(d+0x268); uint64_t wd=(ws>=0&&ws<4)?ST_DATUM(wp[ws]):0; uint32_t def=0; if(wd && hb_read(wd,&def,4)) st_tag(s,def); else sb_put(s,"null");
            sb_put(s,",\"weapons\":["); for(int w=0,fw=1;w<4;w++){ uint64_t wd2=ST_DATUM(wp[w]); uint32_t df=0; if(!wd2 || !hb_read(wd2,&df,4)) continue; sb_put(s,"%s",fw?"":","); fw=0; st_tag(s,df); } sb_put(s,"]");
            const int8_t*gr=(const int8_t*)(d+0x294); sb_put(s,",\"grenades\":{\"frag\":%d,\"plasma\":%d,\"spike\":%d,\"firebomb\":%d}",gr[0],gr[1],gr[2],gr[3]);
            uint32_t parent=*(const uint32_t*)(d+0x10); sb_put(s,",\"vehicle\":"); { uint64_t vd=parent!=0xFFFFFFFFu?ST_DATUM(parent):0; uint32_t df=0; if(vd && hb_read(vd,&df,4)){ st_tag(s,df); sb_put(s,",\"seat\":%d",*(const int16_t*)(d+0x24E)); } else sb_put(s,"null"); }
            const float*fw=(const float*)(d+0x5c); sb_put(s,",\"facing\":%.0f",atan2f(fw[1],fw[0])*57.2958f); }
        else sb_put(s,"null,\"weapons\":[],\"grenades\":{\"frag\":0,\"plasma\":0,\"spike\":0,\"firebomb\":0},\"vehicle\":null");
        sb_put(s,",\"pos\":"); sb_f3(s,o); sb_put(s,",\"speed\":%.2f",spd);
        sb_put(s,",\"kills\":%d",st_kills_of(pl[k].name));
        if(t) sb_put(s,",\"deaths\":%d,\"hits\":%d,\"accuracy\":%.3f,\"shots\":%d,\"life\":%.1f,\"longest_life\":%.1f,\"distance\":%.1f,\"top_speed\":%.2f",t->deaths,t->hits,t->shots?(double)t->hits/t->shots:0.0,t->shots,t->alive?now-t->life_start:0.0,
                     t->longest>(t->alive?now-t->life_start:0)?t->longest:(t->alive?now-t->life_start:0),t->dist,t->top_speed);
        if(g_st_var_ok) st_player_traits(s,pl[k].rec);
        { const StSess*vs=0; for(int q=0;q<nss && !vs;q++) if(ss[q].idx==pl[k].slot && !strcmp(ss[q].name,pl[k].name)) vs=&ss[q];
          for(int q=0;q<nss && !vs;q++) if(!strcmp(ss[q].name,pl[k].name)) vs=&ss[q];
          if(vs) sb_put(s,",\"voice\":\"%016llx\",\"muted\":%s,\"talking\":%s,\"host\":%s",(unsigned long long)vs->voice,vs->muted?"true":"false",st_talk_age(vs->voice)<0.5?"true":"false",vs->host?"true":"false");
          { const char*ip=vs?st_addr_of(vs->voice):0; if(ip){ sb_put(s,",\"ip\":"); sb_str(s,ip); } } }
        sb_put(s,"}"); }
    sb_put(s,"]");
    fx_roster_tick(m,ss,nss,pl,np);   // forensic archive: roster changes + session boundaries (forensic.c)
    // vehicles and projectiles in flight
    sb_put(s,",\"vehicles\":["); first=1;
    for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=1) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0x120]; if(!p || !hb_read(p,d,sizeof d)) continue;
        int dead=(*(const uint32_t*)(d+0x110)>>2)&1; sb_put(s,"%s{\"type\":",first?"":","); first=0; st_tag(s,*(const uint32_t*)d);
        sb_put(s,",\"health\":%.3f,\"shield\":%.3f,\"pos\":",dead?0:*(const float*)(d+0xF4),dead?0:*(const float*)(d+0xF8)); sb_f3(s,(const float*)(d+0x50)); sb_put(s,"}"); }
    sb_put(s,"],\"projectiles\":["); first=1;
    for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=5) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0xC8]; if(!p || !hb_read(p,d,sizeof d)) continue;
        uint32_t own=*(const uint32_t*)(d+0xC0); const char*on=0; for(int k=0;k<np;k++) if(pl[k].unit==own){ on=pl[k].name; break; }
        const float*v=(const float*)(d+0x74); sb_put(s,"%s{\"type\":",first?"":","); first=0; st_tag(s,*(const uint32_t*)d);
        sb_put(s,",\"owner\":"); sb_str(s,on?on:""); sb_put(s,",\"pos\":"); sb_f3(s,(const float*)(d+0x50)); sb_put(s,",\"speed\":%.1f}",sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2])); }
    sb_put(s,"]"); nav_json(s); sb_put(s,"}");
    #undef ST_DATUM
    #undef ST_TYPE
}
static void* st_snap_thread(void*a){ (void)a; pthread_setname_np("shim-stats-snap");
    for(;;){ StBuf s={0}; st_snapshot(&s); if(s.b){ pthread_mutex_lock(&g_st_mx); free(g_st_json); g_st_json=s.b; g_st_len=s.n; g_st_seq++; pthread_cond_broadcast(&g_st_cv); pthread_mutex_unlock(&g_st_mx); }
        usleep(100000); }
    return 0; }

// the page: shim/src/stats_page.html, embedded at build time
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const char g_st_page[]={
#embed "stats_page.html"
,0};
// every game option the client accepts: key, label, game types, choices, where it lands in halo3's variant
// (shim/docs/option-map.json, from the client's option table; shim/docs/game-options.md)
static const char g_st_optmap[]={
#embed "../docs/option-map.json"
,0};
#pragma clang diagnostic pop
static int st_send(int fd,const char*b,size_t n){ while(n){ ssize_t k=send(fd,b,n,0); if(k<=0){ if(k<0&&errno==EINTR) continue; return 0; } b+=k; n-=(size_t)k; } return 1; }
static volatile unsigned long g_st_hits;   // page requests (/events, /stats.json): a tab is showing this game
// GET /recordings/<file>: a clip from ~/Movies/Reclaimer (rec_file_path checks the name), streamed; Range requests are
// answered (a browser playing the clip in a tab seeks with them)
static void st_rec_file(int fd,const char*req,const char*name){
    char fp[1100], h[512]; struct stat st; int f=rec_file_path(name,fp,sizeof fp)?open(fp,O_RDONLY):-1;
    if(f<0 || fstat(f,&st)){ const char*nf="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,nf,strlen(nf)); if(f>=0) close(f); return; }
    long long a=0, b=(long long)st.st_size-1; int part=0; const char*rg=strcasestr(req,"\r\nRange: bytes=");
    if(rg && st.st_size>0){ long long x=-1, y=-1; int c=sscanf(rg+15,"%lld-%lld",&x,&y); if(c>=1 && x>=0 && x<st.st_size){ a=x; if(c==2 && y>=a && y<st.st_size) b=y; part=1; } }
    int k=snprintf(h,sizeof h,"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n",part?"206 Partial Content":"200 OK",
        strstr(name,".m4a")?"audio/mp4":"video/mp4",st.st_size?b-a+1:0);
    if(part) k+=snprintf(h+k,sizeof h-(size_t)k,"Content-Range: bytes %lld-%lld/%lld\r\n",a,b,(long long)st.st_size);
    k+=snprintf(h+k,sizeof h-(size_t)k,"Content-Disposition: inline; filename=\"%s\"\r\nConnection: close\r\n\r\n",name);
    if(st_send(fd,h,(size_t)k) && st.st_size){ char*buf=malloc(1<<18); long long left=b-a+1, off=a;
        while(buf && left>0){ ssize_t n=pread(f,buf,left<(1<<18)?(size_t)left:(size_t)(1<<18),off); if(n<=0 || !st_send(fd,buf,(size_t)n)) break; off+=n; left-=n; }
        free(buf); }
    close(f); }
static void* st_conn(void*a){ int fd=(int)(intptr_t)a; pthread_setname_np("shim-stats-http");
    int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one); setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    char req[2048]; ssize_t n=recv(fd,req,sizeof req-1,0); if(n<=0){ close(fd); return 0; } req[n]=0;
    char meth[8]="", path[1024]="/", qs[768]=""; sscanf(req,"%7s %1023s",meth,path); char*q=strchr(path,'?'); if(q){ snprintf(qs,sizeof qs,"%s",q+1); *q=0; }
    char h[512];
    if(!strcmp(path,"/control")){
        // POST + X-Reclaimer-Control header (see st_controls_ok); ?do=<action>
        char act[32]=""; const char*d=strstr(qs,"do="); if(d) sscanf(d+3,"%31[a-z]",act);
        int hdr=strcasestr(req,"\r\nX-Reclaimer-Control:")!=0, code=!strcmp(meth,"POST")&&hdr?(st_controls_ok()?(st_control(act,qs)?200:400):403):403;
        const char*body=code==200?"{\"ok\":true}":code==400?"{\"ok\":false,\"error\":\"unknown or disabled action\"}":!st_controls_ok()?"{\"ok\":false,\"error\":\"controls are off\"}":"{\"ok\":false,\"error\":\"needs POST with an X-Reclaimer-Control header\"}";
        int k=snprintf(h,sizeof h,"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",code,code==200?"OK":code==400?"Bad Request":"Forbidden",strlen(body),body);
        st_send(fd,h,(size_t)k); }
    else if(!strcmp(path,"/option-map.json")){
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: max-age=300\r\nConnection: close\r\n\r\n",sizeof g_st_optmap-1);
        if(st_send(fd,h,(size_t)k)) st_send(fd,g_st_optmap,sizeof g_st_optmap-1); }
    else if(!strncmp(path,"/maptop/",8)){
        // top-down map pictures made from the .map files (shim/tools/maptop.sh -> re-tools/h3bsp.py):
        // ~/Library/Application Support/ReclaimerMac/maptop/<map>.png + <map>.json (world bounds)
        const char*nm=path+8; size_t L=strlen(nm); int ok=L>4 && L<64; for(size_t i=0;ok && i<L;i++){ char c=nm[i]; ok=(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'; }
        const char*ct=ok&&L>4&&!strcmp(nm+L-4,".png")?"image/png":ok&&L>5&&!strcmp(nm+L-5,".json")?"application/json":ok&&L>6&&!strcmp(nm+L-6,".z.bin")?"application/octet-stream":0;   // .z.bin: ground heights (teleport here)
        char fp[1024]; const char*home=getenv("HOME"); FILE*f=0; if(ct && !strstr(nm,"..")){ snprintf(fp,sizeof fp,"%s/Library/Application Support/ReclaimerMac/maptop/%s",home?home:"",nm); f=fopen(fp,"rb"); }
        char*buf=0; long n=0; if(f){ fseek(f,0,SEEK_END); n=ftell(f); fseek(f,0,SEEK_SET); if(n>0 && n<(32<<20) && (buf=malloc((size_t)n)) && fread(buf,1,(size_t)n,f)==(size_t)n){} else { free(buf); buf=0; } fclose(f); }
        if(buf){ int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: max-age=60\r\nConnection: close\r\n\r\n",ct,n);
            if(st_send(fd,h,(size_t)k)) st_send(fd,buf,(size_t)n); free(buf); }
        else { const char*nf="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,nf,strlen(nf)); } }
    else if(strcmp(meth,"GET")){ const char*na="HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,na,strlen(na)); }
    else if(!strncmp(path,"/forensics",10) && (!path[10] || path[10]=='/')) fx_http(fd,path,qs,req);   // the forensic archive viewer (forensic.c)
    else if(!strcmp(path,"/events")){ g_st_hits++;
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        if(st_send(fd,h,(size_t)k)) for(unsigned long seen=0;;){ pthread_mutex_lock(&g_st_mx); while(g_st_seq==seen) pthread_cond_wait(&g_st_cv,&g_st_mx);
                seen=g_st_seq; size_t len=g_st_len; char*j=malloc(len+16); if(j){ memcpy(j,"data: ",6); memcpy(j+6,g_st_json,len); memcpy(j+6+len,"\n\n",2); } pthread_mutex_unlock(&g_st_mx);
                int ok=j && st_send(fd,j,len+8); free(j); if(!ok) break; } }
    else if(!strcmp(path,"/feed.json")){   // events after ?since=<seq> (the last 1024 at most)
        const char*v=strstr(qs,"since="); uint32_t since=v?(uint32_t)strtoul(v+6,0,10):0; StBuf b={0}; st_feed_json(&b,since);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",b.n);
        if(st_send(fd,h,(size_t)k) && b.n) st_send(fd,b.b,b.n); free(b.b); }
    else if(!strcmp(path,"/recordings.json")){   // clip recording (rec.m): ~/Movies/Reclaimer's clips, newest first
        size_t n=0; char*j=rec_list_json(&n);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",j?n:0);
        if(st_send(fd,h,(size_t)k) && j) st_send(fd,j,n); free(j); }
    else if(!strncmp(path,"/recordings/",12)) st_rec_file(fd,req,path+12);
    else if(!strcmp(path,"/heat.json")){
        StBuf b={0}; hm_json(&b);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",b.n);
        if(st_send(fd,h,(size_t)k) && b.n) st_send(fd,b.b,b.n); free(b.b); }
    else if(!strcmp(path,"/stats.json")){ g_st_hits++;
        pthread_mutex_lock(&g_st_mx); size_t len=g_st_len; char*j=malloc(len+1); if(j && g_st_json) memcpy(j,g_st_json,len); else len=0; pthread_mutex_unlock(&g_st_mx);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",len);
        if(st_send(fd,h,(size_t)k) && len) st_send(fd,j,len); free(j); }
    else if(!strcmp(path,"/")){
        // PELOAD_STATS_PAGE=<file>: serve the page from disk on every request (play.sh sets it in a source checkout, so
        // page edits show on a browser refresh); otherwise the copy embedded at build time
        const char*pg=g_st_page; size_t pn=sizeof g_st_page-1; char*disk=0; const char*pf=getenv("PELOAD_STATS_PAGE");
        if(pf && *pf){ FILE*f=fopen(pf,"rb"); if(f){ fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
                if(n>0 && n<(8<<20) && (disk=malloc((size_t)n)) && fread(disk,1,(size_t)n,f)==(size_t)n){ pg=disk; pn=(size_t)n; } fclose(f); } }
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",pn);
        if(st_send(fd,h,(size_t)k)) st_send(fd,pg,pn); free(disk); }
    else { const char*nf="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,nf,strlen(nf)); }
    close(fd); return 0; }
// PELOAD_STATS_OPEN=1 (play.sh, quiet-run.sh): show the page when the game starts. A tab that is already open reconnects on
// its own (EventSource retries every few seconds) and reloads itself for the new game (the snapshot's "inst"), so the
// browser is only asked to open the page (macOS `open <url>`: a new tab, or the browser started) when no page request
// arrived within PELOAD_STATS_OPEN_WAIT seconds (default 5) of the server starting.
static void* st_open_thread(void*a){ (void)a; pthread_setname_np("shim-stats-open");
    const char*w=getenv("PELOAD_STATS_OPEN_WAIT"); double ws=w?atof(w):5.0; usleep((useconds_t)(ws*1e6));
    if(g_st_hits) return 0;
    const char*ad=getenv("PELOAD_STATS_ADDR"); char url[128]; snprintf(url,sizeof url,"http://%s:%d/",ad&&*ad&&strcmp(ad,"0.0.0.0")?ad:"127.0.0.1",g_st_port);
    char*argv[]={"/usr/bin/open",url,0}; pid_t pid; extern char**environ;
    if(posix_spawn(&pid,"/usr/bin/open",0,0,argv,environ)==0){ int st; waitpid(pid,&st,0); char m[200]; int k=snprintf(m,sizeof m,"  [stats] opened %s in the browser\n",url); (void)!write(2,m,k); }
    return 0; }
// One listening socket, close-on-exec: children (map picture / walking graph generators, the Workshop helper) used to
// inherit it, and one still running after its game was killed kept 8765 bound -- the next launch had no stats page
// (2026-10-18). The port is always PELOAD_STATS_HTTP (8765; no fallback port -- the user's choice): retried every
// second; a leftover generator holding it (python running re-tools/h3bsp.py or h3nav.py) is killed; another game holding
// it is only named in the log (it may be the user's own) and waited for.
static int st_bind(int port,const char*ad){ int ls=socket(AF_INET,SOCK_STREAM,0); if(ls<0) return -1; int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    fcntl(ls,F_SETFD,FD_CLOEXEC); struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)port); sa.sin_addr.s_addr=inet_addr(ad);
    if(bind(ls,(struct sockaddr*)&sa,sizeof sa) || listen(ls,16)){ int e=errno; close(ls); errno=e; return -1; } return ls; }
static void st_port_holder(int port){   // who listens on the port (lsof): kill a leftover generator, name anything else
    char cmd[200]; snprintf(cmd,sizeof cmd,"/usr/sbin/lsof -nP -iTCP:%d -sTCP:LISTEN -t 2>/dev/null",port);
    FILE*f=popen(cmd,"r"); if(!f) return; char line[64]; int pids[8], n=0; while(n<8 && fgets(line,sizeof line,f)){ int p2=atoi(line); if(p2>0 && p2!=getpid()) pids[n++]=p2; } pclose(f);
    for(int i=0;i<n;i++){ char pc[300]=""; snprintf(cmd,sizeof cmd,"/bin/ps -o command= -p %d 2>/dev/null",pids[i]); FILE*g=popen(cmd,"r"); if(g){ if(!fgets(pc,sizeof pc,g)) pc[0]=0; pclose(g); }
        size_t L=strlen(pc); while(L && (pc[L-1]=='\n'||pc[L-1]==' ')) pc[--L]=0;
        int gen=strstr(pc,"h3bsp.py") || strstr(pc,"h3nav.py") || strstr(pc,"maptop.sh");
        char m[460]; int k=snprintf(m,sizeof m,"  [stats] port %d is held by pid %d (%.300s)%s\n",port,pids[i],pc[0]?pc:"?",gen?": a leftover map generator, killing it":": waiting for it to close");
        (void)!write(2,m,k); if(gen) kill(pids[i],SIGKILL); } }
static void* st_listen_thread(void*a){ (void)a; pthread_setname_np("shim-stats-listen");
    // only the process that runs the engine serves (the client's launcher process inherits the variable too)
    while(!hb_halo3_base()) usleep(500000);
    // collection (snapshot: heatmap, kills, forensic roster ...) runs whether or not the page can be served
    { pthread_t t; pthread_create(&t,0,st_snap_thread,0); pthread_detach(t); }
    const char*ad=getenv("PELOAD_STATS_ADDR"); if(!ad||!*ad) ad="127.0.0.1";
    char m[200]; int k; int ls=-1, port=g_st_port;
    for(int tr=0; ls<0; tr++){
        ls=st_bind(g_st_port,ad); if(ls>=0) break;
        int e=errno;
        if(tr==0){ k=snprintf(m,sizeof m,"  [stats] cannot listen on %s:%d (%s), retrying every second\n",ad,g_st_port,strerror(e)); (void)!write(2,m,k); }
        if(e==EADDRINUSE && (tr==0 || tr%30==0)) st_port_holder(g_st_port);
        sleep(1); }
    g_st_port=port;
    k=snprintf(m,sizeof m,"  [stats] live game stats on http://%s:%d/ (also /stats.json, /events, /forensics)\n",ad,port); (void)!write(2,m,k);
    { const char*o=getenv("PELOAD_STATS_OPEN"); if(o && atoi(o)){ pthread_t ot; pthread_create(&ot,0,st_open_thread,0); pthread_detach(ot); } }
    for(;;){ int fd=accept(ls,0,0); if(fd<0){ if(errno==EINTR) continue; usleep(100000); continue; } fcntl(fd,F_SETFD,FD_CLOEXEC); pthread_t c; if(pthread_create(&c,0,st_conn,(void*)(intptr_t)fd)) close(fd); else pthread_detach(c); }
    return 0; }
static void stats_start(void){ if(!stats_on()) return; pthread_t t; pthread_create(&t,0,st_listen_thread,0); pthread_detach(t); }

// ---- PELOAD_TEST_BOOT_PROMPT=1: open halo3's native "boot the betrayer?" popup on demand (test hook) --------------
// halo3+0xfcc20 shows the popup while a player is dead (player +0x28 unit = NONE) and its record holds a betrayer
// handle at +0x1F0 (and the game variant has betrayal booting on). When the local player (the one whose biped held the
// camera; or PELOAD_TEST_BOOT_PROMPT=<its name>) is dead and has no betrayer, this names the first other player as the
// betrayer, once per death.
static void* st_bootprompt_thread(void*a){ (void)a; pthread_setname_np("shim-test-bootprompt"); int local=-1; uint32_t done_for=0xFFFFFFFFu;
    for(;;){ usleep(200000);
        uint8_t*h3=hb_halo3_base(); uint32_t idx=0; if(!h3 || !hb_read((uint64_t)(h3+0xa39f9c),&idx,4) || idx>=128) continue;
        void*blks[1025]; int na=tls_blocks(idx,blks,1025);
        uint64_t pd=0; int32_t es=0, hi=0;
        for(int q=0;q<na;q++){ uint64_t blk=(uint64_t)blks[q], parr=0, oarr=0; uint8_t hdr[0x60];
            if(!blk || !hb_read(blk+0x38,&oarr,8) || !oarr || !hb_read(blk+0x10,&parr,8) || !parr || !hb_read(parr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) continue;
            es=*(int32_t*)(hdr+0x24); hi=*(int32_t*)(hdr+0x3c); pd=*(uint64_t*)(hdr+0x48); break; }
        if(!pd || es<0x200 || hi<=0 || hi>256) continue;
        // the local player: its biped contains the camera
        float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); int camok=!(s0&1) && g_hb_vpseq==s0 && (vp[28]||vp[29]||vp[30]);
        uint64_t arr=hb_object_array(); uint8_t ohdr[0x60]; uint64_t odata=0; int ohi=0;
        if(arr && hb_read(arr,ohdr,sizeof ohdr) && *(uint32_t*)(ohdr+0x2c)==0x64407440){ odata=*(uint64_t*)(ohdr+0x48); ohi=*(int32_t*)(ohdr+0x3c); }
        const char*want=getenv("PELOAD_TEST_BOOT_PROMPT"); int byname=want && strcmp(want,"1");
        for(int i=0;i<hi && camok && odata && !byname;i++){ uint8_t rec[0x30]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue; uint32_t u=*(uint32_t*)(rec+0x28);
            if(u==0xFFFFFFFFu || (int)(u&0xFFFF)>=ohi) continue; uint8_t e[0x18]; uint64_t p=0; float c[4];
            if(!hb_read(odata+(uint64_t)(u&0xFFFF)*0x18,e,sizeof e) || *(uint16_t*)e!=(uint16_t)(u>>16) || !(p=*(uint64_t*)(e+0x10)) || !hb_read(p+0x1c,c,16)) continue;
            float dx=c[0]-vp[28],dy=c[1]-vp[29],dz=c[2]-vp[30]; if(dx*dx+dy*dy+dz*dz<(c[3]+0.3f)*(c[3]+0.3f)){ if(local!=i){ local=i; char m[80]; int k=snprintf(m,sizeof m,"  [bootprompt] local player = slot %d\n",i); (void)!write(2,m,k); } break; } }
        // PELOAD_TEST_BOOT_PROMPT=<name>: the local player by name (works before the first spawn, while still dead)
        { const char*nm=getenv("PELOAD_TEST_BOOT_PROMPT"); if(nm && strcmp(nm,"1")) for(int i=0;i<hi;i++){ uint8_t rec[0x98]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue;
              char n8[40]; int j=0; const uint16_t*w=(const uint16_t*)(rec+0x58); for(int c=0;c<32 && w[c] && j<38;c++) n8[j++]=(char)(w[c]<128?w[c]:'?'); n8[j]=0;
              if(!strcmp(n8,nm)){ if(local!=i){ local=i; char m[80]; int k=snprintf(m,sizeof m,"  [bootprompt] local player %s = slot %d\n",nm,i); (void)!write(2,m,k); } break; } } }
        if(local<0) continue;
        uint8_t*me=(uint8_t*)(uintptr_t)(pd+(uint64_t)local*es); uint32_t unit=*(volatile uint32_t*)(me+0x28), betrayer=*(volatile uint32_t*)(me+0x1F0);
        if(unit!=0xFFFFFFFFu){ done_for=0xFFFFFFFFu; continue; }   // alive
        if(betrayer!=0xFFFFFFFFu || done_for==1) continue;
        for(int i=0;i<hi;i++){ if(i==local) continue; uint16_t salt=*(volatile uint16_t*)(uintptr_t)(pd+(uint64_t)i*es); if(!salt) continue;
            uint32_t h=((uint32_t)salt<<16)|(uint32_t)i; *(volatile uint32_t*)(me+0x1F0)=h; done_for=1;
            char m[120]; int k=snprintf(m,sizeof m,"  [bootprompt] local player (slot %d) is dead: betrayer set to player %08x\n",local,h); (void)!write(2,m,k); break; } }
    return 0; }
static void bootprompt_test_start(void){ if(!getenv("PELOAD_TEST_BOOT_PROMPT")) return; pthread_t t; pthread_create(&t,0,st_bootprompt_thread,0); pthread_detach(t); }
