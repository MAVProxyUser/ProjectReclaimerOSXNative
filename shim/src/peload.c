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

// MUST precede every include: makes <sys/ucontext.h> define ucontext_t WITH its inline
// __mcontext_data, so getcontext/makecontext don't overflow the struct (fiber support).
// _DARWIN_C_SOURCE keeps all the BSD/mach APIs visible alongside _XOPEN_SOURCE.
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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
#include <malloc/malloc.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <ucontext.h>
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
static void* g_present_glctx; static int g_present_started; // fwd (hold-window present thread)

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
static void __attribute__((ms_abi)) i_glLinkProgram(unsigned); // fwd
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
    if(g_glthoff+128 > 128*8192){ pthread_mutex_unlock(&gtm); return real; }
    uint8_t *p=g_glthunk+g_glthoff; g_glthoff+=128; int o=0;
    // Win64 -> SysV ABI thunk forwarding up to 12 args. Win64 passes args1-4 in
    // rcx/rdx/r8/r9 and args5+ on the stack above a 32-byte shadow area; SysV
    // passes args1-6 in rdi/rsi/rdx/rcx/r8/r9 and args7+ on the stack. The OLD
    // 6-arg thunk dropped args7+, so e.g. glReadPixels' 7th arg (dst pointer) and
    // glTexSubImage2D's 9th (pixels) were garbage -> the GL driver scribbled over
    // the heap. This forwards all 12, so any >6-arg GL entry point is safe.
    // NB: rdi and rsi are CALLEE-saved in the Win64 ABI (caller-saved in SysV),
    // so we must save/restore them around the SysV call or the guest's rdi/rsi
    // get trashed. rbp is set before the saves, so the rbp-relative offsets to the
    // caller's Win64 stack args are: ret@+8, shadow@+0x10..+0x28, arg5@+0x30, ...
    uint8_t code[]={
        0x55,                               // push rbp
        0x48,0x89,0xE5,                     // mov rbp,rsp
        0x56,                               // push rsi   (Win64 callee-saved)
        0x57,                               // push rdi   (Win64 callee-saved)
        0x48,0x83,0xEC,0x40,                // sub rsp,0x40  (6 stack-arg slots; keeps 16-align)
        0x48,0x89,0xCF,                     // mov rdi,rcx    (arg1)
        0x48,0x89,0xD6,                     // mov rsi,rdx    (arg2)
        0x4C,0x89,0xC2,                     // mov rdx,r8     (arg3)
        0x4C,0x89,0xC9,                     // mov rcx,r9     (arg4)
        0x4C,0x8B,0x45,0x30,                // mov r8,[rbp+0x30]  (arg5)
        0x4C,0x8B,0x4D,0x38,                // mov r9,[rbp+0x38]  (arg6)
        0x48,0x8B,0x45,0x40, 0x48,0x89,0x04,0x24,          // arg7  -> [rsp+0x00]
        0x48,0x8B,0x45,0x48, 0x48,0x89,0x44,0x24,0x08,     // arg8  -> [rsp+0x08]
        0x48,0x8B,0x45,0x50, 0x48,0x89,0x44,0x24,0x10,     // arg9  -> [rsp+0x10]
        0x48,0x8B,0x45,0x58, 0x48,0x89,0x44,0x24,0x18,     // arg10 -> [rsp+0x18]
        0x48,0x8B,0x45,0x60, 0x48,0x89,0x44,0x24,0x20,     // arg11 -> [rsp+0x20]
        0x48,0x8B,0x45,0x68, 0x48,0x89,0x44,0x24,0x28,     // arg12 -> [rsp+0x28]
        0x48,0xB8,0,0,0,0,0,0,0,0,          // mov rax, real
        0xFF,0xD0,                          // call rax
        0x48,0x8D,0x65,0xF0,                // lea rsp,[rbp-0x10]  (back to saved rdi)
        0x5F,                               // pop rdi
        0x5E,                               // pop rsi
        0x5D,                               // pop rbp
        0xC3                                // ret
    };
    for(size_t i=0;i<sizeof code;i++) p[i]=code[i];
    int immoff=0; for(int i=0;i<(int)sizeof code-1;i++) if(code[i]==0x48&&code[i+1]==0xB8){ immoff=i+2; break; }
    *(uint64_t*)(p+immoff)=(uint64_t)real; (void)o;
    sys_icache_invalidate(p,128);  // Rosetta: translate the freshly written thunk
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
// Interlocked SList: Windows' LOCK-FREE singly-linked list, used by the engine for
// concurrent memory pools / work queues across threads. A non-atomic version races
// (two threads corrupt the list -> bad pointers -> non-deterministic heap crashes),
// so serialize every op under one mutex. Our header/entry layout is just "next
// pointer at offset 0" (consistent because the engine only touches these lists
// through this API after InitializeSListHead zeroes the header).
static pthread_mutex_t g_slist_m = PTHREAD_MUTEX_INITIALIZER;
static void*  MSABI i_InterlockedPushEntrySList(void *head, void *entry){ pthread_mutex_lock(&g_slist_m); void*old=*(void**)head; *(void**)entry=old; *(void**)head=entry; pthread_mutex_unlock(&g_slist_m); return old; }
static void*  MSABI i_InterlockedPopEntrySList(void *head){ pthread_mutex_lock(&g_slist_m); void*e=*(void**)head; if(e)*(void**)head=*(void**)e; pthread_mutex_unlock(&g_slist_m); return e; }
static void*  MSABI i_InterlockedFlushSList(void *head){ pthread_mutex_lock(&g_slist_m); void*e=*(void**)head; *(void**)head=0; pthread_mutex_unlock(&g_slist_m); return e; }
static uint16_t MSABI i_QueryDepthSList(void *head){ pthread_mutex_lock(&g_slist_m); uint16_t d=0; for(void*e=*(void**)head;e;e=*(void**)e) d++; pthread_mutex_unlock(&g_slist_m); return d; }
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
    const char*le=getenv("PELOAD_TRACE_LE");
    if(le && (g_lasterr==2 || !strcmp(le,"all"))){ void*ra=__builtin_return_address(0); char t[640]; uint64_t r=(uint64_t)ra; snprintf(t,sizeof t,"  [GLE=%u] caller=%s+0x%llx lastfail='%s'\n",g_lasterr,(r>=0x140000000&&r<0x14c000000)?"exe":(r>=0x180000000&&r<0x187000000)?"halo3":"?",(unsigned long long)((r>=0x140000000&&r<0x14c000000)?r-0x140000000:(r>=0x180000000&&r<0x187000000)?r-0x180000000:r),t_lastfail); ws(t); }
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
// PELOAD_HEAP_GUARD: diagnostic guarded allocator — each HeapAlloc goes on its own
// page(s) with a trailing PROT_NONE guard, so ANY overflow of a guest heap buffer
// faults exactly at the offending write (Guard Malloc doesn't work under Rosetta).
// We tag the block so HeapFree/Size/ReAlloc recognise it. Slow + VM-heavy; diag only.
static int g_heapguard=-1;
static inline int heapguard(void){ if(g_heapguard<0) g_heapguard=getenv("PELOAD_HEAP_GUARD")?1:0; return g_heapguard; }
#define HG_SLACK 512
static void* hg_alloc(uint64_t n){ uint64_t ps=4096, slack=HG_SLACK, nb=((n+slack+ps-1)/ps)*ps; if(nb==0) nb=ps;
    uint8_t* m=mmap(0,nb+ps,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); if(m==MAP_FAILED) return malloc(n?n:1);
    // Guard page is READ-ONLY: benign optimized memcpy/memset that over-READS (and
    // over-WRITES by a few bytes rounding to SIMD width, or Windows heap-granularity
    // over-use) stays within `slack` and won't fault; an over-WRITE beyond `slack`
    // (a real heap overflow into the next allocation) faults at the store.
    mprotect(m+nb,ps,PROT_READ);
    return m+(nb-n-slack); }   // `slack` bytes between buffer end and guard
// PELOAD_HEAP_DEBUG: redzone allocator for the GUEST heap (Rust + C++ both go
// through HeapAlloc here). Each block gets a header (size+magic) + head/tail
// redzones filled with a canary. On free (and, with PELOAD_HEAP_SCAN, on every
// alloc for ALL live blocks) we verify the canary; a clobbered byte means an
// out-of-bounds write, and we report the victim block's size, the overrun offset,
// and a hexdump of what overwrote it (which usually identifies the source). This
// finds the corruption that Guard Malloc (broken under Rosetta) and guard pages
// (false-positive on SIMD over-reads) could not.
#define HD_MAGIC 0x484432ULL         /* 'HD2' */
#define HD_RZ    64                  /* redzone bytes each side */
#define HD_CANARY 0xBE
static int g_heapdbg=-1;
static inline int heapdbg(void){ if(g_heapdbg<0) g_heapdbg=getenv("PELOAD_HEAP_DEBUG")?1:0; return g_heapdbg; }
// Metadata is stored SEPARATELY from the heap buffer (so an overflow can't corrupt
// our size/list). The user buffer is [head RZ | data | tail RZ]; the 8 bytes right
// before the user data hold a back-pointer to the HDBlk (the rest of the head RZ is
// canary). A block whose TAIL redzone is clobbered is the OVERFLOW SOURCE.
typedef struct HDBlk { uint64_t magic; uint64_t size; uint8_t* buf; struct HDBlk *next,*prev; } HDBlk;
static HDBlk* g_hdlist; static pthread_mutex_t g_hdm=PTHREAD_MUTEX_INITIALIZER; static uint64_t g_hdcount; static int g_hd_reported;
static void hd_report(HDBlk*b,const char*when){
    uint8_t* user=b->buf+HD_RZ; int head=-1; long tail=-1;
    // count how many bytes of each redzone are clobbered. Benign Windows heap
    // rounding lets guests use a few bytes past the request, so only flag a redzone
    // that is clobbered deeply (>= 48 of 64 bytes) — that means the write ran well
    // past the block, i.e. a real overflow into the adjacent allocation.
    int tailbad=0; for(uint64_t i=0;i<HD_RZ;i++) if(user[b->size+i]!=HD_CANARY) tailbad++;
    int headbad=0; for(int i=0;i<HD_RZ-8;i++) if(user[-HD_RZ+i]!=HD_CANARY) headbad++;
    for(uint64_t i=0;i<HD_RZ;i++) if(user[b->size+i]!=HD_CANARY){ tail=(long)i; break; }
    for(int i=0;i<HD_RZ-8;i++) if(user[-HD_RZ+i]!=HD_CANARY){ head=i; break; }
    if(tailbad<48) tail=-1;
    if(headbad<40) head=-1;
    if(head<0&&tail<0) return;
    // skip blocks whose own data is entirely zero: those are downstream VICTIMS of
    // a big zero-run, not the source. The source has real (non-zero) data then
    // zeros spilling past its end.
    if(tail>=0){ int nz=0; uint64_t st=b->size>64?b->size-64:0; for(uint64_t i=st;i<b->size;i++) if(user[i]){ nz=1; break; } if(!nz) return; }
    char m[320]; int n=snprintf(m,sizeof m,"\n[HEAP-CORRUPT @%s] user=%p size=%llu %s clobbered (+%ld); bytes: ",
        when,(void*)user,(unsigned long long)b->size, tail>=0?"TAIL(=overflow SOURCE)":"HEAD(=victim)", tail>=0?tail:(long)(head-HD_RZ));
    (void)!write(2,m,n);
    uint8_t* z = tail>=0? user+b->size : user-HD_RZ+(head<0?0:head);
    char hx[160]; int hn=0; for(int i=0;i<40;i++) hn+=snprintf(hx+hn,sizeof hx-hn,"%02x ",z[i]); hx[hn++]='\n'; (void)!write(2,hx,hn);
    if(tail>=0){ char tb[220]; int tn=snprintf(tb,sizeof tb,"  last 32B of this block's data: ");
        uint64_t st=b->size>32?b->size-32:0; for(uint64_t i=st;i<b->size;i++) tn+=snprintf(tb+tn,sizeof tb-tn,"%02x ",user[i]); tb[tn++]='\n'; (void)!write(2,tb,tn); }
}
static void hd_scan_all(const char*when){ pthread_mutex_lock(&g_hdm); for(HDBlk*b=g_hdlist;b;b=b->next) hd_report(b,when); pthread_mutex_unlock(&g_hdm); }
static void* hd_alloc(uint64_t n){
    uint8_t* buf=malloc(HD_RZ+n+HD_RZ); if(!buf) return 0; HDBlk* b=malloc(sizeof(HDBlk)); if(!b){ free(buf); return 0; }
    b->magic=HD_MAGIC; b->size=n; b->buf=buf;
    uint8_t* user=buf+HD_RZ;
    memset(user-HD_RZ,HD_CANARY,HD_RZ-8); *(HDBlk**)(user-8)=b; memset(user+n,HD_CANARY,HD_RZ);
    pthread_mutex_lock(&g_hdm); b->next=g_hdlist; b->prev=0; if(g_hdlist)g_hdlist->prev=b; g_hdlist=b; uint64_t c=++g_hdcount; pthread_mutex_unlock(&g_hdm);
    if(getenv("PELOAD_HEAP_SCAN") && (c & 0xFF)==0) hd_scan_all("periodic");
    return user; }
static HDBlk* hd_blk(void*user){ if(!user) return 0; HDBlk* b=*(HDBlk**)((uint8_t*)user-8); return (b && b->magic==HD_MAGIC)? b : 0; }
static void hd_free(void*user){ HDBlk* b=hd_blk(user); if(!b){ free(user); return; } hd_report(b,"free");
    pthread_mutex_lock(&g_hdm); if(b->prev)b->prev->next=b->next; else g_hdlist=b->next; if(b->next)b->next->prev=b->prev; pthread_mutex_unlock(&g_hdm);
    b->magic=0; free(b->buf); free(b); }
// only guard large allocations (the observed overflow victims were >=4KB); small
// blocks stay on malloc so the millions of tiny allocs don't exhaust VM.
static inline int hg_want(uint64_t n){ return heapguard() && n>=2048; }
// track guarded (mmap) pointers so HeapFree can leak them but free() the rest.
#define HGSET (1u<<19)
static void* g_hgset[HGSET]; static uint64_t g_hgsz[HGSET]; static pthread_mutex_t g_hgm=PTHREAD_MUTEX_INITIALIZER;
static void hg_add(void*p,uint64_t n){ pthread_mutex_lock(&g_hgm); uint64_t i=((uintptr_t)p>>4)%HGSET; for(uint32_t k=0;k<HGSET;k++){ if(!g_hgset[i]||g_hgset[i]==(void*)1){g_hgset[i]=p;g_hgsz[i]=n;break;} i=(i+1)%HGSET; } pthread_mutex_unlock(&g_hgm); }
// returns requested size of the guarded block (and tombstones it), or 0 if not found/not guarded.
static uint64_t hg_del(void*p){ pthread_mutex_lock(&g_hgm); uint64_t i=((uintptr_t)p>>4)%HGSET,sz=0; for(uint32_t k=0;k<HGSET;k++){ if(g_hgset[i]==p){g_hgset[i]=(void*)1;sz=g_hgsz[i]?g_hgsz[i]:1;break;} if(!g_hgset[i])break; i=(i+1)%HGSET; } pthread_mutex_unlock(&g_hgm); return sz; }
// If fault address `f` lands in (or just past) a guarded block's guard page,
// return the owning block's user pointer and requested size — proves a real
// guest overflow and says by how much. slack=48 matches hg_alloc.
static void* hg_guardhit(void*f, void**buf, uint64_t*rsz, int64_t*over){
    uintptr_t fa=(uintptr_t)f; pthread_mutex_lock(&g_hgm);
    for(uint32_t i=0;i<HGSET;i++){ void*u=g_hgset[i]; if(!u||u==(void*)1) continue;
        uintptr_t guard=(uintptr_t)u + g_hgsz[i] + HG_SLACK;   // page-aligned guard start
        if(fa>=guard && fa<guard+4096){ if(buf)*buf=u; if(rsz)*rsz=g_hgsz[i];
            if(over)*over=(int64_t)(fa - ((uintptr_t)u + g_hgsz[i])); pthread_mutex_unlock(&g_hgm); return u; } }
    pthread_mutex_unlock(&g_hgm); return 0; }
static void*  MSABI i_HeapAlloc(void *h, uint32_t fl, uint64_t n){ (void)h;
    if(heapdbg()){ void*p=hd_alloc(n?n:1); if(p&&(fl&8)) memset(p,0,n); return p; }
    void*p; if(hg_want(n)){ p=hg_alloc(n); if(p) hg_add(p,n); } else p=malloc(n?n:1);
    if(p&&(fl&8)) memset(p,0,n);
    { const char*ts=getenv("PELOAD_TRACE_ALLOC"); if(ts && n==(uint64_t)strtoull(ts,0,0)){
        void*ret=__builtin_return_address(0); char t[96];
        int k=snprintf(t,sizeof t,"  [ALLOC %llu] p=%p caller=%p\n",(unsigned long long)n,p,ret); (void)!write(2,t,k); } }
    return p; }
static void*  MSABI i_HeapReAlloc(void *h, uint32_t fl, void *p, uint64_t n){ (void)h;(void)fl;
    if(heapdbg()){ HDBlk*b=hd_blk(p); void*q=hd_alloc(n?n:1); if(q&&p){ uint64_t cp = b? (b->size<n?b->size:n) : n; memcpy(q,p,cp); } if(b) hd_free(p); return q; }
    if(heapguard()){ void*q = hg_want(n)? hg_alloc(n) : malloc(n?n:1); if(q&&hg_want(n)) hg_add(q,n);
        if(p&&q){ uint64_t osz=hg_del(p);                 // osz>0 => old block was guarded
            uint64_t cp = osz? (osz<n?osz:n) : (malloc_size(p)<n?malloc_size(p):n); // copy min(old,new): never over-read p
            memcpy(q,p,cp); if(!osz) free(p); }           // guarded old -> leak; malloc'd old -> free
        return q; }
    return realloc(p,n?n:1); }
static int    MSABI i_HeapFree(void *h, uint32_t fl, void *p){ (void)h;(void)fl;
    if(heapdbg()){ hd_free(p); return 1; }
    if(heapguard()){ if(!hg_del(p)) free(p); return 1; }   // guarded -> leak; else free
    free(p); return 1; }
static uint64_t MSABI i_HeapSize(void *h, uint32_t fl, void *p){ (void)h;(void)fl;
    if(heapdbg()){ HDBlk*b=hd_blk(p); return b? b->size : 0; }
    if(heapguard()) return 0; return p? malloc_size(p) : 0; }
static void*  MSABI i_HeapCreate(uint32_t o, uint64_t a, uint64_t b){ (void)o;(void)a;(void)b; return (void*)0x48454150; }

// Virtual memory backed by mmap
static void*  MSABI i_VirtualAlloc(void *addr, uint64_t sz, uint32_t type, uint32_t prot){
    (void)type;(void)prot; uint64_t want=sz?sz:4096;
    // Trailing guard page (PELOAD_ALLOC_GUARD): the GL-on-Metal render path's SIMD texture/buffer
    // copies read up to a vector width PAST the end of a source buffer; when it ends flush on an
    // unmapped page the benign overread faults (~6s into Guardian; read at a page boundary,
    // rbx=rsi-0x10). Pad ONLY general/high allocations (hint==NULL or >=0x200000000), never the
    // in-window reservations (hint in [0x100000000,0x200000000)) that the engine's "64MB free
    // within 2GB of halo3" scan depends on — padding those fragments the window and the scan fails.
    uint64_t hint=(uint64_t)addr;
    uint64_t guard = (getenv("PELOAD_ALLOC_GUARD") && (hint==0 || hint>=0x200000000ULL)) ? 4096 : 0;
    void *p=mmap(addr, want+guard, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1,0);
    if(getenv("PELOAD_TRACE_VA")){ char t[144]; snprintf(t,sizeof t,"  [VirtualAlloc] want=%p size=0x%llx type=0x%x -> %p (+0x%llx)\n",addr,(unsigned long long)sz,type,p==MAP_FAILED?0:p,(unsigned long long)guard); ws(t); }
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

// TLS: Windows TLS slots are PER-THREAD. A shared global table would let threads
// clobber each other's thread-local state -> timing-dependent corruption. Store
// values in each thread's TEB TlsSlots (TEB+0x1480), which thread_tramp/main set
// up per thread at %gs:0x30. (Implicit __declspec(thread) TLS uses gs:0x58 and is
// separate.) Only the allocator index counter is global, guarded by a mutex.
static uint32_t g_tlsnext=1; static pthread_mutex_t g_tlsm=PTHREAD_MUTEX_INITIALIZER;
static uint32_t MSABI i_TlsAlloc(void){ pthread_mutex_lock(&g_tlsm); uint32_t i = g_tlsnext<256? g_tlsnext++ : 0xFFFFFFFFu; pthread_mutex_unlock(&g_tlsm); return i; }
static inline void** tls_slot(uint32_t i){ uint64_t teb=0; __asm__ volatile("movq %%gs:0x30,%0":"=r"(teb)); return teb? (void**)(teb+0x1480+(uint64_t)i*8) : 0; }
static void*  MSABI i_TlsGetValue(uint32_t i){ if(i>=256) return 0; void**s=tls_slot(i); return s? *s : 0; }
static int    MSABI i_TlsSetValue(uint32_t i, void *v){ if(i>=256) return 0; void**s=tls_slot(i); if(!s) return 0; *s=v; return 1; }
static int    MSABI i_TlsFree(uint32_t i){ (void)i; return 1; }

// ---- expanded kernel32/core surface that genuine ucrtbase needs ----
// console/file I/O: pseudo-handles 1/2/3 == stdin/stdout/stderr
static void*  MSABI i_GetStdHandle(uint32_t id){ return (void*)(intptr_t)(id==(uint32_t)-10?1: id==(uint32_t)-11?2: id==(uint32_t)-12?3: 0); }
static int pipe_is(intptr_t hv);          // fwd (named-pipe handle predicate)
static int pipe_write(void*h,const void*buf,uint32_t n,uint32_t*wr); // fwd (defined with the pipe subsystem)
static int pipe_read(void*h,void*buf,uint32_t n,uint32_t*rd);        // fwd
static int pipe_close(void*h);                                       // fwd
static int pipe_name_is(const char*s);                               // fwd
static void* pipe_client_open(const char*narrow);                    // fwd
static int    MSABI i_WriteFile(void *h, const void *buf, uint32_t n, uint32_t *wr, void *ov){
    (void)ov; intptr_t hv=(intptr_t)h;
    if(pipe_is(hv)) return pipe_write(h,buf,n,wr);
    int fd = (hv==2)?1 : (hv==3)?2 : (hv>=0x1000)?(int)(hv-0x1000) : -1;
    // PELOAD_CAP_ADV: dump the exact bytes written to a local-server advertisement file
    // (the <uuid>.partial / .json), so we learn the real schema the discovery validator wants.
    if(fd>=0 && fd<MAXFD && g_fdpath[fd] && buf && n>0 && n<8192 && getenv("PELOAD_CAP_ADV")
       && (strstr(g_fdpath[fd],"local-servers")||strstr(g_fdpath[fd],".partial"))){
        ws("  [adv-write] "); ws(g_fdpath[fd]); ws(" <= ");
        (void)!write(2,buf,n); ws("\n");
    }
    // PELOAD_TRACE_BT: when a watched log line is written, scan the stack for exe/halo3
    // return addresses to find the REAL emitter + callers (bypasses unreliable symmap).
    if(fd>=0 && n>8 && n<4096 && getenv("PELOAD_TRACE_BT")){ const char*w=getenv("PELOAD_TRACE_BT");
        if(memmem(buf,n,w,strlen(w))){ (void)!write(2,"  [BT] matched; stack exe/halo3 returns:\n",41);
            uint64_t*sp=(uint64_t*)__builtin_frame_address(0); int pr=0;
            for(int i=0;i<4000 && pr<60;i++){ uint64_t v=sp[i];
                if(v>=0x140000000ULL&&v<0x142000000ULL){ char b[48]; int m=snprintf(b,sizeof b,"     exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,b,m); pr++; }
                else if(v>=0x180000000ULL&&v<0x187000000ULL){ char b[48]; int m=snprintf(b,sizeof b,"     halo3+0x%llx\n",(unsigned long long)(v-0x180000000ULL)); (void)!write(2,b,m); pr++; } } } }
    // PELOAD_FORCE_HOSTING: the worker reports its hosting-verification result to the
    // orchestrator as JSON on stdout. Flip "hosting_verified":false -> true (length-preserving
    // "false"->"true ") so the orchestrator proceeds past the probe to the rendering client.
    // Experiment to discover the post-verification behavior; not a real verification.
    if(fd==1 && buf && n>20 && (size_t)n<(1u<<20) && getenv("PELOAD_FORCE_HOSTING") && memmem(buf,n,"_verified\":false",16)){
        char*cp=malloc(n); if(cp){ memcpy(cp,buf,n);
            // replace every `_verified":false` -> `_verified":true ` (length-preserving)
            for(size_t i=0; i+16<=(size_t)n; i++){ if(!memcmp(cp+i,"_verified\":false",16)) memcpy(cp+i+11,"true ",5); }
            long k2=write(fd,cp,n); free(cp); if(wr)*wr=(uint32_t)(k2<0?0:k2); return 1; } }
    // Robust std write: never report failure for a standard handle (Rust panics
    // "failed printing to stderr" and aborts if WriteFile fails). Fall back to our
    // real stderr when the mapped fd is bad, and always report the full count written.
    long k = fd>=0? write(fd,buf,n) : -1;
    if(k<0 && (hv==1||hv==2||hv==3||fd>=0)){ (void)!write(2,buf,n); k=n; }
    if(wr)*wr=(uint32_t)(k<0?0:k);
    return (fd>=0 || hv==1||hv==2||hv==3) ? 1 : 0; }
static int    MSABI i_WriteConsoleW(void *h, const uint16_t *s, uint32_t n, uint32_t *wr, void *rsv){
    (void)rsv; int fd=(h==(void*)3)?2:1; for(uint32_t i=0;i<n;i++){ char c=(s[i]<128)?(char)s[i]:'?'; (void)!write(fd,&c,1);} if(wr)*wr=n; return 1; }
// ReadConsoleW/A: the dedicated server's game-server host reads its console (stdin) to wait for a
// shutdown command / EOF while its server threads run. The old generic stub returned 0 (FALSE) ->
// Rust saw a read error -> the host shut down immediately (exit code 1). Read from the real stdin fd,
// BLOCKING like a true console, so the host parks instead of exiting. One byte -> one UTF-16 char.
static int    MSABI i_ReadConsoleW(void *h, uint16_t *buf, uint32_t nchars, uint32_t *nread, void *rsv){
    (void)rsv; intptr_t hv=(intptr_t)h; int fd=(hv==1)?0 : (hv>=0x1000)?(int)(hv-0x1000):0;
    uint32_t got=0; while(got<nchars){ unsigned char c; ssize_t r=read(fd,&c,1); if(r<=0) break; buf[got++]=c; if(c=='\n') break; }
    if(nread)*nread=got; return 1; }
static int    MSABI i_ReadConsoleA(void *h, char *buf, uint32_t nb, uint32_t *nread, void *rsv){
    (void)rsv; intptr_t hv=(intptr_t)h; int fd=(hv==1)?0 : (hv>=0x1000)?(int)(hv-0x1000):0;
    uint32_t got=0; while(got<nb){ char c; ssize_t r=read(fd,&c,1); if(r<=0) break; buf[got++]=c; if(c=='\n') break; }
    if(nread)*nread=got; return 1; }
static uint32_t MSABI i_GetFileType(void *h){ intptr_t hv=(intptr_t)h; return hv>=0x1000? 1 : 2; } // DISK : CHAR
static int    MSABI i_FlushFileBuffers(void *h){ (void)h; return 1; }
static void*  MSABI i_GetCurrentProcess(void){ return (void*)-1; }
// Read/WriteProcessMemory: the engine probe reads/patches halo3.dll in THIS
// process (shell-callback mechanism), so these are plain in-process memcpy.
// PELOAD_FORGE_MAP: the guardian-setup shell callback reads halo3's selected-MP-map
// slot (a Rust &str {ptr,len}) at halo3+<rva> and finds it empty (the disposable probe
// never populated it), so it refuses with "got ''". When set, intercept that 16-byte
// read and return {ptr->"halo3\maps\guardian", len=19} so validation passes. rva is
// configurable via PELOAD_FORGE_MAP_RVA (default 0xb43c08).
static const char g_forge_mapname[] = "halo3\\maps\\guardian"; // 19 chars
static int MSABI i_ReadProcessMemory(void*h,const void*addr,void*buf,uint64_t n,uint64_t*rd){ (void)h;
    if(getenv("PELOAD_TRACE_RPM")){ char t[160]; uint64_t a=(uint64_t)addr; const char*mod=(a>=0x180000000&&a<0x187000000)?"halo3":(a>=0x140000000&&a<0x14c000000)?"exe":"?";
        uint64_t off=(a>=0x180000000&&a<0x187000000)?a-0x180000000:(a>=0x140000000&&a<0x14c000000)?a-0x140000000:a;
        uint64_t v=0; if(n>=1&&n<=8) memcpy(&v,addr,n);
        snprintf(t,sizeof t,"  [RPM] %s+0x%llx n=0x%llx val=0x%llx\n",mod,(unsigned long long)off,(unsigned long long)n,(unsigned long long)v); ws(t);}
    { const char*fm=getenv("PELOAD_FORGE_MAP");
      if(fm && g_base){ uint64_t a=(uint64_t)addr;
        const char*rs=getenv("PELOAD_FORGE_MAP_RVA"); uint64_t frva=rs?strtoull(rs,0,16):0xb43c08;
        uint64_t slot=0x180000000ULL+frva;
        if(a==slot && n>=16){ memcpy(buf,addr,n);
            *(uint64_t*)buf=(uint64_t)g_forge_mapname;          // ptr
            *(uint64_t*)((char*)buf+8)=(uint64_t)(sizeof g_forge_mapname-1); // len=19
            if(rd)*rd=n; char t[96]; int k=snprintf(t,sizeof t,"  [FORGE-MAP] halo3+0x%llx -> \"%s\"(len %zu)\n",(unsigned long long)frva,g_forge_mapname,sizeof g_forge_mapname-1); (void)!write(2,t,k);
            return 1; } } }
    memcpy(buf,addr,n); if(rd)*rd=n; return 1; }
static int MSABI i_WriteProcessMemory(void*h,void*addr,const void*buf,uint64_t n,uint64_t*wr){ (void)h;
    uintptr_t pg=(uintptr_t)addr & ~0xfffULL; size_t len=(((uintptr_t)addr+n)-pg+0xfff)&~0xfffULL;
    if(mprotect((void*)pg,len,PROT_READ|PROT_WRITE|PROT_EXEC)!=0) mprotect((void*)pg,len,PROT_READ|PROT_WRITE);
    // Do NOT downgrade the page to r-x afterwards: if the target is writable
    // data/stack (or `wr` shares the page), forcing r-x faults the next write.
    // Leave it writable+executable (RWX runs under Rosetta) and invalidate the
    // instruction cache so a code patch re-translates.
    if(getenv("PELOAD_TRACE_RPM")){ uint64_t a=(uint64_t)addr; if(a>=0x180000000&&a<0x187000000){ uint64_t v=0; if(n>=1&&n<=8) memcpy(&v,buf,n);
        char t[128]; snprintf(t,sizeof t,"  [WPM] halo3+0x%llx n=0x%llx val=0x%llx\n",(unsigned long long)(a-0x180000000),(unsigned long long)n,(unsigned long long)v); ws(t);} }
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
// ---- Minimal IDXGIAdapter (PELOAD_DXGI_ADAPTER) ----------------------------
// By default dxgi_EnumAdapters reports "no adapters", which steers the engine onto its
// GL/menu fallback path: it then submits ONLY fullscreen composite passes and NEVER draws the
// 3D world (zero DrawIndexed, census geometry=0). Reporting a real adapter makes the engine take
// the D3D hardware path so it submits world geometry we can then translate. Opt-in while bringing
// up the D3D11->GL renderer.
static int MSABI dxgiadapter_EnumOutputs(void*self,unsigned i,void**pp){ (void)self;(void)i; if(pp)*pp=0; return HR_DXGI_NOT_FOUND; }
static int MSABI dxgiadapter_CheckInterfaceSupport(void*self,GUID_*g,void*umd){ (void)self;(void)g; if(umd)*(uint64_t*)umd=((uint64_t)11<<48)|((uint64_t)0<<32); return HR_OK; }
static ComObj g_dxgiadapter; // real adapter object (vtbl populated in d3d_init); reused here (tentative def)
static int MSABI dxgi_EnumAdapters(void*self,unsigned i,void**pp){ (void)self;
    if(getenv("PELOAD_DXGI_ADAPTER") && i==0){ if(pp)*pp=&g_dxgiadapter; return HR_OK; }
    if(pp)*pp=0; return HR_DXGI_NOT_FOUND; }
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
typedef struct { void** vtbl; uint32_t kind; uint32_t rowpitch; uint64_t size; void* data; uint32_t w,h;
    uint32_t fmt;                 // texture/index DXGI format
    int nelem;                    // input-layout element count
    struct { uint32_t off, fmt, slot; char sem[12]; } elem[16];
    // ---- GL translation state (real D3D11->GL renderer) ----
    void* bc; uint32_t bclen;     // retained shader bytecode (DXBC) for translation
    int stype;                    // shader stage: 0=VS 1=PS 2=GS 3=HS 4=DS 5=CS
    unsigned gltex;               // GL texture name (textures) / color texture for an RT
    unsigned glbuf;               // GL buffer name (VB/IB/CB)
    unsigned glfbo;               // GL framebuffer (render targets: FBO wrapping gltex)
    unsigned glprog;              // GL program (translated shader)
    int gl_dirty;                 // backing data changed since last GL upload
    int gl_uploaded;              // texture contents have been uploaded at least once
    char* glsl;                   // translated GLSL source (shaders; loaded in dev_crShader)
    unsigned glsh;                // compiled GL shader object (0 until first compile)
    int gl_locinit;               // shader: uniform-location cache populated (0=no)
    int gl_cbloc[16];             // shader: glGetUniformLocation("cb<b>")  (-1 absent)
    int gl_tloc[32];              // shader: glGetUniformLocation("t<r>")   (-1 absent)
    int vs_vtxin;                 // VS: has classic vertex inputs (POSITION/NORMAL/..) not just SV_*
    void* pdata;                  // lazily-allocated private-data store (SetPrivateData/GetPrivateData)
    int is_cb;                    // buffer created with D3D11_BIND_CONSTANT_BUFFER
    uint64_t cb_upd;              // draw-tick at which this CB was last written (Map/UpdateSubresource)
} D3DObj;
// ID3D11DeviceChild private data: halo3 stashes the VS bytecode-blob pointer on the shader object
// (SetPrivateData with GUID g_180801e00) and reads it back for CreateInputLayout. Our old no-op
// SetPrivateData/GetPrivateData dropped it -> CreateInputLayout got a null bytecode provider ->
// sub_1802ac24c would fault -> PELOAD_SKINGUARD made it return 0 -> NO input layout EVER -> all
// world geometry drawn without a layout (invisible). Real per-object storage fixes it.
typedef struct { GUID_ g; uint32_t sz; uint8_t buf[64]; } PDEnt;
typedef struct { int n; PDEnt e[12]; } PDStore;
// Bound pipeline state we track to interpret/translate draws.
static struct {
    D3DObj *il, *vb[8], *ib, *psrv[32], *rtv;
    D3DObj *rtvs[8]; int num_rt; D3DObj *dsv;  // multiple render targets (deferred G-buffer) + depth
    D3DObj *vs, *ps, *vscb[16], *pscb[16];   // bound shaders + constant buffers (for D3DGL)
    uint32_t vscb_1st[16], vscb_cnt[16], pscb_1st[16], pscb_cnt[16]; // *SetConstantBuffers1 ranges (16B units)
    D3DObj *vssrv[16];                        // VS-stage shader resources (vertex-pulling geometry!)
    D3DObj *blend, *depth, *raster;           // bound state objects (descs captured at create time)
    uint32_t vstride[8], voff[8], ibfmt, iboff, topo; int vp_w, vp_h;
} g_d3dst;
// IA/VS census (PELOAD_IA_CENSUS): is halo3 submitting classic VB geometry, or vertex-pulling via
// VS shader-resources (slot 25, historically unhooked => invisible)? This settles "geometry=0".
static struct { uint64_t ia_vb, ia_il, vs_srv, vs_classic, vs_pull, vs_set; } g_iacen;
// The engine composites its whole frame into a ~720p BGRA texture and draws it as a
// fullscreen quad to the swap chain. We capture that texture and present it to the window.
static void* g_d3d_frame_bits; static int g_d3d_frame_w, g_d3d_frame_h, g_d3d_frame_pitch;
static ComObj g_d3ddevice, g_d3dcontext;
static void*  g_d3dchild_vtbl[16];   // ID3D11DeviceChild (shaders, states, layouts, linkage, query...)
static void*  g_d3dres_vtbl[16];     // ID3D11Resource (buffers, textures)
static void*  g_d3dview_vtbl[16];    // ID3D11View (SRV/RTV/DSV/UAV)
static void*  g_d3ddev_vtbl[48];
static void*  g_d3dctx_vtbl[160];
static void*  g_dxgiswap_vtbl[40];
static uint8_t g_d3dscratch[1<<20];  // fallback Map target
static D3DObj* d3dobj(void** vt, uint32_t kind, uint64_t size){
    D3DObj* o=calloc(1,sizeof *o); o->vtbl=vt; o->kind=kind; o->size=size;
    if(size){
        if(getenv("PELOAD_D3D_GUARD")){
            // place the buffer's END against a PROT_NONE guard page so any write
            // past `size` faults immediately at the offending instruction.
            uint64_t ps=4096, nb=((size+ps-1)/ps)*ps;
            uint8_t* m=mmap(0,nb+ps,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
            if(m!=MAP_FAILED){ mprotect(m+nb,ps,PROT_NONE); o->data=m+(nb-size); }
            else o->data=calloc(1,size);
        } else o->data=calloc(1,size);
    }
    return o; }
// ---- known interface IIDs + per-IID QueryInterface ----
// The frontend (ui_composition) does IDXGISwapChain::GetDevice(ID3D11Device) and
// device.QueryInterface(IDXGIDevice); returning the WRONG vtable makes later method
// calls misbehave. Hand back the right object per IID.
static const GUID_ IID_ID3D11Device   ={0xdb6f6ddb,0xac77,0x4e88,{0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40}};
static const GUID_ IID_ID3D11Device1  ={0xa04bfb29,0x08ef,0x43d6,{0xa4,0x9c,0xa9,0xbd,0xbd,0xcb,0xe6,0x86}};
static const GUID_ IID_IDXGIDevice    ={0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
static const GUID_ IID_IDXGIDevice1   ={0x77db970f,0x6276,0x48ba,{0xba,0x28,0x07,0x01,0x43,0xb4,0x39,0x2c}};
static const GUID_ IID_IDXGIAdapter   ={0x2411e7e1,0x12ac,0x4ccf,{0xbd,0x14,0x97,0x98,0xe8,0x53,0x4d,0xc0}};
static const GUID_ IID_ID3D11DeviceContext={0xc0bfa96c,0xe089,0x44fb,{0x8e,0xaf,0x26,0xf8,0x79,0x61,0x90,0xda}};
static int guid_eq(const GUID_*a,const GUID_*b){ return a&&b&&a->D1==b->D1&&a->D2==b->D2&&a->D3==b->D3&&!memcmp(a->D4,b->D4,8); }
static ComObj g_dxgidevice, g_dxgiadapter; static void* g_dxgidev_vtbl[16]; static void* g_dxgiadapter_vtbl[16];
// ---- ID3D11DeviceChild base (slots 0-6) ----
static void qi_trace(const char*who,GUID_*r);
static int      MSABI d3d_QI(void*s,GUID_*r,void**pp){ qi_trace("obj",r); if(pp)*pp=s; return HR_OK; }
// device-level QI: route ID3D11Device*/IDXGIDevice*/context to the right object.
static void qi_trace(const char*who,GUID_*r); // fwd
static int      MSABI dev_QI(void*s,GUID_*r,void**pp){ (void)s; qi_trace("device",r); if(!pp) return HR_OK;
    if(guid_eq(r,&IID_IDXGIDevice)||guid_eq(r,&IID_IDXGIDevice1)) *pp=&g_dxgidevice;
    else if(guid_eq(r,&IID_ID3D11DeviceContext)) *pp=&g_d3dcontext;
    else *pp=&g_d3ddevice;   // ID3D11Device/Device1/unknown -> the device
    return HR_OK; }
static int  MSABI dxgidev_GetAdapter(void*s,void**pp){ (void)s; if(getenv("PELOAD_TRACE_QI"))(void)!write(2,"  [GetAdapter]\n",15); if(pp)*pp=&g_dxgiadapter; return HR_OK; } // IDXGIDevice::GetAdapter -> HRESULT
static int  MSABI dxgidev_GetParent(void*s,GUID_*r,void**pp){ (void)s;(void)r; if(pp)*pp=&g_dxgifactory; return HR_OK; }
// Adapter QI: the frontend probes IDXGIAdapter3 (QueryVideoMemoryInfo for GPU mem
// usage/budget). Returning self for that IID makes it think Adapter3 EXISTS, then
// QueryVideoMemoryInfo returns garbage and it builds a null string -> fault. Report
// only IUnknown/IDXGIObject/IDXGIAdapter; E_NOINTERFACE for Adapter1/2/3/4 so the
// engine takes its valid "IDXGIAdapter3 unavailable" fallback.
static const GUID_ IID_IUnknown     ={0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID_ IID_IDXGIObject  ={0xaec22fb8,0x76f3,0x4639,{0x9b,0xe0,0x28,0xeb,0x43,0xa6,0x7a,0x2e}};
static void qi_trace(const char*who,GUID_*r){ if(!getenv("PELOAD_TRACE_QI")||!r) return;
    char b[120]; int k=snprintf(b,sizeof b,"  [QI %s] {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",who,r->D1,r->D2,r->D3,
        r->D4[0],r->D4[1],r->D4[2],r->D4[3],r->D4[4],r->D4[5],r->D4[6],r->D4[7]); (void)!write(2,b,k); }
static int  MSABI adapter_QI(void*s,GUID_*r,void**pp){ qi_trace("adapter",r); if(!pp) return HR_OK;
    if(guid_eq(r,&IID_IUnknown)||guid_eq(r,&IID_IDXGIObject)||guid_eq(r,&IID_IDXGIAdapter)){ *pp=s; return HR_OK; }
    *pp=0; return (int)0x80004002; /*E_NOINTERFACE*/ }
// DXGI_ADAPTER_DESC: Description WCHAR[128]@0, VendorId@256, DeviceId@260, ...,
// DedicatedVideoMemory(SIZE_T)@272. Give a real name + nonzero VRAM so the
// frontend's "Graphics card: <name>" path has valid data.
// DXGI_ADAPTER_DESC is EXACTLY 0x130 (304) bytes: Description WCHAR[128]@0,
// VendorId@256, DeviceId@260, SubSysId@264, Revision@268, DedicatedVideoMemory@272,
// DedicatedSystemMemory@280, SharedSystemMemory@288, AdapterLuid@296. Writing more
// (we used 320) overflows the caller's 304-byte buffer and smashes its saved
// registers (rbx) -> a null string arg -> 0xC0000005 in the GPU-info log formatter.
static int  MSABI adapter_GetDesc(void*s,void*pd){ (void)s; if(getenv("PELOAD_TRACE_QI"))(void)!write(2,"  [adapter.GetDesc]\n",20); if(!pd) return HR_OK; uint8_t*d=pd; memset(d,0,304);
    const char*nm="Reclaimer GL (OpenGL on Metal)"; uint16_t*w=(uint16_t*)d; int i=0; for(; nm[i]&&i<127;i++) w[i]=(uint8_t)nm[i]; w[i]=0;
    *(uint32_t*)(d+256)=0x10de; *(uint32_t*)(d+260)=0x1; *(uint64_t*)(d+272)=(uint64_t)4096*1024*1024; *(uint64_t*)(d+280)=(uint64_t)2048*1024*1024; *(uint64_t*)(d+288)=(uint64_t)2048*1024*1024;
    return HR_OK; }
static unsigned MSABI d3d_AddRef(void*s){ (void)s; return 1; }
static unsigned MSABI d3d_Release(void*s){ (void)s; return 0; }
static void     MSABI d3d_GetDevice(void*s,void**pp){ (void)s; if(pp)*pp=&g_d3ddevice; }
static int      MSABI d3d_GetPD(void*s,GUID_*g,uint32_t*ps,void*pd){ (void)s;(void)g;(void)pd; if(ps)*ps=0; return HR_NOTFOUND; }
static int      MSABI d3d_SetPD(void*s,GUID_*g,uint32_t sz,const void*d){ (void)s;(void)g;(void)sz;(void)d; return HR_OK; }
static int      MSABI d3d_SetPDI(void*s,GUID_*g,const void*d){ (void)s;(void)g;(void)d; return HR_OK; }
// Real private-data storage for D3DObj-backed children (shaders etc.). halo3 round-trips the VS
// bytecode-blob pointer through here to build input layouts (see PDStore note on D3DObj).
static void pd_trace(const char*tag,void*s,GUID_*g,uint32_t sz,int found){ if(!getenv("PELOAD_TRACE_PD")) return;
    static int c; if(c++>=120) return; char b[160]; int k=snprintf(b,sizeof b,"  [pd] %s obj=%p sz=%u found=%d guid={%08X-%04X-%04X} stype=%d\n",
        tag,s,sz,found,g?g->D1:0,g?g->D2:0,g?g->D3:0, s?((D3DObj*)s)->stype:-9); (void)!write(2,b,k); }
static int      MSABI child_SetPD(void*s,GUID_*g,uint32_t sz,const void*d){ D3DObj*o=s; if(!o||!g){ return HR_OK; }
    pd_trace("SetPD",s,g,sz,0);
    PDStore*p=o->pdata; if(!p){ p=calloc(1,sizeof *p); o->pdata=p; if(!p) return HR_OK; }
    uint32_t n=sz>sizeof p->e[0].buf?(uint32_t)sizeof p->e[0].buf:sz;
    for(int i=0;i<p->n;i++) if(guid_eq(&p->e[i].g,g)){ p->e[i].sz=sz; if(d&&n)memcpy(p->e[i].buf,d,n); return HR_OK; }
    if(p->n<(int)(sizeof p->e/sizeof p->e[0])){ PDEnt*e=&p->e[p->n++]; e->g=*g; e->sz=sz; if(d&&n)memcpy(e->buf,d,n); }
    return HR_OK; }
static int      MSABI child_GetPD(void*s,GUID_*g,uint32_t*ps,void*pd){ D3DObj*o=s; PDStore*p=o?o->pdata:0;
    if(p&&g) for(int i=0;i<p->n;i++) if(guid_eq(&p->e[i].g,g)){ uint32_t sz=p->e[i].sz;
        if(pd){ uint32_t cap=ps?*ps:sz; uint32_t n=sz<cap?sz:cap; if(n>sizeof p->e[0].buf)n=(uint32_t)sizeof p->e[0].buf; memcpy(pd,p->e[i].buf,n); }
        if(ps)*ps=sz; pd_trace("GetPD",s,g,sz,1); return HR_OK; }
    pd_trace("GetPD",s,g,ps?*ps:0,0);
    if(ps)*ps=0; return HR_NOTFOUND; }
// SetPrivateDataInterface: halo3 stashes an interface pointer (e.g. its VS bytecode-blob object)
// here, then reads it back via GetPrivateData as an 8-byte value. Store the pointer so the
// round-trip works (this is what feeds CreateInputLayout; our old no-op broke all input layouts).
static int      MSABI child_SetPDI(void*s,GUID_*g,const void*d){ D3DObj*o=s; if(!o||!g) return HR_OK;
    pd_trace("SetPDI",s,g,8,0);
    PDStore*p=o->pdata; if(!p){ p=calloc(1,sizeof *p); o->pdata=p; if(!p) return HR_OK; }
    for(int i=0;i<p->n;i++) if(guid_eq(&p->e[i].g,g)){ p->e[i].sz=8; memcpy(p->e[i].buf,&d,8); return HR_OK; }
    if(p->n<(int)(sizeof p->e/sizeof p->e[0])){ PDEnt*e=&p->e[p->n++]; e->g=*g; e->sz=8; memcpy(e->buf,&d,8); }
    return HR_OK; }
// ---- ID3D11Resource (7-10) ----
static void     MSABI res_GetType(void*s,uint32_t*pd){ D3DObj*o=s; if(pd)*pd=o?o->kind:1; }
static void     MSABI res_SetEvict(void*s,uint32_t p){ (void)s;(void)p; }
static unsigned MSABI res_GetEvict(void*s){ (void)s; return 0; }
static void     MSABI res_GetDesc(void*s,void*pd){ D3DObj*o=s; if(!pd||!o) return; uint32_t*u=pd;
    // write ONLY the caller's desc size (overwriting more corrupts its stack/heap)
    if(o->kind==1){ memset(pd,0,24); u[0]=(uint32_t)o->size; }                 // D3D11_BUFFER_DESC (24B): ByteWidth
    else if(o->kind==3){ memset(pd,0,32); u[0]=o->w; u[1]=1; u[2]=1; u[3]=28; } // TEXTURE1D_DESC (32B): Width,Mip,Array,Format
    else if(o->kind==4){ memset(pd,0,36); u[0]=o->w; u[1]=o->h; u[2]=1; u[3]=1; u[4]=28; } // TEXTURE3D_DESC (36B): W,H,Depth,Mip,Format
    else { memset(pd,0,44); u[0]=o->w; u[1]=o->h; u[2]=1; u[3]=1; u[4]=28; u[5]=1; } } // TEXTURE2D_DESC (44B)
// ---- ID3D11View (7-8). o->data holds the resource the view was created over, so
// GetResource returns a valid (never-null) resource the engine then dereferences.
static D3DObj* d3dobj(void** vt, uint32_t kind, uint64_t size); // fwd
static D3DObj  g_d3ddummyres_obj; static D3DObj* g_d3ddummyres;
static void     MSABI view_GetResource(void*s,void**pp){ D3DObj*o=s; if(pp)*pp=(o&&o->data)?o->data:g_d3ddummyres; }
static void     MSABI view_GetDesc(void*s,void*pd){ (void)s; if(pd) memset(pd,0,16); } // RTV/DSV descs are ~16B; keep small to avoid overflow
// ---- create helpers by out-param position ----
// Registry of CONSTANT buffers (BindFlags & D3D11_BIND_CONSTANT_BUFFER=0x4) so we can locate the
// global view-projection (b0) that halo3's material path binds via a vtable we don't drive.
static D3DObj* g_cbreg[8192]; static int g_ncbreg;
// CreateBuffer(desc,init,pp): D3D11_BUFFER_DESC { ByteWidth(0),Usage(4),BindFlags(8),CPUAccess(12),Misc(16),Stride(20) }.
static int MSABI dev_CreateBuffer(void*t,const void*desc,const void*init,void**pp){ (void)t;
    uint64_t sz = desc? (uint64_t)(*(const uint32_t*)desc) : 16; if(sz<16) sz=16;
    uint32_t bind = desc? *(const uint32_t*)((const char*)desc+8) : 0;
    D3DObj* o=d3dobj(g_d3dres_vtbl,1/*BUFFER*/,sz); o->rowpitch=(uint32_t)sz; o->w=(uint32_t)sz; o->h=1;
    if(init && o->data){ const void*src=*(void*const*)init; if(src) memcpy(o->data,src,sz); }
    if((bind&0x4) && o){ o->is_cb=1; if(g_ncbreg<(int)(sizeof g_cbreg/sizeof*g_cbreg)) g_cbreg[g_ncbreg++]=o; }
    if(pp)*pp=o; return HR_OK; }
// textures: compute the REAL footprint from the DXGI format so our buffer matches
// exactly what the engine writes via Map (no over/under-allocation -> no overflow).
static int is_bc(uint32_t f){ return (f>=70&&f<=84)||(f>=94&&f<=99); }      // BC1..BC7
static uint32_t bc_block(uint32_t f){ return ((f>=70&&f<=72)||(f>=79&&f<=81))?8:16; } // BC1,BC4=8B blocks
static uint32_t dxgi_bpp(uint32_t f){
    if(f>=1&&f<=4) return 16; if(f>=5&&f<=8) return 12; if(f>=9&&f<=14) return 8;
    if(f>=15&&f<=22) return 8; if(f==0) return 4; return 4; }                 // default RGBA8-class
static D3DObj* mktex(uint32_t w,uint32_t h,uint32_t arr,uint32_t fmt,uint32_t kind){
    if(w<1)w=1; if(h<1)h=1; if(arr<1)arr=1;
    uint64_t rp,sz;
    if(is_bc(fmt)){ uint32_t bb=bc_block(fmt); rp=(uint64_t)((w+3)/4)*bb; sz=rp*((h+3)/4); }
    else { rp=(uint64_t)w*dxgi_bpp(fmt); sz=rp*h; }
    sz = sz*arr*2 + 65536;                                                    // array + mip-chain + guard
    D3DObj* o=d3dobj(g_d3dres_vtbl,kind,sz); o->rowpitch=(uint32_t)rp; o->w=w; o->h=h; o->fmt=fmt; return o; }
static int MSABI dev_CreateTexture1D(void*t,const void*d,const void*init,void**pp){ (void)t;(void)init;
    uint32_t w=d?*(const uint32_t*)d:1, arr=d?*(const uint32_t*)((const char*)d+8):1, f=d?*(const uint32_t*)((const char*)d+12):0;
    if(pp)*pp=mktex(w,1,arr,f,3); return HR_OK; }
static void d3dtrace(int i,const char*nm); // fwd
static void draw_log(const char*tag,uint32_t count); // fwd
static int MSABI dev_CreateTexture2D(void*t,const void*d,const void*init,void**pp){ (void)t; d3dtrace(5,"CreateTexture2D");
    uint32_t w=d?*(const uint32_t*)d:1, h=d?*(const uint32_t*)((const char*)d+4):1, arr=d?*(const uint32_t*)((const char*)d+12):1, f=d?*(const uint32_t*)((const char*)d+16):0;
    // PELOAD_DUMP_TEX: capture textures created WITH pixel data (loaded game assets: loading art,
    // UI, fonts, map textures). Logs all >=128px textures; dumps 32bpp (RGBA8=28/29, BGRA8=87/91)
    // ones to BMP so we can display real Guardian imagery through the GL path (compressed BC* skipped).
    if(getenv("PELOAD_DUMP_TEX") && init && w>=128 && h>=128 && arr==1){
        const void* sys=*(const void* const*)init; uint32_t pitch=*(const uint32_t*)((const char*)init+8);
        char m[160]; int k=snprintf(m,sizeof m,"  [tex] %ux%u fmt=%u pitch=%u init=%p\n",w,h,f,pitch,sys); (void)!write(2,m,k);
        int is32=(f==28||f==29||f==87||f==91||f==88); int bgra=(f==87||f==91||f==88);
        if(sys && is32 && pitch>=w*4 && (uint64_t)pitch*h<=(64ull<<20)){
            static int tn; const char*home=getenv("HOME"); char p[600];
            snprintf(p,sizeof p,"%s/reclaimer_tex_%03d_%ux%u_f%u.bmp",home?home:"/tmp",tn++,w,h,f);
            FILE*fp=fopen(p,"wb");
            if(fp){ uint32_t isz=w*h*4,fsz=54+isz,o=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&o,4);
                uint8_t ih[40]={0}; uint32_t v=40; memcpy(ih,&v,4); memcpy(ih+4,&w,4); memcpy(ih+8,&h,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
                fwrite(fh,1,14,fp); fwrite(ih,1,40,fp);
                const uint8_t*src=sys;
                for(int y=(int)h-1;y>=0;y--){ const uint8_t*row=src+(size_t)y*pitch;
                    for(uint32_t x=0;x<w;x++){ const uint8_t*px=row+x*4; uint8_t b[4];
                        if(bgra){ b[0]=px[0];b[1]=px[1];b[2]=px[2]; } else { b[0]=px[2];b[1]=px[1];b[2]=px[0]; } b[3]=255; fwrite(b,1,4,fp);} }
                fclose(fp); char mm[200]; int kk=snprintf(mm,sizeof mm,"  [tex] dumped -> %s\n",p); (void)!write(2,mm,kk); } }
    }
    D3DObj* tx=mktex(w,h,arr,f,2);
    // COPY pInitialData into the texture. halo3 creates map textures (diffuse/control/detail/normal)
    // WITH their pixel data in pInitialData -- without this they stay the zeroed mktex buffer (all
    // black), so every sampled texture returns 0 and gameplay renders black. init = D3D11_SUBRESOURCE_DATA*
    // {pSysMem, SysMemPitch, SysMemSlicePitch}; copy mip 0 (what d3dgl uploads). For BC/DXT the row count
    // is ceil(h/4) (4-row blocks); otherwise h. Disable with PELOAD_NO_TEXINIT.
    if(init && tx && tx->data && tx->size && !getenv("PELOAD_NO_TEXINIT")){
        const void* sys=*(const void* const*)init; uint32_t pitch=*(const uint32_t*)((const char*)init+8);
        if(sys){ int bc=(f>=70&&f<=84)||(f>=94&&f<=99);
            // For BC use the TIGHT compressed mip-0 size (ceil(w/4)*ceil(h/4)*blockbytes) -- the source is
            // tightly packed and SysMemPitch has been seen 4x short, truncating the copy. Uncompressed uses pitch*h.
            uint64_t sz; if(bc){ int blk=((f>=70&&f<=72)||(f>=79&&f<=81))?8:16; sz=(uint64_t)((w+3)/4)*((h+3)/4)*blk; }
            else sz=(uint64_t)pitch*h;
            if(sz>tx->size)sz=tx->size; memcpy(tx->data,sys,(size_t)sz); tx->gl_dirty=1;
            if(getenv("PELOAD_TRACE_TEXINIT")){ static int ti; if(ti++<20){ char m[160]; int k=snprintf(m,sizeof m,"  [texinit] %ux%u f%u pitch=%u copy=%lluB tsize=%lluB\n",w,h,f,pitch,(unsigned long long)sz,(unsigned long long)tx->size); (void)!write(2,m,k);} } } }
    if(pp)*pp=tx; return HR_OK; }
static int MSABI dev_CreateTexture3D(void*t,const void*d,const void*init,void**pp){ (void)t;(void)init;
    uint32_t w=d?*(const uint32_t*)d:1, h=d?*(const uint32_t*)((const char*)d+4):1, dp=d?*(const uint32_t*)((const char*)d+8):1, f=d?*(const uint32_t*)((const char*)d+16):0;
    if(pp)*pp=mktex(w,h,dp,f,4); return HR_OK; }
static int MSABI dev_crView3(void*t,const void*res,const void*desc,void**pp){ (void)t;(void)desc; D3DObj*o=d3dobj(g_d3dview_vtbl,0,0); o->data=(void*)res; if(pp)*pp=o; return HR_OK; }
static int MSABI dev_cr2(void*t,const void*a,void**pp){ (void)t;(void)a; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; }      // (desc,pp) out=r8
static int MSABI dev_cr1(void*t,void**pp){ (void)t; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; }                         // (pp) out=rdx
// Shader creation: RETAIN the DXBC bytecode (for GL translation) and tag the stage.
// Per-stage wrappers so we know VS vs PS etc. PELOAD_DUMP_SHADERS writes each blob to
// $HOME/reclaimer_shaders/<stage><idx>.dxbc for offline workload analysis.
static const char* const g_stage_name[6]={"vs","ps","gs","hs","ds","cs"};
static int g_shcount[6];
// In-process DXBC->GLSL translator (dg_translate). A byte-for-byte C port of re-tools/dxbc2glsl.py
// (verified identical over the 2833-shader corpus) so the shim does its OWN shader translation at
// runtime -- no Python subprocess, no offline pre-pass. Python stays strictly a test oracle.
#include "dxbc2glsl.c"
// FNV-1a 64 over the DXBC bytes (matches the cache key) -> ~/reclaimer_glsl/<hash>.glsl.
static uint64_t d3dgl_fnv(const void*b,uint64_t n){ const uint8_t*p=b; uint64_t h=0xcbf29ce484222325ULL;
    for(uint64_t i=0;i<n;i++){ h^=p[i]; h*=0x100000001b3ULL; } return h; }
static char* d3dgl_load_glsl(const void*bc,uint64_t len){
    const char*home=getenv("HOME"); char path[512];
    uint64_t hash=d3dgl_fnv(bc,len);
    snprintf(path,sizeof path,"%s/reclaimer_glsl/%016llx.glsl",home?home:"/tmp",(unsigned long long)hash);
    FILE*f=fopen(path,"rb");
    if(f){ fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if(sz<=0||sz>(4<<20)){ fclose(f); return 0; } char*s=malloc(sz+1); if(!s){ fclose(f); return 0; }
        if(fread(s,1,sz,f)!=(size_t)sz){ free(s); fclose(f); return 0; } s[sz]=0; fclose(f); return s; }
    // Cache miss: translate IN-PROCESS now (the program's own routine) and cache it, so ANY shader the
    // game uses gets GLSL at first use with no Python and no "miss -> skipped draw -> black" gap.
    if(!(bc && len>=4 && !memcmp(bc,"DXBC",4)) || getenv("PELOAD_D3DGL_NOXLAT")) return 0;
    char*g=dg_translate((const uint8_t*)bc,(size_t)len);
    if(!g) return 0;
    char gdir[400]; snprintf(gdir,sizeof gdir,"%s/reclaimer_glsl",home?home:"/tmp"); mkdir(gdir,0755);
    FILE*o=fopen(path,"wb"); if(o){ fwrite(g,1,strlen(g),o); fclose(o); }
    if(getenv("PELOAD_TRACE_D3DGL")){ char m[96]; int k=snprintf(m,sizeof m,"  [d3dgl] translated %016llx in-process (%zuB)\n",(unsigned long long)hash,strlen(g)); (void)!write(2,m,k); }
    return g;
}
// Does a VS DXBC declare a classic vertex input (a non-system semantic in ISGN)? If so it needs a
// bound vertex buffer; if ISGN is empty / all SV_* it is a vertex-pulling / fullscreen shader.
static int dxbc_has_vtxinput(const uint8_t*d,uint64_t n){
    if(!d||n<36||memcmp(d,"DXBC",4)) return 0;
    uint32_t nch=*(const uint32_t*)(d+28); if((uint64_t)32+4*(uint64_t)nch>n) return 0;
    for(uint32_t i=0;i<nch;i++){ uint32_t co=*(const uint32_t*)(d+32+4*i); if((uint64_t)co+8>n) continue;
        if(memcmp(d+co,"ISGN",4)) continue;
        const uint8_t*cd=d+co+8; uint64_t crem=n-(co+8); if(crem<8) return 0;
        uint32_t ec=*(const uint32_t*)cd; if((uint64_t)8+24*(uint64_t)ec>crem) return 0;
        for(uint32_t e=0;e<ec;e++){ const uint8_t*el=cd+8+24*e; uint32_t no=*(const uint32_t*)el;
            if((uint64_t)no>=crem) continue; const char*nm=(const char*)cd+no;
            uint64_t maxl=crem-no; size_t sl=strnlen(nm,maxl<64?maxl:64);
            if(sl>=1 && !(sl>=3 && nm[0]=='S'&&nm[1]=='V'&&nm[2]=='_')) return 1;  // a real vertex attribute
        }
        return 0;
    }
    return 0;
}
static int MSABI dev_crShader(int stage,const void*bc,uint64_t len,void**pp){
    D3DObj*o=d3dobj(g_d3dchild_vtbl,0,0); o->stype=stage;
    if(bc&&len&&len<(16<<20)){ o->bc=malloc(len); if(o->bc){ memcpy(o->bc,bc,len); o->bclen=(uint32_t)len; } }
    if(stage==0 && o->bc) o->vs_vtxin=dxbc_has_vtxinput(o->bc,o->bclen);
    if(getenv("PELOAD_D3DGL") && o->bc){ o->glsl=d3dgl_load_glsl(o->bc,o->bclen);
        if(getenv("PELOAD_TRACE_D3DGL")){ static int lo,hi; if(o->glsl)hi++; else lo++; if(((lo+hi)%200)==0){ char m[64]; int k=snprintf(m,sizeof m,"  [d3dgl] shaders translated=%d miss=%d\n",hi,lo); (void)!write(2,m,k);} } }
    int idx=g_shcount[stage]++;
    if(getenv("PELOAD_DUMP_SHADERS") && o->bc){ char d[512]; const char*h=getenv("HOME");
        snprintf(d,sizeof d,"%s/reclaimer_shaders",h?h:"/tmp"); mkdir(d,0755);
        char p[640]; snprintf(p,sizeof p,"%s/%s%03d.dxbc",d,g_stage_name[stage],idx);
        FILE*f=fopen(p,"wb"); if(f){ fwrite(o->bc,1,o->bclen,f); fclose(f); } }
    if(getenv("PELOAD_TRACE_SHADER")){ char m[160]; const uint8_t*b=bc;
        int k=snprintf(m,sizeof m,"  [shader] %s#%d len=%llu magic=%c%c%c%c\n",g_stage_name[stage],idx,
            (unsigned long long)len,b&&len>=4?b[0]:'?',b&&len>=4?b[1]:'?',b&&len>=4?b[2]:'?',b&&len>=4?b[3]:'?'); (void)!write(2,m,k); }
    if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crVS(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)lk; return dev_crShader(0,bc,len,pp); }
static int MSABI dev_crPS(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)lk; return dev_crShader(1,bc,len,pp); }
static int MSABI dev_crGS(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)lk; return dev_crShader(2,bc,len,pp); }
static int MSABI dev_crHS(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)lk; return dev_crShader(3,bc,len,pp); }
static int MSABI dev_crDS(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)lk; return dev_crShader(4,bc,len,pp); }
static int MSABI dev_crCS(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)lk; return dev_crShader(5,bc,len,pp); }
// GSWithStreamOutput has a different (longer) signature; just retain as GS.
static int MSABI dev_crGSSO(void*t,const void*bc,uint64_t len,void*a,uint64_t b,void*c,uint64_t d,uint32_t e,void*lk,void**pp){
    (void)t;(void)a;(void)b;(void)c;(void)d;(void)e;(void)lk; return dev_crShader(2,bc,len,pp); }
static int MSABI dev_cr5(void*t,const void*bc,uint64_t len,void*lk,void**pp){ (void)t;(void)bc;(void)len;(void)lk; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; } // shaders (generic fallback)
static uint32_t dxgi_vtx_bytes(uint32_t f){ // byte size of a per-vertex attribute format
    switch(f){ case 1:case 2:case 3:case 4: return 16;                 // R32G32B32A32_*
        case 5:case 6:case 7:case 8: return 12;                        // R32G32B32_*
        case 9:case 10:case 11:case 12:case 13:case 14: return 8;      // R16G16B16A16_*
        case 15:case 16:case 17:case 18:case 19: return 8;             // R32G32_*
        case 20:case 21:case 22:case 23: return 4; /* R10G10B10A2 etc */
        case 24:case 25:case 26: return 4;                             // R10G10B10A2_*
        case 27:case 28:case 29:case 30:case 31:case 32: return 4;     // R8G8B8A8_*
        case 33:case 34:case 35:case 36:case 37:case 38: return 4;     // R16G16_*
        case 39:case 40:case 41:case 42:case 43: return 4;             // R32_*
        case 48:case 49:case 50:case 51:case 52:case 53:case 54:case 55:case 56:case 57: return 2; // R8G8_*,R16_*
        case 58:case 59:case 60:case 61:case 62:case 63:case 64:case 65: return 1; // R8_*
        default: return 4; } }
static int MSABI dev_crIL(void*t,const void*d,uint32_t n,const void*bc,uint64_t len,void**pp){ (void)t;(void)bc;(void)len; // CreateInputLayout
    D3DObj* o=d3dobj(g_d3dchild_vtbl,0,0);
    // D3D11_INPUT_ELEMENT_DESC (32B): SemanticName*@0, SemanticIndex@8, Format@12, InputSlot@16, AlignedByteOffset@20, ...
    if(d&&n&&o){ int m=n>16?16:(int)n; o->nelem=m; const uint8_t* e=d; uint32_t run[8]={0};
        for(int i=0;i<m;i++){ const uint8_t* el=e+(size_t)i*32; const char* sem=*(const char* const*)el;
            uint32_t fmt=*(const uint32_t*)(el+12), slot=*(const uint32_t*)(el+16), off=*(const uint32_t*)(el+20);
            if(slot>7) slot=0;
            if(off==0xFFFFFFFFu) off=run[slot];                        // D3D11_APPEND_ALIGNED_ELEMENT
            run[slot]=off+dxgi_vtx_bytes(fmt);
            o->elem[i].fmt=fmt; o->elem[i].slot=slot; o->elem[i].off=off;
            int j=0; if(sem) for(; sem[j]&&j<11;j++) o->elem[i].sem[j]=sem[j]; o->elem[i].sem[j]=0; } }
    if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crCtx(void*t,uint32_t fl,void**pp){ (void)t;(void)fl; if(pp)*pp=&g_d3dcontext; return HR_OK; }                 // CreateDeferredContext
static D3DObj* mktex(uint32_t w,uint32_t h,uint32_t arr,uint32_t fmt,uint32_t kind); // fwd
static int MSABI dev_OpenShared(void*t,void*h,GUID_*r,void**pp){ (void)t;(void)h;(void)r; if(pp)*pp=mktex(4096,4096,1,28,2); return HR_OK; }
static int MSABI dev_CheckFormat(void*t,uint32_t fmt,uint32_t*ps){ (void)t;(void)fmt; if(ps)*ps=0x1FFFFF; return HR_OK; }           // claim broad support
static int MSABI dev_CheckMSAA(void*t,uint32_t fmt,uint32_t cnt,uint32_t*pq){ (void)t;(void)fmt;(void)cnt; if(pq)*pq=1; return HR_OK; }
static void MSABI dev_CheckCounterInfo(void*t,void*pi){ (void)t; if(pi) memset(pi,0,12); }
static int MSABI dev_CheckFeature(void*t,uint32_t f,void*pd,uint32_t sz){ (void)t;(void)f; if(pd) memset(pd,0,sz); return HR_OK; }
static uint32_t MSABI dev_GetFeatureLevel(void*t){ (void)t; return 0xb000; }   // D3D_FEATURE_LEVEL_11_0
static uint32_t MSABI dev_GetCreationFlags(void*t){ (void)t; return 0; }
static int MSABI dev_GetRemovedReason(void*t){ (void)t; return HR_OK; }
static void MSABI dev_GetImmediateContext(void*t,void**pp){ (void)t; if(pp)*pp=&g_d3dcontext; }
static int MSABI dev_SetExceptionMode(void*t,uint32_t m){ (void)t;(void)m; return HR_OK; }
static uint32_t MSABI dev_GetExceptionMode(void*t){ (void)t; return 0; }
// ---- ID3D11DeviceContext: Map returns a real buffer; everything else no-op/0 ----
static int  MSABI ctx_ret0(void){ return 0; }                            // safe for void + HRESULT + UINT returns
static uint64_t g_cb_tick;           // advances each present; marks CB freshness for view-proj auto-detect
static int  MSABI ctx_Map(void*t,void*res,uint32_t sub,uint32_t type,uint32_t fl,void*pM){ (void)t;(void)sub;(void)type;(void)fl;
    D3DObj*o=res; if(o){ o->gl_dirty=1; if(o->is_cb) o->cb_upd=g_cb_tick; }   // mapped for write -> re-upload + mark CB fresh
    if(pM){ void* d=(o&&o->data)?o->data:g_d3dscratch;
        uint32_t rp = o? (o->rowpitch?o->rowpitch:(uint32_t)o->size) : (uint32_t)sizeof g_d3dscratch;   // bytes per row
        // DepthPitch = one 2D slice. Must NOT be the whole buffer: for 3D/array
        // textures the engine strides depth slices by DepthPitch (slice N at N*dp),
        // so an oversized dp writes far past our buffer and corrupts the heap.
        uint32_t dp = o? (uint32_t)(o->rowpitch? (uint64_t)o->rowpitch*(o->h?o->h:1) : o->size) : (uint32_t)sizeof g_d3dscratch;
        if(o && dp>o->size) dp=(uint32_t)o->size;
        *(void**)pM=d; *(uint32_t*)((char*)pM+8)=rp; *(uint32_t*)((char*)pM+12)=dp; } return HR_OK; }
static void MSABI ctx_GetDevice(void*t,void**pp){ (void)t; if(pp)*pp=&g_d3ddevice; }
// Context Get* methods (indices 72-109): if left as no-ops they leave the caller's
// out-param array UNINITIALIZED (stack garbage), and the engine then uses/releases
// a garbage pointer -> heap corruption (garbage varies => timing-dependent crash).
// Zero the out-params so the engine gets NULLs it can handle. Signatures below
// match the standard ID3D11DeviceContext vtable.
static void MSABI ctx_get_arr(void*t,uint32_t start,uint32_t num,void**pp){ (void)t;(void)start; if(pp) for(uint32_t i=0;i<num&&i<256;i++) pp[i]=0; }        // *Get{CB,SRV,Samplers}(start,num,ppArray)
static void MSABI ctx_get_shader(void*t,void**ppS,void**ppCI,uint32_t*pN){ (void)t; if(ppS)*ppS=0; if(pN)*pN=0; (void)ppCI; }                                 // *GetShader(ppShader,ppClassInst,pNum)
static void MSABI ctx_get_1(void*t,void**pp){ (void)t; if(pp)*pp=0; }                                                                                          // IAGetInputLayout(ppIL)
static void MSABI ctx_get_ib(void*t,void**ppIB,uint32_t*pFmt,uint32_t*pOff){ (void)t; if(ppIB)*ppIB=0; if(pFmt)*pFmt=0; if(pOff)*pOff=0; }                      // IAGetIndexBuffer
static void MSABI ctx_get_vb(void*t,uint32_t start,uint32_t num,void**ppVB,uint32_t*pStr,uint32_t*pOff){ (void)t;(void)start; if(ppVB)for(uint32_t i=0;i<num&&i<256;i++)ppVB[i]=0; if(pStr)for(uint32_t i=0;i<num&&i<256;i++)pStr[i]=0; if(pOff)for(uint32_t i=0;i<num&&i<256;i++)pOff[i]=0; } // IAGetVertexBuffers
static void MSABI ctx_om_get(void*t,uint32_t num,void**ppRTV,void**ppDSV){ (void)t; if(ppRTV)for(uint32_t i=0;i<num&&i<256;i++)ppRTV[i]=0; if(ppDSV)*ppDSV=0; } // OMGetRenderTargets
// ---- IDXGISwapChain ----
// --- D3D11 call tracing (PELOAD_TRACE_D3D): counts the hot render methods so we
// can tell whether halo3 actually DRAWS via D3D11 (=> needs real D3D11->GL) or
// only creates resources while the GL frontend does the drawing. ---
static uint64_t g_d3dcnt[8]; // 0 Draw,1 DrawIndexed,2 DrawInstanced,3 DrawIdxInst,4 Present,5 CreateTex2D,6 CreateBuffer,7 Dispatch
static void d3dtrace(int i,const char*nm){ if(!getenv("PELOAD_TRACE_D3D")) return; if((g_d3dcnt[i]++ %200)==0){ char b[80]; int k=snprintf(b,sizeof b,"  [d3d] %s #%llu\n",nm,(unsigned long long)g_d3dcnt[i]); (void)!write(2,b,k);} }
// A fullscreen-quad draw (4-vert tri-strip, no VB) sampling a >=640x360 texture is the
// engine presenting its composited frame. Capture that texture as the frame to show.
static void d3d_capture_frame(uint32_t n){ if(n>6) return; D3DObj* tx=g_d3dst.psrv[0];
    // psrv[0] is a BOUND INPUT texture; in-game it's usually a square shadow map / atlas
    // (1024x1024), not the final frame. Only latch a window-aspect (16:9-ish, wide) texture so
    // intermediates can't hijack the presented frame with left-crushed garbage.
    if(tx&&tx->data&&tx->w>=1152&&tx->h>=360){ double ar=(double)tx->w/tx->h; if(ar>=1.6&&ar<=1.9){
        g_d3d_frame_bits=tx->data; g_d3d_frame_w=(int)tx->w; g_d3d_frame_h=(int)tx->h; g_d3d_frame_pitch=tx->rowpitch?(int)tx->rowpitch:(int)tx->w*4; } } }
// ---- PELOAD_D3DGL live draw-path executor (defined later, near the GL/cocoa code) ----
static int g_d3dgl_en=-1;
static volatile const char* g_d3dgl_phase="(none)"; // last d3dgl step reached (for crash diagnosis)
static volatile unsigned long g_d3dgl_phase_draw;    // which draw # we were on
static int d3dgl_enabled(void){ if(g_d3dgl_en<0) g_d3dgl_en=getenv("PELOAD_D3DGL")?1:0; return g_d3dgl_en; }
static void d3dgl_draw(uint32_t vcount,int indexed,uint32_t instcount); // fwd
static void d3dgl_present(void); // fwd (blit the D3DGL backbuffer to the window at Present)
static void d3dgl_clear_rtv(void* rtvview,const float* col); // fwd (ClearRenderTargetView -> GL clear)
static void d3dgl_clear_dsv(void); // fwd (ClearDepthStencilView -> GL depth clear)
static void MSABI ctx_ClearRTV(void*t,void*rtv,const float*col){ (void)t; if(d3dgl_enabled()) d3dgl_clear_rtv(rtv,col); }
static void MSABI ctx_ClearDSV(void*t,void*dsv,uint32_t fl,float d,uint8_t s){ (void)t;(void)dsv;(void)fl;(void)d;(void)s; if(d3dgl_enabled()) d3dgl_clear_dsv(); }
// PELOAD_DRAW_CALLER: log the first few distinct halo3 return addresses that call each draw type,
// so we can decompile the actual geometry-submit function and find why it binds no input layout.
static void draw_caller(const char*tag,void*ra,uint32_t vcount){ if(!getenv("PELOAD_DRAW_CALLER")) return;
    static uint64_t seen[64]; static int ns; uint64_t v=(uint64_t)ra;
    for(int i=0;i<ns;i++) if(seen[i]==v) return; if(ns<64) seen[ns++]=v;
    const char*mod="?"; uint64_t off=v; if(v>=0x180000000ULL&&v<0x187000000ULL){mod="halo3";off=v-0x180000000ULL;} else if(v>=0x140000000ULL&&v<0x14c000000ULL){mod="exe";off=v-0x140000000ULL;}
    char m[128]; int k=snprintf(m,sizeof m,"  [draw-caller] %s ra=%s+0x%llx n=%u vb0=%lluB stride=%u classicVS=%d il=%p\n",tag,mod,(unsigned long long)off,vcount,
        g_d3dst.vb[0]?(unsigned long long)g_d3dst.vb[0]->size:0,g_d3dst.vstride[0], (g_d3dst.vs?g_d3dst.vs->vs_vtxin:-1), (void*)g_d3dst.il); (void)!write(2,m,k); }
static void MSABI ctx_DrawIndexed(void*t,uint32_t n,uint32_t s,int b){ (void)t;(void)s;(void)b; d3dtrace(1,"DrawIndexed"); draw_caller("DrawIndexed",__builtin_return_address(0),n); draw_log("drawI",n); d3d_capture_frame(n); if(d3dgl_enabled()) d3dgl_draw(n,1,1); }
static void MSABI ctx_Draw(void*t,uint32_t n,uint32_t s){ (void)t;(void)s; d3dtrace(0,"Draw"); draw_caller("Draw",__builtin_return_address(0),n); draw_log("draw",n); d3d_capture_frame(n); if(d3dgl_enabled()) d3dgl_draw(n,0,1); }
static void MSABI ctx_UpdateSubresource(void*t,void*res,uint32_t sub,const void*box,const void*src,uint32_t srp,uint32_t sdp){
    (void)t;(void)sub;(void)sdp; D3DObj*o=res; if(!o||!o->data||!src) return;
    o->gl_dirty=1; if(o->is_cb) o->cb_upd=g_cb_tick;
    // MUST honor pDstBox: a partial (box) update supplies a source sized only for the box. Reading the
    // full-resource height instead overreads past the source end -> SIGSEGV in memmove. D3D11_BOX =
    // {Left,Top,Front,Right,Bottom,Back}; for a BUFFER Left/Right are byte offsets, rows=1.
    const uint32_t*b=box; const uint8_t*s=src; uint8_t*d=o->data; uint64_t cap=o->size;
    if(o->kind==1){                                   // BUFFER
        uint64_t off=b?b[0]:0, n=b?(uint64_t)(b[3]-b[0]):cap;
        if(off<cap){ if(off+n>cap)n=cap-off; memcpy(d+off,s,(size_t)n); } return; }
    // Block-compressed (BC/DXT) textures stream in via UpdateSubresource too, but they have NO per-pixel
    // bpp -- rows are 4-high blocks and the pitch is (blocks_per_row * blockbytes). Treating them like
    // uncompressed (bpp=rowpitch/w) scrambled/truncated the copy, so map textures only partly filled.
    { int bc=(o->fmt>=70&&o->fmt<=84)||(o->fmt>=94&&o->fmt<=99);
      if(bc){ int blk=((o->fmt>=70&&o->fmt<=72)||(o->fmt>=79&&o->fmt<=81))?8:16;
        uint32_t bw=(((o->w?o->w:1)+3)/4), drp2=bw*blk, sr=srp?srp:drp2;
        uint32_t ltop=b?b[1]:0, lbot=b?b[4]:(o->h?o->h:1), lleft=b?b[0]:0, lright=b?b[3]:(o->w?o->w:1);
        uint32_t btop=ltop/4, bbot=(lbot+3)/4, bleft=lleft/4, brbytes=((((lright+3)/4)-bleft)*(uint32_t)blk);
        for(uint32_t by=btop;by<bbot;by++){ uint64_t doff=(uint64_t)by*drp2+(uint64_t)bleft*blk, soff=(uint64_t)(by-btop)*sr;
            if(doff+brbytes>cap) break; memcpy(d+doff,s+soff,brbytes); }
        return; } }
    uint32_t bpp=(o->rowpitch&&o->w)?(uint32_t)(o->rowpitch/o->w):4; if(!bpp)bpp=4;
    uint32_t drp=o->rowpitch?o->rowpitch:(o->w?o->w*bpp:0); if(!drp) return;
    uint32_t left=b?b[0]:0, top=b?b[1]:0, right=b?b[3]:o->w, bottom=b?b[4]:(o->h?o->h:1);
    if(right<=left||bottom<=top) return;
    uint32_t rowbytes=(right-left)*bpp; uint32_t sr=srp?srp:rowbytes;
    for(uint32_t y=top;y<bottom;y++){ uint64_t doff=(uint64_t)y*drp+(uint64_t)left*bpp, soff=(uint64_t)(y-top)*sr;
        if(doff+rowbytes>cap) break; memcpy(d+doff,s+soff,rowbytes); } }
static void MSABI ctx_CopyResource(void*t,void*dst,void*src){ (void)t; D3DObj*a=dst,*b=src; if(a&&b&&a->data&&b->data){ uint64_t n=a->size<b->size?a->size:b->size; memcpy(a->data,b->data,n); if(a)a->gl_dirty=1; } }
// CopySubresourceRegion(dst,dstSub,dstX,dstY,dstZ,src,srcSub,pSrcBox). Was a no-op -> 12k/frame copies
// (texture staging / composite steps) were dropped. Implement the CPU copy (full or box region).
static void MSABI ctx_CopySubresourceRegion(void*t,void*dstR,uint32_t dstSub,uint32_t dstX,uint32_t dstY,uint32_t dstZ,void*srcR,uint32_t srcSub,const void*srcBox){
    (void)t;(void)dstSub;(void)srcSub;(void)dstZ; D3DObj*dst=dstR,*src=srcR; if(!dst||!src||!dst->data||!src->data) return;
    dst->gl_dirty=1;
    if(!srcBox && dstX==0 && dstY==0){ uint64_t n=dst->size<src->size?dst->size:src->size; memcpy(dst->data,src->data,n); return; }
    const uint32_t*bx=srcBox;                                  // D3D11_BOX = {left,top,front,right,bottom,back}
    uint32_t left=bx?bx[0]:0, top=bx?bx[1]:0, right=bx?bx[3]:src->w, bottom=bx?bx[4]:src->h;
    if(right<=left||bottom<=top) return;
    uint32_t sbpp = (src->rowpitch&&src->w)?(uint32_t)(src->rowpitch/src->w):4; if(!sbpp)sbpp=4;
    uint32_t srp = src->rowpitch?src->rowpitch:src->w*sbpp;
    uint32_t drp = dst->rowpitch?dst->rowpitch:dst->w*sbpp;
    uint64_t rw=(uint64_t)(right-left)*sbpp;
    for(uint32_t y=top;y<bottom;y++){ uint32_t dy=dstY+(y-top);
        uint64_t soff=(uint64_t)y*srp+(uint64_t)left*sbpp, doff=(uint64_t)dy*drp+(uint64_t)dstX*sbpp;
        if(soff+rw<=src->size && doff+rw<=dst->size) memcpy((char*)dst->data+doff,(char*)src->data+soff,rw); } }
static void draw_census(void); // fwd
static void MSABI ctx_DrawIndexedInstanced(void*t,uint32_t a,uint32_t b,uint32_t c,int d,uint32_t e){ (void)t;(void)c;(void)d;(void)e; d3dtrace(3,"DrawIndexedInstanced"); draw_caller("DrawIdxInst",__builtin_return_address(0),a); draw_census(); if(d3dgl_enabled()) d3dgl_draw(a,1,b?b:1); }
static void MSABI ctx_DrawInstanced(void*t,uint32_t a,uint32_t b,uint32_t c,uint32_t d){ (void)t;(void)c;(void)d; d3dtrace(2,"DrawInstanced"); draw_caller("DrawInst",__builtin_return_address(0),a); draw_census(); if(d3dgl_enabled()) d3dgl_draw(a,0,b?b:1); }
static void MSABI ctx_Dispatch(void*t,uint32_t a,uint32_t b,uint32_t c){ (void)t;(void)a;(void)b;(void)c; d3dtrace(7,"Dispatch"); }
// ---- pipeline-state recorders (hooked into the context vtable) ----
static void iacen_log(void){ if(!getenv("PELOAD_IA_CENSUS")) return; static uint64_t last;
    uint64_t tot=g_iacen.vs_set; if(tot<last+20000) return; last=tot;
    char m[200]; int k=snprintf(m,sizeof m,"  [ia-census] IASetVB=%llu IASetIL=%llu VSSetSRV=%llu | VSSet classic=%llu pull=%llu total=%llu\n",
        (unsigned long long)g_iacen.ia_vb,(unsigned long long)g_iacen.ia_il,(unsigned long long)g_iacen.vs_srv,
        (unsigned long long)g_iacen.vs_classic,(unsigned long long)g_iacen.vs_pull,(unsigned long long)g_iacen.vs_set); (void)!write(2,m,k); }
static void MSABI ctx_IASetInputLayout(void*t,void*il){ (void)t; g_d3dst.il=il; if(il)g_iacen.ia_il++; }
static void MSABI ctx_IASetVertexBuffers(void*t,uint32_t start,uint32_t num,void*const*vbs,const uint32_t*str,const uint32_t*off){ (void)t;
    for(uint32_t i=0;i<num&&start+i<8;i++){ g_d3dst.vb[start+i]=vbs?(D3DObj*)vbs[i]:0; g_d3dst.vstride[start+i]=str?str[i]:0; g_d3dst.voff[start+i]=off?off[i]:0; if(vbs&&vbs[i])g_iacen.ia_vb++; } }
static void MSABI ctx_VSSetShaderResources(void*t,uint32_t start,uint32_t num,void*const*srvs){ (void)t;
    for(uint32_t i=0;i<num&&start+i<16;i++){ D3DObj*v=srvs?(D3DObj*)srvs[i]:0; g_d3dst.vssrv[start+i]=(v&&v->data)?(D3DObj*)v->data:v; if(srvs&&srvs[i])g_iacen.vs_srv++; } }
static void MSABI ctx_IASetIndexBuffer(void*t,void*ib,uint32_t fmt,uint32_t off){ (void)t; g_d3dst.ib=(D3DObj*)ib; g_d3dst.ibfmt=fmt; g_d3dst.iboff=off; }
static void MSABI ctx_IASetPrimitiveTopology(void*t,uint32_t topo){ (void)t; g_d3dst.topo=topo; }
static void MSABI ctx_VSSetShader(void*t,void*sh,void*ci,uint32_t nci){ (void)t;(void)ci;(void)nci; g_d3dst.vs=(D3DObj*)sh;
    if(sh){ D3DObj*o=sh; g_iacen.vs_set++; if(o->vs_vtxin)g_iacen.vs_classic++; else g_iacen.vs_pull++; iacen_log(); } }
static void MSABI ctx_PSSetShader(void*t,void*sh,void*ci,uint32_t nci){ (void)t;(void)ci;(void)nci; g_d3dst.ps=(D3DObj*)sh; }
static uint64_t g_cbcnt[4]; // VSSetCB, VSSetCB1, PSSetCB, PSSetCB1 call counters (PELOAD_TRACE_CB)
static const char* guest_sym(uint64_t v,char*buf,size_t n){
    if(v>=0x140000000ULL&&v<0x142000000ULL) snprintf(buf,n,"exe+0x%llx",(unsigned long long)(v-0x140000000ULL));
    else if(v>=0x180000000ULL&&v<0x187000000ULL) snprintf(buf,n,"halo3+0x%llx",(unsigned long long)(v-0x180000000ULL));
    else snprintf(buf,n,"%#llx",(unsigned long long)v);
    return buf; }
static void cb_trace2(int w,uint32_t start,uint32_t num,void*const*cbs,void*ra){ if(!getenv("PELOAD_TRACE_CB"))return; g_cbcnt[w]++;
    const char*nm[4]={"VSSetCB","VSSetCB1","PSSetCB","PSSetCB1"}; D3DObj*c0=(num&&cbs)?(D3DObj*)cbs[0]:0;
    // Log each UNIQUE (stage,start,num,caller) combo once (unbounded) so rare start=0 global binds + their call site are never missed.
    static uint64_t seen[512]; static int nseen; uint64_t key=((uint64_t)w<<60)|((uint64_t)(start&0x3fff)<<46)|((uint64_t)(num&0x3fff)<<32)|((uint64_t)(uintptr_t)ra & 0xffffffff);
    int fresh=1; for(int s=0;s<nseen;s++) if(seen[s]==key){ fresh=0; break; }
    if(fresh && nseen<512){ seen[nseen++]=key; char sym[48]; guest_sym((uint64_t)(uintptr_t)ra,sym,sizeof sym);
        char m[200]; int k=snprintf(m,sizeof m,"  [cb] %s start=%u num=%u cb0sz=%lluB cb1sz=%lluB  <- %s\n",nm[w],start,num,
            (unsigned long long)(c0?c0->size:0),(unsigned long long)((num>1&&cbs&&cbs[1])?((D3DObj*)cbs[1])->size:0),sym); (void)!write(2,m,k); } }
D3DObj* g_last_b12;  // most recent buffer bound to VS slot 12 (per-object world) — for cb-source diagnosis
float g_b12_org[32][3]; int g_nb12_org;   // recent distinct per-object world origins (for view-proj convention search)
static void MSABI ctx_VSSetConstantBuffers(void*t,uint32_t start,uint32_t num,void*const*cbs){ (void)t; cb_trace2(0,start,num,cbs,__builtin_return_address(0));
    for(uint32_t i=0;i<num&&start+i<16;i++){ g_d3dst.vscb[start+i]=cbs?(D3DObj*)cbs[i]:0; g_d3dst.vscb_1st[start+i]=0; g_d3dst.vscb_cnt[start+i]=0;
        if(start+i==12 && cbs && cbs[i]){ D3DObj*o=(D3DObj*)cbs[i]; g_last_b12=o;
            if(o->data && o->size>=48){ const float*f=(const float*)o->data; float x=f[3],y=f[7],z=f[11];
                if((x==x)&&(y==y)&&(z==z)&&(x||y||z) && g_nb12_org<32){ int dup=0; for(int k=0;k<g_nb12_org;k++){ float*p=g_b12_org[k]; float dx=p[0]-x,dy=p[1]-y,dz=p[2]-z; if((dx<0?-dx:dx)<0.5f&&(dy<0?-dy:dy)<0.5f&&(dz<0?-dz:dz)<0.5f){dup=1;break;} }
                    if(!dup){ g_b12_org[g_nb12_org][0]=x; g_b12_org[g_nb12_org][1]=y; g_b12_org[g_nb12_org][2]=z; g_nb12_org++; } } } } } }
static void MSABI ctx_PSSetConstantBuffers(void*t,uint32_t start,uint32_t num,void*const*cbs){ (void)t; cb_trace2(2,start,num,cbs,__builtin_return_address(0));
    for(uint32_t i=0;i<num&&start+i<16;i++){ g_d3dst.pscb[start+i]=cbs?(D3DObj*)cbs[i]:0; g_d3dst.pscb_1st[start+i]=0; g_d3dst.pscb_cnt[start+i]=0; } }
// ID3D11DeviceContext1::*SetConstantBuffers1 — bind a RANGE (FirstConstant/NumConstants, in 16-byte
// units) of a shared constant buffer. halo3 uses THESE (not the plain versions) for its matrices, so
// without hooking them the VS gets no cb0/cb1 -> gl_Position computes against zeros -> black geometry.
static void MSABI ctx_VSSetConstantBuffers1(void*t,uint32_t start,uint32_t num,void*const*cbs,const uint32_t*first,const uint32_t*cnt){ (void)t; cb_trace2(1,start,num,cbs,__builtin_return_address(0));
    for(uint32_t i=0;i<num&&start+i<16;i++){ g_d3dst.vscb[start+i]=cbs?(D3DObj*)cbs[i]:0;
        g_d3dst.vscb_1st[start+i]=first?first[i]:0; g_d3dst.vscb_cnt[start+i]=cnt?cnt[i]:0; } }
static void MSABI ctx_PSSetConstantBuffers1(void*t,uint32_t start,uint32_t num,void*const*cbs,const uint32_t*first,const uint32_t*cnt){ (void)t; cb_trace2(3,start,num,cbs,__builtin_return_address(0));
    for(uint32_t i=0;i<num&&start+i<16;i++){ g_d3dst.pscb[start+i]=cbs?(D3DObj*)cbs[i]:0;
        g_d3dst.pscb_1st[start+i]=first?first[i]:0; g_d3dst.pscb_cnt[start+i]=cnt?cnt[i]:0; } }
static void MSABI ctx_PSSetShaderResources(void*t,uint32_t start,uint32_t num,void*const*srvs){ (void)t;
    for(uint32_t i=0;i<num&&start+i<32;i++){ D3DObj*v=srvs?(D3DObj*)srvs[i]:0; g_d3dst.psrv[start+i]=(v&&v->data)?(D3DObj*)v->data:v; } }
static void MSABI ctx_OMSetRenderTargets(void*t,uint32_t num,void*const*rtvs,void*dsv){ (void)t;
    g_d3dst.num_rt=0;
    for(uint32_t i=0;i<num&&i<8;i++){ D3DObj*v=rtvs?(D3DObj*)rtvs[i]:0; D3DObj*res=(v&&v->data)?(D3DObj*)v->data:v; g_d3dst.rtvs[i]=res; if(res)g_d3dst.num_rt=i+1; }
    for(uint32_t i=num;i<8;i++) g_d3dst.rtvs[i]=0;
    g_d3dst.rtv=g_d3dst.rtvs[0];
    { D3DObj*d=(D3DObj*)dsv; g_d3dst.dsv=(d&&d->data)?(D3DObj*)d->data:d; } }
static void MSABI ctx_RSSetViewports(void*t,uint32_t num,const void*vps){ (void)t; if(num&&vps){ const float*f=vps; g_d3dst.vp_w=(int)f[2]; g_d3dst.vp_h=(int)f[3]; } }
static const char* dxgi_fmt_name(uint32_t f){ switch(f){ case 2:return"RGBA32F";case 6:return"RGBA32U";case 16:return"RGBA16F";case 28:return"RGBA8";case 87:return"BGRA8";case 41:return"R32F";case 42:return"R32U";case 49:return"R8";case 57:return"R8U";default:return"?";} }
// Draw census (PELOAD_DRAW_CENSUS): tally fullscreen (no VB) vs geometry (VB-backed) draws and
// the distinct input-layout signatures, so we know whether this worker renders the 3D world.
static void slotcen_dump(void);
static void draw_census(void){ slotcen_dump(); if(!getenv("PELOAD_DRAW_CENSUS")) return;
    static uint64_t fs, geo, geo_vcapture; static int reported;
    D3DObj*vb=g_d3dst.vb[0]; D3DObj*il=g_d3dst.il;
    if(vb && vb->size && il && il->nelem) { geo++; if(vb->data) geo_vcapture++; } else fs++;
    if(((fs+geo)%500)==0 && reported!=(int)(fs+geo)){ reported=(int)(fs+geo);
        char m[128]; int k=snprintf(m,sizeof m,"  [census] fullscreen=%llu geometry=%llu (with-vdata=%llu)\n",
            (unsigned long long)fs,(unsigned long long)geo,(unsigned long long)geo_vcapture); (void)!write(2,m,k); } }
static void draw_log(const char*tag,uint32_t count){ draw_census(); if(!getenv("PELOAD_TRACE_DRAW")) return;
    D3DObj*il0=g_d3dst.il,*rt0=g_d3dst.rtv;
    // PELOAD_TRACE_DRAW_RTW=<w>: only log draws whose render target has this width (e.g. 1920 = the
    // scene G-buffer, skipping the thousands of 4096 backbuffer/UI draws). Also dump psrv slots 0..31.
    const char*rtw=getenv("PELOAD_TRACE_DRAW_RTW");
    if(rtw && (!rt0 || rt0->w!=(uint32_t)atoi(rtw))) return;
    // PELOAD_D3DGL_DUMPSHADER: dump the bound VS/PS GLSL for scene draws that bind >=6 textures (the
    // map MATERIAL shaders, not the texture-less dither/discard passes) so we can read the material math.
    if(rtw && getenv("PELOAD_D3DGL_DUMPSHADER")){ static int ds; if(ds<4){ int nt=0; for(int r=0;r<32;r++) if(g_d3dst.psrv[r]) nt++;
        D3DObj*vs=g_d3dst.vs,*ps=g_d3dst.ps;
        if(nt>=6 && vs&&vs->glsl&&ps&&ps->glsl){ char p[64]; int k=snprintf(p,sizeof p,"/tmp/scene_ps_%d.glsl",ds); FILE*f=fopen(p,"wb"); if(f){fwrite(ps->glsl,1,strlen(ps->glsl),f);fclose(f);}
            k=snprintf(p,sizeof p,"/tmp/scene_vs_%d.glsl",ds); f=fopen(p,"wb"); if(f){fwrite(vs->glsl,1,strlen(vs->glsl),f);fclose(f);} ds++;
            D3DObj*c13p=g_d3dst.pscb[13],*c13v=g_d3dst.vscb[13],*c12=g_d3dst.vscb[12]; char cm[300]; int ck=snprintf(cm,sizeof cm,"  [cbdata] pscb13=%p(%lluB) vscb13=%p(%lluB) vscb12=%p(%lluB)",(void*)c13p,c13p?(unsigned long long)c13p->size:0,(void*)c13v,c13v?(unsigned long long)c13v->size:0,(void*)c12,c12?(unsigned long long)c12->size:0);
            if(c13p&&c13p->data&&c13p->size>=16){ const float*fp=(const float*)c13p->data; ck+=snprintf(cm+ck,sizeof cm-ck," pscb13[0..7]=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",fp[0],fp[1],fp[2],fp[3],fp[4],fp[5],fp[6],fp[7]); }
            ck+=snprintf(cm+ck,sizeof cm-ck,"\n"); (void)!write(2,cm,ck);
            // sample the bound textures' raw data: how many nonzero bytes (black vs has-content) + the
            // GL-uploaded flag + format, for slots 0..8 (the terrain blend/detail textures)
            char tm[400]; int tk2=snprintf(tm,sizeof tm,"  [texdata]"); for(int r=0;r<9;r++){ D3DObj*t=g_d3dst.psrv[r];
                if(!t){ tk2+=snprintf(tm+tk2,sizeof tm-tk2," t%d=-",r); continue; }
                int nz=0; if(t->data&&t->size>=1024){ const uint8_t*d=t->data; uint64_t step=t->size/2048; if(step<1)step=1; for(uint64_t i=0;i<t->size&&nz<40;i+=step) if(d[i])nz++; }
                tk2+=snprintf(tm+tk2,sizeof tm-tk2," t%d=%ux%u(f%u,%lluB,nz%d,gl%u)",r,t->w,t->h,t->fmt,(unsigned long long)t->size,nz,t->gltex); }
            tk2+=snprintf(tm+tk2,sizeof tm-tk2,"\n"); (void)!write(2,tm,tk2); } } }
    static int c; if(c++>=(rtw?400:200)) return;
    if(rtw){ int ntex=0; char tb[160]; int tk=0; for(int r=0;r<32;r++){ D3DObj*t=g_d3dst.psrv[r]; if(t){ntex++; if(tk<140)tk+=snprintf(tb+tk,sizeof tb-tk," t%d=%ux%u(f%u)",r,t->w,t->h,t->fmt);} } if(!ntex){tb[0]=' ';tb[1]='n';tb[2]='o';tb[3]='n';tb[4]='e';tb[5]=0;}
        char cbb[96]; int ck=snprintf(cbb,sizeof cbb," VScb:"); for(int b=0;b<16;b++) if(g_d3dst.vscb[b]) ck+=snprintf(cbb+ck,sizeof cbb-ck,"%d ",b); ck+=snprintf(cbb+ck,sizeof cbb-ck,"PScb:"); for(int b=0;b<16;b++) if(g_d3dst.pscb[b]) ck+=snprintf(cbb+ck,sizeof cbb-ck,"%d ",b);
        char m2[500]; int k2=snprintf(m2,sizeof m2,"  [%s] rt=%ux%u stride=%u elems=%d ntex=%d%s:%s |",tag,rt0->w,rt0->h,g_d3dst.vstride[0],il0?il0->nelem:0,ntex,cbb,tb);
        if(il0) for(int i=0;i<il0->nelem&&k2<380;i++) k2+=snprintf(m2+k2,sizeof m2-k2," %s",il0->elem[i].sem);
        if(k2<398){m2[k2++]='\n';m2[k2]=0;} (void)!write(2,m2,k2); return; }
    D3DObj*il=g_d3dst.il,*vb=g_d3dst.vb[0],*tex=g_d3dst.psrv[0],*rt=g_d3dst.rtv;
    char m[640]; int k=snprintf(m,sizeof m,"  [%s] n=%u topo=%u stride=%u vb=%lluB ibfmt=%u tex=%ux%u(f%u) rt=%ux%u vp=%dx%d elems=%d:",tag,count,g_d3dst.topo,g_d3dst.vstride[0],vb?(unsigned long long)vb->size:0,g_d3dst.ibfmt,tex?tex->w:0,tex?tex->h:0,tex?tex->fmt:0,rt?rt->w:0,rt?rt->h:0,g_d3dst.vp_w,g_d3dst.vp_h,il?il->nelem:0);
    if(il) for(int i=0;i<il->nelem&&k<600;i++) k+=snprintf(m+k,sizeof m-k," %s@%u/%s",il->elem[i].sem,il->elem[i].off,dxgi_fmt_name(il->elem[i].fmt));
    if(k<638){ m[k++]='\n'; m[k]=0; } (void)!write(2,m,k); }
static int MSABI swap_Present(void*t,uint32_t sync,uint32_t fl){ (void)t;(void)sync;(void)fl; d3dtrace(4,"Present");
    // PELOAD_TRACE_PRESENT_RATE: periodic count+timestamp so we can see if/when the engine stops
    // calling Present (render-thread stall) vs keeps calling it (present-bridge stall).
    if(getenv("PELOAD_TRACE_PRESENT_RATE")){ static unsigned long pc; static long t0; struct timeval tv; gettimeofday(&tv,0);
        long ms=tv.tv_sec*1000L+tv.tv_usec/1000; if(!t0)t0=ms; if((pc++ % 60)==0){ char m[96]; int k=snprintf(m,sizeof m,"[present-rate] n=%lu t=%ldms thr=0x%x\n",pc,ms-t0,pthread_mach_thread_np(pthread_self())); (void)!write(2,m,k);} }
    if(getenv("PELOAD_TRACE_PRESENT")){ static int c; if(c++<3){ char m[64]; int k=snprintf(m,sizeof m,"[present] call #%d sync=%u fl=0x%x chain:\n",c,sync,fl); (void)!write(2,m,k);
        uint64_t* sp=(uint64_t*)__builtin_frame_address(0);
        for(int i=0;i<400;i++){ uint64_t v=sp[i];
            if(v>=0x180000000ULL&&v<0x187000000ULL){ char b[48]; int n=snprintf(b,sizeof b,"   halo3+0x%llx\n",(unsigned long long)(v-0x180000000ULL)); (void)!write(2,b,n); }
            else if(v>=0x140000000ULL&&v<0x14c000000ULL){ char b[48]; int n=snprintf(b,sizeof b,"   exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,b,n); } } } }
    if(d3dgl_enabled()) d3dgl_present();
    return HR_OK; }
// The back buffer must be STABLE across GetBuffer calls (the engine caches/compares it and
// renders into it each frame); a fresh texture per call breaks present bookkeeping.
static D3DObj* g_d3d_backbuf;
static int MSABI swap_GetBuffer(void*t,uint32_t buf,GUID_*r,void**pp){ (void)t;(void)buf;(void)r;
    if(!g_d3d_backbuf) g_d3d_backbuf=mktex(4096,4096,1,28,2);
    if(pp)*pp=g_d3d_backbuf; return HR_OK; }
// IDXGIDeviceSubObject::GetDevice (vtable slot 7): MUST populate *pp with the
// device, else callers (e.g. ui_composition's IDXGISwapChain::GetDevice(ID3D11Device))
// get a null device, take their error path, and LEAK a RefCell borrow -> the
// frontend's "RefCell already borrowed" re-entrancy panic on the next UI draw.
static int MSABI swap_GetDevice(void*t,GUID_*riid,void**pp){ (void)t;(void)riid; if(pp)*pp=&g_d3ddevice; return HR_OK; }
static int MSABI dxgi_CreateSwapChain(void*t,void*dev,void*desc,void**pp){ (void)t;(void)dev;(void)desc; if(pp)*pp=d3dobj(g_dxgiswap_vtbl,0,0); return HR_OK; }
// DXGI_SWAP_CHAIN_DESC: BufferDesc{W@0,H@4,RR num@8/den@12,Format@16,...} SampleDesc{Count@28,Q@32}
// BufferUsage@36 BufferCount@40 OutputWindow@48 Windowed@56 SwapEffect@60 Flags@64. A zeroed
// desc (our old no-op) made the engine see buffers=0/format=0 and never count a "ready present".
static int MSABI swap_GetDesc(void*t,void*pd){ (void)t; if(!pd) return HR_OK; uint8_t*d=pd; memset(d,0,80);
    *(uint32_t*)(d+0)=4096; *(uint32_t*)(d+4)=4096; *(uint32_t*)(d+8)=60; *(uint32_t*)(d+12)=1;
    *(uint32_t*)(d+16)=87; *(uint32_t*)(d+28)=1; *(uint32_t*)(d+36)=0x20; *(uint32_t*)(d+40)=2;
    *(int32_t*)(d+56)=0; *(uint32_t*)(d+60)=0; *(uint32_t*)(d+64)=0; return HR_OK; }
static int MSABI swap_GetFullscreenState(void*t,int32_t*pfs,void**pp){ (void)t; if(pfs)*pfs=0; if(pp)*pp=0; return HR_OK; }
// swap-chain method tracer (PELOAD_TRACE_SWAP): log which vtable slots the engine calls.
static int64_t swaptr_log(int slot){ static int c[40]; if(slot>=0&&slot<40&&c[slot]++<4){ char b[48]; int k=snprintf(b,sizeof b,"  [swap] slot %d\n",slot); (void)!write(2,b,k);} return 0; }
#define SWT(n) static int64_t MSABI swaptr_##n(void*a,void*b,void*c,void*d,void*e,void*f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return swaptr_log(n);}
SWT(10)SWT(11)SWT(12)SWT(13)SWT(14)SWT(15)SWT(16)SWT(17)SWT(18)SWT(19)SWT(20)SWT(21)SWT(22)SWT(23)SWT(24)
SWT(25)SWT(26)SWT(27)SWT(28)SWT(29)SWT(30)SWT(31)SWT(32)SWT(33)SWT(34)SWT(35)SWT(36)SWT(37)SWT(38)SWT(39)
void* g_swaptrace[40]={0,0,0,0,0,0,0,0,0,0,(void*)swaptr_10,(void*)swaptr_11,(void*)swaptr_12,(void*)swaptr_13,(void*)swaptr_14,(void*)swaptr_15,(void*)swaptr_16,(void*)swaptr_17,(void*)swaptr_18,(void*)swaptr_19,(void*)swaptr_20,(void*)swaptr_21,(void*)swaptr_22,(void*)swaptr_23,(void*)swaptr_24,(void*)swaptr_25,(void*)swaptr_26,(void*)swaptr_27,(void*)swaptr_28,(void*)swaptr_29,(void*)swaptr_30,(void*)swaptr_31,(void*)swaptr_32,(void*)swaptr_33,(void*)swaptr_34,(void*)swaptr_35,(void*)swaptr_36,(void*)swaptr_37,(void*)swaptr_38,(void*)swaptr_39};
// PELOAD_SLOT_CENSUS: counting trampolines installed over every still-unhooked context vtable slot,
// to discover which slot the engine ACTUALLY calls for IASetInputLayout (IASetIL via slot 17 reads 0
// while IASetVertexBuffers via slot 18 reads millions — the input layout must go through some slot).
static uint64_t g_slotcnt[160];
#define SLT(n) static int64_t MSABI slt_##n(void*a,void*b,void*c,void*d,void*e,void*f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; g_slotcnt[n]++; return 0;}
SLT(0)SLT(1)SLT(2)SLT(3)SLT(4)SLT(5)SLT(6)SLT(7)SLT(8)SLT(9)SLT(10)SLT(11)SLT(12)SLT(13)SLT(14)SLT(15)
SLT(16)SLT(17)SLT(18)SLT(19)SLT(20)SLT(21)SLT(22)SLT(23)SLT(24)SLT(25)SLT(26)SLT(27)SLT(28)SLT(29)SLT(30)SLT(31)
SLT(32)SLT(33)SLT(34)SLT(35)SLT(36)SLT(37)SLT(38)SLT(39)SLT(40)SLT(41)SLT(42)SLT(43)SLT(44)SLT(45)SLT(46)SLT(47)
SLT(48)SLT(49)SLT(50)SLT(51)SLT(52)SLT(53)SLT(54)SLT(55)SLT(56)SLT(57)SLT(58)SLT(59)SLT(60)SLT(61)SLT(62)SLT(63)
SLT(64)SLT(65)SLT(66)SLT(67)SLT(68)SLT(69)SLT(70)SLT(71)SLT(72)SLT(73)SLT(74)SLT(75)SLT(76)SLT(77)SLT(78)SLT(79)
SLT(80)SLT(81)SLT(82)SLT(83)SLT(84)SLT(85)SLT(86)SLT(87)SLT(88)SLT(89)SLT(90)SLT(91)SLT(92)SLT(93)SLT(94)SLT(95)
SLT(96)SLT(97)SLT(98)SLT(99)SLT(100)SLT(101)SLT(102)SLT(103)SLT(104)SLT(105)SLT(106)SLT(107)SLT(108)SLT(109)SLT(110)SLT(111)
SLT(112)SLT(113)SLT(114)SLT(115)SLT(116)SLT(117)SLT(118)SLT(119)SLT(120)SLT(121)SLT(122)SLT(123)SLT(124)SLT(125)SLT(126)SLT(127)
SLT(128)SLT(129)SLT(130)SLT(131)SLT(132)SLT(133)SLT(134)SLT(135)SLT(136)SLT(137)SLT(138)SLT(139)SLT(140)SLT(141)SLT(142)SLT(143)
SLT(144)SLT(145)SLT(146)SLT(147)SLT(148)SLT(149)SLT(150)SLT(151)SLT(152)SLT(153)SLT(154)SLT(155)SLT(156)SLT(157)SLT(158)SLT(159)
#undef SLT
#define SF(n) slt_##n
static void* const g_sltfns[160]={
 SF(0),SF(1),SF(2),SF(3),SF(4),SF(5),SF(6),SF(7),SF(8),SF(9),SF(10),SF(11),SF(12),SF(13),SF(14),SF(15),
 SF(16),SF(17),SF(18),SF(19),SF(20),SF(21),SF(22),SF(23),SF(24),SF(25),SF(26),SF(27),SF(28),SF(29),SF(30),SF(31),
 SF(32),SF(33),SF(34),SF(35),SF(36),SF(37),SF(38),SF(39),SF(40),SF(41),SF(42),SF(43),SF(44),SF(45),SF(46),SF(47),
 SF(48),SF(49),SF(50),SF(51),SF(52),SF(53),SF(54),SF(55),SF(56),SF(57),SF(58),SF(59),SF(60),SF(61),SF(62),SF(63),
 SF(64),SF(65),SF(66),SF(67),SF(68),SF(69),SF(70),SF(71),SF(72),SF(73),SF(74),SF(75),SF(76),SF(77),SF(78),SF(79),
 SF(80),SF(81),SF(82),SF(83),SF(84),SF(85),SF(86),SF(87),SF(88),SF(89),SF(90),SF(91),SF(92),SF(93),SF(94),SF(95),
 SF(96),SF(97),SF(98),SF(99),SF(100),SF(101),SF(102),SF(103),SF(104),SF(105),SF(106),SF(107),SF(108),SF(109),SF(110),SF(111),
 SF(112),SF(113),SF(114),SF(115),SF(116),SF(117),SF(118),SF(119),SF(120),SF(121),SF(122),SF(123),SF(124),SF(125),SF(126),SF(127),
 SF(128),SF(129),SF(130),SF(131),SF(132),SF(133),SF(134),SF(135),SF(136),SF(137),SF(138),SF(139),SF(140),SF(141),SF(142),SF(143),
 SF(144),SF(145),SF(146),SF(147),SF(148),SF(149),SF(150),SF(151),SF(152),SF(153),SF(154),SF(155),SF(156),SF(157),SF(158),SF(159)};
#undef SF
static void slotcen_dump(void){ if(!getenv("PELOAD_SLOT_CENSUS")) return; static uint64_t last,tick;
    uint64_t sum=0; for(int i=0;i<160;i++) sum+=g_slotcnt[i]; if(sum<last+200000) return; last=sum; if(tick++ %1) {}
    char b[1800]; int k=0; k+=snprintf(b+k,sizeof b-k,"  [slot-census]");
    for(int i=0;i<160;i++) if(g_slotcnt[i]) k+=snprintf(b+k,sizeof b-k," %d=%llu",i,(unsigned long long)g_slotcnt[i]);
    k+=snprintf(b+k,sizeof b-k,"\n"); (void)!write(2,b,k); }
static void d3d_init(void){
    static int done; if(done) return; done=1;
    void** c=g_d3dchild_vtbl; c[0]=(void*)d3d_QI;c[1]=(void*)d3d_AddRef;c[2]=(void*)d3d_Release;c[3]=(void*)d3d_GetDevice;c[4]=(void*)child_GetPD;c[5]=(void*)child_SetPD;c[6]=(void*)child_SetPDI;
    for(int i=7;i<16;i++) c[i]=(void*)ctx_ret0;
    void** r=g_d3dres_vtbl; for(int i=0;i<7;i++) r[i]=c[i]; r[7]=(void*)res_GetType;r[8]=(void*)res_SetEvict;r[9]=(void*)res_GetEvict;r[10]=(void*)res_GetDesc; for(int i=11;i<16;i++) r[i]=(void*)ctx_ret0;
    void** v=g_d3dview_vtbl; for(int i=0;i<7;i++) v[i]=c[i]; v[7]=(void*)view_GetResource;v[8]=(void*)view_GetDesc; for(int i=9;i<16;i++) v[i]=(void*)ctx_ret0;
    g_d3ddummyres_obj.vtbl=g_d3dres_vtbl; g_d3ddummyres_obj.kind=1; g_d3ddummyres_obj.size=1<<16; g_d3ddummyres_obj.data=calloc(1,1<<16); g_d3ddummyres=&g_d3ddummyres_obj;
    void** d=g_d3ddev_vtbl;
    d[0]=(void*)dev_QI;d[1]=(void*)d3d_AddRef;d[2]=(void*)d3d_Release;   // per-IID QI (device/dxgi device/context)
    d[3]=(void*)dev_CreateBuffer;d[4]=(void*)dev_CreateTexture1D;d[5]=(void*)dev_CreateTexture2D;d[6]=(void*)dev_CreateTexture3D;
    d[7]=(void*)dev_crView3;d[8]=(void*)dev_crView3;d[9]=(void*)dev_crView3;d[10]=(void*)dev_crView3; // SRV/UAV/RTV/DSV
    d[11]=(void*)dev_crIL;                                                                            // CreateInputLayout
    d[12]=(void*)dev_crVS;d[13]=(void*)dev_crGS;d[14]=(void*)dev_crGSSO;d[15]=(void*)dev_crPS;d[16]=(void*)dev_crHS;d[17]=(void*)dev_crDS;d[18]=(void*)dev_crCS; // 12=VS,13=GS,14=GSSO,15=PS,16=HS,17=DS,18=CS
    d[19]=(void*)dev_cr1;                                                                             // CreateClassLinkage
    d[20]=(void*)dev_cr2;d[21]=(void*)dev_cr2;d[22]=(void*)dev_cr2;d[23]=(void*)dev_cr2;d[24]=(void*)dev_cr2;d[25]=(void*)dev_cr2;d[26]=(void*)dev_cr2; // Blend,DSS,RS,Sampler,Query,Predicate,Counter
    d[27]=(void*)dev_crCtx;                                                                           // CreateDeferredContext
    d[28]=(void*)dev_OpenShared;d[29]=(void*)dev_CheckFormat;d[30]=(void*)dev_CheckMSAA;d[31]=(void*)dev_CheckCounterInfo;d[32]=(void*)ctx_ret0;d[33]=(void*)dev_CheckFeature;
    d[34]=(void*)d3d_GetPD;d[35]=(void*)d3d_SetPD;d[36]=(void*)d3d_SetPDI;
    d[37]=(void*)dev_GetFeatureLevel;d[38]=(void*)dev_GetCreationFlags;d[39]=(void*)dev_GetRemovedReason;d[40]=(void*)dev_GetImmediateContext;d[41]=(void*)dev_SetExceptionMode;d[42]=(void*)dev_GetExceptionMode;
    for(int i=43;i<48;i++) d[i]=(void*)ctx_ret0;
    g_d3ddevice.vtbl=d;
    // IDXGIDevice (returned by device.QI(IDXGIDevice)): IUnknown + IDXGIObject +
    // GetAdapter(7). A valid object here keeps ui_composition on its success path
    // instead of the error path that leaks the app-state RefCell borrow.
    void** gd=g_dxgidev_vtbl; for(int i=0;i<16;i++) gd[i]=(void*)ctx_ret0;
    gd[0]=(void*)dev_QI;gd[1]=(void*)d3d_AddRef;gd[2]=(void*)d3d_Release;
    gd[6]=(void*)dxgidev_GetParent;gd[7]=(void*)dxgidev_GetAdapter;
    g_dxgidevice.vtbl=gd;
    void** ga=g_dxgiadapter_vtbl; for(int i=0;i<16;i++) ga[i]=(void*)ctx_ret0;
    ga[0]=(void*)adapter_QI;ga[1]=(void*)d3d_AddRef;ga[2]=(void*)d3d_Release;ga[6]=(void*)dxgidev_GetParent;ga[8]=(void*)adapter_GetDesc;
    ga[7]=(void*)dxgiadapter_EnumOutputs; ga[9]=(void*)dxgiadapter_CheckInterfaceSupport; ga[10]=(void*)adapter_GetDesc; // GetDesc1
    g_dxgiadapter.vtbl=ga;
    void** x=g_d3dctx_vtbl; for(int i=0;i<160;i++) x[i]=(void*)ctx_ret0;
    x[0]=(void*)d3d_QI;x[1]=(void*)d3d_AddRef;x[2]=(void*)d3d_Release;x[3]=(void*)ctx_GetDevice;x[4]=(void*)d3d_GetPD;x[5]=(void*)d3d_SetPD;x[6]=(void*)d3d_SetPDI;
    x[12]=(void*)ctx_DrawIndexed; x[13]=(void*)ctx_Draw;                                             // DrawIndexed, Draw
    x[14]=(void*)ctx_Map;                                                                            // Map (Unmap=15 stays ret0)
    x[20]=(void*)ctx_DrawIndexedInstanced; x[21]=(void*)ctx_DrawInstanced; x[41]=(void*)ctx_Dispatch;
    x[7]=(void*)ctx_VSSetConstantBuffers; x[9]=(void*)ctx_PSSetShader; x[11]=(void*)ctx_VSSetShader; x[16]=(void*)ctx_PSSetConstantBuffers;
    x[8]=(void*)ctx_PSSetShaderResources; x[17]=(void*)ctx_IASetInputLayout; x[18]=(void*)ctx_IASetVertexBuffers;
    x[25]=(void*)ctx_VSSetShaderResources;   // VS-stage SRVs (vertex-pulling geometry source)
    x[19]=(void*)ctx_IASetIndexBuffer; x[24]=(void*)ctx_IASetPrimitiveTopology; x[33]=(void*)ctx_OMSetRenderTargets; x[44]=(void*)ctx_RSSetViewports;
    x[46]=(void*)ctx_CopySubresourceRegion; x[47]=(void*)ctx_CopyResource; x[48]=(void*)ctx_UpdateSubresource;
    x[50]=(void*)ctx_ClearRTV; x[53]=(void*)ctx_ClearDSV;   // ClearRenderTargetView / ClearDepthStencilView (d3dgl)
    x[119]=(void*)ctx_VSSetConstantBuffers1; x[123]=(void*)ctx_PSSetConstantBuffers1;  // ID3D11DeviceContext1 (halo3 binds matrices here)
    // Get* out-param zeroing (standard ID3D11DeviceContext vtable indices)
    x[72]=(void*)ctx_get_arr;   // VSGetConstantBuffers
    x[73]=(void*)ctx_get_arr;   // PSGetShaderResources
    x[74]=(void*)ctx_get_shader;// PSGetShader
    x[75]=(void*)ctx_get_arr;   // PSGetSamplers
    x[76]=(void*)ctx_get_shader;// VSGetShader
    x[77]=(void*)ctx_get_arr;   // PSGetConstantBuffers
    x[78]=(void*)ctx_get_1;     // IAGetInputLayout
    x[79]=(void*)ctx_get_vb;    // IAGetVertexBuffers
    x[80]=(void*)ctx_get_ib;    // IAGetIndexBuffer
    x[81]=(void*)ctx_get_arr;   // GSGetConstantBuffers
    x[82]=(void*)ctx_get_shader;// GSGetShader
    // 83 IAGetPrimitiveTopology (out = UINT*) -> ret0 writes nothing; engine rarely reads. leave.
    x[84]=(void*)ctx_get_arr;   // VSGetShaderResources
    x[85]=(void*)ctx_get_arr;   // VSGetSamplers
    x[87]=(void*)ctx_get_arr;   // GSGetShaderResources
    x[88]=(void*)ctx_get_arr;   // GSGetSamplers
    x[89]=(void*)ctx_om_get;    // OMGetRenderTargets
    x[97]=(void*)ctx_get_arr;   // HSGetShaderResources
    x[98]=(void*)ctx_get_shader;// HSGetShader
    x[99]=(void*)ctx_get_arr;   // HSGetSamplers
    x[100]=(void*)ctx_get_arr;  // HSGetConstantBuffers
    x[101]=(void*)ctx_get_arr;  // DSGetShaderResources
    x[102]=(void*)ctx_get_shader;// DSGetShader
    x[103]=(void*)ctx_get_arr;  // DSGetSamplers
    x[104]=(void*)ctx_get_arr;  // DSGetConstantBuffers
    x[105]=(void*)ctx_get_arr;  // CSGetShaderResources
    x[107]=(void*)ctx_get_shader;// CSGetShader
    x[108]=(void*)ctx_get_arr;  // CSGetSamplers
    x[109]=(void*)ctx_get_arr;  // CSGetConstantBuffers
    // slot census: over every still-unhooked slot 0..63, install a counting trampoline (does not
    // disturb the real handlers; reveals which slot the engine uses for IASetInputLayout etc.)
    if(getenv("PELOAD_SLOT_CENSUS")) for(int i=0;i<160;i++) if(x[i]==(void*)ctx_ret0) x[i]=(void*)g_sltfns[i];
    g_d3dcontext.vtbl=x;
    void** s=g_dxgiswap_vtbl; for(int i=0;i<40;i++) s[i]=(void*)ctx_ret0;
    s[0]=(void*)d3d_QI;s[1]=(void*)d3d_AddRef;s[2]=(void*)d3d_Release;
    s[7]=(void*)swap_GetDevice;                           // IDXGIDeviceSubObject::GetDevice
    s[8]=(void*)swap_Present;s[9]=(void*)swap_GetBuffer;
    s[11]=(void*)swap_GetFullscreenState; s[12]=(void*)swap_GetDesc;
    if(getenv("PELOAD_TRACE_SWAP")){ extern void* g_swaptrace[40]; for(int i=10;i<40;i++) if(s[i]==(void*)ctx_ret0) s[i]=g_swaptrace[i]; }
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
// D3DReflect: halo3 reflects its compiled shaders to discover constant buffers /
// resource bindings / input params. Unimplemented, it left **ppReflector
// uninitialized -> the engine deref'd a garbage pointer. Return a fake reflection
// whose GetDesc reports ZERO of everything, so the engine's enumeration loops run
// zero times and it binds nothing (fine for a null renderer).
static int      MSABI refl_ret0(void){ return 0; }
static int      MSABI refl_GetDesc(void*s,void*pd){ (void)s; if(pd) memset(pd,0,128); return HR_OK; } // D3D11_SHADER_DESC: all counts 0 (>=128B, no overflow)
static void*    g_refl_vtbl[40];
static ComObj   g_refl = { g_refl_vtbl };
static int MSABI i_D3DReflect(const void*src,uint64_t sz,GUID_*iid,void**pp){ (void)src;(void)sz;(void)iid;
    static int init; if(!init){ for(int i=0;i<40;i++) g_refl_vtbl[i]=(void*)refl_ret0;
        g_refl_vtbl[0]=(void*)com_QI; g_refl_vtbl[1]=(void*)com_AddRef; g_refl_vtbl[2]=(void*)com_Release;
        g_refl_vtbl[3]=(void*)refl_GetDesc; init=1; }
    if(pp)*pp=&g_refl; return HR_OK; }
static Blob* make_blob(const void*data,uint64_t n); // fwd
static int MSABI i_D3DGetInputSignatureBlob(const void*src,uint64_t sz,void**pp){ (void)src;(void)sz;
    uint8_t hdr[64]; memset(hdr,0,sizeof hdr); hdr[0]='D';hdr[1]='X';hdr[2]='B';hdr[3]='C'; if(pp)*pp=make_blob(hdr,sizeof hdr); return HR_OK; }
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
// Volume state: the worker's per-tick audio-settings apply (exe+0x481560) READS BACK the
// voice's current volume (IXAudio2Voice::GetVolume slot12 / GetChannelVolumes slot15) and
// compares it to its target; if they differ it re-applies via SetVolume/SetChannelVolumes and
// logs "Game audio volume applied". A ret0 getter leaves the caller's buffer at 0 != target,
// so it re-applied (and spammed the log) EVERY tick. Store what Set* writes and hand it back
// from Get* so the compare matches after one apply. Voices share g_xavoice, so one store set
// suffices for the master/submix volume this path drives.
static float g_xa_vol = 1.0f;          // scalar overall volume (SetVolume/GetVolume)
static float g_xa_chvol[64];           // per-channel volumes (Set/GetChannelVolumes)
static uint32_t g_xa_chn;              // channels last set
static int MSABI xa_SetVolume(void*self,float vol,uint32_t op){ (void)self;(void)op; g_xa_vol=vol; return 0; }
static void MSABI xa_GetVolume(void*self,float*pv){ (void)self; if(pv)*pv=g_xa_vol; }
static int MSABI xa_SetChannelVolumes(void*self,uint32_t ch,const float*v,uint32_t op){ (void)self;(void)op;
    if(v&&ch<=64){ for(uint32_t i=0;i<ch;i++) g_xa_chvol[i]=v[i]; g_xa_chn=ch; } return 0; }
static void MSABI xa_GetChannelVolumes(void*self,uint32_t ch,float*v){ (void)self;
    if(v) for(uint32_t i=0;i<ch;i++) v[i]=(i<64)?g_xa_chvol[i]:0.0f; }
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
    static int init; if(!init){ for(int i=0;i<64;i++) g_xavoice_vtbl[i]=(void*)xa_ret0; g_xavoice_vtbl[0]=(void*)xa_GetVoiceDetails; g_xavoice_vtbl[25]=(void*)xa_GetState;
        g_xavoice_vtbl[12]=(void*)xa_SetVolume; g_xavoice_vtbl[13]=(void*)xa_GetVolume;
        g_xavoice_vtbl[14]=(void*)xa_SetChannelVolumes; g_xavoice_vtbl[15]=(void*)xa_GetChannelVolumes; init=1; }
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
// Private-bridge UDP peer transport: the engine binds AND sends to the same fixed low loopback
// ports (1000/1002/1003) on both peers. Symmetric +40000 made host+client collide on the same
// bound ports in separate processes, so gameplay packets never crossed (gameplay_join_verified
// stayed false). PELOAD_UDP_ROLE makes the remap ROLE-ASYMMETRIC: a peer BINDS at its own base and
// SENDS at the other peer's base, so host-bind receives client-send and vice versa.
//   host  : bind +40000, send +50000      client: bind +50000, send +40000
// Unset -> symmetric +40000 (single-process / unchanged). t_sa_send marks a sa_w2b done for a
// sendto/connect (use the SEND base) vs a bind (use the BIND base).
static __thread int t_sa_send;
static void udp_bases(int*bindbase,int*sendbase){ const char*r=getenv("PELOAD_UDP_ROLE");
    if(r&&!strcmp(r,"host")){ *bindbase=40000; *sendbase=50000; }
    else if(r&&!strcmp(r,"client")){ *bindbase=50000; *sendbase=40000; }
    else { *bindbase=40000; *sendbase=40000; } }
// ---- per-peer loopback-IP identity preservation ---------------------------
// The engine assigns DISTINCT loopback IPs per peer/role: host transport 127.0.0.3,
// client transport 127.0.1.1, client native bind 127.0.2.1, etc. (deterministic from the
// join slot). On Windows all of 127.0.0.0/8 is loopback so each process binds its own IP and
// the peers address + recognize each other by IP. macOS routes only 127.0.0.1, so sa_w2b
// collapses 127.x.x.x -> 127.0.0.1 to make bind/sendto work (the port alone still uniquely
// identifies each UDP socket across both processes). But the collapse ALSO erases the source
// IP: the host's private-bridge sends its MCV1 verify datagrams to 127.0.1.1:0 (it knows the
// peer IP from the slot assignment but must learn the PORT from the client's incoming MCH1),
// and it only adopts that port from a packet whose source IP matches the expected peer
// (127.0.1.1). With the collapse the client's packets arrive from 127.0.0.1, the match never
// succeeds, the host keeps replying to port 0, and gameplay_join_verified stays false.
// Fix: record per-fd the guest destination IP the engine last sent to (before the collapse),
// and when a UDP recv hands back a 127.0.0.1 source, present it as that recorded guest IP so
// the engine's peer-identity check passes. Disable with PELOAD_NO_PEERIP.
static uint32_t g_peer_gip[MAXFD];   // fd -> guest dest IPv4 (network order) last sent on it
static uint32_t g_last_peer_gip;     // most recent distinct (non-127.0.0.1) guest peer IP, any fd
static int g_peerip_off=-1;
static void peer_gip_rec(int fd,const void*win,int wl){
    if(g_peerip_off<0) g_peerip_off=getenv("PELOAD_NO_PEERIP")?1:0;
    if(g_peerip_off||fd<0||fd>=MAXFD||!win||wl<8) return; const uint8_t*w=win;
    if(((w[0]|(w[1]<<8)))!=2) return; uint32_t ip; memcpy(&ip,w+4,4);
    if((ntohl(ip)>>24)==127){ if(g_peer_gip[fd]!=ip && getenv("PELOAD_TRACE_UDP")){ char ips[32]; inet_ntop(AF_INET,&ip,ips,sizeof ips); char b[96]; int k=snprintf(b,sizeof b,"  [udp] rec fd=%d dest-guest-ip=%s\n",fd,ips); (void)!write(2,b,k);} g_peer_gip[fd]=ip;
        if(ip!=htonl(0x7f000001)) g_last_peer_gip=ip; } }
static int peer_gip_restore(int fd,void*win){
    if(g_peerip_off<0) g_peerip_off=getenv("PELOAD_NO_PEERIP")?1:0;
    if(g_peerip_off||fd<0||fd>=MAXFD||!win) return 0; uint8_t*w=win;
    if(((w[0]|(w[1]<<8)))!=2) return 0; uint32_t ip; memcpy(&ip,w+4,4);
    // Pick the guest IP this socket last addressed; if this fd never sent (a bridge channel the
    // engine only RECEIVES on until it learns the peer port), fall back to the most recent distinct
    // peer IP seen on any fd (single-peer loopback bridge). Opt out of the fallback with
    // PELOAD_NO_PEERIP_GLOBAL; the whole mechanism with PELOAD_NO_PEERIP.
    uint32_t gip=g_peer_gip[fd]; if(!gip && !getenv("PELOAD_NO_PEERIP_GLOBAL")) gip=g_last_peer_gip;
    if(!gip) return 0;
    if(ip==htonl(0x7f000001) && gip!=htonl(0x7f000001)){ memcpy(w+4,&gip,4);
        if(getenv("PELOAD_TRACE_UDP")){ char ips[32]; inet_ntop(AF_INET,&gip,ips,sizeof ips); char b[96]; int k=snprintf(b,sizeof b,"  [udp] restore fd=%d src 127.0.0.1 -> %s%s\n",fd,ips,g_peer_gip[fd]?"":" (global)"); (void)!write(2,b,k);} return 1; } return 0; }
static uint32_t errno2wsa(int e){ switch(e){ case EWOULDBLOCK:return 10035; case EINPROGRESS:return 10036;
    case EALREADY:return 10037; case ENOTSOCK:return 10038; case EMSGSIZE:return 10040; case EADDRINUSE:return 10048;
    case EADDRNOTAVAIL:return 10049; case ENETUNREACH:return 10051; case ECONNRESET:return 10054; case ENOBUFS:return 10055;
    case EISCONN:return 10056; case ENOTCONN:return 10057; case ECONNREFUSED:return 10061; case EAFNOSUPPORT:return 10047;
    case EINVAL:return 10022; default:return 10022; } }
// Winsock keeps WSAGetLastError and GetLastError as the SAME value; Rust's mio reads
// socket errors through GetLastError (io::Error::last_os_error), so both slots must match
// or a non-blocking connect's WSAEWOULDBLOCK is misread and the connection is abandoned.
static void ws2_err(int e){ t_wsa_err = e? (int)errno2wsa(e):0; g_lasterr=(uint32_t)t_wsa_err; }
// Is this IPv4 (network order) a locally-assigned address? Used so the 127/8 collapse only
// applies to loopback IPs that are NOT actually bindable here. If the user adds lo0 aliases for
// the engine's distinct per-peer loopback IPs (127.0.0.3 / 127.0.1.1 / 127.0.2.1 ...), those
// become real and we DON'T collapse them -> the native bridge addresses peers exactly like
// Windows (correct source IPs, no hole-punch needed). Un-aliased 127.x still collapses as before.
// Cached once via getifaddrs (refreshed if PELOAD_NO_IFCACHE set, for tests that add aliases live).
static int ip_is_local(uint32_t ipn){
    static uint32_t locals[128]; static int nloc=-1;
    if(nloc<0 || getenv("PELOAD_NO_IFCACHE")){ nloc=0; struct ifaddrs*ia=0;
        if(getifaddrs(&ia)==0){ for(struct ifaddrs*p=ia;p&&nloc<128;p=p->ifa_next){
            if(p->ifa_addr && p->ifa_addr->sa_family==AF_INET) locals[nloc++]=((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr; }
            freeifaddrs(ia); } }
    for(int i=0;i<nloc;i++) if(locals[i]==ipn) return 1; return 0; }
// Windows sockaddr_in -> BSD sockaddr_storage; returns BSD length or 0
static int sa_w2b(const void*win,int wl,struct sockaddr_storage*bsd){ if(!win||wl<2) return 0; const uint8_t*w=win;
    uint16_t fam=(uint16_t)(w[0]|(w[1]<<8)); memset(bsd,0,sizeof *bsd);
    if(fam==2){ struct sockaddr_in*b=(void*)bsd; b->sin_len=sizeof *b; b->sin_family=AF_INET; memcpy(&b->sin_port,w+2,2); memcpy(&b->sin_addr,w+4,4);
        // macOS routes ONLY 127.0.0.1 to loopback, not all of 127/8 like Linux/Windows.
        // Reclaimer's native-UDP transport assigns unique 127.x.x.x peer IPs, so bind/
        // connect/sendto to e.g. 127.3.164.238 fail with EADDRNOTAVAIL -> the guardian
        // match can't host -> halo3 never selects the map -> "map catalog ... got ''".
        // Collapse 127/8 -> 127.0.0.1 consistently (peers stay distinct by port). Disable
        // with PELOAD_NO_LOOPBACK_FIX.
        { uint8_t*ip=(uint8_t*)&b->sin_addr; if(ip[0]==127 && !(ip[1]==0&&ip[2]==0&&ip[3]==1) && !getenv("PELOAD_NO_LOOPBACK_FIX") && !ip_is_local(b->sin_addr.s_addr)){ ip[1]=0;ip[2]=0;ip[3]=1; } }
        // macOS can't bind privileged ports (<1024) without root. Reclaimer's native-UDP
        // transport uses low loopback ports (e.g. 127.0.0.1:1000), so bind fails -> the
        // session never establishes -> loading stalls. Remap loopback ports <1024 to a high
        // range consistently (reversed in sa_b2w so getsockname still reports the original).
        { uint8_t*ip=(uint8_t*)&b->sin_addr; if(ip[0]==127 && !getenv("PELOAD_NO_PRIVPORT_FIX")){ uint16_t p=ntohs(b->sin_port); if(p>0&&p<1024){ int bb,sb; udp_bases(&bb,&sb); b->sin_port=htons((uint16_t)(p+(t_sa_send?sb:bb))); } } }
        return sizeof *b; }
    if(fam==23){ struct sockaddr_in6*b=(void*)bsd; b->sin6_len=sizeof *b; b->sin6_family=AF_INET6; memcpy(&b->sin6_port,w+2,2); memcpy(&b->sin6_flowinfo,w+4,4); memcpy(&b->sin6_addr,w+8,16); memcpy(&b->sin6_scope_id,w+24,4); return sizeof *b; }
    return 0; }
// BSD sockaddr -> Windows sockaddr; writes up to *wl bytes, sets *wl to size used
static void sa_b2w(const struct sockaddr*bsd,void*win,int*wl){ if(!bsd||!win) return; uint8_t*w=win;
    if(bsd->sa_family==AF_INET){ const struct sockaddr_in*b=(void*)bsd; if(!wl||*wl>=16){ w[0]=2;w[1]=0; memcpy(w+2,&b->sin_port,2); memcpy(w+4,&b->sin_addr,4); memset(w+8,0,8);
        const uint8_t*ip=(const uint8_t*)&b->sin_addr; if(ip[0]==127 && !getenv("PELOAD_NO_PRIVPORT_FIX")){ uint16_t p=ntohs(b->sin_port); uint16_t np=0; if(p>40000&&p<41024) np=p-40000; else if(p>50000&&p<51024) np=p-50000; if(np){ uint16_t n=htons(np); memcpy(w+2,&n,2); } } } if(wl)*wl=16; }
    else if(bsd->sa_family==AF_INET6){ const struct sockaddr_in6*b=(void*)bsd; if(!wl||*wl>=28){ w[0]=23;w[1]=0; memcpy(w+2,&b->sin6_port,2); memcpy(w+4,&b->sin6_flowinfo,4); memcpy(w+8,&b->sin6_addr,16); memcpy(w+24,&b->sin6_scope_id,4);} if(wl)*wl=28; } }
static int MSABI i_ws2_WSAStartup(uint16_t ver,void*data){ if(data){ uint8_t*d=data; *(uint16_t*)d=ver; *(uint16_t*)(d+2)=ver; memset(d+4,0,398);} t_wsa_err=0; return 0; }
static int MSABI i_ws2_WSACleanup(void){ return 0; }
static int MSABI i_ws2_WSAGetLastError(void){ return t_wsa_err; }
static void MSABI i_ws2_WSASetLastError(int e){ t_wsa_err=e; }
static void net_tracef(const char*fmt,...);
static WSASOCK MSABI i_ws2_socket(int af,int type,int proto){ int fd=socket(af==23?AF_INET6:af,type,proto); if(fd<0){ws2_err(errno);return WSA_INVALID;} net_tracef("[net] socket(af=%d,type=%d,proto=%d) -> fd=%d\n",af,type,proto,fd); return (WSASOCK)(unsigned)fd; }
static WSASOCK MSABI i_ws2_WSASocketW(int af,int type,int proto,void*pi,unsigned g,uint32_t fl){ (void)pi;(void)g;(void)fl; return i_ws2_socket(af,type,proto); }
static int MSABI i_ws2_closesocket(WSASOCK s){ net_tracef("[net] closesocket(fd=%d)\n",(int)s); return close((int)s)==0?0:(ws2_err(errno),-1); }
static int MSABI i_ws2_bind(WSASOCK s,const void*name,int nl){ struct sockaddr_storage ss; int bl=sa_w2b(name,nl,&ss); if(!bl){ws2_err(EINVAL);return -1;}
    // Windows bind defaults to exclusive-but-reusable for the loopback browser port; without
    // SO_REUSEADDR a quick worker restart hits EADDRINUSE on the TIME_WAIT'd port and the local
    // server never comes up. Set it to match Windows' effective reuse behaviour on relaunch.
    { int one=1; setsockopt((int)s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one); }
    if(bind((int)s,(struct sockaddr*)&ss,bl)<0){ws2_err(errno);
        if(getenv("PELOAD_TRACE_NET")){ char b[96]; int k=snprintf(b,sizeof b,"[net] bind(fd=%d) FAIL errno=%d\n",(int)s,errno); (void)!write(2,b,k);} return -1; }
    if(getenv("PELOAD_TRACE_NET")){ struct sockaddr_in me; socklen_t ml=sizeof me; int rp=0,reqp=0;
        if(ss.ss_family==AF_INET) reqp=ntohs(((struct sockaddr_in*)&ss)->sin_port);
        if(getsockname((int)s,(struct sockaddr*)&me,&ml)==0&&me.sin_family==AF_INET) rp=ntohs(me.sin_port);
        char b[128]; int k=snprintf(b,sizeof b,"[net] bind(fd=%d) req=%d -> actual=%d\n",(int)s,reqp,rp); (void)!write(2,b,k); }
    return 0; }
// listen/accept: the local-server "browser" opens a TCP listener on 127.0.0.1 and the client
// (local-browse / local-join) connects to the advertised port. These were previously
// unimplemented ("TCP, not needed for UDP"), so the listener socket was torn down and every
// client connect was refused -> "Selected server restarted". Back them with real BSD calls.
static void net_tracef(const char*fmt,...);
static int MSABI i_ws2_listen(WSASOCK s,int backlog){
    struct sockaddr_in me; socklen_t ml=sizeof me; int port=0;
    if(getsockname((int)s,(struct sockaddr*)&me,&ml)==0 && me.sin_family==AF_INET) port=ntohs(me.sin_port);
    net_tracef("[net] listen(fd=%d,backlog=%d,port=%d)\n",(int)s,backlog,port);
    if(listen((int)s,backlog>0?backlog:16)<0){ws2_err(errno);return -1;}
    // PELOAD_ADVERTISE: the private host listens on a loopback TCP port but (private-descriptor path)
    // never writes the local-servers/<uuid>.json that local-join's discovery enumerates. Write it so
    // the client can find + connect to this host. {"port","pid"} is the format local-join parses.
    if(getenv("PELOAD_ADVERTISE") && port>1024 && g_drivec[0]){
        static int wrote; if(!wrote){ wrote=1;
            // local-join enumerates this exact directory (observed via FindFirst).
            char dir[1200]; snprintf(dir,sizeof dir,"%s/ReclaimerCache/Temp/Project Reclaimer/local-servers",g_drivec);
            { char p1[1100]; snprintf(p1,sizeof p1,"%s/ReclaimerCache",g_drivec); mkdir(p1,0755);
              snprintf(p1,sizeof p1,"%s/ReclaimerCache/Temp",g_drivec); mkdir(p1,0755);
              snprintf(p1,sizeof p1,"%s/ReclaimerCache/Temp/Project Reclaimer",g_drivec); mkdir(p1,0755); }
            mkdir(dir,0755);
            // local-join parses the filename as a UUID and validates version/variant. A real browser
            // host writes a proper RFC-4122 v4 UUID (e.g. 7a64fbc5-7070-4676-a317-766a867eb1b3); the old
            // "%08x%08x%08x%08x" of pid|port|time had version nibble 'c' (not 4) + wrong variant, so the
            // host was found+probed but then REJECTED ("No local server found"). Emit a valid v4 UUID.
            unsigned char uu[16];
            { uint64_t s=((uint64_t)getpid()<<32) ^ ((uint64_t)port<<16) ^ (uint64_t)time(0);
              struct timeval tvv; gettimeofday(&tvv,0); s ^= ((uint64_t)tvv.tv_usec<<13) ^ (uint64_t)(uintptr_t)&s;
              for(int i=0;i<16;i++){ s=s*6364136223846793005ULL+1442695040888963407ULL; uu[i]=(unsigned char)(s>>33); }
              uu[6]=(uu[6]&0x0f)|0x40;   // version 4
              uu[8]=(uu[8]&0x3f)|0x80; } // variant RFC 4122 (10xx)
            char uuid[40];
            snprintf(uuid,sizeof uuid,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                uu[0],uu[1],uu[2],uu[3],uu[4],uu[5],uu[6],uu[7],uu[8],uu[9],uu[10],uu[11],uu[12],uu[13],uu[14],uu[15]);
            char path[1200]; snprintf(path,sizeof path,"%s/%s.json",dir,uuid);
            char js[64]; int n=snprintf(js,sizeof js,"{\"port\":%d,\"pid\":%d}",port,(int)getpid());
            FILE*f=fopen(path,"wb"); if(f){ (void)!fwrite(js,1,n,f); fclose(f);
                char m[256]; int k=snprintf(m,sizeof m,"  [advertise] wrote %s = %s\n",path,js); (void)!write(2,m,k); } } }
    return 0; }
static WSASOCK MSABI i_ws2_accept(WSASOCK s,void*name,int*nl){ struct sockaddr_storage ss; socklen_t sl=sizeof ss;
    int fd=accept((int)s,(struct sockaddr*)&ss,&sl);
    if(fd<0){ ws2_err(errno); net_tracef("[net] accept(fd=%d) -> errno=%d wsa=%d\n",(int)s,errno,(int)t_wsa_err); return WSA_INVALID; }
    // Windows accepted sockets inherit the listener's non-blocking mode; BSD accept() does not.
    // The browser accept-handler (mio) expects non-blocking: a read with no data yet must yield
    // WSAEWOULDBLOCK (poll again), NOT a blocking wait / spurious 0 that it reads as EOF and closes
    // on the client before the client has sent its request. Inherit O_NONBLOCK from the listener.
    { int lfl=fcntl((int)s,F_GETFL,0); if(lfl!=-1 && (lfl&O_NONBLOCK)){ int af=fcntl(fd,F_GETFL,0); if(af!=-1) fcntl(fd,F_SETFL,af|O_NONBLOCK); } }
    if(name&&nl) sa_b2w((struct sockaddr*)&ss,name,nl);
    net_tracef("[net] accept(fd=%d) -> fd=%d\n",(int)s,fd); return (WSASOCK)(unsigned)fd; }
static int g_net_trace=-1;
static void net_tracef(const char*fmt,...){ if(g_net_trace<0) g_net_trace=getenv("PELOAD_TRACE_NET")?1:0; if(!g_net_trace) return; char b[256]; int p=snprintf(b,sizeof b,"[pid %d]",getpid()); va_list ap; va_start(ap,fmt); vsnprintf(b+p,sizeof b-p,fmt,ap); va_end(ap); ws(b); }
static int MSABI i_ws2_connect(WSASOCK s,const void*name,int nl){ struct sockaddr_storage ss; if(name)peer_gip_rec((int)s,name,nl); t_sa_send=1; int bl=sa_w2b(name,nl,&ss); t_sa_send=0; if(!bl){ws2_err(EINVAL);return -1;}
    char ips[64]="?"; int port=0; if(ss.ss_family==AF_INET){ struct sockaddr_in*si=(void*)&ss; inet_ntop(AF_INET,&si->sin_addr,ips,sizeof ips); port=ntohs(si->sin_port); }
    // PELOAD_LOOPBACK_ONLY: refuse any non-127.x connect so a forced browser host can be tested purely
    // locally (no internet master registration). Keeps all traffic on loopback.
    if(getenv("PELOAD_LOOPBACK_ONLY") && ss.ss_family==AF_INET){ struct sockaddr_in*si=(void*)&ss;
        if((ntohl(si->sin_addr.s_addr)>>24)!=127){ net_tracef("[net] connect(fd=%d,%s:%d) -> BLOCKED(loopback-only)\n",(int)s,ips,port); ws2_err(ECONNREFUSED); return -1; } }
    if(connect((int)s,(struct sockaddr*)&ss,bl)<0){ int e=errno;
        // On a non-blocking socket connect() returns EINPROGRESS (first call) or
        // EALREADY (subsequent). Windows reports BOTH as WSAEWOULDBLOCK (10035);
        // mio/tokio key on exactly that code to mean "pending, poll for writable".
        if(e==EINPROGRESS||e==EWOULDBLOCK||e==EALREADY){
            if(getenv("PELOAD_SYNC_CONNECT")){
                // Opt-in: finish the connect synchronously (poll for writability). Not the
                // default — the async path below works once the error slots agree.
                struct pollfd pf={ (int)s, POLLOUT, 0 }; int pr=poll(&pf,1,10000);
                if(pr>0){ int serr=0; socklen_t sl=sizeof serr; getsockopt((int)s,SOL_SOCKET,SO_ERROR,&serr,&sl);
                    if(serr==0){ net_tracef("[net] connect(fd=%d,%s:%d) -> OK (sync)\n",(int)s,ips,port); t_wsa_err=0; g_lasterr=0; return 0; }
                    ws2_err(serr); net_tracef("[net] connect(fd=%d,%s:%d) -> sync serr=%d wsa=%d\n",(int)s,ips,port,serr,t_wsa_err); return -1; }
                t_wsa_err=10060; g_lasterr=10060; net_tracef("[net] connect(fd=%d,%s:%d) -> sync timeout\n",(int)s,ips,port); return -1; // WSAETIMEDOUT
            }
            t_wsa_err=10035; g_lasterr=10035; // WSAEWOULDBLOCK: connect in progress
        } else ws2_err(e);
        net_tracef("[net] connect(fd=%d,%s:%d) -> errno=%d wsa=%d\n",(int)s,ips,port,e,t_wsa_err);
        return -1; }
    net_tracef("[net] connect(fd=%d,%s:%d) -> OK\n",(int)s,ips,port);
    return 0; }
static void udp_tracef(const char*dir,WSASOCK s,const struct sockaddr_storage*ss,int len){ if(!getenv("PELOAD_TRACE_UDP")) return;
    char ip[64]="?"; int port=0; if(ss->ss_family==AF_INET){ const struct sockaddr_in*si=(const void*)ss; inet_ntop(AF_INET,&si->sin_addr,ip,sizeof ip); port=ntohs(si->sin_port);}
    int lp=0; { struct sockaddr_in me; socklen_t ml=sizeof me; if(getsockname((int)s,(struct sockaddr*)&me,&ml)==0) lp=ntohs(me.sin_port);}
    char b[160]; int k=snprintf(b,sizeof b,"  [udp] %s fd=%d local:%d %s %s:%d len=%d\n",dir,(int)s,lp,strcmp(dir,"TX")?"<-":"->",ip,port,len); (void)!write(2,b,k); }
static int MSABI i_ws2_sendto(WSASOCK s,const char*buf,int len,int fl,const void*to,int tl){ struct sockaddr_storage ss; if(to)peer_gip_rec((int)s,to,tl); t_sa_send=1; int bl=to?sa_w2b(to,tl,&ss):0; t_sa_send=0; ssize_t r=sendto((int)s,buf,len,fl,bl?(struct sockaddr*)&ss:0,bl); if(r<0){ws2_err(errno);if(getenv("PELOAD_TRACE_UDP")){char b[96];int k=snprintf(b,sizeof b,"  [udp] TX fd=%d FAIL errno=%d\n",(int)s,errno);(void)!write(2,b,k);}return -1;} if(bl)udp_tracef("TX",s,&ss,(int)r); return (int)r; }
static int MSABI i_ws2_recvfrom(WSASOCK s,char*buf,int len,int fl,void*from,int*fl2){ struct sockaddr_storage ss; socklen_t sl=sizeof ss; ssize_t r=recvfrom((int)s,buf,len,fl,(struct sockaddr*)&ss,&sl); if(r<0){ws2_err(errno);return -1;} if(r>=0)udp_tracef("RX",s,&ss,(int)r);
    if(getenv("PELOAD_TRACE_SCATTER") && r>200){ void*ra=__builtin_return_address(0); char b[180]; int k=snprintf(b,sizeof b,"  [scatter] recvfrom fd=%d wire=%d bufcap=%d caller=%p%s\n",(int)s,(int)r,len,ra,(r==len)?" (==cap)":""); (void)!write(2,b,k); }
    if(from&&fl2){ sa_b2w((struct sockaddr*)&ss,from,fl2); peer_gip_restore((int)s,from); } return (int)r; }
// PELOAD_TRACE_WIRE: hex+ascii dump of TCP payloads (to RE the local-server query/join protocol).
static void wire_dump(const char*dir,int fd,const char*buf,int n){ if(!getenv("PELOAD_TRACE_WIRE")||n<=0) return;
    int cap=getenv("PELOAD_TRACE_WIRE_FULL")?560:128; int m=n>cap?cap:n; char o[2400]; int k=snprintf(o,sizeof o,"  [wire] %s fd=%d n=%d: ",dir,fd,n);
    for(int i=0;i<m&&k<1200;i++) k+=snprintf(o+k,sizeof o-k,"%02x",(unsigned char)buf[i]);
    k+=snprintf(o+k,sizeof o-k," | "); for(int i=0;i<m&&k<2300;i++){ unsigned char c=buf[i]; o[k++]=(c>=32&&c<127)?c:'.'; }
    o[k++]='\n'; (void)!write(2,o,k); }
static int MSABI i_ws2_send(WSASOCK s,const char*buf,int len,int fl){ ssize_t r=send((int)s,buf,len,fl); if(r<0){ws2_err(errno);net_tracef("[net] send(fd=%d,len=%d) -> errno=%d wsa=%d\n",(int)s,len,errno,t_wsa_err);return -1;} net_tracef("[net] send(fd=%d,len=%d) -> %d\n",(int)s,len,(int)r); wire_dump("TX",(int)s,buf,(int)r); return (int)r; }
static int MSABI i_ws2_recv(WSASOCK s,char*buf,int len,int fl){ ssize_t r=recv((int)s,buf,len,fl); if(r<0){ws2_err(errno);net_tracef("[net] recv(fd=%d,len=%d) -> errno=%d wsa=%d\n",(int)s,len,errno,t_wsa_err);return -1;} net_tracef("[net] recv(fd=%d,len=%d) -> %d\n",(int)s,len,(int)r); wire_dump("RX",(int)s,buf,(int)r); return (int)r; }
static int MSABI i_ws2_shutdown(WSASOCK s,int how){ return shutdown((int)s,how)==0?0:(ws2_err(errno),-1); }
static int MSABI i_ws2_getsockname(WSASOCK s,void*name,int*nl){ struct sockaddr_storage ss; socklen_t sl=sizeof ss; if(getsockname((int)s,(struct sockaddr*)&ss,&sl)<0){ws2_err(errno);return -1;} sa_b2w((struct sockaddr*)&ss,name,nl); return 0; }
static int MSABI i_ws2_getpeername(WSASOCK s,void*name,int*nl){ struct sockaddr_storage ss; socklen_t sl=sizeof ss; if(getpeername((int)s,(struct sockaddr*)&ss,&sl)<0){ws2_err(errno);return -1;} sa_b2w((struct sockaddr*)&ss,name,nl); return 0; }
static int MSABI i_ws2_setsockopt(WSASOCK s,int lvl,int opt,const char*val,int len){
    // Winsock SO_RCVTIMEO(0x1006)/SO_SNDTIMEO(0x1005) at SOL_SOCKET(0xffff) pass a 4-byte DWORD of
    // MILLISECONDS; BSD/macOS expect a 16-byte struct timeval. Passing the raw 4 bytes gives EINVAL(-1),
    // which made local-join's probe caller (sub_14058cfe0) take its failure path and NEVER send its query
    // -> "No local server found" even though connect succeeded. Convert ms -> timeval.
    if(lvl==0xffff && (opt==0x1005||opt==0x1006) && val && len>=4){
        uint32_t ms=*(const uint32_t*)val; struct timeval tv; tv.tv_sec=(time_t)(ms/1000); tv.tv_usec=(suseconds_t)((ms%1000)*1000);
        if(setsockopt((int)s,SOL_SOCKET,opt==0x1006?SO_RCVTIMEO:SO_SNDTIMEO,&tv,sizeof tv)==0) return 0;
        ws2_err(errno); return -1; }
    if(setsockopt((int)s,lvl,opt,val,len)==0) return 0; if(errno==ENOPROTOOPT) return 0; /*lenient: ignore unknown opts*/ ws2_err(errno); return -1; }
static int MSABI i_ws2_getsockopt(WSASOCK s,int lvl,int opt,char*val,int*len){ socklen_t sl=len?*len:0; int rc=getsockopt((int)s,lvl,opt,val,&sl);
    if(g_net_trace>0 && opt==0x1007/*SO_ERROR*/){ char b[96]; int k=snprintf(b,sizeof b,"[net] getsockopt(fd=%d,SO_ERROR) rc=%d val=%d\n",(int)s,rc,(rc==0&&val)?*(int*)val:-1); (void)!write(2,b,k); }
    if(rc==0){ if(len)*len=sl; return 0;} if(errno==ENOPROTOOPT){ if(val&&len&&*len>=4)*(int*)val=0; return 0;} ws2_err(errno); return -1; }
static int MSABI i_ws2_ioctlsocket(WSASOCK s,long cmd,uint32_t*argp){ net_tracef("[net] ioctlsocket(fd=%d,cmd=0x%lx,arg=%u)\n",(int)s,(unsigned long)cmd,argp?*argp:0); if(ioctl((int)s,(unsigned long)cmd,argp)==0) return 0; ws2_err(errno); return -1; }
// select: Windows fd_set = { u_int count; SOCKET array[64] }. Bridge via poll().
typedef struct { uint32_t cnt; WSASOCK arr[64]; } WSAFDSET;
static int MSABI i_ws2_select(int nfds,WSAFDSET*rd,WSAFDSET*wr,WSAFDSET*ex,const void*tv){ (void)nfds;
    net_tracef("[net] select(r=%u,w=%u,e=%u)\n",rd?rd->cnt:0,wr?wr->cnt:0,ex?ex->cnt:0);
    struct pollfd pfd[192]; int np=0; WSAFDSET*sets[3]={rd,wr,ex}; short ev[3]={POLLIN,POLLOUT,POLLPRI};
    for(int si=0;si<3;si++){ WSAFDSET*S=sets[si]; if(!S)continue; for(uint32_t i=0;i<S->cnt&&np<192;i++){ int fd=(int)S->arr[i]; int f=-1; for(int k=0;k<np;k++) if(pfd[k].fd==fd){f=k;break;} if(f<0){ pfd[np].fd=fd; pfd[np].events=0; pfd[np].revents=0; f=np++; } pfd[f].events|=ev[si]; } }
    int to=-1; if(tv){ const int32_t*t=tv; to=t[0]*1000 + t[1]/1000; }
    int r=poll(pfd,np,to); if(r<0){ws2_err(errno);return -1;}
    if(g_net_trace>0){ for(int k=0;k<np;k++){ char b[96]; int kk=snprintf(b,sizeof b,"[net]   poll fd=%d revents=0x%x%s%s%s%s\n",pfd[k].fd,pfd[k].revents,(pfd[k].revents&POLLOUT)?" OUT":"",(pfd[k].revents&POLLIN)?" IN":"",(pfd[k].revents&POLLHUP)?" HUP":"",(pfd[k].revents&POLLERR)?" ERR":""); (void)!write(2,b,kk);} }
    for(int si=0;si<3;si++){ WSAFDSET*S=sets[si]; if(!S)continue; WSASOCK keep[64]; uint32_t kc=0; for(uint32_t i=0;i<S->cnt;i++){ int fd=(int)S->arr[i]; for(int k=0;k<np;k++) if(pfd[k].fd==fd && (pfd[k].revents&(ev[si]|POLLHUP|POLLERR))){ if(kc<64)keep[kc++]=S->arr[i]; break; } } S->cnt=kc; for(uint32_t i=0;i<kc;i++)S->arr[i]=keep[i]; }
    return r; }
static int MSABI i_ws2___WSAFDIsSet(WSASOCK s,WSAFDSET*set){ if(!set)return 0; for(uint32_t i=0;i<set->cnt;i++) if(set->arr[i]==s) return 1; return 0; }
static uint32_t MSABI i_ws2_htonl(uint32_t x){ return htonl(x); }
static uint32_t MSABI i_ws2_ntohl(uint32_t x){ return ntohl(x); }
static uint16_t MSABI i_ws2_htons(uint16_t x){ return htons(x); }
static uint16_t MSABI i_ws2_ntohs(uint16_t x){ return ntohs(x); }
static uint32_t MSABI i_ws2_inet_addr(const char*c){ return inet_addr(c); }
static int MSABI i_ws2_WSASendMsg(WSASOCK s,uint8_t*msg,uint32_t flags,uint32_t*sent,uint8_t*ov,void*cr); // fwd
static int MSABI i_ws2_WSARecvMsg(WSASOCK s,uint8_t*msg,uint32_t*recvd,uint8_t*ov,void*cr);             // fwd
static int MSABI i_ws2_WSAIoctl(WSASOCK s,uint32_t code,void*ib,uint32_t ibl,void*ob,uint32_t obl,uint32_t*ret,void*ov,void*cb){
    (void)ib;(void)ibl;(void)ov;(void)cb;
    // SIO_BASE_HANDLE (0x48000022) and the BSP_HANDLE variants: mio needs the underlying
    // socket handle to feed AFD_POLL. We have no layered providers, so the base handle IS
    // the socket. Returning it (not refusing) is what lets mio register for readiness.
    if(getenv("PELOAD_TRACE_NET")){ char m[80]; snprintf(m,sizeof m,"[net] WSAIoctl(fd=%d,code=0x%x)\n",(int)s,code); ws(m); }
    if(code==0x48000022||code==0x4800001D||code==0x4800001A||code==0x4800001C){
        if(ob&&obl>=8) *(uint64_t*)ob=(uint64_t)(uintptr_t)s; if(ret)*ret=8; t_wsa_err=0; return 0; }
    // SIO_KEEPALIVE_VALS (0x98000004): struct tcp_keepalive {onoff, time_ms, interval_ms}.
    // hyper/socket2 set this on every connection; refusing it aborts the connect. Apply it.
    if(code==0x98000004){ if(ib&&ibl>=4){ uint32_t*kv=ib; int on=kv[0]?1:0; setsockopt((int)s,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof on);
#ifdef TCP_KEEPALIVE
            if(on&&ibl>=8){ int sec=(int)(kv[1]/1000); if(sec<1)sec=1; setsockopt((int)s,IPPROTO_TCP,TCP_KEEPALIVE,&sec,sizeof sec); }
#endif
        } if(ret)*ret=0; t_wsa_err=0; return 0; }
    // SIO_GET_EXTENSION_FUNCTION_POINTER (0xC8000006): now that WSASendMsg/WSARecvMsg are implemented,
    // hand back their real pointers (the native-UDP private bridge resolves them this way too). Match
    // the GUID in ib; refuse anything else so callers that can fall back still do.
    if(code==0xC8000006 && ib && ibl>=16 && ob && obl>=8){
        static const uint8_t G_RECVMSG[16]={0xc8,0xd7,0x89,0xf6,0x1f,0x6f,0x6b,0x43,0x8a,0x59,0xfd,0xc5,0x2e,0xde,0xf8,0xd8};
        static const uint8_t G_SENDMSG[16]={0x12,0xe7,0x41,0xa4,0x4f,0x75,0xca,0x43,0x84,0xa7,0x0d,0xee,0x44,0xcf,0x60,0x6d};
        if(!memcmp(ib,G_RECVMSG,16)){ *(void**)ob=(void*)i_ws2_WSARecvMsg; if(ret)*ret=8; t_wsa_err=0;
            if(getenv("PELOAD_TRACE_NET"))ws("[net] WSAIoctl -> WSARecvMsg ptr\n"); return 0; }
        if(!memcmp(ib,G_SENDMSG,16)){ *(void**)ob=(void*)i_ws2_WSASendMsg; if(ret)*ret=8; t_wsa_err=0;
            if(getenv("PELOAD_TRACE_NET"))ws("[net] WSAIoctl -> WSASendMsg ptr\n"); return 0; } }
    if(ret)*ret=0; t_wsa_err=10022 /*WSAEINVAL*/; return -1; }
// map a ws2_32 ordinal to a real impl (0 = none, fall back to stub)
static void* ws2_real_ord(uint32_t o){ switch(o){
    case 1: return (void*)i_ws2_accept;
    case 2: return (void*)i_ws2_bind; case 3: return (void*)i_ws2_closesocket; case 4: return (void*)i_ws2_connect;
    case 13: return (void*)i_ws2_listen;
    case 5: return (void*)i_ws2_getpeername; case 6: return (void*)i_ws2_getsockname; case 7: return (void*)i_ws2_getsockopt;
    case 8: return (void*)i_ws2_htonl; case 9: return (void*)i_ws2_htons; case 10: return (void*)i_ws2_ioctlsocket;
    case 11: return (void*)i_ws2_inet_addr; case 14: return (void*)i_ws2_ntohl; case 15: return (void*)i_ws2_ntohs;
    case 16: return (void*)i_ws2_recv; case 17: return (void*)i_ws2_recvfrom; case 18: return (void*)i_ws2_select;
    case 19: return (void*)i_ws2_send; case 20: return (void*)i_ws2_sendto; case 21: return (void*)i_ws2_setsockopt;
    case 22: return (void*)i_ws2_shutdown; case 23: return (void*)i_ws2_socket;
    case 111: return (void*)i_ws2_WSAGetLastError; case 115: return (void*)i_ws2_WSAStartup; case 116: return (void*)i_ws2_WSACleanup;
    case 151: return (void*)i_ws2___WSAFDIsSet; default: return 0; } }
// ---- getaddrinfo / GetAddrInfoW: Windows ADDRINFO layout over the system resolver ----
// Windows ADDRINFOA/W (x64): int flags@0,family@4,socktype@8,protocol@12; size_t addrlen@16;
// <char*|wchar*> canonname@24; sockaddr* addr@32; next@40  => 48 bytes.
typedef struct WinAI { int32_t flags,family,socktype,protocol; uint64_t addrlen; void* canonname; void* addr; struct WinAI* next; } WinAI;
// Windows AF_INET6 = 23; BSD = 30. AF_INET = 2 on both. Translate both ways.
static int ai_fam_w2b(int f){ return f==23?AF_INET6:f; }
static int ai_fam_b2w(int f){ return f==AF_INET6?23:f; }
// Build a Windows sockaddr (16 / 28 bytes) from a BSD one. Returns bytes written.
static int ai_sa_b2w(const struct sockaddr*b,uint8_t*w){
    if(b->sa_family==AF_INET){ const struct sockaddr_in*s=(const void*)b; w[0]=2;w[1]=0; memcpy(w+2,&s->sin_port,2); memcpy(w+4,&s->sin_addr,4); memset(w+8,0,8); return 16; }
    if(b->sa_family==AF_INET6){ const struct sockaddr_in6*s=(const void*)b; w[0]=23;w[1]=0; memcpy(w+2,&s->sin6_port,2); memcpy(w+4,&s->sin6_flowinfo,4); memcpy(w+8,&s->sin6_addr,16); memcpy(w+24,&s->sin6_scope_id,4); return 28; }
    return 0; }
static int g_ai_trace=-1;
static WinAI* ai_build(struct addrinfo*br,int wide){
    WinAI *head=0,*tail=0;
    for(struct addrinfo*a=br;a;a=a->ai_next){
        uint8_t sabuf[32]; int sl=a->ai_addr?ai_sa_b2w(a->ai_addr,sabuf):0; if(!sl) continue;
        WinAI* w=calloc(1,sizeof *w); if(!w) break;
        w->flags=0; w->family=ai_fam_b2w(a->ai_family); w->socktype=a->ai_socktype; w->protocol=a->ai_protocol;
        w->addrlen=(uint64_t)sl; w->addr=malloc(sl); memcpy(w->addr,sabuf,sl);
        if(a->ai_canonname){ size_t n=strlen(a->ai_canonname);
            if(wide){ uint16_t* cn=malloc((n+1)*2); for(size_t i=0;i<n;i++)cn[i]=(uint8_t)a->ai_canonname[i]; cn[n]=0; w->canonname=cn; }
            else { w->canonname=strdup(a->ai_canonname); } }
        if(!head)head=w; else tail->next=w; tail=w;
    }
    return head; }
static void ai_free(WinAI* w){ while(w){ WinAI* n=w->next; free(w->addr); free(w->canonname); free(w); w=n; } }
static int MSABI i_ws2_getaddrinfo(const char* node,const char* service,const WinAI* hintsW,WinAI** resW){
    if(resW)*resW=0;
    struct addrinfo bh,*br=0,*ph=0;
    if(hintsW){ memset(&bh,0,sizeof bh); bh.ai_family=ai_fam_w2b(hintsW->family); bh.ai_socktype=hintsW->socktype; bh.ai_protocol=hintsW->protocol;
        int wf=hintsW->flags; int bf=0; if(wf&0x1)bf|=AI_PASSIVE; if(wf&0x2)bf|=AI_CANONNAME; if(wf&0x4)bf|=AI_NUMERICHOST; bh.ai_flags=bf; ph=&bh; }
    int rc=getaddrinfo(node,service,ph,&br);
    if(g_ai_trace<0) g_ai_trace=getenv("PELOAD_TRACE_NET")?1:0;
    if(g_ai_trace){ char m[256]; snprintf(m,sizeof m,"[net] getaddrinfo(%s,%s) rc=%d%s\n",node?node:"(null)",service?service:"(null)",rc,rc?" FAIL":""); ws(m); }
    if(rc!=0||!br){ if(br)freeaddrinfo(br); return 11001; /*WSAHOST_NOT_FOUND*/ }
    WinAI* head=ai_build(br,0); freeaddrinfo(br);
    if(!head) return 11001; if(resW)*resW=head; return 0; }
static void MSABI i_ws2_freeaddrinfo(WinAI* w){ ai_free(w); }
// GetAddrInfoW: node/service are UTF-16; canonname out is wide.
static int w16_to_utf8(const uint16_t* w,char* out,int cap){ int i=0; if(!w){ if(cap)out[0]=0; return 0; } for(; w[i]&&i<cap-1;i++) out[i]=(char)(w[i]&0xff); out[i]=0; return i; }
static int MSABI i_ws2_GetAddrInfoW(const uint16_t* node,const uint16_t* service,const WinAI* hintsW,WinAI** resW){
    char nb[512],sb[64]; w16_to_utf8(node,nb,sizeof nb); w16_to_utf8(service,sb,sizeof sb);
    if(resW)*resW=0;
    struct addrinfo bh,*br=0,*ph=0;
    if(hintsW){ memset(&bh,0,sizeof bh); bh.ai_family=ai_fam_w2b(hintsW->family); bh.ai_socktype=hintsW->socktype; bh.ai_protocol=hintsW->protocol;
        int wf=hintsW->flags; int bf=0; if(wf&0x1)bf|=AI_PASSIVE; if(wf&0x2)bf|=AI_CANONNAME; if(wf&0x4)bf|=AI_NUMERICHOST; bh.ai_flags=bf; ph=&bh; }
    int rc=getaddrinfo(node?nb:0,service?sb:0,ph,&br);
    if(g_ai_trace<0) g_ai_trace=getenv("PELOAD_TRACE_NET")?1:0;
    if(g_ai_trace){ char m[256]; snprintf(m,sizeof m,"[net] GetAddrInfoW(%s,%s) rc=%d%s\n",node?nb:"(null)",service?sb:"(null)",rc,rc?" FAIL":""); ws(m); }
    if(rc!=0||!br){ if(br)freeaddrinfo(br); return 11001; }
    WinAI* head=ai_build(br,1); freeaddrinfo(br);
    if(!head) return 11001; if(resW)*resW=head; return 0; }
static void MSABI i_ws2_FreeAddrInfoW(WinAI* w){ ai_free(w); }
// WSASend: gathered send over a connected socket. WSABUF{ULONG len@0; char* buf@8} = 16B.
static int iocp_post_fd(int fd,uint64_t bytes,void*ov); // defined after the IOCP block
static int MSABI i_ws2_WSASend(WSASOCK s,uint8_t*bufs,uint32_t cnt,uint32_t*sent,uint32_t flags,uint8_t*ov,void*cr){
    (void)flags;(void)cr; uint32_t total=0; int err=0;
    for(uint32_t i=0;i<cnt;i++){ uint32_t len=*(uint32_t*)(bufs+(size_t)i*16); char* p=*(char**)(bufs+(size_t)i*16+8);
        uint32_t off=0; while(off<len){ ssize_t k=send((int)s,p+off,len-off,0); if(k<0){ ws2_err(errno); err=1; break; } off+=(uint32_t)k; }
        total+=off; if(err) break; }
    net_tracef("[net] WSASend(fd=%d,bufs=%u) -> %u%s\n",(int)s,cnt,total,err?" ERR":"");
    if(sent)*sent=total;
    if(ov){ *(uint64_t*)(ov+0)=0; *(uint64_t*)(ov+8)=total; iocp_post_fd((int)s,total,ov); return err&&total==0?-1:0; }
    if(err&&total==0) return -1; return 0; }
// WSASendMsg / WSARecvMsg: the native-UDP private-bridge transport uses these (both IMPORTED), NOT
// sendto/recvfrom. They were unimplemented (generic stub -> return 0 = fake success, zero bytes on the
// wire), so the client created its 3 UDP sockets but no datagram ever crossed to the host -> the
// private bridge never formed -> gameplay_join_verified stayed false. Implement them over sendto/
// recvfrom with gather/scatter. WSAMSG (x64): name@0, namelen@8, lpBuffers@16, dwBufferCount@24,
// Control{len@32,buf@40}, dwFlags@48. WSABUF{len@0,buf@8}=16B. The dest/src address goes through the
// same sa_w2b/sa_b2w private-port remap (t_sa_send marks the send direction for PELOAD_UDP_ROLE).
static int MSABI i_ws2_WSASendMsg(WSASOCK s,uint8_t*msg,uint32_t flags,uint32_t*sent,uint8_t*ov,void*cr){
    (void)flags;(void)cr; if(!msg){ws2_err(EFAULT);return -1;}
    void*name=*(void**)(msg+0); int namelen=*(int*)(msg+8);
    uint8_t*bufs=*(uint8_t**)(msg+16); uint32_t cnt=*(uint32_t*)(msg+24);
    static __thread char gbuf[65536]; uint32_t tot=0;
    for(uint32_t i=0;i<cnt;i++){ uint32_t blen=*(uint32_t*)(bufs+(size_t)i*16); char*bp=*(char**)(bufs+(size_t)i*16+8);
        if(blen>sizeof gbuf-tot) blen=(uint32_t)(sizeof gbuf-tot); if(bp&&blen) memcpy(gbuf+tot,bp,blen); tot+=blen; if(tot>=sizeof gbuf) break; }
    if(name)peer_gip_rec((int)s,name,namelen);
    struct sockaddr_storage ss; t_sa_send=1; int bl=name?sa_w2b(name,namelen,&ss):0; t_sa_send=0;
    ssize_t r=sendto((int)s,gbuf,tot,0,bl?(struct sockaddr*)&ss:0,bl);
    if(r<0){ ws2_err(errno); if(ov){*(uint64_t*)(ov+0)=0;*(uint64_t*)(ov+8)=0;} return -1; }
    if(sent)*sent=(uint32_t)r; if(bl)udp_tracef("TX",s,&ss,(int)r);
    if(ov){ *(uint64_t*)(ov+0)=0; *(uint64_t*)(ov+8)=(uint64_t)r; iocp_post_fd((int)s,(uint64_t)r,ov); }
    return 0; }
static int MSABI i_ws2_WSARecvMsg(WSASOCK s,uint8_t*msg,uint32_t*recvd,uint8_t*ov,void*cr){
    (void)cr; if(!msg){ws2_err(EFAULT);return -1;}
    void*name=*(void**)(msg+0); int*namelenp=(int*)(msg+8);
    uint8_t*bufs=*(uint8_t**)(msg+16); uint32_t cnt=*(uint32_t*)(msg+24);
    static __thread char rbuf[65536];
    struct sockaddr_storage ss; socklen_t sl=sizeof ss;
    ssize_t r=recvfrom((int)s,rbuf,sizeof rbuf,0,(struct sockaddr*)&ss,&sl);
    if(r<0){ ws2_err(errno); return -1; }
    uint32_t cap=0; for(uint32_t i=0;i<cnt;i++) cap+=*(uint32_t*)(bufs+(size_t)i*16);
    uint32_t off=0; for(uint32_t i=0;i<cnt && off<(uint32_t)r;i++){ uint32_t blen=*(uint32_t*)(bufs+(size_t)i*16); char*bp=*(char**)(bufs+(size_t)i*16+8);
        uint32_t n=(uint32_t)r-off; if(n>blen)n=blen; if(bp&&n) memcpy(bp,rbuf+off,n); off+=n; }
    if((getenv("PELOAD_TRACE_SCATTER") && r>200) || (uint32_t)r>cap){ char b[160]; int k=snprintf(b,sizeof b,"  [scatter] WSARecvMsg fd=%d wire=%d bufs=%u cap=%u delivered=%u%s\n",(int)s,(int)r,cnt,cap,off,((uint32_t)r>cap)?" TRUNCATED!":""); (void)!write(2,b,k); }
    if(recvd)*recvd=(uint32_t)r;
    if(name&&namelenp){ int wl=*namelenp; sa_b2w((struct sockaddr*)&ss,name,&wl); *namelenp=wl; peer_gip_restore((int)s,name); }
    *(uint32_t*)(msg+48)=0; /* dwFlags out */ *(uint32_t*)(msg+32)=0; /* Control.len out (no cmsg) */
    udp_tracef("RX",s,&ss,(int)r);
    if(ov){ *(uint64_t*)(ov+0)=0; *(uint64_t*)(ov+8)=(uint64_t)r; iocp_post_fd((int)s,(uint64_t)r,ov); }
    return 0; }
static void* ws2_real_name(const char*n){
    if(!strcmp(n,"WSASend")) return (void*)i_ws2_WSASend;
    if(!strcmp(n,"WSASendMsg")) return (void*)i_ws2_WSASendMsg;
    if(!strcmp(n,"WSARecvMsg")) return (void*)i_ws2_WSARecvMsg;
    if(!strcmp(n,"WSASocketW")||!strcmp(n,"WSASocketA")) return (void*)i_ws2_WSASocketW;
    if(!strcmp(n,"WSAIoctl")) return (void*)i_ws2_WSAIoctl;
    if(!strcmp(n,"WSASetLastError")) return (void*)i_ws2_WSASetLastError;
    if(!strcmp(n,"WSARecvFrom")) return (void*)i_ws2_recvfrom; // close-enough arg prefix for fallback probing
    if(!strcmp(n,"getaddrinfo")) return (void*)i_ws2_getaddrinfo;
    if(!strcmp(n,"freeaddrinfo")) return (void*)i_ws2_freeaddrinfo;
    if(!strcmp(n,"GetAddrInfoW")) return (void*)i_ws2_GetAddrInfoW;
    if(!strcmp(n,"FreeAddrInfoW")) return (void*)i_ws2_FreeAddrInfoW;
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
    { char t[96]; int k=snprintf(t,sizeof t,"  [i_ExitProcess] code=%u present_started=%d hold=%d\n",c,g_present_started,getenv("PELOAD_HOLD_WINDOW")?1:0); (void)!write(2,t,k); }
    // PELOAD_PROBE_EXIT0: force the probe-engine-worker to report success so the
    // parent orchestrator proceeds to launch its real client (experiment).
    if(c && getenv("PELOAD_PROBE_EXIT0") && strstr(g_cmdlineA,"probe-engine-worker")){
        ws("  [probe-exit0] forcing worker exit 0 (was "); { char t[16]; int n=snprintf(t,sizeof t,"%u)\n",c); ws(t);} _exit(0); }
    // PELOAD_HOLD_WINDOW: keep the worker process alive so the present thread keeps the
    // rendered window on screen (instead of exiting on the "no map selected" abort).
    if(getenv("PELOAD_HOLD_WINDOW") && g_present_started){ ws("  [hold-window] worker exit suppressed; keeping window up\n"); for(;;){ struct timespec ts={1,0}; nanosleep(&ts,0); } }
    _exit((int)c); }
static void   MSABI i_TerminateProcess(void *h,uint32_t c){ (void)h; _exit((int)c); }
static uint32_t MSABI i_GetModuleFileNameW(void*m,uint16_t*buf,uint32_t sz){ (void)m; uint32_t i=0; for(; i<sz && g_exewin[i]; i++) buf[i]=g_exewin[i]; if(i<sz) buf[i]=0; if(getenv("PELOAD_TRACE_SPAWN")){ char n[1100]; uint32_t k=0; for(;k<i&&k<1099;k++)n[k]=(char)(buf[k]&0xff); n[k]=0; char t[1200]; snprintf(t,sizeof t,"  [GetModuleFileNameW] mod=%p -> %s\n",m,n); ws(t);} return i; }
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
    if(getenv("PELOAD_TRACE_SPAWN")) ws("  [CreateProcessW ENTRY]\n");
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
    // PELOAD_LOCAL_RUNTIME: override the RECLAIMER_LOCAL_RUNTIME mode handshake the
    // orchestrator hands the worker (default "menu:0"). "__unset__" removes it (which
    // routes engine_startup to stage 2 = the map-catalog build). Lets us flip the
    // disposable probe toward the full-client/catalog path.
    { const char* lro=getenv("PELOAD_LOCAL_RUNTIME");
      if(lro){ if(!strcmp(lro,"__unset__")){ unsetenv("RECLAIMER_LOCAL_RUNTIME"); ws("  [runtime-override] unset RECLAIMER_LOCAL_RUNTIME\n"); }
               else { setenv("RECLAIMER_LOCAL_RUNTIME",lro,1); ws("  [runtime-override] RECLAIMER_LOCAL_RUNTIME="); ws(lro); ws("\n"); } } }
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
    // Dump the RECLAIMER_* handshake env the worker inherits (mode is encoded here, not argv).
    if(getenv("PELOAD_TRACE_ENV")){ extern char**environ; for(char**e=environ;*e;e++) if(!strncmp(*e,"RECLAIMER_",10)){ ws("    [penv] "); ws(*e); ws("\n"); } }
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
    if(getenv("PELOAD_TRACE_IPC")){ char b[96]; int k=snprintf(b,sizeof b,"  [DuplicateHandle] srcproc=%p src=%p opt=0x%x\n",sp,src,opt); (void)!write(2,b,k); }
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
// A Windows environment block: a run of UTF-16 "NAME=VALUE\0" strings terminated
// by one extra NUL. The old stub returned an EMPTY block ({0,0}); Rust's
// std::sys::windows::os::env()/Command path then read a varless environment and,
// finding no PATH/var, returned ErrorKind::NotFound -> the caller surfaced the
// stale GetLastError (2, from an earlier benign ModInfo.json probe) as a fatal
// "os error 2" (e.g. the dedicated server's "Cannot start '<game>'"). Build a real
// block from `environ` so env reads (and %PATH% program resolution) succeed.
// (environ is declared extern above, near CreateProcessW.)
static uint16_t* build_env_block_w(void){
    size_t need=1; // trailing extra NUL
    for(char**e=environ; e&&*e; e++) need += strlen(*e)+1; // each entry + its NUL (1 u16 each, ASCII)
    uint16_t *blk=(uint16_t*)malloc((need+1)*sizeof(uint16_t)); if(!blk) return 0;
    size_t o=0;
    for(char**e=environ; e&&*e; e++){ const char*s=*e; if(s[0]=='=') continue; // skip drive-cwd pseudo-vars
        for(const char*p=s; *p; p++) blk[o++]=(uint8_t)*p; blk[o++]=0; }
    blk[o++]=0; // extra terminating NUL
    return blk;
}
static uint16_t* MSABI i_GetEnvironmentStringsW(void){ uint16_t*b=build_env_block_w();
    if(getenv("PELOAD_TRACE_ENV")){ int nv=0; for(char**e=environ;e&&*e;e++)nv++; char t[64]; snprintf(t,sizeof t,"  [GetEnvStringsW] %d vars\n",nv); ws(t); }
    if(!b){ static uint16_t empty[2]={0,0}; return empty; } return b; }
static int    MSABI i_FreeEnvironmentStringsW(void*p){ if(p) free(p); return 1; }
// ANSI variant: same block, one byte per char.
static char* MSABI i_GetEnvironmentStrings(void){ size_t need=1; for(char**e=environ;e&&*e;e++) need+=strlen(*e)+1;
    char*blk=(char*)malloc(need+1); if(!blk){ static char empty[2]={0,0}; return empty; } size_t o=0;
    for(char**e=environ;e&&*e;e++){ const char*s=*e; if(s[0]=='=') continue; size_t l=strlen(s); memcpy(blk+o,s,l); o+=l; blk[o++]=0; } blk[o++]=0; return blk; }
static int MSABI i_FreeEnvironmentStringsA(void*p){ if(p) free(p); return 1; }
static uint32_t MSABI i_GetTickCount(void){ return (uint32_t)(mach_absolute_time()/1000000ULL); }
// NtWriteFile: route writes to our stdin/out/err pseudo-handles (1/2/3)
static int MSABI i_NtWriteFile(void*h,void*ev,void*apc,void*apcctx,void*iosb,const void*buf,uint32_t len,void*off,void*key){
    (void)ev;(void)apc;(void)apcctx;(void)key; intptr_t hv=(intptr_t)h;
    int fd = (hv==2)?1 : (hv==3)?2 : (hv>=0x1000)?(int)(hv-0x1000) : -1;
    if(fd>=0 && len>8 && len<4096 && getenv("PELOAD_TRACE_BT")){ const char*w=getenv("PELOAD_TRACE_BT");
        if(memmem(buf,len,w,strlen(w))){ (void)!write(2,"  [BT-Nt] matched; stack exe/halo3 returns:\n",44);
            uint64_t*sp=(uint64_t*)__builtin_frame_address(0); int pr=0;
            for(int i=0;i<4000 && pr<60;i++){ uint64_t v=sp[i];
                if(v>=0x140000000ULL&&v<0x142000000ULL){ char b[48]; int m=snprintf(b,sizeof b,"     exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,b,m); pr++; }
                else if(v>=0x180000000ULL&&v<0x187000000ULL){ char b[48]; int m=snprintf(b,sizeof b,"     halo3+0x%llx\n",(unsigned long long)(v-0x180000000ULL)); (void)!write(2,b,m); pr++; } } } }
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
static void* g_last_hwnd;   // most-recently created real window (reported as foreground/active)
static void*  MSABI i_CreateWindowExW(uint32_t ex,const void*cls,const void*name,uint32_t style,int x,int y,int w,int h,void*parent,void*menu,void*inst,void*param){
    WWin* win=calloc(1,sizeof *win); if(!win) return (void*)0x4857;
    win->magic=WWIN_MAGIC; win->wndproc=wcls_find(cls); win->x=x; win->y=y; win->w=w>0?w:1280; win->h=h>0?h:720;
    g_last_hwnd=win;
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
// The engine's present loop calls MsgWaitForMultipleObjectsEx and only keeps
// looping (rendering + presenting each frame) when it returns WAIT_TIMEOUT
// (0x102); any other value makes it exit and tear down the GL context. Return
// 0x102 after a short sleep (cap the engine's timeout to ~16ms = ~60fps) so the
// menu/loading-screen keeps rendering instead of exiting after one frame.
// Lightweight, NON-intrusive frame grab: called from the message-pump stub
// (between frames, no engine borrow / no mid-draw GL state to corrupt). Plain
// glReadPixels of the current drawable into a BMP; no FBO/blit juggling (that
// re-enters and breaks the frontend). PELOAD_SHOT gates it.
static void gl_grab_simple(void){
    if(!getenv("PELOAD_SHOT")) return;
    static int every; if((every++ % 15)!=0) return;   // ~every 15th frame
    void*(*cur)(void)=dlsym(RTLD_DEFAULT,"CGLGetCurrentContext"); if(cur&&!cur()) return;
    void(*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels");
    void(*rb)(unsigned)=dlsym(RTLD_DEFAULT,"glReadBuffer");
    void(*ge)(void); unsigned(*gerr)(void)=dlsym(RTLD_DEFAULT,"glGetError"); (void)ge;
    if(!rp) return; int W=1280,H=720;
    // Engine never SwapBuffers, so the rendered frame lives in GL_BACK, not FRONT.
    if(rb) rb(getenv("PELOAD_SHOT_FRONT")?0x0404/*FRONT*/:0x0405/*GL_BACK*/);
    unsigned char*px=calloc(1,(size_t)W*H*4); if(!px) return;
    if(gerr) gerr(); rp(0,0,W,H,0x1908/*RGBA*/,0x1401/*UBYTE*/,px); unsigned e=gerr?gerr():0;
    const char*home=getenv("HOME"); char path[512]; snprintf(path,sizeof path,"%s/reclaimer_menu_latest.bmp",home?home:"/tmp");
    FILE*f=fopen(path,"wb"); if(f){ uint32_t isz=(uint32_t)W*H*4,fsz=54+isz,o=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&o,4);
        uint8_t ih[40]={0}; uint32_t v40=40; memcpy(ih,&v40,4); memcpy(ih+4,&W,4); memcpy(ih+8,&H,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
        fwrite(fh,1,14,f); fwrite(ih,1,40,f);
        for(int i=0;i<W*H;i++){ uint8_t*p=px+i*4; uint8_t b[4]={p[2],p[1],p[0],255}; fwrite(b,1,4,f);} fclose(f);
        static int once; if(!once){ once=1; char m[160]; int k=snprintf(m,sizeof m,"  [gl_grab_simple] %dx%d readpixels err=0x%x -> %s\n",W,H,e,path); (void)!write(2,m,k);} }
    free(px); }
// Present the frontend's offscreen render target to the visible window. Reclaimer
// draws its menu/client-screens to an OFFSCREEN FBO (observed: FBO 2) and hands it
// to halo3's D3D swap-chain Present (our no-op) — so nothing ever reaches the GL
// window (fb 0 stays black). This bridges that: blit the frontend FBO -> fb 0 and
// CGLFlushDrawable. FBO id defaults to 2, override with PELOAD_PRESENT_FBO. Gated by
// PELOAD_PRESENT so it's opt-in.
static void present_frontend(void){
    if(!getenv("PELOAD_PRESENT")) return;
    int fbo = getenv("PELOAD_PRESENT_FBO")? atoi(getenv("PELOAD_PRESENT_FBO")) : 2;
    void*(*cur)(void)=dlsym(RTLD_DEFAULT,"CGLGetCurrentContext"); void*ctx=cur?cur():0; if(!ctx) return;
    void*(*flush)(void*)=dlsym(RTLD_DEFAULT,"CGLFlushDrawable");
    void(*bf)(unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
    void(*bl)(int,int,int,int,int,int,int,int,unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBlitFramebuffer");
    void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv");
    if(!bf||!bl||!gi||!flush) return;
    int W=1280,H=720; int vp[4]={0,0,0,0}; gi(0x0BA2,vp); if(vp[2]>0)W=vp[2]; if(vp[3]>0)H=vp[3]; if(W>4096)W=4096; if(H>4096)H=4096;
    int sdraw=0,sread=0; gi(0x8CA6,&sdraw); gi(0x8CAA,&sread);
    bf(0x8CA8/*READ_FRAMEBUFFER*/,(unsigned)fbo); bf(0x8CA6/*DRAW_FRAMEBUFFER*/,0);
    bl(0,0,W,H, 0,0,W,H, 0x4000/*COLOR*/,0x2600/*NEAREST*/);
    // verify: read EACH candidate FBO's color attachment 0 directly (no blit),
    // writing a per-FBO BMP, + the window back buffer, so we can find any FBO with content.
    static int vevery; if(getenv("PELOAD_SHOT") && (vevery++ % 15)==0){
        void(*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels");
        void(*rb)(unsigned)=dlsym(RTLD_DEFAULT,"glReadBuffer");
        void(*ge)(void)=dlsym(RTLD_DEFAULT,"glGetError");
        if(rp){ const char*home=getenv("HOME");
          for(int cand=0;cand<=6;cand++){ bf(0x8CA8,(unsigned)cand); if(rb) rb(cand?0x8CE0/*COLOR_ATTACHMENT0*/:0x0405/*GL_BACK*/); if(ge)ge();
            unsigned char*px=calloc(1,(size_t)W*H*4); if(!px) break; rp(0,0,W,H,0x1908,0x1401,px);
            long sum=0; for(int i=0;i<W*H;i+=97){ unsigned char*p=px+(size_t)i*4; sum+=p[0]+p[1]+p[2]; }
            char path[512]; snprintf(path,sizeof path,"%s/reclaimer_fb%d_%dx%d.bmp",home?home:"/tmp",cand,W,H);
            FILE*f=fopen(path,"wb"); if(f){ uint32_t isz=(uint32_t)W*H*4,fsz=54+isz,o=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&o,4);
                uint8_t ih[40]={0}; uint32_t v=40; memcpy(ih,&v,4); memcpy(ih+4,&W,4); memcpy(ih+8,&H,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
                fwrite(fh,1,14,f); fwrite(ih,1,40,f); for(int i=0;i<W*H;i++){ unsigned char*p=px+(size_t)i*4; uint8_t bb[4]={p[2],p[1],p[0],255}; fwrite(bb,1,4,f);} fclose(f);}
            if(sum>0){ char m[128]; int k=snprintf(m,sizeof m,"  [present] FBO %d has CONTENT (sum=%ld) %dx%d\n",cand,sum,W,H); (void)!write(2,m,k);} free(px);} } }
    bf(0x8CA8,(unsigned)sread); bf(0x8CA6,(unsigned)sdraw);
    flush(ctx); }
static void gdi_present_main(void); // fwd (presents the engine's GDI composite DIB)
static void* cocoa_make_gl_window_i(int w,int h); // fwd
static int gdi_has_main(void); // fwd (1 once the engine's composite DIB exists)
static void cocoa_pump(void); // fwd (pump the macOS event loop so the window actually displays)
// MsgWaitForMultipleObjectsEx runs on the MAIN thread (the message loop). This is the
// only place we can legally create the NSWindow. On the D3D-only path the engine makes
// no GL window of its own, so we create ours here (idempotent) and present the engine's
// GDI composite. PELOAD_HOLD_WINDOW keeps the window up (and presenting) indefinitely.
static uint32_t MSABI i_MsgWaitForMultipleObjectsEx(uint32_t n,const void*h,uint32_t ms,uint32_t mask,uint32_t flags){
    (void)n;(void)h;(void)mask;(void)flags;
    if(getenv("PELOAD_PRESENT")){ cocoa_make_gl_window_i(1280,720); // main thread: creates NSWindow + (under HOLD) spawns present thread
        cocoa_pump();   // MAIN thread: service the window server so the window actually appears on screen
    }
    if(!getenv("PELOAD_HOLD_WINDOW")){ present_frontend(); gdi_present_main(); } // non-hold: present inline
    gl_grab_simple();
    uint32_t cap=ms; if(cap==0xFFFFFFFFu||cap>16) cap=16;
    if(cap){ struct timespec ts={0,(long)cap*1000000L}; nanosleep(&ts,0); }
    return 0x102; /* WAIT_TIMEOUT */ }
// Injected message queue: the worker never gets WM_ACTIVATE/SETFOCUS, so it believes its
// window is not foreground and won't count presents as "ready" (loading stalls). Deliver
// activation messages through the real pump so the engine's wndproc updates its focus state.
extern void* g_last_hwnd;
static struct { void* hwnd; uint32_t msg; uint64_t wp; int64_t lp; } g_injmsg[32]; static int g_injn, g_inji, g_act_done;
static void inject_msg(void*h,uint32_t m,uint64_t wp,int64_t lp){ if(g_injn<32){ g_injmsg[g_injn].hwnd=h; g_injmsg[g_injn].msg=m; g_injmsg[g_injn].wp=wp; g_injmsg[g_injn].lp=lp; g_injn++; } }
static void inject_activation_once(void){ if(g_act_done||!g_last_hwnd||getenv("PELOAD_NO_ACTIVATE")) return; g_act_done=1;
    inject_msg(g_last_hwnd,0x001C,1,0);  // WM_ACTIVATEAPP active
    inject_msg(g_last_hwnd,0x0086,1,0);  // WM_NCACTIVATE
    inject_msg(g_last_hwnd,0x0006,1,0);  // WM_ACTIVATE WA_ACTIVE
    inject_msg(g_last_hwnd,0x0007,0,0);  // WM_SETFOCUS
    if(getenv("PELOAD_TRACE_MSG")) ws("  [msg] injected window activation\n"); }
static void msg_fill(void*m,void*h,uint32_t msg,uint64_t wp,int64_t lp){ if(!m)return; uint8_t*p=m; *(void**)(p+0)=h; *(uint32_t*)(p+8)=msg; *(uint64_t*)(p+16)=wp; *(int64_t*)(p+24)=lp; *(uint32_t*)(p+32)=(uint32_t)i_GetTickCount(); *(int32_t*)(p+36)=0; *(int32_t*)(p+40)=0; }
static int    MSABI i_PeekMessageW(void*m,void*h,uint32_t f1,uint32_t f2,uint32_t r){ (void)h;(void)f1;(void)f2; inject_activation_once();
    if(g_inji<g_injn){ msg_fill(m,g_injmsg[g_inji].hwnd,g_injmsg[g_inji].msg,g_injmsg[g_inji].wp,g_injmsg[g_inji].lp); if(r&1/*PM_REMOVE*/) g_inji++; return 1; } return 0; }
static int    MSABI i_GetMessageW(void*m,void*h,uint32_t f1,uint32_t f2){ (void)h;(void)f1;(void)f2; inject_activation_once();
    if(g_inji<g_injn){ msg_fill(m,g_injmsg[g_inji].hwnd,g_injmsg[g_inji].msg,g_injmsg[g_inji].wp,g_injmsg[g_inji].lp); g_inji++; return 1; } return 0; }
static int64_t MSABI i_DispatchMessageW(const void*m){ if(!m) return 0; const uint8_t*p=m; WWin* w=*(WWin**)(p+0); uint32_t msg=*(uint32_t*)(p+8); uint64_t wp=*(uint64_t*)(p+16); int64_t lp=*(int64_t*)(p+24);
    if(w&&w->magic==WWIN_MAGIC&&w->wndproc){ if(getenv("PELOAD_TRACE_MSG")){ char b[64]; int k=snprintf(b,sizeof b,"  [msg] dispatch 0x%x -> wndproc\n",msg); ws(b);} return w->wndproc(w,msg,wp,lp); } return 0; }
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
// Window/screen size MUST match the actual GL drawable (cocoa_make_gl_window =
// 1280x720). Reporting a different size makes the engine size its framebuffer/
// readback to the wrong dimensions -> glReadPixels reads past the real drawable
// (OOB crash) and buffer sizes mismatch (corruption).
#define WIN_W 1280
#define WIN_H 720
static int    MSABI i_GetSystemMetrics(int idx){ switch(idx){ case 0:return WIN_W; case 1:return WIN_H; case 4:return 0; default:return 0; } }
static int    MSABI i_EnumDisplayMonitors(void*dc,const void*clip,void*cb,int64_t data){ (void)dc;(void)clip;(void)cb;(void)data; return 1; }
static int    MSABI i_GetClientRect(void*h,int32_t*r){ (void)h; if(r){r[0]=0;r[1]=0;r[2]=WIN_W;r[3]=WIN_H;} return 1; }
static int    MSABI i_GetWindowRect(void*h,int32_t*r){ (void)h; if(r){r[0]=0;r[1]=0;r[2]=WIN_W;r[3]=WIN_H;} return 1; }
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
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
// ---- GDI DIB surface (the menu renders into a CPU bitmap, then presents) ----
// The engine builds its menu surface with CreateCompatibleDC + CreateDIBSection
// and draws into the DIB bits. Stubs returned NULL -> 'Cannot allocate menu
// surface' + NULL deref. Provide real memory-backed DIB surfaces.
// kind: 0=bitmap, 1=brush, 2=pen, 3=font. topdown: 1=row0 is top (DIB neg height).
typedef struct { uint32_t magic; int w,h,bpp; void*bits; int owns; uint32_t color; int kind; int topdown; char face[32]; } GdiObj;
#define GDI_MAGIC 0x47444942u
static GdiObj* gdi_new(int w,int h,int bpp,void*bits,int owns){ GdiObj*g=calloc(1,sizeof*g); if(!g)return 0; g->magic=GDI_MAGIC; g->w=w; g->h=h; g->bpp=bpp; g->bits=bits; g->owns=owns; return g; }
// iswin: this DC targets the on-screen window (GetDC/GetWindowDC) -> blits present.
typedef struct { uint32_t magic; GdiObj*bmp; int iswin; uint32_t textcolor; int bkmode; int font_h; } GdiDC;
char g_cur_face[32];   // face of the DC's currently selected font (engine asks for Bahnschrift/Segoe UI)
#define GDIDC_MAGIC 0x47444344u
// ---- software rasteriser (all surfaces treated as 32bpp BGRA) ----------------
static void gdi_present_window(void); // fwd
static inline uint32_t* gdi_rowp(GdiObj*g,int y){ int yy = g->topdown? y : (g->h-1-y); return (uint32_t*)((uint8_t*)g->bits + (size_t)yy*(size_t)g->w*4); }
static uint32_t gdi_colorref_to_px(uint32_t c){ unsigned r=c&0xff,gg=(c>>8)&0xff,b=(c>>16)&0xff; return (uint32_t)b | (gg<<8) | (r<<16) | 0xFF000000u; }
static void gdi_fill(GdiObj*d,int x,int y,int w,int h,uint32_t px){
    if(!d||!d->bits||d->bpp!=32||d->w<=0||d->h<=0) return;
    if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} if(x+w>d->w)w=d->w-x; if(y+h>d->h)h=d->h-y; if(w<=0||h<=0)return;
    for(int j=0;j<h;j++){ uint32_t*row=gdi_rowp(d,y+j); for(int i=0;i<w;i++) row[x+i]=px; } }
// scaled blit src->dst (nearest). alpha: 0=copy, 1=src-alpha over dst.
static void gdi_blit(GdiObj*d,int dx,int dy,int dw,int dh, GdiObj*s,int sx,int sy,int sw,int sh,int alpha){
    if(!d||!d->bits||d->bpp!=32||!s||!s->bits||s->bpp!=32) return;
    if(dw<=0||dh<=0||sw<=0||sh<=0) return;
    for(int j=0;j<dh;j++){ int ty=dy+j; if(ty<0||ty>=d->h) continue; int syy=sy+(int)((long)j*sh/dh); if(syy<0||syy>=s->h) continue;
        uint32_t*drow=gdi_rowp(d,ty); uint32_t*srow=gdi_rowp(s,syy);
        for(int i=0;i<dw;i++){ int tx=dx+i; if(tx<0||tx>=d->w) continue; int sxx=sx+(int)((long)i*sw/dw); if(sxx<0||sxx>=s->w) continue;
            uint32_t sp=srow[sxx];
            if(alpha){ unsigned a=sp>>24; if(a==0) continue; if(a==255){ drow[tx]=sp; continue; }
                uint32_t dp=drow[tx]; unsigned na=255-a;
                unsigned ob=((sp&0xff)*a+(dp&0xff)*na)/255, og=(((sp>>8)&0xff)*a+((dp>>8)&0xff)*na)/255, orr=(((sp>>16)&0xff)*a+((dp>>16)&0xff)*na)/255;
                drow[tx]=ob|(og<<8)|(orr<<16)|0xFF000000u; }
            else drow[tx]=sp; } } }
static void*  MSABI i_CreateCompatibleDC(void*hdc){ (void)hdc; GdiDC*d=calloc(1,sizeof*d); if(d){d->magic=GDIDC_MAGIC;d->bkmode=2;} return d?d:(void*)0xDC02; }
static void*  MSABI i_CreateDIBSection(void*hdc,const uint8_t*bmi,uint32_t usage,void**ppvBits,void*hSection,uint32_t off){
    (void)hdc;(void)usage;(void)hSection;(void)off;
    int w=bmi?*(const int32_t*)(bmi+4):0; int h=bmi?*(const int32_t*)(bmi+8):0; int bpp=bmi?*(const uint16_t*)(bmi+14):32;
    if(bpp<=0)bpp=32; int ah=h<0?-h:h; if(w<=0)w=1; if(ah<=0)ah=1; size_t sz=(size_t)w*ah*4; if(sz<4)sz=4;
    void*bits=calloc(1,sz); GdiObj*g=gdi_new(w,ah,32,bits,1); if(g){ g->topdown=(h<0); }
    if(ppvBits)*ppvBits=bits;
    // The engine's full-screen composite (map loading screen / client screens) is a
    // window-sized DIB it writes into directly, then presents via the D3D swap chain
    // (our no-op). Track the largest window-sized DIB so the frame loop can present it.
    // Only latch a WINDOW-ASPECT (16:9-ish, wide) DIB as the presented composite. In-game the
    // engine also creates square render targets / texture atlases (1024x1024) and 2:1 surfaces
    // (1024x512) as DIBs; those are NOT the frame — latching them showed a black frame with
    // content crushed into the left column. Require ~16:9 and >=1152 wide so only the real
    // frontend/client composite (1280x720) wins.
    extern GdiObj* g_gdi_mainsurf; if(g&&w>=1152&&ah>=360){ double ar=(double)w/ah; if(ar>=1.6&&ar<=1.9) g_gdi_mainsurf=g; }
    if(getenv("PELOAD_TRACE_GDI")){ char t[96]; snprintf(t,sizeof t,"  [CreateDIBSection] %dx%d %dbpp td=%d bits=%p\n",w,ah,bpp,h<0,bits); ws(t);}
    return g; }
GdiObj* g_gdi_mainsurf;
static void*  MSABI i_CreateCompatibleBitmap(void*hdc,int w,int h){ (void)hdc; if(w<=0)w=1; if(h<=0)h=1; void*b=calloc(1,(size_t)w*h*4); GdiObj*g=gdi_new(w,h,32,b,1); if(g)g->topdown=1; return g; }
static void*  MSABI i_CreateSolidBrush(uint32_t color){ GdiObj*g=gdi_new(1,1,32,0,0); if(g){g->kind=1;g->color=color;} return g?g:(void*)0x1; }
static void*  MSABI i_CreatePen(int style,int w,uint32_t color){ (void)style;(void)w; GdiObj*g=gdi_new(1,1,32,0,0); if(g){g->kind=2;g->color=color;} return g?g:(void*)0x1; }
static int    MSABI i_FillRect(void*dc,const int32_t*rc,void*br){ GdiDC*d=dc; GdiObj*b=br; if(!d||d->magic!=GDIDC_MAGIC||!d->bmp||!rc) return 1;
    uint32_t px = (b&&b->magic==GDI_MAGIC&&b->kind==1)? gdi_colorref_to_px(b->color) : 0xFF000000u;
    gdi_fill(d->bmp,rc[0],rc[1],rc[2]-rc[0],rc[3]-rc[1],px); if(d->iswin) gdi_present_window(); return 1; }
static int    MSABI i_GdiAlphaBlend(void*dst,int xd,int yd,int wd,int hd,void*src,int xs,int ys,int ws,int hs,uint32_t blend){
    GdiDC*dd=dst,*sd=src; if(dd&&dd->magic==GDIDC_MAGIC&&sd&&sd->magic==GDIDC_MAGIC&&dd->bmp&&sd->bmp) gdi_blit(dd->bmp,xd,yd,wd,hd,sd->bmp,xs,ys,ws,hs,1);
    if(dd&&dd->magic==GDIDC_MAGIC&&dd->iswin) gdi_present_window(); (void)blend; return 1; }
static GdiObj g_gdi_stockprev = { GDI_MAGIC, 1,1,32, 0, 0, 0,0,0 };
static void*  MSABI i_SelectObject(void*hdc,void*obj){ GdiDC*d=hdc; GdiObj*g=obj; void*prev=0;
    if(d&&d->magic==GDIDC_MAGIC){ if(g&&g->magic==GDI_MAGIC){ if(g->kind==0){ prev=d->bmp; d->bmp=g; } else { if(g->kind==3){ d->font_h=g->h; snprintf(g_cur_face,sizeof g_cur_face,"%s",g->face); } prev=(void*)&g_gdi_stockprev; } } }
    return prev?prev:(void*)&g_gdi_stockprev; }
static int    MSABI i_DeleteObject(void*obj){ GdiObj*g=obj; if(g&&g!=&g_gdi_stockprev&&g->magic==GDI_MAGIC){ if(g->owns)free(g->bits); g->magic=0; free(g); } return 1; }
static int    MSABI i_DeleteDC(void*hdc){ GdiDC*d=hdc; if(d&&d->magic==GDIDC_MAGIC){ d->magic=0; free(d); } return 1; }
static int    MSABI i_GetObjectW(void*obj,int cb,void*out){ GdiObj*g=obj; if(!g||g->magic!=GDI_MAGIC||!out) return 0;
    if(cb>=32){ int32_t*p=out; p[0]=0; p[1]=g->w; p[2]=g->h; p[3]=g->w*4; *(uint16_t*)(p+4)=1; *((uint16_t*)(p+4)+1)=(uint16_t)g->bpp; *(void**)((uint8_t*)out+24)=g->bits; return 32; } return 0; }
static int    MSABI i_BitBlt(void*dst,int x,int y,int w,int h,void*src,int sx,int sy,uint32_t rop){ GdiDC*dd=dst,*sd=src;
    if(dd&&dd->magic==GDIDC_MAGIC&&dd->bmp&&sd&&sd->magic==GDIDC_MAGIC&&sd->bmp){ if(rop==0x42/*BLACKNESS*/) gdi_fill(dd->bmp,x,y,w,h,0xFF000000u); else if(rop==0xFF0062/*WHITENESS*/) gdi_fill(dd->bmp,x,y,w,h,0xFFFFFFFFu); else gdi_blit(dd->bmp,x,y,w,h,sd->bmp,sx,sy,w,h,0); }
    if(getenv("PELOAD_TRACE_GDI")){ char t[128]; snprintf(t,sizeof t,"  [BitBlt] dst=%p(win=%d) %d,%d %dx%d <- src=%p rop=0x%x\n",dst,dd&&dd->magic==GDIDC_MAGIC?dd->iswin:-1,x,y,w,h,src,rop); ws(t);}
    if(dd&&dd->magic==GDIDC_MAGIC&&dd->iswin) gdi_present_window(); return 1; }
static int    MSABI i_StretchBlt(void*dst,int x,int y,int w,int h,void*src,int sx,int sy,int sw,int sh,uint32_t rop){ (void)rop; GdiDC*dd=dst,*sd=src;
    if(dd&&dd->magic==GDIDC_MAGIC&&dd->bmp&&sd&&sd->magic==GDIDC_MAGIC&&sd->bmp) gdi_blit(dd->bmp,x,y,w,h,sd->bmp,sx,sy,sw,sh,0);
    if(dd&&dd->magic==GDIDC_MAGIC&&dd->iswin) gdi_present_window(); return 1; }
// DIB -> DC blits: wrap the raw DIB bits in a temporary GdiObj and blit.
static int    MSABI i_SetDIBitsToDevice(void*dc,int x,int y,uint32_t w,uint32_t h,int sx,int sy,uint32_t sl,uint32_t nl,const void*bits,const uint8_t*bmi,uint32_t u){
    (void)sl;(void)nl;(void)u; GdiDC*dd=dc; if(dd&&dd->magic==GDIDC_MAGIC&&dd->bmp&&bits&&bmi){ int bw=*(const int32_t*)(bmi+4),bh=*(const int32_t*)(bmi+8);
        GdiObj s={GDI_MAGIC,bw,bh<0?-bh:bh,32,(void*)bits,0,0,0,bh<0}; gdi_blit(dd->bmp,x,y,(int)w,(int)h,&s,sx,sy,(int)w,(int)h,0); if(dd->iswin) gdi_present_window(); }
    return (int)h; }
static int    MSABI i_StretchDIBits(void*dc,int xd,int yd,int wd,int hd,int xs,int ys,int ws,int hs,const void*bits,const uint8_t*bmi,uint32_t usage,uint32_t rop){
    (void)usage;(void)rop; GdiDC*dd=dc; if(dd&&dd->magic==GDIDC_MAGIC&&dd->bmp&&bits&&bmi){ int bw=*(const int32_t*)(bmi+4),bh=*(const int32_t*)(bmi+8);
        GdiObj s={GDI_MAGIC,bw,bh<0?-bh:bh,32,(void*)bits,0,0,0,bh<0}; gdi_blit(dd->bmp,xd,yd,wd,hd,&s,xs,ys,ws,hs,0);
        if(getenv("PELOAD_TRACE_GDI")){ char t[128]; int k=snprintf(t,sizeof t,"  [StretchDIBits] dst(win=%d) %d,%d %dx%d <- DIB %dx%d\n",dd->iswin,xd,yd,wd,hd,bw,bh); (void)!write(2,t,k);}
        if(dd->iswin) gdi_present_window(); }
    return hs>0?hs:1; }
static int    MSABI i_SetStretchBltMode(void*dc,int mode){ (void)dc;(void)mode; return 1; }
static int    MSABI i_SetBrushOrgEx(void*dc,int x,int y,void*pt){ (void)dc;(void)x;(void)y; if(pt){((int32_t*)pt)[0]=0;((int32_t*)pt)[1]=0;} return 1; }
static uint32_t MSABI i_SetTextColor(void*dc,uint32_t c){ GdiDC*d=dc; uint32_t o=0; if(d&&d->magic==GDIDC_MAGIC){o=d->textcolor;d->textcolor=c;} return o; }
static int    MSABI i_SetBkMode(void*dc,int m){ GdiDC*d=dc; int o=2; if(d&&d->magic==GDIDC_MAGIC){o=d->bkmode;d->bkmode=m;} return o; }
static uint32_t MSABI i_SetBkColor(void*dc,uint32_t c){ (void)dc;(void)c; return 0; }
// ---- CoreText glyph rasterization into the GDI BGRA DIB ----
// The Reclaimer frontend composites its menu into a CPU DIB (which we present) and
// draws every label via GDI DrawTextW/TextOutW. Those were no-ops, so the UI showed a
// bare dark background with no text. Rasterize runs with CoreText and composite into
// the DIB so the menu is actually legible.
// Font resolution for the GDI->CoreText overlay. The engine asks for real Windows fonts by name
// (observed: "Bahnschrift" for headers, "Segoe UI" for body). We honor the requested face: use the
// genuine font when it is installed/available, otherwise a close macOS substitute. Overrides:
//   PELOAD_FONT_FILE=<path>  force one .ttf/.otf for all text
//   PELOAD_FONT=<family>     force one family for all text
//   PELOAD_FONT_HEAD / PELOAD_FONT_BODY = family for the Bahnschrift / Segoe-UI slots
extern char g_drivec[1024];
extern char g_cur_face[32];   // face name of the DC's currently selected font (set in SelectObject)
// One real font file for all overlay text: PELOAD_FONT_FILE, else the staged
// $drive_c/ReclaimerFonts/bahnschrift.ttf (the engine's intended UI font) — but only if it is the
// GENUINE font, not the Arial placeholder that ships by default. PELOAD_FONT (a family name) opts out.
static CGFontRef ct_load_cgfont(void){
    static int tried; static CGFontRef cg; if(tried) return cg; tried=1;
    if(getenv("PELOAD_FONT")) return (cg=0);                 // family override -> use the name path
    char path[1200]={0}; const char*ff=getenv("PELOAD_FONT_FILE");
    if(ff&&*ff) snprintf(path,sizeof path,"%s",ff);
    else if(g_drivec[0]) snprintf(path,sizeof path,"%s/ReclaimerFonts/bahnschrift.ttf",g_drivec);
    if(!path[0]||access(path,R_OK)!=0) return (cg=0);
    CGDataProviderRef dp=CGDataProviderCreateWithFilename(path); if(!dp) return (cg=0);
    CGFontRef f=CGFontCreateWithDataProvider(dp); CGDataProviderRelease(dp); if(!f) return (cg=0);
    if(!ff){ // auto-default: reject the Arial placeholder so we fall back to the face-aware substitutes
        CFStringRef fn=CGFontCopyFullName(f); char nm[128]={0};
        if(fn){ CFStringGetCString(fn,nm,sizeof nm,kCFStringEncodingUTF8); CFRelease(fn); }
        if(strcasestr(nm,"arial")){ CGFontRelease(f); return (cg=0); } }
    cg=f;
    if(getenv("PELOAD_TRACE_FONT")){ char m[220]; int k=snprintf(m,sizeof m,"  [font] all text uses file %s\n",path); (void)!write(2,m,k); }
    return cg; }
static int ct_family_is(CTFontRef f,const char*want){
    if(!f) return 0; CFStringRef fam=CTFontCopyFamilyName(f); if(!fam) return 0;
    char buf[128]={0}; Boolean ok=CFStringGetCString(fam,buf,sizeof buf,kCFStringEncodingUTF8); CFRelease(fam); if(!ok) return 0;
    for(char*p=buf;*p;p++)*p=(char)tolower((unsigned char)*p);
    char w[128]={0}; int i=0; for(;want[i]&&i<127;i++) w[i]=(char)tolower((unsigned char)want[i]);
    return strstr(buf,w)!=0; }
// Pick the resolved family name for one of 3 slots (0=header/Bahnschrift, 1=body/Segoe, 2=default),
// deciding ONCE: genuine font if installed, else the substitute (or an env override).
static const char* ct_resolve_family(int slot){
    static const char* cached[3]; static int done[3]; if(done[slot]) return cached[slot]; done[slot]=1;
    const char*glob=getenv("PELOAD_FONT");
    const char*env = slot==0?getenv("PELOAD_FONT_HEAD") : slot==1?getenv("PELOAD_FONT_BODY") : 0;
    const char*want= slot==0?"Bahnschrift" : slot==1?"Segoe UI" : "Helvetica Neue";
    const char*sub = slot==0?"Avenir Next Condensed" : "Helvetica Neue";
    const char*fam = env?env : glob?glob : want;
    if(!env && !glob){   // use the genuine font if it is actually installed, else the substitute
        CFStringRef nm=CFStringCreateWithCString(0,want,kCFStringEncodingUTF8);
        CTFontRef g=nm?CTFontCreateWithName(nm,16,0):0; if(nm)CFRelease(nm);
        if(!ct_family_is(g,want)) fam=sub; if(g)CFRelease(g);
        if(getenv("PELOAD_TRACE_FONT")){ char m[160]; int k=snprintf(m,sizeof m,"  [font] slot %d '%s' -> '%s'\n",slot,want,fam); (void)!write(2,m,k);} }
    cached[slot]=fam; return cached[slot]; }
static int ct_slot_for(const char*face){
    if(face&&face[0]){ if(strcasestr(face,"bahn")||strcasestr(face,"din")) return 0; if(strcasestr(face,"segoe")) return 1; }
    return 1; }   // default menu body = Segoe slot
static CTFontRef ct_font_for(int pxh){
    if(pxh<=0) pxh=16; if(pxh>256) pxh=256;
    int slot=ct_slot_for(g_cur_face);
    static CTFontRef cache[3][257];
    if(cache[slot][pxh]) return cache[slot][pxh];
    CTFontRef f=0;
    CGFontRef cg=ct_load_cgfont();
    if(cg) f=CTFontCreateWithGraphicsFont(cg,(CGFloat)pxh,0,0);
    if(!f){ const char*fam=ct_resolve_family(slot);
        CFStringRef nm=CFStringCreateWithCString(0,fam,kCFStringEncodingUTF8);
        f=CTFontCreateWithName(nm?nm:CFSTR("Helvetica Neue"),(CGFloat)pxh,0); if(nm)CFRelease(nm); }
    cache[slot][pxh]=f; return cache[slot][pxh]; }
static double ct_measure(const uint16_t*s,int clen,int pxh,double*asc,double*desc){
    if(clen<0){ clen=0; while(s&&s[clen])clen++; }
    CTFontRef font=ct_font_for(pxh);
    if(asc)*asc=CTFontGetAscent(font); if(desc)*desc=CTFontGetDescent(font);
    if(clen<=0||!s) return 0;
    CFStringRef str=CFStringCreateWithCharacters(0,(const UniChar*)s,clen); if(!str) return 0;
    CFStringRef k[1]={kCTFontAttributeName}; CFTypeRef v[1]={font};
    CFDictionaryRef at=CFDictionaryCreate(0,(const void**)k,(const void**)v,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as=CFAttributedStringCreate(0,str,at); CTLineRef line=CTLineCreateWithAttributedString(as);
    double w=CTLineGetTypographicBounds(line,0,0,0);
    CFRelease(line);CFRelease(as);CFRelease(at);CFRelease(str); return w; }
// Draw a UTF-16 run into a 32bpp BGRA GdiObj at GDI logical (x=left, y=top-of-cell).
extern GdiObj* g_gdi_mainsurf;
// The engine draws full-screen menu text into a DC whose bitmap is the 1x1 default
// placeholder (its real surface was restored away), so route text with no real target
// onto the presented main surface — the coordinates are already full-screen (1280x720).
static GdiObj* gdi_text_target(GdiDC*d){
    if(d&&d->bmp&&d->bmp->magic==GDI_MAGIC&&d->bmp->bits&&d->bmp->w>1&&d->bmp->h>1) return d->bmp;
    if(g_gdi_mainsurf&&g_gdi_mainsurf->magic==GDI_MAGIC&&g_gdi_mainsurf->bits) return g_gdi_mainsurf;
    return d?d->bmp:0; }
static void gdi_text_render(GdiObj*dst,int x,int y,const uint16_t*s,int clen,uint32_t cref,int pxh){
    if(getenv("PELOAD_TRACE_TEXT")){ char m[160];
        if(!dst) snprintf(m,sizeof m,"  [text] dst=NULL\n");
        else snprintf(m,sizeof m,"  [text] dst=%p magic=0x%x bpp=%d bits=%p %dx%d td=%d main=%d @%d,%d h=%d\n",(void*)dst,dst->magic,dst->bpp,dst->bits,dst->w,dst->h,dst->topdown,dst==g_gdi_mainsurf,x,y,pxh);
        ws(m); }
    if(!dst||dst->magic!=GDI_MAGIC||!dst->bits||dst->bpp!=32||!s) return;
    if(clen<0){ clen=0; while(s[clen])clen++; } if(clen<=0) return; if(pxh<=0)pxh=16;
    CTFontRef font=ct_font_for(pxh); double asc=CTFontGetAscent(font), desc=CTFontGetDescent(font);
    double wd=ct_measure(s,clen,pxh,0,0); int wc=(int)(wd+2.5), hc=(int)(asc+desc+2.5);
    if(wc<=0||hc<=0||wc>8192||hc>1024) return;
    uint32_t* tmp=calloc((size_t)wc*hc,4); if(!tmp) return;
    double r=((cref)&0xff)/255.0, g=((cref>>8)&0xff)/255.0, b=((cref>>16)&0xff)/255.0;
    CFStringRef str=CFStringCreateWithCharacters(0,(const UniChar*)s,clen);
    CGColorRef col=CGColorCreateGenericRGB(r,g,b,1.0);
    CFStringRef k[2]={kCTFontAttributeName,kCTForegroundColorAttributeName}; CFTypeRef v[2]={font,col};
    CFDictionaryRef at=str?CFDictionaryCreate(0,(const void**)k,(const void**)v,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks):0;
    CFAttributedStringRef as=at?CFAttributedStringCreate(0,str,at):0; CTLineRef line=as?CTLineCreateWithAttributedString(as):0;
    CGColorSpaceRef csp=CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx=CGBitmapContextCreate(tmp,wc,hc,8,(size_t)wc*4,csp,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
    if(ctx&&line){ CGContextSetShouldAntialias(ctx,true); CGContextSetShouldSmoothFonts(ctx,true);
        CGContextSetTextPosition(ctx,1.0,(CGFloat)desc+1.0); CTLineDraw(line,ctx); }
    if(ctx) CGContextRelease(ctx);
    // tmp is top-down (CG bitmap row 0 = top). Composite over dst honoring dst orientation.
    uint32_t* db=(uint32_t*)dst->bits;
    for(int ty=0;ty<hc;ty++){ int dy=y+ty; if(dy<0||dy>=dst->h) continue;
        int drow = dst->topdown? dy : dst->h-1-dy; uint32_t* drp=db+(size_t)drow*dst->w; uint32_t* trp=tmp+(size_t)ty*wc;
        for(int cx=0;cx<wc;cx++){ int dx=x+cx; if(dx<0||dx>=dst->w) continue; uint32_t sp=trp[cx]; uint32_t a=sp>>24; if(!a) continue;
            if(a==255){ drp[dx]=sp; continue; }
            uint32_t sb=sp&0xff,sg=(sp>>8)&0xff,sr=(sp>>16)&0xff; uint32_t dp=drp[dx];
            uint32_t ddb=dp&0xff,ddg=(dp>>8)&0xff,ddr=(dp>>16)&0xff,dda=(dp>>24)&0xff; uint32_t ia=255-a;
            uint32_t ob=sb+(ddb*ia+127)/255, og=sg+(ddg*ia+127)/255, orr=sr+(ddr*ia+127)/255, oa=a+(dda*ia+127)/255;
            if(ob>255)ob=255; if(og>255)og=255; if(orr>255)orr=255; if(oa>255)oa=255;
            drp[dx]=(oa<<24)|(orr<<16)|(og<<8)|ob; } }
    free(tmp); CGColorSpaceRelease(csp); if(line)CFRelease(line); if(as)CFRelease(as); if(at)CFRelease(at); CFRelease(col); if(str)CFRelease(str);
}
// Composite a top-down BGRA temp buffer (premultiplied) over the DIB at GDI (x,y).
static void gdi_composite_tmp(GdiObj*dst,int x,int y,const uint32_t*tmp,int wc,int hc){
    uint32_t* db=(uint32_t*)dst->bits;
    for(int ty=0;ty<hc;ty++){ int dy=y+ty; if(dy<0||dy>=dst->h) continue;
        int drow = dst->topdown? dy : dst->h-1-dy; uint32_t* drp=db+(size_t)drow*dst->w; const uint32_t* trp=tmp+(size_t)ty*wc;
        for(int cx=0;cx<wc;cx++){ int dx=x+cx; if(dx<0||dx>=dst->w) continue; uint32_t sp=trp[cx]; uint32_t a=sp>>24; if(!a) continue;
            if(a==255){ drp[dx]=sp; continue; }
            uint32_t sb=sp&0xff,sg=(sp>>8)&0xff,sr=(sp>>16)&0xff; uint32_t dp=drp[dx];
            uint32_t ddb=dp&0xff,ddg=(dp>>8)&0xff,ddr=(dp>>16)&0xff,dda=(dp>>24)&0xff; uint32_t ia=255-a;
            uint32_t ob=sb+(ddb*ia+127)/255, og=sg+(ddg*ia+127)/255, orr=sr+(ddr*ia+127)/255, oa=a+(dda*ia+127)/255;
            if(ob>255)ob=255; if(og>255)og=255; if(orr>255)orr=255; if(oa>255)oa=255;
            drp[dx]=(oa<<24)|(orr<<16)|(og<<8)|ob; } }
}
// Word-wrapped multi-line text within [x,y,w,hmax] using CTFramesetter. Returns height.
static int gdi_text_block(GdiObj*dst,int x,int y,int w,int hmax,const uint16_t*s,int clen,uint32_t cref,int pxh,int center){
    if(!dst||dst->magic!=GDI_MAGIC||!dst->bits||dst->bpp!=32||!s||w<=1) return 0;
    if(clen<0){ clen=0; while(s[clen])clen++; } if(clen<=0) return 0; if(pxh<=0)pxh=16; if(hmax<=0)hmax=2048;
    CTFontRef font=ct_font_for(pxh); double r=((cref)&0xff)/255.0,g=((cref>>8)&0xff)/255.0,b=((cref>>16)&0xff)/255.0;
    CFStringRef str=CFStringCreateWithCharacters(0,(const UniChar*)s,clen); if(!str) return 0;
    CGColorRef col=CGColorCreateGenericRGB(r,g,b,1.0);
    CTTextAlignment al = center?kCTTextAlignmentCenter:kCTTextAlignmentLeft;
    CTParagraphStyleSetting ps[1]={{kCTParagraphStyleSpecifierAlignment,sizeof(al),&al}};
    CTParagraphStyleRef para=CTParagraphStyleCreate(ps,1);
    CFStringRef k[3]={kCTFontAttributeName,kCTForegroundColorAttributeName,kCTParagraphStyleAttributeName}; CFTypeRef v[3]={font,col,para};
    CFDictionaryRef at=CFDictionaryCreate(0,(const void**)k,(const void**)v,3,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as=CFAttributedStringCreate(0,str,at);
    CTFramesetterRef fs=CTFramesetterCreateWithAttributedString(as);
    CFRange fit; CGSize sug=CTFramesetterSuggestFrameSizeWithConstraints(fs,CFRangeMake(0,0),0,CGSizeMake(w,hmax),&fit);
    int hc=(int)(sug.height+2.5); if(hc>hmax)hc=hmax; if(hc<=0){ CFRelease(fs);CFRelease(as);CFRelease(at);CFRelease(para);CFRelease(col);CFRelease(str); return 0; }
    uint32_t* tmp=calloc((size_t)w*hc,4);
    CGColorSpaceRef csp=CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx=tmp?CGBitmapContextCreate(tmp,w,hc,8,(size_t)w*4,csp,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little):0;
    if(ctx){ CGContextSetShouldAntialias(ctx,true); CGContextSetShouldSmoothFonts(ctx,true);
        CGPathRef path=CGPathCreateWithRect(CGRectMake(0,0,w,hc),0);
        CTFrameRef frame=CTFramesetterCreateFrame(fs,CFRangeMake(0,0),path,0);
        if(frame){ CTFrameDraw(frame,ctx); CFRelease(frame); } CGPathRelease(path);
        gdi_composite_tmp(dst,x,y,tmp,w,hc); CGContextRelease(ctx); }
    if(tmp)free(tmp); CGColorSpaceRelease(csp); CFRelease(fs);CFRelease(as);CFRelease(at);CFRelease(para);CFRelease(col);CFRelease(str);
    return hc; }
static void*  MSABI i_CreateFontW(int h,int w,int e,int o,int wt,uint32_t it,uint32_t u,uint32_t s,uint32_t cs,uint32_t op,uint32_t cp,uint32_t q,uint32_t pf,const uint16_t*face){
    (void)w;(void)e;(void)o;(void)u;(void)s;(void)cs;(void)op;(void)cp;(void)q;(void)pf;
    char nm[32]; int i=0; if(face) for(;i<31&&face[i];i++) nm[i]=(char)face[i]; nm[i]=0;
    if(getenv("PELOAD_TRACE_FONT")){ char m[128]; int k=snprintf(m,sizeof m,"  [font] CreateFontW face='%s' h=%d weight=%d italic=%u\n",nm,h,wt,it); (void)!write(2,m,k); }
    int fh=h<0?-h:h; if(fh==0)fh=16; GdiObj*g=gdi_new(fh,fh,32,0,0); if(g){ g->kind=3; memcpy(g->face,nm,sizeof g->face<sizeof nm?sizeof g->face:sizeof nm); g->face[31]=0; } return g?g:(void*)0x1; }
static void*  MSABI i_CreateFontIndirectW(const void*lf){ int lh=lf?*(const int32_t*)lf:0; int fh=lh<0?-lh:lh; if(!fh)fh=16;
    char nm[32]={0}; if(lf){ const uint16_t*face=(const uint16_t*)((const char*)lf+28); int i=0; for(;i<31&&face[i];i++) nm[i]=(char)face[i]; nm[i]=0; }
    if(getenv("PELOAD_TRACE_FONT")&&lf){ int wt=*(const int32_t*)((const char*)lf+16); int it=*((const uint8_t*)lf+20);
        char m[128]; int k=snprintf(m,sizeof m,"  [font] CreateFontIndirectW face='%s' h=%d weight=%d italic=%d\n",nm,lh,wt,it); (void)!write(2,m,k); }
    GdiObj*g=gdi_new(fh,fh,32,0,0); if(g){ g->kind=3; memcpy(g->face,nm,sizeof nm); g->face[31]=0; } return g?g:(void*)0x1; }
static int    MSABI i_GdiFlush(void){ return 1; }
static int    MSABI i_GetTextExtentPoint32W(void*dc,const uint16_t*s,int c,void*sz){ GdiDC*d=dc; int ph=(d&&d->magic==GDIDC_MAGIC&&d->font_h)?d->font_h:16;
    double asc=0,desc=0; double w=ct_measure(s,c,ph,&asc,&desc); if(sz){((int32_t*)sz)[0]=(int32_t)(w+0.5);((int32_t*)sz)[1]=(int32_t)(asc+desc+0.5);} return 1; }
static int    MSABI i_TextOutW(void*dc,int x,int y,const uint16_t*s,int c){ GdiDC*d=dc;
    if(getenv("PELOAD_TRACE_TEXT")){ char m[96]; snprintf(m,sizeof m,"  [TextOutW] dc=%p dcok=%d bmp=%p c=%d\n",dc,d&&d->magic==GDIDC_MAGIC,(d&&d->magic==GDIDC_MAGIC)?(void*)d->bmp:0,c); ws(m); }
    if(d&&d->magic==GDIDC_MAGIC){ gdi_text_render(gdi_text_target(d),x,y,s,c,d->textcolor,d->font_h?d->font_h:16); if(d->iswin) gdi_present_window(); } return 1; }
static int    MSABI i_ExtTextOutW(void*dc,int x,int y,uint32_t f,const void*rc,const uint16_t*s,uint32_t c,const int*dx){ (void)f;(void)rc;(void)dx; GdiDC*d=dc;
    if(getenv("PELOAD_TRACE_TEXT")){ char m[96]; snprintf(m,sizeof m,"  [ExtTextOutW] dc=%p dcok=%d bmp=%p c=%u\n",dc,d&&d->magic==GDIDC_MAGIC,(d&&d->magic==GDIDC_MAGIC)?(void*)d->bmp:0,c); ws(m); }
    if(d&&d->magic==GDIDC_MAGIC){ gdi_text_render(gdi_text_target(d),x,y,s,(int)c,d->textcolor,d->font_h?d->font_h:16); if(d->iswin) gdi_present_window(); } return 1; }
static int    MSABI i_DrawTextW(void*dc,const uint16_t*s,int count,int32_t*rc,uint32_t fmt){ GdiDC*d=dc;
    if(getenv("PELOAD_TRACE_TEXT")){ char m[96]; snprintf(m,sizeof m,"  [DrawTextW] dc=%p dcok=%d bmp=%p fmt=0x%x cnt=%d\n",dc,d&&d->magic==GDIDC_MAGIC,(d&&d->magic==GDIDC_MAGIC)?(void*)d->bmp:0,fmt,count); ws(m); }
    if(!d||d->magic!=GDIDC_MAGIC||!rc) return 0;
    int ph=d->font_h?d->font_h:16; int clen=count; if(clen<0){ clen=0; while(s&&s[clen])clen++; }
    double asc=0,desc=0; double w=ct_measure(s,clen,ph,&asc,&desc); int th=(int)(asc+desc+0.5);
    if(fmt&0x400){ rc[2]=rc[0]+(int32_t)(w+0.5); rc[3]=rc[1]+th; return th; } // DT_CALCRECT
    GdiObj*tgt=gdi_text_target(d); int rw=rc[2]-rc[0], rh=rc[3]-rc[1];
    if(!(fmt&0x20) && rw>1 && (fmt&0x10)){ // not DT_SINGLELINE + DT_WORDBREAK -> wrap in the rect
        int hh=gdi_text_block(tgt,rc[0],rc[1],rw,rh>0?rh:2048,s,clen,d->textcolor,ph,(fmt&0x1)!=0);
        if(d->iswin) gdi_present_window(); return hh?hh:th; }
    int x=rc[0], y=rc[1];
    if(fmt&0x1) x=rc[0]+(int)((rw-w)/2);                   // DT_CENTER
    else if(fmt&0x2) x=rc[2]-(int)(w+0.5);                 // DT_RIGHT
    if(fmt&0x4) y=rc[1]+(rh-th)/2;                         // DT_VCENTER (single line)
    gdi_text_render(tgt,x,y,s,clen,d->textcolor,ph); if(d->iswin) gdi_present_window(); return th; }
static int    MSABI i_GetTextMetricsW(void*dc,void*tm){ GdiDC*d=dc; int ph=(d&&d->magic==GDIDC_MAGIC&&d->font_h)?d->font_h:16; if(!tm) return 0;
    CTFontRef f=ct_font_for(ph); int asc=(int)(CTFontGetAscent(f)+0.5), desc=(int)(CTFontGetDescent(f)+0.5); int32_t*t=tm; memset(t,0,60);
    t[0]=asc+desc; t[1]=asc; t[2]=desc; t[5]=ph/2; t[6]=ph; t[7]=400; return 1; } // tmHeight,Ascent,Descent,AveCharWidth,MaxCharWidth,Weight
static int    MSABI i_GetTextFaceW(void*dc,int c,uint16_t*face){ (void)dc; static const uint16_t nm[]={'H','e','l','v','e','t','i','c','a',0}; int n=0; while(nm[n])n++; if(face&&c>0){ int k=n<c-1?n:c-1; for(int i=0;i<k;i++)face[i]=nm[i]; face[k]=0; } return n+1; }
static uint32_t MSABI i_SetTextCharacterExtra(void*dc,int extra){ (void)dc;(void)extra; return 0; }
// Window DC: a real on-screen surface (WIN_WxWIN_H BGRA, top-down). Blits to it are
// presented to the GL window. CreateCompatibleDC makes an off-screen (iswin=0) DC.
static uint32_t* g_gdi_winbuf;                 // WIN_WxWIN_H BGRA, top-down
static GdiObj     g_gdi_winobj;
static void gdi_win_init(void){ if(g_gdi_winbuf) return; g_gdi_winbuf=calloc(1,(size_t)WIN_W*WIN_H*4);
    g_gdi_winobj.magic=GDI_MAGIC; g_gdi_winobj.w=WIN_W; g_gdi_winobj.h=WIN_H; g_gdi_winobj.bpp=32; g_gdi_winobj.bits=g_gdi_winbuf; g_gdi_winobj.topdown=1; }
static void*  MSABI i_GetDC(void*h){ (void)h; gdi_win_init(); GdiDC*d=calloc(1,sizeof*d); if(d){ d->magic=GDIDC_MAGIC; d->bmp=&g_gdi_winobj; d->iswin=1; d->bkmode=2; } return d?d:(void*)0xDC01; }
static int    MSABI i_ReleaseDC(void*h,void*dc){ (void)h; GdiDC*d=dc; if(d&&d->magic==GDIDC_MAGIC){ d->magic=0; free(d); } return 1; }
static void*  MSABI i_GetWindowDC(void*h){ return i_GetDC(h); }
// Present the window BGRA buffer to the GL window: upload to a texture-backed FBO
// and blit (flipping Y, since GL is bottom-up) to the default framebuffer.
// Present a BGRA surface to the GL window (texture-backed FBO blit). topdown=1
// surfaces are flipped (GL is bottom-up); bottom-up DIBs present without a flip.
static void gdi_present_surface(const void*bits,int w,int h,int topdown){
    if(!bits||w<=0||h<=0) return;
    if(getenv("PELOAD_GDI_FLIP")) topdown=!topdown;   // empirical orientation toggle
    // Diagnostic: dump the surface to a BMP so we can confirm the engine actually
    // composited content into it (independent of any GL present path).
    if(getenv("PELOAD_SHOT")){ static int seq; if((seq++%30)==0){ const char*home=getenv("HOME"); char path[256]; snprintf(path,sizeof path,"%s/reclaimer_gdi_%d_%dx%d.bmp",home?home:"/tmp",seq,w,h);
        long sum=0; const uint32_t*p=bits; for(int i=0;i<w*h;i+=131) sum+=p[i]&0xffffff;
        FILE*f=fopen(path,"wb"); if(f){ uint32_t isz=(uint32_t)w*h*4,fsz=54+isz,o=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&o,4);
            uint8_t ih[40]={0}; uint32_t v=40; memcpy(ih,&v,4); memcpy(ih+4,&w,4); int hh=topdown?-h:h; memcpy(ih+8,&hh,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
            fwrite(fh,1,14,f); fwrite(ih,1,40,f); fwrite(bits,1,isz,f); fclose(f); }
        char t[96]; int k=snprintf(t,sizeof t,"  [gdi-dump] %s nonblack-sum=%ld\n",path,sum); (void)!write(2,t,k); } }
    if(!getenv("PELOAD_PRESENT")) return;
    void*(*cur)(void)=dlsym(RTLD_DEFAULT,"CGLGetCurrentContext"); void*ctx=cur?cur():0; if(!ctx){ if(getenv("PELOAD_TRACE_GDI"))(void)!write(2,"  [gdi-present] skip (no GL ctx on this thread)\n",46); return; }
    void*(*flush)(void*)=dlsym(RTLD_DEFAULT,"CGLFlushDrawable");
    void(*gtex)(int,unsigned*)=dlsym(RTLD_DEFAULT,"glGenTextures");
    void(*btex)(unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBindTexture");
    void(*teximg)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glTexImage2D");
    void(*texp)(unsigned,unsigned,int)=dlsym(RTLD_DEFAULT,"glTexParameteri");
    void(*gfb)(int,unsigned*)=dlsym(RTLD_DEFAULT,"glGenFramebuffers");
    void(*bfb)(unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
    void(*fbtex)(unsigned,unsigned,unsigned,unsigned,int)=dlsym(RTLD_DEFAULT,"glFramebufferTexture2D");
    void(*blit)(int,int,int,int,int,int,int,int,unsigned,unsigned)=dlsym(RTLD_DEFAULT,"glBlitFramebuffer");
    if(!flush||!gtex||!btex||!teximg||!gfb||!bfb||!fbtex||!blit) return;
    static unsigned tex=0,fbo=0;
    if(!tex){ gtex(1,&tex); btex(0x0DE1,tex); if(texp){texp(0x0DE1,0x2801,0x2600);texp(0x0DE1,0x2800,0x2600);} }
    btex(0x0DE1,tex);
    teximg(0x0DE1,0,0x8058/*RGBA8*/,w,h,0,0x80E1/*GL_BGRA*/,0x1401/*UNSIGNED_BYTE*/,bits);
    if(!fbo) gfb(1,&fbo);
    bfb(0x8CA8,fbo); fbtex(0x8CA8,0x8CE0,0x0DE1,tex,0); bfb(0x8CA6,0);
    if(topdown) blit(0,0,w,h, 0,WIN_H,WIN_W,0, 0x4000,0x2600);   // flip top-down -> window
    else        blit(0,0,w,h, 0,0,WIN_W,WIN_H, 0x4000,0x2600);   // bottom-up DIB, no flip
    bfb(0x8CA8,0); bfb(0x8CA6,0); flush(ctx);
    if(getenv("PELOAD_TRACE_GDI")){ long sum=0; const uint32_t*p=bits; for(int i=0;i<w*h;i+=131) sum+=p[i]&0xffffff; char t[96]; int k=snprintf(t,sizeof t,"  [gdi-present] %dx%d td=%d nonblack-sum=%ld\n",w,h,topdown,sum); (void)!write(2,t,k);} }
static void gdi_present_window(void){ if(g_gdi_winbuf) gdi_present_surface(g_gdi_winbuf,WIN_W,WIN_H,1); }
static void gdi_present_main(void){
    // Prefer the engine's own composited D3D frame (BGRA, top-down) when captured.
    if(g_d3d_frame_bits && g_d3d_frame_w>0 && g_d3d_frame_h>0 && !getenv("PELOAD_NO_D3D_PRESENT")){
        gdi_present_surface(g_d3d_frame_bits,g_d3d_frame_w,g_d3d_frame_h, getenv("PELOAD_D3D_FLIP")?1:0); return; }
    GdiObj*g=g_gdi_mainsurf; if(g&&g->magic==GDI_MAGIC&&g->bits) gdi_present_surface(g->bits,g->w,g->h,g->topdown); }
static int gdi_has_main(void){ return g_gdi_mainsurf && g_gdi_mainsurf->magic==GDI_MAGIC && g_gdi_mainsurf->bits ? 1:0; }
static int    MSABI i_InvalidateRect(void*h,const void*r,int e){ (void)h;(void)r;(void)e; return 1; }
static int    MSABI i_SetWindowTextW(void*h,const uint16_t*t){ (void)h;(void)t; return 1; }
static void*  MSABI i_GetForegroundWindow(void){ extern void* g_last_hwnd; return g_last_hwnd?g_last_hwnd:(void*)0x4857; }
static int    MSABI i_SetForegroundWindow(void*h){ (void)h; return 1; }
static int    MSABI i_IsWindow(void*h){ return h?1:0; }
static int    MSABI i_IsWindowVisible(void*h){ (void)h; return 1; }
static void*  MSABI i_GetActiveWindow(void){ extern void* g_last_hwnd; return g_last_hwnd?g_last_hwnd:(void*)0x4857; }
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
static id g_nsapp, g_nswin, g_nsctx, g_nspf; extern id NSDefaultRunLoopMode;
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
    id pf = ((id(*)(id,SEL,const uint32_t*))objc_msgSend)(om0(ocls("NSOpenGLPixelFormat"),"alloc"),osel("initWithAttributes:"),attrs); g_nspf=pf;
    id ctx = ((id(*)(id,SEL,id,id))objc_msgSend)(om0(ocls("NSOpenGLContext"),"alloc"),osel("initWithFormat:shareContext:"),pf,(id)0);
    NSRect_ rect={0,0,(double)w,(double)h};
    id win = ((id(*)(id,SEL,NSRect_,unsigned long,unsigned long,signed char))objc_msgSend)(
        om0(ocls("NSWindow"),"alloc"),osel("initWithContentRect:styleMask:backing:defer:"),rect,15/*Titled|Closable|Mini|Resizable*/,2/*Buffered*/,0);
    id title = ((id(*)(id,SEL,const char*))objc_msgSend)(ocls("NSString"),osel("stringWithUTF8String:"),"Project Reclaimer (Mac, Wine-free)");
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("setTitle:"),title);
    id view = om0(win,"contentView");
    ((void(*)(id,SEL,id))objc_msgSend)(g_nsctx=ctx,osel("setView:"),view); // setView wants main thread; fine on the GL thread here
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("center"),(id)0);
    if(getenv("PELOAD_NO_FOCUS")){
        // show the window WITHOUT making it key or activating our app, so it never
        // steals the user's keyboard/focus (used for background testing).
        ((void(*)(id,SEL,id))objc_msgSend)(win,osel("orderFrontRegardless"),(id)0);
    } else {
        ((void(*)(id,SEL,id))objc_msgSend)(win,osel("makeKeyAndOrderFront:"),(id)0);
        ((void(*)(id,SEL,signed char))objc_msgSend)(g_nsapp,osel("activateIgnoringOtherApps:"),1);
    }
    om0(ctx,"makeCurrentContext"); om0(ctx,"update"); // bind + validate the drawable
    oretain(ctx); oretain(win); oretain(pf); g_nswin=win;   // keep alive past any pool drain
    ws("  [cocoa] NSWindow + NSOpenGLContext created (visible)\n");
    return (CGLContextObj)((void*(*)(id,SEL))objc_msgSend)(ctx,osel("CGLContextObj"));
}
// ============================================================================
// PELOAD_D3DGL: live D3D11 -> GL draw-path executor.
// For each engine draw we link the bound (pre-translated) VS and PS, bind the bound
// render target as a GL framebuffer, upload+bind the bound shader-resource textures
// and constant buffers, and issue the draw through real OpenGL. The swapchain
// backbuffer's framebuffer is blitted to a dedicated window at Present. Everything
// runs single-threaded on the engine's render thread (the thread that calls Draw/
// Present) via one private NSOpenGLContext+window, so there is NO cross-thread GL
// sharing. Shaders are separable programs driven by a program pipeline so VS/PS
// uniform namespaces never collide; VS->PS varyings are connected by explicit
// locations injected into the translated GLSL. Opt-in & heavy; off by default.
// ----------------------------------------------------------------------------
static void cocoa_pump(void); // fwd
typedef struct {
    unsigned(*CreateShader)(unsigned); void(*ShaderSource)(unsigned,int,const char*const*,const int*);
    void(*CompileShader)(unsigned); void(*GetShaderiv)(unsigned,unsigned,int*); void(*GetShaderInfoLog)(unsigned,int,int*,char*);
    unsigned(*CreateProgram)(void); void(*AttachShader)(unsigned,unsigned); void(*DetachShader)(unsigned,unsigned);
    void(*DeleteShader)(unsigned); void(*LinkProgram)(unsigned); void(*GetProgramiv)(unsigned,unsigned,int*);
    void(*GetProgramInfoLog)(unsigned,int,int*,char*); void(*ProgramParameteri)(unsigned,unsigned,int);
    void(*GenProgramPipelines)(int,unsigned*); void(*BindProgramPipeline)(unsigned); void(*UseProgramStages)(unsigned,unsigned,unsigned);
    void(*UseProgram)(unsigned); int(*GetUniformLocation)(unsigned,const char*);
    void(*ProgramUniform4fv)(unsigned,int,int,const float*); void(*ProgramUniform1i)(unsigned,int,int);
    void(*GenTextures)(int,unsigned*); void(*BindTexture)(unsigned,unsigned); void(*ActiveTexture)(unsigned);
    void(*TexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*); void(*TexParameteri)(unsigned,unsigned,int);
    void(*GenFramebuffers)(int,unsigned*); void(*BindFramebuffer)(unsigned,unsigned);
    void(*FramebufferTexture2D)(unsigned,unsigned,unsigned,unsigned,int); unsigned(*CheckFramebufferStatus)(unsigned);
    void(*GenVertexArrays)(int,unsigned*); void(*BindVertexArray)(unsigned);
    void(*GenBuffers)(int,unsigned*); void(*BindBuffer)(unsigned,unsigned); void(*BufferData)(unsigned,long,const void*,unsigned);
    void(*VertexAttribPointer)(unsigned,int,unsigned,unsigned char,int,const void*); void(*EnableVertexAttribArray)(unsigned); void(*DisableVertexAttribArray)(unsigned);
    void(*DrawArrays)(unsigned,int,int); void(*DrawElements)(unsigned,int,unsigned,const void*);
    void(*DrawArraysInstanced)(unsigned,int,int,int); void(*DrawElementsInstanced)(unsigned,int,unsigned,const void*,int);
    void(*Viewport)(int,int,int,int); void(*ClearColor)(float,float,float,float); void(*Clear)(unsigned);
    void(*Enable)(unsigned); void(*Disable)(unsigned); void(*BlendFunc)(unsigned,unsigned); unsigned(*GetError)(void); void(*Flush)(void);
    void(*ReadBuffer)(unsigned); void(*ReadPixels)(int,int,int,int,unsigned,unsigned,void*);
    void(*GenRenderbuffers)(int,unsigned*); void(*BindRenderbuffer)(unsigned,unsigned);
    void(*RenderbufferStorage)(unsigned,unsigned,int,int); void(*FramebufferRenderbuffer)(unsigned,unsigned,unsigned,unsigned);
    void(*DepthMask)(unsigned char);
} D3DGLApi;
static void bmp_write_rgba(const char*path,int W,int H,const unsigned char*px); // fwd (defined in gl_selftest section)
static unsigned char* bmp_load_rgba(const char*path,int*pw,int*ph);              // fwd (defined in gl_selftest section)
static D3DGLApi G;
static pthread_mutex_t g_d3dgl_lock=PTHREAD_MUTEX_INITIALIZER; // guards one-time init (race-free)
static int      g_d3dgl_ready;           // init completed (context + pipeline live)
static int      g_d3dgl_offscreen;       // 1 = render-thread offscreen ctx; present via present thread
static id       g_d3dgl_nsctx;           // private NSOpenGLContext (only when we own a window)
static CGLContextObj g_d3dgl_cgl;        // our rendering CGLContextObj (window or offscreen)
static pthread_t g_d3dgl_tid;            // the render thread (first to call us); we only drive that one
static int      g_d3dgl_has_tid;
static unsigned g_d3dgl_pipe;            // program pipeline object
static unsigned g_d3dgl_vao;             // scratch VAO
static int      g_d3dgl_W=1280, g_d3dgl_H=720;
static int      g_d3dgl_fail;            // creation failed -> stop trying
static unsigned g_d3dgl_present_tex;     // backbuffer GL texture to show
static int      g_d3dgl_present_vw, g_d3dgl_present_vh, g_d3dgl_present_tw, g_d3dgl_present_th;
static unsigned g_d3dgl_blit_prog, g_d3dgl_blit_vao;
static int      g_d3dgl_blit_uloc=-2, g_d3dgl_blit_sloc=-2;
static long     g_d3dgl_frame_draws, g_d3dgl_max_draws;
static int      g_d3dgl_trace, g_d3dgl_depth;
// PELOAD_D3DGL_DUMP_RT: record every distinct render target drawn into, then dump each one's GL
// texture to ~/reclaimer_rt_NNN.bmp so we can SEE which RT the geometry rasterizes into (or that it
// rasterizes at all). Answers: present/composite problem vs geometry (matrix/attribute) problem.
static D3DObj*  g_rtseen[128]; static int g_nrtseen, g_rtdumped;
static void d3dgl_note_rt(D3DObj* rt){ if(!rt||!getenv("PELOAD_D3DGL_DUMP_RT")) return;
    for(int i=0;i<g_nrtseen;i++) if(g_rtseen[i]==rt) return; if(g_nrtseen<128) g_rtseen[g_nrtseen++]=rt; }
#define GLE_ARRAY 0x8892
#define GLE_ELEM  0x8893
#define GLE_TEX2D 0x0DE1
#define GLE_FBO   0x8D40
#define GLE_CA0   0x8CE0
#define GLE_FBOK  0x8CD5
#define GLE_TEX0  0x84C0
#define GLE_FLOAT 0x1406
#define GLE_UBYTE 0x1401
#define GLE_US    0x1403
#define GLE_UI    0x1405
#define GLE_RGBA  0x1908
#define GLE_BGRA  0x80E1
#define GLE_RGBA8 0x8058
#define GLE_UI8REV 0x8367
#define GLE_LINEAR 0x2601
#define GLE_CLAMP 0x812F
#define GLE_VS    0x8B31
#define GLE_FS    0x8B30
#define GLE_VSBIT 0x1
#define GLE_FSBIT 0x2
#define GLE_SEPARABLE 0x8258
#define GLE_CSTAT 0x8B81
#define GLE_LSTAT 0x8B82
#define GLE_STREAM 0x88E0
#define GLE_COLORBIT 0x4000
static int d3dgl_loadgl(void){
    static int done,ok;
    if(done) return ok;
    done=1;
    #define L(n) G.n=dlsym(RTLD_DEFAULT,"gl" #n)
    L(CreateShader);L(ShaderSource);L(CompileShader);L(GetShaderiv);L(GetShaderInfoLog);
    L(CreateProgram);L(AttachShader);L(DetachShader);L(DeleteShader);L(LinkProgram);L(GetProgramiv);
    L(GetProgramInfoLog);L(ProgramParameteri);L(GenProgramPipelines);L(BindProgramPipeline);L(UseProgramStages);
    L(UseProgram);L(GetUniformLocation);L(ProgramUniform4fv);L(ProgramUniform1i);
    L(GenTextures);L(BindTexture);L(ActiveTexture);L(TexImage2D);L(TexParameteri);
    L(GenFramebuffers);L(BindFramebuffer);L(FramebufferTexture2D);L(CheckFramebufferStatus);
    L(GenVertexArrays);L(BindVertexArray);L(GenBuffers);L(BindBuffer);L(BufferData);
    L(VertexAttribPointer);L(EnableVertexAttribArray);L(DisableVertexAttribArray);
    L(DrawArrays);L(DrawElements);L(DrawArraysInstanced);L(DrawElementsInstanced);
    L(Viewport);L(ClearColor);L(Clear);L(Enable);L(Disable);L(BlendFunc);L(GetError);L(Flush);
    L(ReadBuffer);L(ReadPixels);
    L(GenRenderbuffers);L(BindRenderbuffer);L(RenderbufferStorage);L(FramebufferRenderbuffer);L(DepthMask);
    #undef L
    ok = G.CreateShader && G.LinkProgram && G.GenProgramPipelines && G.UseProgramStages &&
         G.GenFramebuffers && G.DrawArrays && G.ProgramUniform4fv;
    if(!ok) ws("  [d3dgl] GL symbols missing (need 4.1 core + separable programs)\n");
    return ok;
}
// Create a private NSWindow + NSOpenGLContext on THIS (render) thread and make it current.
static int d3dgl_make_window(void){
    objc_autoreleasePoolPush();
    id app = om0(ocls("NSApplication"),"sharedApplication");
    ((void(*)(id,SEL,long))objc_msgSend)(app,osel("setActivationPolicy:"),0);
    om0(app,"finishLaunching");
    uint32_t attrs[]={5,99,0x4100,8,24,11,8,12,24,13,8,73,72,0};
    id pf = ((id(*)(id,SEL,const uint32_t*))objc_msgSend)(om0(ocls("NSOpenGLPixelFormat"),"alloc"),osel("initWithAttributes:"),attrs);
    if(!pf){ ws("  [d3dgl] pixel format failed\n"); return 0; }
    id ctx = ((id(*)(id,SEL,id,id))objc_msgSend)(om0(ocls("NSOpenGLContext"),"alloc"),osel("initWithFormat:shareContext:"),pf,(id)0);
    if(!ctx){ ws("  [d3dgl] NSOpenGLContext failed\n"); return 0; }
    NSRect_ rect={0,0,(double)g_d3dgl_W,(double)g_d3dgl_H};
    id win = ((id(*)(id,SEL,NSRect_,unsigned long,unsigned long,signed char))objc_msgSend)(
        om0(ocls("NSWindow"),"alloc"),osel("initWithContentRect:styleMask:backing:defer:"),rect,15,2,0);
    id title = ((id(*)(id,SEL,const char*))objc_msgSend)(ocls("NSString"),osel("stringWithUTF8String:"),"Project Reclaimer — halo3 D3D11->GL");
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("setTitle:"),title);
    id view = om0(win,"contentView");
    ((void(*)(id,SEL,id))objc_msgSend)(ctx,osel("setView:"),view);
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("center"),(id)0);
    if(getenv("PELOAD_NO_FOCUS")) ((void(*)(id,SEL,id))objc_msgSend)(win,osel("orderFrontRegardless"),(id)0);
    else { ((void(*)(id,SEL,id))objc_msgSend)(win,osel("makeKeyAndOrderFront:"),(id)0);
           ((void(*)(id,SEL,signed char))objc_msgSend)(app,osel("activateIgnoringOtherApps:"),1); }
    om0(ctx,"makeCurrentContext"); om0(ctx,"update");
    oretain(ctx); oretain(win); oretain(pf);
    g_nsapp=app; // enables cocoa_pump
    g_d3dgl_nsctx=ctx; g_d3dgl_cgl=(CGLContextObj)((void*(*)(id,SEL))objc_msgSend)(ctx,osel("CGLContextObj"));
    ws("  [d3dgl] NSWindow + GL context created (main thread)\n");
    return 1;
}
// Render-thread path: a windowless CGL context (AppKit windows must not be created off the main
// thread — doing so aborts). It shares objects with the window/present context (g_present_glctx)
// when one exists, so the present thread can display our backbuffer texture.
static int d3dgl_make_offscreen(void){
    CGLPixelFormatAttribute at[]={kCGLPFAAccelerated,kCGLPFADoubleBuffer,kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,kCGLPFADepthSize,(CGLPixelFormatAttribute)24,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf=0; GLint n=0; CGLError e; int pf_borrowed=0;
    CGLContextObj share=(CGLContextObj)g_present_glctx; CGLContextObj ctx=0;
    // FIRST choice: build this (render) context from the PRESENT context's OWN pixel format, so the
    // two share compatibly. CGLCreateContext returns kCGLBadMatch (10009) when the share context's
    // pixel format is incompatible with the new context's -- which was dropping us to an UNSHARED
    // context, so the present thread could never see our rendered backbuffer texture => black screen
    // (and the loading-screen artwork never appeared). Reusing the present's NSOpenGLPixelFormat's
    // CGLPixelFormatObj makes the match exact. HOWEVER: with sharing ON, the present thread displays
    // the render thread's (currently BLACK) backbuffer texture INSTEAD of the GDI text overlay it
    // otherwise shows, so until the render thread actually produces content this makes the window
    // WORSE (black vs text). Default OFF; enable with PELOAD_D3DGL_SHAREPF once the render output is real.
    if(share && g_nspf && getenv("PELOAD_D3DGL_SHAREPF")){
        CGLPixelFormatObj ppf=(CGLPixelFormatObj)((void*(*)(id,SEL))objc_msgSend)(g_nspf,osel("CGLPixelFormatObj"));
        if(ppf){ e=CGLCreateContext(ppf,share,&ctx);
            if(e==kCGLNoError&&ctx){ pf=ppf; pf_borrowed=1; }
            else { char m[112]; int k=snprintf(m,sizeof m,"  [d3dgl] shared via present PF err=%d; trying own PF\n",e); (void)!write(2,m,k); ctx=0; } } }
    if(!ctx){
        e=CGLChoosePixelFormat(at,&pf,&n);
        if(e!=kCGLNoError||!pf){
            char m[96]; int k=snprintf(m,sizeof m,"  [d3dgl] CGLChoosePixelFormat(full) err=%d; retrying minimal\n",e); (void)!write(2,m,k);
            CGLPixelFormatAttribute b[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,(CGLPixelFormatAttribute)0};
            e=CGLChoosePixelFormat(b,&pf,&n);
            if(e!=kCGLNoError||!pf){ char m2[80]; int k2=snprintf(m2,sizeof m2,"  [d3dgl] CGLChoosePixelFormat(min) err=%d\n",e); (void)!write(2,m2,k2); return 0; } }
        // share with the present/window context if possible; else fall back to an unshared context
        // (capture via PELOAD_D3DGL_SHOT still works; live display may not).
        e=CGLCreateContext(pf,share,&ctx);
        if((e!=kCGLNoError||!ctx) && share){ char m[96]; int k=snprintf(m,sizeof m,"  [d3dgl] CGLCreateContext(shared) err=%d; retrying unshared\n",e); (void)!write(2,m,k);
            e=CGLCreateContext(pf,0,&ctx); if(e==kCGLNoError&&ctx) share=0; }
    }
    if(pf && !pf_borrowed) CGLDestroyPixelFormat(pf);
    if(e!=kCGLNoError||!ctx){ char m[80]; int k=snprintf(m,sizeof m,"  [d3dgl] CGLCreateContext err=%d\n",e); (void)!write(2,m,k); return 0; }
    CGLError se=CGLSetCurrentContext(ctx);
    if(se!=kCGLNoError){ char m[80]; int k=snprintf(m,sizeof m,"  [d3dgl] CGLSetCurrentContext err=%d\n",se); (void)!write(2,m,k); return 0; }
    g_d3dgl_cgl=ctx; g_d3dgl_nsctx=0;
    char m[88]; int k=snprintf(m,sizeof m,"  [d3dgl] offscreen GL context (render thread; shared=%d)\n",share?1:0); (void)!write(2,m,k);
    return 1;
}
static int d3dgl_ensure(void){
    if(g_d3dgl_ready){
        // only the thread that created the context may drive GL on it
        if(g_d3dgl_has_tid && !pthread_equal(g_d3dgl_tid,pthread_self())) return 0;
        if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
        return 1;
    }
    if(g_d3dgl_fail) return 0;
    // one-time init, serialized so two render threads can't double-create the context/window
    pthread_mutex_lock(&g_d3dgl_lock);
    if(g_d3dgl_ready || g_d3dgl_fail){ pthread_mutex_unlock(&g_d3dgl_lock);
        if(g_d3dgl_fail) return 0;
        if(g_d3dgl_has_tid && !pthread_equal(g_d3dgl_tid,pthread_self())) return 0;
        if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl); return 1; }
    g_d3dgl_tid=pthread_self(); g_d3dgl_has_tid=1;
    g_d3dgl_trace=getenv("PELOAD_TRACE_D3DGL")?1:0; g_d3dgl_depth=getenv("PELOAD_D3DGL_DEPTH")?1:0;
    g_d3dgl_max_draws=getenv("PELOAD_D3DGL_MAX")?atol(getenv("PELOAD_D3DGL_MAX")):0;
    if(getenv("PELOAD_D3DGL_W")) g_d3dgl_W=atoi(getenv("PELOAD_D3DGL_W"));
    if(getenv("PELOAD_D3DGL_H")) g_d3dgl_H=atoi(getenv("PELOAD_D3DGL_H"));
    int made=0;
    if(d3dgl_loadgl()){
        // Only create an AppKit window when we are on the main thread; otherwise go offscreen and
        // let the present thread (which safely owns the window) display our backbuffer.
        if(pthread_main_np() && !getenv("PELOAD_D3DGL_FORCE_OFFSCREEN")){ made=d3dgl_make_window(); g_d3dgl_offscreen=0; }
        if(!made){ made=d3dgl_make_offscreen(); g_d3dgl_offscreen=1; }
    }
    if(!made){ ws("  [d3dgl] context creation FAILED (loadgl/make_offscreen returned 0)\n"); g_d3dgl_fail=1; g_d3dgl_en=0; pthread_mutex_unlock(&g_d3dgl_lock); return 0; }
    G.GenProgramPipelines(1,&g_d3dgl_pipe); G.BindProgramPipeline(g_d3dgl_pipe);
    G.GenVertexArrays(1,&g_d3dgl_vao); G.BindVertexArray(g_d3dgl_vao);
    __sync_synchronize(); g_d3dgl_ready=1;        // publish after everything is built
    pthread_mutex_unlock(&g_d3dgl_lock);
    ws("  [d3dgl] executor ready\n");
    return 1;
}
// Inject explicit varying/attribute/fragdata locations (so VS outputs connect to PS
// inputs across separable programs) and, for a VS, the required gl_PerVertex block.
// Rewrites lines beginning "in vec4 v<N>" / "out vec4 o<N>" -> "layout(location=<N>) ...".
// Map a D3D varying SEMANTIC (e.g. "TEXCOORD0") to a stable GL interface location, assigned on first
// sight and shared across every shader. A VS output and a PS input that carry the SAME semantic then
// get the SAME location, so a monolithic VS+PS link matches them (their raw register numbers differ
// between stages). Separate namespace from vertex attributes / render targets, so 0.. is fine.
static long d3dgl_sem_loc(const char*sem){
    static char tbl[30][32]; static int n;
    for(int i=0;i<n;i++) if(!strcmp(tbl[i],sem)) return i;
    if(n<30){ int j=0; for(;sem[j]&&j<31;j++) tbl[n][j]=sem[j]; tbl[n][j]=0; return n++; }
    return -1;
}
static char* d3dgl_inject(const char*src,int isvs,int mono){
    size_t cap=strlen(src)+4096, len=0; char*out=malloc(cap); if(!out) return 0;
    #define PUT(s,n) do{ if(len+(n)+1>cap){cap=(len+(n)+1)*2; char*nn=realloc(out,cap); if(!nn){free(out);return 0;} out=nn;} memcpy(out+len,(s),(n)); len+=(n);}while(0)
    const char*p=src;
    int injected_pv=0;
    while(*p){
        const char*nl=strchr(p,'\n'); size_t ln = nl? (size_t)(nl-p+1) : strlen(p);
        // after the #version line, inject gl_PerVertex for a vertex shader
        // detect "in vec4 vN" or "out vec4 oN" at line start (allow leading spaces)
        const char*q=p; while(*q==' '||*q=='\t') q++;
        int is_in = !strncmp(q,"in vec4 v",9);
        int is_out= !strncmp(q,"out vec4 o",10);
        if(is_in||is_out){
            const char*d = q + (is_in?9:10); long num=0; int has=0,ovf=0;
            while(*d>='0'&&*d<='9'){ num=num*10+(*d-'0'); d++; has=1; if(num>65535){ovf=1;break;} }
            long loc=num;
            // Inter-stage varyings (a VS output, or a PS input) get a location keyed by their D3D
            // SEMANTIC so the two stages agree; vertex attributes (VS input) and render-target outputs
            // (PS output) keep the register index (they bind to attrib arrays / draw buffers by index).
            int varying = (isvs && is_out) || (!isvs && is_in);
            if(varying && has){
                const char*c=strstr(q,"//");
                if(c && c < q+ (nl?(size_t)(nl-q):strlen(q)) ){ c+=2; while(*c==' '||*c=='\t')c++;
                    char sem[32]; int si=0; while(c[si]&&c[si]!='\n'&&c[si]!='\r'&&c[si]!=' '&&si<31){ sem[si]=c[si]; si++; } sem[si]=0;
                    if(si>0){ long sl=d3dgl_sem_loc(sem); if(sl>=0) loc=sl; } }
            }
            // only inject a valid, small location (valid GL locations are small; a huge/overflowing
            // register like SV_Depth's 0xFFFFFFFF must never become a negative layout(location=)).
            if(has && !ovf && loc<64){ char pre[40]; int k=snprintf(pre,sizeof pre,"layout(location=%ld) ",loc); PUT(pre,k); }
        }
        PUT(p,ln);
        // gl_PerVertex redeclaration is REQUIRED for a separable VS program, but must be OMITTED for a
        // monolithic (VS+FS linked) program -- Apple GL-on-Metal rejects the redecl there. Mono path skips it.
        if(isvs && !mono && !injected_pv && !strncmp(q,"#version",8)){
            const char*pv="out gl_PerVertex { vec4 gl_Position; };\n"; PUT(pv,strlen(pv)); injected_pv=1;
        }
        p+=ln; if(!nl) break;
    }
    PUT("",0); out[len]=0;
    #undef PUT
    return out;
}
// Build (or fetch cached) a separable program for a shader D3DObj. On failure, free the
// GLSL so the draw is skipped and we don't retry. Returns the GL program (0 = unavailable).
static unsigned d3dgl_prog(D3DObj*o,unsigned glstage){
    if(!o) return 0;
    if(o->glprog) return o->glprog;
    if(!o->glsl) return 0;
    char*inj=d3dgl_inject(o->glsl, glstage==GLE_VS, 0); if(!inj) return 0;
    unsigned sh=G.CreateShader(glstage); const char*cp=inj; G.ShaderSource(sh,1,&cp,0); G.CompileShader(sh);
    int ok=0; G.GetShaderiv(sh,GLE_CSTAT,&ok);
    if(!ok){ if(g_d3dgl_trace){ char log[1024]={0}; if(G.GetShaderInfoLog)G.GetShaderInfoLog(sh,1024,0,log);
            ws("  [d3dgl] shader compile FAIL: "); ws(log); ws("\n"); }
        G.DeleteShader(sh); free(inj); free(o->glsl); o->glsl=0; return 0; }
    unsigned prog=G.CreateProgram(); G.ProgramParameteri(prog,GLE_SEPARABLE,1);
    G.AttachShader(prog,sh); G.LinkProgram(prog); G.DetachShader(prog,sh); G.DeleteShader(sh); free(inj);
    int lok=0; G.GetProgramiv(prog,GLE_LSTAT,&lok);
    if(!lok){ if(g_d3dgl_trace){ char log[1024]={0}; if(G.GetProgramInfoLog)G.GetProgramInfoLog(prog,1024,0,log);
            ws("  [d3dgl] program link FAIL: "); ws(log); ws("\n"); }
        free(o->glsl); o->glsl=0; return 0; }
    o->glprog=prog;
    // populate the uniform-location cache for this program's shader
    o->gl_locinit=1; char nm[16];
    for(int b=0;b<16;b++){ snprintf(nm,sizeof nm,"cb%d",b); o->gl_cbloc[b]=G.GetUniformLocation(prog,nm); }
    for(int r=0;r<32;r++){ snprintf(nm,sizeof nm,"t%d",r);  o->gl_tloc[r] =G.GetUniformLocation(prog,nm); }
    if(g_d3dgl_trace){ static int n; if(++n<=40){ char m[64]; int k=snprintf(m,sizeof m,"  [d3dgl] linked program #%d (%s)\n",n,glstage==GLE_VS?"vs":"ps"); (void)!write(2,m,k);} }
    return prog;
}
// DXGI texture format -> GL (internalFmt unused; we always store RGBA8). Returns upload
// (format,type) for 32bpp classes; 0 for formats we can't upload yet (BC* etc).
static int d3dgl_tex_upload_fmt(uint32_t f,unsigned*gfmt,unsigned*gtype){
    switch(f){
        case 27: case 28: case 29: *gfmt=GLE_RGBA; *gtype=GLE_UBYTE; return 1;   // R8G8B8A8_TYPELESS/UNORM(_SRGB)
        case 87: case 88: case 90: case 91: case 92: case 93: *gfmt=GLE_BGRA; *gtype=GLE_UI8REV; return 1; // B8G8R8A8/X8 UNORM/TYPELESS/SRGB (90 = the loading-screen artwork texture)
        default: return 0;
    }
}
// BC/DXT block-compressed DXGI formats -> GL internal format + block byte size. These are the real
// artwork/texture assets (map hero photo, decals, UI art): D3D ships them compressed, and without
// this path d3dgl_tex_upload_fmt rejected them -> magenta/black (the "missing hero artwork"). GL
// uploads them untouched via glCompressedTexImage2D. S3TC(BC1-3) + RGTC(BC4-5) are available on
// Apple's GL; BPTC(BC6-7) is GL 4.2 core and may be unsupported on the 4.1 GL-on-Metal context, so
// those fall through to the placeholder if the driver rejects the enum (checked via glGetError).
static int d3dgl_bc_glfmt(uint32_t f,unsigned*ifmt,int*blkbytes){
    switch(f){
        case 70: case 71: *ifmt=0x83F1; *blkbytes=8;  return 1; // BC1_UNORM        RGBA_S3TC_DXT1
        case 72:          *ifmt=0x8C4D; *blkbytes=8;  return 1; // BC1_UNORM_SRGB    SRGB_ALPHA_S3TC_DXT1
        case 73: case 74: *ifmt=0x83F2; *blkbytes=16; return 1; // BC2_UNORM        RGBA_S3TC_DXT3
        case 75:          *ifmt=0x8C4E; *blkbytes=16; return 1; // BC2_UNORM_SRGB
        case 76: case 77: *ifmt=0x83F3; *blkbytes=16; return 1; // BC3_UNORM        RGBA_S3TC_DXT5
        case 78:          *ifmt=0x8C4F; *blkbytes=16; return 1; // BC3_UNORM_SRGB
        case 79: case 80: *ifmt=0x8DBB; *blkbytes=8;  return 1; // BC4_UNORM        RED_RGTC1
        case 81:          *ifmt=0x8DBC; *blkbytes=8;  return 1; // BC4_SNORM
        case 82: case 83: *ifmt=0x8DBD; *blkbytes=16; return 1; // BC5_UNORM        RG_RGTC2
        case 84:          *ifmt=0x8DBE; *blkbytes=16; return 1; // BC5_SNORM
        case 94: case 95: *ifmt=0x8E8F; *blkbytes=16; return 1; // BC6H_UF16        RGB_BPTC_UF
        case 96:          *ifmt=0x8E8E; *blkbytes=16; return 1; // BC6H_SF16
        case 97: case 98: *ifmt=0x8E8C; *blkbytes=16; return 1; // BC7_UNORM        RGBA_BPTC_UNORM
        case 99:          *ifmt=0x8E8D; *blkbytes=16; return 1; // BC7_UNORM_SRGB
        default: return 0;
    }
}
// PELOAD_D3DGL_DUMPGLTEX: read the currently-bound GL texture back (glGetTexImage) + save a BMP, so we
// can see whether the (BC) decode produced a real map texture vs dark/garbage. Caller gates the env.
static void dg_gltex_dump(int w,int h,unsigned fmt){
    if(!getenv("PELOAD_D3DGL_DUMPGLTEX") || w<256 || h<256) return;
    if(!(((fmt>=70&&fmt<=84)||(fmt>=94&&fmt<=99)))) return;   // BC/DXT only (the map textures)
    static int gt2; if(gt2>=8) return;
    void(*gti)(unsigned,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glGetTexImage"); if(!gti) return;
    unsigned char*buf=calloc(1,(size_t)w*h*4); if(!buf) return; gti(GLE_TEX2D,0,GLE_RGBA,GLE_UBYTE,buf);
    const char*home=getenv("HOME"); char p[160]; snprintf(p,sizeof p,"%s/reclaimer_gltex_%d_%dx%d_f%u.bmp",home?home:"/tmp",gt2,w,h,fmt);
    FILE*f=fopen(p,"wb"); if(f){ uint32_t isz=(uint32_t)w*h*4,fsz=54+isz,oo=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&oo,4);
        uint8_t ih[40]={0}; uint32_t v=40; memcpy(ih,&v,4); memcpy(ih+4,&w,4); memcpy(ih+8,&h,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
        fwrite(fh,1,14,f); fwrite(ih,1,40,f); for(int y=h-1;y>=0;y--){ unsigned char*row=buf+(size_t)y*w*4; for(int x=0;x<w;x++){ unsigned char*px=row+x*4; unsigned char b[4]={px[2],px[1],px[0],255}; fwrite(b,1,4,f);} } fclose(f);
        char m[200]; int k=snprintf(m,sizeof m,"  [gltex] dumped %dx%d f%u -> %s\n",w,h,fmt,p); (void)!write(2,m,k); } free(buf); gt2++;
}
static void d3dgl_ensure_tex(D3DObj*o){
    if(!o) return;
    if(!o->gltex){ G.GenTextures(1,&o->gltex); G.BindTexture(GLE_TEX2D,o->gltex);
        G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2800,GLE_LINEAR);
        G.TexParameteri(GLE_TEX2D,0x2802,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x2803,GLE_CLAMP);
        o->gl_uploaded=0; }
    if(o->gl_uploaded && !o->gl_dirty) return;
    G.BindTexture(GLE_TEX2D,o->gltex);
    unsigned gf,gt; int w=o->w?(int)o->w:1, h=o->h?(int)o->h:1; if(w>4096)w=4096; if(h>4096)h=4096;
    // Block-compressed (BC/DXT) upload: the real artwork assets ship compressed. Upload level 0
    // untouched; GL decodes it. Only when the backing buffer holds the level-0 block bytes.
    unsigned bcif; int bcbb;
    if(!getenv("PELOAD_NO_BC") && o->data && d3dgl_bc_glfmt(o->fmt,&bcif,&bcbb)){
        uint64_t csz=(uint64_t)((w+3)/4)*((h+3)/4)*bcbb;
        static void(*cti)(unsigned,int,unsigned,int,int,int,int,const void*);
        static int cti_init; if(!cti_init){ cti=dlsym(RTLD_DEFAULT,"glCompressedTexImage2D"); cti_init=1; }
        unsigned(*gerr)(void)=dlsym(RTLD_DEFAULT,"glGetError");
        if(cti && o->size>=csz){
            if(gerr) gerr(); // clear
            cti(GLE_TEX2D,0,bcif,w,h,0,(int)csz,o->data);
            unsigned e=gerr?gerr():0;
            if(getenv("PELOAD_TRACE_TEXUP")){ static int bc; if(bc++<40){ char b[160]; int k=snprintf(b,sizeof b,"  [texup-bc] %dx%d fmt=%u ifmt=0x%x csz=%lluB err=0x%x%s\n",w,h,o->fmt,bcif,(unsigned long long)csz,e,e?" (FALLBACK)":""); (void)!write(2,b,k);} }
            if(!e){ o->gl_uploaded=1; o->gl_dirty=0; dg_gltex_dump(w,h,o->fmt); return; } // success: BPTC may fail on GL4.1 -> fall through
        }
    }
    // Only upload real pixels when the backing buffer actually holds >= w*h*4 bytes; otherwise
    // glTexImage2D would read past o->data (heap OOB -> crash). Fall back to a 1x1 placeholder.
    int can = o->data && d3dgl_tex_upload_fmt(o->fmt,&gf,&gt) && o->size>=(uint64_t)w*h*4;
    // PELOAD_TRACE_TEXUP: for sizeable textures, report w/h/fmt/can + whether the backing data is all
    // black (so we can tell if the loading-screen 1280x720 texture is uploaded empty vs the engine
    // actually rendered content into it).
    if(getenv("PELOAD_TRACE_TEXUP") && w>=256 && h>=256){ uint64_t sum=0; int nz=0;
        if(o->data && o->size>=1024){ const uint8_t*d=o->data; uint64_t step=o->size/4096; if(step<1)step=1;
            for(uint64_t i=0;i<o->size && nz<32; i+=step){ if(d[i]){ sum+=d[i]; nz++; } } }
        static int tc; if(tc++<60){ char b[160]; int k=snprintf(b,sizeof b,"  [texup] %dx%d fmt=%u size=%lluB can=%d data=%p nonzero-samples=%d sum=%llu%s\n",
            w,h,o->fmt,(unsigned long long)o->size,can,o->data,nz,(unsigned long long)sum,nz?"":" (ALL BLACK)"); (void)!write(2,b,k);} }
    if(can) G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,w,h,0,gf,gt,o->data);
    else { static const unsigned char mag[4]={255,0,255,255}; G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,1,1,0,GLE_RGBA,GLE_UBYTE,mag); }
    o->gl_uploaded=1; o->gl_dirty=0;
    dg_gltex_dump(w,h,o->fmt);
}
static void d3dgl_ensure_rt(D3DObj*o){
    if(!o) return;
    int w=o->w?(int)o->w:g_d3dgl_W, h=o->h?(int)o->h:g_d3dgl_H; if(w>4096)w=4096; if(h>4096)h=4096;
    if(!o->gltex){ G.GenTextures(1,&o->gltex); G.BindTexture(GLE_TEX2D,o->gltex);
        G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2800,GLE_LINEAR);
        G.TexParameteri(GLE_TEX2D,0x2802,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x2803,GLE_CLAMP);
        G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,w,h,0,GLE_RGBA,GLE_UBYTE,0); o->gl_uploaded=1; }
    if(!o->glfbo){ G.GenFramebuffers(1,&o->glfbo); G.BindFramebuffer(GLE_FBO,o->glfbo);
        G.FramebufferTexture2D(GLE_FBO,GLE_CA0,GLE_TEX2D,o->gltex,0);
        // attach a depth renderbuffer so geometry depth-tests correctly (opt-in; adds VRAM per RT)
        if(g_d3dgl_depth && G.GenRenderbuffers){ unsigned rb=0; G.GenRenderbuffers(1,&rb); G.BindRenderbuffer(0x8D41/*RENDERBUFFER*/,rb);
            G.RenderbufferStorage(0x8D41,0x81A6/*DEPTH_COMPONENT24*/,w,h);
            G.FramebufferRenderbuffer(GLE_FBO,0x8D00/*DEPTH_ATTACHMENT*/,0x8D41,rb); } }
}
static unsigned d3dgl_topo(uint32_t t){ switch(t){ case 1:return 0; /*POINTS*/ case 2:return 1;/*LINES*/ case 3:return 3;/*LINE_STRIP*/
    case 5:return 5;/*TRIANGLE_STRIP*/ default:return 4;/*TRIANGLES*/ } }
// Upload the bound D3D constant buffers into one program's "cb<b>"-style uniforms, given that
// program and its per-register uniform-location table (so the monolithic path can pass the mono
// program + the stage-prefixed vXcb/pXcb locations, and the separable path passes the stage's own).
static void d3dgl_bind_cbs_p(unsigned prog,const int*cbloc,D3DObj**cbarr,const uint32_t*first,const uint32_t*cnt){
    if(!prog) return;
    for(int b=0;b<16;b++){ int loc=cbloc[b]; if(loc<0) continue; D3DObj*cb=cbarr[b]; if(!cb||!cb->data||!cb->size) continue;
        uint64_t offb=(uint64_t)(first?first[b]:0)*16;            // *SetConstantBuffers1 sub-range offset
        if(offb>=cb->size) offb=0;
        int n=(cnt&&cnt[b])?(int)cnt[b]:(int)((cb->size-offb)/16);
        int avail=(int)((cb->size-offb)/16); if(n>avail)n=avail; if(n<1)n=1; if(n>4096)n=4096;
        G.ProgramUniform4fv(prog,loc,n,(const float*)((const char*)cb->data+offb)); }
}
static void d3dgl_bind_cbs(D3DObj*sh,D3DObj**cbarr,const uint32_t*first,const uint32_t*cnt){
    if(!sh) return; d3dgl_bind_cbs_p(sh->glprog,sh->gl_cbloc,cbarr,first,cnt);
}
// Synthesized view-projection (PELOAD_D3DGL_SYNVP). cb0 = P(FOV 70) * V, V = the camera view matrix
// found at runtime (fresh pool CB whose P*V projects the current object on-screen). See cb0find.
static D3DObj g_syn_cb0; static uint8_t g_syn_cb0_buf[256];
static void d3dgl_mul44(const float*A,const float*B,float*O){ for(int r=0;r<4;r++)for(int c=0;c<4;c++){ float a=0; for(int k=0;k<4;k++)a+=A[r*4+k]*B[k*4+c]; O[r*4+c]=a; } }
static int d3dgl_synth_cb0(void){
    D3DObj*b=g_last_b12; if(!b||!b->data) return 0;
    const float*wf=(const float*)b->data; float wp[4]={wf[3],wf[7],wf[11],1.0f};   // object world origin
    float aspect=(g_d3dst.vp_h>0)?(float)g_d3dst.vp_w/(float)g_d3dst.vp_h:1.0f; if(aspect<0.2f||aspect>5.0f)aspect=1.0f;
    float fovy=70.0f*3.14159265f/180.0f, sy=1.0f/tanf(fovy*0.5f), sx=sy/aspect, n=0.0625f, fr=1000.0f;
    float P[16]={ sx,0,0,0, 0,sy,0,0, 0,0,fr/(fr-n),-n*fr/(fr-n), 0,0,1,0 };
    extern float g_b12_org[32][3]; extern int g_nb12_org;
    // PELOAD_D3DGL_CAM="ex,ey,ez[,tx,ty,tz[,ux,uy,uz]]": build a look-at VIEW matrix (column-vector
    // convention: V*[world,1]=view) and use P*V as cb0, bypassing the pool-CB search (the real
    // view-proj is in NO cbuffer -- cb0find only ever finds per-object matrices). Target defaults to
    // the scene center (avg of the known object origins); up defaults to +Z (Halo is Z-up).
    const char*camenv=getenv("PELOAD_D3DGL_CAM");
    if(camenv){
        float e[3]={0,0,0},t[3]={0,0,0},up[3]={0,0,1}; int ne=sscanf(camenv,"%f,%f,%f,%f,%f,%f,%f,%f,%f",
            &e[0],&e[1],&e[2],&t[0],&t[1],&t[2],&up[0],&up[1],&up[2]);
        if(ne<6){ float c[3]={0,0,0}; int nn=g_nb12_org>0?g_nb12_org:1; for(int o=0;o<g_nb12_org;o++){ c[0]+=g_b12_org[o][0]; c[1]+=g_b12_org[o][1]; c[2]+=g_b12_org[o][2]; } t[0]=c[0]/nn; t[1]=c[1]/nn; t[2]=c[2]/nn; }
        float z[3]={t[0]-e[0],t[1]-e[1],t[2]-e[2]}; float zl=sqrtf(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]); if(zl<1e-6f)zl=1; z[0]/=zl;z[1]/=zl;z[2]/=zl;
        float x[3]={up[1]*z[2]-up[2]*z[1], up[2]*z[0]-up[0]*z[2], up[0]*z[1]-up[1]*z[0]}; float xl=sqrtf(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); if(xl<1e-6f)xl=1; x[0]/=xl;x[1]/=xl;x[2]/=xl;
        float y[3]={z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0]};
        float Vm[16]={ x[0],x[1],x[2], -(x[0]*e[0]+x[1]*e[1]+x[2]*e[2]),
                       y[0],y[1],y[2], -(y[0]*e[0]+y[1]*e[1]+y[2]*e[2]),
                       z[0],z[1],z[2], -(z[0]*e[0]+z[1]*e[1]+z[2]*e[2]),
                       0,0,0,1 };
        float*out=(float*)g_syn_cb0_buf; memset(out,0,256); d3dgl_mul44(P,Vm,out);
        if(getenv("PELOAD_SYNVP_OBJ")) out[48]=out[49]=out[50]=out[51]=1.0f;
        g_syn_cb0.data=g_syn_cb0_buf; g_syn_cb0.size=256; g_syn_cb0.is_cb=1;
        if(getenv("PELOAD_TRACE_D3DGL")){ static int cc; if(cc++<2){ char m[200]; int k=snprintf(m,sizeof m,"  [cam] eye=[%.1f %.1f %.1f] tgt=[%.1f %.1f %.1f] cb0row0=[%.3f %.3f %.3f %.3f]\n",e[0],e[1],e[2],t[0],t[1],t[2],out[0],out[1],out[2],out[3]); (void)!write(2,m,k);} }
        return 1;
    }
    // camera VIEW = the fresh small CB whose P*V projects the MOST per-object origins on-screen (LH, V
    // as-is -- determined empirically, 30/32). Re-detect every frame; cheap (a few small CBs).
    // PELOAD_SYNVP_CB=<i> forces g_cbreg[i] as V (debug: confirm the cb0find winner renders).
    D3DObj* V=0; int bestN=1;
    const char*force=getenv("PELOAD_SYNVP_CB");
    if(force){ int fi=atoi(force); if(fi>=0&&fi<g_ncbreg&&g_cbreg[fi]&&g_cbreg[fi]->data) V=g_cbreg[fi]; }
    else
    for(int i=0;i<g_ncbreg;i++){ D3DObj*c=g_cbreg[i]; if(!c||!c->data||c->size<64||c->size>512) continue; if((g_cb_tick-c->cb_upd)>2) continue;
        const float*vm=(const float*)c->data; int bad=0; for(int j=0;j<16;j++){ float v=vm[j]; if(v!=v||v<-1e9f||v>1e9f){bad=1;break;} } if(bad)continue;
        // Reject a near-identity rotation (the real camera view has a non-trivial basis): require the
        // upper-left 3x3 to differ meaningfully from I, else a trivial "looking down +z from origin"
        // matrix wins the on-screen count without being the actual camera.
        float idd=fabsf(vm[0]-1)+fabsf(vm[5]-1)+fabsf(vm[10]-1)+fabsf(vm[1])+fabsf(vm[2])+fabsf(vm[4])+fabsf(vm[6])+fabsf(vm[8])+fabsf(vm[9]);
        if(idd<0.25f) continue;
        float VP[16]; d3dgl_mul44(P,vm,VP); int on=0;
        for(int o=0;o<g_nb12_org;o++){ float cl[4]; for(int r=0;r<4;r++) cl[r]=VP[r*4]*g_b12_org[o][0]+VP[r*4+1]*g_b12_org[o][1]+VP[r*4+2]*g_b12_org[o][2]+VP[r*4+3];
            if(cl[3]>0.05f){ float nx=cl[0]/cl[3],ny=cl[1]/cl[3],nz=cl[2]/cl[3]; if(nx>-1.1f&&nx<1.1f&&ny>-1.1f&&ny<1.1f&&nz>-0.05f&&nz<1.05f) on++; } }
        if(on>bestN){ bestN=on; V=c; } }
    if(!V||!V->data) return 0;
    float*out=(float*)g_syn_cb0_buf; memset(out,0,256);
    d3dgl_mul44(P,(const float*)V->data,out);           // cb0[0..3] = P*V (view-projection)
    // cb0[12] gates the vertex path: !=0 => decompress+world (needs cb3+cb12, both unbound here);
    // ==0 => r1=v0 directly (pre-transformed world/BSP geometry) -> only cb0 needed. Use the world path
    // unless PELOAD_SYNVP_OBJ asks for the object path. Default 0 renders the static map geometry.
    if(getenv("PELOAD_SYNVP_OBJ")) out[48]=out[49]=out[50]=out[51]=1.0f;
    g_syn_cb0.data=g_syn_cb0_buf; g_syn_cb0.size=256; g_syn_cb0.is_cb=1;
    if(getenv("PELOAD_TRACE_D3DGL")){ static int c; if(c++<3){ char m[200]; int k=snprintf(m,sizeof m,"  [synvp] cb0 from V=%p: row0=[%.3f %.3f %.3f %.3f] row3=[%.3f %.3f %.3f %.3f]\n",(void*)V,out[0],out[1],out[2],out[3],out[12],out[13],out[14],out[15]); (void)!write(2,m,k);} }
    return 1; }
static void d3dgl_bind_texs_p(unsigned prog,const int*tloc){
    if(!prog) return; int unit=0;
    // halo3 binds the map/material textures to high, SPARSE SRV slots (seen: t13,t14,t16,t17), not just
    // t0-t7. Cover slots 0..31, and PACK each declared sampler into the next contiguous GL texture unit
    // (Apple GL-on-Metal has ~16 fragment units, so binding to unit 17 would fail) -- the sampler uniform
    // is set to the packed unit, so the shader samples the right texture.
    for(int r=0;r<32 && unit<16;r++){ int loc=tloc[r]; if(loc<0) continue; D3DObj*tx=g_d3dst.psrv[r];
        G.ActiveTexture(GLE_TEX0+unit);
        if(tx){ d3dgl_ensure_tex(tx); G.BindTexture(GLE_TEX2D,tx->gltex); } else G.BindTexture(GLE_TEX2D,0);
        G.ProgramUniform1i(prog,loc,unit); unit++; }
}
static void d3dgl_bind_texs(D3DObj*sh){
    if(!sh) return; d3dgl_bind_texs_p(sh->glprog,sh->gl_tloc);
}
// ---- Monolithic (non-separable) program path ------------------------------------------------
// Apple's OpenGL-on-Metal fails to build a Metal pipeline from a SEPARABLE program pipeline
// (GL_ARB_separate_shader_objects) for halo3's real gameplay shaders: it logs "buildPipelineState
// failed", sets m_disable_code:1000, and drops ALL draws to SW vertex processing -> the scene never
// paints even though the sim runs. The proven-working self-test instead links VS+FS into ONE program.
// So for real draws we link monolithically, cached per (VS,PS) pair.
// VS and PS each name their constant buffers "cb<N>"; linked together they'd merge (or conflict on
// array size). d3dgl_rename_cb prefixes them per stage: VS "cb<N>"->"vXcb<N>", PS -> "pXcb<N>".
static int d3dgl_idchar(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
static char* d3dgl_rename_cb(const char*src,int isvs){
    size_t cap=strlen(src)+1024,len=0; char*out=malloc(cap); if(!out) return 0;
    char p0=isvs?'v':'p';
    #define PUT1(ch) do{ if(len+2>cap){cap*=2;char*nn=realloc(out,cap);if(!nn){free(out);return 0;}out=nn;} out[len++]=(char)(ch);}while(0)
    for(size_t i=0;src[i];){
        int previd=(i>0)&&d3dgl_idchar(src[i-1]);
        if(!previd && src[i]=='c' && src[i+1]=='b' && src[i+2]>='0' && src[i+2]<='9'){
            PUT1(p0); PUT1('X'); PUT1('c'); PUT1('b'); i+=2;   // leave the digit(s) for the normal copy
        } else { PUT1(src[i]); i++; }
    }
    PUT1(0); len--;
    #undef PUT1
    return out;
}
// Compile+cache a shader OBJECT (not a standalone program) for the mono path; kept attached-able and
// never deleted so multiple (VS,PS) pairs can reuse it. Caches in o->glsh.
// A MONOLITHIC GL program matches inter-stage varyings by NAME (not by location). The translator
// names VS outputs "oN" and PS inputs "vM" with per-stage register numbers, so they never match ->
// link fails ("Input of FS 'v0' not written", "Output of VS 'o4' not read"). Rename every inter-stage
// varying -- declaration AND body uses -- to a shared canonical "xfer<L>", where L is the semantic
// location the injector already assigned via layout(location=L). VS output oN and PS input vM for the
// same semantic then share both name and location, and the monolithic link matches them.
static char* d3dgl_rename_varyings(const char*src,int isvs){
    int map[64]; for(int i=0;i<64;i++) map[i]=-1;          // register name N (oN for VS / vN for PS) -> L
    const char*key=isvs?"out vec4 o":"in vec4 v"; int koff=isvs?10:9;
    const char*p=src;
    while(*p){ const char*nl=strchr(p,'\n'); size_t ln=nl?(size_t)(nl-p+1):strlen(p);
        const char*lo=strstr(p,"layout(location="); const char*k=strstr(p,key);
        if(lo&&k&&lo<p+ln&&k<p+ln){ long L=atol(lo+16); long N=atol(k+koff); if(N>=0&&N<64&&L>=0&&L<64) map[N]=(int)L; }
        p+=ln; if(!nl)break; }
    size_t cap=strlen(src)+1024,len=0; char*out=malloc(cap); if(!out) return 0;
    #define PUT1(c) do{ if(len+2>cap){cap*=2;char*nn=realloc(out,cap);if(!nn){free(out);return 0;}out=nn;} out[len++]=(char)(c);}while(0)
    char pre=isvs?'o':'v';
    for(size_t i=0;src[i];){
        int previd=(i>0)&&d3dgl_idchar(src[i-1]);
        if(!previd && src[i]==pre && src[i+1]>='0'&&src[i+1]<='9'){
            long N=0; size_t j=i+1; while(src[j]>='0'&&src[j]<='9'){ N=N*10+(src[j]-'0'); j++; }
            if(!d3dgl_idchar(src[j]) && N>=0 && N<64 && map[N]>=0){
                char rep[16]; int rl=snprintf(rep,sizeof rep,"xfer%d",map[N]);
                for(int r=0;r<rl;r++) PUT1(rep[r]); i=j; continue; }
        }
        PUT1(src[i]); i++;
    }
    PUT1(0); len--;
    #undef PUT1
    return out;
}
// Remove DUPLICATE inter-stage varying decls (same direction+location). Two different shader inputs
// can map to the same location -- e.g. a body-referenced input register not in the signature gets
// loc=register in d3dgl_inject and collides with another input whose D3D semantic maps to the same
// location -- yielding two "layout(location=L) in vec4 xferL;" -> GLSL "xferL may not be redeclared"
// (the dominant mono-compile failure, ~20% of draws skipped). Keep the first; the body var is shared.
static char* d3dgl_dedup_varyings(char*src){
    if(!src) return src; size_t n=strlen(src); char*out=malloc(n+1); if(!out) return src;
    uint64_t seen_in=0,seen_out=0; size_t ol=0; const char*p=src;
    while(*p){ const char*nl=strchr(p,'\n'); size_t ln=nl?(size_t)(nl-p+1):strlen(p);
        const char*q=p; while(*q==' '||*q=='\t')q++; int skip=0;
        if(!strncmp(q,"layout(location=",16)){
            const char*in_x=strstr(q," in vec4 xfer"), *out_x=strstr(q," out vec4 xfer");
            int isin=(in_x&&in_x<p+ln), isout=(out_x&&out_x<p+ln);
            if(isin||isout){ int L=atoi(q+16); if(L>=0&&L<64){ uint64_t bit=1ULL<<L;
                if(isout){ if(seen_out&bit)skip=1; else seen_out|=bit; } else { if(seen_in&bit)skip=1; else seen_in|=bit; } } }
        }
        if(!skip){ memcpy(out+ol,p,ln); ol+=ln; }
        p+=ln; if(!nl)break; }
    out[ol]=0; free(src); return out;
}
// Build the injected + cb-renamed + varying-renamed GLSL for one stage of the mono path. Caller frees.
// Does NOT touch o->glsl (other pairs reuse it; augmentation is per-pair so nothing is cached here).
static char* d3dgl_mono_src(D3DObj*o,int isvs){
    if(!o||!o->glsl) return 0;
    char*inj=d3dgl_inject(o->glsl,isvs,1); if(!inj) return 0;
    char*ren=d3dgl_rename_cb(inj,isvs); free(inj); if(!ren) return 0;
    char*var=d3dgl_rename_varyings(ren,isvs); free(ren); if(!var) return 0;
    return d3dgl_dedup_varyings(var);
}
// Bitmask of the inter-stage interface locations a stage declares: want_out=1 -> VS outputs
// ("out vec4 o"), want_out=0 -> FS inputs ("in vec4 v"), read from each line's layout(location=L).
static uint64_t d3dgl_iface_mask(const char*src,int want_out){
    // after d3dgl_rename_varyings, every inter-stage varying is named "xfer<L>" (VS out / PS in).
    uint64_t m=0; const char*p=src; const char*key=want_out?"out vec4 xfer":"in vec4 xfer";
    while(*p){ const char*nl=strchr(p,'\n'); size_t ln=nl?(size_t)(nl-p+1):strlen(p);
        const char*lo=strstr(p,"layout(location="); const char*kk=strstr(p,key);
        if(lo&&kk&&lo<p+ln&&kk<p+ln){ long L=atol(lo+16); if(L>=0&&L<64) m|=(1ULL<<L); }
        p+=ln; if(!nl)break; }
    return m;
}
// Pad a stage's inter-stage interface with dummy decls at the given locations so a monolithic VS+FS
// link validates even when one stage declares a varying the other omits (Apple GL-on-Metal FAILS,
// not just warns, on an unmatched interface). as_out=1 adds VS outputs (zero-written before main's
// closing brace); as_out=0 adds FS inputs (declared, unused). Names opad/vpad can't clash with o/v.
static char* d3dgl_augment(const char*src,uint64_t add,int as_out){
    size_t base=strlen(src);
    if(!add){ char*c=malloc(base+1); if(c) memcpy(c,src,base+1); return c; }
    char decls[64*56]; int dl=0; char wr[64*28]; int wl=0;
    for(int L=0;L<64;L++) if(add&(1ULL<<L)){
        dl+=snprintf(decls+dl,sizeof decls-dl,"layout(location=%d) %s vec4 xfer%d;\n",L,as_out?"out":"in",L);
        if(as_out) wl+=snprintf(wr+wl,sizeof wr-wl,"xfer%d=vec4(0.0);\n",L);
    }
    const char*nl=strchr(src,'\n'); size_t head=nl?(size_t)(nl-src+1):base;   // keep the #version line first
    const char*rest=src+head; size_t restlen=base-head;
    size_t cap=base+dl+wl+8; char*out=malloc(cap); if(!out) return 0; size_t len=0;
    memcpy(out+len,src,head); len+=head;
    memcpy(out+len,decls,(size_t)dl); len+=dl;
    if(as_out&&wl){ const char*lb=strrchr(rest,'}');
        if(lb){ size_t pre=(size_t)(lb-rest); memcpy(out+len,rest,pre); len+=pre; memcpy(out+len,wr,(size_t)wl); len+=wl; memcpy(out+len,lb,restlen-pre); len+=restlen-pre; }
        else { memcpy(out+len,rest,restlen); len+=restlen; } }
    else { memcpy(out+len,rest,restlen); len+=restlen; }
    out[len]=0; return out;
}
static unsigned d3dgl_compile(const char*src,unsigned glstage){
    unsigned sh=G.CreateShader(glstage); G.ShaderSource(sh,1,&src,0); G.CompileShader(sh);
    int ok=0; G.GetShaderiv(sh,GLE_CSTAT,&ok);
    if(!ok){ if(g_d3dgl_trace){ char log[1024]={0}; if(G.GetShaderInfoLog)G.GetShaderInfoLog(sh,1024,0,log); ws("  [d3dgl] mono compile FAIL: "); ws(log); ws("\n");
            if(getenv("PELOAD_D3DGL_DUMPFAIL")){ static int fn; if(fn<6){ char pth[64]; int k=snprintf(pth,sizeof pth,"/tmp/monofail_%d.glsl",fn++); FILE*f=fopen(pth,"wb"); if(f){ fwrite(src,1,strlen(src),f); fclose(f); char mm[80]; int j=snprintf(mm,sizeof mm,"  [d3dgl] dumped failing src -> %s\n",pth); (void)!write(2,mm,j);} } } }
        G.DeleteShader(sh); return 0; }
    return sh;
}
typedef struct { void*vs; void*ps; unsigned prog; int tried; int vcb[16],pcb[16],tloc[32]; } MonoProg;
static MonoProg g_mono[1024];
static MonoProg* d3dgl_monoprog(D3DObj*vs,D3DObj*ps){
    uintptr_t h=(((uintptr_t)vs>>4)*2654435761u) ^ (((uintptr_t)ps>>4)*40503u);
    for(int probe=0;probe<1024;probe++){ MonoProg*m=&g_mono[(int)((h+probe)&1023)];
        if(m->tried){ if(m->vs==vs&&m->ps==ps) return m; continue; }   // occupied by another pair -> probe on
        m->vs=vs; m->ps=ps; m->tried=1; m->prog=0;                     // claim slot; prog stays 0 on any failure
        char*vsrc=d3dgl_mono_src(vs,1), *psrc=d3dgl_mono_src(ps,0);
        if(!vsrc||!psrc){ free(vsrc); free(psrc); return m; }
        uint64_t vo=d3dgl_iface_mask(vsrc,1), fi=d3dgl_iface_mask(psrc,0);
        char*vfull=d3dgl_augment(vsrc, fi & ~vo, 1);   // VS gets the outputs the PS reads but the VS lacked
        char*pfull=d3dgl_augment(psrc, vo & ~fi, 0);   // PS gets inputs for the VS outputs it didn't read
        free(vsrc); free(psrc);
        if(!vfull||!pfull){ free(vfull); free(pfull); return m; }
        unsigned vsh=d3dgl_compile(vfull,GLE_VS), psh=d3dgl_compile(pfull,GLE_FS);
        free(vfull); free(pfull);
        if(!vsh||!psh){ if(vsh)G.DeleteShader(vsh); if(psh)G.DeleteShader(psh); return m; }
        unsigned prog=G.CreateProgram(); G.AttachShader(prog,vsh); G.AttachShader(prog,psh); G.LinkProgram(prog);
        G.DeleteShader(vsh); G.DeleteShader(psh);                      // flagged; freed with the program
        int lok=0; G.GetProgramiv(prog,GLE_LSTAT,&lok);
        if(!lok){ if(g_d3dgl_trace){ char log[1024]={0}; if(G.GetProgramInfoLog)G.GetProgramInfoLog(prog,1024,0,log);
                ws("  [d3dgl] mono link FAIL: "); ws(log); ws("\n"); } return m; }
        m->prog=prog; char nm[24];
        for(int b=0;b<16;b++){ snprintf(nm,sizeof nm,"vXcb%d",b); m->vcb[b]=G.GetUniformLocation(prog,nm);
                               snprintf(nm,sizeof nm,"pXcb%d",b); m->pcb[b]=G.GetUniformLocation(prog,nm); }
        for(int b=0;b<32;b++){ snprintf(nm,sizeof nm,"t%d",b);    m->tloc[b]=G.GetUniformLocation(prog,nm); }
        if(g_d3dgl_trace){ static int n; if(++n<=40){ char mm[64]; int k=snprintf(mm,sizeof mm,"  [d3dgl] mono-linked program #%d\n",n); (void)!write(2,mm,k);} }
        return m;
    }
    return 0;   // table full
}
// ClearRenderTargetView -> clear that RT's GL FBO color (engine's clears are no-ops in our stub, so
// without this the GL render targets accumulate across frames). Runs only on our render thread/context.
static void d3dgl_clear_rtv(void* rtvview,const float* col){
    if(!g_d3dgl_ready) return; if(g_d3dgl_has_tid && !pthread_equal(g_d3dgl_tid,pthread_self())) return;
    if(CGLGetCurrentContext()!=g_d3dgl_cgl) return;
    D3DObj* v=rtvview; D3DObj* rt=(v&&v->data)?(D3DObj*)v->data:v; if(!rt) return;
    d3dgl_ensure_rt(rt); G.BindFramebuffer(GLE_FBO,rt->glfbo);
    if(col) G.ClearColor(col[0],col[1],col[2],col[3]); else G.ClearColor(0,0,0,0);
    if(G.DepthMask)G.DepthMask(1); G.Clear(GLE_COLORBIT); }
static void d3dgl_clear_dsv(void){
    if(!g_d3dgl_ready || !g_d3dgl_depth) return; if(g_d3dgl_has_tid && !pthread_equal(g_d3dgl_tid,pthread_self())) return;
    if(CGLGetCurrentContext()!=g_d3dgl_cgl) return;
    D3DObj* rt=g_d3dst.rtv; if(!rt||!rt->glfbo) return;
    G.BindFramebuffer(GLE_FBO,rt->glfbo); if(G.DepthMask)G.DepthMask(1); G.Clear(0x100/*DEPTH_BUFFER_BIT*/); }
// DXGI vertex-attribute format -> GL (components, type, normalized) for glVertexAttribPointer.
static void d3dgl_attrib_fmt(uint32_t f,unsigned*comps,unsigned*gt,unsigned char*norm){ *norm=0;
    switch(f){
        case 2: *comps=4;*gt=0x1406;break;                       // R32G32B32A32_FLOAT
        case 6: *comps=3;*gt=0x1406;break;                       // R32G32B32_FLOAT
        case 16:*comps=2;*gt=0x1406;break;                       // R32G32_FLOAT
        case 41:*comps=1;*gt=0x1406;break;                       // R32_FLOAT
        case 10:*comps=4;*gt=0x140B;break;                       // R16G16B16A16_FLOAT
        case 34:*comps=2;*gt=0x140B;break;                       // R16G16_FLOAT
        case 54:*comps=1;*gt=0x140B;break;                       // R16_FLOAT
        case 11:*comps=4;*gt=0x1403;*norm=1;break;               // R16G16B16A16_UNORM
        case 35:*comps=2;*gt=0x1403;*norm=1;break;               // R16G16_UNORM
        case 13:*comps=4;*gt=0x1402;*norm=1;break;               // R16G16B16A16_SNORM
        case 28:case 29:*comps=4;*gt=0x1401;*norm=1;break;       // R8G8B8A8_UNORM(_SRGB)
        case 87:case 91:*comps=4;*gt=0x1401;*norm=1;break;       // BGRA8 (approx; channel order off)
        case 31:*comps=4;*gt=0x1400;*norm=1;break;               // R8G8B8A8_SNORM
        case 30:*comps=4;*gt=0x1401;break;                       // R8G8B8A8_UINT
        case 49:*comps=2;*gt=0x1401;*norm=1;break;               // R8G8_UNORM
        case 61:*comps=1;*gt=0x1401;*norm=1;break;               // R8_UNORM
        case 24:*comps=4;*gt=0x8368;*norm=1;break;               // R10G10B10A2_UNORM -> UINT_2_10_10_10_REV
        default:*comps=4;*gt=0x1406;break;
    } }
static void d3dgl_draw(uint32_t vcount,int indexed,uint32_t instcount){
    if(!d3dgl_ensure()) return;
    if(g_d3dgl_max_draws && g_d3dgl_frame_draws>=g_d3dgl_max_draws) return;
    // PELOAD_TRACE_D3DGL: tally WHY draws are skipped (so we can see what state is missing)
    static unsigned long c_tot,c_novsps,c_noglsl,c_noprog,c_exec;
    c_tot++;
    D3DObj*vs=g_d3dst.vs,*ps=g_d3dst.ps;
    if(g_d3dgl_trace && (c_tot%100000)==0){ char m[160]; int k=snprintf(m,sizeof m,"  [d3dgl] draws tot=%lu exec=%lu skip(novsps=%lu noglsl=%lu noprog=%lu)\n",c_tot,c_exec,c_novsps,c_noglsl,c_noprog); (void)!write(2,m,k); }
    if(!vs||!ps){ c_novsps++; return; }
    if((!vs->glsl&&!vs->glprog)||(!ps->glsl&&!ps->glprog)){ c_noglsl++; return; }   // need both stages translated
    g_d3dgl_phase_draw=c_tot; g_d3dgl_phase="prog";
    // Default to the monolithic (VS+FS linked) program path; PELOAD_D3DGL_SEP forces the old separable
    // program-pipeline path (which Apple GL-on-Metal fails to build -> all-SW fallback). See d3dgl_monoprog.
    int mono=!getenv("PELOAD_D3DGL_SEP");
    unsigned vsp=0,psp=0; MonoProg*mp=0;
    if(mono){ mp=d3dgl_monoprog(vs,ps); if(!mp||!mp->prog){ c_noprog++; return; } }
    else { vsp=d3dgl_prog(vs,GLE_VS); psp=d3dgl_prog(ps,GLE_FS); if(!vsp||!psp){ c_noprog++; return; } }
    c_exec++;
    // render target (default to the swapchain backbuffer)
    g_d3dgl_phase="rt";
    D3DObj*rt=g_d3dst.rtv; if(!rt){ if(!g_d3d_backbuf) g_d3d_backbuf=mktex(4096,4096,1,28,2); rt=g_d3d_backbuf; }
    d3dgl_ensure_rt(rt); G.BindFramebuffer(GLE_FBO,rt->glfbo);
    int vw=g_d3dst.vp_w>0?g_d3dst.vp_w:(rt->w?(int)rt->w:g_d3dgl_W); if(vw>4096)vw=4096;
    int vh=g_d3dst.vp_h>0?g_d3dst.vp_h:(rt->h?(int)rt->h:g_d3dgl_H); if(vh>4096)vh=4096;
    G.Viewport(0,0,vw,vh);
    // PELOAD_D3DGL_RTTEST: paint every RT a real draw binds a distinctive magenta. If the RT dump then
    // shows magenta, the FBO-bind + capture pipeline is sound and the black frame is purely geometry
    // projection (bad cb0); if it stays black, the FBO/capture path itself is broken.
    if(getenv("PELOAD_D3DGL_RTTEST") && G.ClearColor){ G.ClearColor(1.0f,0.0f,1.0f,1.0f); G.Clear(0x4000/*COLOR_BUFFER_BIT*/); }
    G.Disable(0x0B71/*DEPTH_TEST*/); G.Disable(0x0B44/*CULL_FACE*/);
    // Opaque by default: forcing SRC_ALPHA blending made opaque geometry (whose PS often writes
    // alpha=0) fully transparent -> invisible/black. Blend is opt-in per the engine's blend state.
    G.Disable(0x0BE2/*BLEND*/);
    // pipeline
    g_d3dgl_phase="pipeline";
    if(mono){ G.BindProgramPipeline(0); G.UseProgram(mp->prog); }
    else { G.UseProgram(0); G.BindProgramPipeline(g_d3dgl_pipe);
        G.UseProgramStages(g_d3dgl_pipe,GLE_VSBIT,vsp); G.UseProgramStages(g_d3dgl_pipe,GLE_FSBIT,psp); }
    g_d3dgl_phase="cbs";
    // EXPERIMENT (PELOAD_VP0..VP3=<cbreg index>): halo3 binds the per-frame global constant buffers
    // (view-proj=cb0, lighting=cb1..) through a material vtable we don't drive, so slots 0..3 reach the
    // VS unbound -> gl_Position collapses -> black. Force-bind chosen registry buffers into unbound
    // VS slots 0..3 to find the view-projection empirically.
    D3DObj* vscb_eff[16]; for(int b=0;b<16;b++) vscb_eff[b]=g_d3dst.vscb[b];
    { const char*vn[4]={"PELOAD_VP0","PELOAD_VP1","PELOAD_VP2","PELOAD_VP3"};
      for(int b=0;b<4;b++){ const char*e=getenv(vn[b]); if(!e) continue; int idx=atoi(e);
          if(idx>=0 && idx<g_ncbreg && g_cbreg[idx] && (!vscb_eff[b]||!vscb_eff[b]->data)) vscb_eff[b]=g_cbreg[idx]; } }
    // PELOAD_D3DGL_SYNVP: halo3 never binds the view-projection (cb0) to the VS through the context
    // (it's bound via a material path we don't drive) -> gl_Position collapses -> black. Synthesize it:
    // cb0 = P(FOV 70, aspect = scene w/h) * V, where V is the camera VIEW matrix found at runtime
    // (the fresh pool CB whose P*V projects the current object on-screen). Inject into unbound VS slot 0.
    int vs_has_cb0 = mono ? (mp->vcb[0]>=0) : (vs->gl_cbloc[0]>=0);
    if(getenv("PELOAD_D3DGL_SYNVP") && vs && (!vscb_eff[0]||!vscb_eff[0]->data) && vs_has_cb0 && g_last_b12 && g_last_b12->data){
        if(d3dgl_synth_cb0()) vscb_eff[0]=&g_syn_cb0; }
    if(mono){
        d3dgl_bind_cbs_p(mp->prog,mp->vcb,vscb_eff,g_d3dst.vscb_1st,g_d3dst.vscb_cnt);
        d3dgl_bind_cbs_p(mp->prog,mp->pcb,g_d3dst.pscb,g_d3dst.pscb_1st,g_d3dst.pscb_cnt);
        g_d3dgl_phase="texs"; d3dgl_bind_texs_p(mp->prog,mp->tloc);
    } else {
        d3dgl_bind_cbs(vs,vscb_eff,g_d3dst.vscb_1st,g_d3dst.vscb_cnt); d3dgl_bind_cbs(ps,g_d3dst.pscb,g_d3dst.pscb_1st,g_d3dst.pscb_cnt);
        g_d3dgl_phase="texs"; d3dgl_bind_texs(ps); d3dgl_bind_texs(vs);
    }
    G.BindVertexArray(g_d3dgl_vao);
    D3DObj*vb=g_d3dst.vb[0]; D3DObj*il=g_d3dst.il;
    int nattr=0;
    // PELOAD_D3DGL_DRAWLOG: dump the state of the first geometry draws (what the ENGINE supplies):
    // the VS-cb0 matrix, vertex format/stride, viewport, RT — to see why real geometry stays black.
    if(getenv("PELOAD_D3DGL_DRAWLOG") && il && il->nelem){
        // Log each DISTINCT VS shader once: which cb slots it references vs which are actually bound,
        // whether it has a vertex input, and whether it draws indexed. Ground truth on the black frame.
        uint64_t vf=(vs&&vs->bc)?d3dgl_fnv(vs->bc,vs->bclen):0;
        static uint64_t seen[128]; static int nseen; int fresh=1;
        for(int s=0;s<nseen;s++) if(seen[s]==vf){ fresh=0; break; }
        if(fresh && nseen<128){ seen[nseen++]=vf;
            char m[640]; int k=snprintf(m,sizeof m,"  [drawlog] VS=%016llx n=%u idx=%d il0=%s/fmt%u refs cb:",(unsigned long long)vf,vcount,indexed,il->elem[0].sem,il->elem[0].fmt);
            int nref=0,nbound=0; const int*vcbloc = mono ? mp->vcb : vs->gl_cbloc;
            for(int b=0;b<16;b++) if(vcbloc[b]>=0){ k+=snprintf(m+k,sizeof m-k,"%d,",b); nref++; D3DObj*c=g_d3dst.vscb[b]; if(c&&c->size)nbound++; }
            k+=snprintf(m+k,sizeof m-k," bound:");
            for(int b=0;b<16;b++){ D3DObj*c=g_d3dst.vscb[b]; if(c&&c->size) k+=snprintf(m+k,sizeof m-k,"%d:%lluB,",b,(unsigned long long)c->size); }
            k+=snprintf(m+k,sizeof m-k," | %d/%d refs bound%s\n",nbound,nref,(nref>nbound)?" <<MISSING":"");
            (void)!write(2,m,k); } }
    if(vb && vb->data && vb->size && il && il->nelem){
        g_d3dgl_phase="vb";
        // geometry path. Upload each referenced VB at most once, and only when the engine wrote to it
        // (gl_dirty, set in ctx_Map / ctx_UpdateSubresource) — re-uploading a 3MB VB per draw crawls.
        for(int s=0;s<8;s++){ D3DObj*sb=g_d3dst.vb[s]; if(!sb||!sb->data||!sb->size) continue;
            if(!sb->glbuf){ G.GenBuffers(1,&sb->glbuf); sb->gl_dirty=1; }
            if(sb->gl_dirty){ long cap=(long)sb->size; if(cap>(96<<20))cap=(96<<20);
                G.BindBuffer(GLE_ARRAY,sb->glbuf); G.BufferData(GLE_ARRAY,cap,sb->data,GLE_STREAM); sb->gl_dirty=0; } }
        for(int i=0;i<il->nelem && i<16;i++){
            uint32_t slot=il->elem[i].slot; if(slot>=8) slot=0; D3DObj*sb=g_d3dst.vb[slot]; if(!sb||!sb->glbuf) continue;
            unsigned comps,gt; unsigned char norm; d3dgl_attrib_fmt(il->elem[i].fmt,&comps,&gt,&norm);
            G.BindBuffer(GLE_ARRAY,sb->glbuf);
            G.VertexAttribPointer((unsigned)i,(int)comps,gt,norm,(int)g_d3dst.vstride[slot],(const void*)(size_t)(g_d3dst.voff[slot]+il->elem[i].off));
            G.EnableVertexAttribArray((unsigned)i); nattr++;
        }
        if(g_d3dgl_depth) G.Enable(0x0B71/*DEPTH_TEST*/);   // opt-in depth (needs PELOAD_D3DGL_DEPTH + depth RB)
        // PELOAD_D3DGL_VTXDUMP: ground-truth the world-space vertex range of element 0 (POSITION) so we
        // can size/aim the camera. Only meaningful for an uncompressed float position (DXGI 2/6).
        if(getenv("PELOAD_D3DGL_VTXDUMP")){ static int vd; if(vd<5){ D3DObj*sb=g_d3dst.vb[il->elem[0].slot<8?il->elem[0].slot:0];
            uint32_t stride=g_d3dst.vstride[il->elem[0].slot<8?il->elem[0].slot:0]; uint32_t off=g_d3dst.voff[0]+il->elem[0].off; uint32_t fmt=il->elem[0].fmt;
            if(sb&&sb->data&&stride>=12&&(fmt==2||fmt==6)){ vd++; const uint8_t*d=(const uint8_t*)sb->data; uint64_t nv=sb->size/stride; if(nv>400)nv=400;
                float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f}; for(uint64_t v=0;v<nv;v++){ const float*p=(const float*)(d+off+v*stride);
                    for(int c=0;c<3;c++){ float val=p[c]; if(val==val){ if(val<mn[c])mn[c]=val; if(val>mx[c])mx[c]=val; } } }
                char m[220]; int k=snprintf(m,sizeof m,"  [vtxdump] fmt=%u stride=%u nvsamp=%llu posX[%.2f..%.2f] Y[%.2f..%.2f] Z[%.2f..%.2f] v0=[%.2f %.2f %.2f]\n",
                    fmt,stride,(unsigned long long)nv,mn[0],mx[0],mn[1],mx[1],mn[2],mx[2],((const float*)(d+off))[0],((const float*)(d+off))[1],((const float*)(d+off))[2]); (void)!write(2,m,k); } } }
    }
    g_d3dgl_phase="draw";
    unsigned mode=d3dgl_topo(g_d3dst.topo);
    unsigned n=vcount? vcount : 3;   // SV_VertexID fullscreen passes often issue 3/4 verts
    if(n>8000000) n=8000000;         // guard against a garbage count driving a wild GL read
    if(instcount>65536) instcount=65536;
    if(indexed && g_d3dst.ib && g_d3dst.ib->data && g_d3dst.ib->size){
        D3DObj*ib=g_d3dst.ib; if(!ib->glbuf){ G.GenBuffers(1,&ib->glbuf); ib->gl_dirty=1; }
        if(ib->gl_dirty){ long cap=(long)ib->size; if(cap>(96<<20))cap=(96<<20); G.BindBuffer(GLE_ELEM,ib->glbuf); G.BufferData(GLE_ELEM,cap,ib->data,GLE_STREAM); ib->gl_dirty=0; }
        else G.BindBuffer(GLE_ELEM,ib->glbuf);
        unsigned itype=(g_d3dst.ibfmt==42)?GLE_UI:GLE_US; size_t isz=(itype==GLE_UI)?4:2;
        // don't let (offset + count) run past the uploaded index buffer
        uint64_t maxn=(g_d3dst.iboff<ib->size)?(ib->size-g_d3dst.iboff)/isz:0; if(n>maxn) n=(unsigned)maxn;
        const void*off=(const void*)(size_t)g_d3dst.iboff;
        if(n){ if(instcount>1) G.DrawElementsInstanced(mode,(int)n,itype,off,(int)instcount);
               else G.DrawElements(mode,(int)n,itype,off); }
    } else if(!indexed){
        if(instcount>1) G.DrawArraysInstanced(mode,0,(int)n,(int)instcount);
        else G.DrawArrays(mode,0,(int)n);
    }
    for(int i=0;i<nattr;i++) G.DisableVertexAttribArray((unsigned)i);
    g_d3dgl_phase="done"; g_d3dgl_frame_draws++; d3dgl_note_rt(rt);
    // Latch the present source. The backbuffer (final composite) is preferred; if the engine never
    // composites to it, fall back to the largest sizeable RT drawn into so we still show the scene.
    // The blit shows the [0,vw/tw]x[0,vh/th] sub-rect of the texture, so if a LATER small-viewport draw
    // (a UI element, a sub-region update) shrank present_vw/vh the frame "zoomed" into the corner. Keep
    // the LARGEST viewport seen for the current texture so a small draw can't zoom the composite.
    // PELOAD_D3DGL_PRESENT_SCENE: the engine composites the loading screen / UI into the backbuffer but
    // renders the 3D world into separate scene RTs; by default we present the backbuffer (so we show the
    // loading screen). In scene mode, ignore the backbuffer and latch the largest non-backbuffer RT that
    // received a real geometry draw this call -> show the rendered map instead of the loading overlay.
    int scene_mode = getenv("PELOAD_D3DGL_PRESENT_SCENE")!=0;
    if(vw>=512 && vh>=288){ long a=(long)vw*vh; static long best;
        int isbb=(rt==g_d3d_backbuf);
        int take = scene_mode ? (!isbb && nattr>0 && a>=best) : (isbb || a>=best);
        if(take){ best = (!scene_mode && isbb)?(1L<<60):a;
            unsigned newtex=rt->gltex; int reset=(newtex!=g_d3dgl_present_tex);
            g_d3dgl_present_tex=newtex;
            g_d3dgl_present_tw=(rt->w>4096?4096:(int)rt->w); g_d3dgl_present_th=(rt->h>4096?4096:(int)rt->h);
            if(g_d3dgl_present_tw<1)g_d3dgl_present_tw=vw; if(g_d3dgl_present_th<1)g_d3dgl_present_th=vh;
            if(reset || vw>g_d3dgl_present_vw) g_d3dgl_present_vw=vw;      // keep MAX viewport, don't shrink
            if(reset || vh>g_d3dgl_present_vh) g_d3dgl_present_vh=vh;
            // if the biggest viewport essentially fills the texture, show the whole texture (sc=1)
            if(g_d3dgl_present_vw >= g_d3dgl_present_tw*0.85) g_d3dgl_present_vw=g_d3dgl_present_tw;
            if(g_d3dgl_present_vh >= g_d3dgl_present_th*0.85) g_d3dgl_present_vh=g_d3dgl_present_th; } }
}
// Build the fullscreen-blit program (gl_VertexID triangle sampling a sub-rect of a texture,
// vertically flipped D3D->GL). The program object is shared across contexts in a share group;
// VAOs are NOT, so each presenting context passes its own `vao`.
static void d3dgl_build_blit(unsigned*vao){
    if(!g_d3dgl_blit_prog){
        const char*bvs="#version 410 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);uv=(p*0.5+0.5);gl_Position=vec4(p,0.0,1.0);}\n";
        // The d3dgl pipeline renders both the engine UI and 3D into the FBO right-side up already;
        // flipping v here inverts correctly-oriented content (confirmed: loading screen was upside
        // down). Default = no flip. PELOAD_BLIT_FLIP restores the old vertical flip if ever needed.
        const char*bfs = getenv("PELOAD_BLIT_FLIP")
            ? "#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2D s;uniform vec4 sc;void main(){vec2 t=vec2(uv.x*sc.x,(1.0-uv.y)*sc.y);c=texture(s,t);}\n"
            : "#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2D s;uniform vec4 sc;void main(){vec2 t=vec2(uv.x*sc.x,uv.y*sc.y);c=texture(s,t);}\n";
        unsigned v=G.CreateShader(GLE_VS),f=G.CreateShader(GLE_FS);
        G.ShaderSource(v,1,&bvs,0); G.CompileShader(v); G.ShaderSource(f,1,&bfs,0); G.CompileShader(f);
        g_d3dgl_blit_prog=G.CreateProgram(); G.AttachShader(g_d3dgl_blit_prog,v); G.AttachShader(g_d3dgl_blit_prog,f); G.LinkProgram(g_d3dgl_blit_prog);
        G.DeleteShader(v); G.DeleteShader(f);
        g_d3dgl_blit_sloc=G.GetUniformLocation(g_d3dgl_blit_prog,"s");
        g_d3dgl_blit_uloc=G.GetUniformLocation(g_d3dgl_blit_prog,"sc");
    }
    if(vao && !*vao) G.GenVertexArrays(1,vao);
}
// Draw the latched backbuffer texture across the current default framebuffer. Caller has made the
// presenting context current and passes its own VAO. Safe to call from render or present thread.
static void d3dgl_blit_fullscreen(unsigned vao){
    if(!g_d3dgl_present_tex) return;
    G.BindFramebuffer(GLE_FBO,0); G.Viewport(0,0,g_d3dgl_W,g_d3dgl_H);
    G.Disable(0x0B71); G.Disable(0x0BE2);
    G.ClearColor(0.02f,0.02f,0.05f,1.0f); G.Clear(GLE_COLORBIT);
    G.UseProgram(0); G.BindProgramPipeline(0); G.UseProgram(g_d3dgl_blit_prog);
    G.ActiveTexture(GLE_TEX0); G.BindTexture(GLE_TEX2D,g_d3dgl_present_tex);
    if(g_d3dgl_blit_sloc>=0) G.ProgramUniform1i(g_d3dgl_blit_prog,g_d3dgl_blit_sloc,0);
    if(g_d3dgl_blit_uloc>=0){ float tw=g_d3dgl_present_tw?g_d3dgl_present_tw:1, th=g_d3dgl_present_th?g_d3dgl_present_th:1;
        float sc[4]={ g_d3dgl_present_vw/tw, g_d3dgl_present_vh/th, 0,0 }; G.ProgramUniform4fv(g_d3dgl_blit_prog,g_d3dgl_blit_uloc,1,sc); }
    G.BindVertexArray(vao); G.DrawArrays(4,0,3);
}
// Called on the PRESENT thread (owns the window context g_present_glctx, which shares objects with
// our offscreen render context). Displays the backbuffer the render thread produced.
static unsigned g_d3dgl_pt_vao;   // present-thread-local VAO (VAOs are not shared across contexts)
static void d3dgl_blit_present(void){
    if(!g_d3dgl_ready || !g_d3dgl_offscreen || !g_d3dgl_present_tex) return;
    if(!G.DrawArrays) return;
    d3dgl_build_blit(&g_d3dgl_pt_vao);
    d3dgl_blit_fullscreen(g_d3dgl_pt_vao);
    if(G.Flush) G.Flush();
}
// Blit the latched backbuffer texture to the visible window (default framebuffer) and flush.
// Window mode (main thread): blit inline here. Offscreen mode (render thread): only flush so the
// shared present-thread context sees the backbuffer; that thread calls d3dgl_blit_present().
// Scan the constant-buffer registry for the global VIEW-PROJECTION buffer (halo3 binds it via a
// material vtable we don't drive, so it reaches no VS slot -> gl_Position collapses -> black). A
// perspective view-proj stored row-major (clip.i = dot(worldpos, row i)) has row3 (the w/clip row)
// acting as a direction+offset: its xyz are NOT ~(0,0,0) (that would be orthographic / identity).
static int cb_looks_viewproj(const float*m,uint64_t bytes){
    if(bytes<64) return 0;
    int nz=0; for(int i=0;i<16;i++){ float v=m[i]; if(v!=v) return 0; if(v<-1e9f||v>1e9f) return 0; if(v!=0.0f) nz++; }
    if(nz<6) return 0;
    float r3x=m[12],r3y=m[13],r3z=m[14];
    float r3=r3x*r3x+r3y*r3y+r3z*r3z; if(r3<1e-4f) return 0;
    float r0=m[0]*m[0]+m[1]*m[1]+m[2]*m[2], r1=m[4]*m[4]+m[5]*m[5]+m[6]*m[6];
    if(r0<1e-6f||r1<1e-6f) return 0;
    return 1; }
// A PERSPECTIVE PROJECTION matrix (clip.i = dot(view, cb0[i]) convention): rows 0/1 are axis scale
// (row0 only .x, row1 only .y), rows 2/3 live in z/w (.xy ~ 0). Near-static (set on FOV/resolution),
// so it's usually NOT "fresh" each frame — detect it structurally anywhere in the registry.
static int cb_looks_projection(const float*m,uint64_t bytes){
    if(bytes<64) return 0;
    for(int i=0;i<16;i++){ float v=m[i]; if(v!=v||v<-1e9f||v>1e9f) return 0; }
    float ax=fabsf(m[0]),ay=fabsf(m[5]);                 // row0.x (X scale), row1.y (Y scale)
    if(ax<0.3f||ax>8.0f||ay<0.3f||ay>8.0f) return 0;     // plausible proj scales
    float off0=fabsf(m[1])+fabsf(m[2])+fabsf(m[3]);      // row0 .yzw should be ~0
    float off1=fabsf(m[4])+fabsf(m[6])+fabsf(m[7]);      // row1 .xzw ~0
    float r2xy=fabsf(m[8])+fabsf(m[9]);                  // row2 .xy ~0
    float r3xy=fabsf(m[12])+fabsf(m[13]);                // row3 .xy ~0
    if(off0>0.05f||off1>0.05f||r2xy>0.05f||r3xy>0.05f) return 0;
    float r2zw=fabsf(m[10])+fabsf(m[11]), r3zw=fabsf(m[14])+fabsf(m[15]); // depth + w rows nonzero
    if(r2zw<1e-3f||r3zw<1e-3f) return 0;
    return 1; }
static void cb_dump_scan(void){ if(!getenv("PELOAD_CB_DUMP")) return; static int done; if(done) return; done=1;
    char m[256]; int k=snprintf(m,sizeof m,"  [cbdump] registry=%d tick=%llu\n",g_ncbreg,(unsigned long long)g_cb_tick); (void)!write(2,m,k);
    int vp=0, shown=0;
    // Only FRESH (written this frame), frame-global-sized (64..512B) buffers = the per-frame globals
    // (view-proj / lighting / fog). Print the full first 4 rows so the projection matrix is recognisable.
    for(int i=0;i<g_ncbreg && shown<60;i++){ D3DObj*c=g_cbreg[i]; if(!c||!c->data||c->size<64||c->size>512) continue;
        if((g_cb_tick - c->cb_upd) > 1) continue;      // fresh this frame only
        const float*f=(const float*)c->data; if(cb_looks_viewproj(f,c->size)) vp++;
        shown++;
        k=snprintf(m,sizeof m,"   cb[%d] %lluB upd=%llu\n      [%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f]\n",
            i,(unsigned long long)c->size,(unsigned long long)c->cb_upd,
            f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]); (void)!write(2,m,k); }
    k=snprintf(m,sizeof m,"  [cbdump] fresh-small-CBs shown=%d viewproj-flagged=%d\n",shown,vp); (void)!write(2,m,k);
    // Scan the WHOLE registry (any freshness) for a PROJECTION matrix in the first 4 rows.
    int proj=0;
    for(int i=0;i<g_ncbreg && proj<30;i++){ D3DObj*c=g_cbreg[i]; if(!c||!c->data||c->size<64) continue;
        const float*f=(const float*)c->data; if(!cb_looks_projection(f,c->size)) continue; proj++;
        k=snprintf(m,sizeof m,"   PROJ cb[%d] %lluB upd=%llu\n      [%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f]\n",
            i,(unsigned long long)c->size,(unsigned long long)c->cb_upd,
            f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]); (void)!write(2,m,k); }
    k=snprintf(m,sizeof m,"  [cbdump] projection-matches=%d\n",proj); (void)!write(2,m,k);
    // Where does the b12 world matrix actually live? Is it a registered CB?
    D3DObj*b=g_last_b12; if(b){ int inreg=0; for(int i=0;i<g_ncbreg;i++) if(g_cbreg[i]==b){ inreg=1; break; }
        const float*f=b->data?(const float*)b->data:0;
        k=snprintf(m,sizeof m,"  [cbdump] g_last_b12=%p sz=%lluB is_cb=%d inReg=%d upd=%llu data=%s",b,(unsigned long long)b->size,b->is_cb,inreg,(unsigned long long)b->cb_upd,f?"":"NULL\n");
        (void)!write(2,m,k);
        if(f){ k=snprintf(m,sizeof m,"[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]); (void)!write(2,m,k); } }
    // cb0 FINDER: the shader computes r1=objpos*cb12 (world) then clip=r1*cb0. The world origin of a
    // visible object = cb12's 4th column (f[3],f[7],f[11],f[15]). The TRUE view-projection cb0 maps it
    // to valid clip (w>0, |x/w|<~1, |y/w|<~1). Scan every CB as a candidate cb0 and report hits.
    if(b&&b->data&&b->size>=64){ const float*w=(const float*)b->data;
        float wp[4]={w[3],w[7],w[11], 1.0f};   // object world origin = bone-0 translation column, homogeneous w=1
        k=snprintf(m,sizeof m,"  [cb0find] object world origin = [%.2f %.2f %.2f %.2f]; candidates that put it on-screen:\n",wp[0],wp[1],wp[2],wp[3]); (void)!write(2,m,k);
        int hits=0;
        for(int i=0;i<g_ncbreg && hits<24;i++){ D3DObj*c=g_cbreg[i]; if(!c||!c->data||c->size<64||c==b) continue;
            const float*M=(const float*)c->data; int bad=0; for(int j=0;j<16;j++){ float v=M[j]; if(v!=v||v<-1e9f||v>1e9f){bad=1;break;} }
            if(bad) continue;
            float cl[4]; for(int r=0;r<4;r++) cl[r]=M[r*4]*wp[0]+M[r*4+1]*wp[1]+M[r*4+2]*wp[2]+M[r*4+3]*wp[3];
            if(cl[3]<=0.05f) continue; float nx=cl[0]/cl[3], ny=cl[1]/cl[3], nz=cl[2]/cl[3];
            if(nx>-1.3f&&nx<1.3f && ny>-1.3f&&ny<1.3f && nz>-0.1f&&nz<1.3f){ hits++;
                k=snprintf(m,sizeof m,"   cb[%d] %lluB upd=%llu NDC=(%.2f %.2f %.2f) w=%.2f r0=[%.3f %.3f %.3f %.3f] r3=[%.3f %.3f %.3f %.3f]\n",
                    i,(unsigned long long)c->size,(unsigned long long)c->cb_upd,nx,ny,nz,cl[3],M[0],M[1],M[2],M[3],M[12],M[13],M[14],M[15]); (void)!write(2,m,k); } }
        k=snprintf(m,sizeof m,"  [cb0find] on-screen candidates=%d\n",hits); (void)!write(2,m,k);
        // P x V finder, ROBUST: score each (candidate V, convention) by how many of the collected
        // per-object world origins (g_b12_org, %d of them) project on-screen. The TRUE camera+convention
        // projects the MOST. Conventions: V as-is vs transposed, P left-handed vs right-handed.
        float fovy=70.0f*3.14159265f/180.0f, s=1.0f/tanf(fovy*0.5f), n=0.0625f, fr=1000.0f;
        float PL[16]={ s,0,0,0, 0,s,0,0, 0,0,fr/(fr-n),-n*fr/(fr-n), 0,0,1,0 };
        float PR[16]={ s,0,0,0, 0,s,0,0, 0,0,-fr/(fr-n),-n*fr/(fr-n), 0,0,-1,0 };
        float*Ps[2]={PL,PR}; const char*pn[2]={"LH","RH"};
        k=snprintf(m,sizeof m,"  [cb0find] testing %d object origins across V/Vt x LH/RH:\n",g_nb12_org); (void)!write(2,m,k);
        int bestN=0; char bestdesc[96]="(none)";
        for(int pi=0;pi<2;pi++){ float*P=Ps[pi];
          for(int tr=0;tr<2;tr++){            // tr=0: V as-is; tr=1: V transposed
            for(int i=0;i<g_ncbreg;i++){ D3DObj*c=g_cbreg[i]; if(!c||!c->data||c->size<64||c->size>512) continue; if((g_cb_tick-c->cb_upd)>1) continue;
              const float*V0=(const float*)c->data; float V[16]; int bad=0;
              for(int r=0;r<4;r++)for(int cc=0;cc<4;cc++){ float v=tr? V0[cc*4+r]:V0[r*4+cc]; if(v!=v||v<-1e9f||v>1e9f)bad=1; V[r*4+cc]=v; } if(bad)continue;
              float VP[16]; for(int r=0;r<4;r++)for(int cc=0;cc<4;cc++){ float a=0; for(int kk=0;kk<4;kk++)a+=P[r*4+kk]*V[kk*4+cc]; VP[r*4+cc]=a; }
              int on=0; for(int o=0;o<g_nb12_org;o++){ float wpx=g_b12_org[o][0],wpy=g_b12_org[o][1],wpz=g_b12_org[o][2];
                  float cl[4]; for(int r=0;r<4;r++) cl[r]=VP[r*4]*wpx+VP[r*4+1]*wpy+VP[r*4+2]*wpz+VP[r*4+3];
                  if(cl[3]>0.05f){ float nx=cl[0]/cl[3],ny=cl[1]/cl[3],nz=cl[2]/cl[3]; if(nx>-1.1f&&nx<1.1f&&ny>-1.1f&&ny<1.1f&&nz>-0.05f&&nz<1.05f) on++; } }
              if(on>bestN){ bestN=on; snprintf(bestdesc,sizeof bestdesc,"cb[%d] %lluB P=%s V%s on=%d/%d",i,(unsigned long long)c->size,pn[pi],tr?"^T":"",on,g_nb12_org);
                  k=snprintf(m,sizeof m,"   BEST so far: %s\n",bestdesc); (void)!write(2,m,k); } } } }
        k=snprintf(m,sizeof m,"  [cb0find] BEST convention: %s\n",bestdesc); (void)!write(2,m,k); } }
static void d3dgl_present(void){
    g_cb_tick++;                                   // advance CB freshness clock each present
    if(getenv("PELOAD_CB_DUMP") && g_last_b12){ static int pc; if(pc++==3) cb_dump_scan(); }  // dump a few presents after gameplay CBs go live (before any late crash)
    if(!g_d3dgl_ready) return;                     // nothing built yet (no draw reached us)
    if(g_d3dgl_has_tid && !pthread_equal(g_d3dgl_tid,pthread_self())) return;
    if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
    if(!g_d3dgl_offscreen){                         // we own the drawable: blit to our window
        d3dgl_build_blit(&g_d3dgl_blit_vao);
        d3dgl_blit_fullscreen(g_d3dgl_blit_vao);
        if(g_d3dgl_nsctx) om0(g_d3dgl_nsctx,"flushBuffer");
        cocoa_pump();
    }
    if(G.Flush) G.Flush();
    // PELOAD_D3DGL_SHOT: dump the composited backbuffer FBO (the real game frame produced by the
    // translated shaders) to ~/reclaimer_d3dgl_<n>.bmp every PELOAD_D3DGL_SHOT presents (default 60).
    if(getenv("PELOAD_D3DGL_SHOT") && g_d3dgl_present_tex && g_d3d_backbuf && g_d3d_backbuf->glfbo && G.ReadPixels){
        static unsigned long sc; int every=atoi(getenv("PELOAD_D3DGL_SHOT")); if(every<1)every=60;
        if((sc++ % every)==0){ int w=g_d3dgl_present_vw,h=g_d3dgl_present_vh; if(w>0&&h>0&&(long)w*h<=(64<<20)){
            unsigned char*cap=calloc(1,(size_t)w*h*4);
            if(cap){ G.BindFramebuffer(GLE_FBO,g_d3d_backbuf->glfbo); if(G.ReadBuffer)G.ReadBuffer(GLE_CA0); if(G.GetError)G.GetError();
                G.ReadPixels(0,0,w,h,GLE_RGBA,GLE_UBYTE,cap);
                static int sn; const char*home=getenv("HOME"); char p[512]; snprintf(p,sizeof p,"%s/reclaimer_d3dgl_%03d.bmp",home?home:"/tmp",sn++);
                bmp_write_rgba(p,w,h,cap); char m[200]; int k=snprintf(m,sizeof m,"  [d3dgl] shot -> %s (%dx%d)\n",p,w,h); (void)!write(2,m,k); free(cap);
                G.BindFramebuffer(GLE_FBO,0); } } }
    }
    if(g_d3dgl_trace){ static unsigned long pc; if((pc++ %120)==0){ char m[96]; int k=snprintf(m,sizeof m,"  [d3dgl] present #%lu draws/frame=%ld tex=%u %dx%d\n",pc,g_d3dgl_frame_draws,g_d3dgl_present_tex,g_d3dgl_present_vw,g_d3dgl_present_vh); (void)!write(2,m,k);} }
    // PELOAD_D3DGL_DUMP_RT: once ~90 presents in (so RTs have content), dump every distinct RT's
    // GL texture so we can see where geometry lands.
    if(getenv("PELOAD_D3DGL_DUMP_RT") && G.ReadPixels && g_rtdumped<g_nrtseen){ static unsigned long pc2;
        if(pc2++>=90){ for(; g_rtdumped<g_nrtseen; g_rtdumped++){ D3DObj*rt=g_rtseen[g_rtdumped]; if(!rt||!rt->glfbo) continue;
            int w=rt->w>2048?2048:(int)rt->w, h=rt->h>2048?2048:(int)rt->h; if(w<1||h<1)continue; if((long)w*h>(16<<20))continue;
            unsigned char*cap=calloc(1,(size_t)w*h*4); if(!cap)continue;
            G.BindFramebuffer(GLE_FBO,rt->glfbo); if(G.ReadBuffer)G.ReadBuffer(GLE_CA0); if(G.GetError)G.GetError();
            G.ReadPixels(0,0,w,h,GLE_RGBA,GLE_UBYTE,cap);
            unsigned long sum=0; for(long k=0;k<(long)w*h*4;k+=4) sum+=cap[k]+cap[k+1]+cap[k+2];
            const char*home=getenv("HOME"); char p[512]; snprintf(p,sizeof p,"%s/reclaimer_rt_%03d_%dx%d.bmp",home?home:"/tmp",g_rtdumped,(int)rt->w,(int)rt->h);
            bmp_write_rgba(p,w,h,cap); char m[220]; int k=snprintf(m,sizeof m,"  [d3dgl] RT dump %d: %dx%d rgbsum=%lu fmt=%u -> %s\n",g_rtdumped,(int)rt->w,(int)rt->h,sum,rt->fmt,p); (void)!write(2,m,k); free(cap); }
            G.BindFramebuffer(GLE_FBO,0); } }
    g_d3dgl_frame_draws=0;
}
// PELOAD_D3DGL_SELFTEST: drive the REAL executor (d3dgl_prog separable-link -> FBO -> texture
// upload+bind -> CB upload -> draw -> present -> capture) with translator-convention shaders and
// a synthetic texture, WITHOUT the (flaky) engine reaching gameplay. Proves the live draw path
// renders real translated-GLSL output onto the window + a captured BMP. The shaders use the exact
// conventions dxbc2glsl emits: VS writes out o<reg>, PS reads in v<reg> (connected by injected
// locations), PS samples t0 and references cb0 (exercising the uniform path).
static char* d3dgl_strdup(const char*s){ size_t n=strlen(s)+1; char*p=malloc(n); if(p) memcpy(p,s,n); return p; }
static void d3dgl_selftest(void){
    const char* vs_src=
        "#version 410 core\n// stage=vs\n"
        "out vec4 o1;\n"
        "void main(){ vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);"
        " o1=vec4(p*0.5+0.5,0.0,0.0); gl_Position=vec4(p,0.0,1.0); }\n";
    const char* ps_src=
        "#version 410 core\n// stage=ps\n"
        "in vec4 v1;\nout vec4 o0;\nuniform sampler2D t0;\nuniform vec4 cb0[64];\n"
        "void main(){ o0 = texture(t0, v1.xy) + cb0[0]*0.0; }\n";
    static D3DObj vso,pso,tx,cb; memset(&vso,0,sizeof vso); memset(&pso,0,sizeof pso); memset(&tx,0,sizeof tx); memset(&cb,0,sizeof cb);
    vso.glsl=d3dgl_strdup(vs_src); pso.glsl=d3dgl_strdup(ps_src);
    // PELOAD_D3DGL_TEX=<bmp>: render a REAL dumped Halo 3 texture through the executor instead of the
    // synthetic pattern (proves real game assets flow texture-upload -> translated PS -> FBO -> screen).
    const char* texpath=getenv("PELOAD_D3DGL_TEX");
    if(texpath){ int iw=0,ih=0; unsigned char*img=bmp_load_rgba(texpath,&iw,&ih);
        if(img){ tx.w=iw; tx.h=ih; tx.fmt=28; tx.size=(uint64_t)iw*ih*4; tx.data=img;
            char m[200]; int k=snprintf(m,sizeof m,"  [d3dgl-selftest] loaded real texture %dx%d from %s\n",iw,ih,texpath); (void)!write(2,m,k); }
        else ws("  [d3dgl-selftest] texture load failed; using synthetic\n"); }
    if(!tx.data){
        // synthetic 256x256 RGBA8 texture: colored gradient + white grid (unambiguous when displayed)
        int TW=256,TH=256; tx.w=TW; tx.h=TH; tx.fmt=28; tx.size=(uint64_t)TW*TH*4; tx.data=calloc(1,tx.size);
        unsigned char*px=tx.data;
        for(int y=0;y<TH;y++) for(int x=0;x<TW;x++){ unsigned char*p=px+((size_t)y*TW+x)*4;
            p[0]=(unsigned char)x; p[1]=(unsigned char)y; p[2]=(unsigned char)(255-x); p[3]=255;
            if((x&31)==0||(y&31)==0){ p[0]=p[1]=p[2]=255; } }
    }
    cb.size=64*16; cb.data=calloc(1,cb.size);
    memset(&g_d3dst,0,sizeof g_d3dst);
    g_d3dst.vs=&vso; g_d3dst.ps=&pso; g_d3dst.psrv[0]=&tx; g_d3dst.vscb[0]=&cb; g_d3dst.pscb[0]=&cb;
    g_d3dst.vp_w=g_d3dgl_W; g_d3dst.vp_h=g_d3dgl_H; g_d3dst.topo=4/*TRIANGLELIST*/; g_d3dst.rtv=0;
    // PELOAD_D3DGL_GEOTEST: exercise the GEOMETRY path (VB + input layout + attributes + MVP cb)
    // with a known NDC triangle, in isolation — no engine. If this triangle renders, the geometry
    // path works and any black gameplay is the engine's matrices/attributes; if not, the path is bugged.
    static D3DObj gvs,gps,gvb,gil,gcb;
    if(getenv("PELOAD_D3DGL_GEOTEST")){
        memset(&gvs,0,sizeof gvs); memset(&gps,0,sizeof gps); memset(&gvb,0,sizeof gvb); memset(&gil,0,sizeof gil); memset(&gcb,0,sizeof gcb);
        gvs.glsl=d3dgl_strdup("#version 410 core\n// stage=vs\nin vec4 v0;\nuniform vec4 cb0[4];\n"
            "void main(){ gl_Position=vec4(dot(v0,cb0[0]),dot(v0,cb0[1]),dot(v0,cb0[2]),dot(v0,cb0[3])); }\n");
        gps.glsl=d3dgl_strdup("#version 410 core\n// stage=ps\nout vec4 o0;\nvoid main(){ o0=vec4(0.20,0.90,0.35,1.0); }\n");
        static float tri[12]={ -0.8f,-0.8f,0.5f,1.0f,  0.8f,-0.8f,0.5f,1.0f,  0.0f,0.8f,0.5f,1.0f };
        gvb.data=tri; gvb.size=sizeof tri; gvb.w=3; gvb.h=1;
        gil.nelem=1; gil.elem[0].fmt=2; gil.elem[0].slot=0; gil.elem[0].off=0; gil.elem[0].sem[0]='P';
        gcb.size=4*16; gcb.data=calloc(1,gcb.size); float*m=(float*)gcb.data; m[0]=1;m[5]=1;m[10]=1;m[15]=1; // identity
        memset(&g_d3dst,0,sizeof g_d3dst);
        g_d3dst.vs=&gvs; g_d3dst.ps=&gps; g_d3dst.vb[0]=&gvb; g_d3dst.vstride[0]=16; g_d3dst.il=&gil;
        g_d3dst.vscb[0]=&gcb; g_d3dst.vp_w=g_d3dgl_W; g_d3dst.vp_h=g_d3dgl_H; g_d3dst.topo=4; g_d3dst.rtv=0;
    }
    g_d3dgl_en=1;
    if(!d3dgl_ensure()){ ws("  [d3dgl-selftest] executor init FAILED\n"); return; }
    int frames=getenv("PELOAD_D3DGL_FRAMES")?atoi(getenv("PELOAD_D3DGL_FRAMES")):90;
    for(int i=0;i<frames;i++){ d3dgl_draw(3,0,1); d3dgl_present(); struct timespec ts={0,16*1000000L}; nanosleep(&ts,0); }
    // final explicit capture of the composited backbuffer FBO -> BMP (independent of PELOAD_D3DGL_SHOT)
    if(g_d3d_backbuf && g_d3d_backbuf->glfbo && G.ReadPixels){
        int w=g_d3dgl_present_vw>0?g_d3dgl_present_vw:g_d3dgl_W, h=g_d3dgl_present_vh>0?g_d3dgl_present_vh:g_d3dgl_H;
        unsigned char*cap=calloc(1,(size_t)w*h*4);
        if(cap){ G.BindFramebuffer(GLE_FBO,g_d3d_backbuf->glfbo); if(G.ReadBuffer)G.ReadBuffer(GLE_CA0); if(G.GetError)G.GetError();
            G.ReadPixels(0,0,w,h,GLE_RGBA,GLE_UBYTE,cap);
            unsigned long sum=0; for(long k=0;k<(long)w*h*4;k++) sum+=cap[k];
            const char*home=getenv("HOME"); char p[512]; snprintf(p,sizeof p,"%s/reclaimer_d3dgl_selftest.bmp",home?home:"/tmp");
            bmp_write_rgba(p,w,h,cap);
            char m[220]; int kk=snprintf(m,sizeof m,"  [d3dgl-selftest] %dx%d rendered via executor; pixelsum=%lu -> %s\n",w,h,sum,p); (void)!write(2,m,kk); free(cap);
            G.BindFramebuffer(GLE_FBO,0); }
    }
    ws("  [d3dgl-selftest] done\n");
}
// ---- Direct GL render self-test (PELOAD_GLTEST) ------------------------------
// Prove the GL(-on-Metal) render path works on this Mac WITHOUT launching the game or going
// through halo3's (gated) render dispatch: open the window, compile a 4.1-core shader, draw a
// colored triangle on an animated background, present, and capture a BMP. This is the standalone
// graphics-driver harness; the same GL plumbing (shaders/VAO/VBO/draw/present/capture) is what a
// D3D11->GL translation layer reuses once halo3 submits geometry.
static void bmp_write_rgba(const char*path,int W,int H,const unsigned char*px){
    FILE*f=fopen(path,"wb"); if(!f) return; uint32_t isz=(uint32_t)W*H*4,fsz=54+isz,o=54;
    uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&o,4);
    uint8_t ih[40]={0}; uint32_t v40=40; memcpy(ih,&v40,4); memcpy(ih+4,&W,4); memcpy(ih+8,&H,4);
    uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
    fwrite(fh,1,14,f); fwrite(ih,1,40,f);
    for(int i=0;i<W*H;i++){ const unsigned char*p=px+(size_t)i*4; uint8_t b[4]={p[2],p[1],p[0],255}; fwrite(b,1,4,f);} fclose(f); }
// Load a 32bpp bottom-up BMP (as written by bmp_write_rgba / the tex dumper) into a malloc'd
// RGBA top-down buffer. Returns pixels (caller frees) + dims. NULL on failure.
static unsigned char* bmp_load_rgba(const char*path,int*pw,int*ph){
    FILE*f=fopen(path,"rb"); if(!f) return 0; uint8_t hdr[54]; if(fread(hdr,1,54,f)!=54){fclose(f);return 0;}
    int W=*(int*)(hdr+18),H=*(int*)(hdr+22); uint16_t bpp=*(uint16_t*)(hdr+28);
    if(W<=0||H<=0||bpp!=32||(int64_t)W*H>(64<<20)){fclose(f);return 0;}
    unsigned char*raw=malloc((size_t)W*H*4); if(!raw){fclose(f);return 0;} if(fread(raw,1,(size_t)W*H*4,f)!=(size_t)W*H*4){free(raw);fclose(f);return 0;} fclose(f);
    unsigned char*out=malloc((size_t)W*H*4); if(!out){free(raw);return 0;}
    for(int y=0;y<H;y++){ const unsigned char*s=raw+(size_t)(H-1-y)*W*4; unsigned char*d=out+(size_t)y*W*4;
        for(int x=0;x<W;x++){ d[x*4]=s[x*4+2]; d[x*4+1]=s[x*4+1]; d[x*4+2]=s[x*4]; d[x*4+3]=255; } }
    free(raw); *pw=W; *ph=H; return out; }
static void gl_selftest(void){
    int W=1280,H=720;
    if(getenv("PELOAD_D3DGL_SELFTEST")){ d3dgl_selftest(); return; }   // exercise the live draw-path executor
    cocoa_make_gl_window(W,H);               // creates + makes current on THIS (main) thread
    #define GLSYM(r,n,a) r(*n)a=dlsym(RTLD_DEFAULT,#n)
    GLSYM(unsigned,glCreateShader,(unsigned)); GLSYM(void,glShaderSource,(unsigned,int,const char*const*,const int*));
    GLSYM(void,glCompileShader,(unsigned)); GLSYM(void,glGetShaderiv,(unsigned,unsigned,int*));
    GLSYM(void,glGetShaderInfoLog,(unsigned,int,int*,char*)); GLSYM(unsigned,glCreateProgram,(void));
    GLSYM(void,glAttachShader,(unsigned,unsigned)); GLSYM(void,glLinkProgram,(unsigned));
    GLSYM(void,glGetProgramiv,(unsigned,unsigned,int*)); GLSYM(void,glUseProgram,(unsigned));
    GLSYM(void,glGenVertexArrays,(int,unsigned*)); GLSYM(void,glBindVertexArray,(unsigned));
    GLSYM(void,glGenBuffers,(int,unsigned*)); GLSYM(void,glBindBuffer,(unsigned,unsigned));
    GLSYM(void,glBufferData,(unsigned,long,const void*,unsigned)); GLSYM(void,glVertexAttribPointer,(unsigned,int,unsigned,unsigned char,int,const void*));
    GLSYM(void,glEnableVertexAttribArray,(unsigned)); GLSYM(void,glClearColor,(float,float,float,float));
    GLSYM(void,glClear,(unsigned)); GLSYM(void,glViewport,(int,int,int,int)); GLSYM(void,glDrawArrays,(unsigned,int,int));
    GLSYM(unsigned,glGetError,(void)); GLSYM(void,glReadBuffer,(unsigned)); GLSYM(void,glReadPixels,(int,int,int,int,unsigned,unsigned,void*));
    GLSYM(const unsigned char*,glGetString,(unsigned));
    #undef GLSYM
    if(!glCreateShader||!glDrawArrays){ ws("  [gltest] GL core symbols missing\n"); return; }
    const unsigned char*ver=glGetString?glGetString(0x1F02/*GL_VERSION*/):0;
    if(ver){ ws("  [gltest] GL_VERSION="); ws((const char*)ver); ws("\n"); }
    // PELOAD_D3DGL_CHECK=<dir>: run the REAL live-path builder (d3dgl_inject + separable-program
    // d3dgl_prog) over every *.glsl in <dir> (default ~/reclaimer_glsl) and tally compile/link
    // pass/fail. Validates the exact runtime translation path at scale without launching the game.
    const char* dchk=getenv("PELOAD_D3DGL_CHECK");
    if(dchk){
        if(!d3dgl_loadgl()){ ws("  [d3dglcheck] GL load failed\n"); return; }
        char dir[512]; if(dchk[0]=='1'||!dchk[0]){ const char*h=getenv("HOME"); snprintf(dir,sizeof dir,"%s/reclaimer_glsl",h?h:"/tmp"); } else snprintf(dir,sizeof dir,"%s",dchk);
        DIR*dp=opendir(dir); if(!dp){ ws("  [d3dglcheck] opendir fail: "); ws(dir); ws("\n"); return; }
        g_d3dgl_trace=getenv("PELOAD_TRACE_D3DGL")?1:0;
        int nvs=0,nps=0,okvs=0,okps=0,other=0; struct dirent*de;
        while((de=readdir(dp))){ const char*nm=de->d_name; size_t l=strlen(nm); if(l<6||strcmp(nm+l-5,".glsl")) continue;
            char path[1200]; snprintf(path,sizeof path,"%s/%s",dir,nm); FILE*f=fopen(path,"rb"); if(!f) continue;
            fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0||sz>(4<<20)){ fclose(f); continue; }
            char*s=malloc(sz+1); if(!s){ fclose(f); continue; } if(fread(s,1,sz,f)!=(size_t)sz){ free(s); fclose(f); continue; } s[sz]=0; fclose(f);
            int isvs = memmem(s,sz<128?sz:128,"stage=vs",8)!=0;
            D3DObj t; memset(&t,0,sizeof t); t.glsl=s;            // d3dgl_prog frees t.glsl on failure
            unsigned pr=d3dgl_prog(&t, isvs?GLE_VS:GLE_FS);
            if(isvs){ nvs++; if(pr)okvs++; } else { nps++; if(pr)okps++; }
            if(!pr){ char mm[300]; int kk=snprintf(mm,sizeof mm,"  [d3dglcheck] FAIL file=%s (%s)\n",nm,isvs?"vs":"ps"); (void)!write(2,mm,kk); }
            if(t.glsl) free(t.glsl);                              // success path leaves it set
        }
        closedir(dp);
        char m[160]; int k=snprintf(m,sizeof m,"  [d3dglcheck] VS %d/%d  PS %d/%d  (other=%d) built as separable programs\n",okvs,nvs,okps,nps,other); (void)!write(2,m,k);
        return;
    }
    // PELOAD_GLSL_CHECK=<file>: compile that file as a fragment shader + report (verifies the
    // DXBC->GLSL translator output actually compiles on this GL driver).
    const char* chk=getenv("PELOAD_GLSL_CHECK");
    if(chk){ FILE*cf=fopen(chk,"rb"); if(!cf){ ws("  [glslcheck] open fail\n"); return; }
        fseek(cf,0,SEEK_END); long sz=ftell(cf); fseek(cf,0,SEEK_SET); char*srcb=malloc(sz+1);
        if(!srcb||fread(srcb,1,sz,cf)!=(size_t)sz){ ws("  [glslcheck] read fail\n"); fclose(cf); return; } srcb[sz]=0; fclose(cf);
        unsigned stype = memmem(srcb,sz<128?sz:128,"stage=vs",8)? 0x8B31/*VERTEX*/ : 0x8B30/*FRAGMENT*/;
        unsigned fsh=glCreateShader(stype); const char*cp=srcb; glShaderSource(fsh,1,&cp,0); glCompileShader(fsh);
        int okc=0; glGetShaderiv(fsh,0x8B81,&okc); char log[2048]={0}; if(glGetShaderInfoLog)glGetShaderInfoLog(fsh,2048,0,log);
        char m[160]; int k=snprintf(m,sizeof m,"  [glslcheck] %s: %s\n",chk,okc?"COMPILED OK":"FAILED"); (void)!write(2,m,k);
        if(!okc||log[0]){ ws("  [glslcheck] log: "); ws(log); ws("\n"); }
        free(srcb); return; }
    // PELOAD_GLTEST_IMG=<bmp>: display a captured game texture as a fullscreen textured quad,
    // rendered through this same GL-on-Metal path (real Guardian imagery via the direct driver).
    const char* imgpath=getenv("PELOAD_GLTEST_IMG");
    if(imgpath){
        #define GS(r,n,a) r(*n)a=dlsym(RTLD_DEFAULT,#n)
        GS(unsigned,glCreateShader,(unsigned)); GS(void,glShaderSource,(unsigned,int,const char*const*,const int*)); GS(void,glCompileShader,(unsigned));
        GS(unsigned,glCreateProgram,(void)); GS(void,glAttachShader,(unsigned,unsigned)); GS(void,glLinkProgram,(unsigned)); GS(void,glUseProgram,(unsigned));
        GS(void,glGenVertexArrays,(int,unsigned*)); GS(void,glBindVertexArray,(unsigned)); GS(void,glGenBuffers,(int,unsigned*)); GS(void,glBindBuffer,(unsigned,unsigned));
        GS(void,glBufferData,(unsigned,long,const void*,unsigned)); GS(void,glVertexAttribPointer,(unsigned,int,unsigned,unsigned char,int,const void*)); GS(void,glEnableVertexAttribArray,(unsigned));
        GS(void,glGenTextures,(int,unsigned*)); GS(void,glBindTexture,(unsigned,unsigned)); GS(void,glTexImage2D,(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)); GS(void,glTexParameteri,(unsigned,unsigned,int));
        GS(void,glClearColor,(float,float,float,float)); GS(void,glClear,(unsigned)); GS(void,glViewport,(int,int,int,int)); GS(void,glDrawArrays,(unsigned,int,int));
        GS(int,glGetUniformLocation,(unsigned,const char*)); GS(void,glUniform1i,(int,int)); GS(unsigned,glGetError,(void));
        GS(void,glReadBuffer,(unsigned)); GS(void,glReadPixels,(int,int,int,int,unsigned,unsigned,void*));
        #undef GS
        int iw=0,ih=0; unsigned char*img=bmp_load_rgba(imgpath,&iw,&ih);
        if(!img){ ws("  [gltest] failed to load image "); ws(imgpath); ws("\n"); return; }
        char m[200]; int k=snprintf(m,sizeof m,"  [gltest] loaded %dx%d from %s\n",iw,ih,imgpath); (void)!write(2,m,k);
        unsigned tx=0; glGenTextures(1,&tx); glBindTexture(0x0DE1/*TEXTURE_2D*/,tx);
        glTexParameteri(0x0DE1,0x2801/*MIN_FILTER*/,0x2601/*LINEAR*/); glTexParameteri(0x0DE1,0x2800/*MAG_FILTER*/,0x2601);
        glTexParameteri(0x0DE1,0x2802/*WRAP_S*/,0x812F/*CLAMP_EDGE*/); glTexParameteri(0x0DE1,0x2803/*WRAP_T*/,0x812F);
        glTexImage2D(0x0DE1,0,0x8058/*RGBA8*/,iw,ih,0,0x1908/*RGBA*/,0x1401/*UBYTE*/,img); free(img);
        const char*tvs="#version 410 core\nlayout(location=0) in vec2 aPos;out vec2 vUv;void main(){vUv=vec2((aPos.x+1.0)*0.5,1.0-(aPos.y+1.0)*0.5);gl_Position=vec4(aPos,0.0,1.0);}\n";
        const char*tfs="#version 410 core\nin vec2 vUv;out vec4 o;uniform sampler2D t;void main(){o=texture(t,vUv);}\n";
        unsigned vv=glCreateShader(0x8B31),ff=glCreateShader(0x8B30); glShaderSource(vv,1,&tvs,0); glCompileShader(vv); glShaderSource(ff,1,&tfs,0); glCompileShader(ff);
        unsigned pr=glCreateProgram(); glAttachShader(pr,vv); glAttachShader(pr,ff); glLinkProgram(pr);
        float quad[]={ -1,-1, 1,-1, -1,1,  -1,1, 1,-1, 1,1 };
        unsigned va=0,vb=0; glGenVertexArrays(1,&va); glBindVertexArray(va); glGenBuffers(1,&vb); glBindBuffer(0x8892,vb); glBufferData(0x8892,sizeof quad,quad,0x88E4);
        glVertexAttribPointer(0,2,0x1406,0,8,(void*)0); glEnableVertexAttribArray(0);
        glViewport(0,0,W,H);
        int frames=getenv("PELOAD_GLTEST_FRAMES")?atoi(getenv("PELOAD_GLTEST_FRAMES")):90;
        for(int i=0;i<frames;i++){ glClearColor(0,0,0,1); glClear(0x4000); glUseProgram(pr);
            int loc=glGetUniformLocation(pr,"t"); if(loc>=0)glUniform1i(loc,0); glBindTexture(0x0DE1,tx); glBindVertexArray(va); glDrawArrays(0x0004,0,6);
            if(g_nsctx) om0(g_nsctx,"flushBuffer"); cocoa_pump();
            if(i==frames/2){ unsigned char*cap=calloc(1,(size_t)W*H*4); if(cap){ if(glReadBuffer)glReadBuffer(0x0405); if(glGetError)glGetError();
                glReadPixels(0,0,W,H,0x1908,0x1401,cap); const char*home=getenv("HOME"); char pth[512]; snprintf(pth,sizeof pth,"%s/reclaimer_gltest_img.bmp",home?home:"/tmp");
                bmp_write_rgba(pth,W,H,cap); ws("  [gltest] captured textured frame -> "); ws(pth); ws("\n"); free(cap);} }
            struct timespec ts={0,16*1000000L}; nanosleep(&ts,0); }
        ws("  [gltest] image done\n"); return;
    }
    const char*vs="#version 410 core\nlayout(location=0) in vec2 aPos;layout(location=1) in vec3 aCol;out vec3 vCol;void main(){vCol=aCol;gl_Position=vec4(aPos,0.0,1.0);}\n";
    const char*fs="#version 410 core\nin vec3 vCol;out vec4 o;void main(){o=vec4(vCol,1.0);}\n";
    unsigned v=glCreateShader(0x8B31),p=glCreateShader(0x8B30); int ok=0;
    glShaderSource(v,1,&vs,0); glCompileShader(v); glGetShaderiv(v,0x8B81,&ok);
    if(!ok){ char log[512]={0}; if(glGetShaderInfoLog)glGetShaderInfoLog(v,512,0,log); ws("  [gltest] VS compile FAIL: "); ws(log); ws("\n"); }
    glShaderSource(p,1,&fs,0); glCompileShader(p); glGetShaderiv(p,0x8B81,&ok);
    if(!ok){ char log[512]={0}; if(glGetShaderInfoLog)glGetShaderInfoLog(p,512,0,log); ws("  [gltest] FS compile FAIL: "); ws(log); ws("\n"); }
    unsigned prog=glCreateProgram(); glAttachShader(prog,v); glAttachShader(prog,p); glLinkProgram(prog);
    glGetProgramiv(prog,0x8B82,&ok); ws(ok?"  [gltest] program linked\n":"  [gltest] program LINK FAIL\n");
    float tri[]={ -0.8f,-0.7f, 1,0,0,   0.8f,-0.7f, 0,1,0,   0.0f,0.85f, 0,0,1 };
    unsigned vao=0,vbo=0; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(0x8892,vbo); glBufferData(0x8892,sizeof tri,tri,0x88E4);
    glVertexAttribPointer(0,2,0x1406,0,20,(void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,0x1406,0,20,(void*)8); glEnableVertexAttribArray(1);
    glViewport(0,0,W,H);
    int frames = getenv("PELOAD_GLTEST_FRAMES")? atoi(getenv("PELOAD_GLTEST_FRAMES")) : 90;
    for(int i=0;i<frames;i++){
        float t=i/60.0f; glClearColor(0.10f+0.08f*sinf(t*2),0.12f,0.22f+0.10f*sinf(t*1.3f),1);
        glClear(0x4000|0x100); glUseProgram(prog); glBindVertexArray(vao); glDrawArrays(0x0004,0,3);
        if(g_nsctx) om0(g_nsctx,"flushBuffer"); cocoa_pump();
        if(i==frames/2){ unsigned char*cap=calloc(1,(size_t)W*H*4);
            if(cap){ if(glReadBuffer)glReadBuffer(0x0405/*GL_BACK*/); if(glGetError)glGetError();
                glReadPixels(0,0,W,H,0x1908,0x1401,cap); unsigned e=glGetError?glGetError():0;
                const char*home=getenv("HOME"); char pth[512]; snprintf(pth,sizeof pth,"%s/reclaimer_gltest.bmp",home?home:"/tmp");
                bmp_write_rgba(pth,W,H,cap); char m[160]; int k=snprintf(m,sizeof m,"  [gltest] captured frame %d (readpixels err=0x%x) -> %s\n",i,e,pth); (void)!write(2,m,k); free(cap);} }
        struct timespec ts={0,16*1000000L}; nanosleep(&ts,0);
    }
    ws("  [gltest] done\n");
}
// void* wrapper so earlier code can create/own the GL window without needing
// CGLContextObj in scope. The NSWindow is created on the (main) calling thread.
// Under PELOAD_HOLD_WINDOW we hand the GL context to a dedicated present thread that
// shows the engine's GDI composite continuously (so it survives the worker's abort).
static void* present_thread_fn(void*a){ (void)a; void(*set)(void*)=dlsym(RTLD_DEFAULT,"CGLSetCurrentContext");
    if(set&&g_present_glctx) set(g_present_glctx);
    void(*cglflush)(void*)=dlsym(RTLD_DEFAULT,"CGLFlushDrawable");
    // PELOAD_POKE=<rva>:<u8>[,...] continuously writes a byte into guest memory each frame
    // (e.g. force the worker's "window foreground" flag so presents count as ready).
    extern uint8_t* g_base;
    for(;;){
        // When the D3D11->GL executor is running offscreen, it produced the frame; display THAT
        // (the real translated-shader output) on this thread's window instead of the GDI composite.
        int use_d3dgl = d3dgl_enabled() && g_d3dgl_ready && g_d3dgl_offscreen && g_d3dgl_present_tex;
        if(getenv("PELOAD_TRACE_PRESENT")){ static int lp; if((lp++ %60)==0){ char m[96]; int k=snprintf(m,sizeof m,"  [present] branch=%s d3dgl(en=%d ready=%d off=%d tex=%u)\n",use_d3dgl?"d3dgl_blit":"gdi_main",d3dgl_enabled(),g_d3dgl_ready,g_d3dgl_offscreen,g_d3dgl_present_tex); (void)!write(2,m,k);} }
        if(use_d3dgl){
            d3dgl_blit_present(); if(cglflush&&g_present_glctx) cglflush(g_present_glctx);
        } else gdi_present_main();
        // PELOAD_WIN_SHOT: capture the ACTUAL window framebuffer (post-blit) so we see exactly what the
        // user sees, independent of screencapture z-order/timing. Writes bottom-up BMP (GL readpixels).
        if(getenv("PELOAD_WIN_SHOT")){ static int wc; int every=getenv("PELOAD_WIN_SHOT_EVERY")?atoi(getenv("PELOAD_WIN_SHOT_EVERY")):30; if(every<1)every=30; wc++; if(wc>=5 && (wc % every)==0){
            void(*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels");
            void(*rb)(unsigned)=dlsym(RTLD_DEFAULT,"glReadBuffer");
            if(rp){ int W=WIN_W,H=WIN_H; unsigned char*px=calloc(1,(size_t)W*H*4);
                if(px){ if(rb)rb(0x0405/*BACK*/); rp(0,0,W,H,0x1908/*RGBA*/,0x1401,px);
                    const char*home=getenv("HOME"); char p[256]; snprintf(p,sizeof p,"%s/reclaimer_win_%d_%d.bmp",home?home:"/tmp",getpid(),wc);
                    FILE*f=fopen(p,"wb"); if(f){ uint32_t isz=(uint32_t)W*H*4,fsz=54+isz,o=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&o,4);
                        uint8_t ih[40]={0}; uint32_t v=40; memcpy(ih,&v,4); memcpy(ih+4,&W,4); memcpy(ih+8,&H,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
                        fwrite(fh,1,14,f); fwrite(ih,1,40,f);
                        for(int i=0;i<W*H;i++){ uint8_t*q=px+i*4; uint8_t b[4]={q[2],q[1],q[0],255}; fwrite(b,1,4,f);} fclose(f); }
                    free(px); } } } }
        const char* pk=getenv("PELOAD_POKE");
        if(pk&&g_base){ char buf[128]; snprintf(buf,sizeof buf,"%s",pk); for(char*tok=strtok(buf,",");tok;tok=strtok(0,",")){ char*c=strchr(tok,':'); if(!c)continue; *c=0; uint64_t rva=strtoull(tok,0,16); uint32_t v=(uint32_t)strtoull(c+1,0,16); g_base[rva]=(uint8_t)v; } }
        // PELOAD_POKE32=<abs_hex>:<v32>[,...] writes a 32-bit value at an absolute guest addr
        // (e.g. a halo3 GPU-frame-ready flag at 0x1821b0a74 + 24*i) each present cycle.
        const char* pk32=getenv("PELOAD_POKE32");
        if(pk32){ char buf[256]; snprintf(buf,sizeof buf,"%s",pk32); for(char*tok=strtok(buf,",");tok;tok=strtok(0,",")){ char*c=strchr(tok,':'); if(!c)continue; *c=0; uint64_t a=strtoull(tok,0,16); uint32_t v=(uint32_t)strtoull(c+1,0,16); if(a>0x100000000ULL) *(volatile uint32_t*)a=v; } }
        struct timespec ts={0,33*1000000L}; nanosleep(&ts,0); } return 0; }
static void* cocoa_make_gl_window_i(int w,int h){ CGLContextObj c=cocoa_make_gl_window(w,h);
    if(c){ g_present_glctx=(void*)c; void(*set)(void*)=dlsym(RTLD_DEFAULT,"CGLSetCurrentContext");
        if(getenv("PELOAD_HOLD_WINDOW")){ if(!g_present_started){ g_present_started=1; if(set)set(0); /*release from this thread*/ pthread_t th; pthread_create(&th,0,present_thread_fn,0); ws("  [hold-window] present thread started\n"); } }
        else if(set) set(c); }
    return (void*)c; }
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
static void gl_check_src(const char*who,int w,int h,unsigned fmt,unsigned type,const void*d); // fwd
static void MSABI i_glTexImage2D(unsigned t,int l,int ifmt,int w,int h,int b,unsigned fmt,unsigned type,const void*d){
    if(d && getenv("PELOAD_TRACE_TEX")) gl_check_src("glTexImage2D",w,h,fmt,type,d);
    void(*r)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glTexImage2D"); if(r)r(t,l,ifmt,w,h,b,fmt,type,d);
    if(getenv("PELOAD_TRACE_GL")){ char s[160]; snprintf(s,sizeof s,"  [glTexImage2D] ifmt=0x%x %dx%d fmt=0x%x type=0x%x d=%p\n",ifmt,w,h,fmt,type,d); ws(s);} }
// bytes-per-pixel for the common GL client format/type combos used by glow uploads
static int gl_bpp(unsigned fmt,unsigned type){
    int comp = (fmt==0x1903/*RED*/||fmt==0x1902/*DEPTH*/)?1 : (fmt==0x8227/*RG*/)?2 :
               (fmt==0x1907/*RGB*/||fmt==0x80E0/*BGR*/)?3 :
               (fmt==0x1908/*RGBA*/||fmt==0x80E1/*BGRA*/)?4 : 4;
    int sz = (type==0x1401/*UBYTE*/||type==0x1400/*BYTE*/)?1 :
             (type==0x1403/*USHORT*/||type==0x1402/*SHORT*/||type==0x140B/*HALF*/)?2 :
             (type==0x1405/*UINT*/||type==0x1406/*FLOAT*/||type==0x8368/*UINT_8_8_8_8_REV*/||type==0x8367)?4 : 1;
    // packed pixel types: one unit covers all components
    if(type==0x8368||type==0x8367||type==0x8035||type==0x8033) return sz;
    return comp*sz; }
// Report whether source pointer `d` is a guarded block too small for w*h bytes.
static void gl_check_src(const char*who,int w,int h,unsigned fmt,unsigned type,const void*d){
    if(!d||!heapguard()) return; void*buf=0; uint64_t rsz=0; int64_t over=0;
    // find the guarded block CONTAINING d (not just its guard page)
    pthread_mutex_lock(&g_hgm);
    for(uint32_t i=0;i<HGSET;i++){ void*u=g_hgset[i]; if(!u||u==(void*)1) continue;
        if((uintptr_t)d>=(uintptr_t)u && (uintptr_t)d<(uintptr_t)u+g_hgsz[i]){ buf=u; rsz=g_hgsz[i]; break; } }
    pthread_mutex_unlock(&g_hgm);
    int bpp=gl_bpp(fmt,type); long long need=(long long)w*h*bpp;
    char s[200]; int n=snprintf(s,sizeof s,"  [%s] %dx%d fmt=0x%x type=0x%x bpp=%d need=%lld src=%p%s",
        who,w,h,fmt,type,bpp,need,d, buf?" GUARDED-BUF":"");
    if(buf) n+=snprintf(s+n,sizeof s-n," buf=%p sz=%llu %s\n",buf,(unsigned long long)rsz, (need>(long long)rsz)?"*** TOO SMALL ***":"ok");
    else n+=snprintf(s+n,sizeof s-n,"\n");
    (void)over; (void)!write(2,s,n); }
static void MSABI i_glTexSubImage2D(unsigned t,int l,int xo,int yo,int w,int h,unsigned fmt,unsigned type,const void*d){
    if(getenv("PELOAD_TRACE_TEX")) gl_check_src("glTexSubImage2D",w,h,fmt,type,d);
    void(*r)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glTexSubImage2D"); if(r)r(t,l,xo,yo,w,h,fmt,type,d); }
static void MSABI i_glTextureSubImage2D(unsigned tex,int l,int xo,int yo,int w,int h,unsigned fmt,unsigned type,const void*d){
    if(getenv("PELOAD_TRACE_TEX")) gl_check_src("glTextureSubImage2D",w,h,fmt,type,d);
    void(*r)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glTextureSubImage2D"); if(r)r(tex,l,xo,yo,w,h,fmt,type,d); }
static void MSABI i_glReadPixels(int x,int y,int w,int h,unsigned fmt,unsigned type,void*d){
    if(getenv("PELOAD_TRACE_TEX")) gl_check_src("glReadPixels(DST)",w,h,fmt,type,d);
    void(*r)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels"); if(r)r(x,y,w,h,fmt,type,d); }
static void MSABI i_glGetTexImage(unsigned tgt,int l,unsigned fmt,unsigned type,void*d){
    if(getenv("PELOAD_TRACE_TEX")){ // query the bound texture's level dimensions for the size check
        void(*gl)(unsigned,int,unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetTexLevelParameteriv");
        int w=0,h=0; if(gl){ gl(tgt,l,0x1000/*TEX_WIDTH*/,&w); gl(tgt,l,0x1001/*TEX_HEIGHT*/,&h);} gl_check_src("glGetTexImage(DST)",w,h,fmt,type,d); }
    void(*r)(unsigned,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glGetTexImage"); if(r)r(tgt,l,fmt,type,d); }
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
        const char*home=getenv("HOME"); char path[512]; snprintf(path,sizeof path,"%s/reclaimer_fbo_%dx%d.bmp",home?home:"/tmp",W,H); // FBO the engine draws to, size-stamped (keep distinct passes)
        FILE*f=fopen(path,"wb"); if(f){ uint32_t isz=(uint32_t)W*H*4,fsz=54+isz,off=54; uint8_t fh[14]={'B','M'}; memcpy(fh+2,&fsz,4); memcpy(fh+10,&off,4);
            uint8_t ih[40]={0}; uint32_t v40=40; memcpy(ih,&v40,4); memcpy(ih+4,&W,4); memcpy(ih+8,&H,4); uint16_t pl=1,bpp=32; memcpy(ih+12,&pl,2); memcpy(ih+14,&bpp,2); memcpy(ih+20,&isz,4);
            fwrite(fh,1,14,f); fwrite(ih,1,40,f);
            for(int i=0;i<W*H;i++){ uint8_t*p=px+i*4; uint8_t b[4]={p[2],p[1],p[0],255}; fwrite(b,1,4,f);} fclose(f);
            if(ncap==0||ncap%50==0){ char m[600]; snprintf(m,sizeof m,"  [gl_capture_resolve] #%d %dx%d drawfb=%d readfb=%d err=0x%x -> %s\n",ncap,W,H,drawfb,readfb,e,path); ws(m);} }
        free(px); ncap++; }
    bf(0x8CA8,(unsigned)readfb); bf(0x8CA6,(unsigned)drawfb); }
// Present from the RENDER thread (this is where the GL context is current). The
// engine never SwapBuffers, so its rendered frame never reaches the window. Throttled
// present_frontend/gdi_present_main here (state-safe: they save/restore FBO bindings)
// flushes the frontend FBO + GDI composite to the visible window. PELOAD_PRESENT gates.
static void MSABI i_glDrawElements(unsigned mode,int count,unsigned type,const void*idx){ void(*r)(unsigned,int,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glDrawElements"); if(r)r(mode,count,type,idx);
    if(getenv("PELOAD_SHOT")) gl_capture_resolve();
    static unsigned long dc; if(getenv("PELOAD_PRESENT") && (dc++ % 200)==0){ present_frontend(); gdi_present_main(); } }
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
    if(!strcmp(name,"glTexSubImage2D")) return (void*)i_glTexSubImage2D;
    if(!strcmp(name,"glTextureSubImage2D")) return (void*)i_glTextureSubImage2D;
    if(!strcmp(name,"glReadPixels")) return (void*)i_glReadPixels;
    if(!strcmp(name,"glGetTexImage")) return (void*)i_glGetTexImage;
    if(!strcmp(name,"glGetString")) return (void*)i_glGetString;
    if(!strcmp(name,"glShaderSource")||!strcmp(name,"glShaderSourceARB")) return (void*)i_glShaderSource;
    if(!strcmp(name,"glCompileShader")||!strcmp(name,"glCompileShaderARB")) return (void*)i_glCompileShader;
    if(!strcmp(name,"glLinkProgram")) return (void*)i_glLinkProgram;
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
    void(*real)(unsigned)=dlsym(RTLD_DEFAULT,"glCompileShader"); if(real) real(shader);
    if(getenv("PELOAD_TRACE_GLSL")){ void(*giv)(unsigned,unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetShaderiv"); void(*gil)(unsigned,int,int*,char*)=dlsym(RTLD_DEFAULT,"glGetShaderInfoLog");
        int ok=1; if(giv) giv(shader,0x8B81,&ok); if(!ok){ char log[1024]={0}; int n=0; if(gil) gil(shader,1024,&n,log); ws("  [SHADER COMPILE FAIL] "); ws(log); ws("\n"); } } }
static void MSABI i_glLinkProgram(unsigned prog){
    void(*real)(unsigned)=dlsym(RTLD_DEFAULT,"glLinkProgram"); if(real) real(prog);
    if(getenv("PELOAD_TRACE_GLSL")){ void(*giv)(unsigned,unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetProgramiv"); void(*gil)(unsigned,int,int*,char*)=dlsym(RTLD_DEFAULT,"glGetProgramInfoLog");
        int ok=1; if(giv) giv(prog,0x8B82,&ok); if(!ok){ char log[1024]={0}; int n=0; if(gil) gil(prog,1024,&n,log); ws("  [PROGRAM LINK FAIL] "); ws(log); ws("\n"); } else ws("  [program linked ok]\n"); } }
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
// The guest runs engine/panic code on a SWITCHED stack (0x30xxxxxxx), but by the
// time __CxxFrameHandler3 drives RtlUnwindEx the context it hands us can carry a
// stale host-stack RSP (0x7ff8...) that cannot unwind to the guest-stack target.
// i_RaiseException stashes the true exception-point context (on the guest stack)
// here so RtlUnwindEx can unwind from the right stack to reach the catch frame.
static __thread uint8_t g_exc_ctx[CTXSZ]; static __thread int g_exc_valid;
// Unwind to TargetFrame, running termination handlers; then jump to TargetIp.
static void MSABI i_RtlUnwindEx(uint64_t target, void* targetip, EXREC* rec, void* retval, void* ctx, void* hist){
    (void)hist; uint8_t tmp[CTXSZ]; if(!ctx){ ctx=tmp; memset(ctx,0,CTXSZ); seh_capture(ctx); }
    EXREC localrec; if(!rec){ memset(&localrec,0,sizeof localrec); localrec.ExceptionCode=0xC0000027/*STATUS_UNWIND*/; rec=&localrec; }
    rec->ExceptionFlags |= 2; // EXCEPTION_UNWINDING
    uint8_t work[CTXSZ];
    // If the passed context's RSP is not on the same stack as the target frame
    // (it is above target, or more than 128MB below it), but our stashed guest
    // exception context IS a plausible starting point to reach target, unwind
    // from the stash instead. This bridges the guest's host->switched stack gap.
    if(g_exc_valid && target){ uint64_t prsp=CTX_RSP(ctx), gsp=CTX_RSP((void*)g_exc_ctx);
        int passed_ok = prsp && prsp<=target && target-prsp < 0x8000000ull;
        int stash_ok  = gsp  && gsp <=target && target-gsp  < 0x8000000ull;
        if(!passed_ok && stash_ok){ memcpy(ctx,g_exc_ctx,CTXSZ);
            if(getenv("PELOAD_TRACE_SEH")) ws("    [unwind] using stashed guest ctx (passed ctx on wrong stack)\n"); } }
    // Unwinding only ever moves RSP UP (toward the stack base); if a frame's unwind
    // codes are misinterpreted and RSP jumps to a bogus value, reading [RSP] faults.
    // Bound RSP to a sane window above where we started so a lost target can't walk
    // us into garbage — stop and transfer to the catch with the best context so far.
    uint64_t sp_start=CTX_RSP(ctx);
    if(getenv("PELOAD_TRACE_SEH")){ void*caller=__builtin_return_address(0);
        uint64_t cb=0; uint8_t*cm=seh_module((uint64_t)caller,&cb,0); const char*cn="?";
        if(cm){ if(cm==g_base)cn="exe"; else for(int i=0;i<g_nmods;i++) if(g_mods[i].base==cm){cn=g_mods[i].name;break;} }
        char t[200]; int k=snprintf(t,sizeof t,"    [unwind] target=0x%llx targetip=0x%llx(exe+0x%llx) sp_start=0x%llx caller=%s+0x%llx\n",(unsigned long long)target,(unsigned long long)(uint64_t)targetip,(unsigned long long)((uint64_t)targetip-0x140000000ull),(unsigned long long)sp_start,cn,(unsigned long long)((uint64_t)caller-cb)); (void)!write(2,t,k);}
    for(int guard=0; guard<512; guard++){
        // Stack grows down, so each unwound frame's EstablisherFrame is a strictly
        // HIGHER address; once we reach OR pass `target` we must stop. Requiring
        // exact equality let a one-off estab miss walk the loop past the target
        // into garbage (CTX_RSP -> a bogus low address, faulting in i_RtlUnwindEx).
        uint64_t sp0=CTX_RSP(ctx);
        if(sp0 && target && sp0 > target) break;     // already at/above the target frame's stack
        if(sp_start && (sp0 < sp_start || sp0 > sp_start + 0x8000000ull)) break; // RSP left the sane window
        uint64_t base=0; RUNFN* fn=i_RtlLookupFunctionEntry(CTX_RIP(ctx),&base,0);
        if(!fn){ uint64_t sp=CTX_RSP(ctx); if(sp<sp_start||sp>sp_start+0x8000000ull) break; CTX_RIP(ctx)=*(uint64_t*)sp; CTX_RSP(ctx)=sp+8; if((uint64_t)CTX_RIP(ctx)==0) break; continue; }
        memcpy(work,ctx,CTXSZ); void* hdata=0; uint64_t estab=0;
        PEXHANDLER h=i_RtlVirtualUnwind(2/*UHANDLER*/,base,CTX_RIP(ctx),fn,work,&hdata,&estab,0);
        if(getenv("PELOAD_TRACE_SEH")){ char t[112]; int k=snprintf(t,sizeof t,"    [unwind] rip=exe+0x%llx estab=0x%llx h=%p\n",(unsigned long long)(CTX_RIP(ctx)-0x140000000ull),(unsigned long long)estab,(void*)h); (void)!write(2,t,k);}
        if(target && estab>=target){               // reached the destination frame
            // Call its handler one last time with EXCEPTION_TARGET_UNWIND set: this
            // is what makes __CxxFrameHandler3 run the C++ CATCH FUNCLET (the catch
            // body). Without it we'd jump to the continuation with nothing caught.
            if(h){ rec->ExceptionFlags |= 0x20; /*EXCEPTION_TARGET_UNWIND*/
                DISPCTX dc; memset(&dc,0,sizeof dc); dc.ControlPc=CTX_RIP(ctx); dc.ImageBase=base; dc.FunctionEntry=fn;
                dc.EstablisherFrame=estab; dc.ContextRecord=ctx; dc.LanguageHandler=h; dc.HandlerData=hdata; dc.TargetIp=(uint64_t)targetip;
                if(getenv("PELOAD_TRACE_SEH")) ws("    [unwind] TARGET_UNWIND -> running catch funclet\n");
                h(rec, estab, ctx, &dc);
            }
            break;
        }
        if(h && (estab>CTX_RSP(ctx))){               // run this frame's termination handler
            DISPCTX dc; memset(&dc,0,sizeof dc); dc.ControlPc=CTX_RIP(ctx); dc.ImageBase=base; dc.FunctionEntry=fn;
            dc.EstablisherFrame=estab; dc.ContextRecord=ctx; dc.LanguageHandler=h; dc.HandlerData=hdata; dc.TargetIp=(uint64_t)targetip;
            h(rec, estab, ctx, &dc);
        }
        memcpy(ctx,work,CTXSZ);
        if(!CTX_RIP(ctx)) break;
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
    // Capture, then rebase the context onto the GUEST call site: seh_capture
    // records a RIP/RSP inside THIS shim, which has no guest .pdata, so the
    // unwinder would find no frames and no catch_unwind handler. Our return
    // address is the guest instruction after its `call RaiseException`, and the
    // guest's RSP there is just above our return-address slot ([rbp+8]).
    void* guest_rip = __builtin_return_address(0);
    uint64_t guest_rsp = (uint64_t)__builtin_frame_address(0) + 16;
    uint8_t ctxbuf[CTXSZ]; memset(ctxbuf,0,CTXSZ); void* ctx=ctxbuf; seh_capture(ctx);
    CTX_RIP(ctx)=(uint64_t)guest_rip; CTX_RSP(ctx)=guest_rsp;
    // Stash the exception-point context (on the guest's switched stack) so a later
    // RtlUnwindEx driven by __CxxFrameHandler3 can unwind from the correct stack.
    // Only for C++/Rust throws (the stack-switch case); cleared after dispatch.
    if(code==0xE06D7363){ memcpy(g_exc_ctx,ctx,CTXSZ); g_exc_valid=1; }
    if(getenv("PELOAD_TRACE_SEH")&&code==0xE06D7363){ char t[128]; int k=snprintf(t,sizeof t,
        "  [raise] thr=0x%x main=%d guest_rip=0x%llx guest_rsp=0x%llx\n",pthread_mach_thread_np(pthread_self()),
        pthread_main_np(),(unsigned long long)(uint64_t)guest_rip,(unsigned long long)guest_rsp); (void)!write(2,t,k);}
    if(getenv("PELOAD_TRACE_STACK")&&code==0xE06D7363){ // raw scan: all exe/halo3 return addrs on the stack
        uint64_t sp=guest_rsp; int n=0, uic=0;
        for(int i=0;i<65536 && n<400;i++){ uint64_t v=*(uint64_t*)(sp+i*8);
            int e=v>=0x140000000ull&&v<0x142000000ull, h=v>=0x180000000ull&&v<0x182000000ull;
            uint64_t rv=v-0x140000000ull;
            if(e && rv>=0x550e00 && rv<0x557d3c){ char r[80]; int k=snprintf(r,sizeof r,"    *** UI_COMPOSITION frame #%d stk[%d] exe+0x%llx\n",uic++,n,(unsigned long long)rv); (void)!write(2,r,k); }
            if(e||h){ if(n<160){ char r[64]; int k=snprintf(r,sizeof r,"    stk[%d] %s+0x%llx\n",n,e?"exe":"halo3",(unsigned long long)(v-(e?0x140000000ull:0x180000000ull))); (void)!write(2,r,k);} n++; } } }
    EXREC rec; memset(&rec,0,sizeof rec); rec.ExceptionCode=code; rec.ExceptionFlags=flags;
    rec.ExceptionAddress=(void*)CTX_RIP(ctx); rec.NumberParameters=(n>15?15:n);
    if(args) for(uint32_t i=0;i<rec.NumberParameters;i++) rec.ExceptionInformation[i]=((const uint64_t*)args)[i];
    uint8_t work[CTXSZ]; int tr=getenv("PELOAD_TRACE_SEH")!=0;
    for(int guard=0; guard<512; guard++){
        uint64_t base=0; RUNFN* fn=i_RtlLookupFunctionEntry(CTX_RIP(ctx),&base,0);
        if(!fn){ uint64_t sp=CTX_RSP(ctx); if(!sp) break; uint64_t rip=*(uint64_t*)sp; CTX_RSP(ctx)=sp+8; CTX_RIP(ctx)=rip; if(!rip) break;
            if(tr){ char t[80]; int k=snprintf(t,sizeof t,"    [seh] leaf rip=0x%llx (no .pdata), ret->0x%llx\n",(unsigned long long)(sp?*(uint64_t*)(sp-8):0),(unsigned long long)rip); (void)!write(2,t,k);} continue; }
        memcpy(work,ctx,CTXSZ); void* hdata=0; uint64_t estab=0;
        PEXHANDLER h=i_RtlVirtualUnwind(1/*EHANDLER*/,base,CTX_RIP(ctx),fn,work,&hdata,&estab,0);
        if(tr){ char t[128]; int k=snprintf(t,sizeof t,"    [seh] frame rip=0x%llx (exe+0x%llx) handler=%p estab=0x%llx hdata=%p\n",
            (unsigned long long)CTX_RIP(ctx),(unsigned long long)(CTX_RIP(ctx)-0x140000000ull),(void*)h,(unsigned long long)estab,hdata); (void)!write(2,t,k); }
        if(h){
            DISPCTX dc; memset(&dc,0,sizeof dc); dc.ControlPc=CTX_RIP(ctx); dc.ImageBase=base; dc.FunctionEntry=fn;
            dc.EstablisherFrame=estab; dc.ContextRecord=ctx; dc.LanguageHandler=h; dc.HandlerData=hdata;
            int disp=h(&rec, estab, ctx, &dc);     // __CxxFrameHandler3 et al.
            if(tr){ char t[64]; int k=snprintf(t,sizeof t,"    [seh] handler disp=%d\n",disp); (void)!write(2,t,k); }
            // 0=ContinueExecution, 1=ContinueSearch. A C++ catch does its own
            // RtlUnwindEx+transfer and never returns here.
            if(disp==0){ seh_restore(ctx); }       // resume at (possibly modified) ctx
            // else ContinueSearch: fall through to next frame
        }
        memcpy(ctx,work,CTXSZ);                     // move to the caller frame
        if(!CTX_RIP(ctx)) break;
    }
    if(tr) ws("    [seh] dispatch loop ended (unhandled)\n");
    g_exc_valid=0;   // consumed/unhandled: don't let a later normal unwind use a stale stash
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
enum { K_EVENT, K_MUTEX, K_SEMA, K_THREAD };
typedef struct { uint32_t magic; int kind; pthread_mutex_t m; pthread_cond_t c; int signaled, manual; long count, maxc; pthread_t owner; uint32_t tid, exitcode;
    int is_timer, timer_armed; uint64_t timer_due_us; long timer_period_ms; } SyncObj;
static SyncObj* sync_new(int kind){ SyncObj*s=calloc(1,sizeof *s); s->magic=SYNC_MAGIC; s->kind=kind;
    pthread_mutex_init(&s->m,0); pthread_cond_init(&s->c,0); return s; }
static int is_sync(intptr_t hv){ if(hv<0x100000) return 0; SyncObj*s=(SyncObj*)hv; return s && s->magic==SYNC_MAGIC; }
static void name_log(const char*api,const uint16_t*nm); // fwd
static void* MSABI i_CreateEventW(void*sec,int man,int sig,const uint16_t*nm){ (void)sec; name_log("CreateEvent",nm); SyncObj*s=sync_new(K_EVENT); s->manual=man; s->signaled=sig; return s; }
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
// wait one object with a deadline; returns 0 signaled, 0x102 timeout.
// Waitable timers fire by wall clock (SetWaitableTimer arms timer_due_us).
static uint32_t sync_wait(SyncObj*s,uint32_t ms){
    struct timeval tv0; gettimeofday(&tv0,0);
    uint64_t caller_us = (ms==0xFFFFFFFF)? (uint64_t)-1 : ((uint64_t)tv0.tv_sec*1000000+tv0.tv_usec + (uint64_t)ms*1000);
    pthread_mutex_lock(&s->m); uint32_t rc=0;
    for(;;){
        struct timeval now; gettimeofday(&now,0); uint64_t nus=(uint64_t)now.tv_sec*1000000+now.tv_usec;
        if(s->is_timer && s->timer_armed && nus>=s->timer_due_us){ s->signaled=1; if(s->timer_period_ms>0) s->timer_due_us+=(uint64_t)s->timer_period_ms*1000; else s->timer_armed=0; }
        int avail = (s->kind==K_SEMA)? (s->count>0) : (s->kind==K_MUTEX)? (s->count>0) : s->signaled;
        if(avail){ if(s->kind==K_SEMA) s->count--; else if(s->kind==K_MUTEX){ s->count=0; s->owner=pthread_self(); } else if(!s->manual) s->signaled=0; rc=0; break; }
        if(ms==0 || nus>=caller_us){ rc=0x102; break; }
        uint64_t dl=caller_us; if(s->is_timer && s->timer_armed && s->timer_due_us<dl) dl=s->timer_due_us;
        if(dl==(uint64_t)-1){ pthread_cond_wait(&s->c,&s->m); }
        else { struct timespec ts; ts.tv_sec=(time_t)(dl/1000000); ts.tv_nsec=(long)((dl%1000000)*1000); pthread_cond_timedwait(&s->c,&s->m,&ts); }
    }
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
// The *Ex (alertable) variants MUST actually wait — stubbing them returns
// "signaled" instantly, so a thread waiting on an event/thread proceeds before
// the awaited work finishes = data race. We ignore the alertable flag (no APCs).
static uint32_t MSABI i_WaitForSingleObjectEx(void*h,uint32_t ms,int alert){ (void)alert; return i_WaitForSingleObject(h,ms); }
static uint32_t MSABI i_WaitForMultipleObjectsEx(uint32_t n,void*const*h,int all,uint32_t ms,int alert){ (void)alert; return i_WaitForMultipleObjects(n,h,all,ms); }
// Sleep: a no-op Sleep breaks yield/backoff-based coordination. Actually sleep.
static void     MSABI i_Sleep(uint32_t ms){ if(ms==0){ sched_yield(); return; } struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&ts,0); }
static uint32_t MSABI i_SleepEx(uint32_t ms,int alert){ (void)alert; i_Sleep(ms); return 0; }
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
#include <malloc/malloc.h>
static uint8_t *g_peb_ptr;
// Multi-module static TLS. Each PE with a TLS directory (exe, halo3.dll, CRT DLLs)
// has its OWN __declspec(thread) index + its own per-thread data block. If only the
// exe's TLS is set up (index 0), halo3's _tls_index global stays 0 and halo3 reads
// the EXE's block at gs:[0x58][0] -> wrong data -> e.g. a thread-local singleton
// slot reads a bogus pointer and faults. register_tls() assigns each module a
// distinct index (sharing g_tlsnext with TlsAlloc so dynamic+static never collide),
// publishes the main-thread block, and records the template so thread_tramp can
// build a fresh block per index on every new thread.
#define MAX_TLSMODS 16
static struct { uint8_t* src; uint64_t raw, total; uint32_t index; } g_tlsmods[MAX_TLSMODS];
static int g_ntlsmods;
static void* g_tls_array[128];   // the MAIN thread's TLS pointer array (published at gs:0x58)
typedef struct { uint64_t Start, End, Index, Callbacks; uint32_t Zero, Flags; } TLSDIR_;
static void register_tls(void* tlsdir, uint8_t* modbase, int fixed_index){
    TLSDIR_* t=(TLSDIR_*)tlsdir; if(!t) return;
    uint32_t idx;
    if(fixed_index>=0) idx=(uint32_t)fixed_index;
    else { pthread_mutex_lock(&g_tlsm); idx = g_tlsnext<128? g_tlsnext++ : 0; pthread_mutex_unlock(&g_tlsm); }
    uint64_t raw = t->End - t->Start, total = raw + t->Zero;
    uint8_t* block = malloc(total?total:1);
    if(raw) memcpy(block,(void*)t->Start,raw);
    if(total>raw) memset(block+raw,0,total-raw);
    *(uint32_t*)t->Index = idx;          // write the module's TLS index global
    g_tls_array[idx]=block;              // main thread's block for this index
    if(g_ntlsmods<MAX_TLSMODS){ g_tlsmods[g_ntlsmods].src=(uint8_t*)t->Start; g_tlsmods[g_ntlsmods].raw=raw; g_tlsmods[g_ntlsmods].total=total; g_tlsmods[g_ntlsmods].index=idx; g_ntlsmods++; }
    ws("  static TLS idx "); whex(idx); ws("  block "); whexn((uint64_t)block); ws(" size "); whex(total);
    if(t->Callbacks){ uint64_t* cb=(uint64_t*)t->Callbacks;
        for(int i=0; cb[i]; i++){ ws("  TLS callback "); whex(cb[i]);
            void (__attribute__((ms_abi)) *fn)(void*,uint32_t,void*) = (void*)cb[i]; fn(modbase,1/*PROCESS_ATTACH*/,0); } }
}
typedef uint32_t (MSABI *WinThreadProc)(void*);
typedef struct { WinThreadProc fn; void *param; SyncObj *done; } TArg;
static void *thread_tramp(void *a){
    TArg *t=a;
    uint8_t *teb=calloc(1,0x2000);
    void **tarr=calloc(128,sizeof(void*));
    // Fresh per-thread block for EVERY module's TLS index (exe, halo3, CRT), not
    // just index 0 — otherwise halo3's thread-locals alias the exe's on new threads.
    for(int i=0;i<g_ntlsmods;i++){ uint64_t tot=g_tlsmods[i].total, raw=g_tlsmods[i].raw;
        uint8_t *blk=malloc(tot?tot:1); if(raw) memcpy(blk,g_tlsmods[i].src,raw); if(tot>raw) memset(blk+raw,0,tot-raw);
        tarr[g_tlsmods[i].index]=blk; }
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
    // Signal the thread handle so WaitForSingleObject(hThread) actually blocks
    // until the thread finishes. Without this, waiting on a thread handle returned
    // instantly and the engine raced its own worker threads (e.g. parallel map
    // load) -> corrupted shared state. The handle stays signaled (manual-reset).
    if(t->done){ pthread_mutex_lock(&t->done->m); t->done->exitcode=r; t->done->signaled=1; pthread_cond_broadcast(&t->done->c); pthread_mutex_unlock(&t->done->m); }
    free(t);
    return (void*)(uintptr_t)r;
}
static void* MSABI i_CreateThread(void*attr,uint64_t stack,WinThreadProc start,void*param,uint32_t fl,uint32_t*tid){
    (void)attr;(void)stack;(void)fl;
    if(getenv("PELOAD_TRACE_THREAD")){ uint64_t s=(uint64_t)start; char t[96];
        int k=snprintf(t,sizeof t,"  [CreateThread] start=%s+0x%llx param=%p\n",
            (s>=0x140000000&&s<0x142000000)?"exe":(s>=0x180000000&&s<0x187000000)?"halo3":"?",
            (unsigned long long)(s>=0x140000000&&s<0x142000000?s-0x140000000:s>=0x180000000&&s<0x187000000?s-0x180000000:s),param);
        (void)!write(2,t,k); }
    if(getenv("PELOAD_NO_THREADS")){ if(tid)*tid=0x7333; return (void*)0x7333; } // diagnostic: don't spawn
    SyncObj* done=sync_new(K_THREAD); done->manual=1;              // thread handle = waitable, manual-reset
    TArg *t=malloc(sizeof *t); t->fn=start; t->param=param; t->done=done;
    pthread_t th; int pcrc=pthread_create(&th,0,thread_tramp,t);
    if(pcrc){ if(getenv("PELOAD_TRACE_THREAD")){ char eb[96]; int k=snprintf(eb,sizeof eb,"  [CreateThread] pthread_create FAILED rc=%d (start=%p param=%p)\n",pcrc,(void*)start,param); (void)!write(2,eb,k); } free(t); done->exitcode=0; done->signaled=1; return 0; }
    done->owner=th; done->tid=(uint32_t)(uintptr_t)th;
    if(tid)*tid=done->tid; return done;                           // HANDLE = the SyncObj
}
// ucrtbase _beginthreadex: the CRT thread spawn halo3 uses. Unimplemented, it
// stubbed to 0 (thread never created) -> halo3 waits on / uses data from a thread
// that never ran. Route it through CreateThread so the thread actually runs and
// is waitable. start is unsigned __stdcall(void*) == our MSABI WinThreadProc on x64.
static uintptr_t MSABI i_beginthreadex(void*sec,unsigned stack,WinThreadProc start,void*arg,unsigned initflag,unsigned*thrdaddr){
    uint32_t tid=0; void* h=i_CreateThread(sec,(uint64_t)stack,start,arg,initflag,&tid); if(thrdaddr)*thrdaddr=tid; return (uintptr_t)h; }
static void MSABI i_endthreadex(unsigned code){ (void)code; pthread_exit(0); }
static uintptr_t MSABI i_beginthread(WinThreadProc start,unsigned stack,void*arg){ uint32_t tid=0; return (uintptr_t)i_CreateThread(0,(uint64_t)stack,start,arg,0,&tid); }
static void MSABI i_endthread(void){ pthread_exit(0); }
static uint32_t MSABI i_GetThreadId(void*h){ if(is_sync((intptr_t)h)) return ((SyncObj*)h)->tid; return (uint32_t)(uintptr_t)h; }
static int MSABI i_GetExitCodeThread(void*h,uint32_t*code){
    if(is_sync((intptr_t)h)){ SyncObj*s=h; if(code)*code = s->signaled? s->exitcode : 259/*STILL_ACTIVE*/; return 1; }
    if(code)*code=259; return 1; }
static void* MSABI i_OpenProcess(uint32_t a,int inh,uint32_t pid){ (void)a;(void)inh;
    // Return a pid-encoded handle (0x8000+pid, same scheme as CreateProcessW children) so
    // GetProcessId/GetExitCodeProcess work on it: a non-child live pid reports STILL_ACTIVE.
    // Previously this returned (void*)-1, and GetExitCodeProcess(-1) falls through to code=0,
    // so the local-server discovery liveness check saw the advertised host as "exited" and
    // reported "Selected server restarted". Verify liveness with kill(pid,0).
    int alive = (pid!=0) && (kill((pid_t)pid,0)==0 || errno==EPERM);
    if(getenv("PELOAD_TRACE_IPC")){ char b[96]; int k=snprintf(b,sizeof b,"  [OpenProcess] pid=%u acc=0x%x -> %s\n",pid,a,alive?"alive":"dead"); (void)!write(2,b,k); }
    if(alive && pid<0x100000-0x8000) return (void*)(intptr_t)(0x8000+pid);
    return (void*)-1; /*keep legacy non-null for odd pids rather than failing callers*/ }
static uint32_t MSABI i_GetProcessId(void*h){ intptr_t hv=(intptr_t)h;
    if(hv>=0x8000 && hv<0x100000) return (uint32_t)(hv-0x8000);
    return (uint32_t)getpid(); /*current-process pseudo-handle (-1) and friends*/ }
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
    // FileRenameInfo(10) / FileRenameInfoEx(65): atomic rename. The engine's local-server
    // advertisement (and other atomic writes) write "<name>.partial" then RENAME to the final
    // name via this call; without it the final file never appears and client discovery fails.
    // struct: [0]=ReplaceIfExists/Flags, [8]=RootDirectory, [0x10]=FileNameLength(bytes),
    // [0x14]=FileName (UTF-16, an NT/\??\ path or relative to RootDirectory).
    else if((cls==10||cls==65) && info && g_fdpath[fd]){
        uint32_t nlen=*(uint32_t*)((char*)info+0x10); const uint16_t*wn=(const uint16_t*)((char*)info+0x14);
        uint16_t wb[1400]; int n=(int)(nlen/2); if(n>1399)n=1399; for(int i=0;i<n;i++) wb[i]=wn[i]; wb[n]=0;
        // strip NT prefixes \??\ or \\?\ ; then win2unix maps drive/path
        const uint16_t*p=wb; if(wb[0]=='\\'&&wb[1]=='?'&&wb[2]=='?'&&wb[3]=='\\') p=wb+4;
        char newp[2048];
        if(p[0] && p[1]==':'){ win2unix(p,newp,sizeof newp); }       // absolute drive path
        else { // relative to the old file's directory
            char d[2048]; snprintf(d,sizeof d,"%s",g_fdpath[fd]); char*sl=strrchr(d,'/'); if(sl)*sl=0; else d[0]=0;
            char rel[1600]; int j=0; for(int i=0;p[i]&&j<1599;i++){ char c=(char)(p[i]<128?p[i]:'?'); rel[j++]=(c=='\\')?'/':c; } rel[j]=0;
            snprintf(newp,sizeof newp,"%s/%s",d,rel);
        }
        int rr=rename(g_fdpath[fd],newp);
        if(getenv("PELOAD_TRACE_FS")){ char t[96]; int k=snprintf(t,sizeof t,"  [rename cls=%d] -> %d ",cls,rr); ws(t); ws(g_fdpath[fd]); ws(" => "); ws(newp); ws("\n"); }
        if(rr==0){ free(g_fdpath[fd]); g_fdpath[fd]=strdup(newp); g_fddel[fd]=0; }
        else { g_lasterr=win_err(); return 0; }
        return 1;
    }
    if(getenv("PELOAD_TRACE_FS")){ char t[96]; snprintf(t,sizeof t,"  [SetFileInfo] cls=%d d0=0x%x -> del=%d ",cls, info?*(uint32_t*)info:0, (cls==4||cls==21)?g_fddel[fd]:-1); ws(t); ws(g_fdpath[fd]?g_fdpath[fd]:"?"); ws("\n"); }
    return 1;
}
// Rust anon_pipe (Stdio::piped) builds a pipe via the NT API, not CreateNamedPipeW:
//   NtOpenFile("\Device\NamedPipe\")          -> a root handle to the pipe FS
//   NtCreateNamedPipeFile(root, <random name>) -> the server end (ours)
//   NtOpenFile(root=server, "")                -> the client end (theirs, opposite direction)
// We back it with a bidirectional socketpair (so read/write direction is a non-issue) and pair the
// two fds, so the client reopen (root == a pipe-server fd) returns the mate. This is what the
// dedicated server needs: it captures each game-server worker's stdout/stderr over piped stdio, so
// Command::spawn fails at the first anon_pipe (os error 2) without this.
#define PIPE_DEV_HANDLE ((void*)0x5500)
static int g_pipe_other[MAXFD];   // pipe fd -> (mate fd + 1); 0 = not an anon-pipe end
static int nt_name_is_pipedev(const char*n){ // n is already in slash form; Rust uses the \??\PIPE\ alias
    return n && (!strcasecmp(n,"/Device/NamedPipe") || !strncasecmp(n,"/Device/NamedPipe/",18)
                 || !strcasecmp(n,"/??/PIPE") || !strncasecmp(n,"/??/PIPE/",9)); }
static int MSABI i_NtCreateNamedPipeFile(void**ph,uint32_t acc,void*oa,void*iosb,uint32_t share,
        uint32_t disp,uint32_t opts,uint32_t ptype,uint32_t rmode,uint32_t cmode,uint32_t maxinst,
        uint32_t inq,uint32_t outq,void*timeout){
    (void)acc;(void)oa;(void)share;(void)disp;(void)opts;(void)ptype;(void)rmode;(void)cmode;
    (void)maxinst;(void)inq;(void)outq;(void)timeout;
    int sv[2]; if(socketpair(AF_UNIX,SOCK_STREAM,0,sv)!=0){ if(iosb)((uint64_t*)iosb)[0]=0xC0000001; return (int)0xC0000001; }
    if(sv[0]<MAXFD) g_pipe_other[sv[0]]=sv[1]+1;
    if(sv[1]<MAXFD) g_pipe_other[sv[1]]=sv[0]+1;
    if(ph)*ph=(void*)(intptr_t)(0x1000+sv[0]); if(iosb){((uint64_t*)iosb)[0]=0;((uint64_t*)iosb)[1]=1;}
    if(getenv("PELOAD_TRACE_FS")){ char b[64]; int k=snprintf(b,sizeof b,"  [NtCreateNamedPipe] sv=%d/%d\n",sv[0],sv[1]); (void)!write(2,b,k);} return 0;
}
// NtCreateFile: supports NT absolute names (\??\C:\..) and names relative to an
// OBJECT_ATTRIBUTES.RootDirectory handle — the path Rust remove_dir_all uses.
// mio opens "\Device\Afd\Mio" as its socket-readiness poller; forward to our AFD bridge.
static int afd_is_device_name(const char*name);
static void* afd_make_device(void);
static int nt_open_common(void **ph,uint32_t acc,void *oa,void *iosb,uint32_t disp,uint32_t opts){
    uint8_t *OA=oa; void *root = OA? *(void**)(OA+8) : 0; uint8_t *uni = OA? *(uint8_t**)(OA+16) : 0;
    char name[1600]={0}; if(uni){ uint16_t len=*(uint16_t*)uni; uint16_t *bufw=*(uint16_t**)(uni+8); int n=len/2,j=0; for(int i=0;i<n&&j<1599;i++){ char c=(char)(bufw[i]<128?bufw[i]:'?'); name[j++]=c;} name[j]=0; }
    for(char*q=name;*q;q++) if(*q=='\\') *q='/';
    // anon_pipe: client reopen (RootDirectory is a pipe-server fd) -> return the socketpair mate.
    { intptr_t rv=(intptr_t)root; if(rv>=0x1000){ int rfd=(int)(rv-0x1000); if(rfd>=0&&rfd<MAXFD&&g_pipe_other[rfd]){
        int mate=g_pipe_other[rfd]-1; if(ph)*ph=(void*)(intptr_t)(0x1000+mate); if(iosb){((uint64_t*)iosb)[0]=0;((uint64_t*)iosb)[1]=1;}
        if(getenv("PELOAD_TRACE_FS")){ char b[64]; int k=snprintf(b,sizeof b,"  [NtOpen pipe-mate] srv=%d -> %d\n",rfd,mate); (void)!write(2,b,k);} return 0; } } }
    // anon_pipe: device-root open ("\Device\NamedPipe\") -> a sentinel used only as RootDirectory.
    if(!root && nt_name_is_pipedev(name)){ if(ph)*ph=PIPE_DEV_HANDLE; if(iosb){((uint64_t*)iosb)[0]=0;((uint64_t*)iosb)[1]=1;}
        if(getenv("PELOAD_TRACE_FS")) ws("  [NtOpen NamedPipe device]\n"); return 0; }
    if(pipe_name_is(name)){ void*ph2=pipe_client_open(name);
        if(ph2==(void*)-1){ if(iosb)((uint64_t*)iosb)[0]=0xC0000034; return (int)0xC0000034; }
        if(ph)*ph=ph2; if(iosb){((uint64_t*)iosb)[0]=0;((uint64_t*)iosb)[1]=1;} return 0; }
    if(getenv("PELOAD_TRACE_NET") && (strstr(name,"Device")||strstr(name,"Afd")||strstr(name,"afd"))){ ws("  [NtCreate-dev] "); ws(name); ws("\n"); }
    if(afd_is_device_name(name)){ void*d=afd_make_device(); if(!d){ if(iosb)((uint64_t*)iosb)[0]=0xC0000017; return (int)0xC0000017; }
        if(ph)*ph=d; if(iosb){((uint64_t*)iosb)[0]=0;((uint64_t*)iosb)[1]=1;}
        if(getenv("PELOAD_TRACE_NET")) ws("  [AFD] opened \\Device\\Afd poller\n"); return 0; }
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
    (void)share;
    if(getenv("PELOAD_TRACE_FS")){ uint8_t*OA=oa; uint8_t*uni=OA?*(uint8_t**)(OA+16):0; char nm[256]={0};
        if(uni){ uint16_t ln=*(uint16_t*)uni; uint16_t*bw=*(uint16_t**)(uni+8); int n=ln/2,j=0; for(int i=0;i<n&&j<255;i++){char c=(char)(bw[i]<128?bw[i]:'?'); nm[j++]=c;} nm[j]=0; }
        char b[320]; int k=snprintf(b,sizeof b,"  [NtOpenFile entry] root=%p name='%s'\n",OA?*(void**)(OA+8):0,nm); (void)!write(2,b,k); }
    return nt_open_common(ph,acc,oa,iosb,1 /*FILE_OPEN*/,opts);
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
// ---- BCrypt asymmetric signature verify (forge library + custom-cache signing) ----
// The engine imports a public key and verifies a signature over its forge library and
// over custom cache data. Stubbed, ImportKeyPair returned 0 but never set *phKey (null
// key) -> the engine refused the content ("Forge library unavailable", "Refused custom
// cache data"). Provide a key handle and report the signature as valid (user's own game).
static long MSABI i_BCryptImportKeyPair(void*hAlg,void*hImport,const uint16_t*blob,void**phKey,uint8_t*in,uint32_t cb,uint32_t fl){
    (void)hAlg;(void)hImport;(void)blob;(void)in;(void)cb;(void)fl; BAlg*k=malloc(sizeof *k); if(!k) return 0xC0000017; k->magic=BMAGIC; k->alg=BALG_SHA256; if(phKey)*phKey=k; return 0; }
static long MSABI i_BCryptImportKey(void*hAlg,void*hImport,const uint16_t*blob,void**phKey,uint8_t*obj,uint32_t cbobj,uint8_t*in,uint32_t cb,uint32_t fl){
    (void)hAlg;(void)hImport;(void)blob;(void)obj;(void)cbobj;(void)in;(void)cb;(void)fl; BAlg*k=malloc(sizeof *k); if(!k) return 0xC0000017; k->magic=BMAGIC; if(phKey)*phKey=k; return 0; }
static long MSABI i_BCryptVerifySignature(void*hKey,void*pad,uint8_t*hash,uint32_t cbhash,uint8_t*sig,uint32_t cbsig,uint32_t fl){
    (void)hKey;(void)pad;(void)hash;(void)cbhash;(void)sig;(void)cbsig;(void)fl; return 0; } // STATUS_SUCCESS: signature valid
static long MSABI i_BCryptDestroyKey(void*hKey){ if(hKey)free(hKey); return 0; }
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
static int  MSABI i_MoveFileExW(const uint16_t*from,const uint16_t*to,uint32_t fl){ (void)fl; char a[2048],b[2048]; char A[2048],B[2048]; win2unix(from,A,sizeof A); if(!to){ int r=unlink(A); if(getenv("PELOAD_TRACE_FS")){ws("  [MoveFileEx del] ");ws(A);ws(r?" FAIL\n":" ok\n");} return r==0; } win2unix(to,B,sizeof B); (void)a;(void)b; int r=rename(A,B);
    if(getenv("PELOAD_TRACE_FS")){ws("  [MoveFileEx] ");ws(A);ws(" => ");ws(B);ws(r?" FAIL\n":" ok\n");}
    // DEBUG: for local-server advertisements, confirm the dest exists after rename + snapshot it
    if(getenv("PELOAD_CAP_ADV") && strstr(B,"local-servers") && strstr(B,".json")){
        struct stat st; int ex=(stat(B,&st)==0); char m[2200]; int k=snprintf(m,sizeof m,"  [adv] rename r=%d dest-exists=%d size=%lld %s\n",r,ex,ex?(long long)st.st_size:-1,B); (void)!write(2,m,k);
        if(ex){ char cap[2300]; snprintf(cap,sizeof cap,"/tmp/reclaimer-adv-%d.json",getpid()); int in=open(B,O_RDONLY),out=open(cap,O_WRONLY|O_CREAT|O_TRUNC,0644); if(in>=0&&out>=0){ char bf[8192]; ssize_t n; while((n=read(in,bf,sizeof bf))>0)(void)!write(out,bf,n);} if(in>=0)close(in); if(out>=0)close(out); } }
    if(r)g_lasterr=win_err(); return r==0; }
static int  MSABI i_DeleteFileW(const uint16_t*w){ char p[2048]; win2unix(w,p,sizeof p);
    // PELOAD_KEEP_ADV: keep local-server advertisement files alive so a concurrently
    // launched client has a stable discovery window (the host otherwise writes then
    // removes <uuid>.json almost immediately). Opt-in; off for the canonical sim path.
    if(getenv("PELOAD_KEEP_ADV") && strstr(p,"local-servers") && strstr(p,".json")){
        if(getenv("PELOAD_TRACE_FS")){ws("  [keep-adv] skip delete ");ws(p);ws("\n");} return 1; }
    return unlink(p)==0 || 1; }
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
static void patch_install(void); // fwd (PELOAD_PATCH guest-code overwrites)
static void hook_install(void); // fwd (PELOAD_HOOK trace detours)
static void skinguard_install(uint8_t*halo3base); // fwd (PELOAD_SKINGUARD null-key guard)
static void vtguard_install(uint8_t*halo3base); // fwd (PELOAD_VTGUARD null-vtable release guard)
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
    // Reclaimer's font cache builds only the base "font_package.bin" (x1), never the
    // scaled "font_package_x2.bin"/"font_package_x3.bin" the engine loads at high res
    // -> text_font_loaded:false. Fall back to the base font so text renders.
    if(!strncmp(b,"font_package_x",14) && !strcasecmp(p+strlen(p)-4,".bin") && access(p,R_OK)!=0){
        char base[2048]; int dl=(int)(b-p); snprintf(base,sizeof base,"%.*sfont_package.bin",dl,p);
        if(access(base,R_OK)==0){ snprintf(p,cap,"%s",base); ws("  [font-subst] font_package_xN.bin -> font_package.bin\n"); }
        return; }
    const char *ext=b+L-4; if(strcasecmp(ext,".ttf")&&strcasecmp(ext,".otf")&&strcasecmp(ext,".ttc")) return;
    if(access(p,R_OK)==0) return;                 // real file present -> keep
    char cand[2048]; snprintf(cand,sizeof cand,"%s/ReclaimerFonts/%s",g_drivec,b);
    if(access(cand,R_OK)!=0) snprintf(cand,sizeof cand,"%s/ReclaimerFonts/bahnschrift.ttf",g_drivec); // generic fallback
    if(access(cand,R_OK)==0) snprintf(p,cap,"%s",cand);
}
// ---- Minimal IOCP, for the async (mio) I/O driver the Forge-library builder uses
// to read OTHER maps. Without it the builder's "wake I/O driver"
// (PostQueuedCompletionStatus) fails -> "Other maps' Forge objects unavailable" ->
// forge library not built -> empty map catalog. We implement a real completion
// queue: overlapped ReadFile on an IOCP-associated handle does the read and posts
// a packet; GetQueuedCompletionStatus dequeues it; PostQueuedCompletionStatus wakes.
typedef struct { uint64_t bytes; uint64_t key; void* ov; } IocpPkt;
typedef struct { uint32_t magic; pthread_mutex_t m; pthread_cond_t c; IocpPkt q[2048]; int head,tail,count; } Iocp;
#define IOCP_MAGIC 0x50434f49u   // 'IOCP'
static int is_iocp(intptr_t h){ if(h<0x100000) return 0; Iocp*p=(Iocp*)h; return p && p->magic==IOCP_MAGIC; }
// AFD poller device handle (mio's "\Device\Afd\Mio"); carries the IOCP it's bound to.
#define AFD_MAGIC 0x31444641u    // 'AFD1'
typedef struct { uint32_t magic; Iocp* iocp; uint64_t key; } AfdDev;
static int is_afd(intptr_t h){ if(h<0x100000) return 0; AfdDev*p=(AfdDev*)h; return p && p->magic==AFD_MAGIC; }
static Iocp*    g_fdiocp[MAXFD]; static uint64_t g_fdiocpkey[MAXFD];
static void iocp_post(Iocp*p,uint64_t bytes,uint64_t key,void*ov){ pthread_mutex_lock(&p->m);
    if(p->count<2048){ p->q[p->tail].bytes=bytes; p->q[p->tail].key=key; p->q[p->tail].ov=ov; p->tail=(p->tail+1)&2047; p->count++; }
    pthread_cond_signal(&p->c); pthread_mutex_unlock(&p->m); }
static void*  MSABI i_CreateIoCompletionPort(void*fh,void*existing,uint64_t key,uint32_t nthreads){ (void)nthreads;
    Iocp*p; if(is_iocp((intptr_t)existing)) p=(Iocp*)existing;
    else { p=calloc(1,sizeof *p); if(!p) return 0; p->magic=IOCP_MAGIC; pthread_mutex_init(&p->m,0); pthread_cond_init(&p->c,0); }
    intptr_t hv=(intptr_t)fh;
    if(is_afd(hv)){ AfdDev*d=(AfdDev*)hv; d->iocp=p; d->key=key; if(getenv("PELOAD_TRACE_NET")) ws("  [AFD] bound poller to IOCP\n"); return p; }
    if(hv>=0x1000&&hv<0x100000){ int fd=(int)(hv-0x1000); if(fd>=0&&fd<MAXFD){ g_fdiocp[fd]=p; g_fdiocpkey[fd]=key; } }
    return p; }
static int    MSABI i_PostQueuedCompletionStatus(void*port,uint32_t bytes,uint64_t key,void*ov){
    if(!is_iocp((intptr_t)port)){ g_lasterr=6; return 0; } iocp_post((Iocp*)port,bytes,key,ov); return 1; }
static int    MSABI i_GetQueuedCompletionStatus(void*port,uint32_t*pbytes,uint64_t*pkey,void**pov,uint32_t ms){
    if(!is_iocp((intptr_t)port)){ g_lasterr=6; if(pov)*pov=0; return 0; } Iocp*p=(Iocp*)port;
    pthread_mutex_lock(&p->m); struct timespec ts;
    if(ms!=0xFFFFFFFFu){ clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=ms/1000; ts.tv_nsec+=(long)(ms%1000)*1000000L; if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;} }
    while(p->count==0){ int r=(ms==0xFFFFFFFFu)?pthread_cond_wait(&p->c,&p->m):pthread_cond_timedwait(&p->c,&p->m,&ts);
        if(r!=0){ pthread_mutex_unlock(&p->m); g_lasterr=258; if(pov)*pov=0; if(pbytes)*pbytes=0; return 0; } }
    IocpPkt pk=p->q[p->head]; p->head=(p->head+1)&2047; p->count--; pthread_mutex_unlock(&p->m);
    if(pbytes)*pbytes=(uint32_t)pk.bytes; if(pkey)*pkey=pk.key; if(pov)*pov=pk.ov; g_lasterr=0; return 1; }
static int    MSABI i_GetOverlappedResult(void*h,uint8_t*ov,uint32_t*pbytes,int wait){ (void)h;(void)wait;
    if(pbytes)*pbytes= ov? (uint32_t)*(uint64_t*)(ov+8) : 0; return 1; }
// GetQueuedCompletionStatusEx: batch dequeue (tokio/mio's Windows poller uses this).
// OVERLAPPED_ENTRY = {ULONG_PTR key; LPOVERLAPPED ov; ULONG_PTR Internal; DWORD bytes;} (32 bytes).
static int    MSABI i_GetQueuedCompletionStatusEx(void*port,uint8_t*entries,uint32_t count,uint32_t*removed,uint32_t ms,int alertable){
    (void)alertable; if(!is_iocp((intptr_t)port)){ g_lasterr=6; if(removed)*removed=0; return 0; } Iocp*p=(Iocp*)port;
    pthread_mutex_lock(&p->m); struct timespec ts;
    if(ms!=0xFFFFFFFFu){ clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=ms/1000; ts.tv_nsec+=(long)(ms%1000)*1000000L; if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;} }
    while(p->count==0){ int r=(ms==0xFFFFFFFFu)?pthread_cond_wait(&p->c,&p->m):pthread_cond_timedwait(&p->c,&p->m,&ts);
        if(r!=0){ pthread_mutex_unlock(&p->m); g_lasterr=258; if(removed)*removed=0; return 0; } }
    uint32_t got=0; while(got<count && p->count>0){ IocpPkt pk=p->q[p->head]; p->head=(p->head+1)&2047; p->count--;
        uint8_t*e=entries+(size_t)got*32; *(uint64_t*)(e+0)=pk.key; *(uint64_t*)(e+8)=(uint64_t)pk.ov; *(uint64_t*)(e+16)=0; *(uint32_t*)(e+24)=(uint32_t)pk.bytes; got++; }
    pthread_mutex_unlock(&p->m); if(removed)*removed=got; g_lasterr=0; return 1; }
static int iocp_post_fd(int fd,uint64_t bytes,void*ov){ if(fd>=0&&fd<MAXFD&&g_fdiocp[fd]){ iocp_post(g_fdiocp[fd],bytes,g_fdiocpkey[fd],ov); return 1; } return 0; }
// ---- AFD poll bridge: translate mio's \Device\Afd IOCTL_AFD_POLL into real poll() ----
// mio 1.2.3 (sys/windows) learns socket readiness by submitting an AFD_POLL_INFO to the
// AFD device via NtDeviceIoControlFile and waiting for the IOCP completion. We service
// those requests on a dedicated poll() thread over the underlying BSD fds and post the
// completion (with the satisfied AFD event bits) back to the bound IOCP. Without this the
// non-blocking connect() reports WSAEWOULDBLOCK but readiness is never delivered, so every
// reqwest/hyper request fails with "error sending request".
// AFD event bits (mio afd.rs):
#define AFD_RECEIVE 0x001u
#define AFD_RECEIVE_EXPEDITED 0x002u
#define AFD_SEND 0x004u
#define AFD_DISCONNECT 0x008u
#define AFD_ABORT 0x010u
#define AFD_LOCAL_CLOSE 0x020u
#define AFD_CONNECT 0x040u
#define AFD_ACCEPT 0x080u
#define AFD_CONNECT_FAIL 0x100u
#define IOCTL_AFD_POLL 0x00012024u
// AFD_POLL_INFO (x64): Timeout i64@0; NumberOfHandles u32@8; Exclusive u32@12;
//   Handles[0]: Handle(8)@16; Events u32@24; Status i32@28.  => 32 bytes for one handle.
typedef struct { int active; int fd; uint32_t mask; uint8_t* info; uint8_t* iosb; void* ov; Iocp* iocp; uint64_t key; } AfdReq;
#define AFD_MAXREQ 512
static AfdReq g_afdreq[AFD_MAXREQ];
static pthread_mutex_t g_afdm=PTHREAD_MUTEX_INITIALIZER;
static int g_afdwake[2]={-1,-1};
static int g_afdstarted;
static void afd_complete(AfdReq*r,uint32_t evts,int32_t ntstatus){
    if(r->info){ *(uint32_t*)(r->info+8)=1; *(uint32_t*)(r->info+24)=evts; *(int32_t*)(r->info+28)=0; } // NumberOfHandles, Events, per-handle Status
    if(r->iosb){ *(int64_t*)(r->iosb+0)=ntstatus; *(uint64_t*)(r->iosb+8)=32; }                        // IoStatusBlock {Status, Information}
    if(r->iocp) iocp_post(r->iocp,32,r->key,r->ov);
    r->active=0;
}
static void* afd_thread_fn(void* a){ (void)a;
    struct pollfd pfd[AFD_MAXREQ+1]; int map[AFD_MAXREQ+1];
    for(;;){ pthread_mutex_lock(&g_afdm);
        int np=0; pfd[np].fd=g_afdwake[0]; pfd[np].events=POLLIN; pfd[np].revents=0; map[np]=-1; np++;
        for(int i=0;i<AFD_MAXREQ;i++){ AfdReq*r=&g_afdreq[i]; if(!r->active) continue;
            short ev=0; if(r->mask&(AFD_RECEIVE|AFD_RECEIVE_EXPEDITED|AFD_ACCEPT)) ev|=POLLIN; if(r->mask&(AFD_SEND|AFD_CONNECT)) ev|=POLLOUT;
            pfd[np].fd=r->fd; pfd[np].events=ev; pfd[np].revents=0; map[np]=i; np++; if(np>=AFD_MAXREQ+1) break; }
        pthread_mutex_unlock(&g_afdm);
        int pr=poll(pfd,np,-1); if(pr<0){ if(errno==EINTR) continue; break; }
        if(pfd[0].revents&POLLIN){ char tmp[256]; while(read(g_afdwake[0],tmp,sizeof tmp)>0){} }
        pthread_mutex_lock(&g_afdm);
        for(int k=1;k<np;k++){ short re=pfd[k].revents; if(!re) continue; int i=map[k]; if(i<0) continue; AfdReq*r=&g_afdreq[i]; if(!r->active||r->fd!=pfd[k].fd) continue;
            uint32_t out=0;
            if(re&POLLOUT) out|=(r->mask&(AFD_SEND|AFD_CONNECT));
            if(re&POLLIN)  out|=(r->mask&(AFD_RECEIVE|AFD_ACCEPT));
            if(re&POLLPRI) out|=(r->mask&AFD_RECEIVE_EXPEDITED);
            if(re&POLLHUP) out|=(r->mask&(AFD_DISCONNECT|AFD_ABORT));
            if(re&(POLLERR|POLLNVAL)){ int serr=0; socklen_t sl=sizeof serr; getsockopt(r->fd,SOL_SOCKET,SO_ERROR,&serr,&sl);
                out|=(r->mask&(AFD_CONNECT_FAIL|AFD_ABORT)); if(!out) out|=(r->mask&AFD_DISCONNECT); }
            if(!out){ // readiness we weren't asked for; report under whatever the mask covers so mio re-evaluates
                if(re&POLLOUT) out|=(r->mask&AFD_SEND); if(re&POLLIN) out|=(r->mask&AFD_RECEIVE); }
            if(out){ if(getenv("PELOAD_TRACE_NET")){ char m[96]; snprintf(m,sizeof m,"[net] afd ready fd=%d mask=0x%x re=0x%x -> 0x%x\n",r->fd,r->mask,re,out); ws(m);} afd_complete(r,out,0); }
        }
        pthread_mutex_unlock(&g_afdm);
    }
    return 0; }
static void afd_wake(void){ if(g_afdwake[1]>=0){ char b=1; (void)!write(g_afdwake[1],&b,1); } }
static int afd_is_device_name(const char*name){ if(!name) return 0; for(const char*p=name;*p;p++){ if((p[0]=='A'||p[0]=='a')&&(p[1]=='f'||p[1]=='F')&&(p[2]=='d'||p[2]=='D')) return 1; } return 0; }
static void* afd_make_device(void){
    pthread_mutex_lock(&g_afdm);
    if(!g_afdstarted){ if(pipe(g_afdwake)==0){ fcntl(g_afdwake[0],F_SETFL,O_NONBLOCK); fcntl(g_afdwake[1],F_SETFL,O_NONBLOCK);
            pthread_t t; if(pthread_create(&t,0,afd_thread_fn,0)==0){ pthread_detach(t); g_afdstarted=1; } } }
    pthread_mutex_unlock(&g_afdm);
    AfdDev*d=calloc(1,sizeof *d); if(d) d->magic=AFD_MAGIC; return d; }
// NtDeviceIoControlFile: service IOCTL_AFD_POLL; everything else -> STATUS_NOT_SUPPORTED.
static int MSABI i_NtDeviceIoControlFile(void*fh,void*evt,void*apcr,void*apcctx,void*iosb,uint32_t ioctl,
        void*inbuf,uint32_t inlen,void*outbuf,uint32_t outlen){ (void)evt;(void)apcr;(void)inlen;(void)outlen;
    if(getenv("PELOAD_TRACE_NET")){ char m[96]; snprintf(m,sizeof m,"[net] NtDeviceIoControlFile(h=%p,ioctl=0x%x,afd=%d)\n",fh,ioctl,is_afd((intptr_t)fh)); ws(m); }
    if(ioctl==IOCTL_AFD_POLL && is_afd((intptr_t)fh)){ AfdDev*d=(AfdDev*)fh; uint8_t*info=outbuf?outbuf:inbuf; if(!info) return (int)0xC000000D;
        uint32_t nh=*(uint32_t*)(info+8); if(nh<1){ if(iosb){*(int64_t*)iosb=0;*(uint64_t*)((uint8_t*)iosb+8)=0;} return 0; }
        uint64_t sh=*(uint64_t*)(info+16); uint32_t mask=*(uint32_t*)(info+24); int fd=(int)(intptr_t)sh;
        pthread_mutex_lock(&g_afdm); int slot=-1;
        for(int i=0;i<AFD_MAXREQ;i++){ if(g_afdreq[i].active && g_afdreq[i].ov==apcctx){ slot=i; break; } }
        if(slot<0) for(int i=0;i<AFD_MAXREQ;i++){ if(!g_afdreq[i].active){ slot=i; break; } }
        if(slot<0){ pthread_mutex_unlock(&g_afdm); return (int)0xC000009A; /*INSUFFICIENT_RESOURCES*/ }
        AfdReq*r=&g_afdreq[slot]; r->fd=fd; r->mask=mask; r->info=info; r->iosb=iosb; r->ov=apcctx; r->iocp=d->iocp; r->key=d->key; r->active=1;
        pthread_mutex_unlock(&g_afdm); afd_wake();
        if(getenv("PELOAD_TRACE_NET")){ char m[96]; snprintf(m,sizeof m,"[net] afd poll submit fd=%d mask=0x%x slot=%d\n",fd,mask,slot); ws(m);}
        if(iosb){*(int64_t*)iosb=0x103;} return (int)0x00000103; /*STATUS_PENDING*/ }
    return (int)0xC00000BB; /*STATUS_NOT_SUPPORTED*/ }
// NtCancelIoFileEx: mio cancels a pending AFD poll (iosb identifies it). Complete it as cancelled.
static int MSABI i_NtCancelIoFileEx(void*fh,void*iosb_in,void*iosb_out){ (void)fh;
    pthread_mutex_lock(&g_afdm); int found=0;
    for(int i=0;i<AFD_MAXREQ;i++){ AfdReq*r=&g_afdreq[i]; if(r->active && (r->iosb==iosb_in || !iosb_in)){ afd_complete(r,0,(int32_t)0xC0000120/*STATUS_CANCELLED*/); found=1; if(iosb_in) break; } }
    pthread_mutex_unlock(&g_afdm);
    if(iosb_out){ *(int64_t*)iosb_out = found?0:(int64_t)0xC0000225/*NOT_FOUND*/; }
    return found?0:(int)0xC0000225; }
static int MSABI i_SetFileCompletionNotificationModes(void*h,uint8_t flags){ (void)h;(void)flags; return 1; }
// ---- File mapping: the Forge-library builder memory-maps each map (CreateFileMappingW
// + MapViewOfFile) to read its Forge objects. Without these the mapping fails ->
// "Other maps' Forge objects unavailable" -> empty catalog. Back it with mmap().
typedef struct { uint32_t magic; int fd; uint64_t size; int prot; } FileMap;
#define FMAP_MAGIC 0x50414d46u   // 'FMAP'
static void name_log(const char*api,const uint16_t*nm){ if(!getenv("PELOAD_TRACE_NAMED")||!nm||!nm[0]) return;
    char b[160]; int k=snprintf(b,sizeof b,"  [named] %s '",api); for(int i=0;nm[i]&&k<150;i++) b[k++]=(char)(nm[i]<128?nm[i]:'?'); b[k++]='\'';b[k++]='\n'; (void)!write(2,b,k); }
static void*  MSABI i_CreateFileMappingW(void*hFile,void*sec,uint32_t protect,uint32_t maxHigh,uint32_t maxLow,const uint16_t*name){ name_log("CreateFileMapping",name);
    (void)sec;(void)name; intptr_t hv=(intptr_t)hFile; FileMap*fm=calloc(1,sizeof *fm); if(!fm) return 0; fm->magic=FMAP_MAGIC;
    fm->fd=(hv>=0x1000&&hv<0x100000)?(int)(hv-0x1000):-1;
    uint64_t sz=((uint64_t)maxHigh<<32)|maxLow; if(sz==0&&fm->fd>=0){ struct stat st; if(fstat(fm->fd,&st)==0) sz=st.st_size; }
    fm->size=sz; fm->prot=(protect==0x02)?PROT_READ:PROT_READ|PROT_WRITE;
    if(getenv("PELOAD_TRACE_FS")){ char b[64]; int k=snprintf(b,sizeof b,"  [CreateFileMapping fd=%d size=0x%llx]\n",fm->fd,(unsigned long long)sz); (void)!write(2,b,k);} return fm; }
static void*  MSABI i_MapViewOfFile(void*hMap,uint32_t access,uint32_t offHigh,uint32_t offLow,uint64_t bytes){
    FileMap*fm=hMap; if(!fm||fm->magic!=FMAP_MAGIC||fm->fd<0){ g_lasterr=6; return 0; }
    uint64_t off=((uint64_t)offHigh<<32)|offLow; size_t len=bytes?bytes:(size_t)(fm->size>off?fm->size-off:0); if(len==0){ g_lasterr=8; return 0; }
    // FILE_MAP_COPY=0x01, FILE_MAP_WRITE=0x02, FILE_MAP_READ=0x04. READ alone must map
    // PROT_READ (requesting PROT_WRITE on an O_RDONLY fd gives EACCES=err 5).
    int cow=(access&0x01)!=0, wr=(access&0x02)!=0;
    int prot=(wr||cow)?(PROT_READ|PROT_WRITE):PROT_READ;
    int mflags=(wr&&!cow)?MAP_SHARED:MAP_PRIVATE;
    size_t pg=(int)sysconf(_SC_PAGESIZE); uint64_t aoff=off&~(uint64_t)(pg-1); uint64_t delta=off-aoff;
    void*base=mmap(0,len+delta,prot,mflags,fm->fd,(off_t)aoff);
    if(base==MAP_FAILED){ g_lasterr=win_err(); return 0; } g_lasterr=0; return (uint8_t*)base+delta; }
static void*  MSABI i_MapViewOfFileEx(void*hMap,uint32_t access,uint32_t offHigh,uint32_t offLow,uint64_t bytes,void*baseHint){
    (void)baseHint; return i_MapViewOfFile(hMap,access,offHigh,offLow,bytes); }
static int    MSABI i_UnmapViewOfFile(void*p){ (void)p; return 1; } // size unknown here; leak the view (harmless)
// ---- Named pipes (dedicated-server <-> game-worker IPC) bridged to UNIX domain sockets ----
// Windows named pipes (\\.\pipe\NAME) cross arbitrary processes by name. Back each with a UNIX socket at
// /tmp/rnp_<fnv(tail)> where tail is the part after "pipe\\"/"pipe/" (so \\.\pipe\NAME and
// \Device\NamedPipe\NAME map to the SAME socket). The server (CreateNamedPipeW) binds+listens; a client
// (CreateFileW/NtCreateFile on a pipe path) connects; ConnectNamedPipe accepts. Synchronous only.
#define PIPE_MAGIC 0x45504950u  // 'PIPE'
typedef struct { uint32_t magic; int listenfd; int connfd; int is_server; char sp[108]; } PipeObj;
static int pipe_name_is(const char*s){ if(!s) return 0;
    for(const char*p=s;p[0]&&p[1]&&p[2]&&p[3]&&p[4];p++)
        if((p[0]|32)=='p'&&(p[1]|32)=='i'&&(p[2]|32)=='p'&&(p[3]|32)=='e'&&(p[4]=='\\'||p[4]=='/')) return 1;
    return 0; }
static const char* pipe_tail(const char*s){ const char*best=s;
    for(const char*p=s;p[0]&&p[1]&&p[2]&&p[3]&&p[4];p++)
        if((p[0]|32)=='p'&&(p[1]|32)=='i'&&(p[2]|32)=='p'&&(p[3]|32)=='e'&&(p[4]=='\\'||p[4]=='/')) best=p+5;
    return best; }
static void pipe_sockpath(const char*name,char*out,size_t osz){ const char*t=pipe_tail(name);
    uint64_t h=1469598103934665603ULL; for(const char*p=t;*p;p++){ h^=(unsigned char)*p; h*=1099511628211ULL; }
    snprintf(out,osz,"/tmp/rnp_%016llx",(unsigned long long)h); }
static int pipe_is(intptr_t hv){ return hv>0x100000 && !((hv)&3) && ((PipeObj*)hv)->magic==PIPE_MAGIC; }
static void* MSABI i_CreateNamedPipeW(const uint16_t*w,uint32_t openMode,uint32_t pipeMode,uint32_t maxInst,uint32_t outBuf,uint32_t inBuf,uint32_t timeout,void*sec){
    (void)openMode;(void)pipeMode;(void)maxInst;(void)outBuf;(void)inBuf;(void)timeout;(void)sec;
    char nm[1024]; int i=0; for(; w&&w[i]&&i<1023;i++) nm[i]=(char)(w[i]<128?w[i]:'?'); nm[i]=0;
    PipeObj*o=calloc(1,sizeof *o); if(!o) return (void*)-1; o->magic=PIPE_MAGIC; o->listenfd=-1; o->connfd=-1; o->is_server=1;
    pipe_sockpath(nm,o->sp,sizeof o->sp);
    int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0){ free(o); g_lasterr=win_err(); return (void*)-1; }
    struct sockaddr_un un; memset(&un,0,sizeof un); un.sun_family=AF_UNIX; snprintf(un.sun_path,sizeof un.sun_path,"%s",o->sp);
    unlink(o->sp); if(bind(fd,(struct sockaddr*)&un,sizeof un)<0 || listen(fd,8)<0){ close(fd); free(o); g_lasterr=win_err(); return (void*)-1; }
    o->listenfd=fd; if(getenv("PELOAD_TRACE_FS")){ char b[200]; int k=snprintf(b,sizeof b,"  [CreateNamedPipe] %s -> %s\n",nm,o->sp); (void)!write(2,b,k);} g_lasterr=0; return o; }
static void* MSABI i_CreateNamedPipeA(const char*a,uint32_t om,uint32_t pm,uint32_t mi,uint32_t ob,uint32_t ib,uint32_t to,void*sec){
    uint16_t w[1024]; int i=0; for(; a&&a[i]&&i<1023;i++) w[i]=(uint8_t)a[i]; w[i]=0; return i_CreateNamedPipeW(w,om,pm,mi,ob,ib,to,sec); }
static void* pipe_client_open(const char*narrow){
    PipeObj*o=calloc(1,sizeof *o); if(!o) return (void*)-1; o->magic=PIPE_MAGIC; o->listenfd=-1; o->connfd=-1; o->is_server=0;
    pipe_sockpath(narrow,o->sp,sizeof o->sp);
    int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0){ free(o); g_lasterr=win_err(); return (void*)-1; }
    struct sockaddr_un un; memset(&un,0,sizeof un); un.sun_family=AF_UNIX; snprintf(un.sun_path,sizeof un.sun_path,"%s",o->sp);
    int ok=0; for(int t=0;t<100;t++){ if(connect(fd,(struct sockaddr*)&un,sizeof un)==0){ ok=1; break; } struct timespec ts={0,2*1000000L}; nanosleep(&ts,0); }
    if(!ok){ close(fd); free(o); g_lasterr=2 /*ERROR_FILE_NOT_FOUND*/; return (void*)-1; }
    o->connfd=fd; if(getenv("PELOAD_TRACE_FS")){ char b[200]; int k=snprintf(b,sizeof b,"  [pipe-open] %s -> %s\n",narrow,o->sp); (void)!write(2,b,k);} g_lasterr=0; return o; }
static int MSABI i_ConnectNamedPipe(void*h,void*ov){ (void)ov; intptr_t hv=(intptr_t)h; if(!pipe_is(hv)){ g_lasterr=6; return 0; }
    PipeObj*o=(PipeObj*)hv; if(o->connfd>=0) return 1; int c=accept(o->listenfd,0,0); if(c<0){ g_lasterr=win_err(); return 0; } o->connfd=c; return 1; }
static int MSABI i_DisconnectNamedPipe(void*h){ intptr_t hv=(intptr_t)h; if(!pipe_is(hv)) return 0; PipeObj*o=(PipeObj*)hv; if(o->connfd>=0){ close(o->connfd); o->connfd=-1; } return 1; }
static int pipe_rw_fd(PipeObj*o){ if(o->connfd<0 && o->is_server){ int c=accept(o->listenfd,0,0); if(c>=0) o->connfd=c; } return o->connfd; }
static int pipe_write(void*h,const void*buf,uint32_t n,uint32_t*wr){ PipeObj*o=h; int f=pipe_rw_fd(o);
    ssize_t k=(f>=0&&buf)?send(f,buf,n,0):-1; if(wr)*wr=(uint32_t)(k<0?0:k); if(k<0)g_lasterr=win_err(); return k>=0; }
static int pipe_read(void*h,void*buf,uint32_t n,uint32_t*rd){ PipeObj*o=h; int f=pipe_rw_fd(o);
    ssize_t k=(f>=0&&buf)?recv(f,buf,n,0):-1; if(rd)*rd=(uint32_t)(k<0?0:k); if(k<0)g_lasterr=win_err(); return k>0 || (k==0); }
static int pipe_close(void*h){ PipeObj*o=h; if(o->connfd>=0)close(o->connfd); if(o->listenfd>=0){ close(o->listenfd); if(o->is_server)unlink(o->sp);} o->magic=0; free(o); return 1; }
static void*  MSABI i_CreateFileW(const uint16_t *w,uint32_t acc,uint32_t share,void*sec,uint32_t disp,uint32_t fl,void*tmpl){
    (void)share;(void)sec;(void)fl;(void)tmpl; char p[2048]; win2unix(w,p,sizeof p);
    { char nn[1024]; int i=0; for(; w&&w[i]&&i<1023;i++) nn[i]=(char)(w[i]<128?w[i]:'?'); nn[i]=0;
      if(pipe_name_is(nn)) return pipe_client_open(nn); }
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
    if(fd<0){ g_lasterr=win_err(); if(g_lasterr==2) snprintf(t_lastfail,sizeof t_lastfail,"CFW:%s",p[0]?p:"<empty>");
        if(getenv("PELOAD_TRACE_CFWBT") && errno==ENOENT){ ws("  [CFW-ENOENT] "); ws(p); ws("\n    chain:");
            uint64_t *sp=(uint64_t*)__builtin_frame_address(0); int shown=0;
            for(int i=0;i<800 && shown<8;i++){ uint64_t v=sp[i]; if(v>=0x140001000ULL&&v<0x141aae000ULL){ char t[32]; snprintf(t,sizeof t," +0x%llx",(unsigned long long)(v-0x140000000ULL)); ws(t); shown++; } }
            ws("\n"); }
        return (void*)-1; } g_lasterr=0; // INVALID_HANDLE_VALUE
    fd_record(fd,p);
    if(fl&0x04000000) g_fddel[fd<MAXFD?fd:0]=1; // FILE_FLAG_DELETE_ON_CLOSE
    return (void*)(intptr_t)(0x1000+fd);
}
static int    MSABI i_ReadFile(void*h,void*buf,uint32_t n,uint32_t*rd,uint8_t*ov){
    intptr_t hv=(intptr_t)h;
    if(pipe_is(hv)) return pipe_read(h,buf,n,rd);
    if(hv<0x1000){ if(rd)*rd=0; return 0; } int fd=(int)(hv-0x1000);
    long k; if(ov){ uint64_t off=*(uint64_t*)(ov+16); k=pread(fd,buf,n,(off_t)off); } else k=read(fd,buf,n);
    if(rd)*rd=(uint32_t)(k<0?0:k);
    if(ov){ *(uint64_t*)(ov+0)=0; *(uint64_t*)(ov+8)=(uint64_t)(k<0?0:k);  // Internal, InternalHigh
        if(fd>=0&&fd<MAXFD&&g_fdiocp[fd]){ iocp_post(g_fdiocp[fd],(uint64_t)(k<0?0:k),g_fdiocpkey[fd],ov);
            g_lasterr=997/*ERROR_IO_PENDING*/; return 0; } }        // async: completion delivered via IOCP
    return k>=0;
}
static int    MSABI i_SetFilePointerEx(void*h,int64_t dist,int64_t*newp,uint32_t method){
    intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; off_t r=lseek((int)(hv-0x1000),dist, method); if(newp)*newp=r; return r>=0;
}
static int    MSABI i_GetFileSizeEx(void*h,int64_t*sz){ intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; struct stat st; if(fstat((int)(hv-0x1000),&st)) return 0; if(sz)*sz=st.st_size; return 1; }
// CreateFileA: ANSI CreateFile. The Reclaimer forge-library builder reads OTHER
// maps' Forge objects via halo3.dll's kernel32 imports, and first verifies each
// of {CreateFileW,CreateFileA,CloseHandle,GetFileSizeEx,CreateIoCompletionPort,
// ReadFileEx,ReadFile} resolves to the SAME pointer as GetProcAddress(kernel32,name)
// (engine_player_slots_bounds check at exe+0x4b9480). Any missing name got a fresh
// make_stub per call -> mismatch -> "halo3.dll's CreateFileA import is not kernel32's"
// -> forge library not built -> empty map catalog -> guardian/map resolve fails.
// So these must live in resolve_impl's table (stable ptr) AND actually work.
static void*  MSABI i_CreateFileA(const char*a,uint32_t acc,uint32_t share,void*sec,uint32_t disp,uint32_t fl,void*tmpl){
    uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0;
    return i_CreateFileW(w,acc,share,sec,disp,fl,tmpl); }
// ReadFileEx: overlapped read + APC completion routine. Read synchronously at the
// OVERLAPPED.Offset and invoke the completion so the engine's async wait resolves.
static int    MSABI i_ReadFileEx(void*h,void*buf,uint32_t n,uint8_t*ov,void*completion){
    intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; int fd=(int)(hv-0x1000);
    uint64_t off = ov? *(uint64_t*)(ov+16) : 0;                 // OVERLAPPED.Offset (Offset/OffsetHigh union)
    long k = ov? pread(fd,buf,n,(off_t)off) : read(fd,buf,n);
    if(ov){ *(uint64_t*)(ov+0)=0; *(uint64_t*)(ov+8)=(uint64_t)(k<0?0:k); } // Internal=0, InternalHigh=bytes
    if(completion){ void(__attribute__((ms_abi))*cr)(uint32_t,uint32_t,void*)=completion; cr(k<0?5u:0u,(uint32_t)(k<0?0:k),ov); }
    return k>=0; }
static int    MSABI i_CloseHandleReal(void*h){ intptr_t hv=(intptr_t)h;
    if(pipe_is(hv)) return pipe_close(h);
    if(is_sync(hv)) return 1;   // sync object / thread handle: no-op (leaked; a thread handle may still be live)
    if(hv>=0x1000 && hv<0x100000){ int fd=(int)(hv-0x1000);
    if(fd>=0 && fd<MAXFD){ if(g_fddir[fd]){ closedir(g_fddir[fd]); g_fddir[fd]=0; }
        // delete-on-close with Windows last-handle-wins semantics: only remove
        // when no OTHER open handle still references the same path. This lets
        // the client delete scratch sub-dirs (files & dirs) without yanking the
        // top scratch dir out from under a still-open session handle.
        if(g_fddel[fd]&&g_fdpath[fd]){ char *path=g_fdpath[fd]; int other=0;
            for(int i=0;i<MAXFD;i++) if(i!=fd && g_fdpath[i] && !strcmp(g_fdpath[i],path)){ other=1; break; }
            int keepadv = getenv("PELOAD_KEEP_ADV") && strstr(path,"local-servers") && strstr(path,".json");
            if(!other && !keepadv){ if(getenv("PELOAD_TRACE_FS")){ws("  [del-on-close] ");ws(path);ws("\n");} struct stat ds; if(stat(path,&ds)==0){ if(S_ISDIR(ds.st_mode)) rmrf(path); else unlink(path); } } }
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
    // Full length of the (already-absolute) input. Windows semantics: return the
    // number of chars written (excl. NUL) when it fits, or the required size
    // (incl. NUL) when buf is too small so the caller can retry with a bigger buf.
    uint32_t nlen=0; while(name[nlen]) nlen++;
    if(getenv("PELOAD_TRACE_SPAWN")){ char n[512]; int k=0; for(;name[k]&&k<511;k++)n[k]=(char)(name[k]&0xff); n[k]=0; char t[600]; snprintf(t,sizeof t,"  [GetFullPathNameW] %s\n",n); ws(t);}
    if(!buf || len < nlen+1){ g_lasterr=0; return nlen+1; }           // caller must grow buffer
    uint32_t i=0; for(; i<nlen; i++) buf[i]=name[i]; buf[i]=0;
    if(part){ *part=0; for(uint32_t j=0;j<nlen;j++) if(buf[j]=='\\'||buf[j]=='/') *part=&buf[j+1]; } // last component
    g_lasterr=0; return nlen;                                          // chars written (excl NUL)
}
// System/Windows directory queries: resolve_exe (Command::spawn) searches these.
// Return the prefix's Windows dirs so the search is well-formed. C:\Windows[\System32].
static uint32_t MSABI i_GetSystemDirectoryW(uint16_t*buf,uint32_t sz){ const char*s="C:\\Windows\\System32"; uint32_t i=0; for(; s[i]&&i<sz-1;i++) buf[i]=(uint8_t)s[i]; if(i<sz)buf[i]=0; if(getenv("PELOAD_TRACE_SPAWN"))ws("  [GetSystemDirectoryW]\n"); return i; }
static uint32_t MSABI i_GetWindowsDirectoryW(uint16_t*buf,uint32_t sz){ const char*s="C:\\Windows"; uint32_t i=0; for(; s[i]&&i<sz-1;i++) buf[i]=(uint8_t)s[i]; if(i<sz)buf[i]=0; if(getenv("PELOAD_TRACE_SPAWN"))ws("  [GetWindowsDirectoryW]\n"); return i; }
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
    char sn[256]; int j=0; for(; sub&&sub[j]&&j<255;j++) sn[j]=(char)sub[j]; sn[j]=0;
    const char *ans=0;
    if(!strcasecmp(vn,"SteamPath")||!strcasecmp(vn,"InstallPath")||!strcasecmp(vn,"SteamExe"))
        ans="C:\\Program Files\\Steam";
    if(getenv("PELOAD_TRACE_REG")){ char t[420]; snprintf(t,sizeof t,"  [RegGetValue] sub='%s' val='%s' -> %s\n",sn,vn,ans?ans:"ERROR_FILE_NOT_FOUND(2)"); ws(t); }
    if(!ans) return 2; // ERROR_FILE_NOT_FOUND
    if(ty)*ty=1; // REG_SZ
    uint32_t need=((uint32_t)strlen(ans)+1)*2;
    if(!data){ if(cb)*cb=need; return 0; }
    if(cb && *cb<need){ *cb=need; return 234; } // ERROR_MORE_DATA
    int n=wput((uint16_t*)data,ans, cb?*cb:need); if(cb)*cb=n; return 0; // ERROR_SUCCESS
}
static long MSABI i_RegOpenKeyExW(void*hk,const uint16_t*sub,uint32_t o,uint32_t sam,void**out){ (void)hk;(void)o;(void)sam; if(getenv("PELOAD_TRACE_REG")){ char sn[256]; int j=0; for(; sub&&sub[j]&&j<255;j++) sn[j]=(char)sub[j]; sn[j]=0; char t[320]; snprintf(t,sizeof t,"  [RegOpenKeyEx] sub='%s'\n",sn); ws(t); } if(out)*out=(void*)0x4b4559; return 0; }
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
    if(getenv("PELOAD_TRACE_FS")){ ws("  [FindFirst] dir="); ws(dir); ws(" pat="); ws(pat); ws("\n"); }
    DIR*d=opendir(dir); if(!d){ g_lasterr=win_err(); if(getenv("PELOAD_TRACE_FS")){ws("  [FindFirst] opendir FAIL ");ws(dir);ws("\n");} return (void*)-1; }
    Find*f=&g_finds[g_nfinds]; f->d=d; snprintf(f->dir,sizeof f->dir,"%s",dir); snprintf(f->pat,sizeof f->pat,"%s",pat);
    struct dirent*de; while((de=readdir(d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(pat,de->d_name)){ if(getenv("PELOAD_TRACE_FS")){ws("  [FindFirst] -> ");ws(de->d_name);ws("\n");} fill_find(f,de,fd); return (void*)(intptr_t)(0x2000+g_nfinds++); } }
    closedir(d); g_lasterr=2; if(getenv("PELOAD_TRACE_FS")){ws("  [FindFirst] EMPTY ");ws(dir);ws("\n");} return (void*)-1; // ERROR_FILE_NOT_FOUND
}
static int MSABI i_FindNextFileW(void*h,void*fd){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); if(idx<0||idx>=g_nfinds||!g_finds[idx].d) return 0;
    Find*f=&g_finds[idx]; struct dirent*de; while((de=readdir(f->d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(f->pat,de->d_name)){ fill_find(f,de,fd); return 1; } } g_lasterr=18; return 0; } // ERROR_NO_MORE_FILES
static int MSABI i_FindClose(void*h){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); if(idx>=0&&idx<g_nfinds&&g_finds[idx].d){ closedir(g_finds[idx].d); g_finds[idx].d=0; } return 1; }
// ANSI variants: halo3 enumerates its maps/ with FindFirstFileA, so a stub here
// leaves the map catalog EMPTY ("guardian" never resolves). WIN32_FIND_DATAA has
// cFileName (CHAR[260]) at +44 instead of the wide WCHAR.
static void fill_find_a(Find*f,struct dirent*de,void*fd){ if(getenv("PELOAD_TRACE_FS")){ ws("  [findA] dir="); ws(f->dir); ws(" pat="); ws(f->pat); ws(" -> "); ws(de->d_name); ws("\n"); }
    uint8_t*d=fd; memset(d,0,320); char full[1800]; struct stat st;
    snprintf(full,sizeof full,"%s/%s",f->dir,de->d_name);
    uint32_t attr=0x80; if(stat(full,&st)==0){ attr=S_ISDIR(st.st_mode)?0x10:0x80; *(uint32_t*)(d+28)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+32)=(uint32_t)st.st_size; }
    *(uint32_t*)d=attr; char*nm=(char*)(d+44); int i=0; for(; de->d_name[i]&&i<259;i++) nm[i]=de->d_name[i]; nm[i]=0; }
static void* MSABI i_FindFirstFileA(const char*p0,void*fd){ if(g_nfinds>=64) return (void*)-1;
    char p[2048]; { int i=0; for(; p0&&p0[i]&&i<2047;i++) p[i]=(p0[i]=='\\')?'/':p0[i]; p[i]=0; }
    char *slash=strrchr(p,'/'); char dir[1536],pat[260];
    if(slash){ int dl=slash-p; if(dl>1535)dl=1535; memcpy(dir,p,dl); dir[dl]=0; snprintf(pat,sizeof pat,"%s",slash+1); }
    else { snprintf(dir,sizeof dir,"."); snprintf(pat,sizeof pat,"%s",p); }
    DIR*d=opendir(dir); if(!d){ g_lasterr=win_err(); return (void*)-1; }
    Find*f=&g_finds[g_nfinds]; f->d=d; snprintf(f->dir,sizeof f->dir,"%s",dir); snprintf(f->pat,sizeof f->pat,"%s",pat);
    struct dirent*de; while((de=readdir(d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(pat,de->d_name)){ fill_find_a(f,de,fd); return (void*)(intptr_t)(0x2000+g_nfinds++); } }
    closedir(d); g_lasterr=2; return (void*)-1; }
static int MSABI i_FindNextFileA(void*h,void*fd){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); if(idx<0||idx>=g_nfinds||!g_finds[idx].d) return 0;
    Find*f=&g_finds[idx]; struct dirent*de; while((de=readdir(f->d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(f->pat,de->d_name)){ fill_find_a(f,de,fd); return 1; } } g_lasterr=18; return 0; }
static int MSABI i_GetFileInformationByHandle(void*h,void*info){ intptr_t hv=(intptr_t)h; if(getenv("PELOAD_TRACE_FS")) ws("  [GetFileInformationByHandle]\n"); if(hv<0x1000||!info) return 0; struct stat st; if(fstat((int)(hv-0x1000),&st)) return 0;
    // BY_HANDLE_FILE_INFORMATION: +0 attrs, +4 ftCreation, +12 ftLastAccess, +20 ftLastWrite,
    // +28 volSerial, +32 sizeHigh, +36 sizeLow, +40 nLinks, +44/+48 fileIndex.
    // Earlier this memset the timestamps to 0 (= 1601-01-01), so freshness checks (e.g. the
    // local-server advertisement staleness gate in local-join) saw every file as ancient and
    // rejected it. Fill real FILETIMEs from stat (100ns ticks since 1601; +11644473600s epoch).
    uint8_t*d=info; memset(d,0,52); *(uint32_t*)d=S_ISDIR(st.st_mode)?0x10:0x80;
    uint64_t ft_c=(uint64_t)st.st_ctime*10000000ULL+116444736000000000ULL;
    uint64_t ft_a=(uint64_t)st.st_atime*10000000ULL+116444736000000000ULL;
    uint64_t ft_w=(uint64_t)st.st_mtime*10000000ULL+116444736000000000ULL;
    *(uint64_t*)(d+4)=ft_c; *(uint64_t*)(d+12)=ft_a; *(uint64_t*)(d+20)=ft_w;
    *(uint32_t*)(d+28)=0x1337c0de;
    *(uint32_t*)(d+32)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+36)=(uint32_t)st.st_size; *(uint32_t*)(d+40)=1;
    *(uint32_t*)(d+44)=(uint32_t)((uint64_t)st.st_ino>>32); *(uint32_t*)(d+48)=(uint32_t)st.st_ino; return 1; }

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

// ===== Batch stub closures (timing / file IO / window / misc) =====
// TIMING
static uint32_t MSABI i_timeGetTime(void){ return i_GetTickCount(); }
static uint32_t MSABI i_timeBeginPeriod(uint32_t p){ (void)p; return 0; /*TIMERR_NOERROR*/ }
static uint32_t MSABI i_timeEndPeriod(uint32_t p){ (void)p; return 0; }
static void* MSABI i_CreateWaitableTimerExW(void*sec,const uint16_t*nm,uint32_t fl,uint32_t acc){ (void)sec;(void)nm;(void)acc;
    SyncObj*s=sync_new(K_EVENT); if(s){ s->manual=(fl&1)?1:0; s->signaled=0; s->is_timer=1; } return s; } // CREATE_WAITABLE_TIMER_MANUAL_RESET=1
static void* MSABI i_CreateWaitableTimerW(void*sec,int manual,const uint16_t*nm){ (void)sec;(void)nm; SyncObj*s=sync_new(K_EVENT); if(s){ s->manual=manual?1:0; s->signaled=0; s->is_timer=1; } return s; }
static int MSABI i_SetWaitableTimer(void*h,const int64_t*due,int32_t period,void*cr,void*arg,int resume){ (void)cr;(void)arg;(void)resume;
    SyncObj*s=h; if(!is_sync((intptr_t)h)) return 1; pthread_mutex_lock(&s->m);
    struct timeval now; gettimeofday(&now,0); uint64_t nus=(uint64_t)now.tv_sec*1000000+now.tv_usec;
    int64_t dt=due?*due:0; uint64_t delay_us;
    if(dt<0) delay_us=(uint64_t)(-dt)/10;                               // relative, 100ns units -> us
    else if(dt>0){ uint64_t ft=(uint64_t)dt; uint64_t u100=ft>116444736000000000ULL?ft-116444736000000000ULL:0; uint64_t abs_us=u100/10; delay_us=abs_us>nus?abs_us-nus:0; } // absolute FILETIME
    else delay_us=0;
    s->is_timer=1; s->timer_armed=1; s->timer_due_us=nus+delay_us; s->timer_period_ms=period; s->signaled=0;
    pthread_cond_broadcast(&s->c); pthread_mutex_unlock(&s->m); return 1; }
static int MSABI i_CancelWaitableTimer(void*h){ SyncObj*s=h; if(is_sync((intptr_t)h)){ pthread_mutex_lock(&s->m); s->timer_armed=0; pthread_cond_broadcast(&s->c); pthread_mutex_unlock(&s->m); } return 1; }
// FILE IO (ANSI -> wide routing + 32-bit pointer wrapper + times)
static int MSABI i_DeleteFileA(const char*a){ uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0; return i_DeleteFileW(w); }
static uint32_t MSABI i_GetFileAttributesA(const char*a){ uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0; return i_GetFileAttributesW(w); }
static int MSABI i_GetFileAttributesExA(const char*a,int lvl,void*info){ uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0; return i_GetFileAttributesExW(w,lvl,info); }
static void* MSABI i_LoadLibraryExA(const char*a,void*h,uint32_t fl){ uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0; return i_LoadLibraryExW(w,h,fl); }
static uint32_t MSABI i_SetFilePointer(void*h,int32_t dist,int32_t*high,uint32_t method){
    int64_t d = high? (((int64_t)(*high)<<32)|(uint32_t)dist) : (int64_t)dist; int64_t np=0;
    if(!i_SetFilePointerEx(h,d,&np,method)){ g_lasterr=win_err(); return 0xFFFFFFFFu; }
    if(high)*high=(int32_t)(np>>32); return (uint32_t)np; }
static int MSABI i_SetFileTime(void*h,const uint64_t*cre,const uint64_t*acc,const uint64_t*wri){ (void)cre;
    intptr_t hv=(intptr_t)h; if(hv<0x1000) return 0; int fd=(int)(hv-0x1000); struct stat st; if(fstat(fd,&st)) return 0;
    struct timeval tv[2]; tv[0].tv_sec=st.st_atime; tv[0].tv_usec=0; tv[1].tv_sec=st.st_mtime; tv[1].tv_usec=0;
    const uint64_t EPOCH=116444736000000000ULL; // 100ns ticks 1601->1970
    if(acc&&*acc){ uint64_t t=(*acc-EPOCH)/10; tv[0].tv_sec=(time_t)(t/1000000); tv[0].tv_usec=(int)(t%1000000); }
    if(wri&&*wri){ uint64_t t=(*wri-EPOCH)/10; tv[1].tv_sec=(time_t)(t/1000000); tv[1].tv_usec=(int)(t%1000000); }
    futimes(fd,tv); return 1; }
// WINDOW / DISPLAY
static void* MSABI i_GetParent(void*h){ (void)h; return 0; }           // our windows are top-level
static void* MSABI i_GetFocus(void){ extern void* g_last_hwnd; return g_last_hwnd?g_last_hwnd:(void*)0x4857; }   // the one logical window
static int MSABI i_RedrawWindow(void*h,const void*rc,void*rgn,uint32_t fl){ (void)h;(void)rc;(void)rgn;(void)fl; return 1; } // we present on our own thread
static int MSABI i_EnumWindows(void*proc,int64_t lparam){ extern void* g_last_hwnd; if(!proc) return 0; int(__attribute__((ms_abi))*cb)(void*,int64_t)=proc; cb(g_last_hwnd?g_last_hwnd:(void*)0x4857,lparam); return 1; }
static int MSABI i_EnumDisplaySettingsW(const uint16_t*dev,uint32_t mode,uint8_t*dm){ (void)dev;(void)mode; if(!dm) return 0;
    // DEVMODEW: fill the display fields the caller reads. dmSize@68, dmFields@72,
    // dmBitsPerPel@104, dmPelsWidth@108, dmPelsHeight@112, dmDisplayFrequency@120.
    *(uint16_t*)(dm+68)=220; *(uint32_t*)(dm+72)=0x40000|0x80000|0x100000|0x400000;
    *(uint32_t*)(dm+104)=32; *(uint32_t*)(dm+108)=WIN_W; *(uint32_t*)(dm+112)=WIN_H; *(uint32_t*)(dm+120)=60; return 1; }
// MISC
static int MSABI i_SetThreadPriority(void*h,int p){ (void)h;(void)p; return 1; }
static int32_t MSABI i_SetThreadDescription(void*h,const uint16_t*desc){ (void)h; char nm[64]; int i=0; for(; desc&&desc[i]&&i<63;i++) nm[i]=(char)desc[i]; nm[i]=0; pthread_setname_np(nm); return 0; }
static void* MSABI i_CreateJobObjectW(void*sec,const uint16_t*nm){ (void)sec;(void)nm; return sync_new(K_EVENT); }
static int MSABI i_AssignProcessToJobObject(void*job,void*proc){ (void)job;(void)proc; return 1; }
static uint32_t MSABI i_GetConsoleProcessList(uint32_t*list,uint32_t count){ if(list&&count>=1) list[0]=(uint32_t)getpid(); return 1; }
static int32_t MSABI i_GetCurrentPackageId(uint32_t*len,uint8_t*buf){ (void)buf; if(len)*len=0; return 15700; /*APPMODEL_ERROR_NO_PACKAGE*/ }
static uint16_t MSABI i_RtlCaptureStackBackTrace(uint32_t skip,uint32_t count,void**bt,uint32_t*hash){
    void*tmp[64]; int n=backtrace(tmp,64); uint32_t got=0;
    for(uint32_t i=skip+1;i<(uint32_t)n && got<count;i++) bt[got++]=tmp[i];
    if(hash){ uintptr_t h=0; for(uint32_t i=0;i<got;i++) h=h*131+(uintptr_t)bt[i]; *hash=(uint32_t)h; }
    return (uint16_t)got; }
static int MSABI i_GetLocaleInfoEx(const uint16_t*loc,uint32_t lctype,uint16_t*data,int cch){ (void)loc;
    static const uint16_t sname[]={'e','n','-','U','S',0}; static const uint16_t one[]={'1',0};
    const uint16_t*out = ((lctype&0xFFFF)==0x5c||(lctype&0xFFFF)==0x59)? sname : one; // SNAME/SISO639LANGNAME else "1"
    int n=0; while(out[n]) n++; n++;
    if(cch==0) return n; if(cch<n) return 0; if(data) for(int i=0;i<n;i++) data[i]=out[i]; return n; }
static uint32_t MSABI i_GetTimeZoneInformation(void*tzi){ if(tzi) memset(tzi,0,172); return 0; /*TIME_ZONE_ID_UNKNOWN, Bias=0 (UTC)*/ }
static int32_t MSABI i_GetErrorInfo(uint32_t res,void**ei){ (void)res; if(ei)*ei=0; return 1; /*S_FALSE: no error object*/ }
static uint32_t MSABI i_waveOutGetNumDevs(void){ return 0; } // no audio-out devices
// VERSION info: report the client's own version so engine queries succeed.
static uint32_t MSABI i_GetFileVersionInfoSizeW(const uint16_t*f,uint32_t*h){ (void)f; if(h)*h=0; return 64; }
static int MSABI i_GetFileVersionInfoW(const uint16_t*f,uint32_t h,uint32_t len,void*data){ (void)f;(void)h;
    if(data&&len>=64){ memset(data,0,64); uint32_t*d=data; d[0]=0xFEEF04BDu; d[1]=0x00010000u;
        d[2]=(0u<<16)|9u; d[3]=(13u<<16)|0u; d[4]=(0u<<16)|9u; d[5]=(13u<<16)|0u; } return 1; }
static int MSABI i_VerQueryValueW(void*blk,const uint16_t*sub,void**out,uint32_t*len){ (void)sub; if(out)*out=blk; if(len)*len=52; return blk?1:0; }
// Deeper-path closures (reached once the worker gets into the menu)
static int MSABI i_CompareFileTime(const uint64_t*a,const uint64_t*b){ uint64_t x=a?*a:0,y=b?*b:0; return x<y?-1:(x>y?1:0); }
static int MSABI i_CreateDirectoryA(const char*a,void*sec){ (void)sec; uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0; char p[2048]; win2unix(w,p,sizeof p); if(mkdir(p,0755)==0||errno==EEXIST) return 1; g_lasterr=win_err(); return 0; }
static int MSABI i_SetInformationJobObject(void*job,int cls,void*info,uint32_t len){ (void)job;(void)cls;(void)info;(void)len; return 1; }
static uint32_t MSABI i_GetWindowThreadProcessId(void*hwnd,uint32_t*pid){ (void)hwnd; if(pid)*pid=(uint32_t)getpid(); return (uint32_t)getpid(); }
static int MSABI i_IsIconic(void*hwnd){ (void)hwnd; return 0; } // never minimized
static int32_t MSABI i_WavesFX_Initialize(void){ return 0; } // audio FX: report ok, no-op
// Fibers: the engine converts its main thread to a fiber. Hand back a non-NULL fiber so the
// conversion succeeds; our single-thread model needs no real fiber switching here.
// ---- Real fibers (ucontext). halo3 streams/decompresses assets via a fiber coroutine:
// a worker fiber does the work and SwitchToFiber()s back to yield; the caller loops until
// it signals done. Stubbing SwitchToFiber as a no-op meant the worker never ran -> the load
// spun forever at 15%. Implement proper cooperative context switching. ----
typedef struct Fiber { uint32_t magic; ucontext_t ctx; void* stack; size_t ssize; void* start; void* param; int is_thread; } Fiber;
#define FIBER_MAGIC 0x46494252u   // 'FIBR'
static __thread Fiber* g_cur_fiber;
// TEB->NtTib.FiberData (TEB+0x20): GetFiberData()/GetCurrentFiber() read this inline via gs.
static inline uint64_t fiber_teb(void){ uint64_t t; __asm__ volatile("movq %%gs:0x30, %0":"=r"(t)); return t; }
static void fiber_set_data(void* p){ uint64_t t=fiber_teb(); if(t>0x1000) *(void**)(t+0x20)=p; }
static void fiber_entry(void){ Fiber* f=g_cur_fiber; if(f) fiber_set_data(f->param);
    if(f&&f->start){ ((void(__attribute__((ms_abi))*)(void*))f->start)(f->param); }
    // A fiber routine must not fall off the end (Windows exits the thread). Park it.
    for(;;){ struct timespec ts={1,0}; nanosleep(&ts,0); } }
static void* MSABI i_ConvertThreadToFiber(void*param){ Fiber* f=g_cur_fiber; if(f&&f->magic==FIBER_MAGIC) return f;
    f=calloc(1,sizeof *f); if(!f) return 0; f->magic=FIBER_MAGIC; f->is_thread=1; f->param=param; getcontext(&f->ctx); g_cur_fiber=f; fiber_set_data(param); return f; }
static void* MSABI i_ConvertThreadToFiberEx(void*param,uint32_t fl){ (void)fl; return i_ConvertThreadToFiber(param); }
static void* MSABI i_CreateFiberEx(size_t commit,size_t reserve,uint32_t fl,void*start,void*param){ (void)commit;(void)fl;
    size_t ss=reserve?reserve:(2u<<20); if(ss<(1u<<20)) ss=1u<<20;   // >=1MB like Windows fiber default
    ss=(ss+0xffff)&~(size_t)0xffff;
    Fiber* f=calloc(1,sizeof *f); if(!f) return 0; f->magic=FIBER_MAGIC; f->start=start; f->param=param; f->ssize=ss;
    // page-aligned stack with a PROT_NONE guard page below it (faults loudly on overflow).
    uint8_t* m=mmap(0,ss+0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(m==MAP_FAILED){ free(f); return 0; } mprotect(m,0x4000,PROT_NONE); f->stack=m; f->ssize=ss+0x4000;
    getcontext(&f->ctx); f->ctx.uc_stack.ss_sp=m+0x4000; f->ctx.uc_stack.ss_size=ss; f->ctx.uc_link=0;
    makecontext(&f->ctx,fiber_entry,0);
    if(getenv("PELOAD_TRACE_FIBER")){ char b[96]; int k=snprintf(b,sizeof b,"[fiber] Create f=%p start=%p param=%p t=%x\n",(void*)f,start,param,pthread_mach_thread_np(pthread_self())); (void)!write(2,b,k); }
    return f; }
static void* MSABI i_CreateFiber(size_t ss,void*start,void*param){ return i_CreateFiberEx(ss,ss,0,start,param); }
static void MSABI i_SwitchToFiber(void*fiber){ Fiber* t=fiber;
    if(getenv("PELOAD_TRACE_FIBER")){ static int sc; if(sc<120){ sc++; char b[120]; int k=snprintf(b,sizeof b,"[fiber] Switch from=%p to=%p valid=%d to.param=%p t=%x\n",(void*)g_cur_fiber,fiber,(t&&t->magic==FIBER_MAGIC),t&&t->magic==FIBER_MAGIC?t->param:0,pthread_mach_thread_np(pthread_self())); (void)!write(2,b,k);} }
    if(!t||t->magic!=FIBER_MAGIC) return; Fiber* cur=g_cur_fiber; if(cur==t) return;
    g_cur_fiber=t; fiber_set_data(t->param); if(cur) swapcontext(&cur->ctx,&t->ctx); else setcontext(&t->ctx); }
static int MSABI i_ConvertFiberToThread(void){ Fiber* f=g_cur_fiber; if(f&&f->magic==FIBER_MAGIC&&f->is_thread){ g_cur_fiber=0; free(f); } return 1; }
static void MSABI i_DeleteFiber(void*fiber){ Fiber* f=fiber; if(f&&f->magic==FIBER_MAGIC&&f!=g_cur_fiber){ f->magic=0; if(f->stack&&!f->is_thread)munmap(f->stack,f->ssize); free(f); } }
static void* MSABI i_GetCurrentFiber(void){ return g_cur_fiber; }
static void* MSABI i_GetFiberData(void){ return g_cur_fiber? g_cur_fiber->param : 0; }
static int MSABI i_IsThreadAFiber(void){ return g_cur_fiber?1:0; }
static void* MSABI i_GetWindow(void*h,uint32_t cmd){ (void)cmd; return h; } // no related windows; return self
// MessageBoxW: the engine shows error/assert dialogs here. We can't pop a dialog, but the TEXT
// is the actual error — LOG it (don't swallow it) and return IDOK(1) so startup continues.
static int MSABI i_MessageBoxW(void*h,const uint16_t*text,const uint16_t*cap,uint32_t type){ (void)h;(void)type;
    char t[1024],c[256]; int i=0; for(; text&&text[i]&&i<1023;i++) t[i]=(char)(text[i]<128?text[i]:'?'); t[i]=0;
    int j=0; for(; cap&&cap[j]&&j<255;j++) c[j]=(char)(cap[j]<128?cap[j]:'?'); c[j]=0;
    ws("  [MessageBoxW] <"); ws(c[0]?c:"(no title)"); ws("> "); ws(t[0]?t:"(no text)"); ws("\n"); return 1; }
// Named dispatch: returns a real impl for a known name, else NULL.
static void *resolve_impl(const char *fn){
    struct { const char *n; void *f; } T[] = {
        {"CompareFileTime",(void*)i_CompareFileTime},
        {"CreateDirectoryA",(void*)i_CreateDirectoryA},
        {"SetInformationJobObject",(void*)i_SetInformationJobObject},
        {"GetWindowThreadProcessId",(void*)i_GetWindowThreadProcessId},
        {"IsIconic",(void*)i_IsIconic},
        {"WavesFX_Initialize",(void*)i_WavesFX_Initialize},
        {"ConvertThreadToFiber",(void*)i_ConvertThreadToFiber},
        {"ConvertThreadToFiberEx",(void*)i_ConvertThreadToFiberEx},
        {"ConvertFiberToThread",(void*)i_ConvertFiberToThread},
        {"CreateFiber",(void*)i_CreateFiber},
        {"CreateFiberEx",(void*)i_CreateFiberEx},
        {"SwitchToFiber",(void*)i_SwitchToFiber},
        {"DeleteFiber",(void*)i_DeleteFiber},
        {"GetCurrentFiber",(void*)i_GetCurrentFiber},
        {"GetFiberData",(void*)i_GetFiberData},
        {"IsThreadAFiber",(void*)i_IsThreadAFiber},
        {"GetWindow",(void*)i_GetWindow},
        {"MessageBoxW",(void*)i_MessageBoxW},
        {"timeGetTime",(void*)i_timeGetTime},
        {"timeBeginPeriod",(void*)i_timeBeginPeriod},
        {"timeEndPeriod",(void*)i_timeEndPeriod},
        {"CreateWaitableTimerExW",(void*)i_CreateWaitableTimerExW},
        {"CreateWaitableTimerW",(void*)i_CreateWaitableTimerW},
        {"SetWaitableTimer",(void*)i_SetWaitableTimer},
        {"CancelWaitableTimer",(void*)i_CancelWaitableTimer},
        {"DeleteFileA",(void*)i_DeleteFileA},
        {"GetFileAttributesA",(void*)i_GetFileAttributesA},
        {"GetFileAttributesExA",(void*)i_GetFileAttributesExA},
        {"LoadLibraryExA",(void*)i_LoadLibraryExA},
        {"SetFilePointer",(void*)i_SetFilePointer},
        {"SetFileTime",(void*)i_SetFileTime},
        {"GetParent",(void*)i_GetParent},
        {"GetFocus",(void*)i_GetFocus},
        {"RedrawWindow",(void*)i_RedrawWindow},
        {"EnumWindows",(void*)i_EnumWindows},
        {"EnumDisplaySettingsW",(void*)i_EnumDisplaySettingsW},
        {"SetThreadPriority",(void*)i_SetThreadPriority},
        {"SetThreadDescription",(void*)i_SetThreadDescription},
        {"CreateJobObjectW",(void*)i_CreateJobObjectW},
        {"AssignProcessToJobObject",(void*)i_AssignProcessToJobObject},
        {"GetConsoleProcessList",(void*)i_GetConsoleProcessList},
        {"GetCurrentPackageId",(void*)i_GetCurrentPackageId},
        {"RtlCaptureStackBackTrace",(void*)i_RtlCaptureStackBackTrace},
        {"GetLocaleInfoEx",(void*)i_GetLocaleInfoEx},
        {"GetTimeZoneInformation",(void*)i_GetTimeZoneInformation},
        {"GetErrorInfo",(void*)i_GetErrorInfo},
        {"waveOutGetNumDevs",(void*)i_waveOutGetNumDevs},
        {"GetFileVersionInfoSizeW",(void*)i_GetFileVersionInfoSizeW},
        {"GetFileVersionInfoW",(void*)i_GetFileVersionInfoW},
        {"VerQueryValueW",(void*)i_VerQueryValueW},
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
        {"ReadConsoleW",(void*)i_ReadConsoleW},
        {"ReadConsoleA",(void*)i_ReadConsoleA},
        {"GetFileType",(void*)i_GetFileType},
        {"CloseHandle",(void*)i_CloseHandleReal},
        {"CreateFileW",(void*)i_CreateFileW},
        {"CreateFileA",(void*)i_CreateFileA},
        {"CreateNamedPipeW",(void*)i_CreateNamedPipeW},
        {"CreateNamedPipeA",(void*)i_CreateNamedPipeA},
        {"ConnectNamedPipe",(void*)i_ConnectNamedPipe},
        {"DisconnectNamedPipe",(void*)i_DisconnectNamedPipe},
        {"ReadFileEx",(void*)i_ReadFileEx},
        {"CreateIoCompletionPort",(void*)i_CreateIoCompletionPort},
        {"PostQueuedCompletionStatus",(void*)i_PostQueuedCompletionStatus},
        {"GetQueuedCompletionStatus",(void*)i_GetQueuedCompletionStatus},
        {"GetQueuedCompletionStatusEx",(void*)i_GetQueuedCompletionStatusEx},
        {"SetFileCompletionNotificationModes",(void*)i_SetFileCompletionNotificationModes},
        {"GetOverlappedResult",(void*)i_GetOverlappedResult},
        {"CreateFileMappingW",(void*)i_CreateFileMappingW},
        {"CreateFileMappingA",(void*)i_CreateFileMappingW},
        {"MapViewOfFile",(void*)i_MapViewOfFile},
        {"MapViewOfFileEx",(void*)i_MapViewOfFileEx},
        {"UnmapViewOfFile",(void*)i_UnmapViewOfFile},
        {"GetFileAttributesW",(void*)i_GetFileAttributesW},
        {"GetFileAttributesExW",(void*)i_GetFileAttributesExW},
        {"ReadFile",(void*)i_ReadFile},
        {"SetFilePointerEx",(void*)i_SetFilePointerEx},
        {"GetFileSizeEx",(void*)i_GetFileSizeEx},
        {"GetFullPathNameW",(void*)i_GetFullPathNameW},
        {"GetSystemDirectoryW",(void*)i_GetSystemDirectoryW},
        {"GetWindowsDirectoryW",(void*)i_GetWindowsDirectoryW},
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
        {"FindFirstFileA",(void*)i_FindFirstFileA},
        {"FindNextFileA",(void*)i_FindNextFileA},
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
        {"D3DReflect",(void*)i_D3DReflect},
        {"D3DGetInputSignatureBlob",(void*)i_D3DGetInputSignatureBlob},
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
        {"CorExitProcess",(void*)i_ExitProcess},
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
        {"GetEnvironmentStrings",(void*)i_GetEnvironmentStrings},
        {"GetEnvironmentStringsA",(void*)i_GetEnvironmentStrings},
        {"FreeEnvironmentStringsA",(void*)i_FreeEnvironmentStringsA},
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
        {"BCryptImportKeyPair",(void*)i_BCryptImportKeyPair},
        {"BCryptImportKey",(void*)i_BCryptImportKey},
        {"BCryptVerifySignature",(void*)i_BCryptVerifySignature},
        {"BCryptDestroyKey",(void*)i_BCryptDestroyKey},
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
        {"MsgWaitForMultipleObjectsEx",(void*)i_MsgWaitForMultipleObjectsEx},
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
        {"CreateSolidBrush",(void*)i_CreateSolidBrush},
        {"CreatePen",(void*)i_CreatePen},
        {"GdiAlphaBlend",(void*)i_GdiAlphaBlend},
        {"AlphaBlend",(void*)i_GdiAlphaBlend},
        {"FillRect",(void*)i_FillRect},
        {"SetTextColor",(void*)i_SetTextColor},
        {"SetBkMode",(void*)i_SetBkMode},
        {"SetBkColor",(void*)i_SetBkColor},
        {"CreateFontW",(void*)i_CreateFontW},
        {"CreateFontIndirectW",(void*)i_CreateFontIndirectW},
        {"GdiFlush",(void*)i_GdiFlush},
        {"GetTextExtentPoint32W",(void*)i_GetTextExtentPoint32W},
        {"TextOutW",(void*)i_TextOutW},
        {"ExtTextOutW",(void*)i_ExtTextOutW},
        {"DrawTextW",(void*)i_DrawTextW},
        {"GetTextMetricsW",(void*)i_GetTextMetricsW},
        {"GetTextFaceW",(void*)i_GetTextFaceW},
        {"SetTextCharacterExtra",(void*)i_SetTextCharacterExtra},
        {"StretchDIBits",(void*)i_StretchDIBits},
        {"SetStretchBltMode",(void*)i_SetStretchBltMode},
        {"SetBrushOrgEx",(void*)i_SetBrushOrgEx},
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
        {"WaitForSingleObjectEx",(void*)i_WaitForSingleObjectEx},
        {"WaitForMultipleObjectsEx",(void*)i_WaitForMultipleObjectsEx},
        {"Sleep",(void*)i_Sleep},
        {"SleepEx",(void*)i_SleepEx},
        {"WaitForMultipleObjects",(void*)i_WaitForMultipleObjects},
        {"CreateThread",(void*)i_CreateThread},
        {"_beginthreadex",(void*)i_beginthreadex},
        {"_endthreadex",(void*)i_endthreadex},
        {"_beginthread",(void*)i_beginthread},
        {"_endthread",(void*)i_endthread},
        {"GetThreadId",(void*)i_GetThreadId},
        {"GetExitCodeThread",(void*)i_GetExitCodeThread},
        {"OpenProcess",(void*)i_OpenProcess},
        {"GetProcessId",(void*)i_GetProcessId},
        {"SwitchToThread",(void*)i_SwitchToThread},
        {"CompareStringOrdinal",(void*)i_CompareStringOrdinal},
        {"GetTempPathW",(void*)i_GetTempPathW},
        {"GetTempPath2W",(void*)i_GetTempPath2W},
        {"GetFileInformationByHandleEx",(void*)i_GetFileInformationByHandleEx},
        {"GetFinalPathNameByHandleW",(void*)i_GetFinalPathNameByHandleW},
        {"RemoveDirectoryW",(void*)i_RemoveDirectoryW},
        {"SetFileInformationByHandle",(void*)i_SetFileInformationByHandle},
        {"NtCreateFile",(void*)i_NtCreateFile},
        {"NtDeviceIoControlFile",(void*)i_NtDeviceIoControlFile},
        {"NtCancelIoFileEx",(void*)i_NtCancelIoFileEx},
        {"NtOpenFile",(void*)i_NtOpenFile},
        {"NtCreateNamedPipeFile",(void*)i_NtCreateNamedPipeFile},
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
    // Static TLS for this DLL (halo3, CRT): give it its own index + per-thread block
    // BEFORE its DllMain/TLS callbacks run, so its __declspec(thread) globals resolve
    // to its own data instead of aliasing the exe's TLS block (index 0).
    if(opt->Dir[DIR_TLS].VA) register_tls((void*)(base+opt->Dir[DIR_TLS].VA), base, -1);
    if(strcasestr(name,"halo3")){ waves_prefill(base); patch_install(); hook_install(); skinguard_install(base); vtguard_install(base); } // patches can target halo3 now it's mapped
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
// PELOAD_BP=<exe-rva-hex>: one-shot software breakpoints. Each hit logs "[bp]"
// then restores the original byte and resumes (so the probed code runs normally).
static uint64_t g_bp_addr[8]; static uint8_t g_bp_orig[8]; static int g_bp_n; static uint64_t g_bp_cnt[8]; static int g_bp_rearm=-1;
// Resolve a loaded guest module base by (case-insensitive) name substring.
static uint8_t* mod_base(const char*want){ for(int i=0;i<g_nmods;i++){ const char*n=g_mods[i].name; for(const char*p=n;*p;p++){ if(!strncasecmp(p,want,strlen(want))) return g_mods[i].base; } } return 0; }
// PELOAD_PATCH=<mod>+<rva>:<hexbytes>[,...]  — overwrite guest code (e.g. NOP out
// a re-entrant callback). mod is "exe" or a module-name substring ("halo3").
static void patch_install(void){ const char*s=getenv("PELOAD_PATCH"); if(!s) return;
    char buf[256]; strncpy(buf,s,sizeof buf-1); buf[sizeof buf-1]=0;
    for(char*tok=strtok(buf,",");tok;tok=strtok(0,",")){
        char*plus=strchr(tok,'+'),*colon=strchr(tok,':'); if(!plus||!colon) continue; *plus=0;*colon=0;
        uint8_t*base = !strcasecmp(tok,"exe")? g_base : mod_base(tok); if(!base) continue;
        uint64_t rva=strtoull(plus+1,0,16); uint8_t*p=base+rva;
        uintptr_t pg=(uintptr_t)p & ~0xFFFull; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
        const char*h=colon+1; int n=0; while(h[0]&&h[1]){ char hh[3]={h[0],h[1],0}; p[n++]=(uint8_t)strtoul(hh,0,16); h+=2; }
        sys_icache_invalidate(p,n);
        char m[96]; int k=snprintf(m,sizeof m,"  [patch] %s+0x%llx <- %d bytes\n",tok,(unsigned long long)rva,n); (void)!write(2,m,k); } }
// Built-in workaround for the Reclaimer frontend's UI-composition re-entrancy in
// this environment. ui_composition (exe+0x550e00) runs a once-per-second cache
// cleanup at exe+0x553ad9 that borrows the app-state RefCell; on our first-call
// path the outer borrow (exe+0x551043) isn't released first, so the cleanup's
// borrow panics ("RefCell already borrowed", std thread/local.rs:757). The cleanup
// only drops 8 cached sub-objects (null this early) and then releases the borrow,
// so we neutralise it: NOP the borrow-conflict panic branch (0x553add) and the
// drop-dispatch branch (0x553af1) so it takes the no-drop path that just sets the
// RefCell flag back to free. This lets the engine reach its native swap-chain
// present path and the client-screen draw. Disable with PELOAD_NO_REENTRANCY_FIX.
static void apply_patch(uint8_t*base,uint64_t rva,const uint8_t*bytes,int n){
    uint8_t*p=base+rva; uintptr_t pg=(uintptr_t)p & ~0xFFFull; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
    memcpy(p,bytes,n); sys_icache_invalidate(p,n); }
static void reentrancy_fix(void){ if(getenv("PELOAD_NO_REENTRANCY_FIX")||!g_base) return;
    static const uint8_t nop6[6]={0x90,0x90,0x90,0x90,0x90,0x90}, nop2[2]={0x90,0x90};
    apply_patch(g_base,0x553add,nop6,6);   // jne 0x553dff (borrow-conflict panic) -> nops
    apply_patch(g_base,0x553af1,nop2,2);   // jne 0x553af7 (drop dispatch) -> nops (take no-drop/release path)
    ws("  [reentrancy-fix] ui_composition cache-cleanup borrow neutralised (exe+0x553add,0x553af1)\n"); }
// ---- JMP-trampoline trace hooks (int3 doesn't work under Rosetta; code detours do) ----
// PELOAD_HOOK=<mod>+<rva>:<origlen>:<id>[,...]  installs a detour at rva that logs
// id + the guest rbx/rsi/rdi/rbp (so we can watch RefCell borrow sites etc.), runs
// the <origlen> relocated original bytes, and jumps back. origlen must be >=13 and
// cover whole, non-rip-relative instructions.
static uint64_t g_hookseq;
// base points at the saved-regs block: [0]=r15 [1]=r14 [2]=r13 [3]=r12 [4]=r11
// [5]=r10 [6]=r9 [7]=r8 [8]=rdi [9]=rsi [10]=rbp [11]=rbx [12]=rdx [13]=rcx [14]=rax
// Try to render up to n bytes at p as ASCII (for Rust &str args: ptr=rcx,len=rdx).
static void hook_str(char*out,int outsz,uint64_t p,uint64_t len){ int o=0;
    if(p<0x10000||len==0||len>512){ out[0]=0; return; }
    for(uint64_t i=0;i<len&&o<outsz-1;i++){ unsigned char c=((unsigned char*)p)[i]; out[o++]=(c>=0x20&&c<0x7f)?c:'.'; } out[o]=0; }
static int hook_safe(uint64_t p){ return p>=0x10000 && p<0x800000000000ULL; }
static void hook_log(uint64_t id,uint64_t* r){
    uint64_t seq=__sync_fetch_and_add(&g_hookseq,1);
    uint64_t rax=r[14],r15=r[0],r14=r[1],r13=r[2],r12=r[3],rbx=r[11],rcx=r[13],rdx=r[12],r8=r[7],rsi=r[9],rdi=r[8];
    uint64_t gsp=(uint64_t)(r+15); uint64_t ret=*(uint64_t*)gsp;
    // Catalog-dump mode (id==0xca7a): interpret rax/rdx/r15 as the map-catalog resolve loop
    // state and print each entry's id (+0x1b8) and data ptr (+0x10) so we can see why a
    // map's entry+0x10 is null. Guesses base from rax/rdx; dumps up to 64 entries.
    if(id==0xca7a){
        char h[256]; int k=snprintf(h,sizeof h,"[CAT seq=%llu] thr=0x%x rax=0x%llx rdx(count?)=0x%llx r15(query)=0x%llx query_id=0x%x\n",
            (unsigned long long)seq,pthread_mach_thread_np(pthread_self()),(unsigned long long)rax,(unsigned long long)rdx,
            (unsigned long long)r15,hook_safe(r15)?*(uint32_t*)(r15+0xd8):0xffffffff); (void)!write(2,h,k);
        uint64_t cnt=rdx; if(cnt>64)cnt=64;
        for(uint64_t i=0;i<cnt;i++){ uint64_t e=rax-0xe0*(i+1); if(!hook_safe(e))break;
            uint32_t eid=hook_safe(e+0x1b8)?*(uint32_t*)(e+0x1b8):0; uint64_t dptr=*(uint64_t*)(e+0x10); uint64_t d8=*(uint64_t*)(e+8);
            char nm[48]; nm[0]=0; if(hook_safe(dptr)) hook_str(nm,sizeof nm,dptr, d8<64?d8:32);
            char b[160]; int m=snprintf(b,sizeof b,"  [cat#%llu] e=0x%llx id=0x%x [+8]=0x%llx [+0x10]=0x%llx \"%s\"\n",
                (unsigned long long)i,(unsigned long long)e,eid,(unsigned long long)d8,(unsigned long long)dptr,nm); (void)!write(2,b,m); }
        return; }
    // dispatch-callback mode (id==0xd15): at a callback dispatcher's entry, log the index
    // (ecx) and the caller return addr (halo3-relative) so we can see what a spin loop runs.
    if(id==0xd15){ static int dc; if(dc<120){ dc++;
        uint32_t idx=(uint32_t)rcx; uint64_t tbl=0x18076a0b0ULL; uint64_t fnp=tbl+(uint64_t)idx*40+0x18;
        uint64_t fn=hook_safe(fnp)?*(uint64_t*)fnp:0; uint64_t argp=tbl+(uint64_t)idx*40+0x20; uint64_t arg=hook_safe(argp)?*(uint64_t*)argp:0;
        char m[160]; int k=snprintf(m,sizeof m,"[DISP seq=%llu] thr=0x%x idx=%u fn=halo3+0x%llx arg=0x%llx\n",
            (unsigned long long)seq,pthread_mach_thread_np(pthread_self()),idx,
            (unsigned long long)(fn>=0x180000000ULL&&fn<0x187000000ULL?fn-0x180000000ULL:fn),(unsigned long long)arg); (void)!write(2,m,k);} return; }
    // decompressor-trace: id==0x5ff at sub_1805ff2e0 entry (rcx=idx, rdx=a1 input ptr, r8=a2 cnt ptr);
    //                     id==0x5fd at sub_1805fd838 entry (rcx=idx; log in/out ptrs + count).
    if(id==0x5ff){ static int c; if(c<80){ c++; char m[200]; int k=snprintf(m,sizeof m,
        "[ff2e0 #%llu t%x] idx=%llx a1in=%llx a2cnt=%llx *a2=%d f985c=%x\n",(unsigned long long)seq,pthread_mach_thread_np(pthread_self()),
        (unsigned long long)rcx,(unsigned long long)rdx,(unsigned long long)r8,hook_safe(r8)?*(int*)r8:-1,
        hook_safe(rcx+0x985c)?*(uint8_t*)(rcx+0x985c):0xff); (void)!write(2,m,k);} return; }
    if(id==0x5fd){ static int c; if(c<80){ c++; uint64_t ix=rcx;
        uint64_t f1800=hook_safe(ix+0x1800)?*(uint64_t*)(ix+0x1800):~0ull, f1810=hook_safe(ix+0x1810)?*(uint64_t*)(ix+0x1810):~0ull;
        int f1818=hook_safe(ix+0x1818)?*(int*)(ix+0x1818):-999; uint8_t f985c=hook_safe(ix+0x985c)?*(uint8_t*)(ix+0x985c):0xff;
        char m[220]; int k=snprintf(m,sizeof m,"[fd838 #%llu t%x] idx=%llx f1800=%llx f1810=%llx f1818=%d f985c=%x%s\n",(unsigned long long)seq,
        pthread_mach_thread_np(pthread_self()),(unsigned long long)ix,(unsigned long long)f1800,(unsigned long long)f1810,f1818,f985c,
        f1800==0?"  <== f1800 NULL!":""); (void)!write(2,m,k);} return; }
    // host-setup arg dump (id==0x6c): sub_14058da30(idx=rcx, idx1=rdx, a2=R8). Gate 1 proceeds iff
    // a2->field_6c==1. Dump field_6c (+0x6c) + nearby bytes to see which early gate bails on a real join.
    if(id==0x6c){ uint64_t a2=r8;
        char m[200]; int k=snprintf(m,sizeof m,"[HS seq=%llu a2=0x%llx] field_6c=0x%x  [+0x60..0x74]=",
            (unsigned long long)seq,(unsigned long long)a2, hook_safe(a2+0x6c)?*(uint8_t*)(a2+0x6c):0xee);
        (void)!write(2,m,k);
        for(int o=0x60;o<=0x74 && hook_safe(a2+o);o++){ char b2[8]; int j=snprintf(b2,sizeof b2,"%02x ",*(uint8_t*)(a2+o)); (void)!write(2,b2,j); }
        (void)!write(2,"\n",1); return; }
    // global-dump mode (id==0x2263c): print the resolve/menu-skip mode flags.
    if(id==0x2263c){ uint64_t b=0x140000000ULL;
        char m[128]; int k=snprintf(m,sizeof m,"[GLOB seq=%llu] [0x2263c60]=0x%x [0x2263c64]=0x%x\n",
            (unsigned long long)seq,*(uint32_t*)(b+0x2263c60),*(uint8_t*)(b+0x2263c64)); (void)!write(2,m,k); return; }
    // catalog-getter mode (id==0x6d7e): at getter entry rcx=&global; dump the global
    // struct fields ([+0]/[+8]/[+0x10]/[+0x20]) so we can see if the registry is empty.
    if(id==0x6d7e){ uint64_t g=rcx;
        char m[300]; int k=snprintf(m,sizeof m,"[GET seq=%llu] thr=0x%x &g=0x%llx",
            (unsigned long long)seq,pthread_mach_thread_np(pthread_self()),(unsigned long long)g);
        (void)!write(2,m,k);
        if(hook_safe(g)){ char b[300];
            uint64_t f0=*(uint64_t*)(g+0), f8=*(uint64_t*)(g+8), f10=*(uint64_t*)(g+0x10), f20h=*(uint64_t*)(g+0x20);
            int j=snprintf(b,sizeof b," [+0]=0x%llx [+8]=0x%llx [+0x10]=0x%llx [+0x20]=0x%llx\n",
                (unsigned long long)f0,(unsigned long long)f8,(unsigned long long)f10,(unsigned long long)f20h);
            (void)!write(2,b,j);
            // if [+8] is a ptr, peek the sub-structure it feeds to 0x6e0fb0
            if(hook_safe(f8)){ char c[200]; int jj=snprintf(c,sizeof c,"   sub[+8]: [0]=0x%llx [8]=0x%llx [0x10]=0x%llx [0x18]=0x%llx\n",
                (unsigned long long)*(uint64_t*)(f8+0),(unsigned long long)*(uint64_t*)(f8+8),
                (unsigned long long)*(uint64_t*)(f8+0x10),(unsigned long long)*(uint64_t*)(f8+0x18)); (void)!write(2,c,jj); }
        } else { (void)!write(2,"\n",1); }
        return; }
    // path-dump mode (id==0x9590): at engine_startup catalog-init site, dump the path
    // {ptr@rbp+0x590, len@rbp+0x598} that gates the catalog build (null -> skip).
    if(id==0x9590){ uint64_t rbp=r[10];
        char hd[96]; int k=snprintf(hd,sizeof hd,"[PATH590 seq=%llu] thr=0x%x rbp=0x%llx",
            (unsigned long long)seq,pthread_mach_thread_np(pthread_self()),(unsigned long long)rbp); (void)!write(2,hd,k);
        if(hook_safe(rbp)){ uint64_t pp=*(uint64_t*)(rbp+0x590), ln=*(uint64_t*)(rbp+0x598);
            char s[160]; hook_str(s,sizeof s,pp, ln<160?ln:159);
            char b[220]; int j=snprintf(b,sizeof b," ptr=0x%llx len=0x%llx \"%s\"\n",(unsigned long long)pp,(unsigned long long)ln,s); (void)!write(2,b,j);
        } else (void)!write(2,"\n",1);
        return; }
    // catalog-state mode (id==0xca70): dump the Reclaimer map-catalog globals so we can
    // see if the BTreeMap (0x2264448..0x2264498) is populated and whether the cache
    // Option (0x22648e0) is Some, at the moment the guardian callback runs.
    if(id==0xca70){ uint64_t b=0x140000000ULL;
        char m[400]; int k=snprintf(m,sizeof m,
          "[CATSTATE seq=%llu] once@0x2264458=0x%llx len@0x2264460=0x%llx dataptr@0x2264468=0x%llx cap@0x2264470=0x%llx cache@0x22648e0=0x%llx once2@0x2264950=0x%x\n",
          (unsigned long long)seq,
          (unsigned long long)*(uint64_t*)(b+0x2264458),
          (unsigned long long)*(uint64_t*)(b+0x2264460),
          (unsigned long long)*(uint64_t*)(b+0x2264468),
          (unsigned long long)*(uint64_t*)(b+0x2264470),
          (unsigned long long)*(uint64_t*)(b+0x22648e0),
          *(uint32_t*)(b+0x2264950));
        (void)!write(2,m,k); return; }
    // object-dump mode (id==0x0b1): rcx=object ptr, edx=stage. Dump [rcx+0..0x120] as
    // qwords; for any slot that points to ASCII, show the string (hunt for the map name).
    if(id==0x0b1){ uint64_t o=rcx;
        char hd[160]; int k=snprintf(hd,sizeof hd,"[OBJ seq=%llu] thr=0x%x rcx=0x%llx edx(stage)=0x%x\n",
            (unsigned long long)seq,pthread_mach_thread_np(pthread_self()),(unsigned long long)o,(uint32_t)rdx);
        (void)!write(2,hd,k);
        if(hook_safe(o)) for(int off=0;off<=0x120;off+=8){ uint64_t v=*(uint64_t*)(o+off);
            char nm[64]; nm[0]=0;
            if(hook_safe(v)){ // try v as a str ptr; guess a short len
                uint64_t ln=(off+8<=0x120)?*(uint64_t*)(o+off+8):0;
                if(ln>0&&ln<48) hook_str(nm,sizeof nm,v,ln);
                else { int g=0; for(;g<24;g++){unsigned char c=((unsigned char*)v)[g]; if(c<0x20||c>=0x7f)break;} if(g>=3){ hook_str(nm,sizeof nm,v,g);} }
            }
            char b[140]; int m=snprintf(b,sizeof b,"   [+0x%03x]=0x%llx %s%s%s\n",off,(unsigned long long)v,
                nm[0]?"\"":"",nm,nm[0]?"\"":""); (void)!write(2,b,m);
        }
        return; }
    // rbp-args mode (id==0x90b0): dump the fmt arg slots the refusal builder fills
    // ([rbp+0x160/0x168] and [rbp+0x170/0x178] = (ptr,len) pairs) + their strings.
    if(id==0x90b0){ uint64_t rbp=r[10];
        char b[400]; int k=snprintf(b,sizeof b,"[ARGS seq=%llu] rbp=0x%llx\n",(unsigned long long)seq,(unsigned long long)rbp); (void)!write(2,b,k);
        for(int o=0x150;o<=0x1a0;o+=0x10){ if(!hook_safe(rbp)) break; uint64_t pp=*(uint64_t*)(rbp+o), ln=*(uint64_t*)(rbp+o+8);
            char s[80]; hook_str(s,sizeof s,pp, ln<80?ln:79);
            char m[160]; int j=snprintf(m,sizeof m,"   [rbp+0x%x]=0x%llx len=0x%llx \"%s\"\n",o,(unsigned long long)pp,(unsigned long long)ln,s); (void)!write(2,m,j); }
        return; }
    // Stack-walk mode (id==0x57acc): scan the guest stack for exe/halo3 return addrs,
    // printing module+offset. Gives the REAL call chain regardless of the (unreliable)
    // symbol map — use at a confirmed point to find callers/callees dynamically.
    if(id==0x57acc){
        char hd[96]; int k=snprintf(hd,sizeof hd,"[STACK seq=%llu] thr=0x%x gsp=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx r12=0x%llx\n",
            (unsigned long long)seq,pthread_mach_thread_np(pthread_self()),(unsigned long long)gsp,
            (unsigned long long)rcx,(unsigned long long)rdx,(unsigned long long)rbx,(unsigned long long)r12); (void)!write(2,hd,k);
        uint64_t*sp=(uint64_t*)gsp; int printed=0;
        for(int i=0;i<700 && printed<40;i++){ uint64_t v=sp[i];
            if(v>=0x140000000ULL&&v<0x142000000ULL){ char b[64]; int m=snprintf(b,sizeof b,"   exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,b,m); printed++; }
            else if(v>=0x180000000ULL&&v<0x187000000ULL){ char b[64]; int m=snprintf(b,sizeof b,"   halo3+0x%llx\n",(unsigned long long)(v-0x180000000ULL)); (void)!write(2,b,m); printed++; } }
        return; }
    char s1[80]; hook_str(s1,sizeof s1,rcx,rdx); // rcx=ptr,rdx=len (common Rust &str arg)
    char b[420]; int k=snprintf(b,sizeof b,"[hk#%llu id=%llu] thr=0x%x rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx rbx=0x%llx rsi=0x%llx rdi=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx [gsp]=exe+0x%llx str(rcx,rdx)=\"%s\"\n",
        (unsigned long long)seq,(unsigned long long)id,pthread_mach_thread_np(pthread_self()),
        (unsigned long long)rax,(unsigned long long)rcx,(unsigned long long)rdx,(unsigned long long)r8,(unsigned long long)rbx,
        (unsigned long long)rsi,(unsigned long long)rdi,(unsigned long long)r12,(unsigned long long)r13,(unsigned long long)r14,(unsigned long long)r15,
        (unsigned long long)(ret>=0x140000000&&ret<0x142000000?ret-0x140000000:ret),s1); (void)!write(2,b,k); }
static uint8_t* g_hookpool; static size_t g_hookoff;
static void hook_one(uint8_t*base,uint64_t rva,int origlen,uint64_t id){
    if(!g_hookpool){ g_hookpool=mmap(0,1<<20,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(g_hookpool==MAP_FAILED){g_hookpool=0;return;} }
    uint8_t*site=base+rva; uint8_t*t=g_hookpool+g_hookoff;
    uint8_t*w=t;
    // save 15 GPRs (not rsp): rax rcx rdx rbx rbp rsi rdi r8..r15
    static const uint8_t pushes[]={0x50,0x51,0x52,0x53,0x55,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    for(unsigned i=0;i<sizeof pushes;i++) *w++=pushes[i];
    // after pushes, from rsp: r15@0 r14@8 r13@10 r12@18 r11@20 r10@28 r9@30 r8@38 rdi@40 rsi@48 rbp@50 rbx@58 rdx@60 rcx@68 rax@70
    // mov rbp,rsp ; and rsp,-16
    *w++=0x48;*w++=0x89;*w++=0xE5;                      // mov rbp,rsp
    *w++=0x48;*w++=0x83;*w++=0xE4;*w++=0xF0;            // and rsp,-16
    // args (SysV): rdi=id, rsi=rbp (saved-regs base)
    *w++=0x48;*w++=0xBF; *(uint64_t*)w=id; w+=8;        // mov rdi,id
    *w++=0x48;*w++=0x89;*w++=0xEE;                      // mov rsi,rbp
    *w++=0x4C;*w++=0x8B;*w++=0x45;*w++=0x50;            // mov r8,[rbp+0x50]
    *w++=0x48;*w++=0xB8; *(uint64_t*)w=(uint64_t)hook_log; w+=8; // mov rax,&hook_log
    *w++=0xFF;*w++=0xD0;                                 // call rax
    *w++=0x48;*w++=0x89;*w++=0xEC;                      // mov rsp,rbp
    static const uint8_t pops[]={0x41,0x5F,0x41,0x5E,0x41,0x5D,0x41,0x5C,0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5F,0x5E,0x5D,0x5B,0x5A,0x59,0x58};
    for(unsigned i=0;i<sizeof pops;i++) *w++=pops[i];
    // relocated original bytes
    for(int i=0;i<origlen;i++) *w++=site[i];
    // jmp back to site+origlen (abs)
    *w++=0x49;*w++=0xBB; *(uint64_t*)w=(uint64_t)(site+origlen); w+=8; // mov r11,ret
    *w++=0x41;*w++=0xFF;*w++=0xE3;                       // jmp r11
    sys_icache_invalidate(t,w-t); g_hookoff+=(w-t+15)&~15ull;
    // patch the site: mov r11,trampoline ; jmp r11 (13 bytes) + nop pad
    uintptr_t pg=(uintptr_t)site & ~0xFFFull; mprotect((void*)pg,0x3000,PROT_READ|PROT_WRITE|PROT_EXEC);
    uint8_t*p=site; *p++=0x49;*p++=0xBB; *(uint64_t*)p=(uint64_t)t; p+=8; *p++=0x41;*p++=0xFF;*p++=0xE3;
    for(int i=13;i<origlen;i++) site[i]=0x90;
    sys_icache_invalidate(site,origlen);
    char m[96]; int k=snprintf(m,sizeof m,"  [hook %llu] @%p origlen=%d tramp=%p\n",(unsigned long long)id,(void*)site,origlen,(void*)t); (void)!write(2,m,k);
}
static void hook_install(void){ const char*s=getenv("PELOAD_HOOK"); if(!s) return;
    char buf[512]; strncpy(buf,s,sizeof buf-1); buf[sizeof buf-1]=0;
    for(char*tok=strtok(buf,",");tok;tok=strtok(0,",")){
        char*plus=strchr(tok,'+'),*c1=strchr(tok,':'); if(!plus||!c1) continue; char*c2=strchr(c1+1,':'); if(!c2) continue;
        *plus=0;*c1=0;*c2=0; uint8_t*base = !strcasecmp(tok,"exe")? g_base : mod_base(tok); if(!base) continue;
        hook_one(base,strtoull(plus+1,0,16),(int)strtoull(c1+1,0,10),strtoull(c2+1,0,16)); } }
// ---- Armor/skin-registry null-key guard (PELOAD_SKINGUARD) --------------------
// During round-start the engine registers each player's custom armor material into an
// RB-tree (halo3+0x2aacf0). It derives the tree key by calling a provider's field_20
// virtual, which hands sub_1802ac24c a "key object" (arg3 = r8). Under our stubbed GPU
// the provider is a valid object but its backing render resource is absent, so field_20
// yields a NULL key object -> sub_1802ac24c dereferences it (`mov rax,[r14]`) and faults.
// The engine's own contract already tolerates an empty registration (the parent checks
// `if(result)`), so we install an entry detour on sub_1802ac24c: when the key object is
// null, return 0 (skip the hash/insert) instead of crashing. Non-null keys run normally.
// origlen=14 covers `mov rax,rsp` + 7 pushes (no rip-relative / jcc -> safe to relocate).
static void skinguard_one(uint8_t*base,uint64_t rva,int origlen){
    if(!g_hookpool){ g_hookpool=mmap(0,1<<20,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(g_hookpool==MAP_FAILED){g_hookpool=0;return;} }
    uint8_t*site=base+rva; uint8_t*t=g_hookpool+g_hookoff; uint8_t*w=t;
    *w++=0x4D;*w++=0x85;*w++=0xC0;                       // test r8,r8
    uint8_t*jz=w; *w++=0x74;*w++=0x00;                   // jz ret0 (patched below)
    for(int i=0;i<origlen;i++) *w++=site[i];             // relocated prologue
    *w++=0x49;*w++=0xBB; *(uint64_t*)w=(uint64_t)(site+origlen); w+=8; *w++=0x41;*w++=0xFF;*w++=0xE3; // jmp site+origlen
    jz[1]=(uint8_t)(w-(jz+2));                           // ret0 target (rel8)
    *w++=0x31;*w++=0xC0;*w++=0xC3;                       // xor eax,eax ; ret
    sys_icache_invalidate(t,w-t); g_hookoff+=(w-t+15)&~15ull;
    uintptr_t pg=(uintptr_t)site & ~0xFFFull; mprotect((void*)pg,0x3000,PROT_READ|PROT_WRITE|PROT_EXEC);
    uint8_t*p=site; *p++=0x49;*p++=0xBB; *(uint64_t*)p=(uint64_t)t; p+=8; *p++=0x41;*p++=0xFF;*p++=0xE3; // mov r11,t; jmp r11
    for(int i=13;i<origlen;i++) site[i]=0x90;
    sys_icache_invalidate(site,origlen);
    char m[96]; int k=snprintf(m,sizeof m,"  [skinguard] @%p tramp=%p (null-key -> return 0)\n",(void*)site,(void*)t); (void)!write(2,m,k);
}
static void skinguard_install(uint8_t*halo3base){ if(!getenv("PELOAD_SKINGUARD")) return; skinguard_one(halo3base,0x2ac24c,14); }
// ---- NULL-vtable release guard (PELOAD_VTGUARD) ------------------------------
// A refcounted-resource sweep (halo3+0x172ad0) releases each live object in a hash table:
// after the strong-refcount hits 1 it loads the object's vtable (`mov rax,[r14]` @0x172bce)
// and calls the drop (`call [rax]`) + dealloc (`call [rax+8]`). Under the stubbed GPU some
// resource ends up with a NULL vtable, so `call [rax]` faults (0xC0000005 read of 0 at RVA
// 0x172BD4, early in Guardian gameplay). Detour the vtable load: if the vtable is NULL, skip
// the whole drop/dealloc (leaks that one malformed stub) and jump to the entry-cleanup at
// 0x172bee; a non-null vtable runs the original release unchanged. origlen=17 ends on the
// instruction boundary at 0x172bdf; the relocated bytes are position-independent
// (mov/call[rax]/or/lock xadd — no rip-relative or jcc).
static void vtguard_one(uint8_t*base,uint64_t rva,int origlen,uint64_t skiprva){
    if(!g_hookpool){ g_hookpool=mmap(0,1<<20,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(g_hookpool==MAP_FAILED){g_hookpool=0;return;} }
    uint8_t*site=base+rva; uint8_t*t=g_hookpool+g_hookoff; uint8_t*w=t;
    for(int i=0;i<3;i++) *w++=site[i];                   // relocated `mov rax,[r14]` (vtable)
    *w++=0x48;*w++=0x85;*w++=0xC0;                        // test rax,rax
    uint8_t*jz=w; *w++=0x74;*w++=0x00;                    // jz skip (patched below)
    for(int i=3;i<origlen;i++) *w++=site[i];              // relocated remainder (to boundary)
    *w++=0x49;*w++=0xBB; *(uint64_t*)w=(uint64_t)(site+origlen); w+=8; *w++=0x41;*w++=0xFF;*w++=0xE3; // jmp site+origlen (resume)
    jz[1]=(uint8_t)(w-(jz+2));                            // skip target (rel8)
    *w++=0x49;*w++=0xBB; *(uint64_t*)w=(uint64_t)(base+skiprva); w+=8; *w++=0x41;*w++=0xFF;*w++=0xE3; // jmp skiprva (skip drop)
    sys_icache_invalidate(t,w-t); g_hookoff+=(w-t+15)&~15ull;
    uintptr_t pg=(uintptr_t)site & ~0xFFFull; mprotect((void*)pg,0x3000,PROT_READ|PROT_WRITE|PROT_EXEC);
    uint8_t*p=site; *p++=0x49;*p++=0xBB; *(uint64_t*)p=(uint64_t)t; p+=8; *p++=0x41;*p++=0xFF;*p++=0xE3; // mov r11,t; jmp r11
    for(int i=13;i<origlen;i++) site[i]=0x90;
    sys_icache_invalidate(site,origlen);
    char m[128]; int k=snprintf(m,sizeof m,"  [vtguard] @%p tramp=%p (null-vtable release -> skip to +0x%llx)\n",(void*)site,(void*)t,(unsigned long long)skiprva); (void)!write(2,m,k);
}
static void vtguard_install(uint8_t*halo3base){ if(!getenv("PELOAD_VTGUARD")) return; vtguard_one(halo3base,0x172bce,17,0x172bee); }
static void bp_install(void){ const char*s=getenv("PELOAD_BP"); if(!s||!g_base) return;
    while(*s && g_bp_n<8){ uint64_t rva=strtoull(s,0,16); if(rva){ uint8_t*p=g_base+rva;
            uintptr_t pg=(uintptr_t)p & ~0xFFFull; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
            g_bp_orig[g_bp_n]=*p; g_bp_addr[g_bp_n]=(uint64_t)p; *p=0xCC; sys_icache_invalidate(p,1); g_bp_n++; }
        const char*c=strchr(s,','); if(!c) break; s=c+1; } }
#include <mach/thread_act.h>
// On SIGUSR1, suspend every thread and print its guest x86 rip + a scan of its stack for
// exe/halo3 return addresses. Rosetta exposes x86_THREAD_STATE64, so this shows exactly where
// each thread is blocked/spinning (unlike `sample`, which shows the ARM/JIT translation).
static void dump_all_guest_stacks(void){
    thread_act_array_t th; mach_msg_type_number_t n=0;
    if(task_threads(mach_task_self(),&th,&n)!=KERN_SUCCESS) return;
    mach_port_t me=mach_thread_self();
    char hdr[64]; int hk=snprintf(hdr,sizeof hdr,"\n==== GUEST THREAD DUMP (%u threads) ====\n",n); (void)!write(2,hdr,hk);
    for(unsigned i=0;i<n;i++){
        if(th[i]==me) continue;
        thread_suspend(th[i]);
        x86_thread_state64_t st; mach_msg_type_number_t sc=x86_THREAD_STATE64_COUNT;
        if(thread_get_state(th[i],x86_THREAD_STATE64,(thread_state_t)&st,&sc)==KERN_SUCCESS){
            uint64_t rip=st.__rip,rsp=st.__rsp;
            const char* mn=(rip>=0x140000000ULL&&rip<0x14c000000ULL)?"exe+0x":(rip>=0x180000000ULL&&rip<0x187000000ULL)?"halo3+0x":"jit:0x";
            uint64_t ro=(rip>=0x180000000ULL&&rip<0x187000000ULL)?rip-0x180000000ULL:(rip>=0x140000000ULL&&rip<0x14c000000ULL)?rip-0x140000000ULL:rip;
            char b[160]; int k=snprintf(b,sizeof b,"-- thr%u rip=%s%llx rsp=0x%llx\n",i,mn,(unsigned long long)ro,(unsigned long long)rsp); (void)!write(2,b,k);
            int shown=0; for(int w=0;w<700 && shown<16;w++){ uint64_t sa=rsp+(uint64_t)w*8; if(!hook_safe(sa)) break; uint64_t v=*(uint64_t*)sa;
                if(v>=0x180000000ULL&&v<0x187000000ULL){ char c[48]; int kk=snprintf(c,sizeof c,"      halo3+0x%llx\n",(unsigned long long)(v-0x180000000ULL)); (void)!write(2,c,kk); shown++; }
                else if(v>=0x140000000ULL&&v<0x14c000000ULL){ char c[48]; int kk=snprintf(c,sizeof c,"      exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,c,kk); shown++; } }
        }
        thread_resume(th[i]);
    }
    (void)!write(2,"==== END DUMP ====\n",19);
    for(unsigned i=0;i<n;i++) mach_port_deallocate(mach_task_self(),th[i]);
    mach_port_deallocate(mach_task_self(),me);
}
static void usr1sig(int s){ (void)s; dump_all_guest_stacks(); }
static void crashsig(int sig, siginfo_t *info, void *ucv){
    ucontext_t *uc=ucv; uint64_t rip = uc? (uint64_t)uc->uc_mcontext->__ss.__rip : 0;
    if(sig==SIGTRAP && uc){
        // Re-arm phase: a prior hit set TF to single-step over the original insn;
        // this trap re-plants the int3 so the breakpoint counts EVERY hit.
        if(g_bp_rearm>=0){ int i=g_bp_rearm; g_bp_rearm=-1; uint8_t*p=(uint8_t*)g_bp_addr[i]; *p=0xCC; sys_icache_invalidate(p,1);
            uc->uc_mcontext->__ss.__rflags &= ~0x100ull; return; }
        uint64_t hit=rip-1;   // int3 already advanced rip
        for(int i=0;i<g_bp_n;i++) if(g_bp_addr[i]==hit){ uint8_t*p=(uint8_t*)hit; *p=g_bp_orig[i]; sys_icache_invalidate(p,1);
            g_bp_cnt[i]++; uc->uc_mcontext->__ss.__rip=hit;   // rewind to re-execute original insn
            uc->uc_mcontext->__ss.__rflags |= 0x100ull; g_bp_rearm=i;   // single-step, then re-arm
            if(g_bp_cnt[i]<=40){ uint64_t rbx=uc->uc_mcontext->__ss.__rbx, rsi=uc->uc_mcontext->__ss.__rsi, rdi=uc->uc_mcontext->__ss.__rdi, r15=uc->uc_mcontext->__ss.__r15;
                uint64_t rax=uc->uc_mcontext->__ss.__rax, r10=uc->uc_mcontext->__ss.__r10, r9=uc->uc_mcontext->__ss.__r9, r14=uc->uc_mcontext->__ss.__r14;
                uint64_t vbx = (rbx>0x1000)? *(uint64_t*)rbx : 0;
                // PELOAD_BP_CORR: also dump the connection-correlation operands (cmp r10,[rax+0x200])
                uint64_t exp200=0; if(getenv("PELOAD_BP_CORR") && (hit-(uint64_t)g_base)==0x5880a5 && rax>0x100000000ull && rax<0x800000000000ull) exp200=*(uint64_t*)(rax+0x200);
                char b[280]; int k;
                if(getenv("PELOAD_BP_CORR")) k=snprintf(b,sizeof b,"[bp] HIT exe+0x%llx #%llu thr=0x%x rax=0x%llx r10(pktID)=0x%llx [rax+0x200](expID)=0x%llx r9=0x%llx r14=0x%llx\n",
                    (unsigned long long)(hit-(uint64_t)g_base),(unsigned long long)g_bp_cnt[i],pthread_mach_thread_np(pthread_self()),
                    (unsigned long long)rax,(unsigned long long)r10,(unsigned long long)exp200,(unsigned long long)r9,(unsigned long long)r14);
                else k=snprintf(b,sizeof b,"[bp] HIT exe+0x%llx #%llu thr=0x%x rbx=0x%llx [rbx]=0x%llx rsi=0x%llx rdi=0x%llx r15=0x%llx\n",
                    (unsigned long long)(hit-(uint64_t)g_base),(unsigned long long)g_bp_cnt[i],pthread_mach_thread_np(pthread_self()),
                    (unsigned long long)rbx,(unsigned long long)vbx,(unsigned long long)rsi,(unsigned long long)rdi,(unsigned long long)r15); (void)!write(2,b,k);} return; } }
    // Decompressor fault dump: on a fault in halo3's bitstream reader, print the fiber ctx
    // fields so we can see the inconsistent state (f1800/f1810 null while f1818>0).
    if((sig==SIGSEGV||sig==SIGBUS) && uc && rip>=0x1805fc000ull && rip<0x180601000ull){
        uint64_t ix = (rip==0x1805fec1aull)? uc->uc_mcontext->__ss.__rdi : uc->uc_mcontext->__ss.__rcx;
        if(ix>0x100000 && ix<0x800000000000ull){
            uint64_t f1800=*(uint64_t*)(ix+0x1800), f1810=*(uint64_t*)(ix+0x1810), f1820=*(uint64_t*)(ix+0x1820);
            int f1818=*(int*)(ix+0x1818); uint8_t f985c=*(uint8_t*)(ix+0x985c); uint64_t f1850=*(uint64_t*)(ix+0x1850);
            char b[256]; int k=snprintf(b,sizeof b,"[DECOMP-FAULT rip=halo3+0x%llx thr=0x%x idx=0x%llx] f1800=0x%llx f1810=0x%llx f1818=%d f1820=0x%llx f985c=0x%x f1850=0x%llx\n",
                (unsigned long long)(rip-0x180000000ull),pthread_mach_thread_np(pthread_self()),(unsigned long long)ix,
                (unsigned long long)f1800,(unsigned long long)f1810,f1818,(unsigned long long)f1820,f985c,(unsigned long long)f1850); (void)!write(2,b,k); } }
    // If this fault is a write/read into a guard page, name the owning block +
    // overflow distance BEFORE handing off to the guest VEH (which may exit).
    if((sig==SIGSEGV||sig==SIGBUS) && info && info->si_addr && heapguard()){
        void*buf=0; uint64_t rsz=0; int64_t over=0;
        if(hg_guardhit(info->si_addr,&buf,&rsz,&over)){
            char gb[160]; int n=snprintf(gb,sizeof gb,
              "  [GUARD-HIT] faultaddr=%p in guard of buf=%p reqsize=%llu overflow=+%lld bytes rip=0x%llx\n",
              info->si_addr,buf,(unsigned long long)rsz,(long long)over,(unsigned long long)rip);
            (void)!write(2,gb,n);
            { Dl_info di; if(dladdr((void*)rip,&di)){ char db[256]; int k=snprintf(db,sizeof db,
                "      fault-rip in %s : %s +0x%llx\n", di.dli_fname?di.dli_fname:"?",
                di.dli_sname?di.dli_sname:"?", (unsigned long long)((uint64_t)rip-(uint64_t)di.dli_saddr));
                (void)!write(2,db,k); } }
            // Scan the stack for the first exe-range (0x140000000..0x142000000)
            // return address = the guest function that invoked the faulting copy.
            if(uc){ uint64_t sp=(uint64_t)uc->uc_mcontext->__ss.__rsp;
                uint64_t r_rcx=uc->uc_mcontext->__ss.__rcx, r_rdx=uc->uc_mcontext->__ss.__rdx,
                         r_r8=uc->uc_mcontext->__ss.__r8, r_rdi=uc->uc_mcontext->__ss.__rdi,
                         r_rsi=uc->uc_mcontext->__ss.__rsi;
                char rb[200]; int k=snprintf(rb,sizeof rb,
                    "      regs rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx r8=0x%llx rsp=0x%llx\n",
                    (unsigned long long)r_rdi,(unsigned long long)r_rsi,(unsigned long long)r_rdx,(unsigned long long)r_rcx,(unsigned long long)r_r8,(unsigned long long)sp); (void)!write(2,rb,k);
                int found=0;
                for(int i=0;i<512 && found<6;i++){ uint64_t v=*(uint64_t*)(sp+i*8);
                    int exe = v>=0x140000000ull && v<0x142000000ull;
                    int h3  = v>=0x180000000ull && v<0x182000000ull;
                    if(exe||h3){ int kk=snprintf(rb,sizeof rb,"      guest-ret[%d] @sp+0x%x = %s+0x%llx\n",
                        found,i*8, exe?"exe":"halo3", (unsigned long long)(v-(exe?0x140000000ull:0x180000000ull)));
                        (void)!write(2,rb,kk); found++; } } } } }
    // PELOAD_TRACE_FAULT: log the REAL faulting rip (the engine's own report
    // prints exe+0x0 whenever rip isn't inside the exe image) + a guest-return
    // scan, so hardware faults in halo3/system/shim code are identifiable.
    if(getenv("PELOAD_TRACE_FAULT") && (sig==SIGSEGV||sig==SIGBUS||sig==SIGILL||sig==SIGFPE) && uc && !g_in_crashsig){
        Dl_info di; const char*fn="?",*mod="?"; uint64_t fo=0;
        if(dladdr((void*)rip,&di)){ if(di.dli_sname)fn=di.dli_sname; if(di.dli_fname)mod=di.dli_fname; if(di.dli_saddr)fo=rip-(uint64_t)di.dli_saddr; }
        const char*reg = rip>=0x140000000ull&&rip<0x142000000ull?"exe":(rip>=0x180000000ull&&rip<0x182000000ull?"halo3":"other");
        char b[320]; int k=snprintf(b,sizeof b,"\n[FAULT] sig=%d thr=0x%x faultaddr=%p rip=0x%llx (%s%s+0x%llx | %s:%s+0x%llx)\n",
            sig,pthread_mach_thread_np(pthread_self()),info?info->si_addr:0,(unsigned long long)rip,reg,
            reg[0]=='e'?" exe+0x":(reg[0]=='h'?" halo3+0x":" "),
            (unsigned long long)(reg[0]=='e'?rip-0x140000000ull:(reg[0]=='h'?rip-0x180000000ull:rip)),mod,fn,(unsigned long long)fo);
        (void)!write(2,b,k);
        uint64_t sp=(uint64_t)uc->uc_mcontext->__ss.__rsp; int found=0;
        for(int i=0;i<256 && found<8;i++){ uint64_t v=*(uint64_t*)(sp+i*8);
            int e=v>=0x140000000ull&&v<0x142000000ull, h=v>=0x180000000ull&&v<0x182000000ull;
            if(e||h){ char r[80]; int kk=snprintf(r,sizeof r,"    ret[%d] %s+0x%llx\n",found,e?"exe":"halo3",(unsigned long long)(v-(e?0x140000000ull:0x180000000ull))); (void)!write(2,r,kk); found++; } } }
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
    { int k=snprintf(b,sizeof b,"    [d3dgl] last phase=%s draw#=%lu\n",g_d3dgl_phase?(const char*)g_d3dgl_phase:"?",g_d3dgl_phase_draw); (void)!write(2,b,k); }
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
// PELOAD_NETTEST=host:port/path (or "1" for the default discovery endpoint): exercise the
// translated non-blocking socket path end to end (connect -> wait writable -> send -> wait
// readable -> recv) and print the byte count. Proves the wrapped HTTP stack really fetches.
static void net_selftest(const char* spec){
    char host[256]="148.251.153.44"; int port=49175; char pq[512]="/v3/discovery";
    if(spec && strcmp(spec,"1")){ const char* p=spec; const char* slash=strchr(p,'/'); const char* colon=strchr(p,':');
        size_t hl = colon? (size_t)(colon-p) : (slash? (size_t)(slash-p) : strlen(p)); if(hl>=sizeof host) hl=sizeof host-1; memcpy(host,p,hl); host[hl]=0;
        if(colon){ port=atoi(colon+1); } if(slash){ snprintf(pq,sizeof pq,"%s",slash); } }
    WSASOCK s=i_ws2_socket(2,1,6); if(s==WSA_INVALID){ ws("[nettest] socket failed\n"); return; }
    uint32_t one=1; i_ws2_ioctlsocket(s,(long)0x8004667e,&one);
    uint8_t sa[16]; sa[0]=2; sa[1]=0; uint16_t bp=htons((uint16_t)port); memcpy(sa+2,&bp,2);
    struct in_addr ia; inet_pton(AF_INET,host,&ia); memcpy(sa+4,&ia,4); memset(sa+8,0,8);
    int rc=i_ws2_connect(s,sa,16);
    if(rc<0 && t_wsa_err==10035){ struct pollfd pf={(int)s,POLLOUT,0}; poll(&pf,1,10000); int se=0; socklen_t sl=sizeof se; getsockopt((int)s,SOL_SOCKET,SO_ERROR,&se,&sl); if(se){ ws("[nettest] connect failed\n"); i_ws2_closesocket(s); return; } }
    char req[1024]; int rl=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",pq,host);
    int sent=0; while(sent<rl){ int k=i_ws2_send(s,req+sent,rl-sent,0); if(k>0){ sent+=k; continue; } if(t_wsa_err==10035){ struct pollfd pf={(int)s,POLLOUT,0}; if(poll(&pf,1,5000)<=0) break; continue; } break; }
    char buf[16384]; int total=0,body=-1; for(;;){ int k=i_ws2_recv(s,buf,sizeof buf,0);
        if(k>0){ if(body<0){ buf[k<(int)sizeof buf?k:(int)sizeof buf-1]=0; char* h=strstr(buf,"\r\n\r\n"); if(h) body=total+(int)(h+4-buf); } total+=k; continue; }
        if(k<0 && t_wsa_err==10035){ struct pollfd pf={(int)s,POLLIN,0}; if(poll(&pf,1,8000)<=0) break; continue; } break; }
    i_ws2_closesocket(s);
    char m[256]; snprintf(m,sizeof m,"[nettest] http://%s:%d%s -> %d bytes total (headers+body); body starts at %d\n",host,port,pq,total,body); ws(m);
}
int main(int argc, char **argv){
    { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=crashsig; sa.sa_flags=SA_SIGINFO;
      sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGABRT,&sa,0);
      if(getenv("PELOAD_BP")) sigaction(SIGTRAP,&sa,0); }  // PELOAD_BP software breakpoints need SIGTRAP caught (else "Trace/BPT trap")
    { struct sigaction su; memset(&su,0,sizeof su); su.sa_handler=usr1sig; sigaction(SIGUSR1,&su,0); }  // SIGUSR1 -> guest thread dump
    { const char* nt=getenv("PELOAD_NETTEST"); if(nt){ net_selftest(nt); } }
    if(getenv("PELOAD_GLTEST")){ gl_selftest(); return 0; }  // direct GL render test; no game loaded
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
    g_glthunk = mmap(0, 128*8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    mprotect(g_glthunk, 128*8192, PROT_READ|PROT_WRITE|PROT_EXEC); g_glth_exec = 0; // permanent RWX (thread-safe; no toggle)

    // ---- TEB/PEB via %gs overlay, set up BEFORE any guest code runs ----
    static uint8_t teb[0x2000] __attribute__((aligned(16)));
    static uint8_t peb[0x1000] __attribute__((aligned(16)));
    *(uint64_t*)(teb+0x30) = (uint64_t)teb;   // NtTib.Self
    *(uint64_t*)(teb+0x60) = (uint64_t)peb;   // PEB
    void **tls_array = g_tls_array;           // the global main-thread TLS array (register_tls fills it)
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
    // The exe takes TLS index 0; DLLs (CRT, halo3) were/are registered via
    // register_tls() in load_guest and get their own indices. All share g_tls_array.
    uint32_t tlsrva = opt->Dir[DIR_TLS].VA;
    if(tlsrva) register_tls((void*)rva(tlsrva), g_base, 0);

    // entry point
    uint64_t entry = (uint64_t)rva(opt->EntryRVA);
    ws("  entry @ "); whex(entry);
    // debug: pause before entry so lldb can attach (set PELOAD_WAIT=seconds).
    // Writes our pid to /tmp/peload.pid for the attacher.
    if(getenv("PELOAD_WAIT") && strstr(g_cmdlineA,"probe-engine-worker")){ char pb[32]; int pn=snprintf(pb,sizeof pb,"%d\n",getpid());
        int fd=open("/tmp/peload.pid",O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){(void)!write(fd,pb,pn); close(fd);}
        ws("  PELOAD_WAIT: pid "); whex(getpid()); sleep(atoi(getenv("PELOAD_WAIT"))); }
    bp_install();   // PELOAD_BP software breakpoints (diagnostic)
    reentrancy_fix(); // built-in: neutralise the ui_composition RefCell re-entrancy
    patch_install(); hook_install(); // PELOAD_PATCH overwrites + PELOAD_HOOK trace detours
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
    // Hold the window open after the worker finishes (the present thread keeps drawing).
    if(getenv("PELOAD_HOLD_WINDOW") && g_present_started){ ws("  [hold-window] main done; keeping window up\n"); for(;;){ struct timespec ts={1,0}; nanosleep(&ts,0); } }
    return 0;
}
