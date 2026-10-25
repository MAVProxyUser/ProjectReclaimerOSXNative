// latency.c (2026-10-25; user: "the window adds its own display delay on top of these numbers -- THIS is what I need you to
// calculate, cuz that feel is all that will matter to the player"): input -> screen latency, stage by stage, and a running
// per-second performance log. #included by peload.c after stats.c; the hooks (lat_*) are declared before wininput.c.
//
// Frame timeline, by engine frame number F (d3dgl_present's count, g_eng_present_no; ring of LT_N):
//   cam      the world camera's forward vector, seen while F was built (hb_capture_view: the first world-camera draw)
//   present  halo3's render thread presented F (sub_present / g_lag_t)
//   gpu      the GL helper's fence for F's blit signalled: F is drawn (glh_pq_publish)
//   win      the present thread flushed F into the Mac window (present_thread_fn, after CGLFlushDrawable)
//   screen   MODEL: the window server takes the new window contents at its next composite, which starts at a display refresh
//            and is shown at the refresh after: (next refresh after win) + one refresh period. Refresh times come from a
//            CVDisplayLink on the main display (the output time of the next refresh and the period); without one, from the
//            window screen's maximumFramesPerSecond (+1.5 periods on average). Scan-out reaches the middle of the screen half a
//            period later still (not included).
// Input onsets: a look input -- a raw mouse delta (wi_raw_mouse: the Mac mouse, or the probe) or a right stick leaving its
// dead zone (XInputGetState) -- after >= 250 ms without one, while the camera has not turned for 3 frames. Its frame on
// screen = the first frame whose camera forward turned by > 0.03 degrees from the frame before the onset; every stage is
// then measured from the onset. The pad's onset is stamped at the game's poll that first saw it (GameController updates
// in between are not seen: up to one poll interval more in reality). Discarded after 1 s without a turn (no camera: menu,
// dead, vehicle seats that don't turn with the mouse).
// PELOAD_LATPROBE=<secs> (test clients: quiet-run / target): every <secs> once gameplay runs, a mouse delta of
// PELOAD_LATPROBE_DX (40, alternating sign) through the raw-input path, exactly as a mouse moved by a player.
// Output: `[latency]` lines (each measured input and, every 10 s, the medians), the snapshot's "perf" object, and the CSV.
// PELOAD_LATENCY=0 = off.
//
// Per-second log (PELOAD_PERF_LOG=0 off): ~/Library/Application Support/ReclaimerMac/perf/<date>_<time>_<pid>.csv, one row a
// second while a game (menu or match) runs: engine fps, frames shown in the window, frame-time median / 95th / worst, draws per
// frame, objects by type and how many are in the camera's view, players, map, quality, frames allowed on the GPU, the lag of
// each stage for that second's frames, the last measured input -> screen latency. Every 10 s a `[perf]` line too. The 30 newest
// files are kept.
#include <CoreVideo/CoreVideo.h>
#include <sys/resource.h>
#define LT_N 512
typedef struct { volatile uint64_t fno; float fwd[3]; volatile int camok; double t_present, t_gpu, t_win, t_enc; } LtFrame;   // t_enc: the GL submission thread reached its present (all its GL calls sent)
static LtFrame g_lt[LT_N];
static int g_lt_on=-1;
static int lt_on(void){ if(g_lt_on<0){ const char*e=getenv("PELOAD_LATENCY"); g_lt_on=!(e && !atoi(e)); } return g_lt_on; }
static LtFrame* lt_at(uint64_t fno){ LtFrame*f=&g_lt[fno%LT_N]; if(f->fno!=fno){ memset(f,0,sizeof *f); f->fno=fno; } return f; }
static LtFrame* lt_get(uint64_t fno){ LtFrame*f=&g_lt[fno%LT_N]; return f->fno==fno?f:0; }

// ---- display refresh (CVDisplayLink) ----
static volatile double g_vs_next, g_vs_period;   // mono_s() time of the next refresh, and the period (seconds)
static CVReturn lt_vs_cb(CVDisplayLinkRef l,const CVTimeStamp*now,const CVTimeStamp*out,CVOptionFlags fi,CVOptionFlags*fo,void*ctx){
    (void)l;(void)fi;(void)fo;(void)ctx;
    static double tb; if(!tb){ mach_timebase_info_data_t t; mach_timebase_info(&t); tb=(double)t.numer/t.denom*1e-9; }
    double ahead=(double)((int64_t)(out->hostTime-now->hostTime))*tb;
    if(out->videoTimeScale && out->videoRefreshPeriod) g_vs_period=(double)out->videoRefreshPeriod/out->videoTimeScale;
    g_vs_next=mono_s()+ahead; return kCVReturnSuccess; }
static void lt_vs_start(void){ static int done; if(done) return; done=1;
    CVDisplayLinkRef l=0; if(CVDisplayLinkCreateWithCGDisplay(CGMainDisplayID(),&l)!=kCVReturnSuccess || !l) return;
    CVDisplayLinkSetOutputCallback(l,lt_vs_cb,0); CVDisplayLinkStart(l); }
static double lt_screen_time(double twin){   // when a frame flushed at twin is on the screen (the model above)
    double p=g_vs_period, nx=g_vs_next;
    if(p>0.0005 && p<0.1 && nx>0){ double k=ceil((twin-nx)/p); double v=nx+k*p; if(v<twin) v+=p; return v+p; }
    double hz=g_screen_hz>0?g_screen_hz:60; return twin+1.5/hz; }
static double lt_period(void){ return g_vs_period>0.0005?g_vs_period:1.0/(g_screen_hz>0?g_screen_hz:60); }

// ---- hooks ----
static uint64_t lt_cur_fno(void){ extern unsigned long g_eng_present_no_x; return (uint64_t)g_eng_present_no_x; }
void lat_cam(const float*vp){   // (draw thread) the frame under construction is g_eng_present_no_x + 1
    if(!lt_on()) return; LtFrame*f=lt_at(lt_cur_fno()+1); if(f->camok) return;
    float x=vp[12], y=vp[13], z=vp[14], n=sqrtf(x*x+y*y+z*z); if(!(n>1e-6f)) return;
    f->fwd[0]=x/n; f->fwd[1]=y/n; f->fwd[2]=z/n; __atomic_store_n(&f->camok,1,__ATOMIC_RELEASE); }
// halo3's render thread presented frame fno at t (d3dsubmit.c: SQ_PRESENT moves its time to g_lag_t; read here at gpu time)
static double g_perf_pt[256]; static volatile unsigned g_perf_pn;   // present times for the per-second frame-time stats
void lat_present(double t){ unsigned n=__atomic_fetch_add(&g_perf_pn,1,__ATOMIC_RELAXED); g_perf_pt[n&255]=t; }
void lat_enc(uint64_t fno,double t){ if(!lt_on()) return; lt_at(fno)->t_enc=t; }   // (d3dsubmit.c SQ_PRESENT, GL submission thread on)
void lat_gpu(uint64_t fno,double t){ if(!lt_on()) return; extern double g_lag_t[256]; LtFrame*f=lt_at(fno); f->t_gpu=t; f->t_present=g_lag_t[fno&255]; }
static volatile unsigned g_perf_wn;   // frames flushed into the window
void lat_win(uint64_t fno,double t){ __atomic_fetch_add(&g_perf_wn,1,__ATOMIC_RELAXED); if(!lt_on()) return; LtFrame*f=lt_get(fno); if(f) f->t_win=t; lt_vs_start(); }

// input onsets
static volatile double g_li_last;           // the last look input of any kind
static struct { volatile int pending; double t0; uint64_t fno0; float fwd0[3]; int src; } g_lo;
static pthread_mutex_t g_lo_mx=PTHREAD_MUTEX_INITIALIZER;
static int lt_fwd_of(uint64_t fno,float*out){ LtFrame*f=lt_get(fno); if(!f || !__atomic_load_n(&f->camok,__ATOMIC_ACQUIRE)) return 0; memcpy(out,f->fwd,12); return 1; }
static float lt_angle(const float*a,const float*b){ float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; if(d>1) d=1; if(d<-1) d=-1; return acosf(d)*57.29578f; }
void lat_input(int src){   // src 0 mouse, 1 pad, 2 probe
    if(!lt_on()) return; extern double g_gameplay_t0; double now=mono_s(); double last=g_li_last; g_li_last=now;
    if(src<0 || !g_gameplay_t0 || now-last<0.25) return;   // (src -1: a look input that only keeps the quiet timer running)
    pthread_mutex_lock(&g_lo_mx);
    if(!g_lo.pending){ uint64_t c=lt_cur_fno(); float a[3],b[3],d[3]; int still=0;
        // the camera of the newest frames must agree (not turning already): 3 frames back
        for(uint64_t k=c+1;k>=c-1 && k>2;k--) if(lt_fwd_of(k,a) && lt_fwd_of(k-1,b) && lt_fwd_of(k-2,d)){ still=lt_angle(a,b)<0.03f && lt_angle(b,d)<0.03f; break; }
        if(still){ g_lo.pending=1; g_lo.t0=now; g_lo.fno0=c; memcpy(g_lo.fwd0,a,12); g_lo.src=src; } }
    pthread_mutex_unlock(&g_lo_mx); }

// results
typedef struct { double in_present, in_gpu, in_win, in_screen, in_enc; int frames, src, haswin; } LtRes;
static LtRes g_lr[64]; static unsigned g_lr_n; static LtRes g_lr_last; static pthread_mutex_t g_lr_mx=PTHREAD_MUTEX_INITIALIZER;
static void lt_check(void){   // (present thread / GL thread / stats thread) finish a pending onset when its frame is known
    if(!g_lo.pending) return; pthread_mutex_lock(&g_lo_mx); if(!g_lo.pending){ pthread_mutex_unlock(&g_lo_mx); return; }
    double now=mono_s(); uint64_t c=lt_cur_fno(); int haswin=!win_hidden();
    if(now-g_lo.t0>1.0){ g_lo.pending=0; pthread_mutex_unlock(&g_lo_mx); return; }   // no turn within 1 s
    uint64_t turn=0; float f[3];
    for(uint64_t k=g_lo.fno0+1;k<=c+1;k++) if(lt_fwd_of(k,f) && lt_angle(f,g_lo.fwd0)>0.03f){ turn=k; break; }
    if(!turn){ pthread_mutex_unlock(&g_lo_mx); return; }
    // the first frame at or after it that reached each stage (the window may skip frames: a newer one shows the turn too)
    double tp=0, tg=0, tw=0, te=0;
    for(uint64_t k=turn;k<=c+1 && k<turn+64;k++){ LtFrame*e=lt_get(k); if(!e) continue;
        if(!te && e->t_enc>0) te=e->t_enc;
        if(!tp && e->t_present>0) tp=e->t_present; if(!tg && e->t_gpu>0) tg=e->t_gpu; if(!tw && e->t_win>0) tw=e->t_win; }
    if(haswin ? !tw : !tg){ pthread_mutex_unlock(&g_lo_mx); return; }   // not there yet (no helper: no GPU-done times)
    LtRes r={0}; r.src=g_lo.src; r.frames=(int)(turn-g_lo.fno0); r.haswin=haswin && tw>0;
    r.in_present=tp>0?(tp-g_lo.t0)*1e3:-1; r.in_gpu=tg>0?(tg-g_lo.t0)*1e3:-1; r.in_enc=te>0?(te-g_lo.t0)*1e3:-1;
    if(r.haswin){ r.in_win=(tw-g_lo.t0)*1e3; r.in_screen=(lt_screen_time(tw)-g_lo.t0)*1e3; }
    g_lo.pending=0; pthread_mutex_unlock(&g_lo_mx);
    pthread_mutex_lock(&g_lr_mx); g_lr[g_lr_n++%64]=r; g_lr_last=r; pthread_mutex_unlock(&g_lr_mx);
    static const char*const sn[3]={"mouse","stick","probe (mouse)"};
    char m[300]; int k;
    char enc[48]=""; if(r.in_enc>0) snprintf(enc,sizeof enc,"; GL submission thread done %.0f ms",r.in_enc);
    if(r.haswin) k=snprintf(m,sizeof m,"  [latency] %s -> halo3 presents the turned frame %.0f ms -> GPU done %.0f -> in the window %.0f -> on screen ~%.0f ms (display %.0f Hz; frame +%d after the input%s)\n",
        sn[r.src<3?r.src:0],r.in_present,r.in_gpu,r.in_win,r.in_screen,1.0/lt_period(),r.frames,enc);
    else k=snprintf(m,sizeof m,"  [latency] %s -> halo3 presents the turned frame %.0f ms -> GPU done %.0f ms (no window: hidden client; frame +%d after the input)\n",sn[r.src<3?r.src:0],r.in_present,r.in_gpu,r.frames);
    (void)!write(2,m,(size_t)k); }
static int lt_cmpd(const void*a,const void*b){ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:x>y; }
static double lt_median(double*v,int n){ if(n<=0) return -1; qsort(v,(size_t)n,sizeof *v,lt_cmpd); return n&1?v[n/2]:(v[n/2-1]+v[n/2])/2; }

// probe
static void* lt_probe_thread(void*a){ (void)a; pthread_setname_np("shim-latprobe"); extern double g_gameplay_t0; extern void wi_probe_mouse(int dx);
    double every=atof(getenv("PELOAD_LATPROBE")); if(every<0.5) every=0.5; const char*e=getenv("PELOAD_LATPROBE_DX"); int dx=e?atoi(e):40, sgn=1;
    for(;;){ usleep((useconds_t)(every*1e6)); if(!g_gameplay_t0 || mono_s()-g_gameplay_t0<5) continue;
        g_li_last=0; lat_input(2); wi_probe_mouse(sgn*dx); sgn=-sgn; }
    return 0; }
static void lt_probe_start(void){ static int done; if(done || !getenv("PELOAD_LATPROBE")) return; done=1; pthread_t t; pthread_create(&t,0,lt_probe_thread,0); pthread_detach(t); }

// ---- the per-second performance log (stats thread, perf_tick every 100 ms) ----
static int strcmp_ptr(const void*a,const void*b){ return strcmp(*(const char*const*)a,*(const char*const*)b); }
static FILE*g_perf_f;   // (g_perf_json, the snapshot's "perf", is in stats.c)
static void perf_open(void){ static int tried; if(tried) return; tried=1; const char*e=getenv("PELOAD_PERF_LOG"); if(e && !atoi(e)) return;
    const char*h=getenv("HOME"); if(!h) return; char d[1024]; snprintf(d,sizeof d,"%s/Library/Application Support/ReclaimerMac/perf",h); mkdir(d,0755);
    // keep the 30 newest
    { DIR*dd=opendir(d); char*names[512]; int n=0; struct dirent*de; while(dd && (de=readdir(dd)) && n<512) if(strstr(de->d_name,".csv")) names[n++]=strdup(de->d_name); if(dd) closedir(dd);
      qsort(names,(size_t)n,sizeof *names,(int(*)(const void*,const void*))strcmp_ptr); for(int i=0;i<n-29;i++){ char p[1300]; snprintf(p,sizeof p,"%s/%s",d,names[i]); unlink(p); } for(int i=0;i<n;i++) free(names[i]); }
    time_t now=time(0); struct tm tm; localtime_r(&now,&tm); char p[1300];
    snprintf(p,sizeof p,"%s/%04d%02d%02d_%02d%02d%02d_%d.csv",d,tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,(int)getpid());
    g_perf_f=fopen(p,"w"); if(!g_perf_f) return; setvbuf(g_perf_f,0,_IOLBF,0);
    fprintf(g_perf_f,"time,up_s,in_match,map,players,fps,window_fps,frame_ms_p50,frame_ms_p95,frame_ms_max,draws_per_frame,objects,bipeds,vehicles,weapons,equipment,projectiles,scenery,other,in_view,"
        "quality,gpu_frames,submit_thread,present_to_gpu_ms,present_to_window_ms,input_to_screen_ms,input_to_window_ms,input_to_present_ms,inputs_measured,cpu_pct\n");
    char m[1400]; int k=snprintf(m,sizeof m,"  [perf] per-second log: %s\n",p); (void)!write(2,m,(size_t)k); }
static void perf_objects(int*cnt,int*inview,int*total){   // object table: counts by type, and centres inside the camera's frustum
    memset(cnt,0,sizeof(int)*8); *inview=0; *total=0;
    uint64_t arr=hb_object_array(); if(!arr) return; uint8_t hdr[0x60]; if(!hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) return;
    int32_t maxc=*(int32_t*)(hdr+0x20), esz=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t data=*(uint64_t*)(hdr+0x48);
    if(esz!=0x18 || hi<=0 || hi>maxc || hi>8192) return;
    static uint8_t tab[8192*0x18]; if(!hb_read(data,tab,(size_t)hi*0x18)) return;
    float vp[32]; int camok=0; for(int t=0;t<20;t++){ unsigned s0=g_hb_vpseq; if(s0&1) continue; memcpy(vp,g_hb_vp,sizeof vp); if(g_hb_vpseq==s0){ camok=vp[28]||vp[29]||vp[30]; break; } }
    for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e) continue; int t=e[3]; (*total)++;
        int c=t==0?0:t==1||t==12?1:t==2?2:t==3?3:t==5?4:t==6?5:6; cnt[c]++;
        if(!camok || t==5) continue; uint64_t p=*(const uint64_t*)(e+0x10); float s[4]; if(!p || !hb_read(p+0x1c,s,16)) continue;
        float cx=vp[0]*s[0]+vp[1]*s[1]+vp[2]*s[2]+vp[3], cy=vp[4]*s[0]+vp[5]*s[1]+vp[6]*s[2]+vp[7], cw=vp[12]*s[0]+vp[13]*s[1]+vp[14]*s[2]+vp[15];
        float r=s[3]>0&&s[3]<100?s[3]:0; if(cw+r>0.05f && fabsf(cx)<=cw+r*2 && fabsf(cy)<=cw+r*2) (*inview)++; } }
static double perf_cpu(void){ static double lu, lt; struct rusage ru; getrusage(RUSAGE_SELF,&ru); double u=ru.ru_utime.tv_sec+ru.ru_utime.tv_usec*1e-6+ru.ru_stime.tv_sec+ru.ru_stime.tv_usec*1e-6, t=mono_s();
    double pct=lt>0&&t>lt?(u-lu)/(t-lt)*100:0; lu=u; lt=t; return pct; }
void perf_tick(void){   // (stats thread, every ~100 ms)
    lt_check(); lt_probe_start(); lt_vs_start();
    static double tl; double now=mono_s(); if(!tl){ tl=now; return; } if(now-tl<1.0) return; double dt=now-tl; tl=now;
    extern double g_gameplay_t0, g_menu_ready_t0; if(!g_gameplay_t0 && !g_menu_ready_t0) return;
    perf_open();
    // frame times of this second's presents
    static unsigned pn0; unsigned pn=g_perf_pn; int nf=(int)(pn-pn0); if(nf>255) nf=255; double ft[256]; int nft=0;
    for(int i=1;i<nf;i++){ double a=g_perf_pt[(pn-nf+i-1)&255], b=g_perf_pt[(pn-nf+i)&255]; if(b>a && b-a<2.0) ft[nft++]=(b-a)*1e3; }
    double fps=(pn-pn0)/dt; pn0=pn; double p50=-1,p95=-1,mx=-1; if(nft){ qsort(ft,(size_t)nft,sizeof *ft,lt_cmpd); p50=ft[nft/2]; p95=ft[(int)(nft*0.95)<nft?(int)(nft*0.95):nft-1]; mx=ft[nft-1]; }
    static unsigned wn0; unsigned wn=g_perf_wn; double wfps=(wn-wn0)/dt; wn0=wn;
    static unsigned long dr0; static unsigned pnd0; double dpf=pn>pnd0?(double)(g_draws_total-dr0)/(pn-pnd0):0; dr0=g_draws_total; pnd0=pn;
    // stage lags of this second's frames
    double sg=0, sw=0; int ng=0, nw=0; uint64_t c=lt_cur_fno();
    for(uint64_t k=c>LT_N?c-LT_N+1:1;k<=c;k++){ LtFrame*f=lt_get(k); if(!f || f->t_present<=0) continue;
        if(f->t_gpu>0 && f->t_gpu>now-dt && f->t_gpu<=now){ sg+=f->t_gpu-f->t_present; ng++; }
        if(f->t_win>0 && f->t_win>now-dt && f->t_win<=now){ sw+=f->t_win-f->t_present; nw++; } }
    // input -> screen: the median of the last 64 measurements
    double vs[64], vw[64], vp_[64]; int nm=0, nmw=0; pthread_mutex_lock(&g_lr_mx); int have=g_lr_n<64?(int)g_lr_n:64;
    for(int i=0;i<have;i++){ vp_[nm]=g_lr[i].in_present; if(g_lr[i].haswin){ vs[nmw]=g_lr[i].in_screen; vw[nmw]=g_lr[i].in_win; nmw++; } nm++; } pthread_mutex_unlock(&g_lr_mx);
    double ms_scr=lt_median(vs,nmw), ms_win=lt_median(vw,nmw), ms_pre=lt_median(vp_,nm);
    int cnt[8], inview, total; perf_objects(cnt,&inview,&total);
    char map[64]=""; { char mp[1024]=""; hb_current_map(mp,sizeof mp); const char*b=strrchr(mp,'/'); b=b?b+1:mp; snprintf(map,sizeof map,"%s",b); char*dot=strrchr(map,'.'); if(dot)*dot=0; }
    int players=st_player_count(); int qn=st_qual_now(); const char*q=qn>=0?g_qp_names[qn]:(g_st_qual[0]?g_st_qual:"");
    extern volatile int g_gpu_frames; int sub=sub_on(); double cpu=perf_cpu();
    double pg=ng?sg/ng*1e3:-1, pw=nw?sw/nw*1e3:-1;
    snprintf(g_perf_json,sizeof g_perf_json,"{\"fps\":%.1f,\"window_fps\":%.1f,\"frame_ms\":[%.1f,%.1f,%.1f],\"draws\":%.0f,\"objects\":%d,\"in_view\":%d,\"bipeds\":%d,\"vehicles\":%d,"
        "\"projectiles\":%d,\"present_to_gpu_ms\":%.0f,\"present_to_window_ms\":%.0f,\"input_to_screen_ms\":%.0f,\"input_to_window_ms\":%.0f,\"input_to_present_ms\":%.0f,\"inputs\":%d,\"refresh_hz\":%.0f,\"cpu_pct\":%.0f}",
        fps,wfps,p50,p95,mx,dpf,total,inview,cnt[0],cnt[1],cnt[4],pg,pw,ms_scr,ms_win,ms_pre,nm,1.0/lt_period(),cpu);
    if(g_perf_f){ time_t tt=time(0); struct tm tm; localtime_r(&tt,&tm); launch_time_init();
        fprintf(g_perf_f,"%02d:%02d:%02d,%.0f,%d,%s,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%.0f\n",
            tm.tm_hour,tm.tm_min,tm.tm_sec,now-g_launch_mono,g_gameplay_t0?1:0,map,players,fps,wfps,p50,p95,mx,dpf,total,cnt[0],cnt[1],cnt[2],cnt[3],cnt[4],cnt[5],cnt[6],inview,
            q,g_gpu_frames,sub,pg,pw,ms_scr,ms_win,ms_pre,nm,cpu); }
    static int n10; if(++n10>=10){ n10=0; char m[400]; int k=snprintf(m,sizeof m,"  [perf] %.1f fps (window %.1f) | frame %.1f / %.1f / %.1f ms (median / 95%% / worst) | %.0f draws | objects %d, %d in view (%d bipeds, %d vehicles) | present->GPU %.0f ms, ->window %.0f ms | input->screen %.0f ms (%d measured)\n",
        fps,wfps,p50,p95,mx,dpf,total,inview,cnt[0],cnt[1],pg,pw,ms_scr,nm); (void)!write(2,m,(size_t)k); } }
