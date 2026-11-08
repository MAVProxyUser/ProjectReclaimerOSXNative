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
#include <math.h>
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
#include "nhcost.h"                            // the hook cost meter: every hook times itself
#include "nhchk.h"                             // the hooks check: every hook / address / tap and whether it works
static int g_logfd = -1;                         // NHOOK_LOG=<file>: our lines go there too

static void nlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static __thread char t_lastlog[200];             // this thread's last line (the hooks check's "why" for a hook that failed)
static const char* hk_why(void) { return !strncmp(t_lastlog, "[nhook] ", 8) ? t_lastlog + 8 : t_lastlog; }
static void nlog(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    snprintf(t_lastlog, sizeof t_lastlog, "%s", b);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    write(2, b, n);                              // (dyld does not interpose the interposing image's own calls)
    if (g_logfd >= 0) write(g_logfd, b, n);
}

// our background threads (stats snapshot, HTTP, forensic writer, reverse DNS, ...) run at QOS_CLASS_UTILITY: on Apple silicon
// that keeps them on the efficiency cores while the game's threads want the performance cores, so they never take a core
// the game needs (user, 2026-11-07: "does the stats snapshot lag our view? we don't want it to ever do that")
#include <pthread/qos.h>
static int nh_thread(pthread_t* t, void* (*fn)(void*), void* arg, qos_class_t q) {
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_set_qos_class_np(&at, q, 0); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(t, &at, fn, arg); pthread_attr_destroy(&at); return r; }
void nh_log(const char* fmt, ...) {                 // (nrender.m's logger)
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); nlog("%s", b);
}

// a safe read of our own memory: 0 = ok. Every call is a system call (~1-2 us): t_rd_n / t_rd_b count them per thread;
// NHOOK_RD_PROF=1 also counts them per call site on the threads that ask (t_rd_prof: the stats snapshot) -- nstats.c prints them.
static __thread unsigned long t_rd_n, t_rd_b; static __thread int t_rd_prof;
static struct { uintptr_t site; unsigned long n, b; } g_rd_site[256]; static int g_rd_nsite;
__attribute__((noinline)) static void rd_prof(uintptr_t site, size_t n) {
    for (int i = 0; i < g_rd_nsite; i++) if (g_rd_site[i].site == site) { g_rd_site[i].n++; g_rd_site[i].b += n; return; }
    if (g_rd_nsite < 256) { g_rd_site[g_rd_nsite].site = site; g_rd_site[g_rd_nsite].n = 1; g_rd_site[g_rd_nsite].b = n; g_rd_nsite++; } }
// The stats snapshot copies the engine's object memory once per run (st_ocache_fill) and its thread's reads inside that copy
// are answered from it (t_oc_*: no system call). A read is never more than RD_CHUNK bytes per system call: above ~32 KB the
// kernel copies by marking the SOURCE pages copy-on-write -- the game's own pages -- and the game's next write to each one
// then faults on the game's thread (a stall caused by a reader; the old snapshot read the 156 KB results table that way).
#define RD_CHUNK 16384
typedef struct { const uint8_t* buf, * ok; uint64_t lo, hi; } RdCopy;   // (ok: per 16 KB chunk)
enum { RD_NCOPY = 4 };
static __thread RdCopy t_oc[RD_NCOPY]; static __thread int t_noc;     // the snapshot thread's copies (st_ocache_fill)
static __thread unsigned long t_oc_hits;
static int rd_sys(uint64_t a, void* out, size_t n) {
    for (size_t o = 0; o < n; ) { size_t c = n - o < RD_CHUNK ? n - o : RD_CHUNK; mach_vm_size_t got = 0;
        kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), a + o, c, (mach_vm_address_t)((uint8_t*)out + o), &got);
        if (kr != KERN_SUCCESS || got != c) return -1; o += c; }
    return 0; }
static int rd(uint64_t a, void* out, size_t n) {
    for (int r = 0; r < t_noc && n; r++) { const RdCopy* q = &t_oc[r]; if (a < q->lo || a + n > q->hi) continue;   // (the snapshot's copies)
        uint64_t c0 = (a - q->lo) / RD_CHUNK, c1 = (a + n - 1 - q->lo) / RD_CHUNK; int ok = 1; for (uint64_t c = c0; c <= c1; c++) if (!q->ok[c]) { ok = 0; break; }
        if (ok) { memcpy(out, q->buf + (a - q->lo), n); t_oc_hits++; return 0; } break; }
    t_rd_n++; t_rd_b += n; if (t_rd_prof) rd_prof((uintptr_t)__builtin_return_address(0), n);
    return rd_sys(a, out, n);
}


// ---- the app's runtime: its addresses per build (2026-11-08, client 0.10.1) ----------------------------------------
// Everything nhook uses at a fixed app address comes from napp_builds.h: one row per app build, keyed by the app binary's
// LC_UUID, made by re-tools/machoport.py (it finds each point again in a new build by its code and checks what it means;
// 0 = not found there: that feature stays off). Unslid vmaddrs + the app's slide (dyld). shim/docs/native-client.md "Inside
// the app" / "When the app updates". NHOOK_APP_BUILD=<version> = that row whatever the UUID (a re-signed copy of a known build).
// Until 2026-11-08 these were 0.10.0 constants (APP_IMAGE 0x103f94000, APP_CODETAB 0x103f44418, ...): 0.10.1 came out and
// every hook stayed off ("no code table: hooks off").
typedef struct { uint64_t image, codetab, opus_decode, mixer_call, mixer_id, mixer_id_off, frame_fn, chat_gate_a, chat_gate_b, chat_log, muted, set_muted; } AppAddrs;
typedef struct { const char* version, * uuid; AppAddrs a; } AppBuild;
#include "napp_builds.h"
static AppAddrs g_app; static const char* g_app_ver = "unknown"; static char g_app_uuid[40]; static uint64_t g_app_slide; static int g_app_known;
#define APP(f) (g_app.f ? g_app.f + g_app_slide : 0ull)
static uint64_t g_slide, g_tab, g_ntab;
static void app_build_pick(void) {
    const struct mach_header_64* mh = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) { const char* n = _dyld_get_image_name(i);
        if (n && strstr(n, "/project-reclaimer") && !strstr(n, ".dylib")) { mh = (const struct mach_header_64*)_dyld_get_image_header(i); g_app_slide = (uint64_t)_dyld_get_image_vmaddr_slide(i); break; } }
    if (!mh) { mh = (const struct mach_header_64*)_dyld_get_image_header(0); g_app_slide = (uint64_t)_dyld_get_image_vmaddr_slide(0); }
    const uint8_t* p = (const uint8_t*)(mh + 1);   // (our own process's loaded header: a known address)
    for (uint32_t k = 0; mh && k < mh->ncmds; k++) { const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_UUID) { const uint8_t* u = ((const struct uuid_command*)lc)->uuid;
            snprintf(g_app_uuid, sizeof g_app_uuid, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]); break; }
        p += lc->cmdsize; }
    const char* force = getenv("NHOOK_APP_BUILD"); const char* src = "built in";
    for (size_t i = 0; i < sizeof g_app_builds / sizeof g_app_builds[0]; i++)
        if (force ? !strcmp(force, g_app_builds[i].version) : !strcmp(g_app_uuid, g_app_builds[i].uuid)) { g_app = g_app_builds[i].a; g_app_ver = g_app_builds[i].version; g_app_known = 1; break; }
    if (!g_app_known) {   // a row re-tools/machoport.py wrote after this dylib was built (a release bundle on a newer app)
        static char ver[32]; char path[1024], line[2048]; const char* home = getenv("HOME");
        snprintf(path, sizeof path, "%s/Library/Application Support/ReclaimerMac/native/app-builds.txt", home ? home : ""); FILE* f = fopen(path, "r");
        while (f && fgets(line, sizeof line, f)) { char v[32], u[40]; int k = 0; if (line[0] == '#' || sscanf(line, "%31s %39s %n", v, u, &k) < 2) continue;
            if (force ? strcmp(force, v) : strcmp(u, g_app_uuid)) continue;
            AppAddrs a; memset(&a, 0, sizeof a); char* t = line + k; char* sv = 0;
            for (char* tok = strtok_r(t, " \n", &sv); tok; tok = strtok_r(0, " \n", &sv)) { char* eq = strchr(tok, '='); if (!eq) continue; *eq = 0; uint64_t x = strtoull(eq + 1, 0, 16);
                #define APP_SET(fl) if (!strcmp(tok, #fl)) a.fl = x;
                APP_SET(image) APP_SET(codetab) APP_SET(opus_decode) APP_SET(mixer_call) APP_SET(mixer_id) APP_SET(mixer_id_off) APP_SET(frame_fn)
                APP_SET(chat_gate_a) APP_SET(chat_gate_b) APP_SET(chat_log) APP_SET(muted) APP_SET(set_muted)
                #undef APP_SET
            }
            g_app = a; snprintf(ver, sizeof ver, "%s", v); g_app_ver = ver; g_app_known = 1; src = "app-builds.txt"; break; }
        if (f) fclose(f); }
    int h = hk_named("app build"), miss = 0; char m[160] = ""; size_t ml = 0;
    #define APP_MISS(f) if (g_app_known && !g_app.f) { miss++; ml += (size_t)snprintf(m + ml, ml < sizeof m ? sizeof m - ml : 0, " %s", #f); }
    APP_MISS(image) APP_MISS(codetab) APP_MISS(opus_decode) APP_MISS(mixer_call) APP_MISS(mixer_id) APP_MISS(mixer_id_off) APP_MISS(frame_fn)
    APP_MISS(chat_gate_a) APP_MISS(chat_gate_b) APP_MISS(chat_log) APP_MISS(muted) APP_MISS(set_muted)
    #undef APP_MISS
    if (!g_app_known) hk_state(h, HK_FAIL, 0, "this app (UUID %s) is not in napp_builds.h: python3 re-tools/machoport.py port <0.10.0 binary> <this binary> --apply, then rebuild (shim/native.sh does it itself)", g_app_uuid);
    else if (miss) hk_state(h, HK_FAIL, 0, "%s (UUID %s%s): %d point(s) not found in this build:%s", g_app_ver, g_app_uuid, force ? ", forced by NHOOK_APP_BUILD" : "", miss, m);
    else hk_state(h, HK_OK, 0, "%s (UUID %s%s), every point found", g_app_ver, g_app_uuid, force ? ", forced by NHOOK_APP_BUILD" : "");
    nlog("[nhook] app build %s (UUID %s, slide 0x%llx, %s)%s", g_app_ver, g_app_uuid, (unsigned long long)g_app_slide, g_app_known ? src : "-", g_app_known ? "" : ": NOT in napp_builds.h -- the hooks stay off (re-tools/machoport.py)");
}

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

static uint8_t* near_page(uint64_t code, size_t size) {   // a fresh RW range within +-120 MB of the lifted code
    // The free gaps between the mapped regions in that window (region boundaries only -- mach_vm_region, nothing read),
    // nearest first; each gap's end nearest the code. The old 1 MB steps out from the code region's edges found nothing
    // in some launches: the gaps there are 16-352 KB and scattered (2026-11-07, "no stub page near ...").
    const uint64_t R = 0x7800000; uint64_t lo = code > R + 0x100000000ull ? code - R : 0x100000000ull, hi = code + R;
    static uint64_t cand[4096]; int nc = 0; mach_vm_address_t a = lo; uint64_t prev = lo;   // (hooks are installed from one thread)
    for (int guard = 0; guard < 100000 && prev < hi; guard++) {
        mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t bc = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on;
        kern_return_t kr = mach_vm_region(mach_task_self(), &a, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &bc, &on);
        uint64_t gs = prev, ge = kr == KERN_SUCCESS ? (a < hi ? a : hi) : hi;   // the gap [gs, ge) before this region
        if (ge > gs && ge - gs >= size && nc < 4096) cand[nc++] = ge <= code ? ge - size : gs;   // (the end nearest the code)
        if (kr != KERN_SUCCESS) break;
        prev = a + rs; a = prev;
    }
    for (int i = 1; i < nc; i++) { uint64_t x = cand[i]; int64_t dx = (int64_t)(x - code); if (dx < 0) dx = -dx; int j = i - 1;
        while (j >= 0) { int64_t dj = (int64_t)(cand[j] - code); if (dj < 0) dj = -dj; if (dj <= dx) break; cand[j + 1] = cand[j]; j--; } cand[j + 1] = x; }
    for (int i = 0; i < nc; i++) {
        int64_t d = (int64_t)(cand[i] - code); if (d >= (int64_t)R || d <= -(int64_t)R || d + (int64_t)size >= (int64_t)R) continue;
        mach_vm_address_t t = cand[i]; if (mach_vm_allocate(mach_task_self(), &t, size, VM_FLAGS_FIXED) == KERN_SUCCESS) return (uint8_t*)t;
    }
    nlog("[nhook] near_page: %d free gaps of %zu bytes within 120 MB of 0x%llx, none could be allocated (nearest 0x%llx)", nc, size, (unsigned long long)code, nc ? (unsigned long long)cand[0] : 0ull);
    return 0;
}

// ---- stub slots (2026-11-08): many stubs to a page near the lifted code. Each entry / body hook used to take its own 16 KB
// page within +-120 MB of the code, and the free gaps there are few: in a 0.10.1 test three hooks took them all and the
// fourth (the host tools' game tick, which a dedicated server needs) found "0 free gaps" -- the hooks check showed it FAILED.
// Now a near page is mapped twice: RX where the hooks branch to, RW anywhere (mach_vm_remap, shared) where stubs are written,
// so a page that holds live stubs is never made non-executable and one gap holds 16 KB of stubs (each is ~0.2-0.4 KB).
// NHOOK_STUB_PAGE_EACH=1 = the old one page per stub.
typedef struct { uint64_t rx; uint8_t* rw; size_t used; } StubPage;
static StubPage g_stubp[32]; static int g_nstubp; static pthread_mutex_t g_stub_mx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stub_alloc(uint64_t code, size_t len, uint8_t** rw) {   // the RX address of a free slot near code; *rw = its writable alias
    static int each = -1, far = -1; if (each < 0) each = getenv("NHOOK_STUB_PAGE_EACH") != 0; if (far < 0) far = getenv("NHOOK_FAR_TEST") != 0;
    if (far) return 0;   // NHOOK_FAR_TEST=1: as if no gap were in range (tests the far hooks)
    if (each) { uint8_t* p = near_page(code, 0x4000); *rw = p; return (uint64_t)(uintptr_t)p; }   // (the caller makes it RX)
    len = (len + 63) & ~(size_t)63; const int64_t R = 0x7800000; uint64_t a = 0;
    pthread_mutex_lock(&g_stub_mx);
    for (int i = 0; i < g_nstubp && !a; i++) { StubPage* p = &g_stubp[i]; int64_t d = (int64_t)(p->rx - code);
        if (d > -R + 0x4000 && d < R - 0x4000 && p->used + len <= 0x4000) { a = p->rx + p->used; *rw = p->rw + p->used; p->used += len; } }
    if (!a && g_nstubp < 32) { uint8_t* np = near_page(code, 0x4000); mach_vm_address_t rwa = 0; vm_prot_t cur = 0, mx = 0;
        if (np && mach_vm_remap(mach_task_self(), &rwa, 0x4000, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)(uintptr_t)np, FALSE, &cur, &mx, VM_INHERIT_NONE) == KERN_SUCCESS
               && mach_vm_protect(mach_task_self(), (mach_vm_address_t)(uintptr_t)np, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS) {
            g_stubp[g_nstubp++] = (StubPage){(uint64_t)(uintptr_t)np, (uint8_t*)(uintptr_t)rwa, len}; a = (uint64_t)(uintptr_t)np; *rw = (uint8_t*)(uintptr_t)rwa; }
        else if (np) nlog("[nhook] stub page near 0x%llx: no writable alias (remap / protect refused)", (unsigned long long)code); }
    pthread_mutex_unlock(&g_stub_mx); return a; }
static void stub_seal(uint64_t rx, uint8_t* rw, size_t len) {   // after writing a stub: make it runnable
    static int each = -1; if (each < 0) each = getenv("NHOOK_STUB_PAGE_EACH") != 0;
    if (each) mach_vm_protect(mach_task_self(), (mach_vm_address_t)rx, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    (void)rw; __atomic_thread_fence(__ATOMIC_SEQ_CST); sys_icache_invalidate((void*)(uintptr_t)rx, len); }

// NHOOK_PKG_PROBE=1 (2026-11-07, backlog N9): halo3's network message table (halo3+0x46b6e98 -> 46 x 0x40 {valid, name,
// flags, size, max size, encode +0x20, decode +0x28}; the loader's pktlog.c) in the native app: each message's encode /
// decode function and how the app runs it (a lifted wrapper we could hook, the app's own code, or not lifted), and the five
// message senders / receivers the loader hooked -- whether a "game layer" of message names can be done natively.
enum { PKC_N = 16 }; static volatile unsigned long g_pkc[PKC_N];
#define PKC(i) static void pkc##i(uint64_t* c) { (void)c; g_pkc[i]++; }
PKC(0) PKC(1) PKC(2) PKC(3) PKC(4) PKC(5) PKC(6) PKC(7) PKC(8) PKC(9) PKC(10) PKC(11) PKC(12) PKC(13) PKC(14) PKC(15)
static int hook_install(const char* name, uint32_t rva, nh_handler h);
static const char* rva_kind(uint32_t rva) {
    uint64_t e = host_of(rva); uint32_t w[4]; if (!e || rd(e, w, 16)) return "not lifted";
    if ((w[0] & 0xfc000000u) == 0x14000000u) return "the app's own code";
    return w[0] == 0xa9bf7bfd && w[1] == 0x910003fd ? "lifted" : "other"; }
static void* pkg_probe(void* a) {
    (void)a; for (int tries = 0; tries < 80; tries++) { sleep(3); uint64_t tb = 0; if (!g_img || rd(g_img + 0x46b6e98, &tb, 8) || !tb) continue;
        uint8_t e[0x40]; char nm[32]; uint64_t np = 0; if (rd(tb, e, 0x40) || e[0] != 1) continue; memcpy(&np, e + 8, 8); memset(nm, 0, sizeof nm); if (!np || rd(np, nm, 5) || memcmp(nm, "ping", 5)) continue;
        nlog("[nhook] [pkg] halo3's message table at 0x%llx (guest image 0x%llx)", (unsigned long long)tb, (unsigned long long)g_img);
        for (int i = 0; i < 46; i++) { if (rd(tb + (uint64_t)i * 0x40, e, 0x40) || e[0] != 1) continue; memcpy(&np, e + 8, 8); memset(nm, 0, sizeof nm); if (np) rd(np, nm, 31);
            uint64_t en = 0, de = 0; memcpy(&en, e + 0x20, 8); memcpy(&de, e + 0x28, 8);
            uint32_t er = en > g_img ? (uint32_t)(en - g_img) : 0, dr = de > g_img ? (uint32_t)(de - g_img) : 0;
            nlog("[nhook] [pkg] %2d %-28s encode halo3+0x%x (%s), decode halo3+0x%x (%s)", i, nm, er, er ? rva_kind(er) : "-", dr, dr ? rva_kind(dr) : "-"); }
        static const uint32_t disp[5] = {0x249dc, 0x24900, 0x5f0b0, 0x5f2f0, 0x5efa8};
        for (int i = 0; i < 5; i++) nlog("[nhook] [pkg] dispatcher halo3+0x%x: %s", disp[i], rva_kind(disp[i]));
        if (atoi(getenv("NHOOK_PKG_PROBE")) >= 2) {   // =2: counting hooks on the dispatchers and the busiest messages, counts every 10 s
            static const char* const nm[PKC_N] = {"gateway send", "gateway receive", "channel send", "channel receive", "channel reply", "ping encode", "ping decode",
                "membership-update encode", "membership-update decode", "synchronous-update encode", "synchronous-update decode", "synchronous-actions encode",
                "synchronous-actions decode", "synchronous-gamestate decode", "game-results decode", "player-properties decode"};
            static const uint32_t rv[PKC_N] = {0x249dc, 0x24900, 0x5f0b0, 0x5f2f0, 0x5efa8, 0x73ab8, 0x73b80, 0x75e78, 0x76f50, 0x78e90, 0x78f78, 0x7921c, 0x79328, 0x79650, 0x7999c, 0x77b98};
            static nh_handler const hh[PKC_N] = {pkc0, pkc1, pkc2, pkc3, pkc4, pkc5, pkc6, pkc7, pkc8, pkc9, pkc10, pkc11, pkc12, pkc13, pkc14, pkc15};
            for (int i = 0; i < PKC_N; i++) nlog("[nhook] [pkg] counting %s (halo3+0x%x): %s", nm[i], rv[i], hook_install(nm[i], rv[i], hh[i]) ? "NOT hooked" : "hooked");
            for (int r = 0; r < 30; r++) { sleep(10); char b[600]; int k = 0; for (int i = 0; i < PKC_N; i++) k += snprintf(b + k, sizeof b - (size_t)k, " %s %lu,", nm[i], g_pkc[i]); nlog("[nhook] [pkg] calls:%s", b); } }
        return 0; }
    nlog("[nhook] [pkg] halo3's message table never appeared"); return 0; }
// Call a lifted halo3 function from a handler that holds the engine thread's guest context (2026-11-07, the host tools:
// nhost.c). A lifted wrapper (e.g. halo3+0x259410's) takes x0 = the context, reads rcx / rdx / r8 / r9 at +0x08 / +0x10 /
// +0x40 / +0x48 and the guest stack at +0x20, runs, stores rax at +0x00 and pops the return address (rsp += 8). So: save
// the context's registers (the first 0x280 bytes: the general registers, then the vector ones -- everything below the event
// word at +0x290), set the arguments, a fresh guest stack frame 1 KB below the current one (an entry handler's guest stack
// below rsp is free) with a return-address slot and arguments 5.. at +0x28, call, read rax, restore. Pointer arguments are
// plain host addresses (guest memory is this process's memory).
static uint64_t nh_call(uint64_t* ctx, uint32_t rva, int n, const uint64_t* a) {
    uint64_t e = host_of(rva); if (!e || !ctx) return ~0ull;
    static int lg = -1; if (lg < 0) lg = getenv("NHOOK_HOST_LOG") != 0;
    if (lg) nlog("[nhook] [call] halo3+0x%x (host 0x%llx): %d args %llx %llx %llx %llx %llx; ctx rsp 0x%llx tls 0x%llx", rva, (unsigned long long)e, n,
                 (unsigned long long)(n > 0 ? a[0] : 0), (unsigned long long)(n > 1 ? a[1] : 0), (unsigned long long)(n > 2 ? a[2] : 0), (unsigned long long)(n > 3 ? a[3] : 0), (unsigned long long)(n > 4 ? a[4] : 0),
                 (unsigned long long)ctx[4], (unsigned long long)ctx[0x2d8 / 8]);
    static __thread uint64_t save[0x280 / 8]; memcpy(save, ctx, sizeof save);
    uint64_t rsp = ((ctx[4] - 0x400) & ~0xfull) - 8;                              // (as after a call: rsp % 16 == 8)
    *(volatile uint64_t*)(uintptr_t)rsp = 0;                                     // the return-address slot
    for (int i = 4; i < n; i++) *(volatile uint64_t*)(uintptr_t)(rsp + 0x28 + 8 * (uint64_t)(i - 4)) = a[i];
    if (n > 0) ctx[1] = a[0]; if (n > 1) ctx[2] = a[1]; if (n > 2) ctx[8] = a[2]; if (n > 3) ctx[9] = a[3];
    ctx[4] = rsp;
    ((void (*)(uint64_t*))(uintptr_t)e)(ctx);
    uint64_t r = ctx[0]; if (lg) nlog("[nhook] [call] halo3+0x%x returned 0x%llx", rva, (unsigned long long)r); memcpy(ctx, save, sizeof save); return r;
}
extern char nh_tpls_start[], nh_tpls_displaced[], nh_tpls_handler[], nh_tpls_return[], nh_tpls_end[];
static int hook_install_x0(const char* name, uint32_t rva, void* h, int skip, uint64_t* stub_out);
static int jit_far_hook(const char* name, uint64_t e, void* h, int skip, uint64_t* stub_out);   // (below: no stub page in branch range)
static int insn_pcrel(uint32_t w); static int nh_world_stop(thread_act_array_t* th, mach_msg_type_number_t* n, uint64_t lo, uint64_t hi, int* busy);
static void nh_world_go(thread_act_array_t th, mach_msg_type_number_t n);
// the hooks check around each installer: the item by the hook's name, a counting trampoline in front of the handler, the
// outcome (ok + where, or FAILED + the installer's own last log line)
static int hook_install_x(const char* name, uint32_t rva, void* h, int skip, uint64_t* stub_out) {
    int id = hk_named(name); int r = hook_install_x0(name, rva, hk_wrap(id, h), skip, stub_out);
    if (r) hk_state(id, HK_FAIL, 0, "halo3+0x%x: %s", rva, hk_why()); else { g_hk[id].kind = "entry hook"; hk_state(id, HK_OK, host_of(rva), "entry hook on halo3+0x%x", rva); } return r; }
static int hook_install_x0(const char* name, uint32_t rva, void* h, int skip, uint64_t* stub_out) {   // skip: stub.S _nh_tpls (the handler may do the function's work)
    char* T0 = skip ? nh_tpls_start : nh_tpl_start; char* TE = skip ? nh_tpls_end : nh_tpl_end; char* TD = skip ? nh_tpls_displaced : nh_tpl_displaced;
    char* TH = skip ? nh_tpls_handler : nh_tpl_handler; char* TR = skip ? nh_tpls_return : nh_tpl_return;
    uint64_t e = host_of(rva); uint32_t w[4];
    if (!e || rd(e, w, 16)) { nlog("[nhook] hook %s: halo3+0x%x not lifted", name, rva); return -1; }
    // An entry that is one `b` (object_delete, halo3+0x342ed8: the app replaced the function with its own code, the lifted
    // entry jumps there): that branch becomes `b stub`, and the stub runs the handler, executes nothing in its place and
    // continues at the branch's target -- whatever is there (the guest registers are untouched either way).
    uint32_t displaced = w[0]; uint64_t ret = e + 4;
    if ((w[0] & 0xfc000000u) == 0x14000000u) { int64_t off = (int64_t)((int32_t)(w[0] << 6) >> 6) * 4;
        ret = e + (uint64_t)off; displaced = 0xd503201fu;   // nop
        nlog("[nhook] hook %s: halo3+0x%x is a branch to 0x%llx: hooking the branch", name, rva, (unsigned long long)ret); }
    else if (w[0] != 0xa9bf7bfd || w[1] != 0x910003fd || w[2] != 0xa9bf53f3 || (w[3] & 0xffffffe0) != 0xb9429000) {   // ldr w<n>,[x0,#0x290]
        nlog("[nhook] hook %s: halo3+0x%x entry %08x %08x %08x %08x is not a lifted wrapper: refused", name, rva, w[0], w[1], w[2], w[3]); return -1; }
    size_t tl = TE - T0; uint8_t* sw = 0;
    uint64_t sx = stub_alloc(e, tl, &sw);            // a slot near the code (stub_alloc: RX there, written through its RW alias)
    if (!sx) { nlog("[nhook] hook %s: no stub page near 0x%llx: a far hook instead", name, (unsigned long long)e);
        return (w[0] & 0xfc000000u) == 0x14000000u ? -1 : jit_far_hook(name, e, h, skip, stub_out); }
    uint8_t* st = (uint8_t*)(uintptr_t)sx; int64_t d = (int64_t)(sx - e);
    memcpy(sw, T0, tl);
    memcpy(sw + (TD - T0), &displaced, 4);            // the displaced stp x29,x30,[sp,#-16]! (or a nop for a branch)
    uint64_t hv = (uint64_t)h, rv = ret;
    memcpy(sw + (TH - T0), &hv, 8); memcpy(sw + (TR - T0), &rv, 8);
    stub_seal(sx, sw, tl);
    uint32_t b = 0x14000000u | ((uint32_t)(d >> 2) & 0x03ffffffu);
    pthread_jit_write_protect_np(0);                // the code is MAP_JIT: writable only for a thread that turns it so
    __atomic_store_n((uint32_t*)e, b, __ATOMIC_RELEASE);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate((void*)e, 4);
    nlog("[nhook] hook %s: halo3+0x%x (host 0x%llx) -> stub 0x%llx", name, rva, (unsigned long long)e, (unsigned long long)st);
    if (stub_out) *stub_out = (uint64_t)st + (uint64_t)(TD - T0);   // (the stub's tail: runs the original)
    return 0;
}
static int hook_install(const char* name, uint32_t rva, nh_handler h) { return hook_install_x(name, rva, (void*)h, 0, 0); }

// ---- hooks on a lifted function's BODY (2026-11-07) ------------------------------------------------------------------
// Entry hooks see only calls through the code table; lifted code calls lifted code DIRECTLY, at the body its wrapper `bl`s
// (wrapper: stp/mov/stp, ldr w,[x0,#0x290], ..., x2 = context, x3 = guest rsp, x4..x7 = rcx, rdx, r8, r9, `bl body`,
// str x2 -> guest rax). body_hook_after puts one `b <stub>` over the body's first instruction (it must be `stp x29, x30,
// [sp, #-16]!`; one atomic word on the MAP_JIT code, like hook_install_x), so every caller is seen; stub.S _nh_ba runs the
// original first, then h(a) (a[0..5] = x2..x7, a[6..9] = stack arguments 5-8, a[10] = the result).
extern char nh_ba_start[], nh_ba_displaced[], nh_ba_handler[], nh_ba_return[], nh_ba_end[];
static uint64_t body_of(uint32_t rva) {   // the body a lifted wrapper calls (0 = not that shape)
    uint64_t e = host_of(rva); uint32_t w[24]; if (!e || rd(e, w, sizeof w) || w[0] != 0xa9bf7bfd || w[1] != 0x910003fd) return 0;
    for (int i = 4; i < 24; i++) if ((w[i] & 0xfc000000u) == 0x94000000u) {
        uint64_t b = e + 4u * (unsigned)i + (uint64_t)((int64_t)((int32_t)(w[i] << 6) >> 6) * 4); uint32_t b0 = 0;
        return rd(b, &b0, 4) || b0 != 0xa9bf7bfd ? 0 : b; }
    return 0; }
static int body_hook_after0(const char* name, uint32_t rva, void (*h)(uint64_t*));
static int body_hook_after(const char* name, uint32_t rva, void (*h)(uint64_t*)) {
    int id = hk_named(name); int r = body_hook_after0(name, rva, (void (*)(uint64_t*))hk_wrap(id, (void*)h));
    if (r) hk_state(id, HK_FAIL, 0, "halo3+0x%x: %s", rva, hk_why()); else { g_hk[id].kind = "body hook"; hk_state(id, HK_OK, body_of(rva), "body hook on halo3+0x%x", rva); } return r; }
static int body_hook_after0(const char* name, uint32_t rva, void (*h)(uint64_t*)) {
    uint64_t b = body_of(rva); if (!b) { nlog("[nhook] body hook %s: halo3+0x%x has no lifted body of the known shape: refused", name, rva); return -1; }
    size_t tl = (size_t)(nh_ba_end - nh_ba_start); uint8_t* sw = 0; uint64_t sx = stub_alloc(b, tl, &sw);
    if (!sx) {   // no stub page in branch range: a far body hook -- the body's first four instructions move into the stub (its 4 slots)
        uint32_t w4[4]; if (getenv("NHOOK_NO_FAR_HOOKS") || rd(b, w4, 16)) { nlog("[nhook] body hook %s: no stub page near 0x%llx", name, (unsigned long long)b); return -1; }
        for (int i = 0; i < 4; i++) if (insn_pcrel(w4[i])) { nlog("[nhook] body hook %s: no stub page near 0x%llx and instruction %d is PC-relative", name, (unsigned long long)b, i); return -1; }
        mach_vm_address_t st = 0; if (mach_vm_allocate(mach_task_self(), &st, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return -1;
        memcpy((void*)st, nh_ba_start, tl); memcpy((uint8_t*)st + (nh_ba_displaced - nh_ba_start), w4, 16);
        uint64_t hv = (uint64_t)(uintptr_t)h, rv = b + 16; memcpy((uint8_t*)st + (nh_ba_handler - nh_ba_start), &hv, 8); memcpy((uint8_t*)st + (nh_ba_return - nh_ba_start), &rv, 8);
        mach_vm_protect(mach_task_self(), st, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE); sys_icache_invalidate((void*)st, tl);
        uint32_t patch[4] = {0x58000050u, 0xd61f0200u, (uint32_t)st, (uint32_t)((uint64_t)st >> 32)}; int ok = 0;
        for (int tries = 0;; tries++) { thread_act_array_t ths = 0; mach_msg_type_number_t n = 0; int busy = 0;
            if (nh_world_stop(&ths, &n, b + 1, b + 16, &busy)) break;
            if (busy && tries < 50) { nh_world_go(ths, n); usleep(1000); continue; }
            if (!busy) { pthread_jit_write_protect_np(0); memcpy((void*)(uintptr_t)(b + 8), &patch[2], 8); __atomic_thread_fence(__ATOMIC_SEQ_CST);
                         memcpy((void*)(uintptr_t)b, patch, 8); pthread_jit_write_protect_np(1); sys_icache_invalidate((void*)(uintptr_t)b, 16); ok = 1; }
            nh_world_go(ths, n); break; }
        if (!ok) return -1;
        nlog("[nhook] far body hook %s: halo3+0x%x body 0x%llx -> stub 0x%llx", name, rva, (unsigned long long)b, (unsigned long long)st); return 0; }
    uint8_t* st = (uint8_t*)(uintptr_t)sx;
    int64_t d = (int64_t)(sx - b); if (d >= (1ll << 27) || d < -(1ll << 27)) { nlog("[nhook] body hook %s: stub out of branch range", name); return -1; }
    memcpy(sw, nh_ba_start, tl); uint32_t disp = 0xa9bf7bfd; memcpy(sw + (nh_ba_displaced - nh_ba_start), &disp, 4);
    uint64_t hv = (uint64_t)(uintptr_t)h, rv = b + 4; memcpy(sw + (nh_ba_handler - nh_ba_start), &hv, 8); memcpy(sw + (nh_ba_return - nh_ba_start), &rv, 8);
    stub_seal(sx, sw, tl);
    uint32_t br = 0x14000000u | ((uint32_t)(d >> 2) & 0x03ffffffu);
    pthread_jit_write_protect_np(0); __atomic_store_n((uint32_t*)(uintptr_t)b, br, __ATOMIC_RELEASE); pthread_jit_write_protect_np(1);
    sys_icache_invalidate((void*)(uintptr_t)b, 4);
    nlog("[nhook] body hook %s: halo3+0x%x body 0x%llx -> stub 0x%llx", name, rva, (unsigned long long)b, (unsigned long long)(uintptr_t)st);
    return 0; }

// ---- hooks on the app's OWN code (its signed __TEXT: functions the app replaced, e.g. object_delete) --------------------
// The app is signed ad hoc with no hardened runtime (codesign flags 0x2): its text pages are made writable as private copies
// (VM_PROT_COPY), the function's first four instructions become `ldr x16, #8; br x16; .quad stub` and go to the stub
// (stub.S _nh_tp4: handler(x0), then those four instructions, then back). Refused when one of them is PC-relative.
// Installed at startup or later, while the game runs (the camera hook, host tools): the page is not executable while it is
// patched, so every other thread is suspended for that moment (nh_world_stop: a thread running in the page would fault --
// signal 10 at a frame-function address, 2026-11-07), and a thread stopped inside the four patched instructions makes it try
// again (up to 50 times, 1 ms apart). Nothing between stop and resume may take a lock (no log, no malloc). NHOOK_NO_TEXT_HOOKS=1 = none.
extern char nh_tp4_start[], nh_tp4_displaced[], nh_tp4_handler[], nh_tp4_return[], nh_tp4_end[];
static int insn_pcrel(uint32_t w) {
    return (w & 0x1f000000u) == 0x10000000u            // adr / adrp
        || (w & 0x7c000000u) == 0x14000000u            // b / bl
        || (w & 0x7e000000u) == 0x34000000u            // cbz / cbnz
        || (w & 0x7e000000u) == 0x36000000u            // tbz / tbnz
        || (w & 0xff000010u) == 0x54000000u            // b.cond
        || (w & 0x3b000000u) == 0x18000000u;           // ldr (literal)
}
// Suspend every thread but this one (0 = done); *busy = some thread's pc lies in [lo, hi) (then they are resumed again).
static int nh_world_stop(thread_act_array_t* th, mach_msg_type_number_t* n, uint64_t lo, uint64_t hi, int* busy) {
    *busy = 0; if (task_threads(mach_task_self(), th, n) != KERN_SUCCESS) return -1;
    thread_t me = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < *n; i++) if ((*th)[i] != me) thread_suspend((*th)[i]);
    for (mach_msg_type_number_t i = 0; i < *n && !*busy; i++) { if ((*th)[i] == me) continue;
        arm_thread_state64_t s; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state((*th)[i], ARM_THREAD_STATE64, (thread_state_t)&s, &c) != KERN_SUCCESS) continue;
        uint64_t pc = (uint64_t)arm_thread_state64_get_pc(s); if (pc >= lo && pc < hi) *busy = 1; }
    mach_port_deallocate(mach_task_self(), me); return 0;
}
static void nh_world_go(thread_act_array_t th, mach_msg_type_number_t n) {
    thread_t me = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < n; i++) { if (th[i] != me) thread_resume(th[i]); mach_port_deallocate(mach_task_self(), th[i]); }
    mach_port_deallocate(mach_task_self(), me);
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)th, (mach_vm_size_t)n * sizeof(thread_act_t));
}
extern char nh_tpf_start[], nh_tpf_want[], nh_tpf_displaced[], nh_tpf_handler[], nh_tpf_return[], nh_tpf_end[];
extern char nh_tps_start[], nh_tps_displaced[], nh_tps_handler[], nh_tps_return[], nh_tps_end[];
static int text_hook_x0(const char* name, uint64_t f, void* h, int filt, uint32_t want, int skip, uint64_t* stub_out);
extern char nh_opus_shim[];
static int text_hook_x(const char* name, uint64_t f, void* h, int filt, uint32_t want, int skip, uint64_t* stub_out) {
    int id = hk_named(name); if (getenv("NHOOK_NO_TEXT_HOOKS")) { hk_state(id, HK_OFF, f, "NHOOK_NO_TEXT_HOOKS"); return -1; }
    int r = text_hook_x0(name, f, h == (void*)nh_opus_shim ? h : hk_wrap(id, h), filt, want, skip, stub_out);   // (the Opus shim reads the stub's stack: no trampoline; nh_on_opus counts)
    if (r) hk_state(id, HK_FAIL, f, "%s", hk_why()); else { g_hk[id].kind = "text hook"; hk_state(id, HK_OK, f, "text hook at 0x%llx (the app's own code)", (unsigned long long)f); } return r; }
static int text_hook_x0(const char* name, uint64_t f, void* h, int filt, uint32_t want, int skip, uint64_t* stub_out) {   // skip: stub.S _nh_tps   // filt: _nh_tpf, the handler only for parameter id `want`
    uint32_t w[4]; if (rd(f, w, 16)) { nlog("[nhook] text hook %s: 0x%llx unreadable", name, (unsigned long long)f); return -1; }
    for (int i = 0; i < 4; i++) if (insn_pcrel(w[i])) { nlog("[nhook] text hook %s: instruction %d (%08x) is PC-relative: refused", name, i, w[i]); return -1; }
    char* ts = filt ? nh_tpf_start : skip ? nh_tps_start : nh_tp4_start; char* te = filt ? nh_tpf_end : skip ? nh_tps_end : nh_tp4_end;
    char* td = filt ? nh_tpf_displaced : skip ? nh_tps_displaced : nh_tp4_displaced, * th = filt ? nh_tpf_handler : skip ? nh_tps_handler : nh_tp4_handler, * tr = filt ? nh_tpf_return : skip ? nh_tps_return : nh_tp4_return;
    size_t tl = te - ts; mach_vm_address_t st = 0;
    if (mach_vm_allocate(mach_task_self(), &st, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return -1;
    memcpy((void*)st, ts, tl); memcpy((uint8_t*)st + (td - ts), w, 16);
    uint64_t hv = (uint64_t)h, rv = f + 16; memcpy((uint8_t*)st + (th - ts), &hv, 8); memcpy((uint8_t*)st + (tr - ts), &rv, 8);
    if (filt) memcpy((uint8_t*)st + (nh_tpf_want - ts), &want, 4);
    mach_vm_protect(mach_task_self(), st, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE); sys_icache_invalidate((void*)st, tl);
    uint64_t pg = f & ~0x3fffull, end = (f + 16 + 0x3fff) & ~0x3fffull;
    uint32_t patch[4] = {0x58000050u, 0xd61f0200u, (uint32_t)st, (uint32_t)((uint64_t)st >> 32)};   // ldr x16, #8 ; br x16 ; .quad stub
    kern_return_t kr = KERN_FAILURE, kr2 = KERN_FAILURE; int tries = 0, stopped = !getenv("NHOOK_TEXT_NOSTOP");
    for (;; tries++) {
        thread_act_array_t th = 0; mach_msg_type_number_t n = 0; int busy = 0;
        if (stopped && nh_world_stop(&th, &n, f + 1, f + 16, &busy)) stopped = 0;
        if (busy && tries < 50) { nh_world_go(th, n); usleep(1000); continue; }
        kr = mach_vm_protect(mach_task_self(), pg, end - pg, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (kr == KERN_SUCCESS) {
            memcpy((void*)(uintptr_t)(f + 8), &patch[2], 8); __atomic_thread_fence(__ATOMIC_SEQ_CST); memcpy((void*)(uintptr_t)f, patch, 8);
            kr2 = mach_vm_protect(mach_task_self(), pg, end - pg, 0, VM_PROT_READ | VM_PROT_EXECUTE);
            sys_icache_invalidate((void*)(uintptr_t)f, 16); }
        if (stopped) nh_world_go(th, n);
        break; }
    if (kr != KERN_SUCCESS) { nlog("[nhook] text hook %s: the page stays read-only (kr %d)", name, kr); return -1; }
    nlog("[nhook] text hook %s: 0x%llx -> stub 0x%llx (back to executable: kr %d; other threads %s%s)", name, (unsigned long long)f, (unsigned long long)st, kr2,
         stopped ? "stopped" : "NOT stopped", tries ? ", retried" : "");
    if (stub_out) *stub_out = (uint64_t)st + (uint64_t)(td - ts);
    return 0;
}
// A far hook on a lifted wrapper's entry (2026-11-08): some launches have NO free 16 KB gap within branch range of the JIT
// code (a walker's engine_clock hook failed: "no stub page near ..." -- that client then never saw its players), so instead of one
// `b stub` the entry's first four instructions (the wrapper prologue: stp / mov / stp / ldr w,[x0,#0x290] -- nothing PC-relative)
// become `ldr x16, #8 ; br x16 ; .quad stub`, the stub (anywhere) runs them after the handler (stub.S _nh_tp4 / _nh_tps, as a text
// hook) and returns to entry + 16. Written with the other threads stopped (none may be inside those 16 bytes) through
// pthread_jit_write_protect_np (the code is MAP_JIT). NHOOK_NO_FAR_HOOKS=1 = none (the old failure).
static int jit_far_hook(const char* name, uint64_t e, void* h, int skip, uint64_t* stub_out) {
    if (getenv("NHOOK_NO_FAR_HOOKS")) return -1;
    uint32_t w[4]; if (rd(e, w, 16)) return -1;
    for (int i = 0; i < 4; i++) if (insn_pcrel(w[i])) { nlog("[nhook] far hook %s: instruction %d (%08x) is PC-relative: refused", name, i, w[i]); return -1; }
    char* ts = skip ? nh_tps_start : nh_tp4_start; char* te = skip ? nh_tps_end : nh_tp4_end; char* td = skip ? nh_tps_displaced : nh_tp4_displaced;
    char* th = skip ? nh_tps_handler : nh_tp4_handler; char* tr = skip ? nh_tps_return : nh_tp4_return;
    size_t tl = te - ts; mach_vm_address_t st = 0; if (mach_vm_allocate(mach_task_self(), &st, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return -1;
    memcpy((void*)st, ts, tl); memcpy((uint8_t*)st + (td - ts), w, 16);
    uint64_t hv = (uint64_t)h, rv = e + 16; memcpy((uint8_t*)st + (th - ts), &hv, 8); memcpy((uint8_t*)st + (tr - ts), &rv, 8);
    mach_vm_protect(mach_task_self(), st, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE); sys_icache_invalidate((void*)st, tl);
    uint32_t patch[4] = {0x58000050u, 0xd61f0200u, (uint32_t)st, (uint32_t)((uint64_t)st >> 32)}; int tries = 0, ok = 0;
    for (;; tries++) { thread_act_array_t ths = 0; mach_msg_type_number_t n = 0; int busy = 0;
        if (nh_world_stop(&ths, &n, e + 1, e + 16, &busy)) break;
        if (busy && tries < 50) { nh_world_go(ths, n); usleep(1000); continue; }
        if (!busy) { pthread_jit_write_protect_np(0); memcpy((void*)(uintptr_t)(e + 8), &patch[2], 8); __atomic_thread_fence(__ATOMIC_SEQ_CST);
                     memcpy((void*)(uintptr_t)e, patch, 8); pthread_jit_write_protect_np(1); sys_icache_invalidate((void*)(uintptr_t)e, 16); ok = 1; }
        nh_world_go(ths, n); break; }
    if (!ok) { nlog("[nhook] far hook %s: could not patch 0x%llx", name, (unsigned long long)e); return -1; }
    nlog("[nhook] far hook %s: 0x%llx -> stub 0x%llx (16 bytes, %d retries)", name, (unsigned long long)e, (unsigned long long)st, tries);
    if (stub_out) *stub_out = (uint64_t)st + (uint64_t)(td - ts);
    return 0; }
static int text_hook_t(const char* name, uint64_t f, nh_handler h, int filt, uint32_t want) { return text_hook_x(name, f, (void*)h, filt, want, 0, 0); }
static int text_hook(const char* name, uint64_t f, nh_handler h) { return text_hook_t(name, f, h, 0, 0); }
// the app's Opus decode (nstats.c nh_on_opus: forensic voice audio) -- this build's addresses (napp_builds.h), each checked
// first by what it must be: decode's prologue, the mixer's argument moves + a bl that reaches decode, its sender id load
// (sub x8, x26, #off ; ldr x19, [x8] with off = the table's mixer_id_off: 0x278 in 0.10.0, 0x2a0 in 0.10.1 -- the sender
// entry grew). Until 2026-11-08 these were exact 0.10.0 words (incl. the bl's offset and #0x278).
extern char nh_opus_shim[];
static uint64_t g_opus_ret;      // (nstats.c nh_on_opus: other callers are ignored)
static uint64_t g_mixer_id_off;  // (nstats.c nh_on_opus: the sender's peer id at x26 - this)
static int g_hk_opus = -1;
static int bl_reaches(uint64_t at, uint64_t want) { uint32_t w; if (rd(at, &w, 4) || (w & 0xfc000000u) != 0x94000000u) return 0;
    return at + (uint64_t)((int64_t)((int32_t)(w << 6) >> 6) * 4) == want; }
static void voice_hook_install(void) {
    int h = g_hk_opus = hk_named("opus decode (voice)"), mx = hk_named("voice mixer");
    uint64_t dec = APP(opus_decode), call = APP(mixer_call), idl = APP(mixer_id), off = g_app.mixer_id_off;
    if (!dec || !call || !idl || !off) { hk_state(h, HK_FAIL, 0, "this build's table has no opus_decode / mixer_call / mixer_id"); hk_state(mx, HK_FAIL, 0, "not in this build's table");
        nlog("[nhook] voice packets: not hooked (this app build's table has no voice points)"); return; }
    static const uint32_t wd[4] = {0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}, wc[3] = {0xaa1903e1, 0xaa1803e2, 0x52807803};
    uint32_t d[4], c[3], i[2];
    if (rd(dec, d, 16) || memcmp(d, wd, 16)) { hk_state(h, HK_FAIL, dec, "decode at 0x%llx does not start stp d9,d8 / x28,x27 / x26,x25 / x24,x23", (unsigned long long)dec);
        nlog("[nhook] voice packets: not hooked (0x%llx is not the Opus decode)", (unsigned long long)dec); return; }
    if (rd(call, c, 12) || memcmp(c, wc, 12) || !bl_reaches(call + 12, dec)) { hk_state(mx, HK_FAIL, call, "0x%llx is not mov x1, x25 ; mov x2, x24 ; mov w3, #0x3c0 ; bl decode", (unsigned long long)call);
        hk_state(h, HK_FAIL, dec, "its caller (the voice mixer) did not check out"); nlog("[nhook] voice packets: not hooked (the mixer's decode call at 0x%llx is not the expected code)", (unsigned long long)call); return; }
    if (rd(idl, i, 8) || (i[0] & 0xffc003ffu) != 0xd1000348u || ((i[0] >> 10) & 0xfffu) != off || i[1] != 0xf9400113u) {
        hk_state(mx, HK_FAIL, idl, "0x%llx is not sub x8, x26, #0x%llx ; ldr x19, [x8]", (unsigned long long)idl, (unsigned long long)off);
        hk_state(h, HK_FAIL, dec, "the mixer's sender id load did not check out"); nlog("[nhook] voice packets: not hooked (the mixer's id load at 0x%llx is not the expected code)", (unsigned long long)idl); return; }
    g_mixer_id_off = off; g_opus_ret = call + 16;
    hk_state(mx, HK_OK, call, "decode call at 0x%llx, sender id at x26 - 0x%llx, channel byte at x26 - 7", (unsigned long long)call, (unsigned long long)off);
    text_hook("opus decode (voice)", dec, (nh_handler)(void*)nh_opus_shim);
}
// The target of an entry that is one `b` (the code table's entry for a function the app replaced)
static uint64_t branch_target_of(uint32_t rva) {
    uint64_t e = host_of(rva); uint32_t w0; if (!e || rd(e, &w0, 4) || (w0 & 0xfc000000u) != 0x14000000u) return 0;
    return e + (uint64_t)((int64_t)((int32_t)(w0 << 6) >> 6) * 4);
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
static struct { const char* name; uint32_t rva; } g_cnt_fns[] = {   // (NHOOK_COUNT_RVAS=<rva>,... replaces the list: any 8 functions)
    {"main_loop_body", 0xb2250}, {"sim_tick", 0xef120}, {"local_user_count", 0xfbb94}, {"local_player", 0xfbc04},
    {"display_event", 0x11b0ec}, {"object_delete", 0x342ed8}, {"render_prep", 0x2d1390}, {"objects_update", 0x34067c},
};
#define NCNT (sizeof g_cnt_fns / sizeof g_cnt_fns[0])
static volatile uint64_t g_cnt[NCNT];
#define CNT_HANDLER(i) static void cnt_##i(uint64_t* ctx) { g_cnt[i]++; if (ctx[0x2d8 / 8]) g_main_ctx = (uint64_t)ctx; }
CNT_HANDLER(0) CNT_HANDLER(1) CNT_HANDLER(2) CNT_HANDLER(3) CNT_HANDLER(4) CNT_HANDLER(5) CNT_HANDLER(6) CNT_HANDLER(7)
static nh_handler g_cnt_h[NCNT] = { cnt_0, cnt_1, cnt_2, cnt_3, cnt_4, cnt_5, cnt_6, cnt_7 };
// halo3+0x9d22c also runs on other engine threads, whose TLS blocks hold no players table: once a thread's block is
// seen to hold it (tls_block), only that thread's context is kept (its TLS pointer, ctx+0x2d8). Taking any caller's
// context made ~1 snapshot in 7 empty in a 72-player game (2026-11-07). A thread that stops validating for 2 s (a new
// engine thread) is given up and the next good one learned.
static volatile uint64_t g_good_tlsp;
// ---- the frame's camera: halo3's view constants (shader parameter 0x27, set through halo3+0x2af478 -- the app's own code):
// element 0 = the view-projection rows 0-3 (count 64 bytes), 4 / 5 / 6 forward / left / up, 7 the camera position. Halo 3's
// projection is reverse-Z with no far plane: row 2 = (0, 0, 0, 1/128) -> scene depth = (1/128) / distance along the view.
// Several views set it each frame (the main view, shadow / reflection views): a "world camera" is one whose rows 0, 1 and 3
// vanish at its position (the loader's hb_is_world_camera test); the overlay takes the latest one that sits within 1 unit of
// the observer camera (the player's view). Captured on the render thread; read with a sequence lock.
typedef struct { float rows[16], pos[3]; } NhCam;
static NhCam g_cam_pend; static volatile uint32_t g_cam_have_rows;
static NhCam g_cam; static volatile unsigned g_cam_seq; static volatile double g_cam_t;
static int cam_is_world(const float* m, const float* c) {
    float c2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2]; if (c2 < 0.25f || c2 > 1e8f) return 0;
    static const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; i++) { const float* r = m + rows[i] * 4; float sc = fabsf(r[0]) + fabsf(r[1]) + fabsf(r[2]); if (sc < 1e-4f) return 0;
        float v = r[0] * c[0] + r[1] * c[1] + r[2] * c[2] + r[3]; if (fabsf(v) > 0.02f * sc * (1.0f + sqrtf(c2))) return 0; }
    return 1;
}
static void on_view_param_body(uint64_t* ctx) {      // (only parameter 0x27 gets here: the stub filters)
    uint32_t ecx = (uint32_t)ctx[1], el = ecx & 0xffff, cnt = (uint32_t)ctx[8]; uint64_t data = ctx[2];
    if (data < 0x100000000ull || data >= 0x800000000000ull) return;
    if (el == 0 && cnt >= 64) { memcpy(g_cam_pend.rows, (const void*)(uintptr_t)data, 64); g_cam_have_rows = 1; return; }
    if (el == 7 && cnt >= 12 && g_cam_have_rows) { memcpy(g_cam_pend.pos, (const void*)(uintptr_t)data, 12); g_cam_have_rows = 0;
        if (!cam_is_world(g_cam_pend.rows, g_cam_pend.pos)) return;
        extern int nov_cam_is_player(const float* pos);
        if (!nov_cam_is_player(g_cam_pend.pos)) return;
        g_cam_seq++; __atomic_thread_fence(__ATOMIC_SEQ_CST); g_cam = g_cam_pend; __atomic_thread_fence(__ATOMIC_SEQ_CST); g_cam_seq++;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); g_cam_t = ts.tv_sec + ts.tv_nsec * 1e-9; }
}
static volatile int g_view_wanted;              // the overlay is on (noverlay.c nov_build sets it every frame it runs, clears when off)
static void on_view_param(uint64_t* ctx) { if (g_hc_off[HC_VIEW] || !g_view_wanted) return; uint64_t t0 = hc_now(); on_view_param_body(ctx); hc_add(HC_VIEW, t0); }
static uint64_t g_view_hook_at; static int g_view_hooked;
static void nov_view_hook_want(void) {        // (render thread) install the camera hook once, now that the overlay wants a view
    if (g_view_hooked || !g_view_hook_at) return; g_view_hooked = 1;
    if (!text_hook_t("view constants", g_view_hook_at, on_view_param, 1, 0x27)) nlog("[nhook] camera hook installed (the overlay was turned on)"); }
int nh_camera(float* rows16, float* pos3) {     // the latest player view (1 = fresh: < 0.25 s)
    NhCam c; unsigned s1, s2; int tries = 0;
    do { s1 = g_cam_seq; memcpy(&c, (const void*)&g_cam, sizeof c); s2 = g_cam_seq; } while ((s1 != s2 || (s1 & 1)) && ++tries < 8);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (!s1 || s1 != s2 || now - g_cam_t > 0.25) return 0;
    memcpy(rows16, c.rows, 64); memcpy(pos3, c.pos, 12); return 1;
}

// NHOOK_CBPROBE=1: halo3+0x2af478 (set a shader parameter: ecx bits 16-26 = parameter id, low 16 bits = element, rdx = data,
// r8d = count -- the app's own code) records each parameter id's latest call; dumped to the log every 10 s
typedef struct { volatile uint32_t ecx, count; volatile unsigned long n; float d[32]; } CbSeen;
static CbSeen g_cbs[4096];   // by (parameter id, element): ids 0x37 / 0x43 / 0x27 / 0x15 / 0x2c keep every element (the view constants)
static void on_set_param(uint64_t* ctx) {
    uint32_t ecx = (uint32_t)ctx[1], id = (ecx >> 16) & 0x7ff, el = ecx & 0xffff; uint64_t data = ctx[2]; uint32_t cnt = (uint32_t)ctx[8];
    int view = id == 0x37 || id == 0x43 || id == 0x27 || id == 0x15 || id == 0x2c;
    CbSeen* c = &g_cbs[view ? (2048 + ((id & 0x3f) << 5) + (el & 31)) % 4096 : id]; c->n++; c->ecx = ecx; c->count = cnt;
    if (data >= 0x100000000ull && data < 0x800000000000ull && (view || (c->n & 1023) == 1)) memcpy((void*)c->d, (const void*)(uintptr_t)data, cnt > 128 ? 128 : cnt);
}
static void* cbprobe_dump(void* a) {
    (void)a; for (int r = 0; r < 6; r++) { sleep(10);
        for (int i = 0; i < 4096; i++) { CbSeen* c = &g_cbs[i]; if (!c->n) continue;
            char b[900]; int k = 0; for (int q = 0; q < 32 && q * 4 < (int)c->count; q++) k += snprintf(b + k, sizeof b - (size_t)k, "%s%.4f", q && !(q & 3) ? " | " : " ", c->d[q]);
            nlog("[nhook] [cb] ecx 0x%08x count %u calls %lu:%s", c->ecx, c->count, c->n, b); } }
    return 0;
}
volatile unsigned long g_texthook_test_n, g_tht_n[8]; uint32_t g_tht_rva[8];
#define THT(i) static void tht_##i(uint64_t* ctx) { (void)ctx; g_tht_n[i]++; g_texthook_test_n++; }
THT(0) THT(1) THT(2) THT(3) THT(4) THT(5) THT(6) THT(7)
static nh_handler g_tht_h[8] = { tht_0, tht_1, tht_2, tht_3, tht_4, tht_5, tht_6, tht_7 };
static void on_main_loop(uint64_t* ctx) {           // the engine thread's context (hook_install "engine_clock")
    uint64_t t = ctx[0x2d8 / 8]; if (!t) return;
    uint64_t good = g_good_tlsp; if (good && t != good) return;
    g_main_ctx = (uint64_t)ctx; g_frames++;
}

static int tls_holds_players(uint64_t blk) {        // the block's +0x10 is halo3's "players" data array
    uint64_t parr = 0; uint8_t hdr[0x30];
    return blk && !rd(blk + 0x10, &parr, 8) && parr && !rd(parr, hdr, sizeof hdr) && *(uint32_t*)(hdr + 0x2c) == 0x64407440 && !memcmp(hdr, "players", 8);
}
static __thread int t_tls_memo; static __thread uint64_t t_tls_val;   // (the snapshot thread: one lookup per run, st_snap_thread)
static uint64_t tls_block_now(void);
static uint64_t tls_block(void) { if (t_tls_memo) { if (t_tls_memo == 1) { t_tls_val = tls_block_now(); t_tls_memo = 2; } return t_tls_val; } return tls_block_now(); }
static uint64_t tls_block_now(void) {                   // halo3's TLS block of the engine main thread (index 0 here)
    static volatile uint64_t last_good; static double bad_since;
    uint64_t c = g_main_ctx, arr = 0, blk = 0; uint32_t idx = 0;
    if (c && !rd(c + 0x2d8, &arr, 8) && arr && !rd(g_img + 0xa39f9c, &idx, 4) && idx <= 64 && !rd(arr + idx * 8, &blk, 8) && tls_holds_players(blk)) {
        if (!g_good_tlsp) g_good_tlsp = arr;
        last_good = blk; bad_since = 0; return blk;
    }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (!bad_since) bad_since = now; else if (now - bad_since > 2.0) { g_good_tlsp = 0; bad_since = 0; last_good = 0; }
    return last_good && tls_holds_players(last_good) ? last_good : 0;
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

#include "nstats.c"   // the stats page (h3maps.c's map files, the players / objects readers, the HTTP server)

// hooks off (an app build we have no addresses for, a wrong row): the stats page and the frame hooks still start, so the
// page's Hooks window (and GET /hooks.json) says what is missing -- until 2026-11-08 there was no page at all then
static void hooks_off_page(void) { nstats_start(); { extern void nr_install(void); nr_install(); } }
static void hooks_start(void) {
    int hi = hk_named("guest image"), ht = hk_named("code table");
    g_slide = g_app_slide;
    if (g_app.image && g_img != APP(image)) { hk_state(hi, HK_FAIL, g_img, "the app logged 0x%llx, this build's table says 0x%llx: the wrong row (hooks off)", (unsigned long long)g_img, (unsigned long long)APP(image));
        hk_state(ht, HK_FAIL, 0, "not read: the guest image is not where this build's table has it"); nlog("[nhook] the engine image 0x%llx is not this build's (0x%llx): hooks off", (unsigned long long)g_img, (unsigned long long)APP(image)); hooks_off_page(); return; }
    hk_state(hi, HK_OK, g_img, "at 0x%llx%s", (unsigned long long)g_img, g_app.image ? " (as this build's table has it)" : "");
    uint64_t tab[2], ta = APP(codetab);
    if (!ta || rd(ta, tab, 16) || !tab[0] || tab[1] < 1000 || tab[1] > 1000000) {
        hk_state(ht, HK_FAIL, ta, "%s", !g_app_known ? "this app build is not in napp_builds.h (re-tools/machoport.py)" : !ta ? "not in this build's table" : "no {ptr, count} at this build's code-table address");
        nlog("[nhook] no code table: hooks off%s", g_app_known ? "" : " (this app build is not in napp_builds.h: re-tools/machoport.py)"); hooks_off_page(); return; }
    g_tab = tab[0]; g_ntab = tab[1]; hk_state(ht, HK_OK, ta, "%llu entries at 0x%llx", (unsigned long long)g_ntab, (unsigned long long)g_tab);
    if (getenv("NHOOK_CENSUS")) census_install();
    else if (getenv("NHOOK_COUNT")) {
        if (getenv("NHOOK_COUNT_RVAS")) { const char* w = getenv("NHOOK_COUNT_RVAS"); static char nm[NCNT][16];
            for (size_t k = 0; k < NCNT; k++) { if (w && *w) { g_cnt_fns[k].rva = (uint32_t)strtoul(w, 0, 16); snprintf(nm[k], 16, "%x", g_cnt_fns[k].rva); g_cnt_fns[k].name = nm[k]; w = strchr(w, ','); if (w) w++; } else g_cnt_fns[k].rva = 0; } }
        for (size_t k = 0; k < NCNT; k++) if (g_cnt_fns[k].rva) hook_install(g_cnt_fns[k].name, g_cnt_fns[k].rva, g_cnt_h[k]); }
    // the engine thread's context: halo3+0x9d22c (a millisecond clock read from ~420 places) runs lifted on the engine
    // thread ~7000 times a second in a match -- main_loop_body (0xb2250) and the sim tick are the app's own code now
    else if (!getenv("NHOOK_NO_HOOKS")) { hook_install("engine_clock", 0x9d22c, on_main_loop);
        // display_event: the app's own native code too (its entry is a `b` into the app's __TEXT; the lifted entry never ran)
        if (!getenv("NHOOK_NO_EVHOOK")) { uint64_t t = branch_target_of(0x11b0ec); if (!t || text_hook("display_event", t, on_display_event)) hook_install("display_event", 0x11b0ec, on_display_event); }
        if (getenv("NHOOK_LIST_BRANCH_ENTRIES")) {   // which halo3 functions the app replaced with its own code (entry = one `b`)
            uint64_t* tb = malloc(g_ntab * 16); int n = 0; char line[1024]; int k = 0;
            if (tb && !rd(g_tab, tb, g_ntab * 16)) for (uint64_t i = 0; i < g_ntab; i++) { uint32_t w0; if (rd(tb[2 * i + 1], &w0, 4) || (w0 & 0xfc000000u) != 0x14000000u) continue;
                n++; k += snprintf(line + k, sizeof line - (size_t)k, " %llx", (unsigned long long)(tb[2 * i] - 0x180000000ull));
                if (k > 900) { nlog("[nhook] branch entries:%s", line); k = 0; } }
            if (k) nlog("[nhook] branch entries:%s", line); nlog("[nhook] %d of %llu code-table entries are one `b` (functions the app replaced)", n, (unsigned long long)g_ntab); free(tb); }
        // the camera hook (the overlay's view): installed by the render thread the first time an overlay feature is on
        // (nov_view_hook_want), not here -- a session that never turns the overlay on never pays its ~700k filter passes a second
        if (!getenv("NHOOK_NO_VIEWHOOK") && !getenv("NHOOK_CBPROBE")) g_view_hook_at = branch_target_of(0x2af478);
        if (getenv("NHOOK_CBPROBE")) { uint64_t t = branch_target_of(0x2af478); if (t && !text_hook("set_param", t, on_set_param)) { pthread_t th; pthread_create(&th, 0, cbprobe_dump, 0); pthread_detach(th); } }
        if (getenv("NHOOK_TEXTHOOK_TEST")) {   // test: counting text hooks on functions the app replaced (NHOOK_TEXTHOOK_TEST=<rva>,<rva>...)
            const char* w = getenv("NHOOK_TEXTHOOK_TEST");
            for (int i = 0; i < 8 && w && *w; i++) { uint32_t rva = (uint32_t)strtoul(w, 0, 16); uint64_t t = branch_target_of(rva); char nm[32]; snprintf(nm, sizeof nm, "test halo3+0x%x", rva);
                g_tht_rva[i] = rva; nlog("[nhook] text hook test: halo3+0x%x's entry -> 0x%llx", rva, (unsigned long long)t); if (t) text_hook(nm, t, g_tht_h[i]);
                w = strchr(w, ','); if (w) w++; } }
        // object_delete: the app's own native function (its code-table entry is a `b` into the app's __TEXT): a text hook
        if (!getenv("NHOOK_NO_DELHOOK")) { uint64_t t = branch_target_of(0x342ed8); if (t) text_hook("object_delete", t, on_object_delete); else hook_install("object_delete", 0x342ed8, on_object_delete); }
        if (!getenv("NHOOK_NO_VOICEHOOK")) voice_hook_install(); }
    if (getenv("NHOOK_PLAYERS_LOG")) { pthread_t t; pthread_create(&t, 0, engine_thread, 0); pthread_detach(t); }
    if (getenv("NHOOK_PKG_PROBE")) { pthread_t t; pthread_create(&t, 0, pkg_probe, 0); pthread_detach(t); }
    if (getenv("NHOOK_CODETAB_DUMP")) { FILE* f = fopen(getenv("NHOOK_CODETAB_DUMP"), "w");   // the code table as "halo3 rva, host entry" lines:
        if (f) { for (uint64_t i = 0; i < g_ntab; i++) { uint64_t e[2]; if (rd(g_tab + i * 16, e, 16)) break;   // shim/tools/native-frames.py maps a
                     fprintf(f, "%llx %llx\n", (unsigned long long)(e[0] - 0x180000000ull), (unsigned long long)e[1]); }      // crash's host frames to halo3 functions
                 fclose(f); nlog("[nhook] code table (%llu entries) -> %s", (unsigned long long)g_ntab, getenv("NHOOK_CODETAB_DUMP")); } }
    if (getenv("NHOOK_DUMP_ENTRY")) { const char* w = getenv("NHOOK_DUMP_ENTRY");   // NHOOK_DUMP_ENTRY=<rva>[:<words>],...: a lifted function's first words (arm64; RE)
        while (w && *w) { uint32_t r = (uint32_t)strtoul(w, 0, 16); const char* c = strchr(w, ':'), * cm = strchr(w, ','); int n = c && (!cm || c < cm) ? atoi(c + 1) : 48;
            if (n > 512) n = 512; uint64_t e = host_of(r); uint32_t buf[512];
            if (e && !rd(e, buf, 4u * (unsigned)n)) { char line[64 * 9 + 64]; for (int i = 0; i < n; i += 64) { int k = 0; for (int j = i; j < n && j < i + 64; j++) k += snprintf(line + k, sizeof line - (size_t)k, "%08x ", buf[j]);
                nlog("[nhook] [entry] halo3+0x%x at 0x%llx +%d: %s", r, (unsigned long long)e, i * 4, line); } }
            w = cm; if (w) w++; } }
    if (getenv("NHOOK_RVA_KIND")) { const char* w = getenv("NHOOK_RVA_KIND");   // NHOOK_RVA_KIND=<rva>,<rva>...: how the app runs each halo3 function
        while (w && *w) { uint32_t r = (uint32_t)strtoul(w, 0, 16); uint64_t e = host_of(r), t = branch_target_of(r);
            nlog("[nhook] halo3+0x%x: %s (entry 0x%llx%s0x%llx)", r, rva_kind(r), (unsigned long long)e, t ? ", the app's code at " : "", (unsigned long long)t);
            w = strchr(w, ','); if (w) w++; } }
    hk_preflight();                                      // nhchk.c: the first-use hooks' shape checks (nothing written)
    nstats_start();
    { extern void nr_install(void); nr_install(); }      // nrender.m: the overlay, recording and captures in the app's frames
}

static void hooks_start(void);
static void* probe_thread(void* arg) {
    (void)arg;
    sleep(2);
    // ... and until the image is mapped (its MZ header) and the runtime's code table is filled: a dedicated server's worker,
    // starting beside its supervisor, was not ready after 2 s once ("image+0: no MZ", "code table at 0x0" -> hooks off, no
    // stats page; 2026-11-07, its fifth start that day). Up to 10 minutes: once (2026-11-08, a server's engine process while
    // four clients started) it was not ready after 60 s, the old limit, and the hooks stayed off for the process's life.
    { int w = 0; for (; w < 2400; w++) { uint8_t h[2]; uint64_t t[2];
          if (!rd(g_img, h, 2) && h[0] == 'M' && h[1] == 'Z' && (!g_app.codetab || (!rd(APP(codetab), t, 16) && t[0] && t[1] >= 1000 && t[1] <= 1000000))) break;
          if (w == 240) nlog("[nhook] the engine image / code table are not ready 60 s later than the usual 2 s: still waiting (up to 10 minutes)");
          usleep(250000); }
      if (w) nlog("[nhook] the engine image / code table were ready %.2f s later than the usual 2 s%s", w * 0.25, w >= 2400 ? " -- not at all: giving up" : ""); }
    const char* path = getenv("NHOOK_HALO3");
    if (!path) { nlog("[nhook] NHOOK_HALO3 not set: no image check"); hooks_start(); return 0; }   // (the hooks start anyway: shim/tools/native-dedicated.sh once lost them all)
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { nlog("[nhook] cannot open %s", path); return 0; }
    uint8_t* f = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (f == MAP_FAILED) return 0;
    uint32_t pe = *(uint32_t*)(f + 0x3c); uint16_t nsec = *(uint16_t*)(f + pe + 6), opt = *(uint16_t*)(f + pe + 20);
    uint8_t* oh = f + pe + 24; uint64_t ibase = *(uint64_t*)(oh + 24); uint32_t isize = *(uint32_t*)(oh + 56);
    uint8_t* dd = oh + 112; uint8_t* sh = oh + opt;
    uint64_t img = g_img;
    nlog("[nhook] halo3.dll preferred base 0x%llx size 0x%x; engine image at 0x%llx (pid %d)", (unsigned long long)ibase, isize,
         (unsigned long long)img, (int)getpid());
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
        if (!strcmp(nm, ".text")) { int h = hk_named("halo3.dll image check"); const char* sp = strstr(path, "/halo3/"); sp = sp ? sp + 1 : path;   // (no home folder on the page)
            if (ok && same == n) hk_state(h, HK_OK, img + va, ".text's first %u bytes as in MCC's %s", n, sp); else hk_state(h, HK_FAIL, img + va, ".text: %u of %u bytes as in MCC's %s (another halo3.dll?)", same, n, sp); hk_fire(h); }
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
    // the runtime's guest -> host code table (sorted {preferred-base address, host code} pairs, (ptr, count) at this build's
    // `codetab`; the guest image is the app's own reserved range at its `image` -- both + the app's slide: napp_builds.h)
    nlog("[nhook] app %s slide 0x%llx, image 0x%llx (this build's table: 0x%llx)", g_app_ver, (unsigned long long)g_app_slide, (unsigned long long)img, (unsigned long long)APP(image));
    uint64_t tab[2] = {0};
    if (APP(codetab) && !rd(APP(codetab), tab, 16)) {
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
    { HK_ONCE(hl, "log tap (write)"); hk_fire(hl); }
    if (n >= 12 && !memcmp(p, "[menus] open", 12)) { HK_ONCE(hm, "line: [menus] open"); hk_fire(hm); }
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
    { static FILE* tf; static int ti; if (!ti) { ti = 1; const char* f = getenv("NHOOK_TAP_LOG"); if (f && *f) tf = fopen(f, "a"); }   // NHOOK_TAP_LOG=<file>: every
      if (tf) { fwrite(p, 1, n, tf); fputc('\n', tf); fflush(tf); } }   //  line the hooks read (what a process logs that nobody shows: a server worker)
    nst_on_line(p, n);                                // match state and events (nstats.c)
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
    ssize_t r = write(fd, buf, n); if (r > 0 && fd > 2) pk_tcp_bytes(fd, PK_TX, r); return r;
}
static ssize_t my_writev(int fd, const struct iovec* iov, int cnt) {
    for (int i = 0; i < cnt; i++) if (iov[i].iov_len) { trace_write("writev", fd, iov[i].iov_base, iov[i].iov_len); tap(fd, iov[i].iov_base, iov[i].iov_len); }
    return writev(fd, iov, cnt);
}

// the .map files the app opens (h3maps.c hb_note_map: the map being played, tag names)
static void note_open(const char* p) { size_t L = p ? strlen(p) : 0; if (L > 4 && !strcasecmp(p + L - 4, ".map")) hb_note_map(p); }
static int my_open(const char* p, int fl, ...) {
    int mode = 0; if (fl & O_CREAT) { va_list ap; va_start(ap, fl); mode = va_arg(ap, int); va_end(ap); }
    int fd = open(p, fl, mode); if (fd >= 0) note_open(p); return fd;
}
static int my_openat(int dfd, const char* p, int fl, ...) {
    int mode = 0; if (fl & O_CREAT) { va_list ap; va_start(ap, fl); mode = va_arg(ap, int); va_end(ap); }
    int fd = openat(dfd, p, fl, mode); if (fd >= 0 && p && p[0] == '/') note_open(p); return fd;
}

// the packet log (nnet.c): every datagram's type and size, TCP connections; host names from getaddrinfo
static ssize_t my_sendto(int fd, const void* b, size_t n, int fl, const struct sockaddr* to, socklen_t tl) {
    ssize_t r = sendto(fd, b, n, fl, to, tl); if (r >= 0) { pk_wire(PK_TX, fd, to, b, r); pk_tcp_bytes(fd, PK_TX, r); } return r; }
static ssize_t my_recvfrom(int fd, void* b, size_t n, int fl, struct sockaddr* from, socklen_t* fln) {
    ssize_t r = recvfrom(fd, b, n, fl, from, fln); if (r >= 0) { pk_wire(PK_RX, fd, from && fln && *fln ? from : 0, b, r); pk_tcp_bytes(fd, PK_RX, r); } return r; }
static ssize_t my_sendmsg(int fd, const struct msghdr* m, int fl) {
    ssize_t r = sendmsg(fd, m, fl);
    if (r >= 0 && m) { pk_wire(PK_TX, fd, m->msg_namelen ? (const struct sockaddr*)m->msg_name : 0, m->msg_iovlen ? m->msg_iov[0].iov_base : 0, r); pk_tcp_bytes(fd, PK_TX, r); }
    return r; }
static ssize_t my_recvmsg(int fd, struct msghdr* m, int fl) {
    ssize_t r = recvmsg(fd, m, fl);
    if (r >= 0 && m) { pk_wire(PK_RX, fd, m->msg_namelen ? (const struct sockaddr*)m->msg_name : 0, m->msg_iovlen ? m->msg_iov[0].iov_base : 0, r); pk_tcp_bytes(fd, PK_RX, r); }
    return r; }
static ssize_t my_send(int fd, const void* b, size_t n, int fl) { ssize_t r = send(fd, b, n, fl); if (r >= 0) { pk_wire(PK_TX, fd, 0, b, r); pk_tcp_bytes(fd, PK_TX, r); } return r; }
static ssize_t my_recv(int fd, void* b, size_t n, int fl) { ssize_t r = recv(fd, b, n, fl); if (r >= 0) { pk_wire(PK_RX, fd, 0, b, r); pk_tcp_bytes(fd, PK_RX, r); } return r; }
static int my_connect(int fd, const struct sockaddr* sa, socklen_t sl) {
    int r = connect(fd, sa, sl); if (r == 0 || errno == EINPROGRESS) { int e = errno; pk_tcp_connect(fd, sa, sl); errno = e; } return r; }
static int my_close(int fd) { pk_close(fd); return close(fd); }
static int my_getaddrinfo(const char* h, const char* sv, const struct addrinfo* hi, struct addrinfo** res) {
    int r = getaddrinfo(h, sv, hi, res); if (!r && res) pk_dns(h, *res); return r; }

// recording's sound (shim/src/rec.m rec_audio_push): every render callback an output AudioUnit gets (the app's cpal
// streams: halo3's sound, the menus, voice) is wrapped; what it wrote goes into the recording while one runs
#include <AudioToolbox/AudioToolbox.h>
#include "../src/rec.h"
static volatile int g_takeover;   // (nvpad.m nh_takeover_install: the user took the observer's controls -- sound back on)
void nh_takeover_fire(void) {
    g_takeover = 1; const char* f = getenv("NHOOK_TAKEOVER_FILE");   // (shim/tools/observe-public.sh: the visit lasts until the user quits)
    if (f && *f) { int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) { char b[64]; int n = snprintf(b, sizeof b, "%ld\n", (long)time(0)); write(fd, b, n); close(fd); } } }
typedef struct { AudioUnit au; AURenderCallback cb; void* ref; AudioStreamBasicDescription fmt; int have; } AuWrap;
static AuWrap g_auw[32]; static int g_nauw; static pthread_mutex_t g_au_mx = PTHREAD_MUTEX_INITIALIZER;
static AuWrap* au_slot(AudioUnit au) { for (int i = 0; i < g_nauw; i++) if (g_auw[i].au == au) return &g_auw[i]; if (g_nauw >= 32) return 0; AuWrap* w = &g_auw[g_nauw++]; memset(w, 0, sizeof *w); w->au = au; return w; }
static OSStatus au_tap(void* ref, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames, AudioBufferList* io) {
    AuWrap* w = ref; OSStatus r = w->cb(w->ref, fl, ts, bus, frames, io);
    { static int mute = -1; if (mute < 0) mute = getenv("NHOOK_MUTE_OUTPUT") && strcmp(getenv("NHOOK_MUTE_OUTPUT"), "0");   // (nmic.c: silent test clients)
      if (mute && !g_takeover && !r && io) { for (UInt32 i = 0; i < io->mNumberBuffers; i++) if (io->mBuffers[i].mData) memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize); return r; } }
    if (r || !g_rec_audio || !io || !w->have || !io->mNumberBuffers || (fl && (*fl & kAudioUnitRenderAction_OutputIsSilence))) return r;
    const AudioStreamBasicDescription* f = &w->fmt; int fl2 = (int)f->mFormatFlags, isf = (fl2 & kAudioFormatFlagIsFloat) != 0, bits = (int)f->mBitsPerChannel;
    if (!(fl2 & kAudioFormatFlagIsNonInterleaved)) {
        int ch = (int)f->mChannelsPerFrame; if (ch < 1) return r;
        rec_audio_push(w, io->mBuffers[0].mData, frames, ch, (int)f->mSampleRate, bits, (int)f->mBytesPerFrame, isf);
    } else if (isf && bits == 32 && frames <= 8192) {   // one buffer per channel: the first two interleaved
        static __thread float tmp[8192 * 2]; int ch = io->mNumberBuffers >= 2 ? 2 : 1;
        for (UInt32 i = 0; i < frames; i++) for (int c = 0; c < ch; c++) tmp[i * ch + c] = ((const float*)io->mBuffers[c].mData)[i];
        rec_audio_push(w, tmp, frames, ch, (int)f->mSampleRate, 32, ch * 4, 1);
    }
    return r;
}
static int vmic_set(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, const void* data, UInt32 sz, OSStatus* r);   // (nmic.c)
static int vmic_render(AudioUnit au, UInt32 bus, UInt32 frames, AudioBufferList* io, OSStatus* r);
static const AudioStreamBasicDescription* mic_fmt(AudioUnit au);
static void sb_into(AudioBufferList* io, UInt32 frames, const AudioStreamBasicDescription* f, int mix);
static OSStatus my_AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, const void* data, UInt32 sz) {
    { OSStatus vr; if (vmic_set(au, id, sc, el, data, sz, &vr)) return vr; }   // (the soundboard microphone's units; capture formats)
    { HK_ONCE(ha, "AudioUnitSetProperty"); hk_fire(ha); }
    if (id == kAudioUnitProperty_SetRenderCallback && data && sz >= sizeof(AURenderCallbackStruct) && !getenv("NHOOK_NO_AUDIO_TAP")) {
        const AURenderCallbackStruct* cs = data;
        if (cs->inputProc && cs->inputProc != au_tap) { pthread_mutex_lock(&g_au_mx); AuWrap* w = au_slot(au);
            if (w) { w->cb = cs->inputProc; w->ref = cs->inputProcRefCon; } pthread_mutex_unlock(&g_au_mx);
            if (w) { AURenderCallbackStruct n = {au_tap, w}; return AudioUnitSetProperty(au, id, sc, el, &n, sizeof n); } }
    }
    if (id == kAudioUnitProperty_StreamFormat && sc == kAudioUnitScope_Input && data && sz >= sizeof(AudioStreamBasicDescription)) {
        pthread_mutex_lock(&g_au_mx); AuWrap* w = au_slot(au); if (w) { memcpy(&w->fmt, data, sizeof w->fmt); w->have = 1; } pthread_mutex_unlock(&g_au_mx);
    }
    return AudioUnitSetProperty(au, id, sc, el, data, sz);
}

#define INTERPOSE(rep, orig) __attribute__((used)) static struct { const void* r; const void* o; } \
    _ip_##orig __attribute__((section("__DATA,__interpose"))) = { (const void*)(rep), (const void*)(orig) }
// the page's mic switch (nstats.c mic_json, g_mic_muted): the app's capture pulls its samples on the AUHAL's input bus 1
static OSStatus my_AudioUnitRender(AudioUnit au, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames, AudioBufferList* io) {
    { OSStatus vr; if (vmic_render(au, bus, frames, io, &vr)) return vr; }   // (nmic.c: the virtual microphone's samples = the soundboard)
    OSStatus r = AudioUnitRender(au, fl, ts, bus, frames, io);
    if (bus == 1) { HK_ONCE(hm, "AudioUnitRender (mic)"); hk_fire(hm); }
    if (bus == 1 && !g_hc_off[HC_MIC]) { uint64_t t0 = hc_now(); g_mic_t = mono_s();
        if (mic_muted() && !r && io) for (UInt32 i = 0; i < io->mNumberBuffers; i++) if (io->mBuffers[i].mData) memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
        if (!r && io) sb_into(io, frames, mic_fmt(au), 1);   // the soundboard over a real microphone (nmic.c)
        hc_add(HC_MIC, t0); }
    return r; }
INTERPOSE(my_write, write);
INTERPOSE(my_writev, writev);
INTERPOSE(my_open, open);
INTERPOSE(my_openat, openat);
INTERPOSE(my_sendto, sendto);
INTERPOSE(my_recvfrom, recvfrom);
INTERPOSE(my_sendmsg, sendmsg);
INTERPOSE(my_recvmsg, recvmsg);
INTERPOSE(my_send, send);
INTERPOSE(my_recv, recv);
INTERPOSE(my_connect, connect);
INTERPOSE(my_close, close);
INTERPOSE(my_getaddrinfo, getaddrinfo);
INTERPOSE(my_AudioUnitSetProperty, AudioUnitSetProperty);
INTERPOSE(my_AudioUnitRender, AudioUnitRender);
#include "nmic.c"   // the microphone path: NHOOK_AUDIO_TRACE, NHOOK_MUTE_OUTPUT, the soundboard microphone

#include <dispatch/dispatch.h>
static void vpad_start(void* c) { (void)c; g_vpad_on = nh_vpad_install(); }
void nh_takeover_install(void);   // nvpad.m
__attribute__((constructor)) static void nhook_init(void) {
    const char* lf = getenv("NHOOK_LOG");
    if (lf) g_logfd = open(lf, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (getenv("NHOOK_TRACE_WRITES")) g_trace_writes = atoi(getenv("NHOOK_TRACE_WRITES"));
    nlog("[nhook] loaded (pid %d)", getpid());
    hk_init(); app_build_pick();                     // the hooks check's items; this app build's addresses (napp_builds.h)
    if (getenv("NHOOK_VPAD") && strcmp(getenv("NHOOK_VPAD"), "0") && getenv("RECLAIMER_PROFILE_DIR"))   // nvpad.m: test identities only
        vpad_start(0);   // (at once: the app may enumerate controllers only early -- 300 ms later it never asked again)
    if (getenv("NHOOK_TAKEOVER") && strcmp(getenv("NHOOK_TAKEOVER"), "0")) {   // (the observer: a click gives the user the controls)
        if (!g_vpad_on) vpad_start(0);   // (the bridge's virtual pad, at once like the walkers': the app enumerates controllers early; the user drives it, never a bot)
        nh_takeover_install(); }
    if (getenv("NHOOK_SELFTEST")) nstats_start();       // shim/tools/nhook-selftest.sh: the stats server without the app
}
