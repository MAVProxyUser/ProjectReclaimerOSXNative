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
        {0,0}
    };
    for(int i=0; T[i].n; i++) if(!strcmp(T[i].n, fn)) return T[i].f;
    return 0;
}

extern char **environ;
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "halo3.dll";
    ws("peload: loading "); ws(path); ws("\n");

    // guest argv/argc/environ: pass the PE path + any args after it
    g_argv = argv+1; g_argc = argc-1; if(g_argc<1){ g_argc=1; }
    g_environ = environ;
    { // build a minimal UTF-16 command line = the PE path
        int i=0; for(; path[i] && i<2047; i++) g_cmdlineW[i]=(uint16_t)path[i]; g_cmdlineW[i]=0;
    }

    int fd = open(path, O_RDONLY);
    if(fd<0){ ws("open failed\n"); return 1; }
    struct stat st; fstat(fd,&st);
    uint8_t *file = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(file==MAP_FAILED){ ws("mmap file failed\n"); return 1; }

    DOS *dos = (DOS*)file;
    if(dos->e_magic!=0x5A4D){ ws("not MZ\n"); return 1; }
    COFF *coff = (COFF*)(file + dos->e_lfanew);
    if(coff->Sig!=0x00004550){ ws("not PE\n"); return 1; }
    if(coff->Machine!=0x8664){ ws("not x86-64\n"); return 1; }
    OPT64 *opt = (OPT64*)((uint8_t*)coff + sizeof(COFF));
    SEC *sec = (SEC*)((uint8_t*)opt + coff->OptSz);
    g_pref = opt->ImageBase;

    ws("  image base "); whexn(opt->ImageBase); ws("  size "); whex(opt->ImageSize);
    ws("  subsystem "); whex(opt->Subsys);  // 2=GUI 3=console

    // reserve image
    g_base = mmap(0, opt->ImageSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if(g_base==MAP_FAILED){ ws("mmap image failed\n"); return 1; }
    ws("  mapped at  "); whex((uint64_t)g_base);

    // headers + sections
    memcpy(g_base, file, opt->HdrSize);
    for(int i=0;i<coff->NumSec;i++){
        if(sec[i].RawSize) memcpy(g_base+sec[i].VA, file+sec[i].RawPtr, sec[i].RawSize);
    }

    // base relocations
    int64_t delta = (int64_t)(uint64_t)g_base - (int64_t)g_pref;
    uint32_t relrva = opt->Dir[DIR_BASERELOC].VA, relsz = opt->Dir[DIR_BASERELOC].Size;
    if(relrva && delta){
        uint8_t *p = rva(relrva), *end = p+relsz; uint64_t fixups=0;
        while(p < end){
            uint32_t page = *(uint32_t*)p, blk = *(uint32_t*)(p+4);
            uint16_t *ent = (uint16_t*)(p+8); int n = (blk-8)/2;
            for(int i=0;i<n;i++){
                int type = ent[i]>>12, off = ent[i]&0xfff;
                if(type==10){ *(uint64_t*)rva(page+off) += delta; fixups++; } // DIR64
            }
            p += blk;
        }
        ws("  relocated  "); whex(fixups);
    }

    // stub pool
    g_stubpool = mmap(0, 32*8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);

    // imports -> logging stubs
    uint32_t imprva = opt->Dir[DIR_IMPORT].VA;
    if(imprva){
        IMPDESC *d = (IMPDESC*)rva(imprva);
        for(; d->Name; d++){
            const char *dll = (char*)rva(d->Name);
            uint64_t *oft = (uint64_t*)rva(d->OFT ? d->OFT : d->FT);
            uint64_t *ft  = (uint64_t*)rva(d->FT);
            for(int i=0; oft[i]; i++){
                const char *fn = "#ordinal";
                if(!(oft[i]>>63)) fn = (char*)rva((uint32_t)oft[i]+2);
                void *impl = resolve_impl(fn);          // real impl if we have one
                if(impl){ ft[i] = (uint64_t)impl; }
                else if(imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn;
                    ft[i] = (uint64_t)make_stub(imp_count); imp_count++; }
            }
        }
        ws("  bound imps "); whex((uint64_t)imp_count);
    }

    // delay imports: report only (never called by a headless server)
    uint32_t delayrva = opt->Dir[DIR_DELAY].VA;
    if(delayrva){
        DELAYDESC *dd = (DELAYDESC*)rva(delayrva);
        ws("  delay-load (not bound; unused headless):\n");
        for(; dd->Name; dd++){ ws("     "); ws((char*)rva(dd->Name)); ws("\n"); }
    }

    // finalize protections: code RX, writable data RW, else RO
    for(int i=0;i<coff->NumSec;i++){
        int prot = PROT_READ;
        if(sec[i].Chars & 0x20000000) prot |= PROT_EXEC;   // IMAGE_SCN_MEM_EXECUTE
        if(sec[i].Chars & 0x80000000) prot |= PROT_WRITE;  // IMAGE_SCN_MEM_WRITE
        uint32_t len = sec[i].VSize ? sec[i].VSize : sec[i].RawSize;
        len = (len + 0xfff) & ~0xfffu;
        mprotect(g_base+sec[i].VA, len, prot);
    }
    mprotect(g_stubpool, 32*8192, PROT_READ|PROT_EXEC);
    g_pool_exec = 1;   // runtime make_stub (via GetProcAddress) now toggles prot

    // minimal TEB via %gs overlay (proven writable under Rosetta).
    // Windows x64 reads TEB at gs:[0x30]=self, PEB at gs:[0x60].
    static uint8_t teb[0x2000] __attribute__((aligned(16)));
    static uint8_t peb[0x1000] __attribute__((aligned(16)));
    *(uint64_t*)(teb+0x30) = (uint64_t)teb;   // NtTib.Self / TEB self
    *(uint64_t*)(teb+0x60) = (uint64_t)peb;   // ProcessEnvironmentBlock
    __asm__ volatile("movq %0, %%gs:0x30" :: "r"((uint64_t)teb));
    __asm__ volatile("movq %0, %%gs:0x60" :: "r"((uint64_t)peb));
    ws("  TEB @ "); whexn((uint64_t)teb); ws("  PEB @ "); whex((uint64_t)peb);

    // TLS callbacks
    uint32_t tlsrva = opt->Dir[DIR_TLS].VA;
    if(tlsrva){
        TLSDIR *t = (TLSDIR*)rva(tlsrva);
        if(t->Callbacks){
            uint64_t *cb = (uint64_t*)t->Callbacks; // VA already relocated
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
