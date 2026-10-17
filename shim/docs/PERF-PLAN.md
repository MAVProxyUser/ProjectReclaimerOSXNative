# Frame-rate plan

Status 2026-10-16. Measurements are from a direct Guardian match on an M4 Max (`PELOAD_D3DGL_PROF=1`,
2-second windows) unless noted.

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

### 1. Native GL helper process (recommended big step; estimated up to ~2× fps)

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

### 2. Submission thread inside the process (estimated +25–30%)

Run the whole D3D11→GL translation on a GL thread; the engine's `ID3D11DeviceContext` calls only
enqueue. Overlaps the engine's ~22% with the loader and driver, but the driver stays translated.

- `Map(WRITE_DISCARD)` must rename buffers (fresh backing store per discard), `UpdateSubresource` data
  goes into the queue, and readbacks (`Map(READ)`, `GetData`) drain the queue.
- Superseded by option 1 if that is done; option 1 gets this overlap for free.

### 3. Smaller wins (a few % each)

- Program switches are the most expensive real state change; count them per frame (`PELOAD_D3DGL_PROF`
  could report program/texture/UBO changes), and see whether an identical-bytecode shader shares
  programs (`canon`) more aggressively across materials.
- `PrimitiveRestartIndex` is set on every strip draw; cache it.
- Uniform buffers: ~6 range binds per draw; one combined block per program would cut binds but needs
  translator support.

## Not the bottleneck (checked)

- `getenv` in the draw path: `PELOAD_*` lookups are cached (`peload_cgetenv`).
- Redundant VAO/framebuffer binds and attribute pointers: under 0.05 µs each in the benchmark.
- The hitbox overlay (`PELOAD_HITBOXES`, F2): one read of the object table plus one small read per
  object per frame, and one line batch. Compare with F2 in a busy match; the title shows the fps.
