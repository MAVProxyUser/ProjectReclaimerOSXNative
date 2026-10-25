// cowtest: is the loader's copy-on-write guest copy (peload.c crt_big_memmove: mach_vm_copy of whole 16 KB pages) a real
// copy under Rosetta when it runs the way halo3 uses it -- the same ~16 MB game state copied between the same buffers many
// times a second, the source written between copies, the destination read (and sometimes written) meanwhile, on another
// thread too? Every round checks that the destination holds exactly what the source held at the copy, and stays so while
// the source is changed afterwards; a mismatch is printed with its offset.
//     clang -arch x86_64 -O2 -o /tmp/cowtest shim/tools/cowtest.c && arch -x86_64 /tmp/cowtest [rounds] [MB] [chain]
// chain = 1: three buffers inside one bigger allocation, copied in a ring (A -> B, B -> C, C -> A: copies of copies), each
// checked against what its source held.
// Also prints vm_region's share mode of both buffers now and then (the loader's cow_private_range reads it).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static size_t N;
static uint8_t *A, *B, *C;   // A: the live state; B: the copy (game-state snapshot); C: what A held at the copy
static volatile int stop;
static volatile uint64_t reads;

static const char *sm(int m){ switch(m){ case SM_COW: return "COW"; case SM_PRIVATE: return "PRIVATE"; case SM_EMPTY: return "EMPTY";
    case SM_SHARED: return "SHARED"; case SM_TRUESHARED: return "TRUESHARED"; case SM_PRIVATE_ALIASED: return "PRIVATE_ALIASED";
    case SM_SHARED_ALIASED: return "SHARED_ALIASED"; case SM_LARGE_PAGE: return "LARGE_PAGE"; default: return "?"; } }
static void region(const char *nm, void *p){
    mach_vm_address_t a=(mach_vm_address_t)(uintptr_t)p; mach_vm_size_t s=0; vm_region_extended_info_data_t ei;
    mach_msg_type_number_t cnt=VM_REGION_EXTENDED_INFO_COUNT; mach_port_t o=MACH_PORT_NULL;
    if(mach_vm_region(mach_task_self(),&a,&s,VM_REGION_EXTENDED_INFO,(vm_region_info_t)&ei,&cnt,&o)==KERN_SUCCESS)
        printf("    %s: region %#llx +%#llx share mode %s, external pager %u, ref count %u\n",nm,(unsigned long long)a,(unsigned long long)s,sm(ei.share_mode),ei.external_pager,ei.ref_count); }

static void *reader(void *x){ (void)x; uint64_t sum=0;   // another thread reading the snapshot all the time (halo3's render thread)
    while(!stop){ for(size_t i=0;i<N;i+=4096) sum+=B[i]; reads++; }
    return (void*)(uintptr_t)sum; }

int main(int argc, char **argv){
    int rounds=argc>1?atoi(argv[1]):2000; N=(size_t)(argc>2?atoi(argv[2]):16)<<20; int chain=argc>3?atoi(argv[3]):0;
    if(chain){   // copies of copies, the buffers 16 KB-congruent inside one VirtualAlloc-like region
        uint8_t*big=mmap(0,3*N+3*16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); if(big==MAP_FAILED){ perror("mmap"); return 1; }
        uint8_t*buf[3]={big+16384,big+N+2*16384,big+2*N+3*16384}; uint8_t*ref[3]; for(int i=0;i<3;i++){ ref[i]=malloc(N); for(size_t k=0;k<N;k++) buf[i][k]=ref[i][k]=(uint8_t)(k*(3+i)); }
        B=buf[1]; pthread_t th; pthread_create(&th,0,reader,0); uint64_t bad=0, cows=0; uint32_t seed=1;
        for(int r=0;r<rounds;r++){ int s=r%3, d=(r+1)%3;
            if(mach_vm_copy(mach_task_self(),(mach_vm_address_t)(uintptr_t)buf[s],N,(mach_vm_address_t)(uintptr_t)buf[d])==KERN_SUCCESS) cows++; else memmove(buf[d],buf[s],N);
            memcpy(ref[d],ref[s],N);
            for(int b=0;b<3;b++) for(int k=0;k<1500;k++){ seed=seed*1103515245u+12345u; size_t o=(seed>>4)%N; uint8_t v=(uint8_t)(seed>>24)|1; buf[b][o]^=v; ref[b][o]^=v; }
            for(int b=0;b<3;b++) if(memcmp(buf[b],ref[b],N)){ size_t i=0; while(i<N && buf[b][i]==ref[b][i]) i++; bad++; if(bad<=10) printf("  round %d buffer %d: MISMATCH at +%#zx (%02x, expected %02x)\n",r,b,i,buf[b][i],ref[b][i]); }
            if(r==0 || r==30 || r==rounds-1){ printf("  after round %d:\n",r); region("A",buf[0]); region("B",buf[1]); region("C",buf[2]); } }
        stop=1; pthread_join(th,0);
        printf("chain: %d rounds of %zu MB: %llu by mach_vm_copy, %llu mismatches; reader passes %llu\n",rounds,N>>20,(unsigned long long)cows,(unsigned long long)bad,(unsigned long long)reads);
        return bad?2:0; }
    // like VirtualAlloc'd guest memory: anonymous, private; the two buffers 16 KB-congruent as halo3's are
    A=mmap(0,N,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); B=mmap(0,N,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); C=malloc(N);
    if(A==MAP_FAILED||B==MAP_FAILED||!C){ perror("mmap"); return 1; }
    for(size_t i=0;i<N;i++) A[i]=(uint8_t)(i*7);
    pthread_t th; pthread_create(&th,0,reader,0);
    uint64_t bad=0, cows=0; uint32_t seed=1;
    for(int r=0;r<rounds;r++){
        memcpy(C,A,N);
        if(mach_vm_copy(mach_task_self(),(mach_vm_address_t)(uintptr_t)A,N,(mach_vm_address_t)(uintptr_t)B)==KERN_SUCCESS) cows++; else memmove(B,A,N);
        // the source changes after the copy (the next tick); the snapshot must not see it
        for(int k=0;k<4000;k++){ seed=seed*1103515245u+12345u; size_t o=(seed>>4)%N; A[o]^=(uint8_t)(seed>>24)|1; }
        // and the snapshot is written now and then too (halo3 writes into its copies)
        if(r%7==3){ for(int k=0;k<100;k++){ seed=seed*1103515245u+12345u; size_t o=(seed>>4)%N; B[o]^=1; C[o]^=1; } }
        if(memcmp(B,C,N)){ size_t i=0; while(i<N && B[i]==C[i]) i++; bad++; if(bad<=10) printf("  round %d: MISMATCH at +%#zx (copy %02x, expected %02x)\n",r,i,B[i],C[i]); }
        if(r==0 || r==10 || r==rounds-1){ printf("  after round %d:\n",r); region("source",A); region("copy  ",B); }
    }
    stop=1; pthread_join(th,0);
    printf("%d rounds of %zu MB: %llu by mach_vm_copy, %llu mismatches; reader passes %llu\n",rounds,N>>20,(unsigned long long)cows,(unsigned long long)bad,(unsigned long long)reads);
    return bad?2:0; }
