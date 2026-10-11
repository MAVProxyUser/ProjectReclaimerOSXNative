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
static uint32_t MSABI i_GetLastError(void){ return 0; }
static void   MSABI i_SetLastError(uint32_t e){ (void)e; }

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
    (void)ov; int fd = (h==(void*)2)?1 : (h==(void*)3)?2 : -1; long k = fd>=0? write(fd,buf,n) : 0; if(wr)*wr=(uint32_t)(k<0?0:k); return fd>=0; }
static int    MSABI i_WriteConsoleW(void *h, const uint16_t *s, uint32_t n, uint32_t *wr, void *rsv){
    (void)rsv; int fd=(h==(void*)3)?2:1; for(uint32_t i=0;i<n;i++){ char c=(s[i]<128)?(char)s[i]:'?'; (void)!write(fd,&c,1);} if(wr)*wr=n; return 1; }
static uint32_t MSABI i_GetFileType(void *h){ (void)h; return 2; } // FILE_TYPE_CHAR
static int    MSABI i_CloseHandle(void *h){ (void)h; return 1; }
static void*  MSABI i_CreateFileW(void *a,uint32_t b,uint32_t c,void*d,uint32_t e,uint32_t f,void*g){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; return (void*)-1; }
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
static uint32_t MSABI i_GetEnvironmentVariableW(const uint16_t*n,uint16_t*v,uint32_t sz){ (void)n;(void)v;(void)sz; return 0; }
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
    (void)ev;(void)apc;(void)apcctx;(void)off;(void)key;
    int fd = (h==(void*)2)?1 : (h==(void*)3)?2 : (h==(void*)1)?-1 : 1;
    long k = fd>=0? write(fd,buf,len) : len;
    if(iosb){ ((uint64_t*)iosb)[0]=0; ((uint64_t*)iosb)[1]=(uint64_t)(k<0?0:k); }
    return 0; // STATUS_SUCCESS
}
static uint32_t MSABI i_GetConsoleMode(void*h,uint32_t*m){ (void)h; if(m)*m=0; return 1; }
static uint32_t MSABI i_SetConsoleMode(void*h,uint32_t m){ (void)h;(void)m; return 1; }
static int MSABI i_SetThreadStackGuarantee(uint32_t*s){ (void)s; return 1; }
static int MSABI i_FreeLibrary(void*h){ (void)h; return 1; }
static uint32_t MSABI i_GetModuleFileNameA(void*m,char*buf,uint32_t sz){ (void)m; uint32_t i=0; for(; i<sz && g_cmdlineA[i]; i++) buf[i]=g_cmdlineA[i]; if(i<sz) buf[i]=0; return i; }
static uint32_t MSABI i_GetConsoleOutputCP(void){ return 65001; }
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
        {"CloseHandle",(void*)i_CloseHandle},
        {"CreateFileW",(void*)i_CreateFileW},
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
        {"GetConsoleMode",(void*)i_GetConsoleMode},
        {"SetConsoleMode",(void*)i_SetConsoleMode},
        {"GetConsoleOutputCP",(void*)i_GetConsoleOutputCP},
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
        for(int i=0; oft[i]; i++){
            const char *fn = (oft[i]>>63)? "#ordinal" : (char*)(base+(uint32_t)oft[i]+2);
            void *r = uni_resolve(dll, fn);
            if(r){ ft[i]=(uint64_t)r; }
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
    ws("  TEB @ "); whexn((uint64_t)teb); ws("  PEB @ "); whex((uint64_t)peb);

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
