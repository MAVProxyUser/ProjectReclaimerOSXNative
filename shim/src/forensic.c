// forensic.c — forensic archive of the voice, chat and game events this client receives, and its viewer (/forensics).
// Included by peload.c right after stats.c: it uses stats.c's StBuf / sb_put / sb_str / st_send / st_wall and its
// session reader. Formats, endpoints, settings and the consent rules: shim/docs/forensics.md.
//
// Recorded (only what reaches this client -- packets its own voice and chat code receives, events it shows):
//   voice   every packet the client's voice_recv sees (stats.c st_on_voice, an entry hook that runs BEFORE the client's
//           own echo / block-list / mute / channel checks, so muted players are included): time, sender peer id,
//           channel, sequence, Opus length + TOC. The Opus bytes themselves only with "voice audio" on, which needs the
//           one-party-consent box; without it the archive keeps who talked when and no audio.
//   events  every stats.c feed event (st_event): chat with channel and sender peer id, [SERVER] lines, notices, joins,
//           leaves, kills, deaths, votes, match start / end -- plus the archive's own setting changes.
//   roster  every session member (peer id, name, team, player slot, host / self flags) whenever that list changes.
// Layout: <base>/<YYYYmmdd-HHMMSS>_<map>/{session.json, voice.bin, roster.jsonl, events.jsonl} and <base>/settings.json,
// base = $HOME/Library/Application Support/ReclaimerMac/forensics (PELOAD_FORENSIC_DIR overrides). A new session starts
// when the server (the session's host peer id) or the map changes, or when the client was out of any session for 5 s.
// Threads: the hooks append to an in-memory queue under one short lock (g_fxq_mx); fx_writer, a loader thread, writes
// it out once a second. With the archive off the hooks cost one flag check (g_fx_on), the snapshot thread one more.
// FX_TEST=1 (shim/tools/forensic-oggtest.c) compiles only the parts that need no engine: the Ogg writer, the archive,
// the readers and the HTTP side.
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#ifndef FX_TEST
#define FX_TEST 0
#endif

// ---- Ogg Opus (RFC 7845) ------------------------------------------------------------------------------------------
// The archive keeps the Opus packets exactly as received; the viewer wraps them into an Ogg stream on request (no
// re-encoding). A packet's length in samples comes from its TOC byte (RFC 6716 3.1): the config (bits 7..3) gives the
// frame duration, the code (bits 1..0) the frame count (0: one, 1 and 2: two, 3: the count in the next byte, bits 5..0).
static int opus_frame_samples(uint8_t toc){ int c=toc>>3;
    if(c<12){ static const int s[4]={480,960,1920,2880}; return s[c&3]; }   // SILK 10/20/40/60 ms
    if(c<16) return (c&1)?960:480;                                           // Hybrid 10/20 ms
    static const int s[4]={120,240,480,960}; return s[c&3]; }                // CELT 2.5/5/10/20 ms
static int opus_packet_samples(const uint8_t*p,size_t n){   // samples at 48 kHz; -1 = not an Opus packet
    if(!n) return -1; int f;
    switch(p[0]&3){ case 0: f=1; break; case 1: if((n-1)&1) return -1; f=2; break; case 2: f=2; if(n<2) return -1; break;   // code 1: two equal halves
        default: if(n<2) return -1; f=p[1]&0x3F; if(!f) return -1; }
    int s=f*opus_frame_samples(p[0]); return s>5760?-1:s; }                  // at most 120 ms per packet
static const char* opus_mode(uint8_t toc){ int c=toc>>3; return c<12?"SILK":c<16?"Hybrid":"CELT"; }
static const char* opus_bw(uint8_t toc){ int c=toc>>3;
    if(c<12) return c<4?"NB":c<8?"MB":"WB"; if(c<16) return c<14?"SWB":"FB"; return c<20?"NB":c<24?"WB":c<28?"SWB":"FB"; }
// 20 ms of digital silence: CELT fullband 20 ms mono, code 0, the range coder's silence flag set
static const uint8_t g_opus_silence[3]={0xF8,0xFF,0xFE};

static uint32_t g_ogg_crc[256]; static pthread_once_t g_ogg_crc_once=PTHREAD_ONCE_INIT;
static void ogg_crc_init(void){ for(uint32_t i=0;i<256;i++){ uint32_t r=i<<24; for(int k=0;k<8;k++) r=(r&0x80000000u)?(r<<1)^0x04C11DB7u:(r<<1); g_ogg_crc[i]=r; } }
static uint32_t ogg_crc(uint32_t c,const uint8_t*p,size_t n){ for(size_t i=0;i<n;i++) c=(c<<8)^g_ogg_crc[((c>>24)^p[i])&0xFF]; return c; }
// One logical stream, written to memory. Pages hold whole packets only (an Opus packet is < 61 KB, a page takes
// 255 x 255 bytes) and about a second of audio each; the granule position of a page is the number of samples decoded
// through its last packet, pre-skip included (RFC 7845 4).
typedef struct { uint8_t*b; size_t n, cap; uint32_t serial, pageno; int64_t gran, page_samples; uint8_t seg[255]; int nseg;
                 uint8_t*body; size_t bn, bcap; int npk, ok; } OggW;
static void ogg_grow(uint8_t**b,size_t*cap,size_t need,int*ok){ if(need<=*cap) return; size_t nc=*cap?*cap:4096; while(nc<need) nc*=2;
    uint8_t*nb=realloc(*b,nc); if(!nb){ *ok=0; return; } *b=nb; *cap=nc; }
static void ogg_page(OggW*w,int flags){   // flags: 2 first page (BOS), 4 last page (EOS)
    uint8_t h[27+255]; memcpy(h,"OggS",4); h[4]=0; h[5]=(uint8_t)flags; memcpy(h+6,&w->gran,8); memcpy(h+14,&w->serial,4); memcpy(h+18,&w->pageno,4);
    memset(h+22,0,4); h[26]=(uint8_t)w->nseg; memcpy(h+27,w->seg,(size_t)w->nseg); size_t hn=27+(size_t)w->nseg;
    uint32_t c=ogg_crc(ogg_crc(0,h,hn),w->body,w->bn); memcpy(h+22,&c,4);
    ogg_grow(&w->b,&w->cap,w->n+hn+w->bn,&w->ok); if(!w->ok) return; memcpy(w->b+w->n,h,hn); memcpy(w->b+w->n+hn,w->body,w->bn); w->n+=hn+w->bn;
    w->pageno++; w->nseg=0; w->bn=0; w->npk=0; w->page_samples=0; }
static int ogg_add(OggW*w,const uint8_t*p,size_t n){   // a whole packet into the page being built; 0 = no room
    if(w->nseg+(int)(n/255)+1>255) return 0;
    for(size_t r=n;;){ if(r>=255){ w->seg[w->nseg++]=255; r-=255; } else { w->seg[w->nseg++]=(uint8_t)r; break; } }   // a multiple of 255 ends in a 0
    ogg_grow(&w->body,&w->bcap,w->bn+n,&w->ok); if(!w->ok) return 1; memcpy(w->body+w->bn,p,n); w->bn+=n; w->npk++; return 1; }
static void oggw_begin(OggW*w,uint32_t serial,int channels,uint16_t preskip,const char*vendor,const char*const*com,int ncom){
    pthread_once(&g_ogg_crc_once,ogg_crc_init); memset(w,0,sizeof *w); w->ok=1; w->serial=serial;
    uint8_t hd[19]; memcpy(hd,"OpusHead",8); hd[8]=1; hd[9]=(uint8_t)(channels==2?2:1); memcpy(hd+10,&preskip,2);
    uint32_t rate=48000; memcpy(hd+12,&rate,4); hd[16]=0; hd[17]=0; hd[18]=0;   // output gain 0, mapping family 0
    ogg_add(w,hd,19); ogg_page(w,2);
    // OpusTags: "OpusTags", u32 vendor length + vendor, u32 comment count, each u32 length + "KEY=value" (UTF-8)
    uint8_t t[8192]; size_t tn=0; uint32_t k32;
    #define TPUT(p,l) do{ size_t l_=(size_t)(l); if(tn+l_<=sizeof t){ memcpy(t+tn,(p),l_); tn+=l_; } else w->ok=0; }while(0)
    TPUT("OpusTags",8); k32=(uint32_t)strlen(vendor); TPUT(&k32,4); TPUT(vendor,k32); k32=(uint32_t)ncom; TPUT(&k32,4);
    for(int i=0;i<ncom;i++){ k32=(uint32_t)strlen(com[i]); TPUT(&k32,4); TPUT(com[i],k32); }
    #undef TPUT
    if(w->ok && ogg_add(w,t,tn)) ogg_page(w,0); else w->ok=0; }   // it must start and end its own page (it does: < 8 KB)
static int oggw_packet(OggW*w,const uint8_t*p,size_t n){   // -> the packet's samples, -1 = skipped (not Opus)
    int s=opus_packet_samples(p,n); if(s<0) return -1;
    if(w->npk && (w->page_samples>=48000 || w->nseg+(int)(n/255)+1>255)) ogg_page(w,0);   // flush BEFORE adding: the last page always has packets for EOS
    ogg_add(w,p,n); w->gran+=s; w->page_samples+=s; return s; }
static void oggw_end(OggW*w){ if(w->npk) ogg_page(w,4); free(w->body); w->body=0; }

// ---- settings --------------------------------------------------------------------------------------------------------
static volatile int g_fx_on;      // archive on: the hooks' only check (stats.c declares it too)
static volatile int g_fx_audio;   // keep Opus bytes: voice audio on AND one-party consent confirmed
static int g_fx_env_off;          // PELOAD_FORENSIC=0
static struct { int archive, voice, consent; double consent_t; } g_fx_set={1,0,0,0};
static pthread_mutex_t g_fx_setmx=PTHREAD_MUTEX_INITIALIZER;
static char g_fx_base[900];
static uint64_t g_fx_self_id;     // our own peer id (roster tick): records from it get FXF_SELF
static void fx_mkdirs(const char*p){ char t[1100]; snprintf(t,sizeof t,"%s",p); for(char*q=t+1;*q;q++) if(*q=='/'){ *q=0; mkdir(t,0755); *q='/'; } mkdir(t,0755); }
static int fx_jbool(const char*j,const char*key,int*v){ char k[64]; snprintf(k,sizeof k,"\"%s\":",key); const char*p=j?strstr(j,k):0; if(!p) return 0;
    p+=strlen(k); while(*p==' ') p++; if(!strncmp(p,"true",4)){ *v=1; return 1; } if(!strncmp(p,"false",5)){ *v=0; return 1; } return 0; }
static int fx_jnum(const char*j,const char*key,double*v){ char k[64]; snprintf(k,sizeof k,"\"%s\":",key); const char*p=j?strstr(j,k):0; if(!p) return 0;
    p+=strlen(k); while(*p==' ') p++; char*e; double d=strtod(p,&e); if(e==p) return 0; *v=d; return 1; }
static char* fx_slurp(const char*path,size_t*n,size_t max){   // a whole file, NUL-terminated
    FILE*f=fopen(path,"rb"); if(!f) return 0; struct stat st; char*b=0;
    if(!fstat(fileno(f),&st) && st.st_size>=0 && (size_t)st.st_size<=max && (b=malloc((size_t)st.st_size+1))){
        size_t k=fread(b,1,(size_t)st.st_size,f); b[k]=0; if(n) *n=k; }
    fclose(f); return b; }
static void fx_iso(double t,char*o,size_t on){ time_t s=(time_t)t; struct tm lt; localtime_r(&s,&lt); strftime(o,on,"%Y-%m-%dT%H:%M:%S%z",&lt); }
static void fx_settings_save(void){   // (g_fx_setmx held)
    fx_mkdirs(g_fx_base); char p[1000], tp[1010]; snprintf(p,sizeof p,"%s/settings.json",g_fx_base); snprintf(tp,sizeof tp,"%s.tmp",p);
    char iso[40]=""; if(g_fx_set.consent) fx_iso(g_fx_set.consent_t,iso,sizeof iso);
    FILE*f=fopen(tp,"wb"); if(!f) return;
    fprintf(f,"{\"archive\":%s,\"voice_audio\":%s,\"one_party_consent\":%s,\"consent_confirmed_unix\":%.3f,\"consent_confirmed\":\"%s\"}\n",
        g_fx_set.archive?"true":"false",g_fx_set.voice?"true":"false",g_fx_set.consent?"true":"false",g_fx_set.consent?g_fx_set.consent_t:0.0,iso);
    fclose(f); rename(tp,p); }
static void fx_apply(void){ g_fx_audio=g_fx_set.voice && g_fx_set.consent; if(g_fx_env_off || !g_fx_set.archive) g_fx_on=0; }   // the roster tick turns it on
static void* fx_writer(void*a);
static pthread_once_t g_fx_wonce=PTHREAD_ONCE_INIT;
static void fx_writer_once(void){ fx_mkdirs(g_fx_base); pthread_t t; pthread_create(&t,0,fx_writer,0); pthread_detach(t); }
static void fx_writer_start(void){ pthread_once(&g_fx_wonce,fx_writer_once); }   // the first time the archive is on
static void fx_init_once(void){
    const char*e=getenv("PELOAD_FORENSIC"); g_fx_env_off=e && !strcmp(e,"0");
    const char*d=getenv("PELOAD_FORENSIC_DIR"), *h=getenv("HOME");
    if(d && *d) snprintf(g_fx_base,sizeof g_fx_base,"%s",d); else snprintf(g_fx_base,sizeof g_fx_base,"%s/Library/Application Support/ReclaimerMac/forensics",h?h:"/tmp");
    char p[1000]; snprintf(p,sizeof p,"%s/settings.json",g_fx_base); char*j=fx_slurp(p,0,65536);
    pthread_mutex_lock(&g_fx_setmx);
    if(j){ int v; double t; if(fx_jbool(j,"archive",&v)) g_fx_set.archive=v; if(fx_jbool(j,"voice_audio",&v)) g_fx_set.voice=v;
        if(fx_jbool(j,"one_party_consent",&v)) g_fx_set.consent=v; if(fx_jnum(j,"consent_confirmed_unix",&t)) g_fx_set.consent_t=t; free(j); }
    if(!g_fx_set.consent) g_fx_set.voice=0;   // voice audio is only ever on with consent on record
    fx_apply(); pthread_mutex_unlock(&g_fx_setmx);
    char m[1200]; int k;
    if(g_fx_env_off) k=snprintf(m,sizeof m,"  [forensic] archive off (PELOAD_FORENSIC=0)\n");
    else { if(g_fx_set.archive) fx_writer_start();
        k=snprintf(m,sizeof m,"  [forensic] archive %s, voice audio %s: %s (viewer: /forensics)\n",g_fx_set.archive?"on (chat, events, roster, who talked when)":"off",
            g_fx_audio?"on (one-party consent on record)":"off",g_fx_base); }
    (void)!write(2,m,(size_t)k); }
static pthread_once_t g_fx_once=PTHREAD_ONCE_INIT;
static void fx_init(void){ pthread_once(&g_fx_once,fx_init_once); }

// ---- the queue: hooks -> writer ---------------------------------------------------------------------------------------
// items: u8 kind, 3 pad, u32 length, then the bytes. FXI_SWITCH carries an FxSw (the next session; none = left it).
enum { FXI_VOICE=1, FXI_EVENT=2, FXI_ROSTER=3, FXI_SWITCH=4 };
typedef struct { char label[64], map[64], server[96]; uint64_t host, self; int none; } FxSw;
typedef struct { uint8_t*b; size_t n, cap; } FxQ;
static FxQ g_fxq, g_fxq_spare; static pthread_mutex_t g_fxq_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fxq_cv=PTHREAD_COND_INITIALIZER;
static uint64_t g_fx_dropped, g_fx_nvoice, g_fx_naudio, g_fx_nevent;
#define FX_QMAX ((size_t)64<<20)   // 64 MB pending (the writer stalled for a long time): newer records are dropped and counted
static int fx_push(int kind,const void*a,size_t an,const void*b,size_t bn){
    uint32_t len=(uint32_t)(an+bn); int ok=0; pthread_mutex_lock(&g_fxq_mx); size_t need=g_fxq.n+8+len;
    if(need<=FX_QMAX){ if(need>g_fxq.cap){ size_t nc=g_fxq.cap?g_fxq.cap:(1<<20); while(nc<need) nc*=2; uint8_t*nb=realloc(g_fxq.b,nc); if(nb){ g_fxq.b=nb; g_fxq.cap=nc; } }
        if(need<=g_fxq.cap){ uint8_t*p=g_fxq.b+g_fxq.n; p[0]=(uint8_t)kind; p[1]=p[2]=p[3]=0; memcpy(p+4,&len,4); if(an) memcpy(p+8,a,an); if(bn) memcpy(p+8+an,b,bn); g_fxq.n=need; ok=1;
            if(g_fxq.n>(4u<<20)) pthread_cond_signal(&g_fxq_cv); } }
    if(!ok) g_fx_dropped++;
    pthread_mutex_unlock(&g_fxq_mx); return ok; }

// ---- voice.bin ------------------------------------------------------------------------------------------------------
// 64-byte header, then one 40-byte record header per packet + its stored bytes; all little-endian. Packets come as the
// client's voice_recv gets them: [0] channel (1 global, 2 team, 3 proximity; 0 = a control packet from the voice
// server peer, 10 bytes long), [1..9] sequence (u64 LE; the receiver drops a packet whose sequence is <= the last one
// it accepted from that sender: exe+0xb2ca4d), [9..] Opus.
#define FX_SYNC 0xA55Au
#define FX_VMAX 4096
enum { FXF_AUDIO=1, FXF_CONTROL=2, FXF_TRUNC=4, FXF_SHORT=8, FXF_SELF=16, FXF_UNREAD=32 };
typedef struct { uint16_t sync; uint8_t ch, fl; uint16_t olen, slen; uint64_t t, id, seq; uint8_t toc, toc2; uint16_t rsv; uint32_t rsv2; } FxVRec;
typedef struct { char magic[8]; uint32_t ver, hsize; uint64_t created, self; uint32_t rsize, flags; uint8_t rsv[24]; } FxVHdr;
_Static_assert(sizeof(FxVRec)==40,"voice.bin record header is 40 bytes"); _Static_assert(sizeof(FxVHdr)==64,"voice.bin header is 64 bytes");
static int fx_hbread(uint64_t a,void*d,size_t n);
static uint64_t fx_now_ns(void){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec; }
// One packet (`payload` is guest memory: copied with a checked read). Called on the client's voice thread.
static void fx_voice_at(uint64_t tns,uint64_t sender,const uint8_t*payload,uint64_t len){
    uint8_t buf[9+FX_VMAX]; FxVRec r; memset(&r,0,sizeof r); r.sync=FX_SYNC; r.t=tns; r.id=sender; r.ch=0xFF;
    int audio=g_fx_audio; size_t want=len<(uint64_t)(9+(audio?FX_VMAX:64))?(size_t)len:(size_t)(9+(audio?FX_VMAX:64)), stored=0; const uint8_t*sp=buf;
    if(sender && sender==g_fx_self_id) r.fl|=FXF_SELF;
    if(!payload || !want || !fx_hbread((uint64_t)(uintptr_t)payload,buf,want)){ r.fl|=FXF_UNREAD; r.olen=(uint16_t)(len>65535?65535:len); }
    else if(len<9){ r.fl|=FXF_SHORT; r.ch=buf[0]; r.olen=(uint16_t)len; if(audio){ stored=want; r.fl|=FXF_AUDIO; } }   // malformed: kept raw (with audio on)
    else { r.ch=buf[0]; memcpy(&r.seq,buf+1,8); uint64_t ol=len-9; r.olen=(uint16_t)(ol>65535?65535:ol); sp=buf+9;
        if(ol){ r.toc=buf[9]; if(want>10) r.toc2=buf[10]; }
        if(r.ch==0){ r.fl|=FXF_CONTROL; stored=want-9; }                     // the voice server's control byte(s): not audio
        else if(audio && ol){ stored=want-9; r.fl|=FXF_AUDIO; if(stored<ol) r.fl|=FXF_TRUNC; } }
    r.slen=(uint16_t)stored; fx_push(FXI_VOICE,&r,sizeof r,sp,stored);
    __atomic_fetch_add(&g_fx_nvoice,1,__ATOMIC_RELAXED); if(r.fl&FXF_AUDIO) __atomic_fetch_add(&g_fx_naudio,1,__ATOMIC_RELAXED); }
static void fx_voice(uint64_t sender,const uint8_t*payload,uint64_t len){ fx_voice_at(fx_now_ns(),sender,payload,len); }

// ---- events.jsonl ---------------------------------------------------------------------------------------------------
static void fx_event_rec(double t,uint32_t seq,const char*kind,const char*a,const char*b,const char*how,const char*verb,int ta,int tb,int flags,const char*text,uint64_t sender){
    StBuf s={0}; sb_put(&s,"{\"t\":%.3f,\"seq\":%u,\"kind\":",t,seq); sb_str(&s,kind); sb_put(&s,",\"text\":"); sb_str(&s,text?text:"");
    if(a && a[0]){ sb_put(&s,",\"a\":"); sb_str(&s,a); } if(b && b[0]){ sb_put(&s,",\"b\":"); sb_str(&s,b); } if(how && how[0]){ sb_put(&s,",\"how\":"); sb_str(&s,how); }
    if(verb && verb[0]){ sb_put(&s,",\"verb\":"); sb_str(&s,verb); }
    if(ta>=0) sb_put(&s,",\"ta\":%d",ta); if(tb>=0) sb_put(&s,",\"tb\":%d",tb);
    if(flags&1) sb_put(&s,",\"estimated\":true"); if(flags&2) sb_put(&s,",\"mine\":true"); if(flags&4) sb_put(&s,",\"betrayal\":true");
    if(!strcmp(kind,"chat")){ const char*ch=!how||!how[0]?"all":how; sb_put(&s,",\"channel\":"); sb_str(&s,ch); }   // all, team, nearby, server, notice
    if(sender) sb_put(&s,",\"sender_id\":\"%016llx\"",(unsigned long long)sender);
    sb_put(&s,"}"); if(s.b){ fx_push(FXI_EVENT,s.b,s.n,0,0); __atomic_fetch_add(&g_fx_nevent,1,__ATOMIC_RELAXED); } free(s.b); }

// ---- roster.jsonl + session boundaries (the stats snapshot thread, every 100 ms) -----------------------------------------
typedef struct { uint64_t id; char name[64]; int peer, sidx, slot, team, host, self, muted; } FxMem;
static const char*const g_fx_teams[8]={"red","blue","green","orange","purple","gold","brown","pink"};
static struct { char key[200], cand[200]; double cand_t; int armed; uint64_t sig; } g_fxr;
static void fx_join_server(char*o,size_t on){   // the server of a reclaimer://join?server=<host:port> launch (play.sh RECLAIMER_JOIN)
    o[0]=0; const char*p=strstr(g_cmdlineA,"join?server="); if(!p) return; p+=12; size_t j=0;
    while(*p && *p!=' ' && *p!='"' && *p!='&' && j+1<on) o[j++]=*p++; o[j]=0; }
static void fx_roster_members(double now,const char*map,const FxMem*m,int n,uint64_t host_id,uint64_t self_id,int host_peer,int self_peer){
    pthread_mutex_lock(&g_fx_setmx); int on=!g_fx_env_off && g_fx_set.archive; pthread_mutex_unlock(&g_fx_setmx);
    if(!on){ g_fx_on=0; g_fxr.armed=0; return; }
    g_fx_self_id=self_id;
    int in=n>0 || host_id; char realmap[64]; snprintf(realmap,sizeof realmap,"%s",map && strcmp(map,"mainmenu")?map:"");
    char key[200]=""; if(in) snprintf(key,sizeof key,"%016llx|%s",(unsigned long long)host_id,realmap);
    int sw=0;
    if(!g_fxr.armed){ sw=strcmp(key,g_fxr.key)!=0; g_fxr.armed=1; snprintf(g_fxr.cand,sizeof g_fxr.cand,"%s",key); }   // archive (re)enabled: no debounce
    else if(strcmp(key,g_fxr.key)){
        if(strcmp(key,g_fxr.cand)){ snprintf(g_fxr.cand,sizeof g_fxr.cand,"%s",key); g_fxr.cand_t=now; }
        else if(now-g_fxr.cand_t>=(in?2.0:5.0)) sw=1; }   // stable for 2 s (5 s for "left the session"): map loads flicker
    else snprintf(g_fxr.cand,sizeof g_fxr.cand,"%s",key);
    if(sw){ FxSw s; memset(&s,0,sizeof s); s.none=!in; s.host=host_id; s.self=self_id; snprintf(s.map,sizeof s.map,"%s",realmap); fx_join_server(s.server,sizeof s.server);
        snprintf(s.label,sizeof s.label,"%s",realmap[0]?realmap:s.server[0]?s.server:"lobby");
        fx_push(FXI_SWITCH,&s,sizeof s,0,0); snprintf(g_fxr.key,sizeof g_fxr.key,"%s",key); g_fxr.sig=0; }
    pthread_mutex_lock(&g_fx_setmx); g_fx_on=!g_fx_env_off && g_fx_set.archive; pthread_mutex_unlock(&g_fx_setmx);   // (a page switch-off in between wins)
    if(!in) return;
    uint64_t sig=1469598103934665603ull;   // FNV-1a over what a roster line holds
    #define FXH(p,l) do{ const uint8_t*q_=(const uint8_t*)(p); for(size_t i_=0;i_<(size_t)(l);i_++){ sig^=q_[i_]; sig*=1099511628211ull; } }while(0)
    FXH(&host_id,8); FXH(&self_id,8); FXH(&host_peer,4); FXH(&self_peer,4); FXH(realmap,strlen(realmap));
    for(int i=0;i<n;i++){ FXH(&m[i].id,8); FXH(m[i].name,strlen(m[i].name)+1); FXH(&m[i].peer,4*7); }
    #undef FXH
    if(sig==g_fxr.sig) return; g_fxr.sig=sig;
    StBuf s={0}; sb_put(&s,"{\"t\":%.3f,\"map\":",now); sb_str(&s,realmap);
    sb_put(&s,",\"host_peer\":%d,\"host_id\":\"%016llx\",\"self_peer\":%d,\"self_id\":\"%016llx\",\"members\":[",host_peer,(unsigned long long)host_id,self_peer,(unsigned long long)self_id);
    for(int i=0;i<n;i++){ sb_put(&s,"%s{\"id\":\"%016llx\",\"name\":",i?",":"",(unsigned long long)m[i].id); sb_str(&s,m[i].name);
        sb_put(&s,",\"peer\":%d,\"index\":%d,\"slot\":%d,\"team\":%d,\"team_name\":\"%s\",\"host\":%s,\"self\":%s,\"muted\":%s}",m[i].peer,m[i].sidx,m[i].slot,m[i].team,
            m[i].team>=0&&m[i].team<8?g_fx_teams[m[i].team]:"none",m[i].host?"true":"false",m[i].self?"true":"false",m[i].muted?"true":"false"); }
    sb_put(&s,"]}"); if(s.b) fx_push(FXI_ROSTER,s.b,s.n,0,0); free(s.b); }

// ---- the writer thread ----------------------------------------------------------------------------------------------
static struct { int open; char name[128], dir[1100]; FILE*voice,*roster,*events; FxSw meta; double t0; } g_fxw;
static FxSw g_fxw_pend={"menu","","",0,0,0};   // what the next lazily opened session is (the last switch)
static pthread_mutex_t g_fxw_mx=PTHREAD_MUTEX_INITIALIZER;
static void fx_sanitize(const char*s,char*o,size_t on){ size_t j=0; int us=0;
    for(;*s && j+1<on && j<48;s++){ char c=*s; int ok=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.';
        if(ok){ o[j++]=c; us=0; } else if(!us && j){ o[j++]='_'; us=1; } }
    while(j && (o[j-1]=='_'||o[j-1]=='.')) j--; o[j]=0; if(!j) snprintf(o,on,"session"); }
static void fx_sess_json(double ended){   // (g_fxw_mx held)
    char p[1200]; snprintf(p,sizeof p,"%s/session.json",g_fxw.dir); FILE*f=fopen(p,"wb"); if(!f) return;
    char iso[40]; fx_iso(g_fxw.t0,iso,sizeof iso); StBuf s={0};
    sb_put(&s,"{\"name\":"); sb_str(&s,g_fxw.name); sb_put(&s,",\"label\":"); sb_str(&s,g_fxw.meta.label); sb_put(&s,",\"map\":"); sb_str(&s,g_fxw.meta.map);
    sb_put(&s,",\"server\":"); sb_str(&s,g_fxw.meta.server);
    sb_put(&s,",\"host_id\":\"%016llx\",\"self_id\":\"%016llx\",\"started_unix\":%.3f,\"started\":\"%s\",\"ended_unix\":",(unsigned long long)g_fxw.meta.host,(unsigned long long)g_fxw.meta.self,g_fxw.t0,iso);
    if(ended>0) sb_put(&s,"%.3f",ended); else sb_put(&s,"null");
    pthread_mutex_lock(&g_fx_setmx); sb_put(&s,",\"pid\":%d,\"voice_audio\":%s,\"one_party_consent\":%s,\"consent_confirmed_unix\":%.3f",(int)getpid(),g_fx_audio?"true":"false",
        g_fx_set.consent?"true":"false",g_fx_set.consent?g_fx_set.consent_t:0.0); pthread_mutex_unlock(&g_fx_setmx);
    sb_put(&s,",\"formats\":{\"voice\":\"voice.bin v1 (shim/docs/forensics.md)\",\"roster\":\"roster.jsonl\",\"events\":\"events.jsonl\"},\"recorder\":\"Reclaimer Mac loader (shim/src/forensic.c)\"}\n");
    if(s.b) fwrite(s.b,1,s.n,f); free(s.b); fclose(f); }
static void fx_sess_close(void){ if(!g_fxw.open) return;
    if(g_fxw.voice) fclose(g_fxw.voice); if(g_fxw.roster) fclose(g_fxw.roster); if(g_fxw.events) fclose(g_fxw.events);
    g_fxw.voice=g_fxw.roster=g_fxw.events=0; fx_sess_json(st_wall()); g_fxw.open=0;
    char m[300]; int k=snprintf(m,sizeof m,"  [forensic] session %s closed\n",g_fxw.name); (void)!write(2,m,(size_t)k); }
static int fx_sess_open(void){ if(g_fxw.open) return 1;
    double now=st_wall(); time_t ts=(time_t)now; struct tm lt; localtime_r(&ts,&lt); char tsb[32]; strftime(tsb,sizeof tsb,"%Y%m%d-%H%M%S",&lt);
    char lab[64]; fx_sanitize(g_fxw_pend.label,lab,sizeof lab); fx_mkdirs(g_fx_base);
    for(int k=1;k<100;k++){ if(k==1) snprintf(g_fxw.name,sizeof g_fxw.name,"%s_%s",tsb,lab); else snprintf(g_fxw.name,sizeof g_fxw.name,"%s_%s-%d",tsb,lab,k);
        snprintf(g_fxw.dir,sizeof g_fxw.dir,"%s/%s",g_fx_base,g_fxw.name); if(!mkdir(g_fxw.dir,0755)) break; if(errno!=EEXIST || k==99) return 0; }
    g_fxw.meta=g_fxw_pend; g_fxw.t0=now; char p[1200];
    snprintf(p,sizeof p,"%s/voice.bin",g_fxw.dir); g_fxw.voice=fopen(p,"ab");
    if(g_fxw.voice){ FxVHdr h; memset(&h,0,sizeof h); memcpy(h.magic,"RCVOICE\0",8); h.ver=1; h.hsize=sizeof h; h.created=(uint64_t)(now*1e9); h.self=g_fxw.meta.self;
        h.rsize=sizeof(FxVRec); h.flags=g_fx_audio?1:0; fwrite(&h,1,sizeof h,g_fxw.voice); }
    snprintf(p,sizeof p,"%s/roster.jsonl",g_fxw.dir); g_fxw.roster=fopen(p,"ab");
    snprintf(p,sizeof p,"%s/events.jsonl",g_fxw.dir); g_fxw.events=fopen(p,"ab");
    g_fxw.open=1; fx_sess_json(0);
    char m[1300]; int k=snprintf(m,sizeof m,"  [forensic] session %s\n",g_fxw.dir); (void)!write(2,m,(size_t)k); return 1; }
static void fx_process(const uint8_t*b,size_t n){   // (g_fxw_mx held)
    for(size_t o=0;o+8<=n;){ int kind=b[o]; uint32_t len; memcpy(&len,b+o+4,4); const uint8_t*p=b+o+8; o+=8+(size_t)len; if(o>n) break;
        if(kind==FXI_SWITCH && len==sizeof(FxSw)){ FxSw s; memcpy(&s,p,sizeof s); fx_sess_close(); g_fxw_pend=s; if(s.none) snprintf(g_fxw_pend.label,sizeof g_fxw_pend.label,"menu"); continue; }
        if(!fx_sess_open()) continue;
        FILE*f=kind==FXI_VOICE?g_fxw.voice:kind==FXI_ROSTER?g_fxw.roster:kind==FXI_EVENT?g_fxw.events:0; if(!f) continue;
        fwrite(p,1,len,f); if(kind!=FXI_VOICE) fputc('\n',f); } }
static void fx_flush(void){   // write out everything queued so far (the writer thread; HTTP readers before reading a live session)
    pthread_mutex_lock(&g_fxw_mx);
    pthread_mutex_lock(&g_fxq_mx); FxQ q=g_fxq; g_fxq=g_fxq_spare; g_fxq.n=0; pthread_mutex_unlock(&g_fxq_mx);
    if(q.n) fx_process(q.b,q.n); q.n=0; g_fxq_spare=q;
    if(g_fxw.voice) fflush(g_fxw.voice); if(g_fxw.roster) fflush(g_fxw.roster); if(g_fxw.events) fflush(g_fxw.events);
    pthread_mutex_unlock(&g_fxw_mx); }
static void* fx_writer(void*a){ (void)a; pthread_setname_np("shim-forensic");
    for(;;){ pthread_mutex_lock(&g_fxq_mx); struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=1;
        if(g_fxq.n<(4u<<20)) pthread_cond_timedwait(&g_fxq_cv,&g_fxq_mx,&ts); pthread_mutex_unlock(&g_fxq_mx); fx_flush(); }
    return 0; }

// ---- page controls: POST /control?do=forensic&archive=0|1&voice=0|1&consent=0|1 ---------------------------------------
// voice=1 is refused unless consent is on record (consent=1 in the same request counts). consent=0 also turns voice off.
static int fx_control(const char*qs){ fx_init(); if(g_fx_env_off) return 0;
    int a=-1, v=-1, c=-1; const char*p;
    if((p=strstr(qs,"archive="))) a=atoi(p+8)?1:0; if((p=strstr(qs,"voice="))) v=atoi(p+6)?1:0; if((p=strstr(qs,"consent="))) c=atoi(p+8)?1:0;
    if(a<0 && v<0 && c<0) return 0;
    pthread_mutex_lock(&g_fx_setmx);
    int consent=c>=0?c:g_fx_set.consent; if(v==1 && !consent){ pthread_mutex_unlock(&g_fx_setmx); return 0; }
    if(c==1 && !g_fx_set.consent){ g_fx_set.consent=1; g_fx_set.consent_t=st_wall(); }
    if(c==0){ g_fx_set.consent=0; g_fx_set.consent_t=0; g_fx_set.voice=0; }
    if(v>=0) g_fx_set.voice=v; if(a>=0) g_fx_set.archive=a;
    fx_settings_save(); fx_apply(); if(g_fx_set.archive) fx_writer_start();
    char iso[40]=""; if(g_fx_set.consent) fx_iso(g_fx_set.consent_t,iso,sizeof iso);
    char m[300]; snprintf(m,sizeof m,"archive %s, voice audio %s%s%s%s",g_fx_set.archive?"on":"off",g_fx_audio?"on":"off",
        g_fx_set.consent?" (one-party consent confirmed ":"",iso,g_fx_set.consent?")":" (no consent on record)");
    pthread_mutex_unlock(&g_fx_setmx);
    if(g_fx_on) fx_event_rec(st_wall(),0,"archive",0,0,0,0,-1,-1,0,m,0);   // the archive notes its own setting changes
    char lm[360]; int k=snprintf(lm,sizeof lm,"  [forensic] page: %s\n",m); (void)!write(2,lm,(size_t)k); return 1; }

// ---- reading a session back ---------------------------------------------------------------------------------------------
typedef struct { uint64_t t, id, seq; uint8_t ch, fl, toc, toc2; uint16_t olen, slen; size_t off; int utt; } FxPk;
typedef struct { uint64_t id, t0, t1, seq0, seq1; int ch, npk, naud, ntrunc, minlen, maxlen; long samples, bytes, step, lost; uint8_t toc; } FxUtt;
typedef struct { double t; char name[64]; int team, slot, host, self; } FxIdAt;
typedef struct { uint64_t id; FxIdAt*h; int n, cap; } FxIdHist;
typedef struct { char name[160]; uint8_t*buf; size_t n, poff; FxVHdr h; int hok; FxPk*pk; int np, pcap, bad; FxUtt*u; int nu; } FxSess;
static FxSess g_fxc; static pthread_mutex_t g_fxc_mx=PTHREAD_MUTEX_INITIALIZER;   // the last session read (one at a time)
static int fx_name_ok(const char*s,size_t n){ if(!n || n>=128 || s[0]=='.') return 0; for(size_t i=0;i<n;i++){ char c=s[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return 0; } return !strstr(s,".."); }
static int fx_pk_samples(const FxPk*k,const uint8_t*buf){   // from the stored bytes, else from the TOC bytes kept in the record
    if((k->fl&FXF_AUDIO) && k->slen && !(k->fl&FXF_SHORT)) return opus_packet_samples(buf+k->off,k->slen);
    if(!k->olen) return -1; uint8_t t2[2]={k->toc,k->toc2}; return opus_packet_samples(t2,k->olen); }
// Utterances: per sender, a run of packets on one channel whose arrival gaps stay under 0.6 s.
static void fx_segment(FxSess*s){
    free(s->u); s->u=0; s->nu=0; int cap=0; struct { uint64_t id; int u; uint64_t last; } open[256]; int no=0;
    for(int i=0;i<s->np;i++){ FxPk*k=&s->pk[i]; k->utt=-1; if(k->fl&(FXF_CONTROL|FXF_SHORT|FXF_UNREAD)) continue;
        int oi=-1; for(int j=0;j<no;j++) if(open[j].id==k->id){ oi=j; break; }
        FxUtt*u=oi>=0?&s->u[open[oi].u]:0;
        if(!u || u->ch!=k->ch || k->t<open[oi].last || k->t-open[oi].last>=600000000ull){
            if(s->nu>=cap){ int nc=cap?cap*2:256; FxUtt*nu=realloc(s->u,(size_t)nc*sizeof *nu); if(!nu) return; s->u=nu; cap=nc; }
            u=&s->u[s->nu]; memset(u,0,sizeof *u); u->id=k->id; u->ch=k->ch; u->t0=k->t; u->seq0=k->seq; u->minlen=1<<30; u->toc=k->toc;
            if(oi<0){ if(no<256) oi=no++; else oi=0; } open[oi].id=k->id; open[oi].u=s->nu++; }
        open[oi].last=k->t; k->utt=open[oi].u;
        int sm=fx_pk_samples(k,s->buf); if(sm<0) sm=0;
        if(u->npk){ uint64_t d=k->seq-u->seq1; if(d>1 && d<=1000) u->lost+=(long)(d-1); if(!u->step || d<(uint64_t)u->step) u->step=(long)d; }   // lost: GUESSED (a packet counter)
        u->npk++; u->seq1=k->seq; u->t1=k->t+(uint64_t)sm*1000000ull/48; u->samples+=sm; u->bytes+=k->olen;
        if(k->olen<u->minlen) u->minlen=k->olen; if(k->olen>u->maxlen) u->maxlen=k->olen;
        if(k->fl&FXF_AUDIO) u->naud++; if(k->fl&FXF_TRUNC) u->ntrunc++; } }
// (g_fxc_mx held) <session>/voice.bin -> g_fxc. The file only grows (append-only), so a session read before is
// brought up to date by reading and parsing just its new tail (the live session is asked for every few seconds).
static void fx_parse(FxSess*s){
    const FxVHdr*h=&s->h; uint64_t lo=h->created-86400ull*1000000000ull, hi=h->created+40ull*86400ull*1000000000ull; size_t o=s->poff;
    while(o+h->rsize<=s->n){ FxVRec r; memcpy(&r,s->buf+o,sizeof r);
        if(r.sync!=FX_SYNC || r.slen>9+FX_VMAX || r.t<lo || r.t>hi){   // damaged: skip to the next plausible record
            s->bad++; size_t q=o+1; for(;q+h->rsize<=s->n;q++){ FxVRec t2; memcpy(&t2,s->buf+q,sizeof t2); if(t2.sync==FX_SYNC && t2.slen<=9+FX_VMAX && t2.t>=lo && t2.t<=hi) break; } o=q; continue; }
        if(o+h->rsize+r.slen>s->n) break;   // the writer's last record is not complete yet
        if(s->np>=s->pcap){ int nc=s->pcap?s->pcap*2:4096; FxPk*np=realloc(s->pk,(size_t)nc*sizeof *np); if(!np) break; s->pk=np; s->pcap=nc; }
        FxPk*k=&s->pk[s->np++]; k->t=r.t; k->id=r.id; k->seq=r.seq; k->ch=r.ch; k->fl=r.fl; k->toc=r.toc; k->toc2=r.toc2; k->olen=r.olen; k->slen=r.slen; k->off=o+h->rsize; k->utt=-1;
        o+=h->rsize+r.slen; }
    s->poff=o; }
static int fx_load(const char*name){
    char p[1200]; snprintf(p,sizeof p,"%s/%s/voice.bin",g_fx_base,name); struct stat st; if(stat(p,&st) || st.st_size<0) return 0; size_t sz=(size_t)st.st_size;
    if(!g_fxc.buf || strcmp(g_fxc.name,name) || sz<g_fxc.n){ free(g_fxc.buf); free(g_fxc.pk); free(g_fxc.u); memset(&g_fxc,0,sizeof g_fxc); snprintf(g_fxc.name,sizeof g_fxc.name,"%s",name); }
    else if(sz==g_fxc.n) return 1;
    if(sz>((size_t)2<<30)) return 0;
    uint8_t*nb=realloc(g_fxc.buf,sz?sz:1); if(!nb) return 0; g_fxc.buf=nb;
    int fd=open(p,O_RDONLY); if(fd<0) return 0; size_t got=g_fxc.n;
    while(got<sz){ ssize_t k=pread(fd,g_fxc.buf+got,sz-got,(off_t)got); if(k<=0){ if(k<0 && errno==EINTR) continue; break; } got+=(size_t)k; }
    close(fd); g_fxc.n=got;
    if(!g_fxc.hok){ if(g_fxc.n<sizeof(FxVHdr)) return 1; memcpy(&g_fxc.h,g_fxc.buf,sizeof g_fxc.h);
        if(memcmp(g_fxc.h.magic,"RCVOICE\0",8) || g_fxc.h.ver!=1 || g_fxc.h.hsize<sizeof(FxVHdr) || g_fxc.h.rsize<sizeof(FxVRec)) return 1;
        g_fxc.hok=1; g_fxc.poff=g_fxc.h.hsize; }
    fx_parse(&g_fxc); fx_segment(&g_fxc); return 1; }
static int fx_jstr(const char*p,char*o,size_t on){   // a JSON string at p (p at the opening quote) -> UTF-8; returns chars consumed
    const char*s=p; size_t j=0; if(*p!='"'){ o[0]=0; return 0; } p++;
    while(*p && *p!='"'){ unsigned c=(unsigned char)*p++;
        if(c=='\\'){ char e=*p++; if(e=='u'){ unsigned u=(unsigned)strtoul((char[5]){p[0],p[1],p[2],p[3],0},0,16); p+=4; c=u; if(c>=0x80){
                    if(c<0x800){ if(j+2<on){ o[j++]=(char)(0xC0|(c>>6)); o[j++]=(char)(0x80|(c&63)); } } else if(j+3<on){ o[j++]=(char)(0xE0|(c>>12)); o[j++]=(char)(0x80|((c>>6)&63)); o[j++]=(char)(0x80|(c&63)); } continue; } }
            else c=e=='n'?'\n':e=='t'?'\t':e=='r'?'\r':e=='b'?'\b':e=='f'?'\f':(unsigned char)e; }
        if(j+1<on) o[j++]=(char)c; }
    o[j]=0; if(*p=='"') p++; return (int)(p-s); }
// roster.jsonl -> per peer id, what it was called (and its team / slot) from when on
static FxIdHist* fx_roster_load(const char*name,int*nh){
    char p[1200]; snprintf(p,sizeof p,"%s/%s/roster.jsonl",g_fx_base,name); char*j=fx_slurp(p,0,(size_t)512<<20); *nh=0; if(!j) return 0;
    FxIdHist*hs=0; int nhs=0, hcap=0;
    for(char*ln=j; ln && *ln; ){ char*e=strchr(ln,'\n'); if(e) *e=0; double t=0;
        if(fx_jnum(ln,"t",&t)) for(char*q=strstr(ln,"{\"id\":\""); q; q=strstr(q+7,"{\"id\":\"")){
            FxIdAt a; memset(&a,0,sizeof a); a.t=t; uint64_t id=strtoull(q+7,0,16); char*nm=strstr(q,"\"name\":"); if(!nm) break; fx_jstr(nm+7,a.name,sizeof a.name);
            char*r=strstr(nm,"\"slot\":"); a.slot=r?atoi(r+7):-1; r=strstr(nm,"\"team\":"); a.team=r?atoi(r+7):-1;
            r=strstr(nm,"\"host\":"); a.host=r&&!strncmp(r+7,"true",4); r=strstr(nm,"\"self\":"); a.self=r&&!strncmp(r+7,"true",4);
            int k=0; for(;k<nhs;k++) if(hs[k].id==id) break;
            if(k==nhs){ if(nhs>=hcap){ int nc=hcap?hcap*2:64; FxIdHist*nn=realloc(hs,(size_t)nc*sizeof *nn); if(!nn) break; hs=nn; hcap=nc; } memset(&hs[nhs],0,sizeof *hs); hs[nhs++].id=id; }
            FxIdHist*h=&hs[k]; FxIdAt*l=h->n?&h->h[h->n-1]:0;
            if(l && !strcmp(l->name,a.name) && l->team==a.team && l->slot==a.slot && l->host==a.host && l->self==a.self) continue;   // unchanged
            if(h->n>=h->cap){ int nc=h->cap?h->cap*2:4; FxIdAt*na=realloc(h->h,(size_t)nc*sizeof *na); if(!na) continue; h->h=na; h->cap=nc; } h->h[h->n++]=a; }
        ln=e?e+1:0; }
    free(j); *nh=nhs; return hs; }
static const FxIdAt* fx_who(const FxIdHist*hs,int nh,uint64_t id,double t){   // the entry in force at t (or the first one after)
    for(int k=0;k<nh;k++) if(hs[k].id==id){ const FxIdAt*b=hs[k].n?&hs[k].h[0]:0; for(int i=0;i<hs[k].n;i++) if(hs[k].h[i].t<=t+0.5) b=&hs[k].h[i]; return b; }
    return 0; }
static void fx_roster_free(FxIdHist*hs,int nh){ for(int k=0;k<nh;k++) free(hs[k].h); free(hs); }
static const char* fx_chname(int ch){ return ch==1?"global":ch==2?"team":ch==3?"proximity":ch==0?"control":"?"; }
static void fx_who_json(StBuf*s,const FxIdAt*w,uint64_t id){
    char nm[64]; if(w) snprintf(nm,sizeof nm,"%s",w->name); else snprintf(nm,sizeof nm,"unknown %016llx",(unsigned long long)id);
    sb_put(s,",\"name\":"); sb_str(s,nm); int tm=w?w->team:-1;
    sb_put(s,",\"team\":%d,\"team_name\":\"%s\",\"slot\":%d,\"host\":%s,\"self\":%s,\"known\":%s",tm,tm>=0&&tm<8?g_fx_teams[tm]:"none",w?w->slot:-1,w&&w->host?"true":"false",w&&w->self?"true":"false",w?"true":"false"); }
// /forensics/<s>/index.json: utterances (with who they were at the time), speakers, control packets, the events
static void fx_index_json(StBuf*s,const char*name){
    char p[1200]; snprintf(p,sizeof p,"%s/%s/session.json",g_fx_base,name); char*meta=fx_slurp(p,0,1<<20);
    while(meta && *meta && (meta[strlen(meta)-1]=='\n'||meta[strlen(meta)-1]==' ')) meta[strlen(meta)-1]=0;
    int nh=0; FxIdHist*hs=fx_roster_load(name,&nh); const FxSess*d=&g_fxc;
    sb_put(s,"{\"name\":"); sb_str(s,name); sb_put(s,",\"session\":%s",meta && meta[0]=='{'?meta:"null"); free(meta);
    uint64_t t0=0,t1=0; for(int i=0;i<d->np;i++){ if(!t0 || d->pk[i].t<t0) t0=d->pk[i].t; if(d->pk[i].t>t1) t1=d->pk[i].t; }
    long nctl=0, naud=0; for(int i=0;i<d->np;i++){ if(d->pk[i].fl&FXF_CONTROL) nctl++; if(d->pk[i].fl&FXF_AUDIO) naud++; }
    sb_put(s,",\"stats\":{\"packets\":%d,\"audio_packets\":%ld,\"control_packets\":%ld,\"bad_records\":%d,\"voice_bytes\":%zu,\"first\":%.3f,\"last\":%.3f}",d->np,naud,nctl,d->bad,d->n,t0/1e9,t1/1e9);
    sb_put(s,",\"utterances\":[");
    for(int i=0;i<d->nu;i++){ const FxUtt*u=&d->u[i]; double st=u->t0/1e9; const FxIdAt*w=fx_who(hs,nh,u->id,st);
        sb_put(s,"%s{\"n\":%d,\"id\":\"%016llx\",\"start\":%.3f,\"end\":%.3f,\"channel\":%d,\"channel_name\":\"%s\",\"packets\":%d,\"audio_packets\":%d,\"audio_s\":%.3f,\"bytes\":%ld",
            i?",":"",i,(unsigned long long)u->id,st,u->t1/1e9,u->ch,fx_chname(u->ch),u->npk,u->naud,u->samples/48000.0,u->bytes);
        sb_put(s,",\"seq\":[%llu,%llu],\"seq_step\":%ld,\"seq_gaps\":%ld,\"truncated\":%d,\"len\":[%d,%d]",(unsigned long long)u->seq0,(unsigned long long)u->seq1,u->step,u->step==1?u->lost:0,u->ntrunc,u->minlen==(1<<30)?0:u->minlen,u->maxlen);
        sb_put(s,",\"opus\":{\"toc\":%u,\"mode\":\"%s\",\"bandwidth\":\"%s\",\"frame_ms\":%.1f,\"stereo\":%s}",u->toc,opus_mode(u->toc),opus_bw(u->toc),opus_frame_samples(u->toc)/48.0,(u->toc>>2)&1?"true":"false");
        fx_who_json(s,w,u->id); sb_put(s,"}"); }
    sb_put(s,"],\"speakers\":[");   // one per sender, in order of their first utterance
    typedef struct { uint64_t id; int first; long nu, np, na; double secs, last; } FxSpk;
    FxSpk*sp=calloc((size_t)(d->nu?d->nu:1),sizeof *sp); int ns=0;
    for(int i=0;i<d->nu && sp;i++){ const FxUtt*u=&d->u[i]; int j=0; for(;j<ns;j++) if(sp[j].id==u->id) break;
        if(j==ns){ sp[ns].id=u->id; sp[ns].first=i; ns++; }
        sp[j].nu++; sp[j].np+=u->npk; sp[j].na+=u->naud; sp[j].secs+=u->samples/48000.0; if(u->t1/1e9>sp[j].last) sp[j].last=u->t1/1e9; }
    for(int j=0;j<ns;j++){ const FxUtt*u=&d->u[sp[j].first]; const FxIdAt*w=fx_who(hs,nh,u->id,u->t0/1e9);
        sb_put(s,"%s{\"id\":\"%016llx\",\"utterances\":%ld,\"packets\":%ld,\"audio_packets\":%ld,\"seconds\":%.3f,\"first\":%.3f,\"last\":%.3f",j?",":"",(unsigned long long)u->id,sp[j].nu,sp[j].np,sp[j].na,sp[j].secs,u->t0/1e9,sp[j].last);
        fx_who_json(s,w,u->id); sb_put(s,",\"names\":[");
        for(int k=0,f=0;k<nh;k++) if(hs[k].id==u->id) for(int q=0;q<hs[k].n;q++){ int dup=0; for(int r=0;r<q && !dup;r++) dup=!strcmp(hs[k].h[r].name,hs[k].h[q].name); if(dup) continue; sb_put(s,"%s",f++?",":""); sb_str(s,hs[k].h[q].name); }
        sb_put(s,"]}"); }
    free(sp);
    sb_put(s,"],\"control\":["); int nc=0;
    for(int i=0;i<d->np && nc<5000;i++){ const FxPk*k=&d->pk[i]; if(!(k->fl&FXF_CONTROL)) continue;
        sb_put(s,"%s{\"t\":%.3f,\"id\":\"%016llx\",\"seq\":%llu,\"code\":%d}",nc++?",":"",k->t/1e9,(unsigned long long)k->id,(unsigned long long)k->seq,k->slen?d->buf[k->off]:-1); }
    sb_put(s,"],\"people\":[");   // everyone the roster ever listed, voice or not
    for(int k=0;k<nh;k++){ const FxIdAt*w=hs[k].n?&hs[k].h[hs[k].n-1]:0; sb_put(s,"%s{\"id\":\"%016llx\"",k?",":"",(unsigned long long)hs[k].id); fx_who_json(s,w,hs[k].id); sb_put(s,",\"names\":[");
        for(int q=0;q<hs[k].n;q++){ int dup=0; for(int r=0;r<q && !dup;r++) dup=!strcmp(hs[k].h[r].name,hs[k].h[q].name); if(!dup){ if(q) sb_put(s,","); sb_str(s,hs[k].h[q].name); } } sb_put(s,"]}"); }
    sb_put(s,"],\"events\":[");
    snprintf(p,sizeof p,"%s/%s/events.jsonl",g_fx_base,name); char*ev=fx_slurp(p,0,(size_t)256<<20); int ne=0;
    for(char*ln=ev; ln && *ln; ){ char*e=strchr(ln,'\n'); if(e) *e=0; size_t L=strlen(ln); if(L>1 && ln[0]=='{' && ln[L-1]=='}') sb_put(s,"%s%s",ne++?",":"",ln); ln=e?e+1:0; }
    free(ev); sb_put(s,"]}"); fx_roster_free(hs,nh); }
// An Ogg Opus file of packets: a whole utterance (uttn >= 0), or every utterance of one sender (id) with each gap
// between two of them shortened to at most 1 s of silence. Deterministic (serial from the name), so range requests of
// the same file agree.
static int fx_ogg(OggW*w,const char*name,int uttn,uint64_t id){
    memset(w,0,sizeof *w); const FxSess*d=&g_fxc; int first=-1; for(int i=0;i<d->np && first<0;i++){ const FxPk*k=&d->pk[i]; if(k->utt<0 || !(k->fl&FXF_AUDIO) || (k->fl&FXF_TRUNC)) continue;
        if(uttn>=0?k->utt==uttn:k->id==id) first=i; }
    if(first<0) return 0;
    const FxUtt*u0=&d->u[d->pk[first].utt]; int nh=0; FxIdHist*hs=fx_roster_load(name,&nh); const FxIdAt*w0=fx_who(hs,nh,u0->id,u0->t0/1e9);
    char who[64]; if(w0) snprintf(who,sizeof who,"%s",w0->name); else snprintf(who,sizeof who,"unknown %016llx",(unsigned long long)u0->id);
    char iso[40]; fx_iso(u0->t0/1e9,iso,sizeof iso); char c[8][200]; const char*cp[8]; int nc=0;
    snprintf(c[nc++],200,"TITLE=%s, %s, %s",who,uttn>=0?fx_chname(u0->ch):"all utterances",iso); snprintf(c[nc++],200,"ARTIST=%s",who);
    snprintf(c[nc++],200,"RECLAIMER_SENDER_ID=%016llx",(unsigned long long)u0->id); snprintf(c[nc++],200,"RECLAIMER_SESSION=%s",name);
    snprintf(c[nc++],200,"RECLAIMER_START_UNIX=%.3f",u0->t0/1e9); if(uttn>=0) snprintf(c[nc++],200,"RECLAIMER_CHANNEL=%s",fx_chname(u0->ch));
    snprintf(c[nc++],200,"RECLAIMER_NOTE=Opus packets exactly as this client received them, put into Ogg, not re-encoded%s",uttn<0?"; gaps between utterances shortened to at most 1 s of silence":"");
    for(int i=0;i<nc;i++) cp[i]=c[i];
    uint32_t serial=2166136261u; for(const char*q=name;*q;q++){ serial^=(uint8_t)*q; serial*=16777619u; } serial^=(uint32_t)(uttn>=0?(uint32_t)uttn*2654435761u:(uint32_t)(id^(id>>32)));
    const uint8_t*f=d->buf+d->pk[first].off; oggw_begin(w,serial,(f[0]>>2)&1?2:1,0,"Reclaimer Mac forensic archive",cp,nc);
    int lastu=-1; uint64_t lastend=0;
    for(int i=first;i<d->np;i++){ const FxPk*k=&d->pk[i]; if(k->utt<0 || !(k->fl&FXF_AUDIO) || (k->fl&FXF_TRUNC)) continue; if(uttn>=0?k->utt!=uttn:k->id!=id) continue;
        if(uttn<0 && lastu>=0 && k->utt!=lastu){ const FxUtt*u=&d->u[k->utt]; double gap=u->t0>lastend?(u->t0-lastend)/1e9:0; if(gap>1.0) gap=1.0;
            for(int z=0;z<(int)(gap/0.02);z++) oggw_packet(w,g_opus_silence,sizeof g_opus_silence); }
        oggw_packet(w,d->buf+k->off,k->slen); if(k->utt!=lastu){ lastu=k->utt; } lastend=d->u[k->utt].t1; }
    oggw_end(w); fx_roster_free(hs,nh); return w->ok; }

// ---- HTTP (stats.c st_conn routes GET /forensics...) ----------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const char g_fx_page[]={
#embed "forensic_page.html"
,0};
#pragma clang diagnostic pop
// a response; binary ones honour a "Range: bytes=a-b" request (Safari's <audio> asks for ranges)
static void fx_reply(int fd,int code,const char*ct,const void*b,size_t n,const char*req,const char*dlname){
    char h[700]; size_t a=0, e=n?n-1:0; int part=0, k;
    const char*r=req?strcasestr(req,"\r\nRange:"):0;
    if(r && code==200 && n){ r+=8; while(*r==' ') r++; if(!strncasecmp(r,"bytes=",6)){ r+=6; char*x; unsigned long long s=0, t=n-1;
            if(*r=='-'){ unsigned long long sfx=strtoull(r+1,&x,10); if(x>r+1 && sfx){ s=sfx>=n?0:n-sfx; part=1; } }
            else { s=strtoull(r,&x,10); if(x>r && *x=='-'){ part=1; if(x[1]>='0'&&x[1]<='9') t=strtoull(x+1,0,10); if(t>=n) t=n-1; } }
            if(part && s>=n){ k=snprintf(h,sizeof h,"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%zu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",n); st_send(fd,h,(size_t)k); return; }
            if(part && t>=s){ a=(size_t)s; e=(size_t)t; } else part=0; } }
    char dl[300]=""; if(dlname){ char sn[200]; size_t j=0; for(const char*q=dlname;*q && j+1<sizeof sn;q++){ char c=*q; sn[j++]=((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_')?c:'_'; } sn[j]=0;
        snprintf(dl,sizeof dl,"Content-Disposition: attachment; filename=\"%s\"\r\n",sn); }
    const char*msg=code==200?(part?"Partial Content":"OK"):code==404?"Not Found":code==400?"Bad Request":"Error";
    k=snprintf(h,sizeof h,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\n%s",part?206:code,msg,ct,n?e-a+1:0,dl);
    if(part) k+=snprintf(h+k,sizeof h-(size_t)k,"Content-Range: bytes %zu-%zu/%zu\r\n",a,e,n);
    k+=snprintf(h+k,sizeof h-(size_t)k,"Connection: close\r\n\r\n");
    if(st_send(fd,h,(size_t)k) && n) st_send(fd,(const char*)b+a,e-a+1); }
static void fx_reply_json_err(int fd,int code,const char*msg){ char b[300]; int k=snprintf(b,sizeof b,"{\"ok\":false,\"error\":\"%s\"}",msg); fx_reply(fd,code,"application/json",b,(size_t)k,0,0); }
static int fx_name_cmp(const void*a,const void*b){ return strcmp((const char*)a,(const char*)b); }
static void fx_sessions_json(StBuf*s){
    pthread_mutex_lock(&g_fx_setmx); char iso[40]=""; if(g_fx_set.consent) fx_iso(g_fx_set.consent_t,iso,sizeof iso);
    sb_put(s,"{\"settings\":{\"archive\":%s,\"voice_audio\":%s,\"one_party_consent\":%s,\"consent_confirmed_unix\":%.3f,\"consent_confirmed\":\"%s\",\"disabled\":%s,\"recording\":%s,\"recording_audio\":%s}",
        g_fx_set.archive?"true":"false",g_fx_set.voice?"true":"false",g_fx_set.consent?"true":"false",g_fx_set.consent?g_fx_set.consent_t:0.0,iso,g_fx_env_off?"true":"false",g_fx_on?"true":"false",g_fx_on&&g_fx_audio?"true":"false");
    pthread_mutex_unlock(&g_fx_setmx);
    pthread_mutex_lock(&g_fxw_mx); sb_put(s,",\"current\":"); sb_str(s,g_fxw.open?g_fxw.name:""); pthread_mutex_unlock(&g_fxw_mx);
    sb_put(s,",\"counters\":{\"voice_packets\":%llu,\"audio_packets\":%llu,\"events\":%llu,\"dropped\":%llu},\"dir\":",(unsigned long long)g_fx_nvoice,(unsigned long long)g_fx_naudio,(unsigned long long)g_fx_nevent,(unsigned long long)g_fx_dropped);
    sb_str(s,g_fx_base); sb_put(s,",\"sessions\":[");
    DIR*dr=opendir(g_fx_base); char (*names)[128]=0; int nn=0, cap=0;
    if(dr){ struct dirent*de; while((de=readdir(dr))){ size_t L=strlen(de->d_name); if(!fx_name_ok(de->d_name,L) || !strchr(de->d_name,'_')) continue;
            char p[1200]; struct stat st; snprintf(p,sizeof p,"%s/%s/session.json",g_fx_base,de->d_name); if(stat(p,&st)) continue;
            if(nn>=cap){ int nc=cap?cap*2:64; void*np=realloc(names,(size_t)nc*128); if(!np) break; names=np; cap=nc; } snprintf(names[nn++],128,"%s",de->d_name); }
        closedir(dr); }
    if(nn>1) qsort(names,(size_t)nn,128,fx_name_cmp);
    for(int i=nn-1,f=0;i>=0;i--){ char p[1200]; char*meta; struct stat st; off_t sz[3]={0,0,0}; double last=0; static const char*const fn[3]={"voice.bin","events.jsonl","roster.jsonl"};
        for(int q=0;q<3;q++){ snprintf(p,sizeof p,"%s/%s/%s",g_fx_base,names[i],fn[q]); if(!stat(p,&st)){ sz[q]=st.st_size; double mt=st.st_mtimespec.tv_sec+st.st_mtimespec.tv_nsec*1e-9; if(mt>last) last=mt; } }
        snprintf(p,sizeof p,"%s/%s/session.json",g_fx_base,names[i]); meta=fx_slurp(p,0,1<<20); while(meta && *meta && (meta[strlen(meta)-1]=='\n')) meta[strlen(meta)-1]=0;
        sb_put(s,"%s{\"name\":",f++?",":""); sb_str(s,names[i]); sb_put(s,",\"voice_bytes\":%lld,\"events_bytes\":%lld,\"roster_bytes\":%lld,\"last_write\":%.3f,\"session\":%s}",(long long)sz[0],(long long)sz[1],(long long)sz[2],last,meta&&meta[0]=='{'?meta:"null");
        free(meta); }
    free(names); sb_put(s,"]}"); }
static void fx_http(int fd,const char*path,const char*qs,const char*req){
    fx_init(); const char*rest=path+10;   // after "/forensics"
    if(!*rest || !strcmp(rest,"/")){   // the viewer: forensic_page.html next to PELOAD_STATS_PAGE in a checkout (edits show on refresh), else embedded
        const char*pg=g_fx_page; size_t pn=sizeof g_fx_page-1; char*disk=0; const char*sp=getenv("PELOAD_STATS_PAGE"); char fp[1100]="";
        if(getenv("PELOAD_FORENSIC_PAGE")) snprintf(fp,sizeof fp,"%s",getenv("PELOAD_FORENSIC_PAGE"));
        else if(sp && *sp){ snprintf(fp,sizeof fp,"%s",sp); char*sl=strrchr(fp,'/'); if(sl) snprintf(sl+1,sizeof fp-(size_t)(sl+1-fp),"forensic_page.html"); else fp[0]=0; }
        size_t dn=0; if(fp[0] && (disk=fx_slurp(fp,&dn,8<<20))){ pg=disk; pn=dn; }
        fx_reply(fd,200,"text/html; charset=utf-8",pg,pn,0,0); free(disk); return; }
    if(!strcmp(rest,"/sessions.json")){ StBuf b={0}; fx_sessions_json(&b); fx_reply(fd,200,"application/json",b.b,b.n,0,0); free(b.b); return; }
    if(*rest!='/'){ fx_reply_json_err(fd,404,"not found"); return; }
    const char*sn=rest+1, *sl=strchr(sn,'/'); if(!sl || !fx_name_ok(sn,(size_t)(sl-sn))){ fx_reply_json_err(fd,404,"no such session"); return; }
    char name[128]; snprintf(name,sizeof name,"%.*s",(int)(sl-sn),sn); const char*what=sl+1; int dl=strstr(qs,"dl=1")!=0;
    char p[1200]; snprintf(p,sizeof p,"%s/%s/session.json",g_fx_base,name); struct stat st; if(stat(p,&st)){ fx_reply_json_err(fd,404,"no such session"); return; }
    pthread_mutex_lock(&g_fxw_mx); int live=g_fxw.open && !strcmp(g_fxw.name,name); pthread_mutex_unlock(&g_fxw_mx); if(live) fx_flush();   // the session being written: everything so far
    static const char*const raw[4]={"events.jsonl","roster.jsonl","voice.bin","session.json"};
    for(int i=0;i<4;i++) if(!strcmp(what,raw[i])){ snprintf(p,sizeof p,"%s/%s/%s",g_fx_base,name,raw[i]); size_t n=0; char*b=fx_slurp(p,&n,(size_t)2<<30);
        char dn[200]; snprintf(dn,sizeof dn,"%s_%s",name,raw[i]);
        if(!b) fx_reply_json_err(fd,404,"no such file"); else fx_reply(fd,200,i==2?"application/octet-stream":i==3?"application/json":"application/x-ndjson",b,n,req,dl||i==2?dn:0); free(b); return; }
    pthread_mutex_lock(&g_fxc_mx);
    if(!fx_load(name)){ pthread_mutex_unlock(&g_fxc_mx); fx_reply_json_err(fd,404,"no voice.bin"); return; }
    if(!strcmp(what,"index.json")){ StBuf b={0}; fx_index_json(&b,name); pthread_mutex_unlock(&g_fxc_mx); fx_reply(fd,200,"application/json",b.b,b.n,0,0); free(b.b); return; }
    int un=-1; uint64_t id=0; char ext[8]="";
    if(!strncmp(what,"utt/",4)){ char*x; long v=strtol(what+4,&x,10); if(x>what+4 && v>=0 && v<g_fxc.nu) un=(int)v; snprintf(ext,sizeof ext,"%s",x); }
    else if(!strncmp(what,"speaker/",8)){ char*x; id=strtoull(what+8,&x,16); if(x-(what+8)!=16) id=0; snprintf(ext,sizeof ext,"%s",x); }
    if((un<0 && !id) || (strcmp(ext,".ogg") && strcmp(ext,".json"))){ pthread_mutex_unlock(&g_fxc_mx); fx_reply_json_err(fd,404,"not found"); return; }
    if(!strcmp(ext,".json")){   // every packet of the utterance / speaker, its Opus TOC decoded
        StBuf b={0}; sb_put(&b,"{\"packets\":["); int f=0;
        for(int i=0;i<g_fxc.np;i++){ const FxPk*k=&g_fxc.pk[i]; if(k->utt<0 || (un>=0?k->utt!=un:k->id!=id)) continue; int sm=fx_pk_samples(k,g_fxc.buf);
            sb_put(&b,"%s{\"t\":%.6f,\"id\":\"%016llx\",\"utt\":%d,\"channel\":%d,\"seq\":%llu,\"len\":%u,\"stored\":%u,\"toc\":%u,\"mode\":\"%s\",\"bandwidth\":\"%s\",\"frame_ms\":%.1f,\"stereo\":%s,\"code\":%d,\"ms\":%.1f,\"flags\":%u}",
                f++?",":"",k->t/1e9,(unsigned long long)k->id,k->utt,k->ch,(unsigned long long)k->seq,k->olen,k->slen,k->toc,opus_mode(k->toc),opus_bw(k->toc),opus_frame_samples(k->toc)/48.0,(k->toc>>2)&1?"true":"false",k->toc&3,sm>0?sm/48.0:0.0,k->fl);
            if(f>=20000) break; }
        sb_put(&b,"]}"); pthread_mutex_unlock(&g_fxc_mx); fx_reply(fd,200,"application/json",b.b,b.n,0,0); free(b.b); return; }
    OggW w; int ok=fx_ogg(&w,name,un,id); pthread_mutex_unlock(&g_fxc_mx);
    if(!ok){ free(w.b); fx_reply_json_err(fd,404,"no audio stored here (voice audio was off, or nothing was received)"); return; }
    char dn[200]; if(un>=0) snprintf(dn,sizeof dn,"%s_utt%d.ogg",name,un); else snprintf(dn,sizeof dn,"%s_speaker_%016llx.ogg",name,(unsigned long long)id);
    fx_reply(fd,200,"audio/ogg",w.b,w.n,req,dl?dn:0); free(w.b); }

#if !FX_TEST
// ---- the glue to stats.c / the engine ----------------------------------------------------------------------------------
static int fx_hbread(uint64_t a,void*d,size_t n){ return hb_read(a,d,n); }
// one feed event (stats.c st_eventv, after it is in the ring); a chat line's sender peer id rides in t_st_chat_id
static void fx_event(const StEvt*e){ fx_event_rec(e->t,e->seq,e->kind,e->a,e->b,e->how,e->verb,e->ta,e->tb,e->flags,e->text,!strcmp(e->kind,"chat")?t_st_chat_id:0); }
static uint64_t fx_peer_id(int pi){   // peer pi's id (reclaimer-voice-mute.md: peer records at sess+0x58 / +0x1381c0, stride 0x110)
    if(pi<0 || g_st_layout<0) return 0; uint8_t*h3=hb_halo3_base(); uint64_t sess=0, id=0; uint8_t qm[16]={0}; int nq=g_st_layout?128:17;
    if(pi>=nq || !h3 || !hb_read((uint64_t)(h3+0x1e250d0),&sess,8) || !sess || !hb_read(sess+0x54,qm,g_st_layout?16:4) || !((qm[pi>>3]>>(pi&7))&1)) return 0;
    hb_read(sess+(g_st_layout?0x1381c0:0x58)+(uint64_t)pi*0x110,&id,8); return id; }
// The stats snapshot (every 100 ms): the session members and the players table -> roster.jsonl / session boundaries.
static void fx_roster_tick(const char*map,const StSess*ss,int nss,const StPlayer*pl,int np){
    fx_init(); if(g_fx_env_off || !g_fx_set.archive){ g_fx_on=0; g_fxr.armed=0; return; }   // off: nothing else runs
    static FxMem m[128]; int n=0;
    for(int i=0;i<nss && n<128;i++){ FxMem*e=&m[n++]; memset(e,0,sizeof *e); e->id=ss[i].voice; snprintf(e->name,sizeof e->name,"%s",ss[i].name); e->peer=ss[i].peer; e->sidx=ss[i].idx;
        e->host=ss[i].host; e->self=g_st_localpeer>=0 && ss[i].peer==g_st_localpeer; e->muted=ss[i].muted; e->slot=-1; e->team=-1;
        const StPlayer*p=0; for(int k=0;k<np && !p;k++) if(pl[k].slot==ss[i].idx && !strcmp(pl[k].name,ss[i].name)) p=&pl[k];   // as st_snapshot pairs them
        for(int k=0;k<np && !p;k++) if(!strcmp(pl[k].name,ss[i].name)) p=&pl[k];
        if(p){ e->slot=p->slot; for(int t=0;t<g_ntrk;t++) if(g_trk[t].slot==p->slot && !strcmp(g_trk[t].name,p->name)){ e->team=g_trk[t].team; break; } } }   // last team seen on its biped
    fx_roster_members(st_wall(),map,m,n,fx_peer_id(g_st_hostpeer),fx_peer_id(g_st_localpeer),g_st_hostpeer,g_st_localpeer); }
#endif
