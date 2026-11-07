// nhook: our tooling inside the NATIVE macOS Reclaimer app (0.10+), loaded with DYLD_INSERT_LIBRARIES (the app has no
// hardened runtime). It taps the app's log (write() on stdout/stderr, interposed) for the recompiled engine's image
// address ("[host] linked recompiled engine, image at 0x..."), checks that image against MCC's halo3.dll, then hooks
// lifted halo3 functions at their entries (the runtime's guest -> host code table; stub.S) to get the engine thread's
// guest context, and through it halo3's TLS block: the players table etc., as the loader's stats code reads them.
// Reads go through vm_read_overwrite (known addresses in our own process; fail cleanly) -- never a sweep.
// NHOOK_CENSUS=1 counts the calls of every lifted function (which halo3 code still runs); NHOOK_COUNT=1 a few named ones.
// Build: bash shim/native/build.sh. Run: bash shim/native.sh (test profile by default). Docs: shim/docs/native-client.md.
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>

static volatile uint64_t g_img;                 // the recompiled engine's image (guest halo3) as the app logs it
static int g_logfd = -1;                         // NHOOK_LOG=<file>: our lines go there too

static void nlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void nlog(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    write(2, b, n);                              // (dyld does not interpose the interposing image's own calls)
    if (g_logfd >= 0) write(g_logfd, b, n);
}

// a safe read of our own memory: 0 = ok
static int rd(uint64_t a, void* out, size_t n) {
    mach_vm_size_t got = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)out, &got);
    return (kr == KERN_SUCCESS && got == n) ? 0 : -1;
}


// ---- the app's runtime (0.10.0; shim/docs/native-client.md "Inside the app") ---------------------------------------
#define APP_IMAGE   0x103f94000ull   // the guest halo3 image: a reserved range of the app itself (unslid)
#define APP_CODETAB 0x103f44418ull   // {ptr, count}: sorted {preferred-base guest address, host code} pairs (22894)
static uint64_t g_slide, g_tab, g_ntab;

static uint64_t host_of(uint32_t rva) {           // the lifted entry of halo3+rva, 0 = not lifted
    uint64_t ga = 0x180000000ull + rva, lo = 0, hi = g_ntab;
    while (lo < hi) { uint64_t mid = (lo + hi) / 2, e[2]; if (rd(g_tab + mid * 16, e, 16)) return 0;
                      if (e[0] < ga) lo = mid + 1; else if (e[0] > ga) hi = mid; else return e[1]; }
    return 0;
}

// ---- entry hooks on lifted functions ------------------------------------------------------------------------------
// A lifted function's entry (its guest-ABI wrapper) starts stp x29,x30,[sp,#-16]! / mov x29,sp / stp x19,x20,[sp,#-16]! /
// ldr w<n>,[x0,#0x290], with x0 = the calling guest thread's context: guest rax +0, rcx +8, rdx +0x10, rbx +0x18, rsp +0x20,
// rbp +0x28, rsi +0x30, rdi +0x38, r8..r15 +0x40..+0x78, +0x2d8 its TLS pointer (gs:[0x58], cached by the runtime's helper
// once the thread read it). Lifted code calls lifted code through these entries with the guest state in the context.
// The first instruction becomes `b <stub>` (one atomic word; the code is MAP_JIT, written with pthread_jit_write_protect_np); the stub (stub.S) calls handler(ctx).
typedef void (*nh_handler)(uint64_t* ctx);
extern char nh_tpl_start[], nh_tpl_displaced[], nh_tpl_handler[], nh_tpl_return[], nh_tpl_end[];

static uint8_t* near_page(uint64_t code, size_t size) {   // a fresh RW page within +-120 MB of the lifted code
    mach_vm_address_t ra = code; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t bc = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &bc, &on) != KERN_SUCCESS) return 0;
    // fixed-address allocation attempts (nothing is read): just past the code region, then just below it, 1 MB steps
    for (uint64_t step = 0; step < 120; step++)
        for (int side = 0; side < 2; side++) {
            mach_vm_address_t a = side ? ra - size - step * 0x100000 : ra + rs + step * 0x100000;
            int64_t d = (int64_t)(a - code);
            if (d >= 0x7800000 || d <= -0x7800000) continue;
            if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) == KERN_SUCCESS) return (uint8_t*)a;
        }
    return 0;
}

static int hook_install(const char* name, uint32_t rva, nh_handler h) {
    uint64_t e = host_of(rva); uint32_t w[4];
    if (!e || rd(e, w, 16)) { nlog("[nhook] hook %s: halo3+0x%x not lifted", name, rva); return -1; }
    if (w[0] != 0xa9bf7bfd || w[1] != 0x910003fd || w[2] != 0xa9bf53f3 || (w[3] & 0xffffffe0) != 0xb9429000) {   // ldr w<n>,[x0,#0x290]
        nlog("[nhook] hook %s: halo3+0x%x entry %08x %08x %08x %08x is not a lifted wrapper: refused", name, rva, w[0], w[1], w[2], w[3]); return -1; }
    size_t tl = nh_tpl_end - nh_tpl_start;
    uint8_t* st = near_page(e, 0x4000);              // its own page: a page that holds a live stub is never made writable again
    if (!st) { nlog("[nhook] hook %s: no stub page near 0x%llx", name, (unsigned long long)e); return -1; }
    int64_t d = (int64_t)((uint64_t)st - e);
    memcpy(st, nh_tpl_start, tl);
    memcpy(st + (nh_tpl_displaced - nh_tpl_start), &w[0], 4);                 // the displaced stp x29,x30,[sp,#-16]!
    uint64_t hv = (uint64_t)h, rv = e + 4;
    memcpy(st + (nh_tpl_handler - nh_tpl_start), &hv, 8); memcpy(st + (nh_tpl_return - nh_tpl_start), &rv, 8);
    mach_vm_protect(mach_task_self(), (mach_vm_address_t)st, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    sys_icache_invalidate(st, tl);
    uint32_t b = 0x14000000u | ((uint32_t)(d >> 2) & 0x03ffffffu);
    pthread_jit_write_protect_np(0);                // the code is MAP_JIT: writable only for a thread that turns it so
    __atomic_store_n((uint32_t*)e, b, __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate((void*)e, 4);
    nlog("[nhook] hook %s: halo3+0x%x (host 0x%llx) -> stub 0x%llx", name, rva, (unsigned long long)e, (unsigned long long)st);
    return 0;
}

// ---- census (NHOOK_CENSUS=1): which lifted functions run at all ---------------------------------------------------
// Every lifted wrapper in the code table gets a 48-byte counting stub (stub.S _nh_cen_*); NHOOK_CENSUS_SECS later the
// functions that ran are written to NHOOK_CENSUS_OUT (default /tmp/nhook_census.txt): rva, calls, last context.
extern char nh_cen_start[], nh_cen_displaced[], nh_cen_slot[], nh_cen_return[], nh_cen_end[];
typedef struct { uint64_t calls, ctx; } CenSlot;
static CenSlot* g_cen; static uint64_t* g_cen_ga; static uint32_t g_cen_n;
static void* census_dump(void* arg) {
    (void)arg;
    int secs = getenv("NHOOK_CENSUS_SECS") ? atoi(getenv("NHOOK_CENSUS_SECS")) : 30;
    for (int round = 1;; round++) {
        sleep(secs);
        const char* out = getenv("NHOOK_CENSUS_OUT") ? getenv("NHOOK_CENSUS_OUT") : "/tmp/nhook_census.txt";
        FILE* f = fopen(out, "w"); if (!f) return 0;
        uint32_t ran = 0;
        for (uint32_t i = 0; i < g_cen_n; i++) if (g_cen[i].calls) { ran++;
            fprintf(f, "halo3+0x%llx\t%llu\t0x%llx\n", (unsigned long long)(g_cen_ga[i] - 0x180000000ull), (unsigned long long)g_cen[i].calls, (unsigned long long)g_cen[i].ctx); }
        fclose(f);
        nlog("[nhook] census round %d: %u of %u lifted functions ran -> %s", round, ran, g_cen_n, out);
    }
    return 0;
}
static void census_install(void) {
    size_t tl = nh_cen_end - nh_cen_start, slot = (tl + 15) & ~(size_t)15, psz = 0x40000; uint64_t* tab = malloc(g_ntab * 16);
    if (!tab || rd(g_tab, tab, g_ntab * 16)) return;
    g_cen = calloc(g_ntab, sizeof *g_cen); g_cen_ga = calloc(g_ntab, 8);
    uint64_t* ent = calloc(g_ntab, 8); uint32_t* br = calloc(g_ntab, 4);
    uint8_t* pages[512]; int np = 0; uint8_t* page = 0; size_t used = 0; uint32_t odd = 0, far = 0;
    for (uint64_t i = 0; i < g_ntab; i++) {         // pass 1: every stub written into RW pages
        uint64_t e = tab[2 * i + 1]; uint32_t w[4];
        if (rd(e, w, 16) || w[0] != 0xa9bf7bfd || w[1] != 0x910003fd || w[2] != 0xa9bf53f3 || (w[3] & 0xffffffe0) != 0xb9429000) { odd++; continue; }
        if (!page || used + slot > psz || (int64_t)((uint64_t)page - e) > 0x7000000 || (int64_t)((uint64_t)page - e) < -0x7000000) {
            if (np == 512 || !(page = near_page(e, psz))) { far++; page = 0; continue; }
            pages[np++] = page; used = 0; }
        uint8_t* st = page + used; int64_t d = (int64_t)((uint64_t)st - e);
        if (d >= (1ll << 27) || d < -(1ll << 27)) { far++; continue; }
        memcpy(st, nh_cen_start, tl); memcpy(st + (nh_cen_displaced - nh_cen_start), &w[0], 4);
        uint64_t sv = (uint64_t)&g_cen[g_cen_n], rv = e + 4;
        memcpy(st + (nh_cen_slot - nh_cen_start), &sv, 8); memcpy(st + (nh_cen_return - nh_cen_start), &rv, 8);
        used += slot; g_cen_ga[g_cen_n] = tab[2 * i]; ent[g_cen_n] = e; br[g_cen_n] = 0x14000000u | ((uint32_t)(d >> 2) & 0x03ffffffu); g_cen_n++;
    }
    for (int k = 0; k < np; k++) {                   // pass 2: the pages executable
        mach_vm_protect(mach_task_self(), (mach_vm_address_t)pages[k], psz, 0, VM_PROT_READ | VM_PROT_EXECUTE);
        sys_icache_invalidate(pages[k], psz); }
    pthread_jit_write_protect_np(0);                 // pass 3: the entries
    for (uint32_t i = 0; i < g_cen_n; i++) __atomic_store_n((uint32_t*)ent[i], br[i], __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    for (uint32_t i = 0; i < g_cen_n; i++) sys_icache_invalidate((void*)ent[i], 4);
    nlog("[nhook] census: %u stubs on %d pages, %u entries not lifted wrappers, %u without a near page", g_cen_n, np, odd, far);
    free(tab); free(ent); free(br);
    pthread_t t; pthread_create(&t, 0, census_dump, 0); pthread_detach(t);
}

// ---- engine state from the main thread's context --------------------------------------------------------------------
static volatile uint64_t g_main_ctx;               // the engine main thread's guest context (a hooked engine function's caller)
static volatile uint64_t g_frames;
// NHOOK_COUNT=1: entry hooks that only count calls (which engine functions run in the menus / in a match)
static const struct { const char* name; uint32_t rva; } g_cnt_fns[] = {
    {"main_loop_body", 0xb2250}, {"sim_tick", 0xef120}, {"local_user_count", 0xfbb94}, {"local_player", 0xfbc04},
    {"display_event", 0x11b0ec}, {"object_delete", 0x342ed8}, {"render_prep", 0x2d1390}, {"objects_update", 0x34067c},
};
#define NCNT (sizeof g_cnt_fns / sizeof g_cnt_fns[0])
static volatile uint64_t g_cnt[NCNT];
#define CNT_HANDLER(i) static void cnt_##i(uint64_t* ctx) { g_cnt[i]++; if (ctx[0x2d8 / 8]) g_main_ctx = (uint64_t)ctx; }
CNT_HANDLER(0) CNT_HANDLER(1) CNT_HANDLER(2) CNT_HANDLER(3) CNT_HANDLER(4) CNT_HANDLER(5) CNT_HANDLER(6) CNT_HANDLER(7)
static nh_handler g_cnt_h[NCNT] = { cnt_0, cnt_1, cnt_2, cnt_3, cnt_4, cnt_5, cnt_6, cnt_7 };
static void on_main_loop(uint64_t* ctx) {           // the engine thread's context (hook_install "engine_clock")
    g_main_ctx = (uint64_t)ctx; g_frames++;
}

static uint64_t tls_block(void) {                   // halo3's TLS block of the engine main thread (index 0 here)
    uint64_t c = g_main_ctx, arr = 0, blk = 0; uint32_t idx = 0;
    if (!c || rd(c + 0x2d8, &arr, 8) || !arr || rd(g_img + 0xa39f9c, &idx, 4) || idx > 64 || rd(arr + idx * 8, &blk, 8)) return 0;
    return blk;
}

static void players_log(void) {
    uint64_t blk = tls_block(), parr = 0; uint8_t hdr[0x60];
    if (!blk) { nlog("[nhook] players: no TLS block yet (frames %llu)", (unsigned long long)g_frames); return; }
    if (rd(blk + 0x10, &parr, 8) || !parr || rd(parr, hdr, sizeof hdr) || *(uint32_t*)(hdr + 0x2c) != 0x64407440) {
        nlog("[nhook] players: TLS block 0x%llx, +0x10 = 0x%llx is no data array", (unsigned long long)blk, (unsigned long long)parr); return; }
    int32_t es = *(int32_t*)(hdr + 0x24), hi = *(int32_t*)(hdr + 0x3c); uint64_t pd = *(uint64_t*)(hdr + 0x48);
    char nm[33] = {0}; memcpy(nm, hdr, 32);
    nlog("[nhook] players: TLS block 0x%llx, array \"%s\" elem 0x%x, %d slots used, frames %llu", (unsigned long long)blk, nm, es, hi, (unsigned long long)g_frames);
    for (int i = 0; i < hi && i < 32; i++) {
        uint8_t rec[0x98]; if (rd(pd + (uint64_t)i * es, rec, sizeof rec) || !*(uint16_t*)rec) continue;
        char name[40] = {0}; const uint16_t* w = (const uint16_t*)(rec + 0x58);
        for (int c = 0; c < 32 && w[c]; c++) name[c] = w[c] < 0x80 ? (char)w[c] : '?';
        nlog("[nhook]   slot %d unit 0x%08x \"%s\"", i, *(uint32_t*)(rec + 0x28), name);
    }
}

static void* engine_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        sleep(5);
        if (getenv("NHOOK_COUNT")) { char b[512]; int n = 0;
            for (size_t k = 0; k < NCNT; k++) n += snprintf(b + n, sizeof b - n, " %s %llu", g_cnt_fns[k].name, (unsigned long long)g_cnt[k]);
            nlog("[nhook] calls:%s", b); }
        players_log();
    }
    return 0;
}

static void hooks_start(void) {
    g_slide = g_img - APP_IMAGE;
    uint64_t tab[2]; if (rd(APP_CODETAB + g_slide, tab, 16) || !tab[0] || tab[1] < 1000 || tab[1] > 1000000) { nlog("[nhook] no code table: hooks off"); return; }
    g_tab = tab[0]; g_ntab = tab[1];
    if (getenv("NHOOK_CENSUS")) census_install();
    else if (getenv("NHOOK_COUNT")) for (size_t k = 0; k < NCNT; k++) hook_install(g_cnt_fns[k].name, g_cnt_fns[k].rva, g_cnt_h[k]);
    // the engine thread's context: halo3+0x9d22c (a millisecond clock read from ~420 places) runs lifted on the engine
    // thread ~7000 times a second in a match -- main_loop_body (0xb2250) and the sim tick are the app's own code now
    else if (!getenv("NHOOK_NO_HOOKS")) hook_install("engine_clock", 0x9d22c, on_main_loop);
    pthread_t t; pthread_create(&t, 0, engine_thread, 0); pthread_detach(t);
}

static void* probe_thread(void* arg) {
    (void)arg;
    sleep(2);
    const char* path = getenv("NHOOK_HALO3");
    if (!path) { nlog("[nhook] NHOOK_HALO3 not set: no image check"); return 0; }
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { nlog("[nhook] cannot open %s", path); return 0; }
    uint8_t* f = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (f == MAP_FAILED) return 0;
    uint32_t pe = *(uint32_t*)(f + 0x3c); uint16_t nsec = *(uint16_t*)(f + pe + 6), opt = *(uint16_t*)(f + pe + 20);
    uint8_t* oh = f + pe + 24; uint64_t ibase = *(uint64_t*)(oh + 24); uint32_t isize = *(uint32_t*)(oh + 56);
    uint8_t* dd = oh + 112; uint8_t* sh = oh + opt;
    uint64_t img = g_img;
    nlog("[nhook] halo3.dll preferred base 0x%llx size 0x%x; engine image at 0x%llx", (unsigned long long)ibase, isize,
         (unsigned long long)img);
    uint8_t hdr[2]; nlog("[nhook] image+0: %s", rd(img, hdr, 2) ? "unreadable" : (hdr[0] == 'M' && hdr[1] == 'Z' ? "MZ (headers mapped)" : "no MZ"));
    #define FOFF(rva) ({ uint32_t _r = (rva), _o = 0; for (int _i = 0; _i < nsec; _i++) { uint8_t* s = sh + _i * 40; \
        uint32_t va = *(uint32_t*)(s + 12), vs = *(uint32_t*)(s + 8), ra = *(uint32_t*)(s + 20), rs = *(uint32_t*)(s + 16); \
        if (_r >= va && _r < va + (vs > rs ? vs : rs)) { _o = _r - va + ra; break; } } _o; })
    for (int i = 0; i < nsec; i++) {                 // each section: how much of its first 64 KB matches the file
        uint8_t* s = sh + i * 40; char nm[9] = {0}; memcpy(nm, s, 8);
        uint32_t va = *(uint32_t*)(s + 12), rs = *(uint32_t*)(s + 16), ra = *(uint32_t*)(s + 20);
        uint32_t n = rs < 65536 ? rs : 65536; if (!n) { nlog("[nhook] %-8s rva 0x%x: no file data", nm, va); continue; }
        uint8_t* m = malloc(n); int ok = !rd(img + va, m, n); uint32_t same = 0;
        if (ok) for (uint32_t k = 0; k < n; k++) same += m[k] == f[ra + k];
        nlog("[nhook] %-8s rva 0x%-8x %s: %u of %u bytes as in the file", nm, va, ok ? "readable" : "UNREADABLE", same, n);
        free(m);
    }
    // TLS directory: the index variable
    uint32_t tls_rva = *(uint32_t*)(dd + 9 * 8);
    if (tls_rva) {
        uint8_t* t = f + FOFF(tls_rva); uint64_t aoi = *(uint64_t*)(t + 16);
        uint32_t idx = 0xdeadbeef; rd(img + (aoi - ibase), &idx, 4);
        nlog("[nhook] TLS: template 0x%llx..0x%llx zero fill %u, index at rva 0x%llx = %u",
             (unsigned long long)(*(uint64_t*)t - ibase), (unsigned long long)(*(uint64_t*)(t + 8) - ibase),
             *(uint32_t*)(t + 24), (unsigned long long)(aoi - ibase), idx);
    }
    // import slots: what the runtime put there
    uint32_t imp = *(uint32_t*)(dd + 1 * 8); int shown = 0;
    for (uint8_t* id = f + FOFF(imp); *(uint32_t*)(id + 12) && shown < 400; id += 20) {
        const char* dll = (const char*)f + FOFF(*(uint32_t*)(id + 12));
        uint32_t ilt = *(uint32_t*)id ? *(uint32_t*)id : *(uint32_t*)(id + 16), iat = *(uint32_t*)(id + 16);
        for (int k = 0;; k++) {
            uint64_t e = *(uint64_t*)(f + FOFF(ilt) + k * 8); if (!e) break;
            const char* name = (e >> 63) ? "#ord" : (const char*)f + FOFF((uint32_t)e) + 2;
            uint64_t v = 0; int ok = !rd(img + iat + k * 8, &v, 8);
            Dl_info di = {0}; int dl = ok && v && dladdr((void*)v, &di);
            const char* where = !ok ? "unreadable" : (v >= img && v < img + isize) ? "inside the guest image" :
                                 dl ? (di.dli_sname ? di.dli_sname : (di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?")) : "anonymous memory";
            if (shown < 400 && (k < 3 || strstr(name, "Tls") || strstr(name, "QueryPerformanceCounter") || strstr(name, "GetCurrentThreadId")))
                nlog("[nhook] import %s!%s slot rva 0x%x = 0x%llx (%s%s0x%llx)", dll, name, iat + k * 8, (unsigned long long)v, where,
                     dl ? " +" : " ", dl ? (unsigned long long)(v - (uint64_t)di.dli_saddr) : (unsigned long long)(v - img)), shown++;
        }
    }
    // the runtime's guest -> host code table (0.10.0: sorted {preferred-base address, host code} pairs, (ptr, count) at
    // app 0x103f44418; the guest image is the app's own reserved range at 0x103f94000 -- both + the app's slide)
    uint64_t slide = _dyld_get_image_vmaddr_slide(0), aslide = img - 0x103f94000ull;
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if (strstr(_dyld_get_image_name(i), "/project-reclaimer")) slide = _dyld_get_image_vmaddr_slide(i);
    nlog("[nhook] app slide 0x%llx, image - 0x103f94000 = 0x%llx", (unsigned long long)slide, (unsigned long long)aslide);
    uint64_t tab[2] = {0};
    if (!rd(0x103f44418ull + slide, tab, 16)) {
        nlog("[nhook] code table at 0x%llx, %llu entries", (unsigned long long)tab[0], (unsigned long long)tab[1]);
        uint64_t e[8]; if (!rd(tab[0], e, sizeof e)) for (int i = 0; i < 4; i++) nlog("[nhook]   [%d] 0x%llx -> 0x%llx", i, (unsigned long long)e[2*i], (unsigned long long)e[2*i+1]);
        const char* want = getenv("NHOOK_DUMPFN");   // NHOOK_DUMPFN=<rva>[,<rva>...]: hex of each function's host code
        for (const char* w = want; w && *w; ) {
            uint64_t rva = strtoull(w, 0, 16), ga = 0x180000000ull + rva, lo = 0, hi = tab[1], host = 0;
            while (lo < hi) { uint64_t mid = (lo + hi) / 2, ent[2]; if (rd(tab[0] + mid * 16, ent, 16)) break;
                              if (ent[0] < ga) lo = mid + 1; else if (ent[0] > ga) hi = mid; else { host = ent[1]; break; } }
            nlog("[nhook] halo3+0x%llx -> host 0x%llx", (unsigned long long)rva, (unsigned long long)host);
            if (host) { mach_vm_address_t ra = host; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t bc = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on;
                if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &bc, &on) == KERN_SUCCESS)
                    nlog("[nhook]   its region 0x%llx+0x%llx prot %d/%d", (unsigned long long)ra, (unsigned long long)rs, bi.protection, bi.max_protection); }
            int nw = getenv("NHOOK_DUMPLEN") ? atoi(getenv("NHOOK_DUMPLEN")) & ~7 : 160; if (nw <= 0 || nw > 4096) nw = 160;
            if (host) {
                uint32_t* code = calloc(nw, 4);
                if (rd(host, code, nw * 4)) nlog("[nhook]   host code unreadable");
                else for (int k = 0; k < nw; k += 8)
                    nlog("[nhook] code 0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned long long)(host + k * 4),
                         code[k], code[k+1], code[k+2], code[k+3], code[k+4], code[k+5], code[k+6], code[k+7]);
                free(code);
            }
            w = strchr(w, ','); if (w) w++;
        }
    }
    munmap(f, st.st_size);
    nlog("[nhook] probe done");
    hooks_start();
    return 0;
}

// one whole log line of the app (stdout / stderr)
static void on_line(const char* p, size_t n) {
    static const char key[] = "engine, image at 0x";
    if (n > 7 && !memcmp(p, "[nhook]", 7)) return;
    if (!g_img)
        for (size_t i = 0; i + sizeof key - 1 <= n; i++)
            if (p[i] == 'e' && !memcmp(p + i, key, sizeof key - 1)) {
                uint64_t v = 0;
                for (size_t j = i + sizeof key - 1; j < n; j++) {
                    int c = p[j], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                    if (d < 0) break;
                    v = v * 16 + d;
                }
                if (v) { g_img = v; pthread_t t; pthread_create(&t, 0, probe_thread, 0); pthread_detach(t); }
                break;
            }
}

// The app writes a line in pieces ("[host] ", "Project Reclaimer", ...; Rust's unbuffered stderr, the pieces of one line
// under its lock) and later moves fd 2 onto a pipe it copies into its log file: assemble lines per fd (1, 2) here.
static pthread_mutex_t g_lb_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_lb[3][4096]; static size_t g_lbn[3];
static void tap(int fd, const char* p, size_t n) {
    if (fd != 1 && fd != 2) return;
    pthread_mutex_lock(&g_lb_mu);
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n') { on_line(g_lb[fd], g_lbn[fd]); g_lbn[fd] = 0; continue; }
        if (g_lbn[fd] < sizeof g_lb[fd]) g_lb[fd][g_lbn[fd]++] = c;
    }
    pthread_mutex_unlock(&g_lb_mu);
}

static int g_trace_writes;                       // NHOOK_TRACE_WRITES=<n>: log the first n writes to fds 1 / 2
static void trace_write(const char* what, int fd, const void* buf, size_t n) {
    if (g_trace_writes <= 0 || (fd != 1 && fd != 2) || (n > 7 && !memcmp(buf, "[nhook]", 7))) return;
    g_trace_writes--; char s[48] = {0}; size_t k = n < 40 ? n : 40;
    for (size_t i = 0; i < k; i++) { char c = ((const char*)buf)[i]; s[i] = c >= 32 && c < 127 ? c : '.'; }
    nlog("[nhook] %s(fd %d, %zu bytes) \"%s\"", what, fd, n, s);
}
static ssize_t my_write(int fd, const void* buf, size_t n) {
    trace_write("write", fd, buf, n);
    if (n && !(n > 7 && !memcmp(buf, "[nhook]", 7))) tap(fd, buf, n);
    return write(fd, buf, n);
}
static ssize_t my_writev(int fd, const struct iovec* iov, int cnt) {
    for (int i = 0; i < cnt; i++) if (iov[i].iov_len) { trace_write("writev", fd, iov[i].iov_base, iov[i].iov_len); tap(fd, iov[i].iov_base, iov[i].iov_len); }
    return writev(fd, iov, cnt);
}

#define INTERPOSE(rep, orig) __attribute__((used)) static struct { const void* r; const void* o; } \
    _ip_##orig __attribute__((section("__DATA,__interpose"))) = { (const void*)(rep), (const void*)(orig) }
INTERPOSE(my_write, write);
INTERPOSE(my_writev, writev);

__attribute__((constructor)) static void nhook_init(void) {
    const char* lf = getenv("NHOOK_LOG");
    if (lf) g_logfd = open(lf, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (getenv("NHOOK_TRACE_WRITES")) g_trace_writes = atoi(getenv("NHOOK_TRACE_WRITES"));
    nlog("[nhook] loaded (pid %d)", getpid());
}
