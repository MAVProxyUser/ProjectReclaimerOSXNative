# Frame-rate plan

## Overnight 2026-10-17: where big games stand

Targets (the user: at least as good as existing Mac ports; the best published one is a Wine+DXVK+MoltenVK Reclaimer
port, steady 60 capped / ~95-120 uncapped on an M2 Max, BTB "playable"): small games 120 fps median, BTB a steady 60,
100+ players 60 median (dips to 45).

| game (M4 Max, quiet-run client) | before | after |
|---|---|---|
| [US-W] Infection, ~90-100 players, Valhalla | 15-24 fps @ ~3,000 draws | 37-48 fps @ 2,900-3,300 draws |
| same server, 57-62 players, Sandtrap (auto mode) | -- | 35-61 fps (mostly 42-53) @ 1,300-1,800 draws |
| 66-player Slayer (AU), Snowbound | -- | 55-68 fps @ ~2,500 draws (submit on or off) |
| local Valhalla, 1 player, spawn view | -- | 62-64 fps @ 2,190 draws (submit off) |

What moved the 100-player number, in order of effect:
1. **CPU particle compute** (`dxbc_cpu.c`): the renderer waited ~10 ms a frame for particle results. Now a SIMT
   executor (each instruction across 64 lanes, active lanes only; `PELOAD_CS_SCALAR` = old, `PELOAD_CS_CHECK` runs both
   and compares the UAVs byte for byte -- 0 differences over 10k+ dispatches), 4 dependency-ordered workers
   (`PELOAD_CS_WORKERS`), and read-only inputs copied once per resource version instead of per dispatch (was ~0.5 GB/s).
   235 -> 87 us per dispatch; compute waits 22-55% -> 1-11% of the GL thread.
2. **No per-frame queue drains** under the submission thread: `Map(READ, DO_NOT_WAIT)` of a staging texture answers
   `DXGI_ERROR_WAS_STILL_DRAWING` while a queued copy into it hasn't run; `Map(WRITE)` renames the block like
   WRITE_DISCARD (copying the newest contents) when no queued write targets the resource.
3. **GL submission thread** (`PELOAD_D3DGL_SUBMIT`, d3dsubmit.c), in-game A/B (`PELOAD_AB=submit:20` flips it every 20 s
   and logs each phase): +10-15% with ~60-100 players, even at 66, -7% alone (its per-draw cost plus up to a frame staler
   occlusion results = ~4% more draws). So `play.sh` runs it in `auto` mode: on at 40+ players in the session.

Measured and found NOT to be the limit (keep for reference):
- **GPU**: 15-27% utilised in a 57-player game at 28 fps (`ioreg ... "Device Utilization %"`).
- **Redundant GL calls**: big games make ~31 GL calls per draw (`PELOAD_GLCOUNT=1`): BindBuffer 8, Map/Unmap 2.6 each,
  BindBufferRange 2.6, VertexAttribPointer 2.4, stencil 2.6. A per-context state cache (`PELOAD_D3DGL_GLCACHE=1`) cuts
  that to ~18 with no measurable fps change -- Apple's driver returns early on redundant binds; the cost is in real state
  changes, the draw calls themselves and copies. Batching a draw's CB uploads into one ring map: neutral too.
- **halo3's main thread**: copies its whole game state with vcruntime's memcpy every frame (23% of that thread); big
  copies now run on the host's memmove (`PELOAD_GUEST_MEMCPY=1` = old), but the main thread wasn't the limit (A/B neutral).
- Some phases of public games are limited outside this Mac (an Australian server: every thread mostly idle at 17-33 fps).

Native GL helper (option 1 below), re-measured with `shim/tools/glbench.sh` (the same GL draw loop built x86_64 and
arm64): Apple's driver is **~1.5x** faster natively (2.9 -> 1.9 us per draw with 2 textures + 2 CBs; 3.3 -> 2.1 with
6 + 4), not ~2x. With the GL thread the limit in BTB-sized games (~6.3 us per draw, ~160k draws/s), that is the
remaining big step towards a steady 60 there.

Other finds on the way: the client's `mcc-voice` thread polled the push-to-talk key ~500k times a second with no sleep
(no microphone) -- now damped (`PELOAD_POLL_DAMP`); `mach_absolute_time` is in nanoseconds under Rosetta (timebase 1/1),
so tick math assuming 24 MHz was 41x off.

Status 2026-10-16, Big Team Battle numbers added 2026-10-17. Measurements are from a direct Guardian match on an M4 Max (`PELOAD_D3DGL_PROF=1`,
2-second windows) unless noted.

## 80-100 player games with the helper (2026-10-17 late): the engine's main thread is the wall

BTB with the helper, submit=auto, 8 compute workers (80 players, Valhalla -> Highground): Valhalla median 63 fps, p10 39,
min 21; Highground median 50, p10 28. In the slowest stretches (20-30 fps at 3,000-5,000 draws a frame) the game waits
0 ms for the helper and the helper is 50-75% busy: the limit is in the game process. PELOAD_SAMPLE in a 76-player Sandtrap
game: halo3's main thread (guest-0x18000a120) 99% busy every sample -- halo3 code 55-60%, the client's hooks 8-10%,
system 30-33% (host memmove ~20-22%, malloc internals); the render thread (guest-0x1800d1e9c) 39-49%, the GL submission
thread 36-41%, mcc-voice 99% (its key poll, own core). PELOAD_CALLCOUNT on that thread in gameplay, per second:
GetCurrentThreadId 887k, GetProcessHeap 662k, memset 444k, HeapAlloc/HeapFree ~400k each (a quarter of them via the
CRT's malloc), memcpy 423k, towlower 151k (3.8 M/s while loading), GetAsyncKeyState 94k, TlsGetValue 68k. The memmove
share is mostly halo3's per-frame ~14 MB game-state copies (copy-on-write path PELOAD_MEMCPY_COW, off since the map-change
corruption report; being re-tested with PELOAD_AB=cow). Every Win32 call costs the ms_abi->SysV transition under
Rosetta; the allocation churn costs malloc + free + clearing. Ideas: COW back on if clean, a per-thread small-block cache in
HeapAlloc/HeapFree, cheaper hot imports (GetCurrentThreadId / GetProcessHeap / TlsGetValue as tiny hand-written stubs).

## Where the time goes

| | per 2 s of wall time | share |
|---|---|---|
| Inside the loader's draw path (`d3dgl_draw`) | ~1,540 ms | ~77% |
| of which: the GL draw call itself | ~800 ms (2.9 µs/draw) | |
| of which: vertex setup / constant buffers / textures | ~170 / ~160 / ~280 ms | |
| Present | ~15 ms | <1% |
| Engine (everything else on the render thread) | ~450 ms | ~22% |

- About 1,400–1,700 draw calls per frame in Guardian, about 4,000 in a 60-player Big Team Battle (27 fps there).
- Total loader time per draw is about 5.7 µs. Before the program-bind cache (4afd0a9) it was 6.0 µs, and
  the draw call alone was 3.8 µs.

## Big Team Battle measurements (2026-10-17)

FKK BTB, 60 players, Sandtrap/Avalanche/High Ground, `PELOAD_D3DGL_PROF=1` (per 2 s; split = loader phases):

| | value |
|---|---|
| draws per frame | 3,000-4,700 (Guardian: ~1,400) |
| fps | 13-25 |
| split ms (typical) | prog 8-14, rt 17-38, pipe 9-21, cbs 110-270, **texs 430-1220**, vtx 60-140, draw 320-720 |
| texture re-uploads | 60-430 per 2 s, 2-430 ms (BC decode on the CPU + upload + glGenerateMipmap) |
| buffer-texture re-uploads | ~130 per 2 s (3 buffers, once per frame each), ~136 MB, ~15 ms |
| compute (particles) waits at bind | none |

Per-call counts (2026-10-17, `[prof] texture binds`, local Valhalla, ~2,200 draws/frame): per 2 s ~110k
glActiveTexture, ~120k glBindTexture, ~68k glBindSampler = ~300k calls in 540-590 ms, about 1.9 us per call -- the
game changes about one texture per draw. Trimmed: the active unit is cached (gs_active) and a slot whose texture is
unchanged only rebinds its sampler (glBindSampler names its unit; PELOAD_D3DGL_NOSPLITBIND = old). GL 4.1 on Apple has
no multi-bind (glBindTextures is 4.4) or DSA, so each texture change stays two driver calls.

So "texs" is the driver's cost for texture and sampler binds themselves, which grows with the number of distinct
materials on screen (60 players' armour, weapons, effects). Caching can't remove it (binds that repeat are already
skipped); fewer draws (the client's Advanced graphics settings) or a native driver (option 1) can.

## Why a draw costs so much: Apple's GL driver, under Rosetta

Microbenchmark of GL 4.1 core on Apple's OpenGL-on-Metal (µs per draw, offscreen 1080p target):

| state changed per draw | x86_64 under Rosetta | native arm64 |
|---|---|---|
| nothing | 0.61 | 0.13 |
| 6 uniform-buffer ranges | 0.86 | 0.47 |
| 4 texture binds | 1.43 | 0.87 |
| program switch | 2.39 | 1.54 |
| all of the above | 2.68 | 1.73 |
| redundant `glUseProgram(current)` | 2.09 | — |
| 4 redundant same-texture binds | 1.08 | — |

Takeaways:
1. Apple's GL framework runs as translated x86 code inside our Rosetta process, so every GL call costs
   1.5–4.7× its native cost.
2. The driver revalidates on any state call, even a redundant one: `glUseProgram(current)` costs nearly
   as much as a real program switch. That is fixed now (`gs_useprog`/`gs_pipeline`). Texture, sampler
   and uniform-buffer binds were already cached per unit/binding.
3. What remains is the driver's real per-draw cost for the state the game really changes. Caching can't
   remove it; only doing the GL work elsewhere can.

## Options

### 1. Native GL helper process (recommended big step; the driver is ~1.5× faster natively, glbench 2026-10-17)

Every GL call already goes through one function table (`G.*`, loaded with dlsym). Point those entries at
encoders that append opcodes and arguments to a shared-memory ring. A small native arm64 helper
process maps the ring, creates the GL context and replays the calls natively.

- Wins twice: the driver runs at native speed (about −35% per draw), and in parallel with the engine
  and the loader's own translation, which stay in the Rosetta process.
- Object names (`Gen*`/`Create*`): allocate client-side, map to real names in the helper.
- Data uploads (`BufferSubData`, `TexImage*`, `ShaderSource`): copy into the ring. Large uploads and the
  streaming ring (`MapBufferRange` UNSYNCHRONIZED) become shared-memory buffers.
- Calls with results (`GetUniformLocation`, `GetProgramiv`, `GetQueryObject*`, `ReadPixels`, fences):
  synchronous round trips. They happen only at link time, for queries, and for debug captures.
- Presentation: the helper renders into an IOSurface; the game window shows it through a CALayer
  (no copy). The present queue (`pq_*`) moves into the helper.
- Keep the in-process path behind a switch (e.g. `PELOAD_GL_HELPER=0`) for A/B.
- Effort: multiple days. The `G` table has ~150 entry points, so generate the encoders and decoders
  from one signature list.

#### Experiment 2026-10-17: shim/tools/glring (a two-process GL stream)

`bash shim/tools/glring.sh [draws] [frames] [textures/draw] [cbs/draw]`: glbench's draw stream (texture + sampler
binds, 256-byte uniform uploads, a program change every 4 draws, blend/depth toggles, indexed draws into a 1080p FBO),
encoded by an x86_64 producer (Rosetta, like the loader) into a 64 MB shared-memory ring and replayed by a native arm64
consumer that owns the GL context; up to 2 frames in flight. First numbers (1000 draws, 6 textures + 2 cbs per draw;
measured while a game ran, to be repeated on an idle machine):

| | us per draw |
|---|---|
| in-process x86_64 (today's loader) | 3.58 |
| in-process arm64 | 2.44 |
| ring: producer encoding (x86_64) | **0.18** |
| ring: consumer replay (arm64, Apple's driver) | 2.45 |
| ring: end to end, both in parallel | 2.72 (1.3x) |

What it says for the real helper: the loader's GL thread would spend ~0.2 us per draw on GL instead of ~3.6 (Apple's
driver under Rosetta), so its D3D11->GL translation (~2.5-3 us per draw in BTB) and the native driver (~2.4 us) run in
parallel instead of in sequence: in BTB-sized frames (~4,000 draws, ~6.3 us per draw today) the bound becomes the
larger of the two, roughly 2x. The ring and the encoding are cheap; the remaining work is breadth (every G entry point),
object names, the synchronous calls, buffer/texture data and presentation (below).

Plan, in steps that each stay usable behind PELOAD_GL_HELPER (0 = in-process, the default until it wins an in-game A/B):
1. Generate the encoder/decoder pair from one list of the G table's ~150 signatures (scalars inline; pointer arguments
   with a size rule: BufferSubData/TexImage*/UniformBlock data, ShaderSource strings).
2. Names: the loader allocates GL names itself (Gen*/Create* answered locally), the helper maps them.
3. Synchronous calls (GetUniformLocation, GetProgramiv, link status, query results, fences, ReadPixels): a round trip
   that waits for the helper; they happen at link time, for occlusion queries (already harvested in batches) and
   captures.
4. Streaming data: the loader's uniform ring and dynamic buffers become shared memory the helper maps as client storage,
   so constant uploads cost a memcpy, not a ring copy.
5. Presentation: the helper renders into an IOSurface, the game window shows it through a CALayer (no copy); the
   present queue (pq_*) moves into the helper.
6. In-game A/B (PELOAD_AB=helper:<secs>) in a 60-player BTB and a 100-player game.

Inventory of the loader's G table (100 entry points, D3DGLApi in peload.c) by how the helper carries them:

| Kind | Entry points | Transport |
|---|---|---|
| state and draws, scalar arguments (the hot path) | ActiveTexture BindTexture TexParameteri BindSampler SamplerParameteri/f BindFramebuffer BindVertexArray BindBuffer BindBufferRange VertexAttribPointer VertexAttribIPointer VertexAttribDivisor Enable/DisableVertexAttribArray UseProgram BindProgramPipeline UseProgramStages UniformBlockBinding Viewport Scissor Enable Disable Enablei Disablei BlendFunc BlendFuncSeparate(i) BlendEquationSeparate(i) BlendColor ColorMask(i) DepthMask DepthFunc CullFace FrontFace StencilFuncSeparate StencilOpSeparate StencilMask PrimitiveRestartIndex ClearColor ClearDepth ClearStencil Clear DrawArrays(Instanced) DrawElements(Instanced)(BaseVertex) BlitFramebuffer FramebufferTexture2D FramebufferRenderbuffer RenderbufferStorage ReadBuffer DrawBuffer BeginQuery EndQuery Flush | fixed-size records in the ring |
| small arrays | ProgramUniform4fv ProgramUniform1i SamplerParameterfv DrawBuffers | inline payload |
| names | GenTextures GenBuffers GenVertexArrays GenFramebuffers GenRenderbuffers GenQueries GenSamplers GenProgramPipelines CreateShader CreateProgram | allocated by the loader, mapped by the helper |
| data | TexImage2D BufferData BufferSubData TexBuffer ShaderSource MapBufferRange/UnmapBuffer | ring payload (small) or shared memory (uniform ring, dynamic buffers, big textures) |
| synchronous | GetError GetShaderiv GetShaderInfoLog GetProgramiv GetProgramInfoLog GetUniformLocation GetUniformBlockIndex CheckFramebufferStatus GetQueryObjectiv/ui64v ReadPixels Finish | round trip (link time, query harvest, captures) |
| setup | AttachShader DetachShader DeleteShader CompileShader LinkProgram ProgramParameteri | records; LinkProgram's status read back lazily |

#### The real helper (2026-10-17): PELOAD_GL_HELPER=1

Built as planned (shim/src/glh_client.c, glhelper.c, glh_shm.h; encoders/decoders generated from gl3.h by
shim/tools/glh_gen.py). How it fits in: peload.c redefines `dlsym` after the D3DGL globals, so every GL entry point the
D3D11->GL path resolves becomes a router that encodes the call into a 256 MB shared-memory ring while the render context
(a dummy in helper mode) is current, and calls the real function on every other context (present thread, the client's own
GL UI). Names come from pools the helper keeps filled; results come back by round trip only for link-time and Get* calls
(a program link asks for all its uniform locations and block indices in ONE round trip, GLH_X_LOCS); GetError and
CheckFramebufferStatus answer without one; occlusion-query results come back through a result table; the F9 depth readback
and other PBO reads are asynchronous (GLH_X_READBACK into a mailbox); frames come back as three global IOSurfaces the
present thread samples as rectangle textures. The helper wakes on a shared-memory os_sync wait (a wake pipe's write end
was closed under us by the game's handle traffic).

Clean-machine experiment (shim/tools/glring.sh, before the real helper; us per draw):

| | 1000 draws | 2000 | 4000 |
|---|---|---|---|
| in-process x86_64 (the loader today) | 2.9 | 2.9 | 3.0-3.2 |
| in-process arm64 | 2.0 | 2.0 | 2.0 |
| ring producer (loader side) | 0.15 | 0.14 | 0.12 |
| ring consumer (native driver) | 2.2 | 2.3 | 2.2-2.3 |
| end to end, pipelined | 2.3 | 2.3 | 2.3-2.45 |

In the game (the draw count follows the random spawn, so compare us per frame-draw):

| scene | in-process | helper |
|---|---|---|
| direct Guardian match, alone | 122 fps @ 1,379 draws (5.9 us/draw); 140 fps @ 1,245 (queries off) | 204 fps @ 1,883 (2.6 us/draw) |
| UK Hugegrass, 56 players, wide views (~9,500 draws/frame) | 11.7 fps (8.9 us/draw) | 17-19 fps (6.1 us/draw) |
| same server, ~1,800-2,000 draws/frame | -- | 22-43 fps |

What limits big games with the helper on (Hugegrass, PELOAD_D3DGL_PROF + PELOAD_SAMPLE + `sample` of the helper):
- the GL thread's own work fell to ~2.3 us per draw (in-draw 378 ms per 2 s at 82k draws/s); it now mostly waits;
- particle compute: with 4 workers the GL thread waited ~1.6 s per 2 s for compute jobs; PELOAD_CS_WORKERS=8 cut that to
  0.08-0.65 s (the M4 Max has 10 performance cores);
- the helper: 20-95% busy depending on the scene, ~19 GL calls per draw at ~0.6 us each natively; the GPU finishes a frame
  1-24 ms after its blit;
- the engine's render thread (Rosetta): ~30% of it is halo3 copying its game state every frame (already the host memmove);
- two client threads burn a core each in a modded game: the mods scan (stat/open/getdirentries non-stop; "Mods scan took
  7 s" again and again) and halo3's streaming thread (memmove + open). Worth a look on the file-system side.

Inside the helper (PELOAD_GLH_OPSTATS=1: calls and time per GL function every 4 s; direct Guardian, ~1,900 draws/frame):
19 GL calls per draw, ~2.7 us of helper time per draw, 90% busy. DrawElements 2.9 us each and DrawArrays 1.2 us = 70% of
it (the native driver's real per-draw cost); BlitFramebuffer ~270 us each, ~2.5 a frame (the engine's 1920x1080 frame copy
and the present blit; Apple's driver encodes a render pass's deferred draws when the pass ends at a blit, so that time is
largely draw cost); BindBuffer 6 a draw but 0.015 us each. Not worth it (in-session A/B, us per draw): Apple's
multithreaded GL engine in the helper (PELOAD_GLH_MP=1 / ab:<secs>: 5.80 vs 5.78) and the loader's GL state cache with the
helper (PELOAD_AB=glcache:30: 4.9 vs 5.1, noise). What would still help: fewer draws (the client's graphics settings), the
particle simulation natively, parallel command encoding (several contexts, much more work).

DONE: a 25-minute BTB soak (median 49 fps) and the play.sh default (the in-process path stays behind PELOAD_GL_HELPER=0). Next: move the CPU
particle compute into the helper (native arm64, ~1.5-2x) or onto the GPU; cut the link-time round trips further (the
link status itself is still one per program).

#### GPU particle compute (2026-10-17, experimental, OFF: PELOAD_CS_GPU=1)

macOS GL stops at 4.1 (Apple's own driver, not MCC: MCC is D3D11 only): no compute shaders, so halo3's 7 particle kernels
(numthreads(64,1,1), one RWStructuredBuffer u0) run on the CPU interpreter (dxbc_cpu.c). Five of them (update, collision,
init: 803-3923 instructions) only touch their own element u0[SV_DispatchThreadID.x + cbN[i].c] (cstf_analyze proves it from
the bytecode); with PELOAD_CS_GPU=1 those run on the GPU as transform-feedback vertex shaders (dxbc2glsl g_dg_tf_dw: one
vertex per thread, the element written back whole into the buffer's GL storage). The two 26-28 instruction spawn kernels
scatter to listed slots: they stay on the CPU and only the elements they wrote are sent (write log in dxbc_cpu.c).
Staging copies into a GPU-owned buffer go straight into its GL storage; any other CPU access reads it back (logged).

Correct now: PELOAD_CS_GPUCHECK=<n> runs every n-th dispatch on the CPU too and compares: 248,775 dwords exact, 311 within
float rounding (max rel 1e-6), 2 different of ~250k. Muzzle flashes, tracers and grenade explosions render. Three Apple
GL-on-Metal facts it took to get there (all reproduced natively in tiny CGL programs):
- glCopyBufferSubData is not implemented ("gldCopyBufferSubData: NEEDS IMPLEMENTATION", nothing is copied): the copy of the
  dispatched range is a transform-feedback pass too (one point per dword, read through the buffer's texture buffer).
- A transform-feedback write is NOT ordered before a later read of that buffer inside the same render pass (the game's
  open pass): the kernels read stale input and particles vanished. The passes run on their own 4x4 framebuffer
  (one render-pass break per run of dispatches, ~1 a frame).
- A draw without a complete framebuffer fails with GL_INVALID_FRAMEBUFFER_OPERATION even under RASTERIZER_DISCARD.

But slower: in-match A/B (PELOAD_AB=csgpu:20, local Valhalla, 26 dispatches/frame): GPU 100-116 fps vs CPU 151-161. No
read-backs or synchronous calls in the GPU phases; the game waits ~1.07 s per 2 s for the helper's frames (CPU phases
~0.2 s): Apple's driver spends ~60 us per transform-feedback pass (2 per dispatch). A BTB-size frame (~100 dispatches) would
lose more. Ideas left: fewer passes per dispatch (render the kernel into an RGBA32UI target and glReadPixels into the buffer
as a pack buffer), batching every dispatch of a frame behind one framebuffer switch, or Metal compute in the helper.

CPU interpreter bug found with it (fixed, default): a 4-component immediate l(a,b,c,d) was broadcast from lane x
("and r2.yz, r2.zzwz, l(0,65535,65535,0)" gave 0), so halo3's particle kernels unpacked their packed half-float fields to
zero. PELOAD_CS_IMMBCAST=1 = old.

#### Copy-on-write game-state copies (2026-10-17, default on)

PELOAD_MEMCPY_LOG=1 (big copies by caller every 5 s) showed one call site doing nearly all of them: halo3+0x13f23f copies
its ~14 MB game state about 5 times a frame (13 GB/s on Guardian alone; ~15% of the engine thread in big games), always
with source and destination the same distance apart modulo 16 KB. The host memmove stub now does such copies (>= 1 MB,
private memory on both sides -- never a shared mapping) copy-on-write for their whole pages with mach_vm_copy. In-session
A/B (PELOAD_AB=cow:15, direct Guardian with the helper, same scene): 243-250 vs 226-233 fps, and 194 vs 183-187 with the
private-memory check: +5-7%. PELOAD_MEMCPY_COW=0 = plain copies.

### 2. Submission thread inside the process (DONE 2026-10-17: +10-15% in 60-100 player games, -7% alone; `auto` mode)

Run the whole D3D11→GL translation on a GL thread; the engine's `ID3D11DeviceContext` calls only
enqueue. Overlaps the engine's ~22% with the loader and driver, but the driver stays translated.

- `Map(WRITE_DISCARD)` must rename buffers (fresh backing store per discard), `UpdateSubresource` data
  goes into the queue, and readbacks (`Map(READ)`, `GetData`) drain the queue.
- Superseded by option 1 if that is done; option 1 gets this overlap for free.

### 3. Smaller wins (a few % each)

- Program switches are the most expensive real state change; count them per frame (`PELOAD_D3DGL_PROF`
  could report program/texture/UBO changes), and see whether an identical-bytecode shader shares
  programs (`canon`) more aggressively across materials.
- `PrimitiveRestartIndex` is set on every strip draw; cache it. (Done in the opt-in GL cache, 2026-10-17: no fps change.)
- Uniform buffers: ~6 range binds per draw; one combined block per program would cut binds but needs
  translator support.

## Not the bottleneck (checked)

- `getenv` in the draw path: `PELOAD_*` lookups are cached (`peload_cgetenv`).
- Redundant VAO/framebuffer binds and attribute pointers: under 0.05 µs each in the benchmark.
- The hitbox overlay (`PELOAD_HITBOXES`, F2): one read of the object table plus one small read per
  object per frame, and one line batch. Compare with F2 in a busy match; the title shows the fps.
