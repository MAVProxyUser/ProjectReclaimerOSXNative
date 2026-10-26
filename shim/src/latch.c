// latch.c (2026-10-25; user: "is there anything novel we can do to improve hugegrass? 239ms of input lag is huge"): late
// camera update -- the newest mouse movement applied to a frame's camera when its constant buffer goes to the GPU, 40-110 ms
// after halo3 built the frame (the idea of VR "late latching" / NVIDIA Reflex 2's Frame Warp, done on the camera matrix rather
// than on the finished image, so the HUD and the first-person weapon stay where they are). #included after latency.c.
//
// 1. Input: every raw mouse delta posted to the game (wi_raw_mouse -> latch_received), with its time (an 8192-event ring of
//    cumulative counts).
// 2. Learner: for every frame, the engine's own camera (yaw = atan2(fy, fx), pitch = asin(fz) of the view-projection's row 3)
//    at the moment its camera buffer is uploaded, against the counts received in windows ending tau before that moment
//    (tau = 0 .. 250 ms): least-squares degrees per count for x -> yaw and y -> pitch and the fit (R^2); the tau that fits
//    best is how old a frame's input is at upload. `[latch]` every 10 s (frames, tau, k, R^2, direction reversals per
//    second of the engine's and of the shown yaw -- the smoothness check). Frames while the local player sits in a vehicle
//    are left out (the camera orbits the vehicle there).
//    (A first learner snapshotted the counts the game had READ -- GetRawInputData -- at the render thread's frame start and
//    at the engine loop's start: R^2 0.25-0.6, halo3 reflects input later than any of those points.)
// 3. Apply (PELOAD_LATCH=1, once >= 200 moving frames fit with R^2 >= PELOAD_LATCH_MINR2, 0.5; HugeGrass: 0.82-0.87):
//    a predictive angle that follows the mouse, A(t) = off + k * C(t), with `off` pulled toward the engine's camera each
//    frame (PELOAD_LATCH_TC, 0.1 s; a gap over 20 degrees snaps it); the correction A(now) - engine angle, clamped to
//    PELOAD_LATCH_MAX degrees (12) and the pitch range seen; none in a vehicle. The view-projection rows a (xyz) / d (w)
//    become a' = R a, d' = d + a.c - a'.c (R = the extra rotation: yaw about world z, then pitch about the camera's right
//    axis; c = the camera position, row 7). Every constant buffer whose bytes at its bound offset pass hb_is_world_camera is
//    patched in its GPU copy only (the CPU copy stays the game's) -- d3dgl_bind_ubo / d3dgl_ubo_batch call latch_src and
//    copy such a buffer again every frame (D3DObj latch_f); the correction is computed once per frame at its first world
//    camera (latch_overlay_vp: the overlay's capture often comes before the upload), so the overlay and the latency probe
//    see the shown camera. (A first apply added k * the counts of the last tau ms to each frame: the shown camera reversed
//    direction 11.7 times a second against the engine's 0.9 -- the engine applies input in steps.)
// Measured (PELOAD_AB=latch:20, Tour Guide's HugeGrass, ~97 players, ~42 fps): input -> screen 120 -> 84 ms; yaw reversals
// 0.8 -> 1.2-1.3 a second. Local Valhalla ~230 fps: 62 -> 49 ms. PELOAD_LATCH_TEST=<counts> (test clients): a back-and-forth
// mouse motion, half of every 2 s, so the learner has data. PELOAD_LATCH_SKYCHECK=1 counts unpatched matrices facing the
// camera's way (none were camera matrices). PELOAD_LATCH=0 = off entirely (no learning either). Mouse only: pad look is
// a rate the engine integrates with acceleration.
static volatile int64_t g_lc_rx, g_lc_ry;
static int g_lc_mode=-1;   // 0 off, 1 learn only (the default: cheap), 2 apply
static int lc_mode(void){ if(g_lc_mode<0){ const char*e=getenv("PELOAD_LATCH"); g_lc_mode=e&&atoi(e)?2:0; } return g_lc_mode; }   // off unless PELOAD_LATCH=1 (2026-10-25: mouse only, and the user plays with a controller)
static void lc_event(int dx,int dy);
void latch_received(int dx,int dy){ __atomic_add_fetch(&g_lc_rx,dx,__ATOMIC_RELAXED); __atomic_add_fetch(&g_lc_ry,dy,__ATOMIC_RELAXED); if(lc_mode()) lc_event(dx,dy); }

// learner (second version: the first, per-frame snapshots of the counts the game had read, fitted with R^2 0.25-0.6 -- halo3
// reflects input later than any snapshot point, and its camera is smoothed between ticks). Now: the counts RECEIVED in a time
// window. With C(t) = cumulative counts received up to time t and t_u(F) = when frame F's camera buffer is uploaded, try
// lags tau = 0, 5, .. 250 ms: dyaw(F) ~ k * (C(t_u(F) - tau) - C(t_u(F-1) - tau)); the tau with the best fit is how old the
// input in a frame is at upload, and the correction is k * (C(now) - C(now - tau)).
#define LC_EV 8192
#define LC_NT 51
static struct { double t; int64_t x, y; } g_lc_ev[LC_EV]; static volatile uint64_t g_lc_evn;   // cumulative counts after each event
static pthread_mutex_t g_lc_evmx=PTHREAD_MUTEX_INITIALIZER;
static void lc_event(int dx,int dy){ pthread_mutex_lock(&g_lc_evmx); uint64_t n=g_lc_evn; int64_t x=n?g_lc_ev[(n-1)%LC_EV].x:0, y=n?g_lc_ev[(n-1)%LC_EV].y:0;
    g_lc_ev[n%LC_EV].t=mono_s(); g_lc_ev[n%LC_EV].x=x+dx; g_lc_ev[n%LC_EV].y=y+dy; g_lc_evn=n+1; pthread_mutex_unlock(&g_lc_evmx); }
static void lc_at(double t,int64_t*x,int64_t*y){   // cumulative counts received up to time t
    pthread_mutex_lock(&g_lc_evmx); uint64_t n=g_lc_evn, lo=n>LC_EV?n-LC_EV:0, hi=n; *x=0; *y=0;
    if(n && g_lc_ev[(n-1)%LC_EV].t<=t){ *x=g_lc_ev[(n-1)%LC_EV].x; *y=g_lc_ev[(n-1)%LC_EV].y; pthread_mutex_unlock(&g_lc_evmx); return; }
    while(lo<hi){ uint64_t mid=(lo+hi)/2; if(g_lc_ev[mid%LC_EV].t<=t) lo=mid+1; else hi=mid; }   // first event after t
    if(lo>0 && lo-1>=(n>LC_EV?n-LC_EV:0)){ *x=g_lc_ev[(lo-1)%LC_EV].x; *y=g_lc_ev[(lo-1)%LC_EV].y; }
    else if(n>LC_EV){ *x=g_lc_ev[lo%LC_EV].x; *y=g_lc_ev[lo%LC_EV].y; }   // (older than the ring: its oldest value)
    pthread_mutex_unlock(&g_lc_evmx); }
static struct { uint64_t f; double yaw, pitch, t; int ok; } g_lc_prev;
static double g_lc_s[LC_NT][2][3];   // [tau][axis][Sxy, Sxx, Syy]
static uint64_t g_lc_nmov, g_lc_n;
static double g_lc_k[2], g_lc_tau, g_lc_r2[2], g_lc_pmin=-80, g_lc_pmax=80; static int g_lc_best=-1;
static double lc_wrap(double d){ while(d>180) d-=360; while(d<-180) d+=360; return d; }
static struct { double last, dlast; uint64_t rev; } g_lc_sm[2];   // [engine, shown] yaw: direction reversals (smoothness)
static void lc_smooth(int w,double yaw){ double d=lc_wrap(yaw-g_lc_sm[w].last); g_lc_sm[w].last=yaw; if(fabs(d)<0.002) return;
    if(g_lc_sm[w].dlast!=0 && (d>0)!=(g_lc_sm[w].dlast>0)) g_lc_sm[w].rev++; g_lc_sm[w].dlast=d; }
static void lc_fit(void){ int b=0; double br=-1;
    for(int i=0;i<LC_NT;i++){ double*s=g_lc_s[i][0]; double r=s[1]>0&&s[2]>0?(s[0]*s[0])/(s[1]*s[2]):0; if(r>br){ br=r; b=i; } }
    g_lc_best=b; g_lc_tau=b*0.005;
    for(int a=0;a<2;a++){ double*s=g_lc_s[b][a]; g_lc_k[a]=s[1]>0?s[0]/s[1]:0; g_lc_r2[a]=s[1]>0&&s[2]>0?(s[0]*s[0])/(s[1]*s[2]):0; } }
static void lc_learn(uint64_t f,const float*m){
    double fx=m[12], fy=m[13], fz=m[14], n=sqrt(fx*fx+fy*fy+fz*fz); if(!(n>1e-9)) return; fx/=n; fy/=n; fz/=n;
    double yaw=atan2(fy,fx)*57.29578, pitch=asin(fz>1?1:fz<-1?-1:fz)*57.29578, t=mono_s();
    if(pitch<g_lc_pmin) g_lc_pmin=pitch; if(pitch>g_lc_pmax) g_lc_pmax=pitch;
    if(g_lc_prev.ok && f==g_lc_prev.f+1){ double dy=lc_wrap(yaw-g_lc_prev.yaw), dp=pitch-g_lc_prev.pitch;
        if(fabs(dy)<45 && fabs(dp)<45){ g_lc_n++; int mov=fabs(dy)>0.0005;
            for(int i=0;i<LC_NT;i++) for(int a=0;a<2;a++) for(int q=0;q<3;q++) g_lc_s[i][a][q]*=0.998;   // forget slowly (~350-frame half-life): a sensitivity change re-learns
            for(int i=0;i<LC_NT;i++){ int64_t x0,y0,x1,y1; lc_at(g_lc_prev.t-i*0.005,&x0,&y0); lc_at(t-i*0.005,&x1,&y1); double cx=(double)(x1-x0), cy=(double)(y1-y0);
                if(cx!=0) mov=1;
                g_lc_s[i][0][0]+=cx*dy; g_lc_s[i][0][1]+=cx*cx; g_lc_s[i][0][2]+=dy*dy;
                g_lc_s[i][1][0]+=cy*dp; g_lc_s[i][1][1]+=cy*cy; g_lc_s[i][1][2]+=dp*dp; }
            if(mov) g_lc_nmov++; } }
    g_lc_prev.f=f; g_lc_prev.yaw=yaw; g_lc_prev.pitch=pitch; g_lc_prev.t=t; g_lc_prev.ok=1;
    static double tl, tfit; if(!tl) tl=t; if(t-tfit>=1.0){ tfit=t; lc_fit(); }
    if(t-tl>=10.0 && g_lc_n){ double dt=t-tl; tl=t;
        char rv[96]; snprintf(rv,sizeof rv," | yaw reversals per s: engine %.1f, shown %.1f",g_lc_sm[0].rev/dt,g_lc_sm[1].rev/dt); g_lc_sm[0].rev=g_lc_sm[1].rev=0;
        char msg[400]; int k=snprintf(msg,sizeof msg,"  [latch] %llu frames (%llu moving): a frame's camera holds the input up to %.0f ms before its upload; yaw %.5f deg/count (R^2 %.3f), pitch %.5f (R^2 %.3f)%s%s\n",
            (unsigned long long)g_lc_n,(unsigned long long)g_lc_nmov,g_lc_tau*1e3,g_lc_k[0],g_lc_r2[0],g_lc_k[1],g_lc_r2[1],
            lc_mode()==2?(g_lc_nmov>=200 && g_lc_r2[0]>=0.5?" -> applying":" -> not applying yet (fit)"):"",rv);
        (void)!write(2,msg,(size_t)k); } }

// apply
static uint64_t g_lc_ff; static double g_lc_dyaw, g_lc_dpitch; static float g_lc_rot[9]; static int g_lc_on;
static volatile int g_latch_vp_ok; static float g_latch_vp[32]; static uint64_t g_latch_vp_f;   // the patched camera (overlay)
static void lc_rotation(const float*m,double dyaw,double dpitch,float*R){
    double fx=m[12], fy=m[13], fz=m[14], n=sqrt(fx*fx+fy*fy+fz*fz); fx/=n; fy/=n; fz/=n;
    double cy=cos(dyaw/57.29578), sy=sin(dyaw/57.29578);
    double Rz[9]={cy,-sy,0, sy,cy,0, 0,0,1};
    double gx=cy*fx-sy*fy, gy=sy*fx+cy*fy, gz=fz;                  // forward after the yaw
    double ux=gy*1-gz*0, uy=gz*0-gx*1, uz=0; double un=sqrt(ux*ux+uy*uy+uz*uz); if(un<1e-9){ ux=1; uy=0; un=1; } ux/=un; uy/=un; uz/=un;   // u = f x z: +angle looks up
    double th=dpitch/57.29578, c=cos(th), s=sin(th), t=1-c;
    double Ru[9]={t*ux*ux+c, t*ux*uy-s*uz, t*ux*uz+s*uy,  t*ux*uy+s*uz, t*uy*uy+c, t*uy*uz-s*ux,  t*ux*uz-s*uy, t*uy*uz+s*ux, t*uz*uz+c};
    for(int i=0;i<3;i++) for(int j=0;j<3;j++){ double v=0; for(int k=0;k<3;k++) v+=Ru[i*3+k]*Rz[k*3+j]; R[i*3+j]=(float)v; } }
static void lc_patch(float*m,const float*R){   // rows 0-3 of a view-projection around the camera position (row 7)
    const float*c=m+28;
    for(int r=0;r<4;r++){ float*a=m+r*4; float b[3]={R[0]*a[0]+R[1]*a[1]+R[2]*a[2], R[3]*a[0]+R[4]*a[1]+R[5]*a[2], R[6]*a[0]+R[7]*a[1]+R[8]*a[2]};
        a[3]+=a[0]*c[0]+a[1]*c[1]+a[2]*c[2]-(b[0]*c[0]+b[1]*c[1]+b[2]*c[2]); a[0]=b[0]; a[1]=b[1]; a[2]=b[2]; } }
// (encoding thread, a constant buffer about to be copied for the GPU) the bytes to copy: cb->data, or a patched copy in tmp
static int lc_is_cam(const D3DObj*cb,uint64_t sub){ return cb->data && cb->size>=sub+128 && hb_is_world_camera((const float*)((const uint8_t*)cb->data+sub)); }
// the local player sits in a vehicle (its unit's parent +0x10 is an object): the camera orbits the vehicle there, which a
// rotation about the camera position would not reproduce -- or has no unit (dead / respawning / spectating): no correction
// and no learning then (checked every 100 ms; the local unit comes from the stats snapshot, so the stats server must run)
static int lc_in_vehicle(void){ static double tl; static int res; double t=mono_s(); if(t-tl<0.1) return res; tl=t; res=1;
    uint32_t u=g_hb_local_unit; if(u==0xFFFFFFFFu) return 1;   // no live local player (dead, respawning, spectating: those cameras don't follow the mouse alike)
    res=0; uint64_t arr=hb_object_array(); uint8_t hdr[0x60];
    if(!arr || !hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) return 0;
    int32_t hi=*(int32_t*)(hdr+0x3c); uint64_t data=*(uint64_t*)(hdr+0x48); uint32_t i=u&0xFFFF; if((int32_t)i>=hi) return 0;
    uint8_t e[0x18]; uint64_t p=0; uint32_t parent=0xFFFFFFFFu;
    if(!hb_read(data+(uint64_t)i*0x18,e,sizeof e) || *(uint16_t*)e!=(uint16_t)(u>>16) || !(p=*(uint64_t*)(e+0x10)) || !hb_read(p+0x10,&parent,4)) return 0;
    res=parent!=0xFFFFFFFFu; return res; }
static void lc_prepare(const float*m){   // once per frame, at its first world camera (the overlay's capture or a buffer upload)
    int mode=lc_mode(); extern unsigned long g_eng_present_no_x; uint64_t f=g_eng_present_no_x+1;
    if(f!=g_lc_ff){ g_lc_ff=f; g_lc_on=0; int veh=lc_in_vehicle(); if(!veh) lc_learn(f,m); else g_lc_prev.ok=0;
        static double minr2=-1; if(minr2<0){ const char*e=getenv("PELOAD_LATCH_MINR2"); minr2=e?atof(e):0.5; }
        static int anch; if(veh) anch=0;   // (re-anchored on leaving the vehicle)
        if(mode==2 && !veh && g_lc_best>=0 && g_lc_nmov>=200 && g_lc_r2[0]>=minr2 && g_lc_k[0]!=0){
            // predictive (second version; the first added k * (counts of the last tau ms) to each frame and the shown camera
            // reversed direction 11.7 times a second against the engine's 0.9 -- the engine applies input in steps): the shown
            // angle follows the mouse itself, A(t) = off + k * C(t), and `off` is pulled toward the engine's camera every frame
            // (time constant PELOAD_LATCH_TC, 0.1 s) at the input time of that frame (upload - tau); a gap over 20 degrees
            // (respawn, a vehicle seat, a scripted camera) snaps it. The correction is A(now) - the engine's angle, clamped.
            double t=mono_s(); int64_t xn,yn,xi,yi; lc_at(t,&xn,&yn); lc_at(t-g_lc_tau,&xi,&yi);
            static double offy, offp, tlast; static double tc=-1; if(tc<0){ const char*e=getenv("PELOAD_LATCH_TC"); tc=e?atof(e):0.1; }
            double fn=sqrt(m[12]*m[12]+m[13]*m[13]+m[14]*m[14]); double ey=atan2(m[13],m[12])*57.29578, ep=asin(fmax(-1,fmin(1,m[14]/fn)))*57.29578;
            double ky=g_lc_k[0], kp=g_lc_r2[1]>=minr2?g_lc_k[1]:0;
            double ry=lc_wrap(ey-(offy+ky*(double)xi)), rp=ep-(offp+kp*(double)yi);
            double a=t>tlast?1-exp(-(t-tlast)/tc):1; tlast=t;
            if(!anch || fabs(ry)>20 || fabs(rp)>20){ offy+=ry; offp+=rp; anch=1; } else { offy+=a*ry; offp+=a*rp; }
            static double mx=-1; if(mx<0){ const char*e=getenv("PELOAD_LATCH_MAX"); mx=e?atof(e):12; }
            double dyaw=lc_wrap(offy+ky*(double)xn-ey), dpitch=(offp+kp*(double)yn)-ep;
            if(dyaw>mx) dyaw=mx; if(dyaw<-mx) dyaw=-mx; if(dpitch>mx) dpitch=mx; if(dpitch<-mx) dpitch=-mx;
            double p=ep; if(p+dpitch>g_lc_pmax) dpitch=g_lc_pmax-p; if(p+dpitch<g_lc_pmin) dpitch=g_lc_pmin-p;
            g_lc_dyaw=dyaw; g_lc_dpitch=dpitch; g_lc_on=fabs(dyaw)>0.001 || fabs(dpitch)>0.001; if(g_lc_on) lc_rotation(m,dyaw,dpitch,g_lc_rot); }
        { double ey=atan2(m[13],m[12])*57.29578; lc_smooth(0,ey); lc_smooth(1,ey+(g_lc_on?g_lc_dyaw:0)); } }
}
// PELOAD_LATCH_SKYCHECK=1: count constant buffers holding a perspective view-projection that faces where the world camera faces
// but is NOT a world camera (no camera position: e.g. a sky drawn with the rotation only) -- those would not be patched and
// would trail the world during a corrected turn
static uint64_t g_lc_sky, g_lc_skyall; static float g_lc_lastfwd[3];
static void lc_skycheck(const D3DObj*cb,uint64_t sub){
    if(cb->size<sub+64) return; const float*m=(const float*)((const uint8_t*)cb->data+sub);
    if(!cb_looks_viewproj(m,64)) return; g_lc_skyall++;
    float fx=m[12],fy=m[13],fz=m[14], n=sqrtf(fx*fx+fy*fy+fz*fz); if(!(n>1e-6f)) return;
    float d=(fx*g_lc_lastfwd[0]+fy*g_lc_lastfwd[1]+fz*g_lc_lastfwd[2])/n; if(d>0.999f){ g_lc_sky++;
        static int shown; if(shown<6 && g_lc_sky%500==1){ shown++; char m2[400]; int k=snprintf(m2,sizeof m2,"  [latch] unpatched, facing the camera's way: buffer %llu B at +%llu | rows 0-3:",(unsigned long long)cb->size,(unsigned long long)sub);
            for(int i=0;i<16;i++) k+=snprintf(m2+k,sizeof m2-k," %.3g",m[i]);
            if(cb->size>=sub+128) k+=snprintf(m2+k,sizeof m2-k," | row 7: %.3g %.3g %.3g %.3g",m[28],m[29],m[30],m[31]);
            m2[k++]='\n'; (void)!write(2,m2,(size_t)k); } } }
const uint8_t* latch_src(D3DObj*cb,uint64_t sub,uint64_t n,uint8_t*tmp,size_t cap){
    int mode=lc_mode(); if(!mode || !cb || !cb->data) return cb?cb->data:0;
    if(!lc_is_cam(cb,sub)){ static int sc=-1; if(sc<0) sc=getenv("PELOAD_LATCH_SKYCHECK")?1:0;
        if(sc){ lc_skycheck(cb,sub); static double tl; double t=mono_s(); if(t-tl>10){ tl=t; char m2[160]; int k=snprintf(m2,sizeof m2,"  [latch] other view-projection buffers: %llu, facing the camera's way: %llu\n",(unsigned long long)g_lc_skyall,(unsigned long long)g_lc_sky); (void)!write(2,m2,(size_t)k); } }
        return cb->data; }
    { const float*mm=(const float*)((const uint8_t*)cb->data+sub); float n2=sqrtf(mm[12]*mm[12]+mm[13]*mm[13]+mm[14]*mm[14]); if(n2>1e-6f){ g_lc_lastfwd[0]=mm[12]/n2; g_lc_lastfwd[1]=mm[13]/n2; g_lc_lastfwd[2]=mm[14]/n2; } }
    extern unsigned long g_eng_present_no_x; uint64_t f=g_eng_present_no_x+1; const float*m=(const float*)((const uint8_t*)cb->data+sub);
    lc_prepare(m);
    cb->latch_f=f;   // (camera buffers are copied again every frame while this runs)
    static uint64_t npat, nbig; static double tlog;
    if(g_lc_on && n>cap){ static __thread uint8_t*big; static __thread size_t bigcap; if(bigcap<n){ free(big); big=malloc((size_t)n); bigcap=big?(size_t)n:0; } tmp=big; cap=bigcap; nbig++; }   // (a big constant buffer)
    if(!g_lc_on || n>cap){ if(mode==2){ lat_cam(m); } return cb->data; }
    memcpy(tmp,cb->data,(size_t)(n<cb->size?n:cb->size)); float*pm=(float*)(tmp+sub); lc_patch(pm,g_lc_rot); npat++;
    { double t=mono_s(); if(t-tlog>10){ tlog=t; char msg[200]; int k=snprintf(msg,sizeof msg,"  [latch] camera buffer %llu bytes, camera at +%llu: %llu patched copies (%llu over 64 KB), last correction yaw %.2f pitch %.2f deg\n",
        (unsigned long long)cb->size,(unsigned long long)sub,(unsigned long long)npat,(unsigned long long)nbig,g_lc_dyaw,g_lc_dpitch); (void)!write(2,msg,(size_t)k); } }
    lat_cam(pm); memcpy(g_latch_vp,pm,sizeof g_latch_vp); g_latch_vp_f=f; g_latch_vp_ok=1;
    return tmp; }
int latch_applying(void){ return lc_mode()==2; }
int latch_off(void){ return lc_mode()==0; }
// the stats snapshot's "latch": mode (off / learning / on), whether it is correcting now, what it learned, the last correction
void latch_json(char*o,size_t n){ int m=lc_mode();
    snprintf(o,n,"{\"mode\":\"%s\",\"applying\":%s,\"deg_per_count\":%.5f,\"r2\":%.2f,\"input_age_ms\":%.0f,\"moving_frames\":%llu,\"last_deg\":%.2f,\"in_vehicle_or_dead\":%s}",
        m==0?"off":m==1?"learning":"on",(m==2 && g_lc_on)?"true":"false",g_lc_k[0],g_lc_r2[0],g_lc_tau*1e3,(unsigned long long)g_lc_nmov,g_lc_dyaw,lc_in_vehicle()?"true":"false"); }
int ab_latch(int set){ lc_mode(); if(set>=0) g_lc_mode=set?2:1; return g_lc_mode==2; }   // PELOAD_AB=latch: applied vs learning only
// the overlay's camera for this frame: the patched one when the latch moved it
const float* latch_overlay_vp(const float*engine){   // (the draw that captures the world camera: often before its buffer upload)
    if(lc_mode()!=2) return engine; lc_prepare(engine); if(!g_lc_on) return engine;
    static __thread float vp[32]; memcpy(vp,engine,sizeof vp); lc_patch(vp,g_lc_rot); return vp; }

// PELOAD_LATCH_TEST=<amplitude>: a smooth back-and-forth mouse motion during matches (learner data in test clients)
static void* lc_test_thread(void*a){ (void)a; pthread_setname_np("shim-latchtest"); extern double g_gameplay_t0; extern void wi_probe_mouse(int dx);
    extern void wi_probe_mouse_xy(int dx,int dy);
    double amp=atof(getenv("PELOAD_LATCH_TEST")); if(amp<1) amp=20; double ax=0, ay=0;
    for(;;){ usleep(4000); if(!g_gameplay_t0 || mono_s()-g_gameplay_t0<5) continue; double t=mono_s();
        if(fmod(t,2.0)>=1.0) continue;   // half of every 2 s still: the latency probe (PELOAD_LATPROBE) needs 250 ms without look input
        ax+=amp*sin(t*2*M_PI*0.37)+amp*0.4*sin(t*2*M_PI*1.3); ay+=amp*0.3*sin(t*2*M_PI*0.23); int ix=(int)ax, iy=(int)ay; ax-=ix; ay-=iy;
        if(ix||iy) wi_probe_mouse_xy(ix,iy); }
    return 0; }
void latch_tick(void){ static int done; if(done) return; done=1; if(getenv("PELOAD_LATCH_TEST")){ pthread_t t; pthread_create(&t,0,lc_test_thread,0); pthread_detach(t); } }
