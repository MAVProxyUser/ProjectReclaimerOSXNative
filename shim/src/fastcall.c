// fastcall.c -- fast paths for the hottest Win32 imports of halo3's main thread (backlog 11 / 41). Included by peload.c
// after the implementations it shortcuts (and by tools/callcost.c, which measures it). OPT-IN:
//   PELOAD_FASTCALL=all | <list of: tid, heap, keys, clock>    bound at load, on from the start ("all" = tid, heap, keys;
//                                                               clock only by name until it has run in a match)
//   PELOAD_AB=fastcall:<secs>                                   the in-match A/B: flips them (all, or PELOAD_FASTCALL's list)
//                                                               between on and off every <secs> (starts off)
// Unset: the original functions are bound -- the same code as before. Off at run time (the A/B), every call goes to the
// original function; blocks the fast heap handed out stay valid (they are freed into its pool).
//
// Where an import call's time goes under Rosetta (tools/callcost.sh, M4 Max, ns per call made the way halo3 makes it:
// `call [IAT]` from translated guest code):
//   - the call itself is cheap: ~1 ns into a leaf, in loader text or in generated code alike;
//   - an ms_abi function that calls ANY ordinary (SysV) function -- even on a cold path it never takes -- saves and restores
//     xmm6-15 + rsi/rdi on every call: ~4 ns. A __thread variable counts as such a call (its access calls tlv_get_addr), so
//     the fast paths keep their per-thread state in the guest thread's TEB (gs:0x30) instead;
//   - malloc + free themselves: 22 ns per pair (LIFO, one size), 62-67 ns per pair (64 mixed sizes, then 64 frees).
// Main-thread calls per second in a BTB (PELOAD_CALLCOUNT): GetCurrentThreadId 887k, GetProcessHeap 662k, HeapAlloc and
// HeapFree ~400k each, GetAsyncKeyState 94k. The numbers per path: shim/docs/PERF-PLAN.md ("Import fast paths").
//   tid    GetCurrentThreadId: the thread's id cached in its TEB (+0x1FF0, filled on the first call)     ~4.1 -> 2.1 ns
//   keys   GetAsyncKeyState: a table lookup leaf (stays off with PELOAD_POLL_DAMP, which sleeps there)  ~4.5 -> 1.3 ns
//   heap   HeapAlloc/HeapFree/HeapReAlloc/HeapSize: a per-thread cache of small blocks (1..1024 bytes, 16-byte classes)
//          carved from one reserved arena                                       alloc+free 27-76 -> 4.5-6 ns
//   clock  QueryPerformanceCounter: rdtsc minus a start-up offset (under Rosetta rdtsc counts nanoseconds, the same
//          rate as mach_absolute_time, which the original calls) -- a leaf          ~15 -> ~7.5 ns (tools/xcallbench.c)
// Not done (measured, not worth it): GetProcessHeap and TlsGetValue (leaves already: ~1 and ~2.6 ns); towlower (guest
// ucrtbase code already, 2.2 ns); memset/memcpy below 2 KB (guest vcruntime code already; `rep stosb` / `rep movsb`
// routines are many times SLOWER under Rosetta, ~0.27 ns a byte).
#include <os/lock.h>
enum { FC_TID=1, FC_HEAP=2, FC_KEYS=4, FC_CLOCK=8, FC_ALL=7 };   // (clock not in "all" until an in-match test: name it)
volatile int g_fastcall;            // the fast paths in use now (PELOAD_AB=fastcall flips it between 0 and g_fc_mask)
static int g_fc_mask=-1;            // the configured ones (-1 = not read yet, 0 = none: the originals are bound)
// Per-thread state lives past the end of the Windows TEB layout (and of the loader's 256 TLS slots at +0x1480..+0x1C80) in
// the 0x2000-byte TEB every guest thread has (thread_tramp, main). Loader threads have none (gs:0x30 = 0, the TSD slot
// macOS reserves for Wine): they take the original functions.
#define FC_TEB_TID 0x1FF0
#define FC_TEB_TC  0x1FF8
// The dispatchers are noinline: Apple clang's hot/cold splitting otherwise moves a branch it thinks is cold into an
// ordinary (SysV) function -- and a call to that costs every call the xmm6-15 saves again. (noinline functions are never split.)
#define FC_FN MSABI __attribute__((noinline))
static inline uint8_t* fc_teb(void){ uint8_t*t; __asm__("movq %%gs:0x30,%0":"=r"(t)); return t; }

// ---- GetCurrentThreadId ----
static uint32_t MSABI __attribute__((noinline)) fc_tid_slow(void){
    uint32_t t=i_GetCurrentThreadId(); uint8_t*teb=fc_teb(); if(teb) *(volatile uint32_t*)(teb+FC_TEB_TID)=t; return t; }
static uint32_t FC_FN fc_GetCurrentThreadId(void){
    if(!(g_fastcall&FC_TID)) return i_GetCurrentThreadId();
    uint8_t*teb=fc_teb(); uint32_t t;
    if(__builtin_expect(teb!=0 && (t=*(volatile uint32_t*)(teb+FC_TEB_TID))!=0,1)) return t;
    return teb? fc_tid_slow() : i_GetCurrentThreadId(); }

// ---- GetAsyncKeyState ----
static int16_t FC_FN fc_GetAsyncKeyState(int k){
    if(__builtin_expect(g_fastcall&FC_KEYS,1)) return (k>=0&&k<256&&g_keydown[k])?(int16_t)0x8000:0;
    return wi_GetAsyncKeyState(k); }

// ---- QueryPerformanceCounter ----
// Under Rosetta rdtsc counts nanoseconds (1 tick per ns, mach_absolute_time's rate; tools/xcallbench.c) and costs ~6.5 ns,
// where the original pays mach_absolute_time (~11 ns) plus the ms_abi -> SysV switch (~3 ns). The two clocks differ by a
// constant (the time the Mac has slept: mach_absolute_time stops then): fcc_init takes it, so the values are the ones the
// original returns -- after a sleep this one runs ahead of mach_absolute_time by the time asleep, never backwards (which is
// what Windows' own QPC does across a sleep). Off when rdtsc doesn't tick at exactly that rate (not under Rosetta).
static uint64_t g_fc_tscoff;
static inline uint64_t fc_rdtsc(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }
static int FC_FN fc_QueryPerformanceCounter(uint64_t*c){
    if(__builtin_expect(g_fastcall&FC_CLOCK,1)){ if(c) *c=fc_rdtsc()-g_fc_tscoff; return 1; }
    return i_QueryPerformanceCounter(c); }
static int fcc_init(void){
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t m0=mach_absolute_time(), r0=fc_rdtsc(); usleep(20000); uint64_t m1=mach_absolute_time(), r1=fc_rdtsc();
    double rate=m1>m0?(double)(r1-r0)/(double)(m1-m0):0;
    if(tb.numer!=1 || tb.denom!=1 || rate<0.9995 || rate>1.0005){ char m[140]; int k=snprintf(m,sizeof m,"  [fastcall] clock: rdtsc runs at %.4f per ns (timebase %u/%u), not mach_absolute_time's: clock part off\n",rate,tb.numer,tb.denom); (void)!write(2,m,k); return 0; }
    uint64_t best=~0ull, off=0; for(int i=0;i<32;i++){ uint64_t a=mach_absolute_time(), r=fc_rdtsc(), b=mach_absolute_time(); if(b-a<best){ best=b-a; off=r-(a+(b-a)/2); } }
    g_fc_tscoff=off; return 1; }

// ---- the small-block heap ----
// Blocks of 1..1024 bytes come from 64 KB slabs of ONE reserved arena (2 GB of address space, PELOAD_FASTCALL_ARENA_MB;
// pages are only used once touched, and never given back), one size class per slab: class c = 16*c bytes, the same
// rounding as malloc's (so HeapSize answers what it did before; checked at start, else the heap part stays off).
// "Is it ours" is one range check and the class one table lookup, so HeapFree never asks malloc. Each guest thread keeps
// up to 32 free blocks per class in a stack of pointers OUTSIDE the blocks (a write after free cannot corrupt the cache);
// an empty / full stack trades 16 at a time with a per-class pool (a lock). Cross-thread frees are fine (blocks have no
// owner). A thread's blocks go back to the pools when it exits. Off (the A/B): new blocks come from malloc again, arena
// blocks still free into the pools. Loader threads (no TEB) allocate with malloc and free arena blocks into the pools.
#define FCH_MAX   1024
#define FCH_NC    (FCH_MAX/16+1)
#define FCH_TCAP  32
#define FCH_SH    16                 // slab = 64 KB
typedef struct { uint32_t n[FCH_NC]; void* b[FCH_NC][FCH_TCAP]; uint8_t*teb; } FcTc;
#define FCH_DEAD  ((FcTc*)1)         // the TEB's cache of an exiting thread: frees go straight to the pools
static uint8_t* g_fch_cls;           // class of each slab
static uint64_t g_fch_top;           // bytes of the arena handed out as slabs
static struct { os_unfair_lock lk; uint32_t n, cap; void** v; uint8_t *cur, *end; uint64_t carved; } g_fch_pool[FCH_NC];
static pthread_key_t g_fch_key;
static uint32_t fch_take(uint32_t c,void**out,uint32_t want){
    __typeof__(g_fch_pool[0])*pl=&g_fch_pool[c]; uint32_t got=0, sz=c*16;
    os_unfair_lock_lock(&pl->lk);
    while(got<want && pl->n) out[got++]=pl->v[--pl->n];
    while(got<want){
        if(pl->cur+sz>pl->end){
            uint64_t off=__atomic_load_n(&g_fch_top,__ATOMIC_RELAXED);
            do{ if(off+(1ull<<FCH_SH)>g_fch_size) goto full; } while(!__atomic_compare_exchange_n(&g_fch_top,&off,off+(1ull<<FCH_SH),1,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
            g_fch_cls[off>>FCH_SH]=(uint8_t)c; pl->cur=g_fch_base+off; pl->end=pl->cur+(1ull<<FCH_SH); }
        out[got++]=pl->cur; pl->cur+=sz; pl->carved++; }
full:
    os_unfair_lock_unlock(&pl->lk); return got; }
static void fch_put(uint32_t c,void*const*v,uint32_t k){
    __typeof__(g_fch_pool[0])*pl=&g_fch_pool[c];
    os_unfair_lock_lock(&pl->lk);
    if(pl->n+k>pl->cap){ uint32_t nc=pl->cap?pl->cap*2:256; while(nc<pl->n+k) nc*=2; void**nv=realloc(pl->v,(size_t)nc*sizeof(void*));
        if(!nv){ os_unfair_lock_unlock(&pl->lk); return; }   // (no memory for the list: these blocks are lost, not reused)
        pl->v=nv; pl->cap=nc; }
    memcpy(pl->v+pl->n,v,(size_t)k*sizeof(void*)); pl->n+=k;
    os_unfair_lock_unlock(&pl->lk); }
static void fch_tc_exit(void*a){ FcTc*tc=a; for(uint32_t c=1;c<FCH_NC;c++) if(tc->n[c]) fch_put(c,tc->b[c],tc->n[c]);
    *(FcTc*volatile*)(tc->teb+FC_TEB_TC)=FCH_DEAD; free(tc); }   // (a TEB lives 30 s past its thread: peload.c thr_reap)
// this guest thread's cache, made on first use; 0 on a loader thread / an exiting thread / no memory
static FcTc* fch_tc(void){ uint8_t*teb=fc_teb(); if(!teb) return 0; FcTc*tc=*(FcTc**)(teb+FC_TEB_TC); if(tc) return tc==FCH_DEAD?0:tc;
    tc=calloc(1,sizeof *tc); if(!tc) return 0; tc->teb=teb; *(FcTc**)(teb+FC_TEB_TC)=tc; pthread_setspecific(g_fch_key,tc); return tc; }
static void fch_log(void){
    static int on=-1; if(on<0) on=getenv("PELOAD_FASTCALL_LOG")?1:0; if(!on) return;
    static double tl; double now=mono_s(); if(now-tl<10.0) return; tl=now;
    uint64_t pooled=0, carved=0; for(int c=1;c<FCH_NC;c++){ pooled+=g_fch_pool[c].n; carved+=g_fch_pool[c].carved; }
    char m[200]; int k=snprintf(m,sizeof m,"  [fastcall] heap arena: %.1f MB in slabs, %llu blocks carved, %llu free in the pools\n",
        __atomic_load_n(&g_fch_top,__ATOMIC_RELAXED)/1048576.0,(unsigned long long)carved,(unsigned long long)pooled); (void)!write(2,m,k); }
// The slow sides are separate ms_abi functions: they call SysV code (locks, calloc); the fast sides call only ms_abi code,
// so they save no registers. Allocation (fast path on, a guest thread): refill the thread's stack.
static void* MSABI __attribute__((noinline)) fch_alloc_slow(uint32_t c){
    fch_log(); FcTc*tc=fch_tc(); if(!tc) return 0;   // (-> malloc)
    if(tc->n[c]<FCH_TCAP/2) tc->n[c]+=fch_take(c,tc->b[c]+tc->n[c],FCH_TCAP/2-tc->n[c]);
    return tc->n[c]? tc->b[c][--tc->n[c]] : 0; }     // (arena full -> malloc)
static int MSABI __attribute__((noinline)) fch_free_slow(void*p,uint32_t c){
    FcTc*tc=(g_fastcall&FC_HEAP)? fch_tc() : 0;
    if(tc){ if(tc->n[c]>=FCH_TCAP){ fch_put(c,tc->b[c],FCH_TCAP/2); memmove(tc->b[c],tc->b[c]+FCH_TCAP/2,(FCH_TCAP-FCH_TCAP/2)*sizeof(void*)); tc->n[c]-=FCH_TCAP/2; }
        tc->b[c][tc->n[c]++]=p; return 1; }
    fch_put(c,&p,1); return 1; }
static void fch_free_any(void*p){ fch_free_slow(p,g_fch_cls[((uintptr_t)p-(uintptr_t)g_fch_base)>>FCH_SH]); }   // LocalFree, CoTaskMemFree
// HEAP_ZERO_MEMORY: n rounded up to 16 (inside the block), with 16-byte stores and no call
static inline void fch_zero(void*p,uint64_t n){ uint64_t k=(n+15)>>4; if(!k) return; char*q=p;
    __asm__ volatile("pxor %%xmm0,%%xmm0\n1: movaps %%xmm0,(%0)\n addq $16,%0\n decq %1\n jnz 1b" : "+r"(q),"+r"(k) :: "xmm0","memory","cc"); }
static void* FC_FN fc_HeapAlloc(void*h,uint32_t fl,uint64_t n){
    if(!(g_fastcall&FC_HEAP) || n>FCH_MAX) return i_HeapAlloc(h,fl,n);
    uint8_t*teb=fc_teb(); if(!teb) return i_HeapAlloc(h,fl,n);
    uint32_t c=n?(uint32_t)((n+15)>>4):1; FcTc*tc=*(FcTc**)(teb+FC_TEB_TC); void*p;
    if(__builtin_expect((uintptr_t)tc>1 && tc->n[c]!=0,1)) p=tc->b[c][--tc->n[c]];
    else if(!(p=fch_alloc_slow(c))) return i_HeapAlloc(h,fl,n);
    if(fl&8) fch_zero(p,n);
    return p; }
static int FC_FN fc_HeapFree(void*h,uint32_t fl,void*p){
    uint64_t off=(uintptr_t)p-(uintptr_t)g_fch_base; if(off>=g_fch_size) return i_HeapFree(h,fl,p);
    uint32_t c=g_fch_cls[off>>FCH_SH]; uint8_t*teb=fc_teb(); FcTc*tc;
    if(__builtin_expect((g_fastcall&FC_HEAP) && teb && (uintptr_t)(tc=*(FcTc**)(teb+FC_TEB_TC))>1 && tc->n[c]<FCH_TCAP,1)){ tc->b[c][tc->n[c]++]=p; return 1; }
    return fch_free_slow(p,c); }
static uint64_t FC_FN fc_HeapSize(void*h,uint32_t fl,void*p){
    uint64_t off=(uintptr_t)p-(uintptr_t)g_fch_base; if(off<g_fch_size) return (uint64_t)g_fch_cls[off>>FCH_SH]*16;
    return i_HeapSize(h,fl,p); }
// (flags are ignored, as i_HeapReAlloc ignores them: realloc may move a block and does not zero the new part.) Like malloc's
// realloc, a block stays where it is when the new size fits and is at least half of it; else it moves to its class. HeapSize
// after a shrink can then differ from what malloc's realloc left (both are >= the size asked for).
static void* FC_FN fc_HeapReAlloc(void*h,uint32_t fl,void*p,uint64_t n){
    uint64_t off=(uintptr_t)p-(uintptr_t)g_fch_base; if(off>=g_fch_size) return i_HeapReAlloc(h,fl,p,n);
    uint32_t c=g_fch_cls[off>>FCH_SH]; uint64_t have=(uint64_t)c*16;
    uint64_t want=n?n:1; if(want<=have && (2*want>=have || ((want+15)>>4)==c)) return p;   // in place
    void*q=fc_HeapAlloc(h,0,n); if(!q) return 0;
    memcpy(q,p,have<n?have:n); fc_HeapFree(h,0,p); return q; }
static int fch_init(void){
    // the classes must round exactly as malloc does, or HeapSize would answer differently for the same request
    for(uint64_t n=0;n<=FCH_MAX;n++) if(malloc_good_size(n?n:1)!=(n?(n+15)/16*16:16)){
        char m[120]; int k=snprintf(m,sizeof m,"  [fastcall] heap: malloc rounds %llu bytes to %zu, not to 16: heap part off\n",(unsigned long long)n,malloc_good_size(n?n:1)); (void)!write(2,m,k); return 0; }
    uint64_t sz=2048; { const char*e=getenv("PELOAD_FASTCALL_ARENA_MB"); if(e && strtoull(e,0,0)>=64) sz=strtoull(e,0,0); } sz<<=20;
    // far from the images and from the window halo3's "64 MB free within 2 GB of the game" scan needs (0x1'0000'0000..0x2'0000'0000)
    static const uint64_t hints[]={0x300000000000ull,0x60000000000ull,0};
    uint8_t*a=0; for(int i=0;i<3 && !a;i++){ void*p=mmap((void*)hints[i],sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); if(p==MAP_FAILED) continue;
        if((uintptr_t)p<0x200000000ull && (uintptr_t)p+sz>0x100000000ull){ munmap(p,sz); continue; } a=p; }
    if(!a || !(g_fch_cls=calloc(sz>>FCH_SH,1)) || pthread_key_create(&g_fch_key,fch_tc_exit)){ ws("  [fastcall] heap: no arena: heap part off\n"); return 0; }
    for(int c=0;c<FCH_NC;c++) g_fch_pool[c].lk=OS_UNFAIR_LOCK_INIT;
    g_fch_base=a; __atomic_store_n(&g_fch_size,sz,__ATOMIC_RELEASE); return 1; }

// ---- configuration, binding, A/B ----
static void fc_init(void){
    if(g_fc_mask>=0) return;
    const char*e=getenv("PELOAD_FASTCALL"), *ab=getenv("PELOAD_AB"); int on=e && *e && strcmp(e,"0"), abf=ab && !strncmp(ab,"fastcall",8);
    int m=0;
    if(on){ if(!strcmp(e,"1")) m=FC_ALL;
        else for(const char*p=e; *p; ){ const char*q=strchr(p,','); size_t l=q?(size_t)(q-p):strlen(p);
            if(l==3 && !strncmp(p,"tid",3)) m|=FC_TID; else if(l==4 && !strncmp(p,"heap",4)) m|=FC_HEAP; else if(l==4 && !strncmp(p,"keys",4)) m|=FC_KEYS;
            else if(l==5 && !strncmp(p,"clock",5)) m|=FC_CLOCK; else if(l==3 && !strncmp(p,"all",3)) m|=FC_ALL;
            if(!q) break; p=q+1; } }
    else if(abf) m=FC_ALL;
    if((m&FC_HEAP) && (heapdbg() || heapguard() || getenv("PELOAD_HEAP_STATS") || getenv("PELOAD_TRACE_ALLOC"))){ m&=~FC_HEAP; ws("  [fastcall] heap part off (a heap diagnostic is on)\n"); }
    if((m&FC_HEAP) && !fch_init()) m&=~FC_HEAP;
    { const char*d=getenv("PELOAD_POLL_DAMP"); if((m&FC_KEYS) && d && *d && strcmp(d,"0")) m&=~FC_KEYS; }   // the damper sleeps inside GetAsyncKeyState
    if((m&FC_CLOCK) && !fcc_init()) m&=~FC_CLOCK;
    g_fc_mask=m; g_fastcall=on && !abf ? m : 0;
    if(m){ char t[160]; int k=snprintf(t,sizeof t,"  [fastcall]%s%s%s%s: %s\n",m&FC_TID?" tid":"",m&FC_HEAP?" heap":"",m&FC_KEYS?" keys":"",m&FC_CLOCK?" clock":"",
        abf?"PELOAD_AB flips them (starts off)":"on"); (void)!write(2,t,k); } }
// resolve_impl's result for an import -> that fast path's dispatcher when it is configured
static void* fc_bind(void*f){
    fc_init(); int m=g_fc_mask; if(!m || !f) return f;
    if((m&FC_TID) && f==(void*)i_GetCurrentThreadId) return (void*)fc_GetCurrentThreadId;
    if((m&FC_KEYS) && f==(void*)wi_GetAsyncKeyState) return (void*)fc_GetAsyncKeyState;
    if((m&FC_CLOCK) && f==(void*)i_QueryPerformanceCounter) return (void*)fc_QueryPerformanceCounter;
    if(m&FC_HEAP){ if(f==(void*)i_HeapAlloc) return (void*)fc_HeapAlloc; if(f==(void*)i_HeapFree) return (void*)fc_HeapFree;
        if(f==(void*)i_HeapReAlloc) return (void*)fc_HeapReAlloc; if(f==(void*)i_HeapSize) return (void*)fc_HeapSize; }
    return f; }
// PELOAD_AB=fastcall (d3dsubmit.c sub_switch): -1 = is it on; 0/1 = every configured fast path off / on
int ab_fastcall(int set){ if(set<0) return g_fastcall!=0; g_fastcall=set&&g_fc_mask>0?g_fc_mask:0; return g_fastcall!=0; }
