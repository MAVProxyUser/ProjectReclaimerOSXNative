// forensic-oggtest.c — offline test of shim/src/forensic.c (run shim/tools/forensic-oggtest.sh; no game, no loader).
// It compiles forensic.c with FX_TEST=1 (the Ogg writer, the archive writer/readers and the HTTP side, without the
// engine glue) against small copies of the stats.c helpers it uses.
//   forensic-oggtest demux  <in.ogg>                 print the stream's packets (count, samples, pre-skip), check CRCs
//   forensic-oggtest direct <in.ogg> <out.ogg>       re-wrap in.ogg's Opus packets with the writer (same pre-skip), then
//                                                    read the result back: CRCs, packets byte-identical, granules
//   forensic-oggtest silence <out.ogg> <frames>      <frames> x the 20 ms silence packet
//   forensic-oggtest pipeline <in.ogg> <dir>         the whole archive: settings + consent, roster, voice packets from
//                                                    in.ogg as 4 speakers, events, then every /forensics endpoint;
//                                                    writes <dir>/expect.txt ("<file> <samples>") for the script
//   forensic-oggtest serve <in.ogg> <dir> <port>     the pipeline, then serve /forensics on 127.0.0.1:<port> to look at
//                                                    the viewer in a browser (PELOAD_STATS_PAGE=<checkout>/shim/src/
//                                                    stats_page.html serves forensic_page.html from disk)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#define FX_TEST 1
// ---- what forensic.c takes from stats.c / peload.c (copies) ----
typedef struct { char*b; size_t n, cap; } StBuf;
static void sb_put(StBuf*s,const char*fmt,...){ va_list ap; for(;;){ size_t room=s->cap-s->n; va_start(ap,fmt); int k=vsnprintf(s->b?s->b+s->n:0,room,fmt,ap); va_end(ap);
        if(k<0) return; if((size_t)k<room){ s->n+=(size_t)k; return; } size_t nc=(s->cap?s->cap*2:16384); while(nc<s->n+(size_t)k+1) nc*=2; char*nb=realloc(s->b,nc); if(!nb) return; s->b=nb; s->cap=nc; } }
static void sb_str(StBuf*s,const char*v){ sb_put(s,"\""); for(const unsigned char*p=(const unsigned char*)v; p&&*p; p++){
        if(*p=='"'||*p=='\\') sb_put(s,"\\%c",*p); else if(*p<0x20) sb_put(s,"\\u%04x",*p); else sb_put(s,"%c",*p); } sb_put(s,"\""); }
static double st_wall(void){ struct timeval tv; gettimeofday(&tv,0); return tv.tv_sec+tv.tv_usec*1e-6; }
static int st_send(int fd,const char*b,size_t n){ while(n){ ssize_t k=send(fd,b,n,0); if(k<=0){ if(k<0&&errno==EINTR) continue; return 0; } b+=k; n-=(size_t)k; } return 1; }
static char g_cmdlineA[2048]="\"C:\\client.exe\" reclaimer://join?server=203.0.113.7:11774";
#include "../src/forensic.c"
static int fx_hbread(uint64_t a,void*d,size_t n){ memcpy(d,(const void*)(uintptr_t)a,n); return 1; }   // the "guest" is this process

static int g_fail;
#define CHECK(c,...) do{ if(c){ printf("ok   "); } else { printf("FAIL "); g_fail=1; } printf(__VA_ARGS__); printf("\n"); }while(0)

// ---- an Ogg demuxer (independent of the writer: checks its CRCs against ours) ----
typedef struct { uint8_t*p; size_t n; int64_t gran_after; } Pk;
static uint8_t* slurp(const char*f,size_t*n){ FILE*fp=fopen(f,"rb"); if(!fp) return 0; fseek(fp,0,SEEK_END); long L=ftell(fp); fseek(fp,0,SEEK_SET); uint8_t*b=malloc((size_t)L+1); *n=fread(b,1,(size_t)L,fp); fclose(fp); return b; }
static int demux(const uint8_t*b,size_t n,Pk**out,int*badcrc,int*pages,int*eos_last){
    pthread_once(&g_ogg_crc_once,ogg_crc_init); Pk*pk=0; int np=0, cap=0; uint8_t*cur=0; size_t cn=0; *badcrc=0; *pages=0; *eos_last=0;
    for(size_t o=0;o+27<=n;){ if(memcmp(b+o,"OggS",4)) return -1; int ns=b[o+26]; size_t hl=27+(size_t)ns, bl=0; for(int i=0;i<ns;i++) bl+=b[o+27+i]; if(o+hl+bl>n) return -1;
        uint8_t h[27+255]; memcpy(h,b+o,hl); uint32_t want; memcpy(&want,h+22,4); memset(h+22,0,4); uint32_t c=ogg_crc(ogg_crc(0,h,hl),b+o+hl,bl); if(c!=want) (*badcrc)++;
        int64_t g; memcpy(&g,b+o+6,8); size_t bo=o+hl; (*pages)++; *eos_last=(b[o+5]&4)!=0;
        for(int i=0;i<ns;i++){ int L=b[o+27+i]; cur=realloc(cur,cn+(size_t)L+1); memcpy(cur+cn,b+bo,(size_t)L); cn+=(size_t)L; bo+=(size_t)L;
            if(L<255){ if(np>=cap){ cap=cap?cap*2:256; pk=realloc(pk,(size_t)cap*sizeof *pk); } pk[np].p=cur; pk[np].n=cn; pk[np].gran_after=g; np++; cur=0; cn=0; } }
        o+=hl+bl; }
    free(cur); *out=pk; return np; }

static int cmd_demux(const char*in){ size_t n; uint8_t*b=slurp(in,&n); if(!b){ printf("FAIL cannot read %s\n",in); return 1; }
    Pk*pk; int bad, pages, eos; int np=demux(b,n,&pk,&bad,&pages,&eos); if(np<2){ printf("FAIL %s: not an Ogg stream\n",in); return 1; }
    long sum=0; for(int i=2;i<np;i++){ int s=opus_packet_samples(pk[i].p,pk[i].n); if(s>0) sum+=s; }
    uint16_t pre; memcpy(&pre,pk[0].p+10,2); printf("%s: %d audio packets, %ld samples, pre-skip %u, %d pages, %d CRC errors, last page EOS %d, channels %d\n",in,np-2,sum,pre,pages,bad,eos,pk[0].p[9]);
    return bad?1:0; }
// re-wrap in.ogg's packets; returns the summed samples (or -1)
static long rewrap(const char*in,const char*out,int expect_ok){
    size_t n; uint8_t*b=slurp(in,&n); if(!b){ printf("FAIL cannot read %s\n",in); g_fail=1; return -1; }
    Pk*pk; int bad, pages, eos; int np=demux(b,n,&pk,&bad,&pages,&eos);
    CHECK(np>2 && !memcmp(pk[0].p,"OpusHead",8) && !memcmp(pk[1].p,"OpusTags",8),"%s: Ogg Opus with %d audio packets",in,np-2);
    CHECK(bad==0,"%s: our CRC-32 agrees with ffmpeg's on all %d pages",in,pages); if(np<3) return -1;
    uint16_t pre; memcpy(&pre,pk[0].p+10,2); int ch=pk[0].p[9];
    const char*com[2]={"TITLE=forensic-oggtest","RECLAIMER_NOTE=re-wrapped"}; OggW w; oggw_begin(&w,0x5EED0001u,ch,pre,"forensic-oggtest",com,2);
    long sum=0; int skipped=0; for(int i=2;i<np;i++){ int s=oggw_packet(&w,pk[i].p,pk[i].n); if(s>0) sum+=s; else skipped++; } oggw_end(&w);
    CHECK(!skipped,"%s: the writer took all %d packets (TOC sample counts valid)",out,np-2);
    CHECK(w.ok && ((pk[2].p[0]>>2)&1)+1==ch,"%s: writer OK, channel count %d from the first packet's TOC agrees with the source",out,ch);
    FILE*f=fopen(out,"wb"); fwrite(w.b,1,w.n,f); fclose(f);
    Pk*rk; int rbad, rpages, reos; int rn=demux(w.b,w.n,&rk,&rbad,&rpages,&reos);
    int same=rn==np; for(int i=2;i<rn && same;i++) same=rk[i].n==pk[i].n && !memcmp(rk[i].p,pk[i].p,pk[i].n);
    CHECK(rbad==0 && reos && same,"%s: %d pages read back, CRCs valid, EOS on the last page, %d packets byte-identical",out,rpages,rn-2);
    uint16_t rpre; memcpy(&rpre,rk[0].p+10,2); uint32_t rate; memcpy(&rate,rk[0].p+12,4);
    CHECK(rk[0].n==19 && rk[0].p[8]==1 && rpre==pre && rate==48000,"%s: OpusHead version 1, pre-skip %u, input rate %u",out,rpre,rate);
    long acc=0; int gok=1; for(int i=2;i<rn;i++){ acc+=opus_packet_samples(rk[i].p,rk[i].n); if(i+1==rn || rk[i+1].gran_after!=rk[i].gran_after) gok&=rk[i].gran_after==acc; }
    CHECK(gok && rk[rn-1].gran_after==sum,"%s: every page's granule = samples through its last packet (final %ld)",out,sum);
    (void)expect_ok; free(w.b); return sum; }
static int cmd_silence(const char*out,int frames){ OggW w; oggw_begin(&w,7,1,0,"forensic-oggtest",0,0);
    for(int i=0;i<frames;i++) oggw_packet(&w,g_opus_silence,sizeof g_opus_silence); oggw_end(&w);
    FILE*f=fopen(out,"wb"); fwrite(w.b,1,w.n,f); fclose(f); printf("%s: %d silence frames, %ld samples\n",out,frames,(long)w.gran); return 0; }

// ---- HTTP over a socketpair ----
typedef struct { int fd; StBuf r; } Rd;
static void* rd_thread(void*a){ Rd*r=a; char b[65536]; for(;;){ ssize_t k=recv(r->fd,b,sizeof b,0); if(k<=0) break; size_t at=r->r.n; sb_put(&r->r,"%*s",(int)k,""); memcpy(r->r.b+at,b,(size_t)k); } return 0; }
typedef struct { int code; char*body; size_t n; char hdr[2048]; } Resp;
static Resp get(const char*path,const char*qs,const char*extra){
    int sv[2]; socketpair(AF_UNIX,SOCK_STREAM,0,sv); Rd r={sv[1],{0}}; pthread_t t; pthread_create(&t,0,rd_thread,&r);
    char req[1024]; snprintf(req,sizeof req,"GET %s%s%s HTTP/1.1\r\nHost: x\r\n%s\r\n",path,qs&&*qs?"?":"",qs?qs:"",extra?extra:"");
    char p[256]; snprintf(p,sizeof p,"%s",path); fx_http(sv[0],p,qs?qs:"",req); shutdown(sv[0],SHUT_WR); close(sv[0]); pthread_join(t,0); close(sv[1]);
    Resp o; memset(&o,0,sizeof o); if(!r.r.b) return o; char*e=strstr(r.r.b,"\r\n\r\n"); if(!e) return o;
    size_t hl=(size_t)(e-r.r.b); snprintf(o.hdr,sizeof o.hdr,"%.*s",(int)hl,r.r.b); sscanf(r.r.b,"HTTP/1.1 %d",&o.code);
    o.n=r.r.n-hl-4; o.body=malloc(o.n+1); memcpy(o.body,e+4,o.n); o.body[o.n]=0; free(r.r.b); return o; }
static void save(const char*dir,const char*name,const Resp*r){ char p[1200]; snprintf(p,sizeof p,"%s/%s",dir,name); FILE*f=fopen(p,"wb"); if(f){ fwrite(r->body,1,r->n,f); fclose(f); } }

// the client's payload: [0] channel, [1..9] sequence, [9] frame count (1 in every live packet seen), [10..] one Opus packet
static void payload(uint8_t*o,size_t*on,int ch,uint64_t seq,const uint8_t*opus,size_t n){ o[0]=(uint8_t)ch; memcpy(o+1,&seq,8); o[9]=1; memcpy(o+10,opus,n); *on=10+n; }
static int cmd_pipeline(const char*in,const char*dir){
    size_t fn; uint8_t*fb=slurp(in,&fn); Pk*pk; int bad, pages, eos; int np=demux(fb,fn,&pk,&bad,&pages,&eos); if(np<260){ printf("FAIL %s: need >= 258 audio packets\n",in); return 1; }
    Pk*a=pk+2; int na=np-2; int ps=opus_packet_samples(a[0].p,a[0].n), same=1; for(int i=0;i<na;i++) same&=opus_packet_samples(a[i].p,a[i].n)==ps;
    if(!same || ps<=0){ printf("FAIL %s: the pipeline test needs packets of one length\n",in); return 1; }
    char fd[1100]; snprintf(fd,sizeof fd,"%s/forensics",dir); setenv("PELOAD_FORENSIC_DIR",fd,1); unsetenv("PELOAD_FORENSIC");
    fx_init();
    CHECK(g_fx_set.consent && g_fx_set.voice,"a fresh archive starts with one-party consent and voice audio on (the user's default)");
    CHECK(fx_control("do=forensic&consent=0") && !g_fx_set.voice,"withdrawing consent turns voice audio off");
    CHECK(!fx_control("do=forensic&voice=1"),"voice audio refused without one-party consent");
    CHECK(fx_control("do=forensic&consent=1&voice=1") && g_fx_audio,"consent + voice audio accepted");
    { char p[1200]; snprintf(p,sizeof p,"%s/settings.json",fd); char*j=fx_slurp(p,0,1<<20); int c=0, v=0; double t=0;
      CHECK(j && fx_jbool(j,"one_party_consent",&c) && c && fx_jbool(j,"voice_audio",&v) && v && fx_jnum(j,"consent_confirmed_unix",&t) && t>1e9,"settings.json persists consent with its time"); free(j); }
    const uint64_t A=0x1111111111111111ull, B=0x2222222222222222ull, ME=0x3333333333333333ull, HOST=0x9999999999999999ull;
    FxMem m[3]; memset(m,0,sizeof m);
    m[0].id=A; snprintf(m[0].name,64,"Alice"); m[0].peer=1; m[0].sidx=0; m[0].slot=0; m[0].team=0;
    m[1].id=B; snprintf(m[1].name,64,"Bob \"the\" \\builder\\"); m[1].peer=2; m[1].sidx=1; m[1].slot=1; m[1].team=1;
    m[2].id=ME; snprintf(m[2].name,64,"Me"); m[2].peer=3; m[2].sidx=2; m[2].slot=2; m[2].team=1; m[2].self=1;
    double now=st_wall(); fx_roster_members(now,"riverworld",m,3,HOST,ME,0,3);
    CHECK(g_fx_on,"archive on after the first roster tick");
    uint64_t T=fx_now_ns(); const uint64_t MS=1000000ull; uint8_t pl[9+4096]; size_t pn; int ai=0;
    #define V(t,id,ch,seq,i) do{ payload(pl,&pn,ch,seq,a[(i)%na].p,a[(i)%na].n); fx_voice_at(T+(t),id,pl,pn); }while(0)
    for(int i=0;i<150;i++,ai++) V(i*20*MS,A,1,1000+i,ai);                                   // utt 0: Alice global, 3 s
    { uint8_t c[10]={0}; uint64_t s=5; memcpy(c+1,&s,8); c[9]=7; fx_voice_at(T+3100*MS,HOST,c,10); }   // a control packet from the voice server
    for(int i=0,s=1;i<100;i++,s++,ai++){ if(i==60) s++; V(5000*MS+i*20*MS+(i>=50?300*MS:0),B,2,s,ai); }   // utt 1: Bob team, 0.3 s pause, one sequence missing
    { uint8_t sh[5]={2,1,2,3,4}; fx_voice_at(T+8000*MS,B,sh,5); }                         // malformed (shorter than the header)
    for(int i=0;i<50;i++,ai++) V(10000*MS+i*20*MS,A,3,1150+i,ai);                          // utt 2: Alice proximity
    V(12000*MS,ME,1,77,ai); ai++;                                                          // utt 3: our own echo
    fx_event_rec(st_wall(),1,"chat","Alice",0,"team",0,-1,-1,0,"hello team",A);
    fx_event_rec(st_wall(),2,"kill","Alice","Bob \"the\" \\builder\\","Battle rifle","killed",0,1,0,"Alice killed Bob (Battle rifle)",0);
    fx_event_rec(st_wall(),3,"chat",0,0,"server",0,-1,-1,0,"[SERVER] welcome",0);
    m[1].team=0; fx_roster_members(now+30,"riverworld",m,3,HOST,ME,0,3);                   // Bob changes team later: a second roster line
    CHECK(fx_control("do=forensic&voice=0") && !g_fx_audio,"voice audio off again (consent stays on record)");
    for(int i=0;i<25;i++,ai++) V(15000*MS+i*20*MS,A,1,1200+i,ai);                          // utt 4: metadata only
    fx_flush();
    FILE*ex=0; { char p[1200]; snprintf(p,sizeof p,"%s/expect.txt",dir); ex=fopen(p,"w"); }
    Resp r=get("/forensics","",0); CHECK(r.code==200 && strstr(r.body,"Reclaimer forensic archive"),"GET /forensics serves the viewer (%zu bytes)",r.n);
    r=get("/forensics/sessions.json","",0); save(dir,"sessions.json",&r); CHECK(r.code==200 && strstr(r.body,"_riverworld\""),"GET /forensics/sessions.json lists the session");
    char sname[128]; pthread_mutex_lock(&g_fxw_mx); snprintf(sname,sizeof sname,"%s",g_fxw.name); pthread_mutex_unlock(&g_fxw_mx);
    CHECK(strstr(sname,"_riverworld")!=0,"session named %s",sname);
    char base[300]; snprintf(base,sizeof base,"/forensics/%s",sname); char path[300];
    snprintf(path,sizeof path,"%s/index.json",base); r=get(path,"",0); save(dir,"index.json",&r);
    int nu=0; for(char*q=r.body;(q=strstr(q,"{\"n\":"));q++) nu++;
    CHECK(r.code==200 && nu==5,"index.json: %d utterances (want 5)",nu);
    CHECK(strstr(r.body,"\"name\":\"Alice\"") && strstr(r.body,"\"name\":\"Bob \\\"the\\\" \\\\builder\\\\\""),"index.json: names from the roster (with JSON escapes)");
    CHECK(strstr(r.body,"\"seq_gaps\":1")!=0,"index.json: Bob's missing sequence number counted");
    CHECK(strstr(r.body,"\"code\":7")!=0 && strstr(r.body,"\"control_packets\":1"),"index.json: the control packet");
    CHECK(strstr(r.body,"\"sender_id\":\"1111111111111111\"") && strstr(r.body,"\"channel\":\"team\""),"index.json: chat with sender id and channel");
    CHECK(strstr(r.body,"builder\\\\\",\"team\":1,\"team_name\":\"blue\"")!=0,"index.json: Bob was blue when he talked (before his team change)");
    CHECK(strstr(r.body,"builder\\\\\",\"team\":0,\"team_name\":\"red\"")!=0,"index.json: people lists Bob as red (his latest roster entry)");
    CHECK(strstr(r.body,"\"self\":true")!=0,"index.json: our own echo marked self");
    snprintf(path,sizeof path,"%s/utt/0.ogg",base); r=get(path,"",0); save(dir,"utt0.ogg",&r);
    CHECK(r.code==200 && r.n>1000 && !memcmp(r.body,"OggS",4),"utt/0.ogg: %zu bytes",r.n); if(ex) fprintf(ex,"utt0.ogg %ld\n",150L*ps);
    Resp full=r;
    r=get(path,"","Range: bytes=0-99\r\n"); CHECK(r.code==206 && r.n==100 && !memcmp(r.body,full.body,100) && strstr(r.hdr,"Content-Range: bytes 0-99/"),"utt/0.ogg Range bytes=0-99 -> 206, 100 bytes");
    r=get(path,"","Range: bytes=-50\r\n"); CHECK(r.code==206 && r.n==50 && !memcmp(r.body,full.body+full.n-50,50),"utt/0.ogg Range bytes=-50 -> the last 50 bytes");
    { char rg[80]; snprintf(rg,sizeof rg,"Range: bytes=%zu-\r\n",full.n+10); r=get(path,"",rg); CHECK(r.code==416,"utt/0.ogg Range past the end -> 416"); }
    r=get(path,"",0); CHECK(r.n==full.n && !memcmp(r.body,full.body,r.n),"utt/0.ogg is byte-identical on a second request (range requests agree)");
    snprintf(path,sizeof path,"%s/utt/1.ogg",base); r=get(path,"",0); save(dir,"utt1.ogg",&r); CHECK(r.code==200,"utt/1.ogg (Bob, team)"); if(ex) fprintf(ex,"utt1.ogg %ld\n",100L*ps);
    snprintf(path,sizeof path,"%s/utt/4.ogg",base); r=get(path,"",0); CHECK(r.code==404,"utt/4.ogg -> 404 (voice audio was off: metadata only)");
    snprintf(path,sizeof path,"%s/utt/0.json",base); r=get(path,"",0); save(dir,"utt0.json",&r);
    int npk=0; for(char*q=r.body;(q=strstr(q,"{\"t\":"));q++) npk++; CHECK(r.code==200 && npk==150,"utt/0.json: %d packets",npk);
    snprintf(path,sizeof path,"%s/speaker/%016llx.ogg",base,(unsigned long long)A); r=get(path,"dl=1",0); save(dir,"speakerA.ogg",&r);
    CHECK(r.code==200 && strstr(r.hdr,"Content-Disposition: attachment"),"speaker/<Alice>.ogg?dl=1 as an attachment");
    if(ex) fprintf(ex,"speakerA.ogg %ld\n",200L*ps+50L*960);   // utt 0 + 1 s of silence + utt 2 (utt 4 kept no audio)
    snprintf(path,sizeof path,"%s/events.jsonl",base); r=get(path,"",0); save(dir,"events.jsonl",&r);
    int nev=0; for(char*q=r.body;*q;q++) nev+=*q=='\n'; CHECK(r.code==200 && nev==4,"events.jsonl: %d lines (3 events + the voice-off setting note)",nev);
    snprintf(path,sizeof path,"%s/roster.jsonl",base); r=get(path,"",0); save(dir,"roster.jsonl",&r);
    nev=0; for(char*q=r.body;*q;q++) nev+=*q=='\n'; CHECK(r.code==200 && nev==2,"roster.jsonl: %d lines",nev);
    r=get("/forensics/..%2f/index.json","",0); CHECK(r.code==404,"a bad session name -> 404");
    // a torn record (the process died mid-write), then more packets: the reader skips the damage and goes on
    { char p[1200]; snprintf(p,sizeof p,"%s/%s/voice.bin",fd,sname); FILE*f=fopen(p,"ab"); fwrite("\x5a\xa5garb",1,7,f); fclose(f); }
    snprintf(path,sizeof path,"%s/index.json",base); r=get(path,"",0); CHECK(r.code==200 && strstr(r.body,"\"bad_records\":0"),"a torn tail is not read yet (incomplete)");
    CHECK(fx_control("do=forensic&voice=1") && g_fx_audio,"voice audio on again");
    for(int i=0;i<30;i++,ai++) V(20000*MS+i*20*MS,B,1,500+i,ai);                           // utt 5, after the damage
    fx_flush(); r=get(path,"",0); save(dir,"index2.json",&r); nu=0; for(char*q=r.body;(q=strstr(q,"{\"n\":"));q++) nu++;
    CHECK(r.code==200 && nu==6 && strstr(r.body,"\"bad_records\":1"),"incremental read: %d utterances, the damaged record skipped",nu);
    snprintf(path,sizeof path,"%s/utt/5.ogg",base); r=get(path,"",0); save(dir,"utt5.ogg",&r); CHECK(r.code==200,"utt/5.ogg after the damage"); if(ex) fprintf(ex,"utt5.ogg %ld\n",30L*ps);
    // a new map on the same server: the same session (sessions follow the server's host peer id)
    fx_roster_members(now+2,"guardian",m,3,HOST,ME,0,3); fx_roster_members(now+4.5,"guardian",m,3,HOST,ME,0,3); fx_flush();
    CHECK(g_fxw.open && strstr(g_fxw.name,"_riverworld"),"map change on the same server: the session stays open");
    // another server (host peer id): a new session after the 2 s debounce
    const uint64_t HOST2=0x8888888888888888ull;
    fx_roster_members(now+5,"guardian",m,3,HOST2,ME,0,3); fx_roster_members(now+6,"guardian",m,3,HOST2,ME,0,3); fx_flush();
    CHECK(g_fxw.open && strstr(g_fxw.name,"_riverworld"),"server change, 1 s later: still debouncing (the old session stays open)");
    fx_roster_members(now+7.5,"guardian",m,3,HOST2,ME,0,3); fx_flush();
    fx_event_rec(st_wall(),4,"game","guardian",0,0,0,-1,-1,0,"Match started: guardian",0); fx_flush();
    r=get("/forensics/sessions.json","",0); save(dir,"sessions2.json",&r);
    CHECK(strstr(r.body,"_guardian\"") && strstr(r.body,"_riverworld\""),"server change -> a second session");
    { char p[1200]; snprintf(p,sizeof p,"%s/%s/session.json",fd,sname); char*j=fx_slurp(p,0,1<<20); double e=0;
      CHECK(j && fx_jnum(j,"ended_unix",&e) && e>0 && strstr(j,"\"server\":\"203.0.113.7:11774\"") && strstr(j,"\"host_id\":\"9999999999999999\""),"the first session.json: ended, server from the join link, host peer id"); free(j); }
    CHECK(fx_control("do=forensic&archive=0") && !g_fx_on,"archive off: the hooks' flag is 0");
    if(ex) fclose(ex);
    printf("%s\n",g_fail?"SOME CHECKS FAILED":"all pipeline checks passed"); return g_fail; }

// serve <in.ogg> <dir> <port>: run the pipeline into <dir>, then answer /forensics... (and POST /control?do=forensic)
// on 127.0.0.1:<port> the way stats.c's st_conn does -- to look at the viewer page in a browser without a game.
#include <netinet/in.h>
#include <arpa/inet.h>
static int cmd_serve(const char*in,const char*dir,int port){
    cmd_pipeline(in,dir); fx_control("do=forensic&archive=1");
    int ls=socket(AF_INET,SOCK_STREAM,0), one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)port); sa.sin_addr.s_addr=inet_addr("127.0.0.1");
    if(bind(ls,(struct sockaddr*)&sa,sizeof sa) || listen(ls,16)){ perror("listen"); return 1; }
    printf("serving http://127.0.0.1:%d/forensics\n",port); fflush(stdout);
    for(;;){ int fd=accept(ls,0,0); if(fd<0) continue; char req[2048]; ssize_t n=recv(fd,req,sizeof req-1,0); if(n<=0){ close(fd); continue; } req[n]=0;
        char meth[8]="", path[256]="/", qs[128]=""; sscanf(req,"%7s %255s",meth,path); char*q=strchr(path,'?'); if(q){ snprintf(qs,sizeof qs,"%s",q+1); *q=0; }
        if(!strcmp(path,"/control")){ int ok=!strcmp(meth,"POST") && strstr(qs,"do=forensic") && fx_control(qs); const char*b=ok?"{\"ok\":true}":"{\"ok\":false}";
            char h[256]; int k=snprintf(h,sizeof h,"HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",ok?200:400,strlen(b),b); st_send(fd,h,(size_t)k); }
        else if(!strncmp(path,"/forensics",10)) fx_http(fd,path,qs,req);
        else { const char*nf="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,nf,strlen(nf)); }
        close(fd); } }

int main(int argc,char**argv){
    if(argc>=5 && !strcmp(argv[1],"serve")) return cmd_serve(argv[2],argv[3],atoi(argv[4]));
    if(argc>=3 && !strcmp(argv[1],"demux")) return cmd_demux(argv[2]);
    if(argc>=4 && !strcmp(argv[1],"direct")){ long s=rewrap(argv[2],argv[3],1); if(s>=0) printf("samples %ld\n",s); return g_fail||s<0; }
    if(argc>=4 && !strcmp(argv[1],"silence")) return cmd_silence(argv[2],atoi(argv[3]));
    if(argc>=4 && !strcmp(argv[1],"pipeline")) return cmd_pipeline(argv[2],argv[3]);
    fprintf(stderr,"usage: %s demux <in.ogg> | direct <in.ogg> <out.ogg> | silence <out.ogg> <frames> | pipeline <in.ogg> <dir>\n",argv[0]); return 2; }
