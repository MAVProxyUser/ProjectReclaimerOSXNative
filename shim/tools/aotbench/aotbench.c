// aotbench: does the SAME x86-64 code run faster under Rosetta when it was translated ahead of time (code in a Mach-O image,
// translated by oahd before it runs) than when Rosetta translates it at run time (code copied into memory and jumped to --
// how the loader runs halo3.dll and the client exe today)? And can code we load ourselves get the ahead-of-time treatment
// by putting it in a dylib (shim/tools/aotbench/run.sh builds one from the same bytes)?
//
// The workloads are leaf functions in their own section (__TEXT,__bench): no calls, no globals, no rip-relative constants, so
// their bytes run unchanged at any address. Each is timed (best of 7) three ways:
//   aot     called where it is, in this executable's text            (ahead-of-time translated)
//   jit     its section copied into an RWX (MAP_JIT) page and called  (translated at run time, as the loader's guest code)
//   dylib   the same bytes in benchcode.dylib, dlopen'ed (argv[1])   (does a dylib we build get ahead-of-time translation?)
// Then the patch test: a 14-byte `jmp [rip+0]` written over a function entry in the dylib (as the client's detours do in
// halo3.dll) -- does the patched image still run, and do the other functions keep their speed?
//     bash shim/tools/aotbench/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <mach-o/getsect.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>

#define BENCH __attribute__((noinline, used, section("__TEXT,__bench")))
typedef uint64_t (*BenchFn)(void *p, uint64_t n);

// 1. integer mixing over a buffer (hash / checksum style)
BENCH uint64_t w_mix(void *p, uint64_t n) { uint32_t *a = p; uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) { uint32_t v = a[i & 4095] + (uint32_t)i; h ^= v; h *= 1099511628211ull; h ^= h >> 29; } return h; }   // (reads only: every run gives the same result)
// 2. float math: 4x4 matrix chains (transform-heavy engine code); constants made from integers (no rip-relative loads)
BENCH uint64_t w_mat(void *p, uint64_t n) { float *m = p; float acc = 0;
    for (uint64_t k = 0; k < n; k++) { float r[16]; const float *a = m + ((k * 16) & 1023), *b = m + (((k + 5) * 16) & 1023);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        acc += r[(int)(k & 15)]; m[(k * 7) & 1023] = r[5] * (float)(int)(k & 1) + m[(k * 7) & 1023]; }
    union { float f; uint32_t u; } x = {acc}; return x.u; }
// 3. branchy code: a small state machine over bytes (parser / game-logic style)
BENCH uint64_t w_branch(void *p, uint64_t n) { uint8_t *s = p; uint64_t st = 0, cnt = 0;
    for (uint64_t i = 0; i < n; i++) { uint8_t c = s[i & 65535];
        switch ((st + c) & 7) { case 0: st = st * 3 + 1; break; case 1: cnt += c; st ^= 5; break; case 2: if (c > 128) st += 7; else st -= 3; break;
            case 3: cnt ^= st; break; case 4: st = (st << 1) | (c & 1); break; case 5: if ((c & 3) == 1) cnt++; break; case 6: st += cnt & 15; break; default: st -= 1; }
        if (st > 100000) st &= 1023; }
    return st + cnt; }
// 4. pointer chasing through a shuffled index list (object tables, linked structures)
BENCH uint64_t w_chase(void *p, uint64_t n) { uint32_t *next = p; uint32_t i = 0; uint64_t s = 0;
    for (uint64_t k = 0; k < n; k++) { i = next[i]; s += i; } return s; }
// 5. many small calls (call / ret heavy: engine code calls small helpers constantly)
BENCH __attribute__((noinline)) static uint64_t w_leaf(uint64_t a, uint64_t b) { return (a ^ b) + (a >> 3); }
BENCH uint64_t w_calls(void *p, uint64_t n) { (void)p; uint64_t s = 1; uint64_t (*volatile f)(uint64_t, uint64_t) = w_leaf;
    for (uint64_t k = 0; k < n; k++) s = f(s, k); return s; }

static const struct { const char *name; BenchFn fn; uint64_t n; } W[] = {
    {"integer mixing", w_mix, 40000000}, {"4x4 float matrices", w_mat, 4000000}, {"branchy state machine", w_branch, 60000000},
    {"pointer chasing", w_chase, 30000000}, {"small calls", w_calls, 60000000}};
enum { NW = sizeof W / sizeof W[0] };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *bufs[NW];
static double run(BenchFn f, int w, uint64_t *out) { double best = 1e9; for (int r = 0; r < 7; r++) { double t0 = now(); *out = f(bufs[w], W[w].n); double d = now() - t0; if (d < best) best = d; } return best; }

int main(int argc, char **argv) {
    // inputs
    for (int w = 0; w < NW; w++) { bufs[w] = calloc(1, 1 << 22); }
    { uint32_t *a = bufs[0]; for (int i = 0; i < 4096; i++) a[i] = i * 2654435761u; }
    { float *m = bufs[1]; for (int i = 0; i < 1024; i++) m[i] = (float)((i * 37) % 101) / 101.0f; }
    { uint8_t *s = bufs[2]; uint32_t x = 1; for (int i = 0; i < 65536; i++) { x = x * 1103515245 + 12345; s[i] = (uint8_t)(x >> 16); } }
    { uint32_t *nx = bufs[3]; int N = 1 << 20; uint32_t *perm = malloc(N * 4); for (int i = 0; i < N; i++) perm[i] = i; uint32_t x = 7;
      for (int i = N - 1; i > 0; i--) { x = x * 1664525 + 1013904223; int j = x % (i + 1); uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
      for (int i = 0; i < N; i++) nx[perm[i]] = perm[(i + 1) % N]; free(perm); }
    // the __bench section of this image
    unsigned long sz = 0; const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(0);
    uint8_t *sec = getsectiondata(mh, "__TEXT", "__bench", &sz); if (!sec || !sz) { fprintf(stderr, "no __bench section\n"); return 1; }
    // JIT copy
    uint8_t *jit = mmap(0, (sz + 16383) & ~16383ul, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(jit, sec, sz); __builtin___clear_cache((char *)jit, (char *)jit + sz);
    // dylib with the same bytes (argv[1])
    uint8_t *dsec = 0; unsigned long dsz = 0; void *dh = 0;
    if (argc > 1) { dh = dlopen(argv[1], RTLD_NOW); if (!dh) fprintf(stderr, "dlopen: %s\n", dlerror());
        else { const uint8_t *anchor = dlsym(dh, "bench_code"); if (anchor) { dsec = (uint8_t *)anchor; dsz = sz; } } }
    if (dsec && memcmp(dsec, sec, sz)) { fprintf(stderr, "the dylib's bytes differ from this image's __bench section: rebuild\n"); dsec = 0; }
    printf("__bench: %lu bytes; executable at %p, JIT copy at %p%s%p\n", sz, sec, jit, dsec ? ", dylib at " : "", dsec);
    printf("%-24s %10s %10s %10s   (ms, best of 7)\n", "workload", "aot", "jit", "dylib");
    double sa = 0, sj = 0, sd = 0;
    for (int w = 0; w < NW; w++) {
        uint64_t off = (uint64_t)((uint8_t *)W[w].fn - sec), ra, rj, rd = 0;
        double ta = run(W[w].fn, w, &ra), tj = run((BenchFn)(jit + off), w, &rj), td = dsec ? run((BenchFn)(dsec + off), w, &rd) : 0;
        sa += ta; sj += tj; sd += td;
        printf("%-24s %10.1f %10.1f %10.1f   jit/aot %.2fx%s%s\n", W[w].name, ta * 1e3, tj * 1e3, td * 1e3, tj / ta, dsec ? (td / ta < 1.1 ? "  dylib~aot" : td / tj < 1.1 ? "  dylib~jit" : "") : "",
               (ra != rj || (dsec && rd != ra)) ? "  RESULT MISMATCH" : ""); }
    printf("%-24s %10.1f %10.1f %10.1f   jit/aot %.2fx\n", "total", sa * 1e3, sj * 1e3, sd * 1e3, sj / sa);
    // patch test: a detour over w_leaf's entry in the dylib (jmp [rip+0] -> the JIT copy's w_leaf), like the client's hooks
    if (dsec) {
        uint8_t *tgt = dsec + ((uint8_t *)w_leaf - sec); uint64_t dst = (uint64_t)(jit + ((uint8_t *)w_leaf - sec));
        uintptr_t pg = (uintptr_t)tgt & ~(uintptr_t)16383; int rc = mprotect((void *)pg, 32768, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (rc) { kern_return_t kr = vm_protect(mach_task_self(), pg, 32768, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY); rc = kr ? -1 : 0; }
        printf("patch: making the dylib's code writable %s\n", rc ? "FAILED" : "worked");
        if (!rc) { uint8_t j[14] = {0xFF, 0x25, 0, 0, 0, 0}; memcpy(j + 6, &dst, 8); memcpy(tgt, j, 14); __builtin___clear_cache((char *)tgt, (char *)tgt + 14);
            for (int w = 0; w < NW; w++) { uint64_t off = (uint64_t)((uint8_t *)W[w].fn - sec), rd; double td = run((BenchFn)(dsec + off), w, &rd), ta = run(W[w].fn, w, &rd);
                printf("  after the patch: %-22s dylib %8.1f ms (aot %8.1f)\n", W[w].name, td * 1e3, ta * 1e3); } } }
    return 0; }
