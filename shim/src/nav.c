// ---- Map-aware walking for unattended tests (backlog 32; later the sparring partner) ------------------------------
// PELOAD_NAV=roam | enemy | x,y,z   drives pad 0 (left stick + turning) along walkable paths of the map being played:
//   roam   a random reachable spot at least 12 units away, a new one on arrival
//   enemy  the nearest live biped of another team (any other biped in free-for-all), replanned as it moves
//   x,y,z  one fixed point
// The graph is re-tools/h3nav.py's layered floor grid (0.25-unit cells; floors with 0.6 units of free space, steps of
// 0.35 both ways, drops up to 2 one way; kill volumes and outside-safe-zone floors left out), made in the background
// the first time a map is played: ~/Library/Application Support/ReclaimerMac/maptop/<map>.nav (PELOAD_NAV_TOOL = the
// script, PELOAD_NAV_FILE = a file to use instead). Paths keep away from walls (cost by wall distance); our position
// is our biped (the one the camera sits in, or the stats thread's local unit). Stuck (under 0.35 units in 1.5 s while
// walking): back off and sidestep, jump, raise the cost of the next stretch of path, replan; four times in 10 s: a new goal.
// Runs 30 times a second on its own thread (shim-nav, started from the first Present); XInputGetState takes the sticks
// from nav_pad. Starts PELOAD_INPUT_DELAY s (default 3) after gameplay starts. PELOAD_NAV_LOG=1 logs goals, paths, stuck
// events and a status line every 2 s. PELOAD_NAV_PITCH fixes the look-pitch sign (default: found by trying, -1 first;
// it depends on the profile's inverted-look setting), PELOAD_NAV_TURN the turn gain (2.5), PELOAD_NAV_LOOK how many
// path nodes ahead it steers at (6 = 1.5 units).
typedef struct { float z; uint16_t comp; uint8_t wd, fl; int32_t nbr[8]; } NavNode;     // 40 bytes, as h3nav.py writes
static struct { char name[96], file[1100]; int ok, failed; float x0,y0,cell,step,clearh; int w,h,n; uint32_t*first; NavNode*nd; int32_t*cellof;
    float*g; float*pen; int32_t*came; uint32_t*stamp; uint32_t gen; float*hf; int32_t*hn; int hcap;
    float*dng; double dng_t; uint32_t dng_nd; } g_nav;
static volatile int g_nav_act; static volatile float g_nav_lx, g_nav_ly, g_nav_rx, g_nav_ry; static volatile uint16_t g_nav_btn;
static int nav_mode(void){ static int m=-1; if(m<0){ const char*e=getenv("PELOAD_NAV"); m=!e||!*e?0: !strcasecmp(e,"enemy")?2: strchr(e,',')?3: 1; } return m; }
static int nav_log(void){ static int l=-1; if(l<0) l=getenv("PELOAD_NAV_LOG")?1:0; return l; }
static void nav_say(const char*fmt,...){ if(!nav_log()) return; char m[400]; va_list ap; va_start(ap,fmt); int k=vsnprintf(m,sizeof m,fmt,ap); va_end(ap); if(k>(int)sizeof m-1) k=sizeof m-1; (void)!write(2,m,k); }
static void nav_free(void){ free(g_nav.dng); free(g_nav.first); free(g_nav.nd); free(g_nav.cellof); free(g_nav.g); free(g_nav.pen); free(g_nav.came); free(g_nav.stamp); free(g_nav.hf); free(g_nav.hn);
    char nm[96]; memcpy(nm,g_nav.name,sizeof nm); memset(&g_nav,0,sizeof g_nav); memcpy(g_nav.name,nm,sizeof nm); }
static int nav_load(const char*path){
    int fd=open(path,O_RDONLY); if(fd<0) return 0; struct stat st; uint8_t h[64];
    if(fstat(fd,&st) || pread(fd,h,64,0)!=64 || memcmp(h,"RNV1",4) || *(uint32_t*)(h+4)!=1){ close(fd); return 0; }
    float x0,y0,cell,step,clr; int32_t w,hh,n; memcpy(&x0,h+8,4); memcpy(&y0,h+12,4); memcpy(&cell,h+16,4); memcpy(&w,h+20,4); memcpy(&hh,h+24,4); memcpy(&n,h+28,4); memcpy(&step,h+32,4); memcpy(&clr,h+36,4);
    if(w<=0||hh<=0||n<=0||(int64_t)w*hh>(1<<24)||n>(1<<23) || st.st_size!=64+4*((int64_t)w*hh+1)+40*(int64_t)n){ close(fd); return 0; }
    nav_free(); g_nav.first=malloc(4*((size_t)w*hh+1)); g_nav.nd=malloc(40*(size_t)n);
    if(!g_nav.first||!g_nav.nd || pread(fd,g_nav.first,4*((size_t)w*hh+1),64)!=(ssize_t)(4*((size_t)w*hh+1)) || pread(fd,g_nav.nd,40*(size_t)n,64+4*((off_t)w*hh+1))!=(ssize_t)(40*(size_t)n)){ close(fd); nav_free(); return 0; }
    close(fd); g_nav.x0=x0; g_nav.y0=y0; g_nav.cell=cell; g_nav.w=w; g_nav.h=hh; g_nav.n=n; g_nav.step=step; g_nav.clearh=clr;
    g_nav.cellof=malloc(4*(size_t)n); g_nav.g=malloc(4*(size_t)n); g_nav.pen=calloc((size_t)n,4); g_nav.came=malloc(4*(size_t)n); g_nav.stamp=calloc((size_t)n,4);
    g_nav.hcap=8*n+16; g_nav.hf=malloc(4*(size_t)g_nav.hcap); g_nav.hn=malloc(4*(size_t)g_nav.hcap);
    if(!g_nav.cellof||!g_nav.g||!g_nav.pen||!g_nav.came||!g_nav.stamp||!g_nav.hf||!g_nav.hn){ nav_free(); return 0; }
    for(int c=0;c<w*hh;c++) for(uint32_t i=g_nav.first[c];i<g_nav.first[c+1] && i<(uint32_t)n;i++) g_nav.cellof[i]=c;
    snprintf(g_nav.file,sizeof g_nav.file,"%s",path); g_nav.ok=1; return 1; }
static void nav_pos(int i,float*p){ int c=g_nav.cellof[i]; p[0]=g_nav.x0+(c%g_nav.w+0.5f)*g_nav.cell; p[1]=g_nav.y0+(c/g_nav.w+0.5f)*g_nav.cell; p[2]=g_nav.nd[i].z; }
// the floor under a point: in its cell (or the nearest cell within 3 that has one), the highest floor at most 0.2 above
// it, else the lowest one
static int nav_node_at(const float*p){
    int cx=(int)floorf((p[0]-g_nav.x0)/g_nav.cell), cy=(int)floorf((p[1]-g_nav.y0)/g_nav.cell);
    for(int r=0;r<=3;r++){ int best=-1; float bz=-1e30f, bd=1e30f;
        for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++){ if(r && abs(dx)!=r && abs(dy)!=r) continue; int x=cx+dx,y=cy+dy; if(x<0||y<0||x>=g_nav.w||y>=g_nav.h) continue;
            int c=y*g_nav.w+x; for(uint32_t i=g_nav.first[c];i<g_nav.first[c+1];i++){ float z=g_nav.nd[i].z; float d=(float)(dx*dx+dy*dy);
                if(z<=p[2]+0.2f){ if(d<bd || (d==bd && z>bz)){ bd=d; bz=z; best=(int)i; } }
                else if(best<0 && d<bd){ bd=d+1000; bz=z; best=(int)i; } } }
        if(best>=0) return best; }
    return -1; }
// Danger: where players died on this map (the stats heatmap, stats.c g_hm -- earlier matches too). Every death adds
// 1 - d/R to the floors within R = PELOAD_NAV_AVOID_R units (default 4) and 2 units of its height; a node's danger is
// 1 - exp(-sum/s0) with s0 = max(3, T/ln 2), T = the sum at the PELOAD_NAV_HOT quantile of all floor nodes (default 0.9):
// danger 0.5 marks only about the map's deadliest 10% of floor, also on maps whose heatmap holds thousands of deaths from
// many sessions (Valhalla 2026-10-18: 2,393 deaths had put 59% of the floor above 0.5 with a fixed s0 = 3; sparse maps
// keep s0 = 3, three nearby deaths ~ 0.63). Walking a node costs (1 + PELOAD_NAV_AVOID x danger) times its length
// (default 4: the worst spots cost 5x, so paths detour around them when a detour exists); roam goals skip danger > 0.5.
// Recomputed every 20 s (deaths keep coming in). PELOAD_NAV_AVOID=0: off.
static int nav_fcmp(const void*a,const void*b){ float x=*(const float*)a, y=*(const float*)b; return x<y?-1:x>y; }
static float nav_avoid(void){ static float k=-1; if(k<0){ const char*e=getenv("PELOAD_NAV_AVOID"); k=e?atof(e):4.0f; if(k<0) k=0; } return k; }
static void nav_danger_update(double now){
    if(!nav_avoid() || (g_nav.dng && now-g_nav.dng_t<20.0)) return; g_nav.dng_t=now;
    static float (*pts)[3]; static uint32_t cap; uint32_t nd=0;
    pthread_mutex_lock(&g_hm_mx);
    if(!strcmp(g_hm.map,g_nav.name) && g_hm.nd){ nd=g_hm.nd; if(nd>cap){ free(pts); pts=malloc((size_t)nd*12); cap=pts?nd:0; } if(!pts) nd=0;
        for(uint32_t i=0;i<nd;i++){ pts[i][0]=g_hm.d[i].x; pts[i][1]=g_hm.d[i].y; pts[i][2]=g_hm.d[i].z; } }
    pthread_mutex_unlock(&g_hm_mx);
    if(!g_nav.dng){ g_nav.dng=calloc((size_t)g_nav.n,4); if(!g_nav.dng) return; } else memset(g_nav.dng,0,4*(size_t)g_nav.n);
    static float R=-1; if(R<0){ const char*e=getenv("PELOAD_NAV_AVOID_R"); R=e?atof(e):4.0f; if(R<0.5f) R=0.5f; }
    int rc=(int)ceilf(R/g_nav.cell);
    for(uint32_t k=0;k<nd;k++){ int cx=(int)floorf((pts[k][0]-g_nav.x0)/g_nav.cell), cy=(int)floorf((pts[k][1]-g_nav.y0)/g_nav.cell);
        for(int dy=-rc;dy<=rc;dy++){ int y=cy+dy; if(y<0||y>=g_nav.h) continue;
            for(int dx=-rc;dx<=rc;dx++){ int x=cx+dx; if(x<0||x>=g_nav.w) continue;
                float px=g_nav.x0+(x+0.5f)*g_nav.cell-pts[k][0], py=g_nav.y0+(y+0.5f)*g_nav.cell-pts[k][1], d=sqrtf(px*px+py*py); if(d>=R) continue;
                int c=y*g_nav.w+x; for(uint32_t i=g_nav.first[c];i<g_nav.first[c+1];i++) if(fabsf(g_nav.nd[i].z-pts[k][2])<2.0f) g_nav.dng[i]+=1.0f-d/R; } } }
    static float hq=-1; if(hq<0){ const char*e=getenv("PELOAD_NAV_HOT"); hq=e?atof(e):0.9f; if(hq<0.5f) hq=0.5f; if(hq>0.999f) hq=0.999f; }
    float s0=3.0f; { float*cp=malloc(4*(size_t)g_nav.n); if(cp){ memcpy(cp,g_nav.dng,4*(size_t)g_nav.n); qsort(cp,(size_t)g_nav.n,4,nav_fcmp);
        float T=cp[(size_t)((double)hq*(g_nav.n-1))]; free(cp); if(T/0.6931f>s0) s0=T/0.6931f; } }
    int hot=0; for(int i=0;i<g_nav.n;i++){ g_nav.dng[i]=1.0f-expf(-g_nav.dng[i]/s0); if(g_nav.dng[i]>0.5f) hot++; }
    if(nd!=g_nav.dng_nd) nav_say("  [nav] danger from %u deaths on %s (scale %.1f): %d of %d floor nodes above 0.5\n",nd,g_nav.name,s0,hot,g_nav.n);
    g_nav.dng_nd=nd; }
static int g_nav_noavoid;   // set around a comparison plan (PELOAD_NAV_LOG): the same A* without the danger term
// A* over the floor graph; cost = distance x (1 + wall penalty + avoid x danger) + learned penalties; -> path length
static int nav_astar(int s,int t,int32_t*path,int maxp){
    if(s<0||t<0) return 0; uint32_t gen=++g_nav.gen; if(!gen){ memset(g_nav.stamp,0,4*(size_t)g_nav.n); gen=g_nav.gen=1; }
    float tp[3]; nav_pos(t,tp); int hn=0;
    #define NAV_PUSH(f_,n_) do{ if(hn<g_nav.hcap){ int i_=hn++; while(i_>0){ int p_=(i_-1)/2; if(g_nav.hf[p_]<=(f_)) break; g_nav.hf[i_]=g_nav.hf[p_]; g_nav.hn[i_]=g_nav.hn[p_]; i_=p_; } g_nav.hf[i_]=(f_); g_nav.hn[i_]=(n_); } }while(0)
    g_nav.stamp[s]=gen; g_nav.g[s]=0; g_nav.came[s]=-1; { float sp[3]; nav_pos(s,sp); float d=sqrtf((sp[0]-tp[0])*(sp[0]-tp[0])+(sp[1]-tp[1])*(sp[1]-tp[1])); NAV_PUSH(d,s); }
    static const float wpen[16]={3,1.5f,0.6f,0.2f,0,0,0,0,0,0,0,0,0,0,0,0}; int found=0, pops=0; float kav=g_nav_noavoid?0.0f:nav_avoid();
    while(hn>0){ float f=g_nav.hf[0]; int u=g_nav.hn[0]; (void)f;
        hn--; { float lf=g_nav.hf[hn]; int ln=g_nav.hn[hn]; int i=0; for(;;){ int a=2*i+1,b=a+1,m=i; float mf=lf;
                if(a<hn && g_nav.hf[a]<mf){ m=a; mf=g_nav.hf[a]; } if(b<hn && g_nav.hf[b]<mf){ m=b; mf=g_nav.hf[b]; } if(m==i) break;
                g_nav.hf[i]=g_nav.hf[m]; g_nav.hn[i]=g_nav.hn[m]; i=m; } if(hn>0){ g_nav.hf[i]=lf; g_nav.hn[i]=ln; } }
        if(u==t){ found=1; break; } if(++pops>400000) break;
        float up[3]; nav_pos(u,up); float gu=g_nav.g[u];
        for(int k=0;k<8;k++){ int v=g_nav.nd[u].nbr[k]; if(v<0||v>=g_nav.n) continue; float vp[3]; nav_pos(v,vp);
            float dx=vp[0]-up[0],dy=vp[1]-up[1],dz=vp[2]-up[2]; float c=sqrtf(dx*dx+dy*dy+dz*dz)*(1.0f+wpen[g_nav.nd[v].wd&15]+(g_nav.dng?kav*g_nav.dng[v]:0))+g_nav.pen[v]+((g_nav.nd[v].fl&1)?0.5f:0);
            float gv=gu+c; if(g_nav.stamp[v]==gen && g_nav.g[v]<=gv) continue;
            g_nav.stamp[v]=gen; g_nav.g[v]=gv; g_nav.came[v]=u;
            float hx=vp[0]-tp[0],hy=vp[1]-tp[1],hz=vp[2]-tp[2]; NAV_PUSH(gv+sqrtf(hx*hx+hy*hy+hz*hz),v); } }
    #undef NAV_PUSH
    if(!found) return 0; int n=0; for(int v=t;v>=0 && n<maxp;v=g_nav.came[v]) path[n++]=v;
    for(int i=0;i<n/2;i++){ int32_t x=path[i]; path[i]=path[n-1-i]; path[n-1-i]=x; } return n; }
static uint32_t nav_rand(void){ static uint32_t s=0x9e3779b9u; s^=s<<13; s^=s>>17; s^=s<<5; return s; }
static int nav_pick_roam(int from){ float fp[3]; nav_pos(from,fp); int comp=g_nav.nd[from].comp, best=-1; float bd=0;
    for(int tr=0;tr<400;tr++){ int i=(int)(nav_rand()%(uint32_t)g_nav.n); if(g_nav.nd[i].comp!=comp || g_nav.nd[i].wd<2) continue;
        if(g_nav.dng && g_nav.dng[i]>0.5f && tr<300) continue;   // not into a death hotspot (unless nothing else turns up)
        float p[3]; nav_pos(i,p);
        float d=sqrtf((p[0]-fp[0])*(p[0]-fp[0])+(p[1]-fp[1])*(p[1]-fp[1])); if(d>=12.0f) return i; if(d>bd){ bd=d; best=i; } }
    return best; }
static char g_nav_making[1100];
static void* nav_wait(void*a){ pid_t pid=(pid_t)(intptr_t)a; int st=0; while(waitpid(pid,&st,0)<0 && errno==EINTR){}
    char m[1300]; int k=snprintf(m,sizeof m,"  [nav] walking graph %s: %s\n",g_nav_making,access(g_nav_making,R_OK)?"FAILED (see its .log)":"made"); (void)!write(2,m,k); return 0; }
// the nav file of the map being played: load it, or make it in the background (once per map)
static void nav_ensure(void){
    static double tc; double now=mono_s(); if(now-tc<2.0) return; tc=now;
    char mp[1024]; if(!hb_current_map(mp,sizeof mp)) return; const char*b=strrchr(mp,'/'); b=b?b+1:mp; char name[96]; snprintf(name,sizeof name,"%s",b); char*dot=strrchr(name,'.'); if(dot) *dot=0;
    if((name[0]>='0'&&name[0]<='9') || !strcmp(name,"mainmenu") || !strcmp(name,"campaign") || !strcmp(name,"shared")) return;   // campaign levels (menu backgrounds), the menu
    if(strcmp(name,g_nav.name)){ nav_free(); snprintf(g_nav.name,sizeof g_nav.name,"%s",name); g_nav.failed=0; }
    if(g_nav.ok || g_nav.failed>=2) return;
    char path[1100]; const char*fe=getenv("PELOAD_NAV_FILE"); const char*home=getenv("HOME");
    if(fe&&*fe) snprintf(path,sizeof path,"%s",fe); else snprintf(path,sizeof path,"%s/Library/Application Support/ReclaimerMac/maptop/%s.nav",home?home:"/tmp",name);
    if(nav_load(path)){ nav_say("  [nav] %s: %d floor nodes (%dx%d cells of %.2f) from %s\n",name,g_nav.n,g_nav.w,g_nav.h,g_nav.cell,path); return; }
    // make one only once a match is running: in the menu the 'map being played' is whatever map its scene borrows from
    // (Forge's desert_sandbox, Guardian), and building a big map's graph costs a core for a while
    extern double g_gameplay_t0; if(!g_gameplay_t0) return;
    static char spawned[96]; if(!strcmp(spawned,name) || (fe&&*fe)){ return; }
    snprintf(spawned,sizeof spawned,"%s",name);
    char tool[1100]; const char*e=getenv("PELOAD_NAV_TOOL");
    if(e&&*e) snprintf(tool,sizeof tool,"%s",e);
    else { char exe[1024]; uint32_t es=sizeof exe; tool[0]=0; if(!_NSGetExecutablePath(exe,&es)){ char*sl=strrchr(exe,'/'); if(sl){ *sl=0; snprintf(tool,sizeof tool,"%s/../../re-tools/h3nav.py",exe); } } }
    if(!tool[0] || access(tool,R_OK)){ g_nav.failed=2; nav_say("  [nav] no walking graph for %s: %s not found\n",name,tool[0]?tool:"h3nav.py"); return; }
    char dir[1100]; snprintf(dir,sizeof dir,"%s",path); char*sl=strrchr(dir,'/'); if(sl){ *sl=0; mkdir(dir,0755); }
    char lg[1110]; snprintf(lg,sizeof lg,"%s.log",path);
    char*argv[]={(char*)"/usr/bin/arch",(char*)"-arm64",(char*)"python3",tool,mp,path,(char*)"--quiet",0}; extern char**environ;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_addopen(&fa,0,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&fa,1,lg,O_WRONLY|O_CREAT|O_TRUNC,0644); posix_spawn_file_actions_adddup2(&fa,1,2);
    posix_spawnattr_t at; posix_spawnattr_init(&at); posix_spawnattr_setflags(&at,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_CLOEXEC_DEFAULT); posix_spawnattr_setpgroup(&at,0);   // only its stdio
    pid_t pid=0; int r=posix_spawn(&pid,"/usr/bin/arch",&fa,&at,argv,environ); posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if(r){ g_nav.failed=2; return; }
    { char m[1300]; int k=snprintf(m,sizeof m,"  [nav] making the walking graph for %s (%s)\n",name,mp); (void)!write(2,m,k); }
    snprintf(g_nav_making,sizeof g_nav_making,"%s",path); pthread_t th; pthread_create(&th,0,nav_wait,(void*)(intptr_t)pid); pthread_detach(th); }
static void nav_step(void);
static void nav_publish(const int32_t*path,int from,int plen,int goal);
static void* nav_thread(void*a){ (void)a; pthread_setname_np("shim-nav"); for(;;){ usleep(33000); nav_step(); } return 0; }
// called at each Present: starts the nav thread (planning a long path over a big map takes tens of ms -- not on the
// render thread)
void nav_tick(void){ static int started; if(started || !nav_mode()) return; started=1; pthread_t th; pthread_create(&th,0,nav_thread,0); pthread_detach(th); }
static void nav_step(void){
    double now=mono_s();
    nav_ensure();
    double delay=getenv("PELOAD_INPUT_DELAY")?atof(getenv("PELOAD_INPUT_DELAY")):3.0;
    if(!g_nav.ok || !g_gameplay_t0 || now-g_gameplay_t0<delay){ g_nav_act=0; return; }
    nav_danger_update(now);
    float vp[32]; unsigned s1,s2; int tries=0; do{ s1=g_hb_vpseq; memcpy(vp,(const void*)g_hb_vp,sizeof vp); s2=g_hb_vpseq; }while((s1!=s2 || (s1&1)) && ++tries<8);
    const float*cam=vp+28, *fwd=vp+12; if(cam[0]==0 && cam[1]==0 && cam[2]==0){ g_nav_act=0; return; }
    static HbObj objs[1024]; pthread_mutex_lock(&g_hb_collect_mx); g_hb_units_only=1; int n=hb_collect(objs,1024); g_hb_units_only=0; pthread_mutex_unlock(&g_hb_collect_mx);
    int me=-1; for(int i=0;i<n;i++){ const HbObj*o=&objs[i]; if(o->type) continue; float dx=o->c[0]-cam[0],dy=o->c[1]-cam[1],dz=o->c[2]-cam[2];
        if(dx*dx+dy*dy+dz*dz < (o->r+0.3f)*(o->r+0.3f) || o->handle==g_hb_local_unit){ me=i; break; } }
    static int32_t path[8192]; static int plen, pidx, goal=-1; static double tplan, tstat, tjump, tback, tstuck[4]; static float bstr; static float hist[64][3]; static double histt[64]; static int hh;
    if(me<0 || objs[me].dead || objs[me].seat>=0){ g_nav_act=0; plen=0; return; }   // dead, respawning, or in a vehicle
    const HbObj*o=&objs[me]; float pos[3]={o->c[0],o->c[1],o->c[2]-0.35f};   // feet: just under the bounding centre
    int cur=nav_node_at(pos); if(cur<0){ g_nav_act=0; return; }
    // goal
    int mode=nav_mode(); int need=plen==0 || goal<0 || now-tplan>(mode==2?1.0:3.0);
    if(goal>=0 && plen){ float gp[3]; nav_pos(goal,gp); if((gp[0]-pos[0])*(gp[0]-pos[0])+(gp[1]-pos[1])*(gp[1]-pos[1])<1.0f){ nav_say("  [nav] reached the goal\n"); goal=-1; need=1; } }
    if(mode==2){ float bd=1e30f; int bi=-1; for(int pass=0;pass<2 && bi<0;pass++) for(int i=0;i<n;i++){ const HbObj*t=&objs[i]; if(t->type || t->dead || i==me) continue;
            if(!pass && o->team>=0 && t->team==o->team) continue; float dx=t->c[0]-pos[0],dy=t->c[1]-pos[1],d=dx*dx+dy*dy; if(d<bd){ bd=d; bi=i; } }
        if(bi>=0){ float tp[3]={objs[bi].c[0],objs[bi].c[1],objs[bi].c[2]-0.35f}; int gn=nav_node_at(tp); if(gn>=0) goal=gn; } }
    else if(mode==3 && goal<0){ float tp[3]; if(sscanf(getenv("PELOAD_NAV"),"%f,%f,%f",&tp[0],&tp[1],&tp[2])==3) goal=nav_node_at(tp); }
    else if(mode==1 && goal<0) goal=nav_pick_roam(cur);
    if(goal<0){ g_nav_act=0; return; }
    if(need){ double t0=mono_s(); int np=nav_astar(cur,goal,path,8192); tplan=now;
        if(np<2){ if(mode!=2){ nav_say("  [nav] no path from node %d to %d: a new goal\n",cur,goal); goal=-1; } plen=0; g_nav_act=0; return; }
        if(!plen || need){ char hs[120]=""; double ms=(mono_s()-t0)*1000;
            if(nav_log() && g_nav.dng){   // hotspot nodes (danger > 0.5) on this path vs on the same plan without avoidance
                static int32_t alt[8192]; int hp=0, ha=0; for(int i=0;i<np;i++) hp+=g_nav.dng[path[i]]>0.5f;
                g_nav_noavoid=1; int na=nav_astar(cur,goal,alt,8192); g_nav_noavoid=0; for(int i=0;i<na;i++) ha+=g_nav.dng[alt[i]]>0.5f;
                snprintf(hs,sizeof hs,"; hotspot nodes %d (without avoidance: %d of %d)",hp,ha,na); }
            nav_say("  [nav] path of %d nodes from (%.1f %.1f %.1f) to node %d, %.1f ms%s\n",np,pos[0],pos[1],pos[2],goal,ms,hs); }
        plen=np; pidx=0; }
    // follow: nearest path node ahead of the last one, then aim PELOAD_NAV_LOOK nodes beyond it
    { float bd=1e30f; int bi=pidx; for(int i=pidx;i<plen && i<pidx+60;i++){ float p[3]; nav_pos(path[i],p); float d=(p[0]-pos[0])*(p[0]-pos[0])+(p[1]-pos[1])*(p[1]-pos[1]); if(d<bd){ bd=d; bi=i; } } pidx=bi;
      if(bd>4.0f) tplan=0; }   // more than 2 units off the path: replan next tick
    { static double tpub; if(now-tpub>0.25){ tpub=now; nav_publish(path,pidx,plen,goal); } }
    static int look=-1; if(look<0){ const char*e=getenv("PELOAD_NAV_LOOK"); look=e?atoi(e):6; if(look<1) look=1; }
    int ti=pidx+look<plen?pidx+look:plen-1; float tp[3]; nav_pos(path[ti],tp);
    float yaw_err=atan2f(tp[1]-pos[1],tp[0]-pos[0])-atan2f(fwd[1],fwd[0]); while(yaw_err>3.14159f) yaw_err-=6.28318f; while(yaw_err<-3.14159f) yaw_err+=6.28318f;
    static float kt=-1, ps; static int psfix; if(kt<0){ const char*e=getenv("PELOAD_NAV_TURN"); kt=e?atof(e):2.5f; e=getenv("PELOAD_NAV_PITCH"); ps=e?atof(e):-1.0f; psfix=e!=0; }
    float rx=-yaw_err*kt; rx=rx>0.8f?0.8f:rx<-0.8f?-0.8f:rx;
    float fh=sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]); float pitch=atan2f(fwd[2],fh), perr=-0.08f-pitch; float ry=ps*perr*2.0f; ry=ry>0.5f?0.5f:ry<-0.5f?-0.5f:ry;   // look a little down
    // the look-pitch sign depends on the profile's inverted-look setting: flip it when holding the stick makes the pitch
    // error grow (unless PELOAD_NAV_PITCH sets it)
    // two worse half-seconds in a row (not while jumping) flip it; two better ones after that lock it
    { static double tp; static float pe0, ry0; static int worse, better; if(!psfix && now-tp>0.5){
        if(now-tjump>1.0 && fabsf(ry0)>=0.2f && fabsf(pe0)>0.05f && !(hb_spar_enabled() && g_spar_ok)){   // (the sparring aim owns the view)
            if(fabsf(perr)>fabsf(pe0)+0.02f){ better=0; if(++worse>=2){ ps=-ps; worse=0; nav_say("  [nav] look pitch sign flipped to %+.0f\n",ps); } }
            else if(fabsf(perr)<fabsf(pe0)-0.02f){ worse=0; if(++better>=2){ psfix=1; nav_say("  [nav] look pitch sign %+.0f confirmed\n",ps); } } }
        tp=now; pe0=perr; ry0=ry; } }
    // with the sparring partner's aim on (PELOAD_SPAR) the view is not ours to turn: walk the path in any direction
    // relative to it; in enemy mode within PELOAD_NAV_NEAR units (default 4) of the target, strafe side to side instead
    int spar=hb_spar_enabled() && g_spar_ok;
    float mv=(!spar && fabsf(yaw_err)>1.05f)?0.25f:1.0f; float lx=-sinf(yaw_err)*mv, ly=cosf(yaw_err)*mv; if(ly<0 && !spar) ly=0;
    if(mode==2){ static float near=-1; if(near<0){ const char*e=getenv("PELOAD_NAV_NEAR"); near=e?atof(e):4.0f; } float gp[3]; nav_pos(goal,gp);
        if((gp[0]-pos[0])*(gp[0]-pos[0])+(gp[1]-pos[1])*(gp[1]-pos[1])<near*near){ lx=fmod(now,3.0)<1.5?0.8f:-0.8f; ly=0; } }
    // stuck: barely moved in 1.5 s while walking -> jump, penalise the next stretch, replan
    uint16_t btn=0; hist[hh&63][0]=pos[0]; hist[hh&63][1]=pos[1]; hist[hh&63][2]=pos[2]; histt[hh&63]=now; hh++;
    { int j=hh-1; while(j>0 && hh-j<63 && now-histt[(j-1)&63]<=1.5) j--; float dx=pos[0]-hist[j&63][0], dy=pos[1]-hist[j&63][1];
      if(now-histt[j&63]>=1.4 && dx*dx+dy*dy<0.35f*0.35f && now-tjump>1.5){ tjump=now; hh=0; tback=now; bstr=(nav_rand()&1)?0.7f:-0.7f;
          for(int i=pidx;i<plen && i<pidx+10;i++) g_nav.pen[path[i]]+=4.0f;
          memmove(tstuck,tstuck+1,3*sizeof(double)); tstuck[3]=now;
          if(now-tstuck[0]<10.0){ nav_say("  [nav] stuck 4 times at (%.1f %.1f %.1f): a new goal\n",pos[0],pos[1],pos[2]); if(mode!=2) goal=-1; memset(tstuck,0,sizeof tstuck); }
          else nav_say("  [nav] stuck at (%.1f %.1f %.1f): jump and replan\n",pos[0],pos[1],pos[2]);
          tplan=0; } }
    if(now-tjump<0.25) btn|=0x1000;   // A = jump
    if(now-tback<0.6){ lx=bstr; ly=-0.8f; rx=0; }   // first back off and sidestep (a corner the next plan may lead into again)
    g_nav_lx=lx; g_nav_ly=ly; g_nav_rx=rx; g_nav_ry=ry; g_nav_btn=btn; g_nav_act=1;
    if(now-tstat>2.0){ tstat=now; nav_say("  [nav] at (%.1f %.1f %.1f) node %d (danger %.2f), path %d/%d, next (%.1f %.1f %.1f), turn %.0f deg -> L=(%.2f,%.2f) R=(%.2f,%.2f)\n",
        pos[0],pos[1],pos[2],cur,g_nav.dng?g_nav.dng[cur]:0.0f,pidx,plen,tp[0],tp[1],tp[2],yaw_err*57.2958f,lx,ly,rx,ry); } }
// the stats page's map: the walker's goal and the rest of its path (at most 160 points)
static pthread_mutex_t g_nav_pmx=PTHREAD_MUTEX_INITIALIZER; static float g_nav_pts[160][2], g_nav_goalp[3]; static int g_nav_npts, g_nav_hasgoal;
static void nav_publish(const int32_t*path,int from,int plen,int goal){ pthread_mutex_lock(&g_nav_pmx); g_nav_npts=0; g_nav_hasgoal=goal>=0;
    if(goal>=0) nav_pos(goal,g_nav_goalp);
    int left=plen-from, step=left>159?(left+158)/159:1; for(int i=from;i<plen && g_nav_npts<159;i+=step){ float p[3]; nav_pos(path[i],p); g_nav_pts[g_nav_npts][0]=p[0]; g_nav_pts[g_nav_npts][1]=p[1]; g_nav_npts++; }
    if(plen>from && g_nav_npts<160){ float p[3]; nav_pos(path[plen-1],p); g_nav_pts[g_nav_npts][0]=p[0]; g_nav_pts[g_nav_npts][1]=p[1]; g_nav_npts++; }
    pthread_mutex_unlock(&g_nav_pmx); }
static void nav_json(StBuf*s){ if(!nav_mode()) return; static const char*const mn[4]={"","roam","enemy","point"};
    pthread_mutex_lock(&g_nav_pmx); sb_put(s,",\"nav\":{\"mode\":\"%s\",\"active\":%s",mn[nav_mode()&3],g_nav_act?"true":"false");
    if(g_nav_hasgoal) sb_put(s,",\"goal\":[%.2f,%.2f,%.2f]",g_nav_goalp[0],g_nav_goalp[1],g_nav_goalp[2]);
    sb_put(s,",\"path\":["); for(int i=0;i<g_nav_npts;i++) sb_put(s,"%s[%.2f,%.2f]",i?",":"",g_nav_pts[i][0],g_nav_pts[i][1]); sb_put(s,"]}");
    pthread_mutex_unlock(&g_nav_pmx); }
int nav_pad(int16_t*lx,int16_t*ly,int16_t*rx,int16_t*ry,uint16_t*b){
    if(!g_nav_act) return 0; *lx=ax16(g_nav_lx); *ly=ax16(g_nav_ly); *rx=ax16(g_nav_rx); *ry=ax16(g_nav_ry); *b=g_nav_btn; return 1; }
