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

void nh_log(const char* fmt, ...) {                 // (nrender.m's logger)
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); nlog("%s", b);
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

static int hook_install(const char* name, uint32_t rva, nh_handler h) {
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
    size_t tl = nh_tpl_end - nh_tpl_start;
    uint8_t* st = near_page(e, 0x4000);              // its own page: a page that holds a live stub is never made writable again
    if (!st) { nlog("[nhook] hook %s: no stub page near 0x%llx", name, (unsigned long long)e); return -1; }
    int64_t d = (int64_t)((uint64_t)st - e);
    memcpy(st, nh_tpl_start, tl);
    memcpy(st + (nh_tpl_displaced - nh_tpl_start), &displaced, 4);            // the displaced stp x29,x30,[sp,#-16]! (or a nop for a branch)
    uint64_t hv = (uint64_t)h, rv = ret;
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

// ---- hooks on the app's OWN code (its signed __TEXT: functions the app replaced, e.g. object_delete) --------------------
// The app is signed ad hoc with no hardened runtime (codesign flags 0x2): its text pages are made writable as private copies
// (VM_PROT_COPY), the function's first four instructions become `ldr x16, #8; br x16; .quad stub` and go to the stub
// (stub.S _nh_tp4: handler(x0), then those four instructions, then back). Refused when one of them is PC-relative.
// Installed once at startup. NHOOK_NO_TEXT_HOOKS=1 = none.
extern char nh_tp4_start[], nh_tp4_displaced[], nh_tp4_handler[], nh_tp4_return[], nh_tp4_end[];
static int insn_pcrel(uint32_t w) {
    return (w & 0x1f000000u) == 0x10000000u            // adr / adrp
        || (w & 0x7c000000u) == 0x14000000u            // b / bl
        || (w & 0x7e000000u) == 0x34000000u            // cbz / cbnz
        || (w & 0x7e000000u) == 0x36000000u            // tbz / tbnz
        || (w & 0xff000010u) == 0x54000000u            // b.cond
        || (w & 0x3b000000u) == 0x18000000u;           // ldr (literal)
}
extern char nh_tpf_start[], nh_tpf_want[], nh_tpf_displaced[], nh_tpf_handler[], nh_tpf_return[], nh_tpf_end[];
static int text_hook_t(const char* name, uint64_t f, nh_handler h, int filt, uint32_t want) {   // filt: _nh_tpf, the handler only for parameter id `want`
    if (getenv("NHOOK_NO_TEXT_HOOKS")) return -1;
    uint32_t w[4]; if (rd(f, w, 16)) { nlog("[nhook] text hook %s: 0x%llx unreadable", name, (unsigned long long)f); return -1; }
    for (int i = 0; i < 4; i++) if (insn_pcrel(w[i])) { nlog("[nhook] text hook %s: instruction %d (%08x) is PC-relative: refused", name, i, w[i]); return -1; }
    char* ts = filt ? nh_tpf_start : nh_tp4_start; char* te = filt ? nh_tpf_end : nh_tp4_end;
    size_t tl = te - ts; mach_vm_address_t st = 0;
    if (mach_vm_allocate(mach_task_self(), &st, 0x4000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return -1;
    memcpy((void*)st, ts, tl); memcpy((uint8_t*)st + ((filt ? nh_tpf_displaced : nh_tp4_displaced) - ts), w, 16);
    uint64_t hv = (uint64_t)h, rv = f + 16; memcpy((uint8_t*)st + ((filt ? nh_tpf_handler : nh_tp4_handler) - ts), &hv, 8); memcpy((uint8_t*)st + ((filt ? nh_tpf_return : nh_tp4_return) - ts), &rv, 8);
    if (filt) memcpy((uint8_t*)st + (nh_tpf_want - ts), &want, 4);
    mach_vm_protect(mach_task_self(), st, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE); sys_icache_invalidate((void*)st, tl);
    uint64_t pg = f & ~0x3fffull, end = (f + 16 + 0x3fff) & ~0x3fffull;
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, end - pg, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) { nlog("[nhook] text hook %s: the page stays read-only (kr %d)", name, kr); return -1; }
    uint32_t patch[4] = {0x58000050u, 0xd61f0200u, (uint32_t)st, (uint32_t)((uint64_t)st >> 32)};   // ldr x16, #8 ; br x16 ; .quad stub
    memcpy((void*)(uintptr_t)(f + 8), &patch[2], 8); __atomic_thread_fence(__ATOMIC_SEQ_CST); memcpy((void*)(uintptr_t)f, patch, 8);
    kr = mach_vm_protect(mach_task_self(), pg, end - pg, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    sys_icache_invalidate((void*)(uintptr_t)f, 16);
    nlog("[nhook] text hook %s: 0x%llx -> stub 0x%llx (back to executable: kr %d)", name, (unsigned long long)f, (unsigned long long)st, kr);
    return 0;
}
static int text_hook(const char* name, uint64_t f, nh_handler h) { return text_hook_t(name, f, h, 0, 0); }
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
static const struct { const char* name; uint32_t rva; } g_cnt_fns[] = {
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
static void on_view_param(uint64_t* ctx) {      // (only parameter 0x27 gets here: the stub filters)
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
static uint64_t tls_block(void) {                   // halo3's TLS block of the engine main thread (index 0 here)
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

static void hooks_start(void) {
    g_slide = g_img - APP_IMAGE;
    uint64_t tab[2]; if (rd(APP_CODETAB + g_slide, tab, 16) || !tab[0] || tab[1] < 1000 || tab[1] > 1000000) { nlog("[nhook] no code table: hooks off"); return; }
    g_tab = tab[0]; g_ntab = tab[1];
    if (getenv("NHOOK_CENSUS")) census_install();
    else if (getenv("NHOOK_COUNT")) for (size_t k = 0; k < NCNT; k++) hook_install(g_cnt_fns[k].name, g_cnt_fns[k].rva, g_cnt_h[k]);
    // the engine thread's context: halo3+0x9d22c (a millisecond clock read from ~420 places) runs lifted on the engine
    // thread ~7000 times a second in a match -- main_loop_body (0xb2250) and the sim tick are the app's own code now
    else if (!getenv("NHOOK_NO_HOOKS")) { hook_install("engine_clock", 0x9d22c, on_main_loop);
        if (!getenv("NHOOK_NO_EVHOOK")) hook_install("display_event", 0x11b0ec, on_display_event);
        if (getenv("NHOOK_LIST_BRANCH_ENTRIES")) {   // which halo3 functions the app replaced with its own code (entry = one `b`)
            uint64_t* tb = malloc(g_ntab * 16); int n = 0; char line[1024]; int k = 0;
            if (tb && !rd(g_tab, tb, g_ntab * 16)) for (uint64_t i = 0; i < g_ntab; i++) { uint32_t w0; if (rd(tb[2 * i + 1], &w0, 4) || (w0 & 0xfc000000u) != 0x14000000u) continue;
                n++; k += snprintf(line + k, sizeof line - (size_t)k, " %llx", (unsigned long long)(tb[2 * i] - 0x180000000ull));
                if (k > 900) { nlog("[nhook] branch entries:%s", line); k = 0; } }
            if (k) nlog("[nhook] branch entries:%s", line); nlog("[nhook] %d of %llu code-table entries are one `b` (functions the app replaced)", n, (unsigned long long)g_ntab); free(tb); }
        if (!getenv("NHOOK_NO_VIEWHOOK") && !getenv("NHOOK_CBPROBE")) { uint64_t t = branch_target_of(0x2af478); if (t) text_hook_t("view constants", t, on_view_param, 1, 0x27); }
        if (getenv("NHOOK_CBPROBE")) { uint64_t t = branch_target_of(0x2af478); if (t && !text_hook("set_param", t, on_set_param)) { pthread_t th; pthread_create(&th, 0, cbprobe_dump, 0); pthread_detach(th); } }
        if (getenv("NHOOK_TEXTHOOK_TEST")) {   // test: counting text hooks on functions the app replaced (NHOOK_TEXTHOOK_TEST=<rva>,<rva>...)
            const char* w = getenv("NHOOK_TEXTHOOK_TEST");
            for (int i = 0; i < 8 && w && *w; i++) { uint32_t rva = (uint32_t)strtoul(w, 0, 16); uint64_t t = branch_target_of(rva); char nm[32]; snprintf(nm, sizeof nm, "test halo3+0x%x", rva);
                g_tht_rva[i] = rva; nlog("[nhook] text hook test: halo3+0x%x's entry -> 0x%llx", rva, (unsigned long long)t); if (t) text_hook(nm, t, g_tht_h[i]);
                w = strchr(w, ','); if (w) w++; } }
        // object_delete: the app's own native function (its code-table entry is a `b` into the app's __TEXT): a text hook
        if (!getenv("NHOOK_NO_DELHOOK")) { uint64_t t = branch_target_of(0x342ed8); if (t) text_hook("object_delete", t, on_object_delete); else hook_install("object_delete", 0x342ed8, on_object_delete); } }
    if (getenv("NHOOK_PLAYERS_LOG")) { pthread_t t; pthread_create(&t, 0, engine_thread, 0); pthread_detach(t); }
    nstats_start();
    { extern void nr_install(void); nr_install(); }      // nrender.m: the overlay, recording and captures in the app's frames
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
typedef struct { AudioUnit au; AURenderCallback cb; void* ref; AudioStreamBasicDescription fmt; int have; } AuWrap;
static AuWrap g_auw[32]; static int g_nauw; static pthread_mutex_t g_au_mx = PTHREAD_MUTEX_INITIALIZER;
static AuWrap* au_slot(AudioUnit au) { for (int i = 0; i < g_nauw; i++) if (g_auw[i].au == au) return &g_auw[i]; if (g_nauw >= 32) return 0; AuWrap* w = &g_auw[g_nauw++]; memset(w, 0, sizeof *w); w->au = au; return w; }
static OSStatus au_tap(void* ref, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames, AudioBufferList* io) {
    AuWrap* w = ref; OSStatus r = w->cb(w->ref, fl, ts, bus, frames, io);
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
static OSStatus my_AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, const void* data, UInt32 sz) {
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

__attribute__((constructor)) static void nhook_init(void) {
    const char* lf = getenv("NHOOK_LOG");
    if (lf) g_logfd = open(lf, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (getenv("NHOOK_TRACE_WRITES")) g_trace_writes = atoi(getenv("NHOOK_TRACE_WRITES"));
    nlog("[nhook] loaded (pid %d)", getpid());
    if (getenv("NHOOK_SELFTEST")) nstats_start();       // shim/tools/nhook-selftest.sh: the stats server without the app
}
