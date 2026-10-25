// xcallbench: what a call from guest-like code costs under Rosetta, by where the callee lives.
//     clang -arch x86_64 -O2 -o /tmp/xcallbench shim/tools/xcallbench.c && /tmp/xcallbench
// halo3's code is translated by Rosetta's JIT (it is written into memory at run time); the loader's import implementations
// are part of the loader's Mach-O, translated ahead of time. A caller loop in RWX memory (JIT-translated, like halo3) does
// `call [slot]` N times into:
//   jit leaf   -- `xor eax,eax; ret` in RWX memory too (where a guest-side fast path would live)
//   aot leaf   -- a C function in this binary's text (where the loader's imports live today)
//   aot ms_abi -- an ms_abi C function that calls a SysV function (what most loader imports do: xmm6-15 saved and restored)
// and reports ns per call (best of 5 runs). The difference between the first two is the price of leaving JIT-translated code.
// Then the clocks: mach_absolute_time (what QueryPerformanceCounter calls) vs rdtsc under Rosetta -- the cost of each and
// rdtsc's rate against mach_absolute_time (a leaf QueryPerformanceCounter could use rdtsc if its rate is fixed).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach_time.h>
#include <unistd.h>
static inline uint64_t rdtsc(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }

__attribute__((noinline)) static int aot_leaf(void){ __asm__ volatile(""); return 0; }
__attribute__((noinline)) static int sysv_leaf(int x){ __asm__ volatile(""); return x+1; }
__attribute__((noinline,ms_abi)) static int aot_msabi(int x){ return sysv_leaf(x); }

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }

int main(void){
    uint8_t*m=mmap(0,1<<16,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);
    if(m==MAP_FAILED){ m=mmap(0,1<<16,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); if(m==MAP_FAILED){ perror("mmap"); return 1; } }
    // the JIT leaf at +0x100: xor eax,eax; ret
    static const uint8_t leaf[]={0x31,0xC0,0xC3}; memcpy(m+0x100,leaf,sizeof leaf);
    // the caller at +0: SysV (rdi = count, rsi = target), Win64-style call through a slot with 32 bytes of shadow space:
    //   push rbx; push r12; sub rsp,0x28; mov rbx,rdi; mov r12,rsi
    //   loop: xor ecx,ecx; call r12; dec rbx; jnz loop
    //   add rsp,0x28; pop r12; pop rbx; ret
    static const uint8_t caller[]={0x53, 0x41,0x54, 0x48,0x83,0xEC,0x28, 0x48,0x89,0xFB, 0x49,0x89,0xF4,
        0x31,0xC9, 0x41,0xFF,0xD4, 0x48,0xFF,0xCB, 0x75,0xF6,
        0x48,0x83,0xC4,0x28, 0x41,0x5C, 0x5B, 0xC3};
    memcpy(m,caller,sizeof caller);
    __builtin___clear_cache((char*)m,(char*)m+0x200);
    typedef void (*Caller)(uint64_t n,void*target);
    Caller run=(Caller)(void*)m;
    struct { const char*name; void*t; } T[3]={{"jit leaf (guest-side code)",m+0x100},{"aot leaf (loader text)",(void*)aot_leaf},{"aot ms_abi -> SysV (a typical import)",(void*)aot_msabi}};
    const uint64_t N=20000000;
    for(int k=0;k<3;k++) run(N/10,T[k].t);   // warm up: every path translated first
    for(int k=0;k<3;k++){ double best=1e18; for(int r=0;r<5;r++){ double t0=now_ns(); run(N,T[k].t); double d=now_ns()-t0; if(d<best) best=d; }
        printf("%-40s %6.2f ns per call\n",T[k].name,best/N); }
    { const int M=5000000; volatile uint64_t sink=0; double t0=now_ns(); for(int i=0;i<M;i++) sink+=mach_absolute_time(); double t1=now_ns();
      for(int i=0;i<M;i++) sink+=rdtsc(); double t2=now_ns(); (void)sink;
      mach_timebase_info_data_t tb; mach_timebase_info(&tb);
      uint64_t m0=mach_absolute_time(), r0=rdtsc(); usleep(200000); uint64_t m1=mach_absolute_time(), r1=rdtsc();
      double ns=(double)(m1-m0)*tb.numer/tb.denom;
      printf("%-40s %6.2f ns per call\n","mach_absolute_time",(t1-t0)/M);
      printf("%-40s %6.2f ns per call\n","rdtsc",(t2-t1)/M);
      printf("rdtsc rate: %.6f ticks per ns (timebase %u/%u)\n",(double)(r1-r0)/ns,tb.numer,tb.denom); }
    return 0; }
