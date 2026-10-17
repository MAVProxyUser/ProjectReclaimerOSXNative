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
static int stats_on(void){ if(g_st_port<0){ const char*e=getenv("PELOAD_STATS_HTTP"); g_st_port=e?atoi(e):0; if(g_st_port<0||g_st_port>65535) g_st_port=0; } return g_st_port>0; }
static pthread_mutex_t g_st_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_st_cv=PTHREAD_COND_INITIALIZER;
static char* g_st_json; static size_t g_st_len; static unsigned long g_st_seq;

typedef struct { char*b; size_t n, cap; } StBuf;
static void sb_put(StBuf*s,const char*fmt,...){ va_list ap; for(;;){ size_t room=s->cap-s->n; va_start(ap,fmt); int k=vsnprintf(s->b?s->b+s->n:0,room,fmt,ap); va_end(ap);
        if(k<0) return; if((size_t)k<room){ s->n+=(size_t)k; return; } size_t nc=(s->cap?s->cap*2:16384); while(nc<s->n+(size_t)k+1) nc*=2; char*nb=realloc(s->b,nc); if(!nb) return; s->b=nb; s->cap=nc; } }
static void sb_str(StBuf*s,const char*v){ sb_put(s,"\""); for(const unsigned char*p=(const unsigned char*)v; p&&*p; p++){
        if(*p=='"'||*p=='\\') sb_put(s,"\\%c",*p); else if(*p<0x20) sb_put(s,"\\u%04x",*p); else sb_put(s,"%c",*p); } sb_put(s,"\""); }
static void sb_f3(StBuf*s,const float*v){ sb_put(s,"[%.2f,%.2f,%.2f]",v[0],v[1],v[2]); }

// The engine's players table (TLS block +0x10, a data array like the object table): +0x28 unit handle, +0x58 name.
typedef struct { uint32_t unit; char name[64]; } StPlayer;
static int st_players(StPlayer*out,int max){
    uint8_t*h3=hb_halo3_base(); uint32_t idx=0; if(!h3 || !hb_read((uint64_t)(h3+0xa39f9c),&idx,4) || idx>=128) return 0;
    void**arrs[1025]; int na=0; arrs[na++]=g_tls_array; pthread_mutex_lock(&g_tlsm); for(int q=0;q<g_ntarrs&&na<1025;q++) arrs[na++]=g_tarrs[q]; pthread_mutex_unlock(&g_tlsm);
    for(int q=0;q<na;q++){ uint64_t blk=(uint64_t)arrs[q][idx], parr=0, oarr=0; uint8_t hdr[0x60];
        if(!blk || !hb_read(blk+0x38,&oarr,8) || !oarr) continue;   // the engine thread's block (it also holds the object table)
        if(!hb_read(blk+0x10,&parr,8) || !parr || !hb_read(parr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) continue;
        int32_t es=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t pd=*(uint64_t*)(hdr+0x48); if(es<0x98 || es>0x10000 || hi<=0 || hi>256) continue;
        int n=0; for(int i=0;i<hi && n<max;i++){ uint8_t rec[0x98]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue;
            StPlayer*p=&out[n]; p->unit=*(uint32_t*)(rec+0x28); int j=0; const uint16_t*w=(const uint16_t*)(rec+0x58);
            for(int c=0;c<32 && w[c] && j<60;c++){ uint16_t u=w[c]; if(u<0x80) p->name[j++]=(char)u;
                else if(u<0x800){ p->name[j++]=(char)(0xC0|(u>>6)); p->name[j++]=(char)(0x80|(u&63)); }
                else { p->name[j++]=(char)(0xE0|(u>>12)); p->name[j++]=(char)(0x80|((u>>6)&63)); p->name[j++]=(char)(0x80|(u&63)); } }
            p->name[j]=0; n++; }
        return n; }
    return 0; }

static void st_tag(StBuf*s,uint32_t def){ sb_str(s,hb_tag_name(def)); }

static void st_snapshot(StBuf*s){
    static const char* teams[8]={"red","blue","green","orange","purple","gold","brown","pink"};
    extern double g_gameplay_t0; double now=mono_s();
    sb_put(s,"{\"time\":%.2f,\"in_match\":%s",g_gameplay_t0?now-g_gameplay_t0:0.0,g_gameplay_t0?"true":"false");
    { char mp[1024]=""; pthread_mutex_lock(&g_hb_mapmx);   // newest map the game opened from halo3/maps (not a cache file)
      for(int k=0;k<g_hb_nmappaths;k++) if(strstr(g_hb_mappaths[k],"/halo3/maps/")){ snprintf(mp,sizeof mp,"%s",g_hb_mappaths[k]); break; }
      if(!mp[0] && g_hb_nmappaths) snprintf(mp,sizeof mp,"%s",g_hb_mappaths[0]);
      pthread_mutex_unlock(&g_hb_mapmx);
      const char*b=strrchr(mp,'/'); b=b?b+1:mp; char m[64]; snprintf(m,sizeof m,"%s",b); char*dot=strrchr(m,'.'); if(dot)*dot=0; sb_put(s,",\"map\":"); sb_str(s,m); }
    static uint8_t tab[8192*0x18]; int hi=0; uint64_t arr=hb_object_array(); uint8_t hdr[0x60];
    if(arr && hb_read(arr,hdr,sizeof hdr) && *(uint32_t*)(hdr+0x2c)==0x64407440 && *(int32_t*)(hdr+0x24)==0x18){
        hi=*(int32_t*)(hdr+0x3c); if(hi<0||hi>8192||hi>*(int32_t*)(hdr+0x20) || !hb_read(*(uint64_t*)(hdr+0x48),tab,(size_t)hi*0x18)) hi=0; }
    #define ST_DATUM(h) ((((h)&0xFFFF)<(uint32_t)hi && (h)!=0xFFFFFFFFu && *(uint16_t*)(tab+((h)&0xFFFF)*0x18)==(uint16_t)((h)>>16)) ? *(uint64_t*)(tab+((h)&0xFFFF)*0x18+0x10) : 0)
    StPlayer pl[256]; int np=st_players(pl,256);
    float cam[3]={0}; int camok=0; { float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); if(!(s0&1) && g_hb_vpseq==s0 && (vp[28]||vp[29]||vp[30])){ memcpy(cam,vp+28,12); camok=1; } }
    // players: every biped (with its player's name when it has one)
    sb_put(s,",\"players\":["); int first=1;
    for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=0) continue; uint64_t p=*(const uint64_t*)(e+0x10);
        static uint8_t d[0x380]; if(!p || !hb_read(p,d,sizeof d)) continue;
        uint32_t h=((uint32_t)*(const uint16_t*)e<<16)|(uint32_t)i; const char*name=0; for(int k=0;k<np;k++) if(pl[k].unit==h){ name=pl[k].name; break; }
        if(!name && !getenv("PELOAD_STATS_ALL_BIPEDS")) continue;   // AI / leftover bodies
        const float*c=(const float*)(d+0x1c), *o=(const float*)(d+0x50), *v=(const float*)(d+0x74);
        int dead=(*(const uint32_t*)(d+0x110)>>2)&1, team=*(const int32_t*)(d+0x174), seat=*(const int16_t*)(d+0x24E), ws=*(const int8_t*)(d+0x262);
        float hp=dead?0:*(const float*)(d+0xF4), sh=dead?0:*(const float*)(d+0xF8), camo=*(const float*)(d+0x360);
        const uint32_t*wp=(const uint32_t*)(d+0x268); const int8_t*gr=(const int8_t*)(d+0x294); uint32_t parent=*(const uint32_t*)(d+0x10);
        int you=0; if(camok){ float dx=c[0]-cam[0],dy=c[1]-cam[1],dz=c[2]-cam[2]; you = dx*dx+dy*dy+dz*dz < (c[3]+0.3f)*(c[3]+0.3f); }
        sb_put(s,"%s{\"name\":",first?"":","); first=0; sb_str(s,name?name:"");
        sb_put(s,",\"handle\":\"%08x\",\"team\":%d,\"team_name\":\"%s\",\"you\":%s,\"alive\":%s,\"health\":%.3f,\"shield\":%.3f,\"camo\":%.3f",
               h,team,(team>=0&&team<8)?teams[team]:"none",you?"true":"false",dead?"false":"true",hp,sh,camo);
        sb_put(s,",\"weapon\":"); { uint64_t wd=(ws>=0&&ws<4)?ST_DATUM(wp[ws]):0; uint32_t def=0; if(wd && hb_read(wd,&def,4)) st_tag(s,def); else sb_put(s,"null"); }
        sb_put(s,",\"weapons\":["); for(int w=0,fw=1;w<4;w++){ uint64_t wd=ST_DATUM(wp[w]); uint32_t def=0; if(!wd || !hb_read(wd,&def,4)) continue; sb_put(s,"%s",fw?"":","); fw=0; st_tag(s,def); } sb_put(s,"]");
        sb_put(s,",\"grenades\":{\"frag\":%d,\"plasma\":%d,\"spike\":%d,\"firebomb\":%d}",gr[0],gr[1],gr[2],gr[3]);
        sb_put(s,",\"vehicle\":"); { uint64_t vd=parent!=0xFFFFFFFFu?ST_DATUM(parent):0; uint32_t def=0; if(vd && hb_read(vd,&def,4)){ st_tag(s,def); sb_put(s,",\"seat\":%d",seat); } else sb_put(s,"null"); }
        sb_put(s,",\"pos\":"); sb_f3(s,o); sb_put(s,",\"speed\":%.2f}",sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2])); }
    sb_put(s,"]");
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
    sb_put(s,"]}");
    #undef ST_DATUM
}
static void* st_snap_thread(void*a){ (void)a; pthread_setname_np("shim-stats-snap");
    for(;;){ StBuf s={0}; st_snapshot(&s); if(s.b){ pthread_mutex_lock(&g_st_mx); free(g_st_json); g_st_json=s.b; g_st_len=s.n; g_st_seq++; pthread_cond_broadcast(&g_st_cv); pthread_mutex_unlock(&g_st_mx); }
        usleep(100000); }
    return 0; }

static const char g_st_page[]=
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Reclaimer live stats</title><style>"
":root{color-scheme:dark}body{margin:0;padding:16px;background:#111;color:#ddd;font:13px/1.4 -apple-system,system-ui,sans-serif}"
"h1{font-size:16px;margin:0 0 8px}#hd{color:#999;margin-bottom:10px}table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:4px 8px;border-bottom:1px solid #2a2a2a;white-space:nowrap}"
"th{color:#999;font-weight:500}.bar{display:inline-block;width:70px;height:8px;background:#333;vertical-align:middle;margin-right:6px}.bar i{display:block;height:100%}"
".dead{opacity:.4}.you td:first-child{font-weight:700}.sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:6px;vertical-align:middle}"
".wrap{overflow-x:auto}</style></head><body><h1>Reclaimer live stats</h1><div id=hd>connecting...</div><div class=wrap><table><thead><tr>"
"<th>Player</th><th>Shield</th><th>Health</th><th>Weapon</th><th>Grenades</th><th>Vehicle</th><th>Camo</th><th>Speed</th><th>Position</th></tr></thead><tbody id=tb></tbody></table></div>"
"<script>const C={red:'#e04040',blue:'#4070f0',green:'#40c060',orange:'#f09030',purple:'#a050e0',gold:'#e0c040',brown:'#9a6a40',pink:'#f080c0',none:'#888'};"
"const E=s=>String(s==null?'':s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));"
"const B=(v,c)=>`<span class=bar><i style='width:${Math.max(0,Math.min(1,v))*100}%;background:${c}'></i></span>${Math.round(v*100)}%`;"
"const es=new EventSource('/events');es.onmessage=m=>{const d=JSON.parse(m.data);"
"document.getElementById('hd').textContent=`${d.map||'-'}  |  ${d.in_match?'match time '+d.time.toFixed(0)+' s':'not in a match'}  |  ${d.players.length} players, ${d.vehicles.length} vehicles, ${d.projectiles.length} projectiles in flight`;"
"const ps=d.players.slice().sort((a,b)=>a.team-b.team||a.name.localeCompare(b.name));"
"document.getElementById('tb').innerHTML=ps.map(p=>`<tr class='${p.alive?'':'dead'} ${p.you?'you':''}'><td><span class=sw style='background:${C[p.team_name]||'#888'}'></span>${E(p.name)}${p.you?' (you)':''}</td>"
"<td>${B(p.shield,'#58b8ff')}</td><td>${B(p.health,'#7ddf6a')}</td><td>${E(p.weapon||'')}</td><td>${p.grenades.frag}f ${p.grenades.plasma}p ${p.grenades.spike}s ${p.grenades.firebomb}b</td>"
"<td>${E(p.vehicle||'')}</td><td>${p.camo>0.01?Math.round(p.camo*100)+'%':''}</td><td>${p.speed.toFixed(1)}</td><td>${p.pos.map(v=>v.toFixed(1)).join(', ')}</td></tr>`).join('');};"
"es.onerror=()=>{document.getElementById('hd').textContent='disconnected (is the game running?)'};</script></body></html>";

static int st_send(int fd,const char*b,size_t n){ while(n){ ssize_t k=send(fd,b,n,0); if(k<=0){ if(k<0&&errno==EINTR) continue; return 0; } b+=k; n-=(size_t)k; } return 1; }
static void* st_conn(void*a){ int fd=(int)(intptr_t)a; pthread_setname_np("shim-stats-http");
    int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one); setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    char req[2048]; ssize_t n=recv(fd,req,sizeof req-1,0); if(n<=0){ close(fd); return 0; } req[n]=0;
    char path[256]="/"; sscanf(req,"GET %255s",path); char*q=strchr(path,'?'); if(q)*q=0;
    char h[512];
    if(!strcmp(path,"/events")){
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        if(st_send(fd,h,(size_t)k)) for(unsigned long seen=0;;){ pthread_mutex_lock(&g_st_mx); while(g_st_seq==seen) pthread_cond_wait(&g_st_cv,&g_st_mx);
                seen=g_st_seq; size_t len=g_st_len; char*j=malloc(len+16); if(j){ memcpy(j,"data: ",6); memcpy(j+6,g_st_json,len); memcpy(j+6+len,"\n\n",2); } pthread_mutex_unlock(&g_st_mx);
                int ok=j && st_send(fd,j,len+8); free(j); if(!ok) break; } }
    else if(!strcmp(path,"/stats.json")){
        pthread_mutex_lock(&g_st_mx); size_t len=g_st_len; char*j=malloc(len+1); if(j && g_st_json) memcpy(j,g_st_json,len); else len=0; pthread_mutex_unlock(&g_st_mx);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",len);
        if(st_send(fd,h,(size_t)k) && len) st_send(fd,j,len); free(j); }
    else if(!strcmp(path,"/")){
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",sizeof g_st_page-1);
        if(st_send(fd,h,(size_t)k)) st_send(fd,g_st_page,sizeof g_st_page-1); }
    else { const char*nf="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,nf,strlen(nf)); }
    close(fd); return 0; }
static void* st_listen_thread(void*a){ (void)a; pthread_setname_np("shim-stats-listen");
    // only the process that runs the engine serves (the client's launcher process inherits the variable too)
    while(!hb_halo3_base()) usleep(500000);
    int ls=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)g_st_port);
    const char*ad=getenv("PELOAD_STATS_ADDR"); sa.sin_addr.s_addr=inet_addr(ad&&*ad?ad:"127.0.0.1");
    char m[160]; int k;
    if(ls<0 || bind(ls,(struct sockaddr*)&sa,sizeof sa) || listen(ls,16)){ k=snprintf(m,sizeof m,"  [stats] cannot listen on %s:%d (%s)\n",ad&&*ad?ad:"127.0.0.1",g_st_port,strerror(errno)); (void)!write(2,m,k); if(ls>=0) close(ls); return 0; }
    k=snprintf(m,sizeof m,"  [stats] live game stats on http://%s:%d/ (also /stats.json, /events)\n",ad&&*ad?ad:"127.0.0.1",g_st_port); (void)!write(2,m,k);
    pthread_t t; pthread_create(&t,0,st_snap_thread,0); pthread_detach(t);
    for(;;){ int fd=accept(ls,0,0); if(fd<0){ if(errno==EINTR) continue; usleep(100000); continue; } pthread_t c; if(pthread_create(&c,0,st_conn,(void*)(intptr_t)fd)) close(fd); else pthread_detach(c); }
    return 0; }
static void stats_start(void){ if(!stats_on()) return; pthread_t t; pthread_create(&t,0,st_listen_thread,0); pthread_detach(t); }
