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
// Cached getenv for the shim's own PELOAD_* knobs. The per-draw GL path consulted ~50 PELOAD_* vars per
// draw; each libc getenv() is a linear scan of environ (~1-2 us under Rosetta) -> tens of us per draw at
// ~13k draws/s = most of the frame. PELOAD_* are fixed at process start (the guest's SetEnvironmentVariable
// only touches its own RECLAIMER_*/APPDATA names), so caching them is safe; everything else passes through.
// Keyed by the name POINTER (call sites use string literals = stable addresses), lock-free: value is
// published before the key with release/acquire ordering; a lost race just repeats the real lookup.
static char* peload_cgetenv(const char*name){
    if(!name || strncmp(name,"PELOAD_",7)) return getenv(name);
    enum { N=2048 };
    static const char* keys[N]; static char* vals[N]; static int ready[N];
    uintptr_t h=((uintptr_t)name>>3)*0x9E3779B97F4A7C15ull;
    for(unsigned i=0;i<32;i++){ unsigned s=(unsigned)((h>>40)+i)&(N-1);
        const char*k=__atomic_load_n(&keys[s],__ATOMIC_ACQUIRE);
        if(!k){ const char*exp=0;   // claim the slot FIRST, then publish the value behind ready[]
            if(__atomic_compare_exchange_n(&keys[s],&exp,name,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
                char*v=getenv(name); vals[s]=v; __atomic_store_n(&ready[s],1,__ATOMIC_RELEASE); return v; }
            k=exp; }
        if(k==name) return __atomic_load_n(&ready[s],__ATOMIC_ACQUIRE) ? vals[s] : getenv(name); }
    return getenv(name); }
#define getenv(n) peload_cgetenv(n)
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
typedef struct { const char *name; uint8_t *base; const char *path; } Mod;   // path: host path of a guest DLL
static Mod g_mods[64]; static int g_nmods;
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
static void  unix2win_ex(const char*, uint16_t*, int, int);   // fwd
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
// GL entry points that MIX integer and float arguments can't go through the generic thunk: Win64 passes
// each float in the xmm register of its argument POSITION (glUniform2f(loc,x,y): x in xmm1, y in xmm2),
// SysV in the next free xmm register (x in xmm0, y in xmm1), and the generic thunk leaves xmm untouched.
// egui (Reclaimer's menu UI, drawn through WGL) sets u_screen_size with glUniform2f -> garbage screen size
// -> every vertex off-screen/NaN -> the menu UI read back fully transparent. Typed ms_abi wrappers let the
// compiler do the ABI conversion.
#define GLMIX(ret,name,params,args) static ret (*p_##name) params; static ret __attribute__((ms_abi)) w_##name params { return p_##name args; }
GLMIX(void,glUniform1f,(int l,float a),(l,a))
static void (*p_glUniform2f)(int,float,float);
static void __attribute__((ms_abi)) w_glUniform2f(int l,float a,float b){
    if(getenv("PELOAD_TRACE_UNIFORM")){ static int n; if(n++<6){ char m[96]; int k=snprintf(m,sizeof m,"  [glUniform2f] loc=%d (%g, %g)\n",l,a,b); (void)!write(2,m,k);
            if(n==1){ uint64_t*sp=(uint64_t*)__builtin_frame_address(0); int f=0; for(int i=0;i<3000&&f<16;i++){ uint64_t v=sp[i]; if(v>=0x140001000ULL&&v<0x141aae000ULL){ char b[48]; int q=snprintf(b,sizeof b,"     exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,b,q); f++; } } } } }
    p_glUniform2f(l,a,b); }
GLMIX(void,glUniform3f,(int l,float a,float b,float c),(l,a,b,c))
GLMIX(void,glUniform4f,(int l,float a,float b,float c,float d),(l,a,b,c,d))
GLMIX(void,glProgramUniform1f,(unsigned p,int l,float a),(p,l,a))
GLMIX(void,glProgramUniform2f,(unsigned p,int l,float a,float b),(p,l,a,b))
GLMIX(void,glProgramUniform3f,(unsigned p,int l,float a,float b,float c),(p,l,a,b,c))
GLMIX(void,glProgramUniform4f,(unsigned p,int l,float a,float b,float c,float d),(p,l,a,b,c,d))
GLMIX(void,glTexParameterf,(unsigned t,unsigned n,float v),(t,n,v))
GLMIX(void,glSamplerParameterf,(unsigned s,unsigned n,float v),(s,n,v))
GLMIX(void,glPointParameterf,(unsigned n,float v),(n,v))
GLMIX(void,glVertexAttrib1f,(unsigned i,float a),(i,a))
GLMIX(void,glVertexAttrib2f,(unsigned i,float a,float b),(i,a,b))
GLMIX(void,glVertexAttrib3f,(unsigned i,float a,float b,float c),(i,a,b,c))
GLMIX(void,glVertexAttrib4f,(unsigned i,float a,float b,float c,float d),(i,a,b,c,d))
GLMIX(void,glClearBufferfi,(unsigned b,int db,float d,int st),(b,db,d,st))
GLMIX(void,glTexEnvf,(unsigned t,unsigned n,float v),(t,n,v))
GLMIX(void,glFogf,(unsigned n,float v),(n,v))
GLMIX(void,glLightf,(unsigned l,unsigned n,float v),(l,n,v))
GLMIX(void,glMaterialf,(unsigned f,unsigned n,float v),(f,n,v))
static void *gl_mixed_wrapper(void *real){
    static int init; if(!init){ init=1;
        #define GLMIX_LOAD(name) p_##name=dlsym((void*)-2,#name);
        GLMIX_LOAD(glUniform1f) GLMIX_LOAD(glUniform2f) GLMIX_LOAD(glUniform3f) GLMIX_LOAD(glUniform4f)
        GLMIX_LOAD(glProgramUniform1f) GLMIX_LOAD(glProgramUniform2f) GLMIX_LOAD(glProgramUniform3f) GLMIX_LOAD(glProgramUniform4f)
        GLMIX_LOAD(glTexParameterf) GLMIX_LOAD(glSamplerParameterf) GLMIX_LOAD(glPointParameterf)
        GLMIX_LOAD(glVertexAttrib1f) GLMIX_LOAD(glVertexAttrib2f) GLMIX_LOAD(glVertexAttrib3f) GLMIX_LOAD(glVertexAttrib4f)
        GLMIX_LOAD(glClearBufferfi) GLMIX_LOAD(glTexEnvf) GLMIX_LOAD(glFogf) GLMIX_LOAD(glLightf) GLMIX_LOAD(glMaterialf)
        #undef GLMIX_LOAD
    }
    if(getenv("PELOAD_GL_NOMIXWRAP")) return 0;
    #define GLMIX_PICK(name) if(p_##name && real==(void*)p_##name) return (void*)w_##name;
    GLMIX_PICK(glUniform1f) GLMIX_PICK(glUniform2f) GLMIX_PICK(glUniform3f) GLMIX_PICK(glUniform4f)
    GLMIX_PICK(glProgramUniform1f) GLMIX_PICK(glProgramUniform2f) GLMIX_PICK(glProgramUniform3f) GLMIX_PICK(glProgramUniform4f)
    GLMIX_PICK(glTexParameterf) GLMIX_PICK(glSamplerParameterf) GLMIX_PICK(glPointParameterf)
    GLMIX_PICK(glVertexAttrib1f) GLMIX_PICK(glVertexAttrib2f) GLMIX_PICK(glVertexAttrib3f) GLMIX_PICK(glVertexAttrib4f)
    GLMIX_PICK(glClearBufferfi) GLMIX_PICK(glTexEnvf) GLMIX_PICK(glFogf) GLMIX_PICK(glLightf) GLMIX_PICK(glMaterialf)
    #undef GLMIX_PICK
    return 0; }
static void *make_gl_thunk(void *real){
    if(!real) return real;
    { void*w=gl_mixed_wrapper(real); if(w) return w; }
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
    // Win64 -> SysV ABI call stub, shared by every GL thunk (each thunk is `mov r11,real; jmp stub`).
    // Win64 passes args1-4 in rcx/rdx/r8/r9 and args5+ on the stack above a 32-byte shadow area; SysV
    // passes args1-6 in rdi/rsi/rdx/rcx/r8/r9 and args7+ on the stack. All 12 args are forwarded (an old
    // 6-arg thunk dropped glReadPixels' 7th / glTexSubImage2D's 9th arg and the driver scribbled the heap).
    // Registers that are callee-saved in Win64 but scratch in SysV must survive the call: rdi, rsi and
    // xmm6-xmm15. Without the xmm saves, egui's painter (which keeps pixels_per_point in xmm6 across its
    // GL calls) got 1280.0 back from a texture upload, so every clip rect collapsed and Reclaimer's server
    // list and other scroll areas drew nothing. PELOAD_GL_NOXMMSAVE drops the xmm saves (A/B).
    // Frame: rbp set before the pushes, so the caller's stack args are at ret@+8, shadow@+0x10..+0x28,
    // arg5@+0x30, ...; [rsp+0x00..0x30) holds SysV args 7-12, [rsp+0x40..0xe0) the saved xmm6-15.
    static uint8_t *stub;
    if(!stub){
        if(g_glthoff+512 > 128*8192){ pthread_mutex_unlock(&gtm); return real; }
        uint8_t *q=g_glthunk+g_glthoff; g_glthoff+=512; int n=0; int savex=!getenv("PELOAD_GL_NOXMMSAVE");
        #define EMIT(...) do{ const uint8_t b_[]={__VA_ARGS__}; memcpy(q+n,b_,sizeof b_); n+=(int)sizeof b_; }while(0)
        #define XMM_RSP(op,r,disp) do{ q[n++]=0xF3; if((r)>=8) q[n++]=0x44; q[n++]=0x0F; q[n++]=(op); q[n++]=(uint8_t)(0x84|(((r)&7)<<3)); q[n++]=0x24; uint32_t d_=(disp); memcpy(q+n,&d_,4); n+=4; }while(0)
        EMIT(0x55, 0x48,0x89,0xE5, 0x56, 0x57);                   // push rbp; mov rbp,rsp; push rsi; push rdi
        EMIT(0x48,0x81,0xEC,0xE0,0x00,0x00,0x00);                 // sub rsp,0xe0 (keeps 16-byte alignment)
        if(savex) for(int r=6;r<16;r++) XMM_RSP(0x7F,r,0x40+16*(r-6));   // movdqu [rsp+..],xmmN
        EMIT(0x48,0x89,0xCF, 0x48,0x89,0xD6, 0x4C,0x89,0xC2, 0x4C,0x89,0xC9);   // rdi,rsi,rdx,rcx <- rcx,rdx,r8,r9
        EMIT(0x4C,0x8B,0x45,0x30, 0x4C,0x8B,0x4D,0x38);           // r8,r9 <- arg5,arg6
        EMIT(0x48,0x8B,0x45,0x40, 0x48,0x89,0x04,0x24);           // arg7  -> [rsp+0x00]
        EMIT(0x48,0x8B,0x45,0x48, 0x48,0x89,0x44,0x24,0x08);      // arg8  -> [rsp+0x08]
        EMIT(0x48,0x8B,0x45,0x50, 0x48,0x89,0x44,0x24,0x10);      // arg9  -> [rsp+0x10]
        EMIT(0x48,0x8B,0x45,0x58, 0x48,0x89,0x44,0x24,0x18);      // arg10 -> [rsp+0x18]
        EMIT(0x48,0x8B,0x45,0x60, 0x48,0x89,0x44,0x24,0x20);      // arg11 -> [rsp+0x20]
        EMIT(0x48,0x8B,0x45,0x68, 0x48,0x89,0x44,0x24,0x28);      // arg12 -> [rsp+0x28]
        EMIT(0x41,0xFF,0xD3);                                      // call r11
        if(savex) for(int r=6;r<16;r++) XMM_RSP(0x6F,r,0x40+16*(r-6));   // movdqu xmmN,[rsp+..]
        EMIT(0x48,0x8D,0x65,0xF0, 0x5F, 0x5E, 0x5D, 0xC3);        // lea rsp,[rbp-0x10]; pop rdi; pop rsi; pop rbp; ret
        #undef EMIT
        #undef XMM_RSP
        sys_icache_invalidate(q,512); stub=q;
    }
    if(g_glthoff+128 > 128*8192){ pthread_mutex_unlock(&gtm); return real; }
    uint8_t *p=g_glthunk+g_glthoff; g_glthoff+=128;
    p[0]=0x49; p[1]=0xBB; *(uint64_t*)(p+2)=(uint64_t)real;                 // mov r11, real
    p[10]=0xE9; int32_t rel=(int32_t)(stub-(p+15)); memcpy(p+11,&rel,4);    // jmp stub
    sys_icache_invalidate(p,16);  // Rosetta: translate the freshly written thunk
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
// Must be unique per thread: msvcp140's std::mutex (_Mtx_lock) records the owner's GetCurrentThreadId() and SKIPS
// the real lock when the caller's id equals it ("recursive" re-entry). The old constant 0x1000 made every thread
// look like the owner of any held std::mutex -> no mutual exclusion, cross-thread unlocks of the underlying SRW
// rwlock (refused by macOS) -> locks left held -> halo3 hung at match start in _Mtx_lock(halo3+0x21c2398) from
// 0x173bb0. The mach thread port name is unique per live thread (and is what the thread dumps print as mach_tid).
// PELOAD_TID_CONST = old behavior.
static uint32_t MSABI i_GetCurrentThreadId(void){ static int k=-1; if(k<0) k=getenv("PELOAD_TID_CONST")?1:0; if(k) return 0x1000;
    return (uint32_t)pthread_mach_thread_np(pthread_self()); }
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
    if(le && (g_lasterr==2 || !strcmp(le,"all") || (atoi(le)>2 && (uint32_t)atoi(le)==g_lasterr))){ void*ra=__builtin_return_address(0); char t[640]; uint64_t r=(uint64_t)ra; snprintf(t,sizeof t,"  [GLE=%u] caller=%s+0x%llx lastfail='%s'\n",g_lasterr,(r>=0x140000000&&r<0x14c000000)?"exe":(r>=0x180000000&&r<0x187000000)?"halo3":"?",(unsigned long long)((r>=0x140000000&&r<0x14c000000)?r-0x140000000:(r>=0x180000000&&r<0x187000000)?r-0x180000000:r),t_lastfail); ws(t); }
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
        int k=snprintf(t,sizeof t,"  [ALLOC %llu] p=%p caller=%p\n",(unsigned long long)n,p,ret); (void)!write(2,t,k);
        static int ns; if(ns++<3){ uint64_t*sp=(uint64_t*)__builtin_frame_address(0); int found=0;   // exe return addresses on the stack
            for(int i=0;i<2000 && found<14;i++){ uint64_t v=sp[i]; if(v>=0x140001000ULL&&v<0x141aae000ULL){ char b[48]; int q=snprintf(b,sizeof b,"     exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,b,q); found++; } } } } }
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
// Loaded guest DLL by file name (case-insensitive, with or without a directory / ".dll").
static uint8_t* guest_mod_by_name_w(const uint16_t*n){ if(!n) return 0; char a[512]; int i=0; for(; n[i]&&i<511;i++) a[i]=(char)(n[i]<128?n[i]:'?'); a[i]=0;
    const char*b=a; for(const char*q=a;*q;q++) if(*q=='\\'||*q=='/') b=q+1;
    for(int k=0;k<g_nmods;k++){ const char*mn=g_mods[k].name; if(!mn) continue; const char*mb=mn; for(const char*q=mn;*q;q++) if(*q=='\\'||*q=='/') mb=q+1;
        size_t lb=strlen(b), lm=strlen(mb); if(!strcasecmp(mb,b) || (lm==lb+4 && !strncasecmp(mb,b,lb) && !strcasecmp(mb+lb,".dll"))) return g_mods[k].base; }
    return 0; }
// The loaded image (exe or guest DLL) containing an address.
static uint8_t* guest_mod_by_addr(uint64_t p){
    if(g_base){ OPT64*o=(OPT64*)((uint8_t*)g_base+((DOS*)g_base)->e_lfanew+sizeof(COFF)); if(p>=(uint64_t)g_base && p<(uint64_t)g_base+o->ImageSize) return g_base; }
    for(int k=0;k<g_nmods;k++){ uint8_t*bs=g_mods[k].base; OPT64*o=(OPT64*)(bs+((DOS*)bs)->e_lfanew+sizeof(COFF)); if(p>=(uint64_t)bs && p<(uint64_t)bs+o->ImageSize) return bs; }
    return 0; }
static void*  MSABI i_GetModuleHandleW(uint16_t *n){ if(name_is_ws2_w(n)) return WS2_HANDLE; { uint8_t*m=guest_mod_by_name_w(n); if(m) return m; } return g_base; } // ws2_32 / guest DLLs by name; else image base
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
double g_gameplay_t0;   // monotonic seconds when the client logged "gameplay presentation ready" (0 = not yet)
double g_menu_ready_t0; // ... "Menu loading screen finished" (frontend menu mode)
volatile int g_games_ended; // count of "Carnage report opened" (a match ended; PELOAD_AUTOCLICK_AFTER_GAME)
static double mono_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static int    MSABI i_WriteFile(void *h, const void *buf, uint32_t n, uint32_t *wr, void *ov){
    (void)ov; intptr_t hv=(intptr_t)h;
    if(!g_gameplay_t0 && buf && n>20 && memmem(buf,n,"gameplay presentation ready",27)) g_gameplay_t0=mono_s();
    if(!g_menu_ready_t0 && buf && n>20 && memmem(buf,n,"Menu loading screen finished",28)) g_menu_ready_t0=mono_s();
    if(buf && n>20 && memmem(buf,n,"Carnage report opened",21)) __atomic_add_fetch(&g_games_ended,1,__ATOMIC_SEQ_CST);
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
    (void)rsv; int fd=(h==(void*)3)?2:1;
    if(!g_gameplay_t0 && s && n>20){ char a[512]; uint32_t m=n<511?n:511; for(uint32_t i=0;i<m;i++) a[i]=(s[i]<128)?(char)s[i]:'?'; if(memmem(a,m,"gameplay presentation ready",27)) g_gameplay_t0=mono_s(); }
    if(!g_menu_ready_t0 && s && n>20){ char a[512]; uint32_t m=n<511?n:511; for(uint32_t i=0;i<m;i++) a[i]=(s[i]<128)?(char)s[i]:'?'; if(memmem(a,m,"Menu loading screen finished",28)) g_menu_ready_t0=mono_s(); }
    for(uint32_t i=0;i<n;i++){ char c=(s[i]<128)?(char)s[i]:'?'; (void)!write(fd,&c,1);} if(wr)*wr=n; return 1; }
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
    struct { uint32_t off, fmt, slot, semidx, cls, step; char sem[32]; } elem[16];  // + SemanticIndex, InputSlotClass, InstanceDataStepRate
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
    int rvalid; uint64_t rlo, rhi; // buffer: byte range uploaded to GL since the last Map/Update (partial uploads)
    void* isg; int nisg;          // VS: parsed input signature (semantic -> register), lazily from bc
    int copydst;                  // texture: destination of a GPU copy from a rendered target (diagnostics)
    int last_vpw, last_vph;       // render target: viewport of the most recent draw into it
    char rname[48];               // shader: reflected cbuffer/resource names (diagnostics)
    int stkind; uint32_t sd[72];  // state object: 1=blend 2=depth-stencil 3=rasterizer 4=sampler + raw D3D11 desc
    unsigned glsamp, glsamp_nomip; // sampler state: GL sampler objects (full / no-mip variant)
    unsigned glsamp_skip, glsamp_nomip_skip; // ... with sRGB decode SKIPPED (UNORM view of sRGB-stored texture)
    int glsrgb;                   // texture: GL storage is SRGB8_ALPHA8 (decode per view; write-encode per RTV)
    int glhasmips;                // texture: a real GL mip chain was uploaded/generated
    uint32_t mips, arr, depth, misc, bindf; // texture desc: MipLevels (resolved), ArraySize, Depth (3D), MiscFlags, BindFlags
    unsigned gltarget;            // texture: GL target of gltex (2D / 2D_ARRAY / CUBE_MAP / CUBE_MAP_ARRAY / 3D)
    unsigned gldsatt;             // depth-stencil resource: GL attachment point of gltex (DEPTH / DEPTH_STENCIL), 0 = not a depth texture
    uint32_t vkind, vfmt, vdim, vmip, vfirst, vcount; // view: 1=SRV 2=UAV 3=RTV 4=DSV, desc Format, ViewDimension, mip, first slice, slices
    uint32_t ver;                 // bumped on every CPU write (Map/UpdateSubresource/copy) -> GL re-upload of the UBO copy
    unsigned glubo; uint32_t uboglver, ubosz; // constant buffer: GL uniform-buffer copy, the ver it holds, its byte size
    unsigned gltbo, gltbobuf; uint32_t tbover; int tbook; // buffer SRV: GL texture buffer (R32UI) + its storage, uploaded ver
    void* csprog;                 // compute shader: CPU-interpreter program (dxbc_cpu.c), compiled on first Dispatch
    char* monosrc;                // shader: mono-path GLSL (injected + renamed), built once and shared by every pair
    uint64_t bchash;              // shader: FNV-1a of the bytecode (identifies it across runs for the pair pre-link list)
    unsigned shc_id[4]; uint64_t shc_mask[4]; int shc_n; // shader: compiled GL shaders per interface-padding mask
    void* canon;                  // shader: first-created object with identical bytecode (programs are keyed by it)
    uint32_t intin; int intin_ok; // VS: input registers declared uvec4/ivec4 (integer vertex attributes)
    volatile int cs_pending;      // buffer: queued async compute jobs that write it (wait before reading its CPU copy)
    unsigned glq; int qstate; uint64_t qval; // query: GL query object, 0 idle / 1 begun / 2 ended, cached result (timestamps)
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
    D3DObj *psrvv[32], *rtvv[8];               // the bound VIEW objects (format: sRGB vs UNORM view of a TYPELESS resource)
    D3DObj *rtvs[8]; int num_rt; D3DObj *dsv;  // multiple render targets (deferred G-buffer) + depth
    D3DObj *vs, *ps, *vscb[16], *pscb[16];   // bound shaders + constant buffers (for D3DGL)
    uint32_t vscb_1st[16], vscb_cnt[16], pscb_1st[16], pscb_cnt[16]; // *SetConstantBuffers1 ranges (16B units)
    D3DObj *vssrv[32], *vssrvv[32];           // VS-stage shader resources (+views): particle state buffers (t16/t17), VTF
    D3DObj *blend, *depth, *raster;           // bound state objects (descs captured at create time)
    float blendf[4]; uint32_t sref; int sc_l,sc_t,sc_r,sc_b; int vp_x, vp_y; D3DObj *pssamp[16], *vssamp[16];
    float vp_zn, vp_zf; int vp_set;           // viewport MinDepth/MaxDepth (first-person view models use their own slice)
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
    if(desc){ o->bindf=bind; o->misc=*(const uint32_t*)((const char*)desc+16); o->depth=*(const uint32_t*)((const char*)desc+20); }  // depth = StructureByteStride for buffers
    if(desc && getenv("PELOAD_CSCENSUS") && ((o->misc&0x60)||(bind&0x80))){ char m[160]; int k=snprintf(m,sizeof m,"  [cscensus] CreateBuffer %lluB bind=0x%x misc=0x%x stride=%u init=%p\n",(unsigned long long)sz,bind,o->misc,o->depth,init); (void)!write(2,m,k); }
    if(init && o->data){ const void*src=*(void*const*)init; if(src) memcpy(o->data,src,sz); }
    if((bind&0x4) && o){ o->is_cb=1; if(g_ncbreg<(int)(sizeof g_cbreg/sizeof*g_cbreg)) g_cbreg[g_ncbreg++]=o; }
    if(pp)*pp=o; return HR_OK; }
// textures: compute the REAL footprint from the DXGI format so our buffer matches
// exactly what the engine writes via Map (no over/under-allocation -> no overflow).
static int is_bc(uint32_t f){ return (f>=70&&f<=84)||(f>=94&&f<=99); }      // BC1..BC7
// sRGB handling: 8-bit RGBA/BGRA (and decoded BC1-3) textures are STORED as SRGB8_ALPHA8; whether a read
// decodes and a write encodes is decided per D3D VIEW format (EXT_texture_sRGB_decode + GL_FRAMEBUFFER_SRGB),
// because halo3 renders into a TYPELESS target through an _SRGB RTV and copies it through a UNORM view.
static int dxgi_is_srgb(uint32_t f){ return f==29||f==72||f==75||f==78||f==91||f==93||f==99; }
static int dxgi_srgb_storable(uint32_t f){ return (f>=27&&f<=29)||f==87||f==88||(f>=90&&f<=93)||(f>=70&&f<=78); }
static int g_nosrgb=-1; static int nosrgb(void){ if(g_nosrgb<0) g_nosrgb=getenv("PELOAD_D3DGL_NOSRGB")?1:0; return g_nosrgb; }
static uint32_t bc_block(uint32_t f){ return ((f>=70&&f<=72)||(f>=79&&f<=81))?8:16; } // BC1,BC4=8B blocks
static uint32_t dxgi_bpp(uint32_t f){                                         // bytes per texel (non-BC)
    if(f>=1&&f<=4) return 16; if(f>=5&&f<=8) return 12; if(f>=9&&f<=22) return 8;
    if(f>=48&&f<=59) return 2; if(f>=60&&f<=65) return 1; if(f==85||f==86||f==115) return 2;
    return 4; }                                                               // 23..47, 67..69, 87..93: 32-bit
// Byte offset + size of subresource `sub` (mip level; array=1 common case) inside a texture's data
// buffer, walking the mip chain (BC counts 4x4 blocks). halo3 streams each mip separately via
// CopySubresourceRegion(dstSub,srcSub); honoring these offsets is what puts each mip in the right place.
static uint64_t tex_sub_off(uint32_t w,uint32_t h,uint32_t fmt,uint32_t sub,uint64_t*psz){
    int bc=is_bc(fmt); uint32_t bb=bc?bc_block(fmt):dxgi_bpp(fmt); uint64_t off=0,ms=0;
    for(uint32_t m=0;;m++){ uint32_t mw=w>>m; if(!mw)mw=1; uint32_t mh=h>>m; if(!mh)mh=1;
        ms = bc ? ((uint64_t)((mw+3)/4)*bb)*((mh+3)/4) : (uint64_t)mw*bb*mh;
        if(m==sub){ if(psz)*psz=ms; return off; }
        off+=ms; if(mw==1&&mh==1){ if(psz)*psz=ms; return off; } }
}
static void* g_watch_tex;   // PELOAD_WATCH_TEX: the one texture whose data history we log
static D3DObj* mktex(uint32_t w,uint32_t h,uint32_t arr,uint32_t fmt,uint32_t kind){
    if(w<1)w=1; if(h<1)h=1; if(arr<1)arr=1;
    uint64_t rp,sz;
    if(is_bc(fmt)){ uint32_t bb=bc_block(fmt); rp=(uint64_t)((w+3)/4)*bb; sz=rp*((h+3)/4); }
    else { rp=(uint64_t)w*dxgi_bpp(fmt); sz=rp*h; }
    sz = sz*arr*2 + 65536;                                                    // array + mip-chain + guard
    D3DObj* o=d3dobj(g_d3dres_vtbl,kind,sz); o->rowpitch=(uint32_t)rp; o->w=w; o->h=h; o->fmt=fmt; return o; }
// ---- exact D3D11 subresource layout --------------------------------------------------------------
// data is slice-major [array slice][mip] (a 3D level holds all of its depth slices); subresource index
// = mip + slice*MipLevels (D3D11CalcSubresource). The old model treated `sub` as a mip of slice 0, so
// cube faces / array slices overwrote each other and only face 0 / slice 0 ever existed.
static uint32_t tex_full_mips(uint32_t w,uint32_t h,uint32_t d){ uint32_t m=1; while(w>1||h>1||d>1){ w>>=1; h>>=1; d>>=1; m++; } return m; }
static uint32_t tex_depth(const D3DObj*o){ return (o->kind==4&&o->depth)?o->depth:1; }
static uint32_t tex_nslices(const D3DObj*o){ return (o->kind==4)?1:(o->arr?o->arr:1); }
static uint32_t tex_nmips(const D3DObj*o){ uint32_t full=tex_full_mips(o->w?o->w:1,o->h?o->h:1,tex_depth(o)); return (o->mips&&o->mips<=full)?o->mips:full; }
static uint64_t tex_level(const D3DObj*o,uint32_t m,uint32_t*prp,uint32_t*prows,uint32_t*pmw,uint32_t*pmh,uint32_t*pmd){
    uint32_t mw=(o->w?o->w:1)>>m, mh=(o->h?o->h:1)>>m, md=tex_depth(o)>>m; if(!mw)mw=1; if(!mh)mh=1; if(!md)md=1;
    uint32_t rp,rows; if(is_bc(o->fmt)){ rp=((mw+3)/4)*bc_block(o->fmt); rows=(mh+3)/4; } else { rp=mw*dxgi_bpp(o->fmt); rows=mh; }
    if(prp)*prp=rp; if(prows)*prows=rows; if(pmw)*pmw=mw; if(pmh)*pmh=mh; if(pmd)*pmd=md;
    return (uint64_t)rp*rows*md; }
// offset of subresource `sub`; fills its size/pitch/rows/dims. UINT64_MAX when it doesn't fit the buffer.
static uint64_t tex_sub(const D3DObj*o,uint32_t sub,uint64_t*psz,uint32_t*prp,uint32_t*prows,uint32_t*pmw,uint32_t*pmh,uint32_t*pmd){
    uint32_t nm=tex_nmips(o), m=sub%nm, sl=sub/nm; uint64_t chain=0,off=0;
    for(uint32_t k=0;k<nm;k++){ uint64_t ls=tex_level(o,k,0,0,0,0,0); if(k<m) off+=ls; chain+=ls; }
    uint64_t sz=tex_level(o,m,prp,prows,pmw,pmh,pmd); if(psz)*psz=sz;
    off+=(uint64_t)sl*chain;
    if(!o->data || off+sz>o->size) return UINT64_MAX;
    return off; }
// copy ALL pInitialData subresources (D3D11_SUBRESOURCE_DATA[slices*mips] = {pSysMem, SysMemPitch,
// SysMemSlicePitch}) -- the old path copied only subresource 0 (cube face +X mip 0 / array slice 0 /
// 3D textures nothing at all).
static void tex_init_copy(D3DObj*tx,const void*init){
    if(!init||!tx||!tx->data) return;
    uint32_t nm=tex_nmips(tx), ns=tex_nslices(tx);
    for(uint32_t sl=0;sl<ns;sl++) for(uint32_t m=0;m<nm;m++){ uint32_t sub=m+sl*nm;
        const uint8_t*sd=(const uint8_t*)init+16*(size_t)sub; const uint8_t*sys=*(const uint8_t* const*)sd;
        uint32_t sp=*(const uint32_t*)(sd+8), ssp=*(const uint32_t*)(sd+12); if(!sys) continue;
        uint64_t sz; uint32_t rp,rows,mw,mh,md; uint64_t off=tex_sub(tx,sub,&sz,&rp,&rows,&mw,&mh,&md); if(off==UINT64_MAX) return;
        uint32_t srp=sp>=rp?sp:rp;                                  // BC SysMemPitch seen short: source is tight
        uint64_t sslice=(ssp>=(uint64_t)srp*rows)?ssp:(uint64_t)srp*rows;
        for(uint32_t z=0;z<md;z++) for(uint32_t y=0;y<rows;y++)
            memcpy((uint8_t*)tx->data+off+((uint64_t)z*rows+y)*rp, sys+(uint64_t)z*sslice+(uint64_t)y*srp, rp); }
    tx->gl_dirty=1; }
static int MSABI dev_CreateTexture1D(void*t,const void*d,const void*init,void**pp){ (void)t;
    uint32_t w=d?*(const uint32_t*)d:1, arr=d?*(const uint32_t*)((const char*)d+8):1, f=d?*(const uint32_t*)((const char*)d+12):0;
    D3DObj*tx=mktex(w,1,arr,f,3); if(tx&&d){ tx->arr=arr?arr:1; tx->depth=1; tx->mips=*(const uint32_t*)((const char*)d+4); tx->bindf=*(const uint32_t*)((const char*)d+20); tx->misc=*(const uint32_t*)((const char*)d+28); }
    if(tx){ tx->mips=tex_nmips(tx); if(!getenv("PELOAD_NO_TEXINIT")) tex_init_copy(tx,init); }
    if(pp)*pp=tx; return HR_OK; }
static void d3dtrace(int i,const char*nm); // fwd
static void draw_log(const char*tag,uint32_t count); // fwd
static int MSABI dev_CreateTexture2D(void*t,const void*d,const void*init,void**pp){ (void)t; d3dtrace(5,"CreateTexture2D");
    uint32_t w=d?*(const uint32_t*)d:1, h=d?*(const uint32_t*)((const char*)d+4):1, arr=d?*(const uint32_t*)((const char*)d+12):1, f=d?*(const uint32_t*)((const char*)d+16):0;
    { const char*tf=getenv("PELOAD_TRACE_TEXFMT"); if(tf && d && (uint32_t)atoi(tf)==f){ uint64_t v=(uint64_t)__builtin_return_address(0); const char*mod="?"; uint64_t off=v;
        if(v>=0x180000000ULL&&v<0x187000000ULL){mod="halo3";off=v-0x180000000ULL;} else if(v>=0x140000000ULL&&v<0x14c000000ULL){mod="exe";off=v-0x140000000ULL;}
        const uint32_t*D=d; char m[220]; int k=snprintf(m,sizeof m,"  [texfmt] %ux%u mips=%u arr=%u fmt=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x init=%p ra=%s+0x%llx\n",w,h,D[2],arr,f,D[7],D[8],D[9],D[10],init,mod,(unsigned long long)off); (void)!write(2,m,k); } }
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
    if(tx&&d){ tx->arr=arr?arr:1; tx->depth=1; tx->mips=*(const uint32_t*)((const char*)d+8); tx->bindf=*(const uint32_t*)((const char*)d+32); tx->misc=*(const uint32_t*)((const char*)d+40); }
    if(tx) tx->mips=tex_nmips(tx);                                            // MipLevels 0 = full chain
    if(getenv("PELOAD_TRACE_TEXPATH") && w>=512 && h>=512 && ((f>=70&&f<=84)||(f>=94&&f<=99))){ char m[128]; int k=snprintf(m,sizeof m,"  [texpath] CREATE %ux%u f%u init=%s obj=%p\n",w,h,f,init?"DATA":"NULL",(void*)tx); (void)!write(2,m,k); }
    // PELOAD_WATCH_TEX: pick the first 1024x1024 BC texture and log its ENTIRE data history
    // (create/init/copy/update/upload) so we can see exactly how/if it gets its pixels.
    if(getenv("PELOAD_WATCH_TEX") && !g_watch_tex && w==1024 && h==1024 && ((f>=70&&f<=84)||(f>=94&&f<=99))){
        g_watch_tex=tx; char m[128]; int k=snprintf(m,sizeof m,"  [WATCH] created 1024x1024 f%u init=%s obj=%p size=%lluB\n",f,init?"DATA":"NULL",(void*)tx,(unsigned long long)tx->size); (void)!write(2,m,k); }
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
            if(sz>tx->size)sz=tx->size; tex_init_copy(tx,init);
            if(tx==g_watch_tex){ int nz=0; const uint8_t*dp=tx->data; for(uint64_t i=0;i<sz&&nz<99;i+=(sz/100?sz/100:1)) if(dp[i])nz++; char m[128]; int k=snprintf(m,sizeof m,"  [WATCH] INIT copy %lluB pitch=%u -> nz=%d/100\n",(unsigned long long)sz,pitch,nz); (void)!write(2,m,k); }
            if(getenv("PELOAD_TRACE_TEXINIT")){ static int ti; if(ti++<24){ uint32_t mips=d?*(const uint32_t*)((const char*)d+8):0;
                int q[4]={0,0,0,0}; const uint8_t*sp=sys; uint64_t qb=sz/4; if(qb>0) for(int Q=0;Q<4;Q++){ for(uint64_t i=0;i<qb;i+=((qb/64)?(qb/64):1)) if(sp[Q*qb+i]) q[Q]++; }
                char m[220]; int k=snprintf(m,sizeof m,"  [texinit] %ux%u f%u mips=%u arr=%u pitch=%u copy=%lluB tsize=%lluB src-nz-quarters=[%d %d %d %d]/~64\n",w,h,f,mips,arr,pitch,(unsigned long long)sz,(unsigned long long)tx->size,q[0],q[1],q[2],q[3]); (void)!write(2,m,k);} } } }
    if(getenv("PELOAD_TRACE_TEXDIM") && tx && (tx->arr>1 || (tx->misc&4))){ char m[200]; int k=snprintf(m,sizeof m,"  [texdim] CREATE2D %ux%u arr=%u mips=%u f%u misc=0x%x bind=0x%x init=%p\n",w,h,tx->arr,tx->mips,f,tx->misc,tx->bindf,init); (void)!write(2,m,k); }
    if(pp)*pp=tx; return HR_OK; }
static int MSABI dev_CreateTexture3D(void*t,const void*d,const void*init,void**pp){ (void)t;
    uint32_t w=d?*(const uint32_t*)d:1, h=d?*(const uint32_t*)((const char*)d+4):1, dp=d?*(const uint32_t*)((const char*)d+8):1, f=d?*(const uint32_t*)((const char*)d+16):0;
    D3DObj*tx=mktex(w,h,dp,f,4); if(tx&&d){ tx->arr=1; tx->depth=dp?dp:1; tx->mips=*(const uint32_t*)((const char*)d+12); tx->bindf=*(const uint32_t*)((const char*)d+24); tx->misc=*(const uint32_t*)((const char*)d+32); }
    if(tx){ tx->mips=tex_nmips(tx); if(!getenv("PELOAD_NO_TEXINIT")) tex_init_copy(tx,init); }
    if(getenv("PELOAD_TRACE_TEXDIM")){ char m[160]; int k=snprintf(m,sizeof m,"  [texdim] CREATE3D %ux%ux%u f%u mips=%u init=%p\n",w,h,dp,f,tx?tx->mips:0,init); (void)!write(2,m,k); }
    if(pp)*pp=tx; return HR_OK; }
// Views keep their desc: format (sRGB vs UNORM of a TYPELESS resource), dimension, mip and array-slice
// range (shadow cascades / cube faces rendered through per-slice RTV/DSVs).
static int dev_crViewK(int kind,const void*res,const void*desc,void**pp){ D3DObj*o=d3dobj(g_d3dview_vtbl,0,0); o->data=(void*)res; o->vkind=kind;
    const D3DObj*r=res; o->vfmt=r?r->fmt:0; o->vdim=0; o->vmip=0; o->vfirst=0; o->vcount=r?(r->kind==4?1:(r->arr?r->arr:1)):1;
    if(desc){ const uint32_t*d=desc; o->vfmt=d[0]?d[0]:o->vfmt; o->vdim=d[1];
        int arrdim = (kind==1)? (d[1]==5||d[1]==10||d[1]==3) : (kind==4)? (d[1]==2||d[1]==4||d[1]==6) : (d[1]==3||d[1]==5||d[1]==7);  // *_ARRAY dims (SRV / DSV / RTV enums differ)
        if(kind==1){ o->vmip=d[2]; if(arrdim){ o->vfirst=(d[1]==10)?d[4]*6:d[4]; o->vcount=(d[1]==10)?d[5]*6:d[5]; } else if(d[1]==9){ o->vfirst=0; o->vcount=6; } else o->vcount=1; }
        else if(kind==3){ o->vmip=d[2]; if(arrdim){ o->vfirst=d[3]; o->vcount=d[4]; } else if(d[1]==8){ o->vfirst=d[3]; o->vcount=d[4]; } else o->vcount=1; }  // 3D: FirstWSlice
        else if(kind==4){ o->vmip=d[3]; if(arrdim){ o->vfirst=d[4]; o->vcount=d[5]; } else o->vcount=1; } }
    if(getenv("PELOAD_VIEWCENSUS") && r){ static uint64_t seen[512]; static int ns; uint64_t key=(uint64_t)kind|((uint64_t)o->vfmt<<4)|((uint64_t)r->fmt<<12)|((uint64_t)o->vdim<<20)|((uint64_t)(r->arr>1)<<28)|((uint64_t)(o->vcount>1)<<29)|((uint64_t)(o->vfirst>0)<<30);
        int f=0; for(int i=0;i<ns;i++) if(seen[i]==key){ f=1; break; }
        if(!f && ns<512){ seen[ns++]=key; static const char*kn[5]={"?","SRV","UAV","RTV","DSV"}; char m[220];
            int k=snprintf(m,sizeof m,"  [viewcensus] %s res=%ux%u f%u arr=%u mips=%u misc=0x%x bind=0x%x -> view f%u dim%u mip%u slices[%u+%u]\n",kn[kind&3?kind:0],r->w,r->h,r->fmt,r->arr,r->mips,r->misc,r->bindf,o->vfmt,o->vdim,o->vmip,o->vfirst,o->vcount); (void)!write(2,m,k); } }
    if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crSRV(void*t,const void*res,const void*desc,void**pp){ (void)t; return dev_crViewK(1,res,desc,pp); }
static int MSABI dev_crUAV(void*t,const void*res,const void*desc,void**pp){ (void)t; return dev_crViewK(2,res,desc,pp); }
static int MSABI dev_crRTV(void*t,const void*res,const void*desc,void**pp){ (void)t; return dev_crViewK(3,res,desc,pp); }
static int MSABI dev_crDSV(void*t,const void*res,const void*desc,void**pp){ (void)t; return dev_crViewK(4,res,desc,pp); }
static int MSABI dev_crView3(void*t,const void*res,const void*desc,void**pp){ (void)t; return dev_crViewK(0,res,desc,pp); }
static int MSABI dev_cr2(void*t,const void*a,void**pp){ (void)t;(void)a; if(pp)*pp=d3dobj(g_d3dchild_vtbl,0,0); return HR_OK; }      // (desc,pp) out=r8
// ID3D11Query / ID3D11Predicate: desc {Query type, MiscFlags} kept in sd[0..1] (stkind 5). Backed by GL
// SAMPLES_PASSED queries for OCCLUSION / OCCLUSION_PREDICATE (halo3 sizes lens flares by the visible
// fraction) -- GetData used to return S_OK without writing anything, so flares read stale/garbage
// counts and flashed to full brightness for single frames.
static void*  g_d3dquery_vtbl[16];
static unsigned MSABI query_GetDataSize(void*s){ D3DObj*o=s; switch(o?o->sd[0]:0){ case 0: case 5: return 4; case 1: case 2: return 8; case 3: return 16; case 4: return 88; case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: return 16; } return 0; }
static void MSABI query_GetDesc(void*s,uint32_t*d){ D3DObj*o=s; if(d&&o){ d[0]=o->sd[0]; d[1]=o->sd[1]; } }
static int MSABI dev_crQuery(void*t,const uint32_t*desc,void**pp){ (void)t; D3DObj*o=d3dobj(g_d3dquery_vtbl,0,0); o->stkind=5;
    if(desc){ o->sd[0]=desc[0]; o->sd[1]=desc[1]; }
    if(getenv("PELOAD_TRACE_QUERY")){ char m[96]; int k=snprintf(m,sizeof m,"  [query] create %p type %u misc %u\n",(void*)o,o->sd[0],o->sd[1]); (void)!write(2,m,k); }
    if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crStateK(int kind,const void*desc,uint32_t bytes,void**pp){ D3DObj*o=d3dobj(g_d3dchild_vtbl,0,0); o->stkind=kind;
    if(desc){ if(bytes>sizeof o->sd) bytes=sizeof o->sd; memcpy(o->sd,desc,bytes); } if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crBlend(void*t,const void*d,void**pp){ (void)t; return dev_crStateK(1,d,264,pp); }
static int MSABI dev_crDSS(void*t,const void*d,void**pp){ (void)t; return dev_crStateK(2,d,52,pp); }
static int MSABI dev_crRS(void*t,const void*d,void**pp){ (void)t; return dev_crStateK(3,d,40,pp); }
static int MSABI dev_crSamp(void*t,const void*d,void**pp){ (void)t; return dev_crStateK(4,d,52,pp); }
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
static uint8_t* dg_bc_decode(uint32_t fmt,const uint8_t*src,size_t srclen,int w,int h); // fwd (BC decoder, defined with the GL uploads)
static uint64_t prof_now(void); static int prof_on(void);                                 // fwd (profiling)
#include "dxbc_cpu.c"
// FNV-1a 64 over the DXBC bytes (matches the cache key) -> ~/reclaimer_glsl/<hash>.glsl.
static uint64_t d3dgl_fnv(const void*b,uint64_t n){ const uint8_t*p=b; uint64_t h=0xcbf29ce484222325ULL;
    for(uint64_t i=0;i<n;i++){ h^=p[i]; h*=0x100000001b3ULL; } return h; }
static char* d3dgl_load_glsl(const void*bc,uint64_t len){
    const char*home=getenv("HOME"); char path[512];
    uint64_t hash=d3dgl_fnv(bc,len);
    // versioned cache dir: bump D3DGL_XLAT_VER whenever the translator's output changes, so stale GLSL
    // (older C/Python translator output) is never reused. v2 = lane-correct write masks + resource swizzle + sincos.
    // ALWAYS translate (cheap, in-process): a stale cache silently ran an older translator's output (seen:
    // VS without the y-flip under a flipping present -> upside-down frame; old compare semantics -> GPU
    // hang). Files are still written for inspection; PELOAD_D3DGL_GLSLCACHE=1 reads them back instead
    // (hand-edited GLSL experiments).
    #define D3DGL_XLAT_VER "v5"
    snprintf(path,sizeof path,"%s/reclaimer_glsl_" D3DGL_XLAT_VER "/%016llx.glsl",home?home:"/tmp",(unsigned long long)hash);
    FILE*f=getenv("PELOAD_D3DGL_GLSLCACHE")?fopen(path,"rb"):0;
    if(f){ fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if(sz<=0||sz>(4<<20)){ fclose(f); return 0; } char*s=malloc(sz+1); if(!s){ fclose(f); return 0; }
        if(fread(s,1,sz,f)!=(size_t)sz){ free(s); fclose(f); return 0; } s[sz]=0; fclose(f); return s; }
    // Cache miss: translate IN-PROCESS now (the program's own routine) and cache it, so ANY shader the
    // game uses gets GLSL at first use with no Python and no "miss -> skipped draw -> black" gap.
    if(!(bc && len>=4 && !memcmp(bc,"DXBC",4)) || getenv("PELOAD_D3DGL_NOXLAT")) return 0;
    g_dg_ubo = getenv("PELOAD_D3DGL_NOUBO")?0:1;
    g_dg_nodclsz = getenv("PELOAD_D3DGL_NODCLSZ")?1:0;
    g_dg_nocmp = getenv("PELOAD_D3DGL_NOCMP")?1:0;
    char*g=dg_translate((const uint8_t*)bc,(size_t)len);
    if(!g) return 0;
    char gdir[400]; snprintf(gdir,sizeof gdir,"%s/reclaimer_glsl_" D3DGL_XLAT_VER,home?home:"/tmp"); mkdir(gdir,0755);
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
static void shader_rname(D3DObj*o); // fwd
static void d3dgl_prelink_register(D3DObj*o); // fwd
static int MSABI dev_crShader(int stage,const void*bc,uint64_t len,void**pp){
    D3DObj*o=d3dobj(g_d3dchild_vtbl,0,0); o->stype=stage;
    if(bc&&len&&len<(16<<20)){ o->bc=malloc(len); if(o->bc){ memcpy(o->bc,bc,len); o->bclen=(uint32_t)len; } }
    if(getenv("PELOAD_FRAMELOG")) shader_rname(o);
    if(stage==0 && o->bc) o->vs_vtxin=dxbc_has_vtxinput(o->bc,o->bclen);
    if(getenv("PELOAD_D3DGL") && o->bc){ o->glsl=d3dgl_load_glsl(o->bc,o->bclen);
        { uint64_t h=1469598103934665603ull; for(uint32_t i=0;i<o->bclen;i++){ h^=((const uint8_t*)o->bc)[i]; h*=1099511628211ull; }
          o->bchash=h|1; d3dgl_prelink_register(o); }
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
            o->elem[i].semidx=*(const uint32_t*)(el+8); o->elem[i].cls=*(const uint32_t*)(el+24); o->elem[i].step=*(const uint32_t*)(el+28);
            if(slot>7) slot=0;
            if(off==0xFFFFFFFFu) off=run[slot];                        // D3D11_APPEND_ALIGNED_ELEMENT
            run[slot]=off+dxgi_vtx_bytes(fmt);
            o->elem[i].fmt=fmt; o->elem[i].slot=slot; o->elem[i].off=off;
            int j=0; if(sem) for(; sem[j]&&j<31;j++) o->elem[i].sem[j]=sem[j]; o->elem[i].sem[j]=0; } }
    if(pp)*pp=o; return HR_OK; }
static int MSABI dev_crCtx(void*t,uint32_t fl,void**pp){ (void)t;(void)fl; if(pp)*pp=&g_d3dcontext; return HR_OK; }                 // CreateDeferredContext
static D3DObj* mktex(uint32_t w,uint32_t h,uint32_t arr,uint32_t fmt,uint32_t kind); // fwd
static int MSABI dev_OpenShared(void*t,void*h,GUID_*r,void**pp){ (void)t;(void)r; if(pp)*pp=mktex(4096,4096,1,28,2);
    if(getenv("PELOAD_TRACE_COPY")){ char m[120]; int k=snprintf(m,sizeof m,"  [copy] OpenSharedResource(handle=%p) -> NEW blank 4096x4096 %p\n",h,pp?*pp:0); (void)!write(2,m,k); } return HR_OK; }
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
static void cs_wait_res(D3DObj*r); static void cs_wait_view(void*v); // fwd (async compute)
// PELOAD_D3DGL_FRAMESTAT: per-frame counters, one log line per engine Present (diagnosing one-frame glitches)
static struct { unsigned exec, novsps, noglsl, noprog, ringfail, wraps, disp, copies, maps, mapread, upd, clears, fbochg, rtclears, dsclears; } g_fs;
static int  MSABI ctx_Map(void*t,void*res,uint32_t sub,uint32_t type,uint32_t fl,void*pM){ (void)t;(void)sub;(void)type;(void)fl;
    g_fs.maps++; if(type==1||type==3) g_fs.mapread++;
    cs_wait_res((D3DObj*)res);
    if(getenv("PELOAD_TRACE_TEXUPD") && res && ((D3DObj*)res)->kind>=2 && ((D3DObj*)res)->w){ D3DObj*q=res; extern double g_gameplay_t0; uint64_t ra=(uint64_t)__builtin_return_address(0);
        char m[200]; int k=snprintf(m,sizeof m,"  [texupd] t=%.2f Map %p %ux%u f%u sub=%u type=%u ra=0x%llx\n",g_gameplay_t0?mono_s()-g_gameplay_t0:-1.0,(void*)q,q->w,q->h,q->fmt,sub,type,(unsigned long long)(ra>=0x180000000ULL&&ra<0x187000000ULL?ra-0x180000000ULL:ra)); (void)!write(2,m,k); }
    D3DObj*o=res; if(o){ o->gl_dirty=1; o->rvalid=0; o->ver++; if(o->is_cb) o->cb_upd=g_cb_tick; }   // mapped for write -> re-upload + mark CB fresh
    extern D3DObj* g_last_smallcb; if(o && o->is_cb && o->size>=32 && o->size<=96) g_last_smallcb=o;  // candidate decompress cb1
    if(o && getenv("PELOAD_TRACE_TEXPATH") && o->w>=512 && o->h>=512 && ((o->fmt>=70&&o->fmt<=84)||(o->fmt>=94&&o->fmt<=99))){ char m[128]; int k=snprintf(m,sizeof m,"  [texpath] MAP sub=%u type=%u %ux%u f%u obj=%p\n",sub,type,o->w,o->h,o->fmt,(void*)o); (void)!write(2,m,k); }
    if(pM){ void* d=(o&&o->data)?o->data:g_d3dscratch;
        uint32_t rp = o? (o->rowpitch?o->rowpitch:(uint32_t)o->size) : (uint32_t)sizeof g_d3dscratch;   // bytes per row
        // DepthPitch = one 2D slice. Must NOT be the whole buffer: for 3D/array
        // textures the engine strides depth slices by DepthPitch (slice N at N*dp),
        // so an oversized dp writes far past our buffer and corrupts the heap.
        uint32_t dp = o? (uint32_t)(o->rowpitch? (uint64_t)o->rowpitch*(o->h?o->h:1) : o->size) : (uint32_t)sizeof g_d3dscratch;
        if(o && dp>o->size) dp=(uint32_t)o->size;
        // textures: point at subresource `sub` (mip + slice*MipLevels) with that level's pitches
        if(o && o->kind>=2 && o->kind<=4 && o->data && o->w){ uint64_t sz; uint32_t lrp,rows,mw,mh,md;
            uint64_t off=tex_sub(o,sub,&sz,&lrp,&rows,&mw,&mh,&md); if(off!=UINT64_MAX){ d=(uint8_t*)o->data+off; rp=lrp; dp=lrp*rows; } }
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
static void d3dgl_draw(uint32_t vcount,int indexed,uint32_t instcount,uint32_t start,int32_t base,uint32_t sinst); // fwd
static void d3dgl_present(void); // fwd (blit the D3DGL backbuffer to the window at Present)
static int cb_looks_viewproj(const float*m,uint64_t bytes); static int hb_enabled(void); static int hb_is_world_camera(const float*rows); static void hb_capture_view(const float*rows); static void hb_draw_overlay(void); static void hb_note_map(const char*p); extern volatile int g_hb_taken; // hitbox.c
static void d3dgl_clear_rtv(void* rtvview,const float* col); // fwd (ClearRenderTargetView -> GL clear)
static void d3dgl_clear_dsv(void*dsvview,uint32_t flags,float depth,uint8_t stencil); // fwd (ClearDepthStencilView -> GL depth/stencil clear)
static void MSABI ctx_ClearRTV(void*t,void*rtv,const float*col){ (void)t; if(d3dgl_enabled()) d3dgl_clear_rtv(rtv,col); }
static void MSABI ctx_ClearDSV(void*t,void*dsv,uint32_t fl,float d,uint8_t s){ (void)t; if(d3dgl_enabled()) d3dgl_clear_dsv(dsv,fl,d,s); }
// PELOAD_DRAW_CALLER: log the first few distinct halo3 return addresses that call each draw type,
// so we can decompile the actual geometry-submit function and find why it binds no input layout.
static void draw_caller(const char*tag,void*ra,uint32_t vcount){ if(!getenv("PELOAD_DRAW_CALLER")) return;
    static uint64_t seen[64]; static int ns; uint64_t v=(uint64_t)ra;
    for(int i=0;i<ns;i++) if(seen[i]==v) return; if(ns<64) seen[ns++]=v;
    const char*mod="?"; uint64_t off=v; if(v>=0x180000000ULL&&v<0x187000000ULL){mod="halo3";off=v-0x180000000ULL;} else if(v>=0x140000000ULL&&v<0x14c000000ULL){mod="exe";off=v-0x140000000ULL;}
    char m[128]; int k=snprintf(m,sizeof m,"  [draw-caller] %s ra=%s+0x%llx n=%u vb0=%lluB stride=%u classicVS=%d il=%p\n",tag,mod,(unsigned long long)off,vcount,
        g_d3dst.vb[0]?(unsigned long long)g_d3dst.vb[0]->size:0,g_d3dst.vstride[0], (g_d3dst.vs?g_d3dst.vs->vs_vtxin:-1), (void*)g_d3dst.il); (void)!write(2,m,k); }
static void stg_note_draw(uint32_t n);
static void MSABI ctx_DrawIndexed(void*t,uint32_t n,uint32_t s,int b){ (void)t;(void)s;(void)b; d3dtrace(1,"DrawIndexed"); stg_note_draw(n); draw_caller("DrawIndexed",__builtin_return_address(0),n); draw_log("drawI",n); d3d_capture_frame(n); if(d3dgl_enabled()) d3dgl_draw(n,1,1,s,b,0); }
static void MSABI ctx_Draw(void*t,uint32_t n,uint32_t s){ (void)t;(void)s; d3dtrace(0,"Draw"); stg_note_draw(n); draw_caller("Draw",__builtin_return_address(0),n); draw_log("draw",n); d3d_capture_frame(n); if(d3dgl_enabled()) d3dgl_draw(n,0,1,s,0,0); }
static void MSABI ctx_UpdateSubresource(void*t,void*res,uint32_t sub,const void*box,const void*src,uint32_t srp,uint32_t sdp){
    (void)t; D3DObj*o=res; if(!o||!o->data||!src) return;
    cs_wait_res(o);
    if(getenv("PELOAD_TRACE_TEXUPD") && o->kind>=2 && o->w==1280 && o->h==720 && !box){ static int n; if((n++%300)==0){ uint64_t nz=0; const uint8_t*sp=src; for(uint32_t y=0;y<720;y+=4) for(uint32_t x=0;x<1280*4;x+=4) if(sp[(size_t)y*srp+x+3]) nz++;
        uint64_t nzr=0; for(uint32_t y=0;y<720;y+=4) for(uint32_t x=0;x<1280*4;x+=4) if(sp[(size_t)y*srp+x]|sp[(size_t)y*srp+x+1]|sp[(size_t)y*srp+x+2]) nzr++;
        char m[200]; int k=snprintf(m,sizeof m,"  [texupd-px] #%d %p src=%p pitch=%u nonzero-alpha=%llu nonzero-rgb=%llu /230400\n",n,(void*)o,src,srp,(unsigned long long)nz,(unsigned long long)nzr); (void)!write(2,m,k); } }
    if(getenv("PELOAD_TRACE_TEXUPD") && o->kind>=2 && o->w){ extern double g_gameplay_t0; const uint32_t*bx=box; uint64_t ra=(uint64_t)__builtin_return_address(0);
        char m[220]; int k=snprintf(m,sizeof m,"  [texupd] t=%.2f UpdateSubresource %p %ux%u f%u sub=%u box=%d,%d-%d,%d ra=0x%llx\n",g_gameplay_t0?mono_s()-g_gameplay_t0:-1.0,(void*)o,o->w,o->h,o->fmt,sub,bx?(int)bx[0]:-1,bx?(int)bx[1]:-1,bx?(int)bx[3]:-1,bx?(int)bx[4]:-1,(unsigned long long)(ra>=0x180000000ULL&&ra<0x187000000ULL?ra-0x180000000ULL:ra)); (void)!write(2,m,k); }
    o->gl_dirty=1; o->rvalid=0; o->ver++; if(o->is_cb) o->cb_upd=g_cb_tick;
    { extern D3DObj* g_last_smallcb; if(o->is_cb && o->size>=32 && o->size<=96) g_last_smallcb=o; }
    // MUST honor pDstBox: a partial (box) update supplies a source sized only for the box. Reading the
    // full-resource height instead overreads past the source end -> SIGSEGV in memmove. D3D11_BOX =
    // {Left,Top,Front,Right,Bottom,Back}; for a BUFFER Left/Right are byte offsets, rows=1.
    const uint32_t*b=box; const uint8_t*s=src; uint8_t*d=o->data; uint64_t cap=o->size;
    if(o->kind==1){                                   // BUFFER
        uint64_t off=b?b[0]:0, n=b?(uint64_t)(b[3]-b[0]):cap;
        if(off<cap){ if(off+n>cap)n=cap-off; memcpy(d+off,s,(size_t)n); } return; }
    // Textures: write subresource `sub` (mip + slice*MipLevels) at its own offset/pitch; BC boxes are in
    // pixels but rows are 4-high blocks. pDstBox {Left,Top,Front,Right,Bottom,Back}; 3D uses Front..Back
    // with the source depth pitch. (The old path ignored `sub` -> every mip/slice landed on mip 0.)
    { uint64_t lsz; uint32_t rp,rows,mw,mh,md; uint64_t base=tex_sub(o,sub,&lsz,&rp,&rows,&mw,&mh,&md); if(base==UINT64_MAX) return;
      int bc=is_bc(o->fmt); uint32_t bpe=bc?bc_block(o->fmt):dxgi_bpp(o->fmt), q=bc?4:1;
      uint32_t l=b?b[0]:0, tp=b?b[1]:0, fr=b?b[2]:0, r=b?b[3]:mw, bt=b?b[4]:mh, bk=b?b[5]:md;
      if(r>mw)r=mw; if(bt>mh)bt=mh; if(bk>md)bk=md; if(r<=l||bt<=tp||bk<=fr) return;
      uint32_t c0=l/q, c1=(r+q-1)/q, r0=tp/q, r1=(bt+q-1)/q; uint32_t rowbytes=(c1-c0)*bpe, sr=srp?srp:rowbytes;
      uint64_t sdpitch=sdp?sdp:(uint64_t)sr*(r1-r0);
      for(uint32_t z=fr;z<bk;z++) for(uint32_t y=r0;y<r1;y++){
          uint64_t doff=base+((uint64_t)z*rows+y)*rp+(uint64_t)c0*bpe, soff=(uint64_t)(z-fr)*sdpitch+(uint64_t)(y-r0)*sr;
          if(doff+rowbytes>cap) return; memcpy(d+doff,s+soff,rowbytes); } } }
// PELOAD_TRACE_COPY: log each distinct (dst,src) texture copy pair once (dims/fmt, whether the SOURCE is a
// GPU-rendered target with a GL FBO -- such content lives only in GL, so a CPU memcpy copies stale bytes).
static void flog(const char*fmt,...); static D3DObj* g_d3d_backbuf;
static void copy_trace(const char*what,D3DObj*dst,D3DObj*src){ if(dst&&src) flog("t%x %s dst=",pthread_mach_thread_np(pthread_self()),what); if(dst&&src) flog("   %s dst=%ux%u f%u%s <- src=%ux%u f%u%s",what,dst->w,dst->h,dst->fmt,dst==g_d3d_backbuf?"(BB)":"",src->w,src->h,src->fmt,src->glfbo?"(RT)":"");
    if(!getenv("PELOAD_TRACE_COPY")||!dst||!src) return;
    static struct { void*d,*s; } seen[256]; static int ns; static unsigned long tot; tot++;
    for(int i=0;i<ns;i++) if(seen[i].d==dst&&seen[i].s==src) return; if(ns<256){ seen[ns].d=dst; seen[ns].s=src; ns++; }
    char m[260]; int k=snprintf(m,sizeof m,"  [copy] %s #%lu dst=%p %ux%u f%u kind%u%s <- src=%p %ux%u f%u kind%u%s%s\n",what,tot,
        (void*)dst,dst->w,dst->h,dst->fmt,dst->kind,dst->glfbo?" RT":"",(void*)src,src->w,src->h,src->fmt,src->kind,src->glfbo?" RT":"",src->glfbo?"  <== GPU-RENDERED SRC (CPU copy is stale)":"");
    (void)!write(2,m,k); }
static void d3dgl_ensure_rt(D3DObj*o); static int d3dgl_ensure(void); // fwd
static int d3dgl_gpu_copy(D3DObj*dst,D3DObj*src,uint32_t sx,uint32_t sy,uint32_t sw,uint32_t sh,uint32_t dx,uint32_t dy); // fwd (impl after the GL table)
static void MSABI ctx_CopyResource(void*t,void*dst,void*src){ (void)t; D3DObj*a=dst,*b=src; g_fs.copies++; cs_wait_res(a); cs_wait_res(b); copy_trace("CopyResource",a,b); if(a) a->ver++;
    if(a&&b&&d3dgl_gpu_copy(a,b,0,0,b->w>4096?4096:b->w,b->h>4096?4096:b->h,0,0)) return; if(a&&b&&a->data&&b->data){ uint64_t n=a->size<b->size?a->size:b->size; memcpy(a->data,b->data,n); if(a)a->gl_dirty=1;
    if(getenv("PELOAD_TRACE_TEXPATH") && a->w>=512 && ((a->fmt>=70&&a->fmt<=84)||(a->fmt>=94&&a->fmt<=99))){ char m[128]; int k=snprintf(m,sizeof m,"  [texpath] COPYRES dst%ux%u f%u <- src%ux%u (%lluB)\n",a->w,a->h,a->fmt,b->w,b->h,(unsigned long long)n); (void)!write(2,m,k);} } }
// CopySubresourceRegion(dst,dstSub,dstX,dstY,dstZ,src,srcSub,pSrcBox). Was a no-op -> 12k/frame copies
// (texture staging / composite steps) were dropped. Implement the CPU copy (full or box region).
static void MSABI ctx_CopySubresourceRegion(void*t,void*dstR,uint32_t dstSub,uint32_t dstX,uint32_t dstY,uint32_t dstZ,void*srcR,uint32_t srcSub,const void*srcBox){
    (void)t; D3DObj*dst=dstR,*src=srcR; cs_wait_res(dst); cs_wait_res(src); copy_trace("CopySubresourceRegion",dst,src); if(dst) dst->ver++;
    if(dst&&src&&srcSub==0&&dstSub==0&&src->glfbo){ const uint32_t*bx=srcBox; uint32_t l=bx?bx[0]:0,tp=bx?bx[1]:0,r=bx?bx[3]:src->w,bt=bx?bx[4]:src->h;
        if(r>l&&bt>tp&&d3dgl_gpu_copy(dst,src,l,tp,r-l,bt-tp,dstX,dstY)) return; }
    if(!dst||!src||!dst->data||!src->data) return;
    dst->gl_dirty=1;
    if(dst==g_watch_tex || src==g_watch_tex){ uint64_t ss=0,ds=0; uint64_t soff=tex_sub_off(src->w,src->h,src->fmt,srcSub,&ss); uint64_t doff=tex_sub_off(dst->w,dst->h,dst->fmt,dstSub,&ds);
        int snz=0; const uint8_t*sp=src->data; for(uint64_t i=0;i<ss&&soff+i<src->size&&snz<49;i+=(ss/50?ss/50:1)) if(sp[soff+i])snz++;
        char m[200]; int k=snprintf(m,sizeof m,"  [WATCH] COPYSUB %s dst=%p(sub%u@%llu) <- src=%p %ux%u(sub%u@%llu sz%llu snz=%d/50) box=%s\n",dst==g_watch_tex?"IN":"OUT",(void*)dst,dstSub,(unsigned long long)doff,(void*)src,src->w,src->h,srcSub,(unsigned long long)soff,(unsigned long long)ss,snz,srcBox?"yes":"null"); (void)!write(2,m,k); }
    if(getenv("PELOAD_TRACE_TEXPATH") && dst->w>=512 && ((dst->fmt>=70&&dst->fmt<=84)||(dst->fmt>=94&&dst->fmt<=99))){ const uint32_t*bx=srcBox; char m[160]; int k=snprintf(m,sizeof m,"  [texpath] COPYSUB dst%ux%u f%u dstSub=%u srcSub=%u at(%u,%u) box=%s[%u %u %u %u]\n",dst->w,dst->h,dst->fmt,dstSub,srcSub,dstX,dstY,srcBox?"":"null",bx?bx[0]:0,bx?bx[1]:0,bx?bx[3]:0,bx?bx[4]:0); (void)!write(2,m,k); }
    // Textures: copy between real subresources (mip + slice*MipLevels) with each level's own pitch. BC
    // boxes are in pixels but move as 4x4 blocks; 3D boxes span Front..Back. (Old path: srcSub/dstSub were
    // mip-of-slice-0 and box copies always used mip 0's pitch -> streamed mips/faces/slices scrambled.)
    if(src->kind>=2&&src->kind<=4&&dst->kind>=2&&dst->kind<=4){
        uint64_t ssz,dsz; uint32_t srp,srows,smw,smh,smd, drp,drows,dmw,dmh,dmd;
        uint64_t sb=tex_sub(src,srcSub,&ssz,&srp,&srows,&smw,&smh,&smd), db=tex_sub(dst,dstSub,&dsz,&drp,&drows,&dmw,&dmh,&dmd);
        if(sb==UINT64_MAX||db==UINT64_MAX) return;
        int bc=is_bc(src->fmt); uint32_t bpe=bc?bc_block(src->fmt):dxgi_bpp(src->fmt), q=bc?4:1;
        const uint32_t*bx=srcBox; uint32_t l=bx?bx[0]:0, tp=bx?bx[1]:0, fr=bx?bx[2]:0, r=bx?bx[3]:smw, bt=bx?bx[4]:smh, bk=bx?bx[5]:smd;
        if(r>smw)r=smw; if(bt>smh)bt=smh; if(bk>smd)bk=smd; if(r<=l||bt<=tp||bk<=fr) return;
        uint32_t c0=l/q, c1=(r+q-1)/q, r0=tp/q, r1=(bt+q-1)/q, dc=dstX/q, dr=dstY/q; uint64_t rb=(uint64_t)(c1-c0)*bpe;
        for(uint32_t z=fr;z<bk;z++){ uint32_t dz=dstZ+(z-fr); if(dz>=dmd) break;
            for(uint32_t y=r0;y<r1;y++){ uint32_t dy=dr+(y-r0); if(dy>=drows) break;
                uint64_t so=sb+((uint64_t)z*srows+y)*srp+(uint64_t)c0*bpe, dO=db+((uint64_t)dz*drows+dy)*drp+(uint64_t)dc*bpe;
                uint64_t n=rb; if((uint64_t)dc*bpe+n>drp) n=drp>(uint64_t)dc*bpe?drp-(uint64_t)dc*bpe:0;
                if(!n || so+n>src->size || dO+n>dst->size) continue; memcpy((uint8_t*)dst->data+dO,(uint8_t*)src->data+so,(size_t)n); } }
        return; }
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
static void MSABI ctx_DrawIndexedInstanced(void*t,uint32_t a,uint32_t b,uint32_t c,int d,uint32_t e){ (void)t; d3dtrace(3,"DrawIndexedInstanced"); stg_note_draw(a); draw_caller("DrawIdxInst",__builtin_return_address(0),a); draw_census(); if(d3dgl_enabled() && b) d3dgl_draw(a,1,b,c,d,e); }
static void MSABI ctx_DrawInstanced(void*t,uint32_t a,uint32_t b,uint32_t c,uint32_t d){ (void)t; d3dtrace(2,"DrawInstanced"); stg_note_draw(a); draw_caller("DrawInst",__builtin_return_address(0),a); draw_census(); if(d3dgl_enabled() && b) d3dgl_draw(a,0,b,c,0,d); }
// ---- compute stage state (particles) ----
static struct { D3DObj*cs; D3DObj*srv[32]; D3DObj*uav[8]; D3DObj*cb[16]; uint32_t cbfirst[16]; D3DObj*samp[16]; } g_cs;
static void cscensus(const char*what,uint32_t a,uint32_t b,uint32_t c){ if(!getenv("PELOAD_CSCENSUS")) return;
    static unsigned long n; n++; if(n>400 && (n%500)) return;
    char m[300]; int k=snprintf(m,sizeof m,"  [cscensus] %s(%u,%u,%u) cs=%p bc=%u uav0=%p(%lluB stride %u) srv4=%p srv5=%p cb0=%p\n",what,a,b,c,(void*)g_cs.cs,g_cs.cs?g_cs.cs->bclen:0,
        g_cs.uav[0]?g_cs.uav[0]->data:0,g_cs.uav[0]&&g_cs.uav[0]->data?(unsigned long long)((D3DObj*)g_cs.uav[0]->data)->size:0ull,g_cs.uav[0]&&g_cs.uav[0]->data?((D3DObj*)g_cs.uav[0]->data)->depth:0,
        g_cs.srv[4]?g_cs.srv[4]->data:0,g_cs.srv[5]?g_cs.srv[5]->data:0,(void*)g_cs.cb[0]); (void)!write(2,m,k); }
// Dispatch -> run the compute shader on the CPU (dxbc_cpu.c) over the CPU copies of its buffers; written UAV
// buffers get a new version so their GL texture-buffer copies refresh before the particle draws read them.
static void MSABI ctx_Begin(void*t,void*a); static void MSABI ctx_End(void*t,void*a); static int MSABI ctx_GetData(void*t,void*a,void*data,uint32_t size,uint32_t flags); // fwd (queries)
// ---- asynchronous compute: Dispatch queues the CPU kernel on a worker thread so the particle simulation
// overlaps the opaque draws; the render thread waits only where it touches a buffer a queued job writes
// (texture-buffer upload for the particle draws, Map/Update/Copy/Clear). Constant buffers and read-only
// structured buffers are snapshotted at Dispatch time (the game rewrites them for the next dispatch).
// PELOAD_CS_SYNC runs every dispatch inline (old behaviour).
typedef struct CsQJob { CsProg*p; CsCtx cx; uint32_t a,b,c; void*own[64]; int nown; D3DObj* uavres[8]; struct CsQJob*next; } CsQJob;
static pthread_mutex_t g_csq_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_csq_cv=PTHREAD_COND_INITIALIZER, g_csq_done=PTHREAD_COND_INITIALIZER;
static CsQJob *g_csq_head,*g_csq_tail; static volatile int g_csq_n; static int g_csq_started;
static void* csq_worker(void*arg){ (void)arg; pthread_setname_np("shim-compute");
    for(;;){ pthread_mutex_lock(&g_csq_mx); while(!g_csq_head) pthread_cond_wait(&g_csq_cv,&g_csq_mx); CsQJob*j=g_csq_head; pthread_mutex_unlock(&g_csq_mx);
        uint64_t t0=prof_on()?prof_now():0;
        long rc=cs_dispatch(j->p,&j->cx,j->a,j->b,j->c);
        if(t0){ static uint64_t acc,nn,lacc,lins; acc+=prof_now()-t0; if((++nn%200)==0){ char m[160]; long ins=g_cs_instrs; int k=snprintf(m,sizeof m,"  [cs-cpu] %llu dispatches (async), last 200: %.1f us/dispatch, %.0f instrs/dispatch\n",(unsigned long long)nn,(acc-lacc)/1e3/200,(ins-lins)/200.0); (void)!write(2,m,k); lacc=acc; lins=ins; } }
        if(rc<0){ static int warned; if(warned++<8){ char m[120]; int k=snprintf(m,sizeof m,"  [cs-cpu] unsupported opcode %ld\n",-rc); (void)!write(2,m,k); } }
        pthread_mutex_lock(&g_csq_mx); g_csq_head=j->next; if(!g_csq_head) g_csq_tail=0; g_csq_n--;
        for(int i=0;i<8;i++) if(j->uavres[i]) j->uavres[i]->cs_pending--;
        pthread_cond_broadcast(&g_csq_done); pthread_mutex_unlock(&g_csq_mx);
        for(int i=0;i<j->nown;i++) free(j->own[i]); free(j); }
    return 0; }
static void cs_wait_res(D3DObj*r){ if(!r||!r->cs_pending) return; uint64_t t0=prof_on()?prof_now():0;
    pthread_mutex_lock(&g_csq_mx); while(r->cs_pending) pthread_cond_wait(&g_csq_done,&g_csq_mx); pthread_mutex_unlock(&g_csq_mx);
    if(t0){ static uint64_t acc,nn; acc+=prof_now()-t0; if((++nn%500)==0){ char m[100]; int k=snprintf(m,sizeof m,"  [cs-cpu] render thread waited %.0f us avg (%llu waits)\n",acc/1e3/nn,(unsigned long long)nn); (void)!write(2,m,k); } } }
static void cs_wait_view(void*v){ D3DObj*o=v; if(!o) return; cs_wait_res(o); if(o->vkind&&o->data) cs_wait_res((D3DObj*)o->data); }
static int cs_async(void){ static int a=-1; if(a<0) a=getenv("PELOAD_CS_SYNC")?0:1; return a; }
static void MSABI ctx_Dispatch(void*t,uint32_t a,uint32_t b,uint32_t c){ (void)t; d3dtrace(7,"Dispatch"); g_fs.disp++; cscensus("Dispatch",a,b,c);
    D3DObj*cs=g_cs.cs; if(!cs||!cs->bc||getenv("PELOAD_NOCSCPU")) return;
    if(!cs->csprog){ CsProg*p=cs_compile(cs->bc,cs->bclen); cs->csprog=p;
        char m[200]; int k=snprintf(m,sizeof m,"  [cs-cpu] compiled cs %p (%u B): %s, %d instrs, temps %d, numthreads(%u,%u,%u)\n",(void*)cs,cs->bclen,p&&p->ok?"ok":(p?p->why:"oom"),p?p->nins:0,p?p->ntemps:0,p?p->tg[0]:0,p?p->tg[1]:0,p?p->tg[2]:0); (void)!write(2,m,k); }
    CsProg*p=cs->csprog; if(!p||!p->ok) return;
    if((uint64_t)a*b*c*p->tg[0]*p->tg[1]*p->tg[2] > (1u<<20)) return;   // runaway guard
    if(cs_async()){
        CsQJob*j=calloc(1,sizeof *j); if(!j) return; j->p=p; j->a=a; j->b=b; j->c=c;
        for(int i=0;i<16;i++){ D3DObj*cb=g_cs.cb[i]; if(cb&&cb->data&&cb->size){ uint64_t off=(uint64_t)g_cs.cbfirst[i]*16; if(off>=cb->size) off=0;
                uint64_t n=cb->size-off; if(n>65536) n=65536; uint8_t*cp=malloc(n); if(!cp||j->nown>=64){ free(cp); continue; } memcpy(cp,(const uint8_t*)cb->data+off,n); j->own[j->nown++]=cp; j->cx.cb[i]=cp; j->cx.cbsz[i]=(uint32_t)n; } }
        for(int i=0;i<32;i++){ D3DObj*v=g_cs.srv[i]; if(!v) continue; D3DObj*r=(v->data&&v->vkind)?(D3DObj*)v->data:v; j->cx.srv[i]=v;
            if(r && r->kind==1 && r->data && r->size && r->size<=(8u<<20) && !r->cs_pending && j->nown<62){   // read-only buffer: snapshot
                D3DObj*sh=malloc(sizeof(D3DObj)); uint8_t*d=malloc(r->size); if(!sh||!d){ free(sh); free(d); continue; }
                *sh=*r; sh->vkind=0; sh->data=d; memcpy(d,r->data,r->size); j->own[j->nown++]=sh; j->own[j->nown++]=d; j->cx.srv[i]=sh; } }
        for(int i=0;i<16;i++) j->cx.samp[i]=g_cs.samp[i];
        pthread_mutex_lock(&g_csq_mx);
        for(int i=0;i<8;i++){ D3DObj*v=g_cs.uav[i]; j->cx.uav[i]=v; D3DObj*r=(v&&v->data&&v->vkind)?(D3DObj*)v->data:v;
            if(r){ r->cs_pending++; j->uavres[i]=r; r->ver++; r->gl_dirty=1; r->rvalid=0; } }
        if(g_csq_tail) g_csq_tail->next=j; else g_csq_head=j; g_csq_tail=j; g_csq_n++;
        if(!g_csq_started){ g_csq_started=1; pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_set_qos_class_np(&at,QOS_CLASS_USER_INTERACTIVE,0); pthread_create(&th,&at,csq_worker,0); }
        pthread_cond_signal(&g_csq_cv); pthread_mutex_unlock(&g_csq_mx);
        return; }
    CsCtx cx; memset(&cx,0,sizeof cx);
    for(int i=0;i<16;i++){ D3DObj*cb=g_cs.cb[i]; if(cb&&cb->data&&cb->size){ uint64_t off=(uint64_t)g_cs.cbfirst[i]*16; if(off>=cb->size) off=0; cx.cb[i]=(const uint8_t*)cb->data+off; cx.cbsz[i]=(uint32_t)(cb->size-off); } }
    for(int i=0;i<32;i++) cx.srv[i]=g_cs.srv[i];
    for(int i=0;i<8;i++) cx.uav[i]=g_cs.uav[i];
    for(int i=0;i<16;i++) cx.samp[i]=g_cs.samp[i];
    uint64_t t0=prof_on()?prof_now():0;
    long rc=cs_dispatch(p,&cx,a,b,c);
    if(t0){ static uint64_t acc,nn,lacc,lins; acc+=prof_now()-t0; if((++nn%200)==0){ char m[160]; long ins=g_cs_instrs; int k=snprintf(m,sizeof m,"  [cs-cpu] %llu dispatches, avg %.1f us | last 200: %.1f us/dispatch, %.0f instrs/dispatch, %.1f ns/instr\n",(unsigned long long)nn,acc/1e3/nn,(acc-lacc)/1e3/200,(ins-lins)/200.0,(ins-lins)?(double)(acc-lacc)/(double)(ins-lins):0.0); (void)!write(2,m,k); lacc=acc; lins=ins; } }
    if(rc<0){ static int warned; if(warned++<8){ char m[120]; int k=snprintf(m,sizeof m,"  [cs-cpu] unsupported opcode %ld in cs %p\n",-rc,(void*)cs); (void)!write(2,m,k); } return; }
    for(int i=0;i<8;i++){ D3DObj*v=g_cs.uav[i]; D3DObj*r=(v&&v->data&&v->vkind)?(D3DObj*)v->data:v; if(r){ r->ver++; r->gl_dirty=1; r->rvalid=0; } } }
static void MSABI ctx_DispatchIndirect(void*t,void*args,uint32_t off){ (void)t;(void)args; cscensus("DispatchIndirect",off,0,0); }
static void stg_note_indirect(const char*what,uint32_t off){ if(!getenv("PELOAD_TRACE_STAGES")) return; static unsigned long n; n++;
    if(n<=20 || !(n%2000)){ char m[220]; int k=snprintf(m,sizeof m,"  [stages] %s #%lu off=%u vs=%016llx ps=%016llx\n",what,n,off,
        (unsigned long long)(g_d3dst.vs?g_d3dst.vs->bchash:0),(unsigned long long)(g_d3dst.ps?g_d3dst.ps->bchash:0)); (void)!write(2,m,k); } }
static void MSABI ctx_DrawInstancedIndirect(void*t,void*args,uint32_t off){ (void)t;(void)args; cscensus("DrawInstancedIndirect",off,0,0); stg_note_indirect("DrawInstancedIndirect",off); }
static void MSABI ctx_DrawIndexedInstancedIndirect(void*t,void*args,uint32_t off){ (void)t;(void)args; cscensus("DrawIndexedInstancedIndirect",off,0,0); stg_note_indirect("DrawIndexedInstancedIndirect",off); }
static void MSABI ctx_CopyStructureCount(void*t,void*dst,uint32_t off,void*uav){ (void)t;(void)dst;(void)uav; cscensus("CopyStructureCount",off,0,0); }
static void MSABI ctx_CSSetShader(void*t,void*cs,void*ci,uint32_t n){ (void)t;(void)ci;(void)n; g_cs.cs=cs; }
// Geometry / hull / domain shader stages: tracked so draws that use them can be found (PELOAD_TRACE_STAGES); the GL
// translation only runs VS+PS.
static struct { D3DObj *gs,*hs,*ds; void* so; unsigned long ngs,nhs,nso,nall; } g_stg;
static void MSABI ctx_GSSetShader(void*t,void*sh,void*ci,uint32_t n){ (void)t;(void)ci;(void)n; g_stg.gs=(D3DObj*)sh; }
static void MSABI ctx_HSSetShader(void*t,void*sh,void*ci,uint32_t n){ (void)t;(void)ci;(void)n; g_stg.hs=(D3DObj*)sh; }
static void MSABI ctx_DSSetShader(void*t,void*sh,void*ci,uint32_t n){ (void)t;(void)ci;(void)n; g_stg.ds=(D3DObj*)sh; }
static void MSABI ctx_SOSetTargets(void*t,uint32_t n,void*const*b,const uint32_t*o){ (void)t;(void)o; g_stg.so=(n&&b)?b[0]:0; }
static void stg_note_draw(uint32_t n){ static int on=-1; if(on<0) on=getenv("PELOAD_TRACE_STAGES")?1:0; if(!on) return;
    g_stg.nall++; if(g_stg.gs) g_stg.ngs++; if(g_stg.hs||g_stg.ds) g_stg.nhs++; if(g_stg.so) g_stg.nso++;
    if((g_stg.gs||g_stg.hs||g_stg.ds||g_stg.so)){ static int shown; if(shown<40){ shown++; char m[256];
        int k=snprintf(m,sizeof m,"  [stages] draw n=%u topo=%u vs=%016llx ps=%016llx gs=%016llx hs=%016llx ds=%016llx so=%p\n",n,g_d3dst.topo,
            (unsigned long long)(g_d3dst.vs?g_d3dst.vs->bchash:0),(unsigned long long)(g_d3dst.ps?g_d3dst.ps->bchash:0),(unsigned long long)(g_stg.gs?g_stg.gs->bchash:0),
            (unsigned long long)(g_stg.hs?g_stg.hs->bchash:0),(unsigned long long)(g_stg.ds?g_stg.ds->bchash:0),g_stg.so); (void)!write(2,m,k); } }
    static double t0; double now=mono_s(); if(now-t0>5){ t0=now; char m[160]; int k=snprintf(m,sizeof m,"  [stages] draws %lu: with GS %lu, HS/DS %lu, stream-out %lu\n",g_stg.nall,g_stg.ngs,g_stg.nhs,g_stg.nso); (void)!write(2,m,k); } }
static void MSABI ctx_CSSetShaderResources(void*t,uint32_t st,uint32_t n,void*const*v){ (void)t; for(uint32_t i=0;i<n&&st+i<32;i++) g_cs.srv[st+i]=v?(D3DObj*)v[i]:0; }
static void MSABI ctx_CSSetSamplers(void*t,uint32_t st,uint32_t n,void*const*v){ (void)t; for(uint32_t i=0;i<n&&st+i<16;i++) g_cs.samp[st+i]=v?(D3DObj*)v[i]:0; }
static void MSABI ctx_CSSetConstantBuffers1(void*t,uint32_t st,uint32_t n,void*const*v,const uint32_t*first,const uint32_t*cnt){ (void)t;(void)cnt;
    for(uint32_t i=0;i<n&&st+i<16;i++){ g_cs.cb[st+i]=v?(D3DObj*)v[i]:0; g_cs.cbfirst[st+i]=first?first[i]:0; } }
// ClearUnorderedAccessView{Uint,Float} on buffers (particle pools are reset this way)
static void cs_clear_uav(void*view,uint32_t v0){ D3DObj*v=view; D3DObj*r=(v&&v->data&&v->vkind)?(D3DObj*)v->data:v; if(!r||!r->data||r->kind!=1) return;
    cs_wait_res(r);
    uint32_t*d=(uint32_t*)r->data; for(uint64_t i=0;i<r->size/4;i++) d[i]=v0; r->ver++; r->gl_dirty=1; r->rvalid=0; }
static void MSABI ctx_ClearUAVUint(void*t,void*uav,const uint32_t*vals){ (void)t; cs_clear_uav(uav,vals?vals[0]:0); }
static void MSABI ctx_ClearUAVFloat(void*t,void*uav,const float*vals){ (void)t; uint32_t b=0; if(vals) memcpy(&b,vals,4); cs_clear_uav(uav,b); }
static void MSABI ctx_CSSetUAVs(void*t,uint32_t st,uint32_t n,void*const*v,const uint32_t*cnt){ (void)t;(void)cnt; for(uint32_t i=0;i<n&&st+i<8;i++) g_cs.uav[st+i]=v?(D3DObj*)v[i]:0; }
static void MSABI ctx_CSSetConstantBuffers(void*t,uint32_t st,uint32_t n,void*const*v){ (void)t; for(uint32_t i=0;i<n&&st+i<16;i++){ g_cs.cb[st+i]=v?(D3DObj*)v[i]:0; g_cs.cbfirst[st+i]=0; } }
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
    for(uint32_t i=0;i<num&&start+i<32;i++){ D3DObj*v=srvs?(D3DObj*)srvs[i]:0; g_d3dst.vssrv[start+i]=(v&&v->data)?(D3DObj*)v->data:v; g_d3dst.vssrvv[start+i]=(v&&v->vkind)?v:0; if(srvs&&srvs[i])g_iacen.vs_srv++; } }
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
D3DObj* g_last_smallcb;  // most recent small (32-96B) CB mapped/updated — candidate vertex-decompress cb1
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
    for(uint32_t i=0;i<num&&start+i<32;i++){ D3DObj*v=srvs?(D3DObj*)srvs[i]:0; g_d3dst.psrv[start+i]=(v&&v->data)?(D3DObj*)v->data:v; g_d3dst.psrvv[start+i]=(v&&v->vkind)?v:0; } }
static void MSABI ctx_OMSetRenderTargets(void*t,uint32_t num,void*const*rtvs,void*dsv){ (void)t;
    g_d3dst.num_rt=0;
    for(uint32_t i=0;i<num&&i<8;i++){ D3DObj*v=rtvs?(D3DObj*)rtvs[i]:0; D3DObj*res=(v&&v->data)?(D3DObj*)v->data:v; g_d3dst.rtvs[i]=res; g_d3dst.rtvv[i]=(v&&v->vkind)?v:0; if(res)g_d3dst.num_rt=i+1; }
    for(uint32_t i=num;i<8;i++){ g_d3dst.rtvs[i]=0; g_d3dst.rtvv[i]=0; }
    g_d3dst.rtv=g_d3dst.rtvs[0];
    { D3DObj*d=(D3DObj*)dsv; g_d3dst.dsv=(d&&d->data)?(D3DObj*)d->data:d; } }
static void MSABI ctx_RSSetViewports(void*t,uint32_t num,const void*vps){ (void)t; if(num&&vps){ const float*f=vps; g_d3dst.vp_x=(int)f[0]; g_d3dst.vp_y=(int)f[1]; g_d3dst.vp_w=(int)f[2]; g_d3dst.vp_h=(int)f[3];
    g_d3dst.vp_zn=f[4]; g_d3dst.vp_zf=f[5]; g_d3dst.vp_set=1; } }
static void MSABI ctx_OMSetBlendState(void*t,void*bs,const float*bf,uint32_t mask){ (void)t;(void)mask; g_d3dst.blend=bs;
    for(int i=0;i<4;i++) g_d3dst.blendf[i]=bf?bf[i]:1.0f; }
static void MSABI ctx_OMSetDepthStencilState(void*t,void*ds,uint32_t ref){ (void)t; g_d3dst.depth=ds; g_d3dst.sref=ref; }
static void MSABI ctx_RSSetState(void*t,void*rs){ (void)t; g_d3dst.raster=rs; }
static void MSABI ctx_RSSetScissorRects(void*t,uint32_t num,const int32_t*r){ (void)t; if(num&&r){ g_d3dst.sc_l=r[0]; g_d3dst.sc_t=r[1]; g_d3dst.sc_r=r[2]; g_d3dst.sc_b=r[3]; } }
static void MSABI ctx_PSSetSamplers(void*t,uint32_t st,uint32_t n,void*const*ss){ (void)t; for(uint32_t i=0;i<n&&st+i<16;i++) g_d3dst.pssamp[st+i]=ss?(D3DObj*)ss[i]:0; }
static void MSABI ctx_VSSetSamplers(void*t,uint32_t st,uint32_t n,void*const*ss){ (void)t; for(uint32_t i=0;i<n&&st+i<16;i++) g_d3dst.vssamp[st+i]=ss?(D3DObj*)ss[i]:0; }
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
            if(c13p&&c13p->data&&c13p->size>=16){ const float*fp=(const float*)c13p->data; int nv=(int)(c13p->size/16); if(nv>10)nv=10;
                ck+=snprintf(cm+ck,sizeof cm-ck," pscb13[0..%d]=",nv-1); for(int v=0;v<nv&&ck<(int)sizeof cm-40;v++) ck+=snprintf(cm+ck,sizeof cm-ck,"[%.2f %.2f %.2f %.2f]",fp[v*4],fp[v*4+1],fp[v*4+2],fp[v*4+3]); }
            ck+=snprintf(cm+ck,sizeof cm-ck,"\n"); (void)!write(2,cm,ck);
            // VERTEX FORMAT + sample: is POSITION compressed (16-bit, needs cb3 decompress) or float3?
            // Interpret vertex 0's first bytes both ways so we can tell which path the geometry needs.
            D3DObj*il=g_d3dst.il, *vb=g_d3dst.vb[0]; uint32_t vstr=g_d3dst.vstride[0];
            char vm2[380]; int vk=snprintf(vm2,sizeof vm2,"  [vtxfmt] stride=%u elems=%d:",vstr,il?il->nelem:0);
            if(il) for(int e=0;e<il->nelem&&vk<260;e++) vk+=snprintf(vm2+vk,sizeof vm2-vk," %s@%u/%s",il->elem[e].sem,il->elem[e].off,dxgi_fmt_name(il->elem[e].fmt));
            if(vb&&vb->data&&vstr>=6){ const uint8_t*v0=(const uint8_t*)vb->data; const float*ff=(const float*)v0; const int16_t*ss=(const int16_t*)v0;
                vk+=snprintf(vm2+vk,sizeof vm2-vk," | v0 asF32=[%.2f %.2f %.2f] asI16n=[%.3f %.3f %.3f]",ff[0],ff[1],ff[2],ss[0]/32767.0f,ss[1]/32767.0f,ss[2]/32767.0f); }
            vk+=snprintf(vm2+vk,sizeof vm2-vk,"\n"); (void)!write(2,vm2,vk);
            // ALL VS cbuffer slots for this compressed draw: which are bound + first 2 vec4 (find cb1 =
            // the vertex-decompression scale/offset, needed to un-collapse the [-1,1] compressed positions).
            char vb2[420]; int vk2=snprintf(vb2,sizeof vb2,"  [vscbs]"); for(int b=0;b<16;b++){ D3DObj*cc=g_d3dst.vscb[b]; if(!cc)continue;
                vk2+=snprintf(vb2+vk2,sizeof vb2-vk2," b%d=%lluB",b,(unsigned long long)cc->size);
                if(cc->data&&cc->size>=32&&vk2<340){ const float*ff=(const float*)cc->data; vk2+=snprintf(vb2+vk2,sizeof vb2-vk2,"[%.2f %.2f %.2f %.2f|%.2f %.2f %.2f %.2f]",ff[0],ff[1],ff[2],ff[3],ff[4],ff[5],ff[6],ff[7]); } }
            vk2+=snprintf(vb2+vk2,sizeof vb2-vk2,"\n"); (void)!write(2,vb2,vk2);
            // sample the bound textures' raw data: how many nonzero bytes (black vs has-content) + the
            // GL-uploaded flag + format, for slots 0..8 (the terrain blend/detail textures)
            char tm[400]; int tk2=snprintf(tm,sizeof tm,"  [texdata]"); for(int r=0;r<9;r++){ D3DObj*t=g_d3dst.psrv[r];
                if(!t){ tk2+=snprintf(tm+tk2,sizeof tm-tk2," t%d=-",r); continue; }
                int nz=0; if(t->data&&t->size>=1024){ const uint8_t*d=t->data; uint64_t step=t->size/2048; if(step<1)step=1; for(uint64_t i=0;i<t->size&&nz<40;i+=step) if(d[i])nz++; }
                // for small (1x1/4x4) multiplier textures, show the first texel bytes (the value actually sampled)
                char px[24]=""; if(t->data&&t->w<=4&&t->size>=4){ const uint8_t*d=t->data; snprintf(px,sizeof px,"=%02x%02x%02x%02x",d[0],d[1],d[2],d[3]); }
                tk2+=snprintf(tm+tk2,sizeof tm-tk2," t%d=%ux%u(f%u,%lluB,nz%d,gl%u%s)",r,t->w,t->h,t->fmt,(unsigned long long)t->size,nz,t->gltex,px); }
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
unsigned long g_ct_frame;
static void flog_present(void);
static void fps_title_tick(void); // fwd: engine fps + draws/frame in the window title
static int MSABI swap_Present(void*t,uint32_t sync,uint32_t fl){ (void)sync;(void)fl; d3dtrace(4,"Present"); flog("t%x PRESENT swapchain=%p",pthread_mach_thread_np(pthread_self()),t); g_ct_frame++; g_hb_taken=0; flog_present(); fps_title_tick();
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
// Swap-chain size: 1920x1080 (the engine's render size). It used to be a 4096x4096 placeholder, which
// Reclaimer's own UI (the frontend menu and in-game overlays, composited into the back buffer with a
// viewport of the back buffer's size) laid out for a 4096^2 screen. PELOAD_SWAP_SIZE=<w>x<h> overrides;
// PELOAD_SWAP_SIZE=4096x4096 restores the old placeholder.
static void swap_size(uint32_t*w,uint32_t*h){ static uint32_t W,H; if(!W){ W=1920; H=1080; const char*e=getenv("PELOAD_SWAP_SIZE");
        if(e){ unsigned a=0,b=0; if(sscanf(e,"%ux%u",&a,&b)==2 && a>=320 && b>=200 && a<=4096 && b<=4096){ W=a; H=b; } } } *w=W; *h=H; }
static D3DObj* swap_backbuf(void){ if(!g_d3d_backbuf){ uint32_t w,h; swap_size(&w,&h); g_d3d_backbuf=mktex(w,h,1,28,2); } return g_d3d_backbuf; }
static int MSABI swap_GetBuffer(void*t,uint32_t buf,GUID_*r,void**pp){ (void)t;(void)buf;(void)r;
    swap_backbuf();
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
    { uint32_t w,h; swap_size(&w,&h); *(uint32_t*)(d+0)=w; *(uint32_t*)(d+4)=h; } *(uint32_t*)(d+8)=60; *(uint32_t*)(d+12)=1;
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
    { void** q=g_d3dquery_vtbl; for(int i=0;i<16;i++) q[i]=c[i]; q[7]=(void*)query_GetDataSize; q[8]=(void*)query_GetDesc; }
    void** r=g_d3dres_vtbl; for(int i=0;i<7;i++) r[i]=c[i]; r[7]=(void*)res_GetType;r[8]=(void*)res_SetEvict;r[9]=(void*)res_GetEvict;r[10]=(void*)res_GetDesc; for(int i=11;i<16;i++) r[i]=(void*)ctx_ret0;
    void** v=g_d3dview_vtbl; for(int i=0;i<7;i++) v[i]=c[i]; v[7]=(void*)view_GetResource;v[8]=(void*)view_GetDesc; for(int i=9;i<16;i++) v[i]=(void*)ctx_ret0;
    g_d3ddummyres_obj.vtbl=g_d3dres_vtbl; g_d3ddummyres_obj.kind=1; g_d3ddummyres_obj.size=1<<16; g_d3ddummyres_obj.data=calloc(1,1<<16); g_d3ddummyres=&g_d3ddummyres_obj;
    void** d=g_d3ddev_vtbl;
    d[0]=(void*)dev_QI;d[1]=(void*)d3d_AddRef;d[2]=(void*)d3d_Release;   // per-IID QI (device/dxgi device/context)
    d[3]=(void*)dev_CreateBuffer;d[4]=(void*)dev_CreateTexture1D;d[5]=(void*)dev_CreateTexture2D;d[6]=(void*)dev_CreateTexture3D;
    d[7]=(void*)dev_crSRV;d[8]=(void*)dev_crUAV;d[9]=(void*)dev_crRTV;d[10]=(void*)dev_crDSV; // SRV/UAV/RTV/DSV
    d[11]=(void*)dev_crIL;                                                                            // CreateInputLayout
    d[12]=(void*)dev_crVS;d[13]=(void*)dev_crGS;d[14]=(void*)dev_crGSSO;d[15]=(void*)dev_crPS;d[16]=(void*)dev_crHS;d[17]=(void*)dev_crDS;d[18]=(void*)dev_crCS; // 12=VS,13=GS,14=GSSO,15=PS,16=HS,17=DS,18=CS
    d[19]=(void*)dev_cr1;                                                                             // CreateClassLinkage
    d[20]=(void*)dev_crBlend;d[21]=(void*)dev_crDSS;d[22]=(void*)dev_crRS;d[23]=(void*)dev_crSamp;d[24]=(void*)dev_crQuery;d[25]=(void*)dev_crQuery;d[26]=(void*)dev_cr2; // Blend,DSS,RS,Sampler,Query,Predicate,Counter
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
    x[39]=(void*)ctx_DrawIndexedInstancedIndirect; x[40]=(void*)ctx_DrawInstancedIndirect; x[42]=(void*)ctx_DispatchIndirect; x[49]=(void*)ctx_CopyStructureCount;
    x[23]=(void*)ctx_GSSetShader; x[60]=(void*)ctx_HSSetShader; x[64]=(void*)ctx_DSSetShader; x[37]=(void*)ctx_SOSetTargets;
    x[67]=(void*)ctx_CSSetShaderResources; x[68]=(void*)ctx_CSSetUAVs; x[69]=(void*)ctx_CSSetShader; x[71]=(void*)ctx_CSSetConstantBuffers;
    x[70]=(void*)ctx_CSSetSamplers; x[124]=(void*)ctx_CSSetConstantBuffers1; x[51]=(void*)ctx_ClearUAVUint; x[52]=(void*)ctx_ClearUAVFloat;
    x[7]=(void*)ctx_VSSetConstantBuffers; x[9]=(void*)ctx_PSSetShader; x[11]=(void*)ctx_VSSetShader; x[16]=(void*)ctx_PSSetConstantBuffers;
    x[8]=(void*)ctx_PSSetShaderResources; x[17]=(void*)ctx_IASetInputLayout; x[18]=(void*)ctx_IASetVertexBuffers;
    x[25]=(void*)ctx_VSSetShaderResources;   // VS-stage SRVs (vertex-pulling geometry source)
    x[19]=(void*)ctx_IASetIndexBuffer; x[24]=(void*)ctx_IASetPrimitiveTopology; x[33]=(void*)ctx_OMSetRenderTargets; x[44]=(void*)ctx_RSSetViewports;
    x[35]=(void*)ctx_OMSetBlendState; x[36]=(void*)ctx_OMSetDepthStencilState; x[43]=(void*)ctx_RSSetState; x[45]=(void*)ctx_RSSetScissorRects;
    x[10]=(void*)ctx_PSSetSamplers; x[26]=(void*)ctx_VSSetSamplers;
    x[27]=(void*)ctx_Begin; x[28]=(void*)ctx_End; x[29]=(void*)ctx_GetData;   // ID3D11Asynchronous (queries)
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
// ---- REAL ID3D11ShaderReflection over the DXBC RDEF chunk (PELOAD_REFLECT; default ON with PELOAD_D3DGL)
// halo3's ONLY D3DReflect caller is sub_1802af4b8 (halo3+0x2af4ef), the constant-registry builder:
//  1. GetDesc -> ConstantBuffers(@+20) / BoundResources(@+24)
//  2. per cbuffer: GetConstantBufferByIndex -> cb->GetDesc (Name@0 Type@8 Size@16) -> sub_1802af310(name)
//     = registry id -> sub_1802af0ec(&g_182d3a470[48*id], Size) = CreateBuffer(DYNAMIC, CONSTANT_BUFFER)
//  3. per bound resource: GetResourceBindingDesc (Name@0 Type@8 BindPoint@12) -> slot->registry table
// The per-draw flush sub_1802af8e8 then Map/Unmaps each dirty registry cbuffer and calls
// VS/PS/CSSetConstantBuffers(0,N,table). With the old all-zero stub the table was EMPTY, so cb0 (view-proj),
// cb1 (vertex decompression) and the PS lighting cbuffers were NEVER bound -- the root of the black frame,
// the synthesized camera (SYNVP), the decompression capture (CAPCB1) and the flat-gray shading.
typedef struct ReflS ReflS;
typedef struct { void** vtbl; ReflS* owner; uint32_t idx; } ReflCBObj;
struct ReflS { void** vtbl; int refs; uint8_t* r; uint32_t rsz; uint32_t ncb,cboff,nrb,rboff,rbstride,ver,creatoroff; ReflCBObj cbs[64]; };
static uint32_t rf_u32(const ReflS*s,uint32_t o){ return (o+4<=s->rsz)?*(const uint32_t*)(s->r+o):0; }
static const char* rf_str(const ReflS*s,uint32_t o){ return (o<s->rsz)?(const char*)(s->r+o):""; }
static int g_reflect_on=-1;
static int reflect_enabled(void){ if(g_reflect_on<0){ const char*e=getenv("PELOAD_REFLECT"); g_reflect_on = e?(atoi(e)!=0):(getenv("PELOAD_D3DGL")!=0); } return g_reflect_on; }
static int MSABI reflcb_GetDesc(void*self,void*d){ ReflCBObj*o=(ReflCBObj*)self; if(!o||!o->owner||!d) return (int)0x80004005;
    const ReflS*s=o->owner; uint32_t e=s->cboff+o->idx*24;
    *(const char**)((char*)d+0)=rf_str(s,rf_u32(s,e+0));  // Name
    *(uint32_t*)((char*)d+8)=rf_u32(s,e+20);              // Type (D3D_CBUFFER_TYPE)
    *(uint32_t*)((char*)d+12)=rf_u32(s,e+4);              // Variables
    *(uint32_t*)((char*)d+16)=rf_u32(s,e+12);             // Size
    *(uint32_t*)((char*)d+20)=rf_u32(s,e+16);             // uFlags
    return HR_OK; }
static int MSABI reflcb_null_GetDesc(void*self,void*d){ (void)self;(void)d; return (int)0x80004005; }
static void* MSABI reflcb_null_var(void*self,uintptr_t a){ (void)self;(void)a; return 0; }
static void* g_reflcb_vtbl[3]={ (void*)reflcb_GetDesc,(void*)reflcb_null_var,(void*)reflcb_null_var };
static void* g_reflcb_null_vtbl[3]={ (void*)reflcb_null_GetDesc,(void*)reflcb_null_var,(void*)reflcb_null_var };
static ReflCBObj g_reflcb_null={ g_reflcb_null_vtbl,0,0 };
static int MSABI refl2_QI(void*self,void*iid,void**pp){ (void)iid; if(pp)*pp=self; ((ReflS*)self)->refs++; return HR_OK; }
static unsigned MSABI refl2_AddRef(void*self){ return (unsigned)++((ReflS*)self)->refs; }
static unsigned MSABI refl2_Release(void*self){ ReflS*s=(ReflS*)self; int r=--s->refs; if(r<=0){ free(s->r); free(s); return 0; } return (unsigned)r; }
static int MSABI refl2_GetDesc(void*self,void*d){ ReflS*s=(ReflS*)self; if(!d) return (int)0x80070057;
    memset(d,0,160);                                        // sizeof(D3D11_SHADER_DESC) on x64
    *(uint32_t*)((char*)d+0)=s->ver; *(const char**)((char*)d+8)=rf_str(s,s->creatoroff);
    *(uint32_t*)((char*)d+20)=s->ncb; *(uint32_t*)((char*)d+24)=s->nrb; return HR_OK; }
static void* MSABI refl2_GetCBByIndex(void*self,uint32_t i){ ReflS*s=(ReflS*)self; return (i<s->ncb)?(void*)&s->cbs[i]:(void*)&g_reflcb_null; }
static void* MSABI refl2_GetCBByName(void*self,const char*nm){ ReflS*s=(ReflS*)self;
    for(uint32_t i=0;i<s->ncb;i++) if(nm && !strcmp(rf_str(s,rf_u32(s,s->cboff+i*24)),nm)) return &s->cbs[i]; return &g_reflcb_null; }
static int refl2_fill_rb(const ReflS*s,uint32_t i,void*d){ if(i>=s->nrb||!d) return (int)0x80070057; uint32_t e=s->rboff+i*s->rbstride;
    *(const char**)((char*)d+0)=rf_str(s,rf_u32(s,e+0));  // Name
    *(uint32_t*)((char*)d+8)=rf_u32(s,e+4);               // Type (D3D_SHADER_INPUT_TYPE; 0 = CBUFFER)
    *(uint32_t*)((char*)d+12)=rf_u32(s,e+20);             // BindPoint
    *(uint32_t*)((char*)d+16)=rf_u32(s,e+24);             // BindCount
    *(uint32_t*)((char*)d+20)=rf_u32(s,e+28);             // uFlags
    *(uint32_t*)((char*)d+24)=rf_u32(s,e+8);              // ReturnType
    *(uint32_t*)((char*)d+28)=rf_u32(s,e+12);             // Dimension
    *(uint32_t*)((char*)d+32)=rf_u32(s,e+16);             // NumSamples
    return HR_OK; }
static int MSABI refl2_GetRBDesc(void*self,uint32_t i,void*d){ return refl2_fill_rb((ReflS*)self,i,d); }
static int MSABI refl2_GetRBDescByName(void*self,const char*nm,void*d){ ReflS*s=(ReflS*)self;
    for(uint32_t i=0;i<s->nrb;i++) if(nm && !strcmp(rf_str(s,rf_u32(s,s->rboff+i*s->rbstride)),nm)) return refl2_fill_rb(s,i,d); return (int)0x80070057; }
static void* g_refl2_vtbl[40];
static ReflS* refl_build(const void*src,uint64_t sz){
    const uint8_t*d=(const uint8_t*)src; if(!d||sz<32||memcmp(d,"DXBC",4)) return 0;
    uint32_t nch=*(const uint32_t*)(d+28); if(nch>64) return 0;
    for(uint32_t c=0;c<nch;c++){ if(32+4*c+4>sz) break; uint32_t o=*(const uint32_t*)(d+32+4*c); if((uint64_t)o+8>sz) continue;
        if(memcmp(d+o,"RDEF",4)) continue;
        uint32_t csz=*(const uint32_t*)(d+o+4); if((uint64_t)o+8+csz>sz || csz<28) return 0;
        ReflS*s=calloc(1,sizeof*s); if(!s) return 0; s->r=malloc(csz+1); if(!s->r){ free(s); return 0; }
        memcpy(s->r,d+o+8,csz); s->r[csz]=0; s->rsz=csz; s->refs=1; s->vtbl=g_refl2_vtbl;
        s->ncb=rf_u32(s,0); s->cboff=rf_u32(s,4); s->nrb=rf_u32(s,8); s->rboff=rf_u32(s,12);
        uint32_t vt=rf_u32(s,16); unsigned minor=vt&0xff, major=(vt>>8)&0xff, pt=vt>>16; s->creatoroff=rf_u32(s,24);
        s->rbstride=(major>5||(major==5&&minor>=1))?40:32;
        unsigned ty = pt==0xFFFF?0: pt==0xFFFE?1: pt==0x4753?2: pt==0x4853?3: pt==0x4453?4: pt==0x4353?5: 0;
        s->ver=(ty<<16)|(major<<4)|minor;
        if(s->ncb>64) s->ncb=64;
        if((uint64_t)s->cboff+(uint64_t)s->ncb*24>csz) s->ncb=0;
        if((uint64_t)s->rboff+(uint64_t)s->nrb*s->rbstride>csz) s->nrb=0;
        for(uint32_t i=0;i<s->ncb;i++){ s->cbs[i].vtbl=g_reflcb_vtbl; s->cbs[i].owner=s; s->cbs[i].idx=i; }
        if(getenv("PELOAD_TRACE_REFLECT")){ static int tn; if(tn++<400){ char m[256]; int k=snprintf(m,sizeof m,"  [reflect] sm%u.%u type=%u cbs=%u res=%u:",major,minor,ty,s->ncb,s->nrb);
            for(uint32_t i=0;i<s->ncb && k<200;i++) k+=snprintf(m+k,sizeof m-k," %s(%uB)",rf_str(s,rf_u32(s,s->cboff+i*24)),rf_u32(s,s->cboff+i*24+12));
            for(uint32_t i=0;i<s->nrb && k<240;i++){ uint32_t e=s->rboff+i*s->rbstride; if(rf_u32(s,e+4)==0) k+=snprintf(m+k,sizeof m-k," [%s->b%u]",rf_str(s,rf_u32(s,e)),rf_u32(s,e+20)); }
            if(k>(int)sizeof m-2)k=sizeof m-2; m[k++]='\n'; (void)!write(2,m,k);} }
        return s; }
    return 0; }
static void shader_rname(D3DObj*o){ if(!o||!o->bc) return; ReflS*R=refl_build(o->bc,o->bclen); if(!R){ snprintf(o->rname,sizeof o->rname,"(norefl)"); return; }
    int k=0; for(uint32_t i=0;i<R->nrb && k<(int)sizeof o->rname-2;i++){ uint32_t e=R->rboff+i*R->rbstride; uint32_t ty=rf_u32(R,e+4); if(ty==3) continue;
        k+=snprintf(o->rname+k,sizeof o->rname-k,"%s%s",k?",":"",rf_str(R,rf_u32(R,e))); }
    free(R->r); free(R); }
// PELOAD_FRAMELOG=<present#>: ring of the last 64 D3D events of a frame (draws/copies), dumped at that Present.
enum { FLOGN=4096 }; static char g_flog[FLOGN][160]; static unsigned g_flog_n;
static void flog(const char*fmt,...){ if(!getenv("PELOAD_FRAMELOG")) return; va_list ap; va_start(ap,fmt); vsnprintf(g_flog[g_flog_n%FLOGN],160,fmt,ap); va_end(ap); g_flog_n++; }
static void flog_present(void){ static unsigned long pc; pc++; const char*e=getenv("PELOAD_FRAMELOG"); if(!e) return;
    unsigned long every=strtoul(e,0,10); static int ndump; if(every && (pc%every)==0 && ndump++<20){ char h[80]; int k=snprintf(h,sizeof h,"  [framelog] ---- last events before present #%lu (%u total) ----\n",pc,g_flog_n); (void)!write(2,h,k);
        unsigned n=g_flog_n<FLOGN?g_flog_n:FLOGN; unsigned i=0;
        while(i<n){ const char*l=g_flog[(g_flog_n-n+i)%FLOGN]; unsigned j=i+1; while(j<n && !strcmp(g_flog[(g_flog_n-n+j)%FLOGN],l)) j++;
            char b[200]; int k=snprintf(b,sizeof b,"  [framelog] x%-4u %s\n",j-i,l); (void)!write(2,b,k); i=j; } }
    g_flog_n=0; }
static int MSABI i_D3DReflect(const void*src,uint64_t sz,GUID_*iid,void**pp){ (void)iid;
    if(reflect_enabled()){
        static int vinit; if(!vinit){ for(int i=0;i<40;i++) g_refl2_vtbl[i]=(void*)refl_ret0;
            g_refl2_vtbl[0]=(void*)refl2_QI; g_refl2_vtbl[1]=(void*)refl2_AddRef; g_refl2_vtbl[2]=(void*)refl2_Release;
            g_refl2_vtbl[3]=(void*)refl2_GetDesc; g_refl2_vtbl[4]=(void*)refl2_GetCBByIndex; g_refl2_vtbl[5]=(void*)refl2_GetCBByName;
            g_refl2_vtbl[6]=(void*)refl2_GetRBDesc; g_refl2_vtbl[11]=(void*)refl2_GetRBDescByName; vinit=1; }
        ReflS*s=refl_build(src,sz); if(s){ if(pp)*pp=s; return HR_OK; } }
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
    if(getenv("PELOAD_TRACE_COMPILE")){ static int nc; char m[300]; int k=snprintf(m,sizeof m,"  [D3DCompile #%d] name=%s entry=%s target=%s srclen=%llu\n",nc,name?name:"(null)",entry?entry:"(null)",target?target:"(null)",(unsigned long long)srcsz); (void)!write(2,m,k);
        const char*home=getenv("HOME"); char fp[256]; snprintf(fp,sizeof fp,"%s/reclaimer_hlsl_%d_%s_%s.hlsl",home?home:"/tmp",nc++,entry?entry:"x",target?target:"x");
        FILE*F=fopen(fp,"w"); if(F){ if(src&&srcsz) fwrite(src,1,(size_t)srcsz,F); fclose(F); } }
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
static int MSABI i_XAudio2Create_real(void**pp,uint32_t flags,uint32_t proc);                 // xaudio.c (CoreAudio)
static int MSABI i_X3DAudioInitialize(uint32_t mask,float sos,uint8_t*h);
static void MSABI i_X3DAudioCalculate(const uint8_t*h,const float*L,const uint8_t*E,uint32_t fl,uint8_t*dsp);
static int xa_real(void){ static int r=-1; if(r<0){ const char*e=getenv("PELOAD_AUDIO"); r = e ? (e[0]!='0') : 1; } return r; }   // default ON; PELOAD_AUDIO=0 = silent stub
static void* xa_resolve(uint32_t ord, const char* fn){
    if(xa_real()){   // PELOAD_AUDIO: real XAudio2 on CoreAudio (xaudio.c)
        if(ord==1 || (fn&&!strcmp(fn,"XAudio2Create"))) return (void*)i_XAudio2Create_real;
        if(ord==5 || (fn&&!strcmp(fn,"X3DAudioCalculate"))) return (void*)i_X3DAudioCalculate;
        if(ord==6 || (fn&&!strcmp(fn,"X3DAudioInitialize"))) return (void*)i_X3DAudioInitialize; }
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
// WSADuplicateSocketW + WSASocketW(FROM_PROTOCOL_INFO): Rust's socket try_clone() duplicates a socket
// this way (the dedicated server clones its sockets). WSAPROTOCOL_INFOW (628 B): iAddressFamily @76,
// iSocketType @88, iProtocol @92; we mark dwCatalogEntryId @36 and keep the source fd in
// dwProviderReserved @112, and WSASocketW dup()s it. Same-process only, which is all try_clone needs.
// (It was a stub: WSASocketW then got a garbage family and failed with WSAEAFNOSUPPORT, os error 10047.)
#define WSADUP_MAGIC 0x5EED0D0Fu
static int MSABI i_ws2_WSADuplicateSocketW(WSASOCK s,uint32_t pid,uint8_t*pi){ (void)pid; int fd=(int)(intptr_t)s;
    if(!pi){ t_wsa_err=10014; g_lasterr=10014; return -1; }   // WSAEFAULT
    struct sockaddr_storage ss; socklen_t sl=sizeof ss; int type=0; socklen_t tl=sizeof type;
    if(getsockname(fd,(struct sockaddr*)&ss,&sl)!=0 || getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&tl)!=0){ ws2_err(errno); return -1; }
    memset(pi,0,628); uint32_t magic=WSADUP_MAGIC; memcpy(pi+36,&magic,4);
    int af=ss.ss_family==AF_INET6?23:ss.ss_family; int proto=type==SOCK_STREAM?6:type==SOCK_DGRAM?17:0;
    memcpy(pi+72,&(int){2},4); memcpy(pi+76,&af,4); memcpy(pi+80,&(int){af==23?28:16},4); memcpy(pi+84,&(int){af==23?28:16},4);
    memcpy(pi+88,&type,4); memcpy(pi+92,&proto,4); uint32_t f=(uint32_t)fd; memcpy(pi+112,&f,4);
    net_tracef("[net] WSADuplicateSocketW(fd=%d) af=%d type=%d\n",fd,af,type); return 0; }
static WSASOCK MSABI i_ws2_WSASocketW(int af,int type,int proto,void*pi,unsigned g,uint32_t fl){ (void)g;(void)fl;
    if(pi){ uint32_t magic,src; memcpy(&magic,(uint8_t*)pi+36,4); memcpy(&src,(uint8_t*)pi+112,4);
        if(magic==WSADUP_MAGIC && src<MAXFD){ int nfd=dup((int)src); if(nfd<0){ ws2_err(errno); return WSA_INVALID; }
            if(nfd<MAXFD) g_peer_gip[nfd]=g_peer_gip[src];
            net_tracef("[net] WSASocketW(dup of fd=%d) -> fd=%d\n",(int)src,nfd); return (WSASOCK)(unsigned)nfd; }
        memcpy(&af,(uint8_t*)pi+76,4); memcpy(&type,(uint8_t*)pi+88,4); memcpy(&proto,(uint8_t*)pi+92,4); }
    return i_ws2_socket(af,type,proto); }
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
    if(!strcmp(n,"WSADuplicateSocketW")) return (void*)i_ws2_WSADuplicateSocketW;
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
// GetModuleHandleExW: FROM_ADDRESS (0x4) -> the image containing the address (Waves' WaveShell-Xbox.dll finds its own
// folder this way to load L360.dll/q10.dll next to it; always answering "the exe" sent it to client\waves\);
// a name -> GetModuleHandleW; NULL -> the exe.
static int    MSABI i_GetModuleHandleExW(uint32_t fl, void *name, void **out){
    if(fl&0x4){ uint8_t*m=guest_mod_by_addr((uint64_t)name); if(out)*out=m; if(!m){ g_lasterr=126; return 0; } return 1; }
    void*m= name? i_GetModuleHandleW((uint16_t*)name) : g_base; if(out)*out=m; return m!=0; }
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
static void   MSABI i_TerminateProcess(void *h,uint32_t c){ (void)h;
    { uint64_t r=(uint64_t)__builtin_return_address(0); char m[160]; int k=snprintf(m,sizeof m,"  [TerminateProcess] code=0x%x caller=%s+0x%llx\n",c,
        (r>=0x140000000ULL&&r<0x150000000ULL)?"exe":(r>=0x180000000ULL&&r<0x190000000ULL)?"halo3":"?",(unsigned long long)((r>=0x140000000ULL&&r<0x150000000ULL)?r-0x140000000ULL:(r>=0x180000000ULL&&r<0x190000000ULL)?r-0x180000000ULL:r)); (void)!write(2,m,k);
      // guest return addresses on the stack (who decided to terminate)
      uint64_t*sp=(uint64_t*)__builtin_frame_address(0); int f=0; for(int i=0;i<600&&f<14;i++){ uint64_t v=sp[i];
        if(v>=0x140001000ULL&&v<0x150000000ULL){ k=snprintf(m,sizeof m,"     exe+0x%llx\n",(unsigned long long)(v-0x140000000ULL)); (void)!write(2,m,k); f++; }
        else if(v>=0x180001000ULL&&v<0x187000000ULL){ k=snprintf(m,sizeof m,"     halo3+0x%llx\n",(unsigned long long)(v-0x180000000ULL)); (void)!write(2,m,k); f++; } } }
    _exit((int)c); }
// GetModuleFileName(hModule): a guest DLL's own path (Waves' WaveShell-Xbox.dll, loaded by client v0.9.18,
// builds boost::filesystem paths from it; returning the exe's "\\?\Z:\..." path made boost throw
// "invalid name"). NULL / the exe's base -> the exe path, as before.
static const char* guest_mod_path(void*m){ for(int k=0;k<g_nmods;k++) if(m && g_mods[k].base==(uint8_t*)m) return g_mods[k].path; return 0; }
static uint32_t MSABI i_GetModuleFileNameW(void*m,uint16_t*buf,uint32_t sz){ uint32_t i=0;
    { const char*mp=guest_mod_path(m); if(mp && sz){ uint16_t w[1024]; unix2win_ex(mp,w,1024,1); for(; i<sz && w[i]; i++) buf[i]=w[i]; if(i<sz) buf[i]=0; else { buf[sz-1]=0; g_lasterr=122; } return i; } }
    for(; i<sz && g_exewin[i]; i++) buf[i]=g_exewin[i]; if(i<sz) buf[i]=0; if(getenv("PELOAD_TRACE_SPAWN")){ char n[1100]; uint32_t k=0; for(;k<i&&k<1099;k++)n[k]=(char)(buf[k]&0xff); n[k]=0; char t[1200]; snprintf(t,sizeof t,"  [GetModuleFileNameW] mod=%p -> %s\n",m,n); ws(t);} return i; }
static void   MSABI i_GetStartupInfoW(void *si){ if(si){ memset(si,0,104); *(uint32_t*)si=104; } }
// GetStartupInfoA: the MSVC CRT startup of guest DLLs (bink2w64.dll, loaded by client v0.9.18) reads
// lpReserved2/cbReserved2 for inherited handles; an unfilled stub made it parse garbage and crash.
static void   MSABI i_GetStartupInfoA(void *si){ if(si){ memset(si,0,104); *(uint32_t*)si=104; } }
static int    MSABI i_HeapSetInformation(void*h,int cls,void*info,uint64_t len){ (void)h;(void)cls;(void)info;(void)len; return 1; }
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
    if(!g_gameplay_t0 && buf && len>20 && memmem(buf,len,"gameplay presentation ready",27)) g_gameplay_t0=mono_s();
    { extern double g_menu_ready_t0; if(!g_menu_ready_t0 && buf && len>20 && memmem(buf,len,"Menu loading screen finished",28)) g_menu_ready_t0=mono_s(); }
    if(buf && len>20 && memmem(buf,len,"Carnage report opened",21)) __atomic_add_fetch(&g_games_ended,1,__ATOMIC_SEQ_CST);
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
static uint32_t MSABI i_GetModuleFileNameA(void*m,char*buf,uint32_t sz){ uint32_t i=0;
    { const char*mp=guest_mod_path(m); if(mp && sz){ uint16_t w[1024]; unix2win_ex(mp,w,1024,1); for(; i<sz && w[i]; i++) buf[i]=(char)(w[i]<128?w[i]:'?'); if(i<sz) buf[i]=0; else buf[sz-1]=0; return i; } } for(; i<sz && g_exewinA[i]; i++) buf[i]=g_exewinA[i]; if(i<sz) buf[i]=0; return i; }
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
typedef struct { uint32_t magic; WNDPROC_t wndproc; int64_t userdata; int64_t extra[16]; int x,y,w,h; uint32_t style, exstyle; } WWin;
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
static void* g_shown_hwnd;  // the foreground window: the last top-level window shown or set with ACTIVATION
                            // (the client compares GetForegroundWindow() with its game window before polling
                            // XInput / reading input, and input with no target window goes here)
// Windows activation rules: child windows and WS_EX_NOACTIVATE windows never become the foreground window,
// and SW_SHOWNOACTIVATE / SW_SHOWNA / SW_SHOWMINNOACTIVE / SW_MINIMIZE / SW_HIDE don't activate. Reclaimer's
// menu runtime shows its in-game overlay (a WS_CHILD | WS_EX_NOACTIVATE layered window) with SW_SHOWNOACTIVATE
// many times per match; treating that as activation sent keyboard input to the overlay and made the client
// think its window had lost focus, so its scoreboard/chat never saw a key. PELOAD_FG_LASTSHOWN = old behavior.
static int fg_can_activate(void*h){ if(!h || ((WWin*)h)->magic!=WWIN_MAGIC) return 0; static int old=-1; if(old<0) old=getenv("PELOAD_FG_LASTSHOWN")?1:0;
    if(old) return 1; const WWin*w=h; return !(w->style&0x40000000/*WS_CHILD*/) && !(w->exstyle&0x08000000/*WS_EX_NOACTIVATE*/); }
static void* fg_hwnd(void){ return g_shown_hwnd?g_shown_hwnd:(g_last_hwnd?g_last_hwnd:(void*)0x4857); }
void* fg_hwnd_ext(void){ return fg_hwnd(); }
static void*  MSABI i_CreateWindowExW(uint32_t ex,const void*cls,const void*name,uint32_t style,int x,int y,int w,int h,void*parent,void*menu,void*inst,void*param){
    WWin* win=calloc(1,sizeof *win); if(!win) return (void*)0x4857;
    win->magic=WWIN_MAGIC; win->wndproc=wcls_find(cls); win->x=x; win->y=y; win->w=w>0?w:1280; win->h=h>0?h:720;
    win->style=style; win->exstyle=ex; g_last_hwnd=win;
    if((style&0x10000000/*WS_VISIBLE*/) && fg_can_activate(win)) g_shown_hwnd=win;
    { char t[96]={0}; const uint16_t*wn=name; for(int i=0;wn&&wn[i]&&i<95;i++) t[i]=(char)(wn[i]&0x7f);
      char c[96]={0}; if((uintptr_t)cls>0x10000){ const uint16_t*wc=cls; for(int i=0;wc[i]&&i<95;i++) c[i]=(char)(wc[i]&0x7f); } else snprintf(c,sizeof c,"#atom%u",(unsigned)(uintptr_t)cls);
      char m[300]; int k=snprintf(m,sizeof m,"  [CreateWindowExW] hwnd=%p class=%s title=\"%s\" style=0x%x ex=0x%x parent=%p %dx%d\n",(void*)win,c,t,style,ex,parent,w,h); (void)!write(2,m,k); }
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
static int    MSABI i_ShowWindow(void*h,int c){ int act=(c==1||c==2||c==3||c==5||c==9||c==10)||(getenv("PELOAD_FG_LASTSHOWN")&&c);
    if(act && fg_can_activate(h)) g_shown_hwnd=h;
    { static void*lh; static int lc=-1; if(h!=lh||c!=lc){ lh=h; lc=c; char m[80]; int k=snprintf(m,sizeof m,"  [ShowWindow] hwnd=%p cmd=%d\n",h,c); (void)!write(2,m,k); } } return 1; }
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
typedef struct { void* hwnd; uint32_t msg; uint64_t wp; int64_t lp; } WiMsgFwd;
static int wi_peek_fwd(WiMsgFwd*out,int remove);   // wininput.c (keyboard/mouse queue)
static void cocoa_pump(void);
static int    MSABI i_PeekMessageW(void*m,void*h,uint32_t f1,uint32_t f2,uint32_t r){ (void)h;(void)f1;(void)f2; inject_activation_once();
    if(g_inji<g_injn){ msg_fill(m,g_injmsg[g_inji].hwnd,g_injmsg[g_inji].msg,g_injmsg[g_inji].wp,g_injmsg[g_inji].lp); if(r&1/*PM_REMOVE*/) g_inji++; return 1; }
    WiMsgFwd e; if(wi_peek_fwd(&e,r&1)){ msg_fill(m,e.hwnd,e.msg,e.wp,e.lp); return 1; }
    if(pthread_main_np()){ static double last; double now=mono_s(); if(now-last>0.002){ last=now; cocoa_pump(); if(wi_peek_fwd(&e,r&1)){ msg_fill(m,e.hwnd,e.msg,e.wp,e.lp); return 1; } } }
    return 0; }
static int    MSABI i_GetMessageW(void*m,void*h,uint32_t f1,uint32_t f2){ (void)h;(void)f1;(void)f2; inject_activation_once();
    if(g_inji<g_injn){ msg_fill(m,g_injmsg[g_inji].hwnd,g_injmsg[g_inji].msg,g_injmsg[g_inji].wp,g_injmsg[g_inji].lp); g_inji++; return 1; }
    WiMsgFwd e; if(wi_peek_fwd(&e,1)){ msg_fill(m,e.hwnd,e.msg,e.wp,e.lp); return 1; }
    return 0; }
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
// EnumDisplayMonitors: report the one (primary) 1920x1080 monitor that MonitorFromWindow/GetMonitorInfoW
// describe. It used to return TRUE without calling the callback, so Reclaimer saw zero monitors and its egui
// UI scale fell back to window_width/1 (pixels_per_point 1280 -> the frontend menu's widgets off-screen).
static int    MSABI i_EnumDisplayMonitors(void*dc,const void*clip,void*cb,int64_t data){ (void)dc;(void)clip;
    if(!cb) return 0; int32_t rc[4]={0,0,1920,1080};
    int (__attribute__((ms_abi)) *fn)(void*,void*,int32_t*,int64_t)=cb;
    void*mon=(void*)0x4D01;   // the handle MonitorFromWindow/Point/Rect return
    fn(mon,dc,rc,data); return 1; }
static int    MSABI i_GetClientRect(void*h,int32_t*r){ (void)h; if(r){r[0]=0;r[1]=0;r[2]=WIN_W;r[3]=WIN_H;} return 1; }
static int    MSABI i_GetWindowRect(void*h,int32_t*r){ (void)h; if(r){r[0]=0;r[1]=0;r[2]=WIN_W;r[3]=WIN_H;} return 1; }
static int    MSABI i_SetWindowPos(void*h,void*after,int x,int y,int w,int ht,uint32_t f){ (void)h;(void)after;(void)x;(void)y;(void)w;(void)ht;(void)f; return 1; }
static int64_t g_winlong[8];
static int64_t MSABI i_GetWindowLongPtrW(void*h,int idx){ WWin*w=h; if(w&&w->magic==WWIN_MAGIC){ if(idx==-21)return w->userdata; if(idx==-4)return (int64_t)(intptr_t)w->wndproc; if(idx>=0&&idx<128)return w->extra[idx/8]; return 0; } return (idx>=-8&&idx<0)? g_winlong[-idx-1]:0; }
static int64_t MSABI i_SetWindowLongPtrW(void*h,int idx,int64_t v){ WWin*w=h; if(w&&w->magic==WWIN_MAGIC){ int64_t o=0; if(idx==-21){o=w->userdata;w->userdata=v;} else if(idx==-4){o=(int64_t)(intptr_t)w->wndproc;w->wndproc=(WNDPROC_t)(intptr_t)v;} else if(idx>=0&&idx<128){o=w->extra[idx/8];w->extra[idx/8]=v;} return o; } if(idx>=-8&&idx<0){ int64_t o=g_winlong[-idx-1]; g_winlong[-idx-1]=v; return o;} return 0; }
static int32_t MSABI i_GetWindowLongW(void*h,int idx){ return (int32_t)i_GetWindowLongPtrW(h,idx); }
static int32_t MSABI i_SetWindowLongW(void*h,int idx,int32_t v){ return (int32_t)i_SetWindowLongPtrW(h,idx,v); }
static int    MSABI i_AdjustWindowRectEx(int32_t*r,uint32_t s,int m,uint32_t ex){ (void)r;(void)s;(void)m;(void)ex; return 1; }
// DPI queries: unimplemented they fell through to the 0-returning stub, so Reclaimer's egui UI (its
// frontend menu and in-game overlays) computed pixels_per_point = 0/96 and painted every widget at zero
// size -- the menu showed only egui's cleared background. Report a standard 96-DPI display.
static int    MSABI i_AdjustWindowRectExForDpi(int32_t*r,uint32_t s,int m,uint32_t ex,uint32_t dpi){ (void)dpi; return i_AdjustWindowRectEx(r,s,m,ex); }
static uint32_t MSABI i_GetDpiForWindow(void*h){ (void)h; return 96; }
static uint32_t MSABI i_GetDpiForSystem(void){ return 96; }
static int    MSABI i_IsProcessDPIAware(void){ return 1; }
// shcore.dll (loaded dynamically by Reclaimer): GetDpiForMonitor must WRITE *dpiX/*dpiY -- the generic
// stub returned S_OK and left them unset, and egui's pixels_per_point came out as 1280.
static int    MSABI i_GetDpiForMonitor(void*mon,int type,uint32_t*dx,uint32_t*dy){ (void)mon;(void)type; if(dx)*dx=96; if(dy)*dy=96; return 0; }
static int    MSABI i_GetScaleFactorForMonitor(void*mon,int*scale){ (void)mon; if(scale)*scale=100; return 0; }   // SCALE_100_PERCENT
static int    MSABI i_SetProcessDpiAwareness(int v){ (void)v; return 0; }
static int    MSABI i_SetProcessDpiAwarenessContext(void*v){ (void)v; return 1; }
static int    MSABI i_GetDeviceCaps(void*hdc,int index){ (void)hdc;
    switch(index){ case 88: case 90: return 96;      // LOGPIXELSX / LOGPIXELSY
        case 8: case 118: return 1920;                   // HORZRES / DESKTOPHORZRES
        case 10: case 117: return 1080;                  // VERTRES / DESKTOPVERTRES
        case 12: return 32; case 14: return 1;           // BITSPIXEL / PLANES
        case 116: return 60;                             // VREFRESH
        case 4: return 508; case 6: return 286;          // HORZSIZE / VERTSIZE (mm, ~23" 16:9)
        case 24: return -1;                              // NUMCOLORS (true colour)
        case 2: return 1;                                // TECHNOLOGY: DT_RASDISPLAY
        default: return 0; } }
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
void* fg_hwnd_ext(void);
#include <CoreGraphics/CoreGraphics.h>
#include "wininput.c"
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
typedef struct { uint32_t magic; GdiObj*bmp; int iswin; uint32_t textcolor; int bkmode; int font_h; uint32_t bkcolor; } GdiDC;
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
static void*  MSABI i_CreateCompatibleDC(void*hdc){ (void)hdc; GdiDC*d=calloc(1,sizeof*d); if(d){d->magic=GDIDC_MAGIC;d->bkmode=2;d->bkcolor=0xFFFFFF;} return d?d:(void*)0xDC02; }
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
static GdiObj* gdi_text_target(GdiDC*d); // fwd
static int    MSABI i_StretchDIBits(void*dc,int xd,int yd,int wd,int hd,int xs,int ys,int ws,int hs,const void*bits,const uint8_t*bmi,uint32_t usage,uint32_t rop){
    (void)usage;(void)rop; GdiDC*dd=dc; if(dd&&dd->magic==GDIDC_MAGIC&&dd->bmp&&bits&&bmi){ int bw=*(const int32_t*)(bmi+4),bh=*(const int32_t*)(bmi+8);
        // same destination as the DC's text (gdi_text_target): the client repaints its background with
        // StretchDIBits before redrawing the status text; blitting into the pixel-less DC bitmap left the old
        // text on the surface ("LOADING MAP" over "LOADING ENGINE")
        GdiObj s={GDI_MAGIC,bw,bh<0?-bh:bh,32,(void*)bits,0,0,0,bh<0}; GdiObj*dst=gdi_text_target(dd); gdi_blit(dst?dst:dd->bmp,xd,yd,wd,hd,&s,xs,ys,ws,hs,0);
        if(getenv("PELOAD_TRACE_GDI")){ char t[128]; int k=snprintf(t,sizeof t,"  [StretchDIBits] dst(win=%d) %d,%d %dx%d <- DIB %dx%d\n",dd->iswin,xd,yd,wd,hd,bw,bh); (void)!write(2,t,k);}
        if(dd->iswin) gdi_present_window(); }
    return hs>0?hs:1; }
static int    MSABI i_SetStretchBltMode(void*dc,int mode){ (void)dc;(void)mode; return 1; }
static int    MSABI i_SetBrushOrgEx(void*dc,int x,int y,void*pt){ (void)dc;(void)x;(void)y; if(pt){((int32_t*)pt)[0]=0;((int32_t*)pt)[1]=0;} return 1; }
static uint32_t MSABI i_SetTextColor(void*dc,uint32_t c){ GdiDC*d=dc; uint32_t o=0; if(d&&d->magic==GDIDC_MAGIC){o=d->textcolor;d->textcolor=c;} return o; }
static int    MSABI i_SetBkMode(void*dc,int m){ GdiDC*d=dc; int o=2; if(d&&d->magic==GDIDC_MAGIC){o=d->bkmode;d->bkmode=m;} return o; }
static uint32_t MSABI i_SetBkColor(void*dc,uint32_t c){ GdiDC*d=dc; uint32_t o=0xFFFFFF; if(d&&d->magic==GDIDC_MAGIC){ o=d->bkcolor; d->bkcolor=c; } return o; }
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
static int    MSABI i_ExtTextOutW(void*dc,int x,int y,uint32_t f,const void*rc,const uint16_t*s,uint32_t c,const int*dx){ (void)dx; GdiDC*d=dc;
    // ETO_OPAQUE: fill the rectangle with the DC background colour first -- the client erases its previous
    // status line this way ("LOADING MAP" was drawn over the stale "LOADING ENGINE")
    if((f&2) && rc && d && d->magic==GDIDC_MAGIC){ const int32_t*r=rc; GdiObj*t=gdi_text_target(d); if(t) gdi_fill(t,r[0],r[1],r[2]-r[0],r[3]-r[1],gdi_colorref_to_px(d->bkcolor)); }
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
static void*  MSABI i_GetDC(void*h){ (void)h; gdi_win_init(); GdiDC*d=calloc(1,sizeof*d); if(d){ d->magic=GDIDC_MAGIC; d->bmp=&g_gdi_winobj; d->iswin=1; d->bkmode=2; d->bkcolor=0xFFFFFF; } return d?d:(void*)0xDC01; }
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
// ---- XInput (xinput1_4.dll) -> macOS GameController.framework + scripted sticks -----------------
// The client polls XInputGetState (exe sub_1406108a0) only while GetForegroundWindow()==its game window.
// Pad state comes from the real controller (GCController.extendedGamepad; needs the main run loop, which
// cocoa_pump services), optionally overridden by PELOAD_INPUT_SCRIPT for automated walking tests:
//   PELOAD_INPUT_SCRIPT="secs:lx,ly,rx,ry[,buttonsHex][;...]"  (axes -1..1; "walk" = built-in turn/walk loop)
//   starts PELOAD_INPUT_DELAY s (default 3) after "gameplay presentation ready"; PELOAD_INPUT_LOOP repeats it.
//   PELOAD_TRACE_INPUT logs pad state changes.
#include <objc/runtime.h>
#include <objc/message.h>
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } XPad;
static int g_gc_init; static Class g_gccls;
static void gc_init(void){ if(g_gc_init) return; g_gc_init=1;
    dlopen("/System/Library/Frameworks/GameController.framework/GameController",RTLD_LAZY);
    g_gccls=objc_getClass("GCController"); if(!g_gccls) return;
    SEL sb=sel_registerName("setShouldMonitorBackgroundEvents:");
    if(class_getClassMethod(g_gccls,sb)) ((void(*)(id,SEL,signed char))objc_msgSend)((id)g_gccls,sb,1); }   // input even when not key window
static id gcm(id o,const char*sel){ return o?((id(*)(id,SEL))objc_msgSend)(o,sel_registerName(sel)):(id)0; }
static float gcv(id o){ return o?((float(*)(id,SEL))objc_msgSend)(o,sel_registerName("value")):0.0f; }
static int gcp(id o){ return o?(int)((signed char(*)(id,SEL))objc_msgSend)(o,sel_registerName("isPressed")):0; }
static int16_t ax16(float v){ if(v>1)v=1; if(v<-1)v=-1; return (int16_t)(v*32767.0f); }
static int gc_read(uint32_t idx,XPad*p){
    gc_init(); if(!g_gccls) return 0;
    id arr=gcm((id)g_gccls,"controllers"); long n=arr?((long(*)(id,SEL))objc_msgSend)(arr,sel_registerName("count")):0;
    if((long)idx>=n) return 0;
    id c=((id(*)(id,SEL,unsigned long))objc_msgSend)(arr,sel_registerName("objectAtIndex:"),idx); id g=gcm(c,"extendedGamepad"); if(!g) return 0;
    id ls=gcm(g,"leftThumbstick"), rs=gcm(g,"rightThumbstick"), dp=gcm(g,"dpad");
    p->lx=ax16(gcv(gcm(ls,"xAxis"))); p->ly=ax16(gcv(gcm(ls,"yAxis"))); p->rx=ax16(gcv(gcm(rs,"xAxis"))); p->ry=ax16(gcv(gcm(rs,"yAxis")));
    // inverted look (pitch) by default -- the player's preference; PELOAD_INVERT_PITCH=0 restores normal
    { static int inv=-1; if(inv<0){ const char*e=getenv("PELOAD_INVERT_PITCH"); inv=!(e&&e[0]=='0'); } if(inv) p->ry=(int16_t)(p->ry==-32768?32767:-p->ry); }
    p->lt=(uint8_t)(gcv(gcm(g,"leftTrigger"))*255.0f); p->rt=(uint8_t)(gcv(gcm(g,"rightTrigger"))*255.0f);
    uint16_t b=0;
    if(gcp(gcm(dp,"up")))b|=0x0001; if(gcp(gcm(dp,"down")))b|=0x0002; if(gcp(gcm(dp,"left")))b|=0x0004; if(gcp(gcm(dp,"right")))b|=0x0008;
    if(gcp(gcm(g,"buttonMenu")))b|=0x0010; if(gcp(gcm(g,"buttonOptions")))b|=0x0020;
    if(gcp(gcm(g,"leftThumbstickButton")))b|=0x0040; if(gcp(gcm(g,"rightThumbstickButton")))b|=0x0080;
    if(gcp(gcm(g,"leftShoulder")))b|=0x0100; if(gcp(gcm(g,"rightShoulder")))b|=0x0200;
    if(gcp(gcm(g,"buttonA")))b|=0x1000; if(gcp(gcm(g,"buttonB")))b|=0x2000; if(gcp(gcm(g,"buttonX")))b|=0x4000; if(gcp(gcm(g,"buttonY")))b|=0x8000;
    p->buttons=b; return 1; }
// scripted override for pad 0: returns 1 and fills *p while a segment is active
static int input_script(XPad*p){
    static int parsed; static int nseg; static struct { double dur; float lx,ly,rx,ry; uint16_t b; float rt,lt; } seg[64]; static double total;
    if(!parsed){ parsed=1; const char*e=getenv("PELOAD_INPUT_SCRIPT"); if(!e) return 0;
        // built-in: settle, turn right, walk, turn left, walk
        if(!strcmp(e,"walk")) e="1:0,0,0,0;1.2:0,0,0.8,0;2.5:0,1,0,0;1.2:0,0,-0.8,0;2.5:0,1,0,0;1:0,0,0,0";
        // built-in: fire bursts + a grenade (effects / particles test)
        if(!strcmp(e,"fire")) e="1:0,0,0,0;2:0,0,0,0,0,1;1:0,0,0,0;0.4:0,0,0,0,0,0,1;3:0,0,0,0;2:0,0,0.3,0,0,1";
        char buf[2048]; snprintf(buf,sizeof buf,"%s",e);
        for(char*sv=0,*t=strtok_r(buf,";",&sv); t && nseg<64; t=strtok_r(0,";",&sv)){ unsigned bb=0;
            seg[nseg].rt=seg[nseg].lt=0;
            if(sscanf(t,"%lf:%f,%f,%f,%f,%x,%f,%f",&seg[nseg].dur,&seg[nseg].lx,&seg[nseg].ly,&seg[nseg].rx,&seg[nseg].ry,&bb,&seg[nseg].rt,&seg[nseg].lt)>=5){ seg[nseg].b=(uint16_t)bb; total+=seg[nseg].dur; nseg++; } }
        char m[120]; int k=snprintf(m,sizeof m,"  [input] script: %d segments, %.1fs%s\n",nseg,total,getenv("PELOAD_INPUT_LOOP")?" (loop)":""); (void)!write(2,m,k); }
    if(!nseg || !g_gameplay_t0) return 0;
    double delay=getenv("PELOAD_INPUT_DELAY")?atof(getenv("PELOAD_INPUT_DELAY")):3.0;
    double t=mono_s()-g_gameplay_t0-delay; if(t<0) return 0;
    if(getenv("PELOAD_INPUT_LOOP") && total>0) t=fmod(t,total); else if(t>=total) return 0;
    for(int i=0;i<nseg;i++){ if(t<seg[i].dur){ p->lx=ax16(seg[i].lx); p->ly=ax16(seg[i].ly); p->rx=ax16(seg[i].rx); p->ry=ax16(seg[i].ry); p->buttons=seg[i].b;
            { extern int hb_face_target(float*,float*); float fx,fy; if(i>0 && getenv("PELOAD_INPUT_FACE") && hb_face_target(&fx,&fy)){ p->rx=ax16(fx); p->ry=ax16(fy); } }
            p->rt=(uint8_t)(seg[i].rt*255.0f); p->lt=(uint8_t)(seg[i].lt*255.0f);
            static int last=-1; if(i!=last){ last=i; char m[120]; int k=snprintf(m,sizeof m,"  [input] script segment %d: L=(%.2f,%.2f) R=(%.2f,%.2f) b=%x\n",i,seg[i].lx,seg[i].ly,seg[i].rx,seg[i].ry,seg[i].b); (void)!write(2,m,k); }
            return 1; } t-=seg[i].dur; }
    return 0; }
static uint32_t MSABI i_XInputGetState(uint32_t idx,void*st){
    if(idx>3||!st) return 160; /*ERROR_BAD_ARGUMENTS*/
    // PELOAD_NO_PAD: this instance sees no controllers (e.g. a second client used as a test target, so it
    // doesn't share the player's pad: GameController delivers pad input to every process, focused or not)
    { static int nopad=-1; if(nopad<0) nopad=getenv("PELOAD_NO_PAD")?1:0; if(nopad) return 1167; /*ERROR_DEVICE_NOT_CONNECTED*/ }
    static uint32_t pkt[4]; static XPad last[4]; static int logged;
    XPad p; memset(&p,0,sizeof p); int con=gc_read(idx,&p);
    if(idx==0 && input_script(&p)) con=1;
    if(!logged){ logged=1; char m[96]; int k=snprintf(m,sizeof m,"  [input] XInputGetState first call (pad0 %s)\n",con?"connected":"absent"); (void)!write(2,m,k); }
    if(!con) return 1167; /*ERROR_DEVICE_NOT_CONNECTED*/
    if(memcmp(&p,&last[idx],sizeof p)){ last[idx]=p; pkt[idx]++;
        if(getenv("PELOAD_TRACE_INPUT")){ char m[160]; int k=snprintf(m,sizeof m,"  [input] pad%u pkt=%u b=%04x lt=%u rt=%u L=(%d,%d) R=(%d,%d)\n",idx,pkt[idx],p.buttons,p.lt,p.rt,p.lx,p.ly,p.rx,p.ry); (void)!write(2,m,k); } }
    uint8_t*o=st; *(uint32_t*)o=pkt[idx]; memcpy(o+4,&p.buttons,2); o[6]=p.lt; o[7]=p.rt;
    memcpy(o+8,&p.lx,2); memcpy(o+10,&p.ly,2); memcpy(o+12,&p.rx,2); memcpy(o+14,&p.ry,2);
    return 0; }
static uint32_t MSABI i_XInputSetState(uint32_t idx,void*vib){ (void)vib; XPad p; return (idx<4 && (gc_read(idx,&p)||idx==0))?0:1167; }
static uint32_t MSABI i_XInputGetCapabilities(uint32_t idx,uint32_t fl,void*cap){ (void)fl; XPad p; if(idx>3) return 160; if(getenv("PELOAD_NO_PAD")) return 1167;
    if(!gc_read(idx,&p) && !(idx==0&&getenv("PELOAD_INPUT_SCRIPT"))) return 1167;
    if(cap){ memset(cap,0,20); uint8_t*c=cap; c[0]=1/*GAMEPAD*/; c[1]=1/*SUBTYPE_GAMEPAD*/; *(uint16_t*)(c+4)=0xF3FF; c[6]=c[7]=255; *(int16_t*)(c+8)=*(int16_t*)(c+10)=*(int16_t*)(c+12)=*(int16_t*)(c+14)=(int16_t)0xFFC0; } return 0; }
static void MSABI i_XInputEnable(int en){ (void)en; }
static void*  MSABI i_GetForegroundWindow(void){ return fg_hwnd(); }
static int    MSABI i_SetForegroundWindow(void*h){ if(fg_can_activate(h)) g_shown_hwnd=h; return 1; }
static int    MSABI i_IsWindow(void*h){ return h?1:0; }
static int    MSABI i_IsWindowVisible(void*h){ (void)h; return 1; }
static void*  MSABI i_GetActiveWindow(void){ return fg_hwnd(); }
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
static volatile int g_win_pw, g_win_ph;   // current window drawable size (0 = not yet known)
id g_nswin_ext(void){ return g_nswin; }
extern void* objc_autoreleasePoolPush(void);
static id ocls(const char*n){ return (id)objc_getClass(n); }
static SEL osel(const char*n){ return sel_registerName(n); }
static id om0(id o,const char*s){ return ((id(*)(id,SEL))objc_msgSend)(o,osel(s)); }
static id oretain(id o){ return ((id(*)(id,SEL))objc_msgSend)(o,osel("retain")); }
// Initial window content size (points): PELOAD_WIN_SIZE=WxH, else the largest 16:9 size that fits 90% of the
// main screen's usable area (menu bar and Dock excluded), never smaller than the requested w x h. The game
// renders at its own resolution (swap chain 1920x1080) and the window scales it; input maps by view size.
static int g_screen_hz;   // the main screen's maximum refresh rate (fps_title_tick never reports more than this)
static void win_initial_size(double*w,double*h){
    { id s0=om0(ocls("NSScreen"),"mainScreen"); if(s0 && class_respondsToSelector(object_getClass(s0),osel("maximumFramesPerSecond")))
        g_screen_hz=(int)((long(*)(id,SEL))objc_msgSend)(s0,osel("maximumFramesPerSecond")); }
    const char*e=getenv("PELOAD_WIN_SIZE"); int ew=0,eh=0;
    if(e && sscanf(e,"%dx%d",&ew,&eh)==2 && ew>=320 && eh>=180){ *w=ew; *h=eh; return; }
    id scr=om0(ocls("NSScreen"),"mainScreen"); if(!scr) return;
    NSRect_ vf={0,0,0,0}; ((void(*)(NSRect_*,id,SEL))objc_msgSend_stret)(&vf,scr,osel("visibleFrame"));
    double mw=vf.w*0.9, mh=vf.h*0.9; if(mw<=0||mh<=0) return;
    double fw=mw, fh=mw*9.0/16.0; if(fh>mh){ fh=mh; fw=mh*16.0/9.0; }
    if(fw>*w){ *w=(double)(int)fw; *h=(double)(int)fh; } }
static CGLContextObj cocoa_make_gl_window(int w,int h){
    if(g_nsctx){ return (CGLContextObj)((void*(*)(id,SEL))objc_msgSend)(g_nsctx,osel("CGLContextObj")); }
    objc_autoreleasePoolPush(); // AppKit object creation needs a pool (we never run the runloop); leak it so objects live
    g_nsapp = om0(ocls("NSApplication"),"sharedApplication");
    ((void(*)(id,SEL,long))objc_msgSend)(g_nsapp,osel("setActivationPolicy:"),0); // Regular
    om0(g_nsapp,"finishLaunching");
    uint32_t attrs[]={5/*DoubleBuffer*/,99/*Profile*/,0x4100/*4.1 Core*/,8/*ColorSize*/,24,11/*AlphaSize*/,8,12/*DepthSize*/,24,13/*Stencil*/,8,73/*Accelerated*/,72/*NoRecovery*/,0};
    id pf = ((id(*)(id,SEL,const uint32_t*))objc_msgSend)(om0(ocls("NSOpenGLPixelFormat"),"alloc"),osel("initWithAttributes:"),attrs); g_nspf=pf;
    id ctx = ((id(*)(id,SEL,id,id))objc_msgSend)(om0(ocls("NSOpenGLContext"),"alloc"),osel("initWithFormat:shareContext:"),pf,(id)0);
    NSRect_ rect={0,0,(double)w,(double)h}; win_initial_size(&rect.w,&rect.h);
    id win = ((id(*)(id,SEL,NSRect_,unsigned long,unsigned long,signed char))objc_msgSend)(
        om0(ocls("NSWindow"),"alloc"),osel("initWithContentRect:styleMask:backing:defer:"),rect,15/*Titled|Closable|Mini|Resizable*/,2/*Buffered*/,0);
    id title = ((id(*)(id,SEL,const char*))objc_msgSend)(ocls("NSString"),osel("stringWithUTF8String:"),"Project Reclaimer (Mac, Wine-free)");
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("setTitle:"),title);
    id view = om0(win,"contentView");
    ((void(*)(id,SEL,id))objc_msgSend)(g_nsctx=ctx,osel("setView:"),view); // setView wants main thread; fine on the GL thread here
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("center"),(id)0);
    // mouse-moved events (with deltas) are only delivered to windows that ask for them (keyboard/mouse input)
    ((void(*)(id,SEL,signed char))objc_msgSend)(win,osel("setAcceptsMouseMovedEvents:"),1);
    ((void(*)(id,SEL,unsigned long))objc_msgSend)(win,osel("setCollectionBehavior:"),1ul<<7/*FullScreenPrimary: green button = full screen*/);
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
    { extern void title_timer_start_ext(void); title_timer_start_ext(); }
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
    void(*VertexAttribIPointer)(unsigned,int,unsigned,int,const void*);
    void(*DrawArrays)(unsigned,int,int); void(*DrawElements)(unsigned,int,unsigned,const void*);
    void(*DrawArraysInstanced)(unsigned,int,int,int); void(*DrawElementsInstanced)(unsigned,int,unsigned,const void*,int);
    void(*Viewport)(int,int,int,int); void(*ClearColor)(float,float,float,float); void(*Clear)(unsigned);
    void(*Enable)(unsigned); void(*Disable)(unsigned); void(*BlendFunc)(unsigned,unsigned); unsigned(*GetError)(void); void(*Flush)(void);
    void(*ReadBuffer)(unsigned); void(*ReadPixels)(int,int,int,int,unsigned,unsigned,void*);
    void(*GenRenderbuffers)(int,unsigned*); void(*BindRenderbuffer)(unsigned,unsigned);
    void(*RenderbufferStorage)(unsigned,unsigned,int,int); void(*FramebufferRenderbuffer)(unsigned,unsigned,unsigned,unsigned);
    void(*DepthMask)(unsigned char);
    void(*DrawElementsBaseVertex)(unsigned,int,unsigned,const void*,int); void(*DrawElementsInstancedBaseVertex)(unsigned,int,unsigned,const void*,int,int);
    void(*VertexAttribDivisor)(unsigned,unsigned); void(*BufferSubData)(unsigned,long,long,const void*); void(*PrimitiveRestartIndex)(unsigned);
    void(*BlitFramebuffer)(int,int,int,int,int,int,int,int,unsigned,unsigned); void(*Finish)(void);
    void(*GenQueries)(int,unsigned*); void(*BeginQuery)(unsigned,unsigned); void(*EndQuery)(unsigned); void(*GetQueryObjectiv)(unsigned,unsigned,int*); void(*GetQueryObjectui64v)(unsigned,unsigned,uint64_t*);
    void(*BlendFuncSeparate)(unsigned,unsigned,unsigned,unsigned); void(*BlendEquationSeparate)(unsigned,unsigned); void(*BlendColor)(float,float,float,float);
    void(*ColorMask)(unsigned char,unsigned char,unsigned char,unsigned char); void(*CullFace)(unsigned); void(*FrontFace)(unsigned); void(*Scissor)(int,int,int,int);
    void(*GenSamplers)(int,unsigned*); void(*BindSampler)(unsigned,unsigned); void(*SamplerParameteri)(unsigned,unsigned,int); void(*SamplerParameterf)(unsigned,unsigned,float); void(*SamplerParameterfv)(unsigned,unsigned,const float*);
    void*(*MapBufferRange)(unsigned,long,long,unsigned); unsigned char(*UnmapBuffer)(unsigned); void(*TexBuffer)(unsigned,unsigned,unsigned);
    unsigned(*GetUniformBlockIndex)(unsigned,const char*); void(*UniformBlockBinding)(unsigned,unsigned,unsigned); void(*BindBufferRange)(unsigned,unsigned,unsigned,long,long);
    void(*DrawBuffers)(int,const unsigned*); void(*DrawBuffer)(unsigned); void(*DepthFunc)(unsigned); void(*ClearDepth)(double); void(*ClearStencil)(int);
    void(*StencilFuncSeparate)(unsigned,unsigned,int,unsigned); void(*StencilOpSeparate)(unsigned,unsigned,unsigned,unsigned); void(*StencilMask)(unsigned);
    void(*Enablei)(unsigned,unsigned); void(*Disablei)(unsigned,unsigned); void(*BlendFuncSeparatei)(unsigned,unsigned,unsigned,unsigned,unsigned);
    void(*BlendEquationSeparatei)(unsigned,unsigned,unsigned); void(*ColorMaski)(unsigned,unsigned char,unsigned char,unsigned char,unsigned char);
} D3DGLApi;
static void bmp_write_rgba(const char*path,int W,int H,const unsigned char*px); // fwd (defined in gl_selftest section)
static unsigned char* bmp_load_rgba(const char*path,int*pw,int*ph);              // fwd (defined in gl_selftest section)
static D3DGLApi G;
static pthread_mutex_t g_d3dgl_lock=PTHREAD_MUTEX_INITIALIZER; // guards one-time init (race-free)
// halo3 drives the D3D11 immediate context from MORE THAN ONE thread: gameplay draws on the render
// thread, but the end-of-frame CopyResource(backbuffer <- final frame) + Present (and some clears) come
// from another thread. The old rule "only the thread that created the GL context may use it" silently
// dropped all of those, so the backbuffer never received the gameplay frame. Instead, ONE context is
// shared by every calling thread, serialized by this recursive mutex (same context => FBO/VAO names stay
// valid, which a second shared context would not give us).
static pthread_mutex_t g_glmx=PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#define GLLOCK()   pthread_mutex_lock(&g_glmx)
#define GLUNLOCK() pthread_mutex_unlock(&g_glmx)
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
// ---- on-screen stats: "<fps> fps | <draws> draws/frame | <ms> ms" in the window title (1 Hz) ----------
// Counts the ENGINE's D3D Present calls (real game frame rate, not the window re-blit rate). Draws are the
// D3D draw calls the shim executed. The title is set on the main thread via the main dispatch queue (the
// client's message pump runs it). PELOAD_NO_FPS_TITLE disables; the line is also logged with PELOAD_FPS_LOG.
#include <dispatch/dispatch.h>
static unsigned long g_draws_total;
static char g_fps_title[160];
// Launch time = start of the process that launched this one when that was a peload process started within
// the last 2 minutes (the window lives in the spawned worker), else this process.
#include <sys/sysctl.h>
static double proc_start_wall(pid_t pid){ struct kinfo_proc kp; size_t len=sizeof kp; int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PID,(int)pid};
    if(sysctl(mib,4,&kp,&len,0,0)!=0||len==0) return 0; return kp.kp_proc.p_starttime.tv_sec+kp.kp_proc.p_starttime.tv_usec*1e-6; }
static double g_launch_mono;   // launch time on the CLOCK_MONOTONIC scale of mono_s()
static void launch_time_init(void){ if(g_launch_mono) return; struct timeval tv; gettimeofday(&tv,0); double wall=tv.tv_sec+tv.tv_usec*1e-6;
    double me=proc_start_wall(getpid()), par=proc_start_wall(getppid()); double st=me?me:wall;
    if(par && me && par<me && me-par<120.0){ struct kinfo_proc kp; size_t len=sizeof kp; int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PID,(int)getppid()};
        if(sysctl(mib,4,&kp,&len,0,0)==0 && strstr(kp.kp_proc.p_comm,"peload")) st=par; }
    g_launch_mono=mono_s()-(wall-st); }
// Window title, refreshed every second on the main queue (also while the window is still black during
// startup): "starting... m:ss" until the first engine frame, then fps / draws / frame time, the load time
// once the match or the frontend menu is ready, and the elapsed time since launch.
static void fps_title_main(void*ctx){ (void)ctx; extern id g_nswin_ext(void); id w=g_nswin_ext(); if(!w) return;
    extern double g_gameplay_t0, g_menu_ready_t0; launch_time_init(); double up=mono_s()-g_launch_mono; int um=(int)(up/60), us=(int)up%60;
    char t[260]; double ready=g_gameplay_t0?g_gameplay_t0:g_menu_ready_t0;
    if(!g_fps_title[0]) snprintf(t,sizeof t,"Project Reclaimer (Mac, Wine-free)  |  starting...  %d:%02d",um,us);
    else if(ready) snprintf(t,sizeof t,"%s  |  %s in %.1f s  |  up %d:%02d",g_fps_title,g_gameplay_t0?"match loaded":"menu loaded",ready-g_launch_mono,um,us);
    else snprintf(t,sizeof t,"%s  |  loading...  %d:%02d",g_fps_title,um,us);
    { static int n; if(getenv("PELOAD_FPS_LOG") && (n++%5)==0){ char m[300]; int k=snprintf(m,sizeof m,"  [title] %s\n",t); (void)!write(2,m,k); } }
    // PELOAD_TITLE_PREFIX labels this window (e.g. "TARGET (no input)" for shim/target.sh); PELOAD_MINIMIZE puts
    // it in the Dock once it exists (keep PELOAD_PRESENT_HIDDEN=1 so it keeps presenting while minimized).
    { const char*pre=getenv("PELOAD_TITLE_PREFIX"); if(pre&&*pre){ char t2[300]; snprintf(t2,sizeof t2,"%s  |  %s",pre,t); snprintf(t,sizeof t,"%s",t2); } }
    { static int mini; if(!mini && getenv("PELOAD_MINIMIZE") && atoi(getenv("PELOAD_MINIMIZE"))){ mini=1; ((void(*)(id,SEL,id))objc_msgSend)(w,sel_registerName("miniaturize:"),(id)0); } }
    id str=((id(*)(id,SEL,const char*))objc_msgSend)((id)objc_getClass("NSString"),sel_registerName("stringWithUTF8String:"),t);
    ((void(*)(id,SEL,id))objc_msgSend)(w,sel_registerName("setTitle:"),str); }
static void title_timer_start(void){ static int started; if(started||getenv("PELOAD_NO_FPS_TITLE")) return; started=1; launch_time_init();
    dispatch_source_t ds=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(ds,dispatch_time(DISPATCH_TIME_NOW,0),NSEC_PER_SEC,NSEC_PER_SEC/10);
    dispatch_set_context(ds,0); dispatch_source_set_event_handler_f(ds,fps_title_main); dispatch_resume(ds); }
void title_timer_start_ext(void){ title_timer_start(); }
static void fps_title_tick(void){
    static double t0; static unsigned long n0, d0; static int off=-1; if(off<0) off=getenv("PELOAD_NO_FPS_TITLE")?1:0; if(off) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double now=ts.tv_sec+ts.tv_nsec*1e-9;
    static unsigned long frames; frames++;
    if(!t0){ t0=now; n0=frames; d0=g_draws_total; return; }
    double dt=now-t0; if(dt<1.0) return;
    double fps=(frames-n0)/dt; double dpf=(frames>n0)?(double)(g_draws_total-d0)/(frames-n0):0;
    // fps = frames that can reach the screen. When the engine presents faster than the display refreshes (loading
    // and "synchronizing players" screens spin at ~1000 presents/s with a handful of draws), the screen still shows
    // at most g_screen_hz of them: report that, and the engine's own rate beside it.
    if(g_screen_hz>0 && fps>g_screen_hz*1.02)
        snprintf(g_fps_title,sizeof g_fps_title,"Project Reclaimer (Mac, Wine-free)  |  %d fps (screen max; engine %.0f)  |  %.0f draws/frame",g_screen_hz,fps,dpf);
    else snprintf(g_fps_title,sizeof g_fps_title,"Project Reclaimer (Mac, Wine-free)  |  %.1f fps  |  %.0f draws/frame  |  %.1f ms",fps,dpf,fps>0?1000.0/fps:0);
    if(getenv("PELOAD_FPS_LOG")){ char m[200]; int k=snprintf(m,sizeof m,"  [fps] %s\n",g_fps_title+35); (void)!write(2,m,k); }
    t0=now; n0=frames; d0=g_draws_total; }
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
#define GLE_REPEAT 0x2901
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
    L(VertexAttribPointer);L(EnableVertexAttribArray);L(DisableVertexAttribArray);L(VertexAttribIPointer);
    L(DrawArrays);L(DrawElements);L(DrawArraysInstanced);L(DrawElementsInstanced);
    L(Viewport);L(ClearColor);L(Clear);L(Enable);L(Disable);L(BlendFunc);L(GetError);L(Flush);
    L(ReadBuffer);L(ReadPixels);
    L(GenRenderbuffers);L(BindRenderbuffer);L(RenderbufferStorage);L(FramebufferRenderbuffer);L(DepthMask);
    L(DrawElementsBaseVertex);L(DrawElementsInstancedBaseVertex);L(VertexAttribDivisor);L(BufferSubData);L(PrimitiveRestartIndex);L(BlitFramebuffer);L(Finish);
    L(GenQueries);L(BeginQuery);L(EndQuery);L(GetQueryObjectiv);L(GetQueryObjectui64v);
    L(BlendFuncSeparate);L(BlendEquationSeparate);L(BlendColor);L(ColorMask);L(CullFace);L(FrontFace);L(Scissor);
    L(GenSamplers);L(BindSampler);L(SamplerParameteri);L(SamplerParameterf);L(SamplerParameterfv);
    L(GetUniformBlockIndex);L(UniformBlockBinding);L(BindBufferRange);L(MapBufferRange);L(UnmapBuffer);L(TexBuffer);
    L(DrawBuffers);L(DrawBuffer);L(DepthFunc);L(ClearDepth);L(ClearStencil);L(StencilFuncSeparate);L(StencilOpSeparate);L(StencilMask);
    L(Enablei);L(Disablei);L(BlendFuncSeparatei);L(BlendEquationSeparatei);L(ColorMaski);
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
    NSRect_ rect={0,0,(double)g_d3dgl_W,(double)g_d3dgl_H}; win_initial_size(&rect.w,&rect.h);
    id win = ((id(*)(id,SEL,NSRect_,unsigned long,unsigned long,signed char))objc_msgSend)(
        om0(ocls("NSWindow"),"alloc"),osel("initWithContentRect:styleMask:backing:defer:"),rect,15,2,0);
    id title = ((id(*)(id,SEL,const char*))objc_msgSend)(ocls("NSString"),osel("stringWithUTF8String:"),"Project Reclaimer — halo3 D3D11->GL");
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("setTitle:"),title);
    id view = om0(win,"contentView");
    ((void(*)(id,SEL,id))objc_msgSend)(ctx,osel("setView:"),view);
    ((void(*)(id,SEL,id))objc_msgSend)(win,osel("center"),(id)0);
    ((void(*)(id,SEL,signed char))objc_msgSend)(win,osel("setAcceptsMouseMovedEvents:"),1);
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
    if(share && g_nspf && !getenv("PELOAD_D3DGL_NOSHAREPF")){
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
// ---- queries (ID3D11Query): occlusion on GL SAMPLES_PASSED, the rest answered on the CPU -------------------
// Only active with the GL renderer; otherwise GetData keeps the old "S_OK, nothing written" behaviour.
static D3DObj* g_q_active;   // occlusion query currently recording (GL allows one per target)
static int q_gl(D3DObj*o){ return (o->sd[0]==1||o->sd[0]==5) && g_d3dgl_ready && G.BeginQuery && G.EndQuery && G.GenQueries && G.GetQueryObjectiv && !getenv("PELOAD_D3DGL_NOQUERY"); }
static void MSABI ctx_Begin(void*t,void*a){ (void)t; D3DObj*o=a; if(!o||o->stkind!=5) return;
    if(q_gl(o)){ GLLOCK(); if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
        if(!o->glq) G.GenQueries(1,&o->glq);
        if(g_q_active){ G.EndQuery(0x8914/*SAMPLES_PASSED*/); g_q_active->qstate=2; g_q_active=0; }
        G.BeginQuery(0x8914,o->glq); g_q_active=o; GLUNLOCK(); }
    o->qstate=1; }
static void MSABI ctx_End(void*t,void*a){ (void)t; D3DObj*o=a; if(!o||o->stkind!=5) return;
    if(o->sd[0]==2) o->qval=(uint64_t)(mono_s()*1e9);                    // TIMESTAMP (ns; disjoint reports 1 GHz)
    if(g_q_active==o){ GLLOCK(); if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl); G.EndQuery(0x8914); GLUNLOCK(); g_q_active=0; }
    o->qstate=2; }
static int MSABI ctx_GetData(void*t,void*a,void*data,uint32_t size,uint32_t flags){ (void)t; D3DObj*o=a;
    if(!o||o->stkind!=5||!d3dgl_enabled()) return 0;
    uint8_t buf[88]; memset(buf,0,sizeof buf); uint32_t ty=o->sd[0], n=query_GetDataSize(o);
    switch(ty){
    case 0: *(uint32_t*)buf=1; break;                                     // EVENT: GPU is always "done"
    case 1: case 5: {                                                     // OCCLUSION (UINT64 samples) / _PREDICATE (BOOL)
        if(o->qstate==1 && g_q_active==o) return 1;                        // still recording: S_FALSE
        uint64_t r=0;
        if(o->glq && q_gl(o)){ GLLOCK(); if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
            int av=0; G.GetQueryObjectiv(o->glq,0x8867/*RESULT_AVAILABLE*/,&av);
            if(!av){ if(!(flags&1/*DONOTFLUSH*/)&&G.Flush) G.Flush(); GLUNLOCK(); return 1; }
            if(G.GetQueryObjectui64v) G.GetQueryObjectui64v(o->glq,0x8866/*RESULT*/,&r); else { int ri=0; G.GetQueryObjectiv(o->glq,0x8866,&ri); r=(uint32_t)ri; }
            GLUNLOCK(); }
        if(ty==1) memcpy(buf,&r,8); else *(uint32_t*)buf=r?1:0;
        if(getenv("PELOAD_TRACE_QUERY")){ static int nq; if(nq++<60){ char m[96]; int k=snprintf(m,sizeof m,"  [query] %p type %u -> %llu\n",(void*)o,ty,(unsigned long long)r); (void)!write(2,m,k); } }
        break; }
    case 2: { uint64_t v=o->qval?o->qval:(uint64_t)(mono_s()*1e9); memcpy(buf,&v,8); break; }
    case 3: { uint64_t f=1000000000ull; memcpy(buf,&f,8); break; }        // TIMESTAMP_DISJOINT {Frequency, Disjoint=FALSE}
    default: break;                                                       // statistics: zeros
    }
    if(data&&size) memcpy(data,buf,size<n?size:n);
    return 0; }
static int d3dgl_ensure(void){
    if(g_d3dgl_ready){   // any thread may drive the context (callers hold g_glmx); make it current here
        if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
        return 1;
    }
    if(g_d3dgl_fail) return 0;
    // one-time init, serialized so two render threads can't double-create the context/window
    pthread_mutex_lock(&g_d3dgl_lock);
    if(g_d3dgl_ready || g_d3dgl_fail){ pthread_mutex_unlock(&g_d3dgl_lock);
        if(g_d3dgl_fail) return 0;
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
    G.Enable(0x884F/*TEXTURE_CUBE_MAP_SEAMLESS*/);  // D3D10+ cube sampling filters across face edges
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
static int d3dgl_flipy(void); // fwd (row convention of rendered targets)
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
        if(isvs && (!strncmp(q,"in uvec4 vi",11)||!strncmp(q,"in ivec4 vi",11))){   // integer vertex attribute: location = register
            long r=atol(q+11); if(r>=0&&r<64){ char pre[40]; int k=snprintf(pre,sizeof pre,"layout(location=%ld) ",r); PUT(pre,k); } }
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
        if(!strncmp(q,"#version",8) && !d3dgl_flipy()){ const char*df="#define DX_NOYFLIP 1\n"; PUT(df,strlen(df)); }
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
// Decode one BC1 color block (8B) into 16 RGBA8 texels (row-major 4x4). c1mode: 1 => DXT1 (3-color +
// 1-bit alpha when c0<=c1); 0 => always 4-color (as used inside BC2/BC3).
static void dg_bc1_block(const uint8_t*s,uint8_t out[16][4],int c1mode){
    unsigned c0=s[0]|(s[1]<<8), c1=s[2]|(s[3]<<8); unsigned idx=s[4]|(s[5]<<8)|(s[6]<<16)|((unsigned)s[7]<<24);
    int r0=(c0>>11)&31,g0=(c0>>5)&63,b0=c0&31, r1=(c1>>11)&31,g1=(c1>>5)&63,b1=c1&31;
    int R[4],G[4],B[4],A[4];
    R[0]=(r0<<3)|(r0>>2);G[0]=(g0<<2)|(g0>>4);B[0]=(b0<<3)|(b0>>2);A[0]=255;
    R[1]=(r1<<3)|(r1>>2);G[1]=(g1<<2)|(g1>>4);B[1]=(b1<<3)|(b1>>2);A[1]=255;
    if(!c1mode || c0>c1){ R[2]=(2*R[0]+R[1])/3;G[2]=(2*G[0]+G[1])/3;B[2]=(2*B[0]+B[1])/3;A[2]=255;
                          R[3]=(R[0]+2*R[1])/3;G[3]=(G[0]+2*G[1])/3;B[3]=(B[0]+2*B[1])/3;A[3]=255; }
    else { R[2]=(R[0]+R[1])/2;G[2]=(G[0]+G[1])/2;B[2]=(B[0]+B[1])/2;A[2]=255;
           R[3]=0;G[3]=0;B[3]=0;A[3]=0; }
    for(int i=0;i<16;i++){ int c=(idx>>(2*i))&3; out[i][0]=(uint8_t)R[c];out[i][1]=(uint8_t)G[c];out[i][2]=(uint8_t)B[c];out[i][3]=(uint8_t)A[c]; }
}
// Decode one BC4/alpha block (8B) into 16 single-channel values (used for DXT5 alpha + BC5 R/G).
static void dg_bc4_block(const uint8_t*s,uint8_t out[16],int sgn){
    int a0=s[0],a1=s[1]; int v[8];
    if(sgn){ int sa0=(int8_t)s[0],sa1=(int8_t)s[1]; a0=sa0+128;a1=sa1+128; } // SNORM -> map to 0..255 domain
    v[0]=a0;v[1]=a1;
    if(a0>a1){ for(int i=1;i<=6;i++) v[i+1]=((7-i)*a0+i*a1)/7; }
    else { for(int i=1;i<=4;i++) v[i+1]=((5-i)*a0+i*a1)/5; v[6]=0; v[7]=255; }
    uint64_t bits=0; for(int i=0;i<6;i++) bits|=((uint64_t)s[2+i])<<(8*i);
    for(int i=0;i<16;i++){ int c=(int)((bits>>(3*i))&7); int val=v[c]; if(val<0)val=0; if(val>255)val=255; out[i]=(uint8_t)val; }
}
// Decode a BC1/BC2/BC3/BC4/BC5 image to a freshly-malloc'd RGBA8 buffer (w*h*4), or NULL if unsupported
// or the source is too small. BC5 is treated as a normal map: RG = decoded X,Y; B = reconstructed Z.
static uint8_t* dg_bc_decode(uint32_t fmt,const uint8_t*src,size_t srclen,int w,int h){
    int bc1=(fmt>=70&&fmt<=72), bc2=(fmt>=73&&fmt<=75), bc3=(fmt>=76&&fmt<=78),
        bc4=(fmt>=79&&fmt<=81), bc5=(fmt>=82&&fmt<=84);
    if(!(bc1||bc2||bc3||bc4||bc5)) return 0;              // BC6/7 not decoded here
    int blkb=(bc1||bc4)?8:16; int bw=(w+3)/4, bh=(h+3)/4;
    if((size_t)bw*bh*blkb>srclen) return 0;
    uint8_t*out=calloc(1,(size_t)w*h*4); if(!out) return 0;
    for(int by=0;by<bh;by++) for(int bx=0;bx<bw;bx++){
        const uint8_t*blk=src+((size_t)by*bw+bx)*blkb; uint8_t texel[16][4];
        if(bc1){ dg_bc1_block(blk,texel,1); }
        else if(bc2){ dg_bc1_block(blk+8,texel,0); for(int i=0;i<16;i++){ int a=(blk[i/2]>>((i&1)*4))&0xf; texel[i][3]=(uint8_t)((a<<4)|a);} }
        else if(bc3){ uint8_t al[16]; dg_bc4_block(blk,al,0); dg_bc1_block(blk+8,texel,0); for(int i=0;i<16;i++) texel[i][3]=al[i]; }
        // BC4/BC5 return D3D's (r,0,0,1) / (r,g,0,1) -- shaders reconstruct normal z themselves. The SNORM
        // variants (81/84: halo3's normal maps) are emitted as SIGNED bytes for an RGBA8_SNORM upload, so x,y
        // come back in [-1,1]; mapping them to [0,1] tilted every normal toward +x+y.
        else if(bc4){ uint8_t rr[16]; int sn=(fmt==81); dg_bc4_block(blk,rr,sn);
               for(int i=0;i<16;i++){ texel[i][0]=sn?(uint8_t)(int8_t)(rr[i]-128):rr[i]; texel[i][1]=0; texel[i][2]=0; texel[i][3]=sn?127:255; } }
        else { uint8_t rr[16],gg[16]; int sn=(fmt==84); dg_bc4_block(blk,rr,sn); dg_bc4_block(blk+8,gg,sn);
               for(int i=0;i<16;i++){ texel[i][0]=sn?(uint8_t)(int8_t)(rr[i]-128):rr[i]; texel[i][1]=sn?(uint8_t)(int8_t)(gg[i]-128):gg[i]; texel[i][2]=0; texel[i][3]=sn?127:255; } }
        for(int py=0;py<4;py++) for(int px=0;px<4;px++){ int x=bx*4+px,y=by*4+py; if(x>=w||y>=h)continue;
            uint8_t*d=out+((size_t)y*w+x)*4; const uint8_t*t=texel[py*4+px]; d[0]=t[0];d[1]=t[1];d[2]=t[2];d[3]=t[3]; }
    }
    return out;
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
static void d3dgl_ensure_depth(D3DObj*o); // fwd (depth-stencil resources)
// per-unit binding cache of the draw path (texture+target, sampler); any other BindTexture on the render
// context (uploads, RT/depth creation, blits) invalidates it
static unsigned g_unit_tex[32], g_unit_tgt[32], g_unit_samp[32]; static int g_unitcache_ok;
#define TEXCACHE_INVALIDATE() (g_unitcache_ok=0)
// DXGI -> GL upload format for uncompressed textures. swz: 1 = A8 (0,0,0,R), 2 = X8 formats (alpha = 1).
// Anything not listed (integer/depth formats) falls back to the 1x1 magenta placeholder.
typedef struct { unsigned ifmt, fmt, type; int swz; } GLFmt;
static int d3dgl_fmt(uint32_t f,GLFmt*g){
    #define GF(a,b,c,d) do{ g->ifmt=a; g->fmt=b; g->type=c; g->swz=d; return 1; }while(0)
    switch(f){
        case 1: case 2:   GF(0x8814,0x1908,0x1406,0);   // R32G32B32A32_FLOAT  RGBA32F
        case 5: case 6:   GF(0x8815,0x1907,0x1406,0);   // R32G32B32_FLOAT     RGB32F
        case 9: case 10:  GF(0x881A,0x1908,0x140B,0);   // R16G16B16A16_FLOAT  RGBA16F
        case 11:          GF(0x805B,0x1908,0x1403,0);   // R16G16B16A16_UNORM  RGBA16
        case 13:          GF(0x8F9B,0x1908,0x1402,0);   // R16G16B16A16_SNORM
        case 15: case 16: GF(0x8230,0x8227,0x1406,0);   // R32G32_FLOAT        RG32F
        case 23: case 24: GF(0x8059,0x1908,0x8368,0);   // R10G10B10A2_UNORM   RGB10_A2 / 2_10_10_10_REV
        case 26:          GF(0x8C3A,0x1907,0x8C3B,0);   // R11G11B10_FLOAT
        case 27: case 28: case 29: GF(nosrgb()?GLE_RGBA8:0x8C43,GLE_RGBA,GLE_UBYTE,0); // R8G8B8A8: sRGB storage, decode per view
        case 31:          GF(0x8F97,0x1908,0x1400,0);   // R8G8B8A8_SNORM
        case 33: case 34: GF(0x822F,0x8227,0x140B,0);   // R16G16_FLOAT        RG16F
        case 35:          GF(0x822C,0x8227,0x1403,0);   // R16G16_UNORM
        case 37:          GF(0x8F99,0x8227,0x1402,0);   // R16G16_SNORM
        case 39: case 40: case 41: GF(0x822E,0x1903,0x1406,0); // R32_FLOAT / D32_FLOAT  R32F
        case 48: case 49: GF(0x822B,0x8227,0x1401,0);   // R8G8_UNORM          RG8
        case 51:          GF(0x8F95,0x8227,0x1400,0);   // R8G8_SNORM
        case 53: case 54: GF(0x822D,0x1903,0x140B,0);   // R16_FLOAT           R16F
        case 55: case 56: GF(0x822A,0x1903,0x1403,0);   // D16 / R16_UNORM     R16
        case 58:          GF(0x8F98,0x1903,0x1402,0);   // R16_SNORM
        case 60: case 61: GF(0x8229,0x1903,0x1401,0);   // R8_UNORM            R8
        case 63:          GF(0x8F94,0x1903,0x1400,0);   // R8_SNORM
        case 65:          GF(0x8229,0x1903,0x1401,1);   // A8_UNORM -> R8 swizzled to (0,0,0,R)
        case 67:          GF(0x8C3D,0x1907,0x8C3E,0);   // R9G9B9E5_SHAREDEXP
        case 85:          GF(0x8D62,0x1907,0x8363,0);   // B5G6R5_UNORM        RGB565
        case 86:          GF(0x8057,0x80E1,0x8366,0);   // B5G5R5A1_UNORM
        case 87: case 90: case 91: GF(nosrgb()?GLE_RGBA8:0x8C43,GLE_BGRA,GLE_UI8REV,0); // B8G8R8A8
        case 88: case 92: case 93: GF(nosrgb()?GLE_RGBA8:0x8C43,GLE_BGRA,GLE_UI8REV,2); // B8G8R8X8 (alpha reads 1)
        case 115:         GF(0x8056,0x80E1,0x8365,0);   // B4G4R4A4_UNORM
    }
    #undef GF
    return 0; }
static void d3dgl_apply_swz(unsigned tgt,int swz){
    if(swz==1){ G.TexParameteri(tgt,0x8E42,0); G.TexParameteri(tgt,0x8E43,0); G.TexParameteri(tgt,0x8E44,0); G.TexParameteri(tgt,0x8E45,0x1903); }
    else if(swz==2) G.TexParameteri(tgt,0x8E45,1); }
// GL target for a sampled (non-render-target) texture resource.
static unsigned d3dgl_tex_target(const D3DObj*o){
    if(o->kind==4) return 0x806F;                                                   // TEXTURE_3D
    if((o->misc&4) && o->arr>=6 && o->arr%6==0) return o->arr==6?0x8513:0x9009;     // CUBE_MAP / CUBE_MAP_ARRAY
    if(o->arr>1) return 0x8C1A;                                                     // TEXTURE_2D_ARRAY
    return GLE_TEX2D; }
// Pixels of subresource (slice sl, mip m) ready for GL: BC -> decoded RGBA8 / RGBA8_SNORM (malloc'd, *own=1);
// uncompressed -> pointer into o->data (rows are tight). NULL = unsupported. *bpp = bytes per uploaded texel.
static const uint8_t* d3dgl_sub_pixels(D3DObj*o,uint32_t sl,uint32_t m,GLFmt*g,int*own,uint32_t*bpp,uint32_t*mw,uint32_t*mh,uint32_t*md){
    uint64_t sz; uint32_t rp,rows; uint64_t off=tex_sub(o,m+sl*tex_nmips(o),&sz,&rp,&rows,mw,mh,md); *own=0;
    if(off==UINT64_MAX) return 0;
    if(is_bc(o->fmt)){ int sn=(o->fmt==81||o->fmt==84), srgb=(o->fmt==72||o->fmt==75||o->fmt==78);
        uint64_t lsz=(uint64_t)rp*rows; size_t pl=(size_t)(*mw)*(*mh)*4; uint8_t*out=malloc(pl*(*md)); if(!out) return 0;
        for(uint32_t z=0;z<*md;z++){ uint8_t*dz=dg_bc_decode(o->fmt,(const uint8_t*)o->data+off+z*lsz,(size_t)lsz,(int)*mw,(int)*mh);
            if(!dz){ free(out); return 0; } memcpy(out+z*pl,dz,pl); free(dz); }
        if(!nosrgb() && o->fmt>=70 && o->fmt<=78) srgb=1;
        g->ifmt=sn?0x8F97:(srgb?0x8C43:GLE_RGBA8); g->fmt=GLE_RGBA; g->type=sn?0x1400:GLE_UBYTE; g->swz=0; *own=1; *bpp=4; return out; }
    if(!d3dgl_fmt(o->fmt,g)) return 0;
    *bpp=dxgi_bpp(o->fmt); return (const uint8_t*)o->data+off; }
// Upload a cube map / cube array / 2D array / 3D texture: every face/slice/depth layer (the old path only
// ever had a single 2D image of face 0 / slice 0, sampled with 2D coords). BC assets stream their small
// mips first, so start at the highest-res level whose slice 0 is substantially populated; the chain
// below it is regenerated (env maps are sampled with explicit LOD).
static void d3dgl_upload_layered(D3DObj*o,unsigned tgt){
    static void(*ti3)(unsigned,int,int,int,int,int,int,unsigned,unsigned,const void*); static void(*gm)(unsigned); static void(*psi)(unsigned,int); static unsigned(*ge)(void); static int ini;
    if(!ini){ ini=1; ti3=dlsym(RTLD_DEFAULT,"glTexImage3D"); gm=dlsym(RTLD_DEFAULT,"glGenerateMipmap"); psi=dlsym(RTLD_DEFAULT,"glPixelStorei"); ge=dlsym(RTLD_DEFAULT,"glGetError"); }
    uint32_t nm=tex_nmips(o), ns=tex_nslices(o), L=0;
    if(is_bc(o->fmt)) for(uint32_t m=0;m<nm;m++){ uint64_t sz; uint32_t rp,rows,a,b,c; uint64_t off=tex_sub(o,m,&sz,&rp,&rows,&a,&b,&c); if(off==UINT64_MAX) break;
        int nz=0,n=0; uint64_t st=sz/128?sz/128:1; for(uint64_t i=0;i<sz;i+=st){ n++; if(((const uint8_t*)o->data)[off+i]) nz++; }
        L=m; if(n && nz*100>=n*55) break; }
    GLFmt g; int own; uint32_t bpp=4,mw=1,mh=1,md=1;
    const uint8_t*p0=o->data?d3dgl_sub_pixels(o,0,L,&g,&own,&bpp,&mw,&mh,&md):0;
    if(ge) ge();
    if(psi) psi(0x0CF5,1);
    if(!p0 || !ti3){ static const uint8_t mag[4*6]={255,0,255,255,255,0,255,255,255,0,255,255,255,0,255,255,255,0,255,255,255,0,255,255};
        if(tgt==0x8513) for(int f=0;f<6;f++) G.TexImage2D(0x8515+f,0,GLE_RGBA8,1,1,0,GLE_RGBA,GLE_UBYTE,mag);
        else if(ti3) ti3(tgt,0,GLE_RGBA8,1,1,tgt==0x9009?6:1,0,GLE_RGBA,GLE_UBYTE,mag);
        G.TexParameteri(tgt,0x2801,GLE_LINEAR); G.TexParameteri(tgt,0x813D,0); o->glhasmips=0;
        if(psi) psi(0x0CF5,4);
        if(getenv("PELOAD_TRACE_TEXDIM")){ char m[160]; int k=snprintf(m,sizeof m,"  [texdim] UPLOAD tgt=0x%x %ux%u f%u UNSUPPORTED -> placeholder\n",tgt,o->w,o->h,o->fmt); (void)!write(2,m,k); }
        return; }
    if(own) free((void*)p0);
    if(tgt==0x8513){ for(uint32_t f=0;f<6;f++){ const uint8_t*p=d3dgl_sub_pixels(o,f,L,&g,&own,&bpp,&mw,&mh,&md); if(!p) continue;
            G.TexImage2D(0x8515+f,0,g.ifmt,(int)mw,(int)mh,0,g.fmt,g.type,p); if(own) free((void*)p); } }
    else if(tgt==0x806F){ const uint8_t*p=d3dgl_sub_pixels(o,0,L,&g,&own,&bpp,&mw,&mh,&md);
        if(p){ ti3(tgt,0,g.ifmt,(int)mw,(int)mh,(int)md,0,g.fmt,g.type,p); if(own) free((void*)p); } }
    else { size_t lb=(size_t)mw*mh*bpp; uint8_t*all=calloc(ns,lb);
        if(all){ for(uint32_t sl=0;sl<ns;sl++){ uint32_t a,b,c; const uint8_t*p=d3dgl_sub_pixels(o,sl,L,&g,&own,&bpp,&a,&b,&c);
                if(p){ if(a==mw&&b==mh) memcpy(all+sl*lb,p,lb); if(own) free((void*)p); } }
            ti3(tgt,0,g.ifmt,(int)mw,(int)mh,(int)ns,0,g.fmt,g.type,all); free(all); } }
    if(psi) psi(0x0CF5,4);
    unsigned e=ge?ge():0;
    d3dgl_apply_swz(tgt,g.swz); o->glsrgb=(g.ifmt==0x8C43);
    o->glhasmips=0;
    if(!e && nm-L>1 && gm && !getenv("PELOAD_D3DGL_NOMIP")){ gm(tgt); if(!(ge&&ge())){ o->glhasmips=1; G.TexParameteri(tgt,0x2801,0x2703); G.TexParameteri(tgt,0x813D,1000); } }
    if(!o->glhasmips){ G.TexParameteri(tgt,0x2801,GLE_LINEAR); G.TexParameteri(tgt,0x813D,0); }
    if(getenv("PELOAD_TRACE_TEXDIM")){ static int tc; if(tc++<80){ char m[200]; int k=snprintf(m,sizeof m,"  [texdim] UPLOAD tgt=0x%x %ux%u layers=%u depth=%u f%u base-mip=%u/%u ifmt=0x%x err=0x%x mips=%d\n",tgt,mw,mh,ns,md,o->fmt,L,nm,g.ifmt,e,o->glhasmips); (void)!write(2,m,k); } }
}
static void d3dgl_ensure_tex(D3DObj*o){
    if(!o) return;
    if((o->bindf&0x40) && !getenv("PELOAD_D3DGL_NODEPTH")){ d3dgl_ensure_depth(o); return; }   // DSV resource sampled via an SRV
    // cube / array / 3D resources get their own GL target (render targets are always plain 2D here)
    if(!o->gltarget) o->gltarget=(o->gltex||o->glfbo)?GLE_TEX2D:d3dgl_tex_target(o);
    if(o->gltarget!=GLE_TEX2D){
        if(!o->gltex){ G.GenTextures(1,&o->gltex); o->gl_uploaded=0; }
        if(o->gl_uploaded && !o->gl_dirty) return;
        TEXCACHE_INVALIDATE(); G.BindTexture(o->gltarget,o->gltex);
        unsigned wrap=(o->gltarget==0x8513||o->gltarget==0x9009)?GLE_CLAMP:GLE_REPEAT;
        G.TexParameteri(o->gltarget,0x2800,GLE_LINEAR); G.TexParameteri(o->gltarget,0x2802,wrap); G.TexParameteri(o->gltarget,0x2803,wrap); G.TexParameteri(o->gltarget,0x8072,wrap);
        d3dgl_upload_layered(o,o->gltarget); o->gl_uploaded=1; o->gl_dirty=0; return; }
    if(o==g_watch_tex && o->data){ static int wc; if(wc++<30){ int q[4]={0,0,0,0}; uint64_t mip0=(uint64_t)((o->w+3)/4)*((o->h+3)/4)*(is_bc(o->fmt)?bc_block(o->fmt):4); uint64_t qb=mip0/4; const uint8_t*d=o->data;
        if(qb) for(int Q=0;Q<4;Q++) for(uint64_t i=0;i<qb;i+=(qb/32?qb/32:1)) if(d[Q*qb+i])q[Q]++;
        char m[160]; int k=snprintf(m,sizeof m,"  [WATCH] UPLOAD mip0=%lluB data-quarters(nz)=[%d %d %d %d]/~32 dirty=%d\n",(unsigned long long)mip0,q[0],q[1],q[2],q[3],o->gl_dirty); (void)!write(2,m,k);} }
    if(!o->gltex){ TEXCACHE_INVALIDATE(); G.GenTextures(1,&o->gltex); G.BindTexture(GLE_TEX2D,o->gltex);
        G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2800,GLE_LINEAR);
        // Terrain/detail textures TILE (halo3's cb13 UV scales reach 8-48x); D3D11's default sampler is
        // WRAP, so REPEAT is the correct default. CLAMP_TO_EDGE made tiled UVs sample only the edge texel
        // -> dark/black terrain. PELOAD_D3DGL_CLAMP forces the old clamp.
        unsigned wrap = getenv("PELOAD_D3DGL_CLAMP")?GLE_CLAMP:GLE_REPEAT;
        G.TexParameteri(GLE_TEX2D,0x2802,wrap); G.TexParameteri(GLE_TEX2D,0x2803,wrap);
        o->gl_uploaded=0; }
    if(o->gl_uploaded && !o->gl_dirty) return;
    TEXCACHE_INVALIDATE(); G.BindTexture(GLE_TEX2D,o->gltex);
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
            // halo3 streams map textures mip-by-mip (low-res mips complete FIRST; the full-res mip 0 is
            // often only PARTIALLY streamed -> uploading it gives black BLOCKS that, with the tiled
            // terrain sampling, show as a regular grid of gaps). So upload the HIGHEST-RESOLUTION mip that
            // is SUBSTANTIALLY complete (>= thresh of sampled bytes nonzero); fall back to the most-
            // complete mip available (the smallest mip is always complete). PELOAD_D3DGL_MIPTHRESH tunes it.
            double thresh=getenv("PELOAD_D3DGL_MIPTHRESH")?atof(getenv("PELOAD_D3DGL_MIPTHRESH")):0.55; if(thresh<=0||thresh>1)thresh=0.55;
            uint32_t uw=w,uh=h; uint64_t uoff=0,usz=csz; int picked=0,baselvl=0; double bestfrac=-1; uint32_t bw=w,bh=h; uint64_t boff=0,bsz=csz; int bbl=0;
            for(uint32_t lvl=0;lvl<15;lvl++){ uint32_t mw=w>>lvl; if(!mw)mw=1; uint32_t mh=h>>lvl; if(!mh)mh=1;
                uint64_t msz=0,moff=tex_sub_off(o->w,o->h,o->fmt,lvl,&msz); if(!msz||moff+msz>o->size) break;
                int nz=0,ns=0; const uint8_t*dd=(const uint8_t*)o->data+moff; uint64_t step=msz/128?msz/128:1;
                for(uint64_t i=0;i<msz;i+=step){ ns++; if(dd[i])nz++; }
                double frac=ns?(double)nz/ns:0;
                if(frac>bestfrac){ bestfrac=frac; bw=mw; bh=mh; boff=moff; bsz=msz; bbl=(int)lvl; }   // track most-complete
                if(frac>=thresh){ uw=mw; uh=mh; uoff=moff; usz=msz; picked=1; baselvl=(int)lvl; break; }   // highest-res complete-enough
                if(mw==1&&mh==1) break; }
            if(!picked){ uw=bw; uh=bh; uoff=boff; usz=bsz; baselvl=bbl; }   // nothing met thresh -> use the most-complete mip
            // PELOAD_D3DGL_BCDECODE (default ON): Apple GL-on-Metal CANNOT sample a COMPRESSED texture with
            // any mipmap min-filter (returns BLACK), so heavily-tiled terrain textures alias badly (blue
            // moiré) with no way to mip them while compressed. Fix: DECODE the BC block to RGBA8 in the shim,
            // upload uncompressed, glGenerateMipmap, trilinear -> proper anti-aliasing. PELOAD_D3DGL_NOBCDECODE
            // keeps the raw compressed upload (single level, no mips).
            if(!getenv("PELOAD_D3DGL_NOBCDECODE")){
                uint8_t*rgba=dg_bc_decode(o->fmt,(const uint8_t*)o->data+uoff,(size_t)(o->size-uoff),(int)uw,(int)uh);
                if(rgba){ static void(*genmip)(unsigned); static int gmi; if(!gmi){ genmip=dlsym(RTLD_DEFAULT,"glGenerateMipmap"); gmi=1; }
                    if(gerr) gerr();
                    // SRGB BC formats (BC1/BC2/BC3 _SRGB) store gamma-encoded color -> upload as SRGB8_ALPHA8
                    // so GL de-gammas to linear on sample (matches the compressed-SRGB path we replaced).
                    int issrgb=nosrgb()?(o->fmt==72||o->fmt==75||o->fmt==78):(o->fmt>=70&&o->fmt<=78), issn=(o->fmt==81||o->fmt==84);
                    unsigned iform=issn?0x8F97/*RGBA8_SNORM*/:issrgb?0x8C43/*SRGB8_ALPHA8*/:GLE_RGBA8; o->glsrgb=(iform==0x8C43);
                    G.TexImage2D(GLE_TEX2D,0,iform,(int)uw,(int)uh,0,GLE_RGBA,issn?0x1400/*BYTE*/:GLE_UBYTE,rgba);
                    unsigned de=gerr?gerr():0; free(rgba);
                    if(!de){ int mip=!getenv("PELOAD_D3DGL_NOMIP");
                        if(mip&&genmip){ genmip(GLE_TEX2D); o->glhasmips=1; G.TexParameteri(GLE_TEX2D,0x2801,0x2703/*LINEAR_MIPMAP_LINEAR*/); G.TexParameteri(GLE_TEX2D,0x813D/*MAX_LEVEL*/,1000);
                            // anisotropic filtering: the terrain is viewed at grazing angles (one UV axis
                            // hugely minified) -> isotropic trilinear still aliases into vertical scanline
                            // moiré. GL_TEXTURE_MAX_ANISOTROPY (0x84FE) fixes grazing-angle sampling.
                            static void(*tpf)(unsigned,unsigned,float); static void(*gfv)(unsigned,float*); static int qa; static float qmax;
                            if(!qa){ qa=1; tpf=dlsym(RTLD_DEFAULT,"glTexParameterf"); gfv=dlsym(RTLD_DEFAULT,"glGetFloatv"); qmax=1; if(gfv) gfv(0x84FF/*MAX_TEXTURE_MAX_ANISOTROPY*/,&qmax); if(qmax<1)qmax=1; }
                            if(!getenv("PELOAD_D3DGL_NOANISO")){ float ma=16.0f; if(ma>qmax)ma=qmax; if(tpf) tpf(GLE_TEX2D,0x84FE/*TEXTURE_MAX_ANISOTROPY*/,ma); }
                            // PELOAD_D3DGL_LODBIAS=<f>: positive bias blurs (lower-res mip) -> softens the
                            // grazing-angle silhouette streaks that extreme tiling (48x) + aniso can't resolve.
                            if(getenv("PELOAD_D3DGL_LODBIAS")&&tpf){ float lb=atof(getenv("PELOAD_D3DGL_LODBIAS")); tpf(GLE_TEX2D,0x8501/*TEXTURE_LOD_BIAS*/,lb); } }
                        else G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR);
                        if(getenv("PELOAD_TRACE_TEXUP")){ static int dc; if(dc++<3){ static float qmx; static int qd; if(!qd){ qd=1; void(*gf)(unsigned,float*)=dlsym(RTLD_DEFAULT,"glGetFloatv"); qmx=0; if(gf)gf(0x84FF,&qmx); } char b[180]; int k=snprintf(b,sizeof b,"  [texup-bcdec] %dx%d fmt=%u -> RGBA8 base=%ux%u(dl%d) mips=%d maxaniso=%.1f\n",w,h,o->fmt,uw,uh,baselvl,mip&&genmip?1:0,qmx); (void)!write(2,b,k);} }
                        o->gl_uploaded=1; o->gl_dirty=0; dg_gltex_dump((int)uw,(int)uh,o->fmt); return; }
                    // decode-upload failed -> fall through to the compressed path
                }
            }
            // Upload a MIP CHAIN from the base (GL level 0) down to 1x1 (DXGI levels baselvl, baselvl+1, ...
            // — the smaller mips stream first so they're complete). A real chain + trilinear filtering kills
            // the aliasing MOIRÉ on the heavily-tiled (8-48x) terrain/normal textures. PELOAD_D3DGL_NOMIPCHAIN
            // uploads only the base level. (BC can't glGenerateMipmap, so upload the asset's own mips.)
            unsigned e=0; int gl_lvl=0; int domip=getenv("PELOAD_D3DGL_MIPCHAIN")?1:0;
            for(int dl=baselvl; dl<15; dl++){ uint32_t mw=w>>dl; if(!mw)mw=1; uint32_t mh=h>>dl; if(!mh)mh=1;
                uint64_t msz=0,moff=tex_sub_off(o->w,o->h,o->fmt,dl,&msz); if(!msz||moff+msz>o->size) break;
                if(gerr) gerr();   // clear before each level
                cti(GLE_TEX2D,gl_lvl,bcif,(int)mw,(int)mh,0,(int)msz,(uint8_t*)o->data+moff);
                unsigned ee=gerr?gerr():0;
                // The BASE level (gl_lvl 0) failing is fatal (fall back); a smaller mip failing (some
                // drivers reject BC mips < 4x4) just TRUNCATES the chain -- keep the good levels.
                if(ee){ if(gl_lvl==0){ e=ee; } break; }
                gl_lvl++;
                if(!domip || (mw<=4&&mh<=4)) break;   // single level, or stop at 4x4 (smaller BC mips are flaky)
            }
            if(!e){ // base ok (chain may be truncated): trilinear only if >1 level, cap max level
                unsigned minf=GLE_LINEAR;
                if(domip&&gl_lvl>1) minf=getenv("PELOAD_D3DGL_MIPNEAREST")?0x2701/*LINEAR_MIPMAP_NEAREST*/
                                        :getenv("PELOAD_D3DGL_MIPLINEAR")?GLE_LINEAR/*bilinear base, chain present*/
                                        :0x2703/*LINEAR_MIPMAP_LINEAR*/;
                G.TexParameteri(GLE_TEX2D,0x2801/*MIN*/, minf);
                G.TexParameteri(GLE_TEX2D,0x813D/*MAX_LEVEL*/, gl_lvl>0?gl_lvl-1:0); o->glhasmips=(gl_lvl>1);
            }
            if(getenv("PELOAD_TRACE_TEXUP")){ static int bc; if(bc++<40){ char b[180]; int k=snprintf(b,sizeof b,"  [texup-bc] %dx%d fmt=%u ifmt=0x%x base=%ux%u@%llu(dl%d) levels=%d err=0x%x%s\n",w,h,o->fmt,bcif,uw,uh,(unsigned long long)uoff,baselvl,gl_lvl,e,e?" (FALLBACK)":""); (void)!write(2,b,k);} }
            if(!e){ o->glsrgb=(o->fmt==72||o->fmt==75||o->fmt==78); o->gl_uploaded=1; o->gl_dirty=0; dg_gltex_dump((int)uw,(int)uh,o->fmt); return; } // success: BPTC may fail on GL4.1 -> fall through
        }
    }
    // Only upload real pixels when the backing buffer actually holds >= w*h*4 bytes; otherwise
    // glTexImage2D would read past o->data (heap OOB -> crash). Fall back to a 1x1 placeholder.
    GLFmt gfm; int can = o->data && d3dgl_fmt(o->fmt,&gfm) && o->size>=(uint64_t)w*h*dxgi_bpp(o->fmt); (void)gf;(void)gt;
    // PELOAD_TRACE_TEXUP: for sizeable textures, report w/h/fmt/can + whether the backing data is all
    // black (so we can tell if the loading-screen 1280x720 texture is uploaded empty vs the engine
    // actually rendered content into it).
    if(getenv("PELOAD_TRACE_TEXUP") && w>=256 && h>=256){ uint64_t sum=0; int nz=0;
        if(o->data && o->size>=1024){ const uint8_t*d=o->data; uint64_t step=o->size/4096; if(step<1)step=1;
            for(uint64_t i=0;i<o->size && nz<32; i+=step){ if(d[i]){ sum+=d[i]; nz++; } } }
        static int tc; if(tc++<60){ char b[160]; int k=snprintf(b,sizeof b,"  [texup] %dx%d fmt=%u size=%lluB can=%d data=%p nonzero-samples=%d sum=%llu%s\n",
            w,h,o->fmt,(unsigned long long)o->size,can,o->data,nz,(unsigned long long)sum,nz?"":" (ALL BLACK)"); (void)!write(2,b,k);} }
    if(can){ static void(*psi)(unsigned,int); static int pi; if(!pi){ pi=1; psi=dlsym(RTLD_DEFAULT,"glPixelStorei"); }
        if(psi) psi(0x0CF5,1); G.TexImage2D(GLE_TEX2D,0,(int)gfm.ifmt,w,h,0,gfm.fmt,gfm.type,o->data); if(psi) psi(0x0CF5,4); d3dgl_apply_swz(GLE_TEX2D,gfm.swz);
        o->glsrgb=(gfm.ifmt==0x8C43); }
    else { static const unsigned char mag[4]={255,0,255,255}; G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,1,1,0,GLE_RGBA,GLE_UBYTE,mag); }
    o->gl_uploaded=1; o->gl_dirty=0;
    dg_gltex_dump(w,h,o->fmt);
}
// DXGI render-target format -> GL internal format (renderable). sRGB and 8-bit UNORM stay RGBA8.
static unsigned d3dgl_rt_ifmt(uint32_t f){
    if(getenv("PELOAD_D3DGL_RT8")) return GLE_RGBA8;
    switch(f){ case 1: case 2: return 0x8814; case 9: case 10: return 0x881A; case 11: return 0x805B; case 15: case 16: return 0x8230;
        case 23: case 24: return 0x8059; case 26: return 0x8C3A; case 33: case 34: return 0x822F; case 35: return 0x822C;
        case 39: case 41: return 0x822E; case 48: case 49: return 0x822B; case 53: case 54: return 0x822D; case 56: return 0x822A;
        case 60: case 61: return 0x8229; default: return (dxgi_srgb_storable(f)&&!nosrgb())?0x8C43:GLE_RGBA8; } }
static void d3dgl_ensure_rt(D3DObj*o){
    if(!o) return;
    int w=o->w?(int)o->w:g_d3dgl_W, h=o->h?(int)o->h:g_d3dgl_H; if(w>4096)w=4096; if(h>4096)h=4096;
    if(o->gltex && o->gltarget && o->gltarget!=GLE_TEX2D){ o->gltex=0; o->gl_uploaded=0; }   // was sampled as a layered asset first
    o->gltarget=GLE_TEX2D;
    if(!o->gltex){ TEXCACHE_INVALIDATE(); G.GenTextures(1,&o->gltex); G.BindTexture(GLE_TEX2D,o->gltex);
        G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2800,GLE_LINEAR);
        G.TexParameteri(GLE_TEX2D,0x2802,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x2803,GLE_CLAMP);
        // the RT's real precision: HDR targets (lighting, bloom chain, the 1x1 average-luminance target that
        // drives auto-exposure) clamped at 1.0 in RGBA8 -> blown-out white frame
        G.TexImage2D(GLE_TEX2D,0,(int)d3dgl_rt_ifmt(o->fmt),w,h,0,GLE_RGBA,GLE_UBYTE,0); G.TexParameteri(GLE_TEX2D,0x813D/*MAX_LEVEL*/,0); o->gl_uploaded=1;
        o->glsrgb=(d3dgl_rt_ifmt(o->fmt)==0x8C43); }
    if(!o->glfbo){ G.GenFramebuffers(1,&o->glfbo); G.BindFramebuffer(GLE_FBO,o->glfbo);
        G.FramebufferTexture2D(GLE_FBO,GLE_CA0,GLE_TEX2D,o->gltex,0);
        if(G.CheckFramebufferStatus){ unsigned st=G.CheckFramebufferStatus(GLE_FBO); if(st!=0x8CD5){ static int nb; if(nb++<20){ char m[200];
            int k=snprintf(m,sizeof m,"  [d3dgl] FBO INCOMPLETE status=0x%x rt=%p %ux%u f%u kind%u gltex=%u (gl_uploaded=%d)\n",st,(void*)o,o->w,o->h,o->fmt,o->kind,o->gltex,o->gl_uploaded); (void)!write(2,m,k);} } }
        // attach a depth renderbuffer so geometry depth-tests correctly (opt-in; adds VRAM per RT)
        if(g_d3dgl_depth && G.GenRenderbuffers){ unsigned rb=0; G.GenRenderbuffers(1,&rb); G.BindRenderbuffer(0x8D41/*RENDERBUFFER*/,rb);
            G.RenderbufferStorage(0x8D41,0x81A6/*DEPTH_COMPONENT24*/,w,h);
            G.FramebufferRenderbuffer(GLE_FBO,0x8D00/*DEPTH_ATTACHMENT*/,0x8D41,rb); } }
}
// ---- GPU-side D3D copies -------------------------------------------------------------------------------
// A render target's pixels exist only in GL; its CPU shadow (o->data) is never written. The engine copies
// rendered targets every frame -- CopyResource(1920x1080 frame <- rendered scene RT) and
// CopySubresourceRegion(16x1 <- 4096^2 RT) readbacks -- and the old CPU memcpy copied STALE bytes, so the
// copied frame was empty and CPU readbacks (Map READ) saw zeros. When the source is GL-rendered, copy on
// the GPU (FBO blit) and, for small destinations (readbacks), also pull the pixels into the CPU shadow.
// Row convention of rendered targets. 1 (default) = GL row y holds D3D row y, like uploaded textures: the
// translated VS flips clip y (dx_posfix), so viewport/scissor/copies use D3D coordinates directly, PS
// SV_Position == gl_FragCoord, sampling a rendered target with D3D UVs reads the right rows, and present
// flips once. 0 (PELOAD_D3DGL_NOYFLIP) = the old no-flip layout (D3D row 0 at the TOP GL row).
static int g_d3dgl_flipy_v=-1;
static int d3dgl_flipy(void){ if(g_d3dgl_flipy_v<0) g_d3dgl_flipy_v=getenv("PELOAD_D3DGL_NOYFLIP")?0:1; return g_d3dgl_flipy_v; }
#define g_d3dgl_flipy (d3dgl_flipy())
static unsigned long g_bb_draws, g_bb_vp, g_bb_skip, g_bb_skip_reason; static unsigned g_bb_t0w,g_bb_t0h,g_bb_t0f;
static int d3dgl_fbo_y(uint32_t H,uint32_t top,uint32_t h){ return g_d3dgl_flipy ? (int)top : (int)H-(int)(top+h); }
static int d3dgl_rt_readfmt(uint32_t f,unsigned*fmt,unsigned*type,unsigned*bpp){
    switch(f){ case 27: case 28: case 29: *fmt=0x1908; *type=0x1401; *bpp=4; return 1;     // RGBA8 (+SRGB)
               case 87: case 90: case 91: *fmt=0x80E1; *type=0x1401; *bpp=4; return 1;     // BGRA8
               case 10: *fmt=0x1908; *type=0x140B; *bpp=8;  return 1;                      // RGBA16F
               case 2:  *fmt=0x1908; *type=0x1406; *bpp=16; return 1;                      // RGBA32F
               case 16: *fmt=0x8227; *type=0x1406; *bpp=8;  return 1;                      // RG32F
               case 34: *fmt=0x8227; *type=0x140B; *bpp=4;  return 1;                      // RG16F
               case 41: *fmt=0x1903; *type=0x1406; *bpp=4;  return 1;                      // R32F
               case 54: *fmt=0x1903; *type=0x140B; *bpp=2;  return 1;                      // R16F
               default: return 0; } }
static int d3dgl_gpu_copy_impl(D3DObj*dst,D3DObj*src,uint32_t sx,uint32_t sy,uint32_t sw,uint32_t sh,uint32_t dx,uint32_t dy);
static void d3dgl_reset_raster_ops(void); // fwd
static int d3dgl_gpu_copy(D3DObj*dst,D3DObj*src,uint32_t sx,uint32_t sy,uint32_t sw,uint32_t sh,uint32_t dx,uint32_t dy){
    if(!d3dgl_enabled()||!src||!src->glfbo) return 0; GLLOCK(); int r=d3dgl_gpu_copy_impl(dst,src,sx,sy,sw,sh,dx,dy); GLUNLOCK(); return r; }
static int d3dgl_gpu_copy_impl(D3DObj*dst,D3DObj*src,uint32_t sx,uint32_t sy,uint32_t sw,uint32_t sh,uint32_t dx,uint32_t dy){
    if(!d3dgl_enabled()||!src||!dst||src==dst||!src->glfbo||!src->gltex||src->kind==1||dst->kind==1) return 0;
    if(!G.BlitFramebuffer||!d3dgl_ensure()) return 0;
    uint32_t SH=src->h>4096?4096:src->h, DH=dst->h>4096?4096:dst->h;
    if(!sw||!sh||sx+sw>(src->w>4096?4096:src->w)||sy+sh>SH) return 0;
    d3dgl_ensure_rt(dst);
    int sy0=d3dgl_fbo_y(SH,sy,sh), dy0=d3dgl_fbo_y(DH,dy,sh);
    G.BindFramebuffer(0x8CA8/*READ*/,src->glfbo); G.BindFramebuffer(0x8CA9/*DRAW*/,dst->glfbo); d3dgl_reset_raster_ops();
    G.BlitFramebuffer((int)sx,sy0,(int)(sx+sw),sy0+(int)sh,(int)dx,dy0,(int)(dx+sw),dy0+(int)sh,0x4000/*COLOR*/,0x2600/*NEAREST*/);
    dst->gl_dirty=0; dst->gl_uploaded=1; dst->copydst=1;
    if(dst==g_d3d_backbuf && getenv("PELOAD_TRACE_VP")){ static int n; if((n++%300)==0){ char m[200]; int k=snprintf(m,sizeof m,"  [bbcopy] src=%p %ux%u f%u last_vp=%dx%d copy %ux%u at %u,%u\n",(void*)src,src->w,src->h,src->fmt,src->last_vpw,src->last_vph,sw,sh,sx,sy); (void)!write(2,m,k); } }
    if(dst==g_d3d_backbuf && src->last_vpw>0 && src->last_vph>0 && sw>=(uint32_t)src->last_vpw){
        // the engine composited its frame into (0,0,last_vp) of the source: present exactly that region
        g_d3dgl_present_tex=dst->gltex; g_d3dgl_present_tw=(int)(dst->w>4096?4096:dst->w); g_d3dgl_present_th=(int)(dst->h>4096?4096:dst->h);
        g_d3dgl_present_vw=src->last_vpw; g_d3dgl_present_vh=src->last_vph; }
    if(dst==g_d3d_backbuf && !getenv("PELOAD_D3DGL_NOFINISH") && getenv("PELOAD_D3DGL_NOPQ")){ G.BindFramebuffer(GLE_FBO,0); if(G.Finish) G.Finish(); else if(G.Flush) G.Flush(); }
    if(getenv("PELOAD_D3DGL_COPYDUMP") && dst==g_d3d_backbuf && G.ReadPixels){ static unsigned long cn; static int nd2; unsigned long skip=strtoul(getenv("PELOAD_D3DGL_COPYDUMP"),0,10); cn++; if(cn>skip && (cn%60)==0 && nd2<4){
        static uint8_t*cb3; if(!cb3) cb3=malloc(1920u*1080u*4u); G.BindFramebuffer(0x8CA8,dst->glfbo);
        int y0=d3dgl_fbo_y(DH,0,1080); if(cb3){ G.ReadPixels(0,y0,1920,1080,GLE_RGBA,GLE_UBYTE,cb3); const char*home=getenv("HOME"); char pth[256];
            snprintf(pth,sizeof pth,"%s/reclaimer_bbcopy_%d.bmp",home?home:"/tmp",nd2++); bmp_write_rgba(pth,1920,1080,cb3);
            char m[200]; int k=snprintf(m,sizeof m,"  [copydump] backbuffer after GPU copy -> %s\n",pth); (void)!write(2,m,k); } } }
    unsigned rf,rt,bpp;
    if((uint64_t)sw*sh<=65536 && dst->data && d3dgl_rt_readfmt(dst->fmt,&rf,&rt,&bpp)){
        static uint8_t tmp[65536*16]; G.ReadPixels((int)sx,sy0,(int)sw,(int)sh,rf,rt,tmp);
        uint64_t pitch=dst->rowpitch?dst->rowpitch:(uint64_t)dst->w*bpp;
        for(uint32_t r=0;r<sh;r++){ uint32_t srow=g_d3dgl_flipy?r:(sh-1-r);   // GL rows come bottom-up
            uint64_t o=(uint64_t)(dy+r)*pitch+(uint64_t)dx*bpp; if(o+(uint64_t)sw*bpp>dst->size) break;
            memcpy((uint8_t*)dst->data+o,tmp+(uint64_t)srow*sw*bpp,(size_t)sw*bpp); } }
    G.BindFramebuffer(GLE_FBO,0);
    if(getenv("PELOAD_TRACE_COPY")){ static unsigned long c; c++; if(c<12||(c%200)==0){ char m[240]; int k=snprintf(m,sizeof m,"  [gpucopy #%lu] dst=%p%s %ux%u f%u <- src=%p %ux%u f%u rect(%u,%u %ux%u)->(%u,%u)%s\n",c,(void*)dst,dst==g_d3d_backbuf?"(BACKBUFFER)":"",dst->w,dst->h,dst->fmt,(void*)src,src->w,src->h,src->fmt,sx,sy,sw,sh,dx,dy,((uint64_t)sw*sh<=65536)?" +readback":""); (void)!write(2,m,k);} }
    return 1; }
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
static D3DObj g_syn_cb1; static uint8_t g_syn_cb1_buf[256];   // synthesized vertex-decompression cb1
// PELOAD_D3DGL_CAPCB1: the REAL per-section decompression constants, captured by an entry detour on
// halo3+0x28e8e0 (sub_18028e8e0) which the engine calls right before each section's draws to build the
// 48-byte decompression buffer from the mesh's compression_info bounding box:
//   [0..2]=X/Y/Z extent (scale) [3]=1 ; [4..6]=X/Y/Z min (offset) [7]=0 ; [8..11]=u_ext,v_ext,u_min,v_min
// The engine then binds it via sub_1802af478 into material-param slot 0x22 -- a path dead under the
// loader -- so without this capture cb1 is never bound and each section's compressed verts collapse.
// Layout matches cb1 exactly (what the VS reads: r0 = v0*cb1[0]+cb1[1]), so we feed it straight through.
static float g_decomp_cap[12]; static volatile int g_decomp_cap_valid; static volatile unsigned long g_decomp_cap_seq;
static void d3dgl_mul44(const float*A,const float*B,float*O){ for(int r=0;r<4;r++)for(int c=0;c<4;c++){ float a=0; for(int k=0;k<4;k++)a+=A[r*4+k]*B[k*4+c]; O[r*4+c]=a; } }
static int d3dgl_synth_cb0(void){
    D3DObj*b=g_last_b12; if(!b||!b->data) return 0;
    const float*wf=(const float*)b->data; float wp[4]={wf[3],wf[7],wf[11],1.0f};   // object world origin
    float aspect=(g_d3dst.vp_h>0)?(float)g_d3dst.vp_w/(float)g_d3dst.vp_h:1.0f; if(aspect<0.2f||aspect>5.0f)aspect=1.0f;
    // PELOAD_D3DGL_FOV (vertical degrees, default 70 = the known-good view) / PELOAD_D3DGL_NEAR
    // (default 0.0625). A LARGER near plane clips geometry the uniform-scale decompression lands near/
    // behind the camera (w->0) -> cuts the radiating "spike" streaks.
    float fovy=(getenv("PELOAD_D3DGL_FOV")?atof(getenv("PELOAD_D3DGL_FOV")):70.0f)*3.14159265f/180.0f;
    float sy=1.0f/tanf(fovy*0.5f), sx=sy/aspect;
    float n=getenv("PELOAD_D3DGL_NEAR")?atof(getenv("PELOAD_D3DGL_NEAR")):0.0625f, fr=1000.0f;
    float P[16]={ sx,0,0,0, 0,sy,0,0, 0,0,fr/(fr-n),-n*fr/(fr-n), 0,0,1,0 };
    extern float g_b12_org[32][3]; extern int g_nb12_org;
    // PELOAD_D3DGL_CAM="ex,ey,ez[,tx,ty,tz[,ux,uy,uz]]": build a look-at VIEW matrix (column-vector
    // convention: V*[world,1]=view) and use P*V as cb0, bypassing the pool-CB search (the real
    // view-proj is in NO cbuffer -- cb0find only ever finds per-object matrices). Target defaults to
    // the scene center (avg of the known object origins); up defaults to +Z (Halo is Z-up).
    // PELOAD_D3DGL_AUTOFRAME: robust auto-camera -- frame the object-origin cloud (the live geometry) from
    // outside, looking at its centroid. The pool-CB auto-detect below keeps latching near-vertical "looking
    // up" shadow/env views (fwd=+Z) that throw the ground off the bottom of the screen; this instead always
    // aims AT the geometry so it lands on-screen. PELOAD_D3DGL_AF_DIST scales the pull-back distance,
    // PELOAD_D3DGL_AF_YAW/_PITCH (degrees) orbit the eye around the centroid (default behind -Y, slightly up).
    const char*camenv=getenv("PELOAD_D3DGL_CAM");
    int autoframe = !camenv && getenv("PELOAD_D3DGL_AUTOFRAME") && g_nb12_org>=2;
    if(camenv || autoframe){
        float e[3]={0,0,0},t[3]={0,0,0},up[3]={0,0,1}; int ne = camenv?sscanf(camenv,"%f,%f,%f,%f,%f,%f,%f,%f,%f",
            &e[0],&e[1],&e[2],&t[0],&t[1],&t[2],&up[0],&up[1],&up[2]):0;
        if(autoframe){
            float c[3]={0,0,0}; for(int o=0;o<g_nb12_org;o++){ c[0]+=g_b12_org[o][0]; c[1]+=g_b12_org[o][1]; c[2]+=g_b12_org[o][2]; }
            c[0]/=g_nb12_org; c[1]/=g_nb12_org; c[2]/=g_nb12_org;
            float rad=1.0f; for(int o=0;o<g_nb12_org;o++){ float dx=g_b12_org[o][0]-c[0],dy=g_b12_org[o][1]-c[1],dz=g_b12_org[o][2]-c[2]; float d=sqrtf(dx*dx+dy*dy+dz*dz); if(d>rad)rad=d; }
            float distk=getenv("PELOAD_D3DGL_AF_DIST")?atof(getenv("PELOAD_D3DGL_AF_DIST")):2.2f;
            float dist=rad*distk+8.0f;
            float yaw=(getenv("PELOAD_D3DGL_AF_YAW")?atof(getenv("PELOAD_D3DGL_AF_YAW")):0.0f)*3.14159265f/180.0f;
            float pitch=(getenv("PELOAD_D3DGL_AF_PITCH")?atof(getenv("PELOAD_D3DGL_AF_PITCH")):22.0f)*3.14159265f/180.0f;
            float ch=cosf(pitch); // horizontal component
            t[0]=c[0]; t[1]=c[1]; t[2]=c[2];
            e[0]=c[0]+dist*ch*sinf(yaw); e[1]=c[1]-dist*ch*cosf(yaw); e[2]=c[2]+dist*sinf(pitch);
            up[0]=0; up[1]=0; up[2]=1; ne=6;
        }
        if(ne<6){ float c[3]={0,0,0}; int nn=g_nb12_org>0?g_nb12_org:1; for(int o=0;o<g_nb12_org;o++){ c[0]+=g_b12_org[o][0]; c[1]+=g_b12_org[o][1]; c[2]+=g_b12_org[o][2]; } t[0]=c[0]/nn; t[1]=c[1]/nn; t[2]=c[2]/nn; }
        float z[3]={t[0]-e[0],t[1]-e[1],t[2]-e[2]}; float zl=sqrtf(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]); if(zl<1e-6f)zl=1; z[0]/=zl;z[1]/=zl;z[2]/=zl;
        float x[3]={up[1]*z[2]-up[2]*z[1], up[2]*z[0]-up[0]*z[2], up[0]*z[1]-up[1]*z[0]}; float xl=sqrtf(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); if(xl<1e-6f)xl=1; x[0]/=xl;x[1]/=xl;x[2]/=xl;
        float y[3]={z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0]};
        float Vm[16]={ x[0],x[1],x[2], -(x[0]*e[0]+x[1]*e[1]+x[2]*e[2]),
                       y[0],y[1],y[2], -(y[0]*e[0]+y[1]*e[1]+y[2]*e[2]),
                       z[0],z[1],z[2], -(z[0]*e[0]+z[1]*e[1]+z[2]*e[2]),
                       0,0,0,1 };
        float*out=(float*)g_syn_cb0_buf; memset(out,0,256); d3dgl_mul44(P,Vm,out);
        if(getenv("PELOAD_SYNVP_OBJ")||getenv("PELOAD_D3DGL_DECOMP")) out[48]=out[49]=out[50]=out[51]=1.0f;
        g_syn_cb0.data=g_syn_cb0_buf; g_syn_cb0.size=256; g_syn_cb0.is_cb=1; g_syn_cb0.ver++;
        if(getenv("PELOAD_TRACE_D3DGL")){ static int cc; if(cc++<2){ char m[200]; int k=snprintf(m,sizeof m,"  [cam] eye=[%.1f %.1f %.1f] tgt=[%.1f %.1f %.1f] cb0row0=[%.3f %.3f %.3f %.3f]\n",e[0],e[1],e[2],t[0],t[1],t[2],out[0],out[1],out[2],out[3]); (void)!write(2,m,k);} }
        return 1;
    }
    // camera VIEW = the CB holding the player camera's view matrix. The engine stores it as a STANDALONE
    // 4x4 (64B) ORTHONORMAL matrix (cb0find: [-1 0 0 8.3][0 -1 0 -10][0 0 1 17.1] projects 30/32 object
    // origins). Per-object world matrices live in LARGER buffers (256B+ bone/scale ARRAYS) whose first
    // entry can look view-like but score high only for their own object -> they warped the frame. So:
    // require an ORTHONORMAL rotation (unit, orthogonal rows = a true view) and PREFER the smallest such
    // buffer (the standalone camera), breaking ties by most origins on-screen.
    // PELOAD_SYNVP_CB=<i> forces g_cbreg[i] as V (debug).
    D3DObj* V=0; int bestN=0; uint64_t bestSz=~0ull; int g_cam_is4x3=0;
    const char*force=getenv("PELOAD_SYNVP_CB");
    if(force){ int fi=atoi(force); if(fi>=0&&fi<g_ncbreg&&g_cbreg[fi]&&g_cbreg[fi]->data) V=g_cbreg[fi]; }
    else
    for(int i=0;i<g_ncbreg;i++){ D3DObj*c=g_cbreg[i]; if(!c||!c->data||c->size<64||c->size>512) continue; if((g_cb_tick-c->cb_upd)>2) continue;
        const float*vm=(const float*)c->data; int bad=0; for(int j=0;j<16;j++){ float v=vm[j]; if(v!=v||v<-1e9f||v>1e9f){bad=1;break;} } if(bad)continue;
        // ORTHONORMALITY: the 3x3 upper-left must be unit-length rows that are mutually orthogonal (a pure
        // rotation = a real camera view). This rejects per-object matrices that carry scale/skew.
        float l0=vm[0]*vm[0]+vm[1]*vm[1]+vm[2]*vm[2], l1=vm[4]*vm[4]+vm[5]*vm[5]+vm[6]*vm[6], l2=vm[8]*vm[8]+vm[9]*vm[9]+vm[10]*vm[10];
        if(fabsf(l0-1)>0.06f||fabsf(l1-1)>0.06f||fabsf(l2-1)>0.06f) continue;
        float d01=vm[0]*vm[4]+vm[1]*vm[5]+vm[2]*vm[6], d02=vm[0]*vm[8]+vm[1]*vm[9]+vm[2]*vm[10], d12=vm[4]*vm[8]+vm[5]*vm[9]+vm[6]*vm[10];
        if(fabsf(d01)>0.06f||fabsf(d02)>0.06f||fabsf(d12)>0.06f) continue;
        // Require NON-TRIVIAL TRANSLATION (view translation = -R*eye, magnitude = |eye|). The camera sits
        // outside the scene (|eye|>~2); rotation-only orthonormal matrices (normal/tangent frames, camera
        // at origin) have ~0 translation and must be rejected (they project the scene from the origin -> black).
        float tr=vm[3]*vm[3]+vm[7]*vm[7]+vm[11]*vm[11]; if(tr<4.0f) continue;
        // Reject identity (a trivial "look down +z from origin" view that over-counts without being real).
        float idd=fabsf(vm[0]-1)+fabsf(vm[5]-1)+fabsf(vm[10]-1)+fabsf(vm[1])+fabsf(vm[2])+fabsf(vm[4])+fabsf(vm[6])+fabsf(vm[8])+fabsf(vm[9]);
        if(idd<0.25f) continue;
        float VP[16]; d3dgl_mul44(P,vm,VP); int on=0;
        for(int o=0;o<g_nb12_org;o++){ float cl[4]; for(int r=0;r<4;r++) cl[r]=VP[r*4]*g_b12_org[o][0]+VP[r*4+1]*g_b12_org[o][1]+VP[r*4+2]*g_b12_org[o][2]+VP[r*4+3];
            if(cl[3]>0.05f){ float nx=cl[0]/cl[3],ny=cl[1]/cl[3],nz=cl[2]/cl[3]; if(nx>-1.1f&&nx<1.1f&&ny>-1.1f&&ny<1.1f&&nz>-0.05f&&nz<1.05f) on++; } }
        // Pick purely by MOST origins on-screen (the orthonormal+translation+non-identity gates already
        // exclude per-object/rotation-only matrices; among true views the player camera frames the most).
        if(on < (g_nb12_org/4)) continue;
        if(getenv("PELOAD_D3DGL_CAMDBG")){ static int cd; if(cd++<40){ char mm[240]; int kk=snprintf(mm,sizeof mm,"  [camdbg] cb[%d] %lluB tr=%.1f on=%d/%d eye=[%.1f %.1f %.1f] fwd=[%.2f %.2f %.2f]\n",i,(unsigned long long)c->size,tr,on,g_nb12_org,vm[3],vm[7],vm[11],vm[8],vm[9],vm[10]); (void)!write(2,mm,kk);} }
        if(on>bestN || (on==bestN && c->size<bestSz)){ bestN=on; bestSz=c->size; V=c; } }
    (void)g_cam_is4x3;
    // LATCHING: early frames have few object origins so the score is unreliable and a ground-plane view can
    // win by default -> flip-flop. Once enough origins accumulate AND the winner frames a strong majority,
    // LATCH that CB (re-written each frame with the live view, so reusing it FOLLOWS the camera, stays stable).
    static D3DObj* g_cam_latch;
    if(V && V->data && g_nb12_org>=8 && bestN*3 >= g_nb12_org*2) g_cam_latch=V;   // >=66% on-screen -> latch
    if(!force && g_cam_latch && g_cam_latch->data && (g_cb_tick-g_cam_latch->cb_upd)<=4){
        const float*vm=(const float*)g_cam_latch->data; int ok=1;
        for(int j=0;j<16;j++){ float v=vm[j]; if(v!=v||v<-1e9f||v>1e9f){ok=0;break;} }
        if(ok){ float l0=vm[0]*vm[0]+vm[1]*vm[1]+vm[2]*vm[2],l1=vm[4]*vm[4]+vm[5]*vm[5]+vm[6]*vm[6],l2=vm[8]*vm[8]+vm[9]*vm[9]+vm[10]*vm[10];
            float tr=vm[3]*vm[3]+vm[7]*vm[7]+vm[11]*vm[11];
            if(fabsf(l0-1)>0.08f||fabsf(l1-1)>0.08f||fabsf(l2-1)>0.08f||tr<4.0f) ok=0; }
        if(ok) V=g_cam_latch; else g_cam_latch=0; }
    if(!V||!V->data) return 0;
    if(getenv("PELOAD_D3DGL_CAMDBG")){ static int cw; if(cw++<4){ const float*vm=(const float*)V->data; char mm[220]; int kk=snprintf(mm,sizeof mm,"  [camdbg] WINNER %lluB eye=[%.1f %.1f %.1f] fwd=[%.2f %.2f %.2f] rows0=[%.2f %.2f %.2f %.2f]\n",(unsigned long long)V->size,vm[3],vm[7],vm[11],vm[8],vm[9],vm[10],vm[0],vm[1],vm[2],vm[3]); (void)!write(2,mm,kk);} }
    float*out=(float*)g_syn_cb0_buf; memset(out,0,256);
    d3dgl_mul44(P,(const float*)V->data,out);           // cb0[0..3] = P*V (view-projection)
    // cb0[12] gates the vertex path: !=0 => decompress+world (needs cb3+cb12, both unbound here);
    // ==0 => r1=v0 directly (pre-transformed world/BSP geometry) -> only cb0 needed. Use the world path
    // unless PELOAD_SYNVP_OBJ asks for the object path. Default 0 renders the static map geometry.
    if(getenv("PELOAD_SYNVP_OBJ")||getenv("PELOAD_D3DGL_DECOMP")) out[48]=out[49]=out[50]=out[51]=1.0f;
    g_syn_cb0.data=g_syn_cb0_buf; g_syn_cb0.size=256; g_syn_cb0.is_cb=1; g_syn_cb0.ver++;
    if(getenv("PELOAD_TRACE_D3DGL")){ static int c; if(c++<3){ char m[200]; int k=snprintf(m,sizeof m,"  [synvp] cb0 from V=%p: row0=[%.3f %.3f %.3f %.3f] row3=[%.3f %.3f %.3f %.3f]\n",(void*)V,out[0],out[1],out[2],out[3],out[12],out[13],out[14],out[15]); (void)!write(2,m,k);} }
    return 1; }
// ---- D3D11 pipeline state -> GL ------------------------------------------------------------------------
static unsigned d3d_blend_gl(uint32_t b){ switch(b){ case 1:return 0; case 2:return 1; case 3:return 0x0300; case 4:return 0x0301; case 5:return 0x0302;
    case 6:return 0x0303; case 7:return 0x0304; case 8:return 0x0305; case 9:return 0x0306; case 10:return 0x0307; case 11:return 0x0308;
    case 14:return 0x8001; case 15:return 0x8002; case 16:return 0x88F9; case 17:return 0x88FA; case 18:return 0x8589; case 19:return 0x88FB; default:return 1; } }
static unsigned d3d_blendop_gl(uint32_t o){ switch(o){ case 2:return 0x800A; case 3:return 0x800B; case 4:return 0x8007; case 5:return 0x8008; default:return 0x8006; } }
static unsigned d3d_addr_gl(uint32_t a){ switch(a){ case 1:return 0x2901; case 2:return 0x8370; case 3:return 0x812F; case 4:return 0x812D; case 5:return 0x8743; default:return 0x2901; } }
// D3D11_SAMPLER_DESC {Filter, AddressU,V,W, MipLODBias, MaxAnisotropy, ComparisonFunc, BorderColor[4], MinLOD, MaxLOD}
static unsigned d3dgl_glsampler_x(D3DObj*s,int hasmips,int skip);
static unsigned d3dgl_glsampler(D3DObj*s,int hasmips){ return d3dgl_glsampler_x(s,hasmips,0); }
static unsigned d3d_cmp_gl(uint32_t f); // fwd
static unsigned d3dgl_glsampler_x(D3DObj*s,int hasmips,int skip){
    if(!s||s->stkind!=4||!G.GenSamplers||!G.SamplerParameteri) return 0;
    unsigned*slot=skip?(hasmips?&s->glsamp_skip:&s->glsamp_nomip_skip):(hasmips?&s->glsamp:&s->glsamp_nomip); if(*slot) return *slot;
    G.GenSamplers(1,slot); unsigned g=*slot; const uint32_t*d=s->sd; uint32_t f=d[0]&0x7F;
    int aniso=(f&0x40)!=0, minl=(f&0x10)!=0||aniso, magl=(f&0x04)!=0||aniso, mipl=(f&0x01)!=0||aniso;
    unsigned minf = hasmips ? (minl?(mipl?0x2703:0x2701):(mipl?0x2702:0x2700)) : (minl?0x2601:0x2600);
    G.SamplerParameteri(g,0x2801,(int)minf); G.SamplerParameteri(g,0x2800,magl?0x2601:0x2600);
    G.SamplerParameteri(g,0x2802,(int)d3d_addr_gl(d[1])); G.SamplerParameteri(g,0x2803,(int)d3d_addr_gl(d[2])); G.SamplerParameteri(g,0x8072,(int)d3d_addr_gl(d[3]));
    float bias,mnl,mxl; memcpy(&bias,&d[4],4); memcpy(&mnl,&d[11],4); memcpy(&mxl,&d[12],4);
    if(G.SamplerParameterf){ G.SamplerParameterf(g,0x8501,bias); G.SamplerParameterf(g,0x813A,mnl); G.SamplerParameterf(g,0x813B,mxl>1000.0f?1000.0f:mxl);
        if(aniso) G.SamplerParameterf(g,0x84FE,(float)(d[5]<1?1:(d[5]>16?16:d[5]))); }
    if(G.SamplerParameterfv) G.SamplerParameterfv(g,0x1004,(const float*)&d[7]);
    G.SamplerParameteri(g,0x8A48/*TEXTURE_SRGB_DECODE_EXT*/,skip?0x8A4A/*SKIP_DECODE*/:0x8A49/*DECODE*/);
    // COMPARISON_* filters (0x80): hardware depth compare with the D3D ComparisonFunc (shadow samplers)
    if((d[0]&0x80) && !getenv("PELOAD_D3DGL_NOCMP")){ G.SamplerParameteri(g,0x884C/*TEXTURE_COMPARE_MODE*/,0x884E/*COMPARE_REF_TO_TEXTURE*/); G.SamplerParameteri(g,0x884D/*COMPARE_FUNC*/,(int)d3d_cmp_gl(d[6])); }
    return g; }
// ---- depth-stencil textures + per-(RTVs,DSV) framebuffers ---------------------------------------
// A DSV resource becomes a real GL depth(-stencil) texture (also sampleable through its SRV: GL returns
// (d,0,0,1) like D3D's R24_UNORM_X8 / R32_FLOAT views). Draws render into a cached FBO holding exactly the
// bound RTVs (MRT via glDrawBuffers: the opaque pass writes the normal buffer through o1) + the DSV;
// a DSV-only binding is a depth-only pass (shadow maps / occlusion).
static void d3dgl_ensure_depth(D3DObj*o){
    if(!o) return;
    if(o->gltex && o->gldsatt) return;
    unsigned ifmt,fmt,type,att; uint32_t f=o->fmt;
    if(f>=19&&f<=22){ ifmt=0x8CAD; fmt=0x84F9; type=0x8DAD; att=0x821A; }          // D32_FLOAT_S8X24 -> DEPTH32F_STENCIL8
    else if(f>=39&&f<=41){ ifmt=0x8CAC; fmt=0x1902; type=0x1406; att=0x8D00; }     // D32_FLOAT -> DEPTH_COMPONENT32F
    else if(f>=53&&f<=56){ ifmt=0x81A5; fmt=0x1902; type=0x1403; att=0x8D00; }     // D16 -> DEPTH_COMPONENT16
    else { ifmt=0x88F0; fmt=0x84F9; type=0x84FA; att=0x821A; }                     // D24_UNORM_S8 (44..46) -> DEPTH24_STENCIL8
    int w=o->w?(int)o->w:g_d3dgl_W, h=o->h?(int)o->h:g_d3dgl_H; if(w>4096)w=4096; if(h>4096)h=4096;
    TEXCACHE_INVALIDATE(); G.GenTextures(1,&o->gltex); G.BindTexture(GLE_TEX2D,o->gltex);
    G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2800,GLE_LINEAR);
    G.TexParameteri(GLE_TEX2D,0x2802,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x2803,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x813D,0);
    G.TexImage2D(GLE_TEX2D,0,(int)ifmt,w,h,0,fmt,type,0);
    o->gltarget=GLE_TEX2D; o->gldsatt=att; o->gl_uploaded=1; o->gl_dirty=0; }
typedef struct { D3DObj*c[8]; D3DObj*ds; int n; unsigned fbo; } D3DGLFbo;
static D3DGLFbo g_fbos[512]; static int g_nfbo;
static unsigned d3dgl_fbo_for(D3DObj*const*rts,int n,D3DObj*ds){
    if(n>8) n=8;
    if(!ds && n==1 && rts[0]){ d3dgl_ensure_rt(rts[0]); return rts[0]->glfbo; }      // common case: the RT's own FBO
    for(int i=0;i<g_nfbo;i++){ D3DGLFbo*e=&g_fbos[i]; if(e->ds!=ds||e->n!=n) continue; int same=1;
        for(int k=0;k<n;k++) if(e->c[k]!=rts[k]){ same=0; break; } if(same) return e->fbo; }
    D3DGLFbo*e=&g_fbos[g_nfbo<512?g_nfbo++:(int)(rand()%512)];
    memset(e,0,sizeof *e); e->ds=ds; e->n=n; for(int k=0;k<n;k++) e->c[k]=rts[k];
    unsigned bufs[8]; int nb=0;
    for(int k=0;k<n;k++){ if(rts[k]) d3dgl_ensure_rt(rts[k]); }
    if(ds) d3dgl_ensure_depth(ds);
    G.GenFramebuffers(1,&e->fbo); G.BindFramebuffer(GLE_FBO,e->fbo);
    for(int k=0;k<n;k++){ if(rts[k]&&rts[k]->gltex){ G.FramebufferTexture2D(GLE_FBO,GLE_CA0+k,GLE_TEX2D,rts[k]->gltex,0); bufs[k]=GLE_CA0+k; nb=k+1; } else bufs[k]=0; }
    if(ds&&ds->gltex) G.FramebufferTexture2D(GLE_FBO,ds->gldsatt,GLE_TEX2D,ds->gltex,0);
    if(nb && G.DrawBuffers) G.DrawBuffers(nb,bufs); else if(!nb){ if(G.DrawBuffer) G.DrawBuffer(0); if(G.ReadBuffer) G.ReadBuffer(0); }
    if(G.CheckFramebufferStatus){ unsigned st=G.CheckFramebufferStatus(GLE_FBO); if(st!=0x8CD5){ static int nbad; if(nbad++<20){ char m[200];
        int k=snprintf(m,sizeof m,"  [d3dgl] MRT/DS FBO INCOMPLETE status=0x%x n=%d rt0=%ux%u f%u ds=%ux%u f%u\n",st,n,rts[0]?rts[0]->w:0,rts[0]?rts[0]->h:0,rts[0]?rts[0]->fmt:0,ds?ds->w:0,ds?ds->h:0,ds?ds->fmt:0); (void)!write(2,m,k);} } }
    return e->fbo; }
static unsigned d3d_cmp_gl(uint32_t f){ return (f>=1&&f<=8)?0x0200+(f-1):0x0207; }          // NEVER..ALWAYS
static unsigned d3d_stencilop_gl(uint32_t o){ switch(o){ case 2: return 0; case 3: return 0x1E01; case 4: return 0x1E02; case 5: return 0x1E03;
    case 6: return 0x150A; case 7: return 0x8507; case 8: return 0x8508; default: return 0x1E00; } }
// D3D11 depth-stencil state (only meaningful with a DSV bound; no DSV = depth/stencil tests pass, no writes)
// ---- GL state shadow for the per-draw state (render context) ------------------------------------------
// apply_state used to re-issue every blend/raster/depth/viewport call on every draw; on GL-on-Metal redundant
// state changes cost CPU and pipeline re-validation. Only changes are sent now. Paths that touch this state
// directly (window blit) call gs_invalidate(). PELOAD_D3DGL_NOSTATECACHE disables the cache.
static struct { int valid; uint8_t cap[10]; unsigned bf[4], be[2]; float bc[4]; uint8_t cm[4]; unsigned cull, ff; int sc[4], vp[4]; double dr[2]; float po[2]; unsigned dfunc; int dmask; } g_gs;
static int g_gs_off=-1;
static void gs_invalidate(void){ g_gs.valid=0; }
static void gs_ok(void){ if(g_gs_off<0) g_gs_off=getenv("PELOAD_D3DGL_NOSTATECACHE")?1:0; if(!g_gs.valid||g_gs_off){ memset(&g_gs,0xff,sizeof g_gs); g_gs.valid=1; } }
static int gs_capi(unsigned c){ switch(c){ case 0x0BE2: return 0; case 0x0B44: return 1; case 0x0C11: return 2; case 0x0B71: return 3; case 0x0B90: return 4;
    case 0x8DB9: return 5; case 0x8037: return 6; case 0x864F: return 7; case 0x8F9D: return 8; default: return -1; } }
static void gs_cap(unsigned c,int on){ gs_ok(); int i=gs_capi(c); if(i>=0){ uint8_t v=on?1:0; if(g_gs.cap[i]==v) return; g_gs.cap[i]=v; } if(on) G.Enable(c); else G.Disable(c); }
static void gs_blendfunc(unsigned a,unsigned b,unsigned c,unsigned d){ gs_ok(); if(g_gs.bf[0]==a&&g_gs.bf[1]==b&&g_gs.bf[2]==c&&g_gs.bf[3]==d) return; g_gs.bf[0]=a;g_gs.bf[1]=b;g_gs.bf[2]=c;g_gs.bf[3]=d; G.BlendFuncSeparate(a,b,c,d); }
static void gs_blendeq(unsigned a,unsigned b){ gs_ok(); if(g_gs.be[0]==a&&g_gs.be[1]==b) return; g_gs.be[0]=a; g_gs.be[1]=b; G.BlendEquationSeparate(a,b); }
static void gs_blendcolor(float r,float g,float b,float a){ gs_ok(); if(g_gs.bc[0]==r&&g_gs.bc[1]==g&&g_gs.bc[2]==b&&g_gs.bc[3]==a) return; g_gs.bc[0]=r;g_gs.bc[1]=g;g_gs.bc[2]=b;g_gs.bc[3]=a; G.BlendColor(r,g,b,a); }
static void gs_colormask(unsigned char r,unsigned char g,unsigned char b,unsigned char a){ gs_ok(); if(g_gs.cm[0]==r&&g_gs.cm[1]==g&&g_gs.cm[2]==b&&g_gs.cm[3]==a) return; g_gs.cm[0]=r;g_gs.cm[1]=g;g_gs.cm[2]=b;g_gs.cm[3]=a; G.ColorMask(r,g,b,a); }
static void gs_cullface(unsigned m){ gs_ok(); if(g_gs.cull==m) return; g_gs.cull=m; G.CullFace(m); }
static void gs_frontface(unsigned m){ gs_ok(); if(g_gs.ff==m) return; g_gs.ff=m; G.FrontFace(m); }
static void gs_scissor(int x,int y,int w,int h){ gs_ok(); if(g_gs.sc[0]==x&&g_gs.sc[1]==y&&g_gs.sc[2]==w&&g_gs.sc[3]==h) return; g_gs.sc[0]=x;g_gs.sc[1]=y;g_gs.sc[2]=w;g_gs.sc[3]=h; G.Scissor(x,y,w,h); }
static void gs_viewport(int x,int y,int w,int h){ gs_ok(); if(g_gs.vp[0]==x&&g_gs.vp[1]==y&&g_gs.vp[2]==w&&g_gs.vp[3]==h) return; g_gs.vp[0]=x;g_gs.vp[1]=y;g_gs.vp[2]=w;g_gs.vp[3]=h; G.Viewport(x,y,w,h); }
static void gs_depthrange(void(*dr)(double,double),double n,double f){ gs_ok(); if(g_gs.dr[0]==n&&g_gs.dr[1]==f) return; g_gs.dr[0]=n; g_gs.dr[1]=f; dr(n,f); }
static void gs_polyofs(void(*po)(float,float),float s,float b){ gs_ok(); if(g_gs.po[0]==s&&g_gs.po[1]==b) return; g_gs.po[0]=s; g_gs.po[1]=b; po(s,b); }
static void gs_depthfunc(unsigned f){ gs_ok(); if(g_gs.dfunc==f) return; g_gs.dfunc=f; G.DepthFunc(f); }
static void gs_depthmask(unsigned char m){ gs_ok(); if(g_gs.dmask==(int)m) return; g_gs.dmask=m; G.DepthMask(m); }
static void d3dgl_apply_depth(void){
    D3DObj*d=g_d3dst.depth; int hasds=g_d3dst.dsv!=0 && !getenv("PELOAD_D3DGL_NODEPTH");
    uint32_t de=1,dw=1,df=2,se=0,rm=0xff,wm=0xff; const uint32_t*fr=0,*bk=0;
    if(d && d->stkind==2){ de=d->sd[0]; dw=d->sd[1]; df=d->sd[2]; se=d->sd[3]; rm=d->sd[4]&0xff; wm=(d->sd[4]>>8)&0xff; fr=&d->sd[5]; bk=&d->sd[9]; }
    if(hasds && de){ gs_cap(0x0B71,1); if(G.DepthFunc) gs_depthfunc(d3d_cmp_gl(df)); if(G.DepthMask) gs_depthmask(dw?1:0); }
    else { gs_cap(0x0B71,0); if(G.DepthMask) gs_depthmask(0); }
    if(hasds && se && fr && G.StencilFuncSeparate && G.StencilOpSeparate){ gs_cap(0x0B90,1);
        G.StencilFuncSeparate(0x0404,d3d_cmp_gl(fr[3]),(int)g_d3dst.sref,rm); G.StencilOpSeparate(0x0404,d3d_stencilop_gl(fr[0]),d3d_stencilop_gl(fr[1]),d3d_stencilop_gl(fr[2]));
        G.StencilFuncSeparate(0x0405,d3d_cmp_gl(bk[3]),(int)g_d3dst.sref,rm); G.StencilOpSeparate(0x0405,d3d_stencilop_gl(bk[0]),d3d_stencilop_gl(bk[1]),d3d_stencilop_gl(bk[2]));
        if(G.StencilMask) G.StencilMask(wm); }
    else gs_cap(0x0B90,0); }
// Blend (RT0), rasterizer (cull/front face/scissor), viewport origin. Depth stays off until depth
// buffers exist. Viewport/scissor y: GL row y == D3D row y (see d3dgl_flipy); under NOYFLIP a D3D rect
// starting at y spans GL rows [H-(y+h), H-y).
static void d3dgl_apply_state(D3DObj*rt,int vw,int vh){
    int H=(int)(rt&&rt->h?(rt->h>4096?4096:rt->h):(unsigned)g_d3dgl_H);
    d3dgl_apply_depth();
    D3DObj*b=g_d3dst.blend; int be=0; uint32_t wm=0xF;
    // IndependentBlendEnable with MRT: each RT has its own blend + write mask (indexed GL state)
    if(b && b->stkind==1 && b->sd[1] && g_d3dst.num_rt>1 && G.Enablei && G.BlendFuncSeparatei && G.ColorMaski && !getenv("PELOAD_D3DGL_NOBLEND")){
        for(int i=0;i<g_d3dst.num_rt && i<8;i++){ const uint32_t*ri=b->sd+2+8*i; uint32_t m=ri[7]&0xF;
            if(ri[0]){ G.Enablei(0x0BE2,i); G.BlendFuncSeparatei(i,d3d_blend_gl(ri[1]),d3d_blend_gl(ri[2]),d3d_blend_gl(ri[4]),d3d_blend_gl(ri[5]));
                if(G.BlendEquationSeparatei) G.BlendEquationSeparatei(i,d3d_blendop_gl(ri[3]),d3d_blendop_gl(ri[6])); }
            else G.Disablei(0x0BE2,i);
            G.ColorMaski(i,m&1,(m>>1)&1,(m>>2)&1,(m>>3)&1); }
        if(G.BlendColor) gs_blendcolor(g_d3dst.blendf[0],g_d3dst.blendf[1],g_d3dst.blendf[2],g_d3dst.blendf[3]);
        g_gs.cap[0]=0xff; memset(g_gs.cm,0xff,4); memset(g_gs.bf,0xff,sizeof g_gs.bf); memset(g_gs.be,0xff,sizeof g_gs.be);   // indexed blend touched global state
        b=0; be=-1; }
    if(b && b->stkind==1 && !getenv("PELOAD_D3DGL_NOBLEND")){ const uint32_t*r0=b->sd+2; be=(int)r0[0]; wm=r0[7]&0xF;
        if(be){ gs_cap(0x0BE2,1);
            if(G.BlendFuncSeparate) gs_blendfunc(d3d_blend_gl(r0[1]),d3d_blend_gl(r0[2]),d3d_blend_gl(r0[4]),d3d_blend_gl(r0[5]));
            if(G.BlendEquationSeparate) gs_blendeq(d3d_blendop_gl(r0[3]),d3d_blendop_gl(r0[6]));
            if(G.BlendColor) gs_blendcolor(g_d3dst.blendf[0],g_d3dst.blendf[1],g_d3dst.blendf[2],g_d3dst.blendf[3]); } }
    if(be>=0){ if(!be) gs_cap(0x0BE2,0);
        if(G.ColorMask) gs_colormask(wm&1,(wm>>1)&1,(wm>>2)&1,(wm>>3)&1); }
    D3DObj*r=g_d3dst.raster; uint32_t cull=3, fccw=0, scis=0;   // D3D default: cull back, front = CW, no scissor
    if(r && r->stkind==3){ cull=r->sd[1]; fccw=r->sd[2]; scis=r->sd[7]; }
    if(cull==1 || getenv("PELOAD_D3DGL_NOCULL") || !G.CullFace) gs_cap(0x0B44,0);
    else { gs_cap(0x0B44,1); gs_cullface(cull==2?0x0404:0x0405);
        // D3D judges winding in y-DOWN pixel space, GL in y-UP window space: the same triangle flips, so
        // D3D front=CW maps to GL front=CCW (and vice versa).
        // (Exact with the y-flipped VS: GL's image is D3D's mirrored vertically. Under PELOAD_D3DGL_NOYFLIP
        //  the image is NOT mirrored and this mapping is inverted -- kept only to reproduce the old layout.)
        if(G.FrontFace) gs_frontface(fccw?0x0900/*CW*/:0x0901/*CCW*/); }
    if(scis && G.Scissor && !getenv("PELOAD_D3DGL_NOSCISSOR")){ int sw=g_d3dst.sc_r-g_d3dst.sc_l, sh=g_d3dst.sc_b-g_d3dst.sc_t; if(sw<0)sw=0; if(sh<0)sh=0;
        gs_cap(0x0C11,1); gs_scissor(g_d3dst.sc_l, d3dgl_flipy()?g_d3dst.sc_t:H-g_d3dst.sc_b, sw, sh); }
    else gs_cap(0x0C11,0);
    gs_viewport(g_d3dst.vp_x, d3dgl_flipy()?g_d3dst.vp_y:H-(g_d3dst.vp_y+vh), vw, vh);
    // viewport depth range (D3D MinDepth/MaxDepth; the VS z remap makes GL's [-1,1] land on [n,f] like D3D's
    // [0,1]), depth clip, depth bias. Without the range the first-person weapon (own depth slice) is buried.
    { static void(*dr)(double,double); static void(*po)(float,float); static int ini; if(!ini){ ini=1; dr=dlsym(RTLD_DEFAULT,"glDepthRange"); po=dlsym(RTLD_DEFAULT,"glPolygonOffset"); }
      float zn=g_d3dst.vp_set?g_d3dst.vp_zn:0.0f, zf=g_d3dst.vp_set?g_d3dst.vp_zf:1.0f;
      if(dr && !getenv("PELOAD_D3DGL_NODEPTHRANGE")) gs_depthrange(dr,zn,zf);
      D3DObj*rs=g_d3dst.raster; int dclip=1; float bias=0,slope=0;
      if(rs && rs->stkind==3){ int32_t db=(int32_t)rs->sd[3]; memcpy(&slope,&rs->sd[5],4); bias=(float)db; dclip=rs->sd[6]?1:0; }
      int alton = !getenv("PELOAD_D3DGL_ALT5") || (((long)(mono_s()/5.0))&1);
      // D3D DepthClipEnable=FALSE -> GL_DEPTH_CLAMP made primitives crossing behind the eye rasterize as huge
      // screen-covering polygons on GL-on-Metal (A/B: 4/86 frames vs 0/86). Keep clipping unless opted in.
      if(!getenv("PELOAD_D3DGL_DEPTHCLAMP")) dclip=1;
      if(getenv("PELOAD_D3DGL_NODEPTHCLAMP") && alton) dclip=1;
      if(getenv("PELOAD_D3DGL_NOPOLYOFS") && alton){ bias=0; slope=0; }
      if(dclip) gs_cap(0x864F,0); else gs_cap(0x864F,1);
      if((bias!=0||slope!=0) && po){ gs_cap(0x8037,1); gs_polyofs(po,slope,bias); } else gs_cap(0x8037,0); } }
// state that must NOT leak into clears / copies (D3D clears & copies ignore blend, write mask and scissor)
static void d3dgl_reset_raster_ops(void){ gs_cap(0x0BE2,0); gs_cap(0x0C11,0); if(G.ColorMask) gs_colormask(1,1,1,1); gs_cap(0x0B71,0); gs_cap(0x0B90,0); gs_cap(0x8DB9,0); }
// sampler-type code (dxbc2glsl: 0 sampler2D, 1 sampler3D, 2 samplerCube, 3 sampler2DArray, 4 samplerCubeArray) -> GL target
static const unsigned g_tt_target[6]={GLE_TEX2D,0x806F,0x8513,0x8C1A,0x9009,0x8C2A/*TEXTURE_BUFFER*/};
static void d3dgl_scan_tt_p(const char*src,signed char*tt,const char*pfx){
    char pat[16]; snprintf(pat,sizeof pat," %s%%d;",pfx);
    for(const char*p=src; p && (p=strstr(p,"uniform ")); ){ p+=8; int k=-1;
        if(!strncmp(p,"usamplerBuffer ",15)) k=5;
        else if(!strncmp(p,"sampler",7)){ const char*t=p+7;
            if(!strncmp(t,"2DArrayShadow ",14)) k=3; else if(!strncmp(t,"2DShadow ",9)) k=0; else if(!strncmp(t,"CubeShadow ",11)) k=2; else
            if(!strncmp(t,"2DArray ",8)) k=3; else if(!strncmp(t,"2D ",3)) k=0; else if(!strncmp(t,"3D ",3)) k=1;
            else if(!strncmp(t,"CubeArray ",10)) k=4; else if(!strncmp(t,"Cube ",5)) k=2; }
        const char*q=strchr(p,' '); int r; if(k>=0 && q && sscanf(q,pat,&r)==1 && r>=0 && r<32) tt[r]=(signed char)k; } }
static void d3dgl_scan_tt(const char*src,signed char*tt){ d3dgl_scan_tt_p(src,tt,"t"); }
// buffer SRV (StructuredBuffer / ByteAddressBuffer) -> GL texture buffer over an R32UI copy of its bytes,
// re-uploaded whenever the D3D buffer's version changed (CPU compute / Map / Update)
static void d3dgl_ensure_tbo(D3DObj*o){
    if(!G.TexBuffer||!o->data||!o->size) return;
    cs_wait_res(o);
    if(!o->gltbobuf) G.GenBuffers(1,&o->gltbobuf);
    if(!o->tbook || o->tbover!=o->ver){ G.BindBuffer(0x8C2A,o->gltbobuf); G.BufferData(0x8C2A,(long)((o->size+15)&~15ull),0,0x88E8); G.BufferSubData(0x8C2A,0,(long)o->size,o->data); o->tbover=o->ver; o->tbook=1; }
    if(!o->gltbo){ TEXCACHE_INVALIDATE(); G.GenTextures(1,&o->gltbo); G.BindTexture(0x8C2A,o->gltbo); G.TexBuffer(0x8C2A,0x8236/*R32UI*/,o->gltbobuf); } }
static void d3dgl_bind_res(unsigned prog,const int*tloc,const signed char*tt,int*units_set,D3DObj**srvs,D3DObj**views,D3DObj**samps,int ubase,int umax);
static void d3dgl_bind_texs_p(unsigned prog,const int*tloc,const signed char*tt,int*units_set){
    d3dgl_bind_res(prog,tloc,tt,units_set,g_d3dst.psrv,g_d3dst.psrvv,g_d3dst.pssamp,0,16); }
static void d3dgl_bind_res(unsigned prog,const int*tloc,const signed char*tt,int*units_set,D3DObj**srvs,D3DObj**views,D3DObj**samps,int ubase,int umax){
    if(!prog) return; int unit=ubase;
    if(!g_unitcache_ok){ memset(g_unit_tex,0xff,sizeof g_unit_tex); memset(g_unit_tgt,0,sizeof g_unit_tgt); memset(g_unit_samp,0xff,sizeof g_unit_samp); g_unitcache_ok=1; }
    int nocache=getenv("PELOAD_D3DGL_NOBINDCACHE")!=0;
    // halo3 binds the map/material textures to high, SPARSE SRV slots (seen: t13,t14,t16,t17), not just
    // t0-t7. Cover slots 0..31, and PACK each declared sampler into the next contiguous GL texture unit
    // (Apple GL-on-Metal has ~16 fragment units, so binding to unit 17 would fail) -- the sampler uniform
    // is set to the packed unit, so the shader samples the right texture.
    for(int r=0;r<32 && unit<umax;r++){ int loc=tloc[r]; if(loc<0) continue; D3DObj*tx=srvs[r];
        // PELOAD_TEXCENSUS: log each distinct (format, dimension, RT-ness) texture class the frame SAMPLES,
        // with whether the uploader can represent it (anything else is a 1x1 magenta placeholder).
        if(tx && getenv("PELOAD_TEXCENSUS")){ static uint64_t seen[512]; static int ns;
            uint64_t key=((uint64_t)tx->fmt)|((uint64_t)(tx->arr>1)<<8)|((uint64_t)((tx->misc&4)!=0)<<9)|((uint64_t)(tx->kind&7)<<10)|((uint64_t)(tx->glfbo!=0)<<13)|((uint64_t)(tx->depth>1)<<14);
            int f=0; for(int i=0;i<ns;i++) if(seen[i]==key){ f=1; break; }
            if(!f && ns<512){ seen[ns++]=key; unsigned gf,gt,bi; int bb; int up=d3dgl_tex_upload_fmt(tx->fmt,&gf,&gt)||d3dgl_bc_glfmt(tx->fmt,&bi,&bb);
                char m[240]; int k=snprintf(m,sizeof m,"  [texcensus] t%d f%u kind%u %ux%u arr=%u depth=%u mips=%u misc=0x%x bind=0x%x %s uploadable=%d\n",r,tx->fmt,tx->kind,tx->w,tx->h,tx->arr,tx->depth,tx->mips,tx->misc,tx->bindf,tx->glfbo?"RT":"asset",up||tx->glfbo); (void)!write(2,m,k); } }
        unsigned want = tt ? g_tt_target[(tt[r]>=0&&tt[r]<6)?tt[r]:0] : GLE_TEX2D;
        int isbuf = tx && tx->kind==1;
        if(isbuf){ if(want==0x8C2A){ G.ActiveTexture(GLE_TEX0+unit); d3dgl_ensure_tbo(tx); } }
        else if(tx && (!tx->gltex || tx->gl_dirty || !tx->gl_uploaded)){ G.ActiveTexture(GLE_TEX0+unit); d3dgl_ensure_tex(tx); }   // upload (binds on this unit) first
        unsigned wtex = 0; if(isbuf){ if(want==0x8C2A) wtex=tx->gltbo; }
        else if(tx){ unsigned have=tx->gltarget?tx->gltarget:GLE_TEX2D; if(!tt) want=have; if(have==want) wtex=tx->gltex; }
        D3DObj*sv=views?views[r]:0; int skip = !isbuf && tx && tx->glsrgb && !dxgi_is_srgb(sv?sv->vfmt:tx->fmt);   // UNORM view of sRGB storage: raw read
        unsigned wsamp = (!isbuf && G.BindSampler && r<16 && samps && !getenv("PELOAD_D3DGL_NOSAMPLER")) ? d3dgl_glsampler_x(samps[r],tx?tx->glhasmips:0,skip) : 0;
        if(!wsamp && tx && tx->glsrgb && wtex){ G.ActiveTexture(GLE_TEX0+unit); G.BindTexture(want,wtex); G.TexParameteri(want,0x8A48,skip?0x8A4A:0x8A49); g_unit_tex[unit]=~0u; }   // no sampler object: texture's own decode state
        if(!nocache && g_unitcache_ok && g_unit_tex[unit]==wtex && g_unit_tgt[unit]==want && g_unit_samp[unit]==wsamp){
            if(!(units_set&&*units_set)) G.ProgramUniform1i(prog,loc,unit);
            unit++; continue; }
        G.ActiveTexture(GLE_TEX0+unit);
        g_unit_tex[unit]=wtex; g_unit_tgt[unit]=want; g_unit_samp[unit]=wsamp;
        if(isbuf) G.BindTexture(want,wtex);
        else if(tx){ unsigned have=tx->gltarget?tx->gltarget:GLE_TEX2D;
            if(have==want) G.BindTexture(want,tx->gltex);
            else { G.BindTexture(want,0);   // shader's resource dimension != the resource's (view mismatch): sample black
                if(getenv("PELOAD_TEXCENSUS")){ static uint64_t ms[64]; static int nms; uint64_t key=((uint64_t)have<<32)|((uint64_t)want<<8)|tx->fmt; int f=0;
                    for(int i=0;i<nms;i++) if(ms[i]==key){ f=1; break; }
                    if(!f && nms<64){ ms[nms++]=key; char m[200]; int k=snprintf(m,sizeof m,"  [texcensus] MISMATCH t%d shader-wants=0x%x resource=0x%x %ux%u arr=%u f%u\n",r,want,have,tx->w,tx->h,tx->arr,tx->fmt); (void)!write(2,m,k); } } } }
        else G.BindTexture(want,0);
        if(G.BindSampler) G.BindSampler((unsigned)unit, wsamp);
        if(!(units_set&&*units_set)) G.ProgramUniform1i(prog,loc,unit);
        unit++; }
    if(units_set) *units_set=1;   // the unit assignment is fixed per program: sampler uniforms set once
}
static void d3dgl_bind_texs(D3DObj*sh){
    if(!sh) return; d3dgl_bind_texs_p(sh->glprog,sh->gl_tloc,0,0);
}
// ---- constant buffers as uniform buffer objects ----------------------------------------------
// Each D3D constant buffer keeps a GL buffer copy, re-specified only when the game wrote it (ver), and is
// bound by range to the program's block binding (VS slot b -> b, PS slot b -> 16+b). Replaces a full
// glProgramUniform4fv copy of every bound buffer on every draw.
static unsigned g_zero_ubo; static struct { unsigned buf; long off, sz; } g_ubo_bound[32];
static void d3dgl_ubo_upload(D3DObj*cb,uint32_t extent){
    uint32_t want=(uint32_t)cb->size; if(want<extent) want=extent; want=(want+255)&~255u;
    if(!cb->glubo){ G.GenBuffers(1,&cb->glubo); cb->ubosz=0; cb->uboglver=cb->ver-1; }
    if(cb->uboglver==cb->ver && cb->ubosz>=want) return;
    G.BindBuffer(0x8A11,cb->glubo);
    if(want>cb->ubosz) cb->ubosz=want;
    G.BufferData(0x8A11,(long)cb->ubosz,0,0x88E8/*DYNAMIC_DRAW: orphan old storage*/);
    G.BufferSubData(0x8A11,0,(long)cb->size,cb->data);
    cb->uboglver=cb->ver; }
static long d3dgl_ring_put(const void*src,uint64_t n); static unsigned g_ring; static uint32_t g_ring_gen; static uint64_t g_cbring_bytes, g_cbring_puts, g_cbbind_calls;
static void d3dgl_bind_ubo(unsigned binding,D3DObj*cb,uint32_t first,int nvec){
    long need=(long)nvec*16, off=0; unsigned buf; g_cbbind_calls++;
    if(cb && cb->data && cb->size && !getenv("PELOAD_D3DGL_NORING") && !getenv("PELOAD_D3DGL_NORING_CB")){
        // stream the CB through the ring: one unsynchronized copy per CPU write (ver) per ring generation
        long sub=(long)first*16; if((uint64_t)sub>=cb->size) sub=0;
        if(cb->uboglver!=cb->ver || cb->ubosz!=g_ring_gen || cb->glubo==0){
            uint64_t n=cb->size; if(n<(uint64_t)(sub+need)) n=(uint64_t)(sub+need);
            long ro;
            g_cbring_bytes+=n; g_cbring_puts++;
            if(n==cb->size) ro=d3dgl_ring_put(cb->data,n);
            else { void*tmp=calloc(1,(size_t)n); memcpy(tmp,cb->data,cb->size); ro=d3dgl_ring_put(tmp,n); free(tmp); }
            if(ro>=0){ cb->uboglver=cb->ver; cb->ubosz=g_ring_gen; cb->glubo=(unsigned)(ro+1); } }   // glubo = ring offset+1 here
        if(cb->glubo && cb->ubosz==g_ring_gen){ buf=g_ring; off=(long)cb->glubo-1+sub; goto bind; }
        cb=0; }                                                         // ring copy failed: zero buffer below
    if(cb && cb->data && cb->size){ off=(long)first*16; if((uint64_t)off>=cb->size) off=0;
        d3dgl_ubo_upload(cb,(uint32_t)(off+need)); buf=cb->glubo; }
    else { if(!g_zero_ubo){ G.GenBuffers(1,&g_zero_ubo); G.BindBuffer(0x8A11,g_zero_ubo); void*z=calloc(1,65536); G.BufferData(0x8A11,65536,z,0x88E4); free(z); }
        buf=g_zero_ubo; off=0; }
bind:
    if(binding<32 && g_ubo_bound[binding].buf==buf && g_ubo_bound[binding].off==off && g_ubo_bound[binding].sz==need && !getenv("PELOAD_D3DGL_NOBINDCACHE")) return;
    G.BindBufferRange(0x8A11,binding,buf,off,need);
    if(binding<32){ g_ubo_bound[binding].buf=buf; g_ubo_bound[binding].off=off; g_ubo_bound[binding].sz=need; } }
// ---- Monolithic (non-separable) program path ------------------------------------------------
// Apple's OpenGL-on-Metal fails to build a Metal pipeline from a SEPARABLE program pipeline
// (GL_ARB_separate_shader_objects) for halo3's real gameplay shaders: it logs "buildPipelineState
// failed", sets m_disable_code:1000, and drops ALL draws to SW vertex processing -> the scene never
// paints even though the sim runs. The proven-working self-test instead links VS+FS into ONE program.
// So for real draws we link monolithically, cached per (VS,PS) pair.
// VS and PS each name their constant buffers "cb<N>"; linked together they'd merge (or conflict on
// array size). d3dgl_rename_cb prefixes them per stage: VS "cb<N>"->"vXcb<N>", PS -> "pXcb<N>".
static int d3dgl_idchar(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
// VS resource names t<N> -> vt<N> so a monolithic program's VS and PS resources never alias (different D3D
// slots/types: the particle VS reads StructuredBuffers at t16/t17 while the PS samples t0..)
static int d3dgl_idchar(char c);
static char* d3dgl_rename_vt(const char*src){
    size_t n=strlen(src), cap=n+n/8+64, len=0; char*out=malloc(cap); if(!out) return 0;
    for(size_t i=0;i<n;){
        if(len+4>cap){ cap*=2; char*nn=realloc(out,cap); if(!nn){ free(out); return 0; } out=nn; }
        int previd=(i>0)&&d3dgl_idchar(src[i-1]);
        if(!previd && src[i]=='t' && src[i+1]>='0' && src[i+1]<='9'){ size_t j=i+1; while(src[j]>='0'&&src[j]<='9') j++;
            if(!d3dgl_idchar(src[j])){ out[len++]='v'; } }
        out[len++]=src[i++]; }
    out[len]=0; return out; }
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
    if(isvs){ char*r2=d3dgl_rename_vt(ren); free(ren); ren=r2; if(!ren) return 0; }   // VS resources -> vt# (own units 16..31)
    char*var=d3dgl_rename_varyings(ren,isvs); free(ren); if(!var) return 0;
    var=d3dgl_dedup_varyings(var);
    // PELOAD_D3DGL_PSCLAMP=<c>: wrap the PS main so every colour output has NaN -> 0 and is clamped to
    // [-c,c] (A/B for one-frame HDR blow-outs: a few Inf/huge pixels that bloom spreads over the screen).
    // PELOAD_D3DGL_HUGEPROBE=<t>: (with PELOAD_D3DGL_COVQ) keep ONLY pixels whose output exceeds t (others are
    // discarded), so the per-draw sample counts name the shaders that emit the huge values.
    if(var && !isvs && getenv("PELOAD_D3DGL_HUGEPROBE") && !getenv("PELOAD_D3DGL_PSCLAMP")){
        double t=atof(getenv("PELOAD_D3DGL_HUGEPROBE")); if(t<=0) t=1000.0;
        char*mn=strstr(var,"void main(){"); char*lb=strrchr(var,'}');
        if(mn && lb && lb>mn){ char tail[1024]; int tl=snprintf(tail,sizeof tail,"void main(){ dx_main(); bool hg=false;");
            for(int o=0;o<8;o++){ char d1[32]; snprintf(d1,sizeof d1,"out vec4 o%d;",o); if(strstr(var,d1)) tl+=snprintf(tail+tl,sizeof tail-tl," hg=hg||any(greaterThan(abs(o%d),vec4(%.1f)));",o,t); }
            tl+=snprintf(tail+tl,sizeof tail-tl," if(!hg) discard; }\n");
            size_t vl=strlen(var); char*nv=malloc(vl+tl+16);
            if(nv){ size_t pre=(size_t)(mn-var); memcpy(nv,var,pre); memcpy(nv+pre,"void dx_main(){",15); memcpy(nv+pre+15,mn+12,vl-pre-12); nv[vl+3]=0;
                strcat(nv,"\n"); strcat(nv,tail); free(var); var=nv; } } }
    if(var && !isvs && getenv("PELOAD_D3DGL_PSCLAMP")){
        double c=atof(getenv("PELOAD_D3DGL_PSCLAMP")); if(c<=0) c=1000.0;
        char*mn=strstr(var,"void main(){"); char*lb=strrchr(var,'}');
        if(mn && lb && lb>mn){ char tail[1024]; int tl=snprintf(tail,sizeof tail,"vec4 dx_clampo(vec4 v){ v=mix(v,vec4(0.0),isnan(v)); return clamp(v,vec4(-%.1f),vec4(%.1f)); }\nvoid main(){ dx_main();",c,c);
            for(int o=0;o<8;o++){ char d1[32]; snprintf(d1,sizeof d1,"out vec4 o%d;",o); if(strstr(var,d1)) tl+=snprintf(tail+tl,sizeof tail-tl," o%d=dx_clampo(o%d);",o,o); }
            tl+=snprintf(tail+tl,sizeof tail-tl," }\n");
            size_t vl=strlen(var); char*nv=malloc(vl+tl+16);
            if(nv){ size_t pre=(size_t)(mn-var); memcpy(nv,var,pre); memcpy(nv+pre,"void dx_main(){",15); memcpy(nv+pre+15,mn+12,vl-pre-12); nv[vl+3]=0;
                strcat(nv,"\n"); strcat(nv,tail); free(var); var=nv; } } }
    // PELOAD_D3DGL_SOLIDPS: geometry-coverage diagnostic -- make every PS output solid red and drop
    // discards, so we see exactly what geometry/camera covers the frame independent of shading.
    if(var && !isvs && getenv("PELOAD_D3DGL_SOLIDPS")){
        for(char*p=strstr(var,"discard;"); p; p=strstr(p+1,"discard;")) memcpy(p,"/*skip*/",8);
        for(char*p=strstr(var,"return;"); p; p=strstr(p+1,"return;")) memcpy(p,"/*rtn*/",7);  // strip returns so the override runs
        static char insbuf[128]; const char*ins="  o0=vec4(1.0,0.25,0.25,1.0);\n";
        // PELOAD_D3DGL_TEXVIZ: sample t0 at the real TEXCOORD0 varying (first inter-stage xfer) so we
        // see if the diffuse/control texture has data. PELOAD_D3DGL_UVVIZ: output that texcoord as color
        // (gradient = UVs vary correctly; flat = UVs broken -> everything samples one texel -> black).
        // Locate the varying tagged TEXCOORD0 (the terrain diffuse/control UV). The register that carries
        // TEXCOORD0 is NOT the first inter-stage varying (v1 is often SV_ClipDistance0); find it by comment.
        char xf[16]="xfer0"; const char*tc=strstr(var,"// TEXCOORD0"); const char*xp=0;
        if(tc){ const char*ls=tc; while(ls>var && ls[-1]!='\n') ls--; xp=strstr(ls,"xfer"); if(xp>tc) xp=0; }
        if(!xp) xp=strstr(var,"in vec4 xfer"), xp=xp?xp+8:0;   // fallback: first xfer
        if(xp){ int i=0; while(xp[i]&&xp[i]!=';'&&xp[i]!=' '&&i<15){ xf[i]=xp[i]; i++; } xf[i]=0; }
        if(getenv("PELOAD_D3DGL_UVVIZ")){ snprintf(insbuf,sizeof insbuf,"  o0=vec4(fract(%s.xy),0.0,1.0);\n",xf); ins=insbuf; }
        else if(getenv("PELOAD_D3DGL_TEXVIZ")){ // sample texture unit N at TEXCOORD0. PELOAD_D3DGL_TEXVIZ=<n> picks t<n> (default 1).
            int u=atoi(getenv("PELOAD_D3DGL_TEXVIZ")); if(u<0||u>31)u=1; char tn[8]; snprintf(tn,sizeof tn,"t%d",u);
            char sd[20]; snprintf(sd,sizeof sd,"sampler2D %s",tn);
            const char*use = strstr(var,sd)?tn:(strstr(var,"sampler2D t1")?"t1":(strstr(var,"sampler2D t0")?"t0":0));
            // scale*4 so a dark control/detail map is still visible; show .x in R, .y in G (control maps use xy).
            if(use){ snprintf(insbuf,sizeof insbuf,"  o0=vec4(texture(%s,%s.xy).xyz*4.0,1.0);\n",use,xf); ins=insbuf; } }
        char*lb=strrchr(var,'}'); if(lb){ size_t il=strlen(ins),vl=strlen(var),pre=(size_t)(lb-var);
            char*nv=malloc(vl+il+1); if(nv){ memcpy(nv,var,pre); memcpy(nv+pre,ins,il); memcpy(nv+pre+il,lb,vl-pre+1); free(var); var=nv; } } }
    // PELOAD_D3DGL_ALBVIZ: in the terrain PS, output the raw blended ALBEDO (register r1) brightened,
    // short-circuiting all lighting. The albedo accumulation ends right before the exposure constant
    // 4.59478998; inject an o0 write + return there. Reveals whether the albedo itself is 0 (texture/
    // blend problem) or nonzero (so the darkness is in the downstream lighting/mix math).
    if(var && !isvs && getenv("PELOAD_D3DGL_ALBVIZ")){
        char*m=strstr(var,"4.59478998");
        if(m){ char*ls=m; while(ls>var && ls[-1]!='\n') ls--;   // start of that line
            // find the TEXCOORD0 varying so ALBVIZ=2 samples t1 at the shader's REAL UV
            // (v2*cb13[2]+cb13[2].zw; cb13 is renamed pXcb13 in the PS).
            char xf[16]="xfer0"; const char*tc=strstr(var,"// TEXCOORD0"); const char*xp=0;
            if(tc){ const char*l2=tc; while(l2>var&&l2[-1]!='\n')l2--; xp=strstr(l2,"xfer"); if(xp>tc)xp=0; }
            if(xp){ int i=0; while(xp[i]&&xp[i]!=';'&&xp[i]!=' '&&i<15){xf[i]=xp[i];i++;} xf[i]=0; }
            char inj[224];
            if(atoi(getenv("PELOAD_D3DGL_ALBVIZ"))>=2)
                snprintf(inj,sizeof inj,"  o0=vec4(clamp((texture(t1,%s.xy*pXcb13[2].xy+pXcb13[2].zw)).xyz*4.0,0.0,1.0),1.0);return;\n",xf);
            else snprintf(inj,sizeof inj,"  o0=vec4(clamp(r1.xyz*8.0,0.0,1.0),1.0);return;\n");
            size_t il=strlen(inj),vl=strlen(var),pre=(size_t)(ls-var);
            char*nv=malloc(vl+il+1); if(nv){ memcpy(nv,var,pre); memcpy(nv+pre,inj,il); memcpy(nv+pre+il,ls,vl-pre+1); free(var); var=nv; }
            if(getenv("PELOAD_D3DGL_DUMPINJ")){ static int di; if(di++<2){ char pp[64]; snprintf(pp,sizeof pp,"/tmp/albviz_ps_%d.glsl",di); FILE*f=fopen(pp,"wb"); if(f){ fwrite(var,1,strlen(var),f); fclose(f);} di++; } } } }
    // PELOAD_D3DGL_BRIGHT=<f>: multiply the REAL PS output o0.xyz by f before writeout (tonemap stand-in).
    // Works on the unmodified shader (not SOLIDPS): if the scene is merely dark (linear HDR pre-tonemap),
    // this reveals the textured/lit geometry; if the control-map gate zeroed everything, it stays black.
    // PELOAD_D3DGL_NANVIZ: paint any pixel whose o0 is NaN/Inf GREEN (the lighting math divides by the
    // length of VS-output tangent varyings; if those are 0 -> inversesqrt(0)=Inf -> NaN -> black).
    if(var && !isvs && !getenv("PELOAD_D3DGL_SOLIDPS") && (getenv("PELOAD_D3DGL_BRIGHT")||getenv("PELOAD_D3DGL_NANVIZ"))){
        double f=getenv("PELOAD_D3DGL_BRIGHT")?atof(getenv("PELOAD_D3DGL_BRIGHT")):8.0; if(f<=0)f=8.0;
        for(char*p=strstr(var,"return;"); p; p=strstr(p+1,"return;")) memcpy(p,"/*rtn*/",7);
        static char bb[192];
        if(getenv("PELOAD_D3DGL_NANVIZ"))
            snprintf(bb,sizeof bb,"  if(any(isnan(o0.xyz))||any(isinf(o0.xyz)))o0=vec4(0.0,1.0,0.0,1.0);else o0=vec4(clamp(o0.xyz*%.4f,0.0,1.0),1.0);\n",f);
        else snprintf(bb,sizeof bb,"  o0.xyz*=%.4f;\n",f);
        char*lb=strrchr(var,'}'); if(lb){ size_t il=strlen(bb),vl=strlen(var),pre=(size_t)(lb-var);
            char*nv=malloc(vl+il+1); if(nv){ memcpy(nv,var,pre); memcpy(nv+pre,bb,il); memcpy(nv+pre+il,lb,vl-pre+1); free(var); var=nv; } } }
    return var;
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
// Compiled GL shader for (D3D shader, interface-padding mask): most pairs need no or the same padding, so
// a VS/PS compiles once and is attached to every program that uses it (compile was ~2/3 of a first-use link).
static unsigned d3dgl_shcache(D3DObj*o,uint64_t mask,const char*src,unsigned stage){
    for(int i=0;i<o->shc_n;i++) if(o->shc_mask[i]==mask && o->shc_id[i]) return o->shc_id[i];
    unsigned sh=d3dgl_compile(src,stage); if(!sh) return 0;
    int slot=o->shc_n<4?o->shc_n++:3;
    if(o->shc_n>=4 && o->shc_id[slot] && G.DeleteShader) G.DeleteShader(o->shc_id[slot]);   // evict (programs keep their copy)
    o->shc_id[slot]=sh; o->shc_mask[slot]=mask; return sh; }
// ---- persistent first-use pair list -----------------------------------------------------------------
// A new VS/PS pair costs ~10-40 ms to build (GLSL compile + link on GL-on-Metal); effects introduce a dozen
// at once (a grenade explosion froze the game ~0.4 s). Every linked pair is recorded by bytecode hash in
// ~/Library/Application Support/ReclaimerMac/glpairs_v1.txt; on later runs those pairs are built while the
// loading screen is up (d3dgl_prelink_tick at Present) so gameplay rarely meets an unbuilt one.
// PELOAD_D3DGL_NOPRELINK disables both the recording and the pre-build.
#define PL_MAXSH 16384
#define PL_MAXPAIR 8192
static struct { uint64_t h; D3DObj*o; } g_pl_sh[PL_MAXSH];            // bytecode hash -> shader object
static struct { uint64_t vh, ph; int done; } g_pl_pair[PL_MAXPAIR]; static int g_pl_npair, g_pl_loaded, g_pl_next;
static pthread_mutex_t g_pl_mx=PTHREAD_MUTEX_INITIALIZER;
static int pl_on(void){ static int on=-1; if(on<0) on=!getenv("PELOAD_D3DGL_NOPRELINK"); return on; }
static void pl_path(char*p,size_t n){ const char*h=getenv("HOME"); snprintf(p,n,"%s/Library/Application Support/ReclaimerMac/glpairs_v1.txt",h?h:"/tmp"); }
static D3DObj* pl_find(uint64_t h){ for(unsigned i=0;i<PL_MAXSH;i++){ unsigned k=(unsigned)((h+i)&(PL_MAXSH-1)); if(!g_pl_sh[k].h) return 0; if(g_pl_sh[k].h==h) return g_pl_sh[k].o; } return 0; }
static int pl_havepair(uint64_t vh,uint64_t ph){ for(int i=0;i<g_pl_npair;i++) if(g_pl_pair[i].vh==vh&&g_pl_pair[i].ph==ph) return i; return -1; }
static void pl_load(void){ if(g_pl_loaded) return; g_pl_loaded=1;
    char p[512]; pl_path(p,sizeof p); FILE*f=fopen(p,"r"); if(!f) return;
    unsigned long long a,b; while(g_pl_npair<PL_MAXPAIR && fscanf(f,"%llx %llx",&a,&b)==2){ g_pl_pair[g_pl_npair].vh=a; g_pl_pair[g_pl_npair].ph=b; g_pl_pair[g_pl_npair].done=0; g_pl_npair++; }
    fclose(f);
    char m[160]; int k=snprintf(m,sizeof m,"  [prelink] %d known shader pairs from %s\n",g_pl_npair,p); (void)!write(2,m,k); }
// Register a new shader by bytecode hash. halo3 creates many shaders with byte-identical bytecode (~1/4 of
// pairs); those share the first object's programs (o->canon) instead of building duplicates.
static void d3dgl_prelink_register(D3DObj*o){ if(!o->bchash) return;
    pthread_mutex_lock(&g_pl_mx); if(pl_on()) pl_load();
    for(unsigned i=0;i<PL_MAXSH;i++){ unsigned k=(unsigned)((o->bchash+i)&(PL_MAXSH-1)); if(!g_pl_sh[k].h){ g_pl_sh[k].h=o->bchash; g_pl_sh[k].o=o; break; }
        if(g_pl_sh[k].h==o->bchash){ D3DObj*c=g_pl_sh[k].o;
            if(c&&c!=o&&c->stype==o->stype&&c->bclen==o->bclen&&c->bc&&o->bc&&!memcmp(c->bc,o->bc,o->bclen)&&!getenv("PELOAD_D3DGL_NOCANON")) o->canon=c;
            break; } }
    pthread_mutex_unlock(&g_pl_mx); }
static void d3dgl_prelink_record(D3DObj*vs,D3DObj*ps){ if(!pl_on()||!vs->bchash||!ps->bchash) return;
    pthread_mutex_lock(&g_pl_mx); pl_load();
    int i=pl_havepair(vs->bchash,ps->bchash);
    if(i>=0) g_pl_pair[i].done=1;
    else if(g_pl_npair<PL_MAXPAIR){ g_pl_pair[g_pl_npair].vh=vs->bchash; g_pl_pair[g_pl_npair].ph=ps->bchash; g_pl_pair[g_pl_npair].done=1; g_pl_npair++;
        char p[512]; pl_path(p,sizeof p); FILE*f=fopen(p,"a"); if(f){ fprintf(f,"%016llx %016llx\n",(unsigned long long)vs->bchash,(unsigned long long)ps->bchash); fclose(f); } }
    pthread_mutex_unlock(&g_pl_mx); }
static char *m_vsrc_keep, *m_psrc_keep;   // last mono sources (block-size parsing at link)
typedef struct { void*vs; void*ps; unsigned prog; int tried; int vcb[16],pcb[16],tloc[32]; signed char tt[32];
    int vblk[16], pblk[16];        // UBO path: declared block size in vec4s per D3D slot (0 = slot unused)
    int units_set; int relcb; int vtloc[32]; signed char vtt[32]; int vunits_set; int drawn; } MonoProg;   // relcb: 1 = VS / 2 = PS indexes a constant buffer by register
#define MONO_N 4096
static MonoProg g_mono[MONO_N]; static int g_mono_used;
static MonoProg* d3dgl_monoprog(D3DObj*vs,D3DObj*ps){
    uintptr_t h=(((uintptr_t)vs>>4)*2654435761u) ^ (((uintptr_t)ps>>4)*40503u);
    for(int probe=0;probe<MONO_N;probe++){ MonoProg*m=&g_mono[(int)((h+probe)&(MONO_N-1))];
        if(m->tried){ if(m->vs==vs&&m->ps==ps) return m; continue; }   // occupied by another pair -> probe on
        m->vs=vs; m->ps=ps; m->tried=1; m->prog=0; g_mono_used++;      // claim slot; prog stays 0 on any failure
        double pt0=mono_s();
        // per-shader GLSL is built once (it doesn't depend on the partner); only the padding is per pair
        if(!vs->monosrc) vs->monosrc=d3dgl_mono_src(vs,1);
        if(!ps->monosrc) ps->monosrc=d3dgl_mono_src(ps,0);
        { const char*dh=getenv("PELOAD_D3DGL_DUMPHASH");   // dump mono GLSL + DXBC of the listed shader hashes
          for(int st=0;st<2&&dh;st++){ D3DObj*o=st?ps:vs; char hx[20]; snprintf(hx,sizeof hx,"%016llx",(unsigned long long)o->bchash);
            if(o->monosrc && strstr(dh,hx)){ const char*h=getenv("HOME"); char pth[300]; snprintf(pth,sizeof pth,"%s/reclaimer_dump_%s.glsl",h?h:"/tmp",hx);
                FILE*f=fopen(pth,"w"); if(f){ fputs(o->monosrc,f); fclose(f); }
                snprintf(pth,sizeof pth,"%s/reclaimer_dump_%s.dxbc",h?h:"/tmp",hx); f=fopen(pth,"wb"); if(f){ fwrite(o->bc,1,o->bclen,f); fclose(f); } } } }
        const char*vsrc=vs->monosrc, *psrc=ps->monosrc;
        double pt1=mono_s();
        if(!vsrc||!psrc) return m;
        memset(m->tt,0,sizeof m->tt); d3dgl_scan_tt(psrc,m->tt);   // PS sampler dimension per t#
        memset(m->vtt,0,sizeof m->vtt); d3dgl_scan_tt_p(vsrc,m->vtt,"vt");   // VS resources (renamed vt#)
        uint64_t vo=d3dgl_iface_mask(vsrc,1), fi=d3dgl_iface_mask(psrc,0);
        uint64_t vadd=fi & ~vo, padd=vo & ~fi;
        char*vfull=d3dgl_augment(vsrc, vadd, 1);   // VS gets the outputs the PS reads but the VS lacked
        char*pfull=d3dgl_augment(psrc, padd, 0);   // PS gets inputs for the VS outputs it didn't read
        if(!vfull||!pfull){ free(vfull); free(pfull); return m; }
        unsigned vsh=d3dgl_shcache(vs,vadd,vfull,GLE_VS), psh=d3dgl_shcache(ps,padd,pfull,GLE_FS);
        free(m_vsrc_keep); free(m_psrc_keep); m_vsrc_keep=vfull; m_psrc_keep=pfull;   // block sizes parsed after link
        if(!vsh||!psh) return m;
        double pt2=mono_s();
        unsigned prog=G.CreateProgram(); G.AttachShader(prog,vsh); G.AttachShader(prog,psh); G.LinkProgram(prog);
        if(G.DetachShader){ G.DetachShader(prog,vsh); G.DetachShader(prog,psh); }   // shader objects stay cached for other pairs
        int lok=0; G.GetProgramiv(prog,GLE_LSTAT,&lok);
        if(lok) d3dgl_prelink_record(vs,ps);
        if(getenv("PELOAD_D3DGL_PROGTIME")){ double pt3=mono_s(); extern double g_gameplay_t0; static int np; np++;
            char mm[200]; int k=snprintf(mm,sizeof mm,"  [progtime] #%d t=%.2f translate %.1f ms compile %.1f ms link %.1f ms\n",np,g_gameplay_t0?pt3-g_gameplay_t0:-1.0,(pt1-pt0)*1e3,(pt2-pt1)*1e3,(pt3-pt2)*1e3); (void)!write(2,mm,k); }
        if(!lok){ if(g_d3dgl_trace){ static char log[16384]; log[0]=0; if(G.GetProgramInfoLog)G.GetProgramInfoLog(prog,sizeof log,0,log);
                // print the ERROR lines (unread-varying WARNINGs precede them and used to fill the old 1 KB buffer)
                int any=0; for(char*l=log;l&&*l;){ char*nl=strchr(l,'\n'); if(nl)*nl=0; if(strstr(l,"ERROR")){ ws("  [d3dgl] mono link FAIL: "); ws(l); ws("\n"); any=1; } l=nl?nl+1:0; }
                if(!any){ ws("  [d3dgl] mono link FAIL (no ERROR line): "); ws(log); ws("\n"); }
                if(getenv("PELOAD_D3DGL_LINKDUMP")){ static int ld; if(ld<4){ char pth[256]; const char*h=getenv("HOME"); snprintf(pth,sizeof pth,"%s/reclaimer_linkfail_%d_vs.glsl",h?h:"/tmp",ld); FILE*f=fopen(pth,"w"); if(f){ fputs(m_vsrc_keep?m_vsrc_keep:"",f); fclose(f);} snprintf(pth,sizeof pth,"%s/reclaimer_linkfail_%d_ps.glsl",h?h:"/tmp",ld); f=fopen(pth,"w"); if(f){ fputs(m_psrc_keep?m_psrc_keep:"",f); fclose(f);} ld++; } } } return m; }
        m->prog=prog; char nm[24];
        for(int b=0;b<16;b++){ snprintf(nm,sizeof nm,"vXcb%d",b); m->vcb[b]=G.GetUniformLocation(prog,nm);
                               snprintf(nm,sizeof nm,"pXcb%d",b); m->pcb[b]=G.GetUniformLocation(prog,nm); }
        for(int b=0;b<32;b++){ snprintf(nm,sizeof nm,"t%d",b);    m->tloc[b]=G.GetUniformLocation(prog,nm); }
        for(int b=0;b<32;b++){ snprintf(nm,sizeof nm,"vt%d",b);   m->vtloc[b]=G.GetUniformLocation(prog,nm); }
        m->vunits_set=0;
        // constant-buffer blocks: VS slot b -> binding b, PS slot b -> binding 16+b
        memset(m->vblk,0,sizeof m->vblk); memset(m->pblk,0,sizeof m->pblk);
        if(G.GetUniformBlockIndex && G.UniformBlockBinding) for(int b=0;b<16;b++){
            for(int st=0;st<2;st++){ snprintf(nm,sizeof nm,"%cXcb%d_blk",st?'p':'v',b); unsigned bi=G.GetUniformBlockIndex(prog,nm);
                if(bi==0xFFFFFFFFu) continue; G.UniformBlockBinding(prog,bi,(unsigned)(st?16+b:b));
                const char*src=st?m_psrc_keep:m_vsrc_keep; int n=0; char pat[40]; snprintf(pat,sizeof pat,"uniform %cXcb%d_blk { vec4 %cXcb%d[",st?'p':'v',b,st?'p':'v',b);
                const char*q=src?strstr(src,pat):0; if(q) n=atoi(q+strlen(pat)); if(n<=0) n=4096;
                if(st) m->pblk[b]=n; else m->vblk[b]=n; } }
        m->relcb = ((m_vsrc_keep&&strstr(m_vsrc_keep,"[floatBitsToInt(")) || (m_vsrc_keep&&strstr(m_vsrc_keep," + floatBitsToInt(r")&&strstr(m_vsrc_keep,"Xcb")))?1:0;
        if(m_psrc_keep && (strstr(m_psrc_keep,"Xcb3[1 + floatBitsToInt") || strstr(m_psrc_keep,"Xcb3[floatBitsToInt"))) m->relcb|=2;
        free(m_vsrc_keep); free(m_psrc_keep); m_vsrc_keep=m_psrc_keep=0;
        m->units_set=0;
        if(g_d3dgl_trace){ static int n; if(++n<=40){ char mm[64]; int k=snprintf(mm,sizeof mm,"  [d3dgl] mono-linked program #%d\n",n); (void)!write(2,mm,k);} }
        return m;
    }
    return 0;   // table full
}
// Build known pairs whose shaders exist, within budget_ms (render thread, GL lock held). Returns pairs built.
static int d3dgl_prelink_tick(double budget_ms){
    if(!pl_on()||!g_pl_npair||g_pl_next>=g_pl_npair||g_mono_used>MONO_N*3/4) return 0;   // keep room for new pairs
    double t0=mono_s(); int built=0, i;
    for(i=g_pl_next;i<g_pl_npair;i++){
        if(g_pl_pair[i].done) continue;
        pthread_mutex_lock(&g_pl_mx); D3DObj*vs=pl_find(g_pl_pair[i].vh), *ps=pl_find(g_pl_pair[i].ph); pthread_mutex_unlock(&g_pl_mx);
        if(!vs||!ps||!vs->glsl||!ps->glsl) continue;          // not created yet (this map/load) -- retried next tick
        d3dgl_monoprog(vs,ps); g_pl_pair[i].done=1; built++;
        if((mono_s()-t0)*1e3>=budget_ms){ i++; break; } }
    // advance the cursor past the leading run of finished entries
    while(g_pl_next<g_pl_npair && g_pl_pair[g_pl_next].done) g_pl_next++;
    if(built && getenv("PELOAD_D3DGL_PROGTIME")){ static int tot; tot+=built; char m[120]; int k=snprintf(m,sizeof m,"  [prelink] built %d (total %d) in %.1f ms\n",built,tot,(mono_s()-t0)*1e3); (void)!write(2,m,k); }
    return built; }
// ClearRenderTargetView -> clear that RT's GL FBO color (engine's clears are no-ops in our stub, so
// without this the GL render targets accumulate across frames). Runs only on our render thread/context.
static void d3dgl_clear_rtv(void* rtvview,const float* col){
    if(!g_d3dgl_ready) return; g_fs.rtclears++;
    D3DObj* v=rtvview; D3DObj* rt=(v&&v->data)?(D3DObj*)v->data:v; if(!rt) return;
    GLLOCK(); if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
    d3dgl_ensure_rt(rt); G.BindFramebuffer(GLE_FBO,rt->glfbo); d3dgl_reset_raster_ops();
    if(rt->glsrgb && dxgi_is_srgb((v&&v->vkind)?v->vfmt:rt->fmt)) gs_cap(0x8DB9,1);   // D3D encodes the clear colour for _SRGB RTVs
    if(col) G.ClearColor(col[0],col[1],col[2],col[3]); else G.ClearColor(0,0,0,0);
    G.Clear(GLE_COLORBIT); gs_cap(0x8DB9,0); GLUNLOCK(); }
static void d3dgl_clear_dsv(void*dsvview,uint32_t flags,float depth,uint8_t stencil){
    if(!g_d3dgl_ready || getenv("PELOAD_D3DGL_NODEPTH")) return; g_fs.dsclears++;
    D3DObj* v=dsvview; D3DObj* ds=(v&&v->data)?(D3DObj*)v->data:v; if(!ds) return;
    GLLOCK(); if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
    unsigned fbo=d3dgl_fbo_for(0,0,ds); G.BindFramebuffer(GLE_FBO,fbo); d3dgl_reset_raster_ops();
    unsigned bits=0;
    if(flags&1){ gs_depthmask(1); if(G.ClearDepth) G.ClearDepth((double)depth); bits|=0x100; }      // D3D11_CLEAR_DEPTH
    if((flags&2) && ds->gldsatt==0x821A){ if(G.StencilMask) G.StencilMask(0xff); if(G.ClearStencil) G.ClearStencil(stencil); bits|=0x400; } // _STENCIL
    if(bits) G.Clear(bits);
    G.BindFramebuffer(GLE_FBO,0); GLUNLOCK(); }
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
// DXGI format -> (components, GL integer type) for glVertexAttribIPointer, used when the VS declares the input as
// uvec4/ivec4 (DXBC signature component type uint/sint). 32-bit float formats pass their raw bits (D3D puts
// the float's bits in the register; the shader reinterprets them).
static int d3dgl_attrib_ifmt(uint32_t f,unsigned*comps,unsigned*gt){
    switch(f){
        case 3: *comps=4;*gt=0x1405;return 1;  case 4: *comps=4;*gt=0x1404;return 1;   // R32G32B32A32_UINT/SINT
        case 7: *comps=3;*gt=0x1405;return 1;  case 8: *comps=3;*gt=0x1404;return 1;   // R32G32B32_UINT/SINT
        case 12:*comps=4;*gt=0x1403;return 1;  case 14:*comps=4;*gt=0x1402;return 1;   // R16G16B16A16_UINT/SINT
        case 17:*comps=2;*gt=0x1405;return 1;  case 18:*comps=2;*gt=0x1404;return 1;   // R32G32_UINT/SINT
        case 30:*comps=4;*gt=0x1401;return 1;  case 32:*comps=4;*gt=0x1400;return 1;   // R8G8B8A8_UINT/SINT
        case 36:*comps=2;*gt=0x1403;return 1;  case 38:*comps=2;*gt=0x1402;return 1;   // R16G16_UINT/SINT
        case 42:*comps=1;*gt=0x1405;return 1;  case 43:*comps=1;*gt=0x1404;return 1;   // R32_UINT/SINT
        case 50:*comps=2;*gt=0x1401;return 1;  case 52:*comps=2;*gt=0x1400;return 1;   // R8G8_UINT/SINT
        case 57:*comps=1;*gt=0x1403;return 1;  case 59:*comps=1;*gt=0x1402;return 1;   // R16_UINT/SINT
        case 62:*comps=1;*gt=0x1401;return 1;  case 64:*comps=1;*gt=0x1400;return 1;   // R8_UINT/SINT
        case 2: *comps=4;*gt=0x1405;return 1;  case 6: *comps=3;*gt=0x1405;return 1;   // float32 formats: raw bits
        case 16:*comps=2;*gt=0x1405;return 1;  case 41:*comps=1;*gt=0x1405;return 1;
    } return 0; }
// Bitmask of VS input registers the translator declared as integer ("in uvec4 viN" / "in ivec4 viN").
static uint32_t d3dgl_vs_intin(D3DObj*vs){
    if(!vs||!vs->glsl) return 0; if(vs->intin_ok) return vs->intin; uint32_t m=0;
    for(const char*p=vs->glsl; (p=strstr(p,"vec4 vi"))!=0; p+=7){ if(p>vs->glsl && (p[-1]=='u'||p[-1]=='i')){ long r=atol(p+7); if(r>=0&&r<32) m|=1u<<r; } }
    vs->intin=m; vs->intin_ok=1; return m; }
// ---- D3D11 input-assembler semantics for the GL path ------------------------------------------------
// (1) D3D11 matches input-layout elements to VS inputs BY SEMANTIC (name+index), not by element order; the
//     translator declares each VS input as layout(location=<register>), so the GL attribute location must
//     be the register the VS's ISGN assigns to that semantic. Binding by element index crossed attributes
//     whenever a layout's order differed from the shader's register order.
typedef struct { char name[32]; uint32_t idx, reg; } VsIn;
static int d3dgl_vs_inreg(D3DObj*vs,const char*sem,uint32_t semidx){   // -2 = no signature, -1 = not consumed
    if(!vs||!vs->bc||vs->bclen<32) return -2;
    if(!vs->isg){ VsIn*t=calloc(32,sizeof(VsIn)); int n=0; const uint8_t*d=vs->bc; size_t len=vs->bclen;
        if(t && !memcmp(d,"DXBC",4)){ uint32_t nch=*(const uint32_t*)(d+28);
            for(uint32_t c=0;c<nch && 32+4*c+4<=len;c++){ uint32_t o=*(const uint32_t*)(d+32+4*c); if((size_t)o+8>len) continue;
                if(memcmp(d+o,"ISGN",4)) continue;
                DgSig sg[32]; int k=dg_parse_sig(d,len,o+8,sg,32);
                for(int i=0;i<k && n<32;i++){ if(sg[i].sysval) continue;   // SV_VertexID etc. aren't fed by the IA
                    snprintf(t[n].name,sizeof t[n].name,"%s",sg[i].name); t[n].idx=sg[i].idx; t[n].reg=sg[i].reg; n++; }
                break; } }
        if(!t) return -2; vs->isg=t; vs->nisg=n; }
    if(!vs->nisg) return -2;
    const VsIn*t=(const VsIn*)vs->isg;
    for(int i=0;i<vs->nisg;i++) if(t[i].idx==semidx && !strcasecmp(t[i].name,sem)) return (int)t[i].reg;
    return -1; }
// (2) Ensure GL buffer b holds valid data for bytes [lo,hi). A never-mapped buffer is uploaded whole once;
//     after a Map/UpdateSubresource only the range a draw actually references is re-sent (BufferSubData)
//     instead of re-uploading the whole multi-MB ring buffer on EVERY draw (was ~88% of frame time). Any
//     Map invalidates the tracked range, so NO_OVERWRITE reuse of already-drawn regions stays correct.
static uint64_t g_vx_ns[4], g_vx_bytes, g_vx_calls; static uint64_t prof_now(void); static int prof_on(void);
// Streaming ring for D3D dynamic (NO_OVERWRITE append) vertex data: each draw's freshly-written vertex range
// is copied into a shim-owned GL buffer through an UNSYNCHRONIZED map (D3D guarantees the app never rewrites
// in-flight data; GL can't know that, so glBufferSubData into the game's buffer synchronized: ~19 us x 25k/s
// = half the frame). On wrap the ring is orphaned (fresh storage) so in-flight draws keep their data.
static unsigned g_ring; static uint64_t g_ring_off, g_ring_sz=64ull<<20; static uint32_t g_ring_gen=1;
static long d3dgl_ring_put(const void*src,uint64_t n){
    if(!G.MapBufferRange||!G.UnmapBuffer||n==0||n>g_ring_sz) return -1;
    if(!g_ring){ G.GenBuffers(1,&g_ring); G.BindBuffer(GLE_ARRAY,g_ring); G.BufferData(GLE_ARRAY,(long)g_ring_sz,0,0x88E0/*STREAM_DRAW*/); g_ring_off=0; }
    else G.BindBuffer(GLE_ARRAY,g_ring);
    if(g_ring_off+n>g_ring_sz){
        // Never orphan in the middle of a draw: data this draw already streamed (its constant buffers, other
        // vertex streams) lives in the current storage and its bindings would silently move to the fresh,
        // uninitialised storage -> one frame with a garbage transform (screen-covering polygons, stray lines).
        // d3dgl_ring_reserve() wraps between draws; a put that still doesn't fit uses the synchronous path.
        static int old=-1; if(old<0) old=getenv("PELOAD_D3DGL_RINGWRAP_OLD")?1:0;
        if(!old){ g_fs.ringfail++; return -1; }
        G.BufferData(GLE_ARRAY,(long)g_ring_sz,0,0x88E0); g_ring_off=0; g_ring_gen++; }
    void*p=G.MapBufferRange(GLE_ARRAY,(long)g_ring_off,(long)n,0x0002/*WRITE*/|0x0004/*INVALIDATE_RANGE*/|0x0020/*UNSYNCHRONIZED*/);
    if(!p) return -1;
    memcpy(p,src,(size_t)n); G.UnmapBuffer(GLE_ARRAY);
    long r=(long)g_ring_off; g_ring_off+=(n+255)&~255ull; return r; }
// Called at the start of every draw, before anything is streamed: orphan the ring while no draw has data in
// it if less than 16 MB is left (a draw streams far less), so all of one draw's ring data shares a storage.
static void d3dgl_ring_reserve(void){
    if(!g_ring || g_ring_off+(16ull<<20)<=g_ring_sz) return;
    G.BindBuffer(GLE_ARRAY,g_ring); G.BufferData(GLE_ARRAY,(long)g_ring_sz,0,0x88E0); g_ring_off=0; g_ring_gen++; g_fs.wraps++; }
static void d3dgl_buf_need_i(D3DObj*b,unsigned target,uint64_t lo,uint64_t hi);
static void d3dgl_buf_need(D3DObj*b,unsigned target,uint64_t lo,uint64_t hi){
    if(!prof_on()){ d3dgl_buf_need_i(b,target,lo,hi); return; }
    uint64_t t=prof_now(); int was=b->gl_dirty||!b->glbuf; d3dgl_buf_need_i(b,target,lo,hi); g_vx_ns[target==GLE_ELEM?1:0]+=prof_now()-t;
    if(was){ g_vx_bytes+=hi>lo?hi-lo:0; g_vx_calls++; } }
static void d3dgl_buf_need_i(D3DObj*b,unsigned target,uint64_t lo,uint64_t hi){
    cs_wait_res(b);
    uint64_t cap=b->size; if(cap>(96ull<<20)) cap=96ull<<20;
    if(!b->glbuf){ G.GenBuffers(1,&b->glbuf); G.BindBuffer(target,b->glbuf); G.BufferData(target,(long)cap,b->data,GLE_STREAM); b->gl_dirty=0; b->rvalid=0; return; }
    G.BindBuffer(target,b->glbuf);
    if(!b->gl_dirty) return;
    if(!G.BufferSubData){ G.BufferData(target,(long)cap,b->data,GLE_STREAM); b->gl_dirty=0; return; }
    if(hi>cap) hi=cap; if(lo>=hi) return;
    if(b->rvalid && lo>=b->rlo && hi<=b->rhi) return;
    G.BufferSubData(target,(long)lo,(long)(hi-lo),(const uint8_t*)b->data+lo);
    if(b->rvalid && lo<=b->rhi && hi>=b->rlo){ if(lo<b->rlo)b->rlo=lo; if(hi>b->rhi)b->rhi=hi; }
    else { b->rlo=lo; b->rhi=hi; b->rvalid=1; } }
// (3) min/max vertex index referenced by [byteoff, byteoff+cnt*isz) of the index buffer (skipping the
//     strip-cut index), so a dynamic VB only uploads the vertices this draw can touch.
static int d3dgl_index_range(D3DObj*ib,uint64_t byteoff,uint32_t cnt,int is32,int restart,uint32_t*mn,uint32_t*mx){
    if(!ib||!ib->data) return 0; uint64_t isz=is32?4:2; if(byteoff>=ib->size) return 0;
    uint64_t maxc=(ib->size-byteoff)/isz; if(cnt>maxc) cnt=(uint32_t)maxc;
    uint32_t lo=0xFFFFFFFFu, hi=0; const uint8_t*p=(const uint8_t*)ib->data+byteoff;
    if(is32){ const uint32_t*q=(const uint32_t*)p; for(uint32_t i=0;i<cnt;i++){ uint32_t v=q[i]; if(restart&&v==0xFFFFFFFFu) continue; if(v<lo)lo=v; if(v>hi)hi=v; } }
    else    { const uint16_t*q=(const uint16_t*)p; for(uint32_t i=0;i<cnt;i++){ uint32_t v=q[i]; if(restart&&v==0xFFFFu) continue; if(v<lo)lo=v; if(v>hi)hi=v; } }
    if(lo>hi) return 0; *mn=lo; *mx=hi; return 1; }
// PELOAD_D3DGL_PROF: time spent inside the GL draw translation vs wall clock, logged every 2 s from
// d3dgl_present -> tells whether frame time goes to OUR GL path or to the engine waiting on something.
static void d3dgl_draw_impl(uint32_t vcount,int indexed,uint32_t instcount,uint32_t start,int32_t base,uint32_t sinst);
static uint64_t g_prof_ns, g_prof_n, g_prof_pres_ns, g_prof_pres_n, g_prof_wall0;
static uint64_t g_ph_ns[10], g_ph_t;
static int prof_on(void);
static struct { void*rt; uint32_t w,h,fmt; uint64_t geo,fs; uint32_t t0w,t0h,t0f; } g_rtc[48]; static int g_nrtc;
static void rtc_note(void*rt,uint32_t w,uint32_t h,uint32_t fmt,int geo,D3DObj*t0){ if(!prof_on()) return;
    int i=0; for(;i<g_nrtc;i++) if(g_rtc[i].rt==rt) break; if(i==g_nrtc){ if(g_nrtc>=48) return; g_nrtc++; g_rtc[i].rt=rt; g_rtc[i].w=w; g_rtc[i].h=h; g_rtc[i].fmt=fmt; g_rtc[i].geo=g_rtc[i].fs=0; }
    if(geo) g_rtc[i].geo++; else g_rtc[i].fs++; if(t0){ g_rtc[i].t0w=t0->w; g_rtc[i].t0h=t0->h; g_rtc[i].t0f=t0->fmt; } }
#define PH(i) do{ if(prof_on()){ uint64_t _n=prof_now(); g_ph_ns[i]+=_n-g_ph_t; g_ph_t=_n; } }while(0)
static uint64_t prof_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec; }
static int prof_on(void){ static int p=-1; if(p<0) p=getenv("PELOAD_D3DGL_PROF")!=0; return p; }
static void d3dgl_draw(uint32_t vcount,int indexed,uint32_t instcount,uint32_t start,int32_t base,uint32_t sinst){
    GLLOCK();
    // PELOAD_D3DGL_SLOWDRAW=<ms>: glFinish after every draw; a draw whose GPU work exceeds <ms> (e.g. the
    // shader behind a "Caused GPU Hang" error) is logged with its VS/PS and both GLSL sources are dumped.
    static int sd=-1; if(sd<0) sd=getenv("PELOAD_D3DGL_SLOWDRAW")?atoi(getenv("PELOAD_D3DGL_SLOWDRAW")):0;
    uint64_t t0=sd?prof_now():0;
    if(!prof_on()) d3dgl_draw_impl(vcount,indexed,instcount,start,base,sinst);
    else { uint64_t t=prof_now(); d3dgl_draw_impl(vcount,indexed,instcount,start,base,sinst); g_prof_ns+=prof_now()-t; g_prof_n++; }
    if(sd && G.Finish){ G.Finish(); uint64_t dt=(prof_now()-t0)/1000000; if(dt>=(uint64_t)sd){ static int ns; D3DObj*vs=g_d3dst.vs,*ps=g_d3dst.ps; D3DObj*rt=g_d3dst.rtv;
        char m[300]; int k=snprintf(m,sizeof m,"  [slowdraw #%d] %llums n=%u inst=%u idx=%d vs=%p ps=%p rt=%ux%u f%u t0=%ux%u f%u\n",ns,(unsigned long long)dt,vcount,instcount,indexed,(void*)vs,(void*)ps,
            rt?rt->w:0,rt?rt->h:0,rt?rt->fmt:0,g_d3dst.psrv[0]?g_d3dst.psrv[0]->w:0,g_d3dst.psrv[0]?g_d3dst.psrv[0]->h:0,g_d3dst.psrv[0]?g_d3dst.psrv[0]->fmt:0); (void)!write(2,m,k);
        if(ns<6){ const char*home=getenv("HOME"); char pth[300];
            if(vs&&vs->glsl){ snprintf(pth,sizeof pth,"%s/reclaimer_slow_%d_vs.glsl",home?home:"/tmp",ns); FILE*f=fopen(pth,"w"); if(f){ fputs(vs->glsl,f); fclose(f);} }
            if(ps&&ps->glsl){ snprintf(pth,sizeof pth,"%s/reclaimer_slow_%d_ps.glsl",home?home:"/tmp",ns); FILE*f=fopen(pth,"w"); if(f){ fputs(ps->glsl,f); fclose(f);} } }
        ns++; } }
    GLUNLOCK(); }
static void prof_report(void){ if(!prof_on()) return; uint64_t now=prof_now(); if(!g_prof_wall0){ g_prof_wall0=now; return; }
    uint64_t w=now-g_prof_wall0; if(w<2000000000ull) return;
    char m[220]; int k=snprintf(m,sizeof m,"  [prof] %.2fs wall: draws=%llu in-draw=%.0fms presents=%llu in-present=%.0fms (%.1f fps)\n",
        w/1e9,(unsigned long long)g_prof_n,g_prof_ns/1e6,(unsigned long long)g_prof_pres_n,g_prof_pres_ns/1e6,g_prof_pres_n/(w/1e9));
    (void)!write(2,m,k);
    { char b3[160]; int j3=snprintf(b3,sizeof b3,"  [prof] cb ring: %llu puts %.1f MB, %llu ubo binds\n",(unsigned long long)g_cbring_puts,g_cbring_bytes/1048576.0,(unsigned long long)g_cbbind_calls); (void)!write(2,b3,j3); g_cbring_puts=g_cbring_bytes=g_cbbind_calls=0; }
    { char b2[200]; int j2=snprintf(b2,sizeof b2,"  [prof] vtx detail ms: vb-upload=%.0f ib-upload=%.0f idx-scan=%.0f ring=%.0f | uploads=%llu (%.1f MB)\n",g_vx_ns[0]/1e6,g_vx_ns[1]/1e6,g_vx_ns[2]/1e6,g_vx_ns[3]/1e6,(unsigned long long)g_vx_calls,g_vx_bytes/1048576.0);
      (void)!write(2,b2,j2); g_vx_ns[0]=g_vx_ns[1]=g_vx_ns[2]=g_vx_ns[3]=0; g_vx_bytes=g_vx_calls=0; }
    { static const char*nm[10]={"prog","rt","pipe","cbs","texs","vtx","draw","tail","",""}; char b[300]; int j=snprintf(b,sizeof b,"  [prof] split ms:");
      for(int i=0;i<8;i++){ j+=snprintf(b+j,sizeof b-j," %s=%.0f",nm[i],g_ph_ns[i]/1e6); g_ph_ns[i]=0; } b[j++]='\n'; (void)!write(2,b,j); }
    if(getenv("PELOAD_D3DGL_RTCENSUS")){ for(int i=0;i<g_nrtc;i++){ char b[200]; int j=snprintf(b,sizeof b,"  [rtc] rt=%p %ux%u f%u geo=%llu fs=%llu last-t0=%ux%u f%u\n",g_rtc[i].rt,g_rtc[i].w,g_rtc[i].h,g_rtc[i].fmt,(unsigned long long)g_rtc[i].geo,(unsigned long long)g_rtc[i].fs,g_rtc[i].t0w,g_rtc[i].t0h,g_rtc[i].t0f); (void)!write(2,b,j); } g_nrtc=0; }
    g_prof_ns=g_prof_n=g_prof_pres_ns=g_prof_pres_n=0; g_prof_wall0=now; }
// empty pixel shader for depth-only passes (D3D allows PS=NULL; GL needs a fragment stage in a mono program)
static int g_nullps_color;   // this draw has a NULL PS but colour targets bound -> mask colour writes
static D3DObj* d3dgl_null_ps(void){ static D3DObj o; if(!o.glsl){ o.stype=1; o.glsl=strdup("#version 410 core\n// stage=ps\nvoid main(){}\n"); } return &o; }
static struct { unsigned buf; unsigned comps, type; unsigned char norm; uint32_t stride; uint64_t off; unsigned div; int ipt; } g_va[16];
static uint32_t g_va_en; static int g_va_nocache=-1;
// PELOAD_D3DGL_COVQ: per-draw coverage probe. Every draw into a >=1280-wide colour target runs inside a GL
// SAMPLES_PASSED query (skipped while a game occlusion query is recording); two frames later the results
// are read and draws covering > PELOAD_D3DGL_COVQ_FRAC (default 0.35) of the target are logged with their
// shaders and constant-buffer offsets -> finds the draw behind one-frame "exploded geometry" glitches.
// PELOAD_D3DGL_WHITEDUMP: flight recorder for one-frame HDR blow-outs. Every draw of the current and the
// previous frame is kept in memory; when a 1x1 luminance target (auto exposure) receives a value > 0 while
// the previous one was < -1 (normal log-luminance is about -8..-3), both frames' draw lists are written to
// ~/reclaimer_whitedump_<frame>.txt for diffing.
typedef struct { uint64_t vsh,psh; void*rt; uint32_t n,inst,w,h,fmt; void*blend; float psmax,vsmax; uint32_t pscs; uint32_t pss[4],vss[4]; uint32_t tw[3],th[3],tf[3]; void*tp[3]; } FRec;
#define FREC_N 8192
static FRec g_fr[2][FREC_N]; static int g_frn[2], g_frc; static unsigned long g_fr_frame[2];
static int g_wd_dump; static float g_wd_last=-10;
static int wd_on(void){ static int on=-1; if(on<0) on=(getenv("PELOAD_D3DGL_WHITEDUMP")||getenv("PELOAD_D3DGL_DUMPFRAME_AT"))?1:0; return on; }
static float cb_maxabs(D3DObj*c,uint32_t first,int nvec,uint32_t*cs){ if(!c||!c->data||nvec<=0) return 0; uint64_t o0=(uint64_t)first*16,o1=o0+(uint64_t)nvec*16; if(o1>c->size)o1=c->size; float m=0;
    for(uint64_t q=o0;q+4<=o1;q+=4){ float f; uint32_t u; memcpy(&f,(const uint8_t*)c->data+q,4); memcpy(&u,&f,4); if(cs)*cs=*cs*31+u; if(((u>>23)&0xff)==0xff){ m=1e30f; continue; } if(fabsf(f)>m)m=fabsf(f); } return m; }
static void wd_rec(D3DObj*vs,D3DObj*ps,uint32_t n,uint32_t inst,void*mpv){ if(!wd_on()) return;
    extern unsigned long g_eng_present_no_x; int b=g_frc; if(g_fr_frame[b]!=g_eng_present_no_x){ g_fr_frame[b]=g_eng_present_no_x; g_frn[b]=0; }
    if(g_frn[b]>=FREC_N) return; FRec*r=&g_fr[b][g_frn[b]++]; memset(r,0,sizeof *r);
    D3DObj*rt=g_d3dst.rtv; D3DObj*rr=(rt&&rt->data&&rt->vkind)?(D3DObj*)rt->data:rt;
    r->vsh=vs->bchash; r->psh=ps->bchash; r->rt=rr; r->n=n; r->inst=inst; r->w=rr?rr->w:0; r->h=rr?rr->h:0; r->fmt=rr?rr->fmt:0; r->blend=g_d3dst.blend;
    for(int t=0;t<3;t++){ D3DObj*x=g_d3dst.psrv[t]; D3DObj*xr=(x&&x->data&&x->vkind)?(D3DObj*)x->data:x; r->tp[t]=xr; r->tw[t]=xr?xr->w:0; r->th[t]=xr?xr->h:0; r->tf[t]=xr?xr->fmt:0; }
    const int*pb=(const int*)mpv; (void)pb;
    for(int k=0;k<4;k++){ float a=cb_maxabs(g_d3dst.pscb[k],g_d3dst.pscb_1st[k],64,&r->pscs); if(a>r->psmax) r->psmax=a; float v=cb_maxabs(g_d3dst.vscb[k],g_d3dst.vscb_1st[k],64,0); if(v>r->vsmax) r->vsmax=v; } }
static void wd_present(void){ if(!wd_on()) return;
    { static int done; const char*at=getenv("PELOAD_D3DGL_DUMPFRAME_AT"); extern double g_gameplay_t0;
      if(at && !done && g_gameplay_t0 && mono_s()-g_gameplay_t0>=atof(at)){ done=1; g_wd_dump=1; } }
    if(g_wd_dump){ g_wd_dump=0; extern unsigned long g_eng_present_no_x; const char*h=getenv("HOME"); char p[300]; snprintf(p,sizeof p,"%s/reclaimer_whitedump_%lu.txt",h?h:"/tmp",g_eng_present_no_x);
        FILE*f=fopen(p,"w"); if(f){ for(int pass=0;pass<2;pass++){ int b=pass==0?(g_frc^1):g_frc; fprintf(f,"== frame %lu (%s) %d draws\n",g_fr_frame[b],pass==0?"prev":"WHITE",g_frn[b]);
                for(int i=0;i<g_frn[b];i++){ FRec*r=&g_fr[b][i]; fprintf(f,"%d vsh=%016llx psh=%016llx rt=%p %ux%u f%u n=%u inst=%u blend=%p psmax=%g vsmax=%g pscs=%08x t0=%p %ux%u f%u t1=%p %ux%u f%u t2=%p %ux%u f%u\n",i,(unsigned long long)r->vsh,(unsigned long long)r->psh,r->rt,r->w,r->h,r->fmt,r->n,r->inst,r->blend,r->psmax,r->vsmax,r->pscs,
                    r->tp[0],r->tw[0],r->th[0],r->tf[0],r->tp[1],r->tw[1],r->th[1],r->tf[1],r->tp[2],r->tw[2],r->th[2],r->tf[2]); } }
            fclose(f); char m[340]; int k=snprintf(m,sizeof m,"  [whitedump] wrote %s\n",p); (void)!write(2,m,k); } }
    g_frc^=1; g_frn[g_frc]=0; }
static void wd_lum(float v){ if(!wd_on()) return; if(v>0 && g_wd_last<-1) g_wd_dump=1; g_wd_last=v; }
#define COVQ_N 6144
static struct { unsigned q; unsigned long frame; void*vs,*ps; uint32_t n,inst,w,h; uint32_t c1st[4]; void*cb[4]; } g_cq[3][COVQ_N];
static int g_cq_n[3]; static unsigned long g_cq_frame[3];
static int covq_on(void){ static int on=-1; if(on<0) on=getenv("PELOAD_D3DGL_COVQ")?1:0; return on; }
static int covq_begin(D3DObj*vs,D3DObj*ps,uint32_t n,uint32_t inst){
    if(!covq_on()||g_q_active||!G.BeginQuery) return -1;
    D3DObj*rt=g_d3dst.rtv; D3DObj*rr=(rt&&rt->data&&rt->vkind)?(D3DObj*)rt->data:rt; if(!rr||(rr->w<1280&&!getenv("PELOAD_D3DGL_HUGEPROBE"))) return -1;
    extern unsigned long g_eng_present_no_x; int b=(int)(g_eng_present_no_x%3);
    if(g_cq_frame[b]!=g_eng_present_no_x){ g_cq_frame[b]=g_eng_present_no_x; g_cq_n[b]=0; }
    int i=g_cq_n[b]; if(i>=COVQ_N) return -1; g_cq_n[b]++;
    if(!g_cq[b][i].q) G.GenQueries(1,&g_cq[b][i].q);
    g_cq[b][i].frame=g_eng_present_no_x; g_cq[b][i].vs=vs; g_cq[b][i].ps=ps; g_cq[b][i].n=n; g_cq[b][i].inst=inst; g_cq[b][i].w=rr->w; g_cq[b][i].h=rr->h;
    for(int c=0;c<4;c++){ g_cq[b][i].c1st[c]=g_d3dst.vscb_1st[c]; g_cq[b][i].cb[c]=g_d3dst.vscb[c]; }
    G.BeginQuery(0x8914,g_cq[b][i].q); return b*COVQ_N+i; }
static void covq_end(int id){ (void)id; G.EndQuery(0x8914); }
static void covq_harvest(void){   // at Present: read the frame recorded two presents ago
    if(!covq_on()||!G.GetQueryObjectui64v) return;
    extern unsigned long g_eng_present_no_x; int b=(int)((g_eng_present_no_x+1)%3);   // oldest slot
    if(!g_cq_n[b]) return;
    static double frac=-1; if(frac<0){ const char*e=getenv("PELOAD_D3DGL_COVQ_FRAC"); frac=e?atof(e):0.35; }
    for(int i=0;i<g_cq_n[b];i++){ uint64_t r=0; G.GetQueryObjectui64v(g_cq[b][i].q,0x8866,&r);
        double area=(double)g_cq[b][i].w*g_cq[b][i].h; if(r<area*frac) continue;
        if(frac>=0.01 && g_cq[b][i].n<=6 && g_cq[b][i].inst<=1) continue;          // fullscreen passes
        char m[340]; int k=snprintf(m,sizeof m,"  [covq] f%lu draw#%d cov %.2f vsh=%016llx psh=%016llx vs=%p ps=%p n=%u inst=%u rt=%ux%u cb0=%p@%u cb1=%p@%u cb2=%p@%u cb3=%p@%u\n",
            g_cq[b][i].frame,i,r/area,(unsigned long long)((D3DObj*)g_cq[b][i].vs)->bchash,(unsigned long long)((D3DObj*)g_cq[b][i].ps)->bchash,g_cq[b][i].vs,g_cq[b][i].ps,g_cq[b][i].n,g_cq[b][i].inst,g_cq[b][i].w,g_cq[b][i].h,
            g_cq[b][i].cb[0],g_cq[b][i].c1st[0],g_cq[b][i].cb[1],g_cq[b][i].c1st[1],g_cq[b][i].cb[2],g_cq[b][i].c1st[2],g_cq[b][i].cb[3],g_cq[b][i].c1st[3]); (void)!write(2,m,k); }
    g_cq_n[b]=0; }
static void d3dgl_draw_impl(uint32_t vcount,int indexed,uint32_t instcount,uint32_t start,int32_t base,uint32_t sinst){
    if(prof_on()) g_ph_t=prof_now();
    if(!d3dgl_ensure()) return;
    if(g_d3dgl_max_draws && g_d3dgl_frame_draws>=g_d3dgl_max_draws) return;
    d3dgl_ring_reserve(); g_nullps_color=0;
    // PELOAD_TRACE_D3DGL: tally WHY draws are skipped (so we can see what state is missing)
    static unsigned long c_tot,c_novsps,c_noglsl,c_noprog,c_exec;
    c_tot++;
    D3DObj*vs=g_d3dst.vs,*ps=g_d3dst.ps;
    if(g_d3dgl_trace && (c_tot%100000)==0){ char m[160]; int k=snprintf(m,sizeof m,"  [d3dgl] draws tot=%lu exec=%lu skip(novsps=%lu noglsl=%lu noprog=%lu)\n",c_tot,c_exec,c_novsps,c_noglsl,c_noprog); (void)!write(2,m,k); }
    { D3DObj*rr=g_d3dst.rtv; int tobb=(!rr||rr==g_d3d_backbuf);
      if(!ps && vs && !g_d3dst.rtv && g_d3dst.dsv && !getenv("PELOAD_D3DGL_NODEPTH")) ps=d3dgl_null_ps();   // depth-only pass: no PS
      // NULL PS with colour targets bound: D3D still runs depth/stencil test+write, colour is left untouched
      // (stencil-marking / depth-only object passes). Was skipped outright.
      if(!ps && vs && g_d3dst.rtv && g_d3dst.dsv && !getenv("PELOAD_D3DGL_NODEPTH") && !getenv("PELOAD_D3DGL_SKIPNULLPS")){ ps=d3dgl_null_ps(); g_nullps_color=1; }
      if((!vs||!ps) && getenv("PELOAD_TRACE_NOVSPS")){ static int nn; if(nn++<60){ D3DObj*r0=g_d3dst.rtv; D3DObj*rr0=(r0&&r0->data&&r0->vkind)?(D3DObj*)r0->data:r0; D3DObj*t0=g_d3dst.psrv[0]; D3DObj*tt0=(t0&&t0->data&&t0->vkind)?(D3DObj*)t0->data:t0;
          uint64_t ra=(uint64_t)__builtin_return_address(1);
          char m[260]; int k=snprintf(m,sizeof m,"  [novsps] vs=%p ps=%p rt=%ux%u f%u dsv=%p n=%u inst=%u topo=%u t0=%ux%u f%u ra=0x%llx\n",(void*)vs,(void*)ps,rr0?rr0->w:0,rr0?rr0->h:0,rr0?rr0->fmt:0,(void*)g_d3dst.dsv,vcount,instcount,g_d3dst.topo,tt0?tt0->w:0,tt0?tt0->h:0,tt0?tt0->fmt:0,(unsigned long long)ra); (void)!write(2,m,k); } }
      if(!vs||!ps){ c_novsps++; g_fs.novsps++; if(tobb){ g_bb_skip++; g_bb_skip_reason=1;
          if(getenv("PELOAD_TRACE_COPY")){ static int sk; if(sk++<10){ D3DObj*il0=g_d3dst.il; D3DObj*t0=g_d3dst.psrv[0]; char m[300];
              int k=snprintf(m,sizeof m,"  [bbskip] vs=%p(bc=%u glsl=%d) ps=%p(bc=%u glsl=%d) n=%u idx=%d topo=%u il=%d vb0=%lluB stride=%u t0=%ux%u f%u vp=%dx%d\n",
                  (void*)vs,vs?vs->bclen:0,vs&&vs->glsl?1:0,(void*)ps,ps?ps->bclen:0,ps&&ps->glsl?1:0,vcount,indexed,g_d3dst.topo,il0?il0->nelem:-1,
                  g_d3dst.vb[0]?(unsigned long long)g_d3dst.vb[0]->size:0ull,g_d3dst.vstride[0],t0?t0->w:0,t0?t0->h:0,t0?t0->fmt:0,g_d3dst.vp_w,g_d3dst.vp_h); (void)!write(2,m,k);} } }
          return; }
      if((!vs->glsl&&!vs->glprog)||(!ps->glsl&&!ps->glprog)){ c_noglsl++; g_fs.noglsl++; if(tobb){ g_bb_skip++; g_bb_skip_reason=2+((!vs->glsl&&!vs->glprog)?0:1); } return; } }   // need both stages translated
    g_d3dgl_phase_draw=c_tot; g_d3dgl_phase="prog";
    // Default to the monolithic (VS+FS linked) program path; PELOAD_D3DGL_SEP forces the old separable
    // program-pipeline path (which Apple GL-on-Metal fails to build -> all-SW fallback). See d3dgl_monoprog.
    int mono=!getenv("PELOAD_D3DGL_SEP");
    unsigned vsp=0,psp=0; MonoProg*mp=0;
    if(mono){ mp=d3dgl_monoprog(vs->canon?vs->canon:vs,ps->canon?ps->canon:ps); if(!mp||!mp->prog){ c_noprog++; g_fs.noprog++; return; } }
    else { vsp=d3dgl_prog(vs,GLE_VS); psp=d3dgl_prog(ps,GLE_FS); if(!vsp||!psp){ c_noprog++; return; } }
    c_exec++; g_fs.exec++;
    // render target (default to the swapchain backbuffer)
    PH(0); g_d3dgl_phase="rt";
    // No render target bound = a depth-only pass (shadow maps, occlusion): D3D discards colour. The old
    // "fall back to the backbuffer" scribbled every shadow-caster draw over the swap-chain image.
    D3DObj*rt=g_d3dst.rtv; D3DObj*dsv=getenv("PELOAD_D3DGL_NODEPTH")?0:g_d3dst.dsv;
    if(!rt && !dsv){ if(!getenv("PELOAD_D3DGL_NULLRT_BB")) return; rt=swap_backbuf(); }
    if(!rt && getenv("PELOAD_D3DGL_NODEPTHONLY")) return;      // A/B: skip depth-only (shadow/occlusion) passes
    { D3DObj*one[1]={rt}; unsigned fbo = (g_d3dst.num_rt>1&&rt) ? d3dgl_fbo_for(g_d3dst.rtvs,g_d3dst.num_rt,dsv) : d3dgl_fbo_for(rt?one:0,rt?1:0,dsv);
      { static unsigned lastfbo; if(fbo!=lastfbo){ g_fs.fbochg++; lastfbo=fbo; } }
      G.BindFramebuffer(GLE_FBO,fbo); }
    { D3DObj*v0=g_d3dst.rtvv[0]; int enc = rt && rt->glsrgb && dxgi_is_srgb(v0?v0->vfmt:rt->fmt);   // _SRGB RTV: encode on write
      gs_cap(0x8DB9,enc); }
    if(!rt) rt=dsv;                                            // depth-only: size/viewport from the depth target
    int vw=g_d3dst.vp_w>0?g_d3dst.vp_w:(rt->w?(int)rt->w:g_d3dgl_W); if(vw>4096)vw=4096;
    int vh=g_d3dst.vp_h>0?g_d3dst.vp_h:(rt->h?(int)rt->h:g_d3dgl_H); if(vh>4096)vh=4096;
    // PELOAD_D3DGL_RTTEST: paint every RT a real draw binds a distinctive magenta. If the RT dump then
    // shows magenta, the FBO-bind + capture pipeline is sound and the black frame is purely geometry
    // projection (bad cb0); if it stays black, the FBO/capture path itself is broken.
    if(getenv("PELOAD_D3DGL_RTTEST") && G.ClearColor){ G.ClearColor(1.0f,0.0f,1.0f,1.0f); G.Clear(0x4000/*COLOR_BUFFER_BIT*/); }
    // D3D11 blend / rasterizer / viewport state (was: everything forced off + viewport origin ignored)
    d3dgl_apply_state(rt,vw,vh); rt->last_vpw=vw; rt->last_vph=vh;
    // PELOAD_HITBOXES: the world camera = VS cb0 rows 0-3 (view-projection) + row 7 (camera position) of the first
    // full-size scene draw of the frame (before first-person weapon draws, which use their own projection).
    if(!g_hb_taken && hb_enabled() && g_d3dst.rtv && vw>=640 && vh>=360){ D3DObj*c=g_d3dst.vscb[0];
        if(c && c->data){ uint64_t off=(uint64_t)g_d3dst.vscb_1st[0]*16; if(c->size>=off+128){ const float*f=(const float*)((uint8_t*)c->data+off);
            if(hb_is_world_camera(f)){ hb_capture_view(f); g_hb_taken=1; } } } }
    if(getenv("PELOAD_TRACE_VP") && rt->w>=2048){ static uint64_t seen[256]; static int ns; uint64_t key=((uint64_t)(uintptr_t)rt*31+(uint64_t)vw*7919+(uint64_t)vh)^ps->bchash; int f=0; for(int q=0;q<ns;q++) if(seen[q]==key){ f=1; break; } if(!f && ns<256){ seen[ns++]=key; char m[160]; int k=snprintf(m,sizeof m,"  [vp] rt=%p %ux%u f%u vp=%dx%d at %d,%d n=%u psh=%016llx\n",(void*)rt,rt->w,rt->h,rt->fmt,vw,vh,g_d3dst.vp_x,g_d3dst.vp_y,vcount,(unsigned long long)ps->bchash); (void)!write(2,m,k); } }
    if(g_nullps_color){ g_nullps_color=0; if(G.ColorMask) gs_colormask(0,0,0,0); }   // NULL PS: no colour writes
    // pipeline
    PH(1); g_d3dgl_phase="pipeline";
    if(mono){ G.BindProgramPipeline(0); G.UseProgram(mp->prog); }
    else { G.UseProgram(0); G.BindProgramPipeline(g_d3dgl_pipe);
        G.UseProgramStages(g_d3dgl_pipe,GLE_VSBIT,vsp); G.UseProgramStages(g_d3dgl_pipe,GLE_FSBIT,psp); }
    PH(2); g_d3dgl_phase="cbs";
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
    // PER-DRAW decompress gate: the VS runs its vertex-decompress branch when cb0[12]!=0. synth_cb0 sets it
    // =1 globally for DECOMP, but only COMPRESSED-position draws (R16 UNORM/SNORM, fmt 11/13) are actually
    // packed; forcing it on UNCOMPRESSED (float32, fmt 2/6) object draws re-scales already-world verts into
    // the radiating "streaks". Gate it by THIS draw's position format. (Default on with DECOMP; PELOAD_D3DGL_
    // DECOMP_ALL keeps the old force-everything behavior.)
    if(getenv("PELOAD_D3DGL_DECOMP") && vscb_eff[0]==&g_syn_cb0 && g_d3dst.il && g_d3dst.il->nelem && !getenv("PELOAD_D3DGL_DECOMP_ALL")){
        uint32_t pfmt=g_d3dst.il->elem[0].fmt; int compressed=(pfmt==11||pfmt==13||pfmt==35);
        float*c0=(float*)g_syn_cb0_buf; float g=compressed?1.0f:0.0f; c0[48]=c0[49]=c0[50]=c0[51]=g; }
    // PELOAD_D3DGL_DECOMP: the terrain BSP vertices are 16-bit COMPRESSED to [-1,1]; the VS if-branch
    // (cb0[12]!=0) decompresses them: r0 = v0*cb1[0]+cb1[1], then *cb12 (world). cb1 (per-mesh bounds) is
    // NEVER bound (dead material path) -> without it the raw [-1,1] verts collapse the map into a cube.
    // Synthesize cb1 with a uniform scale (PELOAD_D3DGL_DSCALE, offset via _DOFF) so the sections spread
    // out via their bound cb12 placement -> the map LAYOUT emerges even without exact per-mesh bounds.
    // (synth_cb0 sets cb0[12]=1 when DECOMP so the if-branch runs.)
    if(getenv("PELOAD_D3DGL_DECOMP") && vs){
        // PELOAD_D3DGL_REALCB1: use the most-recently-mapped small CB (g_last_smallcb) as the real per-mesh
        // decompression cb1 (the engine maps it right before each draw, even though the BIND is on the dead
        // material path) -> correct per-section scale/offset = no stretched-spike sections. Falls back to
        // the synthetic uniform scale (PELOAD_D3DGL_DSCALE) when no candidate exists.
        // PELOAD_D3DGL_CAPCB1: bind the REAL per-section decompression captured at halo3+0x28e8e0 (the
        // engine's own scale/offset/uv) instead of the uniform DSCALE guess -> correct per-section bounds,
        // no stretched "spike" sections. PELOAD_D3DGL_CAPSNORM halves the extent + centers the offset for
        // SNORM-packed positions ([-1,1]); PELOAD_D3DGL_CAPSCALE multiplies the position scale (debug).
        if(getenv("PELOAD_D3DGL_CAPCB1") && g_decomp_cap_valid){
            float lc[12]; for(int i=0;i<12;i++) lc[i]=g_decomp_cap[i];   // snapshot (avoid torn read)
            float*c1=(float*)g_syn_cb1_buf; memset(c1,0,256);
            float sm=getenv("PELOAD_D3DGL_CAPSCALE")?atof(getenv("PELOAD_D3DGL_CAPSCALE")):1.0f;
            if(getenv("PELOAD_D3DGL_CAPSNORM")){ // v0 in [-1,1]: decompressed = (v0*0.5+0.5)*ext+min
                c1[0]=lc[0]*0.5f*sm; c1[1]=lc[1]*0.5f*sm; c1[2]=lc[2]*0.5f*sm; c1[3]=1.0f;
                c1[4]=lc[4]+lc[0]*0.5f; c1[5]=lc[5]+lc[1]*0.5f; c1[6]=lc[6]+lc[2]*0.5f;
            } else {                               // v0 in [0,1]: decompressed = v0*ext+min
                c1[0]=lc[0]*sm; c1[1]=lc[1]*sm; c1[2]=lc[2]*sm; c1[3]=1.0f;
                c1[4]=lc[4]; c1[5]=lc[5]; c1[6]=lc[6];
            }
            c1[8]=lc[8]; c1[9]=lc[9]; c1[10]=lc[10]; c1[11]=lc[11];     // texcoord scale+offset
            g_syn_cb1.data=g_syn_cb1_buf; g_syn_cb1.size=256; g_syn_cb1.is_cb=1; g_syn_cb1.ver++;
            if(!vscb_eff[1]||!vscb_eff[1]->data) vscb_eff[1]=&g_syn_cb1;
        } else if(getenv("PELOAD_D3DGL_REALCB1") && g_last_smallcb && g_last_smallcb->data && g_last_smallcb->size>=48){
            if(!vscb_eff[1]||!vscb_eff[1]->data) vscb_eff[1]=g_last_smallcb;
            if(getenv("PELOAD_D3DGL_CAMDBG")){ static int rc; if(rc++<6){ const float*f=(const float*)g_last_smallcb->data; char mm[200]; int k=snprintf(mm,sizeof mm,"  [realcb1] %lluB scale=[%.2f %.2f %.2f] off=[%.2f %.2f %.2f] tc=[%.2f %.2f %.2f %.2f]\n",(unsigned long long)g_last_smallcb->size,f[0],f[1],f[2],f[4],f[5],f[6],f[8],f[9],f[10],f[11]); (void)!write(2,mm,k);} }
        } else {
            float*c1=(float*)g_syn_cb1_buf; memset(c1,0,256);
            float S=getenv("PELOAD_D3DGL_DSCALE")?atof(getenv("PELOAD_D3DGL_DSCALE")):10.0f;
            float O=getenv("PELOAD_D3DGL_DOFF")?atof(getenv("PELOAD_D3DGL_DOFF")):0.0f;
            c1[0]=c1[1]=c1[2]=S;             // cb1[0] = position scale
            c1[4]=c1[5]=c1[6]=O;             // cb1[1] = position offset
            c1[8]=c1[9]=1.0f;                // cb1[2].xy = texcoord scale (passthrough); .zw offset = 0
            g_syn_cb1.data=g_syn_cb1_buf; g_syn_cb1.size=256; g_syn_cb1.is_cb=1; g_syn_cb1.ver++;
            if(!vscb_eff[1]||!vscb_eff[1]->data) vscb_eff[1]=&g_syn_cb1; } }
    if(mono && g_dg_ubo && G.BindBufferRange){
        if(getenv("PELOAD_D3DGL_SKIPVSBUF") && (!getenv("PELOAD_D3DGL_ALT5") || (((long)(mono_s()/5.0))&1))){ int hb=0; for(int q=0;q<32;q++) if(mp->vtloc[q]>=0 && mp->vtt[q]==5) hb=1; if(hb) return; }
        { const char*e=getenv("PELOAD_D3DGL_SKIPRELCB"); if(e && (mp->relcb&atoi(e)) && (!getenv("PELOAD_D3DGL_ALT5") || (((long)(mono_s()/5.0))&1))) return; }
        { const char*e=getenv("PELOAD_D3DGL_SKIPVSBLK"); if(e){ int sb=atoi(e), mv=strchr(e,':')?atoi(strchr(e,':')+1):1; if(sb>=0&&sb<16&&mp->vblk[sb]>=mv && (!getenv("PELOAD_D3DGL_ALT5") || (((long)(mono_s()/5.0))&1))){ return; } } }
        // PELOAD_D3DGL_SKIPPSBLK=<slot>:<minvec>: skip draws whose PS declares cb<slot> with >= minvec vec4s (A/B a shader family)
        { const char*e=getenv("PELOAD_D3DGL_SKIPPSBLK"); if(e){ int sb=atoi(e), mv=strchr(e,':')?atoi(strchr(e,':')+1):1; if(sb>=0&&sb<16&&mp->pblk[sb]>=mv && (!getenv("PELOAD_D3DGL_ALT5") || (((long)(mono_s()/5.0))&1))){ return; } } }
        if(getenv("PELOAD_D3DGL_CBCHECK")){ static struct { void*sh; int b; } seen[256]; static int ns;
            for(int b=0;b<16;b++) for(int st=0;st<2;st++){ int used=st?mp->pblk[b]:mp->vblk[b]; D3DObj*c=st?g_d3dst.pscb[b]:vscb_eff[b]; void*sh=st?(void*)ps:(void*)vs;
                if(!used || (c&&c->data&&c->size)) continue; int f=0; for(int i=0;i<ns;i++) if(seen[i].sh==sh&&seen[i].b==b+16*st){ f=1; break; }
                if(!f && ns<256){ seen[ns].sh=sh; seen[ns].b=b+16*st; ns++; char m[200]; int k=snprintf(m,sizeof m,"  [cbcheck] %s cb%d UNBOUND (block %d vec4) sh=%p rt=%ux%u f%u\n",st?"PS":"VS",b,used,sh,g_d3dst.rtv?g_d3dst.rtv->w:0,g_d3dst.rtv?g_d3dst.rtv->h:0,g_d3dst.rtv?g_d3dst.rtv->fmt:0); (void)!write(2,m,k); } } }
        // PELOAD_D3DGL_CBSCAN: flag draws whose VS constants hold NaN/Inf/|x|>1e7 (diagnoses one-frame
        // exploded geometry); logs per engine frame with the frame tag used in the window-capture names.
        { static int cbs=-1; if(cbs<0) cbs=getenv("PELOAD_D3DGL_CBSCAN")?1:0;
          if(cbs){ extern unsigned long g_eng_present_no_x; static unsigned long lastf=~0ul; static int nbad, lines; static char first[200];
            unsigned long fno=g_eng_present_no_x;
            if(fno!=lastf){ if(nbad && lines<600){ char m[300]; int k=snprintf(m,sizeof m,"  [cbscan] frame %lu: %d bad draws; first %s\n",lastf,nbad,first); (void)!write(2,m,k); lines++; } nbad=0; lastf=fno; }
            int bad=0; for(int b=0;b<16 && !bad;b++){ D3DObj*c=vscb_eff[b]; if(!mp->vblk[b]||!c||!c->data) continue;
                uint64_t o0=(uint64_t)g_d3dst.vscb_1st[b]*16, o1=o0+(uint64_t)mp->vblk[b]*16; if(o1>c->size) o1=c->size;
                for(uint64_t q=o0;q+4<=o1;q+=4){ float f; memcpy(&f,(const uint8_t*)c->data+q,4); uint32_t u; memcpy(&u,&f,4);
                    if(((u>>23)&0xff)==0xff || fabsf(f)>1e7f){ if(!nbad) snprintf(first,sizeof first,"vs=%p ps=%p b%d vec%llu.%llu=%08x n=%u",(void*)vs,(void*)ps,b,(unsigned long long)((q-o0)/16),(unsigned long long)((q-o0)%16/4),u,vcount); bad=1; break; } } }
            if(bad) nbad++; } }
        for(int b=0;b<16;b++){ if(mp->vblk[b]) d3dgl_bind_ubo((unsigned)b,vscb_eff[b],g_d3dst.vscb_1st[b],mp->vblk[b]);
                               if(mp->pblk[b]) d3dgl_bind_ubo((unsigned)(16+b),g_d3dst.pscb[b],g_d3dst.pscb_1st[b],mp->pblk[b]); }
        PH(3); g_d3dgl_phase="texs"; d3dgl_bind_texs_p(mp->prog,mp->tloc,mp->tt,&mp->units_set);
        d3dgl_bind_res(mp->prog,mp->vtloc,mp->vtt,&mp->vunits_set,g_d3dst.vssrv,g_d3dst.vssrvv,g_d3dst.vssamp,16,32);   // VS resources: units 16..31
    } else if(mono){
        d3dgl_bind_cbs_p(mp->prog,mp->vcb,vscb_eff,g_d3dst.vscb_1st,g_d3dst.vscb_cnt);
        d3dgl_bind_cbs_p(mp->prog,mp->pcb,g_d3dst.pscb,g_d3dst.pscb_1st,g_d3dst.pscb_cnt);
        PH(3); g_d3dgl_phase="texs"; d3dgl_bind_texs_p(mp->prog,mp->tloc,mp->tt,&mp->units_set);
    } else {
        d3dgl_bind_cbs(vs,vscb_eff,g_d3dst.vscb_1st,g_d3dst.vscb_cnt); d3dgl_bind_cbs(ps,g_d3dst.pscb,g_d3dst.pscb_1st,g_d3dst.pscb_cnt);
        g_d3dgl_phase="texs"; d3dgl_bind_texs(ps); d3dgl_bind_texs(vs);
    }
    int use_ring=0; int64_t vshift=0;   // streaming-ring rebasing (see d3dgl_ring_put)
    PH(4); G.BindVertexArray(g_d3dgl_vao);
    D3DObj*vb=g_d3dst.vb[0]; D3DObj*il=g_d3dst.il;
    int nattr=0; uint32_t amask=0;
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
        // D3D11 input assembler: per-vertex data spans vertices [vmin,vmax] (StartVertex.. for Draw;
        // BaseVertex + [min,max index] over [StartIndex, StartIndex+count) for DrawIndexed -- the index
        // scan only runs when a referenced VB is dynamic); per-instance data (InputSlotClass=1) starts at
        // StartInstance (GL 4.1 has no base-instance, so the attribute pointer is offset instead) and
        // advances every InstanceDataStepRate instances via glVertexAttribDivisor.
        int is32=(g_d3dst.ibfmt==42); int strip=(g_d3dst.topo==5||g_d3dst.topo==3);
        int need_scan=0;
        for(int i=0;i<il->nelem && i<16;i++){ uint32_t sl=il->elem[i].slot; if(sl>=8)sl=0; D3DObj*sb=g_d3dst.vb[sl];
            if(sb&&sb->glbuf&&sb->gl_dirty&&il->elem[i].cls!=1) need_scan=1; }
        uint64_t vmin=start, vmax=vcount?(uint64_t)start+vcount-1:start;
        if(indexed){ uint32_t mn=0,mx=0; vmin=vmax=0;
            uint64_t _ts=prof_on()?prof_now():0;
            if(need_scan){ if(d3dgl_index_range(g_d3dst.ib,(uint64_t)g_d3dst.iboff+(uint64_t)start*(is32?4:2),vcount,is32,strip,&mn,&mx)){
                    int64_t a=(int64_t)base+mn, b2=(int64_t)base+mx; vmin=a<0?0:(uint64_t)a; vmax=b2<0?0:(uint64_t)b2; }
                else need_scan=-1; }
            if(_ts) g_vx_ns[2]+=prof_now()-_ts; }                    // range unknown -> send the whole buffer when dirty
        // ring mode: a dirty dynamic VB feeds this draw and the referenced vertex range is known -> stream the
        // range through the ring and rebase the draw by vmin (static VBs in the same draw shift by +vmin*stride)
        use_ring = (need_scan==1) && !getenv("PELOAD_D3DGL_NORING") && !getenv("PELOAD_D3DGL_NORING_VB") && G.MapBufferRange;
        vshift = use_ring ? (int64_t)vmin : 0;
        long ringoff[8]; for(int q=0;q<8;q++) ringoff[q]=-2;
        for(int i=0;i<il->nelem && i<16;i++){
            uint32_t slot=il->elem[i].slot; if(slot>=8) slot=0; D3DObj*sb=g_d3dst.vb[slot]; if(!sb||!sb->data||!sb->size) continue;
            int loc=d3dgl_vs_inreg(vs,il->elem[i].sem,il->elem[i].semidx);
            if(getenv("PELOAD_D3DGL_IADBG")){ static void* seenvs[256]; static int nsv; int fresh=1; for(int q=0;q<nsv;q++) if(seenvs[q]==(void*)vs){ fresh=0; break; }
                if(fresh && i==il->nelem-1 && nsv<256) seenvs[nsv++]=vs;
                if(fresh){ char m[260]; int k=snprintf(m,sizeof m,"  [iadbg] vs=%p rt=%ux%u f%u el%d %s%u fmt%u slot%u off%u cls%u -> loc %d |",(void*)vs,g_d3dst.rtv?g_d3dst.rtv->w:0,g_d3dst.rtv?g_d3dst.rtv->h:0,g_d3dst.rtv?g_d3dst.rtv->fmt:0,i,il->elem[i].sem,il->elem[i].semidx,il->elem[i].fmt,il->elem[i].slot,il->elem[i].off,il->elem[i].cls,loc);
                    if(vs&&vs->isg){ const VsIn*tt=(const VsIn*)vs->isg; for(int q=0;q<vs->nisg && k<240;q++) k+=snprintf(m+k,sizeof m-k," %s%u=v%u",tt[q].name,tt[q].idx,tt[q].reg); }
                    m[k++]='\n'; (void)!write(2,m,k); } }
            if(loc==-2) loc=i;                 // no parsable VS signature: legacy element-order binding
            if(loc<0||loc>=16) continue;       // the VS doesn't consume this element
            uint64_t stride=g_d3dst.vstride[slot], vo=g_d3dst.voff[slot];
            int inst=(il->elem[i].cls==1); uint32_t step=il->elem[i].step?il->elem[i].step:1;
            uint64_t lo,hi;
            if(inst){ lo=vo+(uint64_t)sinst*stride; hi=lo+((uint64_t)(instcount+step-1)/step)*stride; }
            else if(need_scan<0){ lo=0; hi=sb->size; }
            else { lo=vo+vmin*stride; hi=vo+(vmax+1)*stride; }
            if(!stride){ lo=vo; hi=vo+64; }
            unsigned comps,gt; unsigned char norm; d3dgl_attrib_fmt(il->elem[i].fmt,&comps,&gt,&norm);
            uint64_t aoff=vo+il->elem[i].off+(inst?(uint64_t)sinst*stride:0);
            int ringed=0;
            if(use_ring && !inst && stride){
                int covered = sb->glbuf && (!sb->gl_dirty || (sb->rvalid && lo>=sb->rlo && hi<=sb->rhi));
                if(!covered && sb->gl_dirty && hi<=sb->size && hi>lo && hi-lo<=(8u<<20)){
                    if(ringoff[slot]==-2){ uint64_t _t=prof_on()?prof_now():0; cs_wait_res(sb); ringoff[slot]=d3dgl_ring_put((const uint8_t*)sb->data+lo,hi-lo);
                        if(_t){ g_vx_ns[3]+=prof_now()-_t; g_vx_calls++; g_vx_bytes+=hi-lo; } }
                    if(ringoff[slot]>=0){ G.BindBuffer(GLE_ARRAY,g_ring); aoff=(uint64_t)ringoff[slot]+il->elem[i].off; ringed=1; } } }
            if(!ringed){ d3dgl_buf_need(sb,GLE_ARRAY,lo,hi);   // also binds GL_ARRAY_BUFFER for the pointer below
                if(use_ring && !inst) aoff+=(uint64_t)vshift*stride; }
            // attribute state of the shared VAO is cached: only changed pointers/divisors/enables are sent
            { unsigned cb=ringed?g_ring:sb->glbuf; unsigned dv=inst?step:0; int nc=g_va_nocache<0?(g_va_nocache=getenv("PELOAD_D3DGL_NOSTATECACHE")?1:0):g_va_nocache;
              int ipt=0; if((d3dgl_vs_intin(vs)>>loc)&1u && G.VertexAttribIPointer){ unsigned ic,ig; if(d3dgl_attrib_ifmt(il->elem[i].fmt,&ic,&ig)){ comps=ic; gt=ig; norm=0; ipt=1; } }
              if(nc || g_va[loc].buf!=cb||g_va[loc].comps!=comps||g_va[loc].type!=gt||g_va[loc].norm!=norm||g_va[loc].stride!=(uint32_t)stride||g_va[loc].off!=aoff||g_va[loc].ipt!=ipt){
                  if(ipt) G.VertexAttribIPointer((unsigned)loc,(int)comps,gt,(int)stride,(const void*)(size_t)aoff);
                  else G.VertexAttribPointer((unsigned)loc,(int)comps,gt,norm,(int)stride,(const void*)(size_t)aoff);
                  g_va[loc].buf=cb; g_va[loc].comps=comps; g_va[loc].type=gt; g_va[loc].norm=norm; g_va[loc].stride=(uint32_t)stride; g_va[loc].off=aoff; g_va[loc].ipt=ipt; }
              if((nc || g_va[loc].div!=dv) && G.VertexAttribDivisor){ G.VertexAttribDivisor((unsigned)loc,dv); g_va[loc].div=dv; }
              if(nc || !(g_va_en&(1u<<loc))){ G.EnableVertexAttribArray((unsigned)loc); g_va_en|=1u<<loc; } }
            amask|=1u<<loc; nattr++;
        }
        // PELOAD_D3DGL_VTXDUMP: ground-truth the world-space vertex range of element 0 (POSITION) so we
        // can size/aim the camera. Only meaningful for an uncompressed float position (DXGI 2/6).
        if(getenv("PELOAD_D3DGL_VTXDUMP")){ static int vd; if(vd<5){ D3DObj*sb=g_d3dst.vb[il->elem[0].slot<8?il->elem[0].slot:0];
            uint32_t stride=g_d3dst.vstride[il->elem[0].slot<8?il->elem[0].slot:0]; uint32_t off=g_d3dst.voff[0]+il->elem[0].off; uint32_t fmt=il->elem[0].fmt;
            if(sb&&sb->data&&stride>=12&&(fmt==2||fmt==6)){ vd++; const uint8_t*d=(const uint8_t*)sb->data; uint64_t nv=sb->size/stride; if(nv>400)nv=400;
                float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f}; for(uint64_t v=0;v<nv;v++){ const float*p=(const float*)(d+off+v*stride);
                    for(int c=0;c<3;c++){ float val=p[c]; if(val==val){ if(val<mn[c])mn[c]=val; if(val>mx[c])mx[c]=val; } } }
                char m[220]; int k=snprintf(m,sizeof m,"  [vtxdump] fmt=%u stride=%u nvsamp=%llu posX[%.2f..%.2f] Y[%.2f..%.2f] Z[%.2f..%.2f] v0=[%.2f %.2f %.2f]\n",
                    fmt,stride,(unsigned long long)nv,mn[0],mx[0],mn[1],mx[1],mn[2],mx[2],((const float*)(d+off))[0],((const float*)(d+off))[1],((const float*)(d+off))[2]); (void)!write(2,m,k); } } }
        // PELOAD_D3DGL_CLIPDUMP: decode v0's POSITION (any fmt), run it through the SAME transform the VS
        // would (decompress via cb1=g_syn_cb1_buf -> world via cb12=g_last_b12 -> clip via cb0=g_syn_cb0_buf),
        // and log the resulting clip/NDC. ONSCREEN iff NDC in [-1,1] & w>0. Grounds the "geometry off-screen"
        // diagnosis in real numbers instead of guessing the camera. Samples a vertex min/max across the draw.
        if(getenv("PELOAD_D3DGL_CLIPDUMP")){ static int cd2; if(cd2<10 && il->nelem>0){
            int slot0=il->elem[0].slot<8?il->elem[0].slot:0; D3DObj*sb=g_d3dst.vb[slot0];
            uint32_t stride=g_d3dst.vstride[slot0]; uint32_t off=g_d3dst.voff[slot0]+il->elem[0].off; uint32_t fmt=il->elem[0].fmt;
            if(sb&&sb->data&&stride>=4&&(uint64_t)off+16<=sb->size){ cd2++;
                const uint8_t*d=(const uint8_t*)sb->data+off; float v0[3]={0,0,0};
                if(fmt==2||fmt==6||fmt==16||fmt==41){ const float*f=(const float*)d; v0[0]=f[0]; v0[1]=(fmt==41)?0:f[1]; v0[2]=(fmt==2||fmt==6)?f[2]:0; }
                else if(fmt==11){ const uint16_t*u=(const uint16_t*)d; v0[0]=u[0]/65535.0f; v0[1]=u[1]/65535.0f; v0[2]=u[2]/65535.0f; }
                else if(fmt==13){ const int16_t*s=(const int16_t*)d; v0[0]=s[0]/32767.0f; v0[1]=s[1]/32767.0f; v0[2]=s[2]/32767.0f; }
                float dec[3]={v0[0],v0[1],v0[2]};
                if(getenv("PELOAD_D3DGL_DECOMP")){ const float*c1=(const float*)g_syn_cb1_buf; dec[0]=v0[0]*c1[0]+c1[4]; dec[1]=v0[1]*c1[1]+c1[5]; dec[2]=v0[2]*c1[2]+c1[6]; }
                float world[3]={dec[0],dec[1],dec[2]};
                if(g_last_b12&&g_last_b12->data){ const float*w=(const float*)g_last_b12->data; for(int r=0;r<3;r++) world[r]=w[r*4]*dec[0]+w[r*4+1]*dec[1]+w[r*4+2]*dec[2]+w[r*4+3]; }
                const float*c0=(const float*)g_syn_cb0_buf; float clip[4]; for(int r=0;r<4;r++) clip[r]=c0[r*4]*world[0]+c0[r*4+1]*world[1]+c0[r*4+2]*world[2]+c0[r*4+3];
                float iw=clip[3]!=0?1.0f/clip[3]:0; float nx=clip[0]*iw,ny=clip[1]*iw,nz=clip[2]*iw;
                int on=clip[3]>0&&nx>-1&&nx<1&&ny>-1&&ny<1&&nz>-0.1f&&nz<1.1f;
                D3DObj*crt=g_d3dst.rtv; int cb0loc = mono ? (mp?mp->vcb[0]:-1) : (vs?vs->gl_cbloc[0]:-1);
                int synthcb0 = (vscb_eff[0]==&g_syn_cb0);
                char m[420]; int k=snprintf(m,sizeof m,"  [clipdump] fmt=%u v0=[%.3f %.3f %.3f] world=[%.1f %.1f %.1f] clipw=%.1f ndc=[%.2f %.2f %.2f] %s | cb0loc=%d cb0=%s rt=%dx%d(bb=%d)\n",
                    fmt,v0[0],v0[1],v0[2],world[0],world[1],world[2],clip[3],nx,ny,nz,on?"ONSCREEN":"off",
                    cb0loc, synthcb0?"SYNTH":(vscb_eff[0]&&vscb_eff[0]->data?"other":"NONE"),
                    crt?(int)crt->w:0,crt?(int)crt->h:0,crt==g_d3d_backbuf); (void)!write(2,m,k); } } }
        // PELOAD_D3DGL_RANGEDUMP: sample up to 300 verts of a fmt-11/13 draw, decompress each via the bound
        // cb1, and report the world-position RANGE + how many verts land FAR from the median (streak verts).
        // A big intra-draw range / many outliers => the draw mixes sections (one cb1 can't fit) or the
        // compression bounds are wrong for part of it = the radiating streaks.
        if(getenv("PELOAD_D3DGL_RANGEDUMP")){ static int rd; if(rd<400 && il->nelem>0){
            int slot0=il->elem[0].slot<8?il->elem[0].slot:0; D3DObj*sb=g_d3dst.vb[slot0];
            uint32_t stride=g_d3dst.vstride[slot0]; uint32_t off=g_d3dst.voff[slot0]+il->elem[0].off; uint32_t fmt=il->elem[0].fmt;
            if(sb&&sb->data&&stride>=8&&(fmt==11||fmt==13)){ rd++;
                const float*c1=(const float*)g_syn_cb1_buf;
                uint64_t nv=sb->size/stride; if(nv>300)nv=300;
                float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f}; double sum[3]={0,0,0}; int cnt=0;
                for(uint64_t vi=0;vi<nv;vi++){ const uint8_t*d=(const uint8_t*)sb->data+off+vi*stride; float raw[3];
                    if(fmt==11){ const uint16_t*u=(const uint16_t*)d; raw[0]=u[0]/65535.0f;raw[1]=u[1]/65535.0f;raw[2]=u[2]/65535.0f; }
                    else { const int16_t*s=(const int16_t*)d; raw[0]=s[0]/32767.0f;raw[1]=s[1]/32767.0f;raw[2]=s[2]/32767.0f; }
                    float dz[3]; for(int c=0;c<3;c++){ dz[c]=raw[c]*c1[c]+c1[4+c]; if(dz[c]!=dz[c])continue; if(dz[c]<mn[c])mn[c]=dz[c]; if(dz[c]>mx[c])mx[c]=dz[c]; sum[c]+=dz[c]; } cnt++; }
                float med[3]={cnt?(float)(sum[0]/cnt):0,cnt?(float)(sum[1]/cnt):0,cnt?(float)(sum[2]/cnt):0};
                float span[3]={mx[0]-mn[0],mx[1]-mn[1],mx[2]-mn[2]};
                // second pass: count verts FAR from the mean (>150u) = outliers that would streak
                int outl=0; for(uint64_t vi=0;vi<nv;vi++){ const uint8_t*d=(const uint8_t*)sb->data+off+vi*stride; float raw[3];
                    if(fmt==11){ const uint16_t*u=(const uint16_t*)d; raw[0]=u[0]/65535.0f;raw[1]=u[1]/65535.0f;raw[2]=u[2]/65535.0f; }
                    else { const int16_t*s=(const int16_t*)d; raw[0]=s[0]/32767.0f;raw[1]=s[1]/32767.0f;raw[2]=s[2]/32767.0f; }
                    float dx=raw[0]*c1[0]+c1[4]-med[0], dy=raw[1]*c1[1]+c1[5]-med[1], dz=raw[2]*c1[2]+c1[6]-med[2];
                    if(dx*dx+dy*dy+dz*dz > 150.0f*150.0f) outl++; }
                char m2[280]; int k2=snprintf(m2,sizeof m2,"  [rangedump] fmt=%u nv=%llu stride=%u span=[%.0f %.0f %.0f] mean=[%.0f %.0f %.0f] cb1scale=[%.0f %.0f %.0f] outliers=%d/%llu\n",
                    fmt,(unsigned long long)nv,stride,span[0],span[1],span[2],med[0],med[1],med[2],c1[0],c1[1],c1[2],outl,(unsigned long long)nv); (void)!write(2,m2,k2); } } }
    }
    { uint32_t stale=g_va_en&~amask; for(int l=0;stale && l<16;l++) if(stale&(1u<<l)){ G.DisableVertexAttribArray((unsigned)l); g_va_en&=~(1u<<l); stale&=~(1u<<l); } }
    PH(5); g_d3dgl_phase="draw";
    unsigned mode=d3dgl_topo(g_d3dst.topo);
    unsigned n=vcount;               // D3D: a zero count draws nothing
    if(n>8000000) n=8000000;         // guard against a garbage count driving a wild GL read
    if(instcount>65536) instcount=65536;
    // PELOAD_D3DGL_SKIPWIDE=<u>: skip draws whose DECOMPRESSED vertex span exceeds <u> units (default 400).
    // Diagnostic for the streaks: the whole-map terrain meshes (verts spread across the full [-500,500] box)
    // are the streak source; the clean local sections (span ~100) are the ground. Skipping the wide meshes
    // isolates whether they are what streaks (and whether the ground alone renders cleanly).
    if(getenv("PELOAD_D3DGL_SKIPWIDE") && getenv("PELOAD_D3DGL_DECOMP") && g_d3dst.il && g_d3dst.il->nelem){
        float thr=atof(getenv("PELOAD_D3DGL_SKIPWIDE")); if(thr<1)thr=400.0f;
        int slot0=g_d3dst.il->elem[0].slot<8?g_d3dst.il->elem[0].slot:0; D3DObj*sb=g_d3dst.vb[slot0];
        uint32_t st2=g_d3dst.vstride[slot0]; uint32_t vo2=g_d3dst.voff[slot0]+g_d3dst.il->elem[0].off; uint32_t f2=g_d3dst.il->elem[0].fmt;
        if(sb&&sb->data&&st2>=8&&(f2==11||f2==13)){ const float*c1=(const float*)g_syn_cb1_buf;
            float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f}; uint64_t nv=sb->size/st2; if(nv>128)nv=128;
            for(uint64_t vi=0;vi<nv;vi++){ const uint8_t*d=(const uint8_t*)sb->data+vo2+vi*st2; float raw[3];
                if(f2==11){ const uint16_t*u=(const uint16_t*)d; raw[0]=u[0]/65535.0f;raw[1]=u[1]/65535.0f;raw[2]=u[2]/65535.0f; }
                else { const int16_t*s=(const int16_t*)d; raw[0]=s[0]/32767.0f;raw[1]=s[1]/32767.0f;raw[2]=s[2]/32767.0f; }
                for(int c=0;c<3;c++){ float dz=raw[c]*c1[c]+c1[4+c]; if(dz<mn[c])mn[c]=dz; if(dz>mx[c])mx[c]=dz; } }
            if((mx[0]-mn[0])>thr||(mx[1]-mn[1])>thr||(mx[2]-mn[2])>thr) n=0; } }
    // PELOAD_D3DGL_SKIPDEGEN=<t>: skip draws whose bound cb1 position scale has any component < t
    // (default 2.0) -- these are DEGENERATE/mis-correlated captures (cb1scale ~[0,0,0]/[1,1,0]) that
    // collapse or stretch the draw. Diagnostic for whether those are the streak source.
    if(getenv("PELOAD_D3DGL_SKIPDEGEN") && getenv("PELOAD_D3DGL_DECOMP")){
        const float*c1=(const float*)g_syn_cb1_buf; float t=atof(getenv("PELOAD_D3DGL_SKIPDEGEN")); if(t<=0)t=2.0f;
        if(c1[0]<t||c1[1]<t||c1[2]<t) n=0; }
    double fd0=(mp && !mp->drawn && getenv("PELOAD_D3DGL_PROGTIME"))?mono_s():0;
    int covq=covq_begin(vs,ps,vcount,instcount);
    wd_rec(vs,ps,vcount,instcount,mp);
    { const char*tp=getenv("PELOAD_TRACE_PSH"); if(tp && strstr(tp,"x")==0){ char hx[20]; snprintf(hx,sizeof hx,"%016llx",(unsigned long long)ps->bchash);
        if(strstr(tp,hx)){ extern double g_gameplay_t0; static unsigned long lastf; static int cnt; extern unsigned long g_eng_present_no_x;
          if(g_eng_present_no_x!=lastf){ if(cnt){ char m[120]; int k=snprintf(m,sizeof m,"  [psh] f%lu t=%.2f %d draws\n",lastf,g_gameplay_t0?mono_s()-g_gameplay_t0:-1.0,cnt); (void)!write(2,m,k); } cnt=0; lastf=g_eng_present_no_x; }
          cnt++; } } }
    // PELOAD_D3DGL_DUMPTEX_PS=<pshash>: once, read back the PS's t0 (mip 0, RGBA8) to ~/reclaimer_pstex_<hash>.bmp
    { const char*dp=getenv("PELOAD_D3DGL_DUMPTEX_PS"); static int dumped;
      static long seen_n; long skipn=getenv("PELOAD_D3DGL_DUMPTEX_SKIP")?atol(getenv("PELOAD_D3DGL_DUMPTEX_SKIP")):0;
      if(dp && !dumped && ps->bchash==strtoull(dp,0,16) && seen_n++>=skipn){ D3DObj*x=g_d3dst.psrv[0]; D3DObj*xr=(x&&x->data&&x->vkind)?(D3DObj*)x->data:x;
        static void(*gti)(unsigned,int,unsigned,unsigned,void*); if(!gti) gti=dlsym(RTLD_DEFAULT,"glGetTexImage");
        if(xr && xr->gltex && xr->w && xr->h && xr->w<=4096 && xr->h<=4096 && gti){ dumped=1; unsigned char*px=calloc(1,(size_t)xr->w*xr->h*4);
            if(px){ G.BindTexture(xr->gltarget?xr->gltarget:GLE_TEX2D,xr->gltex); TEXCACHE_INVALIDATE(); gti(xr->gltarget?xr->gltarget:GLE_TEX2D,0,GLE_RGBA,GLE_UBYTE,px);
                const char*h=getenv("HOME"); char pth[300]; snprintf(pth,sizeof pth,"%s/reclaimer_pstex_%s.bmp",h?h:"/tmp",dp); bmp_write_rgba(pth,(int)xr->w,(int)xr->h,px);
                for(size_t q=0;q<(size_t)xr->w*xr->h;q++){ unsigned char a=px[q*4+3]; px[q*4]=px[q*4+1]=px[q*4+2]=a; }
                snprintf(pth,sizeof pth,"%s/reclaimer_pstex_%s_alpha.bmp",h?h:"/tmp",dp); bmp_write_rgba(pth,(int)xr->w,(int)xr->h,px); free(px);
                char m[340]; int k=snprintf(m,sizeof m,"  [dumptex] %s t0 %ux%u f%u -> %s\n",dp,xr->w,xr->h,xr->fmt,pth); (void)!write(2,m,k); } } } }
    // PELOAD_D3DGL_FSLOG: log every full-screen-ish pass (<=6 verts, 1 instance) with its PS hash, target,
    // blend state and a checksum of the PS constants it reads (finds a stray one-frame overlay).
    { static int fl=-1; if(fl<0) fl=getenv("PELOAD_D3DGL_FSLOG")?1:0;
      if(fl && vcount<=6 && instcount<=1){ extern unsigned long g_eng_present_no_x; D3DObj*rt=g_d3dst.rtv; D3DObj*rr=(rt&&rt->data&&rt->vkind)?(D3DObj*)rt->data:rt;
        uint32_t cs=0; float c0[4]={0,0,0,0};
        for(int b=0;b<4;b++){ D3DObj*c=g_d3dst.pscb[b]; if(!c||!c->data||!mp->pblk[b]) continue; uint64_t o0=(uint64_t)g_d3dst.pscb_1st[b]*16, o1=o0+(uint64_t)mp->pblk[b]*16; if(o1>c->size)o1=c->size;
            for(uint64_t q=o0;q+4<=o1;q+=4){ uint32_t u; memcpy(&u,(const uint8_t*)c->data+q,4); cs=cs*31+u; }
            if(b==0 && o0+16<=c->size) memcpy(c0,(const uint8_t*)c->data+o0,16); }
        char m[260]; int k=snprintf(m,sizeof m,"  [fslog] f%lu psh=%016llx rt=%p %ux%u f%u n=%u blend=%p cbsum=%08x c0=(%.3g %.3g %.3g %.3g)\n",g_eng_present_no_x,(unsigned long long)ps->bchash,(void*)rr,rr?rr->w:0,rr?rr->h:0,rr?rr->fmt:0,vcount,(void*)g_d3dst.blend,cs,c0[0],c0[1],c0[2],c0[3]); (void)!write(2,m,k); } }
    { static int sk=-1; static uint64_t skh[8]; if(sk<0){ sk=0; const char*e=getenv("PELOAD_D3DGL_SKIPVSH"); while(e&&*e&&sk<8){ skh[sk++]=strtoull(e,0,16); e=strchr(e,','); if(e)e++; } }
      for(int q=0;q<sk;q++) if(vs->bchash==skh[q]||ps->bchash==skh[q]){ if(covq>=0) covq_end(covq); return; } }   // A/B: drop draws of these shader hashes
    if(indexed && g_d3dst.ib && g_d3dst.ib->data && g_d3dst.ib->size){
        D3DObj*ib=g_d3dst.ib;
        unsigned itype=(g_d3dst.ibfmt==42)?GLE_UI:GLE_US; size_t isz=(itype==GLE_UI)?4:2;
        uint64_t boff=(uint64_t)g_d3dst.iboff+(uint64_t)start*isz;        // + StartIndexLocation
        uint64_t maxn=(boff<ib->size)?(ib->size-boff)/isz:0; if(n>maxn) n=(unsigned)maxn;
        // dynamic index data streams through the ring too (same NO_OVERWRITE upload stall as vertices)
        { uint64_t lo=boff, hi=boff+(uint64_t)n*isz; int covered = ib->glbuf && (!ib->gl_dirty || (ib->rvalid && lo>=ib->rlo && hi<=ib->rhi)); long ro=-1;
          if(!covered && ib->gl_dirty && n && hi<=ib->size && !getenv("PELOAD_D3DGL_NORING") && !getenv("PELOAD_D3DGL_NORING_IB")){ uint64_t _t=prof_on()?prof_now():0; cs_wait_res(ib); ro=d3dgl_ring_put((const uint8_t*)ib->data+lo,hi-lo); if(_t) g_vx_ns[1]+=prof_now()-_t; }
          if(ro>=0){ G.BindBuffer(GLE_ELEM,g_ring); boff=(uint64_t)ro; }
          else d3dgl_buf_need(ib,GLE_ELEM,boff,boff+(uint64_t)n*isz); }
        // D3D11 strip topologies ALWAYS cut at the all-ones index; GL 4.1 needs PRIMITIVE_RESTART + the index,
        // otherwise the cut index is fetched as vertex 0xFFFF and the strip fans out to a garbage vertex.
        int strip=(g_d3dst.topo==5||g_d3dst.topo==3), pr=strip&&G.PrimitiveRestartIndex;
        if(pr){ gs_cap(0x8F9D,1); G.PrimitiveRestartIndex(itype==GLE_UI?0xFFFFFFFFu:0xFFFFu); }
        const void*off=(const void*)(size_t)boff;
        base = (int32_t)((int64_t)base - vshift);                          // ring-rebased vertices
        if(n){ if(base && G.DrawElementsBaseVertex){                         // + BaseVertexLocation
                   if(instcount>1) G.DrawElementsInstancedBaseVertex(mode,(int)n,itype,off,(int)instcount,base);
                   else G.DrawElementsBaseVertex(mode,(int)n,itype,off,base); }
               else { if(instcount>1) G.DrawElementsInstanced(mode,(int)n,itype,off,(int)instcount);
                      else G.DrawElements(mode,(int)n,itype,off); } }
        if(pr) gs_cap(0x8F9D,0);
    } else if(!indexed){                                                     // + StartVertexLocation
        int first=(int)((int64_t)start - vshift);
        if(instcount>1) G.DrawArraysInstanced(mode,first,(int)n,(int)instcount);
        else G.DrawArrays(mode,first,(int)n);
    }
    if(covq>=0) covq_end(covq);
    // PELOAD_D3DGL_LUMLOG: trace the 1x1 luminance targets (auto exposure): value written by draws into a
    // 1x1 RT and the 1x1 textures each draw samples (GPU readback; diagnostic only, stalls the pipeline).
    { static int ll=-1; if(ll<0) ll=getenv("PELOAD_D3DGL_LUMLOG")?1:(wd_on()?2:0);
      if(ll){ extern unsigned long g_eng_present_no_x; static void(*gi)(unsigned,int*); static void(*gti)(unsigned,int,unsigned,unsigned,void*);
        if(!gi){ gi=dlsym(RTLD_DEFAULT,"glGetIntegerv"); gti=dlsym(RTLD_DEFAULT,"glGetTexImage"); }
        D3DObj*rt=g_d3dst.rtv; D3DObj*rr=(rt&&rt->data&&rt->vkind)?(D3DObj*)rt->data:rt;
        if(rr&&rr->w==1&&rr->h==1&&gi){ int dfb=0,rfb=0; gi(0x8CA6/*DRAW_FB_BINDING*/,&dfb); gi(0x8CAA/*READ_FB_BINDING*/,&rfb); float px[4]={-1,-1,-1,-1};
            G.BindFramebuffer(0x8CA8,(unsigned)dfb); G.ReadPixels(0,0,1,1,GLE_RGBA,0x1406/*FLOAT*/,px); G.BindFramebuffer(0x8CA8,(unsigned)rfb);
            if(rr->fmt==10) wd_lum(px[0]);
            if(ll==1){ char m[200]; int k=snprintf(m,sizeof m,"  [lum] f%lu WRITE rt=%p tex=%u psh=%016llx -> (%g %g %g %g)\n",g_eng_present_no_x,(void*)rr,rr->gltex,(unsigned long long)ps->bchash,px[0],px[1],px[2],px[3]); (void)!write(2,m,k); } }
        for(int t=0;t<32&&gti&&ll==1;t++){ D3DObj*x=g_d3dst.psrv[t]; D3DObj*xr=(x&&x->data&&x->vkind)?(D3DObj*)x->data:x; if(!xr||xr->w!=1||xr->h!=1||!xr->gltex||xr->kind<2) continue;
            float px[4]={-1,-1,-1,-1}; G.BindTexture(GLE_TEX2D,xr->gltex); TEXCACHE_INVALIDATE(); gti(GLE_TEX2D,0,GLE_RGBA,0x1406,px);
            char m[200]; int k=snprintf(m,sizeof m,"  [lum] f%lu READ  t%d=%p tex=%u psh=%016llx rt=%ux%u -> (%g %g %g %g)\n",g_eng_present_no_x,t,(void*)xr,xr->gltex,(unsigned long long)ps->bchash,rr?rr->w:0,rr?rr->h:0,px[0],px[1],px[2],px[3]); (void)!write(2,m,k); } } }
    if(fd0){ mp->drawn=1; double d=(mono_s()-fd0)*1e3; if(d>2.0){ char mm[120]; int k=snprintf(mm,sizeof mm,"  [progtime] first draw %.1f ms t=%.2f\n",d,g_gameplay_t0?mono_s()-g_gameplay_t0:-1.0); (void)!write(2,mm,k); } }
    // PELOAD_D3DGL_COVPROBE (use with SOLIDPS): read back the CURRENTLY-BOUND FBO right after each of the
    // first geometry draws and count red/nonblack pixels. This measures "did THIS draw rasterize into ITS
    // OWN render target" with zero present/latch/capture confusion -- the ground truth for on-screen coverage.
    // PELOAD_D3DGL_COVPROBE: probe every COVINT'th geometry draw (default 40) that renders into a big
    // (>=512x512) RT; read the FULL viewport (geometry projects to CENTER, not the corner) and dump the
    // frame to a BMP whenever coverage hits a NEW HIGH -> captures the FULLEST scene across the run, directly
    // from the geometry's own FBO (bypasses the present/latch). COVINT keeps the 8MB readbacks sparse.
    // PELOAD_D3DGL_COVRT="WxHfF" (e.g. 1920x1080f28): dump THAT render target right after every COVINT'th
    // draw into it (up to 8), instead of coverage new-highs -- to capture a specific stage of the frame graph
    // such as the engine's final tonemapped frame.
    if(getenv("PELOAD_D3DGL_COVRT") && n>0 && G.ReadPixels && g_d3dst.rtv){ static unsigned fw,fh,ff; static int parsed;
        if(!parsed){ sscanf(getenv("PELOAD_D3DGL_COVRT"),"%ux%uf%u",&fw,&fh,&ff); parsed=1; }
        D3DObj*r=g_d3dst.rtv; const char*psf=getenv("PELOAD_D3DGL_COVPS");   // optional PS reflected-name substring filter
        if(psf && !(g_d3dst.ps && strstr(g_d3dst.ps->rname,psf))) r=0;
        if(r && r->w==fw && r->h==fh && r->fmt==ff){ static long hits; static int nd; hits++;
            int interval=getenv("PELOAD_D3DGL_COVINT")?atoi(getenv("PELOAD_D3DGL_COVINT")):20; if(interval<1)interval=20;
            if(nd<8 && (hits%interval)==0){ static uint8_t*cb2; if(!cb2) cb2=malloc(4096u*4096u*4u);
                int W=(int)fw>4096?4096:(int)fw, H=(int)fh>4096?4096:(int)fh;
                { static void(*gi)(unsigned,int*); static unsigned char(*isen)(unsigned); static void(*gb)(unsigned,unsigned char*); if(!gi){ gi=dlsym(RTLD_DEFAULT,"glGetIntegerv"); isen=dlsym(RTLD_DEFAULT,"glIsEnabled"); gb=dlsym(RTLD_DEFAULT,"glGetBooleanv"); }
                  int fb=-1,rfb=-1,vp[4]={0},prog=-1; unsigned char cm[4]={9,9,9,9}; if(gi){ gi(0x8CA6,&fb); gi(0x8CAA,&rfb); gi(0x0BA2,vp); gi(0x8B8D,&prog); } if(gb) gb(0x0C23,cm);
                  unsigned err=G.GetError?G.GetError():0; char m[260]; int k=snprintf(m,sizeof m,"  [covrt-state] drawfb=%d readfb=%d rt->glfbo=%u gltex=%u vp=%d,%d,%d,%d prog=%d scissor=%d depth=%d blend=%d cull=%d colormask=%d%d%d%d glerr=0x%x\n",
                      fb,rfb,r->glfbo,r->gltex,vp[0],vp[1],vp[2],vp[3],prog,isen?isen(0x0C11):-1,isen?isen(0x0B71):-1,isen?isen(0x0BE2):-1,isen?isen(0x0B44):-1,cm[0],cm[1],cm[2],cm[3],err); (void)!write(2,m,k); }
                if(cb2){ G.ReadPixels(0,0,W,H,GLE_RGBA,GLE_UBYTE,cb2); const char*home=getenv("HOME"); char pth[256];
                    snprintf(pth,sizeof pth,"%s/reclaimer_rt_%ux%uf%u_%d_%02d.bmp",home?home:"/tmp",fw,fh,ff,(int)getpid(),nd++);
                    bmp_write_rgba(pth,W,H,cb2); char m[300]; int k=snprintf(m,sizeof m,"  [covrt] -> %s (draw #%ld into it)\n",pth,hits); (void)!write(2,m,k);
                    { D3DObj*V=g_d3dst.vs; if(V){ char m5[200]; int k5=snprintf(m5,sizeof m5,"  [covrt]   VS %p refl=[%s] vb0 stride=%u off=%u\n",(void*)V,V->rname,g_d3dst.vstride[0],g_d3dst.voff[0]); (void)!write(2,m5,k5);
                        const char*home3=getenv("HOME"); char fp3[256]; snprintf(fp3,sizeof fp3,"%s/reclaimer_covrt_vs_%d.glsl",home3?home3:"/tmp",nd); FILE*F3=fopen(fp3,"w"); if(F3){ if(V->glsl) fputs(V->glsl,F3); fclose(F3); }
                        for(int b=0;b<16;b++){ D3DObj*c=g_d3dst.vscb[b]; if(!c||!c->data||!c->size) continue; const float*f=(const float*)c->data; char m6[260];
                            int k6=snprintf(m6,sizeof m6,"  [covrt]   vs cb%d %lluB: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g\n",b,(unsigned long long)c->size,f[0],f[1],f[2],f[3],c->size>=32?f[4]:0,c->size>=32?f[5]:0,c->size>=32?f[6]:0,c->size>=32?f[7]:0,c->size>=48?f[8]:0,c->size>=48?f[9]:0,c->size>=48?f[10]:0,c->size>=48?f[11]:0); (void)!write(2,m6,k6); }
                        D3DObj*vb0=g_d3dst.vb[0]; if(vb0&&vb0->data&&g_d3dst.vstride[0]>=8){ const float*vf=(const float*)((const uint8_t*)vb0->data+g_d3dst.voff[0]); char m7[200];
                            int k7=snprintf(m7,sizeof m7,"  [covrt]   vtx0 pos=(%.3f,%.3f) uv=(%.3f,%.3f) | vtx1 pos=(%.3f,%.3f)\n",vf[0],vf[1],vf[2],vf[3],vf[g_d3dst.vstride[0]/4],vf[g_d3dst.vstride[0]/4+1]); (void)!write(2,m7,k7); } } }
                    for(int b=0;b<16;b++){ D3DObj*c=g_d3dst.pscb[b]; if(!c||!c->data||!c->size) continue; const float*f=(const float*)c->data;
                        char m2[300]; int k2=snprintf(m2,sizeof m2,"  [covrt]   ps cb%d %lluB: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g\n",b,(unsigned long long)c->size,f[0],f[1],f[2],f[3],c->size>=32?f[4]:0,c->size>=32?f[5]:0,c->size>=32?f[6]:0,c->size>=32?f[7]:0); (void)!write(2,m2,k2); }
                    if(g_d3dst.ps){ D3DObj*P=g_d3dst.ps; const char*home2=getenv("HOME"); char fp[256];
                        snprintf(fp,sizeof fp,"%s/reclaimer_covrt_ps.glsl",home2?home2:"/tmp"); FILE*F=fopen(fp,"w"); if(F){ if(P->glsl) fputs(P->glsl,F); fclose(F); }
                        if(P->bc){ ReflS*R=refl_build(P->bc,P->bclen); if(R){ for(uint32_t i=0;i<R->nrb;i++){ uint32_t e=R->rboff+i*R->rbstride;
                            char m4[160]; int k4=snprintf(m4,sizeof m4,"  [covrt]   ps binds %s type=%u -> slot %u\n",rf_str(R,rf_u32(R,e)),rf_u32(R,e+4),rf_u32(R,e+20)); (void)!write(2,m4,k4); }
                            refl2_Release(R); } } }
                    for(int t=0;t<16;t++){ D3DObj*x=g_d3dst.psrv[t]; if(x){ char m3[160]; int k3=snprintf(m3,sizeof m3,"  [covrt]   ps t%d %p %ux%u f%u%s samp=%p\n",t,(void*)x,x->w,x->h,x->fmt,x->glfbo?" RT":"",(void*)g_d3dst.pssamp[t]); (void)!write(2,m3,k3);} } } } } }
    if(getenv("PELOAD_D3DGL_COVPROBE") && nattr>0 && n>0 && G.ReadPixels && g_d3dst.rtv && g_d3dst.rtv->w>=512){
        static long gdraw; gdraw++; int interval=getenv("PELOAD_D3DGL_COVINT")?atoi(getenv("PELOAD_D3DGL_COVINT")):40; if(interval<1)interval=40;
        if((gdraw%interval)==0){
            int W=g_d3dst.vp_w>0?g_d3dst.vp_w:1280, H=g_d3dst.vp_h>0?g_d3dst.vp_h:720; if(W>1920)W=1920; if(H>1080)H=1080;
            static uint8_t*cbuf; if(!cbuf) cbuf=malloc(1920*1080*4);
            if(cbuf){ if(G.GetError)G.GetError(); G.ReadPixels(0,0,W,H,GLE_RGBA,GLE_UBYTE,cbuf); unsigned ge=G.GetError?G.GetError():0;
                int red=0,nb=0; for(int i=0;i<W*H;i++){ uint8_t r=cbuf[i*4],g=cbuf[i*4+1],b=cbuf[i*4+2]; if(r>80&&r>g+30&&r>b+30)red++; if((int)r+g+b>24)nb++; }
                static int best_nb, ndump; D3DObj*crt=g_d3dst.rtv;
                if(nb > best_nb+2000 && ndump<8){ best_nb=nb; ndump++; const char*home=getenv("HOME");
                    char p[256]; snprintf(p,sizeof p,"%s/reclaimer_cov_%d_%02d.bmp",home?home:"/tmp",(int)getpid(),ndump);
                    bmp_write_rgba(p,W,H,cbuf);
                    char m[220]; int k=snprintf(m,sizeof m,"  [covprobe] NEWHIGH DUMP -> %s (vp=%dx%d fbo=%u red=%d nonblack=%d glerr=%u)\n",p,W,H,crt?crt->glfbo:0,red,nb,ge); (void)!write(2,m,k); } } } }
    PH(6);   // (attribute arrays stay enabled for the next draw; stale ones are disabled before each draw)
    g_d3dgl_phase="done"; g_d3dgl_frame_draws++; g_draws_total++; d3dgl_note_rt(rt); rtc_note(rt,rt->w,rt->h,rt->fmt,nattr>0,g_d3dst.psrv[0]);
    { D3DObj*t0=g_d3dst.psrv[0]; { D3DObj*b=g_d3dst.blend; const uint32_t*r0=(b&&b->stkind==1)?b->sd+2:0;
      flog("draw rt=%06lx %ux%u f%u vp=%dx%d ps=[%.40s] t0=%06lx %ux%u f%u t1=%06lx blend=%u:%u/%u wm=%x",((unsigned long)rt>>4)&0xffffff,rt->w,rt->h,rt->fmt,vw,vh,ps?ps->rname:"-",
        ((unsigned long)t0>>4)&0xffffff,t0?t0->w:0,t0?t0->h:0,t0?t0->fmt:0,((unsigned long)g_d3dst.psrv[1]>>4)&0xffffff,r0?r0[0]:0,r0?r0[1]:0,r0?r0[2]:0,r0?(r0[7]&0xF):0xF); } }
    // PELOAD_D3DGL_COLORTRACK=<frame>: on that present-frame, after every draw into a >=1280-wide target, read
    // a 32x32 block at the viewport centre and log draws that change it sharply (pinpoints which pass
    // produces / destroys a colour channel). Keeps the last value per target.
    if(getenv("PELOAD_D3DGL_COLORTRACK") && G.ReadPixels && rt->w>=1280){ extern unsigned long g_ct_frame; unsigned long want=strtoul(getenv("PELOAD_D3DGL_COLORTRACK"),0,10);
        if(g_ct_frame==want){ static void*rts[8]; static float last[8][3]; static int nr; int ri=-1; for(int q=0;q<nr;q++) if(rts[q]==rt){ ri=q; break; }
            if(ri<0 && nr<8){ ri=nr++; rts[ri]=rt; last[ri][0]=last[ri][1]=last[ri][2]=-1; }
            if(ri>=0){ uint8_t px[8*8*4]; float m[3]={0,0,0}; int RH=(int)(rt->h>4096?4096:rt->h);
                for(int gy=0;gy<4;gy++) for(int gx=0;gx<4;gx++){ int x=g_d3dst.vp_x+(vw*(2*gx+1))/8-4, y=d3dgl_fbo_y((uint32_t)RH,(uint32_t)(g_d3dst.vp_y+(vh*(2*gy+1))/8-4),8);
                    G.ReadPixels(x,y,8,8,GLE_RGBA,GLE_UBYTE,px); for(int q=0;q<64;q++){ m[0]+=px[q*4]; m[1]+=px[q*4+1]; m[2]+=px[q*4+2]; } }
                for(int c=0;c<3;c++) m[c]/=1024.0f;
                float d=fabsf(m[0]-last[ri][0])+fabsf(m[1]-last[ri][1])+fabsf(m[2]-last[ri][2]);
                if(last[ri][0]<0 || d>2.0f){ D3DObj*b=g_d3dst.blend; const uint32_t*r0=(b&&b->stkind==1)?b->sd+2:0; D3DObj*t0=g_d3dst.psrv[0]; char mm[300];
                    int k=snprintf(mm,sizeof mm,"  [ctrack] rt=%06lx (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) ps=[%.44s] blend=%u:%u/%u wm=%x t0=%ux%u f%u n=%u\n",((unsigned long)rt>>4)&0xffffff,
                        last[ri][0],last[ri][1],last[ri][2],m[0],m[1],m[2],ps?ps->rname:"-",r0?r0[0]:0,r0?r0[1]:0,r0?r0[2]:0,r0?(r0[7]&0xF):0xF,t0?t0->w:0,t0?t0->h:0,t0?t0->fmt:0,n); (void)!write(2,mm,k); }
                last[ri][0]=m[0]; last[ri][1]=m[1]; last[ri][2]=m[2]; } } }
    if(rt==g_d3d_backbuf){ g_bb_draws++; g_bb_vp=(unsigned long)vw<<16|(unsigned long)vh; if(g_d3dst.psrv[0]){ g_bb_t0w=g_d3dst.psrv[0]->w; g_bb_t0h=g_d3dst.psrv[0]->h; g_bb_t0f=g_d3dst.psrv[0]->fmt; } }
    if(getenv("PELOAD_TRACE_COPY")) for(int t=0;t<16;t++){ D3DObj*x=g_d3dst.psrv[t]; if(x&&x->copydst){ static int cc; if(cc++<20){ char m[200];
        int k=snprintf(m,sizeof m,"  [copyuse] draw samples copy-dst %ux%u f%u at t%d -> writes rt=%p %ux%u f%u%s vp=%dx%d\n",x->w,x->h,x->fmt,t,(void*)rt,rt->w,rt->h,rt->fmt,rt==g_d3d_backbuf?" (BACKBUFFER)":"",vw,vh); (void)!write(2,m,k);} break; } }
    // Latch the present source. The backbuffer (final composite) is preferred; if the engine never
    // composites to it, fall back to the largest sizeable RT drawn into so we still show the scene.
    // The blit shows the [0,vw/tw]x[0,vh/th] sub-rect of the texture, so if a LATER small-viewport draw
    // (a UI element, a sub-region update) shrank present_vw/vh the frame "zoomed" into the corner. Keep
    // the LARGEST viewport seen for the current texture so a small draw can't zoom the composite.
    // PELOAD_D3DGL_PRESENT_SCENE: the engine composites the loading screen / UI into the backbuffer but
    // renders the 3D world into separate scene RTs; by default we present the backbuffer (so we show the
    // loading screen). In scene mode, ignore the backbuffer and latch the largest non-backbuffer RT that
    // received a real geometry draw this call -> show the rendered map instead of the loading overlay.
    // PELOAD_D3DGL_PRESENT_GEOM: latch the RT that accumulates the MOST geometry vertices over the run
    // (= the 3D scene color buffer -- the terrain/object draws submit millions of verts, while UI text
    // quads and the loading-screen composite blit submit a handful). This reliably picks the scene RT
    // regardless of its size/aspect, where the loading-screen overlay and square shadow RTs do not.
    if(getenv("PELOAD_D3DGL_PRESENT_GEOM") && !(rt==g_d3d_backbuf) && nattr>0 && n>0 && rt->w>=256 && rt->h>=256){
        static struct { unsigned tex; double verts; int w,h,vw,vh; } gtbl[64]; static int ngtbl; static unsigned gbesttex;
        int fi=-1; for(int i=0;i<ngtbl;i++) if(gtbl[i].tex==rt->gltex){ fi=i; break; }
        if(fi<0 && ngtbl<64){ fi=ngtbl++; gtbl[fi].tex=rt->gltex; gtbl[fi].verts=0; }
        if(fi>=0){ gtbl[fi].verts += (double)n; gtbl[fi].w=(int)rt->w; gtbl[fi].h=(int)rt->h; gtbl[fi].vw=vw; gtbl[fi].vh=vh;
            double mx=-1; int mi=-1; for(int i=0;i<ngtbl;i++) if(gtbl[i].verts>mx){ mx=gtbl[i].verts; mi=i; }
            if(mi>=0 && gtbl[mi].tex!=gbesttex){ gbesttex=gtbl[mi].tex;
                g_d3dgl_present_tex=gtbl[mi].tex;
                g_d3dgl_present_tw=(gtbl[mi].w>4096?4096:gtbl[mi].w); g_d3dgl_present_th=(gtbl[mi].h>4096?4096:gtbl[mi].h);
                if(g_d3dgl_present_tw<1)g_d3dgl_present_tw=gtbl[mi].vw; if(g_d3dgl_present_th<1)g_d3dgl_present_th=gtbl[mi].vh;
                g_d3dgl_present_vw=g_d3dgl_present_tw; g_d3dgl_present_vh=g_d3dgl_present_th;
                if(getenv("PELOAD_TRACE_D3DGL")){ char m[160]; int k=snprintf(m,sizeof m,"  [present-geom] latch tex=%u %dx%d verts=%.0f\n",gtbl[mi].tex,gtbl[mi].w,gtbl[mi].h,mx); (void)!write(2,m,k);} } }
        return;
    }
    int scene_mode = getenv("PELOAD_D3DGL_PRESENT_SCENE")!=0;
    if(vw>=512 && vh>=288){ long a=(long)vw*vh; static long best;
        int isbb=(rt==g_d3d_backbuf);
        // In scene mode prefer a 16:9 RT (the scene COLOR buffer, 1920x1080 / 1280x720) over square
        // shadow/atlas RTs (4096^2) that also take geometry draws -- a huge bias puts any 16:9 RT above
        // any square one, and within a tier the largest wins (so scene color beats the bloom downsample).
        long score_a = a;
        if(scene_mode){ float ar=(rt->h>0)?(float)rt->w/(float)rt->h:0.0f; if(ar>1.6f && ar<1.95f) score_a = a | (1L<<50); }
        int take = scene_mode ? (!isbb && nattr>0 && score_a>=best) : (isbb || a>=best);
        if(take){ best = (!scene_mode && isbb)?(1L<<60):score_a;
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
        // sub-rect [0,sc.x] x [top sc.y] of the texture: rendered targets hold D3D row 0 at the TOP GL row,
        // so the engine's (0,0,vw,vh) region is t.y in [1-sc.y, 1].
        const char*flipy = getenv("PELOAD_BLIT_FLIP") ? "(1.0-uv.y)" : "uv.y";
        // PELOAD_D3DGL_EXPOSE=<f>: present-time tonemap for the dark linear-HDR scene RT. Scale by f,
        // Reinhard-compress, gamma 2.2. Default (unset) = passthrough (unchanged present for UI/loading).
        static char bfsbuf[512]; const char*bfs;
        if(getenv("PELOAD_D3DGL_EXPOSE")){ double E=atof(getenv("PELOAD_D3DGL_EXPOSE")); if(E<=0)E=8.0;
            snprintf(bfsbuf,sizeof bfsbuf,
              "#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2D s;uniform vec4 sc;\n"
              "void main(){vec2 t=vec2(uv.x*sc.x,%s);vec3 x=texture(s,t).rgb*%f;x=x/(x+vec3(1.0));c=vec4(pow(x,vec3(1.0/2.2)),1.0);}\n",
              d3dgl_flipy()?"(1.0-uv.y)*sc.y":(getenv("PELOAD_BLIT_FLIP")?"1.0-sc.y+(1.0-uv.y)*sc.y":"1.0-sc.y+uv.y*sc.y"),E); bfs=bfsbuf; }
        else if(d3dgl_flipy()){ // targets hold D3D row 0 at t=0: the engine's (0,0,vw,vh) is t in [0,sc]; window top = D3D row 0
            bfs="#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2D s;uniform vec4 sc;void main(){vec2 t=vec2(uv.x*sc.x,(1.0-uv.y)*sc.y);c=texture(s,t);}\n"; }
        else { static char pbuf[320]; snprintf(pbuf,sizeof pbuf,
              "#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2D s;uniform vec4 sc;void main(){vec2 t=vec2(uv.x*sc.x,1.0-sc.y+%s*sc.y);c=texture(s,t);}\n",flipy); bfs=pbuf; }
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
static void d3dgl_blit_fullscreen_i(unsigned vao);
static void d3dgl_blit_fullscreen(unsigned vao){ d3dgl_blit_fullscreen_i(vao); gs_invalidate(); }
static void d3dgl_blit_fullscreen_i(unsigned vao){
    if(!g_d3dgl_present_tex) return;
    G.BindFramebuffer(GLE_FBO,0); G.Viewport(0,0,g_d3dgl_W,g_d3dgl_H);
    G.Disable(0x0B71); G.Disable(0x0BE2);
    G.ClearColor(0.02f,0.02f,0.05f,1.0f); G.Clear(GLE_COLORBIT);
    G.UseProgram(0); G.BindProgramPipeline(0); G.UseProgram(g_d3dgl_blit_prog);
    TEXCACHE_INVALIDATE(); G.ActiveTexture(GLE_TEX0); G.BindTexture(GLE_TEX2D,g_d3dgl_present_tex);
    // present shows the raw stored bytes (the swap chain is UNORM; the game already sRGB-encoded via its RTV)
    if(G.BindSampler) G.BindSampler(0,0); G.TexParameteri(GLE_TEX2D,0x8A48,0x8A4A/*SKIP_DECODE*/); G.Disable(0x8DB9);
    if(g_d3dgl_blit_sloc>=0) G.ProgramUniform1i(g_d3dgl_blit_prog,g_d3dgl_blit_sloc,0);
    if(g_d3dgl_blit_uloc>=0){ float tw=g_d3dgl_present_tw?g_d3dgl_present_tw:1, th=g_d3dgl_present_th?g_d3dgl_present_th:1;
        float sc[4]={ g_d3dgl_present_vw/tw, g_d3dgl_present_vh/th, 0,0 }; G.ProgramUniform4fv(g_d3dgl_blit_prog,g_d3dgl_blit_uloc,1,sc); }
    G.BindVertexArray(vao); G.DrawArrays(4,0,3);
    hb_draw_overlay();   // PELOAD_HITBOXES (hitbox.c): object bounding volumes over the frame
    // PELOAD_D3DGL_SHOTWIN=<n>: capture the WINDOW framebuffer (FB 0) right after the blit = EXACTLY what the
    // user sees (not the G-buffer RT dumps, which are a different, often-black buffer). Every n blits.
    if(getenv("PELOAD_D3DGL_SHOTWIN")){ static unsigned long bc; int every=atoi(getenv("PELOAD_D3DGL_SHOTWIN")); if(every<1)every=20;
        if((bc++ % every)==0 && G.ReadPixels){ int w=g_d3dgl_W,h=g_d3dgl_H; if(w>0&&h>0&&(long)w*h<=(32<<20)){
            unsigned char*cap=calloc(1,(size_t)w*h*4);
            if(cap){ G.BindFramebuffer(GLE_FBO,0); if(G.ReadBuffer)G.ReadBuffer(0x0405/*BACK*/); if(G.GetError)G.GetError();
                G.ReadPixels(0,0,w,h,GLE_RGBA,GLE_UBYTE,cap);
                static int wn; const char*home=getenv("HOME"); char p[512]; snprintf(p,sizeof p,"%s/reclaimer_win_%d_%03d%s.bmp",home?home:"/tmp",(int)getpid(),wn++, getenv("PELOAD_D3DGL_ALT5")?((((long)(mono_s()/5.0))&1)?"_B":"_A"):"");
                bmp_write_rgba(p,w,h,cap); char m[160]; int k=snprintf(m,sizeof m,"  [d3dgl] win-shot -> %s (%dx%d)\n",p,w,h); (void)!write(2,m,k); free(cap); } } } }
}
// ---- fenced present queue (render thread -> present thread) -------------------------------------------
// The present thread used to re-blit whatever the backbuffer held every 33 ms (window capped at ~30 fps,
// and the render thread glFinish()ed every frame so the other context saw finished pixels). Now each engine
// Present copies the frame region into one of 3 window-sized textures, drops a GL fence and signals; the
// present thread wakes, waits on the fence on the GPU (glWaitSync, no CPU stall) and shows that frame.
// PELOAD_D3DGL_NOPQ restores the old timer + glFinish path.
unsigned long g_eng_present_no_x; static unsigned long g_eng_present_no;   // engine Present() count (frame tag for captures / diagnostics)
static unsigned g_pq_tex[3], g_pq_fbo[3]; static void* g_pq_fence[3]; static int g_pq_latest=-1, g_pq_reading=-1, g_pq_w, g_pq_h; static unsigned long g_pq_frameno[3];
static unsigned long g_pq_seq; static pthread_mutex_t g_pq_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pq_cv=PTHREAD_COND_INITIALIZER;
static void*(*pq_fencesync)(unsigned,unsigned); static void(*pq_waitsync)(void*,unsigned,uint64_t); static void(*pq_delsync)(void*);
static int pq_on(void){ static int on=-1; if(on<0){ on=getenv("PELOAD_D3DGL_NOPQ")?0:1; if(on){ pq_fencesync=dlsym(RTLD_DEFAULT,"glFenceSync"); pq_waitsync=dlsym(RTLD_DEFAULT,"glWaitSync"); pq_delsync=dlsym(RTLD_DEFAULT,"glDeleteSync"); if(!pq_fencesync||!pq_waitsync||!pq_delsync) on=0; } } return on; }
// render thread (g_glmx held, render context current)
static void pq_publish(void){
    if(!pq_on() || !g_d3dgl_present_tex || !G.BlitFramebuffer) return;
    D3DObj*bb=g_d3d_backbuf; if(!bb||!bb->glfbo||bb->gltex!=g_d3dgl_present_tex) return;   // only the swap-chain backbuffer path
    int W=g_d3dgl_W, H=g_d3dgl_H; if(W<1||H<1) return;
    int slot; pthread_mutex_lock(&g_pq_mx);
    for(slot=0;slot<3;slot++) if(slot!=g_pq_reading && slot!=g_pq_latest) break;
    void*old=(slot<3)?g_pq_fence[slot]:0; if(slot<3) g_pq_fence[slot]=0;
    pthread_mutex_unlock(&g_pq_mx); if(slot>=3) return;
    if(old) pq_delsync(old);
    if(g_pq_w!=W||g_pq_h!=H||!g_pq_tex[0]){ for(int i=0;i<3;i++){ if(!g_pq_tex[i]){ G.GenTextures(1,&g_pq_tex[i]); G.GenFramebuffers(1,&g_pq_fbo[i]); }
            TEXCACHE_INVALIDATE(); G.BindTexture(GLE_TEX2D,g_pq_tex[i]); G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,W,H,0,GLE_RGBA,GLE_UBYTE,0);
            G.TexParameteri(GLE_TEX2D,0x2801,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2800,GLE_LINEAR); G.TexParameteri(GLE_TEX2D,0x2802,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x2803,GLE_CLAMP); G.TexParameteri(GLE_TEX2D,0x813D,0);
            G.BindFramebuffer(GLE_FBO,g_pq_fbo[i]); G.FramebufferTexture2D(GLE_FBO,GLE_CA0,GLE_TEX2D,g_pq_tex[i],0); }
        g_pq_w=W; g_pq_h=H; }
    int vw=g_d3dgl_present_vw>0?g_d3dgl_present_vw:g_d3dgl_present_tw, vh=g_d3dgl_present_vh>0?g_d3dgl_present_vh:g_d3dgl_present_th;
    d3dgl_reset_raster_ops();
    G.BindFramebuffer(0x8CA8,bb->glfbo); G.BindFramebuffer(0x8CA9,g_pq_fbo[slot]);
    // raw bytes (FRAMEBUFFER_SRGB off), scaled to the window; D3D row 0 (GL row 0) lands at the window TOP
    G.BlitFramebuffer(0,0,vw,vh, 0,H,W,0, 0x4000,0x2601/*LINEAR*/);
    G.BindFramebuffer(GLE_FBO,0);
    void*f=pq_fencesync(0x9117/*SYNC_GPU_COMMANDS_COMPLETE*/,0); G.Flush();
    pthread_mutex_lock(&g_pq_mx); g_pq_fence[slot]=f; g_pq_latest=slot; g_pq_seq++; g_pq_frameno[slot]=g_eng_present_no; pthread_cond_signal(&g_pq_cv); pthread_mutex_unlock(&g_pq_mx); }
// present thread: wait (up to ms) for a frame newer than *seen; draw it. Returns 1 if a frame was shown.
static unsigned g_pq_prog, g_pq_vao; static int g_pq_sloc=-1;
static int pq_present_wait(unsigned long*seen,int ms){
    if(!pq_on()) return 0;
    pthread_mutex_lock(&g_pq_mx);
    if(g_pq_seq==*seen){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_nsec+=(long)ms*1000000L; ts.tv_sec+=ts.tv_nsec/1000000000L; ts.tv_nsec%=1000000000L;
        pthread_cond_timedwait(&g_pq_cv,&g_pq_mx,&ts); }
    if(g_pq_seq==*seen || g_pq_latest<0){ pthread_mutex_unlock(&g_pq_mx); return 0; }
    int slot=g_pq_latest; g_pq_reading=slot; *seen=g_pq_seq; void*f=g_pq_fence[slot];
    pthread_mutex_unlock(&g_pq_mx);
    if(f) pq_waitsync(f,0,0xFFFFFFFFFFFFFFFFull/*TIMEOUT_IGNORED*/);
    if(!g_pq_prog){ const char*vs="#version 410 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);uv=p*0.5+0.5;gl_Position=vec4(p,0.0,1.0);}\n";
        const char*fs="#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2D s;void main(){c=vec4(texture(s,uv).rgb,1.0);}\n";
        unsigned v=G.CreateShader(GLE_VS),fr=G.CreateShader(GLE_FS); G.ShaderSource(v,1,&vs,0); G.CompileShader(v); G.ShaderSource(fr,1,&fs,0); G.CompileShader(fr);
        g_pq_prog=G.CreateProgram(); G.AttachShader(g_pq_prog,v); G.AttachShader(g_pq_prog,fr); G.LinkProgram(g_pq_prog); G.DeleteShader(v); G.DeleteShader(fr);
        g_pq_sloc=G.GetUniformLocation(g_pq_prog,"s"); G.GenVertexArrays(1,&g_pq_vao); }
    CGLContextObj pc=CGLGetCurrentContext(); if(pc) CGLLockContext(pc);
    G.BindFramebuffer(GLE_FBO,0); G.Disable(0x0B71); G.Disable(0x0BE2); G.Disable(0x0C11); G.Disable(0x8DB9);
    { int dw=g_win_pw>0?g_win_pw:g_d3dgl_W, dh=g_win_ph>0?g_win_ph:g_d3dgl_H;   // letterbox the frame's aspect into the window
      double ar=(double)g_d3dgl_W/g_d3dgl_H; int vw=dw, vh=(int)(dw/ar+0.5); if(vh>dh){ vh=dh; vw=(int)(dh*ar+0.5); }
      if(vw!=dw||vh!=dh){ G.Viewport(0,0,dw,dh); G.ClearColor(0,0,0,1); G.Clear(GLE_COLORBIT); }
      G.Viewport((dw-vw)/2,(dh-vh)/2,vw,vh); }
    G.UseProgram(0); G.BindProgramPipeline(0); G.UseProgram(g_pq_prog);
    G.ActiveTexture(GLE_TEX0); G.BindTexture(GLE_TEX2D,g_pq_tex[slot]); if(G.BindSampler) G.BindSampler(0,0);
    if(g_pq_sloc>=0) G.ProgramUniform1i(g_pq_prog,g_pq_sloc,0);
    G.BindVertexArray(g_pq_vao); G.DrawArrays(4,0,3);
    hb_draw_overlay();   // PELOAD_HITBOXES (hitbox.c): object bounding volumes over the frame
    G.Flush(); if(pc) CGLUnlockContext(pc);
    pthread_mutex_lock(&g_pq_mx); g_pq_reading=-1; pthread_mutex_unlock(&g_pq_mx);
    if(getenv("PELOAD_D3DGL_SHOTWIN")){ static unsigned long bc; int every=atoi(getenv("PELOAD_D3DGL_SHOTWIN")); if(every<1)every=20;
        // optional window (seconds after "gameplay presentation ready"): PELOAD_D3DGL_SHOTWIN_FROM / _TO
        double gt=g_gameplay_t0?mono_s()-g_gameplay_t0:-1; const char*fr=getenv("PELOAD_D3DGL_SHOTWIN_FROM"),*to=getenv("PELOAD_D3DGL_SHOTWIN_TO");
        int inwin=(!fr||(gt>=0&&gt>=atof(fr))) && (!to||(gt>=0&&gt<atof(to)));
        if(inwin && (bc++ % every)==0 && G.ReadPixels){ int w=g_win_pw>0?g_win_pw:g_d3dgl_W,h=g_win_ph>0?g_win_ph:g_d3dgl_H; unsigned char*cap=calloc(1,(size_t)w*h*4);
            if(cap){ if(G.ReadBuffer)G.ReadBuffer(0x0405/*BACK*/); G.ReadPixels(0,0,w,h,GLE_RGBA,GLE_UBYTE,cap);
                static int wn; const char*home=getenv("HOME"); char p[512]; snprintf(p,sizeof p,"%s/reclaimer_win_%d_%03d_f%lu%s.bmp",home?home:"/tmp",(int)getpid(),wn++,g_pq_frameno[slot], getenv("PELOAD_D3DGL_ALT5")?((((long)(mono_s()/5.0))&1)?"_B":"_A"):"");
                bmp_write_rgba(p,w,h,cap); free(cap); } } }
    return 1; }
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
static void d3dgl_present_impl(void);
static void d3dgl_present(void){ GLLOCK(); if(!prof_on()) d3dgl_present_impl();
    else { uint64_t t=prof_now(); d3dgl_present_impl(); g_prof_pres_ns+=prof_now()-t; g_prof_pres_n++; prof_report(); } GLUNLOCK(); }
static void d3dgl_present_impl(void){
    if(getenv("PELOAD_TRACE_COPY")){ static unsigned long pc; if((pc++%100)==0){ char m[200]; int k=snprintf(m,sizeof m,"  [presentsrc] present #%lu tex=%u backbuf.gltex=%u vw/vh=%d/%d tw/th=%d/%d | bb draws since last=%lu lastvp=%lux%lu t0=%ux%u f%u | bb SKIPPED=%lu reason=%lu\n",pc,g_d3dgl_present_tex,g_d3d_backbuf?g_d3d_backbuf->gltex:0,g_d3dgl_present_vw,g_d3dgl_present_vh,g_d3dgl_present_tw,g_d3dgl_present_th,g_bb_draws,g_bb_vp>>16,g_bb_vp&0xffff,g_bb_t0w,g_bb_t0h,g_bb_t0f,g_bb_skip,g_bb_skip_reason); g_bb_draws=0; g_bb_skip=0; (void)!write(2,m,k);} }
    g_cb_tick++;                                   // advance CB freshness clock each present
    g_eng_present_no++; g_eng_present_no_x=g_eng_present_no;
    { static int fsl=-1; if(fsl<0) fsl=getenv("PELOAD_D3DGL_FRAMESTAT")?1:0;
      if(fsl){ char m[300]; int k=snprintf(m,sizeof m,"  [fstat] f%lu exec %u skip(vsps %u glsl %u prog %u) ringfail %u wraps %u disp %u copies %u maps %u (read %u) fbochg %u clears rt %u ds %u\n",
            g_eng_present_no,g_fs.exec,g_fs.novsps,g_fs.noglsl,g_fs.noprog,g_fs.ringfail,g_fs.wraps,g_fs.disp,g_fs.copies,g_fs.maps,g_fs.mapread,g_fs.fbochg,g_fs.rtclears,g_fs.dsclears); (void)!write(2,m,k); }
      memset(&g_fs,0,sizeof g_fs); }
    if(getenv("PELOAD_CB_DUMP") && g_last_b12){ static int pc; if(pc++==3) cb_dump_scan(); }  // dump a few presents after gameplay CBs go live (before any late crash)
    if(!g_d3dgl_ready) return;                     // nothing built yet (no draw reached us)
    if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);   // any thread (g_glmx held)
    covq_harvest(); wd_present();
    if(!g_d3dgl_offscreen){                         // we own the drawable: blit to our window
        d3dgl_build_blit(&g_d3dgl_blit_vao);
        d3dgl_blit_fullscreen(g_d3dgl_blit_vao);
        if(g_d3dgl_nsctx) om0(g_d3dgl_nsctx,"flushBuffer");
        cocoa_pump();
    }
    if(g_d3dgl_offscreen) pq_publish(); else if(G.Flush) G.Flush();
    // build previously-seen shader pairs while the loading screen is up (see d3dgl_prelink_tick);
    // during gameplay only with PELOAD_D3DGL_PRELINK_MS=<per-frame budget>
    { static double gb=-1; if(gb<0){ const char*e=getenv("PELOAD_D3DGL_PRELINK_MS"); gb=e?atof(e):0; }
      if(!g_gameplay_t0) d3dgl_prelink_tick(40.0); else if(gb>0) d3dgl_prelink_tick(gb); }
    // PELOAD_D3DGL_SHOTPRESENT: dump the ACTUAL PRESENTED texture (what the window shows) via glGetTexImage,
    // every N presents (default 30). My RT dumps grabbed the wrong G-buffer; this is the real on-screen frame.
    if(getenv("PELOAD_D3DGL_SHOTPRESENT") && g_d3dgl_present_tex){
        static unsigned long pc; int every=atoi(getenv("PELOAD_D3DGL_SHOTPRESENT")); if(every<1)every=30;
        if((pc++ % every)==0){ int w=g_d3dgl_present_tw,h=g_d3dgl_present_th; if(w>0&&h>0&&(long)w*h<=(64<<20)){
            static void(*gti)(unsigned,int,unsigned,unsigned,void*); static int gi; if(!gi){ gti=dlsym(RTLD_DEFAULT,"glGetTexImage"); gi=1; }
            unsigned char*cap=calloc(1,(size_t)w*h*4);
            if(cap&&gti){ TEXCACHE_INVALIDATE(); G.BindTexture(GLE_TEX2D,g_d3dgl_present_tex); if(G.GetError)G.GetError(); gti(GLE_TEX2D,0,GLE_RGBA,GLE_UBYTE,cap);
                static int sn; const char*home=getenv("HOME"); char p[512]; snprintf(p,sizeof p,"%s/reclaimer_present_%03d.bmp",home?home:"/tmp",sn++);
                bmp_write_rgba(p,w,h,cap); char m[200]; int k=snprintf(m,sizeof m,"  [d3dgl] present-shot -> %s (%dx%d tex=%u)\n",p,w,h,g_d3dgl_present_tex); (void)!write(2,m,k); }
            free(cap); } }
    }
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
    for(int i=0;i<frames;i++){ d3dgl_draw(3,0,1,0,0,0); d3dgl_present(); struct timespec ts={0,16*1000000L}; nanosleep(&ts,0); }
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
static void* present_thread_fn(void*a){ (void)a; pthread_setname_np("shim-present"); void(*set)(void*)=dlsym(RTLD_DEFAULT,"CGLSetCurrentContext");
    if(set&&g_present_glctx) set(g_present_glctx);
    // PELOAD_VSYNC=1: vsync the window flips (no tearing). Opt-in: a vsynced flush can block for as long as
    // the display sleeps / the window is occluded, and one soak froze ~105 s in with it enabled.
    if(g_present_glctx && getenv("PELOAD_VSYNC")){ GLint one=1; CGLSetParameter((CGLContextObj)g_present_glctx,kCGLCPSwapInterval,&one); }
    void(*cglflush)(void*)=dlsym(RTLD_DEFAULT,"CGLFlushDrawable");
    // PELOAD_POKE=<rva>:<u8>[,...] continuously writes a byte into guest memory each frame
    // (e.g. force the worker's "window foreground" flag so presents count as ready).
    extern uint8_t* g_base;
    for(;;){
        // When the D3D11->GL executor is running offscreen, it produced the frame; display THAT
        // (the real translated-shader output) on this thread's window instead of the GDI composite.
        int use_d3dgl = d3dgl_enabled() && g_d3dgl_ready && g_d3dgl_offscreen && g_d3dgl_present_tex;
        if(getenv("PELOAD_TRACE_PRESENT")){ static int lp; if((lp++ %60)==0){ char m[96]; int k=snprintf(m,sizeof m,"  [present] branch=%s d3dgl(en=%d ready=%d off=%d tex=%u)\n",use_d3dgl?"d3dgl_blit":"gdi_main",d3dgl_enabled(),g_d3dgl_ready,g_d3dgl_offscreen,g_d3dgl_present_tex); (void)!write(2,m,k);} }
        static unsigned long pq_seen; int pq_shown=0;
        { extern volatile int g_win_visible_x; if(use_d3dgl && !g_win_visible_x && !getenv("PELOAD_PRESENT_HIDDEN")){ usleep(20000); continue; } }
        if(use_d3dgl && pq_on()){
            if(pq_present_wait(&pq_seen,50)){ pq_shown=1; if(cglflush&&g_present_glctx){ CGLLockContext((CGLContextObj)g_present_glctx); cglflush(g_present_glctx); CGLUnlockContext((CGLContextObj)g_present_glctx); } }
            else if(!g_pq_seq){ d3dgl_blit_present(); if(cglflush&&g_present_glctx) cglflush(g_present_glctx); }   // nothing queued yet
        } else if(use_d3dgl){
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
        if(!(use_d3dgl && pq_on() && g_pq_seq)){ struct timespec ts={0,33*1000000L}; nanosleep(&ts,0); }   // frame-driven once the queue runs
        (void)pq_shown; } return 0; }
static void* cocoa_make_gl_window_i(int w,int h){ CGLContextObj c=cocoa_make_gl_window(w,h);
    if(c){ g_present_glctx=(void*)c; void(*set)(void*)=dlsym(RTLD_DEFAULT,"CGLSetCurrentContext");
        if(getenv("PELOAD_HOLD_WINDOW")){ if(!g_present_started){ g_present_started=1; if(set)set(0); /*release from this thread*/ pthread_t th; pthread_create(&th,0,present_thread_fn,0); ws("  [hold-window] present thread started\n"); } }
        else if(set) set(c); }
    return (void*)c; }
volatile int g_win_visible_x=1;
#define g_win_visible g_win_visible_x   // NSWindow occlusionState & Visible, sampled in cocoa_pump
static void cocoa_pump(void){ if(!g_nsapp) return;
    for(;;){ id ev=((id(*)(id,SEL,unsigned long,id,id,signed char))objc_msgSend)(g_nsapp,osel("nextEventMatchingMask:untilDate:inMode:dequeue:"),~0UL,(id)0,NSDefaultRunLoopMode,1);
        if(!ev) break; if(wi_translate(ev)) continue; ((void(*)(id,SEL,id))objc_msgSend)(g_nsapp,osel("sendEvent:"),ev); }
    { const char*tr=getenv("PELOAD_TEST_RESIZE"); static int done; extern double g_gameplay_t0; if(tr && !done && g_gameplay_t0 && mono_s()-g_gameplay_t0>5 && g_nswin){ done=1; int tw=0,th=0; sscanf(tr,"%dx%d",&tw,&th);
        if(tw>0&&th>0){ typedef struct { double w,h; } NSSize_; NSSize_ sz={(double)tw,(double)th}; ((void(*)(id,SEL,NSSize_))objc_msgSend)(g_nswin,osel("setContentSize:"),sz); } } }
    // window visibility (main thread): occluded windows / a sleeping display get no vblank, so a vsynced
    // flush could block the present thread indefinitely -- it skips presenting while not visible.
    if(g_nswin){ unsigned long os=((unsigned long(*)(id,SEL))objc_msgSend)(g_nswin,osel("occlusionState")); g_win_visible=(os&2UL)?1:0; }
    // window resized / full screen: resize the GL drawable (main thread) and record the new size for present
    if(g_nswin && g_nsctx){ id view=om0(g_nswin,"contentView"); NSRect_ b={0,0,0,0}; if(view) ((void(*)(NSRect_*,id,SEL))objc_msgSend_stret)(&b,view,osel("bounds"));   // x86_64: >16-byte struct returns need _stret
        int pw=(int)b.w, ph=(int)b.h; if(pw>0 && ph>0 && (pw!=g_win_pw || ph!=g_win_ph)){
            CGLContextObj c=(CGLContextObj)((void*(*)(id,SEL))objc_msgSend)(g_nsctx,osel("CGLContextObj"));
            if(c) CGLLockContext(c); om0(g_nsctx,"update"); if(c) CGLUnlockContext(c);
            g_win_pw=pw; g_win_ph=ph; } } }
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
    void(*r)(int,int,int,int,unsigned,unsigned,void*)=dlsym(RTLD_DEFAULT,"glReadPixels"); if(r)r(x,y,w,h,fmt,type,d);
    if(getenv("PELOAD_TRACE_READPX")){ static int n; if((n++%200)==0){ int fb=0,rb=0,vp[4]={0}; unsigned err=0;
        void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv"); unsigned(*ge)(void)=dlsym(RTLD_DEFAULT,"glGetError");
        if(gi){ gi(0x8CAA,&fb); gi(0x0C02,&rb); gi(0x0BA2,vp); } if(ge) err=ge();
        uint64_t nz=0; if(d && (fmt==0x1908||fmt==0x80E1) && w>0 && h>0){ const uint8_t*q=d; for(long i=0;i<(long)w*h;i+=7) if(q[i*4]|q[i*4+1]|q[i*4+2]|q[i*4+3]) nz++; }
        extern unsigned long g_guest_gl_draws_x, g_guest_gl_idx_x;
        char m[300]; int k=snprintf(m,sizeof m,"  [readpx] #%d %d,%d %dx%d fmt=0x%x type=0x%x dst=%p readfb=%d readbuf=0x%x vp=%d,%d,%d,%d glerr=0x%x nonzero=%llu cur=%p guest drawElements so far=%lu (%lu idx)\n",n,x,y,w,h,fmt,type,d,fb,rb,vp[0],vp[1],vp[2],vp[3],err,(unsigned long long)nz,(void*)CGLGetCurrentContext(),g_guest_gl_draws_x,g_guest_gl_idx_x); (void)!write(2,m,k);
        static int dumped; if(nz && !dumped && n>400 && w>0 && h>0 && w<=4096 && h<=4096){ dumped=1; unsigned char*c=malloc((size_t)w*h*4); if(c){ const uint8_t*q=d;
            for(long i=0;i<(long)w*h;i++){ uint8_t b0=q[i*4],b1=q[i*4+1],b2=q[i*4+2],a=q[i*4+3]; if(fmt==0x80E1){ c[i*4]=b2; c[i*4+1]=b1; c[i*4+2]=b0; } else { c[i*4]=b0; c[i*4+1]=b1; c[i*4+2]=b2; } c[i*4+3]=a; }
            const char*hm=getenv("HOME"); char pth[300]; snprintf(pth,sizeof pth,"%s/reclaimer_readpx.bmp",hm?hm:"/tmp"); bmp_write_rgba(pth,w,h,c);
            for(long i=0;i<(long)w*h;i++){ c[i*4]=c[i*4+1]=c[i*4+2]=c[i*4+3]; } snprintf(pth,sizeof pth,"%s/reclaimer_readpx_alpha.bmp",hm?hm:"/tmp"); bmp_write_rgba(pth,w,h,c); free(c); } } } } }
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
unsigned long g_guest_gl_draws_x, g_guest_gl_idx_x;   // guest (WGL) draw counters, reported by PELOAD_TRACE_READPX
#define g_guest_gl_draws g_guest_gl_draws_x
#define g_guest_gl_idx g_guest_gl_idx_x
static void MSABI i_glDrawElements(unsigned mode,int count,unsigned type,const void*idx){ void(*r)(unsigned,int,unsigned,const void*)=dlsym(RTLD_DEFAULT,"glDrawElements"); g_guest_gl_draws++; g_guest_gl_idx+=(unsigned long)count;
    static int st=-1; if(st<0) st=getenv("PELOAD_TRACE_GUESTDRAW")?1:0;
    if(st && g_guest_gl_draws>=600 && g_guest_gl_draws<612){ void(*gi)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv"); void(*gf)(unsigned,float*)=dlsym(RTLD_DEFAULT,"glGetFloatv"); unsigned(*ge)(void)=dlsym(RTLD_DEFAULT,"glGetError"); unsigned char(*ie)(unsigned)=dlsym(RTLD_DEFAULT,"glIsEnabled");
        unsigned e0=ge(); int fb=0,prog=0,vao=0,vp[4]={0},sc[4]={0},tex=0,ab=0,eb=0; gi(0x8CA6,&fb); gi(0x8B8D,&prog); gi(0x85B5,&vao); gi(0x0BA2,vp); gi(0x0C10,sc); gi(0x8069,&tex); gi(0x8894,&ab); gi(0x8895,&eb);
        int scis=ie(0x0C11), blend=ie(0x0BE2), depth=ie(0x0B71), cull=ie(0x0B44), srgb=ie(0x8DB9); int bs=0,bd=0; gi(0x80C9,&bs); gi(0x80C8,&bd); float cm[4]={0}; (void)gf;
        int loc=-1; int(*gul)(unsigned,const char*)=dlsym(RTLD_DEFAULT,"glGetUniformLocation"); void(*guf)(unsigned,int,float*)=dlsym(RTLD_DEFAULT,"glGetUniformfv"); float ss[4]={-1,-1,-1,-1};
        if(gul&&guf&&prog){ loc=gul((unsigned)prog,"u_screen_size"); if(loc>=0) guf((unsigned)prog,loc,ss); }
        unsigned char cmask[4]={0}; void(*gb)(unsigned,unsigned char*)=dlsym(RTLD_DEFAULT,"glGetBooleanv"); if(gb) gb(0x0C23,cmask);
        { void(*gbs)(unsigned,long,long,void*)=dlsym(RTLD_DEFAULT,"glGetBufferSubData"); float v[20]={0}; if(gbs&&ab){ gbs(0x8892,0,80,v);
            char m2[300]; int k2=snprintf(m2,sizeof m2,"  [guestvtx] #%lu v0 pos=(%g,%g) uv=(%g,%g) | v1 pos=(%g,%g) | v2 pos=(%g,%g) | v3 pos=(%g,%g)\n",g_guest_gl_draws,v[0],v[1],v[2],v[3],v[5],v[6],v[10],v[11],v[15],v[16]); (void)!write(2,m2,k2); } }
        if(r)r(mode,count,type,idx); unsigned e1=ge();
        char m[400]; int k=snprintf(m,sizeof m,"  [guestdraw] #%lu mode=%u n=%d type=0x%x idx=%p err-before=0x%x after=0x%x fb=%d prog=%d vao=%d tex=%d ab=%d eb=%d vp=%d,%d,%d,%d scissor=%d [%d,%d,%d,%d] blend=%d(src=0x%x dst=0x%x) depth=%d cull=%d srgb=%d cmask=%d%d%d%d u_screen_size=(%g,%g) loc=%d\n",
            g_guest_gl_draws,mode,count,type,idx,e0,e1,fb,prog,vao,tex,ab,eb,vp[0],vp[1],vp[2],vp[3],scis,sc[0],sc[1],sc[2],sc[3],blend,bs,bd,depth,cull,srgb,cmask[0],cmask[1],cmask[2],cmask[3],ss[0],ss[1],loc); (void)!write(2,m,k); return; }
    if(r)r(mode,count,type,idx);
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
            if(m&&(uint64_t)m>0x10000){ int ok=1; for(int i=0;i<64;i++){char c=m[i]; if(!c)break; if(c<32||c>126){ok=0;break;}} if(ok&&m[0]){ws("    what(): ");ws(m);ws("\n");break;} } }
        // every printable string the object holds (inline or pointed to, narrow or UTF-16): e.g. the paths
        // in a boost::filesystem error
        if(obj) for(int off=0; off<0x100; off+=8){ const uint8_t*q=(const uint8_t*)obj+off; uint8_t sb[2][400]; mach_vm_size_t got=0;
            uint64_t pv=0; if(mach_vm_read_overwrite(mach_task_self(),(mach_vm_address_t)(uintptr_t)q,8,(mach_vm_address_t)(uintptr_t)&pv,&got)!=KERN_SUCCESS) continue;
            const uint8_t*src[2]={q,(const uint8_t*)(uintptr_t)pv}; const uint8_t*cand[2]={0,0};
            for(int c=0;c<2;c++){ got=0; memset(sb[c],0,sizeof sb[c]); if((uint64_t)src[c]>=0x10000 && mach_vm_read_overwrite(mach_task_self(),(mach_vm_address_t)(uintptr_t)src[c],sizeof sb[c]-2,(mach_vm_address_t)(uintptr_t)sb[c],&got)==KERN_SUCCESS) cand[c]=sb[c]; }
            for(int c=0;c<2;c++){ const uint8_t*m=cand[c]; if(!m) continue;
                char out[200]; int k=0; if(m[1]==0 && m[0]>=32 && m[0]<127){ for(int i=0;i<180 && m[2*i] && !m[2*i+1];i++){ uint8_t ch=m[2*i]; if(ch<32||ch>126){k=0;break;} out[k++]=(char)ch; } }
                else { for(int i=0;i<180 && m[i];i++){ if(m[i]<32||m[i]>126){k=0;break;} out[k++]=(char)m[i]; } }
                if(k>=4){ out[k]=0; char t[260]; snprintf(t,sizeof t,"    obj+0x%x%s: \"%s\"\n",off,c?" ->":"",out); ws(t); } } } }
}
// MiniDumpWriteDump (dbghelp): the client's crash handler calls it with the exception that is about to end the
// process, then fast-fails. Log that exception (code, address as module+offset, C++ type) so every crash says
// what happened, even without PELOAD_TRACE_* switches; write no dump file.
static const char* addr_mod(uint64_t a,uint64_t*off){
    if(g_base){ OPT64*o=(OPT64*)((uint8_t*)g_base+((DOS*)g_base)->e_lfanew+sizeof(COFF)); if(a>=(uint64_t)g_base && a<(uint64_t)g_base+o->ImageSize){ *off=a-(uint64_t)g_base; return "exe"; } }
    for(int k=0;k<g_nmods;k++){ uint8_t*bs=g_mods[k].base; OPT64*o=(OPT64*)(bs+((DOS*)bs)->e_lfanew+sizeof(COFF)); if(a>=(uint64_t)bs && a<(uint64_t)bs+o->ImageSize){ *off=a-(uint64_t)bs; return g_mods[k].name; } }
    *off=a; return "?"; }
static int MSABI i_MiniDumpWriteDump(void*proc,uint32_t pid,void*file,uint32_t type,const uint8_t*exparam,void*us,void*cb){ (void)proc;(void)pid;(void)file;(void)type;(void)us;(void)cb;
    char m[400]; int k=snprintf(m,sizeof m,"  [crash-report] the client's crash handler is writing a minidump\n"); (void)!write(2,m,k);
    if(exparam){ const uint64_t*ep=*(const uint64_t* const*)(exparam+8);   // EXCEPTION_POINTERS*
        if(ep && ep[0]){ const uint8_t*rec=(const uint8_t*)(uintptr_t)ep[0]; uint32_t code=*(const uint32_t*)rec; uint64_t addr=*(const uint64_t*)(rec+16); uint32_t np=*(const uint32_t*)(rec+24);
            const uint64_t*info=(const uint64_t*)(rec+32); uint64_t off; const char*mod=addr_mod(addr,&off);
            k=snprintf(m,sizeof m,"  [crash-report] exception 0x%08x at %s+0x%llx (thread %u)",code,mod,(unsigned long long)off,*(const uint32_t*)exparam); (void)!write(2,m,k);
            if(code==0xC0000005 && np>=2){ k=snprintf(m,sizeof m,"  %s address 0x%llx",info[0]==1?"write":info[0]==8?"execute":"read",(unsigned long long)info[1]); (void)!write(2,m,k); }
            (void)!write(2,"\n",1);
            if(ep[1]){ const uint8_t*ctx=(const uint8_t*)(uintptr_t)ep[1]; uint64_t rsp=*(const uint64_t*)(ctx+0x98); int shown=0;
                for(int w=0;w<400 && shown<12;w++){ uint64_t v; mach_vm_size_t got=0; if(mach_vm_read_overwrite(mach_task_self(),(mach_vm_address_t)(rsp+8*w),8,(mach_vm_address_t)(uintptr_t)&v,&got)!=KERN_SUCCESS) break;
                    uint64_t o2; const char*mm=addr_mod(v,&o2); if(mm[0]!='?'){ k=snprintf(m,sizeof m,"      %s+0x%llx\n",mm,(unsigned long long)o2); (void)!write(2,m,k); shown++; } } }
            if(code==0xE06D7363 && np>=4){ setenv("PELOAD_TRACE_SEH","1",0); seh_decode_cpp(code,np,info); } } }
    return 0; }
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
// Every guest thread's TLS pointer array (thread_tramp's; never freed), so a DLL loaded later can give each
// EXISTING thread a block for its static-TLS index, as Windows does. Without it a thread that predates the
// DLL read NULL from ThreadLocalStoragePointer[idx] (WavesLibDLL.dll+0xC732, loaded by client v0.9.18).
static void** g_tarrs[1024]; static int g_ntarrs;
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
    pthread_mutex_lock(&g_tlsm);
    g_tls_array[idx]=block;              // main thread's block for this index
    if(g_ntlsmods<MAX_TLSMODS){ g_tlsmods[g_ntlsmods].src=(uint8_t*)t->Start; g_tlsmods[g_ntlsmods].raw=raw; g_tlsmods[g_ntlsmods].total=total; g_tlsmods[g_ntlsmods].index=idx; g_ntlsmods++; }
    for(int k=0;k<g_ntarrs;k++){ void**ta=g_tarrs[k]; if(!ta || ta==g_tls_array || ta[idx]) continue;   // existing threads
        uint8_t*b=malloc(total?total:1); if(raw) memcpy(b,(void*)t->Start,raw); if(total>raw) memset(b+raw,0,total-raw); ta[idx]=b; }
    pthread_mutex_unlock(&g_tlsm);
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
    { char nm[32]; snprintf(nm,sizeof nm,"guest-%p",(void*)t->fn); pthread_setname_np(nm); }   // SetThreadDescription renames it
    // Fresh per-thread block for EVERY module's TLS index (exe, halo3, CRT), not
    // just index 0 — otherwise halo3's thread-locals alias the exe's on new threads.
    pthread_mutex_lock(&g_tlsm);   // (same lock as register_tls: no module can slip in between)
    for(int i=0;i<g_ntlsmods;i++){ uint64_t tot=g_tlsmods[i].total, raw=g_tlsmods[i].raw;
        uint8_t *blk=malloc(tot?tot:1); if(raw) memcpy(blk,g_tlsmods[i].src,raw); if(tot>raw) memset(blk+raw,0,tot-raw);
        tarr[g_tlsmods[i].index]=blk; }
    if(g_ntarrs<(int)(sizeof g_tarrs/sizeof g_tarrs[0])) g_tarrs[g_ntarrs++]=tarr;
    pthread_mutex_unlock(&g_tlsm);
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
    done->owner=th; done->tid=getenv("PELOAD_TID_CONST")?(uint32_t)(uintptr_t)th:(uint32_t)pthread_mach_thread_np(th);   // = its GetCurrentThreadId()
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
#include "xaudio.c"
#include "hitbox.c"
static void MSABI i_endthread(void){ pthread_exit(0); }
static uint32_t MSABI i_GetThreadId(void*h){ if(is_sync((intptr_t)h)) return ((SyncObj*)h)->tid; if(h==(void*)-2) return i_GetCurrentThreadId(); return (uint32_t)(uintptr_t)h; }
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
static int MSABI i_RemoveDirectoryW(const uint16_t*w); // fwd
static int MSABI i_RemoveDirectoryA(const char*a){ uint16_t w[2048]; int i=0; for(; a&&a[i]&&i<2047;i++) w[i]=(uint8_t)a[i]; w[i]=0; return i_RemoveDirectoryW(w); }
static int MSABI i_RemoveDirectoryW(const uint16_t*w){ char p[2048]; win2unix(w,p,sizeof p);
    if(getenv("PELOAD_TRACE_DEL")){ static int n; if(n++<40){ char m[2200]; int k=snprintf(m,sizeof m,"  [RemoveDirectoryW] %s\n",p); (void)!write(2,m,k); } }
    if(rmdir(p)==0) return 1; rmrf(p); return 1; }
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
static int  MSABI i_MoveFileExW(const uint16_t*from,const uint16_t*to,uint32_t fl){ (void)fl; char a[2048],b[2048]; char A[2048],B[2048]; win2unix(from,A,sizeof A); if(!to){ int r=unlink(A); if(getenv("PELOAD_TRACE_FS")){ws("  [MoveFileEx del] ");ws(A);ws(r?" FAIL\n":" ok\n");} return r==0; } win2unix(to,B,sizeof B); (void)a;(void)b; int r=rename(A,B); if(r) snprintf(t_lastfail,sizeof t_lastfail,"MFX(%d):%s => %s",errno,A,B);
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
    { int rc=unlink(p); if(getenv("PELOAD_TRACE_DEL")){ static int n; if(n++<40){ char m[2200]; int k=snprintf(m,sizeof m,"  [DeleteFileW] %s -> %s\n",p,rc==0?"ok":strerror(errno)); (void)!write(2,m,k); } } }
    return 1; }
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
static void h3set_install(uint8_t*halo3base);   // fwd (PELOAD_H3 engine globals)
static void capcb1_install(uint8_t*halo3base); // fwd (PELOAD_D3DGL_CAPCB1 per-section decompression capture)
static void *export_lookup(uint8_t *base, const char *want);    // fwd
static void* MSABI i_LoadLibraryExW(const uint16_t *name, void *h, uint32_t fl);
// PELOAD_NO_WAVES: refuse MCC's Waves audio DLLs (halo3\waves\*.dll), so halo3 keeps the pass-through WavesFX
// table waves_prefill() installs (what client v0.9.13 effectively ran with).
static int no_waves_dll(const uint16_t*n){ static int on=-1; if(on<0) on=getenv("PELOAD_NO_WAVES")?1:0; if(!on||!n) return 0;
    char a[1024]; int i=0; for(; n[i]&&i<1023;i++) a[i]=(char)(n[i]<128?n[i]:'?'); a[i]=0; for(char*q=a;*q;q++) if(*q=='/') *q='\\';
    return strcasestr(a,"\\waves\\")!=0; }
static void* MSABI i_LoadLibraryExW(const uint16_t *name, void *h, uint32_t fl){
    if(no_waves_dll(name)){ g_lasterr=126; ws("  [no-waves] refused a Waves DLL\n"); return 0; }
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
// plain=1: "C:\dir\file" as Windows reports module paths (GetModuleFileName); plain=0: "\\?\C:\..." long-path form.
static void unix2win_ex(const char *u, uint16_t *out, int cap, int plain){
    char w[2048]; size_t dl=strlen(g_drivec); const char*pre=plain?"":"\\\\?\\";
    if(!strncmp(u,g_drivec,dl)) snprintf(w,sizeof w,"%sC:%s", pre, u+dl);
    else snprintf(w,sizeof w,"%sZ:%s", pre, u);
    for(char*p=w;*p;p++) if(*p=='/') *p='\\';
    int i=0; for(; w[i] && i<cap-1; i++) out[i]=(uint8_t)w[i]; out[i]=0;
}
static void unix2win(const char *u, uint16_t *out, int cap){ unix2win_ex(u,out,cap,0); }
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
static int    MSABI i_GetOverlappedResult(void*h,uint8_t*ov,uint32_t*pbytes,int wait){ (void)h;
    if(!ov){ if(pbytes)*pbytes=0; return 1; }
    uint64_t st=__atomic_load_n((uint64_t*)(ov+0),__ATOMIC_ACQUIRE);
    while(st==0x103){ if(!wait){ g_lasterr=996/*ERROR_IO_INCOMPLETE*/; return 0; } usleep(200); st=__atomic_load_n((uint64_t*)(ov+0),__ATOMIC_ACQUIRE); }
    if(pbytes)*pbytes=(uint32_t)*(uint64_t*)(ov+8);
    if(st==0xC000014BULL){ g_lasterr=109/*ERROR_BROKEN_PIPE*/; return 0; }
    if(st>=0xC0000000ULL){ g_lasterr=31; return 0; }
    return 1; }
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
static void* afd_thread_fn(void* a){ (void)a; pthread_setname_np("shim-afd");
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
    if(fd<0){ g_lasterr=win_err(); snprintf(t_lastfail,sizeof t_lastfail,"CFW:%s",p[0]?p:"<empty>");
        if(getenv("PELOAD_TRACE_CFWBT") && errno==ENOENT){ ws("  [CFW-ENOENT] "); ws(p); ws("\n    chain:");
            uint64_t *sp=(uint64_t*)__builtin_frame_address(0); int shown=0;
            for(int i=0;i<800 && shown<8;i++){ uint64_t v=sp[i]; if(v>=0x140001000ULL&&v<0x141aae000ULL){ char t[32]; snprintf(t,sizeof t," +0x%llx",(unsigned long long)(v-0x140000000ULL)); ws(t); shown++; } }
            ws("\n"); }
        return (void*)-1; } g_lasterr=0; // INVALID_HANDLE_VALUE
    fd_record(fd,p); hb_note_map(p);   // hitbox.c: remembers opened .map files (collision models for PELOAD_HITBOXES)
    if(fl&0x04000000) g_fddel[fd<MAXFD?fd:0]=1; // FILE_FLAG_DELETE_ON_CLOSE
    return (void*)(intptr_t)(0x1000+fd);
}
// Overlapped reads on PIPES/SOCKETS (anon pipes are socketpairs): they have no file offset, so pread()
// fails (ESPIPE) and the reader saw an error/EOF at once. Rust's async process I/O (the dedicated server
// supervising its game servers) reads children's stdout that way; it stopped reading, the child blocked on
// a full pipe, and the server never got past "preparing its map". Now: read() when data is waiting, else
// a helper thread completes the read when data arrives (poll). Completion: OVERLAPPED.Internal
// (STATUS_PENDING -> 0 / STATUS_PIPE_BROKEN at EOF), InternalHigh = bytes, then the hEvent and/or the
// handle's IOCP (hEvent low bit set = no IOCP packet, as on Windows).
static int fd_is_stream(int fd){ if(fd<0) return 0; if(fd<MAXFD && g_pipe_other[fd]) return 1; struct stat st;
    return fstat(fd,&st)==0 && (S_ISSOCK(st.st_mode)||S_ISFIFO(st.st_mode)||S_ISCHR(st.st_mode)); }
static void ovread_complete(int fd,uint8_t*ov,long k){
    void*ev=*(void**)(ov+24); int noport=((uintptr_t)ev)&1;
    *(uint64_t*)(ov+8)=(uint64_t)(k>0?k:0);
    __atomic_store_n((uint64_t*)(ov+0), k>0? 0ULL : k==0? 0xC000014BULL /*STATUS_PIPE_BROKEN*/ : 0xC0000001ULL, __ATOMIC_RELEASE);
    if(ev) i_SetEvent((void*)((uintptr_t)ev&~(uintptr_t)1));
    if(!noport) iocp_post_fd(fd,(uint64_t)(k>0?k:0),ov); }
typedef struct { int fd; void*buf; uint32_t n; uint8_t*ov; } OvRead;
static void* ovread_thread(void*a){ OvRead r=*(OvRead*)a; free(a); pthread_setname_np("shim-ovread");
    struct pollfd p={r.fd,POLLIN,0}; while(poll(&p,1,-1)<0 && errno==EINTR){}
    long k=read(r.fd,r.buf,r.n); ovread_complete(r.fd,r.ov,k); return 0; }
static int    MSABI i_ReadFile(void*h,void*buf,uint32_t n,uint32_t*rd,uint8_t*ov){
    intptr_t hv=(intptr_t)h;
    if(pipe_is(hv)) return pipe_read(h,buf,n,rd);
    if(hv<0x1000){ if(rd)*rd=0; return 0; } int fd=(int)(hv-0x1000);
    if(ov && fd_is_stream(fd)){
        *(uint64_t*)(ov+8)=0; __atomic_store_n((uint64_t*)(ov+0),0x103ULL,__ATOMIC_RELEASE);   // STATUS_PENDING
        struct pollfd p={fd,POLLIN,0};
        if(poll(&p,1,0)>0){ long k=read(fd,buf,n); ovread_complete(fd,ov,k); }
        else { OvRead*r=malloc(sizeof *r); pthread_t t; if(!r){ g_lasterr=8; return 0; } r->fd=fd; r->buf=buf; r->n=n; r->ov=ov;
               if(pthread_create(&t,0,ovread_thread,r)!=0){ free(r); g_lasterr=8; return 0; } pthread_detach(t); }
        if(rd)*rd=0; g_lasterr=997/*ERROR_IO_PENDING*/; return 0; }
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
    const char *val=(getenv)(nm); /* guest lookup: bypass the PELOAD_ cache */ if(getenv("PELOAD_TRACE_ENV")){ws("  [GetEnv] ");ws(nm);ws("=");ws(val?val:"(unset)");ws("\n");} if(!val){ g_lasterr=203; return 0; } g_lasterr=0; // ERROR_ENVVAR_NOT_FOUND so Rust env::var -> None, not Some("")
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
static uint32_t MSABI i_GetSystemDirectoryA(char*buf,uint32_t sz){ const char*s="C:\\Windows\\System32"; uint32_t n=(uint32_t)strlen(s); if(!buf||sz<=n) return n+1; memcpy(buf,s,n+1); return n; }
static uint32_t MSABI i_GetWindowsDirectoryA(char*buf,uint32_t sz){ const char*s="C:\\Windows"; uint32_t n=(uint32_t)strlen(s); if(!buf||sz<=n) return n+1; memcpy(buf,s,n+1); return n; }
// GetCurrentThreadStackLimits: this thread's real stack [low, high) (the guest runs on the pthread stack).
static void MSABI i_GetCurrentThreadStackLimits(uint64_t*lo,uint64_t*hi){ pthread_t t=pthread_self(); uint64_t top=(uint64_t)pthread_get_stackaddr_np(t); uint64_t sz=(uint64_t)pthread_get_stacksize_np(t);
    if(lo)*lo=top-sz; if(hi)*hi=top; }
static uint32_t MSABI i_SetHandleCount(uint32_t n){ return n; }
static void*    MSABI i_GetKeyboardLayout(uint32_t tid){ (void)tid; return (void*)(uintptr_t)0x04090409; }   // en-US
static int      MSABI i_MulDiv(int a,int b,int c){ if(!c) return -1; int64_t r=(int64_t)a*b; r = (r>=0)? (r+c/2)/c : (r-c/2)/c; return (int)r; }
static uint16_t MSABI i_GetThreadUILanguage(void){ return 0x409; }
static uint32_t MSABI i_GetWindowsDirectoryW(uint16_t*buf,uint32_t sz){ const char*s="C:\\Windows"; uint32_t i=0; for(; s[i]&&i<sz-1;i++) buf[i]=(uint8_t)s[i]; if(i<sz)buf[i]=0; if(getenv("PELOAD_TRACE_SPAWN"))ws("  [GetWindowsDirectoryW]\n"); return i; }
static void   MSABI i_GetLocalTime(uint16_t *stm){
    if(!stm) return; struct timeval tv; gettimeofday(&tv,0); time_t t=tv.tv_sec; struct tm lt; localtime_r(&t,&lt);
    stm[0]=lt.tm_year+1900; stm[1]=lt.tm_mon+1; stm[2]=lt.tm_wday; stm[3]=lt.tm_mday;
    stm[4]=lt.tm_hour; stm[5]=lt.tm_min; stm[6]=lt.tm_sec; stm[7]=tv.tv_usec/1000;
}
static void   MSABI i_GetSystemTime(uint16_t *stm){ i_GetLocalTime(stm); }
// CreateDirectoryW reports failures the Windows way: ERROR_ALREADY_EXISTS (183) or ERROR_PATH_NOT_FOUND (3)
// for a missing parent. It used to claim success always, so Rust's create_dir_all never created the
// parents ("Created content/Maps" with no folder on disk). PELOAD_MKDIR_LAX = the old always-TRUE behavior.
static int    MSABI i_CreateDirectoryW(const uint16_t*w,void*sec){ (void)sec; char p[2048]; if(mkdir(win2unix(w,p,sizeof p),0755)==0) return 1;
    static int lax=-1; if(lax<0) lax=getenv("PELOAD_MKDIR_LAX")?1:0; if(lax) return 1;
    g_lasterr = errno==EEXIST? 183 : errno==ENOENT? 3 : win_err(); return 0; }
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
static Find g_finds[256]; static int g_nfinds; static pthread_mutex_t g_find_m=PTHREAD_MUTEX_INITIALIZER;
// Claim a free find slot for an open DIR (FindClose frees it). Slots must be reused: the client
// scans directories thousands of times, and a table that only grows makes every later scan fail.
static Find* find_slot(DIR*d,const char*dir,const char*pat,int*idx){ pthread_mutex_lock(&g_find_m);
    int i=0; while(i<g_nfinds && g_finds[i].d) i++;
    if(i>=(int)(sizeof g_finds/sizeof g_finds[0])){ pthread_mutex_unlock(&g_find_m); return 0; }
    if(i==g_nfinds) g_nfinds++; Find*f=&g_finds[i]; f->d=d; pthread_mutex_unlock(&g_find_m);
    snprintf(f->dir,sizeof f->dir,"%s",dir); snprintf(f->pat,sizeof f->pat,"%s",pat); *idx=i; return f; }
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
    (void)lvl;(void)op;(void)filt;(void)fla;
    char p[2048]; win2unix(w,p,sizeof p);
    char *slash=strrchr(p,'/'); char dir[1536],pat[260];
    if(slash){ int dl=slash-p; if(dl>1535)dl=1535; memcpy(dir,p,dl); dir[dl]=0; snprintf(pat,sizeof pat,"%s",slash+1); }
    else { snprintf(dir,sizeof dir,"."); snprintf(pat,sizeof pat,"%s",p); }
    if(getenv("PELOAD_TRACE_FS")){ ws("  [FindFirst] dir="); ws(dir); ws(" pat="); ws(pat); ws("\n"); }
    DIR*d=opendir(dir); if(!d){ g_lasterr=win_err(); if(getenv("PELOAD_TRACE_FS")){ws("  [FindFirst] opendir FAIL ");ws(dir);ws("\n");} return (void*)-1; }
    struct dirent*de; while((de=readdir(d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(pat,de->d_name)){ if(getenv("PELOAD_TRACE_FS")){ws("  [FindFirst] -> ");ws(de->d_name);ws("\n");}
        int idx; Find*f=find_slot(d,dir,pat,&idx); if(!f){ closedir(d); g_lasterr=4; return (void*)-1; } // ERROR_TOO_MANY_OPEN_FILES
        fill_find(f,de,fd); return (void*)(intptr_t)(0x2000+idx); } }
    closedir(d); g_lasterr=2; if(getenv("PELOAD_TRACE_FS")){ws("  [FindFirst] EMPTY ");ws(dir);ws("\n");} return (void*)-1; // ERROR_FILE_NOT_FOUND
}
static int MSABI i_FindNextFileW(void*h,void*fd){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); if(idx<0||idx>=g_nfinds||!g_finds[idx].d) return 0;
    Find*f=&g_finds[idx]; struct dirent*de; while((de=readdir(f->d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(f->pat,de->d_name)){ fill_find(f,de,fd); return 1; } } g_lasterr=18; return 0; } // ERROR_NO_MORE_FILES
static int MSABI i_FindClose(void*h){ intptr_t hv=(intptr_t)h; int idx=(int)(hv-0x2000); pthread_mutex_lock(&g_find_m); if(idx>=0&&idx<g_nfinds&&g_finds[idx].d){ closedir(g_finds[idx].d); g_finds[idx].d=0; } pthread_mutex_unlock(&g_find_m); return 1; }
// ANSI variants: halo3 enumerates its maps/ with FindFirstFileA, so a stub here
// leaves the map catalog EMPTY ("guardian" never resolves). WIN32_FIND_DATAA has
// cFileName (CHAR[260]) at +44 instead of the wide WCHAR.
static void fill_find_a(Find*f,struct dirent*de,void*fd){ if(getenv("PELOAD_TRACE_FS")){ ws("  [findA] dir="); ws(f->dir); ws(" pat="); ws(f->pat); ws(" -> "); ws(de->d_name); ws("\n"); }
    uint8_t*d=fd; memset(d,0,320); char full[1800]; struct stat st;
    snprintf(full,sizeof full,"%s/%s",f->dir,de->d_name);
    uint32_t attr=0x80; if(stat(full,&st)==0){ attr=S_ISDIR(st.st_mode)?0x10:0x80; *(uint32_t*)(d+28)=(uint32_t)(st.st_size>>32); *(uint32_t*)(d+32)=(uint32_t)st.st_size; }
    *(uint32_t*)d=attr; char*nm=(char*)(d+44); int i=0; for(; de->d_name[i]&&i<259;i++) nm[i]=de->d_name[i]; nm[i]=0; }
static void* MSABI i_FindFirstFileA(const char*p0,void*fd){
    char p[2048]; { int i=0; for(; p0&&p0[i]&&i<2047;i++) p[i]=(p0[i]=='\\')?'/':p0[i]; p[i]=0; }
    char *slash=strrchr(p,'/'); char dir[1536],pat[260];
    if(slash){ int dl=slash-p; if(dl>1535)dl=1535; memcpy(dir,p,dl); dir[dl]=0; snprintf(pat,sizeof pat,"%s",slash+1); }
    else { snprintf(dir,sizeof dir,"."); snprintf(pat,sizeof pat,"%s",p); }
    DIR*d=opendir(dir); if(!d){ g_lasterr=win_err(); return (void*)-1; }
    struct dirent*de; while((de=readdir(d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; if(pat_match(pat,de->d_name)){
        int idx; Find*f=find_slot(d,dir,pat,&idx); if(!f){ closedir(d); g_lasterr=4; return (void*)-1; }
        fill_find_a(f,de,fd); return (void*)(intptr_t)(0x2000+idx); } }
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
static void* MSABI i_LoadLibraryA(const char*a){ return i_LoadLibraryExA(a,0,0); }
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
static void* MSABI i_GetFocus(void){ return fg_hwnd(); }   // the one logical window
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
        {"LoadLibraryExA",(void*)i_LoadLibraryExA},{"LoadLibraryA",(void*)i_LoadLibraryA},
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
        {"GetWindowsDirectoryW",(void*)i_GetWindowsDirectoryW},{"GetWindowsDirectoryA",(void*)i_GetWindowsDirectoryA},{"GetSystemDirectoryA",(void*)i_GetSystemDirectoryA},
        {"GetCurrentThreadStackLimits",(void*)i_GetCurrentThreadStackLimits},{"SetHandleCount",(void*)i_SetHandleCount},{"GetKeyboardLayout",(void*)i_GetKeyboardLayout},
        {"MulDiv",(void*)i_MulDiv},{"GetThreadUILanguage",(void*)i_GetThreadUILanguage},{"RemoveDirectoryA",(void*)i_RemoveDirectoryA},
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
        {"GetStartupInfoW",(void*)i_GetStartupInfoW},{"MiniDumpWriteDump",(void*)i_MiniDumpWriteDump},{"GetStartupInfoA",(void*)i_GetStartupInfoA},{"HeapSetInformation",(void*)i_HeapSetInformation},
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
        {"AdjustWindowRectExForDpi",(void*)i_AdjustWindowRectExForDpi},
        {"GetDpiForWindow",(void*)i_GetDpiForWindow},
        {"GetDpiForSystem",(void*)i_GetDpiForSystem},
        {"IsProcessDPIAware",(void*)i_IsProcessDPIAware},
        {"GetDpiForMonitor",(void*)i_GetDpiForMonitor},
        {"GetScaleFactorForMonitor",(void*)i_GetScaleFactorForMonitor},
        {"SetProcessDpiAwareness",(void*)i_SetProcessDpiAwareness},
        {"SetProcessDpiAwarenessContext",(void*)i_SetProcessDpiAwarenessContext},
        {"GetDeviceCaps",(void*)i_GetDeviceCaps},
        {"MoveWindow",(void*)i_MoveWindow},
        {"SetFocus",(void*)i_SetFocus},
        {"SetCapture",(void*)i_SetCapture},
        {"ReleaseCapture",(void*)i_ReleaseCapture},
        {"ClientToScreen",(void*)i_ClientToScreen},
        {"ScreenToClient",(void*)i_ScreenToClient},
        {"RegisterRawInputDevices",(void*)wi_RegisterRawInputDevices},
        {"GetRawInputData",(void*)wi_GetRawInputData},{"GetRawInputDeviceInfoW",(void*)wi_GetRawInputDeviceInfoW},{"GetKeyboardState",(void*)wi_GetKeyboardState},{"GetCursorPos",(void*)wi_GetCursorPos},
        {"GetRegisteredRawInputDevices",(void*)i_GetRegisteredRawInputDevices},
        {"SetTimer",(void*)i_SetTimer},
        {"KillTimer",(void*)i_KillTimer},
        {"GetAsyncKeyState",(void*)wi_GetAsyncKeyState},
        {"GetKeyState",(void*)wi_GetKeyState},
        {"ShowCursor",(void*)wi_ShowCursor},
        {"ClipCursor",(void*)wi_ClipCursor},
        {"SetCursorPos",(void*)wi_SetCursorPos},
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
        {"XInputGetState",(void*)i_XInputGetState},{"XInputSetState",(void*)i_XInputSetState},
        {"XInputGetCapabilities",(void*)i_XInputGetCapabilities},{"XInputEnable",(void*)i_XInputEnable},
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
    // An import from another GUEST DLL that is already loaded (Waves' l360.dll/q10.dll import C++ classes from
    // WavesLibDLL.dll, loaded just before them) binds to that DLL's real export.
    for(int m=0;m<g_nmods;m++){ const char*mn=g_mods[m].name; if(!mn) continue; const char*mb=mn; for(const char*q=mn;*q;q++) if(*q=='\\'||*q=='/') mb=q+1;
        if(!strcasecmp(mb,dll)){ void *g=export_lookup(g_mods[m].base, fn); if(g) return g; break; } }
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
            const char *fn = (oft[i]>>63)? 0 : (char*)(base+(uint32_t)oft[i]+2);
            if(!fn){ char ob[24]; snprintf(ob,sizeof ob,"#%u",(unsigned)(oft[i]&0xffff)); fn=strdup(ob); }   // ordinal (named in stub logs)
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
            const char *fn = (intb[i]>>63)? 0 : (char*)(base+(uint32_t)intb[i]+2);
            if(!fn){ char ob[24]; snprintf(ob,sizeof ob,"#%u",(unsigned)(intb[i]&0xffff)); fn=strdup(ob); }
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
    if(g_nmods<(int)(sizeof g_mods/sizeof g_mods[0])){ g_mods[g_nmods].name=name; g_mods[g_nmods].base=base; g_mods[g_nmods].path=strdup(path); g_nmods++; }
    // Static TLS for this DLL (halo3, CRT): give it its own index + per-thread block
    // BEFORE its DllMain/TLS callbacks run, so its __declspec(thread) globals resolve
    // to its own data instead of aliasing the exe's TLS block (index 0).
    if(opt->Dir[DIR_TLS].VA) register_tls((void*)(base+opt->Dir[DIR_TLS].VA), base, -1);
    if(strcasestr(name,"halo3")){ waves_prefill(base); patch_install(); hook_install(); skinguard_install(base); vtguard_install(base); capcb1_install(base); h3set_install(base); } // patches can target halo3 now it's mapped
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
    // PELOAD_TRACE_FAULT: log every hardware fault BEFORE the guest's handlers see it (RIP attributed to
    // exe/halo3/shim + a scan of the stack for return addresses), even when a guest handler swallows it.
    if(getenv("PELOAD_TRACE_FAULT")){ static int nf; if(nf++<6){ uint64_t rip=CTX_RIP(ctx), rsp=CTX_REG(ctx,4); char m[300]; Dl_info di;
        const char*mod="?"; uint64_t off=rip; if(rip>=0x140000000ULL&&rip<0x150000000ULL){mod="exe";off=rip-0x140000000ULL;} else if(rip>=0x180000000ULL&&rip<0x190000000ULL){mod="halo3";off=rip-0x180000000ULL;}
        else if(dladdr((void*)rip,&di)&&di.dli_sname){ mod=di.dli_sname; off=rip-(uint64_t)di.dli_saddr; }
        int k=snprintf(m,sizeof m,"  [fault] code=0x%x rip=%s+0x%llx addr=0x%llx rax=%llx rcx=%llx rdx=%llx r8=%llx r14=%llx\n",code,mod,(unsigned long long)off,(unsigned long long)rec->ExceptionInformation[1],
            (unsigned long long)CTX_REG(ctx,0),(unsigned long long)CTX_REG(ctx,1),(unsigned long long)CTX_REG(ctx,2),(unsigned long long)CTX_REG(ctx,8),(unsigned long long)CTX_REG(ctx,14)); (void)!write(2,m,k);
        for(int i=0;i<48;i++){ uint64_t v=*(uint64_t*)(rsp+8*i); const char*mm=0; uint64_t o=0;
            if(v>=0x140000000ULL&&v<0x150000000ULL){mm="exe";o=v-0x140000000ULL;} else if(v>=0x180000000ULL&&v<0x190000000ULL){mm="halo3";o=v-0x180000000ULL;}
            else if(v>0x100000000ULL && v<0x200000000ULL && dladdr((void*)v,&di)&&di.dli_sname&&strstr(di.dli_fname?di.dli_fname:"","peload")){ mm=di.dli_sname; o=v-(uint64_t)di.dli_saddr; }
            if(mm){ k=snprintf(m,sizeof m,"     [rsp+0x%x] %s+0x%llx\n",i*8,mm,(unsigned long long)o); (void)!write(2,m,k); } } } }
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
// ---- Reclaimer client patches located by BYTE SIGNATURE (works across client builds) ----
// Each patch is a masked byte pattern ("??" = any byte: relative branch/call targets, RIP-relative and
// stack displacements) that must match exactly once in the exe's executable sections, plus the edits
// at offsets inside the match (the original bytes are checked first). Found in v0.9.13 and v0.9.18.
//   ui-reentrancy  (on by default; PELOAD_NO_REENTRANCY_FIX disables) ui_composition's once-per-second
//                  cache cleanup borrows the app-state RefCell while the outer borrow is still held on our
//                  first-call path and panics ("RefCell already borrowed"); NOP the borrow-conflict branch
//                  and the drop dispatch so it takes the no-drop path that just releases the borrow.
//   xaudio         the client's XAudio2 integrity check (4 halo3 callback slots must point into the retail
//                  XAudio2) fails against our XAudio2 shim; NOP its 4 mismatch branches.
//   watchdog       the worker's 45 s startup deadline parks the input thread ("lost control"); never take it.
// PELOAD_RECLAIMER_PATCHES=all | comma list of names enables the optional ones (play.sh: all).
typedef struct { const char*name; int dflt; const char*sig; int n; int off[2]; const char*orig[2]; const char*repl[2]; } SigPatch;
static const SigPatch g_sigpatches[]={
  {"ui-reentrancy",1,"C6 43 68 01 48 83 3B 00 0F 85 ?? ?? ?? ?? 48 C7 03 FF FF FF FF 81 7B 10 00 CA 9A 3B 75 ?? 31 C0 EB ??",
     2,{8,0x1c},{"0F 85","75"},{"90 90 90 90 90 90","90 90"}},
  {"xaudio",0,"48 8B 44 24 20 48 89 C1 48 F7 D9 0F 81 ?? ?? ?? ?? 48 8D 83 16 C4 60 00 48 39 44 24 28 0F 85 ?? ?? ?? ?? 4C 8D B3 18 9E A3 00",
     1,{0x1d},{"0F 85"},{"90 90 90 90 90 90"}},
  {"xaudio",0,"48 8B 44 24 20 48 89 C1 48 F7 D9 0F 81 ?? ?? ?? ?? 48 8D 83 22 C4 60 00 48 39 44 24 28 0F 85 ?? ?? ?? ?? 4C 8D BB 20 9E A3 00",
     1,{0x1d},{"0F 85"},{"90 90 90 90 90 90"}},
  {"xaudio",0,"48 8B 44 24 20 48 89 C1 48 F7 D9 0F 81 ?? ?? ?? ?? 48 8D 83 0A C4 60 00 48 39 44 24 28 0F 85 ?? ?? ?? ?? 4C 8D A3 28 9E A3 00",
     1,{0x1d},{"0F 85"},{"90 90 90 90 90 90"}},
  {"xaudio",0,"48 8B 44 24 20 48 89 C1 48 F7 D9 71 ?? 48 81 C3 85 C3 60 00 48 39 5C 24 28 75 ?? 48 8D 05",
     1,{0x19},{"75"},{"90 90"}},
  {"watchdog",0,"81 BD ?? ?? ?? ?? 00 CA 9A 3B 74 ?? E8 ?? ?? ?? ?? 90 48 8B 8D ?? ?? ?? ?? 48 39 C8 75 ?? 3B 95 ?? ?? ?? ?? 72 ??",
     1,{0x0a},{"74"},{"EB"}},
};
static int sig_parse(const char*s,uint8_t*b,uint8_t*m,int cap){ int n=0; while(*s && n<cap){ while(*s==' ')s++; if(!*s)break;
    if(s[0]=='?'){ b[n]=0; m[n]=0; } else { char h[3]={s[0],s[1],0}; b[n]=(uint8_t)strtoul(h,0,16); m[n]=1; } n++; s+=2; } return n; }
// unique match of the pattern in the image's executable sections: rva, -1 none, -2 ambiguous
static int64_t sig_find(uint8_t*base,const uint8_t*b,const uint8_t*m,int n){
    uint32_t pe=*(uint32_t*)(base+0x3c); uint16_t ns=*(uint16_t*)(base+pe+6), so=*(uint16_t*)(base+pe+20); uint8_t*st=base+pe+24+so; int64_t hit=-1;
    for(int i=0;i<ns;i++){ uint8_t*sh=st+i*40; uint32_t vs=*(uint32_t*)(sh+8), va=*(uint32_t*)(sh+12), ch=*(uint32_t*)(sh+36);
        if(!(ch&0x20000000)) continue;   // IMAGE_SCN_MEM_EXECUTE
        uint8_t*p=base+va; for(uint32_t k=0;k+(uint32_t)n<=vs;k++){ if(p[k]!=b[0]) continue; int j=1; for(;j<n;j++) if(m[j] && p[k+j]!=b[j]) break;
            if(j==n){ if(hit>=0) return -2; hit=(int64_t)va+k; } } }
    return hit; }
static int sig_enabled(const SigPatch*sp){ const char*e=getenv("PELOAD_RECLAIMER_PATCHES");
    if(!strcmp(sp->name,"ui-reentrancy") && getenv("PELOAD_NO_REENTRANCY_FIX")) return 0;
    if(e && (!strcmp(e,"all") || strstr(e,sp->name))) return 1; return sp->dflt; }
static void reentrancy_fix(void){ if(!g_base) return;   // (name kept: applies every enabled signature patch)
    for(size_t i=0;i<sizeof g_sigpatches/sizeof g_sigpatches[0];i++){ const SigPatch*sp=&g_sigpatches[i]; if(!sig_enabled(sp)) continue;
        uint8_t b[128],m[128]; int n=sig_parse(sp->sig,b,m,128); int64_t rva=sig_find(g_base,b,m,n); char msg[200]; int k;
        if(rva<0){ k=snprintf(msg,sizeof msg,"  [sigpatch] %s: signature %s -- NOT applied (unsupported client build?)\n",sp->name,rva==-1?"not found":"ambiguous"); (void)!write(2,msg,k); continue; }
        int ok=1; for(int e=0;e<sp->n;e++){ uint8_t ob[16],om[16],rb[16],rm[16]; int on=sig_parse(sp->orig[e],ob,om,16); (void)rm;
            if(memcmp(g_base+rva+sp->off[e],ob,on)) ok=0; (void)sig_parse(sp->repl[e],rb,rm,16); }
        if(!ok){ k=snprintf(msg,sizeof msg,"  [sigpatch] %s: unexpected bytes at exe+0x%llx -- NOT applied\n",sp->name,(unsigned long long)rva); (void)!write(2,msg,k); continue; }
        for(int e=0;e<sp->n;e++){ uint8_t rb[16],rm[16]; int rn=sig_parse(sp->repl[e],rb,rm,16); apply_patch(g_base,(uint64_t)rva+sp->off[e],rb,rn); }
        k=snprintf(msg,sizeof msg,"  [sigpatch] %s @ exe+0x%llx\n",sp->name,(unsigned long long)(rva+sp->off[0])); (void)!write(2,msg,k); } }
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
    // decompression-capture mode (id==0xdec0): at sub_18028e8e0 entry rcx=idx (float* compression_info).
    // Build the 48-byte decompression buffer the engine would (extent=max-min, min as offset, uv bounds)
    // and stash it so the D3DGL DECOMP path can bind it as the real per-section cb1 (PELOAD_D3DGL_CAPCB1).
    if(id==0xdec0){
        uint64_t ix=rcx;
        if(hook_safe(ix) && hook_safe(ix+44)){
            const float*f=(const float*)ix;
            float b[12];
            b[0]=f[2]-f[1]; b[1]=f[4]-f[3]; b[2]=f[6]-f[5]; b[3]=1.0f;
            b[4]=f[1];      b[5]=f[3];      b[6]=f[5];      b[7]=0.0f;
            b[8]=f[8]-f[7]; b[9]=f[10]-f[9]; b[10]=f[7];    b[11]=f[9];
            int ok=1; for(int i=0;i<12;i++){ float v=b[i]; if(v!=v||v<-1e9f||v>1e9f){ok=0;break;} }
            if(ok && (b[0]>1e-4f||b[1]>1e-4f||b[2]>1e-4f)){   // non-degenerate position extent
                for(int i=0;i<12;i++) g_decomp_cap[i]=b[i];
                g_decomp_cap_seq++; g_decomp_cap_valid=1;
                static int capdbg=-1; if(capdbg<0) capdbg=getenv("PELOAD_D3DGL_CAPDBG")?1:0;
                if(capdbg){ static int c; if(c++<12){ char m[220]; int k=snprintf(m,sizeof m,
                    "  [capcb1] scale=[%.2f %.2f %.2f] off=[%.2f %.2f %.2f] tc=[%.3f %.3f %.3f %.3f] ret=halo3+0x%llx\n",
                    b[0],b[1],b[2],b[4],b[5],b[6],b[8],b[9],b[10],b[11],
                    (unsigned long long)(ret>=0x180000000ULL&&ret<0x187000000ULL?ret-0x180000000ULL:ret)); (void)!write(2,m,k);} }
            }
        }
        return;
    }
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
// ---- Per-section vertex-decompression capture (PELOAD_D3DGL_CAPCB1) ----------
// Entry detour (via the generic trace-hook trampoline, id 0xdec0) on sub_18028e8e0, the function the
// engine calls per BSP section to build the decompression constant buffer from the mesh bounding box.
// The handler (hook_log id==0xdec0) reads rcx=compression_info and stashes the real scale/offset/uv so
// the D3DGL path binds exact per-section cb1 instead of a uniform guess -> kills the stretched spikes.
// First 14 bytes of the prologue (push rbp;mov rbp,rsp;sub rsp,0x50;movss xmm2,[rcx+4]) are whole,
// non-rip-relative instructions -> safely relocatable.
static void capcb1_install(uint8_t*halo3base){ if(!getenv("PELOAD_D3DGL_CAPCB1")) return;
    hook_one(halo3base,0x28e8e0,14,0xdec0);
    char m[80]; int k=snprintf(m,sizeof m,"  [capcb1] per-section decomp capture installed @halo3+0x28e8e0\n"); (void)!write(2,m,k); }
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
// Second site, same shared_ptr control-block release (halo3+0x172e88, called from the sweep at 0x172c49): a
// player joining a match in progress faulted at RVA 0x172FA0 (`call [rax]` with a NULL vtable) a few
// seconds after the map was ready. mov rax,[rbx] / mov rcx,rbx / call [rax] / mov eax,edi / lock xadd
// [rbx+0xc],eax = 15 bytes to the boundary at 0x172fa9; skip to the epilogue at 0x172fb7.
// ---- PELOAD_H3="name=value[,name=value...]": set halo3 engine (HaloScript) globals at launch ----
// halo3.dll keeps a table of its HaloScript globals: 24-byte entries {const char* name; u64 type; void* storage}.
// In the retail MCC build only ~220 of ~1250 still have storage (the debug_objects_* overlays, among many others,
// are compiled out: storage 0). Types: 5 bool, 6 real, 7 short, 8 long. The value is written at halo3 load and
// re-written every 0.5 s (the engine may reset some during init). List: shim/docs/halo3-engine-globals.md.
typedef struct { volatile uint8_t*addr; int type; double v; char name[96]; } H3Set;
static H3Set g_h3set[32]; static int g_nh3set;
static void h3set_write(const H3Set*e){
    switch(e->type){ case 5: *(volatile uint8_t*)e->addr=(uint8_t)(e->v!=0); break; case 6: *(volatile float*)e->addr=(float)e->v; break;
        case 7: *(volatile int16_t*)e->addr=(int16_t)e->v; break; case 8: *(volatile int32_t*)e->addr=(int32_t)e->v; break; } }
static void* h3set_thread(void*a){ (void)a; pthread_setname_np("shim-h3set"); for(;;){ for(int i=0;i<g_nh3set;i++) h3set_write(&g_h3set[i]); usleep(500000); } return 0; }
static void h3set_install(uint8_t*base){ const char*e=getenv("PELOAD_H3"); if(!e||!*e) return;
    OPT64*o=(OPT64*)(base+((DOS*)base)->e_lfanew+sizeof(COFF)); uint64_t isz=o->ImageSize;
    char buf[2048]; snprintf(buf,sizeof buf,"%s",e);
    for(char*tok=strtok(buf,",; "); tok && g_nh3set<32; tok=strtok(0,",; ")){
        char*eq=strchr(tok,'='); if(!eq) continue; *eq=0; const char*nm=tok; double v=atof(eq+1);
        if(!strcasecmp(eq+1,"true")||!strcasecmp(eq+1,"on")) v=1;
        // the name string ("\0name\0"), then the table entry pointing at it
        char pat[128]; int pl=snprintf(pat,sizeof pat,"%c%s",0,nm)+1;   // includes the terminating NUL
        uint8_t*str=memmem(base,isz,pat,pl); char m[200]; int k;
        if(!str){ k=snprintf(m,sizeof m,"  [h3] %s: no such engine global\n",nm); (void)!write(2,m,k); continue; }
        uint64_t sva=(uint64_t)(str+1); uint8_t*ent=memmem(base,isz,&sva,8);
        if(!ent){ k=snprintf(m,sizeof m,"  [h3] %s: not in the globals table\n",nm); (void)!write(2,m,k); continue; }
        uint64_t ty=*(uint64_t*)(ent+8), ad=*(uint64_t*)(ent+16);
        if(!ad){ k=snprintf(m,sizeof m,"  [h3] %s: compiled out of this halo3.dll (no storage)\n",nm); (void)!write(2,m,k); continue; }
        if(ty<5||ty>8){ k=snprintf(m,sizeof m,"  [h3] %s: unsupported type %llu\n",nm,(unsigned long long)ty); (void)!write(2,m,k); continue; }
        H3Set*h=&g_h3set[g_nh3set++]; h->addr=(volatile uint8_t*)ad; h->type=(int)ty; h->v=v; snprintf(h->name,sizeof h->name,"%s",nm); h3set_write(h);
        k=snprintf(m,sizeof m,"  [h3] %s = %g (%s @ halo3+0x%llx)\n",nm,v,ty==5?"bool":ty==6?"real":ty==7?"short":"long",(unsigned long long)(ad-(uint64_t)base)); (void)!write(2,m,k); }
    if(g_nh3set){ pthread_t t; pthread_create(&t,0,h3set_thread,0); } }
static void vtguard_install(uint8_t*halo3base){ if(!getenv("PELOAD_VTGUARD")) return; vtguard_one(halo3base,0x172bce,17,0x172bee);
    vtguard_one(halo3base,0x172f9a,15,0x172fb7); }
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
    // PELOAD_PEEK=<hexaddr>:<hexlen>[,...]: also hex-dump guest memory (e.g. engine job/queue state).
    { const char*pk=getenv("PELOAD_PEEK"); while(pk && *pk){ char*e; uint64_t a=strtoull(pk,&e,16); uint64_t l=(*e==':')?strtoull(e+1,&e,16):0x40;
        for(uint64_t o=0;o<l;o+=16){ char b[160]; int k=snprintf(b,sizeof b,"  [peek] %llx:",(unsigned long long)(a+o));
            for(int j=0;j<16;j+=4){ uint64_t q=a+o+j; if(!hook_safe(q)){ k+=snprintf(b+k,sizeof b-k," ????????"); continue; } k+=snprintf(b+k,sizeof b-k," %08x",*(uint32_t*)q); }
            b[k++]='\n'; (void)!write(2,b,k); }
        pk=strchr(e,','); if(pk) pk++; } }
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
            { static long cap=-1; if(cap<0){ const char*e=getenv("PELOAD_BP_ARGS"); cap=e?atol(e):0; }
              { const char*bm=getenv("PELOAD_BP_MEM"); if(bm && g_bp_cnt[i]<=3){   // PELOAD_BP_MEM=<rbp|rsp|rcx|rdx|r12>+<hexoff>: dump 16 dwords there
                  uint64_t base=0; const char*pl=strchr(bm,'+'); uint64_t off=pl?strtoull(pl+1,0,16):0;
                  if(!strncmp(bm,"rbp",3)) base=uc->uc_mcontext->__ss.__rbp; else if(!strncmp(bm,"rsp",3)) base=uc->uc_mcontext->__ss.__rsp; else if(!strncmp(bm,"rcx",3)) base=uc->uc_mcontext->__ss.__rcx;
                  else if(!strncmp(bm,"rdx",3)) base=uc->uc_mcontext->__ss.__rdx; else if(!strncmp(bm,"r12",3)) base=uc->uc_mcontext->__ss.__r12;
                  if(base>0x10000){ const uint32_t*q=(const uint32_t*)(base+off); char b[900]; int k=snprintf(b,sizeof b,"[bpm] exe+0x%llx %s: ",(unsigned long long)(hit-(uint64_t)g_base),bm);
                      for(int z=0;z<16;z++){ float f; memcpy(&f,&q[z],4); k+=snprintf(b+k,sizeof b-k,"+%x=%08x(%g) ",z*4,q[z],f); } b[k++]='\n'; (void)!write(2,b,k); } } }
              if(cap>0 && g_bp_cnt[i]<=(uint64_t)cap){ float x0,x1,x2,x3; memcpy(&x0,&uc->uc_mcontext->__fs.__fpu_xmm0,4); memcpy(&x1,&uc->uc_mcontext->__fs.__fpu_xmm1,4); memcpy(&x2,&uc->uc_mcontext->__fs.__fpu_xmm2,4); memcpy(&x3,&uc->uc_mcontext->__fs.__fpu_xmm3,4);
                char xb[120]; int xk=snprintf(xb,sizeof xb,"[bpx] xmm0..3 = %g %g %g %g\n",x0,x1,x2,x3); (void)!write(2,xb,xk);
                char b[200]; int k=snprintf(b,sizeof b,"[bpa] exe+0x%llx #%llu rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx\n",(unsigned long long)(hit-(uint64_t)g_base),(unsigned long long)g_bp_cnt[i],
                    (unsigned long long)uc->uc_mcontext->__ss.__rcx,(unsigned long long)uc->uc_mcontext->__ss.__rdx,(unsigned long long)uc->uc_mcontext->__ss.__r8,(unsigned long long)uc->uc_mcontext->__ss.__r9); (void)!write(2,b,k); return; } }
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
      char tn[64]={0}; pthread_getname_np(pthread_self(),tn,sizeof tn);
      int nn=snprintf(b,sizeof b,"    gs:0x58(tls_array)=0x%llx  tls[0]=0x%llx  mach_tid=0x%x  mainthr=%d  thread='%s'\n",
        (unsigned long long)gs58,(unsigned long long)t0,pthread_mach_thread_np(pthread_self()),pthread_main_np(),tn);
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
// ---- PELOAD_SAMPLE=<us>: in-process sampling profiler (macOS `sample` can't symbolize Rosetta code) ----
// Every <us> microseconds each other thread is suspended and its x86 RIP read (thread_get_state works for
// translated processes); every 10 s the top hotspots are printed: guest code as exe+RVA / halo3+RVA,
// shim and system code via dladdr (shim statics: resolve offline with atos -o shim/build/peload).
typedef struct { uint64_t rip; uint32_t cnt; uint32_t th; } SmpEnt;
static void* sampler_thread(void*a){ (void)a; pthread_setname_np("shim-sampler");
    int us=atoi(getenv("PELOAD_SAMPLE")); if(us<200) us=1000;
    static SmpEnt tab[65536]; uint64_t total=0; double tlast=mono_s();
    static struct { thread_t t; uint64_t n, busy; } thr[64]; int nthr=0;
    mach_port_t self=mach_thread_self();
    for(;;){
        thread_act_array_t th; mach_msg_type_number_t n=0;
        if(task_threads(mach_task_self(),&th,&n)==KERN_SUCCESS){
            for(mach_msg_type_number_t i=0;i<n;i++){ thread_t t=th[i];
                if(t!=self && thread_suspend(t)==KERN_SUCCESS){
                    x86_thread_state64_t st; mach_msg_type_number_t c=x86_THREAD_STATE64_COUNT;
                    int ok=thread_get_state(t,x86_THREAD_STATE64,(thread_state_t)&st,&c)==KERN_SUCCESS;
                    thread_resume(t);
                    if(ok){ uint64_t rip=st.__rip; int ti=-1; for(int k=0;k<nthr;k++) if(thr[k].t==t){ ti=k; break; } if(ti<0 && nthr<64){ ti=nthr++; thr[ti].t=t; thr[ti].n=thr[ti].busy=0; }
                        Dl_info di; int idle=0; if(dladdr((void*)rip,&di) && di.dli_sname && (strstr(di.dli_sname,"mach_msg")||strstr(di.dli_sname,"__psynch")||strstr(di.dli_sname,"__semwait")||strstr(di.dli_sname,"nanosleep")||strstr(di.dli_sname,"__select")||strstr(di.dli_sname,"kevent")||strstr(di.dli_sname,"__wait4")||strstr(di.dli_sname,"__workq")||strstr(di.dli_sname,"pid_for_task")||!strcmp(di.dli_sname,"poll")||strstr(di.dli_sname,"__sigsuspend"))) idle=1;
                        if(ti>=0){ thr[ti].n++; if(!idle) thr[ti].busy++; }
                        if(!idle){ uint64_t key=rip>>4; uint32_t h=(uint32_t)(key*2654435761u)&65535;
                            for(int pr=0;pr<64;pr++){ SmpEnt*e=&tab[(h+pr)&65535]; if(e->rip==0){ e->rip=rip&~15ULL; e->cnt=1; e->th=ti; break; } if(e->rip==(rip&~15ULL)){ e->cnt++; break; } }
                            total++; } } }
                mach_port_deallocate(mach_task_self(),t); }
            vm_deallocate(mach_task_self(),(vm_address_t)th,n*sizeof(thread_t)); }
        usleep(us);
        if(mono_s()-tlast>=10.0){ tlast=mono_s();
            char m[256]; int k=snprintf(m,sizeof m,"  [sample] ---- %llu busy samples; threads (busy/total):",(unsigned long long)total); (void)!write(2,m,k);
            for(int i=0;i<nthr;i++) if(thr[i].busy>20){ k=snprintf(m,sizeof m," t%d=%llu/%llu",i,(unsigned long long)thr[i].busy,(unsigned long long)thr[i].n); (void)!write(2,m,k); } (void)!write(2,"\n",1);
            for(int top=0;top<40;top++){ int bi=-1; uint32_t bc=0; for(int i=0;i<65536;i++) if(tab[i].cnt>bc){ bc=tab[i].cnt; bi=i; }
                if(bi<0||bc<5) break; uint64_t rip=tab[bi].rip; Dl_info di; char where[160];
                if(rip>=0x140000000ULL&&rip<0x150000000ULL) snprintf(where,sizeof where,"exe+0x%llx",(unsigned long long)(rip-0x140000000ULL));
                else if(rip>=0x180000000ULL&&rip<0x190000000ULL) snprintf(where,sizeof where,"halo3+0x%llx",(unsigned long long)(rip-0x180000000ULL));
                else if(dladdr((void*)rip,&di)) snprintf(where,sizeof where,"%s:%s+0x%llx",di.dli_fname?strrchr(di.dli_fname,'/')?strrchr(di.dli_fname,'/')+1:di.dli_fname:"?",di.dli_sname?di.dli_sname:"?",(unsigned long long)(di.dli_saddr?rip-(uint64_t)di.dli_saddr:0));
                else snprintf(where,sizeof where,"0x%llx",(unsigned long long)rip);
                k=snprintf(m,sizeof m,"  [sample] %5.1f%% t%u %s\n",100.0*bc/(total?total:1),tab[bi].th,where); (void)!write(2,m,k);
                tab[bi].cnt=0; }   // (printed entries reset; counts restart for the next window)
            memset(tab,0,sizeof tab); total=0; for(int i=0;i<nthr;i++){ thr[i].n=thr[i].busy=0; } } }
    return 0; }
int main(int argc, char **argv){
    if(getenv("PELOAD_SAMPLE")){ pthread_t st; pthread_create(&st,0,sampler_thread,0); }
    wi_test_start();
    hitbox_start();
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
