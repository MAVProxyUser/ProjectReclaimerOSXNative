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
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <dlfcn.h>

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
static int g_prefer_base;   // map next image at its preferred base (exe, halo3.dll)

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
static void  rmrf(const char*);                       // fwd

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

// ---- Win64 -> System V ABI thunk for raw macOS functions (GL via dlsym) ----
// macOS GL funcs are SysV: args in rdi,rsi,rdx,rcx,r8,r9 and rsi/rdi are
// VOLATILE. The engine calls them Win64-style (rcx,rdx,r8,r9; rsi/rdi
// NONVOLATILE). Direct calls pass args in the wrong registers AND let the GL
// func trash rsi/rdi (which the engine holds live). This thunk shuffles the
// first 6 integer args and preserves rsi/rdi (+ xmm6-15). Float-only GL funcs
// keep their xmm0-3 args unchanged, so they work too.
static uint8_t *g_glthunk; static size_t g_glthoff; static int g_glth_exec;
static void *g_thunk_real[2048]; static void *g_thunk_addr[2048]; static int g_nthunk;
static void __attribute__((ms_abi)) i_glShaderSource(unsigned,int,const char*const*,const int*); // fwd
static void __attribute__((ms_abi)) i_glCompileShader(unsigned); // fwd
static unsigned __attribute__((ms_abi)) i_glCreateShaderProgramv(unsigned,int,const char*const*); // fwd
static void *make_gl_thunk(void *real){
    if(!real) return real;
    // Shader-upload interception by POINTER: the engine may request
    // glShaderSource under an alias (e.g. *ARB) that skips the name-based
    // special-cases, so it reaches here as a raw thunk and its #version is
    // never rewritten. Compare the resolved pointer and substitute our
    // version-rewriting wrapper so macOS Core profile (needs >=150) accepts it.
    { static void *r_ss,*r_ssarb,*r_csp; static int res;
      if(!res){ r_ss=dlsym((void*)-2,"glShaderSource"); r_ssarb=dlsym((void*)-2,"glShaderSourceARB"); r_csp=dlsym((void*)-2,"glCreateShaderProgramv"); res=1; }
      if(real==r_ss||real==r_ssarb) return (void*)i_glShaderSource;
      if(real==r_csp) return (void*)i_glCreateShaderProgramv; }
    for(int i=0;i<g_nthunk;i++) if(g_thunk_real[i]==real) return g_thunk_addr[i]; // cache
    if(getenv("PELOAD_TRACE_GL")){ Dl_info di; if(dladdr(real,&di)&&di.dli_sname){ (void)!write(2,"  [thunk] ",10); (void)!write(2,di.dli_sname,strlen(di.dli_sname)); (void)!write(2,"\n",1); } }
    if(g_glthoff+64 > 64*2048) return real;
    if(g_glth_exec) mprotect(g_glthunk,64*2048,PROT_READ|PROT_WRITE);
    uint8_t *p=g_glthunk+g_glthoff; g_glthoff+=64; int o=0;
    uint8_t code[]={
        0x57,                               // push rdi
        0x56,                               // push rsi
        0x48,0x83,0xEC,0x08,                // sub rsp,8  (16-align)
        0x48,0x89,0xCF,                     // mov rdi,rcx   (arg1)
        0x48,0x89,0xD6,                     // mov rsi,rdx   (arg2)
        0x4C,0x89,0xC2,                     // mov rdx,r8    (arg3)
        0x4C,0x89,0xC9,                     // mov rcx,r9    (arg4)
        0x4C,0x8B,0x44,0x24,0x40,           // mov r8,[rsp+0x40]  (arg5)
        0x4C,0x8B,0x4C,0x24,0x48,           // mov r9,[rsp+0x48]  (arg6)
        0x48,0xB8,0,0,0,0,0,0,0,0,          // mov rax, real
        0xFF,0xD0,                          // call rax
        0x48,0x83,0xC4,0x08,                // add rsp,8
        0x5E,                               // pop rsi
        0x5F,                               // pop rdi
        0xC3                                // ret
    };
    for(size_t i=0;i<sizeof code;i++) p[i]=code[i];
    int immoff=0; for(int i=0;i<(int)sizeof code-1;i++) if(code[i]==0x48&&code[i+1]==0xB8){ immoff=i+2; break; }
    *(uint64_t*)(p+immoff)=(uint64_t)real; (void)o;
    if(g_glth_exec) mprotect(g_glthunk,64*2048,PROT_READ|PROT_EXEC);
    if(g_nthunk<2048){ g_thunk_real[g_nthunk]=real; g_thunk_addr[g_nthunk]=p; g_nthunk++; }
    return p;
}

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
// Interlocked SList: simple non-atomic impl (head pointer at offset 0). Enough
// for the engine's lists at single-threaded startup.
static void*  MSABI i_InterlockedPushEntrySList(void *head, void *entry){ void*old=*(void**)head; *(void**)entry=old; *(void**)head=entry; return old; }
static void*  MSABI i_InterlockedPopEntrySList(void *head){ void*e=*(void**)head; if(e)*(void**)head=*(void**)e; return e; }
static void*  MSABI i_InterlockedFlushSList(void *head){ void*e=*(void**)head; *(void**)head=0; return e; }
static uint16_t MSABI i_QueryDepthSList(void *head){ uint16_t d=0; for(void*e=*(void**)head;e;e=*(void**)e) d++; return d; }
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
    if(getenv("PELOAD_TRACE_VA")){ char t[128]; snprintf(t,sizeof t,"  [VirtualAlloc] want=%p size=0x%llx type=0x%x -> %p\n",addr,(unsigned long long)sz,type,p==MAP_FAILED?0:p); ws(t); }
    return p==MAP_FAILED?0:p;
}
static int    MSABI i_VirtualProtect(void *a, uint64_t n, uint32_t np, uint32_t *op){
    if(op)*op=0x40; int pr=PROT_READ; if(np&0xf0)pr|=PROT_EXEC; if(np&0xcc)pr|=PROT_WRITE;
    // mprotect needs a page-aligned base + length; the engine patches its own
    // .text at unaligned addresses (inline hooks), so align down / round up.
    uintptr_t pg=(uintptr_t)a & ~0xfffULL; size_t len=(((uintptr_t)a+(n?n:1))-pg+0xfff)&~0xfffULL;
    // RWX is refused under Rosetta/hardened runtime; drop EXEC (Rosetta executes
    // via translation from readable pages, so RW still runs after restore).
    int r1=mprotect((void*)pg,len,pr), r2=0; if(r1!=0 && (pr&PROT_WRITE)) r2=mprotect((void*)pg,len,PROT_READ|PROT_WRITE);
    if(getenv("PELOAD_TRACE_VA")){ char t[160]; snprintf(t,sizeof t,"  [VirtualProtect] a=%p n=0x%llx np=0x%x -> pg=%p len=0x%zx pr=%d r1=%d(%s) fb=%d\n",a,(unsigned long long)n,np,(void*)pg,len,pr,r1,r1?strerror(errno):"ok",r2); ws(t);}
    return 1; }
static int    MSABI i_VirtualFree(void *a, uint64_t n, uint32_t t){ (void)t; munmap(a,n?n:4096); return 1; }
// VirtualQuery: report a region as FREE or COMMITTED via mach_vm_region, so the
// engine's "find 64 MB free within 2 GB of the game" scan can proceed.
static uint64_t MSABI i_VirtualQuery(void *addr, uint8_t *mbi, uint64_t len){
    if(!mbi||len<48) return 0; memset(mbi,0,48);
    mach_vm_address_t a=(mach_vm_address_t)addr; mach_vm_size_t sz=0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt=VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    kern_return_t kr=mach_vm_region(mach_task_self(),&a,&sz,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&cnt,&obj);
    if(kr!=KERN_SUCCESS){ *(void**)mbi=addr; *(uint64_t*)(mbi+24)=0x40000000ULL; *(uint32_t*)(mbi+32)=0x10000; return 48; } // MEM_FREE to +1GB
    if(a > (mach_vm_address_t)addr){ *(void**)mbi=addr; *(uint64_t*)(mbi+24)=(uint64_t)(a-(mach_vm_address_t)addr); *(uint32_t*)(mbi+32)=0x10000; return 48; } // free gap
    *(void**)mbi=(void*)a; *(void**)(mbi+8)=(void*)a; *(uint32_t*)(mbi+16)=0x40; *(uint64_t*)(mbi+24)=(uint64_t)sz; *(uint32_t*)(mbi+32)=0x1000; *(uint32_t*)(mbi+36)=0x40; *(uint32_t*)(mbi+40)=0x20000; return 48; } // MEM_COMMIT
static void   MSABI i_GlobalMemoryStatus(uint8_t *ms){ if(!ms) return; memset(ms,0,64); *(uint32_t*)ms=64; *(uint32_t*)(ms+4)=50;
    *(uint64_t*)(ms+8)=(uint64_t)16<<30; *(uint64_t*)(ms+16)=(uint64_t)8<<30; } // 16GB total / 8GB avail (MEMORYSTATUS)
static int    MSABI i_GlobalMemoryStatusEx(uint8_t *ms){ if(!ms) return 0; *(uint32_t*)(ms+4)=50;
    *(uint64_t*)(ms+8)=(uint64_t)16<<30; *(uint64_t*)(ms+16)=(uint64_t)8<<30; *(uint64_t*)(ms+24)=(uint64_t)32<<30; *(uint64_t*)(ms+32)=(uint64_t)24<<30; return 1; }

static uint16_t* MSABI i_GetCommandLineW(void){ return g_cmdlineW; }
#define WS2_HANDLE ((void*)0x7732000ULL)
static int name_is_ws2(const char*s); static int name_is_ws2_w(const uint16_t*w); // fwd
static void*  MSABI i_GetModuleHandleW(uint16_t *n){ if(name_is_ws2_w(n)) return WS2_HANDLE; return g_base; } // ws2_32 gets a distinct handle; else image base
static void*  MSABI i_GetModuleHandleA(char *n){ if(name_is_ws2(n)) return WS2_HANDLE; return g_base; }

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
// Read/WriteProcessMemory: the engine probe reads/patches halo3.dll in THIS
// process (shell-callback mechanism), so these are plain in-process memcpy.
static int MSABI i_ReadProcessMemory(void*h,const void*addr,void*buf,uint64_t n,uint64_t*rd){ (void)h;
    if(getenv("PELOAD_TRACE_RPM")){ char t[160]; uint64_t a=(uint64_t)addr; const char*mod=(a>=0x180000000&&a<0x187000000)?"halo3":(a>=0x140000000&&a<0x14c000000)?"exe":"?";
        uint64_t off=(a>=0x180000000&&a<0x187000000)?a-0x180000000:(a>=0x140000000&&a<0x14c000000)?a-0x140000000:a;
        snprintf(t,sizeof t,"  [RPM] %s+0x%llx n=0x%llx\n",mod,(unsigned long long)off,(unsigned long long)n); ws(t);}
    memcpy(buf,addr,n); if(rd)*rd=n; return 1; }
static int MSABI i_WriteProcessMemory(void*h,void*addr,const void*buf,uint64_t n,uint64_t*wr){ (void)h;
    uintptr_t pg=(uintptr_t)addr & ~0xfffULL; size_t len=(((uintptr_t)addr+n)-pg+0xfff)&~0xfffULL;
    if(mprotect((void*)pg,len,PROT_READ|PROT_WRITE|PROT_EXEC)!=0) mprotect((void*)pg,len,PROT_READ|PROT_WRITE);
    memcpy(addr,buf,n); mprotect((void*)pg,len,PROT_READ|PROT_EXEC); if(wr)*wr=n; return 1; }
static void*  MSABI i_GetCurrentThread(void){ return (void*)-2; }
// FlushInstructionCache: the engine calls this after patching its own .text
// (inline hooks). Under Rosetta the x86 code is translated to arm64 and cached,
// so a write to a code page leaves a STALE translation unless we invalidate it.
// sys_icache_invalidate is the signal Rosetta honors to drop the translation.
extern void sys_icache_invalidate(void *start, size_t len);
static int MSABI i_FlushInstructionCache(void *proc, void *base, uint64_t sz){ (void)proc;
    if(base&&sz) sys_icache_invalidate(base,(size_t)sz);
    if(getenv("PELOAD_TRACE_VA")){ char t[96]; snprintf(t,sizeof t,"  [FlushInstructionCache] base=%p sz=0x%llx\n",base,(unsigned long long)sz); ws(t);} return 1; }
// ---- minimal WASAPI COM ----------------------------------------------------
// cpal (Reclaimer's audio crate) does CoCreateInstance(MMDeviceEnumerator) and
// .unwrap()s it. We hand back a real IMMDeviceEnumerator COM object that reports
// NO endpoints and no default device, so cpal yields "no output device" instead
// of panicking; the menu then runs silently. COM methods are Win64 ABI (this in
// rcx), return HRESULT (32-bit). S_OK=0.
typedef struct { uint32_t D1; uint16_t D2,D3; uint8_t D4[8]; } GUID_;
typedef struct { void **vtbl; } ComObj;
#define HR_OK 0
#define HR_NOTFOUND 0x80070490
#define HR_INVALIDARG 0x80070057
#define HR_CLASSNOTREG 0x80040154
static int MSABI com_QI(void*self, GUID_*riid, void**ppv){ (void)riid; if(ppv)*ppv=self; return HR_OK; }
static unsigned MSABI com_AddRef(void*self){ (void)self; return 1; }
static unsigned MSABI com_Release(void*self){ (void)self; return 1; }
// IMMDeviceCollection — always empty
static int MSABI mmdc_GetCount(void*self, unsigned*pc){ (void)self; if(pc)*pc=0; return HR_OK; }
static int MSABI mmdc_Item(void*self, unsigned i, void**pp){ (void)self;(void)i; if(pp)*pp=0; return HR_INVALIDARG; }
static void* g_mmdc_vtbl[] = { (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)mmdc_GetCount,(void*)mmdc_Item };
static ComObj g_mmdc = { g_mmdc_vtbl };
// IMMDeviceEnumerator — empty collection, no default endpoint
static int MSABI mmde_EnumAudioEndpoints(void*self,int df,unsigned sm,void**pp){ (void)self;(void)df;(void)sm; if(pp)*pp=&g_mmdc; return HR_OK; }
static int MSABI mmde_GetDefaultAudioEndpoint(void*self,int df,int role,void**pp){ (void)self;(void)df;(void)role; if(pp)*pp=0; return HR_NOTFOUND; }
static int MSABI mmde_GetDevice(void*self,const unsigned short*id,void**pp){ (void)self;(void)id; if(pp)*pp=0; return HR_NOTFOUND; }
static int MSABI mmde_RegCB(void*self,void*c){ (void)self;(void)c; return HR_OK; }
static int MSABI mmde_UnregCB(void*self,void*c){ (void)self;(void)c; return HR_OK; }
static void* g_mmde_vtbl[] = { (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)mmde_EnumAudioEndpoints,(void*)mmde_GetDefaultAudioEndpoint,(void*)mmde_GetDevice,(void*)mmde_RegCB,(void*)mmde_UnregCB };
static ComObj g_mmde = { g_mmde_vtbl };
static int   MSABI i_CoInitializeEx(void*p,uint32_t m){ (void)p;(void)m; return HR_OK; }
static void  MSABI i_CoUninitialize(void){ }
static void* MSABI i_CoTaskMemAlloc(uint64_t n){ return malloc(n?n:1); }
static void  MSABI i_CoTaskMemFree(void*p){ free(p); }
static int   MSABI i_PropVariantClear(void*p){ (void)p; return HR_OK; }
static int   MSABI i_CoCreateInstance(GUID_*clsid, void*outer, uint32_t ctx, GUID_*iid, void**ppv){
    (void)outer;(void)ctx;(void)iid;
    // CLSID_MMDeviceEnumerator = BCDE0395-E52F-467C-8E3D-C4579291692E
    if(clsid && clsid->D1==0xBCDE0395u && clsid->D2==0xE52F && clsid->D3==0x467C){ if(ppv)*ppv=&g_mmde; return HR_OK; }
    if(getenv("PELOAD_TRACE_VA")&&clsid){ char t[96]; snprintf(t,sizeof t,"  [CoCreateInstance] clsid=%08x-%04x-%04x unhandled\n",clsid->D1,clsid->D2,clsid->D3); ws(t);}
    if(ppv)*ppv=0; return HR_CLASSNOTREG; }
// ---- ws2_32 (Winsock) consistent resolution --------------------------------
// Reclaimer's --native-udp gate reads halo3.dll's WS2_32 IAT and compares each
// slot (esp. `bind`, ordinal 2) against the function it independently resolves
// via GetModuleHandle("ws2_32")+GetProcAddress. If import-binding and
// GetProcAddress return DIFFERENT stubs for the same function, the gate reports
// "Native transport instruction/import mismatch; refused". So every ws2_32
// function resolves through ONE ordinal/name-keyed table shared by bind_imports
// and GetProcAddress, behind a distinct module handle (WS2_HANDLE, above).
static void* g_ws2ord[600];
static void* ws2_by_ordinal(uint32_t ord){
    if(ord>=600) ord=0; if(g_ws2ord[ord]) return g_ws2ord[ord];
    if(imp_count<8192){ imp_dll[imp_count]="ws2_32.dll"; imp_fn[imp_count]="#ws2ord"; g_ws2ord[ord]=make_stub(imp_count++); }
    return g_ws2ord[ord]; }
static uint32_t ws2_name2ord(const char*n){
    struct{const char*n;uint32_t o;} m[]={{"accept",1},{"bind",2},{"closesocket",3},{"connect",4},
     {"getpeername",5},{"getsockname",6},{"getsockopt",7},{"htonl",8},{"htons",9},{"ioctlsocket",10},
     {"inet_addr",11},{"inet_ntoa",12},{"listen",13},{"ntohl",14},{"ntohs",15},{"recv",16},{"recvfrom",17},
     {"select",18},{"send",19},{"sendto",20},{"setsockopt",21},{"shutdown",22},{"socket",23},
     {"WSAGetLastError",111},{"WSAStartup",115},{"WSACleanup",116},{"__WSAFDIsSet",151},{0,0}};
    for(int i=0;m[i].n;i++) if(!strcmp(n,m[i].n)) return m[i].o; return 0; }
static void* ws2_resolve_name(const char*n){
    uint32_t o=ws2_name2ord(n); if(o) return ws2_by_ordinal(o);
    static struct{const char*n;void*p;} nc[64]; static int ncn;      // by-name only (WSAIoctl, getaddrinfo, …)
    for(int i=0;i<ncn;i++) if(!strcmp(nc[i].n,n)) return nc[i].p;
    void*p=0; if(imp_count<8192){ imp_dll[imp_count]="ws2_32.dll"; imp_fn[imp_count]=n; p=make_stub(imp_count++); }
    if(ncn<64){ nc[ncn].n=n; nc[ncn].p=p; ncn++; } return p; }
// is this (ascii) module name ws2_32 (with/without path or .dll, case-insensitive)?
static int name_is_ws2(const char*s){ if(!s) return 0; const char*b=s; for(const char*p=s;*p;p++) if(*p=='\\'||*p=='/') b=p+1;
    char low[32]; int i=0; for(; b[i] && i<31; i++){ char c=b[i]; if(c>='A'&&c<='Z') c+=32; low[i]=c; } low[i]=0;
    return !strcmp(low,"ws2_32")||!strcmp(low,"ws2_32.dll"); }
static int name_is_ws2_w(const uint16_t*w){ if(!w) return 0; char a[32]; int i=0; for(; w[i] && i<31; i++) a[i]=(char)(w[i]&0xff); a[i]=0; return name_is_ws2(a); }
// pointer obfuscation: identity is a valid implementation
static void*  MSABI i_EncodePointer(void *p){ return p; }
static void*  MSABI i_DecodePointer(void *p){ return p; }
// critical sections / SRW / condvar / init-once: no-ops are correct single-threaded
static void   MSABI i_noop(void){ }
static int    MSABI i_ret1(void){ return 1; }
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
static int    MSABI i_SetEnvironmentVariableW(const uint16_t*n,const uint16_t*v){
    char nm[256]; int i=0; for(; n&&n[i]&&i<255;i++) nm[i]=(char)(n[i]<128?n[i]:'?'); nm[i]=0;
    if(!v){ unsetenv(nm); return 1; }
    char val[2048]; int j=0; for(; v[j]&&j<2047;j++) val[j]=(char)(v[j]<128?v[j]:'?'); val[j]=0;
    setenv(nm,val,1); if(getenv("PELOAD_TRACE_ENV")){ws("  [SetEnv] ");ws(nm);ws("=");ws(val);ws("\n");} return 1; }
static void*  MSABI i_SetStdHandle(uint32_t id,void*h){ (void)id; return h; } // accept redirection
// CreateProcessW: launch the engine worker UNDER the loader. The worker is a
// Windows exe; we re-exec peload on it (arch -x86_64) so it gets the same
// Win64 environment. Child stdio is wired from STARTUPINFOW's std handles.
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
static const char *g_self;          // path to this peload binary (argv[0])
static pid_t g_childpid[256]; static int g_nchild;
// cache reaped child exit statuses so WaitForSingleObject + GetExitCodeProcess
// (two waitpid callers) don't double-reap and fabricate garbage codes.
static struct { pid_t pid; int reaped; uint32_t code; } g_childst[256]; static int g_nchildst;
static int child_reap(pid_t pid,uint32_t *out,int block){
    for(int i=0;i<g_nchildst;i++) if(g_childst[i].pid==pid && g_childst[i].reaped){ if(out)*out=g_childst[i].code; return 1; }
    int st; pid_t r=waitpid(pid,&st,block?0:WNOHANG);
    if(r==pid){ uint32_t c = WIFEXITED(st)?WEXITSTATUS(st): WIFSIGNALED(st)?(0xC0000000u|WTERMSIG(st)):0;
        if(g_nchildst<256){ g_childst[g_nchildst].pid=pid; g_childst[g_nchildst].reaped=1; g_childst[g_nchildst].code=c; g_nchildst++; }
        if(out)*out=c; return 1; }
    return 0; } // 0 = still running (or gone)
static int MSABI i_CreateProcessW(const uint16_t*app,uint16_t*cmd,void*pa,void*ta,int inherit,uint32_t flags,void*env,const uint16_t*cwd,uint8_t*si,uint8_t*pi){
    (void)pa;(void)ta;(void)inherit;
    // lpEnvironment: the parent's env block for the worker carries the native
    // handshake (RECLAIMER_LOCAL_RUNTIME, RECLAIMER_PARENT_CONTROL). Overlay it
    // onto our environ so the posix_spawn'd worker inherits it. UTF-16 when
    // CREATE_UNICODE_ENVIRONMENT (0x400); double-NUL terminated.
    if(env){ int uni = (flags & 0x400)!=0;
        if(uni){ const uint16_t *w=env; while(*w){ char nm[256],val[4096]; int i=0;
            while(*w && *w!='=' && i<255){ nm[i++]=(char)(*w<128?*w:'?'); w++; } nm[i]=0; if(*w=='=')w++;
            int j=0; while(*w && j<4095){ val[j++]=(char)(*w<128?*w:'?'); w++; } val[j]=0; w++;
            if(nm[0]) setenv(nm,val,1); if(getenv("PELOAD_TRACE_ENV")){ws("  [childenv] ");ws(nm);ws("=");ws(val);ws("\n");} } }
        else { const char *c=env; while(*c){ char nm[256],val[4096]; int i=0;
            while(*c && *c!='=' && i<255) nm[i++]=*c++; nm[i]=0; if(*c=='=')c++;
            int j=0; while(*c && j<4095) val[j++]=*c++; val[j]=0; c++;
            if(nm[0]) setenv(nm,val,1); } } }
    // resolve the worker exe path (prefer lpApplicationName)
    char exe[2048]={0};
    if(app) win2unix(app,exe,sizeof exe);
    else if(cmd){ // first token of the command line, honoring quotes
        char c[4096]; int n=0; for(; cmd[n]&&n<4095;n++) c[n]=(char)(cmd[n]<128?cmd[n]:'?'); c[n]=0;
        char *p=c, *e; if(*p=='"'){ p++; e=strchr(p,'"'); } else { e=strchr(p,' '); } if(e)*e=0;
        uint16_t w[1024]; int i=0; for(; p[i]&&i<1023;i++) w[i]=(uint8_t)p[i]; w[i]=0; win2unix(w,exe,sizeof exe);
    }
    // narrow command line (for logging + to append as a single arg)
    char cl[4096]={0}; if(cmd){ int n=0; for(; cmd[n]&&n<4095;n++) cl[n]=(char)(cmd[n]<128?cmd[n]:'?'); cl[n]=0; }
    ws("  [CreateProcessW] exe="); ws(exe[0]?exe:"(from cmdline)"); ws("\n    cmd="); ws(cl); ws("\n");
    if(!exe[0] || access(exe,R_OK)!=0){ g_lasterr=2; return 0; }
    // child stdio from STARTUPINFOW (x64: dwFlags@60, hStdInput@80/out@88/err@96)
    int cfd[3]={-1,-1,-1};
    if(si){ uint32_t dwFlags=*(uint32_t*)(si+60); if(dwFlags&0x100){ // STARTF_USESTDHANDLES
        for(int k=0;k<3;k++){ intptr_t hv=*(intptr_t*)(si+80+8*k); if(hv>=0x1000) cfd[k]=(int)(hv-0x1000); else if(hv==1)cfd[k]=0; else if(hv==2)cfd[k]=1; else if(hv==3)cfd[k]=2; } } }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    if(!getenv("PELOAD_WORKER_INHERIT_STDIO")) // debug: let worker inherit our terminal
        for(int k=0;k<3;k++){ if(cfd[k]>=0) posix_spawn_file_actions_adddup2(&fa,cfd[k],k); }
    if(cwd){ char wd[2048]; win2unix(cwd,wd,sizeof wd); posix_spawn_file_actions_addchdir(&fa,wd);
        ws("    cwd="); ws(wd); ws("\n"); }
    // Tokenize the Windows command line (honoring double-quotes), DROP token0
    // (the original exe path), keep the worker's real args. Then re-exec the
    // loader: arch -x86_64 <peload> <unix-exe> <arg1> <arg2> ...
    static char tokbuf[4096]; char *tok[64]; int nt=0;
    { int bi=0; char *p=cl; while(*p){ while(*p==' ')p++; if(!*p)break;
        char *start=&tokbuf[bi]; if(*p=='"'){ p++; while(*p&&*p!='"'&&bi<4094) tokbuf[bi++]=*p++; if(*p=='"')p++; }
        else { while(*p&&*p!=' '&&bi<4094) tokbuf[bi++]=*p++; }
        tokbuf[bi++]=0; if(nt<63) tok[nt++]=start; } }
    // exec peload DIRECTLY (x86_64 -> Rosetta auto-engages). NOT via /usr/bin/arch,
    // which is an Apple platform binary and strips DYLD_INSERT_LIBRARIES.
    char *av[70]; int ai=0; av[ai++]=(char*)g_self; av[ai++]=exe;
    for(int k=1;k<nt && ai<69;k++) av[ai++]=tok[k];   // worker args (skip token0=exe)
    av[ai]=0;
    pid_t pid=0; int rc=posix_spawn(&pid,g_self,&fa,0,av,environ);
    posix_spawn_file_actions_destroy(&fa);
    if(rc!=0){ g_lasterr=win_err(); return 0; }
    if(g_nchild<256) g_childpid[g_nchild++]=pid;
    if(pi){ *(void**)(pi+0)=(void*)(intptr_t)(0x8000+pid); *(void**)(pi+8)=(void*)(intptr_t)(0x8000+pid);
            *(uint32_t*)(pi+16)=(uint32_t)pid; *(uint32_t*)(pi+20)=(uint32_t)pid; }
    return 1; }
static uint32_t MSABI i_GetExitCodeProcessReal(void*h,uint32_t*code){ intptr_t hv=(intptr_t)h;
    if(hv>=0x8000 && hv<0x100000){ pid_t pid=(pid_t)(hv-0x8000); uint32_t c;
        if(child_reap(pid,&c,0)){ if(code)*code=c; if(getenv("PELOAD_TRACE_GL")){char t[48];snprintf(t,sizeof t,"  [worker exit] code=0x%x\n",c);ws(t);} }
        else if(code)*code=259; /*STILL_ACTIVE*/ return 1; }
    if(code)*code=0; return 1; }
static int    MSABI i_DuplicateHandle(void*sp,void*src,void*tp,void**tgt,uint32_t acc,int inh,uint32_t opt){
    (void)sp;(void)tp;(void)acc;(void)inh;(void)opt; intptr_t hv=(intptr_t)src; void*res=src;
    if(hv>=0x1000){ int sfd=(int)(hv-0x1000); int nfd=dup(sfd); if(nfd<0){ g_lasterr=win_err(); return 0; }
        if(sfd<MAXFD && g_fdpath[sfd]) fd_record(nfd, g_fdpath[sfd]); res=(void*)(intptr_t)(0x1000+nfd); }
    if(tgt)*tgt=res; return 1; }
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
// Fault/exception-handler registration: return non-null so the engine's
// "install worker fault handler" succeeds. (Actual Win64 SEH dispatch is a
// later phase; this just lets startup proceed.)
static void  *g_veh[16]; static int g_nveh;
static void*  MSABI i_AddVectoredExceptionHandler(uint32_t first,void*handler){ (void)first; if(g_nveh<16)g_veh[g_nveh++]=handler; return handler?handler:(void*)1; }
static uint32_t MSABI i_RemoveVectoredExceptionHandler(void*h){ (void)h; return 1; }
static void*  MSABI i_SetUnhandledExceptionFilter(void*f){ static void*prev; void*o=prev; prev=f; return o; }
static uint32_t MSABI i_SetErrorMode(uint32_t m){ static uint32_t prev; uint32_t o=prev; prev=m; return o; }
static int    MSABI i_SetThreadErrorMode(uint32_t m,uint32_t*old){ (void)m; if(old)*old=0; return 1; }
static uint16_t MSABI i_GetUserDefaultUILanguage(void){ return 0x409; } // en-US
static uint16_t MSABI i_GetUserDefaultLangID(void){ return 0x409; }
static int    MSABI i_IsValidCodePage(uint32_t cp){ (void)cp; return 1; }
static int    MSABI i_DisableThreadLibraryCalls(void*h){ (void)h; return 1; }
static int    MSABI i_AreFileApisANSI(void){ return 1; }
// Minimal user32/gdi windowing: succeed so the engine can register its class
// and create a window. (A real NSWindow + D3D11->Metal surface is the next
// phase; these let us reach D3D11CreateDevice.)
static uint16_t MSABI i_RegisterClassExW(const void*wc){ (void)wc; return 0xC001; } // non-zero ATOM
static uint16_t MSABI i_RegisterClassW(const void*wc){ (void)wc; return 0xC001; }
static int    MSABI i_UnregisterClassW(const void*a,void*b){ (void)a;(void)b; return 1; }
static void*  MSABI i_LoadIconW(void*h,const void*n){ (void)h;(void)n; return (void*)0x1C01; }
static void*  MSABI i_LoadCursorW(void*h,const void*n){ (void)h;(void)n; return (void*)0x1C02; }
static void*  MSABI i_LoadImageW(void*h,const void*n,uint32_t t,int cx,int cy,uint32_t f){ (void)h;(void)n;(void)t;(void)cx;(void)cy;(void)f; return (void*)0x1C03; }
static void*  MSABI i_GetStockObject(int i){ (void)i; return (void*)0x1C04; }
static void*  MSABI i_CreateWindowExW(uint32_t ex,const void*cls,const void*name,uint32_t style,int x,int y,int w,int h,void*parent,void*menu,void*inst,void*param){
    (void)ex;(void)cls;(void)name;(void)style;(void)x;(void)y;(void)w;(void)h;(void)parent;(void)menu;(void)inst;(void)param; return (void*)0x4857; } // fake HWND
static int    MSABI i_DestroyWindow(void*h){ (void)h; return 1; }
static int64_t MSABI i_DefWindowProcW(void*h,uint32_t m,uint64_t wp,int64_t lp){ (void)h;(void)m;(void)wp;(void)lp; return 0; }
static int    MSABI i_ShowWindow(void*h,int c){ (void)h;(void)c; return 1; }
static int    MSABI i_UpdateWindow(void*h){ (void)h; return 1; }
static int    MSABI i_PeekMessageW(void*m,void*h,uint32_t f1,uint32_t f2,uint32_t r){ (void)m;(void)h;(void)f1;(void)f2;(void)r; return 0; } // no messages
static int    MSABI i_GetMessageW(void*m,void*h,uint32_t f1,uint32_t f2){ (void)m;(void)h;(void)f1;(void)f2; return 0; } // WM_QUIT
static int64_t MSABI i_DispatchMessageW(const void*m){ (void)m; return 0; }
static int    MSABI i_TranslateMessage(const void*m){ (void)m; return 0; }
static void*  MSABI i_DefRawHandle(void){ return (void*)0x4857; }
// display/monitor: report a single 1920x1080 primary monitor
static void*  MSABI i_MonitorFromWindow(void*h,uint32_t f){ (void)h;(void)f; return (void*)0x4D01; }
static void*  MSABI i_MonitorFromPoint(uint64_t pt,uint32_t f){ (void)pt;(void)f; return (void*)0x4D01; }
static void*  MSABI i_MonitorFromRect(const void*r,uint32_t f){ (void)r;(void)f; return (void*)0x4D01; }
static int    MSABI i_GetMonitorInfoW(void*h,uint8_t*mi){ (void)h; if(!mi) return 0; uint32_t cb=*(uint32_t*)mi;
    int32_t *r=(int32_t*)(mi+4);  r[0]=0;r[1]=0;r[2]=1920;r[3]=1080;      // rcMonitor
    int32_t *wk=(int32_t*)(mi+20); wk[0]=0;wk[1]=0;wk[2]=1920;wk[3]=1080; // rcWork
    *(uint32_t*)(mi+36)=1;                                               // MONITORINFOF_PRIMARY
    if(cb>=104){ uint16_t*dev=(uint16_t*)(mi+40); const char*d="\\\\.\\DISPLAY1"; int i=0; for(;d[i];i++)dev[i]=(uint8_t)d[i]; dev[i]=0; }
    return 1; }
static int    MSABI i_GetSystemMetrics(int idx){ switch(idx){ case 0:return 1920; case 1:return 1080; case 4:return 0; default:return 0; } }
static int    MSABI i_EnumDisplayMonitors(void*dc,const void*clip,void*cb,int64_t data){ (void)dc;(void)clip;(void)cb;(void)data; return 1; }
static int    MSABI i_GetClientRect(void*h,int32_t*r){ (void)h; if(r){r[0]=0;r[1]=0;r[2]=1920;r[3]=1080;} return 1; }
static int    MSABI i_GetWindowRect(void*h,int32_t*r){ (void)h; if(r){r[0]=0;r[1]=0;r[2]=1920;r[3]=1080;} return 1; }
static int    MSABI i_SetWindowPos(void*h,void*after,int x,int y,int w,int ht,uint32_t f){ (void)h;(void)after;(void)x;(void)y;(void)w;(void)ht;(void)f; return 1; }
static int64_t g_winlong[8];
static int64_t MSABI i_GetWindowLongPtrW(void*h,int idx){ (void)h; return (idx>=-8&&idx<0)? g_winlong[-idx-1]:0; }
static int64_t MSABI i_SetWindowLongPtrW(void*h,int idx,int64_t v){ (void)h; if(idx>=-8&&idx<0){ int64_t o=g_winlong[-idx-1]; g_winlong[-idx-1]=v; return o;} return 0; }
static int32_t MSABI i_GetWindowLongW(void*h,int idx){ return (int32_t)i_GetWindowLongPtrW(h,idx); }
static int32_t MSABI i_SetWindowLongW(void*h,int idx,int32_t v){ return (int32_t)i_SetWindowLongPtrW(h,idx,v); }
static int    MSABI i_AdjustWindowRectEx(int32_t*r,uint32_t s,int m,uint32_t ex){ (void)r;(void)s;(void)m;(void)ex; return 1; }
static int    MSABI i_MoveWindow(void*h,int x,int y,int w,int ht,int rp){ (void)h;(void)x;(void)y;(void)w;(void)ht;(void)rp; return 1; }
static void*  MSABI i_SetFocus(void*h){ return h; }
static void*  MSABI i_SetCapture(void*h){ return h; }
static int    MSABI i_ReleaseCapture(void){ return 1; }
static int    MSABI i_ClientToScreen(void*h,int32_t*p){ (void)h;(void)p; return 1; }
static int    MSABI i_ScreenToClient(void*h,int32_t*p){ (void)h;(void)p; return 1; }
static int    MSABI i_RegisterRawInputDevices(const void*d,uint32_t n,uint32_t cb){ (void)d;(void)n;(void)cb; return 1; }
static uint32_t MSABI i_GetRawInputData(void*h,uint32_t cmd,void*data,uint32_t*sz,uint32_t hdr){ (void)h;(void)cmd;(void)data;(void)sz;(void)hdr; return 0; }
static uint32_t MSABI i_GetRegisteredRawInputDevices(void*d,uint32_t*n,uint32_t cb){ (void)d;(void)cb; if(n)*n=0; return 0; }
static void*  MSABI i_SetTimer(void*h,uintptr_t id,uint32_t el,void*fn){ (void)h;(void)el;(void)fn; return (void*)id; }
static int    MSABI i_KillTimer(void*h,uintptr_t id){ (void)h;(void)id; return 1; }
static int16_t MSABI i_GetAsyncKeyState(int k){ (void)k; return 0; }
static int16_t MSABI i_GetKeyState(int k){ (void)k; return 0; }
static int    MSABI i_ShowCursor(int s){ (void)s; return s?0:-1; }
static int    MSABI i_ClipCursor(const void*r){ (void)r; return 1; }
static int    MSABI i_SetCursorPos(int x,int y){ (void)x;(void)y; return 1; }
static void*  MSABI i_SetCursor(void*c){ return c; }
static int    MSABI i_SetLayeredWindowAttributes(void*h,uint32_t key,uint8_t a,uint32_t f){ (void)h;(void)key;(void)a;(void)f; return 1; }
static int    MSABI i_UpdateLayeredWindow(void*h,void*dc,void*pd,void*ps,void*sd,void*sp,uint32_t key,void*bf,uint32_t f){ (void)h;(void)dc;(void)pd;(void)ps;(void)sd;(void)sp;(void)key;(void)bf;(void)f; return 1; }
static void*  MSABI i_GetDC(void*h){ (void)h; return (void*)0xDC01; }
static int    MSABI i_ReleaseDC(void*h,void*dc){ (void)h;(void)dc; return 1; }
static void*  MSABI i_GetWindowDC(void*h){ (void)h; return (void*)0xDC01; }
static int    MSABI i_InvalidateRect(void*h,const void*r,int e){ (void)h;(void)r;(void)e; return 1; }
static int    MSABI i_SetWindowTextW(void*h,const uint16_t*t){ (void)h;(void)t; return 1; }
static void*  MSABI i_GetForegroundWindow(void){ return (void*)0x4857; }
static int    MSABI i_SetForegroundWindow(void*h){ (void)h; return 1; }
static int    MSABI i_IsWindow(void*h){ return h?1:0; }
static int    MSABI i_IsWindowVisible(void*h){ (void)h; return 1; }
static void*  MSABI i_GetActiveWindow(void){ return (void*)0x4857; }
// ---- OpenGL bridge: wgl -> CGL, and opengl32!gl* -> macOS OpenGL.framework.
// The engine's menu renders via OpenGL; macOS has native GL 4.1 (CGL).
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
static CGLContextObj g_cgl;
static int    MSABI i_ChoosePixelFormat(void*hdc,const void*pfd){ (void)hdc;(void)pfd; return 1; }
static int    MSABI i_SetPixelFormat(void*hdc,int fmt,const void*pfd){ (void)hdc;(void)fmt;(void)pfd; return 1; }
static int    MSABI i_GetPixelFormat(void*hdc){ (void)hdc; return 1; }
static int    MSABI i_DescribePixelFormat(void*hdc,int fmt,uint32_t bytes,uint8_t*pfd){ (void)hdc;(void)fmt;
    if(pfd&&bytes>=40){ memset(pfd,0,bytes); *(uint16_t*)pfd=(uint16_t)(bytes<40?bytes:40); *(uint16_t*)(pfd+2)=1; *(uint32_t*)(pfd+4)=0x25; pfd[8]=32; pfd[23]=24; pfd[27]=8; } return 1; } // PFD_DRAW_TO_WINDOW|SUPPORT_OPENGL|DOUBLEBUFFER, 32bpp, 24 depth
static void*  MSABI i_wglCreateContext(void*hdc){ (void)hdc;
    CGLPixelFormatAttribute at[]={kCGLPFAAccelerated,kCGLPFADoubleBuffer,kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,kCGLPFADepthSize,(CGLPixelFormatAttribute)24,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf=0; GLint n=0; if(CGLChoosePixelFormat(at,&pf,&n)!=kCGLNoError||!pf){ CGLPixelFormatAttribute b[]={kCGLPFAColorSize,(CGLPixelFormatAttribute)24,(CGLPixelFormatAttribute)0}; CGLChoosePixelFormat(b,&pf,&n); }
    CGLContextObj ctx=0; if(pf){ CGLCreateContext(pf,0,&ctx); CGLDestroyPixelFormat(pf); } if(ctx){ g_cgl=ctx; CGLSetCurrentContext(ctx); } // make current now (glow may read GL_VERSION before wglMakeCurrent)
    ws(ctx?"  [wgl] CGL context created + current\n":"  [wgl] CGL context FAILED\n"); return ctx; }
static int    MSABI i_wglMakeCurrent(void*hdc,void*ctx){ (void)hdc; CGLError e=CGLSetCurrentContext((CGLContextObj)ctx);
    if(getenv("PELOAD_TRACE_GL")){ const unsigned char*(*gv)(unsigned)=dlsym(RTLD_DEFAULT,"glGetString");
        const unsigned char*v = (gv&&ctx)? gv(0x1F02/*GL_VERSION*/):0; ws("  [wglMakeCurrent] ctx="); ws(ctx?"set":"null");
        ws(" GL_VERSION="); ws(v?(const char*)v:"(null)"); ws("\n"); }
    return e==kCGLNoError; }
// glGetString wrapper: ensure OUR context is current on the calling thread
// (CGL current-context is per-thread; glow may call GL on a thread where we
// never ran wglMakeCurrent). Then delegate to the real framework glGetString.
static const unsigned char* MSABI i_glGetString(unsigned name){
    if(g_cgl && CGLGetCurrentContext()==0) CGLSetCurrentContext(g_cgl);
    const unsigned char*(*real)(unsigned)=dlsym(RTLD_DEFAULT,"glGetString");
    const unsigned char*r = real? real(name):0;
    // normalize macOS's non-standard strings (parsers choke on "4.1 Metal - 90.5")
    if(name==0x1f02) r=(const unsigned char*)"4.1.0";                 // GL_VERSION
    else if(name==0x8B8C) r=(const unsigned char*)"4.10";             // GL_SHADING_LANGUAGE_VERSION
    if(getenv("PELOAD_TRACE_GL")){ char t[64]; snprintf(t,sizeof t,"  [glGetString] 0x%x cur=%p -> ",name,CGLGetCurrentContext()); ws(t); ws(r?(const char*)r:"(null)"); ws("\n"); }
    return r; }
static void *make_gl_thunk(void *real); // fwd
static void*  MSABI i_wglGetProcAddress(const char*name){
    if(getenv("PELOAD_TRACE_GL2")){ ws("  [wglGP] "); ws(name); ws("\n"); }
    if(!strcmp(name,"glGetString")) return (void*)i_glGetString;
    if(!strcmp(name,"glShaderSource")||!strcmp(name,"glShaderSourceARB")) return (void*)i_glShaderSource;
    if(!strcmp(name,"glCompileShader")||!strcmp(name,"glCompileShaderARB")) return (void*)i_glCompileShader;
    return make_gl_thunk(dlsym(RTLD_DEFAULT,name)); }
static int    MSABI i_wglDeleteContext(void*ctx){ if(ctx)CGLDestroyContext((CGLContextObj)ctx); return 1; }
// glShaderSource: macOS Core profile rejects #version <150. Rewrite the
// version directive in each source string (same length: "130"->"150").
static void MSABI i_glShaderSource(unsigned shader,int count,const char*const*strs,const int*lens){
    if(getenv("PELOAD_TRACE_GL"))(void)!write(2,"  [glShaderSource] ENTER\n",25);
    void (*real)(unsigned,int,const char*const*,const int*)=dlsym(RTLD_DEFAULT,"glShaderSource");
    if(!real||count<=0){ if(real) real(shader,count,strs,lens); return; }
    static __thread char *buf; static __thread const char *nstr[64]; static __thread int nlen[64];
    if(!buf) buf=malloc(1<<20); char *w=buf; int nc=count<64?count:64;
    for(int i=0;i<nc;i++){ const char*s=strs[i]; int L = lens? (lens[i]>=0?lens[i]:(int)strlen(s)) : (int)strlen(s);
        if(w+L+1 > buf+(1<<20)){ nstr[i]=s; nlen[i]=lens?lens[i]:-1; continue; }
        memcpy(w,s,L); w[L]=0;
        // bump "#version NNN" to 150 when NNN<150
        char *v=strstr(w,"#version "); if(v){ char*d=v+9; while(*d==' ')d++;
            if(d[0]>='1'&&d[0]<='9'&&d[1]>='0'&&d[1]<='9'&&d[2]>='0'&&d[2]<='9'){ int ver=(d[0]-'0')*100+(d[1]-'0')*10+(d[2]-'0'); if(ver<150){ d[0]='1';d[1]='5';d[2]='0'; } } }
        nstr[i]=w; nlen[i]=L; w+=L+1; }
    real(shader,nc,nstr,lens?nlen:0);
    if(getenv("PELOAD_TRACE_GL")) ws("  [glShaderSource] rewritten\n");
}
// glCompileShader: fetch the shader's source, bump #version<150 to 150, re-set
// it, then compile — catches the source regardless of how it was uploaded.
static void MSABI i_glCompileShader(unsigned shader){
    if(getenv("PELOAD_TRACE_GL"))(void)!write(2,"  [glCompileShader] ENTER\n",26);
    void(*getiv)(unsigned,unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetShaderiv");
    void(*getsrc)(unsigned,int,int*,char*)=dlsym(RTLD_DEFAULT,"glGetShaderSource");
    void(*setsrc)(unsigned,int,const char*const*,const int*)=dlsym(RTLD_DEFAULT,"glShaderSource");
    int len=0; if(getiv) getiv(shader,0x8B88,&len); // GL_SHADER_SOURCE_LENGTH
    if(getsrc&&setsrc&&len>0&&len<(1<<20)){ char*src=malloc(len+1); int got=0; getsrc(shader,len,&got,src); src[got>0?got:0]=0;
        char*v=strstr(src,"#version "); if(v){ char*dd=v+9; while(*dd==' ')dd++;
            if(dd[0]>='1'&&dd[0]<='9'&&dd[1]>='0'&&dd[1]<='9'&&dd[2]>='0'&&dd[2]<='9'){ int ver=(dd[0]-'0')*100+(dd[1]-'0')*10+(dd[2]-'0');
                if(ver<150){ dd[0]='1';dd[1]='5';dd[2]='0'; const char*a=src; setsrc(shader,1,&a,0); if(getenv("PELOAD_TRACE_GL")){ws("  [glCompileShader] bumped #version ");char n[4]={(char)('0'+ver/100),(char)('0'+ver/10%10),(char)('0'+ver%10),0};ws(n);ws("->150\n");} } } }
        free(src); }
    void(*real)(unsigned)=dlsym(RTLD_DEFAULT,"glCompileShader"); if(real) real(shader); }
// glCreateShaderProgramv: separable-program one-call compile; rewrite #version.
static unsigned MSABI i_glCreateShaderProgramv(unsigned type,int count,const char*const*strs){
    unsigned(*real)(unsigned,int,const char*const*)=dlsym(RTLD_DEFAULT,"glCreateShaderProgramv");
    if(!real) return 0; if(count<=0) return real(type,count,strs);
    static __thread char *buf; static __thread const char *ns[32]; if(!buf) buf=malloc(1<<20); char *w=buf; int nc=count<32?count:32;
    for(int i=0;i<nc;i++){ const char*s=strs[i]; int L=(int)strlen(s); if(w+L+1>buf+(1<<20)){ns[i]=s;continue;} memcpy(w,s,L); w[L]=0;
        char*v=strstr(w,"#version "); if(v){char*d=v+9;while(*d==' ')d++; if(d[0]>='1'&&d[0]<='9'&&d[1]>='0'&&d[1]<='9'&&d[2]>='0'&&d[2]<='9'){int ver=(d[0]-'0')*100+(d[1]-'0')*10+(d[2]-'0'); if(ver<150){d[0]='1';d[1]='5';d[2]='0';}}}
        ns[i]=w; w+=L+1; }
    return real(type,nc,ns); }
static void*  MSABI i_wglGetCurrentContext(void){ return CGLGetCurrentContext(); }
static void*  MSABI i_wglGetCurrentDC(void){ return (void*)0xDC01; }
static int    MSABI i_wglShareLists(void*a,void*b){ (void)a;(void)b; return 1; }
static int    MSABI i_wglSwapIntervalEXT(int i){ (void)i; return 1; }
static int    MSABI i_SwapBuffers(void*hdc){ (void)hdc; if(g_cgl) CGLFlushDrawable(g_cgl); return 1; }
// RaiseException: log the code. 0x406D1388 = SetThreadName (ignore); others we
// just return from for now (real SEH dispatch is a later phase).
// Decode an MSVC C++ throw (code 0xE06D7363). args = {magic, &object, ThrowInfo*,
// imagebase}. ThrowInfo's inner fields are RVAs from imagebase; the first
// CatchableType's TypeDescriptor.name is the mangled C++ type (".?AV<name>@@").
static void   MSABI i_RaiseException(uint32_t code,uint32_t flags,uint32_t n,const void*args){ (void)flags;
    if(code==0x406D1388) return; // SetThreadName
    char t[256]; snprintf(t,sizeof t,"  [RaiseException] 0x%08x (%u args)\n",code,n); ws(t);
    if(code==0xE06D7363 && args && n>=4){
        const uint64_t *a=args; void *obj=(void*)a[1]; int32_t *ti=(int32_t*)a[2]; uint64_t base=a[3];
        if(ti){ int32_t ctaRVA=ti[3]; uint8_t *cta=(uint8_t*)(base+(uint32_t)ctaRVA);
            int nct=*(int32_t*)cta; int32_t *arr=(int32_t*)(cta+4);
            if(nct>0){ uint8_t *ct=(uint8_t*)(base+(uint32_t)arr[0]); int32_t tdRVA=*(int32_t*)(ct+4);
                const char *name=(const char*)(base+(uint32_t)tdRVA+16); // TypeDescriptor.name @ +16 (x64)
                ws("    C++ type: "); ws(name); ws("\n");
                // std::exception-derived: obj+? holds a char* message near the start
                if(obj){ for(int off=8; off<=16; off+=8){ const char *m=*(const char**)((uint8_t*)obj+off);
                    if(m && (uint64_t)m>0x10000){ int ok=1; for(int i=0;i<64;i++){ char c=m[i]; if(c==0)break; if(c<32||c>126){ok=0;break;} }
                        if(ok && m[0]){ ws("    what(): "); ws(m); ws("\n"); break; } } } } } }
    }
}
static void   MSABI i_RtlCaptureContext(void*ctx){ if(ctx) memset(ctx,0,1232); } // CONTEXT (x64 ~1232 bytes)
static void*  MSABI i_RtlLookupFunctionEntry(uint64_t pc,uint64_t*base,void*hist){ (void)pc;(void)hist; if(base)*base=0; return 0; }
static void   MSABI i_RtlRaiseException(void*rec){ (void)rec; }
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
// ============ real synchronization (the engine is multi-threaded) ============
// Sync objects are heap-allocated; the pointer IS the Win32 HANDLE. A magic
// field lets WaitForSingleObject tell them from fd/process pseudo-handles.
#define SYNC_MAGIC 0x53796E63u   // 'Sync'
enum { K_EVENT, K_MUTEX, K_SEMA };
typedef struct { uint32_t magic; int kind; pthread_mutex_t m; pthread_cond_t c; int signaled, manual; long count, maxc; pthread_t owner; } SyncObj;
static SyncObj* sync_new(int kind){ SyncObj*s=calloc(1,sizeof *s); s->magic=SYNC_MAGIC; s->kind=kind;
    pthread_mutex_init(&s->m,0); pthread_cond_init(&s->c,0); return s; }
static int is_sync(intptr_t hv){ if(hv<0x100000) return 0; SyncObj*s=(SyncObj*)hv; return s && s->magic==SYNC_MAGIC; }
static void* MSABI i_CreateEventW(void*sec,int man,int sig,const uint16_t*nm){ (void)sec;(void)nm; SyncObj*s=sync_new(K_EVENT); s->manual=man; s->signaled=sig; return s; }
static void* MSABI i_CreateEventA(void*sec,int man,int sig,const char*nm){ return i_CreateEventW(sec,man,sig,0); }
static void* MSABI i_CreateEventExW(void*sec,const uint16_t*nm,uint32_t fl,uint32_t acc){ (void)nm;(void)acc; return i_CreateEventW(sec,(fl&1)/*MANUAL_RESET*/,(fl&2)/*INITIAL_SET*/,0); }
static int   MSABI i_SetEvent(void*h){ SyncObj*s=h; if(!is_sync((intptr_t)h))return 1; pthread_mutex_lock(&s->m); s->signaled=1; pthread_cond_broadcast(&s->c); pthread_mutex_unlock(&s->m); return 1; }
static int   MSABI i_ResetEvent(void*h){ SyncObj*s=h; if(!is_sync((intptr_t)h))return 1; pthread_mutex_lock(&s->m); s->signaled=0; pthread_mutex_unlock(&s->m); return 1; }
static void* MSABI i_CreateSemaphoreW(void*sec,long init,long max,const uint16_t*nm){ (void)sec;(void)nm; SyncObj*s=sync_new(K_SEMA); s->count=init; s->maxc=max; return s; }
static void* MSABI i_CreateSemaphoreA(void*sec,long init,long max,const char*nm){ return i_CreateSemaphoreW(sec,init,max,0); }
static int   MSABI i_ReleaseSemaphore(void*h,long c,long*prev){ SyncObj*s=h; if(!is_sync((intptr_t)h))return 1; pthread_mutex_lock(&s->m); if(prev)*prev=s->count; s->count+=c; pthread_cond_broadcast(&s->c); pthread_mutex_unlock(&s->m); return 1; }
static void* MSABI i_CreateMutexW(void*sec,int owned,const uint16_t*nm){ (void)sec;(void)nm; SyncObj*s=sync_new(K_MUTEX); s->count=owned?0:1; if(owned)s->owner=pthread_self(); return s; }
static void* MSABI i_CreateMutexA(void*sec,int owned,const char*nm){ return i_CreateMutexW(sec,owned,0); }
static int   MSABI i_ReleaseMutex(void*h){ SyncObj*s=h; if(!is_sync((intptr_t)h))return 1; pthread_mutex_lock(&s->m); s->count=1; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return 1; }
// wait one object with a deadline; returns 0 signaled, 0x102 timeout
static uint32_t sync_wait(SyncObj*s,uint32_t ms){
    struct timespec ts; if(ms!=0xFFFFFFFF){ struct timeval tv; gettimeofday(&tv,0); uint64_t ns=(uint64_t)tv.tv_usec*1000+(uint64_t)ms*1000000ULL; ts.tv_sec=tv.tv_sec+ns/1000000000ULL; ts.tv_nsec=ns%1000000000ULL; }
    pthread_mutex_lock(&s->m); uint32_t rc=0;
    for(;;){ int avail = (s->kind==K_SEMA)? (s->count>0) : (s->kind==K_MUTEX)? (s->count>0) : s->signaled;
        if(avail){ if(s->kind==K_SEMA) s->count--; else if(s->kind==K_MUTEX){ s->count=0; s->owner=pthread_self(); } else if(!s->manual) s->signaled=0; break; }
        if(ms==0){ rc=0x102; break; }
        int w = (ms==0xFFFFFFFF)? pthread_cond_wait(&s->c,&s->m) : pthread_cond_timedwait(&s->c,&s->m,&ts);
        if(w==ETIMEDOUT){ rc=0x102; break; } }
    pthread_mutex_unlock(&s->m); return rc; }
static uint32_t MSABI i_WaitForSingleObject(void*h,uint32_t ms){ intptr_t hv=(intptr_t)h;
    if(is_sync(hv)) return sync_wait((SyncObj*)h,ms);
    if(hv>=0x8000 && hv<0x100000){ pid_t pid=(pid_t)(hv-0x8000);
        if(ms==0) return child_reap(pid,0,0)? 0 : 0x102;  // signaled : timeout
        child_reap(pid,0,1); return 0; }
    return 0; } // unknown handle: treat as already-signaled
static uint32_t MSABI i_WaitForMultipleObjects(uint32_t n,void*const*h,int all,uint32_t ms){
    if(all){ for(uint32_t i=0;i<n;i++){ intptr_t hv=(intptr_t)h[i]; if(is_sync(hv)) sync_wait((SyncObj*)h[i],0xFFFFFFFF); } return 0; }
    for(uint32_t spin=0;;spin++){ for(uint32_t i=0;i<n;i++){ intptr_t hv=(intptr_t)h[i]; if(is_sync(hv)){ if(sync_wait((SyncObj*)h[i],0)==0) return i; } else return i; }
        if(ms==0) return 0x102; struct timespec ts={0,2000000}; nanosleep(&ts,0); if(ms!=0xFFFFFFFF && spin*2>=ms) return 0x102; } }
// ---- critical sections / SRW locks / condition variables (real pthreads) ----
static pthread_mutex_t g_lazy=PTHREAD_MUTEX_INITIALIZER;
static void MSABI i_InitializeCriticalSection(void*cs){ pthread_mutex_t*m=malloc(sizeof *m); pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(m,&a); *(void**)cs=m; }
static int  MSABI i_InitializeCriticalSectionAndSpinCount(void*cs,uint32_t s){ (void)s; i_InitializeCriticalSection(cs); return 1; }
static int  MSABI i_InitializeCriticalSectionEx(void*cs,uint32_t s,uint32_t f){ (void)s;(void)f; i_InitializeCriticalSection(cs); return 1; }
static pthread_mutex_t* cs_m(void*cs){ pthread_mutex_t*m=*(void**)cs; if(!m){ pthread_mutex_lock(&g_lazy); m=*(void**)cs; if(!m){ i_InitializeCriticalSection(cs); m=*(void**)cs; } pthread_mutex_unlock(&g_lazy); } return m; }
static void MSABI i_EnterCriticalSection(void*cs){ pthread_mutex_lock(cs_m(cs)); }
static void MSABI i_LeaveCriticalSection(void*cs){ pthread_mutex_t*m=*(void**)cs; if(m) pthread_mutex_unlock(m); }
static void MSABI i_DeleteCriticalSection(void*cs){ pthread_mutex_t*m=*(void**)cs; if(m){ pthread_mutex_destroy(m); free(m); *(void**)cs=0; } }
static int  MSABI i_TryEnterCriticalSection(void*cs){ return pthread_mutex_trylock(cs_m(cs))==0; }
static pthread_rwlock_t* srw(void*l){ pthread_rwlock_t*r=*(void**)l; if(!r){ pthread_mutex_lock(&g_lazy); r=*(void**)l; if(!r){ r=malloc(sizeof *r); pthread_rwlock_init(r,0); *(void**)l=r; } pthread_mutex_unlock(&g_lazy); } return r; }
static void MSABI i_InitializeSRWLock(void*l){ *(void**)l=0; }
static void MSABI i_AcquireSRWLockExclusive(void*l){ pthread_rwlock_wrlock(srw(l)); }
static void MSABI i_ReleaseSRWLockExclusive(void*l){ pthread_rwlock_unlock(srw(l)); }
static void MSABI i_AcquireSRWLockShared(void*l){ pthread_rwlock_rdlock(srw(l)); }
static void MSABI i_ReleaseSRWLockShared(void*l){ pthread_rwlock_unlock(srw(l)); }
static uint8_t MSABI i_TryAcquireSRWLockExclusive(void*l){ return pthread_rwlock_trywrlock(srw(l))==0; }
static uint8_t MSABI i_TryAcquireSRWLockShared(void*l){ return pthread_rwlock_tryrdlock(srw(l))==0; }
static pthread_cond_t* cvar(void*cv){ pthread_cond_t*c=*(void**)cv; if(!c){ pthread_mutex_lock(&g_lazy); c=*(void**)cv; if(!c){ c=malloc(sizeof *c); pthread_cond_init(c,0); *(void**)cv=c; } pthread_mutex_unlock(&g_lazy); } return c; }
static void MSABI i_InitializeConditionVariable(void*cv){ *(void**)cv=0; }
static void MSABI i_WakeConditionVariable(void*cv){ pthread_cond_signal(cvar(cv)); }
static void MSABI i_WakeAllConditionVariable(void*cv){ pthread_cond_broadcast(cvar(cv)); }
static int  MSABI i_SleepConditionVariableCS(void*cv,void*cs,uint32_t ms){ pthread_cond_t*c=cvar(cv); pthread_mutex_t*m=cs_m(cs);
    if(ms==0xFFFFFFFF) return pthread_cond_wait(c,m)==0; struct timeval tv;gettimeofday(&tv,0);struct timespec ts;uint64_t ns=(uint64_t)tv.tv_usec*1000+(uint64_t)ms*1000000ULL;ts.tv_sec=tv.tv_sec+ns/1000000000ULL;ts.tv_nsec=ns%1000000000ULL; return pthread_cond_timedwait(c,m,&ts)!=ETIMEDOUT; }
static int  MSABI i_SleepConditionVariableSRW(void*cv,void*l,uint32_t ms,uint32_t fl){ pthread_cond_t*c=cvar(cv); pthread_rwlock_t*r=srw(l); int shared=(fl&1);
    pthread_rwlock_unlock(r); pthread_mutex_lock(&g_lazy); int rc;
    if(ms==0xFFFFFFFF) rc=pthread_cond_wait(c,&g_lazy); else { struct timeval tv;gettimeofday(&tv,0);struct timespec ts;uint64_t ns=(uint64_t)tv.tv_usec*1000+(uint64_t)ms*1000000ULL;ts.tv_sec=tv.tv_sec+ns/1000000000ULL;ts.tv_nsec=ns%1000000000ULL; rc=pthread_cond_timedwait(c,&g_lazy,&ts); }
    pthread_mutex_unlock(&g_lazy); if(shared) pthread_rwlock_rdlock(r); else pthread_rwlock_wrlock(r); return rc!=ETIMEDOUT; }
static void MSABI i_WakeByAddressSingle(void*a){ (void)a; }
static void MSABI i_WakeByAddressAll(void*a){ (void)a; }
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
        if(!de){ if(getenv("PELOAD_TRACE_FS")){ws("  [enum-end] ");ws(g_fdpath[fd]?g_fdpath[fd]:"?");ws("\n");} g_lasterr=18; return 0; } // ERROR_NO_MORE_FILES
        if(getenv("PELOAD_TRACE_FS")){ ws("  [enum] "); ws(g_fdpath[fd]?g_fdpath[fd]:"?"); ws(" -> "); ws(de->d_name); ws("\n"); }
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
    (void)sz; intptr_t hv=(intptr_t)h; if(hv<0x1000) return 1; int fd=(int)(hv-0x1000); if(fd>=MAXFD) return 1;
    // FileDispositionInfo(4): BOOLEAN DeleteFile. FileDispositionInfoEx(21):
    // ULONG Flags, bit0 = DELETE. Honor BOTH set and clear (the client marks
    // delete-on-close for crash safety, then clears it to keep the file).
    if(cls==4 && info){ g_fddel[fd] = (*(uint8_t*)info)?1:0; }
    else if(cls==21 && info){ g_fddel[fd] = (*(uint32_t*)info & 0x1)?1:0; }
    if(getenv("PELOAD_TRACE_FS")){ char t[96]; snprintf(t,sizeof t,"  [SetFileInfo] cls=%d d0=0x%x -> del=%d ",cls, info?*(uint32_t*)info:0, (cls==4||cls==21)?g_fddel[fd]:-1); ws(t); ws(g_fdpath[fd]?g_fdpath[fd]:"?"); ws("\n"); }
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
static uint8_t *load_guest(const char *path, const char *name); // fwd (defined near main)
static void *export_lookup(uint8_t *base, const char *want);    // fwd
static void* MSABI i_LoadLibraryExW(const uint16_t *name, void *h, uint32_t fl){
    (void)h;(void)fl; char w[2048]; int i=0; for(; name && name[i] && i<2047; i++) w[i]=(char)(name[i]<128?name[i]:'?'); w[i]=0;
    // bare short name of the DLL (for the module registry + dedupe)
    char *bn=w; for(char*p=w;*p;p++) if(*p=='\\'||*p=='/') bn=p+1;
    snprintf(g_lastlib,sizeof g_lastlib,"%s",bn);
    ws("  [LoadLibraryExW] "); ws(w); ws("\n");
    if(name_is_ws2(bn)) return WS2_HANDLE; // Winsock: distinct handle, resolved by ordinal/name
    for(int m=0;m<g_nmods;m++) if(!strcasecmp(g_mods[m].name,bn)) return g_mods[m].base; // already loaded
    char p[2048]; win2unix(name,p,sizeof p);
    if(access(p,R_OK)==0){ g_prefer_base=1; uint8_t *b=load_guest(p, strdup(bn)); g_prefer_base=0; if(b) return b; }
    return g_base; // feature-probe DLLs (api-ms-*) that aren't real files: placeholder
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
    // Windows device names -> /dev equivalents (NUL always opens on Windows)
    { const char *d=s; if(!strncmp(d,"//./",4)||!strncmp(d,"//.",3)) d+=(d[3]=='/')?4:3;
      if(!strcasecmp(d,"nul")){ snprintf(out,outsz,"/dev/null"); return out; }
      if(!strcasecmp(d,"con")){ snprintf(out,outsz,"/dev/tty"); return out; } }
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
        // delete-on-close with Windows last-handle-wins semantics: only remove
        // when no OTHER open handle still references the same path. This lets
        // the client delete scratch sub-dirs (files & dirs) without yanking the
        // top scratch dir out from under a still-open session handle.
        if(g_fddel[fd]&&g_fdpath[fd]){ char *path=g_fdpath[fd]; int other=0;
            for(int i=0;i<MAXFD;i++) if(i!=fd && g_fdpath[i] && !strcmp(g_fdpath[i],path)){ other=1; break; }
            if(!other){ if(getenv("PELOAD_TRACE_FS")){ws("  [del-on-close] ");ws(path);ws("\n");} struct stat ds; if(stat(path,&ds)==0){ if(S_ISDIR(ds.st_mode)) rmrf(path); else unlink(path); } } }
        free(g_fdpath[fd]); g_fdpath[fd]=0; g_fddel[fd]=0; }
    close(fd); } return 1; }
// env var lookup via the host environment (so RECLAIMER_* pass through)
static uint32_t MSABI i_GetEnvironmentVariableW(const uint16_t*n,uint16_t*v,uint32_t sz){
    char nm[256]; int i=0; for(; n && n[i] && i<255; i++) nm[i]=(char)(n[i]<128?n[i]:'?'); nm[i]=0;
    const char *val=getenv(nm); if(getenv("PELOAD_TRACE_ENV")){ws("  [GetEnv] ");ws(nm);ws("=");ws(val?val:"(unset)");ws("\n");} if(!val){ return 0; }
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
static uint32_t MSABI i_GetCurrentDirectoryW(uint32_t sz,uint16_t*buf){ char cwd[2048]; if(!getcwd(cwd,sizeof cwd)) return 0; uint16_t w[2048]; unix2win(cwd,w,2048); uint32_t n=0; while(w[n])n++; if(sz<n+1) return n+1; for(uint32_t i=0;i<=n;i++) buf[i]=w[i]; return n; }
static int MSABI i_SetCurrentDirectoryW(const uint16_t*w){ char p[2048]; return chdir(win2unix(w,p,sizeof p))==0 || (g_lasterr=win_err(),0); }

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
static void fill_find(Find*f,struct dirent*de,void*fd){ if(getenv("PELOAD_TRACE_FS")){ ws("  [find] dir="); ws(f->dir); ws(" pat="); ws(f->pat); ws(" -> "); ws(de->d_name); ws("\n"); }
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
static const unsigned char* MSABI i_glGetString(unsigned name); // fwd (GL bridge)
static void MSABI i_glShaderSource(unsigned,int,const char*const*,const int*); // fwd
static void MSABI i_glCompileShader(unsigned); // fwd
static void* ws2_by_ordinal(uint32_t ord); static void* ws2_resolve_name(const char*n); // fwd
static void* MSABI i_GetProcAddress(void *mod, char *name){
    if(getenv("PELOAD_GPA_ALL") && (uint64_t)name>0x10000){ char h[16]; int n=snprintf(h,sizeof h,"[GPA %d] ",getpid()); (void)!write(2,h,n); ws(name); ws("\n"); }
    // Winsock: resolve through the shared ws2_32 table so the IAT entry and this
    // result are the SAME pointer (the --native-udp gate compares them).
    if(mod==WS2_HANDLE) return ((uint64_t)name<0x10000)? ws2_by_ordinal((uint32_t)(uintptr_t)name) : ws2_resolve_name(name);
    if((uint64_t)name < 0x10000){ // imported by ordinal
        if(imp_count<8192){ imp_dll[imp_count]="(dynamic)"; imp_fn[imp_count]="#ordinal";
            return make_stub(imp_count++); }
        return 0;
    }
    // real export from the requested module (e.g. halo3.dll!CreateGameEngine)
    if((uint64_t)mod>0x10000){ void *e=export_lookup((uint8_t*)mod,name); if(e) return e; }
    for(int m=0;m<g_nmods;m++){ void *e=export_lookup(g_mods[m].base,name); if(e) return e; }
    void *r = resolve_impl(name);
    if(r) return r;
    // OpenGL: route gl*/wgl* to macOS OpenGL.framework (glow/egui resolve core
    // GL functions like glGetString via GetProcAddress, not just imports)
    if(!strcmp(name,"glGetString")) return (void*)i_glGetString; // ensure-current wrapper
    if(!strcmp(name,"glShaderSource")) return (void*)i_glShaderSource;
    if(!strcmp(name,"glCompileShader")) return (void*)i_glCompileShader;
    if((name[0]=='g'&&name[1]=='l')||(name[0]=='w'&&name[1]=='g'&&name[2]=='l')){ void *g=dlsym((void*)-2,name);
        if(getenv("PELOAD_TRACE_GL")){ ws("  [GLproc] "); ws(name); ws(g?"\n":" MISSING\n"); } if(g) return make_gl_thunk(g); }
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
        {"InterlockedPushEntrySList",(void*)i_InterlockedPushEntrySList},
        {"InterlockedPopEntrySList",(void*)i_InterlockedPopEntrySList},
        {"InterlockedFlushSList",(void*)i_InterlockedFlushSList},
        {"QueryDepthSList",(void*)i_QueryDepthSList},
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
        {"VirtualQuery",(void*)i_VirtualQuery},
        {"GlobalMemoryStatus",(void*)i_GlobalMemoryStatus},
        {"GlobalMemoryStatusEx",(void*)i_GlobalMemoryStatusEx},
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
        {"SetCurrentDirectoryW",(void*)i_SetCurrentDirectoryW},
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
        {"FlushInstructionCache",(void*)i_FlushInstructionCache},
        {"CoInitializeEx",(void*)i_CoInitializeEx},
        {"CoUninitialize",(void*)i_CoUninitialize},
        {"CoCreateInstance",(void*)i_CoCreateInstance},
        {"CoTaskMemAlloc",(void*)i_CoTaskMemAlloc},
        {"CoTaskMemFree",(void*)i_CoTaskMemFree},
        {"PropVariantClear",(void*)i_PropVariantClear},
        {"GetCurrentProcess",(void*)i_GetCurrentProcess},
        {"ReadProcessMemory",(void*)i_ReadProcessMemory},
        {"WriteProcessMemory",(void*)i_WriteProcessMemory},
        {"GetCurrentThread",(void*)i_GetCurrentThread},
        {"EncodePointer",(void*)i_EncodePointer},
        {"DecodePointer",(void*)i_DecodePointer},
        {"InitializeCriticalSection",(void*)i_InitializeCriticalSection},
        {"InitializeCriticalSectionAndSpinCount",(void*)i_InitializeCriticalSectionAndSpinCount},
        {"InitializeCriticalSectionEx",(void*)i_InitializeCriticalSectionEx},
        {"EnterCriticalSection",(void*)i_EnterCriticalSection},
        {"LeaveCriticalSection",(void*)i_LeaveCriticalSection},
        {"DeleteCriticalSection",(void*)i_DeleteCriticalSection},
        {"TryEnterCriticalSection",(void*)i_TryEnterCriticalSection},
        {"InitializeSRWLock",(void*)i_InitializeSRWLock},
        {"AcquireSRWLockExclusive",(void*)i_AcquireSRWLockExclusive},
        {"ReleaseSRWLockExclusive",(void*)i_ReleaseSRWLockExclusive},
        {"AcquireSRWLockShared",(void*)i_AcquireSRWLockShared},
        {"ReleaseSRWLockShared",(void*)i_ReleaseSRWLockShared},
        {"TryAcquireSRWLockExclusive",(void*)i_TryAcquireSRWLockExclusive},
        {"TryAcquireSRWLockShared",(void*)i_TryAcquireSRWLockShared},
        {"InitializeConditionVariable",(void*)i_InitializeConditionVariable},
        {"WakeConditionVariable",(void*)i_WakeConditionVariable},
        {"WakeAllConditionVariable",(void*)i_WakeAllConditionVariable},
        {"SleepConditionVariableCS",(void*)i_SleepConditionVariableCS},
        {"SleepConditionVariableSRW",(void*)i_SleepConditionVariableSRW},
        {"WakeByAddressSingle",(void*)i_WakeByAddressSingle},
        {"WakeByAddressAll",(void*)i_WakeByAddressAll},
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
        {"SetEnvironmentVariableW",(void*)i_SetEnvironmentVariableW},
        {"SetStdHandle",(void*)i_SetStdHandle},
        {"DuplicateHandle",(void*)i_DuplicateHandle},
        {"CreateProcessW",(void*)i_CreateProcessW},
        {"GetExitCodeProcess",(void*)i_GetExitCodeProcessReal},
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
        {"AddVectoredExceptionHandler",(void*)i_AddVectoredExceptionHandler},
        {"RemoveVectoredExceptionHandler",(void*)i_RemoveVectoredExceptionHandler},
        {"SetUnhandledExceptionFilter",(void*)i_SetUnhandledExceptionFilter},
        {"SetErrorMode",(void*)i_SetErrorMode},
        {"SetThreadErrorMode",(void*)i_SetThreadErrorMode},
        {"GetUserDefaultUILanguage",(void*)i_GetUserDefaultUILanguage},
        {"GetUserDefaultLangID",(void*)i_GetUserDefaultLangID},
        {"IsValidCodePage",(void*)i_IsValidCodePage},
        {"DisableThreadLibraryCalls",(void*)i_DisableThreadLibraryCalls},
        {"AreFileApisANSI",(void*)i_AreFileApisANSI},
        {"RegisterClassExW",(void*)i_RegisterClassExW},
        {"RegisterClassW",(void*)i_RegisterClassW},
        {"UnregisterClassW",(void*)i_UnregisterClassW},
        {"LoadIconW",(void*)i_LoadIconW},
        {"LoadCursorW",(void*)i_LoadCursorW},
        {"LoadImageW",(void*)i_LoadImageW},
        {"GetStockObject",(void*)i_GetStockObject},
        {"CreateWindowExW",(void*)i_CreateWindowExW},
        {"DestroyWindow",(void*)i_DestroyWindow},
        {"DefWindowProcW",(void*)i_DefWindowProcW},
        {"ShowWindow",(void*)i_ShowWindow},
        {"UpdateWindow",(void*)i_UpdateWindow},
        {"PeekMessageW",(void*)i_PeekMessageW},
        {"GetMessageW",(void*)i_GetMessageW},
        {"DispatchMessageW",(void*)i_DispatchMessageW},
        {"TranslateMessage",(void*)i_TranslateMessage},
        {"MonitorFromWindow",(void*)i_MonitorFromWindow},
        {"MonitorFromPoint",(void*)i_MonitorFromPoint},
        {"MonitorFromRect",(void*)i_MonitorFromRect},
        {"GetMonitorInfoW",(void*)i_GetMonitorInfoW},
        {"GetSystemMetrics",(void*)i_GetSystemMetrics},
        {"EnumDisplayMonitors",(void*)i_EnumDisplayMonitors},
        {"GetClientRect",(void*)i_GetClientRect},
        {"GetWindowRect",(void*)i_GetWindowRect},
        {"SetWindowPos",(void*)i_SetWindowPos},
        {"GetWindowLongPtrW",(void*)i_GetWindowLongPtrW},
        {"SetWindowLongPtrW",(void*)i_SetWindowLongPtrW},
        {"GetWindowLongW",(void*)i_GetWindowLongW},
        {"SetWindowLongW",(void*)i_SetWindowLongW},
        {"AdjustWindowRectEx",(void*)i_AdjustWindowRectEx},
        {"MoveWindow",(void*)i_MoveWindow},
        {"SetFocus",(void*)i_SetFocus},
        {"SetCapture",(void*)i_SetCapture},
        {"ReleaseCapture",(void*)i_ReleaseCapture},
        {"ClientToScreen",(void*)i_ClientToScreen},
        {"ScreenToClient",(void*)i_ScreenToClient},
        {"RegisterRawInputDevices",(void*)i_RegisterRawInputDevices},
        {"GetRawInputData",(void*)i_GetRawInputData},
        {"GetRegisteredRawInputDevices",(void*)i_GetRegisteredRawInputDevices},
        {"SetTimer",(void*)i_SetTimer},
        {"KillTimer",(void*)i_KillTimer},
        {"GetAsyncKeyState",(void*)i_GetAsyncKeyState},
        {"GetKeyState",(void*)i_GetKeyState},
        {"ShowCursor",(void*)i_ShowCursor},
        {"ClipCursor",(void*)i_ClipCursor},
        {"SetCursorPos",(void*)i_SetCursorPos},
        {"SetCursor",(void*)i_SetCursor},
        {"SetLayeredWindowAttributes",(void*)i_SetLayeredWindowAttributes},
        {"UpdateLayeredWindow",(void*)i_UpdateLayeredWindow},
        {"GetDC",(void*)i_GetDC},
        {"ReleaseDC",(void*)i_ReleaseDC},
        {"GetWindowDC",(void*)i_GetWindowDC},
        {"InvalidateRect",(void*)i_InvalidateRect},
        {"SetWindowTextW",(void*)i_SetWindowTextW},
        {"GetForegroundWindow",(void*)i_GetForegroundWindow},
        {"SetForegroundWindow",(void*)i_SetForegroundWindow},
        {"IsWindow",(void*)i_IsWindow},
        {"IsWindowVisible",(void*)i_IsWindowVisible},
        {"GetActiveWindow",(void*)i_GetActiveWindow},
        {"ChoosePixelFormat",(void*)i_ChoosePixelFormat},
        {"SetPixelFormat",(void*)i_SetPixelFormat},
        {"GetPixelFormat",(void*)i_GetPixelFormat},
        {"DescribePixelFormat",(void*)i_DescribePixelFormat},
        {"wglCreateContext",(void*)i_wglCreateContext},
        {"wglMakeCurrent",(void*)i_wglMakeCurrent},
        {"wglGetProcAddress",(void*)i_wglGetProcAddress},
        {"wglDeleteContext",(void*)i_wglDeleteContext},
        {"wglGetCurrentContext",(void*)i_wglGetCurrentContext},
        {"wglGetCurrentDC",(void*)i_wglGetCurrentDC},
        {"wglShareLists",(void*)i_wglShareLists},
        {"wglSwapIntervalEXT",(void*)i_wglSwapIntervalEXT},
        {"SwapBuffers",(void*)i_SwapBuffers},
        {"RaiseException",(void*)i_RaiseException},
        {"RtlCaptureContext",(void*)i_RtlCaptureContext},
        {"RtlLookupFunctionEntry",(void*)i_RtlLookupFunctionEntry},
        {"RtlRaiseException",(void*)i_RtlRaiseException},
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
        {"CreateEventA",(void*)i_CreateEventA},
        {"CreateEventExW",(void*)i_CreateEventExW},
        {"CreateMutexW",(void*)i_CreateMutexW},
        {"CreateMutexA",(void*)i_CreateMutexA},
        {"ReleaseMutex",(void*)i_ReleaseMutex},
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
    void *r=resolve_impl(fn); if(r) return r;
    // opengl32!gl* -> macOS OpenGL.framework (our wgl* are handled by resolve_impl)
    if(!strcasecmp(dll,"opengl32.dll") && (fn[0]=='g'&&fn[1]=='l')){ if(!strcmp(fn,"glGetString")) return (void*)i_glGetString; if(!strcmp(fn,"glShaderSource")) return (void*)i_glShaderSource; if(!strcmp(fn,"glCompileShader")) return (void*)i_glCompileShader; extern void *dlsym(void*,const char*); void *g=dlsym((void*)-2/*RTLD_DEFAULT*/,fn); if(g) return make_gl_thunk(g); }
    return 0;
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
    // Prefer the image's own base when asked (the main exe and halo3.dll:
    // Reclaimer pokes hardcoded addresses into the engine, valid only at its
    // preferred base). CRT guests load anywhere so they don't squat on
    // halo3.dll's 0x180000000. Hint only — never MAP_FIXED.
    uint8_t *base;
    if(g_prefer_base){ uint8_t *want=(uint8_t*)opt->ImageBase;
        base=mmap(want,opt->ImageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(base!=MAP_FAILED && base==want) ws("  (at preferred base)\n");
        else { if(base!=MAP_FAILED) munmap(base,opt->ImageSize); base=mmap(0,opt->ImageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); } }
    else base=mmap(0,opt->ImageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
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
        int is_ws2 = name_is_ws2(dll);
        for(int i=0; oft[i]; i++){
            const char *fn = (oft[i]>>63)? "#ordinal" : (char*)(base+(uint32_t)oft[i]+2);
            // ws2_32: bind through the shared ordinal/name table (see WS2_HANDLE)
            if(is_ws2){ ft[i]=(uint64_t)((oft[i]>>63)? ws2_by_ordinal((uint32_t)(oft[i]&0xffff)) : ws2_resolve_name(fn)); continue; }
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

// crash handler: print faulting signal/address/RIP + which module (so a
// null-pointer GL call or a guest-code fault is immediately identifiable).
static void crashsig(int sig, siginfo_t *info, void *ucv){
    ucontext_t *uc=ucv; uint64_t rip = uc? (uint64_t)uc->uc_mcontext->__ss.__rip : 0;
    char b[192]; const char *mod="?"; uint64_t moff=0;
    if(g_base){ DOS*d=(DOS*)g_base; OPT64*o=(OPT64*)((uint8_t*)g_base+d->e_lfanew+sizeof(COFF));
        if(rip>=(uint64_t)g_base && rip<(uint64_t)g_base+o->ImageSize){ mod="exe"; moff=rip-(uint64_t)g_base; } }
    for(int m=0;m<g_nmods && !moff;m++){ DOS*d=(DOS*)g_mods[m].base; OPT64*o=(OPT64*)(g_mods[m].base+d->e_lfanew+sizeof(COFF));
        if(rip>=(uint64_t)g_mods[m].base && rip<(uint64_t)g_mods[m].base+o->ImageSize){ mod=g_mods[m].name; moff=rip-(uint64_t)g_mods[m].base; } }
    int n=snprintf(b,sizeof b,"\n[CRASH] sig=%d faultaddr=%p RIP=0x%llx (%s+0x%llx)\n",sig,info?info->si_addr:0,(unsigned long long)rip,mod,(unsigned long long)moff);
    (void)!write(2,b,n);
    // manual stack scan: Win64 code defeats backtrace(); find return addresses
    // that land inside a loaded module and print module+offset (the call chain).
    uint64_t rsp = uc? (uint64_t)uc->uc_mcontext->__ss.__rsp : 0; int printed=0;
    for(uint64_t p=rsp; p<rsp+0x4000 && printed<24; p+=8){
        uint64_t v; memcpy(&v,(void*)p,8);
        const char *mm=0; uint64_t mo=0;
        if(g_base){ DOS*d=(DOS*)g_base; OPT64*o=(OPT64*)((uint8_t*)g_base+d->e_lfanew+sizeof(COFF));
            if(v>=(uint64_t)g_base && v<(uint64_t)g_base+o->ImageSize){ mm="exe"; mo=v-(uint64_t)g_base; } }
        for(int m=0;m<g_nmods && !mm;m++){ DOS*d=(DOS*)g_mods[m].base; OPT64*o=(OPT64*)(g_mods[m].base+d->e_lfanew+sizeof(COFF));
            if(v>=(uint64_t)g_mods[m].base && v<(uint64_t)g_mods[m].base+o->ImageSize){ mm=g_mods[m].name; mo=v-(uint64_t)g_mods[m].base; } }
        if(mm){ int k=snprintf(b,sizeof b,"    ret 0x%llx (%s+0x%llx)\n",(unsigned long long)v,mm,(unsigned long long)mo); (void)!write(2,b,k); printed++; } }
    _exit(139);
}
extern char **environ;
int main(int argc, char **argv){
    { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=crashsig; sa.sa_flags=SA_SIGINFO;
      sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGABRT,&sa,0); }
    const char *path = argc>1 ? argv[1] : "halo3.dll";
    { static char selfabs[4096]; if(realpath(argv[0],selfabs)) g_self=selfabs; else g_self=argv[0]; } // absolute (child chdir's)
    ws("peload: loading "); ws(path); ws("\n");

    // guest argv/argc/environ: pass the PE path + any args after it
    g_argv = argv+1; g_argc = argc-1; if(g_argc<1){ g_argc=1; }
    g_environ = environ;
    { // build the command line from the PE path + any args after it (genuine
      // ucrtbase parses argv from GetCommandLineW, so args must live here)
        int n=0;
        for(int a=1; a<argc; a++){
            const char *s=argv[a]; int q = strchr(s,' ')!=0;   // quote args with spaces
            if(a>1 && n<2046){ g_cmdlineW[n]=' '; g_cmdlineA[n]=' '; n++; }
            if(q && n<2046){ g_cmdlineW[n]='"'; g_cmdlineA[n]='"'; n++; }
            for(int j=0; s[j] && n<2046; j++){ g_cmdlineW[n]=(uint16_t)s[j]; g_cmdlineA[n]=s[j]; n++; }
            if(q && n<2046){ g_cmdlineW[n]='"'; g_cmdlineA[n]='"'; n++; }
        }
        g_cmdlineW[n]=0; g_cmdlineA[n]=0;
    }

    // ---- map + relocate the target image (exe or dll) ----
    g_prefer_base=1; OPT64 *opt; COFF *coff; g_base = map_image(path,&opt,&coff); g_prefer_base=0;
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
    g_glthunk = mmap(0, 64*2048, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    mprotect(g_glthunk, 64*2048, PROT_READ|PROT_EXEC); g_glth_exec = 1;

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
      setenv("USERPROFILE","C:\\ReclaimerProfile",0);
      setenv("RECLAIMER_MOUSE_SENSITIVITY","1.0",0); }  // engine input default (valid range 0.05-20)

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
    g_prefer_base=0; for(int i=0;i<3;i++){ char crt[1300]; snprintf(crt,sizeof crt,"%s/%s",crtdir,names[i]); load_guest(crt, names[i]); }

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
    // debug: pause before entry so lldb can attach (set PELOAD_WAIT=seconds).
    // Writes our pid to /tmp/peload.pid for the attacher.
    if(getenv("PELOAD_WAIT") && strstr(g_cmdlineA,"probe-engine-worker")){ char pb[32]; int pn=snprintf(pb,sizeof pb,"%d\n",getpid());
        int fd=open("/tmp/peload.pid",O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){(void)!write(fd,pb,pn); close(fd);}
        ws("  PELOAD_WAIT: pid "); whex(getpid()); sleep(atoi(getenv("PELOAD_WAIT"))); }
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
