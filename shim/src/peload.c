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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <CommonCrypto/CommonDigest.h>
#include <libkern/OSCacheControl.h>

// ---- SEH register capture/restore (CONTEXT at rcx; Win64/ms_abi) ------------
// seh_capture fills a CONTEXT from the caller's registers (Rip = return address,
// Rsp = rsp after return). seh_restore loads a CONTEXT and jumps to its Rip.
__asm__(
".text\n.p2align 4\n.globl _seh_capture\n_seh_capture:\n"
"  movq %rax,0x78(%rcx)\n  movq %rcx,0x80(%rcx)\n  movq %rdx,0x88(%rcx)\n  movq %rbx,0x90(%rcx)\n"
"  movq %rbp,0xa0(%rcx)\n  movq %rsi,0xa8(%rcx)\n  movq %rdi,0xb0(%rcx)\n"
"  movq %r8,0xb8(%rcx)\n  movq %r9,0xc0(%rcx)\n  movq %r10,0xc8(%rcx)\n  movq %r11,0xd0(%rcx)\n"
"  movq %r12,0xd8(%rcx)\n  movq %r13,0xe0(%rcx)\n  movq %r14,0xe8(%rcx)\n  movq %r15,0xf0(%rcx)\n"
"  leaq 8(%rsp),%rax\n  movq %rax,0x98(%rcx)\n  movq (%rsp),%rax\n  movq %rax,0xf8(%rcx)\n"
"  movdqu %xmm0,0x1a0(%rcx)\n  movdqu %xmm1,0x1b0(%rcx)\n  movdqu %xmm2,0x1c0(%rcx)\n  movdqu %xmm3,0x1d0(%rcx)\n"
"  movdqu %xmm4,0x1e0(%rcx)\n  movdqu %xmm5,0x1f0(%rcx)\n  movdqu %xmm6,0x200(%rcx)\n  movdqu %xmm7,0x210(%rcx)\n"
"  movdqu %xmm8,0x220(%rcx)\n  movdqu %xmm9,0x230(%rcx)\n  movdqu %xmm10,0x240(%rcx)\n  movdqu %xmm11,0x250(%rcx)\n"
"  movdqu %xmm12,0x260(%rcx)\n  movdqu %xmm13,0x270(%rcx)\n  movdqu %xmm14,0x280(%rcx)\n  movdqu %xmm15,0x290(%rcx)\n"
"  movq 0x78(%rcx),%rax\n  movl $0x10000f,0x30(%rcx)\n  ret\n"
".p2align 4\n.globl _seh_restore\n_seh_restore:\n"
"  movdqu 0x1a0(%rcx),%xmm0\n  movdqu 0x1b0(%rcx),%xmm1\n  movdqu 0x1c0(%rcx),%xmm2\n  movdqu 0x1d0(%rcx),%xmm3\n"
"  movdqu 0x1e0(%rcx),%xmm4\n  movdqu 0x1f0(%rcx),%xmm5\n  movdqu 0x200(%rcx),%xmm6\n  movdqu 0x210(%rcx),%xmm7\n"
"  movdqu 0x220(%rcx),%xmm8\n  movdqu 0x230(%rcx),%xmm9\n  movdqu 0x240(%rcx),%xmm10\n  movdqu 0x250(%rcx),%xmm11\n"
"  movdqu 0x260(%rcx),%xmm12\n  movdqu 0x270(%rcx),%xmm13\n  movdqu 0x280(%rcx),%xmm14\n  movdqu 0x290(%rcx),%xmm15\n"
"  movq 0x90(%rcx),%rbx\n  movq 0xa0(%rcx),%rbp\n  movq 0xa8(%rcx),%rsi\n  movq 0xb0(%rcx),%rdi\n"
"  movq 0xb8(%rcx),%r8\n  movq 0xc0(%rcx),%r9\n  movq 0xc8(%rcx),%r10\n  movq 0xd0(%rcx),%r11\n"
"  movq 0xd8(%rcx),%r12\n  movq 0xe0(%rcx),%r13\n  movq 0xe8(%rcx),%r14\n  movq 0xf0(%rcx),%r15\n"
"  movq 0x98(%rcx),%rsp\n  pushq 0xf8(%rcx)\n  movq 0x88(%rcx),%rdx\n  movq 0x78(%rcx),%rax\n  movq 0x80(%rcx),%rcx\n  ret\n"
);

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
static char  g_cmdlineA[2048];                        // fwd (full guest cmdline)

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
    // thread-safe: permanent RWX pool (no RW<->RX toggle that would fault a stub
    // executing on another thread), serialized.
    static pthread_mutex_t sm=PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&sm);
    uint8_t *p = g_stubpool + g_stuboff; g_stuboff += 32;
    int o=0;
    p[o++]=0x48; p[o++]=0x83; p[o++]=0xEC; p[o++]=0x28;            // sub rsp,0x28
    p[o++]=0xB9; *(uint32_t*)(p+o)=idx; o+=4;                       // mov ecx, idx
    p[o++]=0x48; p[o++]=0xB8; *(uint64_t*)(p+o)=(uint64_t)&on_stub; o+=8; // mov rax, &on_stub
    p[o++]=0xFF; p[o++]=0xD0;                                       // call rax
    p[o++]=0x48; p[o++]=0x83; p[o++]=0xC4; p[o++]=0x28;            // add rsp,0x28
    p[o++]=0xC3;                                                    // ret
    sys_icache_invalidate(p,32); pthread_mutex_unlock(&sm);
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
static void *g_thunk_real[8192]; static void *g_thunk_addr[8192]; static int g_nthunk;
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
    // Thread-safe: the pool is permanently RWX (no RW<->RX toggle, which would
    // fault a thunk being executed on another thread), and creation is serialized.
    static pthread_mutex_t gtm=PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&gtm);
    for(int i=0;i<g_nthunk;i++) if(g_thunk_real[i]==real){ pthread_mutex_unlock(&gtm); return g_thunk_addr[i]; } // cache
    if(getenv("PELOAD_TRACE_GL")){ Dl_info di; if(dladdr(real,&di)&&di.dli_sname){ (void)!write(2,"  [thunk] ",10); (void)!write(2,di.dli_sname,strlen(di.dli_sname)); (void)!write(2,"\n",1); } }
    if(g_glthoff+64 > 64*8192){ pthread_mutex_unlock(&gtm); return real; }
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
    sys_icache_invalidate(p,64);  // Rosetta: translate the freshly written thunk
    if(g_nthunk<8192){ g_thunk_real[g_nthunk]=real; g_thunk_addr[g_nthunk]=p; g_nthunk++; }
    pthread_mutex_unlock(&gtm);
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
static uint16_t g_exewin[1024];   // exe path, Windows form (GetModuleFileNameW = exe path, NOT the cmdline)
static char     g_exewinA[1024];  // exe path, ascii
static char     g_playlist[1400]; // staged default playlist (empty-path opens redirect here)

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
// Windows GetLastError/SetLastError are PER-THREAD. A global here lets a benign
// ENOENT probe on one thread (mod/map scans) pollute another thread's
// GetLastError, which Rust reads as a spurious "os error 2". Make it __thread.
static __thread uint32_t g_lasterr = 0;
static uint32_t win_err(void){ switch(errno){ case ENOENT: return 2; /*FILE_NOT_FOUND*/
    case EACCES: case EPERM: return 5; /*ACCESS_DENIED*/ case EEXIST: return 80; /*FILE_EXISTS*/
    case ENOTDIR: return 3; /*PATH_NOT_FOUND*/ default: return 1; } }
static __thread char t_lastfail[512];           // last path that failed to open on this thread
static uint32_t MSABI i_GetLastError(void){
    if(g_lasterr==2 && getenv("PELOAD_TRACE_LE")){ void*ra=__builtin_return_address(0); char t[640]; uint64_t r=(uint64_t)ra; snprintf(t,sizeof t,"  [GLE=2] caller=%s+0x%llx lastfail='%s'\n",(r>=0x140000000&&r<0x14c000000)?"exe":(r>=0x180000000&&r<0x187000000)?"halo3":"?",(unsigned long long)((r>=0x140000000&&r<0x14c000000)?r-0x140000000:(r>=0x180000000&&r<0x187000000)?r-0x180000000:r),t_lastfail); ws(t); }
    return g_lasterr; }
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
    // Do NOT downgrade the page to r-x afterwards: if the target is writable
    // data/stack (or `wr` shares the page), forcing r-x faults the next write.
    // Leave it writable+executable (RWX runs under Rosetta) and invalidate the
    // instruction cache so a code patch re-translates.
    memcpy(addr,buf,n); sys_icache_invalidate(addr,(size_t)n); if(wr)*wr=n; return 1; }
static void*  MSABI i_GetCurrentThread(void){ return (void*)-2; }
// FlushInstructionCache: the engine calls this after patching its own .text
// (inline hooks). Under Rosetta the x86 code is translated to arm64 and cached,
// so a write to a code page leaves a STALE translation unless we invalidate it.
// sys_icache_invalidate is the signal Rosetta honors to drop the translation.
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
// DXGI: the engine probes DirectX Graphics Infrastructure even though Reclaimer
// renders via OpenGL. It ignores the HRESULT and calls factory->EnumAdapters on
// the returned interface, so we must hand back a REAL (minimal) IDXGIFactory
// COM object whose EnumAdapters reports "no adapters" (the engine then uses the
// GL/menu path instead of D3D hardware).
#define HR_DXGI_UNSUPPORTED 0x887A0004
#define HR_DXGI_NOT_FOUND   0x887A0002
#define HR_E_NOTIMPL        0x80004001
static int MSABI com_fail(void){ return HR_E_NOTIMPL; }
static int MSABI dxgi_EnumAdapters(void*self,unsigned i,void**pp){ (void)self;(void)i; if(pp)*pp=0; return HR_DXGI_NOT_FOUND; }
// IDXGIFactory(1/2) vtable: IUnknown(0-2) + IDXGIObject(3-6) + IDXGIFactory(7+)
static void* g_dxgifactory_vtbl[] = {
    (void*)com_QI,(void*)com_AddRef,(void*)com_Release,          // 0-2 IUnknown
    (void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail, // 3-6 SetPrivateData.. GetParent
    (void*)dxgi_EnumAdapters,                                    // 7 EnumAdapters
    (void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail, // 8-11 MakeWindowAssoc..CreateSoftwareAdapter
    (void*)dxgi_EnumAdapters,                                    // 12 EnumAdapters1 (Factory1)
    (void*)com_fail,                                             // 13 IsCurrent (Factory1)
    (void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail,(void*)com_fail }; // 14+ Factory2 slack
static ComObj g_dxgifactory = { g_dxgifactory_vtbl };
static void d3d_init(void); // fwd: also patches g_dxgifactory_vtbl[10]=CreateSwapChain
static int MSABI i_CreateDXGIFactory(GUID_*riid,void**pp){ (void)riid; d3d_init(); if(pp)*pp=&g_dxgifactory; return HR_OK; }
static int MSABI i_CreateDXGIFactory1(GUID_*riid,void**pp){ (void)riid; d3d_init(); if(pp)*pp=&g_dxgifactory; return HR_OK; }
static int MSABI i_CreateDXGIFactory2(uint32_t fl,GUID_*riid,void**pp){ (void)fl;(void)riid; d3d_init(); if(pp)*pp=&g_dxgifactory; return HR_OK; }
// ============================================================================
// Minimal NULL D3D11. halo3.dll's render init needs a working D3D11 device:
// Create* must return valid COM objects (NULLs -> the engine derefs them and
// corrupts the shared heap), Map must return a real writable buffer (else the
// engine writes vertex/constant data into garbage). We render nothing visible
// (halo3 draws the game 3D; Reclaimer's OpenGL UI is what the window shows) — the
// goal is only to satisfy the API contract so startup completes without crashing
// or corrupting memory.  All methods are MSABI (this in rcx).
typedef struct { void** vtbl; uint32_t kind; uint32_t pad; uint64_t size; void* data; } D3DObj;
static ComObj g_d3ddevice, g_d3dcontext;
static void*  g_d3dchild_vtbl[16];   // ID3D11DeviceChild (shaders, states, layouts, linkage, query...)
static void*  g_d3dres_vtbl[16];     // ID3D11Resource (buffers, textures)
static void*  g_d3dview_vtbl[16];    // ID3D11View (SRV/RTV/DSV/UAV)
static void*  g_d3ddev_vtbl[48];
static void*  g_d3dctx_vtbl[160];
static void*  g_dxgiswap_vtbl[40];
static uint8_t g_d3dscratch[1<<20];  // fallback Map target
static D3DObj* d3dobj(void** vt, uint32_t kind, uint64_t size){
    D3DObj* o=calloc(1,sizeof *o); o->vtbl=vt; o->kind=kind; o->size=size; if(size) o->data=calloc(1,size); return o; }
// ---- ID3D11DeviceChild base (slots 0-6) ----
static int      MSABI d3d_QI(void*s,GUID_*r,void**pp){ (void)r; if(pp)*pp=s; return HR_OK; }
static unsigned MSABI d3d_AddRef(void*s){ (void)s; return 1; }
static unsigned MSABI d3d_Release(void*s){ (void)s; return 0; }
static void     MSABI d3d_GetDevice(void*s,void**pp){ (void)s; if(pp)*pp=&g_d3ddevice; }
static int      MSABI d3d_GetPD(void*s,GUID_*g,uint32_t*ps,void*pd){ (void)s;(void)g;(void)pd; if(ps)*ps=0; return HR_NOTFOUND; }
static int      MSABI d3d_SetPD(void*s,GUID_*g,uint32_t sz,const void*d){ (void)s;(void)g;(void)sz;(void)d; return HR_OK; }
static int      MSABI d3d_SetPDI(void*s,GUID_*g,const void*d){ (void)s;(void)g;(void)d; return HR_OK; }
// ---- ID3D11Resource (7-10) ----
static void     MSABI res_GetType(void*s,uint32_t*pd){ D3DObj*o=s; if(pd)*pd=o?o->kind:1; }
static void     MSABI res_SetEvict(void*s,uint32_t p){ (void)s;(void)p; }
static unsigned MSABI res_GetEvict(void*s){ (void)s; return 0; }
static void     MSABI res_GetDesc(void*s,void*pd){ D3DObj*o=s; if(pd){ memset(pd,0,64); if(o)*(uint32_t*)pd=(uint32_t)o->size; } }
// ---- ID3D11View (7-8). o->data holds the resource the view was created over, so
// GetResource returns a valid (never-null) resource the engine then dereferences.
static D3DObj* d3dobj(void** vt, uint32_t kind, uint64_t size); // fwd
static D3DObj  g_d3ddummyres_obj; static D3DObj* g_d3ddummyres;
static void     MSABI view_GetResource(void*s,void**pp){ D3DObj*o=s; if(pp)*pp=(o&&o->data)?o->data:g_d3ddummyres; }
static void     MSABI view_GetDesc(void*s,void*pd){ (void)s; if(pd) memset(pd,0,64); }
// ---- create helpers by out-param position ----
static int MSABI dev_crRes(void*t,const void*desc,const void*init,void**pp){ (void)t; // CreateBuffer/Texture*
    uint64_t sz = desc? (uint64_t)(*(const uint32_t*)desc) : 0;          // ByteWidth / Width (offset 0)
    if(sz<16) sz=1<<16;                                                  // textures: desc[0]=Width; give generous room
    D3DObj* o=d3dobj(g_d3dres_vtbl,1,sz); if(init && o->data){ const void*src=*(void*const*)init; if(src) memcpy(o->data,src,sz>4096?4096:sz); }
    if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crView3(void*t,const void*res,const void*desc,void**pp){ (void)t;(void)desc; D3DObj*o=d3dobj(g_d3dview_vtbl,0,0); o->data=(void*)res; if(pp)*pp=o; return HR_OK; }
static int MSABI dev_cr2(void*t,const void*a,void**pp){ (void)t;(void)a; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; }      // (desc,pp) out=r8
static int MSABI dev_cr1(void*t,void**pp){ (void)t; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; }                         // (pp) out=rdx
static int MSABI dev_cr5(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)bc;(void)len;(void)lk; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; } // shaders
static int MSABI dev_crIL(void*t,const void*d,uint32_t n,const void*bc,uint64_t len,void**pp){ (void)t;(void)d;(void)n;(void)bc;(void)len; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; } // CreateInputLayout
static int MSABI dev_crCtx(void*t,uint32_t fl,void**pp){ (void)t;(void)fl; if(pp)*pp=&g_d3dcontext; return HR_OK; }                 // CreateDeferredContext
static int MSABI dev_OpenShared(void*t,void*h,GUID_*r,void**pp){ (void)t;(void)h;(void)r; if(pp)*pp=d3dobj(g_d3dres_vtbl,1,1<<16); return HR_OK; }
static int MSABI dev_CheckFormat(void*t,uint32_t fmt,uint32_t*ps){ (void)t;(void)fmt; if(ps)*ps=0x1FFFFF; return HR_OK; }           // claim broad support
static int MSABI dev_CheckMSAA(void*t,uint32_t fmt,uint32_t cnt,uint32_t*pq){ (void)t;(void)fmt;(void)cnt; if(pq)*pq=1; return HR_OK; }
static void MSABI dev_CheckCounterInfo(void*t,void*pi){ (void)t; if(pi) memset(pi,0,16); }
static int MSABI dev_CheckFeature(void*t,uint32_t f,void*pd,uint32_t sz){ (void)t;(void)f; if(pd) memset(pd,0,sz); return HR_OK; }
static uint32_t MSABI dev_GetFeatureLevel(void*t){ (void)t; return 0xb000; }   // D3D_FEATURE_LEVEL_11_0
static uint32_t MSABI dev_GetCreationFlags(void*t){ (void)t; return 0; }
static int MSABI dev_GetRemovedReason(void*t){ (void)t; return HR_OK; }
static void MSABI dev_GetImmediateContext(void*t,void**pp){ (void)t; if(pp)*pp=&g_d3dcontext; }
static int MSABI dev_SetExceptionMode(void*t,uint32_t m){ (void)t;(void)m; return HR_OK; }
static uint32_t MSABI dev_GetExceptionMode(void*t){ (void)t; return 0; }
// ---- ID3D11DeviceContext: Map returns a real buffer; everything else no-op/0 ----
static int  MSABI ctx_ret0(void){ return 0; }                            // safe for void + HRESULT + UINT returns
static int  MSABI ctx_Map(void*t,void*res,uint32_t sub,uint32_t type,uint32_t fl,void*pM){ (void)t;(void)sub;(void)type;(void)fl;
    D3DObj*o=res; if(pM){ void* d=(o&&o->data)?o->data:g_d3dscratch; uint32_t rp=(o&&o->size)?(uint32_t)o->size:(uint32_t)sizeof g_d3dscratch;
        *(void**)pM=d; *(uint32_t*)((char*)pM+8)=rp; *(uint32_t*)((char*)pM+12)=rp; } return HR_OK; }
static void MSABI ctx_GetDevice(void*t,void**pp){ (void)t; if(pp)*pp=&g_d3ddevice; }
// ---- IDXGISwapChain ----
static int MSABI swap_Present(void*t,uint32_t sync,uint32_t fl){ (void)t;(void)sync;(void)fl; return HR_OK; }
static int MSABI swap_GetBuffer(void*t,uint32_t buf,GUID_*r,void**pp){ (void)t;(void)buf;(void)r; if(pp)*pp=d3dobj(g_d3dres_vtbl,2,1<<20); return HR_OK; }
static int MSABI dxgi_CreateSwapChain(void*t,void*dev,void*desc,void**pp){ (void)t;(void)dev;(void)desc; if(pp)*pp=d3dobj(g_dxgiswap_vtbl,0,0); return HR_OK; }
static void d3d_init(void){
    static int done; if(done) return; done=1;
    void** c=g_d3dchild_vtbl; c[0]=(void*)d3d_QI;c[1]=(void*)d3d_AddRef;c[2]=(void*)d3d_Release;c[3]=(void*)d3d_GetDevice;c[4]=(void*)d3d_GetPD;c[5]=(void*)d3d_SetPD;c[6]=(void*)d3d_SetPDI;
    for(int i=7;i<16;i++) c[i]=(void*)ctx_ret0;
    void** r=g_d3dres_vtbl; for(int i=0;i<7;i++) r[i]=c[i]; r[7]=(void*)res_GetType;r[8]=(void*)res_SetEvict;r[9]=(void*)res_GetEvict;r[10]=(void*)res_GetDesc; for(int i=11;i<16;i++) r[i]=(void*)ctx_ret0;
    void** v=g_d3dview_vtbl; for(int i=0;i<7;i++) v[i]=c[i]; v[7]=(void*)view_GetResource;v[8]=(void*)view_GetDesc; for(int i=9;i<16;i++) v[i]=(void*)ctx_ret0;
    g_d3ddummyres_obj.vtbl=g_d3dres_vtbl; g_d3ddummyres_obj.kind=1; g_d3ddummyres_obj.size=1<<16; g_d3ddummyres_obj.data=calloc(1,1<<16); g_d3ddummyres=&g_d3ddummyres_obj;
    void** d=g_d3ddev_vtbl;
    d[0]=(void*)d3d_QI;d[1]=(void*)d3d_AddRef;d[2]=(void*)d3d_Release;
    d[3]=(void*)dev_crRes;d[4]=(void*)dev_crRes;d[5]=(void*)dev_crRes;d[6]=(void*)dev_crRes;         // Create Buffer/Tex1D/2D/3D
    d[7]=(void*)dev_crView3;d[8]=(void*)dev_crView3;d[9]=(void*)dev_crView3;d[10]=(void*)dev_crView3; // SRV/UAV/RTV/DSV
    d[11]=(void*)dev_crIL;                                                                            // CreateInputLayout
    d[12]=(void*)dev_cr5;d[13]=(void*)dev_cr5;d[14]=(void*)dev_cr5;d[15]=(void*)dev_cr5;d[16]=(void*)dev_cr5;d[17]=(void*)dev_cr5;d[18]=(void*)dev_cr5; // VS,GS,GSSO,PS,HS,DS,CS
    d[19]=(void*)dev_cr1;                                                                             // CreateClassLinkage
    d[20]=(void*)dev_cr2;d[21]=(void*)dev_cr2;d[22]=(void*)dev_cr2;d[23]=(void*)dev_cr2;d[24]=(void*)dev_cr2;d[25]=(void*)dev_cr2;d[26]=(void*)dev_cr2; // Blend,DSS,RS,Sampler,Query,Predicate,Counter
    d[27]=(void*)dev_crCtx;                                                                           // CreateDeferredContext
    d[28]=(void*)dev_OpenShared;d[29]=(void*)dev_CheckFormat;d[30]=(void*)dev_CheckMSAA;d[31]=(void*)dev_CheckCounterInfo;d[32]=(void*)ctx_ret0;d[33]=(void*)dev_CheckFeature;
    d[34]=(void*)d3d_GetPD;d[35]=(void*)d3d_SetPD;d[36]=(void*)d3d_SetPDI;
    d[37]=(void*)dev_GetFeatureLevel;d[38]=(void*)dev_GetCreationFlags;d[39]=(void*)dev_GetRemovedReason;d[40]=(void*)dev_GetImmediateContext;d[41]=(void*)dev_SetExceptionMode;d[42]=(void*)dev_GetExceptionMode;
    for(int i=43;i<48;i++) d[i]=(void*)ctx_ret0;
    g_d3ddevice.vtbl=d;
    void** x=g_d3dctx_vtbl; for(int i=0;i<160;i++) x[i]=(void*)ctx_ret0;
    x[0]=(void*)d3d_QI;x[1]=(void*)d3d_AddRef;x[2]=(void*)d3d_Release;x[3]=(void*)ctx_GetDevice;x[4]=(void*)d3d_GetPD;x[5]=(void*)d3d_SetPD;x[6]=(void*)d3d_SetPDI;
    x[14]=(void*)ctx_Map;                                                                            // Map (Unmap=15 stays ret0)
    g_d3dcontext.vtbl=x;
    void** s=g_dxgiswap_vtbl; for(int i=0;i<40;i++) s[i]=(void*)ctx_ret0;
    s[0]=(void*)d3d_QI;s[1]=(void*)d3d_AddRef;s[2]=(void*)d3d_Release;
    s[8]=(void*)swap_Present;s[9]=(void*)swap_GetBuffer;
    g_dxgifactory_vtbl[10]=(void*)dxgi_CreateSwapChain;   // IDXGIFactory::CreateSwapChain
}
static int MSABI i_D3D11CreateDevice(void*a,int b,void*c,uint32_t d,void*e,uint32_t f,uint32_t g,void**dev,void*fl,void**ctx){
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;
    d3d_init();
    if(getenv("PELOAD_TRACE_SEH")) ws("  [d3d11] CreateDevice -> null device\n");
    if(dev)*dev=&g_d3ddevice; if(ctx)*ctx=&g_d3dcontext; if(fl)*(uint32_t*)fl=0xb000; // FEATURE_LEVEL_11_0
    return HR_OK; }
// ---- D3DCompile (d3dcompiler_47): halo3.dll compiles its video-display HLSL
// shaders (CopyToScreen / YUVToRGB) to DXBC blobs at startup, then calls
// ID3DBlob::GetBufferPointer/Size on the result. A stub that returns S_OK but
// leaves the out blob NULL -> the engine derefs NULL. Return a real ID3DBlob
// wrapping a small placeholder DXBC buffer so these paths complete. ----
typedef struct { void** vtbl; void* buf; uint64_t sz; } Blob;
static void*    MSABI blob_GetBufferPointer(void*s){ return ((Blob*)s)->buf; }
static uint64_t MSABI blob_GetBufferSize(void*s){ return ((Blob*)s)->sz; }
static void* g_blob_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)blob_GetBufferPointer,(void*)blob_GetBufferSize };
static Blob* make_blob(const void*data,uint64_t n){ Blob*b=calloc(1,sizeof*b); b->vtbl=g_blob_vtbl; b->sz=n; b->buf=calloc(1,n?n:4); if(data&&n)memcpy(b->buf,data,n); return b; }
static int MSABI i_D3DCompile(const void*src,uint64_t srcsz,const char*name,const void*defs,void*inc,
        const char*entry,const char*target,uint32_t f1,uint32_t f2,void**ppCode,void**ppErr){
    (void)src;(void)srcsz;(void)name;(void)defs;(void)inc;(void)entry;(void)target;(void)f1;(void)f2;
    // placeholder DXBC container (magic + zeroed header); real bytecode isn't
    // needed because there's no D3D device to consume it — the engine only reads
    // the blob's pointer/size here.
    uint8_t hdr[256]; memset(hdr,0,sizeof hdr); hdr[0]='D';hdr[1]='X';hdr[2]='B';hdr[3]='C';
    *(uint32_t*)(hdr+24)=sizeof hdr; // total size field
    if(ppCode)*ppCode=make_blob(hdr,sizeof hdr);
    if(ppErr)*ppErr=0;
    return HR_OK;
}
// ---- minimal XAudio2 (the engine creates IXAudio2 + voices and calls them) --
// XAudio2Create stub didn't set the out interface -> engine called a method on
// NULL. Provide a silent IXAudio2 + a generic voice object so audio init runs.
static int MSABI xa_ret0(void){ return 0; }                    // S_OK / void methods
static void MSABI xa_GetVoiceDetails(void*self,void*pd){ (void)self; if(pd){ uint32_t*d=pd; d[0]=0; d[1]=0; d[2]=2; d[3]=48000; } } // {flags,active,channels,rate}
static void MSABI xa_GetState(void*self,void*ps,uint32_t fl){ (void)self;(void)fl; if(ps) memset(ps,0,24); } // {ctx*,BuffersQueued,SamplesPlayed}
// IXAudio2Voice/SourceVoice vtable: slot0 GetVoiceDetails; most return 0/void.
static void* g_xavoice_vtbl[64];
static ComObj g_xavoice = { g_xavoice_vtbl };
static void* MSABI xa_CreateVoice(void*self,void**ppv,const void*a,uint32_t b,float c,void*d,const void*e,const void*f){
    (void)self;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; if(ppv)*ppv=&g_xavoice; return 0; }
// IXAudio2 vtable (IUnknown + XAudio2 methods)
static void* g_ixaudio2_vtbl[] = {
    (void*)com_QI,(void*)com_AddRef,(void*)com_Release,          // 0-2
    (void*)xa_ret0,(void*)xa_ret0,                              // 3 RegisterForCallbacks, 4 Unregister
    (void*)xa_CreateVoice,(void*)xa_CreateVoice,(void*)xa_CreateVoice, // 5 CreateSourceVoice,6 Submix,7 Mastering
    (void*)xa_ret0,(void*)xa_ret0,(void*)xa_ret0,(void*)xa_ret0,(void*)xa_ret0 }; // 8 StartEngine..12 SetDebugConfiguration
static ComObj g_ixaudio2 = { g_ixaudio2_vtbl };
static int MSABI i_XAudio2Create(void**pp,uint32_t flags,uint32_t proc){ (void)flags;(void)proc;
    static int init; if(!init){ for(int i=0;i<64;i++) g_xavoice_vtbl[i]=(void*)xa_ret0; g_xavoice_vtbl[0]=(void*)xa_GetVoiceDetails; g_xavoice_vtbl[25]=(void*)xa_GetState; init=1; }
    if(pp)*pp=&g_ixaudio2; return 0; }
// ---- XAPO audio effects: CreateAudioReverb/VolumeMeter/CreateFX (xaudio2
// ordinals 2/3/4). The engine creates these effect objects during shell audio
// setup and immediately calls IXAPO methods on the returned interface; a generic
// stub returns S_OK but leaves the out-pointer NULL -> vtable deref on NULL. We
// return a real (silent) IXAPO object. It's installed into the voice effect
// chain (our no-op voice), so its Process() is never actually driven.
static int MSABI apo_ret0(void){ return 0; }
static uint8_t g_apo_regprops[2048]; // zeroed APO_REG_PROPERTIES template
static int MSABI apo_GetRegistrationProperties(void*self, void**pp){ (void)self;
    // after CLSID(16) + FriendlyName[256] + CopyrightInfo[256] (WCHAR): version/flags/buffer counts
    uint32_t* u=(uint32_t*)(g_apo_regprops+16+256*2+256*2);
    u[0]=1; u[1]=0; u[2]=0; u[3]=1; u[4]=1; u[5]=1; u[6]=1; // Major,Minor,Flags,Min/MaxIn,Min/MaxOut
    if(pp)*pp=g_apo_regprops; return 0; }
static void* g_apo_vtbl[32];
static ComObj g_apo = { g_apo_vtbl };
static int MSABI i_CreateAudioReverb(void**pp){
    static int init; if(!init){ for(int i=0;i<32;i++) g_apo_vtbl[i]=(void*)apo_ret0;
        g_apo_vtbl[0]=(void*)com_QI; g_apo_vtbl[1]=(void*)com_AddRef; g_apo_vtbl[2]=(void*)com_Release;
        g_apo_vtbl[3]=(void*)apo_GetRegistrationProperties; init=1; }
    if(pp)*pp=&g_apo; return HR_OK; }
static int MSABI i_CreateAudioVolumeMeter(void**pp){ return i_CreateAudioReverb(pp); }
static int MSABI i_CreateFX(GUID_*clsid,void**pp,const void*init,uint32_t sz){ (void)clsid;(void)init;(void)sz; return i_CreateAudioReverb(pp); }
// map an xaudio2_9redist import (by ordinal or name) to our implementation.
// ordinals: 1=XAudio2Create 2=CreateAudioReverb 3=CreateAudioVolumeMeter
// 4=CreateFX 5=X3DAudioCalculate 6=X3DAudioInitialize.
static void* xa_resolve(uint32_t ord, const char* fn){
    if(ord==1 || (fn&&!strcmp(fn,"XAudio2Create"))) return (void*)i_XAudio2Create;
    if(ord==2 || (fn&&!strcmp(fn,"CreateAudioReverb"))) return (void*)i_CreateAudioReverb;
    if(ord==3 || (fn&&!strcmp(fn,"CreateAudioVolumeMeter"))) return (void*)i_CreateAudioVolumeMeter;
    if(ord==4 || (fn&&!strcmp(fn,"CreateFX"))) return (void*)i_CreateFX;
    return 0; // X3DAudio* fall through to generic stub (engine ignores results)
}
static int name_has(const char*s,const char*sub){ if(!s)return 0; for(;*s;s++){ const char*a=s,*b=sub; while(*b&&((*a|32)==(*b|32))){a++;b++;} if(!*b)return 1; } return 0; }
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
// ---- real Winsock (BSD sockets) for the loopback UDP transport -------------
// The native transport is loopback-only, which BSD sockets service directly.
// SOCKET values are the BSD fd; INVALID_SOCKET = ~0. sockaddr differs between
// Win (family is LE16 at offset 0) and BSD (sin_len, sin_family bytes) and the
// AF_INET6 constant differs (Win 23 vs macOS 30), so convert both ways.
#define WSASOCK uint64_t
#define WSA_INVALID (~0ULL)
static __thread int t_wsa_err;
static uint32_t errno2wsa(int e){ switch(e){ case EWOULDBLOCK:return 10035; case EINPROGRESS:return 10036;
    case EALREADY:return 10037; case ENOTSOCK:return 10038; case EMSGSIZE:return 10040; case EADDRINUSE:return 10048;
    case EADDRNOTAVAIL:return 10049; case ENETUNREACH:return 10051; case ECONNRESET:return 10054; case ENOBUFS:return 10055;
    case EISCONN:return 10056; case ENOTCONN:return 10057; case ECONNREFUSED:return 10061; case EAFNOSUPPORT:return 10047;
    case EINVAL:return 10022; default:return 10022; } }
static void ws2_err(int e){ t_wsa_err = e? (int)errno2wsa(e):0; }
// Windows sockaddr_in -> BSD sockaddr_storage; returns BSD length or 0
static int sa_w2b(const void*win,int wl,struct sockaddr_storage*bsd){ if(!win||wl<2) return 0; const uint8_t*w=win;
    uint16_t fam=(uint16_t)(w[0]|(w[1]<<8)); memset(bsd,0,sizeof *bsd);
    if(fam==2){ struct sockaddr_in*b=(void*)bsd; b->sin_len=sizeof *b; b->sin_family=AF_INET; memcpy(&b->sin_port,w+2,2); memcpy(&b->sin_addr,w+4,4); return sizeof *b; }
    if(fam==23){ struct sockaddr_in6*b=(void*)bsd; b->sin6_len=sizeof *b; b->sin6_family=AF_INET6; memcpy(&b->sin6_port,w+2,2); memcpy(&b->sin6_flowinfo,w+4,4); memcpy(&b->sin6_addr,w+8,16); memcpy(&b->sin6_scope_id,w+24,4); return sizeof *b; }
    return 0; }
// BSD sockaddr -> Windows sockaddr; writes up to *wl bytes, sets *wl to size used
static void sa_b2w(const struct sockaddr*bsd,void*win,int*wl){ if(!bsd||!win) return; uint8_t*w=win;
    if(bsd->sa_family==AF_INET){ const struct sockaddr_in*b=(void*)bsd; if(!wl||*wl>=16){ w[0]=2;w[1]=0; memcpy(w+2,&b->sin_port,2); memcpy(w+4,&b->sin_addr,4); memset(w+8,0,8);} if(wl)*wl=16; }
    else if(bsd->sa_family==AF_INET6){ const struct sockaddr_in6*b=(void*)bsd; if(!wl||*wl>=28){ w[0]=23;w[1]=0; memcpy(w+2,&b->sin6_port,2); memcpy(w+4,&b->sin6_flowinfo,4); memcpy(w+8,&b->sin6_addr,16); memcpy(w+24,&b->sin6_scope_id,4);} if(wl)*wl=28; } }
static int MSABI i_ws2_WSAStartup(uint16_t ver,void*data){ if(data){ uint8_t*d=data; *(uint16_t*)d=ver; *(uint16_t*)(d+2)=ver; memset(d+4,0,398);} t_wsa_err=0; return 0; }
static int MSABI i_ws2_WSACleanup(void){ return 0; }
static int MSABI i_ws2_WSAGetLastError(void){ return t_wsa_err; }
static void MSABI i_ws2_WSASetLastError(int e){ t_wsa_err=e; }
static WSASOCK MSABI i_ws2_socket(int af,int type,int proto){ int fd=socket(af==23?AF_INET6:af,type,proto); if(fd<0){ws2_err(errno);return WSA_INVALID;} return (WSASOCK)(unsigned)fd; }
static WSASOCK MSABI i_ws2_WSASocketW(int af,int type,int proto,void*pi,unsigned g,uint32_t fl){ (void)pi;(void)g;(void)fl; return i_ws2_socket(af,type,proto); }
static int MSABI i_ws2_closesocket(WSASOCK s){ return close((int)s)==0?0:(ws2_err(errno),-1); }
static int MSABI i_ws2_bind(WSASOCK s,const void*name,int nl){ struct sockaddr_storage ss; int bl=sa_w2b(name,nl,&ss); if(!bl){ws2_err(EINVAL);return -1;} if(bind((int)s,(struct sockaddr*)&ss,bl)<0){ws2_err(errno);return -1;} return 0; }
static int MSABI i_ws2_connect(WSASOCK s,const void*name,int nl){ struct sockaddr_storage ss; int bl=sa_w2b(name,nl,&ss); if(!bl){ws2_err(EINVAL);return -1;} if(connect((int)s,(struct sockaddr*)&ss,bl)<0){ws2_err(errno);return -1;} return 0; }
static int MSABI i_ws2_sendto(WSASOCK s,const char*buf,int len,int fl,const void*to,int tl){ struct sockaddr_storage ss; int bl=to?sa_w2b(to,tl,&ss):0; ssize_t r=sendto((int)s,buf,len,fl,bl?(struct sockaddr*)&ss:0,bl); if(r<0){ws2_err(errno);return -1;} return (int)r; }
static int MSABI i_ws2_recvfrom(WSASOCK s,char*buf,int len,int fl,void*from,int*fl2){ struct sockaddr_storage ss; socklen_t sl=sizeof ss; ssize_t r=recvfrom((int)s,buf,len,fl,(struct sockaddr*)&ss,&sl); if(r<0){ws2_err(errno);return -1;} if(from&&fl2) sa_b2w((struct sockaddr*)&ss,from,fl2); return (int)r; }
static int MSABI i_ws2_send(WSASOCK s,const char*buf,int len,int fl){ ssize_t r=send((int)s,buf,len,fl); if(r<0){ws2_err(errno);return -1;} return (int)r; }
static int MSABI i_ws2_recv(WSASOCK s,char*buf,int len,int fl){ ssize_t r=recv((int)s,buf,len,fl); if(r<0){ws2_err(errno);return -1;} return (int)r; }
static int MSABI i_ws2_shutdown(WSASOCK s,int how){ return shutdown((int)s,how)==0?0:(ws2_err(errno),-1); }
static int MSABI i_ws2_getsockname(WSASOCK s,void*name,int*nl){ struct sockaddr_storage ss; socklen_t sl=sizeof ss; if(getsockname((int)s,(struct sockaddr*)&ss,&sl)<0){ws2_err(errno);return -1;} sa_b2w((struct sockaddr*)&ss,name,nl); return 0; }
static int MSABI i_ws2_getpeername(WSASOCK s,void*name,int*nl){ struct sockaddr_storage ss; socklen_t sl=sizeof ss; if(getpeername((int)s,(struct sockaddr*)&ss,&sl)<0){ws2_err(errno);return -1;} sa_b2w((struct sockaddr*)&ss,name,nl); return 0; }
static int MSABI i_ws2_setsockopt(WSASOCK s,int lvl,int opt,const char*val,int len){ if(setsockopt((int)s,lvl,opt,val,len)==0) return 0; if(errno==ENOPROTOOPT) return 0; /*lenient: ignore unknown opts*/ ws2_err(errno); return -1; }
static int MSABI i_ws2_getsockopt(WSASOCK s,int lvl,int opt,char*val,int*len){ socklen_t sl=len?*len:0; if(getsockopt((int)s,lvl,opt,val,&sl)==0){ if(len)*len=sl; return 0;} if(errno==ENOPROTOOPT){ if(val&&len&&*len>=4)*(int*)val=0; return 0;} ws2_err(errno); return -1; }
static int MSABI i_ws2_ioctlsocket(WSASOCK s,long cmd,uint32_t*argp){ if(ioctl((int)s,(unsigned long)cmd,argp)==0) return 0; ws2_err(errno); return -1; }
// select: Windows fd_set = { u_int count; SOCKET array[64] }. Bridge via poll().
typedef struct { uint32_t cnt; WSASOCK arr[64]; } WSAFDSET;
static int MSABI i_ws2_select(int nfds,WSAFDSET*rd,WSAFDSET*wr,WSAFDSET*ex,const void*tv){ (void)nfds;
    struct pollfd pfd[192]; int np=0; WSAFDSET*sets[3]={rd,wr,ex}; short ev[3]={POLLIN,POLLOUT,POLLPRI};
    for(int si=0;si<3;si++){ WSAFDSET*S=sets[si]; if(!S)continue; for(uint32_t i=0;i<S->cnt&&np<192;i++){ int fd=(int)S->arr[i]; int f=-1; for(int k=0;k<np;k++) if(pfd[k].fd==fd){f=k;break;} if(f<0){ pfd[np].fd=fd; pfd[np].events=0; pfd[np].revents=0; f=np++; } pfd[f].events|=ev[si]; } }
    int to=-1; if(tv){ const int32_t*t=tv; to=t[0]*1000 + t[1]/1000; }
    int r=poll(pfd,np,to); if(r<0){ws2_err(errno);return -1;}
    for(int si=0;si<3;si++){ WSAFDSET*S=sets[si]; if(!S)continue; WSASOCK keep[64]; uint32_t kc=0; for(uint32_t i=0;i<S->cnt;i++){ int fd=(int)S->arr[i]; for(int k=0;k<np;k++) if(pfd[k].fd==fd && (pfd[k].revents&(ev[si]|POLLHUP|POLLERR))){ if(kc<64)keep[kc++]=S->arr[i]; break; } } S->cnt=kc; for(uint32_t i=0;i<kc;i++)S->arr[i]=keep[i]; }
    return r; }
static int MSABI i_ws2___WSAFDIsSet(WSASOCK s,WSAFDSET*set){ if(!set)return 0; for(uint32_t i=0;i<set->cnt;i++) if(set->arr[i]==s) return 1; return 0; }
static uint32_t MSABI i_ws2_htonl(uint32_t x){ return htonl(x); }
static uint32_t MSABI i_ws2_ntohl(uint32_t x){ return ntohl(x); }
static uint16_t MSABI i_ws2_htons(uint16_t x){ return htons(x); }
static uint16_t MSABI i_ws2_ntohs(uint16_t x){ return ntohs(x); }
static uint32_t MSABI i_ws2_inet_addr(const char*c){ return inet_addr(c); }
static int MSABI i_ws2_WSAIoctl(WSASOCK s,uint32_t code,void*ib,uint32_t ibl,void*ob,uint32_t obl,uint32_t*ret,void*ov,void*cb){
    (void)s;(void)ib;(void)ibl;(void)ob;(void)obl;(void)ov;(void)cb;
    // SIO_GET_EXTENSION_FUNCTION_POINTER (0xC8000006): refuse so quinn falls back
    // to plain recvfrom/sendto instead of WSARecvMsg/WSASendMsg.
    if(ret)*ret=0; t_wsa_err=10022 /*WSAEINVAL*/; return -1; }
// map a ws2_32 ordinal to a real impl (0 = none, fall back to stub)
static void* ws2_real_ord(uint32_t o){ switch(o){
    case 1: return 0; /*accept: TCP, not needed for UDP*/
    case 2: return (void*)i_ws2_bind; case 3: return (void*)i_ws2_closesocket; case 4: return (void*)i_ws2_connect;
    case 5: return (void*)i_ws2_getpeername; case 6: return (void*)i_ws2_getsockname; case 7: return (void*)i_ws2_getsockopt;
    case 8: return (void*)i_ws2_htonl; case 9: return (void*)i_ws2_htons; case 10: return (void*)i_ws2_ioctlsocket;
    case 11: return (void*)i_ws2_inet_addr; case 14: return (void*)i_ws2_ntohl; case 15: return (void*)i_ws2_ntohs;
    case 16: return (void*)i_ws2_recv; case 17: return (void*)i_ws2_recvfrom; case 18: return (void*)i_ws2_select;
    case 19: return (void*)i_ws2_send; case 20: return (void*)i_ws2_sendto; case 21: return (void*)i_ws2_setsockopt;
    case 22: return (void*)i_ws2_shutdown; case 23: return (void*)i_ws2_socket;
    case 111: return (void*)i_ws2_WSAGetLastError; case 115: return (void*)i_ws2_WSAStartup; case 116: return (void*)i_ws2_WSACleanup;
    case 151: return (void*)i_ws2___WSAFDIsSet; default: return 0; } }
static void* ws2_real_name(const char*n){
    if(!strcmp(n,"WSASocketW")||!strcmp(n,"WSASocketA")) return (void*)i_ws2_WSASocketW;
    if(!strcmp(n,"WSAIoctl")) return (void*)i_ws2_WSAIoctl;
    if(!strcmp(n,"WSASetLastError")) return (void*)i_ws2_WSASetLastError;
    if(!strcmp(n,"WSARecvFrom")) return (void*)i_ws2_recvfrom; // close-enough arg prefix for fallback probing
    return 0; }
static void* g_ws2ord[600];
static void* ws2_by_ordinal(uint32_t ord){
    if(ord>=600) ord=0; if(g_ws2ord[ord]) return g_ws2ord[ord];
    void*r=ws2_real_ord(ord); if(!r && imp_count<8192){ imp_dll[imp_count]="ws2_32.dll"; imp_fn[imp_count]="#ws2ord"; r=make_stub(imp_count++); }
    g_ws2ord[ord]=r; return r; }
static uint32_t ws2_name2ord(const char*n){
    struct{const char*n;uint32_t o;} m[]={{"accept",1},{"bind",2},{"closesocket",3},{"connect",4},
     {"getpeername",5},{"getsockname",6},{"getsockopt",7},{"htonl",8},{"htons",9},{"ioctlsocket",10},
     {"inet_addr",11},{"inet_ntoa",12},{"listen",13},{"ntohl",14},{"ntohs",15},{"recv",16},{"recvfrom",17},
     {"select",18},{"send",19},{"sendto",20},{"setsockopt",21},{"shutdown",22},{"socket",23},
     {"WSAGetLastError",111},{"WSAStartup",115},{"WSACleanup",116},{"__WSAFDIsSet",151},{0,0}};
    for(int i=0;m[i].n;i++) if(!strcmp(n,m[i].n)) return m[i].o; return 0; }
static void* ws2_resolve_name(const char*n){
    uint32_t o=ws2_name2ord(n); if(o) return ws2_by_ordinal(o);
    static struct{const char*n;void*p;} nc[64]; static int ncn;      // by-name only (WSASocketW, WSAIoctl, …)
    for(int i=0;i<ncn;i++) if(!strcmp(nc[i].n,n)) return nc[i].p;
    void*p=ws2_real_name(n); if(!p && imp_count<8192){ imp_dll[imp_count]="ws2_32.dll"; imp_fn[imp_count]=n; p=make_stub(imp_count++); }
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
static void   MSABI i_ExitProcess(uint32_t c){
    // PELOAD_PROBE_EXIT0: force the probe-engine-worker to report success so the
    // parent orchestrator proceeds to launch its real client (experiment).
    if(c && getenv("PELOAD_PROBE_EXIT0") && strstr(g_cmdlineA,"probe-engine-worker")){
        ws("  [probe-exit0] forcing worker exit 0 (was "); { char t[16]; int n=snprintf(t,sizeof t,"%u)\n",c); ws(t);} _exit(0); }
    _exit((int)c); }
static void   MSABI i_TerminateProcess(void *h,uint32_t c){ (void)h; _exit((int)c); }
static uint32_t MSABI i_GetModuleFileNameW(void*m,uint16_t*buf,uint32_t sz){ (void)m; uint32_t i=0; for(; i<sz && g_exewin[i]; i++) buf[i]=g_exewin[i]; if(i<sz) buf[i]=0; return i; }
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
    char *av[80]; int ai=0; av[ai++]=(char*)g_self; av[ai++]=exe;
    // PELOAD_WORKER_DROP: comma-separated worker args to drop (experiment with the
    // engine-probe flags, e.g. drop --guardian-slayer so the menu fallback runs).
    const char *dropv=getenv("PELOAD_WORKER_DROP");
    for(int k=1;k<nt && ai<69;k++){
        if(dropv && tok[k][0]=='-'){ int drop=0; const char*d=dropv;
            while(*d){ const char*c=tok[k],*e=d; while(*e&&*e!=','&&*e==*c){e++;c++;} if((!*e||*e==',')&&!*c){drop=1;break;} while(*d&&*d!=',')d++; if(*d==',')d++; }
            if(drop){ ws("    [menu-mode] dropped worker arg: "); ws(tok[k]); ws("\n"); continue; } }
        av[ai++]=tok[k];
    }
    // PELOAD_WORKER_ADD: extra space-separated worker args to append.
    static char addbuf[1024]; const char *addv=getenv("PELOAD_WORKER_ADD");
    if(addv){ int bi=0; while(*addv&&bi<1023){ while(*addv==' ')addv++; if(!*addv)break; char*s=&addbuf[bi];
        while(*addv&&*addv!=' '&&bi<1022) addbuf[bi++]=*addv++; addbuf[bi++]=0; if(ai<78){ av[ai++]=s; ws("    [menu-mode] added worker arg: "); ws(s); ws("\n"); } } }
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
        if(child_reap(pid,&c,0)){
            // PELOAD_PROBE_EXIT0: tell the orchestrator the probe worker succeeded
            // (report 0) so it proceeds to whatever it does on probe success.
            if(c && getenv("PELOAD_PROBE_EXIT0")){ ws("  [probe-exit0] reporting worker success to orchestrator\n"); c=0; }
            if(code)*code=c; if(getenv("PELOAD_TRACE_GL")){char t[48];snprintf(t,sizeof t,"  [worker exit] code=0x%x\n",c);ws(t);} }
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
static uint32_t MSABI i_GetModuleFileNameA(void*m,char*buf,uint32_t sz){ (void)m; uint32_t i=0; for(; i<sz && g_exewinA[i]; i++) buf[i]=g_exewinA[i]; if(i<sz) buf[i]=0; return i; }
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
// ---- minimal Win32 window-creation protocol (so winit/eframe work) ----------
// winit's CreateWindowEx relies on the WndProc being called synchronously with
// WM_NCCREATE/WM_CREATE (carrying the CREATESTRUCT) to record the HWND; without
// it winit panics 'initdata.window.unwrap() on None'. Register the class WndProc
// and dispatch those messages. WNDCLASS(EX)W both have lpfnWndProc at +8 and
// lpszClassName at +64.
typedef int64_t (MSABI *WNDPROC_t)(void*,uint32_t,uint64_t,int64_t);
static struct { int used; uint16_t atom; char name[64]; WNDPROC_t wndproc; } g_wcls[64]; static int g_nwcls; static uint16_t g_atomseq=0xC000;
typedef struct { uint32_t magic; WNDPROC_t wndproc; int64_t userdata; int64_t extra[16]; int x,y,w,h; } WWin;
#define WWIN_MAGIC 0x57494E44u
static WNDPROC_t wcls_find(const void*cls){ uintptr_t v=(uintptr_t)cls;
    if(v>0xffff){ char nm[64]; const uint16_t*w=cls; int i=0; for(; w[i]&&i<63;i++) nm[i]=(char)(w[i]&0xff); nm[i]=0;
        for(int k=0;k<g_nwcls;k++) if(g_wcls[k].used&&!strcmp(g_wcls[k].name,nm)) return g_wcls[k].wndproc; }
    else for(int k=0;k<g_nwcls;k++) if(g_wcls[k].used&&g_wcls[k].atom==(uint16_t)v) return g_wcls[k].wndproc;
    return 0; }
static uint16_t wcls_register(const void*wc){ if(!wc||g_nwcls>=64) return 0xC001;
    const uint8_t*p=wc; WNDPROC_t wp=*(WNDPROC_t*)(p+8); const uint16_t*cn=*(const uint16_t**)(p+64);
    int k=g_nwcls++; g_wcls[k].used=1; g_wcls[k].atom=g_atomseq++; g_wcls[k].wndproc=wp;
    int i=0; if(cn) for(; cn[i]&&i<63;i++) g_wcls[k].name[i]=(char)(cn[i]&0xff); g_wcls[k].name[i]=0;
    return g_wcls[k].atom; }
static uint16_t MSABI i_RegisterClassExW(const void*wc){ return wcls_register(wc); }
static uint16_t MSABI i_RegisterClassW(const void*wc){ return wcls_register(wc); }
// RegisterWindowMessage*: unique non-zero message id per string (winit uses it)
static struct { char n[96]; uint32_t id; } g_winmsg[128]; static int g_nwinmsg; static uint32_t g_winmsgseq=0xC000;
static uint32_t winmsg_reg(const char*s){ if(!s) return 0; for(int i=0;i<g_nwinmsg;i++) if(!strcmp(g_winmsg[i].n,s)) return g_winmsg[i].id;
    if(g_nwinmsg<128){ int k=g_nwinmsg++; snprintf(g_winmsg[k].n,sizeof g_winmsg[k].n,"%s",s); g_winmsg[k].id=g_winmsgseq++; return g_winmsg[k].id; } return 0xC000; }
static uint32_t MSABI i_RegisterWindowMessageA(const char*s){ return winmsg_reg(s); }
static uint32_t MSABI i_RegisterWindowMessageW(const uint16_t*s){ char a[96]; int i=0; for(; s&&s[i]&&i<95;i++)a[i]=(char)(s[i]&0xff); a[i]=0; return winmsg_reg(a); }
static int    MSABI i_UnregisterClassW(const void*a,void*b){ (void)a;(void)b; return 1; }
static void*  MSABI i_LoadIconW(void*h,const void*n){ (void)h;(void)n; return (void*)0x1C01; }
static void*  MSABI i_LoadCursorW(void*h,const void*n){ (void)h;(void)n; return (void*)0x1C02; }
static void*  MSABI i_LoadImageW(void*h,const void*n,uint32_t t,int cx,int cy,uint32_t f){ (void)h;(void)n;(void)t;(void)cx;(void)cy;(void)f; return (void*)0x1C03; }
static void*  MSABI i_GetStockObject(int i){ (void)i; return (void*)0x1C04; }
static void*  MSABI i_CreateWindowExW(uint32_t ex,const void*cls,const void*name,uint32_t style,int x,int y,int w,int h,void*parent,void*menu,void*inst,void*param){
    WWin* win=calloc(1,sizeof *win); if(!win) return (void*)0x4857;
    win->magic=WWIN_MAGIC; win->wndproc=wcls_find(cls); win->x=x; win->y=y; win->w=w>0?w:1280; win->h=h>0?h:720;
    if(win->wndproc){
        uint8_t cs[80]; memset(cs,0,sizeof cs);  // CREATESTRUCTW
        *(const void**)(cs+0)=param; *(void**)(cs+8)=inst; *(void**)(cs+16)=menu; *(void**)(cs+24)=parent;
        *(int*)(cs+32)=h; *(int*)(cs+36)=w; *(int*)(cs+40)=y; *(int*)(cs+44)=x; *(uint32_t*)(cs+48)=style;
        *(const void**)(cs+56)=name; *(const void**)(cs+64)=cls; *(uint32_t*)(cs+72)=ex;
        win->wndproc(win, 0x0081/*WM_NCCREATE*/, 0, (int64_t)(intptr_t)cs);
        win->wndproc(win, 0x0001/*WM_CREATE*/,  0, (int64_t)(intptr_t)cs);
    }
    ws("  [CreateWindowExW] real HWND\n"); return win; }
static int    MSABI i_DestroyWindow(void*h){ (void)h; return 1; }
static int64_t MSABI i_DefWindowProcW(void*h,uint32_t m,uint64_t wp,int64_t lp){ (void)h;(void)wp;(void)lp; if(m==0x0081/*WM_NCCREATE*/) return 1; return 0; }
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
static int64_t MSABI i_GetWindowLongPtrW(void*h,int idx){ WWin*w=h; if(w&&w->magic==WWIN_MAGIC){ if(idx==-21)return w->userdata; if(idx==-4)return (int64_t)(intptr_t)w->wndproc; if(idx>=0&&idx<128)return w->extra[idx/8]; return 0; } return (idx>=-8&&idx<0)? g_winlong[-idx-1]:0; }
static int64_t MSABI i_SetWindowLongPtrW(void*h,int idx,int64_t v){ WWin*w=h; if(w&&w->magic==WWIN_MAGIC){ int64_t o=0; if(idx==-21){o=w->userdata;w->userdata=v;} else if(idx==-4){o=(int64_t)(intptr_t)w->wndproc;w->wndproc=(WNDPROC_t)(intptr_t)v;} else if(idx>=0&&idx<128){o=w->extra[idx/8];w->extra[idx/8]=v;} return o; } if(idx>=-8&&idx<0){ int64_t o=g_winlong[-idx-1]; g_winlong[-idx-1]=v; return o;} return 0; }
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
// ---- GDI DIB surface (the menu renders into a CPU bitmap, then presents) ----
// The engine builds its menu surface with CreateCompatibleDC + CreateDIBSection
// and draws into the DIB bits. Stubs returned NULL -> 'Cannot allocate menu
// surface' + NULL deref. Provide real memory-backed DIB surfaces.
typedef struct { uint32_t magic; int w,h,bpp; void*bits; int owns; } GdiObj;
#define GDI_MAGIC 0x47444942u
static GdiObj* gdi_new(int w,int h,int bpp,void*bits,int owns){ GdiObj*g=calloc(1,sizeof*g); if(!g)return 0; g->magic=GDI_MAGIC; g->w=w; g->h=h; g->bpp=bpp; g->bits=bits; g->owns=owns; return g; }
typedef struct { uint32_t magic; GdiObj*bmp; } GdiDC;
#define GDIDC_MAGIC 0x47444344u
static void*  MSABI i_CreateCompatibleDC(void*hdc){ (void)hdc; GdiDC*d=calloc(1,sizeof*d); if(d)d->magic=GDIDC_MAGIC; return d?d:(void*)0xDC02; }
static void*  MSABI i_CreateDIBSection(void*hdc,const uint8_t*bmi,uint32_t usage,void**ppvBits,void*hSection,uint32_t off){
    (void)hdc;(void)usage;(void)hSection;(void)off;
    int w=bmi?*(const int32_t*)(bmi+4):0; int h=bmi?*(const int32_t*)(bmi+8):0; int bpp=bmi?*(const uint16_t*)(bmi+14):32;
    if(bpp<=0)bpp=32; int ah=h<0?-h:h; if(w<=0)w=1; if(ah<=0)ah=1; size_t sz=(size_t)w*ah*((bpp+7)/8); if(sz<4)sz=4;
    void*bits=calloc(1,sz); GdiObj*g=gdi_new(w,ah,bpp,bits,1);
    if(ppvBits)*ppvBits=bits;
    if(getenv("PELOAD_TRACE_GDI")){ char t[96]; snprintf(t,sizeof t,"  [CreateDIBSection] %dx%d %dbpp bits=%p\n",w,ah,bpp,bits); ws(t);}
    return g; }
static void*  MSABI i_CreateCompatibleBitmap(void*hdc,int w,int h){ (void)hdc; if(w<=0)w=1; if(h<=0)h=1; void*b=calloc(1,(size_t)w*h*4); return gdi_new(w,h,32,b,1); }
static void*  MSABI i_SelectObject(void*hdc,void*obj){ GdiDC*d=hdc; GdiObj*g=obj; void*prev=0;
    if(d&&d->magic==GDIDC_MAGIC){ prev=d->bmp; if(g&&g->magic==GDI_MAGIC) d->bmp=g; } return prev?prev:(void*)0x1; }
static int    MSABI i_DeleteObject(void*obj){ GdiObj*g=obj; if(g&&g->magic==GDI_MAGIC){ if(g->owns)free(g->bits); g->magic=0; free(g); } return 1; }
static int    MSABI i_DeleteDC(void*hdc){ GdiDC*d=hdc; if(d&&d->magic==GDIDC_MAGIC){ d->magic=0; free(d); } return 1; }
static int    MSABI i_GetObjectW(void*obj,int cb,void*out){ GdiObj*g=obj; if(!g||g->magic!=GDI_MAGIC||!out) return 0;
    // BITMAP { LONG type; LONG w; LONG h; LONG widthBytes; WORD planes; WORD bpp; void* bits; }
    if(cb>=32){ int32_t*p=out; p[0]=0; p[1]=g->w; p[2]=g->h; p[3]=g->w*((g->bpp+7)/8); *(uint16_t*)(p+4)=1; *((uint16_t*)(p+4)+1)=(uint16_t)g->bpp; *(void**)((uint8_t*)out+24)=g->bits; return 32; } return 0; }
static int    MSABI i_BitBlt(void*dst,int x,int y,int w,int h,void*src,int sx,int sy,uint32_t rop){ (void)dst;(void)x;(void)y;(void)w;(void)h;(void)src;(void)sx;(void)sy;(void)rop; return 1; }
static int    MSABI i_StretchBlt(void*dst,int x,int y,int w,int h,void*src,int sx,int sy,int sw,int sh,uint32_t rop){ (void)dst;(void)x;(void)y;(void)w;(void)h;(void)src;(void)sx;(void)sy;(void)sw;(void)sh;(void)rop; return 1; }
static int    MSABI i_SetDIBitsToDevice(void*dc,int x,int y,uint32_t w,uint32_t h,int sx,int sy,uint32_t sl,uint32_t nl,const void*bits,const void*bmi,uint32_t u){ (void)dc;(void)x;(void)y;(void)w;(void)h;(void)sx;(void)sy;(void)sl;(void)nl;(void)bits;(void)bmi;(void)u; return (int)h; }
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
// ---- real Cocoa window + NSOpenGLContext ------------------------------------
// The engine's menu renders to the default framebuffer; an offscreen CGL context
// has none (GL_FRAMEBUFFER_UNDEFINED -> 'Incomplete menu framebuffer'). Make a
// real NSWindow with an NSOpenGLContext so FBO 0 is complete AND visible.
#include <objc/runtime.h>
#include <objc/message.h>
typedef struct { double x,y,w,h; } NSRect_;
static id g_nsapp, g_nswin, g_nsctx; extern id NSDefaultRunLoopMode;
extern void* objc_autoreleasePoolPush(void);
static id ocls(const char*n){ return (id)objc_getClass(n); }
static SEL osel(const char*n){ return sel_registerName(n); }
static id om0(id o,const char*s){ return ((id(*)(id,SEL))objc_msgSend)(o,osel(s)); }
static id oretain(id o){ return ((id(*)(id,SEL))objc_msgSend)(o,osel("retain")); }
static CGLContextObj cocoa_make_gl_window(int w,int h){
    if(g_nsctx){ return (CGLContextObj)((void*(*)(id,SEL))objc_msgSend)(g_nsctx,osel("CGLContextObj")); }
    objc_autoreleasePoolPush(); // AppKit object creation needs a pool (we never run the runloop); leak it so objects live
    g_nsapp = om0(ocls("NSApplication"),"sharedApplication");
    ((void(*)(id,SEL,long))objc_msgSend)(g_nsapp,osel("setActivationPolicy:"),0); // Regular
    om0(g_nsapp,"finishLaunching");
    uint32_t attrs[]={5/*DoubleBuffer*/,99/*Profile*/,0x4100/*4.1 Core*/,8/*ColorSize*/,24,11/*AlphaSize*/,8,12/*DepthSize*/,24,13/*Stencil*/,8,73/*Accelerated*/,72/*NoRecovery*/,0};
    id pf = ((id(*)(id,SEL,const uint32_t*))objc_msgSend)(om0(ocls("NSOpenGLPixelFormat"),"alloc"),osel("initWithAttributes:"),attrs);
    id ctx = ((id(*)(id,SEL,id,id))objc_msgSend)(om0(ocls("NSOpenGLContext"),"alloc"),osel("initWithFormat:shareContext:"),pf,(id)0);
    NSRect_ rect={0,0,(double)w,(double)h};
    id win = ((id(*)(id,SEL,NSRect_,unsigned long,unsigned long,signed char))objc_msgSend)(
        om0(ocls("NSWindow"),"alloc"),osel("initWithContentRect:styleMask:backing:defer:"),rect,15/*Titled|Closable|Mini|Resizable*/,2/*Buffered*/,0);
    id title = ((id(*)(id,SEL,const char*))objc_msgSend)(ocls("NSString"),osel("stringWithUTF8String:"),"Project Reclaimer (Mac, Wine-free)");
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("setTitle:"),title);
    id view = om0(win,"contentView");
    ((void(*)(id,SEL,id))objc_msgSend)(g_nsctx=ctx,osel("setView:"),view); // setView wants main thread; fine on the GL thread here
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("makeKeyAndOrderFront:"),(id)0);
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("center"),(id)0);
    ((void(*)(id,SEL,signed char))objc_msgSend)(g_nsapp,osel("activateIgnoringOtherApps:"),1);
    om0(ctx,"makeCurrentContext"); om0(ctx,"update"); // bind + validate the drawable
    oretain(ctx); oretain(win); oretain(pf); g_nswin=win;   // keep alive past any pool drain
    ws("  [cocoa] NSWindow + NSOpenGLContext created (visible)\n");
    return (CGLContextObj)((void*(*)(id,SEL))objc_msgSend)(ctx,osel("CGLContextObj"));
}
static void cocoa_pump(void){ if(!g_nsapp) return;
    for(;;){ id ev=((id(*)(id,SEL,unsigned long,id,id,signed char))objc_msgSend)(g_nsapp,osel("nextEventMatchingMask:untilDate:inMode:dequeue:"),~0UL,(id)0,NSDefaultRunLoopMode,1);
        if(!ev) break; ((void(*)(id,SEL,id))objc_msgSend)(g_nsapp,osel("sendEvent:"),ev); } }
static int    MSABI i_ChoosePixelFormat(void*hdc,const void*pfd){ (void)hdc;(void)pfd; return 1; }
static int    MSABI i_SetPixelFormat(void*hdc,int fmt,const void*pfd){ (void)hdc;(void)fmt;(void)pfd; return 1; }
static int    MSABI i_GetPixelFormat(void*hdc){ (void)hdc; return 1; }
static int    MSABI i_DescribePixelFormat(void*hdc,int fmt,uint32_t bytes,uint8_t*pfd){ (void)hdc;(void)fmt;
    if(pfd&&bytes>=40){ memset(pfd,0,bytes); *(uint16_t*)pfd=(uint16_t)(bytes<40?bytes:40); *(uint16_t*)(pfd+2)=1; *(uint32_t*)(pfd+4)=0x25; pfd[8]=32; pfd[23]=24; pfd[27]=8; } return 1; } // PFD_DRAW_TO_WINDOW|SUPPORT_OPENGL|DOUBLEBUFFER, 32bpp, 24 depth
static void*  MSABI i_wglCreateContext(void*hdc){ (void)hdc;
    if(getenv("PELOAD_WINDOW")){ CGLContextObj ctx = cocoa_make_gl_window(1280,720); if(ctx){ g_cgl=ctx; CGLSetCurrentContext(ctx); ws("  [wgl] CGL context (windowed)\n"); return ctx; } }
    CGLPixelFormatAttribute at[]={kCGLPFAAccelerated,kCGLPFADoubleBuffer,kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,kCGLPFADepthSize,(CGLPixelFormatAttribute)24,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf=0; GLint n=0; if(CGLChoosePixelFormat(at,&pf,&n)!=kCGLNoError||!pf){ CGLPixelFormatAttribute b[]={kCGLPFAColorSize,(CGLPixelFormatAttribute)24,(CGLPixelFormatAttribute)0}; CGLChoosePixelFormat(b,&pf,&n); }
    CGLContextObj c2=0; if(pf){ CGLCreateContext(pf,0,&c2); CGLDestroyPixelFormat(pf); } if(c2){ g_cgl=c2; CGLSetCurrentContext(c2); }
    ws(c2?"  [wgl] CGL context (offscreen) created\n":"  [wgl] CGL context FAILED\n"); return c2; }
// ---- WGL ARB extensions (glutin/eframe enumerate pixel formats via these) ----
static const char* MSABI i_wglGetExtensionsStringARB(void*hdc){ (void)hdc; return "WGL_ARB_pixel_format WGL_ARB_create_context WGL_ARB_create_context_profile WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_multisample WGL_EXT_swap_control WGL_ARB_framebuffer_sRGB"; }
static const char* MSABI i_wglGetExtensionsStringEXT(void){ return i_wglGetExtensionsStringARB(0); }
static int MSABI i_wglChoosePixelFormatARB(void*hdc,const int*ia,const float*fa,uint32_t maxf,int*formats,uint32_t*nfmt){ (void)hdc;(void)ia;(void)fa; if(maxf>=1&&formats)formats[0]=1; if(nfmt)*nfmt=1; return 1; }
static int MSABI i_wglGetPixelFormatAttribivARB(void*hdc,int fmt,int layer,uint32_t n,const int*attribs,int*values){ (void)hdc;(void)fmt;(void)layer;
    for(uint32_t i=0;i<n;i++){ int a=attribs[i],v=0; switch(a){
        case 0x2001/*DRAW_TO_WINDOW*/: case 0x2010/*SUPPORT_OPENGL*/: case 0x2011/*DOUBLE_BUFFER*/: v=1; break;
        case 0x2003/*ACCELERATION*/: v=0x2027/*FULL_ACCELERATION*/; break;
        case 0x2013/*PIXEL_TYPE*/: v=0x202B/*TYPE_RGBA*/; break;
        case 0x2014/*COLOR_BITS*/: v=32; break;
        case 0x2015/*RED_BITS*/: case 0x2017/*GREEN*/: case 0x2019/*BLUE*/: case 0x201B/*ALPHA*/: v=8; break;
        case 0x2016/*RED_SHIFT*/: v=0; break; case 0x2018: v=8; break; case 0x201A: v=16; break; case 0x201C: v=24; break;
        case 0x2022/*DEPTH_BITS*/: v=24; break; case 0x2023/*STENCIL_BITS*/: v=8; break;
        case 0x2041/*SAMPLE_BUFFERS*/: case 0x2042/*SAMPLES*/: case 0x200A/*AUX_BUFFERS*/: case 0x2012/*STEREO*/: case 0x2006/*TRANSPARENT*/: v=0; break;
        case 0x2000/*NUMBER_PIXEL_FORMATS*/: v=1; break;
        case 0x20A9/*FRAMEBUFFER_SRGB_CAPABLE*/: v=1; break;
        default: v=0; } if(values)values[i]=v; }
    return 1; }
static void* MSABI i_wglCreateContextAttribsARB(void*hdc,void*share,const int*attribs){ (void)share;(void)attribs; return i_wglCreateContext(hdc); }
static int MSABI i_wglSwapIntervalEXT_(int i){ (void)i; return 1; }
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
static unsigned MSABI i_glCheckFramebufferStatus(unsigned target){
    unsigned(*real)(unsigned)=dlsym(RTLD_DEFAULT,"glCheckFramebufferStatus"); unsigned s=real?real(target):0;
    if(s!=0x8CD5/*COMPLETE*/){ char t[80]; snprintf(t,sizeof t,"  [glCheckFramebufferStatus] target=0x%x -> 0x%x (INCOMPLETE)\n",target,s); ws(t);}
    else if(getenv("PELOAD_TRACE_GL")){ ws("  [glCheckFramebufferStatus] COMPLETE\n"); }
    return s; }
static unsigned gl_err(void){ unsigned(*g)(void)=dlsym(RTLD_DEFAULT,"glGetError"); return g?g():0; }
// Clamp requested MSAA sample count to one macOS supports, consistently, so
// color+depth multisample attachments match (else INCOMPLETE_ATTACHMENT 0x8CD6).
static int gl_clamp_samples(int samp){ static int maxs=-1; if(maxs<0){ void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv"); maxs=4; if(gi){int m=0; gi(0x8D57/*GL_MAX_SAMPLES*/,&m); if(m>0)maxs=m;} }
    if(samp<1) samp=1; if(samp>maxs) samp=maxs; if(samp>4) samp=4; return samp; }
static void MSABI i_glTexStorage2D(unsigned t,int lv,unsigned ifmt,int w,int h){ gl_err(); void(*r)(unsigned,int,unsigned,int,int)=dlsym(RTLD_DEFAULT,"glTexStorage2D"); if(r)r(t,lv,ifmt,w,h);
    char s[140]; snprintf(s,sizeof s,"  [glTexStorage2D] ifmt=0x%x %dx%d lv=%d err=0x%x\n",ifmt,w,h,lv,gl_err()); ws(s); }
static void MSABI i_glTexStorage2DMultisample(unsigned t,int samp,unsigned ifmt,int w,int h,unsigned char fx){ samp=gl_clamp_samples(samp); gl_err(); void(*r)(unsigned,int,unsigned,int,int,unsigned char)=dlsym(RTLD_DEFAULT,"glTexStorage2DMultisample"); if(r)r(t,samp,ifmt,w,h,fx);
    char s[160]; snprintf(s,sizeof s,"  [glTexStorage2DMultisample] samples=%d ifmt=0x%x %dx%d err=0x%x\n",samp,ifmt,w,h,gl_err()); ws(s); }
static void MSABI i_glTexImage2DMultisample(unsigned t,int samp,unsigned ifmt,int w,int h,unsigned char fx){ samp=gl_clamp_samples(samp); gl_err(); void(*r)(unsigned,int,unsigned,int,int,unsigned char)=dlsym(RTLD_DEFAULT,"glTexImage2DMultisample"); if(r)r(t,samp,ifmt,w,h,fx);
    char s[160]; snprintf(s,sizeof s,"  [glTexImage2DMultisample] samples=%d ifmt=0x%x %dx%d err=0x%x\n",samp,ifmt,w,h,gl_err()); ws(s); }
static void MSABI i_glRenderbufferStorage(unsigned t,unsigned ifmt,int w,int h){ gl_err(); void(*r)(unsigned,unsigned,int,int)=dlsym(RTLD_DEFAULT,"glRenderbufferStorage"); if(r)r(t,ifmt,w,h);
    char s[140]; snprintf(s,sizeof s,"  [glRenderbufferStorage] ifmt=0x%x %dx%d err=0x%x\n",ifmt,w,h,gl_err()); ws(s); }
static void MSABI i_glRenderbufferStorageMultisample(unsigned t,int samp,unsigned ifmt,int w,int h){ samp=gl_clamp_samples(samp); gl_err(); void(*r)(unsigned,int,unsigned,int,int)=dlsym(RTLD_DEFAULT,"glRenderbufferStorageMultisample"); if(r)r(t,samp,ifmt,w,h);
    char s[160]; snprintf(s,sizeof s,"  [glRenderbufferStorageMultisample] samples=%d ifmt=0x%x %dx%d err=0x%x\n",samp,ifmt,w,h,gl_err()); ws(s); }
static void MSABI i_glTexImage2D(unsigned t,int l,int ifmt,int w,int h,int b,unsigned fmt,unsigned type,const void*d){ void(*r)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glTexImage2D"); if(r)r(t,l,ifmt,w,h,b,fmt,type,d);
    if(getenv("PELOAD_TRACE_GL")){ char s[160]; snprintf(s,sizeof s,"  [glTexImage2D] ifmt=0x%x %dx%d fmt=0x%x type=0x%x\n",ifmt,w,h,fmt,type); ws(s);} }
static void gl_capture(const char*tag); // fwd
static void gl_capture_wh(const char*tag,int w,int h,int isfbo); // fwd
// Resolve-capture: blit the current draw FBO (possibly multisample) into our own
// single-sample FBO, then read THAT. Overwrites one BMP so the last frame drawn
// before the crash survives. This is how we grab the menu mid-render.
static unsigned g_capfbo, g_captex; static int g_capinit;
static void gl_capture_resolve(void){
    static int ncap; if(ncap>400) return;
    void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv");
    void(*gf)(int,unsigned*)=dlsym(RTLD_DEFAULT,"glGenFramebuffers");
    void(*gt)(int,unsigned*)=dlsym(RTLD_DEFAULT,"glGenTextures");
    void(*bt)(unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBindTexture");
    void(*ti)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glTexImage2D");
    void(*bf)(unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
    void(*ft)(unsigned,unsigned,unsigned,unsigned,int)=dlsym(RTLD_DEFAULT,"glFramebufferTexture2D");
    void(*bl)(int,int,int,int,int,int,int,int,unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBlitFramebuffer");
    void(*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels");
    void(*rb)(unsigned)=dlsym(RTLD_DEFAULT,"glReadBuffer");
    if(!gi||!gf||!gt||!bt||!ti||!bf||!ft||!bl||!rp) return;
    int W=1280,H=720; int vp[4]={0,0,0,0}; gi(0x0BA2,vp); if(vp[2]>0&&vp[3]>0){W=vp[2];H=vp[3];} if(W>4096)W=4096; if(H>4096)H=4096;
    int drawfb=0,readfb=0; gi(0x8CA6,&drawfb); gi(0x8CAA,&readfb);
    if(!g_capinit){ gf(1,&g_capfbo); gt(1,&g_captex); bt(0x0DE1,g_captex); ti(0x0DE1,0,0x8058,W,H,0,0x1908,0x1401,0); bf(0x8D40,g_capfbo); ft(0x8D40,0x8CE0,0x0DE1,g_captex,0); g_capinit=1; }
    bf(0x8CA8/*READ_FB*/,(unsigned)drawfb); bf(0x8CA6/*DRAW_FB*/,g_capfbo); gl_err();
    bl(0,0,W,H, 0,0,W,H, 0x4000/*COLOR*/,0x2600/*NEAREST*/);
    bf(0x8CA8,g_capfbo); if(rb) rb(0x8CE0/*COLOR_ATTACHMENT0*/);
    unsigned char*px=calloc(1,(size_t)W*H*4); if(px){ gl_err(); rp(0,0,W,H,0x1908,0x1401,px); unsigned e=gl_err();
        const char*home=getenv("HOME"); char path[512]; snprintf(path,sizeof path,"%s/reclaimer_menu_latest.bmp",home?home:"/tmp");
        FILE*f=fopen(path,"wb"); if(f){ uint32_t isz=(uint32_t)W*H*4,fsz=54+isz,off=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&off,4);
            uint8_t ih[40]={0}; uint32_t v40=40; memcpy(ih,&v40,4); memcpy(ih+4,&W,4); memcpy(ih+8,&H,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
            fwrite(fh,1,14,f); fwrite(ih,1,40,f);
            for(int i=0;i<W*H;i++){ uint8_t*p=px+i*4; uint8_t b[4]={p[2],p[1],p[0],255}; fwrite(b,1,4,f);} fclose(f);
            if(ncap==0||ncap%50==0){ char m[600]; snprintf(m,sizeof m,"  [gl_capture_resolve] #%d %dx%d err=0x%x -> %s\n",ncap,W,H,e,path); ws(m);} }
        free(px); ncap++; }
    bf(0x8CA8,(unsigned)readfb); bf(0x8CA6,(unsigned)drawfb); }
static void MSABI i_glDrawElements(unsigned mode,int count,unsigned type,const void*idx){ void(*r)(unsigned,int,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glDrawElements"); if(r)r(mode,count,type,idx); if(getenv("PELOAD_SHOT")) gl_capture_resolve(); }
static void MSABI i_glBlitFramebuffer(int sx0,int sy0,int sx1,int sy1,int dx0,int dy0,int dx1,int dy1,unsigned mask,unsigned filter){
    void(*r)(int,int,int,int,int,int,int,int,unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBlitFramebuffer");
    if(r)r(sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,mask,filter);
    if(getenv("PELOAD_SHOT")){ void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv"); void(*bf)(unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
        int draw=0,read=0; if(gi){gi(0x8CA6/*DRAW_FB_BINDING*/,&draw); gi(0x8CAA/*READ_FB_BINDING*/,&read);}
        if(bf) bf(0x8CA8/*READ_FRAMEBUFFER*/,(unsigned)draw);          // read the blit destination
        int w=dx1-dx0,h=dy1-dy0; if(w<0)w=-w; if(h<0)h=-h; gl_capture_wh("blit-dst",w,h,draw!=0);
        if(bf) bf(0x8CA8,(unsigned)read); }
}
static void MSABI i_glFinish(void){ void(*r)(void)=dlsym(RTLD_DEFAULT,"glFinish"); if(r)r(); if(getenv("PELOAD_SHOT")) gl_capture("glFinish"); }
// Capture on glClear too: if the engine never reaches a present/draw, at least
// grab whatever is in the framebuffer each time it clears (self-limited to the
// gl_capture_wh 10-shot cap). PELOAD_SHOT_CLEAR gates it (separate from SHOT).
static void MSABI i_glClear(unsigned mask){ void(*r)(unsigned)=dlsym(RTLD_DEFAULT,"glClear"); if(r)r(mask);
    if(getenv("PELOAD_SHOT_CLEAR")) gl_capture("glClear"); }
static void MSABI i_glFlush(void){ void(*r)(void)=dlsym(RTLD_DEFAULT,"glFlush"); if(r)r(); if(getenv("PELOAD_SHOT")) gl_capture("glFlush"); }
// GL resolver: real framework fn via thunk, else a NAMED logging no-op stub so
// a CALL to an unimplemented GL function is visible (not a NULL-deref crash).
static void* gl_or_missing(const char*name){
    if(!strcmp(name,"glFinish")) return (void*)i_glFinish;
    if(!strcmp(name,"glClear")) return (void*)i_glClear;
    if(!strcmp(name,"glFlush")) return (void*)i_glFlush;
    if(!strcmp(name,"glBlitFramebuffer")) return (void*)i_glBlitFramebuffer;
    if(!strcmp(name,"glDrawElements")) return (void*)i_glDrawElements;
    if(!strcmp(name,"glTexStorage2D")) return (void*)i_glTexStorage2D;
    if(!strcmp(name,"glTexStorage2DMultisample")) return (void*)i_glTexStorage2DMultisample;
    if(!strcmp(name,"glTexImage2DMultisample")) return (void*)i_glTexImage2DMultisample;
    if(!strcmp(name,"glRenderbufferStorage")) return (void*)i_glRenderbufferStorage;
    if(!strcmp(name,"glRenderbufferStorageMultisample")) return (void*)i_glRenderbufferStorageMultisample;
    if(!strcmp(name,"glTexImage2D")) return (void*)i_glTexImage2D;
    if(!strcmp(name,"glGetString")) return (void*)i_glGetString;
    if(!strcmp(name,"glShaderSource")||!strcmp(name,"glShaderSourceARB")) return (void*)i_glShaderSource;
    if(!strcmp(name,"glCompileShader")||!strcmp(name,"glCompileShaderARB")) return (void*)i_glCompileShader;
    if(!strcmp(name,"glCheckFramebufferStatus")) return (void*)i_glCheckFramebufferStatus;
    // Missing GL functions MUST resolve to NULL (not a no-op stub): glow/gl
    // loaders null-check to detect capabilities, and a stub makes them call an
    // unavailable function and get garbage. dlsym-found funcs get an ABI thunk.
    void*g=dlsym((void*)-2,name);
    if(getenv("PELOAD_TRACE_GLRES")){ char t[128]; int n=snprintf(t,sizeof t,"  [glres] %s -> dlsym=%p\n",name,g); (void)!write(2,t,n); }
    return g? make_gl_thunk(g) : 0; }
static void*  MSABI i_wglGetProcAddress(const char*name){
    if(getenv("PELOAD_TRACE_GL2")){ ws("  [wglGP] "); ws(name); ws("\n"); }
    if(!strcmp(name,"wglGetExtensionsStringARB")) return (void*)i_wglGetExtensionsStringARB;
    if(!strcmp(name,"wglGetExtensionsStringEXT")) return (void*)i_wglGetExtensionsStringEXT;
    if(!strcmp(name,"wglChoosePixelFormatARB")||!strcmp(name,"wglChoosePixelFormatEXT")) return (void*)i_wglChoosePixelFormatARB;
    if(!strcmp(name,"wglGetPixelFormatAttribivARB")||!strcmp(name,"wglGetPixelFormatAttribivEXT")) return (void*)i_wglGetPixelFormatAttribivARB;
    if(!strcmp(name,"wglCreateContextAttribsARB")) return (void*)i_wglCreateContextAttribsARB;
    if(!strcmp(name,"wglSwapIntervalEXT")) return (void*)i_wglSwapIntervalEXT_;
    return gl_or_missing(name); }
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
// Capture a framebuffer region to a BMP so we can SEE the menu. Reads the
// currently-bound GL_READ_FRAMEBUFFER (caller sets it up).
static void gl_capture_wh(const char*tag,int w,int h,int isfbo){
    static int nshot; if(nshot>=10) return;
    void(*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels");
    void(*rb)(unsigned)=dlsym(RTLD_DEFAULT,"glReadBuffer");
    if(!rp||w<=0||h<=0||(long)w*h>40000000) return;
    unsigned char*px=calloc(1,(size_t)w*h*4); if(!px) return;
    gl_err(); if(rb) rb(isfbo?0x8CE0/*COLOR_ATTACHMENT0*/:0x0405/*GL_BACK*/);
    rp(0,0,w,h,0x1908/*RGBA*/,0x1401,px);
    unsigned e=gl_err();
    const char*home=getenv("HOME"); char path[512]; snprintf(path,sizeof path,"%s/reclaimer_shot_%d.bmp",home?home:"/tmp",nshot++);
    FILE*f=fopen(path,"wb"); if(f){ uint32_t isz=(uint32_t)w*h*4; uint8_t fh[14]={'B','M'}; uint32_t fsz=54+isz; memcpy(fh+2,&fsz,4); uint32_t off=54; memcpy(fh+10,&off,4);
        uint8_t ih[40]={0}; uint32_t v40=40; memcpy(ih,&v40,4); memcpy(ih+4,&w,4); memcpy(ih+8,&h,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
        fwrite(fh,1,14,f); fwrite(ih,1,40,f);
        for(int i=0;i<w*h;i++){ uint8_t*p=px+i*4; uint8_t bgra[4]={p[2],p[1],p[0],p[3]}; fwrite(bgra,1,4,f); } fclose(f);
        char m[600]; snprintf(m,sizeof m,"  [gl_capture] %s %dx%d err=0x%x -> %s\n",tag,w,h,e,path); ws(m); }
    free(px); }
static void gl_capture(const char*tag){ void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv"); int vp[4]={0,0,1280,720}; if(gi)gi(0x0BA2,vp); gl_capture_wh(tag,vp[2]>0?vp[2]:1280,vp[3]>0?vp[3]:720,0); }
static int    MSABI i_SwapBuffers(void*hdc){ (void)hdc;
    if(getenv("PELOAD_SHOT")) gl_capture("SwapBuffers");
    if(g_nsctx){ om0(g_nsctx,"flushBuffer"); return 1; }
    if(g_cgl) CGLFlushDrawable(g_cgl); return 1; }
// ============================================================================
// Windows x64 SEH / C++ exception unwinding. The genuine vcruntime140 we load
// provides __CxxFrameHandler3 (the C++/Rust catch personality) and
// _CxxThrowException; it stands on these NTDLL primitives, which we implement so
// a Rust panic / C++ throw actually UNWINDS to its catch_unwind/catch instead of
// terminating. CONTEXT integer regs live at 0x78 + reg*8 (reg = the DWARF/UNWIND
// register number: 0=Rax,1=Rcx,2=Rdx,3=Rbx,4=Rsp,5=Rbp,6=Rsi,7=Rdi,8..15=R8..R15),
// Rip at 0xf8, Xmm0 at 0x1a0 (16 bytes each).
#define CTXSZ 1256
#define CTX_REG(c,r) (*(uint64_t*)((uint8_t*)(c)+0x78+(r)*8))
#define CTX_RSP(c)   (*(uint64_t*)((uint8_t*)(c)+0x98))
#define CTX_RIP(c)   (*(uint64_t*)((uint8_t*)(c)+0xf8))
typedef struct { uint32_t Begin, End, Unwind; } RUNFN;     // RUNTIME_FUNCTION (RVAs)
typedef struct { uint32_t ExceptionCode, ExceptionFlags; void* ExceptionRecord; void* ExceptionAddress;
    uint32_t NumberParameters, u; uint64_t ExceptionInformation[15]; } EXREC;
typedef struct { EXREC* ExceptionRecord; void* ContextRecord; } EXCEPTION_POINTERS;
typedef int (MSABI *PEXHANDLER)(EXREC*, uint64_t frame, void* ctx, void* disp);
typedef long (MSABI *PVEH)(EXCEPTION_POINTERS*);
typedef struct { uint64_t ControlPc, ImageBase; RUNFN* FunctionEntry; uint64_t EstablisherFrame, TargetIp;
    void* ContextRecord; PEXHANDLER LanguageHandler; void* HandlerData; void* HistoryTable; uint32_t ScopeIndex, Fill0; } DISPCTX;
// register capture / restore (defined in module asm below)
extern void MSABI seh_capture(void* ctx);
extern void MSABI seh_restore(void* ctx);   // restore CONTEXT and jump to its Rip (no return)
static void MSABI i_RtlCaptureContext(void*ctx){ if(ctx){ memset(ctx,0,CTXSZ); seh_capture(ctx); } }

// find the module (exe + guest DLLs) containing pc; return base+ImageSize
static uint8_t* seh_module(uint64_t pc, uint64_t* obase, uint32_t* osize){
    uint8_t* mods[17]; int nm=0; if(g_base) mods[nm++]=g_base; for(int i=0;i<g_nmods&&nm<17;i++) mods[nm++]=g_mods[i].base;
    for(int i=0;i<nm;i++){ uint8_t*b=mods[i]; DOS*d=(DOS*)b; OPT64*o=(OPT64*)(b+d->e_lfanew+sizeof(COFF));
        if(pc>=(uint64_t)b && pc<(uint64_t)b+o->ImageSize){ if(obase)*obase=(uint64_t)b; if(osize)*osize=o->ImageSize; return b; } }
    return 0; }
static RUNFN* MSABI i_RtlLookupFunctionEntry(uint64_t pc,uint64_t*base,void*hist){ (void)hist;
    uint64_t b=0; uint8_t* mod=seh_module(pc,&b,0); if(base)*base=b; if(!mod) return 0;
    DOS*d=(DOS*)mod; OPT64*o=(OPT64*)(mod+d->e_lfanew+sizeof(COFF));
    uint32_t prva=o->Dir[3].VA, psz=o->Dir[3].Size; if(!prva||!psz) return 0;  // IMAGE_DIRECTORY_ENTRY_EXCEPTION
    RUNFN* fns=(RUNFN*)(mod+prva); int n=psz/sizeof(RUNFN); uint32_t rva=(uint32_t)(pc-b);
    int lo=0,hi=n-1; while(lo<=hi){ int m=(lo+hi)/2; if(rva<fns[m].Begin) hi=m-1; else if(rva>=fns[m].End) lo=m+1; else return &fns[m]; }
    return 0; }
// Interpret UNWIND_INFO: roll CONTEXT back one frame; return the language handler
// (if EHANDLER/UHANDLER requested), set *hdata to the handler-specific data.
static PEXHANDLER MSABI i_RtlVirtualUnwind(uint32_t htype, uint64_t base, uint64_t pc, RUNFN* fn,
        void* ctx, void** hdata, uint64_t* establisher, void* ctxptrs){ (void)ctxptrs;
    uint8_t* info=(uint8_t*)base + fn->Unwind;
    uint8_t ver=info[0]&7, flags=info[0]>>3, prolog=info[1], ncodes=info[2], fr=info[3]&0xf, fro=info[3]>>4;
    (void)ver; uint16_t* codes=(uint16_t*)(info+4);
    uint64_t prolog_off = pc - (base + fn->Begin);
    uint64_t frame_base = fr? (CTX_REG(ctx,fr) - (uint64_t)fro*16) : CTX_RSP(ctx);
    if(establisher) *establisher = frame_base;
    int i=0;
    while(i<ncodes){ uint8_t coff=codes[i]&0xff, op=(codes[i]>>8)&0xf, oi=(codes[i]>>12)&0xf; int slots=1;
        int active = (coff<=prolog_off);   // this prolog op has already executed
        switch(op){
            case 0: /*PUSH_NONVOL*/ if(active){ CTX_REG(ctx,oi)=*(uint64_t*)CTX_RSP(ctx); CTX_RSP(ctx)+=8; } slots=1; break;
            case 1: /*ALLOC_LARGE*/ if(oi==0){ if(active) CTX_RSP(ctx)+=(uint64_t)codes[i+1]*8; slots=2; }
                     else { if(active) CTX_RSP(ctx)+=*(uint32_t*)&codes[i+1]; slots=3; } break;
            case 2: /*ALLOC_SMALL*/ if(active) CTX_RSP(ctx)+=(uint64_t)oi*8+8; slots=1; break;
            case 3: /*SET_FPREG*/   if(active) CTX_RSP(ctx)=CTX_REG(ctx,fr)-(uint64_t)fro*16; slots=1; break;
            case 4: /*SAVE_NONVOL*/ if(active) CTX_REG(ctx,oi)=*(uint64_t*)(frame_base+(uint64_t)codes[i+1]*8); slots=2; break;
            case 5: /*SAVE_NONVOL_FAR*/ if(active) CTX_REG(ctx,oi)=*(uint64_t*)(frame_base+*(uint32_t*)&codes[i+1]); slots=3; break;
            case 8: /*SAVE_XMM128*/ if(active) memcpy((uint8_t*)ctx+0x1a0+oi*16,(void*)(frame_base+(uint64_t)codes[i+1]*16),16); slots=2; break;
            case 9: /*SAVE_XMM128_FAR*/ if(active) memcpy((uint8_t*)ctx+0x1a0+oi*16,(void*)(frame_base+*(uint32_t*)&codes[i+1]),16); slots=3; break;
            case 10:/*PUSH_MACHFRAME*/ if(active){ uint64_t sp=CTX_RSP(ctx)+(oi?8:0); CTX_RIP(ctx)=*(uint64_t*)sp; CTX_RSP(ctx)=*(uint64_t*)(sp+24); } slots=1; break;
            case 6: case 7: default: slots=1; break; // EPILOG/SPARE: ignore
        }
        i+=slots;
    }
    if(flags&4){ /*CHAININFO*/ RUNFN* chain=(RUNFN*)(codes + ((ncodes+1)&~1)); return i_RtlVirtualUnwind(htype,base,base+chain->Begin,chain,ctx,hdata,establisher,0); }
    if(prolog_off >= prolog){ /* in body: pop return address */ CTX_RIP(ctx)=*(uint64_t*)CTX_RSP(ctx); CTX_RSP(ctx)+=8; }
    else { CTX_RIP(ctx)=*(uint64_t*)CTX_RSP(ctx); CTX_RSP(ctx)+=8; } // simplified: treat prolog like body
    if((flags&3) && (htype&3)){ uint32_t* hrva=(uint32_t*)(codes + ((ncodes+1)&~1));
        if(hdata) *hdata = hrva+1; return (PEXHANDLER)(base + *hrva); }
    return 0;
}
// Unwind to TargetFrame, running termination handlers; then jump to TargetIp.
static void MSABI i_RtlUnwindEx(uint64_t target, void* targetip, EXREC* rec, void* retval, void* ctx, void* hist){
    (void)hist; uint8_t tmp[CTXSZ]; if(!ctx){ ctx=tmp; memset(ctx,0,CTXSZ); seh_capture(ctx); }
    EXREC localrec; if(!rec){ memset(&localrec,0,sizeof localrec); localrec.ExceptionCode=0xC0000027/*STATUS_UNWIND*/; rec=&localrec; }
    rec->ExceptionFlags |= 2; // EXCEPTION_UNWINDING
    uint8_t work[CTXSZ];
    for(int guard=0; guard<512; guard++){
        uint64_t base=0; RUNFN* fn=i_RtlLookupFunctionEntry(CTX_RIP(ctx),&base,0);
        if(!fn){ uint64_t sp=CTX_RSP(ctx); if(!sp) break; CTX_RIP(ctx)=*(uint64_t*)sp; CTX_RSP(ctx)=sp+8; if((uint64_t)CTX_RIP(ctx)==0) break; continue; }
        memcpy(work,ctx,CTXSZ); void* hdata=0; uint64_t estab=0;
        PEXHANDLER h=i_RtlVirtualUnwind(2/*UHANDLER*/,base,CTX_RIP(ctx),fn,work,&hdata,&estab,0);
        if(estab==target){ break; }                 // reached the destination frame
        if(h && (estab>CTX_RSP(ctx))){               // run this frame's termination handler
            DISPCTX dc; memset(&dc,0,sizeof dc); dc.ControlPc=CTX_RIP(ctx); dc.ImageBase=base; dc.FunctionEntry=fn;
            dc.EstablisherFrame=estab; dc.ContextRecord=ctx; dc.LanguageHandler=h; dc.HandlerData=hdata; dc.TargetIp=(uint64_t)targetip;
            h(rec, estab, ctx, &dc);
        }
        memcpy(ctx,work,CTXSZ);
        if(estab==target) break;
    }
    CTX_RIP(ctx)=(uint64_t)targetip; CTX_REG(ctx,0)=(uint64_t)retval; // Rax = return value
    seh_restore(ctx); // transfer control to the catch (does not return)
}
static void   MSABI i_RtlRaiseException(void*rec){ (void)rec; }
static void   MSABI i_RtlRestoreContext(void* ctx, void* rec){ (void)rec; seh_restore(ctx); }
static void seh_decode_cpp(uint32_t code,uint32_t n,const void*args); // fwd (logging)
// Dispatch an exception: walk frames, call each EH handler; a C++ catch unwinds
// itself (via RtlUnwindEx) and never returns. If none handles it, we return
// (legacy behavior) so startup logging still works.
static void MSABI i_RaiseException(uint32_t code,uint32_t flags,uint32_t n,const void*args){
    if(code==0x406D1388) return;                 // SetThreadName: ignore
    if(getenv("PELOAD_TRACE_SEH")){ char t[64]; snprintf(t,sizeof t,"  [RaiseException] 0x%08x\n",code); ws(t); }
    seh_decode_cpp(code,n,args);
    uint8_t ctxbuf[CTXSZ]; memset(ctxbuf,0,CTXSZ); void* ctx=ctxbuf; seh_capture(ctx);
    EXREC rec; memset(&rec,0,sizeof rec); rec.ExceptionCode=code; rec.ExceptionFlags=flags;
    rec.ExceptionAddress=(void*)CTX_RIP(ctx); rec.NumberParameters=(n>15?15:n);
    if(args) for(uint32_t i=0;i<rec.NumberParameters;i++) rec.ExceptionInformation[i]=((const uint64_t*)args)[i];
    uint8_t work[CTXSZ];
    for(int guard=0; guard<512; guard++){
        uint64_t base=0; RUNFN* fn=i_RtlLookupFunctionEntry(CTX_RIP(ctx),&base,0);
        if(!fn){ uint64_t sp=CTX_RSP(ctx); if(!sp) break; uint64_t rip=*(uint64_t*)sp; CTX_RSP(ctx)=sp+8; CTX_RIP(ctx)=rip; if(!rip) break; continue; }
        memcpy(work,ctx,CTXSZ); void* hdata=0; uint64_t estab=0;
        PEXHANDLER h=i_RtlVirtualUnwind(1/*EHANDLER*/,base,CTX_RIP(ctx),fn,work,&hdata,&estab,0);
        if(h){
            DISPCTX dc; memset(&dc,0,sizeof dc); dc.ControlPc=CTX_RIP(ctx); dc.ImageBase=base; dc.FunctionEntry=fn;
            dc.EstablisherFrame=estab; dc.ContextRecord=ctx; dc.LanguageHandler=h; dc.HandlerData=hdata;
            int disp=h(&rec, estab, ctx, &dc);     // __CxxFrameHandler3 et al.
            // 0=ContinueExecution, 1=ContinueSearch. A C++ catch does its own
            // RtlUnwindEx+transfer and never returns here.
            if(disp==0){ seh_restore(ctx); }       // resume at (possibly modified) ctx
            // else ContinueSearch: fall through to next frame
        }
        memcpy(ctx,work,CTXSZ);                     // move to the caller frame
        if(!CTX_RIP(ctx)) break;
    }
    // Unhandled. For a Rust panic / C++ throw there is nothing more to do; keep
    // the legacy non-fatal return so non-exception RaiseException callers (and
    // early-startup logging) behave as before.
}
static void seh_decode_cpp(uint32_t code,uint32_t n,const void*args){
    if(!(getenv("PELOAD_TRACE_SEH")) || code!=0xE06D7363 || !args || n<4) return;
    const uint64_t *a=args; void *obj=(void*)a[1]; int32_t *ti=(int32_t*)a[2]; uint64_t base=a[3];
    if(!ti) return; int32_t ctaRVA=ti[3]; uint8_t *cta=(uint8_t*)(base+(uint32_t)ctaRVA);
    int nct=*(int32_t*)cta; int32_t *arr=(int32_t*)(cta+4);
    if(nct>0){ uint8_t *ct=(uint8_t*)(base+(uint32_t)arr[0]); int32_t tdRVA=*(int32_t*)(ct+4);
        const char *name=(const char*)(base+(uint32_t)tdRVA+16); ws("    C++ type: "); ws(name); ws("\n");
        if(obj) for(int off=8; off<=16; off+=8){ const char *m=*(const char**)((uint8_t*)obj+off);
            if(m&&(uint64_t)m>0x10000){ int ok=1; for(int i=0;i<64;i++){char c=m[i]; if(!c)break; if(c<32||c>126){ok=0;break;}} if(ok&&m[0]){ws("    what(): ");ws(m);ws("\n");break;} } } }
}
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
// WaitOnAddress / WakeByAddress*: the Windows futex. Rust's std::sync (Mutex,
// RwLock, Condvar, Once, thread::park) uses these on Windows 8+; no-ops here
// meant Rust locks never actually waited -> data races -> heap corruption and
// the random BTreeMap/Option panics. Implement a real futex (bucketed condvars).
#define WAB_N 512
static struct { pthread_mutex_t m; pthread_cond_t c; } g_wab[WAB_N];
static int g_wab_ready;
static void wab_ensure(void){ static pthread_mutex_t im=PTHREAD_MUTEX_INITIALIZER; if(g_wab_ready) return;
    pthread_mutex_lock(&im); if(!g_wab_ready){ for(int i=0;i<WAB_N;i++){ pthread_mutex_init(&g_wab[i].m,0); pthread_cond_init(&g_wab[i].c,0);} g_wab_ready=1; } pthread_mutex_unlock(&im); }
static int wab_bucket(const volatile void*a){ return (int)(((uintptr_t)a>>3) & (WAB_N-1)); }
static int MSABI i_WaitOnAddress(volatile void*addr, void*cmp, uint64_t size, uint32_t ms){
    wab_ensure(); if(!addr||!cmp||size==0||size>8){ return 1; } int b=wab_bucket(addr);
    pthread_mutex_lock(&g_wab[b].m);
    int changed = memcmp((const void*)addr,cmp,(size_t)size)!=0;
    if(!changed){
        if(ms==0xFFFFFFFF) pthread_cond_wait(&g_wab[b].c,&g_wab[b].m);
        else { struct timeval tv; gettimeofday(&tv,0); uint64_t ns=(uint64_t)tv.tv_usec*1000+(uint64_t)ms*1000000ULL; struct timespec ts; ts.tv_sec=tv.tv_sec+ns/1000000000ULL; ts.tv_nsec=ns%1000000000ULL; pthread_cond_timedwait(&g_wab[b].c,&g_wab[b].m,&ts); }
        changed = memcmp((const void*)addr,cmp,(size_t)size)!=0;
    }
    pthread_mutex_unlock(&g_wab[b].m);
    if(!changed) g_lasterr=1460/*ERROR_TIMEOUT*/; return changed?1:0; }
static void MSABI i_WakeByAddressSingle(void*a){ wab_ensure(); int b=wab_bucket(a); pthread_mutex_lock(&g_wab[b].m); pthread_cond_broadcast(&g_wab[b].c); pthread_mutex_unlock(&g_wab[b].m); }
static void MSABI i_WakeByAddressAll(void*a){ wab_ensure(); int b=wab_bucket(a); pthread_mutex_lock(&g_wab[b].m); pthread_cond_broadcast(&g_wab[b].c); pthread_mutex_unlock(&g_wab[b].m); }
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
    // macOS x86_64 uses %gs as the live pthread TSD base: key K lives at %gs:[K*8].
    // Windows wants TEB at gs:0x30, TLS-array at gs:0x58, PEB at gs:0x60 — which
    // collide with macOS TSD keys 6/11/12. Overwriting them is fine while the
    // thread runs (not on the malloc hot path), but at thread exit
    // _pthread_tsd_cleanup walks every slot and runs its destructor; freeing our
    // non-heap PEB/TEB pointers aborts in malloc_report. Save the originals and
    // restore them before returning so cleanup sees valid macOS TSD values.
    __asm__ volatile("movq %0,%%gs:0x30"::"r"((uint64_t)teb));
    __asm__ volatile("movq %0,%%gs:0x60"::"r"((uint64_t)g_peb_ptr));
    __asm__ volatile("movq %0,%%gs:0x58"::"r"((uint64_t)tarr));
    if(getenv("PELOAD_TRACE_THREAD")){ char tb[96]; int nn=snprintf(tb,sizeof tb,"  [TT] enter tid=0x%x gs58=0x%llx fn=%p\n",pthread_mach_thread_np(pthread_self()),(unsigned long long)(uint64_t)tarr,(void*)t->fn); (void)!write(2,tb,nn); }
    uint32_t r = t->fn ? t->fn(t->param) : 0;
    // The Windows TEB fields live in macOS TSD slots 0x30/0x58/0x60. On this
    // macOS, slot 12 (gs:0x60) has a pthread destructor that FREES the slot value
    // at thread exit (_pthread_tsd_cleanup). We put the *static* PEB pointer there
    // -> free(static) -> malloc abort. So zero ONLY gs:0x60 before returning so
    // cleanup skips it. We deliberately leave gs:0x58 (TLS array) and gs:0x30
    // (TEB self, both heap-backed) intact: zeroing 0x58 would open a window where
    // late guest thread-local destructors fault on a NULL TLS pointer. Leak the
    // teb/tarr (a few KB/thread) rather than risk a use-after-free during cleanup.
    __asm__ volatile("movq $0,%%gs:0x60":::"memory");
    free(t);
    return (void*)(uintptr_t)r;
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
    intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; int fd=(int)(hv-0x1000);
    if(fd>=MAXFD||!g_fdpath[fd]) return 0; uint16_t tmp[2048]; unix2win(g_fdpath[fd],tmp,2048);
    // fl low bits = VOLUME_NAME_*: DOS(0) -> \\?\C:\path, NONE(4) -> \path (no
    // volume). Honor NONE so the engine's relative-path math doesn't break.
    uint16_t *src=tmp; if((fl&0x4)){ uint16_t*c=tmp; /* skip \\?\C: -> leave leading backslash of path */ int k=0; while(c[k]) k++; /* find the ':' and take from next char */ for(int i=0;i<k;i++) if(tmp[i]==':'){ src=&tmp[i+1]; break; } }
    uint32_t n=0; while(src[n]) n++; if(cch<n+1) return n+1; for(uint32_t i=0;i<=n;i++) buf[i]=src[i];
    if(getenv("PELOAD_TRACE_FS")){ char o[1100]; int j=0; for(uint32_t i=0;i<n&&j<1098;i++)o[j++]=(char)(src[i]&0xff); o[j]=0; char hh[64]; snprintf(hh,sizeof hh,"  [GFPNBH fl=0x%x] ",fl); ws(hh); ws(o); ws("\n"); }
    return n;
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
    if(fd<0){ if(errno==ENOENT) snprintf(t_lastfail,sizeof t_lastfail,"NT:%s",path[0]?path:"<empty>"); if(iosb)((uint64_t*)iosb)[0]=0xC0000034; return (int)0xC0000034; } // OBJECT_NAME_NOT_FOUND
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
// ---- real BCrypt hashing (anti-cheat integrity) via CommonCrypto -----------
// Stubs returned garbage handles -> the engine's hash of its assets wrote
// through a junk pointer and crashed. Implement SHA1/256/384/512/MD5 for real.
enum { BALG_SHA1=1,BALG_SHA256,BALG_SHA384,BALG_SHA512,BALG_MD5 };
typedef struct { uint32_t magic; int alg; } BAlg;
typedef struct { uint32_t magic; int alg; union { CC_SHA1_CTX s1; CC_SHA256_CTX s256; CC_SHA512_CTX s512; CC_MD5_CTX md5; } c; } BHash;
#define BMAGIC 0x42435254u
static int balg_from_w(const uint16_t*w){ char a[32]; int i=0; for(; w&&w[i]&&i<31;i++) a[i]=(char)(w[i]&0xff); a[i]=0;
    if(!strcmp(a,"SHA256"))return BALG_SHA256; if(!strcmp(a,"SHA1"))return BALG_SHA1; if(!strcmp(a,"SHA384"))return BALG_SHA384;
    if(!strcmp(a,"SHA512"))return BALG_SHA512; if(!strcmp(a,"MD5"))return BALG_MD5; return BALG_SHA256; }
static int balg_digestlen(int a){ switch(a){case BALG_SHA1:return 20;case BALG_SHA256:return 32;case BALG_SHA384:return 48;case BALG_SHA512:return 64;case BALG_MD5:return 16;default:return 32;} }
static long MSABI i_BCryptOpenAlgorithmProvider(void**phAlg,const uint16_t*algid,const uint16_t*impl,uint32_t fl){ (void)impl;(void)fl;
    BAlg*a=malloc(sizeof *a); if(!a) return 0xC0000017; a->magic=BMAGIC; a->alg=balg_from_w(algid); if(phAlg)*phAlg=a; return 0; }
static long MSABI i_BCryptCloseAlgorithmProvider(void*hAlg,uint32_t fl){ (void)fl; if(hAlg)free(hAlg); return 0; }
static void bhash_init(BHash*h){ switch(h->alg){case BALG_SHA1:CC_SHA1_Init(&h->c.s1);break;case BALG_SHA256:CC_SHA256_Init(&h->c.s256);break;
    case BALG_SHA384:CC_SHA384_Init(&h->c.s512);break;case BALG_SHA512:CC_SHA512_Init(&h->c.s512);break;case BALG_MD5:CC_MD5_Init(&h->c.md5);break;} }
static long MSABI i_BCryptCreateHash(void*hAlg,void**phHash,uint8_t*obj,uint32_t cbobj,uint8_t*sec,uint32_t cbsec,uint32_t fl){
    (void)obj;(void)cbobj;(void)sec;(void)cbsec;(void)fl; BAlg*a=hAlg; BHash*h=malloc(sizeof *h); if(!h) return 0xC0000017;
    h->magic=BMAGIC; h->alg=(a&&a->magic==BMAGIC)?a->alg:BALG_SHA256; bhash_init(h); if(phHash)*phHash=h; return 0; }
static long MSABI i_BCryptHashData(void*hHash,uint8_t*in,uint32_t n,uint32_t fl){ (void)fl; BHash*h=hHash; if(!h||h->magic!=BMAGIC) return 0xC0000008;
    switch(h->alg){case BALG_SHA1:CC_SHA1_Update(&h->c.s1,in,n);break;case BALG_SHA256:CC_SHA256_Update(&h->c.s256,in,n);break;
    case BALG_SHA384:CC_SHA384_Update(&h->c.s512,in,n);break;case BALG_SHA512:CC_SHA512_Update(&h->c.s512,in,n);break;case BALG_MD5:CC_MD5_Update(&h->c.md5,in,n);break;} return 0; }
static long MSABI i_BCryptFinishHash(void*hHash,uint8_t*out,uint32_t n,uint32_t fl){ (void)n;(void)fl; BHash*h=hHash; if(!h||h->magic!=BMAGIC) return 0xC0000008;
    switch(h->alg){case BALG_SHA1:CC_SHA1_Final(out,&h->c.s1);break;case BALG_SHA256:CC_SHA256_Final(out,&h->c.s256);break;
    case BALG_SHA384:CC_SHA384_Final(out,&h->c.s512);break;case BALG_SHA512:CC_SHA512_Final(out,&h->c.s512);break;case BALG_MD5:CC_MD5_Final(out,&h->c.md5);break;} return 0; }
static long MSABI i_BCryptDestroyHash(void*hHash){ if(hHash)free(hHash); return 0; }
static long MSABI i_BCryptGetProperty(void*h,const uint16_t*prop,uint8_t*out,uint32_t cb,uint32_t*cbres,uint32_t fl){ (void)fl;
    char p[48]; int i=0; for(; prop&&prop[i]&&i<47;i++) p[i]=(char)(prop[i]&0xff); p[i]=0; int alg=BALG_SHA256;
    if(h){ BAlg*a=h; if(a->magic==BMAGIC) alg=a->alg; }
    uint32_t v=0; if(!strcmp(p,"HashDigestLength")) v=balg_digestlen(alg); else if(!strcmp(p,"ObjectLength")) v=(uint32_t)sizeof(BHash); else v=0;
    if(cbres)*cbres=4; if(out&&cb>=4)*(uint32_t*)out=v; return 0; }
// one-shot BCryptHash(hAlg, pbSecret, cbSecret, pbInput, cbInput, pbOutput, cbOutput)
static long MSABI i_BCryptHash(void*hAlg,uint8_t*sec,uint32_t cbsec,uint8_t*in,uint32_t cbin,uint8_t*out,uint32_t cbout){
    (void)sec;(void)cbsec;(void)cbout; BAlg*a=hAlg; int alg=(a&&a->magic==BMAGIC)?a->alg:BALG_SHA256;
    switch(alg){case BALG_SHA1:CC_SHA1(in,cbin,out);break;case BALG_SHA256:CC_SHA256(in,cbin,out);break;
    case BALG_SHA384:CC_SHA384(in,cbin,out);break;case BALG_SHA512:CC_SHA512(in,cbin,out);break;case BALG_MD5:CC_MD5(in,cbin,out);break;} return 0; }
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
    if(stat(win2unix(w,p,sizeof p),&st)!=0){ if(getenv("PELOAD_TRACE_FS")){ws("  [GetFileAttributesW] ");ws(p);ws(errno==ENOENT?" ENOENT\n":" FAIL\n");} g_lasterr=win_err(); if(g_lasterr==2)snprintf(t_lastfail,sizeof t_lastfail,"GFA:%s",p[0]?p:"<empty>"); return 0xFFFFFFFF; } g_lasterr=0; // INVALID_FILE_ATTRIBUTES
    return S_ISDIR(st.st_mode)? 0x10 : 0x80; // DIRECTORY : NORMAL
}
static int MSABI i_GetFileAttributesExW(const uint16_t *w,int lvl,void *info){
    (void)lvl; char p[2048]; struct stat st; if(stat(win2unix(w,p,sizeof p),&st)!=0){ if(getenv("PELOAD_TRACE_FS")){ws("  [GetFileAttributesExW] ");ws(p);ws(errno==ENOENT?" ENOENT\n":" FAIL\n");} g_lasterr=win_err(); if(g_lasterr==2)snprintf(t_lastfail,sizeof t_lastfail,"GFAx:%s",p[0]?p:"<empty>"); return 0; } g_lasterr=0;
    if(info){ uint8_t*d=info; memset(d,0,36); *(uint32_t*)d = S_ISDIR(st.st_mode)?0x10:0x80;
        *(uint32_t*)(d+28)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+32)=(uint32_t)st.st_size; }
    return 1;
}
// Font substitution: the engine opens Windows UI fonts (e.g. Fonts/bahnschrift.ttf)
// that don't exist on macOS. If a font path doesn't resolve, redirect it to a
// staged substitute in $drive_c/ReclaimerFonts so text rendering has a real TTF.
static void font_subst(char *p, int cap){
    const char *b=p; for(char*q=p;*q;q++) if(*q=='/') b=q+1;
    int L=(int)strlen(b); if(L<5) return;
    const char *ext=b+L-4; if(strcasecmp(ext,".ttf")&&strcasecmp(ext,".otf")&&strcasecmp(ext,".ttc")) return;
    if(access(p,R_OK)==0) return;                 // real file present -> keep
    char cand[2048]; snprintf(cand,sizeof cand,"%s/ReclaimerFonts/%s",g_drivec,b);
    if(access(cand,R_OK)!=0) snprintf(cand,sizeof cand,"%s/ReclaimerFonts/bahnschrift.ttf",g_drivec); // generic fallback
    if(access(cand,R_OK)==0) snprintf(p,cap,"%s",cand);
}
static void*  MSABI i_CreateFileW(const uint16_t *w,uint32_t acc,uint32_t share,void*sec,uint32_t disp,uint32_t fl,void*tmpl){
    (void)share;(void)sec;(void)fl;(void)tmpl; char p[2048]; win2unix(w,p,sizeof p);
    if((!w||!w[0])){
        if(getenv("PELOAD_TRACE_EMPTY")){ ws("  [CFW EMPTY-PATH caller chain]\n"); uint64_t *sp=(uint64_t*)__builtin_frame_address(0);
            for(int i=0;i<500;i++){ uint64_t v=sp[i]; if(v>=0x140000000ULL&&v<0x14c000000ULL){ char t[48]; snprintf(t,sizeof t,"    exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); ws(t);} } }
        // The engine clones an uninitialized global path -> File::open("") (the
        // playlist path, unset for this experiment). Redirect to a staged valid
        // playlist so the Guardian rotation parses; else an empty file.
        if(!getenv("PELOAD_NO_EMPTY_FIX")){ if(g_playlist[0]&&access(g_playlist,R_OK)==0) snprintf(p,sizeof p,"%s",g_playlist); else snprintf(p,sizeof p,"/dev/null"); }
    }
    if(acc&0x80000000) font_subst(p,sizeof p);    // read opens only
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
    if(getenv("PELOAD_TRACE_FS")){ char hd[48]; snprintf(hd,sizeof hd,"  [CFW pid=%d disp=%u acc=0x%x] ",getpid(),disp,acc); ws(hd); ws(p); ws(fd<0?"  FAIL\n":"  ok\n"); }
    if(fd<0){ g_lasterr=win_err(); if(g_lasterr==2) snprintf(t_lastfail,sizeof t_lastfail,"CFW:%s",p[0]?p:"<empty>"); return (void*)-1; } g_lasterr=0; // INVALID_HANDLE_VALUE
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
    const char *val=getenv(nm); if(getenv("PELOAD_TRACE_ENV")){ws("  [GetEnv] ");ws(nm);ws("=");ws(val?val:"(unset)");ws("\n");} if(!val){ g_lasterr=203; return 0; } g_lasterr=0; // ERROR_ENVVAR_NOT_FOUND so Rust env::var -> None, not Some("")
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
    if(name[0]=='g'&&name[1]=='l') return gl_or_missing(name);
    if(name[0]=='w'&&name[1]=='g'&&name[2]=='l'){ void *g=dlsym((void*)-2,name); if(g) return make_gl_thunk(g); }
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
        {"CreateDXGIFactory",(void*)i_CreateDXGIFactory},
        {"CreateDXGIFactory1",(void*)i_CreateDXGIFactory1},
        {"CreateDXGIFactory2",(void*)i_CreateDXGIFactory2},
        {"D3D11CreateDevice",(void*)i_D3D11CreateDevice},
        {"D3DCompile",(void*)i_D3DCompile},
        {"D3DCompile2",(void*)i_D3DCompile},
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
        {"WaitOnAddress",(void*)i_WaitOnAddress},
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
        {"BCryptOpenAlgorithmProvider",(void*)i_BCryptOpenAlgorithmProvider},
        {"BCryptCloseAlgorithmProvider",(void*)i_BCryptCloseAlgorithmProvider},
        {"BCryptCreateHash",(void*)i_BCryptCreateHash},
        {"BCryptHashData",(void*)i_BCryptHashData},
        {"BCryptFinishHash",(void*)i_BCryptFinishHash},
        {"BCryptDestroyHash",(void*)i_BCryptDestroyHash},
        {"BCryptGetProperty",(void*)i_BCryptGetProperty},
        {"BCryptHash",(void*)i_BCryptHash},
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
        {"RegisterWindowMessageA",(void*)i_RegisterWindowMessageA},
        {"RegisterWindowMessageW",(void*)i_RegisterWindowMessageW},
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
        {"CreateCompatibleDC",(void*)i_CreateCompatibleDC},
        {"CreateDIBSection",(void*)i_CreateDIBSection},
        {"CreateCompatibleBitmap",(void*)i_CreateCompatibleBitmap},
        {"SelectObject",(void*)i_SelectObject},
        {"DeleteObject",(void*)i_DeleteObject},
        {"DeleteDC",(void*)i_DeleteDC},
        {"GetObjectW",(void*)i_GetObjectW},
        {"BitBlt",(void*)i_BitBlt},
        {"StretchBlt",(void*)i_StretchBlt},
        {"SetDIBitsToDevice",(void*)i_SetDIBitsToDevice},
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
        {"RtlVirtualUnwind",(void*)i_RtlVirtualUnwind},
        {"RtlUnwindEx",(void*)i_RtlUnwindEx},
        {"RtlUnwind",(void*)i_RtlUnwindEx},
        {"RtlRestoreContext",(void*)i_RtlRestoreContext},
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
    if(!strcasecmp(dll,"opengl32.dll") && (fn[0]=='g'&&fn[1]=='l')) return gl_or_missing(fn);
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
        int is_ws2 = name_is_ws2(dll); int is_xa = name_has(dll,"xaudio");
        for(int i=0; oft[i]; i++){
            const char *fn = (oft[i]>>63)? "#ordinal" : (char*)(base+(uint32_t)oft[i]+2);
            // ws2_32: bind through the shared ordinal/name table (see WS2_HANDLE)
            if(is_ws2){ ft[i]=(uint64_t)((oft[i]>>63)? ws2_by_ordinal((uint32_t)(oft[i]&0xffff)) : ws2_resolve_name(fn)); continue; }
            if(is_xa){ void* xr=xa_resolve((oft[i]>>63)?(uint32_t)(oft[i]&0xffff):0, (oft[i]>>63)?0:fn); if(xr){ ft[i]=(uint64_t)xr; continue; } }
            void *r = uni_resolve(dll, fn);
            if(r && trace && imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn; imp_real[imp_count]=r; ft[i]=(uint64_t)make_trace_stub(imp_count,r); imp_count++; }
            else if(r){ ft[i]=(uint64_t)r; }
            else if(imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn; ft[i]=(uint64_t)make_stub(imp_count++); }
        }
    }
}

// Bind DELAY imports too. We don't implement __delayLoadHelper2, so an unbound
// delay slot stays a delay-thunk that resolves to NULL -> `call [slot]` to NULL
// crashes (e.g. the engine's dxgi.dll!CreateDXGIFactory1 probe). Resolve each
// delay import up front, same as normal imports.
static void bind_delay_imports(uint8_t *base, OPT64 *opt){
    uint32_t drva=opt->Dir[DIR_DELAY].VA; if(!drva) return;
    DELAYDESC *dd=(DELAYDESC*)(base+drva);
    for(; dd->Name; dd++){
        const char *dll=(char*)(base+dd->Name);
        uint64_t *iat=(uint64_t*)(base+dd->IAT);
        uint64_t *intb=(uint64_t*)(base+dd->INT);
        int is_ws2 = name_is_ws2(dll); int is_xa = name_has(dll,"xaudio");
        for(int i=0; intb[i]; i++){
            const char *fn = (intb[i]>>63)? "#ordinal" : (char*)(base+(uint32_t)intb[i]+2);
            void *r;
            if(is_ws2) r = (intb[i]>>63)? ws2_by_ordinal((uint32_t)(intb[i]&0xffff)) : ws2_resolve_name(fn);
            else if(is_xa && (r=xa_resolve((intb[i]>>63)?(uint32_t)(intb[i]&0xffff):0,(intb[i]>>63)?0:fn))) ;
            else r = uni_resolve(dll, fn);
            if(!r && imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn; r=make_stub(imp_count++); }
            iat[i]=(uint64_t)r;
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

// WavesFX audio-DSP shim. halo3.dll builds a table of WavesFX_* function
// pointers (globals at RVA 0x46bb780..0x46bb7f0) that it would populate by
// LoadLibrary'ing Waves' DLL + GetProcAddress. That loader path never runs in
// our environment, so the pointers stay NULL and the engine later calls
// WavesFX_Initialize (NULL) -> jmp 0 -> AV. Pre-fill them: Initialize returns a
// non-NULL dummy context (the engine stores it at obj+0x388 and asserts it is
// non-NULL), every other entry is a harmless no-op (audio effects just become
// a pass-through / silence, which is fine for reaching the menu).
static uint8_t g_waves_ctx[8192]; // dummy WavesFX context handle (zeroed)
static void*   MSABI waves_init(void*a,uint32_t b,void*c){ (void)a;(void)b;(void)c; return g_waves_ctx; }
static uint64_t MSABI waves_noop(void*a,void*b,void*c,void*d,void*e,void*f){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 0; }
static void waves_prefill(uint8_t* halo3_base){
    // contiguous block of 15 function-pointer globals
    for(uint32_t rva=0x46bb780; rva<=0x46bb7f0; rva+=8)
        *(void**)(halo3_base+rva) = (void*)waves_noop;
    *(void**)(halo3_base+0x46bb7c8) = (void*)waves_init; // WavesFX_Initialize
    if(getenv("PELOAD_TRACE_SEH")) ws("  [waves] prefilled WavesFX_* function table\n");
}
// load a guest DLL end-to-end: map, bind, protect, register exports, DllMain
static uint8_t *load_guest(const char *path, const char *name){
    OPT64 *opt; COFF *coff; uint8_t *base=map_image(path,&opt,&coff);
    if(!base){ ws("  guest load FAILED: "); ws(name); ws("\n"); return 0; }
    bind_imports(base,opt);
    bind_delay_imports(base,opt);   // halo3.dll delay-loads dxgi/d3d11/etc.
    set_protections(base,coff,opt);
    g_mods[g_nmods].name=name; g_mods[g_nmods].base=base; g_nmods++;
    if(strcasestr(name,"halo3")) waves_prefill(base);
    ws("  guest "); ws(name); ws(" @ "); whex((uint64_t)base);
    uint64_t ep=opt->EntryRVA;
    if(ep){ int (MSABI *dm)(void*,uint32_t,void*)=(void*)(base+ep); int r=dm(base,1,0);
        ws("    DllMain -> "); whex((uint64_t)(uint32_t)r); }
    return base;
}

// Bridge the Darwin x86_64 signal mcontext <-> a Windows x64 CONTEXT so a Unix
// hardware fault can be dispatched through the guest's SEH chain.
static void uc2ctx(void* ctx, ucontext_t* uc){
    x86_thread_state64_t* s=&uc->uc_mcontext->__ss;
    CTX_REG(ctx,0)=s->__rax; CTX_REG(ctx,1)=s->__rcx; CTX_REG(ctx,2)=s->__rdx; CTX_REG(ctx,3)=s->__rbx;
    CTX_REG(ctx,4)=s->__rsp; CTX_REG(ctx,5)=s->__rbp; CTX_REG(ctx,6)=s->__rsi; CTX_REG(ctx,7)=s->__rdi;
    CTX_REG(ctx,8)=s->__r8; CTX_REG(ctx,9)=s->__r9; CTX_REG(ctx,10)=s->__r10; CTX_REG(ctx,11)=s->__r11;
    CTX_REG(ctx,12)=s->__r12; CTX_REG(ctx,13)=s->__r13; CTX_REG(ctx,14)=s->__r14; CTX_REG(ctx,15)=s->__r15;
    CTX_RIP(ctx)=s->__rip; *(uint32_t*)((uint8_t*)ctx+0x44)=(uint32_t)s->__rflags;
    *(uint32_t*)((uint8_t*)ctx+0x30)=0x10000f; // CONTEXT_FULL|SEGMENTS
    memcpy((uint8_t*)ctx+0x1a0,&uc->uc_mcontext->__fs.__fpu_xmm0,256);
}
static void ctx2uc(ucontext_t* uc, void* ctx){
    x86_thread_state64_t* s=&uc->uc_mcontext->__ss;
    s->__rax=CTX_REG(ctx,0); s->__rcx=CTX_REG(ctx,1); s->__rdx=CTX_REG(ctx,2); s->__rbx=CTX_REG(ctx,3);
    s->__rsp=CTX_REG(ctx,4); s->__rbp=CTX_REG(ctx,5); s->__rsi=CTX_REG(ctx,6); s->__rdi=CTX_REG(ctx,7);
    s->__r8=CTX_REG(ctx,8); s->__r9=CTX_REG(ctx,9); s->__r10=CTX_REG(ctx,10); s->__r11=CTX_REG(ctx,11);
    s->__r12=CTX_REG(ctx,12); s->__r13=CTX_REG(ctx,13); s->__r14=CTX_REG(ctx,14); s->__r15=CTX_REG(ctx,15);
    s->__rip=CTX_RIP(ctx); s->__rflags=*(uint32_t*)((uint8_t*)ctx+0x44);
    memcpy(&uc->uc_mcontext->__fs.__fpu_xmm0,(uint8_t*)ctx+0x1a0,256);
}
// Run a hardware fault through the guest's Win64 SEH dispatch. Returns 1 if a
// handler asked to CONTINUE execution (ctx updated in place), 0 if unhandled.
static __thread int g_in_crashsig;
static int seh_dispatch_hw(uint32_t code, EXREC* rec, void* ctx){
    EXCEPTION_POINTERS ep; ep.ExceptionRecord=rec; ep.ContextRecord=ctx;
    // (1) first-chance vectored exception handlers (anti-cheat / probe recovery)
    for(int i=0;i<g_nveh;i++){ if(!g_veh[i]) continue; PVEH vh=(PVEH)g_veh[i];
        long r=vh(&ep);
        if(r==-1L){ if(getenv("PELOAD_TRACE_SEH")) ws("  [VEH -> CONTINUE_EXECUTION]\n"); return 1; } }
    // (2) frame-based (.pdata) handlers. A __try/__except that handles the fault
    // transfers via RtlUnwindEx and never returns; one that resumes returns 0.
    uint8_t work[CTXSZ];
    for(int guard=0; guard<512; guard++){
        uint64_t base=0; RUNFN* fn=i_RtlLookupFunctionEntry(CTX_RIP(ctx),&base,0);
        if(!fn){ uint64_t sp=CTX_RSP(ctx); if(!sp) break; uint64_t rip=*(uint64_t*)sp; CTX_RSP(ctx)=sp+8; CTX_RIP(ctx)=rip; if(!rip) break; continue; }
        memcpy(work,ctx,CTXSZ); void* hdata=0; uint64_t estab=0;
        PEXHANDLER h=i_RtlVirtualUnwind(1,base,CTX_RIP(ctx),fn,work,&hdata,&estab,0);
        if(h){ DISPCTX dc; memset(&dc,0,sizeof dc); dc.ControlPc=CTX_RIP(ctx); dc.ImageBase=base; dc.FunctionEntry=fn;
            dc.EstablisherFrame=estab; dc.ContextRecord=ctx; dc.LanguageHandler=h; dc.HandlerData=hdata;
            int disp=h(rec, estab, ctx, &dc);
            if(disp==0){ if(getenv("PELOAD_TRACE_SEH")) ws("  [__except -> CONTINUE_EXECUTION]\n"); return 1; } }
        memcpy(ctx,work,CTXSZ); if(!CTX_RIP(ctx)) break;
    }
    (void)code; return 0;
}
// crash handler: print faulting signal/address/RIP + which module (so a
// null-pointer GL call or a guest-code fault is immediately identifiable).
static void crashsig(int sig, siginfo_t *info, void *ucv){
    ucontext_t *uc=ucv; uint64_t rip = uc? (uint64_t)uc->uc_mcontext->__ss.__rip : 0;
    // PELOAD_SURVIVE: for a non-main thread fault, park BEFORE handing it to the
    // engine's exception handler (which would ExitProcess). This keeps halo3's
    // D3D worker-thread faults from killing the process so the OpenGL menu thread
    // can keep running. (Parked thread keeps its locks — may deadlock; capture aid.)
    if(uc && getenv("PELOAD_SURVIVE") && !pthread_main_np() &&
       (sig==SIGILL||sig==SIGSEGV||sig==SIGBUS||sig==SIGFPE) && !g_in_crashsig){
        static int parked; if(parked<64){ parked++;
            char pb[80]; int n=snprintf(pb,sizeof pb,"  [survive] park faulted worker tid=0x%x rip=0x%llx\n",
                pthread_mach_thread_np(pthread_self()),(unsigned long long)rip); (void)!write(2,pb,n); }
        sigset_t us; sigemptyset(&us); sigaddset(&us,SIGILL); sigaddset(&us,SIGSEGV);
        sigaddset(&us,SIGBUS); sigaddset(&us,SIGFPE); sigaddset(&us,SIGTRAP); sigaddset(&us,SIGABRT);
        pthread_sigmask(SIG_BLOCK,&us,0);
        for(;;) pause();
    }
    // First try to dispatch the fault as a Windows exception (the engine faults
    // deliberately inside probes and expects a handler to recover + continue).
    if(uc && !g_in_crashsig && (sig==SIGILL||sig==SIGSEGV||sig==SIGBUS||sig==SIGFPE||sig==SIGTRAP)){
        g_in_crashsig=1;
        // unblock these so a nested fault during dispatch isn't silently fatal
        sigset_t us; sigemptyset(&us); sigaddset(&us,SIGILL); sigaddset(&us,SIGSEGV);
        sigaddset(&us,SIGBUS); sigaddset(&us,SIGFPE); sigaddset(&us,SIGTRAP);
        sigprocmask(SIG_UNBLOCK,&us,0);
        uint8_t ctxbuf[CTXSZ]; memset(ctxbuf,0,CTXSZ); void*ctx=ctxbuf; uc2ctx(ctx,uc);
        uint32_t code = sig==SIGILL?0xC000001D : (sig==SIGSEGV||sig==SIGBUS)?0xC0000005 :
                        sig==SIGFPE?0xC0000090 : 0x80000003;
        EXREC rec; memset(&rec,0,sizeof rec); rec.ExceptionCode=code; rec.ExceptionAddress=(void*)CTX_RIP(ctx);
        if(code==0xC0000005){ rec.NumberParameters=2;
            rec.ExceptionInformation[0]=(uc->uc_mcontext->__es.__err & 2)?1:0;
            rec.ExceptionInformation[1]=(uint64_t)(info?info->si_addr:0); }
        if(seh_dispatch_hw(code,&rec,ctx)){ ctx2uc(uc,ctx); g_in_crashsig=0; return; }
        g_in_crashsig=0;
    }
    char b[192]; const char *mod="?"; uint64_t moff=0;
    if(g_base){ DOS*d=(DOS*)g_base; OPT64*o=(OPT64*)((uint8_t*)g_base+d->e_lfanew+sizeof(COFF));
        if(rip>=(uint64_t)g_base && rip<(uint64_t)g_base+o->ImageSize){ mod="exe"; moff=rip-(uint64_t)g_base; } }
    for(int m=0;m<g_nmods && !moff;m++){ DOS*d=(DOS*)g_mods[m].base; OPT64*o=(OPT64*)(g_mods[m].base+d->e_lfanew+sizeof(COFF));
        if(rip>=(uint64_t)g_mods[m].base && rip<(uint64_t)g_mods[m].base+o->ImageSize){ mod=g_mods[m].name; moff=rip-(uint64_t)g_mods[m].base; } }
    int n=snprintf(b,sizeof b,"\n[CRASH] sig=%d faultaddr=%p RIP=0x%llx (%s+0x%llx)\n",sig,info?info->si_addr:0,(unsigned long long)rip,mod,(unsigned long long)moff);
    (void)!write(2,b,n);
    { uint64_t gs58=0; __asm__ volatile("movq %%gs:0x58,%0":"=r"(gs58));
      uint64_t t0=(gs58>0x10000)?*(uint64_t*)gs58:0;
      int nn=snprintf(b,sizeof b,"    gs:0x58(tls_array)=0x%llx  tls[0]=0x%llx  mach_tid=0x%x  mainthr=%d\n",
        (unsigned long long)gs58,(unsigned long long)t0,pthread_mach_thread_np(pthread_self()),pthread_main_np());
      (void)!write(2,b,nn); }
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
    // PELOAD_SURVIVE: an unhandled fault on a non-main thread parks that thread
    // instead of killing the process, so the main/render thread can continue to a
    // draw/present (PELOAD_SHOT then captures a frame). Best-effort: a parked
    // thread keeps any locks it held, so this can deadlock — it is a capture aid,
    // not correctness. The main thread still terminates the process.
    if(getenv("PELOAD_SURVIVE") && !pthread_main_np()){
        (void)!write(2,"    [survive] parking faulted non-main thread\n",46);
        g_in_crashsig=0;
        sigset_t us; sigemptyset(&us); sigaddset(&us,SIGSEGV); sigaddset(&us,SIGILL);
        sigaddset(&us,SIGBUS); sigaddset(&us,SIGFPE); sigaddset(&us,SIGTRAP); sigaddset(&us,SIGABRT);
        pthread_sigmask(SIG_BLOCK,&us,0);
        for(;;) pause();
    }
    if(getenv("PELOAD_PROBE_EXIT0") && strstr(g_cmdlineA,"probe-engine-worker")){
        (void)!write(2,"    [probe-exit0] worker faulted; reporting success to parent\n",62); _exit(0); }
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
    mprotect(g_stubpool, 32*8192, PROT_READ|PROT_WRITE|PROT_EXEC); g_pool_exec=0; // permanent RWX (thread-safe)
    g_pool_exec = 1;
    g_trpool = mmap(0, 96*8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    mprotect(g_trpool, 96*8192, PROT_READ|PROT_EXEC); g_trexec = 1;
    g_glthunk = mmap(0, 64*8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    mprotect(g_glthunk, 64*8192, PROT_READ|PROT_WRITE|PROT_EXEC); g_glth_exec = 0; // permanent RWX (thread-safe; no toggle)

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
      // exe path (Windows form) for GetModuleFileNameW / Rust current_exe().
      // Computed here (after g_drivec) so unix2win maps it correctly (the exe
      // lives outside drive_c -> Z:\ , which win2unix round-trips back).
      { char rp[2048]; const char*ep = realpath(path,rp)?rp:path; uint16_t wp[1024]; unix2win(ep,wp,1024);
        int i=0; for(; wp[i]&&i<1023; i++){ g_exewin[i]=wp[i]; g_exewinA[i]=(char)(wp[i]&0xff); } g_exewin[i]=0; g_exewinA[i]=0; }
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
      setenv("RECLAIMER_MOUSE_SENSITIVITY","1.0",0);
      // Content/discovery dirs: if unset the engine may read an empty path and
      // File::open("") -> ENOENT ("os error 2"). Point them at real dirs.
      snprintf(pd,sizeof pd,"%s/ReclaimerContent",g_drivec); mkdir(pd,0755);
      { const char*cs[]={"Maps","Game Modes","Images","Mods","Mods/Downloaded",0}; for(int i=0;cs[i];i++){ char d[1400]; snprintf(d,sizeof d,"%s/ReclaimerContent/%s",g_drivec,cs[i]); mkparents(d); mkdir(d,0755);} }
      setenv("RECLAIMER_CONTENT_DIR","C:\\ReclaimerContent",0);
      snprintf(pd,sizeof pd,"%s/ReclaimerCache/discovery",g_drivec); mkdir(pd,0755); setenv("RECLAIMER_DISCOVERY_DIR","C:\\ReclaimerCache\\discovery",0);
      // Stage substitute TTFs for the Windows UI fonts the engine opens (see
      // font_subst): copy a Mac sans into $drive_c/ReclaimerFonts/<name>.
      { char fd[1400]; snprintf(fd,sizeof fd,"%s/ReclaimerFonts",g_drivec); mkdir(fd,0755);
        const char *mac="/System/Library/Fonts/Supplemental/Arial.ttf";
        const char *names[]={"bahnschrift.ttf","segoeui.ttf","Arial.ttf",0};
        for(int i=0;names[i];i++){ char dst[1500]; snprintf(dst,sizeof dst,"%s/%s",fd,names[i]);
          if(access(dst,R_OK)!=0){ int in=open(mac,O_RDONLY); if(in>=0){ int out=open(dst,O_WRONLY|O_CREAT|O_TRUNC,0644); if(out>=0){ char bf[65536]; long r; while((r=read(in,bf,sizeof bf))>0) (void)!write(out,bf,r); close(out);} close(in);} } } }
      // Cache the MCC + cache folder paths the engine looks up (mcc-folder.txt /
      // cache-folder.txt), so discovery doesn't fall back to an empty path.
      { char fp[1400]; FILE*fh;
        snprintf(fp,sizeof fp,"%s/ReclaimerProfile/mcc-folder.txt",g_drivec); if(access(fp,R_OK)!=0&&(fh=fopen(fp,"w"))){ fputs("C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection",fh); fclose(fh);}
        snprintf(fp,sizeof fp,"%s/ReclaimerCache/cache-folder.txt",g_drivec); if(access(fp,R_OK)!=0&&(fh=fopen(fp,"w"))){ fputs("C:\\ReclaimerCache",fh); fclose(fh);} }
      // A valid default playlist (the engine's own sample rotation). The native
      // worker reads a playlist path that is empty when none is configured;
      // empty-path opens are redirected here (see i_CreateFileW) so the Guardian
      // experiment has a parseable rotation instead of failing on empty JSON.
      { char fp[1400]; snprintf(fp,sizeof fp,"%s/ReclaimerContent/default.playlist.json",g_drivec); FILE*fh=fopen(fp,"w");
        if(fh){ fputs("{\n  \"name\": \"Halo 3 community rotation\",\n  \"vote_seconds\": 20,\n  \"games\": []\n}\n",fh); fclose(fh); }
        snprintf(g_playlist,sizeof g_playlist,"%s/ReclaimerContent/default.playlist.json",g_drivec); } }  // engine input default (valid range 0.05-20)

    // The native worker enumerates its content folders (Maps / Game Modes /
    // Images / Mods / Mods/Downloaded) relative to its cwd (the mcc-startup
    // temp dir). On a real install these exist (empty); here they don't, so the
    // enumeration hits a fatal ENOENT ("os error 2"). Pre-create them empty so
    // enumeration yields zero entries and startup proceeds.
    { char cwd[1200]; if(getcwd(cwd,sizeof cwd) && strstr(cwd,"mcc-startup")){
        const char *subs[]={"Maps","Game Modes","Images","Mods","Mods/Downloaded","Mods/Local",0};
        for(int i=0;subs[i];i++){ char d[1500]; snprintf(d,sizeof d,"%s/%s",cwd,subs[i]); mkparents(d); mkdir(d,0755); } } }

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

    // delay imports: bind them (the graphical client DOES call them, e.g. dxgi)
    bind_delay_imports(g_base, opt);

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
    // (window is created lazily in wglCreateContext on the engine's GL thread)
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
