// callcost: what one call of halo3's hottest Win32 imports costs under Rosetta (backlog 11/41), before and with
// src/fastcall.c's fast paths (PELOAD_FASTCALL), plus equivalence checks and a multi-thread stress test of its heap.
// Built x86_64 and run under Rosetta by tools/callcost.sh, which cuts the loader's real implementations out of
// src/peload.c / src/wininput.c into callcost_orig.inc (fastcall.c is included as it is). The callers are Win64 loops
// copied into writable+executable memory (translated like the game's own code) that `call [slot]` exactly as halo3
// calls an import through its IAT.
//     callcost <CRT dir or ""> [quick]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>
#include <cpuid.h>
#include <pthread/qos.h>
#define MSABI __attribute__((ms_abi))
static double mono_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void ws(const char*s){ (void)!write(2,s,strlen(s)); }
static uint64_t mt_us(uint64_t us){ static double f; if(!f){ mach_timebase_info_data_t tb; mach_timebase_info(&tb); f=1000.0*tb.denom/tb.numer; } return (uint64_t)(us*f); }
// what the cut-out originals refer to
static void* MSABI crt_big_memmove(void*d,const void*s,size_t n){ return memmove(d,s,n); }
static void* MSABI crt_big_memset(void*d,int c,size_t n){ return memset(d,c,n); }
uint64_t g_memcpy_min=2048;
#include "callcost_orig.inc"
#include "fastcall.c"

// ---- Win64 caller loops (position independent; copied into RWX memory) ----
// call: [0] fn, [8] rcx, [0x10] rdx, [0x18] r8, [0x20] r9, [0x28] iterations
// af:   [0] alloc, [8] free, [0x10] heap, [0x18] flags, [0x20] size, [0x28] iterations          (alloc + free per round)
// bat:  [0] alloc, [8] free, [0x10] heap, [0x18] flags, [0x20] sizes*, [0x28] rounds, [0x30] K, [0x38] ptrs*  (K allocs, then K frees)
#define PRO "pushq %rbx\n pushq %rbp\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n subq $0x28,%rsp\n movq %rdi,%r12\n movq 0x28(%r12),%r13\n"
#define EPI "addq $0x28,%rsp\n popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbp\n popq %rbx\n retq\n"
extern char cc_call_beg[], cc_call_end[], cc_none_beg[], cc_none_end[], cc_af_beg[], cc_af_end[], cc_bat_beg[], cc_bat_end[];
__asm__(".text\n"
"_cc_call_beg:\n" PRO
"1: movq 0x08(%r12),%rcx\n movq 0x10(%r12),%rdx\n movq 0x18(%r12),%r8\n movq 0x20(%r12),%r9\n callq *(%r12)\n decq %r13\n jnz 1b\n" EPI
"_cc_call_end:\n"
"_cc_none_beg:\n" PRO
"1: movq 0x08(%r12),%rcx\n movq 0x10(%r12),%rdx\n movq 0x18(%r12),%r8\n movq 0x20(%r12),%r9\n decq %r13\n jnz 1b\n" EPI
"_cc_none_end:\n"
"_cc_af_beg:\n" PRO
"1: movq 0x10(%r12),%rcx\n movq 0x18(%r12),%rdx\n movq 0x20(%r12),%r8\n callq *(%r12)\n"
"   movq %rax,%r8\n movq 0x10(%r12),%rcx\n xorl %edx,%edx\n callq *0x08(%r12)\n decq %r13\n jnz 1b\n" EPI
"_cc_af_end:\n"
"_cc_bat_beg:\n" PRO
"2: xorq %r14,%r14\n"
"3: movq 0x10(%r12),%rcx\n movq 0x18(%r12),%rdx\n movq 0x20(%r12),%rax\n movq (%rax,%r14,8),%r8\n callq *(%r12)\n"
"   movq 0x38(%r12),%rbx\n movq %rax,(%rbx,%r14,8)\n incq %r14\n cmpq 0x30(%r12),%r14\n jb 3b\n"
"   xorq %r14,%r14\n"
"4: movq 0x10(%r12),%rcx\n xorl %edx,%edx\n movq 0x38(%r12),%rbx\n movq (%rbx,%r14,8),%r8\n callq *0x08(%r12)\n"
"   incq %r14\n cmpq 0x30(%r12),%r14\n jb 4b\n"
"   decq %r13\n jnz 2b\n" EPI
"_cc_bat_end:\n");
typedef void (*LoopFn)(uint64_t*ctx);
static uint8_t*g_rwx; static size_t g_rwx_used;
static void* rwx_put(const void*code,size_t n){ uint8_t*p=g_rwx+g_rwx_used; memcpy(p,code,n); g_rwx_used=(g_rwx_used+n+63)&~(size_t)63; sys_icache_invalidate(p,n); return p; }
static LoopFn L_call, L_none, L_af, L_bat;

// ---- building blocks ----
static const uint8_t k_leaf_ret[]={0xB8,0x50,0x41,0x45,0x48, 0xC3};                                      // mov eax,'HEAP'; ret
static const uint8_t k_leaf_tebtid[]={0x65,0x48,0x8B,0x04,0x25,0x30,0,0,0, 0x8B,0x40,0x48, 0xC3};          // mov rax,gs:[0x30]; mov eax,[rax+0x48]; ret
static void* MSABI c_leaf_ret(void){ return (void*)0x48454150; }
static uint32_t MSABI c_leaf_tebtid(void){ uint64_t t; __asm__("movq %%gs:0x30,%0":"=r"(t)); return *(uint32_t*)(t+0x48); }
static void __attribute__((noinline)) sysv_empty(void){ __asm__ volatile(""); }
static int MSABI c_ms_calls_sysv(void){ sysv_empty(); return 1; }          // the ms_abi -> SysV switch: saves xmm6-15 + rsi/rdi
__thread uint32_t t_bench_tlv=7;                                          // (written in main: a never-written one folds to a constant)
static uint32_t MSABI c_ms_reads_tlv(void){ return t_bench_tlv; }         // a __thread read from ms_abi code (calls tlv_get_addr)

// ---- timing ----
static int g_quick;
static uint64_t now_ns(void){ return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
// best of 7 runs (after one warm-up), ns per iteration
static double run_best(LoopFn f,uint64_t*ctx,uint64_t iters,int iterslot,double per){
    double best=1e18; ctx[iterslot]=iters/8+1; f(ctx);
    for(int r=0;r<(g_quick?2:7);r++){ ctx[iterslot]=iters; uint64_t t0=now_ns(); f(ctx); uint64_t t1=now_ns(); double ns=(double)(t1-t0)/iters/per; if(ns<best) best=ns; }
    return best; }
static double g_base;   // the empty loop
static double t_call(const char*name,void*fn,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t iters,const char*note){
    uint64_t ctx[8]={(uint64_t)fn,a1,a2,a3,0,0};
    double ns=run_best(L_call,ctx,iters,5,1)-g_base; printf("  %-50s %7.2f ns%s%s\n",name,ns,note?"   ":"",note?note:""); fflush(stdout); return ns; }
static double t_af(const char*name,void*a,void*f,uint64_t fl,uint64_t n,uint64_t iters){
    uint64_t ctx[8]={(uint64_t)a,(uint64_t)f,0x48454150,fl,n,0};
    double ns=run_best(L_af,ctx,iters,5,1)-g_base; printf("  %-50s %7.2f ns per alloc+free\n",name,ns); fflush(stdout); return ns; }
static double t_bat(const char*name,void*a,void*f,uint64_t fl,const uint64_t*sizes,uint64_t K,uint64_t rounds){
    void**ptrs=calloc(K,sizeof(void*)); uint64_t ctx[8]={(uint64_t)a,(uint64_t)f,0x48454150,fl,(uint64_t)sizes,0,K,(uint64_t)ptrs};
    double ns=run_best(L_bat,ctx,rounds,5,(double)K)-g_base; free(ptrs); printf("  %-50s %7.2f ns per alloc+free\n",name,ns); fflush(stdout); return ns; }

// ---- the guest CRT (Microsoft vcruntime140 / ucrtbase), mapped + relocated like the loader does (no init) ----
static uint8_t* pe_map(const char*path){
    int fd=open(path,O_RDONLY); if(fd<0) return 0; struct stat st; fstat(fd,&st);
    uint8_t*f=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd); if(f==MAP_FAILED) return 0;
    uint32_t pe=*(uint32_t*)(f+0x3C); uint16_t nsec=*(uint16_t*)(f+pe+6), optsz=*(uint16_t*)(f+pe+20); uint8_t*opt=f+pe+24;
    uint64_t ib=*(uint64_t*)(opt+24); uint32_t isz=*(uint32_t*)(opt+56), hsz=*(uint32_t*)(opt+60);
    uint8_t*b=mmap(0,isz,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(b==MAP_FAILED) return 0;
    memcpy(b,f,hsz); uint8_t*sec=opt+optsz;
    for(int i=0;i<nsec;i++,sec+=40){ uint32_t va=*(uint32_t*)(sec+12), rs=*(uint32_t*)(sec+16), rp=*(uint32_t*)(sec+20); if(rs) memcpy(b+va,f+rp,rs); }
    int64_t d=(int64_t)(uint64_t)b-(int64_t)ib; uint32_t rr=*(uint32_t*)(opt+112+5*8), rsz=*(uint32_t*)(opt+112+5*8+4);
    for(uint8_t*q=b+rr,*e=b+rr+rsz; rr && q<e; ){ uint32_t pg=*(uint32_t*)q, bl=*(uint32_t*)(q+4); uint16_t*en=(uint16_t*)(q+8);
        for(uint32_t i=0;i<(bl-8)/2;i++) if((en[i]>>12)==10) *(uint64_t*)(b+pg+(en[i]&0xfff))+=d; q+=bl; }
    sys_icache_invalidate(b,isz); return b; }
static void* pe_export(uint8_t*b,const char*name){
    uint32_t pe=*(uint32_t*)(b+0x3C); uint8_t*opt=b+pe+24; uint32_t er=*(uint32_t*)(opt+112); if(!er) return 0; uint8_t*ed=b+er;
    uint32_t nn=*(uint32_t*)(ed+24), af=*(uint32_t*)(ed+28), an=*(uint32_t*)(ed+32), ao=*(uint32_t*)(ed+36);
    for(uint32_t i=0;i<nn;i++) if(!strcmp((char*)b+((uint32_t*)(b+an))[i],name)) return b+((uint32_t*)(b+af))[((uint16_t*)(b+ao))[i]];
    return 0; }
// vcruntime's string routines test __favor (bit 1 = ERMS: rep movsb/stosb above 128 bytes), set by its DllMain from cpuid
static uint32_t* favor_of(uint8_t*fn){ for(int i=0;i<0x80;i++) if(fn[i]==0x0F&&fn[i+1]==0xBA&&fn[i+2]==0x25&&fn[i+7]==0x01) return (uint32_t*)(fn+i+8+*(int32_t*)(fn+i+3)); return 0; }
static int erms(void){ unsigned a,b,c,d; if(!__get_cpuid_count(7,0,&a,&b,&c,&d)) return 0; return (b>>9)&1; }
static int child_ok(void*fn,uint64_t a){ pid_t p=fork(); if(!p){ uint64_t(MSABI*f)(uint64_t)=fn; _exit(f(a)==0x61?0:1); } int s=0; waitpid(p,&s,0); return WIFEXITED(s)&&WEXITSTATUS(s)==0; }

// ---- the fast heap, checked: equivalence with the originals, then threads hammering it ----
static int g_fails;
#define CHECK(c,...) do{ if(!(c)){ if(g_fails++<20){ printf("  FAIL line %d: %s  ",__LINE__,#c); printf(__VA_ARGS__); printf("\n"); } } }while(0)
#define H ((void*)0x48454150)
static void teb_install(void){ uint8_t*t=calloc(1,0x2000); *(uint64_t*)(t+0x30)=(uint64_t)t; __asm__ volatile("movq %0,%%gs:0x30"::"r"((uint64_t)t)); }   // (leaked, as the loader's)
static void equiv_checks(void){
    for(uint64_t n=0;n<=1100;n++) for(uint32_t fl=0;fl<=8;fl+=8){
        uint8_t*a=fc_HeapAlloc(H,fl,n), *b=i_HeapAlloc(H,fl,n);
        CHECK(a && b,"n %llu",(unsigned long long)n); if(!a||!b) continue;
        CHECK(!((uintptr_t)a&15),"alignment, n %llu",(unsigned long long)n);
        CHECK(fc_HeapSize(H,0,a)==i_HeapSize(H,0,b),"HeapSize n %llu: %llu vs %llu",(unsigned long long)n,(unsigned long long)fc_HeapSize(H,0,a),(unsigned long long)i_HeapSize(H,0,b));
        if(fl&8){ int z=1; for(uint64_t i=0;i<n;i++) if(a[i]) z=0; CHECK(z,"HEAP_ZERO_MEMORY n %llu",(unsigned long long)n); }
        for(uint64_t i=0;i<n;i++) a[i]=(uint8_t)(i*13+1);
        uint64_t m=(n*7+5)%1300; uint8_t*a2=fc_HeapReAlloc(H,0,a,m); CHECK(a2!=0,"realloc %llu -> %llu",(unsigned long long)n,(unsigned long long)m); if(!a2) continue;
        int same=1; for(uint64_t i=0;i<(n<m?n:m);i++) if(a2[i]!=(uint8_t)(i*13+1)) same=0; CHECK(same,"realloc keeps the bytes, %llu -> %llu",(unsigned long long)n,(unsigned long long)m);
        CHECK(fc_HeapSize(H,0,a2)>=(m?m:1) && (fc_HeapSize(H,0,a2)==malloc_good_size(m?m:1) || a2==a),"realloc size %llu",(unsigned long long)m);
        CHECK(fc_HeapFree(H,0,a2)==1 && i_HeapFree(H,0,b)==1,"HeapFree returns 1"); }
    CHECK(fc_HeapFree(H,0,0)==i_HeapFree(H,0,0),"HeapFree(NULL)");
    for(int k=-3;k<300;k++){ g_keydown[k&255]=(uint8_t)(k%3==0); CHECK(fc_GetAsyncKeyState(k)==wi_GetAsyncKeyState(k),"GetAsyncKeyState(%d)",k); }
    memset((void*)g_keydown,0,256);
    CHECK(fc_GetCurrentThreadId()==i_GetCurrentThreadId() && fc_GetCurrentThreadId()==(uint32_t)pthread_mach_thread_np(pthread_self()),"GetCurrentThreadId"); }
typedef struct { uint8_t*p; uint64_t n; uint8_t tag; } Blk;
static Blk* _Atomic g_xch[64];
static uint64_t xs(uint64_t*s){ *s^=*s<<13; *s^=*s>>7; *s^=*s<<17; return *s; }
static uint64_t rsize(uint64_t*s){ uint64_t r=xs(s)%100; return r<10? xs(s)%33 : r<85? xs(s)%1025 : 1025+xs(s)%3000; }
static int blk_ok(const Blk*b){ for(uint64_t i=0;i<b->n;i++) if(b->p[i]!=(uint8_t)(b->tag+i*7)) return 0; return 1; }
static void blk_fill(Blk*b){ for(uint64_t i=0;i<b->n;i++) b->p[i]=(uint8_t)(b->tag+i*7); }
static void blk_free(Blk*b,uint64_t*s){ CHECK(blk_ok(b),"block %p (%llu B) changed while live / handed out twice",(void*)b->p,(unsigned long long)b->n);
    if(xs(s)%20==0) free_or_fch(b->p); else fc_HeapFree(H,0,b->p); }   // (5%: LocalFree / CoTaskMemFree's path)
static int g_iters=150000;
static void* st_thread(void*arg){
    int id=(int)(intptr_t)arg, guest=id%4!=3; uint64_t g30; __asm__ volatile("movq %%gs:0x30,%0":"=r"(g30));
    CHECK(g30==0,"a new thread's gs:0x30 (the TSD slot macOS reserves for Wine) is not 0: %llx",(unsigned long long)g30);
    if(guest) teb_install();   // (every 4th thread is a loader thread: no TEB)
    Blk live[256]; int nl=0; uint64_t s=0x9E3779B97F4A7C15ull*(uint64_t)(id+1)+now_ns();
    for(int it=0;it<g_iters;it++){ uint64_t r=xs(&s)%100;
        if(r<40 && nl<256){ uint64_t n=rsize(&s); uint32_t fl=(xs(&s)&3)==0?8:0; uint8_t*p=fc_HeapAlloc(H,fl,n); CHECK(p!=0,"alloc %llu",(unsigned long long)n); if(!p) continue;
            if(fl&8){ int z=1; for(uint64_t i=0;i<n;i++) if(p[i]) z=0; CHECK(z,"HEAP_ZERO_MEMORY %llu",(unsigned long long)n); }
            CHECK(!((uintptr_t)p&15),"alignment"); CHECK(fc_HeapSize(H,0,p)==malloc_good_size(n?n:1),"HeapSize %llu",(unsigned long long)n);
            Blk b={p,n,(uint8_t)xs(&s)}; blk_fill(&b); live[nl++]=b; }
        else if(r<75 && nl){ int k=(int)(xs(&s)%nl); blk_free(&live[k],&s); live[k]=live[--nl]; }
        else if(r<85 && nl){ int k=(int)(xs(&s)%nl); Blk*b=&live[k]; CHECK(blk_ok(b),"before realloc"); uint64_t nn=rsize(&s);
            uint8_t*q=fc_HeapReAlloc(H,0,b->p,nn); CHECK(q!=0,"realloc"); if(!q) continue;
            uint64_t keep=b->n<nn?b->n:nn; int same=1; for(uint64_t i=0;i<keep;i++) if(q[i]!=(uint8_t)(b->tag+i*7)) same=0; CHECK(same,"realloc keeps the bytes");
            CHECK(fc_HeapSize(H,0,q)>=(nn?nn:1),"realloc size"); b->p=q; b->n=nn; blk_fill(b); }
        else if(nl){ int k=(int)(xs(&s)%nl); Blk*b=malloc(sizeof *b); *b=live[k]; live[k]=live[--nl];   // another thread frees it
            Blk*o=atomic_exchange(&g_xch[xs(&s)%64],b); if(o){ blk_free(o,&s); free(o); } }
        if(id==0 && (it&511)==0) g_fastcall=(xs(&s)&1)? g_fc_mask : 0; }   // the in-match A/B, under load
    while(nl) blk_free(&live[--nl],&s);
    return 0; }
static int cmp_ptr(const void*a,const void*b){ uintptr_t x=*(const uintptr_t*)a, y=*(const uintptr_t*)b; return x<y?-1:x>y; }
static void stress(void){
    int nt=8, rounds=g_quick?2:4; if(g_quick) g_iters=40000; double t0=mono_s();
    for(int r=0;r<rounds;r++){ pthread_t th[8]; for(int i=0;i<nt;i++) pthread_create(&th[i],0,st_thread,(void*)(intptr_t)(i+r)); for(int i=0;i<nt;i++) pthread_join(th[i],0); }
    uint64_t s=1; for(int i=0;i<64;i++){ Blk*o=atomic_exchange(&g_xch[i],(Blk*)0); if(o){ blk_free(o,&s); free(o); } }
    g_fastcall=g_fc_mask;
    // every block ever carved is now free in a pool (the threads exited) or in this main thread's cache, exactly once, at a block start
    FcTc*mt=*(FcTc**)(fc_teb()+FC_TEB_TC); int hasmt=(uintptr_t)mt>1; uint64_t carved=0, n=0;
    for(int c=1;c<FCH_NC;c++){ carved+=g_fch_pool[c].carved; n+=g_fch_pool[c].n+(hasmt?mt->n[c]:0); }
    uintptr_t*all=malloc((n+1)*sizeof *all); uint64_t k=0;
    for(int c=1;c<FCH_NC;c++){ uint64_t k0=k; for(uint32_t i=0;i<g_fch_pool[c].n;i++) all[k++]=(uintptr_t)g_fch_pool[c].v[i]; if(hasmt) for(uint32_t i=0;i<mt->n[c];i++) all[k++]=(uintptr_t)mt->b[c][i];
        for(uint64_t j=k0;j<k;j++){ uint64_t off=all[j]-(uintptr_t)g_fch_base; CHECK(off<g_fch_size && g_fch_cls[off>>FCH_SH]==c && (off&((1ull<<FCH_SH)-1))%(c*16)==0,"block %p in the wrong class / not at a block start",(void*)all[j]); } }
    qsort(all,n,sizeof *all,cmp_ptr); uint64_t dup=0; for(uint64_t i=1;i<n;i++) if(all[i]==all[i-1]) dup++; free(all);
    CHECK(n==carved,"%llu blocks carved, %llu accounted for",(unsigned long long)carved,(unsigned long long)n); CHECK(dup==0,"%llu blocks twice in the free lists",(unsigned long long)dup);
    printf("  stress: %d rounds x %d threads (every 4th without a TEB) x %d ops, A/B flipped under load: %.1f s; %llu blocks carved (%.1f MB of slabs), all back once\n",
        rounds,nt,g_iters,mono_s()-t0,(unsigned long long)carved,g_fch_top/1048576.0); }

int main(int argc,char**argv){
    const char*crt=argc>1?argv[1]:""; g_quick=argc>2&&!strcmp(argv[2],"quick"); t_bench_tlv=(uint32_t)argc;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);   // (performance cores, as the game's main thread)
    g_rwx=mmap(0,1<<20,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(g_rwx==MAP_FAILED){ perror("mmap rwx"); return 1; }
    L_call=rwx_put(cc_call_beg,cc_call_end-cc_call_beg); L_none=rwx_put(cc_none_beg,cc_none_end-cc_none_beg);
    L_af=rwx_put(cc_af_beg,cc_af_end-cc_af_beg); L_bat=rwx_put(cc_bat_beg,cc_bat_end-cc_bat_beg);
    // a Windows TEB at gs:0x30 as the loader gives every guest thread (TLS slots at +0x1480); +0x48 = ClientId for the leaf tests
    static uint8_t teb[0x2000] __attribute__((aligned(16))); *(uint64_t*)(teb+0x30)=(uint64_t)teb; *(uint32_t*)(teb+0x48)=(uint32_t)pthread_mach_thread_np(pthread_self());
    __asm__ volatile("movq %0,%%gs:0x30"::"r"((uint64_t)teb));
    uint32_t tlsi=i_TlsAlloc(); i_TlsSetValue(tlsi,(void*)0x1234);
    setenv("PELOAD_FASTCALL","all",1); fc_init();
    if(g_fc_mask!=FC_ALL){ printf("fastcall: not every fast path came up (mask %d)\n",g_fc_mask); return 1; }
    const uint64_t N=g_quick?4000000:20000000, NA=N/8;
    { uint64_t ctx[8]={0}; g_base=run_best(L_none,ctx,N,5,1); }
    printf("callcost (Rosetta; best of %d runs; the empty caller loop, %.2f ns, is subtracted)\n",g_quick?2:7,g_base);
    printf("building blocks (Win64 caller in generated code -> call [slot]):\n");
    void*j_ret=rwx_put(k_leaf_ret,sizeof k_leaf_ret), *j_teb=rwx_put(k_leaf_tebtid,sizeof k_leaf_tebtid);
    t_call("leaf in loader text (C, sets up a frame)",(void*)c_leaf_ret,0,0,0,N,0);
    t_call("leaf in generated code",j_ret,0,0,0,N,0);
    { uint8_t st[16]; int o=0; st[o++]=0x48; st[o++]=0xB8; memcpy(st+o,&j_ret,8); o+=8; st[o++]=0xFF; st[o++]=0xE0;    // mov rax,leaf; jmp rax
      t_call("  + a `mov rax,x; jmp rax` stub in front",rwx_put(st,o),0,0,0,N,"(crt_fast_stub's shape)"); }
    { uint8_t*st=g_rwx+g_rwx_used; int32_t rel=(int32_t)((uint8_t*)j_ret-(st+5)); uint8_t b[5]={0xE9}; memcpy(b+1,&rel,4);
      t_call("  + a `jmp rel32` stub in front",rwx_put(b,5),0,0,0,N,0); }
    t_call("TEB leaf: gs:[0x30] -> [+0x48], loader text",(void*)c_leaf_tebtid,0,0,0,N,0);
    t_call("TEB leaf: same, generated code",j_teb,0,0,0,N,0);
    t_call("ms_abi fn calling an empty SysV fn",(void*)c_ms_calls_sysv,0,0,0,N,"(saves xmm6-15 + rsi/rdi)");
    t_call("ms_abi fn reading a __thread variable",(void*)c_ms_reads_tlv,0,0,0,N,"(tlv_get_addr: the same saves)");
    printf("current implementation / fast path (PELOAD_FASTCALL) / fast path switched off (the A/B's off phase):\n");
    #define ROW3(nm,orig,fast,a1,it) do{ printf("%s\n",nm); t_call("  current",(void*)(orig),a1,0,0,it,0); g_fastcall=g_fc_mask; t_call("  fast",(void*)(fast),a1,0,0,it,0); \
        g_fastcall=0; t_call("  fast path, switched off",(void*)(fast),a1,0,0,it,0); g_fastcall=g_fc_mask; }while(0)
    ROW3("GetCurrentThreadId",i_GetCurrentThreadId,fc_GetCurrentThreadId,0,N);
    printf("GetProcessHeap (no fast path)\n"); t_call("  current",(void*)i_GetProcessHeap,0,0,0,N,0);
    ROW3("GetAsyncKeyState",wi_GetAsyncKeyState,fc_GetAsyncKeyState,0x51,N);
    printf("TlsGetValue (no fast path)\n"); t_call("  current",(void*)i_TlsGetValue,tlsi,0,0,N,0);
    static uint64_t mix[256]; { static const uint64_t m16[16]={24,40,64,96,128,200,320,512,800,16,48,72,32,256,1000,88}; for(int i=0;i<256;i++) mix[i]=m16[(i*7)%16]; }
    #define HROW(nm,fl,n) do{ printf("HeapAlloc+HeapFree, %s\n",nm); t_af("  current",(void*)i_HeapAlloc,(void*)i_HeapFree,fl,n,NA); g_fastcall=g_fc_mask; \
        t_af("  fast",(void*)fc_HeapAlloc,(void*)fc_HeapFree,fl,n,NA); g_fastcall=0; t_af("  fast path, switched off",(void*)fc_HeapAlloc,(void*)fc_HeapFree,fl,n,NA); g_fastcall=g_fc_mask; }while(0)
    #define BROW(K) do{ printf("HeapAlloc x%d (mixed sizes 16..1000), then HeapFree x%d\n",K,K); t_bat("  current",(void*)i_HeapAlloc,(void*)i_HeapFree,0,mix,K,NA/K); g_fastcall=g_fc_mask; \
        t_bat("  fast",(void*)fc_HeapAlloc,(void*)fc_HeapFree,0,mix,K,NA/K); g_fastcall=0; t_bat("  fast path, switched off",(void*)fc_HeapAlloc,(void*)fc_HeapFree,0,mix,K,NA/K); g_fastcall=g_fc_mask; }while(0)
    HROW("64 B",0,64); HROW("512 B",0,512); HROW("HEAP_ZERO_MEMORY 256 B",8,256); HROW("4000 B (above the fast sizes)",0,4000);
    BROW(64); BROW(256);
    printf("for comparison, malloc+free called from loader C (SysV, no switch):\n");
    { double best=1e18; for(int r=0;r<5;r++){ uint64_t t0=now_ns(); for(uint64_t i=0;i<NA;i++){ void*p=malloc(64); __asm__ volatile(""::"r"(p):"memory"); free(p); } double ns=(double)(now_ns()-t0)/NA; if(ns<best) best=ns; }
      printf("  %-50s %7.2f ns per alloc+free\n","malloc+free 64 B",best); }
    { void*ptr[64]; double best=1e18; for(int r=0;r<5;r++){ uint64_t t0=now_ns(); for(uint64_t i=0;i<NA/64;i++){ for(int k=0;k<64;k++){ ptr[k]=malloc(mix[k]); __asm__ volatile(""::"r"(ptr[k]):"memory"); } for(int k=0;k<64;k++) free(ptr[k]); } double ns=(double)(now_ns()-t0)/(NA/64*64); if(ns<best) best=ns; }
      printf("  %-50s %7.2f ns per alloc+free\n","malloc x64 mixed, then free x64",best); }
    // ---- memset / memcpy / towlower: guest CRT routines ----
    uint8_t*vc=0,*uc=0; char p[1200];
    if(crt&&*crt){ snprintf(p,sizeof p,"%s/vcruntime140.dll",crt); vc=pe_map(p); snprintf(p,sizeof p,"%s/ucrtbase.dll",crt); uc=pe_map(p); }
    if(vc){ void*ms=pe_export(vc,"memset"), *mc=pe_export(vc,"memcpy"); uint32_t*fav=favor_of(ms);
        printf("guest vcruntime140 memset / memcpy (cpuid ERMS %d%s):\n",erms(),erms()?"":": __favor's ERMS bit stays 0, SSE loops");
        if(fav) *fav=erms()?2:0;
        static uint8_t buf[2][4096] __attribute__((aligned(64)));
        void*sms=crt_fast_stub("memset",ms), *smc=crt_fast_stub("memcpy",mc);
        // guest-side `rep stosb` / `rep movsb` routines (Win64: rdi/rsi are callee-saved)
        static const uint8_t k_stosb[]={0x49,0x89,0xCB, 0x49,0x89,0xF9, 0x48,0x89,0xCF, 0x89,0xD0, 0x4C,0x89,0xC1, 0xF3,0xAA, 0x4C,0x89,0xCF, 0x4C,0x89,0xD8, 0xC3};
        static const uint8_t k_movsb[]={0x49,0x89,0xCB, 0x49,0x89,0xF9, 0x49,0x89,0xF2, 0x48,0x89,0xCF, 0x48,0x89,0xD6, 0x4C,0x89,0xC1, 0xF3,0xA4, 0x4C,0x89,0xCF, 0x4C,0x89,0xD6, 0x4C,0x89,0xD8, 0xC3};
        void*jst=rwx_put(k_stosb,sizeof k_stosb), *jmv=rwx_put(k_movsb,sizeof k_movsb);
        static const uint64_t sz[]={16,64,256,1024,2047};
        for(int i=0;i<5;i++){ char nm[80];
            snprintf(nm,sizeof nm,"memset %4llu B: vcruntime, called directly",(unsigned long long)sz[i]); t_call(nm,ms,(uint64_t)buf[0],0,sz[i],N/2,0);
            t_call("           through crt_fast_stub (current)",sms,(uint64_t)buf[0],0,sz[i],N/2,0);
            t_call("           a rep stosb routine",jst,(uint64_t)buf[0],0,sz[i],sz[i]>256?N/40:N/4,0);
            snprintf(nm,sizeof nm,"memcpy %4llu B: vcruntime, called directly",(unsigned long long)sz[i]); t_call(nm,mc,(uint64_t)buf[0],(uint64_t)buf[1],sz[i],N/2,0);
            t_call("           through crt_fast_stub (current)",smc,(uint64_t)buf[0],(uint64_t)buf[1],sz[i],N/2,0);
            t_call("           a rep movsb routine",jmv,(uint64_t)buf[0],(uint64_t)buf[1],sz[i],sz[i]>256?N/40:N/4,0); } }
    else printf("(no Microsoft CRT found: memset/memcpy/towlower skipped)\n");
    if(uc){ void*tl=pe_export(uc,"towlower");
        if(tl && child_ok(tl,0x41)) t_call("towlower('A'): ucrtbase's (guest code, \"C\" locale)",tl,0x41,0,0,N,0);
        else printf("  towlower: ucrtbase's needs its CRT initialised here; skipped\n"); }
    printf("checks:\n");
    g_fastcall=g_fc_mask; equiv_checks(); printf("  equivalence with the originals (HeapSize, zeroing, realloc, key state, thread id, NULL): %s\n",g_fails?"FAILED":"ok");
    stress();
    printf("%s\n",g_fails?"FAILED":"all checks passed");
    return g_fails!=0; }
