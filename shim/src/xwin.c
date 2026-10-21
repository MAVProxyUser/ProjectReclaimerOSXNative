// xwin.c -- windows of OTHER loader processes, the way Win32 lets one program see another's windows (2026-10-20).
// #include'd by peload.c (after wininput.c).
//
// Reach Renaissance's menus (menu-overlay.exe) are a transparent window over the game (host-probe.exe), which runs as its own
// process: started with --follow "host probe", the overlay finds the game's window by its title (EnumWindows /
// GetWindowTextW), keeps itself over that window's client area (GetClientRect + ClientToScreen, SetWinEventHook location
// changes) and hands it the keyboard (SetForegroundWindow) when a match starts. Under the loader every process only knew its
// own windows. Now a process whose game window is up (the present window, g_nswin) publishes it -- title, client rect in Win32
// screen coordinates (top-left origin, points), shown, minimised -- to ~/Library/Application Support/ReclaimerMac/windows/
// <pid>.win whenever it changes (cocoa_pump); other processes see each published window as a pseudo-handle 0x7F000000 | pid
// in EnumWindows, FindWindowW, GetWindowTextW, GetWindowRect / GetClientRect / ClientToScreen, IsWindow / IsWindowVisible /
// IsIconic, GetWindowThreadProcessId and SetForegroundWindow (activates that app), and their SetWinEventHook callbacks fire
// for its location changes, show / hide, minimise and destroy (polled from the message pump, every 50 ms).
// PELOAD_NO_XWIN=1 = old (each process alone).
static int xwin_on(void){ static int on=-1; if(on<0) on=!getenv("PELOAD_NO_XWIN"); return on; }
static void xwin_dir(char*out,size_t n){ const char*h=getenv("HOME"); snprintf(out,n,"%s/Library/Application Support/ReclaimerMac/windows",h?h:"/tmp"); }
#define XWIN_TAG 0x7F000000ull
static int xwin_is(void*h){ uintptr_t v=(uintptr_t)h; return xwin_on() && (v&~0xFFFFFull)==XWIN_TAG && (v&0xFFFFF); }
static int xwin_pid(void*h){ return (int)((uintptr_t)h&0xFFFFF); }
typedef struct { int pid, vis, mini, x, y, w, h; char title[256]; } XWin;
static int xwin_read(int pid,XWin*o){ char d[1024], p[1200]; xwin_dir(d,sizeof d); snprintf(p,sizeof p,"%s/%d.win",d,pid);
    FILE*f=fopen(p,"r"); if(!f) return 0; memset(o,0,sizeof *o); o->pid=pid; int ok=fscanf(f,"v1 %d %d %d %d %d %d\n",&o->vis,&o->mini,&o->x,&o->y,&o->w,&o->h)==6;
    if(ok && fgets(o->title,sizeof o->title,f)){ size_t n=strlen(o->title); while(n && (o->title[n-1]=='\n'||o->title[n-1]=='\r')) o->title[--n]=0; }
    fclose(f); if(!ok) return 0; if(kill(pid,0)!=0 && errno==ESRCH){ unlink(p); return 0; } return 1; }
static int xwin_list(XWin*out,int max){ if(!xwin_on()) return 0; char d[1024]; xwin_dir(d,sizeof d); DIR*dd=opendir(d); if(!dd) return 0; int n=0; struct dirent*e;
    while(n<max && (e=readdir(dd))){ int pid=atoi(e->d_name); if(pid<=0 || pid==getpid() || !strstr(e->d_name,".win")) continue; if(xwin_read(pid,&out[n])) n++; }
    closedir(dd); return n; }
static int xwin_get(void*h,XWin*o){ return xwin_is(h) && xwin_read(xwin_pid(h),o); }
static void* xwin_handle(int pid){ return (void*)(uintptr_t)(XWIN_TAG|(uint64_t)pid); }

// ---- the publisher (a process with a game window): from cocoa_pump, main thread ----
static void xwin_cleanup(void){ char d[1024], p[1200]; xwin_dir(d,sizeof d); snprintf(p,sizeof p,"%s/%d.win",d,(int)getpid()); unlink(p); }
static void xwin_publish(void){
    if(!xwin_on() || !g_nswin) return; static double last; double now=mono_s(); if(now-last<0.1) return; last=now;
    typedef struct { double x,y,w,h; } R_;
    R_ fr; ((void(*)(R_*,id,SEL))objc_msgSend_stret)(&fr,(id)g_nswin,sel_registerName("frame"));
    R_ cr; ((void(*)(R_*,id,SEL,R_))objc_msgSend_stret)(&cr,(id)g_nswin,sel_registerName("contentRectForFrameRect:"),fr);
    id scr=((id(*)(id,SEL))objc_msgSend)((id)objc_getClass("NSScreen"),sel_registerName("screens")); id s0=scr?((id(*)(id,SEL,unsigned long))objc_msgSend)(scr,sel_registerName("objectAtIndex:"),0):0;
    R_ sf={0,0,0,0}; if(s0) ((void(*)(R_*,id,SEL))objc_msgSend_stret)(&sf,s0,sel_registerName("frame"));
    int vis=((signed char(*)(id,SEL))objc_msgSend)((id)g_nswin,sel_registerName("isVisible"))?1:0, mini=((signed char(*)(id,SEL))objc_msgSend)((id)g_nswin,sel_registerName("isMiniaturized"))?1:0;
    int x=(int)lround(cr.x), y=(int)lround(sf.h-(cr.y+cr.h)), w=(int)lround(cr.w), hh=(int)lround(cr.h);
    char title[256]={0}; WWin*gw=fg_hwnd(); if(gw && gw->magic==WWIN_MAGIC){ int i=0; for(; gw->title[i] && i<255; i++) title[i]=(char)(gw->title[i]<128?gw->title[i]:'?'); title[i]=0; }
    static char lastrec[400]; char rec[400]; snprintf(rec,sizeof rec,"v1 %d %d %d %d %d %d\n%s\n",vis,mini,x,y,w,hh,title); if(!strcmp(rec,lastrec)) return;
    char d[1024], p[1200], tp[1250]; xwin_dir(d,sizeof d); mkdir(d,0755); snprintf(p,sizeof p,"%s/%d.win",d,(int)getpid()); snprintf(tp,sizeof tp,"%s.tmp",p);
    FILE*f=fopen(tp,"w"); if(!f) return; fputs(rec,f); fclose(f); rename(tp,p); memcpy(lastrec,rec,sizeof rec);
    static int reg; if(!reg){ reg=1; atexit(xwin_cleanup); } }

// ---- SetWinEventHook: WINEVENTPROC(hook, event, hwnd, idObject, idChild, idEventThread, dwmsEventTime), polled ----
typedef struct { uint32_t emin, emax; void*proc; uint32_t pid; int on; } XHook;
static XHook g_xhooks[16];
typedef struct { int pid, vis, mini, x, y, w, h, seen; } XSeen;
static XSeen g_xseen[32];
static void* MSABI i_SetWinEventHook(uint32_t emin,uint32_t emax,void*hmod,void*proc,uint32_t pid,uint32_t tid,uint32_t fl){ (void)hmod;(void)tid;(void)fl;
    for(int i=0;i<16;i++) if(!g_xhooks[i].on){ g_xhooks[i]=(XHook){emin,emax,proc,pid,1};
        if(getenv("PELOAD_TRACE_XWIN")){ char m[160]; int k=snprintf(m,sizeof m,"  [xwin] SetWinEventHook 0x%x-0x%x pid %u -> #%d\n",emin,emax,pid,i+1); (void)!write(2,m,k); }
        return (void*)(uintptr_t)(0x57E00+i+1); } return 0; }
static int MSABI i_UnhookWinEvent(void*h){ uintptr_t v=(uintptr_t)h; if(v>0x57E00 && v<=0x57E00+16) g_xhooks[v-0x57E01].on=0; return 1; }
static void xwin_fire(uint32_t ev,int pid){ for(int i=0;i<16;i++){ XHook*k=&g_xhooks[i]; if(!k->on || ev<k->emin || ev>k->emax || (k->pid && k->pid!=(uint32_t)pid)) continue;
        if(getenv("PELOAD_TRACE_XWIN")){ char m[96]; int n=snprintf(m,sizeof m,"  [xwin] event 0x%x for pid %d\n",ev,pid); (void)!write(2,m,n); }
        ((void(MSABI*)(void*,uint32_t,void*,int32_t,int32_t,uint32_t,uint32_t))k->proc)((void*)(uintptr_t)(0x57E00+i+1),ev,xwin_handle(pid),0/*OBJID_WINDOW*/,0/*CHILDID_SELF*/,(uint32_t)pid,i_GetTickCount()); } }
static void xwin_poll(void){
    if(!xwin_on()) return; int any=0; for(int i=0;i<16;i++) if(g_xhooks[i].on) any=1; if(!any) return;
    static double last; double now=mono_s(); if(now-last<0.05) return; last=now;
    XWin w[32]; int n=xwin_list(w,32); for(int i=0;i<32;i++) g_xseen[i].seen=0;
    for(int j=0;j<n;j++){ int k=-1; for(int i=0;i<32;i++) if(g_xseen[i].pid==w[j].pid){ k=i; break; }
        if(k<0){ for(int i=0;i<32;i++) if(!g_xseen[i].pid){ k=i; break; } if(k<0) continue; g_xseen[k]=(XSeen){w[j].pid,-1,-1,0,0,0,0,0}; xwin_fire(0x8000/*EVENT_OBJECT_CREATE*/,w[j].pid); }
        XSeen*s=&g_xseen[k]; s->seen=1;
        if(s->vis!=w[j].vis){ if(s->vis>=0 || w[j].vis) xwin_fire(w[j].vis?0x8002/*SHOW*/:0x8003/*HIDE*/,w[j].pid); s->vis=w[j].vis; }
        if(s->mini!=w[j].mini){ if(s->mini>=0) xwin_fire(w[j].mini?0x0016/*MINIMIZESTART*/:0x0017/*MINIMIZEEND*/,w[j].pid); s->mini=w[j].mini; }
        if(s->x!=w[j].x || s->y!=w[j].y || s->w!=w[j].w || s->h!=w[j].h){ s->x=w[j].x; s->y=w[j].y; s->w=w[j].w; s->h=w[j].h; xwin_fire(0x800B/*LOCATIONCHANGE*/,w[j].pid); } }
    for(int i=0;i<32;i++) if(g_xseen[i].pid && !g_xseen[i].seen){ xwin_fire(0x8001/*EVENT_OBJECT_DESTROY*/,g_xseen[i].pid); g_xseen[i].pid=0; } }

// ---- window text / lookup ----
static int MSABI i_GetWindowTextW(void*h,uint16_t*buf,int n){ if(!buf||n<=0) return 0; buf[0]=0; int k=0;
    XWin x; if(xwin_get(h,&x)){ for(; x.title[k] && k<n-1; k++) buf[k]=(uint8_t)x.title[k]; buf[k]=0; return k; }
    WWin*w=h; if(w && w->magic==WWIN_MAGIC){ for(; w->title[k] && k<n-1; k++) buf[k]=w->title[k]; buf[k]=0; } return k; }
static int MSABI i_GetWindowTextLengthW(void*h){ uint16_t b[260]; return i_GetWindowTextW(h,b,260); }
static int xwin_title_eq(const uint16_t*want,const char*have){ if(!want) return 1; int i=0; for(; want[i] && have[i]; i++) if(want[i]!=(uint8_t)have[i]) return 0; return !want[i] && !have[i]; }
static void* MSABI i_FindWindowW(const uint16_t*cls,const uint16_t*title){ (void)cls; if(!title) return 0;
    XWin w[32]; int n=xwin_list(w,32); for(int j=0;j<n;j++) if(xwin_title_eq(title,w[j].title)) return xwin_handle(w[j].pid);
    return 0; }
static void xwin_activate(int pid){ id app=((id(*)(id,SEL,int))objc_msgSend)((id)objc_getClass("NSRunningApplication"),sel_registerName("runningApplicationWithProcessIdentifier:"),pid);
    if(app) ((signed char(*)(id,SEL,unsigned long))objc_msgSend)(app,sel_registerName("activateWithOptions:"),2/*NSApplicationActivateIgnoringOtherApps*/); }
// accessors for the Win32 functions defined before this file (peload.c)
static int xwin_rect(void*h,int*x,int*y,int*w,int*hh,int*vis,int*mini,int*pid){ XWin o; if(!xwin_get(h,&o)) return 0;
    if(x)*x=o.x; if(y)*y=o.y; if(w)*w=o.w; if(hh)*hh=o.h; if(vis)*vis=o.vis; if(mini)*mini=o.mini; if(pid)*pid=o.pid; return 1; }
static int xwin_enum(int(MSABI*cb)(void*,int64_t),int64_t lp){ XWin w[32]; int n=xwin_list(w,32); for(int j=0;j<n;j++) if(!cb(xwin_handle(w[j].pid),lp)) return 0; return 1; }
