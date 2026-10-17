// wininput.c — keyboard + mouse for the guest: Cocoa NSEvents -> Win32 window messages + Raw Input.
// Included by peload.c in place of the old stubs (RegisterRawInputDevices / GetRawInputData /
// Get(Async)KeyState / GetKeyboardState / GetCursorPos / ClipCursor / ShowCursor / SetCursorPos) and the
// 32-entry one-shot injected message list. Events are translated on the main thread inside cocoa_pump()
// and queued; PeekMessageW/GetMessageW hand them to the client's message loop.
//  - WM_INPUT (Raw Input): relative mouse deltas, buttons, wheel; keyboard make/break with set-1 scan codes
//  - WM_KEYDOWN/UP, WM_CHAR, WM_MOUSEMOVE, WM_[LRM]BUTTON*, WM_MOUSEWHEEL for the client UI
//  - ClipCursor(rect) captures the pointer (CGAssociateMouseAndMouseCursorPosition(false)); ShowCursor hides it
// PELOAD_TRACE_INPUT logs translated events. PELOAD_NO_KBM disables the translation.
#include <objc/runtime.h>
#include <objc/message.h>

// ---- message ring ----
typedef struct { void* hwnd; uint32_t msg; uint64_t wp; int64_t lp; } WiMsg;
static WiMsg g_wiq[4096]; static uint32_t g_wiq_h, g_wiq_t; static pthread_mutex_t g_wiq_mx=PTHREAD_MUTEX_INITIALIZER;
static void wi_post(void*h,uint32_t m,uint64_t wp,int64_t lp){
    pthread_mutex_lock(&g_wiq_mx);
    if(g_wiq_t-g_wiq_h<4096){ WiMsg*e=&g_wiq[g_wiq_t%4096]; e->hwnd=h; e->msg=m; e->wp=wp; e->lp=lp; g_wiq_t++; }
    pthread_mutex_unlock(&g_wiq_mx); }
void* fg_hwnd_ext(void);
static int wi_peek(WiMsg*out,int remove){
    pthread_mutex_lock(&g_wiq_mx); int r=0;
    if(g_wiq_h!=g_wiq_t){ *out=g_wiq[g_wiq_h%4096]; if(remove) g_wiq_h++; r=1; }
    pthread_mutex_unlock(&g_wiq_mx); return r; }
typedef struct { void* hwnd; uint32_t msg; uint64_t wp; int64_t lp; } WiMsgFwd2;
static int wi_peek_fwd(WiMsgFwd*out,int remove){ WiMsg m; if(!wi_peek(&m,remove)) return 0; out->hwnd=m.hwnd; out->msg=m.msg; out->wp=m.wp; out->lp=m.lp; return 1; }

// ---- key state ----
static volatile uint8_t g_keydown[256];        // 1 = down (VK index)
static volatile uint8_t g_keytoggle[256];

// ---- raw input ----
static void* g_ri_mouse_hwnd; static void* g_ri_kbd_hwnd; static int g_ri_mouse, g_ri_kbd;
typedef struct { uint8_t data[48]; uint32_t size; } WiRaw;
static WiRaw g_riq[1024]; static uint32_t g_riq_n;    // records referenced by HRAWINPUT = 0x52490000 + slot
static int64_t wi_raw_alloc(const uint8_t*d,uint32_t sz){ uint32_t slot=g_riq_n++ % 1024; memcpy(g_riq[slot].data,d,sz); g_riq[slot].size=sz; return (int64_t)(0x52490000u+slot); }
static void* wi_target(void*reg){ return reg?reg:fg_hwnd_ext(); }
static void wi_raw_mouse(int dx,int dy,uint16_t bflags,int16_t bdata){
    if(!g_ri_mouse) return;
    uint8_t r[48]; memset(r,0,sizeof r); uint32_t type=0/*RIM_TYPEMOUSE*/, size=48;
    memcpy(r,&type,4); memcpy(r+4,&size,4); void*dev=(void*)0x4D53; memcpy(r+8,&dev,8);    // RAWINPUTHEADER (24 B)
    uint16_t usflags=0/*MOUSE_MOVE_RELATIVE*/; memcpy(r+24,&usflags,2); memcpy(r+28,&bflags,2); memcpy(r+30,&bdata,2);
    int32_t lx=dx, ly=dy; memcpy(r+36,&lx,4); memcpy(r+40,&ly,4);
    wi_post(wi_target(g_ri_mouse_hwnd),0x00FF/*WM_INPUT*/,0,wi_raw_alloc(r,48)); }
static void wi_raw_key(uint16_t make,int e0,int brk,uint16_t vk,uint32_t msg){
    if(!g_ri_kbd) return;
    uint8_t r[48]; memset(r,0,sizeof r); uint32_t type=1/*RIM_TYPEKEYBOARD*/, size=40;
    memcpy(r,&type,4); memcpy(r+4,&size,4); void*dev=(void*)0x4B42; memcpy(r+8,&dev,8);
    uint16_t fl=(uint16_t)((brk?1:0)|(e0?2:0)); memcpy(r+24,&make,2); memcpy(r+26,&fl,2); memcpy(r+30,&vk,2); memcpy(r+32,&msg,4);
    wi_post(wi_target(g_ri_kbd_hwnd),0x00FF,0,wi_raw_alloc(r,40)); }

static int MSABI wi_RegisterRawInputDevices(const uint8_t*d,uint32_t n,uint32_t cb){ if(!d||cb<16) return 0;
    for(uint32_t i=0;i<n;i++){ const uint8_t*e=d+(size_t)i*cb; uint16_t page,usage; uint32_t fl; void*h; memcpy(&page,e,2); memcpy(&usage,e+2,2); memcpy(&fl,e+4,4); memcpy(&h,e+8,8);
        if(page!=1) continue; int rm=(fl&1)!=0;   // RIDEV_REMOVE
        if(usage==2){ g_ri_mouse=!rm; g_ri_mouse_hwnd=rm?0:h; } else if(usage==6){ g_ri_kbd=!rm; g_ri_kbd_hwnd=rm?0:h; }
        if(getenv("PELOAD_TRACE_INPUT")){ char m[120]; int k=snprintf(m,sizeof m,"  [kbm] RegisterRawInputDevices usage=%u flags=0x%x hwnd=%p%s\n",usage,fl,h,rm?" (remove)":""); (void)!write(2,m,k); } }
    return 1; }
static uint32_t MSABI wi_GetRawInputData(void*h,uint32_t cmd,void*data,uint32_t*sz,uint32_t hdr){ (void)hdr;
    uint32_t idx=(uint32_t)((uintptr_t)h-0x52490000u); if(idx>=1024||!sz) return (uint32_t)-1;
    uint32_t need = cmd==0x10000005/*RID_HEADER*/ ? 24 : g_riq[idx].size;
    if(!data){ *sz=need; return 0; }
    if(*sz<need) return (uint32_t)-1;
    memcpy(data,g_riq[idx].data,need); return need; }
static uint32_t MSABI wi_GetRawInputDeviceInfoW(void*dev,uint32_t cmd,void*data,uint32_t*sz){ (void)dev;
    if(!sz) return (uint32_t)-1;
    if(cmd==0x2000000b/*RIDI_DEVICEINFO*/){ uint32_t need=32; if(!data){ *sz=need; return 0; } if(*sz<need) return (uint32_t)-1; memset(data,0,need); uint32_t*u=data; u[0]=need; u[1]=(dev==(void*)0x4B42)?1:0; return need; }
    if(!data){ *sz=1; return 0; } if(*sz>=1){ ((uint16_t*)data)[0]=0; return 1; } return (uint32_t)-1; }
static int16_t MSABI wi_GetAsyncKeyState(int k){ return (k>=0&&k<256&&g_keydown[k])?(int16_t)0x8000:0; }
static int16_t MSABI wi_GetKeyState(int k){ if(k<0||k>=256) return 0; return (int16_t)((g_keydown[k]?0x8000:0)|(g_keytoggle[k]?1:0)); }
static int MSABI wi_GetKeyboardState(uint8_t*s){ if(!s) return 0; for(int i=0;i<256;i++) s[i]=(uint8_t)((g_keydown[i]?0x80:0)|(g_keytoggle[i]?1:0)); return 1; }

// ---- cursor ----
static int g_wi_captured, g_wi_cursor_count;   // ShowCursor counter (>=0 visible)
static int32_t g_wi_cx=WIN_W/2, g_wi_cy=WIN_H/2;   // last client-space pointer position
// Unattended input self-tests (PELOAD_KBM_TEST) and PELOAD_NO_SYS_CURSOR never capture or hide the real
// macOS cursor: the guest still sees its pointer captured, but whoever is using the Mac keeps their mouse.
static int wi_syscursor(void){ static int ok=-1; if(ok<0) ok=!(getenv("PELOAD_KBM_TEST")||getenv("PELOAD_NO_SYS_CURSOR")); return ok; }
static int MSABI wi_ClipCursor(const int32_t*r){
    int want = r!=0;
    if(want!=g_wi_captured){ g_wi_captured=want; if(wi_syscursor()) CGAssociateMouseAndMouseCursorPosition(want?0:1);
        if(getenv("PELOAD_TRACE_INPUT")){ char m[64]; int k=snprintf(m,sizeof m,"  [kbm] pointer %s\n",want?"captured":"released"); (void)!write(2,m,k); } }
    return 1; }
static int MSABI wi_ShowCursor(int show){
    int before=g_wi_cursor_count; g_wi_cursor_count += show?1:-1;
    if(!wi_syscursor()) return g_wi_cursor_count;
    if(before>=0 && g_wi_cursor_count<0) CGDisplayHideCursor(kCGDirectMainDisplay);
    else if(before<0 && g_wi_cursor_count>=0) CGDisplayShowCursor(kCGDirectMainDisplay);
    return g_wi_cursor_count; }
static int MSABI wi_GetCursorPos(int32_t*p){ if(p){ p[0]=g_wi_cx; p[1]=g_wi_cy; } return 1; }
static int MSABI wi_SetCursorPos(int x,int y){ g_wi_cx=x; g_wi_cy=y; return 1; }   // (pointer stays put while captured)

// ---- macOS keyCode -> Windows VK + set-1 scan code (+E0) ----
typedef struct { uint8_t vk, sc, e0; } WiKey;
static WiKey wi_keymap(unsigned short kc){
    static const struct { unsigned short kc; uint8_t vk, sc, e0; } T[]={
        {0x00,'A',0x1E,0},{0x01,'S',0x1F,0},{0x02,'D',0x20,0},{0x03,'F',0x21,0},{0x04,'H',0x23,0},{0x05,'G',0x22,0},{0x06,'Z',0x2C,0},{0x07,'X',0x2D,0},
        {0x08,'C',0x2E,0},{0x09,'V',0x2F,0},{0x0B,'B',0x30,0},{0x0C,'Q',0x10,0},{0x0D,'W',0x11,0},{0x0E,'E',0x12,0},{0x0F,'R',0x13,0},{0x10,'Y',0x15,0},
        {0x11,'T',0x14,0},{0x12,'1',0x02,0},{0x13,'2',0x03,0},{0x14,'3',0x04,0},{0x15,'4',0x05,0},{0x16,'6',0x07,0},{0x17,'5',0x06,0},{0x18,0xBB,0x0D,0},
        {0x19,'9',0x0A,0},{0x1A,'7',0x08,0},{0x1B,0xBD,0x0C,0},{0x1C,'8',0x09,0},{0x1D,'0',0x0B,0},{0x1E,0xDD,0x1B,0},{0x1F,'O',0x18,0},{0x20,'U',0x16,0},
        {0x21,0xDB,0x1A,0},{0x22,'I',0x17,0},{0x23,'P',0x19,0},{0x24,0x0D,0x1C,0},{0x25,'L',0x26,0},{0x26,'J',0x24,0},{0x27,0xDE,0x28,0},{0x28,'K',0x25,0},
        {0x29,0xBA,0x27,0},{0x2A,0xDC,0x2B,0},{0x2B,0xBC,0x33,0},{0x2C,0xBF,0x35,0},{0x2D,'N',0x31,0},{0x2E,'M',0x32,0},{0x2F,0xBE,0x34,0},{0x30,0x09,0x0F,0},
        {0x31,0x20,0x39,0},{0x32,0xC0,0x29,0},{0x33,0x08,0x0E,0},{0x35,0x1B,0x01,0},{0x37,0x5B,0x5B,1},{0x38,0xA0,0x2A,0},{0x39,0x14,0x3A,0},{0x3A,0xA4,0x38,0},
        {0x3B,0xA2,0x1D,0},{0x3C,0xA1,0x36,0},{0x3D,0xA5,0x38,1},{0x3E,0xA3,0x1D,1},
        {0x7A,0x70,0x3B,0},{0x78,0x71,0x3C,0},{0x63,0x72,0x3D,0},{0x76,0x73,0x3E,0},{0x60,0x74,0x3F,0},{0x61,0x75,0x40,0},{0x62,0x76,0x41,0},{0x64,0x77,0x42,0},
        {0x65,0x78,0x43,0},{0x6D,0x79,0x44,0},{0x67,0x7A,0x57,0},{0x6F,0x7B,0x58,0},
        {0x73,0x24,0x47,1},{0x74,0x21,0x49,1},{0x75,0x2E,0x53,1},{0x77,0x23,0x4F,1},{0x79,0x22,0x51,1},{0x7B,0x25,0x4B,1},{0x7C,0x27,0x4D,1},{0x7D,0x28,0x50,1},{0x7E,0x26,0x48,1},
        {0x52,0x60,0x52,0},{0x53,0x61,0x4F,0},{0x54,0x62,0x50,0},{0x55,0x63,0x51,0},{0x56,0x64,0x4B,0},{0x57,0x65,0x4C,0},{0x58,0x66,0x4D,0},{0x59,0x67,0x47,0},
        {0x5B,0x68,0x48,0},{0x5C,0x69,0x49,0},{0x41,0x6E,0x53,0},{0x43,0x6A,0x37,0},{0x45,0x6B,0x4E,0},{0x4E,0x6D,0x4A,0},{0x4B,0x6F,0x35,1},{0x4C,0x0D,0x1C,1} };
    for(unsigned i=0;i<sizeof T/sizeof T[0];i++) if(T[i].kc==kc){ WiKey k={T[i].vk,T[i].sc,T[i].e0}; return k; }
    WiKey z={0,0,0}; return z; }
static uint8_t wi_generic_vk(uint8_t vk){ switch(vk){ case 0xA0: case 0xA1: return 0x10; case 0xA2: case 0xA3: return 0x11; case 0xA4: case 0xA5: return 0x12; } return vk; }
static void wi_key(unsigned short kc,int down,int repeat){
    WiKey k=wi_keymap(kc); if(!k.vk) return;
    uint8_t g=wi_generic_vk(k.vk);
    int was=g_keydown[k.vk]; g_keydown[k.vk]=(uint8_t)down; g_keydown[g]=(uint8_t)(down|| (g!=k.vk && (g_keydown[g==0x10?0xA0:g==0x11?0xA2:0xA4]|g_keydown[g==0x10?0xA1:g==0x11?0xA3:0xA5])));
    if(down && !was && k.vk==0x14) g_keytoggle[0x14]^=1;
    int64_t lp=1 | ((int64_t)k.sc<<16) | ((int64_t)(k.e0?1:0)<<24) | (down? (was?((int64_t)1<<30):0) : ((int64_t)3<<30));
    void*h=fg_hwnd_ext();
    uint32_t msg = down?0x0100:0x0101; if(g==0x12) msg = down?0x0104:0x0105;   // WM_SYS* for Alt
    if(!repeat) wi_raw_key(k.sc,k.e0,!down,g,msg);
    wi_post(h,msg,g,lp);
    if(getenv("PELOAD_TRACE_INPUT")){ char m[96]; int q=snprintf(m,sizeof m,"  [kbm] key kc=0x%x vk=0x%x sc=0x%x %s\n",kc,g,k.sc,down?"down":"up"); (void)!write(2,m,q); } }

// F2-F5 toggle the overlay features (hitbox.c: hitboxes, projectile tracers, player info, items); not forwarded to the game.
static volatile int g_hb_toggle, g_hb_teamcycle;   // F6 cycles the team filter (all -> enemies -> friends)
// ---- NSEvent translation (main thread, from cocoa_pump). Returns 1 = consumed (don't forward to AppKit). ----
static int wi_translate(id ev){
    static int off=-1; if(off<0) off=getenv("PELOAD_NO_KBM")?1:0;
    if(off){ unsigned long t=((unsigned long(*)(id,SEL))objc_msgSend)(ev,sel_registerName("type")), md=((unsigned long(*)(id,SEL))objc_msgSend)(ev,sel_registerName("modifierFlags"));
        return (t==10||t==11) && !(md&(1ul<<20)); }   // input-less test client: swallow keys (no system beep), keep Cmd shortcuts
    unsigned long type=((unsigned long(*)(id,SEL))objc_msgSend)(ev,sel_registerName("type"));
    unsigned long mods=((unsigned long(*)(id,SEL))objc_msgSend)(ev,sel_registerName("modifierFlags"));
    void*h=fg_hwnd_ext();
    switch(type){
    case 10: case 11: {   // KeyDown / KeyUp
        if(mods&(1ul<<20)) return 0;                       // Command shortcuts go to AppKit (Cmd+Q, Cmd+Tab ...)
        unsigned short kc=((unsigned short(*)(id,SEL))objc_msgSend)(ev,sel_registerName("keyCode"));
        signed char rep=type==10?((signed char(*)(id,SEL))objc_msgSend)(ev,sel_registerName("isARepeat")):0;
        if(kc==0x62){ if(type==10 && !rep){ static int ok=-1; if(ok<0) ok=getenv("PELOAD_ALLOW_TELEPORT")?1:0;   // F7: teleport (hitbox.c)
                extern volatile int g_hb_job_pending; if(ok){ __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); (void)!write(2,"  [teleport] F7: requested\n",27); } } return 1; }
        if(kc==0x64){ if(type==10 && !rep){ extern volatile int g_hb_cap_req; __atomic_store_n(&g_hb_cap_req,1,__ATOMIC_RELEASE); (void)!write(2,"  [capture] F8: capturing the next frame\n",41); } return 1; }   // F8: frame capture (hitbox.c)
        if(kc==0x61){ if(type==10 && !rep){ g_hb_teamcycle++; static const char*const nm[3]={"all","enemies only","friends only"}; char m[64]; int q=snprintf(m,sizeof m,"  [hitbox] F6: showing %s\n",nm[g_hb_teamcycle%3]); (void)!write(2,m,q); } return 1; }   // F6
        { int bit = kc==0x78?0 : kc==0x63?1 : kc==0x76?2 : kc==0x60?3 : kc==0x65?4 : -1;   // F2 hitboxes, F3 tracers, F4 player info, F5 items, F9 visible only
          if(bit>=0){ if(type==10 && !rep){ g_hb_toggle^=1<<bit; char m[64]; int q=snprintf(m,sizeof m,"  [hitbox] F%d: overlay feature toggled\n",bit==4?9:bit+2); (void)!write(2,m,q); } return 1; } }
        wi_key(kc,type==10,rep);
        if(type==10){ id s=((id(*)(id,SEL))objc_msgSend)(ev,sel_registerName("characters"));   // WM_CHAR (text entry)
            const char*u=s?((const char*(*)(id,SEL))objc_msgSend)(s,sel_registerName("UTF8String")):0;
            if(u && (unsigned char)u[0]>=0x20 && (unsigned char)u[0]<0x7f) wi_post(h,0x0102,(uint64_t)(unsigned char)u[0],1);
            else if(u && (u[0]==0x0d||u[0]==0x08||u[0]==0x1b||u[0]==0x09)) wi_post(h,0x0102,(uint64_t)(unsigned char)u[0],1); }
        return 1; }
    case 12: {            // FlagsChanged: modifier key down/up
        unsigned short kc=((unsigned short(*)(id,SEL))objc_msgSend)(ev,sel_registerName("keyCode"));
        unsigned long bit = (kc==0x38||kc==0x3C)?(1ul<<17) : (kc==0x3B||kc==0x3E)?(1ul<<18) : (kc==0x3A||kc==0x3D)?(1ul<<19) : (kc==0x37||kc==0x36)?(1ul<<20) : (kc==0x39)?(1ul<<16) : 0;
        if(!bit) return 0;
        WiKey k=wi_keymap(kc); int down = (kc==0x39)? !g_keydown[0x14] : ((mods&bit)!=0 && !g_keydown[k.vk]);
        if(kc!=0x39 && !(mods&bit)) down=0;
        wi_key(kc,down,0); return 0; }
    case 5: case 6: case 7: case 27: {   // MouseMoved / Left/Right/Other dragged
        double dx=((double(*)(id,SEL))objc_msgSend)(ev,sel_registerName("deltaX")), dy=((double(*)(id,SEL))objc_msgSend)(ev,sel_registerName("deltaY"));
        static double ax, ay; ax+=dx; ay+=dy; int ix=(int)ax, iy=(int)ay; ax-=ix; ay-=iy;
        if(ix||iy) wi_raw_mouse(ix,iy,0,0);
        // client-space position for WM_MOUSEMOVE (window points -> client pixels, top-left origin)
        id win=((id(*)(id,SEL))objc_msgSend)(ev,sel_registerName("window"));
        if(win && !g_wi_captured){ typedef struct { double x,y; } P; P p=((P(*)(id,SEL))objc_msgSend)(ev,sel_registerName("locationInWindow"));
            id view=((id(*)(id,SEL))objc_msgSend)(win,sel_registerName("contentView")); typedef struct { double x,y,w,h; } R;
            R b={0,0,WIN_W,WIN_H}; if(view) ((void(*)(R*,id,SEL))objc_msgSend_stret)(&b,view,sel_registerName("bounds"));   // x86_64: NSRect via _stret
            if(b.w>0&&b.h>0){ g_wi_cx=(int32_t)(p.x/b.w*WIN_W); g_wi_cy=(int32_t)((b.h-p.y)/b.h*WIN_H); } }
        uint64_t mk=(g_keydown[1]?1:0)|(g_keydown[2]?2:0)|(g_keydown[4]?0x10:0);
        wi_post(h,0x0200,mk,(int64_t)((uint32_t)(g_wi_cx&0xffff)|((uint32_t)(g_wi_cy&0xffff)<<16)));
        return 0; }
    case 1: case 2: case 3: case 4: case 25: case 26: {   // mouse buttons
        long btn = (type==1||type==2)?0 : (type==3||type==4)?1 : ((long(*)(id,SEL))objc_msgSend)(ev,sel_registerName("buttonNumber"));
        int down = (type==1||type==3||type==25);
        uint8_t vk = btn==0?1 : btn==1?2 : btn==2?4 : btn==3?5 : 6;
        g_keydown[vk]=(uint8_t)down;
        uint16_t bf = btn==0?(down?0x1:0x2) : btn==1?(down?0x4:0x8) : btn==2?(down?0x10:0x20) : btn==3?(down?0x40:0x80) : (down?0x100:0x200);
        wi_raw_mouse(0,0,bf,0);
        uint32_t m = btn==0?(down?0x201:0x202) : btn==1?(down?0x204:0x205) : btn==2?(down?0x207:0x208) : (down?0x20B:0x20C);
        uint64_t wp = btn>=3 ? ((uint64_t)(btn==3?1:2)<<16) : 0;
        wi_post(h,m,wp,(int64_t)((uint32_t)(g_wi_cx&0xffff)|((uint32_t)(g_wi_cy&0xffff)<<16)));
        if(getenv("PELOAD_TRACE_INPUT")){ char t[64]; int q=snprintf(t,sizeof t,"  [kbm] mouse button %ld %s\n",btn,down?"down":"up"); (void)!write(2,t,q); }
        return 0; }
    case 22: {            // ScrollWheel
        double dy=((double(*)(id,SEL))objc_msgSend)(ev,sel_registerName("scrollingDeltaY"));
        signed char precise=((signed char(*)(id,SEL))objc_msgSend)(ev,sel_registerName("hasPreciseScrollingDeltas"));
        static double acc; acc += precise? dy/10.0 : dy; int notches=(int)acc; acc-=notches;
        if(notches){ int16_t d=(int16_t)(notches*120); wi_raw_mouse(0,0,0x400/*RI_MOUSE_WHEEL*/,d);
            wi_post(h,0x020A,((uint64_t)(uint16_t)d)<<16,(int64_t)((uint32_t)(g_wi_cx&0xffff)|((uint32_t)(g_wi_cy&0xffff)<<16))); }
        return 0; }
    }
    return 0; }

// ---- PELOAD_KBM_TEST: self-test of the guest keyboard/mouse path (no real devices needed) ----
// 3 s after gameplay starts: left click (the client captures input on click), hold W for 2 s while the
// mouse sweeps right, then release. Uses the same queue/raw-input path real NSEvents feed.
extern double g_gameplay_t0, g_menu_ready_t0;
static void* wi_test_thread(void*a){ (void)a; pthread_setname_np("shim-kbmtest");
    // a match, or Reclaimer's frontend menu (PELOAD_KBM_TEST_GAMEPLAY=1: only a match, e.g. one joined from the menu)
    int wait_match=getenv("PELOAD_KBM_TEST_GAMEPLAY")!=0;
    while(!g_gameplay_t0 && (wait_match || !g_menu_ready_t0)) usleep(100000);
    usleep(3000000);
    void*h=fg_hwnd_ext(); char m[64]; int k;
    g_keydown[1]=1; wi_raw_mouse(0,0,0x1,0); wi_post(h,0x0201,1,(int64_t)((WIN_H/2)<<16|(WIN_W/2)));
    usleep(80000);
    g_keydown[1]=0; wi_raw_mouse(0,0,0x2,0); wi_post(h,0x0202,0,(int64_t)((WIN_H/2)<<16|(WIN_W/2)));
    k=snprintf(m,sizeof m,"  [kbm-test] click sent\n"); (void)!write(2,m,k);
    usleep(500000);
    wi_key(0x0D,1,0);   // W down
    for(int i=0;i<100;i++){ wi_raw_mouse(8,0,0,0); usleep(20000); }
    wi_key(0x0D,0,0);   // W up
    k=snprintf(m,sizeof m,"  [kbm-test] W held 2 s + mouse sweep done\n"); (void)!write(2,m,k);
    // PELOAD_KBM_TEST_CLICK=<x>,<y>[;<x>,<y>...] -- then move to and left-click each window point, 2 s apart
    { const char*e=getenv("PELOAD_KBM_TEST_CLICK"); while(e && *e){ int cx=0,cy=0; if(sscanf(e,"%d,%d",&cx,&cy)!=2) break;
        usleep(2000000); int64_t lp=(int64_t)((cy<<16)|(cx&0xffff));
        wi_post(h,0x0200,0,lp); usleep(100000);
        g_keydown[1]=1; wi_raw_mouse(0,0,0x1,0); wi_post(h,0x0201,1,lp); usleep(80000);
        g_keydown[1]=0; wi_raw_mouse(0,0,0x2,0); wi_post(h,0x0202,0,lp);
        k=snprintf(m,sizeof m,"  [kbm-test] clicked %d,%d\n",cx,cy); (void)!write(2,m,k);
        e=strchr(e,';'); if(e) e++; } }
    // PELOAD_KBM_TEST_HOLD=<mac keycode hex>:<seconds> -- then hold that key (e.g. 30:3 = Tab for 3 s)
    { const char*e=getenv("PELOAD_KBM_TEST_HOLD"); if(e){ unsigned kc=(unsigned)strtoul(e,0,16); double secs=strchr(e,':')?atof(strchr(e,':')+1):2.0;
        if(getenv("PELOAD_KBM_TEST_HOLD_GAMEPLAY")) while(!g_gameplay_t0) usleep(100000);   // e.g. after clicks that start a match
        const char*dl=getenv("PELOAD_KBM_TEST_HOLD_DELAY"); usleep(dl?(useconds_t)(atof(dl)*1e6):500000); wi_key((uint16_t)kc,1,0); k=snprintf(m,sizeof m,"  [kbm-test] key 0x%x down\n",kc); (void)!write(2,m,k);
        usleep((useconds_t)(secs*1e6)); wi_key((uint16_t)kc,0,0); k=snprintf(m,sizeof m,"  [kbm-test] key 0x%x up\n",kc); (void)!write(2,m,k); } }
    return 0; }
// PELOAD_AUTOCLICK="<step>;<step>;...": drive the client's own UI once its menu is up, e.g. so an instance with
// no real input (a test target) can host a game by itself. Steps, in 1280x720 client coordinates:
//   x,y            left-click there            w:x,y,n   mouse wheel n notches there (+ up, - down)
//   s:secs         wait                        k:hex     press + release a key (macOS keycode)
//   t:text         type text (WM_CHAR)
// Each click/wheel/key is followed by PELOAD_AUTOCLICK_GAP s (default 1.5). Starts PELOAD_AUTOCLICK_DELAY s
// (default 3) after "Menu loading screen finished". Never touches the real cursor.
static void wi_click_at(void*h,int x,int y){ int64_t lp=(int64_t)((y<<16)|(x&0xffff)); g_wi_cx=x; g_wi_cy=y;
    wi_post(h,0x0200,0,lp); usleep(100000);
    g_keydown[1]=1; wi_raw_mouse(0,0,0x1,0); wi_post(h,0x0201,1,lp); usleep(80000);
    g_keydown[1]=0; wi_raw_mouse(0,0,0x2,0); wi_post(h,0x0202,0,lp); }
extern volatile int g_games_ended;
static void wi_run_steps(void*h,const char*e,useconds_t gap){ char m[96]; int k;
    for(; e && *e; e=strchr(e,';')?strchr(e,';')+1:0){
        int x=0,y=0,n=0; double secs=0; unsigned kc=0;
        if(sscanf(e,"s:%lf",&secs)==1){ usleep((useconds_t)(secs*1e6)); continue; }
        if(sscanf(e,"w:%d,%d,%d",&x,&y,&n)==3){ int64_t lp=(int64_t)((y<<16)|(x&0xffff)); g_wi_cx=x; g_wi_cy=y; wi_post(h,0x0200,0,lp);
            for(int i=0;i<(n<0?-n:n);i++){ int16_t d=(int16_t)(n<0?-120:120); wi_raw_mouse(0,0,0x400,d); wi_post(h,0x020A,(uint64_t)((uint32_t)(uint16_t)d<<16),lp); usleep(60000); }
            k=snprintf(m,sizeof m,"  [autoclick] wheel %d at %d,%d\n",n,x,y); }
        else if(!strncmp(e,"t:",2)){ const char*t=e+2; int c=0; for(; t[c] && t[c]!=';'; c++){ wi_post(h,0x0102/*WM_CHAR*/,(uint64_t)(uint8_t)t[c],1); usleep(40000); }
            k=snprintf(m,sizeof m,"  [autoclick] typed %d chars\n",c); }
        else if(sscanf(e,"k:%x",&kc)==1){ wi_key((unsigned short)kc,1,0); usleep(80000); wi_key((unsigned short)kc,0,0); k=snprintf(m,sizeof m,"  [autoclick] key 0x%x\n",kc); }
        else if(sscanf(e,"%d,%d",&x,&y)==2){ wi_click_at(h,x,y); k=snprintf(m,sizeof m,"  [autoclick] click %d,%d\n",x,y); }
        else break;
        (void)!write(2,m,k); usleep(gap); } }
// PELOAD_AUTOCLICK_AFTER_GAME="<steps>": run these steps again PELOAD_AUTOCLICK_AFTER_DELAY s (default 8)
// after every finished match ("Carnage report opened"), e.g. Continue + Start Game so a hosting test
// target always has a match running.
static void* wi_auto_thread(void*a){ (void)a; pthread_setname_np("shim-autoclick");
    while(!g_menu_ready_t0) usleep(100000);
    const char*dl=getenv("PELOAD_AUTOCLICK_DELAY"); usleep((useconds_t)((dl?atof(dl):3.0)*1e6));
    const char*gp=getenv("PELOAD_AUTOCLICK_GAP"); useconds_t gap=(useconds_t)((gp?atof(gp):1.5)*1e6);
    void*h=fg_hwnd_ext(); int seen=g_games_ended;
    wi_run_steps(h,getenv("PELOAD_AUTOCLICK"),gap);
    { const char*t="  [autoclick] done\n"; (void)!write(2,t,strlen(t)); }
    const char*again=getenv("PELOAD_AUTOCLICK_AFTER_GAME"); if(!again) return 0;
    const char*ad=getenv("PELOAD_AUTOCLICK_AFTER_DELAY"); useconds_t after=(useconds_t)((ad?atof(ad):8.0)*1e6);
    for(;;){ while(g_games_ended==seen) usleep(200000); seen=g_games_ended;
        { const char*t="  [autoclick] match ended; running the after-game steps\n"; (void)!write(2,t,strlen(t)); }
        usleep(after); wi_run_steps(h,again,gap); }
    return 0; }
static void wi_test_start(void){ if(getenv("PELOAD_KBM_TEST")){ pthread_t t; pthread_create(&t,0,wi_test_thread,0); }
    if(getenv("PELOAD_AUTOCLICK")){ pthread_t t; pthread_create(&t,0,wi_auto_thread,0); } }
