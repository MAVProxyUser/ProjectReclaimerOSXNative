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
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

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

// build a Win64 stub that calls on_stub(idx) and returns its result
static uint8_t *g_stubpool; static size_t g_stuboff;
static void *make_stub(uint32_t idx){
    uint8_t *p = g_stubpool + g_stuboff; g_stuboff += 32;
    int o=0;
    p[o++]=0x48; p[o++]=0x83; p[o++]=0xEC; p[o++]=0x28;            // sub rsp,0x28
    p[o++]=0xB9; *(uint32_t*)(p+o)=idx; o+=4;                       // mov ecx, idx
    p[o++]=0x48; p[o++]=0xB8; *(uint64_t*)(p+o)=(uint64_t)&on_stub; o+=8; // mov rax, &on_stub
    p[o++]=0xFF; p[o++]=0xD0;                                       // call rax
    p[o++]=0x48; p[o++]=0x83; p[o++]=0xC4; p[o++]=0x28;            // add rsp,0x28
    p[o++]=0xC3;                                                    // ret
    return p;
}

static inline uint8_t* rva(uint32_t r){ return g_base + r; }

int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "halo3.dll";
    ws("peload: loading "); ws(path); ws("\n");

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
                if(imp_count<8192){ imp_dll[imp_count]=dll; imp_fn[imp_count]=fn;
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
