// Native GL helper (PERF-PLAN option 1): the shared-memory layout both sides use. The loader (x86_64, Rosetta) is the
// only producer of the command ring; the helper (shim/src/glhelper.c, native arm64) the only consumer. Same layout on
// both architectures (fixed-size fields, 8-byte aligned atomics).
#pragma once
#include <stdint.h>
#include <stdatomic.h>

#define GLH_MAGIC   0x31484C47u        // "GLH1"
#define GLH_VERSION 6
#define GLH_RING    (256ull<<20)       // command ring
#define GLH_REPLY   (64ull<<20)        // results of synchronous calls (one in flight at a time)
#define GLH_POOLN   4096               // pre-generated GL names per kind
#define GLH_QMAX    65536              // occlusion-query results, indexed by query name
#define GLH_AL8(n)  (((uint64_t)(n)+7)&~7ull)
#define GLH_MAILBOX (GLH_REPLY/2)          // asynchronous readbacks land in the reply area's upper half

enum { GLH_POOL_TEX, GLH_POOL_BUF, GLH_POOL_FBO, GLH_POOL_RBO, GLH_POOL_VAO, GLH_POOL_SAMP, GLH_POOL_QUERY, GLH_POOL_PIPE,
       GLH_POOL_PROG, GLH_POOL_VS, GLH_POOL_FS, GLH_NPOOL };

// hand-written commands (the generated GL calls use 1 .. GLH_OP_GEN_END-1)
enum { GLH_X_BASE=1000,
       GLH_X_WRAP,          // the rest of the ring up to its end is padding
       GLH_X_POOLFILL,      // sync: refill the name pools now
       GLH_X_SHADERSOURCE,  // shader, then the source bytes
       GLH_X_MAPWRITE,      // target, offset, length, access, then the bytes: glMapBufferRange + memcpy + glUnmapBuffer
       GLH_X_MAPREAD,       // sync: target, offset, length -> the bytes
       GLH_X_FENCE,         // id: glFenceSync, kept under id
       GLH_X_WAITSYNC,      // id
       GLH_X_DELSYNC,       // id
       GLH_X_GETERROR,      // sync
       GLH_X_GETSTRING,     // sync: name -> string
       GLH_X_CHECKFB,       // sync: target -> status
       GLH_X_FINISH,        // sync: glFinish
       GLH_X_BEGINQUERY,    // target, id, generation
       GLH_X_ENDQUERY,      // target
       GLH_X_CREATESHADER,  // sync: type -> name (types without a pool)
       GLH_X_GETTEXIMAGE,   // sync: target, level, format, type -> the bytes (the helper sizes them)
       GLH_X_PQ_BLIT,       // present: slot, source framebuffer, source w, h, frame number
       GLH_X_NOP_SYNC,      // sync: nothing (wait until everything before it ran)
       GLH_X_READBACK,      // async: buffer, bytes, mailbox offset, ticket -> copy a PIXEL_PACK buffer into the mailbox, then rb_done=ticket
       GLH_X_LOCS,          // sync: program, name count, first block name -> int per name (uniform locations, then block indices)
       GLH_X_TFVARYINGS,    // program, count, buffer mode, then the names (each NUL-terminated)
       GLH_X_END };

typedef struct {
    uint32_t magic, version;
    _Atomic uint32_t ready, quit, failed;
    uint32_t pad0;
    _Atomic uint64_t head, tail;                 // ring byte positions (monotonic)
    _Atomic uint64_t sync_req, sync_done;        // synchronous call sequence numbers
    uint64_t sync_rv, sync_len;                  // return value / reply byte count of the last synchronous call
    _Atomic uint32_t helper_sleeping, wake_seq;     // wake_seq: the loader bumps it and wakes the helper (os_sync_wake_by_address, shared)
    _Atomic uint32_t pool_head[GLH_NPOOL], pool_tail[GLH_NPOOL];
    uint32_t pool[GLH_NPOOL][GLH_POOLN];
    // presentation: three IOSurface-backed frames (global IDs, looked up by the loader's present thread)
    uint32_t pq_ios[3], pq_w, pq_h, pad2;
    _Atomic int32_t pq_latest, pq_reading;
    _Atomic uint64_t pq_seq;
    uint64_t pq_frameno[3];
    _Atomic uint64_t frames_sent, frames_done;
    // asynchronous readbacks: tickets completed so far (mailbox data lives in the reply area's upper half)
    _Atomic uint64_t rb_done;
    // occlusion queries: written by the helper once a result is available
    struct { _Atomic uint32_t gen; _Atomic uint32_t avail; uint64_t value; } q[GLH_QMAX];
    // statistics (helper)
    uint64_t st_cmds, st_bytes, st_syncs; double st_busy_s, st_wait_s;
    double st_gpu_s; uint64_t st_gpu_n;          // present fences: time from the frame's blit to its fence signalling (sum, count)
    char err[256];
} GlhHdr;

static inline uint8_t* glh_ring(GlhHdr*h){ return (uint8_t*)(h+1); }
static inline uint8_t* glh_reply(GlhHdr*h){ return (uint8_t*)(h+1)+GLH_RING; }
static inline uint64_t glh_shm_size(void){ return sizeof(GlhHdr)+GLH_RING+GLH_REPLY; }
