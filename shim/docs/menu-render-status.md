# Where the render stands (2026-10-11, session 3 — null D3D11 + map loads)

## Session-3 part 3: GL window-size fix; failure modes consolidating

More fixes landed:
- **Window size consistency**: GetClientRect/GetWindowRect/GetSystemMetrics reported
  1920x1080 while the real GL drawable is 1280x720, so the engine sized its
  framebuffer/readback to 1920x1080 and glReadPixels read past the drawable ->
  GL-driver OOB (libGLImage). Fixed -> that OOB is GONE (0/12).
- **D3DReflect/D3DGetInputSignatureBlob** implemented (were unimplemented; the
  engine used a garbage reflection pointer) -> heap-corruption aborts down to ~2/12.
- D3D11 context Get* out-params now zeroed; exact per-kind GetDesc sizes.
- PELOAD_NO_FOCUS so background test runs don't steal the keyboard.
- Confirmed Guard Malloc does NOT work under Rosetta (can't use it to find overflows).
- Tried file mapping over mmap -> REVERTED (broke .map COW / named shared-mem IPC,
  raised corruption; halo3's ReadFile fallback is fine).

Current failure distribution (12 runs): glow `glDeleteProgram` panic ~7, heap
`free()` abort ~2, BTreeMap ~2, other ~1. **Still no presented frame.**

Decisive finding on the dominant glow panic: at the panic, the glow context pointer
is NULL (not just one fn-ptr field) — i.e. Reclaimer's egui_glow painter is in a
None/failed state when something calls delete_program on it. So this is a
GL-painter-setup correctness problem (likely a GL error, e.g. the observed
GL_INVALID_OPERATION 0x502, makes painter creation bail), NOT simple heap
corruption. Next: trace the egui_glow painter setup's GL calls and fix whatever
errors on our GL 4.1 Core context.

## Session-3 continued: the big race was thread-join; map load now reliable

Root cause of the #1 non-determinism FOUND and FIXED: `CreateThread` returned a
raw `pthread_t`, and `WaitForSingleObject` on it returned "signaled" instantly
(is_sync failed), so the engine's waits on its own parallel map-load worker
threads returned immediately and it raced the still-running workers. Thread
handles are now waitable SyncObjects signaled on exit. Also implemented
`_beginthreadex`/`Sleep`/`WaitForSingleObjectEx` (were no-op stubs that also
broke waits). **Result: all runs now reliably reach the Guardian map merge and
into post-merge shell setup**, and (under timing perturbation) the engine reaches
actual **GL rendering** in its shell callbacks.

Remaining, lower-frequency issues (still no presented frame):
- Residual heap corruption (tiny-block `free()` abort) during post-merge shell
  setup — reduced by exact D3D `GetDesc` sizes (buffer 24 / tex1d 32 / tex3d 36 /
  tex2d 44) and the thread fixes, but not gone. D3D guard pages show our own D3D
  buffers are NOT the overflow source, so it's likely a context `Get*` method
  leaving an out-param array uninitialized (engine then uses a garbage pointer) or
  another small caller-buffer write.
- A GL-driver OOB read (`libGLImage` `glgConvertTo_32` / `glReadPixels`) reached in
  Reclaimer's shell-callback GL path once past the race — a readback/upload with
  mismatched dimensions.

Still a NULL renderer: this makes the engine *run*; it does not draw halo3's game
video. Real video needs a D3D11->Metal/GL translation layer (next phase, large).

## Session-3 breakthrough: halo3 loads the game with a null D3D11

Built a minimal **null D3D11** (device/context/resources/views/swapchain, Map
returns real sized buffers, D3DCompile/Reflect stubbed). With it, halo3 now:
- creates its D3D11 device, buffers/textures/views, input layouts, shaders;
- **loads the full Guardian map** — "Merged 130_epilogue.map with the multiplayer
  characters from guardian ... 2052 tags" — and runs shell init.

It still does not present. The remaining blocker is a **timing-dependent data
race** in `halo3_shell_stage_5`: non-deterministic crashes (glow `glDeleteProgram
not loaded`, BTreeMap `unwrap None`, `free()` heap-corruption abort). Proof it's a
race not a static overflow: slowing execution (MallocCheckHeap) advances materially
further; our D3D guard pages + Guard Malloc show no buffer overflow of our own
allocations.

Concurrency bugs fixed this session (all real, all kept):
- **Interlocked SList** (halo3's lock-free pools) was non-atomic -> now mutex-serialized.
- **TLS** (TlsAlloc/Get/SetValue) used a shared global array -> now per-thread in the TEB.
- Many **D3D resource-sizing heap overflows** (format-aware texture footprint,
  GetDesc per-type size, Map DepthPitch = one slice, backbuffer size).
- HeapSize returned 0 -> malloc_size.

At least one more concurrency issue remains (the race still reproduces). Candidates
to investigate next: SuspendThread/ResumeThread are no-ops (if the engine uses them
to coordinate/safepoint threads, that races); a genuine engine race exposed by
macOS/Rosetta thread scheduling (SetThreadPriority is a no-op); or a subtle issue
in a shared-memory structure between halo3 (C++) and Reclaimer (Rust) via the shell
callbacks. Repro: run several times; ~1/3 hit each failure mode. Env `PELOAD_D3D_GUARD`
guards D3D buffers; `PELOAD_SURVIVE` parks faulted non-main threads.

---

# Where the menu render stood (2026-10-11, session 2 — post-SEH)

## TL;DR

Halo 3's real MCC engine runs on Apple Silicon with no Wine. Since the last
update we implemented **Windows SEH / exception unwinding**, which was the stated
blocker, plus hardware-fault dispatch, a thread-exit abort fix, and several
audio/D3D effect shims. The native client worker now boots further than ever:
`engine_initialized_flag:1`, `network_lifecycle_flag:1`, reaches
`halo3_shell_stage_5` (menu/shell setup). It still does not draw a frame.

The remaining blockers are now **in Reclaimer's own probe-worker logic and
halo3's D3D11 renderer**, not in the loader primitives.

## What was implemented this session (all in shim/src/peload.c, committed)

### 1. Windows x64 SEH / exception unwinding  (the requested piece)
- `RtlLookupFunctionEntry` (.pdata binary search per module),
  `RtlVirtualUnwind` (full UNWIND_INFO interpreter incl. CHAININFO),
  `RtlUnwindEx`, a dispatching `RaiseException`, `RtlRestoreContext`.
- `seh_capture`/`seh_restore` module asm (CONTEXT <-> registers).
- **Hardware-fault dispatch**: `crashsig` converts Unix SIGSEGV/SIGILL/SIGBUS/
  SIGFPE/SIGTRAP into a Windows EXCEPTION_RECORD+CONTEXT and runs it through the
  guest's **vectored exception handlers** (and frame .pdata handlers);
  EXCEPTION_CONTINUE_EXECUTION resumes via the signal ucontext.
- Result: Rust `panic=unwind` now unwinds to `catch_unwind` (clean exit 1 instead
  of SIGILL/terminate), and the engine's own "Experimental startup exception"
  VEH now fires and prints real **unwound call chains**.

### 2. Thread-exit TSD abort  (was: malloc_report -> abort at _pthread_tsd_cleanup)
macOS x86_64 keeps the pthread TSD in `%gs`; Windows TEB/TLS/PEB map to
`%gs:0x30/0x58/0x60`. Slot `0x60` has a pthread destructor that `free()`s the
slot value at thread exit, and we'd stored the **static** PEB pointer there ->
abort. Fix: zero **only** `gs:0x60` before a worker thread returns (cleanup skips
NULL slots). We deliberately keep `gs:0x58` (TLS array) and `gs:0x30` (TEB)
set and **leak** the per-thread TEB/tarr — zeroing 0x58 opened a window where
late thread-local destructors faulted on NULL TLS, and freeing the TEB risked a
use-after-free during cleanup. (Wine 10.5 instead swaps the gs base on every
Windows<->native transition — the fully-correct fix if the leak/edge cases bite.)

### 3. Audio / D3D effect shims (engine called NULL fn-ptrs or used NULL out-params)
- **WavesFX**: halo3.dll builds a table of `WavesFX_*` fn-ptrs (globals at RVA
  0x46bb780..0x46bb7f0) that it would fill by LoadLibrary+GetProcAddress of Waves'
  DLL. That loader path never runs here, so the ptrs stay NULL and the engine
  calls `WavesFX_Initialize`(NULL). We pre-fill the table in `load_guest` when
  halo3 maps: Initialize returns a dummy ctx, the rest are no-ops.
- **xaudio2_9redist** ordinals 2/3/4 = `CreateAudioReverb`/`CreateAudioVolumeMeter`/
  `CreateFX` -> a real (silent) IXAPO COM object with a safe vtable (incl.
  GetRegistrationProperties). (ord 1 XAudio2Create was already handled.)
- **D3DCompile** (d3dcompiler_47): halo3 compiles its video-display HLSL
  (CopyToScreen / YUVToRGB) to DXBC blobs and reads GetBufferPointer/Size. We
  return a valid ID3DBlob wrapping a placeholder DXBC buffer (no device consumes
  the bytecode on this path).

## The orchestration architecture (mapped this session)

Running `peload project-reclaimer.exe` (no args) runs the **orchestrator**. It
`CreateProcessW`-spawns ONE child: `probe-engine-worker <halo3.dll>
--guardian-slayer --client-window --startup --skip-render-warmup --shell-callbacks
--native-udp`. That subcommand's 6 flags are each `required(true)` (clap), so the
probe is hardwired to this exact set — you cannot drop/alter a flag (clap exits 2).

The flag meanings (from the exe's own `--help` text):
- `guardian_slayer`: try Guardian/Slayer, **suppressing the worker's menu fallback**
- `client_window`: our own window + native rendering, **without the MCC shell**
- `startup`: test native startup (may allocate 2 GiB), `skip_render_warmup`,
  `shell_callbacks`, `native_udp`: as named.
- `real_client`: "accept one launcher-provisioned real client over loopback" — the
  actual client path, NOT launched in the standalone orchestrator flow.

So the standalone flow ONLY runs the guardian-slayer probe, which tries to load a
Guardian map / Slayer match — needing halo3's **D3D11** renderer we don't have —
and explicitly SKIPS the main-menu fallback ("Skipped main-menu-only fallback in
disposable worker memory (RVA 0x12FB1C)"). The probe reaches `halo3_shell_stage_5`
with a VISIBLE window + menu artwork loaded, then faults before presenting
(`ready_presents:0`, no glDrawElements/SwapBuffers ever called — nothing is drawn
to show yet).

The orchestrator does NOT launch a second (real-client) process whether the probe
succeeds or fails (verified: one CreateProcessW per run). So "the menu" in this
flow would come only from the **menu fallback** (suppressed by guardian_slayer) or
the **real-client** path (needs launcher provisioning we don't replicate).

### Experiment hooks added (env-gated, in i_CreateProcessW / i_ExitProcess / crashsig)
- `PELOAD_WORKER_DROP="--flag,..."` / `PELOAD_WORKER_ADD="..."`: rewrite the
  spawned worker's args. (Dropping any required flag -> clap exit 2.)
- `PELOAD_PROBE_EXIT0=1`: force the probe worker to report success. (The worker
  often exits via a path we don't hook — RtlExitUserProcess/Nt* — so it doesn't
  always fire; and the orchestrator launches no real client regardless.)
- `PELOAD_SURVIVE=1`: park a faulted non-main thread instead of killing the proc.

### Paths forward to a VISIBLE menu (all substantial)
1. **Force the menu fallback**: patch the exe/engine branch that skips the
   main-menu-only fallback at worker-RVA 0x12FB1C (guardian_slayer gate). The log
   string is `project-reclaimer.exe` VA 0x141b55e54; it's referenced via a Rust
   data table (no direct code xref), so locate the branch by its data-table entry
   or by tracing the guardian_slayer bool. If the menu fallback renders via
   Reclaimer's own GL (egui) it would NOT need D3D11.
2. **Implement enough D3D11** for halo3's compositor/present (device + context +
   the handful of shaders/draw/Present the shell uses). Large (DXVK/wined3d scope).
3. **Drive the real-client path** (`--real-client`) with loopback provisioning the
   launcher normally supplies. Needs RE of the provisioning handshake.

## IMPORTANT: the menu is OpenGL, not D3D11 (projectreclaimer.dev/play)

The official docs state: "Graphics: A driver supporting **OpenGL 3.0 or later, which
the menus use**. The docs don't mention DirectX requirements." Troubleshooting:
"Menus don't appear (OpenGL) — update your graphics driver; the menus need OpenGL
3.0 or later." So **the menu renders via OpenGL** (Reclaimer's own egui/glow),
which we fully support (GL 4.1). halo3's D3D11 is only for in-game 3D. My earlier
"menu needs D3D11" framing was WRONG. The probe worker IS the native OpenGL menu
renderer ("Initialized standalone Rust window/native graphics contract; no MCC
executable launched"). Normal usage per docs is just: run the exe (Steam/MCC
present) → borderless-fullscreen OpenGL menu. Our no-arg run IS that path; the
orchestrator's probe-engine-worker is how this version does native GL startup.

So the real blocker is the **non-deterministic crash during GL menu setup**, not
D3D11.

## Ruled out as the corruption source (verified this session)
- **%gs TEB/TSD collision**: DISPROVEN by a standalone Rosetta test — slots
  0x30/0x58/0x60 read 0 in a fresh thread, macOS never touches them, and our
  writes SURVIVE malloc/snprintf/errno-setting native calls with errno still
  working. (The only real issue was slot 0x60's exit destructor, already fixed.)
- **TLS block too small**: our static TLS block is 0x49b0; the observed access at
  +0x4908 is within it. Not an overflow.
- **No-op locks**: critical sections + SRW locks are real pthread-backed with
  race-safe lazy init; WaitOnAddress/WakeByAddress is a correct futex (bucket
  mutex serializes check-and-wait against wake — no lost wakeup). Rust Mutex
  (futex-based on Windows) is therefore correctly backed.
- The exe (Reclaimer) only creates threads via **CreateThread** (routed through
  thread_tramp, which installs a TEB), not _beginthreadex.

## The decisive constraint: native GL window is coupled to a game load

The exe's own experiment-validation rules (strings in .rdata):
```
Empty-match experiment requires --guardian-slayer
Guardian experiment requires --native-udp and its startup prerequisites
Native experiments require --startup
Real-client experiment requires empty match and excludes synthetic admission
Standalone client window requires Guardian and excludes headless experiments
```
So in THIS build, `--client-window` (native OpenGL window, no MCC shell)
**requires `--guardian-slayer`** — the native-GL path is inseparable from loading
a Guardian/Slayer match, which drives halo3's **D3D11** renderer. There is no
experimental "native GL window that only shows the menu" combination here.

Consequence for the corruption below: halo3's D3D11 game-load paths run, fed our
FAKE device/APO/D3DCompile data, and corrupt the shared heap (glow fn-table +
BTreeMap). Faking D3D success → halo3 proceeds with garbage → corruption; faking
D3D failure → halo3 null-derefs (engine doesn't check). Neither is clean because
halo3 genuinely needs a working D3D11. So in this build, a visible native-GL menu
is effectively gated on **implementing D3D11** for the coupled game load — or on a
future Reclaimer build that decouples native GL rendering from the game load, or
the launcher-provisioned `--real-client` path.

## The remaining blocker: non-deterministic corruption in GL menu setup
Runs end variously (glow `glDeleteProgram not loaded`, BTreeMap `unwrap None`,
halo3 thread EXC_BAD_ACCESS). The glow panic is a glow Context whose function
table has `DeleteProgram == None` even though our loader resolves it non-NULL and
only ONE GL context is ever created — i.e. the glow function table (and the
BTreeMap) are being **corrupted in memory** at runtime. Root not yet found by code
inspection. Next: catch it at runtime — lldb hardware watchpoint on the glow
Context's DeleteProgram slot once populated, or a controlled single-threaded run,
or guard-malloc (slow/large under Rosetta). This is a GL-path memory-corruption
bug, independent of D3D11.

## Current blockers (in priority order)

### A. Reclaimer probe-worker uses a stub/headless glow context
In `probe-engine-worker` mode the worker panics on the **main thread**:
`glow-0.16.0/src/gl46.rs:4515: called glDeleteProgram but it was not loaded`.
Our GL loader DOES resolve glDeleteProgram non-NULL (verified: dlsym returns it,
make_gl_thunk wraps it; PELOAD_TRACE_GLRES shows one resolution at the real GL
context). The panicking glow Context is a DIFFERENT one that never asked our
loader (glDeleteProgram is resolved exactly once in the whole run) — i.e.
Reclaimer builds a null/stub glow context in probe mode and later calls
`delete_program` on it. This is Reclaimer probe-mode logic triggered by our
environment, now surfaced cleanly because SEH lets the panic propagate.
Next: understand the probe-vs-client-render architecture (does the MAIN process
render after a successful probe? can the probe be made to "succeed"/exit 0?).

### B. halo3 D3D11 native renderer + swarm of worker-thread faults
halo3.dll is a full D3D11 engine. During shell setup it spawns many threads and
compiles/creates D3D shaders. We have no D3D11 device (D3D11CreateDevice returns
UNSUPPORTED), so paths that need one fault. We faked the shader *compile* (blob)
but a real device/context/draw path is a large missing piece (DXVK/wined3d
territory). Intermittent EXC_BAD_ACCESS on halo3 worker threads.
`PELOAD_SURVIVE=1` parks a faulted non-main thread instead of killing the process
(capture aid; can deadlock).

### C. BTreeMap navigate `Option::unwrap() on None` (menu setup, main thread)
A genuine Rust panic in menu setup; now cleanly caught (exit 1). May be a symptom
of (A)/(B) or TSD corruption rather than an independent bug.

## Non-determinism
Runs end variously: glow glDeleteProgram panic (exit 1), BTreeMap panic (exit 1),
halo3 D3D thread EXC_BAD_ACCESS (139/0xc0000005). Signature of concurrent
thread/corruption effects. The `%gs` TEB/TSD sharing (see #2) is the prime
suspect for residual corruption; the Wine-10.5 gs-base-swap is the robust fix.

## How to reproduce / continue
```
cd ~/Desktop/HaloReclaimerMac
MCC="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
( cd "$MCC" && PELOAD_WINDOW=1 PELOAD_SHOT=1 PELOAD_WORKER_INHERIT_STDIO=1 RUST_BACKTRACE=full \
  arch -x86_64 ~/Desktop/HaloReclaimerMac/shim/build/peload \
  ~/Desktop/HaloReclaimerMac/analysis/project-reclaimer-v0.9.13.exe )
```
Env flags: `PELOAD_WINDOW` (real NSWindow), `PELOAD_SHOT` (capture frames to
`~/reclaimer_shot_*.bmp`), `PELOAD_WORKER_INHERIT_STDIO` (see worker stderr — the
crash happens in the CreateProcessW-spawned probe worker), `PELOAD_TRACE_SEH`,
`PELOAD_TRACE_GLRES` (GL name resolution), `PELOAD_TRACE_THREAD` (thread_tramp),
`PELOAD_SURVIVE` (park faulted non-main threads), `PELOAD_WAIT=<s>` (worker sleeps
so lldb can attach; writes pid to /tmp/peload.pid).

Key addresses (this build): halo3.dll base 0x180000000, exe base 0x140000000.
WavesFX table RVA 0x46bb780..0x46bb7f0. Video-shader compile at halo3+0x2a0990.
