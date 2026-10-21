// webview2.m -- Microsoft Edge WebView2 (the COM API Reach Renaissance's menus are hosted in) on WKWebView (2026-10-20).
// Built beside peload.c (Objective-C, manual retain/release, like rec.m); C interface in wv2.h.
//
// Reach Renaissance's bin/menu-overlay.exe draws its Xbox 360 menus (themes/classic/*.html, *.js) in WebView2: it loads
// WebView2Loader.dll, CreateCoreWebView2EnvironmentWithOptions -> ICoreWebView2Environment::CreateCoreWebView2Controller(its
// window) -> ICoreWebView2 (Navigate, ExecuteScript, AddScriptToExecuteOnDocumentCreated, PostWebMessageAsString / Json,
// add_WebMessageReceived, NavigationCompleted ...). No Edge runtime exists here (the genuine loader returns 0x80070002), so
// LoadLibrary("WebView2Loader.dll") gets a pseudo-module and these functions instead (peload.c WV2_HANDLE).
//
// Each controller is a WKWebView in a borderless, transparent NSWindow standing for the guest's window (HWND): placed at the
// HWND's Win32 screen rect (SetWindowPos / MoveWindow, wv2_hwnd_changed), shown and hidden with it, floating above other
// windows (the game it overlays runs in another process). The page's side of WebView2 -- window.chrome.webview with
// postMessage, addEventListener("message"), removeEventListener -- is a document-start user script on WKScriptMessageHandler;
// host -> page messages are dispatched as "message" events. The page gets AppKit's events directly (wv2_pump: no Win32
// translation in this process), so keys, the mouse and the Gamepad API work as in a browser. Shared buffers (ICoreWebView2_17,
// Environment12) are not offered: the menus fall back ("the page keeps the see-through Spartan").
//
// Methods not implemented here return E_NOTIMPL and log "[wv2] <interface> slot N"; QueryInterface logs IIDs it doesn't know.
// PELOAD_WV2_TRACE=1 logs every call that is implemented too. PELOAD_NO_WV2=1 = old (the genuine loader DLL loads and fails).
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include "wv2.h"

#define MSABI __attribute__((ms_abi))
typedef int32_t HR;
#define S_OK 0
#define E_NOTIMPL ((HR)0x80004001)
#define E_NOINTERFACE ((HR)0x80004002)
#define E_POINTER ((HR)0x80004003)
#define E_INVALIDARG ((HR)0x80070057)
#define E_FAIL ((HR)0x80004005)
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } WGUID;
typedef struct { int32_t l, t, r, b; } WRect;

static void wlog(const char*fmt,...) __attribute__((format(printf,1,2)));
static void wlog(const char*fmt,...){ char b[600]; va_list ap; va_start(ap,fmt); int k=vsnprintf(b,sizeof b,fmt,ap); va_end(ap); if(k>0) (void)!write(2,b,(size_t)(k<(int)sizeof b?k:(int)sizeof b-1)); }
static int wv_tr(void){ static int t=-1; if(t<0) t=getenv("PELOAD_WV2_TRACE")?1:0; return t; }
#define TR(...) do{ if(wv_tr()) wlog(__VA_ARGS__); }while(0)

// ---- guest COM objects we call (handlers): vtbl[1] AddRef, [2] Release, [3] Invoke ----
static void* vt(void*o,int i){ return (*(void***)o)[i]; }
static void com_addref(void*o){ if(o) ((uint32_t(MSABI*)(void*))vt(o,1))(o); }
static void com_release(void*o){ if(o) ((uint32_t(MSABI*)(void*))vt(o,2))(o); }
static HR invoke2(void*h,void*a,void*b){ return h?((HR(MSABI*)(void*,void*,void*))vt(h,3))(h,a,b):S_OK; }
static HR invoke_hr_p(void*h,HR hr,void*p){ return h?((HR(MSABI*)(void*,HR,void*))vt(h,3))(h,hr,p):S_OK; }

// ---- strings ----
static NSString* ns_w(const uint16_t*w){ if(!w) return @""; size_t n=0; while(w[n]) n++; return [NSString stringWithCharacters:(const unichar*)w length:n]; }
static uint16_t* w_ns(NSString*s){ NSUInteger n=[s length]; uint16_t*o=malloc((n+1)*2); if(!o) return 0; [s getCharacters:(unichar*)o range:NSMakeRange(0,n)]; o[n]=0; return o; }   // (CoTaskMemFree = free)
static NSString* json_str(NSString*s){ NSData*d=[NSJSONSerialization dataWithJSONObject:@[s?s:@""] options:0 error:nil]; NSString*a=d?[[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] autorelease]:@"[\"\"]";
    return [a substringWithRange:NSMakeRange(1,[a length]-2)]; }

// ---- objects ----
typedef struct WvCtx WvCtx;
typedef struct { void** vtbl; int refs; int kind; WvCtx* c; } WvCom;
enum { K_ENV=1, K_CTL, K_WEB, K_SET, K_ARGS };
typedef struct { int64_t tok; void* h; } WvHandler;
enum { EV_MSG, EV_NAVDONE, EV_NAVSTART, EV_CONTENT, EV_DOMLOADED, EV_SOURCE, EV_TITLE, EV_OTHER, EV_N };
static NSString* wv_url_from(const uint16_t*uri);
@class WvDelegate;
struct WvCtx {
    WvCom ctl, web, set;
    void* hwnd; NSWindow* win; WKWebView* view; WvDelegate* del;
    int bx, by, bw, bh, ctl_visible, want;   // want: the guest has the window shown (ShowWindow)
    WvHandler ev[EV_N][16];
    uint64_t navid; NSString* source;
};
static int64_t g_tok=1;
static WvCom g_env; static int g_active;
// per-HWND state recorded before (and after) a controller exists: shown?
typedef struct { void* h; int shown; WvCtx* c; } WvWin;
static WvWin g_wins[16];
static WvWin* win_rec(void*h,int make){ for(int i=0;i<16;i++) if(g_wins[i].h==h) return &g_wins[i]; if(!make) return 0;
    for(int i=0;i<16;i++) if(!g_wins[i].h){ g_wins[i].h=h; g_wins[i].shown=-1; return &g_wins[i]; } return 0; }

// generic unimplemented-slot stubs, one per (family, slot), so the log names the slot
#define STUB(fam,n) static HR MSABI fam##_s##n(void*self){ (void)self; wlog("  [wv2] " #fam " slot %d not implemented\n",n); return E_NOTIMPL; }
#define STUB10(fam,b) STUB(fam,b##0) STUB(fam,b##1) STUB(fam,b##2) STUB(fam,b##3) STUB(fam,b##4) STUB(fam,b##5) STUB(fam,b##6) STUB(fam,b##7) STUB(fam,b##8) STUB(fam,b##9)
#define REF(fam,n) (void*)fam##_s##n
#define REF10(fam,b) REF(fam,b##0),REF(fam,b##1),REF(fam,b##2),REF(fam,b##3),REF(fam,b##4),REF(fam,b##5),REF(fam,b##6),REF(fam,b##7),REF(fam,b##8),REF(fam,b##9)
STUB(Environment,0) STUB(Environment,1) STUB(Environment,2) STUB(Environment,3) STUB(Environment,4) STUB(Environment,5) STUB(Environment,6) STUB(Environment,7) STUB(Environment,8) STUB(Environment,9)
STUB10(Environment,1) STUB10(Environment,2) STUB10(Environment,3)
STUB(Controller,0) STUB(Controller,1) STUB(Controller,2) STUB(Controller,3) STUB(Controller,4) STUB(Controller,5) STUB(Controller,6) STUB(Controller,7) STUB(Controller,8) STUB(Controller,9)
STUB10(Controller,1) STUB10(Controller,2) STUB10(Controller,3)
STUB(WebView,0) STUB(WebView,1) STUB(WebView,2) STUB(WebView,3) STUB(WebView,4) STUB(WebView,5) STUB(WebView,6) STUB(WebView,7) STUB(WebView,8) STUB(WebView,9)
STUB10(WebView,1) STUB10(WebView,2) STUB10(WebView,3) STUB10(WebView,4) STUB10(WebView,5) STUB10(WebView,6) STUB10(WebView,7) STUB10(WebView,8) STUB10(WebView,9)
STUB10(WebView,10) STUB10(WebView,11) STUB10(WebView,12) STUB10(WebView,13)
STUB(Args,0) STUB(Args,1) STUB(Args,2) STUB(Args,3) STUB(Args,4) STUB(Args,5) STUB(Args,6) STUB(Args,7) STUB(Args,8) STUB(Args,9) STUB10(Args,1)
static void* g_env_vt[40]={ REF(Environment,0),REF(Environment,1),REF(Environment,2),REF(Environment,3),REF(Environment,4),REF(Environment,5),REF(Environment,6),REF(Environment,7),REF(Environment,8),REF(Environment,9),REF10(Environment,1),REF10(Environment,2),REF10(Environment,3) };
static void* g_ctl_vt[40]={ REF(Controller,0),REF(Controller,1),REF(Controller,2),REF(Controller,3),REF(Controller,4),REF(Controller,5),REF(Controller,6),REF(Controller,7),REF(Controller,8),REF(Controller,9),REF10(Controller,1),REF10(Controller,2),REF10(Controller,3) };
static void* g_web_vt[140]={ REF(WebView,0),REF(WebView,1),REF(WebView,2),REF(WebView,3),REF(WebView,4),REF(WebView,5),REF(WebView,6),REF(WebView,7),REF(WebView,8),REF(WebView,9),
    REF10(WebView,1),REF10(WebView,2),REF10(WebView,3),REF10(WebView,4),REF10(WebView,5),REF10(WebView,6),REF10(WebView,7),REF10(WebView,8),REF10(WebView,9),REF10(WebView,10),REF10(WebView,11),REF10(WebView,12),REF10(WebView,13) };
static void* g_set_vt[40];
static void* g_args_vt[20]={ REF(Args,0),REF(Args,1),REF(Args,2),REF(Args,3),REF(Args,4),REF(Args,5),REF(Args,6),REF(Args,7),REF(Args,8),REF(Args,9),REF10(Args,1) };

// ---- IUnknown ----
static const char* iid_str(const WGUID*g,char*b){ snprintf(b,40,"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",g->d1,g->d2,g->d3,g->d4[0],g->d4[1],g->d4[2],g->d4[3],g->d4[4],g->d4[5],g->d4[6],g->d4[7]); return b; }
// IIDs each object answers (ICoreWebView2* interfaces derive in a line, so one flattened vtable serves every version we list)
static const uint32_t IID_ENV[]={0xb96d755e,0x41f3632b,0x80a22ae3,0x20944379,0x319e423d,0xe59ee362,0x43c22296,0xd6eb91dd,0xf06f41bf,0xee0eb9df,0xf0913dc6,0};
static const uint32_t IID_CTL[]={0x4d00c0d1,0xc979903e,0xf9614724,0x97d418d5,0};
static const uint32_t IID_WEB[]={0x76eceacb,0x9e8f0cf8,0xa0d6df20,0x20d02d59,0xbedb11b8,0x499aadac,0x79c24d83,0xe9632730,0x4d7b2eab,0xb1690564,0x0be78e56,0x35d69927,0xf75f09a8,0x6daa4f10,0x517b2d1d,0x0fbc1d33,0};
static const uint32_t IID_SET[]={0xe562e4f0,0xee9a0f68,0xfdb5ab74,0xcb56846c,0x183e7052,0x11cb3acd,0x488dc902,0x9e6b0e8f,0};
static HR MSABI wv_QI(WvCom*o,const WGUID*riid,void**pp){ if(!pp) return E_POINTER; *pp=0; if(!riid) return E_INVALIDARG;
    static const WGUID unk={0,0,0,{0xC0,0,0,0,0,0,0,0x46}};
    const uint32_t* list = o->kind==K_ENV?IID_ENV : o->kind==K_CTL?IID_CTL : o->kind==K_WEB?IID_WEB : o->kind==K_SET?IID_SET : 0;
    int ok=!memcmp(riid,&unk,sizeof unk); for(int i=0; !ok && list && list[i]; i++) if(riid->d1==list[i]) ok=1;
    char b[48]; if(!ok){ wlog("  [wv2] QueryInterface: %s not offered by the %s object\n",iid_str(riid,b),o->kind==K_ENV?"environment":o->kind==K_CTL?"controller":o->kind==K_WEB?"webview":o->kind==K_SET?"settings":"args"); return E_NOINTERFACE; }
    TR("  [wv2] QueryInterface %s on kind %d\n",iid_str(riid,b),o->kind);
    o->refs++; *pp=o; return S_OK; }
static uint32_t MSABI wv_AddRef(WvCom*o){ return (uint32_t)++o->refs; }
static uint32_t MSABI wv_Release(WvCom*o){ return (uint32_t)(o->refs>1?--o->refs:1); }   // (objects live as long as the process)

// ---- the page side: window.chrome.webview ----
static NSString* const kBridge =
    @"(function(){ if(window.chrome&&window.chrome.webview&&window.chrome.webview.__mac) return;"
    @" const L={}; window.chrome=window.chrome||{};"
    @" window.chrome.webview={ __mac:1,"
    @"  postMessage:function(m){ window.webkit.messageHandlers.wv2.postMessage(typeof m==='string'?{s:m}:{j:JSON.stringify(m)}); },"
    @"  addEventListener:function(t,f){ (L[t]=L[t]||[]).push(f); },"
    @"  removeEventListener:function(t,f){ const a=L[t]; if(a){ const i=a.indexOf(f); if(i>=0) a.splice(i,1); } },"
    @"  releaseBuffer:function(){},"
    @"  __dispatch:function(t,d){ const ev={data:d,source:location.href,additionalObjects:[]}; for(const f of (L[t]||[]).slice()){ try{ f(ev); }catch(e){ console.error(e); } } } }; })();";

@interface WvWindow : NSWindow @end
@implementation WvWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

// ---- event args objects ----
typedef struct { WvCom com; WvCtx* c; NSString* str; NSString* json; int isstr; int ok; uint64_t navid; NSString* uri; int cancel; } WvArgs;
static WvArgs* args_new(WvCtx*c){ WvArgs*a=calloc(1,sizeof *a); a->com.vtbl=0; a->com.refs=1; a->com.kind=K_ARGS; a->com.c=c; a->c=c; return a; }
// WebMessageReceived: 3 get_Source, 4 get_WebMessageAsJson, 5 TryGetWebMessageAsString
static HR MSABI msg_get_Source(WvArgs*a,uint16_t**o){ if(!o) return E_POINTER; *o=w_ns(a->c->source?a->c->source:@""); return S_OK; }
static HR MSABI msg_get_Json(WvArgs*a,uint16_t**o){ if(!o) return E_POINTER; *o=w_ns(a->isstr?json_str(a->str):(a->json?a->json:@"null")); return S_OK; }
static HR MSABI msg_TryString(WvArgs*a,uint16_t**o){ if(!o) return E_POINTER; *o=0; if(!a->isstr) return E_INVALIDARG; *o=w_ns(a->str); return S_OK; }
static void* g_msgargs_vt[20];
// NavigationCompleted: 3 get_IsSuccess, 4 get_WebErrorStatus, 5 get_NavigationId, 6 get_HttpStatusCode
static HR MSABI nav_IsSuccess(WvArgs*a,int32_t*o){ if(o)*o=a->ok; return S_OK; }
static HR MSABI nav_ErrorStatus(WvArgs*a,int32_t*o){ if(o)*o=a->ok?0:1; return S_OK; }
static HR MSABI nav_NavId(WvArgs*a,uint64_t*o){ if(o)*o=a->navid; return S_OK; }
static HR MSABI nav_HttpStatus(WvArgs*a,int32_t*o){ if(o)*o=a->ok?200:404; return S_OK; }
static void* g_navdone_vt[20];
// NavigationStarting: 3 get_Uri, 4 get_IsUserInitiated, 5 get_IsRedirected, 6 get_RequestHeaders, 7 get_Cancel, 8 put_Cancel, 9 get_NavigationId
static HR MSABI ns_Uri(WvArgs*a,uint16_t**o){ if(o)*o=w_ns(a->uri?a->uri:@""); return S_OK; }
static HR MSABI ns_False(WvArgs*a,int32_t*o){ (void)a; if(o)*o=0; return S_OK; }
static HR MSABI ns_GetCancel(WvArgs*a,int32_t*o){ if(o)*o=a->cancel; return S_OK; }
static HR MSABI ns_PutCancel(WvArgs*a,int32_t v){ a->cancel=v; return S_OK; }
static void* g_navstart_vt[20];
static void fire(WvCtx*c,int ev,void*args){ for(int i=0;i<16;i++) if(c->ev[ev][i].h) invoke2(c->ev[ev][i].h,&c->web,args); }

// ---- WKWebView delegate ----
@interface WvDelegate : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate> { @public WvCtx* c; } @end
@implementation WvDelegate
- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)m {
    (void)ucc; NSDictionary*d=[m.body isKindOfClass:[NSDictionary class]]?m.body:nil; if(!d) return;
    WvArgs*a=args_new(c); a->com.vtbl=g_msgargs_vt;
    if(d[@"s"]){ a->isstr=1; a->str=[d[@"s"] retain]; } else { a->json=[(d[@"j"]?d[@"j"]:@"null") retain]; }
    TR("  [wv2] page -> host: %.200s\n",[(a->isstr?a->str:a->json) UTF8String]);
    fire(c,EV_MSG,a); }
- (void)webView:(WKWebView*)w didStartProvisionalNavigation:(WKNavigation*)n { (void)w;(void)n; }
- (void)webView:(WKWebView*)w didCommitNavigation:(WKNavigation*)n { (void)n;
    [c->source release]; c->source=[[w.URL absoluteString] retain];
    WvArgs*a=args_new(c); a->com.vtbl=g_navdone_vt; a->ok=1; a->navid=c->navid; fire(c,EV_CONTENT,a); fire(c,EV_SOURCE,a); }
- (void)webView:(WKWebView*)w didFinishNavigation:(WKNavigation*)n { (void)w;(void)n;
    TR("  [wv2] navigation finished: %s\n",[[w.URL absoluteString] UTF8String]);
    WvArgs*a=args_new(c); a->com.vtbl=g_navdone_vt; a->ok=1; a->navid=c->navid; fire(c,EV_DOMLOADED,a); fire(c,EV_NAVDONE,a); }
- (void)webView:(WKWebView*)w didFailProvisionalNavigation:(WKNavigation*)n withError:(NSError*)e { (void)w;(void)n;
    wlog("  [wv2] navigation failed: %s\n",[[e localizedDescription] UTF8String]);
    WvArgs*a=args_new(c); a->com.vtbl=g_navdone_vt; a->ok=0; a->navid=c->navid; fire(c,EV_NAVDONE,a); }
- (void)webView:(WKWebView*)w didFailNavigation:(WKNavigation*)n withError:(NSError*)e { [self webView:w didFailProvisionalNavigation:n withError:e]; }
- (void)webView:(WKWebView*)w runJavaScriptAlertPanelWithMessage:(NSString*)msg initiatedByFrame:(WKFrameInfo*)f completionHandler:(void(^)(void))done {
    (void)w;(void)f; wlog("  [wv2] page alert: %s\n",[msg UTF8String]); done(); }
@end

// ---- the window standing for an HWND ----
static NSRect ns_rect(int x,int y,int w,int h){ NSRect s=[[[NSScreen screens] objectAtIndex:0] frame]; return NSMakeRect(x, s.size.height-(y+h), w, h); }
static void ctx_place(WvCtx*c){ int x=0,y=0,w=640,h=360; if(!pl_hwnd_geom(c->hwnd,&x,&y,&w,&h)) return; if(w<1||h<1) return;
    [c->win setFrame:ns_rect(x,y,w,h) display:YES];
    if(c->bw>0 && c->bh>0) [c->view setFrame:NSMakeRect(c->bx, h-(c->by+c->bh), c->bw, c->bh)];
    else [c->view setFrame:NSMakeRect(0,0,w,h)];
    TR("  [wv2] window %p at %d,%d %dx%d\n",c->hwnd,x,y,w,h); }
// Over the game, not over everything (2026-10-20, the user: the menus stayed on top of every other app): the window is floating
// only while one of OUR processes is frontmost -- this one (the menus) or a game whose window is published (xwin.c's
// ~/Library/Application Support/ReclaimerMac/windows/<pid>.win) -- and out of the way otherwise (wv2_pump re-checks every 200 ms).
// Showing it never takes activation from another app; SetForegroundWindow on it takes it only from our own processes.
static int ours_frontmost(void){ NSRunningApplication*f=[[NSWorkspace sharedWorkspace] frontmostApplication]; if(!f) return 0; pid_t pid=[f processIdentifier];
    if(pid==getpid()) return 1; char p[1200]; const char*h=getenv("HOME"); snprintf(p,sizeof p,"%s/Library/Application Support/ReclaimerMac/windows/%d.win",h?h:"/tmp",(int)pid); return access(p,F_OK)==0; }
static void ctx_show(WvCtx*c,int on){ c->want=on; if(on){ if(ours_frontmost()){ [c->win orderFrontRegardless]; [c->win makeKeyWindow]; } } else [c->win orderOut:nil]; }
static void ctx_frontmost_tick(void){ static double last; double now=[NSDate timeIntervalSinceReferenceDate]; if(now-last<0.2) return; last=now; int ours=-1;
    for(int i=0;i<16;i++){ WvCtx*c=g_wins[i].c; if(!c||!c->want) continue; if(ours<0) ours=ours_frontmost();
        if(ours && ![c->win isVisible]) [c->win orderFrontRegardless]; else if(!ours && [c->win isVisible]) [c->win orderOut:nil]; } }
static void app_init(void){ static int done; if(done) return; done=1;
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular]; [NSApp finishLaunching];
    const char*ic=getenv("PELOAD_APP_ICON"); if(ic){ NSImage*im=[[[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:ic]] autorelease]; if(im) [NSApp setApplicationIconImage:im]; } }

static HR MSABI env_CreateController(WvCom*env,void*hwnd,void*handler); // fwd
static WvCtx* ctx_new(void*hwnd){
    app_init();
    WvCtx*c=calloc(1,sizeof *c); c->hwnd=hwnd; c->ctl_visible=1;
    c->ctl.vtbl=g_ctl_vt; c->ctl.refs=1; c->ctl.kind=K_CTL; c->ctl.c=c;
    c->web.vtbl=g_web_vt; c->web.refs=1; c->web.kind=K_WEB; c->web.c=c;
    c->set.vtbl=g_set_vt; c->set.refs=1; c->set.kind=K_SET; c->set.c=c;
    int x=0,y=0,w=640,h=360; pl_hwnd_geom(hwnd,&x,&y,&w,&h); if(w<1) w=640; if(h<1) h=360;
    c->win=[[WvWindow alloc] initWithContentRect:ns_rect(x,y,w,h) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    [c->win setOpaque:NO]; [c->win setBackgroundColor:[NSColor clearColor]]; [c->win setHasShadow:NO]; [c->win setLevel:NSFloatingWindowLevel];
    [c->win setReleasedWhenClosed:NO]; [c->win setAcceptsMouseMovedEvents:YES]; [c->win setTitle:@"Reach Renaissance menus"];
    WKWebViewConfiguration*cfg=[[[WKWebViewConfiguration alloc] init] autorelease];
    c->del=[[WvDelegate alloc] init]; c->del->c=c;
    [cfg.userContentController addScriptMessageHandler:c->del name:@"wv2"];
    [cfg.userContentController addUserScript:[[[WKUserScript alloc] initWithSource:kBridge injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO] autorelease]];
    @try { [cfg.preferences setValue:@YES forKey:@"allowFileAccessFromFileURLs"]; [cfg.preferences setValue:@YES forKey:@"developerExtrasEnabled"]; } @catch(NSException*e){ (void)e; }
    @try { [cfg setValue:@YES forKey:@"allowUniversalAccessFromFileURLs"]; } @catch(NSException*e){ (void)e; }
    cfg.mediaTypesRequiringUserActionForPlayback=WKAudiovisualMediaTypeNone;
    c->view=[[WKWebView alloc] initWithFrame:NSMakeRect(0,0,w,h) configuration:cfg];
    c->view.navigationDelegate=c->del; c->view.UIDelegate=c->del;
    @try { [c->view setValue:@NO forKey:@"drawsBackground"]; } @catch(NSException*e){ (void)e; }
    if(@available(macOS 13.3,*)){ if(getenv("RR_DEVTOOLS_PORT")||getenv("PELOAD_WV2_INSPECT")) c->view.inspectable=YES; }
    [c->win setContentView:c->view];
    WvWin*r=win_rec(hwnd,1); if(r){ r->c=c; if(r->shown==1) ctx_show(c,1); }
    g_active=1; wlog("  [wv2] WebView2 on WKWebView for window %p (%dx%d)\n",hwnd,w,h);
    return c; }

// ---- ICoreWebView2Environment ----
static HR MSABI env_CreateController(WvCom*env,void*hwnd,void*handler){ (void)env; @autoreleasepool {
    WvCtx*c=ctx_new(hwnd); com_addref(handler); HR hr=invoke_hr_p(handler,S_OK,&c->ctl); com_release(handler); (void)hr; return S_OK; } }
static HR MSABI env_get_BrowserVersion(WvCom*env,uint16_t**o){ (void)env; if(!o) return E_POINTER; *o=w_ns(@"120.0.2210.91"); return S_OK; }
static HR MSABI env_add_event(WvCom*env,void*h,int64_t*tok){ (void)env;(void)h; if(tok)*tok=g_tok++; return S_OK; }
static HR MSABI env_remove_event(WvCom*env,int64_t tok){ (void)env;(void)tok; return S_OK; }
static HR MSABI env_get_UserDataFolder(WvCom*env,uint16_t**o){ (void)env; if(o)*o=w_ns(@"C:\\ReclaimerCache\\ReachRenaissance\\webview"); return S_OK; }

// ---- ICoreWebView2Controller ----
static HR MSABI ctl_get_IsVisible(WvCom*o,int32_t*v){ if(v)*v=o->c->ctl_visible; return S_OK; }
static HR MSABI ctl_put_IsVisible(WvCom*o,int32_t v){ o->c->ctl_visible=v!=0; [o->c->view setHidden:!v]; TR("  [wv2] controller visible %d\n",v); return S_OK; }
static HR MSABI ctl_get_Bounds(WvCom*o,WRect*r){ WvCtx*c=o->c; if(!r) return E_POINTER; int x=0,y=0,w=0,h=0; pl_hwnd_geom(c->hwnd,&x,&y,&w,&h);
    if(c->bw>0){ r->l=c->bx; r->t=c->by; r->r=c->bx+c->bw; r->b=c->by+c->bh; } else { r->l=0; r->t=0; r->r=w; r->b=h; } return S_OK; }
static HR MSABI ctl_put_Bounds(WvCom*o,const WRect*r){ WvCtx*c=o->c; if(!r) return E_POINTER; @autoreleasepool { c->bx=r->l; c->by=r->t; c->bw=r->r-r->l; c->bh=r->b-r->t; ctx_place(c); } TR("  [wv2] bounds %d,%d %dx%d\n",c->bx,c->by,c->bw,c->bh); return S_OK; }
static HR MSABI ctl_get_Zoom(WvCom*o,double*z){ (void)o; if(z)*z=1.0; return S_OK; }
static HR MSABI ctl_put_Zoom(WvCom*o,double z){ (void)o;(void)z; return S_OK; }
static HR MSABI ctl_SetBoundsAndZoom(WvCom*o,const WRect*r,double z){ (void)z; return ctl_put_Bounds(o,r); }
static HR MSABI ctl_MoveFocus(WvCom*o,int32_t reason){ (void)reason; @autoreleasepool { [o->c->win makeFirstResponder:o->c->view]; } return S_OK; }
static HR MSABI ctl_add_event(WvCom*o,void*h,int64_t*tok){ (void)o;(void)h; if(tok)*tok=g_tok++; return S_OK; }
static HR MSABI ctl_remove_event(WvCom*o,int64_t tok){ (void)o;(void)tok; return S_OK; }
static HR MSABI ctl_get_Parent(WvCom*o,void**h){ if(h)*h=o->c->hwnd; return S_OK; }
static HR MSABI ctl_put_Parent(WvCom*o,void*h){ o->c->hwnd=h; @autoreleasepool { ctx_place(o->c); } return S_OK; }
static HR MSABI ctl_NotifyParentMoved(WvCom*o){ @autoreleasepool { ctx_place(o->c); } return S_OK; }
static HR MSABI ctl_Close(WvCom*o){ @autoreleasepool { [o->c->win orderOut:nil]; } return S_OK; }
static HR MSABI ctl_get_CoreWebView2(WvCom*o,void**w){ if(!w) return E_POINTER; o->c->web.refs++; *w=&o->c->web; return S_OK; }
static HR MSABI ctl_get_BgColor(WvCom*o,uint32_t*col){ (void)o; if(col)*col=0; return S_OK; }
static HR MSABI ctl_put_BgColor(WvCom*o,uint32_t col){ (void)o; TR("  [wv2] background color %08x\n",col); return S_OK; }   // (A = low byte; the view never draws a background)
static HR MSABI ctl_get_Scale(WvCom*o,double*s){ @autoreleasepool { if(s)*s=[o->c->win backingScaleFactor]; } return S_OK; }
static HR MSABI ctl_put_Scale(WvCom*o,double s){ (void)o;(void)s; return S_OK; }
static HR MSABI ctl_get_Bool1(WvCom*o,int32_t*b){ (void)o; if(b)*b=1; return S_OK; }
static HR MSABI ctl_put_Any(WvCom*o,int64_t v){ (void)o;(void)v; return S_OK; }
static HR MSABI ctl_get_Int0(WvCom*o,int32_t*v){ (void)o; if(v)*v=0; return S_OK; }

// ---- ICoreWebView2 ----
static HR MSABI web_get_Settings(WvCom*o,void**s){ if(!s) return E_POINTER; o->c->set.refs++; *s=&o->c->set; return S_OK; }
static HR MSABI web_get_Source(WvCom*o,uint16_t**s){ if(!s) return E_POINTER; *s=w_ns(o->c->source?o->c->source:@"about:blank"); return S_OK; }
static HR MSABI web_Navigate(WvCom*o,const uint16_t*uri){ WvCtx*c=o->c; @autoreleasepool {
        NSString*u=wv_url_from(uri); NSURL*url=[NSURL URLWithString:u]; c->navid++;
        wlog("  [wv2] Navigate %s\n",[u UTF8String]);
        if(!url) return E_INVALIDARG;
        WvArgs*a=args_new(c); a->com.vtbl=g_navstart_vt; a->navid=c->navid; a->uri=[u retain]; fire(c,EV_NAVSTART,a); if(a->cancel) return S_OK;
        if([url isFileURL]){ char root[1200]; static const uint16_t croot[]={'C',':','\\',0}; pl_win2unix(croot,root,sizeof root);
            [c->view loadFileURL:url allowingReadAccessToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:root] isDirectory:YES]]; }
        else [c->view loadRequest:[NSURLRequest requestWithURL:url]];
    } return S_OK; }
static HR MSABI web_NavigateToString(WvCom*o,const uint16_t*html){ @autoreleasepool { o->c->navid++; [o->c->view loadHTMLString:ns_w(html) baseURL:nil]; } return S_OK; }
static int ev_for_slot(int slot){ switch(slot){ case 7: return EV_NAVSTART; case 9: return EV_CONTENT; case 11: return EV_SOURCE; case 15: return EV_NAVDONE; case 34: return EV_MSG; case 46: return EV_TITLE; case 64: return EV_DOMLOADED; } return EV_OTHER; }
static HR web_add(WvCom*o,int slot,void*h,int64_t*tok){ WvCtx*c=o->c; int ev=ev_for_slot(slot); int64_t t=g_tok++;
    for(int i=0;i<16;i++) if(!c->ev[ev][i].h){ com_addref(h); c->ev[ev][i].h=h; c->ev[ev][i].tok=t; break; }
    if(tok)*tok=t; TR("  [wv2] add event slot %d -> token %lld\n",slot,(long long)t); return S_OK; }
static HR MSABI web_remove(WvCom*o,int64_t tok){ WvCtx*c=o->c; for(int e=0;e<EV_N;e++) for(int i=0;i<16;i++) if(c->ev[e][i].h && c->ev[e][i].tok==tok){ com_release(c->ev[e][i].h); c->ev[e][i].h=0; } return S_OK; }
#define ADD(n) static HR MSABI web_add##n(WvCom*o,void*h,int64_t*tok){ return web_add(o,n,h,tok); }
ADD(7) ADD(9) ADD(11) ADD(13) ADD(15) ADD(17) ADD(19) ADD(21) ADD(23) ADD(25) ADD(34) ADD(44) ADD(46) ADD(52) ADD(55) ADD(59) ADD(61) ADD(64) ADD(73) ADD(75) ADD(77) ADD(81) ADD(85) ADD(88) ADD(97) ADD(100) ADD(102) ADD(106) ADD(109)
static HR MSABI web_AddScriptOnCreated(WvCom*o,const uint16_t*js,void*handler){ @autoreleasepool {
        static int n; NSString*src=ns_w(js); TR("  [wv2] AddScriptToExecuteOnDocumentCreated (%lu chars)\n",(unsigned long)[src length]);
        [o->c->view.configuration.userContentController addUserScript:[[[WKUserScript alloc] initWithSource:src injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES] autorelease]];
        uint16_t id[16]; snprintf((char*)id,sizeof id,"%d",++n); uint16_t wid[16]; int i=0; for(; ((char*)id)[i]; i++) wid[i]=(uint8_t)((char*)id)[i]; wid[i]=0;
        com_addref(handler); invoke_hr_p(handler,S_OK,wid); com_release(handler); } return S_OK; }
static HR MSABI web_RemoveScriptOnCreated(WvCom*o,const uint16_t*id){ (void)o;(void)id; return S_OK; }
static HR MSABI web_ExecuteScript(WvCom*o,const uint16_t*js,void*handler){ WvCtx*c=o->c; @autoreleasepool {
        NSString*src=ns_w(js); TR("  [wv2] ExecuteScript %.160s\n",[src UTF8String]);
        com_addref(handler);
        [c->view evaluateJavaScript:src completionHandler:^(id r, NSError*e){ @autoreleasepool {
            NSString*json=@"null";
            if(r && !e){ if([r isKindOfClass:[NSString class]]) json=json_str(r);
                else if([NSJSONSerialization isValidJSONObject:@[r]]){ NSData*d=[NSJSONSerialization dataWithJSONObject:@[r] options:0 error:nil]; NSString*a=[[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] autorelease]; if([a length]>=2) json=[a substringWithRange:NSMakeRange(1,[a length]-2)]; } }
            if(handler){ uint16_t*w=w_ns(json); invoke_hr_p(handler,e?E_FAIL:S_OK,w); free(w); com_release(handler); } } }];
    } return S_OK; }
static void post_to_page(WvCtx*c,NSString*dataExpr){ NSString*js=[NSString stringWithFormat:@"window.chrome&&window.chrome.webview&&window.chrome.webview.__dispatch('message',%@)",dataExpr];
    [c->view evaluateJavaScript:js completionHandler:nil]; }
static HR MSABI web_PostJson(WvCom*o,const uint16_t*json){ @autoreleasepool { NSString*s=ns_w(json); TR("  [wv2] host -> page (json): %.200s\n",[s UTF8String]); post_to_page(o->c,[s length]?s:@"null"); } return S_OK; }
static HR MSABI web_PostString(WvCom*o,const uint16_t*str){ @autoreleasepool { NSString*s=ns_w(str); TR("  [wv2] host -> page: %.200s\n",[s UTF8String]); post_to_page(o->c,json_str(s)); } return S_OK; }
static HR MSABI web_Reload(WvCom*o){ @autoreleasepool { [o->c->view reload]; } return S_OK; }
static HR MSABI web_Stop(WvCom*o){ @autoreleasepool { [o->c->view stopLoading]; } return S_OK; }
static HR MSABI web_get_Pid(WvCom*o,uint32_t*p){ (void)o; if(p)*p=(uint32_t)getpid(); return S_OK; }
static HR MSABI web_get_False(WvCom*o,int32_t*b){ (void)o; if(b)*b=0; return S_OK; }
static HR MSABI web_Ok(WvCom*o){ (void)o; return S_OK; }
static HR MSABI web_get_Title(WvCom*o,uint16_t**t){ @autoreleasepool { if(t)*t=w_ns(o->c->view.title?o->c->view.title:@""); } return S_OK; }
static HR MSABI web_OpenDevTools(WvCom*o){ (void)o; wlog("  [wv2] OpenDevToolsWindow: use Safari's Develop menu (PELOAD_WV2_INSPECT=1)\n"); return S_OK; }
static HR MSABI web_AddFilter(WvCom*o,const uint16_t*uri,int32_t ctx){ (void)o;(void)ctx; @autoreleasepool { wlog("  [wv2] AddWebResourceRequestedFilter %s: not served\n",[ns_w(uri) UTF8String]); } return S_OK; }
static HR MSABI web_get_Env(WvCom*o,void**e){ (void)o; if(!e) return E_POINTER; g_env.refs++; *e=&g_env; return S_OK; }
static HR MSABI web_SetVirtualHost(WvCom*o,const uint16_t*host,const uint16_t*folder,int32_t kind){ (void)o;(void)kind; @autoreleasepool { wlog("  [wv2] SetVirtualHostNameToFolderMapping %s -> %s: not done\n",[ns_w(host) UTF8String],[ns_w(folder) UTF8String]); } return S_OK; }
static HR MSABI web_get_Muted(WvCom*o,int32_t*b){ (void)o; if(b)*b=0; return S_OK; }
static HR MSABI web_put_Any(WvCom*o,int64_t v){ (void)o;(void)v; return S_OK; }

// ---- ICoreWebView2Settings: every BOOL property reads TRUE and takes any value ----
static HR MSABI set_get_True(WvCom*o,int32_t*b){ (void)o; if(b)*b=1; return S_OK; }
static HR MSABI set_put(WvCom*o,int64_t v){ (void)o;(void)v; return S_OK; }
static HR MSABI set_get_UA(WvCom*o,uint16_t**s){ @autoreleasepool { if(s)*s=w_ns(@"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0"); } (void)o; return S_OK; }

// Windows path / file URL -> a URL for WKWebView (file:///<prefix path>?query, or as is)
static NSString* wv_url_from(const uint16_t*uri){
    NSString*u=ns_w(uri); if(![[u lowercaseString] hasPrefix:@"file:"]) return u;
    NSString*rest=[u substringFromIndex:5]; NSString*query=@""; NSRange q=[rest rangeOfString:@"?" options:NSBackwardsSearch];
    NSRange q3=[rest rangeOfString:@"%3F/" options:NSCaseInsensitiveSearch];
    if(q3.location!=NSNotFound) rest=[rest substringFromIndex:q3.location+q3.length];   // file://///%3F/C:/... (a \\?\ path made a URL)
    q=[rest rangeOfString:@"?"]; if(q.location!=NSNotFound){ query=[rest substringFromIndex:q.location]; rest=[rest substringToIndex:q.location]; }
    while([rest hasPrefix:@"/"]) rest=[rest substringFromIndex:1];
    rest=[rest stringByRemovingPercentEncoding]?:rest; rest=[rest stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
    uint16_t*w=w_ns(rest); char p[2048]; pl_win2unix(w,p,sizeof p); free(w);
    NSURL*f=[NSURL fileURLWithPath:[NSString stringWithUTF8String:p]]; return [[f absoluteString] stringByAppendingString:query]; }

static void tables_init(void){ static int done; if(done) return; done=1;
    void** e=g_env_vt; e[0]=(void*)wv_QI; e[1]=(void*)wv_AddRef; e[2]=(void*)wv_Release; e[3]=(void*)env_CreateController; e[5]=(void*)env_get_BrowserVersion;
        e[6]=(void*)env_add_event; e[7]=(void*)env_remove_event; e[12]=(void*)env_add_event; e[13]=(void*)env_remove_event; e[15]=(void*)env_get_UserDataFolder; e[16]=(void*)env_add_event; e[17]=(void*)env_remove_event;
    void** c=g_ctl_vt; c[0]=(void*)wv_QI; c[1]=(void*)wv_AddRef; c[2]=(void*)wv_Release;
        c[3]=(void*)ctl_get_IsVisible; c[4]=(void*)ctl_put_IsVisible; c[5]=(void*)ctl_get_Bounds; c[6]=(void*)ctl_put_Bounds; c[7]=(void*)ctl_get_Zoom; c[8]=(void*)ctl_put_Zoom;
        c[9]=(void*)ctl_add_event; c[10]=(void*)ctl_remove_event; c[11]=(void*)ctl_SetBoundsAndZoom; c[12]=(void*)ctl_MoveFocus;
        for(int i=13;i<=20;i++) c[i]=(i&1)?(void*)ctl_add_event:(void*)ctl_remove_event;
        c[21]=(void*)ctl_get_Parent; c[22]=(void*)ctl_put_Parent; c[23]=(void*)ctl_NotifyParentMoved; c[24]=(void*)ctl_Close; c[25]=(void*)ctl_get_CoreWebView2;
        c[26]=(void*)ctl_get_BgColor; c[27]=(void*)ctl_put_BgColor; c[28]=(void*)ctl_get_Scale; c[29]=(void*)ctl_put_Scale; c[30]=(void*)ctl_get_Bool1; c[31]=(void*)ctl_put_Any;
        c[32]=(void*)ctl_add_event; c[33]=(void*)ctl_remove_event; c[34]=(void*)ctl_get_Int0; c[35]=(void*)ctl_put_Any; c[36]=(void*)ctl_get_Bool1; c[37]=(void*)ctl_put_Any;
    void** w=g_web_vt; w[0]=(void*)wv_QI; w[1]=(void*)wv_AddRef; w[2]=(void*)wv_Release;
        w[3]=(void*)web_get_Settings; w[4]=(void*)web_get_Source; w[5]=(void*)web_Navigate; w[6]=(void*)web_NavigateToString;
        w[7]=(void*)web_add7; w[9]=(void*)web_add9; w[11]=(void*)web_add11; w[13]=(void*)web_add13; w[15]=(void*)web_add15; w[17]=(void*)web_add17; w[19]=(void*)web_add19;
        w[21]=(void*)web_add21; w[23]=(void*)web_add23; w[25]=(void*)web_add25; w[34]=(void*)web_add34; w[44]=(void*)web_add44; w[46]=(void*)web_add46; w[52]=(void*)web_add52;
        w[55]=(void*)web_add55; w[59]=(void*)web_add59; w[61]=(void*)web_add61; w[64]=(void*)web_add64; w[73]=(void*)web_add73; w[75]=(void*)web_add75; w[77]=(void*)web_add77;
        w[81]=(void*)web_add81; w[85]=(void*)web_add85; w[88]=(void*)web_add88; w[97]=(void*)web_add97; w[100]=(void*)web_add100; w[102]=(void*)web_add102; w[106]=(void*)web_add106; w[109]=(void*)web_add109;
        static const int rem[]={8,10,12,14,16,18,20,22,24,26,35,45,47,53,56,60,62,65,74,76,78,82,86,89,98,101,103,107,110,0};
        for(int i=0;rem[i];i++) w[rem[i]]=(void*)web_remove;
        w[27]=(void*)web_AddScriptOnCreated; w[28]=(void*)web_RemoveScriptOnCreated; w[29]=(void*)web_ExecuteScript; w[31]=(void*)web_Reload;
        w[32]=(void*)web_PostJson; w[33]=(void*)web_PostString; w[37]=(void*)web_get_Pid; w[38]=(void*)web_get_False; w[39]=(void*)web_get_False; w[40]=(void*)web_Ok; w[41]=(void*)web_Ok;
        w[43]=(void*)web_Stop; w[48]=(void*)web_get_Title; w[51]=(void*)web_OpenDevTools; w[54]=(void*)web_get_False; w[57]=(void*)web_AddFilter; w[58]=(void*)web_Ok;
        w[67]=(void*)web_get_Env; w[71]=(void*)web_SetVirtualHost; w[72]=(void*)web_Ok; w[83]=(void*)web_get_Muted; w[84]=(void*)web_put_Any; w[87]=(void*)web_get_False;
    void** s=g_set_vt; s[0]=(void*)wv_QI; s[1]=(void*)wv_AddRef; s[2]=(void*)wv_Release; for(int i=3;i<40;i++) s[i]=(i&1)?(void*)set_get_True:(void*)set_put; s[21]=(void*)set_get_UA;
    void** a=g_msgargs_vt; memcpy(a,g_args_vt,sizeof g_args_vt); a[0]=(void*)wv_QI; a[1]=(void*)wv_AddRef; a[2]=(void*)wv_Release; a[3]=(void*)msg_get_Source; a[4]=(void*)msg_get_Json; a[5]=(void*)msg_TryString;
    a=g_navdone_vt; memcpy(a,g_args_vt,sizeof g_args_vt); a[0]=(void*)wv_QI; a[1]=(void*)wv_AddRef; a[2]=(void*)wv_Release; a[3]=(void*)nav_IsSuccess; a[4]=(void*)nav_ErrorStatus; a[5]=(void*)nav_NavId; a[6]=(void*)nav_HttpStatus;
    a=g_navstart_vt; memcpy(a,g_args_vt,sizeof g_args_vt); a[0]=(void*)wv_QI; a[1]=(void*)wv_AddRef; a[2]=(void*)wv_Release; a[3]=(void*)ns_Uri; a[4]=(void*)ns_False; a[5]=(void*)ns_False; a[7]=(void*)ns_GetCancel; a[8]=(void*)ns_PutCancel; a[9]=(void*)nav_NavId;
    g_env.vtbl=g_env_vt; g_env.refs=1; g_env.kind=K_ENV; }

// ---- WebView2Loader.dll exports ----
static HR MSABI wv_CreateEnvWithOptions(const uint16_t*browserFolder,const uint16_t*userData,void*options,void*handler){ (void)browserFolder;(void)options;
    tables_init(); @autoreleasepool { wlog("  [wv2] CreateCoreWebView2EnvironmentWithOptions (user data %s): WKWebView\n",userData?[ns_w(userData) UTF8String]:"(default)"); }
    g_env.refs++; com_addref(handler); invoke_hr_p(handler,S_OK,&g_env); com_release(handler); return S_OK; }
static HR MSABI wv_CreateEnv(void*handler){ return wv_CreateEnvWithOptions(0,0,0,handler); }
static HR MSABI wv_GetVersion(const uint16_t*folder,uint16_t**v){ (void)folder; @autoreleasepool { if(v)*v=w_ns(@"120.0.2210.91"); } return S_OK; }
static HR MSABI wv_CompareVersions(const uint16_t*a,const uint16_t*b,int32_t*r){ @autoreleasepool { if(r)*r=(int32_t)[ns_w(a) compare:ns_w(b) options:NSNumericSearch]; } return S_OK; }
void* wv2_resolve(const char*n){ if(!n || getenv("PELOAD_NO_WV2")) return 0;
    if(!strcmp(n,"CreateCoreWebView2EnvironmentWithOptions")) return (void*)wv_CreateEnvWithOptions;
    if(!strcmp(n,"CreateCoreWebView2Environment")) return (void*)wv_CreateEnv;
    if(!strcmp(n,"GetAvailableCoreWebView2BrowserVersionString")) return (void*)wv_GetVersion;
    if(!strcmp(n,"CompareBrowserVersions")) return (void*)wv_CompareVersions;
    return 0; }
// PELOAD_WV2_SNAPSHOT=<secs>: every <secs> seconds, the page as WKWebView renders it -> ~/reclaimer_wv2_<pid>_<n>.png (checks
// without a screenshot of the desktop; the background is transparent where the game shows through)
static void wv_snapshot_tick(void){ static double every=-1, last; static int n; if(every<0){ const char*e=getenv("PELOAD_WV2_SNAPSHOT"); every=e?atof(e):0; }
    if(every<=0) return; double now=[NSDate timeIntervalSinceReferenceDate]; if(!last) last=now; if(now-last<every) return; last=now;
    for(int i=0;i<16;i++){ WvCtx*c=g_wins[i].c; if(!c||!c->view) continue; int k=n++;
        [c->view takeSnapshotWithConfiguration:nil completionHandler:^(NSImage*im, NSError*e){ if(!im){ wlog("  [wv2] snapshot failed: %s\n",e?[[e localizedDescription] UTF8String]:"?"); return; }
            CGImageRef cg=[im CGImageForProposedRect:NULL context:nil hints:nil]; if(!cg) return; NSBitmapImageRep*rep=[[[NSBitmapImageRep alloc] initWithCGImage:cg] autorelease];
            NSData*png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            NSString*p=[NSString stringWithFormat:@"%s/reclaimer_wv2_%d_%03d.png",getenv("HOME")?getenv("HOME"):"/tmp",(int)getpid(),k]; [png writeToFile:p atomically:YES];
            wlog("  [wv2] snapshot %s (%dx%d)\n",[p UTF8String],(int)CGImageGetWidth(cg),(int)CGImageGetHeight(cg)); }]; } }
int wv2_active(void){ return g_active; }
int wv2_owns(void*h){ WvWin*r=win_rec(h,0); return r && r->c; }
void wv2_pump(void){ @autoreleasepool {
    for(;;){ NSEvent*ev=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]; if(!ev) break; [NSApp sendEvent:ev]; }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,0,true); [NSApp updateWindows]; ctx_frontmost_tick(); wv_snapshot_tick(); } }
void wv2_hwnd_changed(void*h,int show){ WvWin*r=win_rec(h,show>=0); if(!r) return; if(show>=0) r->shown=show;
    if(r->c){ @autoreleasepool { ctx_place(r->c); if(show>=0) ctx_show(r->c,show); } } }
// GWL_EXSTYLE: WS_EX_TRANSPARENT (0x20) lets the mouse through to the window below (the game), as on Windows
void wv2_hwnd_style(void*h,uint32_t ex){ WvWin*r=win_rec(h,0); if(!r||!r->c) return; @autoreleasepool { [r->c->win setIgnoresMouseEvents:(ex&0x20)?YES:NO]; } TR("  [wv2] window %p click-through %d\n",h,(ex&0x20)?1:0); }
// SetForegroundWindow on a WebView2 window: activate the menus -- only when one of our processes is frontmost (never from another app)
void wv2_activate(void*h){ WvWin*r=win_rec(h,0); if(!r||!r->c) return; @autoreleasepool { if(!ours_frontmost()) return; [NSApp activateIgnoringOtherApps:YES]; [r->c->win makeKeyWindow]; [r->c->win makeFirstResponder:r->c->view]; } }
