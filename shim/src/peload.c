// peload.c — minimal Win64 PE loader for macOS/Rosetta (x86_64).
// Phase-1 brick for the Project Reclaimer dedicated-server shim.
//
// What it does today:
//   * maps a PE64 image (DLL or EXE), honoring section layout
//   * applies DIR64 base relocations so it can load anywhere
//   * resolves every normal import to a per-function logging stub
//   * decodes (but does not bind) delay imports, just to report them
//   * installs a minimal TEB via the %gs overlay (proven under Rosetta)
//   * runs TLS callbacks, then calls the entry point
//
// It is a diagnostic: logging is via raw write(2) so it survives the gs
// clobber, and every stubbed import prints "DLL!func" on first call. The
// first thing real engine code does that we haven't provided shows up as
// the last line before a fault. That drives the next phase.
//
// Build:  clang -arch x86_64 -O1 -o build/peload src/peload.c
// Run:    arch -x86_64 build/peload <path-to.pe>
//
// Not yet: full SEH unwinding, recursive loading of the real ucrt/msvcp,
// IOCP/AFD, or a correct PEB. Those are later phases.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <time.h>

// ---- raw logging (no libc TLS dependency; safe after we touch %gs) ----
static void w(const char *s, long n){ (void)!write(2, s, n); }
static void ws(const char *s){ long n=0; while(s[n]) n++; w(s,n); }
static void whex(uint64_t v){
    char b[19]; b[0]='0'; b[1]='x'; for(int i=0;i<16;i++){ int nib=(v>>((15-i)*4))&0xf; b[2+i]=nib<10?'0'+nib:'a'+nib-10; } b[18]='\n'; w(b,19);
}
static void whexn(uint64_t v){ char b[18]; b[0]='0'; b[1]='x'; for(int i=0;i<16;i++){int nib=(v>>((15-i)*4))&0xf; b[2+i]=nib<10?'0'+nib:'a'+nib-10;} w(b,18); }

// ---- PE structures (only the fields we use) ----
#pragma pack(push,1)
typedef struct { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; } DOS;
typedef struct { uint32_t Sig; uint16_t Machine, NumSec; uint32_t TS, pSym, nSym; uint16_t OptSz, Chars; } COFF;
typedef struct {
    uint16_t Magic; uint8_t Major, Minor; uint32_t SizeCode, SizeInit, SizeUninit, EntryRVA, BaseCode;
    uint64_t ImageBase; uint32_t SectAlign, FileAlign; uint8_t rest[16]; uint32_t ImageSize, HdrSize, CheckSum;
    uint16_t Subsys, DllChars; uint64_t StackRes, StackCommit, HeapRes, HeapCommit; uint32_t LoaderFlags, NumDirs;
    struct { uint32_t VA, Size; } Dir[16];
} OPT64;
typedef struct { char Name[8]; uint32_t VSize, VA, RawSize, RawPtr, pReloc, pLine; uint16_t nReloc, nLine; uint32_t Chars; } SEC;
typedef struct { uint32_t OFT, TS, Fwd, Name, FT; } IMPDESC;
typedef struct { uint32_t Attrs, Name, Mod, IAT, INT, Bound, Unload, TS; } DELAYDESC;
typedef struct { uint64_t Start, End, Index, Callbacks; uint32_t Zero, Flags; } TLSDIR;
#pragma pack(pop)

#define DIR_IMPORT 1
#define DIR_BASERELOC 5
#define DIR_TLS 9
#define DIR_DELAY 13

static uint8_t *g_base;   // where we mapped the image
static uint64_t g_pref;   // preferred image base from the header

// loaded guest module registry (genuine MS CRT DLLs, etc.)
typedef struct { const char *name; uint8_t *base; } Mod;
static Mod g_mods[16]; static int g_nmods;

// filesystem-shim foundation (declared early; used by handle-based ops)
#include <dirent.h>
#define MAXFD 8192
static char g_drivec[1024];        // abs path of the Wine prefix's drive_c
static char *g_fdpath[MAXFD];       // boxed-fd -> real unix path
static DIR  *g_fddir[MAXFD];        // boxed-fd -> open dir stream (lazy)
static int   g_fddel[MAXFD];        // delete-on-close flag
static char* win2unix(const uint16_t*, char*, int);  // fwd
static void  unix2win(const char*, uint16_t*, int);   // fwd
static void  fd_record(int, const char*);             // fwd
static void  mkparents(const char*);                  // fwd

// import name table, parallel arrays, indexed by stub
static const char *imp_dll[8192];
static const char *imp_fn [8192];
static int imp_count;

// common stub handler: log first call, return 0
static uint64_t g_called[8192/64];
uint64_t __attribute__((ms_abi)) on_stub(uint32_t idx){
    if(idx < (uint32_t)imp_count){
        if(!((g_called[idx>>6]>>(idx&63))&1)){
            g_called[idx>>6] |= (uint64_t)1<<(idx&63);
            ws("  [stub] "); ws(imp_dll[idx]); ws("!"); ws(imp_fn[idx]); ws("\n");
        }
    }
    return 0;
}

// build a Win64 stub that calls on_stub(idx) and returns its result.
// runtime-safe: pool is RX, so flip RW around the write then back to RX
// (the RW<->RX cycle is proven to work under Rosetta).
static uint8_t *g_stubpool; static size_t g_stuboff; static int g_pool_exec;
static void *make_stub(uint32_t idx){
    if(g_pool_exec) mprotect(g_stubpool, 32*8192, PROT_READ|PROT_WRITE);
    uint8_t *p = g_stubpool + g_stuboff; g_stuboff += 32;
    int o=0;
    p[o++]=0x48; p[o++]=0x83; p[o++]=0xEC; p[o++]=0x28;            // sub rsp,0x28
    p[o++]=0xB9; *(uint32_t*)(p+o)=idx; o+=4;                       // mov ecx, idx
    p[o++]=0x48; p[o++]=0xB8; *(uint64_t*)(p+o)=(uint64_t)&on_stub; o+=8; // mov rax, &on_stub
    p[o++]=0xFF; p[o++]=0xD0;                                       // call rax
    p[o++]=0x48; p[o++]=0x83; p[o++]=0xC4; p[o++]=0x28;            // add rsp,0x28
    p[o++]=0xC3;                                                    // ret
    if(g_pool_exec) mprotect(g_stubpool, 32*8192, PROT_READ|PROT_EXEC);
    return p;
}

static inline uint8_t* rva(uint32_t r){ return g_base + r; }

// ---- full-call tracer (PELOAD_TRACE_ALL): a trampoline that logs the import
// name, then tail-jumps to the real target with args intact. ----
static void *imp_real[8192];
void __attribute__((ms_abi)) on_trace(uint32_t idx){ if(idx<(uint32_t)imp_count){ ws("  >> "); ws(imp_dll[idx]); ws("!"); ws(imp_fn[idx]); ws("\n"); } }
static uint8_t *g_trpool; static size_t g_troff; static int g_trexec;
static void *make_trace_stub(uint32_t idx, void *real){
    if(g_trexec) mprotect(g_trpool, 96*8192, PROT_READ|PROT_WRITE);
    uint8_t *p=g_trpool+g_troff; g_troff+=96; int o=0;
    // frame: sub 0x48 keeps 16-align; [rsp..+0x20) = on_trace's shadow,
    // saves live at +0x28..+0x40 (safely below the shadow region).
    p[o++]=0x48;p[o++]=0x83;p[o++]=0xEC;p[o++]=0x48;                       // sub rsp,0x48
    p[o++]=0x48;p[o++]=0x89;p[o++]=0x4C;p[o++]=0x24;p[o++]=0x28;           // mov [rsp+0x28],rcx
    p[o++]=0x48;p[o++]=0x89;p[o++]=0x54;p[o++]=0x24;p[o++]=0x30;           // mov [rsp+0x30],rdx
    p[o++]=0x4C;p[o++]=0x89;p[o++]=0x44;p[o++]=0x24;p[o++]=0x38;           // mov [rsp+0x38],r8
    p[o++]=0x4C;p[o++]=0x89;p[o++]=0x4C;p[o++]=0x24;p[o++]=0x40;           // mov [rsp+0x40],r9
    p[o++]=0xB9; *(uint32_t*)(p+o)=idx; o+=4;                              // mov ecx,idx
    p[o++]=0x48;p[o++]=0xB8; *(uint64_t*)(p+o)=(uint64_t)&on_trace; o+=8;  // mov rax,&on_trace
    p[o++]=0xFF;p[o++]=0xD0;                                               // call rax
    p[o++]=0x48;p[o++]=0x8B;p[o++]=0x4C;p[o++]=0x24;p[o++]=0x28;           // mov rcx,[rsp+0x28]
    p[o++]=0x48;p[o++]=0x8B;p[o++]=0x54;p[o++]=0x24;p[o++]=0x30;           // mov rdx,[rsp+0x30]
    p[o++]=0x4C;p[o++]=0x8B;p[o++]=0x44;p[o++]=0x24;p[o++]=0x38;           // mov r8,[rsp+0x38]
    p[o++]=0x4C;p[o++]=0x8B;p[o++]=0x4C;p[o++]=0x24;p[o++]=0x40;           // mov r9,[rsp+0x40]
    p[o++]=0x48;p[o++]=0x83;p[o++]=0xC4;p[o++]=0x48;                       // add rsp,0x48
    p[o++]=0x48;p[o++]=0xB8; *(uint64_t*)(p+o)=(uint64_t)real; o+=8;       // mov rax,real
    p[o++]=0xFF;p[o++]=0xE0;                                               // jmp rax
    if(g_trexec) mprotect(g_trpool, 96*8192, PROT_READ|PROT_EXEC);
    return p;
}

// ---------------------------------------------------------------------------
// Real implementations of the Win64 bootstrap surface. Enough for the C
// runtime + Rust std startup to get past argv/environ/_initterm. Everything
// is ms_abi. Unimplemented names still fall back to logging stubs.
// ---------------------------------------------------------------------------
static int      g_argc;          // guest argc
static char   **g_argv;          // guest argv (NULL-terminated)
static char   **g_environ;       // guest environ (NULL-terminated)
static uint16_t g_cmdlineW[2048]; // guest command line (UTF-16)

#define MSABI __attribute__((ms_abi))
static void   MSABI i_GetSystemTimeAsFileTime(uint64_t *ft){
    struct timeval tv; gettimeofday(&tv,0);
    // 100ns ticks since 1601-01-01; 11644473600s between 1601 and 1970
    if(ft) *ft = ((uint64_t)tv.tv_sec + 11644473600ULL)*10000000ULL + (uint64_t)tv.tv_usec*10;
}
static int    MSABI i_QueryPerformanceCounter(uint64_t *c){ if(c) *c=mach_absolute_time(); return 1; }
static int    MSABI i_QueryPerformanceFrequency(uint64_t *f){ if(f) *f=1000000000ULL; return 1; }
static uint32_t MSABI i_GetCurrentThreadId(void){ return 0x1000; } // single-threaded startup
static uint32_t MSABI i_GetCurrentProcessId(void){ return (uint32_t)getpid(); }
static void   MSABI i_InitializeSListHead(void *h){ if(h) memset(h,0,16); }
#include <errno.h>
static uint32_t g_lasterr = 0;
static uint32_t win_err(void){ switch(errno){ case ENOENT: return 2; /*FILE_NOT_FOUND*/
    case EACCES: case EPERM: return 5; /*ACCESS_DENIED*/ case EEXIST: return 80; /*FILE_EXISTS*/
    case ENOTDIR: return 3; /*PATH_NOT_FOUND*/ default: return 1; } }
static uint32_t MSABI i_GetLastError(void){ return g_lasterr; }
static void   MSABI i_SetLastError(uint32_t e){ g_lasterr=e; }

// CRT argv/environ accessors
static char*** MSABI i_p_argv(void){ static char **a; a=g_argv; return &a; }
static int*    MSABI i_p_argc(void){ static int c; c=g_argc; return &c; }
static char**  MSABI i_get_initial_narrow_environment(void){ return g_environ; }
static char*** MSABI i_p_environ(void){ static char **e; e=g_environ; return &e; }
static int     MSABI i_configure_narrow_argv(int m){ (void)m; return 0; }
static int     MSABI i_initialize_narrow_environment(void){ return 0; }
static int     MSABI i_initialize_onexit_table(void *t){ (void)t; return 0; }
static int     MSABI i_register_onexit_function(void *t, void *f){ (void)t;(void)f; return 0; }
static int     MSABI i_crt_atexit(void *f){ (void)f; return 0; }
static void    MSABI i_set_app_type(int t){ (void)t; }
static int     MSABI i_seh_filter_exe(uint32_t c, void *p){ (void)c;(void)p; return 0; } // EXCEPTION_CONTINUE_SEARCH

// stdio/locale globals: these __p__* accessors MUST return real pointers
// (the CRT writes through them). NULL stubs segfault here.
static int g_commode, g_fmode, g_dowildcard;
static int*  MSABI i_p_commode(void){ return &g_commode; }
static int*  MSABI i_p_fmode(void){ return &g_fmode; }
static int   MSABI i_set_fmode(int m){ g_fmode=m; return 0; }
static int   MSABI i_get_fmode(int *p){ if(p)*p=g_fmode; return 0; }
static int*  MSABI i_p_dowildcard(void){ return &g_dowildcard; }
static int   MSABI i_configthreadlocale(int m){ (void)m; return 0; }
static void  MSABI i_cexit(void){ }

// _initterm: call each non-null fn ptr in [start,end). This is what actually
// runs C++/Rust global constructors — the reason a NULL stub was not enough.
static void MSABI i_initterm(void (MSABI **start)(void), void (MSABI **end)(void)){
    for(; start<end; start++) if(*start) (*start)();
}
static int MSABI i_initterm_e(int (MSABI **start)(void), int (MSABI **end)(void)){
    for(; start<end; start++) if(*start){ int r=(*start)(); if(r) return r; }
    return 0;
}

// Win32 heap backed by malloc (handle value is arbitrary but non-null)
static void*  MSABI i_GetProcessHeap(void){ return (void*)0x48454150; /* 'HEAP' */ }
static void*  MSABI i_HeapAlloc(void *h, uint32_t fl, uint64_t n){ (void)h; void*p=malloc(n?n:1); if(p&&(fl&8)) memset(p,0,n); return p; }
static void*  MSABI i_HeapReAlloc(void *h, uint32_t fl, void *p, uint64_t n){ (void)h;(void)fl; return realloc(p,n?n:1); }
static int    MSABI i_HeapFree(void *h, uint32_t fl, void *p){ (void)h;(void)fl; free(p); return 1; }
static uint64_t MSABI i_HeapSize(void *h, uint32_t fl, void *p){ (void)h;(void)fl;(void)p; return 0; }
static void*  MSABI i_HeapCreate(uint32_t o, uint64_t a, uint64_t b){ (void)o;(void)a;(void)b; return (void*)0x48454150; }

// Virtual memory backed by mmap
static void*  MSABI i_VirtualAlloc(void *addr, uint64_t sz, uint32_t type, uint32_t prot){
    (void)type;(void)prot; void *p=mmap(addr, sz?sz:4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1,0);
    return p==MAP_FAILED?0:p;
}
static int    MSABI i_VirtualProtect(void *a, uint64_t n, uint32_t np, uint32_t *op){ if(op)*op=0x40; int pr=PROT_READ; if(np&0xf0)pr|=PROT_EXEC; if(np&0xcc)pr|=PROT_WRITE; mprotect(a,n,pr); return 1; }
static int    MSABI i_VirtualFree(void *a, uint64_t n, uint32_t t){ (void)t; munmap(a,n?n:4096); return 1; }

static uint16_t* MSABI i_GetCommandLineW(void){ return g_cmdlineW; }
static void*  MSABI i_GetModuleHandleW(uint16_t *n){ (void)n; return g_base; } // good enough: image base
static void*  MSABI i_GetModuleHandleA(char *n){ (void)n; return g_base; }

// TLS: a tiny fixed table (Rust/CRT use a few slots early)
static void  *g_tls[256]; static uint32_t g_tlsnext=1;
static uint32_t MSABI i_TlsAlloc(void){ return g_tlsnext<256? g_tlsnext++ : 0xFFFFFFFF; }
static void*  MSABI i_TlsGetValue(uint32_t i){ return i<256? g_tls[i]:0; }
static int    MSABI i_TlsSetValue(uint32_t i, void *v){ if(i<256){g_tls[i]=v; return 1;} return 0; }
static int    MSABI i_TlsFree(uint32_t i){ (void)i; return 1; }

// ---- expanded kernel32/core surface that genuine ucrtbase needs ----
// console/file I/O: pseudo-handles 1/2/3 == stdin/stdout/stderr
static void*  MSABI i_GetStdHandle(uint32_t id){ return (void*)(intptr_t)(id==(uint32_t)-10?1: id==(uint32_t)-11?2: id==(uint32_t)-12?3: 0); }
static int    MSABI i_WriteFile(void *h, const void *buf, uint32_t n, uint32_t *wr, void *ov){
    (void)ov; intptr_t hv=(intptr_t)h; int fd = (hv==2)?1 : (hv==3)?2 : (hv>=0x1000)?(int)(hv-0x1000) : -1;
    long k = fd>=0? write(fd,buf,n) : 0; if(wr)*wr=(uint32_t)(k<0?0:k); return fd>=0; }
static int    MSABI i_WriteConsoleW(void *h, const uint16_t *s, uint32_t n, uint32_t *wr, void *rsv){
    (void)rsv; int fd=(h==(void*)3)?2:1; for(uint32_t i=0;i<n;i++){ char c=(s[i]<128)?(char)s[i]:'?'; (void)!write(fd,&c,1);} if(wr)*wr=n; return 1; }
static uint32_t MSABI i_GetFileType(void *h){ intptr_t hv=(intptr_t)h; return hv>=0x1000? 1 : 2; } // DISK : CHAR
static int    MSABI i_FlushFileBuffers(void *h){ (void)h; return 1; }
static void*  MSABI i_GetCurrentProcess(void){ return (void*)-1; }
static void*  MSABI i_GetCurrentThread(void){ return (void*)-2; }
// pointer obfuscation: identity is a valid implementation
static void*  MSABI i_EncodePointer(void *p){ return p; }
static void*  MSABI i_DecodePointer(void *p){ return p; }
// critical sections / SRW / condvar / init-once: no-ops are correct single-threaded
static void   MSABI i_noop(void){ }
static int    MSABI i_ret1(void){ return 1; }
static void   MSABI i_InitializeCriticalSectionEx(void *c,uint32_t s,uint32_t f){ (void)c;(void)s;(void)f; }
static int    MSABI i_InitOnceBeginInitialize(void *o,uint32_t f,int *pend,void**ctx){ (void)o;(void)f; if(pend)*pend=1; if(ctx)*ctx=0; return 1; }
static int    MSABI i_InitOnceComplete(void *o,uint32_t f,void*ctx){ (void)o;(void)f;(void)ctx; return 1; }
// FLS mirrors TLS
static uint32_t MSABI i_FlsAlloc(void *cb){ (void)cb; return i_TlsAlloc(); }
static void*  MSABI i_FlsGetValue(uint32_t i){ return i_TlsGetValue(i); }
static int    MSABI i_FlsSetValue(uint32_t i,void*v){ return i_TlsSetValue(i,v); }
static int    MSABI i_FlsFree(uint32_t i){ return i_TlsFree(i); }
// misc sysinfo
static int    MSABI i_IsProcessorFeaturePresent(uint32_t f){ (void)f; return 1; }
static int    MSABI i_IsDebuggerPresent(void){ return 0; }
static uint32_t MSABI i_GetACP(void){ return 65001; }
static int    MSABI i_GetModuleHandleExW(uint32_t fl, void *name, void **out){ (void)fl;(void)name; if(out)*out=g_base; return 1; }
static int    MSABI i_MultiByteToWideChar(uint32_t cp,uint32_t fl,const char*s,int sl,uint16_t*w,int wl){
    (void)cp;(void)fl; int n= sl<0? (int)strlen(s)+1 : sl; if(!w||wl==0) return n; int i=0; for(;i<n&&i<wl;i++) w[i]=(uint8_t)s[i]; return i; }
static int    MSABI i_WideCharToMultiByte(uint32_t cp,uint32_t fl,const uint16_t*w,int wl,char*s,int sl,const char*df,int*du){
    (void)cp;(void)fl;(void)df;(void)du; int n= wl<0? 0:wl; if(wl<0){ while(w[n]) n++; n++; } if(!s||sl==0) return n; int i=0; for(;i<n&&i<sl;i++) s[i]=(char)(w[i]<128?w[i]:'?'); return i; }
static void   MSABI i_GetSystemInfo(void *si){ if(si){ memset(si,0,48); *(uint32_t*)((char*)si+4)=4096; *(uint32_t*)((char*)si+32)=1; *(uint32_t*)((char*)si+36)=4096; } }
static void   MSABI i_ExitProcess(uint32_t c){ _exit((int)c); }
static void   MSABI i_TerminateProcess(void *h,uint32_t c){ (void)h; _exit((int)c); }
static uint32_t MSABI i_GetModuleFileNameW(void*m,uint16_t*buf,uint32_t sz){ (void)m; uint32_t i=0; for(; i<sz && g_cmdlineW[i]; i++) buf[i]=g_cmdlineW[i]; if(i<sz) buf[i]=0; return i; }
static void   MSABI i_GetStartupInfoW(void *si){ if(si) memset(si,0,104); }
// RtlPcToFileHeader: return the base of the loaded image containing Pc.
static void*  MSABI i_RtlPcToFileHeader(void *pc, void **base){
    uint64_t p=(uint64_t)pc;
    // check exe
    { DOS*d=(DOS*)g_base; OPT64*o=(OPT64*)((uint8_t*)g_base+d->e_lfanew+sizeof(COFF));
      if(p>=(uint64_t)g_base && p<(uint64_t)g_base+o->ImageSize){ if(base)*base=g_base; return g_base; } }
    for(int m=0;m<g_nmods;m++){ DOS*d=(DOS*)g_mods[m].base; OPT64*o=(OPT64*)(g_mods[m].base+d->e_lfanew+sizeof(COFF));
      if(p>=(uint64_t)g_mods[m].base && p<(uint64_t)g_mods[m].base+o->ImageSize){ if(base)*base=g_mods[m].base; return g_mods[m].base; } }
    if(base)*base=0; return 0;
}
static char   g_cmdlineA[2048];
static char*  MSABI i_GetCommandLineA(void){ return g_cmdlineA; }
static uint16_t g_envblock[2] = {0,0};
static uint16_t* MSABI i_GetEnvironmentStringsW(void){ return g_envblock; }
static int    MSABI i_FreeEnvironmentStringsW(void*p){ (void)p; return 1; }
static uint32_t MSABI i_GetTickCount(void){ return (uint32_t)(mach_absolute_time()/1000000ULL); }
// NtWriteFile: route writes to our stdin/out/err pseudo-handles (1/2/3)
static int MSABI i_NtWriteFile(void*h,void*ev,void*apc,void*apcctx,void*iosb,const void*buf,uint32_t len,void*off,void*key){
    (void)ev;(void)apc;(void)apcctx;(void)key; intptr_t hv=(intptr_t)h;
    int fd = (hv==2)?1 : (hv==3)?2 : (hv>=0x1000)?(int)(hv-0x1000) : -1;
    int64_t o = off? *(int64_t*)off : -1; long k;
    if(fd>=0 && hv>=0x1000 && o>=0) k=pwrite(fd,buf,len,(off_t)o);
    else k = fd>=0? write(fd,buf,len) : len;
    if(iosb){ ((uint64_t*)iosb)[0]=0; ((uint64_t*)iosb)[1]=(uint64_t)(k<0?0:k); }
    return k<0 ? (int)0xC0000008 : 0; // STATUS_INVALID_HANDLE : SUCCESS
}
static int MSABI i_NtReadFile(void*h,void*ev,void*apc,void*apcctx,void*iosb,void*buf,uint32_t len,int64_t*off,void*key){
    (void)ev;(void)apc;(void)apcctx;(void)key; intptr_t hv=(intptr_t)h;
    long k=-1; if(hv>=0x1000){ int fd=(int)(hv-0x1000); k = (off&&*off>=0)? pread(fd,buf,len,*off) : read(fd,buf,len); }
    if(iosb){ ((uint64_t*)iosb)[0]= (k<0)?0xC0000008ULL:0; ((uint64_t*)iosb)[1]=(uint64_t)(k<0?0:k); }
    if(k==0) return (int)0xC0000011; // STATUS_END_OF_FILE
    return k<0 ? (int)0xC0000008 : 0;  // STATUS_INVALID_HANDLE : SUCCESS
}
static uint32_t MSABI i_GetConsoleMode(void*h,uint32_t*m){ (void)h; if(m)*m=0; return 1; }
static uint32_t MSABI i_SetConsoleMode(void*h,uint32_t m){ (void)h;(void)m; return 1; }
static int MSABI i_SetThreadStackGuarantee(uint32_t*s){ (void)s; return 1; }
static int MSABI i_FreeLibrary(void*h){ (void)h; return 1; }
static uint32_t MSABI i_GetModuleFileNameA(void*m,char*buf,uint32_t sz){ (void)m; uint32_t i=0; for(; i<sz && g_cmdlineA[i]; i++) buf[i]=g_cmdlineA[i]; if(i<sz) buf[i]=0; return i; }
static uint32_t MSABI i_GetConsoleOutputCP(void){ return 65001; }
// drive/volume queries: present C: as a fixed NTFS volume with free space, so
// the client accepts the cache/scratch as "a drive of this PC".
static uint32_t MSABI i_GetDriveTypeW(const uint16_t*root){ (void)root; return 3; } // DRIVE_FIXED
static uint32_t MSABI i_GetDriveTypeA(const char*root){ (void)root; return 3; }
static int MSABI i_GetVolumeInformationW(const uint16_t*root,uint16_t*vn,uint32_t vnsz,uint32_t*serial,uint32_t*maxcomp,uint32_t*flags,uint16_t*fsn,uint32_t fsnsz){
    (void)root; if(vn&&vnsz){vn[0]='O';vn[1]='S';vn[2]=0;} if(serial)*serial=0x1337c0de; if(maxcomp)*maxcomp=255; if(flags)*flags=0x03;
    if(fsn&&fsnsz){ const char*f="NTFS"; uint32_t i=0; for(; f[i]&&i<fsnsz-1;i++) fsn[i]=(uint8_t)f[i]; fsn[i]=0; } return 1; }
static int MSABI i_GetDiskFreeSpaceExW(const uint16_t*dir,uint64_t*avail,uint64_t*total,uint64_t*tfree){
    (void)dir; uint64_t big=(uint64_t)200*1024*1024*1024; if(avail)*avail=big; if(total)*total=big*2; if(tfree)*tfree=big; return 1; }
static int MSABI i_GetVolumePathNameW(const uint16_t*fn,uint16_t*buf,uint32_t len){ (void)fn; if(len>=4&&buf){buf[0]='C';buf[1]=':';buf[2]='\\';buf[3]=0;} return 1; }
static uint32_t MSABI i_RtlNtStatusToDosError(uint32_t s){ switch(s){ case 0:return 0;
    case 0xC0000034: case 0xC000000F: return 2; case 0xC0000022: return 5;
    case 0xC0000011: return 38; case 0x80000006: return 18; case 0xC00000BA: return 267;
    case 0xC0000035: return 183; default: return (s&0xC0000000)? 1 : (s&0xffff); } }
static uint32_t MSABI i_FormatMessageW(uint32_t fl,const void*src,uint32_t id,uint32_t lang,uint16_t*buf,uint32_t sz,void*args){
    (void)src;(void)lang;(void)args; char tmp[64]; int n=0; const char*pfx="System error "; for(int i=0;pfx[i];i++) tmp[n++]=pfx[i];
    uint32_t v=id; char d[12]; int dn=0; if(!v) d[dn++]='0'; while(v){ d[dn++]='0'+v%10; v/=10; } while(dn) tmp[n++]=d[--dn]; tmp[n]=0;
    if(fl&0x100){ uint16_t*mb=malloc((n+1)*2); for(int i=0;i<=n;i++) mb[i]=(uint8_t)tmp[i]; *(void**)buf=mb; return n; } // ALLOCATE_BUFFER
    uint32_t i=0; for(; i<(uint32_t)n && i<sz-1; i++) buf[i]=(uint8_t)tmp[i]; buf[i]=0; return i; }
static int   MSABI i_SetConsoleCtrlHandler(void*h,int add){ (void)h;(void)add; return 1; }
static void* MSABI i_CreateSemaphoreA(void*sec,long init,long max,const char*nm){ (void)sec;(void)init;(void)max;(void)nm; return (void*)0x5e4a; }
static void* MSABI i_CreateSemaphoreW(void*sec,long init,long max,const uint16_t*nm){ (void)sec;(void)init;(void)max;(void)nm; return (void*)0x5e4a; }
static int   MSABI i_ReleaseSemaphore(void*h,long c,long*prev){ (void)h;(void)c;(void)prev; return 1; }
static void* MSABI i_CreateEventW(void*sec,int man,int sig,const uint16_t*nm){ (void)sec;(void)man;(void)sig;(void)nm; return (void*)0x5e40; }
static int   MSABI i_SetEvent(void*h){ (void)h; return 1; }
static int   MSABI i_ResetEvent(void*h){ (void)h; return 1; }
static uint32_t MSABI i_WaitForSingleObject(void*h,uint32_t ms){ (void)h;(void)ms; return 0; } // WAIT_OBJECT_0
// ---- threads: back Win32 CreateThread with pthreads; each gets its own TEB
// (per-thread %gs) + a fresh copy of the static TLS block. ----
#include <pthread.h>
static uint8_t *g_peb_ptr; static uint8_t *g_tls_src; static uint64_t g_tls_raw, g_tls_total;
typedef uint32_t (MSABI *WinThreadProc)(void*);
typedef struct { WinThreadProc fn; void *param; } TArg;
static void *thread_tramp(void *a){
    TArg *t=a;
    uint8_t *teb=calloc(1,0x2000);
    void **tarr=calloc(128,sizeof(void*));
    if(g_tls_total){ uint8_t *blk=malloc(g_tls_total); if(g_tls_raw) memcpy(blk,g_tls_src,g_tls_raw); if(g_tls_total>g_tls_raw) memset(blk+g_tls_raw,0,g_tls_total-g_tls_raw); tarr[0]=blk; }
    *(uint64_t*)(teb+0x30)=(uint64_t)teb; *(uint64_t*)(teb+0x60)=(uint64_t)g_peb_ptr; *(uint64_t*)(teb+0x58)=(uint64_t)tarr;
    __asm__ volatile("movq %0,%%gs:0x30"::"r"((uint64_t)teb));
    __asm__ volatile("movq %0,%%gs:0x60"::"r"((uint64_t)g_peb_ptr));
    __asm__ volatile("movq %0,%%gs:0x58"::"r"((uint64_t)tarr));
    uint32_t r = t->fn ? t->fn(t->param) : 0; free(t); return (void*)(uintptr_t)r;
}
static void* MSABI i_CreateThread(void*attr,uint64_t stack,WinThreadProc start,void*param,uint32_t fl,uint32_t*tid){
    (void)attr;(void)stack;(void)fl;
    if(getenv("PELOAD_NO_THREADS")){ if(tid)*tid=0x7333; return (void*)0x7333; } // diagnostic: don't spawn
    TArg *t=malloc(sizeof *t); t->fn=start; t->param=param;
    pthread_t th; if(pthread_create(&th,0,thread_tramp,t)){ free(t); return 0; }
    if(tid)*tid=(uint32_t)(uintptr_t)th; return (void*)th;
}
static uint32_t MSABI i_GetThreadId(void*h){ return (uint32_t)(uintptr_t)h; }
static int MSABI i_GetExitCodeThread(void*h,uint32_t*code){ (void)h; if(code)*code=259; return 1; } // STILL_ACTIVE
static uint32_t MSABI i_WaitForMultipleObjects(uint32_t n,void*h,int all,uint32_t ms){ (void)n;(void)h;(void)all;(void)ms; return 0; }
static void* MSABI i_OpenProcess(uint32_t a,int inh,uint32_t pid){ (void)a;(void)inh;(void)pid; return (void*)-1; }
static int MSABI i_SwitchToThread(void){ sched_yield(); return 1; }
// CompareStringOrdinal: UTF-16 ordinal compare -> 1/2/3 (LESS/EQUAL/GREATER)
static uint32_t MSABI i_GetTempPathW(uint32_t n,uint16_t*buf){ const char*t="C:\\ReclaimerCache\\Temp\\"; uint32_t i=0; for(; t[i]&&i<n-1;i++) buf[i]=(uint8_t)t[i]; buf[i]=0; return i; }
static uint32_t MSABI i_GetTempPath2W(uint32_t n,uint16_t*buf){ return i_GetTempPathW(n,buf); }
// GetFileInformationByHandleEx: FileBasic(0)/FileStandard(1) from fstat, and
// directory enumeration (FileIdBothDirInfo=10/restart=11, FileFullDirInfo=14/15)
// one entry per call (NextEntryOffset=0), real readdir.
static int MSABI i_GetFileInformationByHandleEx(void*h,int cls,void*info,uint32_t sz){
    intptr_t hv=(intptr_t)h; if(hv<0x1000||!info){ if(info)memset(info,0,sz); return 1; }
    int fd=(int)(hv-0x1000); struct stat st;
    if(getenv("PELOAD_TRACE_FS")){ char t[48]; snprintf(t,sizeof t,"  [GFIBHEx] cls=%d fd=%d\n",cls,fd); ws(t); }
    if(cls==0){ memset(info,0,40); if(!fstat(fd,&st)) *(uint32_t*)((char*)info+32)=S_ISDIR(st.st_mode)?0x10:0x80; return 1; }
    if(cls==1){ memset(info,0,24); if(!fstat(fd,&st)){ *(int64_t*)info=st.st_size; *(int64_t*)((char*)info+8)=st.st_size; *(uint32_t*)((char*)info+16)=1; *((char*)info+21)=S_ISDIR(st.st_mode)?1:0; } return 1; }
    if(cls==9){ memset(info,0,8); if(!fstat(fd,&st)) *(uint32_t*)info=S_ISDIR(st.st_mode)?0x10:0x80; /*ReparseTag*/*(uint32_t*)((char*)info+4)=0; return 1; } // FileAttributeTagInfo: not a reparse point
    if(cls==18){ memset(info,0,24); if(!fstat(fd,&st)){ *(uint64_t*)info=(uint64_t)st.st_dev; *(uint64_t*)((char*)info+8)=(uint64_t)st.st_ino; } return 1; } // FileIdInfo
    if(cls==10||cls==11||cls==14||cls==15){
        if(fd>=MAXFD){ g_lasterr=18; return 0; }
        if(cls==11||cls==15){ if(g_fddir[fd]){ rewinddir(g_fddir[fd]); } }
        if(!g_fddir[fd]){ int dd=dup(fd); g_fddir[fd]= dd>=0? fdopendir(dd):0; }
        DIR *d=g_fddir[fd]; if(!d){ g_lasterr=18; return 0; }
        struct dirent *de; do{ de=readdir(d); }while(de && (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")));
        if(!de){ g_lasterr=18; return 0; } // ERROR_NO_MORE_FILES
        char full[2300]; snprintf(full,sizeof full,"%s/%s", g_fdpath[fd]?g_fdpath[fd]:".", de->d_name);
        struct stat es; int have=stat(full,&es)==0;
        uint8_t *b=info; if(sz>700) sz=700; memset(b,0,sz);
        int nameoff=(cls==10||cls==11)?104:68;
        *(uint32_t*)(b+56) = (have&&S_ISDIR(es.st_mode))?0x10:0x80;      // FileAttributes
        int nl=0; while(de->d_name[nl]) nl++;
        *(uint32_t*)(b+60) = (uint32_t)(nl*2);                           // FileNameLength (bytes)
        if(have){ *(int64_t*)(b+40)=es.st_size; *(int64_t*)(b+48)=es.st_size; if(cls==10||cls==11) *(int64_t*)(b+96)=(int64_t)es.st_ino; }
        uint16_t *nm=(uint16_t*)(b+nameoff);
        for(int i=0;i<nl && nameoff+2*(i+1)<=(int)sz;i++) nm[i]=(uint8_t)de->d_name[i];
        return 1; // NextEntryOffset already 0
    }
    memset(info,0,sz); return 1;
}
static uint32_t MSABI i_GetFinalPathNameByHandleW(void*h,uint16_t*buf,uint32_t cch,uint32_t fl){
    (void)fl; intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; int fd=(int)(hv-0x1000);
    if(getenv("PELOAD_TRACE_FS")){ ws("  [GetFinalPathNameByHandleW] "); ws((fd<MAXFD&&g_fdpath[fd])?g_fdpath[fd]:"(none)"); ws("\n"); }
    if(fd>=MAXFD||!g_fdpath[fd]) return 0; uint16_t tmp[2048]; unix2win(g_fdpath[fd],tmp,2048);
    uint32_t n=0; while(tmp[n]) n++; if(cch<n+1) return n+1; for(uint32_t i=0;i<=n;i++) buf[i]=tmp[i]; return n;
}
// recursive directory removal (RemoveDirectoryW only needs empty, but Rust may
// hand us a populated one; nuke it for robustness)
static void rmrf(const char *p){ struct stat st; if(lstat(p,&st)) return;
    if(S_ISDIR(st.st_mode)){ DIR*d=opendir(p); if(d){ struct dirent*e; while((e=readdir(d))){ if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue; char c[2300]; snprintf(c,sizeof c,"%s/%s",p,e->d_name); rmrf(c);} closedir(d);} rmdir(p);} else unlink(p); }
static int MSABI i_RemoveDirectoryW(const uint16_t*w){ char p[2048]; win2unix(w,p,sizeof p); if(rmdir(p)==0) return 1; rmrf(p); return 1; }
// SetFileInformationByHandle: honor FileDispositionInfo(4)/Ex(21) = delete
static int MSABI i_SetFileInformationByHandle(void*h,int cls,void*info,uint32_t sz){
    (void)sz; intptr_t hv=(intptr_t)h; if(hv<0x1000) return 1; int fd=(int)(hv-0x1000);
    if((cls==4||cls==21) && info && *(uint8_t*)info && fd<MAXFD) g_fddel[fd]=1; // DeleteFile flag set
    return 1;
}
// NtCreateFile: supports NT absolute names (\??\C:\..) and names relative to an
// OBJECT_ATTRIBUTES.RootDirectory handle — the path Rust remove_dir_all uses.
static int nt_open_common(void **ph,uint32_t acc,void *oa,void *iosb,uint32_t disp,uint32_t opts){
    uint8_t *OA=oa; void *root = OA? *(void**)(OA+8) : 0; uint8_t *uni = OA? *(uint8_t**)(OA+16) : 0;
    char name[1600]={0}; if(uni){ uint16_t len=*(uint16_t*)uni; uint16_t *bufw=*(uint16_t**)(uni+8); int n=len/2,j=0; for(int i=0;i<n&&j<1599;i++){ char c=(char)(bufw[i]<128?bufw[i]:'?'); name[j++]=c;} name[j]=0; }
    for(char*q=name;*q;q++) if(*q=='\\') *q='/';
    char path[2200];
    if(root){ intptr_t rv=(intptr_t)root; int rfd=(int)(rv-0x1000); const char *base=(rv>=0x1000&&rfd<MAXFD&&g_fdpath[rfd])?g_fdpath[rfd]:"."; const char*nm=name; if(nm[0]=='/')nm++; snprintf(path,sizeof path,"%s/%s",base,nm); }
    else { const char*s=name; if(!strncmp(s,"/??/",4)) s+=4; else if(!strncmp(s,"/DosDevices/",12)) s+=12;
           uint16_t tmp[2048]; int i=0; for(; s[i]&&i<2047;i++) tmp[i]=(uint8_t)(s[i]=='/'?'\\':s[i]); tmp[i]=0; win2unix(tmp,path,sizeof path); }
    int flags = (acc & 0x40000000)? O_RDWR : O_RDONLY;
    switch(disp){ case 0: case 5: flags|=O_CREAT|O_TRUNC; break; case 2: flags|=O_CREAT|O_EXCL; break; case 3: flags|=O_CREAT; break; case 4: flags|=O_TRUNC; break; default: break; }
    struct stat ds; if((opts&0x1)||(stat(path,&ds)==0&&S_ISDIR(ds.st_mode))) flags=O_RDONLY; // FILE_DIRECTORY_FILE
    int fd=open(path,flags,0644);
    if(fd<0 && (flags&O_CREAT) && errno==ENOENT){ mkparents(path); fd=open(path,flags,0644); }
    if(getenv("PELOAD_TRACE_FS")){ char dd[64]; snprintf(dd,sizeof dd," disp=%u opts=0x%x fl=0x%x errno=%d",disp,opts,flags,fd<0?errno:0); ws("  [NtCreateFile] "); ws(path); ws(dd); ws(fd<0?" FAIL\n":" ok\n"); }
    if(fd<0){ if(iosb)((uint64_t*)iosb)[0]=0xC0000034; return (int)0xC0000034; } // OBJECT_NAME_NOT_FOUND
    fd_record(fd,path); if(opts&0x1000) g_fddel[fd<MAXFD?fd:0]=1; // FILE_DELETE_ON_CLOSE
    if(ph)*ph=(void*)(intptr_t)(0x1000+fd); if(iosb){((uint64_t*)iosb)[0]=0; ((uint64_t*)iosb)[1]=(disp==2?2:1);} return 0;
}
static int MSABI i_NtCreateFile(void **ph,uint32_t acc,void *oa,void *iosb,void *alloc,uint32_t fattr,uint32_t share,uint32_t disp,uint32_t opts,void *ea,uint32_t eal){
    (void)alloc;(void)fattr;(void)share;(void)ea;(void)eal; return nt_open_common(ph,acc,oa,iosb,disp,opts);
}
static int MSABI i_NtOpenFile(void **ph,uint32_t acc,void *oa,void *iosb,uint32_t share,uint32_t opts){
    (void)share; return nt_open_common(ph,acc,oa,iosb,1 /*FILE_OPEN*/,opts);
}
static int MSABI i_CompareStringOrdinal(const uint16_t*a,int ca,const uint16_t*b,int cb,int ic){
    int la = ca<0? ({int n=0; while(a[n])n++; n;}) : ca;
    int lb = cb<0? ({int n=0; while(b[n])n++; n;}) : cb;
    for(int i=0;i<la&&i<lb;i++){ uint16_t x=a[i],y=b[i]; if(ic){ if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; }
        if(x<y) return 1; if(x>y) return 3; }
    if(la<lb) return 1; if(la>lb) return 3; return 2;
}
// OS randomness (Rust getrandom / community keypair): fill with arc4random
static long MSABI i_BCryptGenRandom(void*alg,uint8_t*buf,uint32_t n,uint32_t fl){ (void)alg;(void)fl; arc4random_buf(buf,n); return 0; }
static int  MSABI i_ProcessPrng(uint8_t*buf,uint64_t n){ arc4random_buf(buf,n); return 1; }
static int  MSABI i_RtlGenRandom(void*buf,uint32_t n){ arc4random_buf(buf,n); return 1; }
static char* win2unix(const uint16_t *w, char *out, int outsz); // fwd (defined in FS block)
// file locking: single process, so locks always succeed
static int  MSABI i_LockFileEx(void*h,uint32_t fl,uint32_t r,uint32_t lo,uint32_t hi,void*ov){ (void)h;(void)fl;(void)r;(void)lo;(void)hi;(void)ov; return 1; }
static int  MSABI i_UnlockFileEx(void*h,uint32_t r,uint32_t lo,uint32_t hi,void*ov){ (void)h;(void)r;(void)lo;(void)hi;(void)ov; return 1; }
static int  MSABI i_LockFile(void*h,uint32_t a,uint32_t b,uint32_t c,uint32_t d){ (void)h;(void)a;(void)b;(void)c;(void)d; return 1; }
static int  MSABI i_UnlockFile(void*h,uint32_t a,uint32_t b,uint32_t c,uint32_t d){ (void)h;(void)a;(void)b;(void)c;(void)d; return 1; }
static int  MSABI i_FlushViewOfFile(void*a,uint64_t b){ (void)a;(void)b; return 1; }
// SetEndOfFile / SetFileInformationByHandle: truncate to current offset
static int  MSABI i_SetEndOfFile(void*h){ intptr_t hv=(intptr_t)h; if(hv<0x1000) return 1; int fd=(int)(hv-0x1000); off_t cur=lseek(fd,0,SEEK_CUR); if(cur>=0) (void)!ftruncate(fd,cur); return 1; }
static int  MSABI i_MoveFileExW(const uint16_t*from,const uint16_t*to,uint32_t fl){ (void)fl; char a[2048],b[2048]; char A[2048],B[2048]; win2unix(from,A,sizeof A); if(!to) return unlink(A)==0; win2unix(to,B,sizeof B); (void)a;(void)b; return rename(A,B)==0; }
static int  MSABI i_DeleteFileW(const uint16_t*w){ char p[2048]; return unlink(win2unix(w,p,sizeof p))==0 || 1; }
static int  MSABI i_CopyFileExW(const uint16_t*src,const uint16_t*dst,void*prog,void*data,int*cancel,uint32_t fl){
    (void)prog;(void)data;(void)cancel;(void)fl; char s[2048],d[2048]; win2unix(src,s,sizeof s); win2unix(dst,d,sizeof d);
    int in=open(s,O_RDONLY); if(in<0){ g_lasterr=win_err(); return 0; }
    mkparents(d); int out=open(d,O_WRONLY|O_CREAT|O_TRUNC,0644); if(out<0){ close(in); g_lasterr=win_err(); return 0; }
    char buf[65536]; long n; int ok=1; while((n=read(in,buf,sizeof buf))>0){ if(write(out,buf,n)!=n){ ok=0; break; } } if(n<0) ok=0;
    close(in); close(out); if(!ok) g_lasterr=win_err(); return ok; }
static int  MSABI i_CopyFileW(const uint16_t*src,const uint16_t*dst,int failIfExists){ (void)failIfExists; return i_CopyFileExW(src,dst,0,0,0,0); }
static int  MSABI i_CreateHardLinkW(const uint16_t*lnk,const uint16_t*tgt,void*sec){ (void)sec; char l[2048],t[2048]; win2unix(lnk,l,sizeof l); win2unix(tgt,t,sizeof t); mkparents(l); if(link(t,l)==0) return 1; return i_CopyFileExW(tgt,lnk,0,0,0,0); }
static void* MSABI i_LocalAlloc(uint32_t fl,uint64_t n){ void*p=malloc(n?n:1); if(p&&(fl&0x40)) memset(p,0,n); return p; }
static void* MSABI i_LocalFree(void*p){ free(p); return 0; }
// DPAPI: no Windows user to bind to, so seal == copy (round-trips correctly,
// which is all the local community key needs). DATA_BLOB = {u32 cb; u8* p;}.
typedef struct { uint32_t cb; uint8_t *p; } DBLOB;
static int MSABI i_CryptProtectData(DBLOB*in,const uint16_t*d,DBLOB*ent,void*r,void*pr,uint32_t fl,DBLOB*out){
    (void)d;(void)ent;(void)r;(void)pr;(void)fl; if(!in||!out) return 0;
    out->p=malloc(in->cb?in->cb:1); out->cb=in->cb; if(in->cb) memcpy(out->p,in->p,in->cb); return 1; }
static int MSABI i_CryptUnprotectData(DBLOB*in,uint16_t**d,DBLOB*ent,void*r,void*pr,uint32_t fl,DBLOB*out){
    (void)d;(void)ent;(void)r;(void)pr;(void)fl; if(!in||!out) return 0;
    out->p=malloc(in->cb?in->cb:1); out->cb=in->cb; if(in->cb) memcpy(out->p,in->p,in->cb); return 1; }
static uint64_t MSABI i_GetTickCount64(void){ return mach_absolute_time()/1000000ULL; }
// LoadLibraryExW: log the requested module (UTF-16 -> ascii) so we can see
// what the engine wants. Returning a non-null handle keeps startup alive;
// actually mapping the dependency DLLs as guests is the next phase.
static char g_lastlib[260];
static void* MSABI i_LoadLibraryExW(const uint16_t *name, void *h, uint32_t fl){
    (void)h;(void)fl; int i=0; for(; name && name[i] && i<259; i++) g_lastlib[i]=(char)(name[i]<128?name[i]:'?'); g_lastlib[i]=0;
    ws("  [LoadLibraryExW] "); ws(g_lastlib); ws("\n");
    return g_base; // placeholder handle; real guest-mapping is the next phase
}

// ---- filesystem shim: translate Windows paths to Unix, do real I/O ----
// drive mapping: C:\ -> $prefix/drive_c, Z:\ -> /, others -> $prefix/drive_<x>
static void fd_record(int fd, const char *unixpath){ if(fd>=0&&fd<MAXFD){ free(g_fdpath[fd]); g_fdpath[fd]=strdup(unixpath); if(g_fddir[fd]){closedir(g_fddir[fd]);g_fddir[fd]=0;} g_fddel[fd]=0; } }
// mkdir -p for a file path's parent chain (scratch mirror needs this)
static void mkparents(const char *path){ char t[2300]; snprintf(t,sizeof t,"%s",path); for(char*p=t+1;*p;p++){ if(*p=='/'){ *p=0; mkdir(t,0755); *p='/'; } } }
// reverse of win2unix: unix path -> \\?\ Windows path (best effort)
static void unix2win(const char *u, uint16_t *out, int cap){
    char w[2048]; size_t dl=strlen(g_drivec);
    if(!strncmp(u,g_drivec,dl)) snprintf(w,sizeof w,"\\\\?\\C:%s", u+dl);
    else snprintf(w,sizeof w,"\\\\?\\Z:%s", u);
    for(char*p=w;*p;p++) if(*p=='/') *p='\\';
    int i=0; for(; w[i] && i<cap-1; i++) out[i]=(uint8_t)w[i]; out[i]=0;
}
static char* win2unix(const uint16_t *w, char *out, int outsz){
    char t[2048]; int n=0; for(; w && w[n] && n<2047; n++) t[n]=(char)(w[n]<128?w[n]:'?'); t[n]=0;
    // strip \\?\ long-path prefix
    char *s=t; if(!strncmp(s,"\\\\?\\",4)) s+=4;
    for(char *p=s; *p; p++) if(*p=='\\') *p='/';
    if(s[0] && s[1]==':'){
        char d=s[0];
        if(d=='Z'||d=='z') snprintf(out,outsz,"%s", s[2]? s+2 : "/");
        else if((d=='C'||d=='c') && g_drivec[0]) snprintf(out,outsz,"%s%s", g_drivec, s+2);
        else snprintf(out,outsz,"%.*s/drive_%c%s",(int)(strrchr(g_drivec,'/')-g_drivec),g_drivec,(char)(d|0x20),s+2);
    } else snprintf(out,outsz,"%s",s);
    return out;
}
// real-file handles are boxed fds offset by 0x1000 (pseudo-handles are 1/2/3)
static uint32_t MSABI i_GetFileAttributesW(const uint16_t *w){
    char p[2048]; struct stat st;
    if(stat(win2unix(w,p,sizeof p),&st)!=0){ g_lasterr=win_err(); return 0xFFFFFFFF; } g_lasterr=0; // INVALID_FILE_ATTRIBUTES
    return S_ISDIR(st.st_mode)? 0x10 : 0x80; // DIRECTORY : NORMAL
}
static int MSABI i_GetFileAttributesExW(const uint16_t *w,int lvl,void *info){
    (void)lvl; char p[2048]; struct stat st; if(stat(win2unix(w,p,sizeof p),&st)!=0){ g_lasterr=win_err(); return 0; } g_lasterr=0;
    if(info){ uint8_t*d=info; memset(d,0,36); *(uint32_t*)d = S_ISDIR(st.st_mode)?0x10:0x80;
        *(uint32_t*)(d+28)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+32)=(uint32_t)st.st_size; }
    return 1;
}
static void*  MSABI i_CreateFileW(const uint16_t *w,uint32_t acc,uint32_t share,void*sec,uint32_t disp,uint32_t fl,void*tmpl){
    (void)share;(void)sec;(void)fl;(void)tmpl; char p[2048]; win2unix(w,p,sizeof p);
    int flags = (acc & 0x40000000)? O_RDWR : O_RDONLY; // GENERIC_WRITE?
    switch(disp){                                   // Win32 creation disposition
        case 1: flags|=O_CREAT|O_EXCL;  break;      // CREATE_NEW
        case 2: flags|=O_CREAT|O_TRUNC; flags=(flags&~O_RDONLY)|O_RDWR; break; // CREATE_ALWAYS
        case 4: flags|=O_CREAT;         break;      // OPEN_ALWAYS
        case 5: flags|=O_TRUNC;         break;      // TRUNCATE_EXISTING
        default: break;                              // 3 OPEN_EXISTING
    }
    // directories: open O_RDONLY (BACKUP_SEMANTICS 0x02000000 or trailing slash)
    struct stat pst; if((fl&0x02000000) || (stat(p,&pst)==0 && S_ISDIR(pst.st_mode))) flags=O_RDONLY;
    int fd=open(p, flags, 0644);
    if(fd<0 && (flags&O_CREAT) && errno==ENOENT){ mkparents(p); fd=open(p, flags, 0644); } // scratch mirror
    if(getenv("PELOAD_TRACE_FS")){ ws("  [CreateFileW] "); ws(p); ws(fd<0?"  FAIL\n":"  ok\n"); }
    if(fd<0){ g_lasterr=win_err(); return (void*)-1; } g_lasterr=0; // INVALID_HANDLE_VALUE
    fd_record(fd,p);
    if(fl&0x04000000) g_fddel[fd<MAXFD?fd:0]=1; // FILE_FLAG_DELETE_ON_CLOSE
    return (void*)(intptr_t)(0x1000+fd);
}
static int    MSABI i_ReadFile(void*h,void*buf,uint32_t n,uint32_t*rd,void*ov){
    (void)ov; intptr_t hv=(intptr_t)h; if(hv<0x1000){ if(rd)*rd=0; return 0; }
    long k=read((int)(hv-0x1000),buf,n); if(rd)*rd=(uint32_t)(k<0?0:k); return k>=0;
}
static int    MSABI i_SetFilePointerEx(void*h,int64_t dist,int64_t*newp,uint32_t method){
    intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; off_t r=lseek((int)(hv-0x1000),dist, method); if(newp)*newp=r; return r>=0;
}
static int    MSABI i_GetFileSizeEx(void*h,int64_t*sz){ intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; struct stat st; if(fstat((int)(hv-0x1000),&st)) return 0; if(sz)*sz=st.st_size; return 1; }
static int    MSABI i_CloseHandleReal(void*h){ intptr_t hv=(intptr_t)h; if(hv>=0x1000){ int fd=(int)(hv-0x1000);
    if(fd<MAXFD){ if(g_fddir[fd]){ closedir(g_fddir[fd]); g_fddir[fd]=0; }
        // delete-on-close: honor for files only. Windows refcounts this across
        // all handles; we don't, so deleting a directory here would yank it
        // while the app still has other opens. Leaking the ephemeral scratch
        // dir is harmless (cleaned on next launch).
        if(g_fddel[fd]&&g_fdpath[fd]){ struct stat ds; if(stat(g_fdpath[fd],&ds)==0 && !S_ISDIR(ds.st_mode)) unlink(g_fdpath[fd]); }
        free(g_fdpath[fd]); g_fdpath[fd]=0; g_fddel[fd]=0; }
    close(fd); } return 1; }
// env var lookup via the host environment (so RECLAIMER_* pass through)
static uint32_t MSABI i_GetEnvironmentVariableW(const uint16_t*n,uint16_t*v,uint32_t sz){
    char nm[256]; int i=0; for(; n && n[i] && i<255; i++) nm[i]=(char)(n[i]<128?n[i]:'?'); nm[i]=0;
    const char *val=getenv(nm); if(!val){ return 0; }
    uint32_t len=(uint32_t)strlen(val);
    if(!v||sz==0) return len+1;                 // required size incl NUL
    uint32_t j=0; for(; val[j] && j<sz-1; j++) v[j]=(uint8_t)val[j]; v[j]=0; return j;
}
static uint32_t MSABI i_GetFullPathNameW(const uint16_t*name,uint32_t len,uint16_t*buf,uint16_t**part){
    (void)part; uint32_t i=0; for(; name[i] && i<len-1; i++) buf[i]=name[i]; buf[i]=0; return i;
}
static void   MSABI i_GetLocalTime(uint16_t *stm){
    if(!stm) return; struct timeval tv; gettimeofday(&tv,0); time_t t=tv.tv_sec; struct tm lt; localtime_r(&t,&lt);
    stm[0]=lt.tm_year+1900; stm[1]=lt.tm_mon+1; stm[2]=lt.tm_wday; stm[3]=lt.tm_mday;
    stm[4]=lt.tm_hour; stm[5]=lt.tm_min; stm[6]=lt.tm_sec; stm[7]=tv.tv_usec/1000;
}
static void   MSABI i_GetSystemTime(uint16_t *stm){ i_GetLocalTime(stm); }
static int    MSABI i_CreateDirectoryW(const uint16_t*w,void*sec){ (void)sec; char p[2048]; return mkdir(win2unix(w,p,sizeof p),0755)==0 || 1; }
static uint32_t MSABI i_GetCurrentDirectoryW(uint32_t sz,uint16_t*buf){ const char*c="C:\\ReclaimerCache"; uint32_t i=0; for(; c[i]&&i<sz-1;i++) buf[i]=(uint8_t)c[i]; if(i<sz)buf[i]=0; return i; }

// RegGetValueW: answer the Steam install-path query so MCC auto-discovery
// works. Steam lives at C:\Program Files\Steam (maps to our prefix).
static int wput(uint16_t*d,const char*s,uint32_t cap){ uint32_t i=0; for(; s[i] && i<cap/2-1; i++) d[i]=(uint8_t)s[i]; d[i]=0; return (i+1)*2; }
static long MSABI i_RegGetValueW(void*hk,const uint16_t*sub,const uint16_t*val,uint32_t fl,uint32_t*ty,void*data,uint32_t*cb){
    (void)hk;(void)sub;(void)fl; char vn[128]; int i=0; for(; val&&val[i]&&i<127;i++) vn[i]=(char)val[i]; vn[i]=0;
    const char *ans=0;
    if(!strcasecmp(vn,"SteamPath")||!strcasecmp(vn,"InstallPath")||!strcasecmp(vn,"SteamExe"))
        ans="C:\\Program Files\\Steam";
    if(!ans) return 2; // ERROR_FILE_NOT_FOUND
    if(ty)*ty=1; // REG_SZ
    uint32_t need=((uint32_t)strlen(ans)+1)*2;
    if(!data){ if(cb)*cb=need; return 0; }
    if(cb && *cb<need){ *cb=need; return 234; } // ERROR_MORE_DATA
    int n=wput((uint16_t*)data,ans, cb?*cb:need); if(cb)*cb=n; return 0; // ERROR_SUCCESS
}
static long MSABI i_RegOpenKeyExW(void*hk,const uint16_t*sub,uint32_t o,uint32_t sam,void**out){ (void)hk;(void)sub;(void)o;(void)sam; if(out)*out=(void*)0x4b4559; return 0; }
static long MSABI i_RegQueryValueExW(void*hk,const uint16_t*val,void*r,uint32_t*ty,void*data,uint32_t*cb){ (void)r; return i_RegGetValueW(hk,0,val,0,ty,data,cb); }
static long MSABI i_RegCloseKey(void*hk){ (void)hk; return 0; }

// directory enumeration: FindFirstFileExW / FindNextFileW / FindClose
#include <dirent.h>
typedef struct { DIR*d; char dir[1536]; char pat[260]; } Find;
static Find g_finds[64]; static int g_nfinds;
static int pat_match(const char*pat,const char*name){
    if(!strcmp(pat,"*")||!strcmp(pat,"*.*")) return name[0]!='.'||1;
    const char*st=strchr(pat,'*');
    if(!st) return !strcasecmp(pat,name);
    size_t pl=st-pat; if(strncasecmp(pat,name,pl)) return 0;         // prefix
    const char*suf=st+1; size_t sl=strlen(suf), nl=strlen(name);
    if(sl==0) return 1; if(nl<pl+sl) return 0;
    return !strcasecmp(name+nl-sl,suf);                              // suffix
}
static void fill_find(Find*f,struct dirent*de,void*fd){
    uint8_t*d=fd; memset(d,0,592); char full[1800]; struct stat st;
    snprintf(full,sizeof full,"%s/%s",f->dir,de->d_name);
    uint32_t attr=0x80; if(stat(full,&st)==0){ attr=S_ISDIR(st.st_mode)?0x10:0x80; *(uint32_t*)(d+28)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+32)=(uint32_t)st.st_size; }
    *(uint32_t*)d=attr; uint16_t*nm=(uint16_t*)(d+44); int i=0; for(; de->d_name[i]&&i<259;i++) nm[i]=(uint8_t)de->d_name[i]; nm[i]=0;
}
static void* MSABI i_FindFirstFileExW(const uint16_t*w,int lvl,void*fd,int op,void*filt,uint32_t fla){
    (void)lvl;(void)op;(void)filt;(void)fla; if(g_nfinds>=64) return (void*)-1;
    char p[2048]; win2unix(w,p,sizeof p);
    char *slash=strrchr(p,'/'); char dir[1536],pat[260];
    if(slash){ int dl=slash-p; if(dl>1535)dl=1535; memcpy(dir,p,dl); dir[dl]=0; snprintf(pat,sizeof pat,"%s",slash+1); }
    else { snprintf(dir,sizeof dir,"."); snprintf(pat,sizeof pat,"%s",p); }
    DIR*d=opendir(dir); if(!d){ g_lasterr=win_err(); return (void*)-1; }
    Find*f=&g_finds[g_nfinds]; f->d=d; snprintf(f->dir,sizeof f->dir,"%s",dir); snprintf(f->pat,sizeof f->pat,"%s",pat);
    struct dirent*de; while((de=readdir(d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(pat,de->d_name)){ fill_find(f,de,fd); return (void*)(intptr_t)(0x2000+g_nfinds++); } }
    closedir(d); g_lasterr=2; return (void*)-1; // ERROR_FILE_NOT_FOUND
}
static int MSABI i_FindNextFileW(void*h,void*fd){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); if(idx<0||idx>=g_nfinds||!g_finds[idx].d) return 0;
    Find*f=&g_finds[idx]; struct dirent*de; while((de=readdir(f->d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(f->pat,de->d_name)){ fill_find(f,de,fd); return 1; } } g_lasterr=18; return 0; } // ERROR_NO_MORE_FILES
static int MSABI i_FindClose(void*h){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); if(idx>=0&&idx<g_nfinds&&g_finds[idx].d){ closedir(g_finds[idx].d); g_finds[idx].d=0; } return 1; }
static int MSABI i_GetFileInformationByHandle(void*h,void*info){ intptr_t hv=(intptr_t)h; if(getenv("PELOAD_TRACE_FS")) ws("  [GetFileInformationByHandle]\n"); if(hv<0x1000||!info) return 0; struct stat st; if(fstat((int)(hv-0x1000),&st)) return 0;
    uint8_t*d=info; memset(d,0,52); *(uint32_t*)d=S_ISDIR(st.st_mode)?0x10:0x80; *(uint32_t*)(d+32)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+36)=(uint32_t)st.st_size; *(uint32_t*)(d+40)=1; return 1; }

// GetProcAddress: hand back a live logging stub for whatever is requested,
// so dynamic resolution yields a callable (not NULL) and startup continues.
static void *resolve_impl(const char *fn); // fwd
static void* MSABI i_GetProcAddress(void *mod, char *name){
    (void)mod;
    if((uint64_t)name < 0x10000){ // imported by ordinal
        if(imp_count<8192){ imp_dll[imp_count]="(dynamic)"; imp_fn[imp_count]="#ordinal";
            return make_stub(imp_count++); }
        return 0;
    }
    void *r = resolve_impl(name);
    if(r) return r;
    if(imp_count<8192){ imp_dll[imp_count]="(dynamic)"; imp_fn[imp_count]=name;
        return make_stub(imp_count++); }
    return 0;
}

// Named dispatch: returns a real impl for a known name, else NULL.
static void *resolve_impl(const char *fn){
    struct { const char *n; void *f; } T[] = {
        {"GetSystemTimeAsFileTime",(void*)i_GetSystemTimeAsFileTime},
        {"QueryPerformanceCounter",(void*)i_QueryPerformanceCounter},
        {"QueryPerformanceFrequency",(void*)i_QueryPerformanceFrequency},
        {"GetCurrentThreadId",(void*)i_GetCurrentThreadId},
        {"GetCurrentProcessId",(void*)i_GetCurrentProcessId},
        {"InitializeSListHead",(void*)i_InitializeSListHead},
        {"GetLastError",(void*)i_GetLastError},
        {"SetLastError",(void*)i_SetLastError},
        {"__p___argv",(void*)i_p_argv},
        {"__p___argc",(void*)i_p_argc},
        {"__p__environ",(void*)i_p_environ},
        {"_get_initial_narrow_environment",(void*)i_get_initial_narrow_environment},
        {"_configure_narrow_argv",(void*)i_configure_narrow_argv},
        {"_initialize_narrow_environment",(void*)i_initialize_narrow_environment},
        {"_initialize_onexit_table",(void*)i_initialize_onexit_table},
        {"_register_onexit_function",(void*)i_register_onexit_function},
        {"_crt_atexit",(void*)i_crt_atexit},
        {"_set_app_type",(void*)i_set_app_type},
        {"_seh_filter_exe",(void*)i_seh_filter_exe},
        {"_initterm",(void*)i_initterm},
        {"_initterm_e",(void*)i_initterm_e},
        {"GetProcessHeap",(void*)i_GetProcessHeap},
        {"HeapAlloc",(void*)i_HeapAlloc},
        {"HeapReAlloc",(void*)i_HeapReAlloc},
        {"HeapFree",(void*)i_HeapFree},
        {"HeapSize",(void*)i_HeapSize},
        {"HeapCreate",(void*)i_HeapCreate},
        {"VirtualAlloc",(void*)i_VirtualAlloc},
        {"VirtualProtect",(void*)i_VirtualProtect},
        {"VirtualFree",(void*)i_VirtualFree},
        {"GetCommandLineW",(void*)i_GetCommandLineW},
        {"GetModuleHandleW",(void*)i_GetModuleHandleW},
        {"GetModuleHandleA",(void*)i_GetModuleHandleA},
        {"TlsAlloc",(void*)i_TlsAlloc},
        {"TlsGetValue",(void*)i_TlsGetValue},
        {"TlsSetValue",(void*)i_TlsSetValue},
        {"TlsFree",(void*)i_TlsFree},
        {"GetProcAddress",(void*)i_GetProcAddress},
        {"__p__commode",(void*)i_p_commode},
        {"__p__fmode",(void*)i_p_fmode},
        {"_set_fmode",(void*)i_set_fmode},
        {"_get_fmode",(void*)i_get_fmode},
        {"__p__dowildcard",(void*)i_p_dowildcard},
        {"_configthreadlocale",(void*)i_configthreadlocale},
        {"_cexit",(void*)i_cexit},
        // expanded kernel32/core surface for genuine ucrtbase
        {"GetStdHandle",(void*)i_GetStdHandle},
        {"WriteFile",(void*)i_WriteFile},
        {"WriteConsoleW",(void*)i_WriteConsoleW},
        {"GetFileType",(void*)i_GetFileType},
        {"CloseHandle",(void*)i_CloseHandleReal},
        {"CreateFileW",(void*)i_CreateFileW},
        {"GetFileAttributesW",(void*)i_GetFileAttributesW},
        {"GetFileAttributesExW",(void*)i_GetFileAttributesExW},
        {"ReadFile",(void*)i_ReadFile},
        {"SetFilePointerEx",(void*)i_SetFilePointerEx},
        {"GetFileSizeEx",(void*)i_GetFileSizeEx},
        {"GetFullPathNameW",(void*)i_GetFullPathNameW},
        {"GetLocalTime",(void*)i_GetLocalTime},
        {"GetSystemTime",(void*)i_GetSystemTime},
        {"CreateDirectoryW",(void*)i_CreateDirectoryW},
        {"GetCurrentDirectoryW",(void*)i_GetCurrentDirectoryW},
        {"CreateDirectoryW",(void*)i_CreateDirectoryW},
        {"RegGetValueW",(void*)i_RegGetValueW},
        {"RegOpenKeyExW",(void*)i_RegOpenKeyExW},
        {"RegQueryValueExW",(void*)i_RegQueryValueExW},
        {"RegCloseKey",(void*)i_RegCloseKey},
        {"FindFirstFileExW",(void*)i_FindFirstFileExW},
        {"FindFirstFileW",(void*)i_FindFirstFileExW},
        {"FindNextFileW",(void*)i_FindNextFileW},
        {"FindClose",(void*)i_FindClose},
        {"GetFileInformationByHandle",(void*)i_GetFileInformationByHandle},
        {"FlushFileBuffers",(void*)i_FlushFileBuffers},
        {"GetCurrentProcess",(void*)i_GetCurrentProcess},
        {"GetCurrentThread",(void*)i_GetCurrentThread},
        {"EncodePointer",(void*)i_EncodePointer},
        {"DecodePointer",(void*)i_DecodePointer},
        {"InitializeCriticalSection",(void*)i_noop},
        {"InitializeCriticalSectionAndSpinCount",(void*)i_ret1},
        {"InitializeCriticalSectionEx",(void*)i_InitializeCriticalSectionEx},
        {"EnterCriticalSection",(void*)i_noop},
        {"LeaveCriticalSection",(void*)i_noop},
        {"DeleteCriticalSection",(void*)i_noop},
        {"TryEnterCriticalSection",(void*)i_ret1},
        {"InitializeSRWLock",(void*)i_noop},
        {"AcquireSRWLockExclusive",(void*)i_noop},
        {"ReleaseSRWLockExclusive",(void*)i_noop},
        {"AcquireSRWLockShared",(void*)i_noop},
        {"ReleaseSRWLockShared",(void*)i_noop},
        {"TryAcquireSRWLockExclusive",(void*)i_ret1},
        {"InitializeConditionVariable",(void*)i_noop},
        {"WakeConditionVariable",(void*)i_noop},
        {"WakeAllConditionVariable",(void*)i_noop},
        {"InitOnceBeginInitialize",(void*)i_InitOnceBeginInitialize},
        {"InitOnceComplete",(void*)i_InitOnceComplete},
        {"FlsAlloc",(void*)i_FlsAlloc},
        {"FlsGetValue",(void*)i_FlsGetValue},
        {"FlsSetValue",(void*)i_FlsSetValue},
        {"FlsFree",(void*)i_FlsFree},
        {"IsProcessorFeaturePresent",(void*)i_IsProcessorFeaturePresent},
        {"IsDebuggerPresent",(void*)i_IsDebuggerPresent},
        {"GetACP",(void*)i_GetACP},
        {"GetModuleHandleExW",(void*)i_GetModuleHandleExW},
        {"MultiByteToWideChar",(void*)i_MultiByteToWideChar},
        {"WideCharToMultiByte",(void*)i_WideCharToMultiByte},
        {"GetSystemInfo",(void*)i_GetSystemInfo},
        {"GetSystemTimePreciseAsFileTime",(void*)i_GetSystemTimeAsFileTime},
        {"ExitProcess",(void*)i_ExitProcess},
        {"TerminateProcess",(void*)i_TerminateProcess},
        {"GetModuleFileNameW",(void*)i_GetModuleFileNameW},
        {"GetEnvironmentVariableW",(void*)i_GetEnvironmentVariableW},
        {"GetStartupInfoW",(void*)i_GetStartupInfoW},
        {"RtlPcToFileHeader",(void*)i_RtlPcToFileHeader},
        {"GetCommandLineA",(void*)i_GetCommandLineA},
        {"GetEnvironmentStringsW",(void*)i_GetEnvironmentStringsW},
        {"FreeEnvironmentStringsW",(void*)i_FreeEnvironmentStringsW},
        {"GetTickCount",(void*)i_GetTickCount},
        {"GetTickCount64",(void*)i_GetTickCount64},
        {"LoadLibraryExW",(void*)i_LoadLibraryExW},
        {"NtWriteFile",(void*)i_NtWriteFile},
        {"NtReadFile",(void*)i_NtReadFile},
        {"BCryptGenRandom",(void*)i_BCryptGenRandom},
        {"ProcessPrng",(void*)i_ProcessPrng},
        {"RtlGenRandom",(void*)i_RtlGenRandom},
        {"SystemFunction036",(void*)i_RtlGenRandom},
        {"LockFileEx",(void*)i_LockFileEx},
        {"UnlockFileEx",(void*)i_UnlockFileEx},
        {"LockFile",(void*)i_LockFile},
        {"UnlockFile",(void*)i_UnlockFile},
        {"FlushViewOfFile",(void*)i_FlushViewOfFile},
        {"SetEndOfFile",(void*)i_SetEndOfFile},
        {"MoveFileExW",(void*)i_MoveFileExW},
        {"DeleteFileW",(void*)i_DeleteFileW},
        {"CopyFileExW",(void*)i_CopyFileExW},
        {"CopyFileW",(void*)i_CopyFileW},
        {"CreateHardLinkW",(void*)i_CreateHardLinkW},
        {"LocalAlloc",(void*)i_LocalAlloc},
        {"LocalFree",(void*)i_LocalFree},
        {"CryptProtectData",(void*)i_CryptProtectData},
        {"CryptUnprotectData",(void*)i_CryptUnprotectData},
        {"GetConsoleMode",(void*)i_GetConsoleMode},
        {"SetConsoleMode",(void*)i_SetConsoleMode},
        {"GetConsoleOutputCP",(void*)i_GetConsoleOutputCP},
        {"GetDriveTypeW",(void*)i_GetDriveTypeW},
        {"GetDriveTypeA",(void*)i_GetDriveTypeA},
        {"GetVolumeInformationW",(void*)i_GetVolumeInformationW},
        {"GetDiskFreeSpaceExW",(void*)i_GetDiskFreeSpaceExW},
        {"GetVolumePathNameW",(void*)i_GetVolumePathNameW},
        {"RtlNtStatusToDosError",(void*)i_RtlNtStatusToDosError},
        {"FormatMessageW",(void*)i_FormatMessageW},
        {"SetConsoleCtrlHandler",(void*)i_SetConsoleCtrlHandler},
        {"CreateSemaphoreA",(void*)i_CreateSemaphoreA},
        {"CreateSemaphoreW",(void*)i_CreateSemaphoreW},
        {"ReleaseSemaphore",(void*)i_ReleaseSemaphore},
        {"CreateEventW",(void*)i_CreateEventW},
        {"SetEvent",(void*)i_SetEvent},
        {"ResetEvent",(void*)i_ResetEvent},
        {"WaitForSingleObject",(void*)i_WaitForSingleObject},
        {"WaitForMultipleObjects",(void*)i_WaitForMultipleObjects},
        {"CreateThread",(void*)i_CreateThread},
        {"GetThreadId",(void*)i_GetThreadId},
        {"GetExitCodeThread",(void*)i_GetExitCodeThread},
        {"OpenProcess",(void*)i_OpenProcess},
        {"SwitchToThread",(void*)i_SwitchToThread},
        {"CompareStringOrdinal",(void*)i_CompareStringOrdinal},
        {"GetTempPathW",(void*)i_GetTempPathW},
        {"GetTempPath2W",(void*)i_GetTempPath2W},
        {"GetFileInformationByHandleEx",(void*)i_GetFileInformationByHandleEx},
        {"GetFinalPathNameByHandleW",(void*)i_GetFinalPathNameByHandleW},
        {"RemoveDirectoryW",(void*)i_RemoveDirectoryW},
        {"SetFileInformationByHandle",(void*)i_SetFileInformationByHandle},
        {"NtCreateFile",(void*)i_NtCreateFile},
        {"NtOpenFile",(void*)i_NtOpenFile},
        {"SetThreadStackGuarantee",(void*)i_SetThreadStackGuarantee},
        {"FreeLibrary",(void*)i_FreeLibrary},
        {"GetModuleFileNameA",(void*)i_GetModuleFileNameA},
        {0,0}
    };
    for(int i=0; T[i].n; i++) if(!strcmp(T[i].n, fn)) return T[i].f;
    return 0;
}

// ---------------------------------------------------------------------------
// Guest DLL loading: map genuine Microsoft ucrtbase/vcruntime140/msvcp140 and
// resolve the exe's CRT imports against their real export tables.
// ---------------------------------------------------------------------------
#define DIR_EXPORT 0

// look up an export by name in a loaded module; follow one forwarder level
static void *export_lookup(uint8_t *base, const char *want){
    DOS *dos=(DOS*)base; COFF *coff=(COFF*)(base+dos->e_lfanew);
    OPT64 *opt=(OPT64*)((uint8_t*)coff+sizeof(COFF));
    uint32_t erva=opt->Dir[DIR_EXPORT].VA, esz=opt->Dir[DIR_EXPORT].Size;
    if(!erva) return 0;
    uint32_t *p=(uint32_t*)(base+erva);
    uint32_t nNames=p[6], aFuncs=p[7], aNames=p[8], aOrds=p[9];
    uint32_t *fns=(uint32_t*)(base+aFuncs), *nms=(uint32_t*)(base+aNames);
    uint16_t *ords=(uint16_t*)(base+aOrds);
    for(uint32_t i=0;i<nNames;i++){
        const char *nm=(char*)(base+nms[i]);
        if(!strcmp(nm,want)){
            uint32_t frva=fns[ords[i]];
            if(frva>erva && frva<erva+esz){ // forwarder "DLL.func"
                const char *fwd=(char*)(base+frva); const char *dot=strchr(fwd,'.');
                if(dot){ for(int m=0;m<g_nmods;m++){ void *r=export_lookup(g_mods[m].base, dot+1); if(r) return r; } }
                return 0;
            }
            return base+frva;
        }
    }
    return 0;
}

static int is_crt_dll(const char *d){
    return !strncasecmp(d,"api-ms-win-crt",14) || !strcasecmp(d,"ucrtbase.dll") ||
           !strcasecmp(d,"vcruntime140.dll") || !strcasecmp(d,"msvcp140.dll") ||
           !strcasecmp(d,"ucrtbased.dll");
}

// unified resolver: CRT names -> guest exports; else native impl; else NULL
static void *uni_resolve(const char *dll, const char *fn){
    if(is_crt_dll(dll)){
        for(int m=0;m<g_nmods;m++){ void *r=export_lookup(g_mods[m].base, fn); if(r) return r; }
    }
    return resolve_impl(fn);
}

// map + relocate a PE image (no import binding). returns base; fills opt/coff.
static uint8_t *map_image(const char *path, OPT64 **popt, COFF **pcoff){
    int fd=open(path,O_RDONLY); if(fd<0) return 0;
    struct stat st; fstat(fd,&st);
    uint8_t *file=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(file==MAP_FAILED) return 0;
    DOS *dos=(DOS*)file; if(dos->e_magic!=0x5A4D) return 0;
    COFF *coff=(COFF*)(file+dos->e_lfanew); if(coff->Sig!=0x4550||coff->Machine!=0x8664) return 0;
    OPT64 *opt=(OPT64*)((uint8_t*)coff+sizeof(COFF));
    SEC *sec=(SEC*)((uint8_t*)opt+coff->OptSz);
    uint8_t *base=mmap(0,opt->ImageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(base==MAP_FAILED) return 0;
    memcpy(base,file,opt->HdrSize);
    for(int i=0;i<coff->NumSec;i++) if(sec[i].RawSize) memcpy(base+sec[i].VA,file+sec[i].RawPtr,sec[i].RawSize);
    int64_t delta=(int64_t)(uint64_t)base-(int64_t)opt->ImageBase;
    uint32_t rr=opt->Dir[DIR_BASERELOC].VA, rs=opt->Dir[DIR_BASERELOC].Size;
    if(rr&&delta){ uint8_t *q=base+rr,*e=q+rs; while(q<e){ uint32_t pg=*(uint32_t*)q,bl=*(uint32_t*)(q+4);
        uint16_t *en=(uint16_t*)(q+8); int n=(bl-8)/2; for(int i=0;i<n;i++) if((en[i]>>12)==10) *(uint64_t*)(base+pg+(en[i]&0xfff))+=delta; q+=bl; } }
    // relocate opt/coff pointers to live in the mapped copy
    *pcoff=(COFF*)(base+dos->e_lfanew); *popt=(OPT64*)((uint8_t*)*pcoff+sizeof(COFF));
    return base;
}

// bind one module's imports using uni_resolve; unresolved -> logging stub
static void bind_imports(uint8_t *base, OPT64 *opt){
    uint32_t ir=opt->Dir[DIR_IMPORT].VA; if(!ir) return;
    IMPDESC *d=(IMPDESC*)(base+ir);
    for(; d->Name; d++){
        const char *dll=(char*)(base+d->Name);
        uint64_t *oft=(uint64_t*)(base+(d->OFT?d->OFT:d->FT)), *ft=(uint64_t*)(base+d->FT);
        int trace = getenv("PELOAD_TRACE_ALL")!=0;
        for(int i=0; oft[i]; i++){
            const char *fn = (oft[i]>>63)? "#ordinal" : (char*)(base+(uint32_t)oft[i]+2);
            void *r = uni_resolve(dll, fn);
            if(r && trace && imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn; imp_real[imp_count]=r; ft[i]=(uint64_t)make_trace_stub(imp_count,r); imp_count++; }
            else if(r){ ft[i]=(uint64_t)r; }
            else if(imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn; ft[i]=(uint64_t)make_stub(imp_count++); }
        }
    }
}

static void set_protections(uint8_t *base, COFF *coff, OPT64 *opt){
    SEC *sec=(SEC*)((uint8_t*)opt+coff->OptSz);
    for(int i=0;i<coff->NumSec;i++){ int pr=PROT_READ;
        if(sec[i].Chars&0x20000000) pr|=PROT_EXEC; if(sec[i].Chars&0x80000000) pr|=PROT_WRITE;
        uint32_t len=sec[i].VSize?sec[i].VSize:sec[i].RawSize; len=(len+0xfff)&~0xfffu;
        mprotect(base+sec[i].VA,len,pr); }
}

// load a guest DLL end-to-end: map, bind, protect, register exports, DllMain
static uint8_t *load_guest(const char *path, const char *name){
    OPT64 *opt; COFF *coff; uint8_t *base=map_image(path,&opt,&coff);
    if(!base){ ws("  guest load FAILED: "); ws(name); ws("\n"); return 0; }
    bind_imports(base,opt);
    set_protections(base,coff,opt);
    g_mods[g_nmods].name=name; g_mods[g_nmods].base=base; g_nmods++;
    ws("  guest "); ws(name); ws(" @ "); whex((uint64_t)base);
    uint64_t ep=opt->EntryRVA;
    if(ep){ int (MSABI *dm)(void*,uint32_t,void*)=(void*)(base+ep); int r=dm(base,1,0);
        ws("    DllMain -> "); whex((uint64_t)(uint32_t)r); }
    return base;
}

extern char **environ;
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "halo3.dll";
    ws("peload: loading "); ws(path); ws("\n");

    // guest argv/argc/environ: pass the PE path + any args after it
    g_argv = argv+1; g_argc = argc-1; if(g_argc<1){ g_argc=1; }
    g_environ = environ;
    { // build the command line from the PE path + any args after it (genuine
      // ucrtbase parses argv from GetCommandLineW, so args must live here)
        int n=0;
        for(int a=1; a<argc; a++){
            const char *s=argv[a];
            if(a>1 && n<2046){ g_cmdlineW[n]=' '; g_cmdlineA[n]=' '; n++; }
            for(int j=0; s[j] && n<2046; j++){ g_cmdlineW[n]=(uint16_t)s[j]; g_cmdlineA[n]=s[j]; n++; }
        }
        g_cmdlineW[n]=0; g_cmdlineA[n]=0;
    }

    // ---- map + relocate the target image (exe or dll) ----
    OPT64 *opt; COFF *coff; g_base = map_image(path,&opt,&coff);
    if(!g_base){ ws("map_image failed\n"); return 1; }
    g_pref = opt->ImageBase;
    ws("  image base "); whexn(opt->ImageBase); ws("  size "); whex(opt->ImageSize);
    ws("  subsystem "); whex(opt->Subsys);
    ws("  mapped at  "); whex((uint64_t)g_base);

    // ---- stub pool (RX; make_stub toggles RW/RX at runtime) ----
    g_stubpool = mmap(0, 32*8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    mprotect(g_stubpool, 32*8192, PROT_READ|PROT_EXEC);
    g_pool_exec = 1;
    g_trpool = mmap(0, 96*8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    mprotect(g_trpool, 96*8192, PROT_READ|PROT_EXEC); g_trexec = 1;

    // ---- TEB/PEB via %gs overlay, set up BEFORE any guest code runs ----
    static uint8_t teb[0x2000] __attribute__((aligned(16)));
    static uint8_t peb[0x1000] __attribute__((aligned(16)));
    *(uint64_t*)(teb+0x30) = (uint64_t)teb;   // NtTib.Self
    *(uint64_t*)(teb+0x60) = (uint64_t)peb;   // PEB
    static void *tls_array[128];              // shared TLS pointer array
    *(uint64_t*)(teb+0x58) = (uint64_t)tls_array;
    __asm__ volatile("movq %0, %%gs:0x30" :: "r"((uint64_t)teb));
    __asm__ volatile("movq %0, %%gs:0x60" :: "r"((uint64_t)peb));
    __asm__ volatile("movq %0, %%gs:0x58" :: "r"((uint64_t)tls_array));
    g_peb_ptr=peb;
    ws("  TEB @ "); whexn((uint64_t)teb); ws("  PEB @ "); whex((uint64_t)peb);

    // ---- filesystem roots + env defaults for the game client ----
    { const char *h=getenv("HOME");
      if(h) snprintf(g_drivec,sizeof g_drivec,"%s/Library/Application Support/ReclaimerMac/prefix/drive_c",h);
      // MCC lives under the prefix; expose it to the client as a C: path and via env.
      // (C:\Program Files\Steam\... maps back to the same real files.)
      setenv("RECLAIMER_MCC_FILES","C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection",0);
      // profile/cache must be absolute; the client complained when unset
      // Rust treats /unix/paths as non-absolute on Windows, so use C: paths
      // that map (via win2unix) to real dirs we create under drive_c.
      char pd[1200];
      snprintf(pd,sizeof pd,"%s/ReclaimerProfile",g_drivec); mkdir(pd,0755); setenv("RECLAIMER_PROFILE_DIR","C:\\ReclaimerProfile",0);
      snprintf(pd,sizeof pd,"%s/ReclaimerCache",g_drivec);   mkdir(pd,0755); setenv("RECLAIMER_CACHE_DIR","C:\\ReclaimerCache",0);
      snprintf(pd,sizeof pd,"%s/ReclaimerCache/Temp",g_drivec); mkdir(pd,0755);
      setenv("LOCALAPPDATA","C:\\ReclaimerCache",0); setenv("APPDATA","C:\\ReclaimerCache",0);
      setenv("TEMP","C:\\ReclaimerCache\\Temp",0); setenv("TMP","C:\\ReclaimerCache\\Temp",0);
      setenv("USERPROFILE","C:\\ReclaimerProfile",0); }

    // ---- locate the genuine MS CRT dir: $RECLAIMER_CRT_DIR, then next to the
    // peload binary (<dir>/crt and <dir>/../crt), then the project tree ----
    const char *home = getenv("HOME");
    char crtdir[1024]={0}; char bindir[1024]={0};
    { const char *b=argv[0]; int s=-1; for(int i=0; b[i]; i++) if(b[i]=='/') s=i;
      if(s>=0){ int n=s<1023?s:1023; memcpy(bindir,b,n); bindir[n]=0; } else snprintf(bindir,sizeof bindir,"."); }
    char cand[5][1024]; int nc=0;
    if(getenv("RECLAIMER_CRT_DIR")) snprintf(cand[nc++],1024,"%s",getenv("RECLAIMER_CRT_DIR"));
    snprintf(cand[nc++],1024,"%s/crt",bindir);
    snprintf(cand[nc++],1024,"%s/../crt",bindir);
    if(home){ snprintf(cand[nc++],1024,"%s/Desktop/HaloReclaimerMac/shim/crt",home);
              snprintf(cand[nc++],1024,"%s/Library/Application Support/ReclaimerMac/ms-crt",home); }
    for(int i=0;i<nc;i++){ char p[1200]; snprintf(p,sizeof p,"%s/ucrtbase.dll",cand[i]);
        if(access(p,R_OK)==0){ snprintf(crtdir,sizeof crtdir,"%s",cand[i]); break; } }

    const char *names[3] = {"ucrtbase.dll","vcruntime140.dll","msvcp140.dll"};
    ws("peload: CRT dir "); ws(crtdir[0]?crtdir:"(not found)"); ws("\n");
    for(int i=0;i<3;i++){ char crt[1300]; snprintf(crt,sizeof crt,"%s/%s",crtdir,names[i]); load_guest(crt, names[i]); }

    // ---- bind the target's imports (CRT -> guest exports, else native/stub) ----
    bind_imports(g_base, opt);
    ws("  bound imps (exe+unresolved stubs) "); whex((uint64_t)imp_count);

    // delay imports: report only (never called by a headless server)
    uint32_t delayrva = opt->Dir[DIR_DELAY].VA;
    if(delayrva){
        DELAYDESC *dd = (DELAYDESC*)rva(delayrva);
        ws("  delay-load (not bound; unused headless):\n");
        for(; dd->Name; dd++){ ws("     "); ws((char*)rva(dd->Name)); ws("\n"); }
    }

    set_protections(g_base, coff, opt);

    // Static TLS + callbacks. MSVC/Rust read the per-thread TLS array from
    // TEB.ThreadLocalStoragePointer (gs:[0x58]) and index it by the module's
    // TLS index. Without this, `mov rcx, gs:[0x58]` yields NULL and the first
    // __declspec(thread) access faults. So: build the TLS data block from the
    // image template, assign index 0, publish the array at TEB+0x58 / gs:0x58.
    uint32_t tlsrva = opt->Dir[DIR_TLS].VA;
    if(tlsrva){
        TLSDIR *t = (TLSDIR*)rva(tlsrva);          // fields are VAs (relocated)
        uint64_t raw = t->End - t->Start, total = raw + t->Zero;
        uint8_t *block = malloc(total ? total : 1);
        if(raw) memcpy(block, (void*)t->Start, raw);
        if(t->Zero) memset(block+raw, 0, t->Zero);
        *(uint32_t*)t->Index = 0;                   // this module's TLS index
        tls_array[0] = block;                        // publish into shared array
        g_tls_src=(uint8_t*)t->Start; g_tls_raw=raw; g_tls_total=total; // template for new threads
        ws("  static TLS block "); whexn((uint64_t)block); ws(" size "); whex(total);
        if(t->Callbacks){
            uint64_t *cb = (uint64_t*)t->Callbacks;
            for(int i=0; cb[i]; i++){
                ws("  TLS callback "); whex(cb[i]);
                void (__attribute__((ms_abi)) *fn)(void*,uint32_t,void*) = (void*)cb[i];
                fn(g_base, 1 /*DLL_PROCESS_ATTACH*/, 0);
            }
        }
    }

    // entry point
    uint64_t entry = (uint64_t)rva(opt->EntryRVA);
    ws("  entry @ "); whex(entry);
    ws("peload: calling entry (expect stub logs, then a fault at the first\n"
       "        unimplemented behavior — that is the signal we want)\n");
    if(opt->Subsys==2 || (coff->Chars & 0x2000)){
        // DLL: DllMain(hinst, reason, reserved)
        int (__attribute__((ms_abi)) *dllmain)(void*,uint32_t,void*) = (void*)entry;
        int r = dllmain(g_base, 1, 0);
        ws("  DllMain returned "); whex((uint64_t)(uint32_t)r);
    } else {
        // EXE: mainCRTStartup(void)
        void (__attribute__((ms_abi)) *start)(void) = (void*)entry;
        start();
        ws("  entry returned\n");
    }
    ws("peload: done\n");
    return 0;
}
