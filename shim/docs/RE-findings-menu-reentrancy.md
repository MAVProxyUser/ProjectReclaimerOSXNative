# Reclaimer menu re-entrancy — RE findings (session 5)

> **Historical log (sessions 5-11, 2026-10-11 .. 2026-10-14), kept as written; read it for method, not status.**
> Current state (2026-10-18): CLAUDE.md, `shim/docs/BACKLOG.md`, `PERF-PLAN.md`; every address the loader touches
> today, with signatures, is in `patch-addresses.md` (client v0.9.18). Exe RVAs below are **v0.9.13**
> (`analysis/project-reclaimer-v0.9.13.exe`, also what `analysis/reclaimer-exe-symmap.tsv` maps); every one moved in
> later releases. Everything left open here was solved later: the re-entrancy fix is the `ui-reentrancy` signature
> patch (peload.c `g_sigpatches`, on by default); the map catalog chain was resolved within this log (session 8, by
> closing the remaining stubs), and play.sh now runs Reclaimer's real menu runtime and joins online games; the black
> frame of session 11 was the loader's `D3DReflect` stub reporting no constant buffers, so halo3's constant registry
> (FUNC halo3+0x2af4b8) bound nothing, and a real RDEF reflector fixed it (the "material bind path never runs"
> explanation there was a symptom). Corrections: software breakpoints (`PELOAD_BP`) do work (note below), and
> halo3+0x2ac24c is the input-layout builder, not an armor-skin registry (session 10).

Deep reverse-engineering of the single remaining blocker to a visible menu: a
`RefCell already borrowed` panic in the custom OpenGL frontend. This documents
exactly what it is, what's been ruled out, and the concrete next levers — so the
next session continues without re-deriving.

## Tooling (reusable — use these for ALL future crashes)
- `analysis/reclaimer-exe-symmap.tsv` — 70,059 exe functions -> source files,
  mined from embedded Rust panic `Location` structs (no PDB ships).
- `tools/build_symmap.py <pe> <imgbase>` — regenerate the map.
- `tools/sym.py <rva> [...]` — resolve exe RVAs to function + source.
- `tools/pe.py` — library: `PE('exe'|'halo3').dis(rva,len)` (disasm with symbol +
  Location resolution), `.sym(rva)`, `.loc(rva)`, `.iat_name(slot)`.
- halo3.dll has NO symbols (PDB-less, no Location structs) — only the exe resolves.

## Environment limitation discovered
- **`int3` software breakpoints DO NOT WORK under Rosetta** — SIGTRAP from `0xCC`
  is never delivered to our handler (verified even at the entry point). So dynamic
  guest-code tracing via breakpoints is out. SIGSEGV (guard pages) DOES work, and
  guest-code JMP-trampoline patches work (PELOAD_PATCH). `PELOAD_BP` is kept but
  is a no-op here.
  **Correction (2026-10-14):** software breakpoints do work under Rosetta once
  the loader catches SIGTRAP (peload.c installs the handler when `PELOAD_BP` is
  set): the one-shot breakpoints pinned the dedicated server's spawn failure, and
  `PELOAD_BP_ARGS=<n>` logs rcx/rdx/r8/r9. One Rosetta-translated network
  thread did crash on them once; `PELOAD_HOOK` detours are the safe fallback.

## The panic — precisely located
- Function: `exe+0x550e00` (28 KB) = the frontend UI draw. Sources:
  `ui_composition.rs`, `engine_forge_outline.rs`, `exclusive_fullscreen.rs`,
  `loading_reveal.rs`, `client_settings.rs`, `menu_backdrop.rs`.
- It is halo3's `vtable+8` shell callback (halo3 calls it at `halo3+0x2741f8`,
  object at halo3 global RVA `0x9fcab8`).
- The panicking borrow: `0x553ad9 cmp qword[rbx],0 ; 0x553add jne 0x553dff`
  (-> `0x553e06 call panic`, LOC std thread/local.rs:757). `rbx` = a thread_local
  RefCell field; `[rbx]` must be 0 (free) to borrow, it is non-zero (held) -> panic.
- The thread_local is accessed via `gs:[0x58]` (TEB TLS array) + index static
  `exe RVA 0x22a4938`. The app-state struct has MANY RefCell sub-fields
  (seen offsets +8, +0x4518, +0x4728, +0x4910 ...), each with its own lazy-init
  state byte (field+0x68). `ui_composition` is the only function that drives the
  full `gs:[0x58]` load for these.

## What it IS (established)
- A **nested RefCell borrow within ONE `ui_composition` invocation** — verified
  `ui_composition` appears exactly ONCE on the panic thread's stack (scanned 64K
  stack slots, 109 total guest frames). It is NOT two stacked ui_composition calls.
- Runs on a **halo3 worker thread** (`main=0`, thread root `halo3+0xa120`), which
  has its OWN TLS array (18 threads = 18 distinct `gs:0x58` — NOT a TLS-sharing bug).
- **During lazy init**: bypassing the borrow panic (PELOAD_PATCH `exe+553add` jne
  -> 6x NOP) removes the panic but then faults at `0x553afe`
  (`call [rax+0x10]` on a sub-object at `[rbx+0x18]` that is null) — i.e. the
  app-state is MID-INITIALIZATION and its sub-objects aren't populated yet when the
  nested call tries to use them.

## Ruled OUT
- **D3D11** is not the blocker: exactly ONE D3D11 call (CreateTexture2D), ZERO
  Draw/Present before the panic (PELOAD_TRACE_D3D). Menu is pure GL
  (glutin+winit+egui-0.33+glow). D3D11->GL only matters for later in-game rendering.
- **Catching the panic**: unreachable — the frontend's `catch_unwind` is past the
  thread root `halo3+0xa209`; the local C++ frame is a Drop/rethrow landing pad
  (continuation -> ud2). (SEH unwind reaches the catch frame correctly now, but the
  real catch isn't on this thread.)
- **TLS sharing**: each thread has its own TLS array (verified).
- **Shell-callback NOP / re-entrancy guard**: the nested call is REQUIRED (skipping
  it => "Unmapped shell callback; stopping", exit 3). Dropping `--shell-callbacks`/
  `--guardian-slayer`/`--client-window` worker flags doesn't change it.

## Leading hypothesis + next levers
halo3 drives the UI draw on a worker thread while the app-state is still lazily
initializing (the outer init holds a borrow), then re-enters it synchronously via
`vtable+8` -> nested borrow -> panic. On Windows this presumably runs on the main
thread where the app-state is already initialized, or the shell callback is
deferred rather than synchronous. Concrete next levers:
1. **Threading**: determine whether `vtable+8` should run on the MAIN thread. The
   app-state may be fully initialized on main; marshaling the shell callback (or
   making halo3's render thread not call it until init completes) could resolve it.
   halo3 global `0x9fcab8` holds the callback object; the thread root is
   `halo3+0xa120`.
2. **Dynamic trace without int3**: build a JMP-trampoline (PELOAD_PATCH-style, works
   under Rosetta) at the borrow site to log who holds the outer borrow and the
   init-state sequence. Or a SIGSEGV guard-page watchpoint on the app-state field.
3. **Reclaimer source** (fastest): `project_reclaimer` is a Rust project; the
   `ui_composition.rs` borrow scope + shell-callback registration would pinpoint it.

## Session-5 commits of record
GL-thunk ABI root-cause fix (3da9e9c) through this doc; symbol map + tools
(dff054c, f572e31); D3D11 tracing + FindFirstFileA (fabd4b7).

---

## UPDATE (session 5 cont.) — ROOT CAUSE FOUND: D3D11/DXGI query failure leaks the borrow

Built a working **JMP-trampoline detour tracer** (PELOAD_HOOK, works under Rosetta
unlike int3): `PELOAD_HOOK=<mod>+<rva>:<origlen>:<id>` logs id + guest regs, runs
the relocated bytes, jumps back. origlen must cover whole non-rip/non-rel insns.

Using it, the re-entrancy is now understood:
- `ui_composition` (exe+0x550e00) is called TWICE from the same caller frame (same
  entry rbp). The FIRST call borrows the app-state RefCell at `exe+0x551038/0x551043`
  (field = TLS_block+8), does work, and LEAKS the borrow (never releases [r12]->0).
  The SECOND call then hits `0x553ad9 cmp[rbx],0` with the field still -1 -> panic.
- WHY it leaks: inside that borrow scope `ui_composition` makes D3D11/DXGI interface
  calls and on failure takes a path that skips the release:
  - `0x551071 call [rax+0x38]` = `IDXGISwapChain::GetDevice(riid, **)` with
    riid = **ID3D11Device** `{DB6F6DDB-AC77-4E88-8253-819DF9BBF140}`.
  - `0x5510a2 call [rax]` = device `QueryInterface(riid, **)` with
    riid = **IDXGIDevice** `{54EC77FA-1377-44E6-8C32-88FD5F44C84C}`.
  - `0x5520aa call [rax+0x138]` on `[r12+0x20]` (normal loop path).
  Our swapchain vtable slot 7 (GetDevice) was `ctx_ret0` -> returned S_OK but left
  the out pointer NULL -> caller's error path -> leaked borrow.

**So D3D11 DOES matter for the menu** — not for drawing, but because the frontend
queries the D3D11 device/DXGI device during UI setup; a failed query leaks a
RefCell borrow and surfaces as the "already borrowed" re-entrancy panic.

### Fix in progress
- Implemented `IDXGISwapChain::GetDevice` (swapchain vtable slot 7 -> `swap_GetDevice`
  returns `&g_d3ddevice`). Necessary but NOT sufficient alone — the panic persists,
  so at least one more call in that scope still fails. Next: use PELOAD_HOOK to log
  the RETURN VALUE (eax) of each interface call in the 0x551043 scope (hook a safe
  point after each call) and make each one return success with a valid out pointer.
  Candidate methods to complete: the DXGI device QI (IDXGIDevice), and the method at
  `[r12+0x20] vtable +0x138`. Also verify `[r12+0x20]`'s object type.
- Tracer caveat: single-step re-arm / int3 DON'T work under Rosetta (see the 2026-10-14 correction at the top:
  int3 does, with a SIGTRAP handler); the detour (PELOAD_HOOK) is the working mechanism. Hook only sites whose first >=13 bytes are
  whole, non-rip-relative, non-rel-branch instructions.

---

## UPDATE 2 — refined model + where it stands

Using the detour tracer (PELOAD_HOOK), the precise model is now:
- halo3 calls `ui_composition` (vtable+8) TWICE from the SAME site `halo3+0x2741fb`
  at the SAME stack slot (sequential calls, NOT nested).
- The app-state RefCell field is `TLS_block+8`. It is borrowed at `exe+0x551043`
  (check `0x551038 cmp[r12],0; jne 0x551355`) and released ONLY at `exe+0x5524b6`
  (`inc qword[r12]`). It is also borrowed again later at `exe+0x553ad9` (reached via
  the region at `0x553757`).
- The panic: on a path, execution goes from the `0x551043` borrow to the `0x553ad9`
  borrow of the SAME field WITHOUT passing the `0x5524b6` release -> nested borrow
  -> "RefCell already borrowed". The normal loop path (`0x5520aa call [rax+0x138]`)
  is the one taken and it SUCCEEDS; the D3D11 `GetDevice`/`IDXGIDevice` queries are
  in the NOT-taken edge path (`0x551058+`).
- The region `0x553757` (2nd borrow) is gated by a global flag `0x228bbe9` and a
  `Once`-style guard `exe+0x617900` (lazy init), not a direct D3D query.

So the trigger is a branch in `ui_composition`'s normal path that, in our
environment's state, routes to the `0x553757`/`0x553ad9` nested borrow instead of
the release. D3D11/DXGI state still plausibly influences it (the queried interfaces
appear in the same function, and `swap_GetDevice` was a genuine missing piece), but
the EXACT routing condition is not yet pinned.

### Concrete next steps (in priority order)
1. **Reclaimer source** — `ui_composition.rs` + the vtable+8 shell-callback
   registration makes this a one-line borrow-scope fix. By far the fastest.
2. **Tracer rip/rel-relative relocation** — enhance `hook_one` to fix up
   rip-relative mem operands and rel8/rel32 branches when relocating, and allocate
   `g_hookpool` near the exe (hint ~0x142000000) so fixup disps fit in int32. Then
   hook the rip-dense release (`0x5524b6`) and 2nd-borrow (`0x553ad9`) sites to see,
   per call, whether the release fires before the nested borrow, and which branch
   routes to `0x553757`.
3. **Finish D3D11/DXGI** — proper per-IID `QueryInterface` (distinct objects for
   ID3D11Device / ID3D11DeviceContext / IDXGIDevice / IDXGISwapChain) + a real
   IDXGIDevice vtable (GetAdapter, etc.). Needed regardless for in-game rendering;
   may also fix the routing if it's a device-capability query.

### Tools delivered this session (all committed)
analysis/reclaimer-exe-symmap.tsv (70k funcs), tools/build_symmap.py, tools/sym.py,
tools/pe.py (disassembler), PELOAD_HOOK detour tracer, PELOAD_TRACE_D3D, PELOAD_PATCH,
PELOAD_TRACE_STACK, PELOAD_TRACE_SEH; plus the swap_GetDevice D3D11 fix.

---

## SESSION 6 — RE-ENTRANCY SOLVED (auto-patched) + halo3 TLS fixed

The re-entrancy is fixed without Reclaimer source. Mechanism pinned via static RE:
- `ui_composition` (exe+0x550e00) write-borrows the app-state thread_local RefCell
  at `0x551043` (field = TLS_block+8). On the FIRST call, the timer check
  (`0x55212a cmp [r12+0x30],0`) forces the heavy init path (`0x55213b`), which
  reaches a once-per-second **cache cleanup** at `0x553ad9` that write-borrows the
  SAME cell again — WITHOUT the outer release (`0x5524b6 inc [r12]`) having run.
  => "RefCell already borrowed" (thread/local.rs:757) -> rust_panic -> ud2 `0x557d3a`.
- The cleanup body (`0x553ad9`): `cmp[rbx],0; jne 0x553dff(panic)`; borrow;
  `cmp dword[rbx+0x10],1e9; jne` -> drop 8 cached sub-objects `[rbx+0x18..0x50]`
  (ALL NULL this early) then `inc[rbx]` (release). The `je` no-drop path
  (`0x553af3 xor eax,eax; jmp 0x553b4d`) sets `[rbx]=0` (free) and skips the drops.
- FIX (`reentrancy_fix()` in peload.c, auto at exe entry; `PELOAD_NO_REENTRANCY_FIX`
  to disable): NOP the panic branch `0x553add` (6 bytes) and the drop-dispatch
  branch `0x553af1` (2 bytes). The cleanup becomes a safe no-op that leaves the
  RefCell free. No panic; outer scope's logical borrow is already done by here.

After this, the next crash was a halo3 thread-local aliasing fault (halo3+0xa50ae),
fixed by multi-module static TLS (register_tls / per-index thread_tramp blocks;
halo3 gets its own TLS index). See memory `reclaimer-mac-goal` session-6 block.

Engine now reaches: native swap-chain present path, real window + GL menu renderer,
"Client screens now drawn into the game's frame". Remaining: the `--guardian-slayer`
probe worker's map-catalog name resolve returns '' for guardian (files read fine) ->
exit 3. engine_catalog.rs = exe+0x1a1ce10.

---

## SESSION 6 (cont.) — ARCHITECTURE MAP + the map-load gate

Confirmed from projectreclaimer.dev/play + behavior:
- The menu renders via **OpenGL** (Reclaimer's own glutin/winit/egui frontend). halo3's
  D3D is only for actual in-GAME rendering, not the menu. So our GL bridge is the right path.
- A normal launch opens the menu directly (borderless fullscreen; Windowed via Settings).
- The single exe ALWAYS spawns a `probe-engine-worker --guardian-slayer --client-window
  --startup --skip-render-warmup --shell-callbacks --native-udp` child as an unconditional
  startup self-test (NOT recovery-only; clearing the `ReclaimerProfile/starting` marker
  doesn't change it). `--guardian-slayer` is clap-REQUIRED for that subcommand and it
  "suppresses the worker's menu fallback"; `--client-window` "requires Guardian". Dropping
  either fails ("Local runtime role does not match native startup mode" / clap error).

Flow in our runs: parent launches -> spawns probe worker -> waits. The worker (what we see
via PELOAD_WORKER_INHERIT_STDIO, logging to STDERR) runs the guardian probe and FAILS at the
map-catalog resolve -> parent prints "probe worker failed: exit code 3" and exits. PROBE_EXIT0
(fake worker success) makes the parent exit cleanly WITHOUT rendering — so the parent needs the
worker's genuine capability result, not just exit code. The worker is the only thing with a
window (--client-window); the full menu client only runs after the probe genuinely passes.

client.log is empty because the probe worker logs to stderr (which we capture); the file sink
belongs to the full client we don't reach. Our i_WriteFile writes real files fine.

### The map-load gate (the real blocker to any rendered frame)
The guardian setup (exe+0x4e0120) resolves the selected map via a linear search over catalog
entries (match a dword id at entry+0x1b8 vs the query's [r15+0xd8]); the matched entry's
pointer at entry+0x10 is **NULL** -> `0x4e0af5 mov r15,[rdx+0x10]; test r15; je 0x4e0b66`
failure -> logs "Native map catalog lookup did not resolve the selected map: expected
halo3\maps\guardian, got ''". So the catalog HAS a guardian entry but its map/scenario/path
pointer was never populated. guardian.map + guardian.mapinfo read fine and guardian's tags even
merge ("Merged ...guardian ... 2052 tags"), so it's not a file problem — the catalog entry's
data slot is null.

### KEY EXPERIMENT — forcing past the assertion makes the engine RUN STABLY
PELOAD_PATCH="exe+4e0bde:9090,exe+4e0bf2:31c0,exe+4e0c38:9090,exe+4e0c48:31c0" NOPs the
resolve-failure branches so the guardian setup proceeds. Result: NO crash, rc=124 (ran the full
90s in its main loop, ~105k log lines, 0 panics/faults). engine_initialized_flag:1,
standalone_present_path_observed:true, ui_mode_available:true, queued_game_mode_raw:3, processes
"Shell lifecycle event 0/7". BUT engine_error_message:"main_game_change_immediate() failed"
(start_error_raw:11) because the forced map is bogus/empty, so no map loads (map_loaded:false,
ready_presents:0, visible:false) -> nothing presents. This is the closest to a running game yet:
the engine loop is alive; it just has no valid map/scenario to present.

`main_game_change_immediate` is a halo3 engine call (Reclaimer logs its failure). The real fix
is making guardian's catalog entry resolve to a valid map so the game-change succeeds. The
"Skipped main-menu-only fallback" path (exe+0x4c2310 logs it; decision in exe+0x4b9480 around
0x4c24d5 gated by exe+0x76d2d0 AND a cl&1 flag) is the menu alternative but has multiple gates;
patching 0x76d2d0->ret0 alone didn't un-skip it.

### Next levers (priority)
1. Empirically dump the catalog during resolve (detour tracer on the loop at exe+0x4e0abf..0af5)
   to see entry count, the query id, and why guardian's entry+0x10 is null — is the catalog
   built but unpopulated, or is +0x10 lazily loaded by a step that fails here?
2. Find where catalog entry+0x10 is WRITTEN (the populate step) and why it's skipped for guardian.
3. If the map genuinely can't be hosted without halo3 D3D game rendering, pivot to the menu
   fallback (un-skip all gates) since the menu is pure GL and needs no halo3 D3D.

---

## SESSION 6 (cont.) — ROOT CAUSE of the empty map catalog: the Forge asset library never builds

The guardian map-catalog resolve ("got ''") traces to the **Forge asset library**
(ReclaimerCache/forge-maps/forgelib, ~340 MB of every map's Forge objects; also the
source of the main-menu scenery) never being built — only a stale 0-byte
`forgelib.lock` from a crashed first-build. Every run saw the lock ("asset library
already being prepared") and skipped. Clearing the lock triggers the build, which
exposed missing kernel32 surface the builder needs (all now implemented, commit e6e2b02):
- Import integrity check (exe+0x4b9480): halo3's imports {CreateFileW, CreateFileA,
  CloseHandle, GetFileSizeEx, CreateIoCompletionPort, ReadFileEx, ReadFile} must each
  == GetProcAddress(kernel32,name). Missing names (CreateFileA, CreateIoCompletionPort,
  ReadFileEx) got a fresh make_stub per call -> mismatch -> "halo3.dll's CreateFileA
  import is not kernel32's". Added to resolve_impl (one stable ptr for both paths).
- Async tokio/mio I/O driver: needs a real IOCP. Added CreateIoCompletionPort (real
  completion-queue obj), PostQueuedCompletionStatus, GetQueuedCompletionStatus(Ex),
  GetOverlappedResult; overlapped ReadFile posts a packet. Fixed "failed to wake I/O
  driver" + tokio err-203 panic.
- Memory-mapping: builder maps each map (CreateFileMappingW/A + MapViewOfFile). Added
  over mmap() (FILE_MAP_READ -> PROT_READ; requesting PROT_WRITE on O_RDONLY = EACCES=5).

With these the per-map "Forge objects unavailable" errors are GONE; the build now runs
and processes ~40 maps. REMAINING: the build is async ("building it while the game
starts") and the short-lived probe worker exits at the guardian resolve before it
finishes, so forgelib is never written -> next run still skips. Forcing past the
guardian assertion keeps the worker alive but the forced bogus map eventually faults
the main thread (0xC0000005 at exe+0x18D46B4), killing the bg build. mainmenu.map does
NOT exist (menu scenery is synthetic from campaign maps; its "error 2" is expected);
040_voi.map (477MB campaign map) is "left out" with os-error-1 (likely no forge data,
non-fatal). NEXT: get one complete forgelib build cached (without the main-thread
crash), then confirm a normal run resolves guardian / renders.

---

## SESSION 6 (cont.) — Forge library BUILT (cached); guardian resolve is a SEPARATE issue

The Forge asset library now builds to completion and is cached on disk:
  ReclaimerCache/forge-maps/forgelib-a067bf91a421.json (1.2MB) + .map (392MB).
How: the build is async ("while the game starts") and the probe worker exits at the
guardian resolve before it finishes. Trick to force completion once: patch the
post-refusal call `exe+0x4e1226` (call engine_shell "stopping") to an infinite loop
(ebfe909090) so the worker's main thread hangs ALIVE while the background build
finalizes (partial grew 16K -> 121MB -> 392MB -> renamed to .json). The cache
persists, so normal runs now use it (no rebuild unless the forgelib hash changes or
the lock/json is deleted). NOTE: this is a manual one-time step; a clean solution
would make the loader run the build to completion itself.

IMPORTANT: building the forgelib did NOT fix the guardian map resolve — a normal run
still fails "did not resolve the selected map: expected halo3\maps\guardian, got ''"
with the Guardian/Slayer variant 'Custom Slayer' at custom_map:false and
launch_inputs map_available:false / map_variant_available:false / game_variant_available:false.
So the forge library (menu scenery/objects) and the available-maps catalog are
DIFFERENT subsystems. The guardian resolve blocker is the available-maps catalog not
containing a usable guardian entry (all match inputs are unset). The error-build at
exe+0x4e0c93 (fn 0x4e0120) is NOT reached via the loop at 0x4e0abf (hooks there never
fire), so that block-mapping was wrong; the real resolve path is still unlocated.
NEXT: find where the available-maps catalog is populated and why guardian (whose
guardian.map + guardian.mapinfo read fine, tags merge) isn't a usable entry.

## Session 7 (2026-10-12): BREAKTHROUGH — past guardian refusal into the launch sequence

### The refusal path, fully mapped
- halo3 state machine (dispatcher halo3+0xb100, state@halo3+0xa3cad4) in STATE 1 invokes
  the exe shell callback at halo3+0xb219 `call [rax+0xb8]` (object halo3+0xa3cab8, edx=stage=1).
  halo3 does NOT check the callback's return (0xb21f just jmps to tail) — the EXE callback
  itself aborts the worker on refusal.
- The guardian-setup shell callback is exe fn **0x4e0120** (edx=stage). At entry rcx = Reclaimer's
  config object (paths, fonts-cache dir, nickname "Player", 32-byte identity key) — NO map name.
- It resolves the selected MP map and compares to expected "halo3\\maps\\guardian". Resolves EMPTY → refuse.
- Backtrace of the refusal logger (exe+0x4e1225, built via std io at exe+0x18ef6b0) confirmed via
  PELOAD_TRACE_BT="did not resolve": refusal <- halo3+0xb21f <- halo3+0x12e187 <- exe+0x79d532 <- deep halo3.
- The resolution reads halo3 state via **ReadProcessMemory** (IAT 0x1ab0070; wrapper exe+0x4b3310 that
  does a thread-id check [0x228aff8] + range checks [0x228b070..098], else RPM). Confirmed with
  PELOAD_TRACE_RPM: last reads before refusal are halo3+0xa3cad4=1 (state), halo3+0xb1887c=0,
  then **halo3+0xb43c08 (16 bytes) = all zeros** = the empty selected-MP-map {ptr,len}. That RVA is
  hardcoded in the callback init (exe+0x4e041a `mov edx,0xb43c08`). halo3+0xb43c08 is in halo3's 65MB
  BSS game-state (sec rva 0x89b000, vsz 0x3e6a714).
- WHY it reads the empty halo3 slot: the callback first checks the Reclaimer map-catalog global
  **0x22648e0** (an Option, niche None = 0x8000000000000001). At exe+0x4e0429 `cmp r15,None; je 0x4e0453`
  — catalog is None → falls back to reading halo3+0xb43c08 (empty) → refuse. If the catalog were Some it
  would resolve from it (forge catalog HAS guardian, 162 refs in forgelib json). The catalog is a
  **menu_ui.rs lazy structure** (getter exe+0x6d7e10 called from menu code exe+0x24864d in src/menu_ui.rs)
  that the DISPOSABLE worker never populates (line "Skipped main-menu-only fallback in disposable worker
  memory (RVA 0x12FB1C)", logged exe+0x4b2310, decided in engine_match fn 0x4b9480 near 0x4c2576).

### The decision point and the breakthrough patch
- exe+0x4e0664 is the callback's result hub: `xor eax,eax; cmp rax,[rbp+0x160]; jno 0x4e11ad`.
  [rbp+0x160] = map-resolution Result. Ok-sentinel (0x8000000000000000) → fall through to 0x4e0673
  (call 0x4db040, clean RET = success). Error → jno 0x4e11ad (build refusal, log at 0x4e1225).
- **PELOAD_PATCH="exe+4e066d:909090909090"** (NOP the `jno 0x4e11ad`, orig bytes 0f813a0b0000) forces
  the callback to return SUCCESS and NOT refuse. RESULT: worker gets PAST guardian — log grows from
  ~200 lines to 117k. It loads menu/customization artwork (rank icons, emblems, Spartan preview 43
  meshes, Spartan armor 0-3), reaches engine_initialized_flag:1, startup_state_raw:2, session_state_raw:6,
  network_lifecycle_flag:1, standalone_present_path_observed:true, then enters a busy-wait
  ("Game audio volume applied" x110k) and finally **Native engine error 4: external_launch_individual_state_timeout**
  (Shell lifecycle event 10 then unmapped event 5 -> stop, exit 3).

### New blocker (next root cause)
- launch_inputs are ALL false (game_variant_available:false, map_available:false, map_variant, membership,
  seed, simulation all false; only ui_mode_available:true) EVEN THOUGH "Constructed native Guardian/Slayer
  variant 'Custom Slayer'" logged. So the constructed variant + map are NEVER applied to halo3's launch
  inputs. The NOP bypass skips the guardian refusal AND the real provisioning (provisioning is on the
  resolved-map success path, which empty-map skips), so launch times out waiting for the map to load.
- Ruled out this session: profile (deleting ReclaimerProfile/profile.json -> auto-recreated "Player",
  same refusal); caches (forgelib 392MB + fonts built OK, guardian.map present); PROBE_EXIT0 (orchestrator
  just exits, there is NO separate menu client — the ONE disposable worker is the engine); dropping
  --guardian-slayer (worker exits 2, requires the flag); forging halo3+0xb43c08 {ptr->"halo3\\maps\\guardian",
  len=19} via PELOAD_FORGE_MAP (fired once but no effect — 0xb43c08 isn't the direct compare source; the
  compare uses a 256-byte local buffer filled elsewhere, and 0x4b3310 range-checks reject a non-halo3 ptr).

### NEXT LEVERS
1. Make catalog global 0x22648e0 become Some (populate the menu_ui map catalog) so the callback resolves
   guardian from the forge catalog and runs the REAL provisioning (not the NOP bypass). Find the populate
   site (menu_ui.rs) and why the disposable worker skips it; consider forcing it to run.
2. OR provision halo3 launch inputs directly: find where game_variant/map/seed/simulation are written to
   halo3 (WPM to halo3+0x2228xxx/0x2229xxx/0x2d73xxx seen in RPM trace) and why map_available stays false.
3. The variant IS constructed in exe memory (engine_match fn 0x4b9480, "Constructed native...Custom Slayer")
   but not pushed to halo3 — trace the push and the gating flag ("testing flags 0x8").

### Session 7 cont. — ROOT CAUSE PROVEN: map catalog never initialized in disposable worker
- The "map push" into halo3 is exe fn **0x4ccb60** (src/engine_bindings.rs/engine_splitscreen.rs):
  loops seat r14=0..3 writing four 0x21c-byte per-seat launch structs to **halo3+0x2228bc0**
  (stride 0x21c), via helper 0x4c4c90 (WPM wrapper). Confirmed by RPM/WPM trace: 14 WPM writes to
  halo3, four 0x10c-byte payloads (all val=0) + flags=1 at 0x2228bd8/0x2228df4/0x2229010/0x222922c,
  a 0x708 zeroed region @0x2229430, flag=3 @0x2229b50, a hash @0x2d73ea0. NONE carry a map name.
  So the push happens but pushes ZEROS — no map/variant data.
- WHY zero: guardian never resolves because Reclaimer's map catalog is never built. CATSTATE dump
  (hook mode 0xca70 @callback entry 0x4e0120): BTreeMap @0x2264448 has once@0x2264458=0 (never ran),
  len@0x2264460=0, dataptr@0x2264468=0x8 (empty-marker), cap=0; cache Option @0x22648e0 =
  0x8000000000000001 (None); second Once @0x2264950 = 0. Catalog is completely empty.
- The catalog builder fn **0x462fb0** is called **0 times** (hook count 0). Its get-or-init fn
  **0x476910** is ENTERED 0 times (hook count 0). So the entire catalog-init path is skipped in the
  disposable worker. Builder callers: 0x477404 (fn 0x476910), 0x9f091c (forge fn 0x9f0540).
  get-or-init callers: 0x46f644, 0x57ce3a, 0xa09dab (0xa08d70 = std mpmc channel code — async).
- The map catalog is a menu/async-populated structure; the headless disposable --guardian-slayer
  worker never triggers it. On Windows the worker presumably either builds it anyway or halo3 has
  guardian selected (halo3+0xb43c08 non-empty). Note: halo3+0xb43c08 has only ONE exe reference
  (the READ at 0x4e041b) — halo3 writes it itself when it loads a scenario; forging it had no effect
  (not the compare source; the "got" comes from a 256-byte local buffer).
- Startup logs "testing flags 0x8" at exe+0x65dbc5 ("Native startup constructor completed") — likely
  the disposable/testing-mode bitflag that gates skipping menu+catalog init.
NEXT: force the catalog to build (flip the disposable/testing-mode gate so the worker runs full
client init = builds catalog + could show the menu), OR replicate the catalog build / provide
guardian's map data directly to the push at fn 0x4ccb60.

### Session 7 final — RECLAIMER_LOCAL_RUNTIME mode handshake + stage 4 reached
- The orchestrator (engine_probe.rs) spawns the worker with env **RECLAIMER_LOCAL_RUNTIME=menu:0**
  (+ RECLAIMER_DOWNLOADED_MODS_PAUSED=1, RECLAIMER_PRIVATE_JOIN_CAPABILITY=, _NO_DEBUG_HEAP=1). This
  is Reclaimer's OWN design (the loader only translates CreateProcessW; it never forces a worker).
  "disposable worker" is Reclaimer's own log string. The worker IS the menu:0 runtime doing a guardian
  self-test at startup.
- fn 0x599920 reads RECLAIMER_LOCAL_RUNTIME (GetEnvironmentVariable) and sets engine_startup's stage:
  menu:0 -> stage 0; UNSET -> stage 2; menu:2 -> stage 1 (the ":N" is not the stage). Added loader knob
  **PELOAD_LOCAL_RUNTIME** (value, or "__unset__") to override it in the child-env overlay.
- Role must MATCH the --guardian-slayer startup: server:0 -> "Local runtime role does not match native
  startup mode" (exit 1); server also tripped a halo3 XAudio2 import-integrity check (fn 0x460250,
  4x cmp halo3+0xa39e10.. vs halo3+0x60c4xx; NOP the 4 jne @0x460294/0x4602cb/0x460302/0x460335 to pass).
  client:0 got furthest of the alt-roles (full engine+Forge+variant) then "Invalid client ticket"
  (engine_startup 0x65d27f, ticket parse 0x63a30). These are dead ends — role is pinned to menu:0.
- **BREAKTHROUGH COMBO**: PELOAD_PATCH="exe+4e066d:909090909090" (bypass guardian refusal) +
  PELOAD_LOCAL_RUNTIME="__unset__" (stage 2) reaches **shell launch notification stage 4** (vs stage 1),
  visible:true, "Client screens now drawn into the game's frame", 4096x4096 render res, loads
  "Loading screen artwork: Guardian". New blockers surfaced:
  * engine_error_message:"no map selected" (the map-catalog root again — map never provisioned).
  * "Forge library unavailable: the engine refused it (error 0)" — forge-lib validate fn 0x15ea240
    returns None (reads globals 0x2264a68/0x2264a70; refusal built at fn 0x4764c0 @0x476618).
  * "Refused custom cache data of guardian (validation kind 1)".
  * CRASH 0xC0000005 (addr=1) in the client-screen/loading-screen GDI compositing: engine calls
    CreateSolidBrush/GdiAlphaBlend/FillRect (added stubs) then SetStretchBltMode/SetBrushOrgEx/
    StretchDIBits (still no-op stubs) and derefs junk. Added CreateSolidBrush/CreatePen/GdiAlphaBlend/
    AlphaBlend/FillRect; StretchDIBits/BitBlt/StretchBlt are still no-ops -> GDI software blit is NOT
    implemented, so client-screen compositing can't produce pixels and crashes/stays blank.
TWO REMAINING BLOCKERS (both substantial): (1) "no map selected" = the map catalog (0x2264478 BTreeMap /
0x22648e0 cache) is never built in the headless worker (its builder engine_forge 0x9f0540 and get-or-init
0x476910 are never called — menu/forge-UI gated). (2) real GDI software rendering (StretchDIBits/BitBlt/DCs)
for the client-screen composite. Neither is a quick lever.

### Session 7 FINAL+ — stub fixes cleared 5 blockers; reach stage 4 clean, variant valid
Live config: PELOAD_PATCH="exe+4e066d:909090909090" + PELOAD_LOCAL_RUNTIME="__unset__".
FIXES THIS SESSION (all in peload.c):
- bcrypt asymmetric signature: i_BCryptImportKeyPair/ImportKey (return a key handle, was null),
  i_BCryptVerifySignature (STATUS_SUCCESS), i_BCryptDestroyKey. RESULT: the game VARIANT signature now
  passes -> launch_inputs game_variant_available:true, game_variant_valid:true, game_mode:"Slayer",
  simulation_available:true, slayer_score_to_win:25, round_limit:1/round_minutes:8. (Was all false.)
- GDI: i_CreateSolidBrush/CreatePen/GdiAlphaBlend/AlphaBlend/FillRect/StretchDIBits/SetStretchBltMode/
  SetBrushOrgEx; FIXED i_SelectObject (was returning (void*)0x1 -> engine deref'd addr 1 -> crash) to
  return a valid static GdiObj; FIXED i_GetDC/GetWindowDC (were bare 0xDC01 -> deref'd as DC) to return
  real GdiDC objects. RESULT: the loading-screen/client-screen GDI crash (0xC0000005 @addr 1) is GONE.
- forge/cache refusals ("Forge library unavailable error 0", "Refused custom cache data validation kind 1")
  DISAPPEAR when the stale forgelib (forge-maps/forgelib-a067bf91a421) is deleted (rebuild path, no stale
  file to refuse) — custom-cache validator = fn 0x4db570 (logs @0x4db91d), forge-lib validate = fn 0x15ea240
  (None -> refuse @ fn 0x4764c0/0x476618). BUT clearing them did NOT make map_available true, so they are
  NOT the map blocker. (Restored the forgelib backup afterwards.)
CURRENT STATE: worker reaches shell launch stage 4 with NO crash, merges guardian MP chars, visible:true,
startup_state_raw:10, 23 asset requests — then stops on **"no map selected" (engine_error_raw:4)**.
IRREDUCIBLE ROOT (confirmed from ~8 angles): launch_inputs map_available/map_variant_available/seed_available
are all false. The game VARIANT is code-constructed (bypassed the catalog via bcrypt), but the MAP requires
catalog resolution to be provisioned into halo3's per-seat launch structs (fn 0x4ccb60 -> halo3+0x2228bc0,
still zeroed for the map). The map catalog (BTreeMap 0x2264478 / cache 0x22648e0) is never built in the
headless worker (builder engine_forge 0x9f0540 / get-or-init 0x476910 never called; menu/forge-UI gated).
NEXT: either replicate the catalog build, or directly inject the map+map_variant+seed launch inputs into
halo3 (byte-level; the per-seat struct format at halo3+0x2228bc0 stride 0x21c is still unmapped).

### Session 7 ++ — GDI SOFTWARE RENDERING IMPLEMENTED (rasteriser) + present threading wall
Implemented real GDI software rasterisation in peload.c (all were no-ops):
- GdiObj extended {color,kind,topdown}; gdi_fill/gdi_blit (32bpp BGRA, scaled nearest + src-alpha).
- FillRect, BitBlt (SRCCOPY/BLACKNESS/WHITENESS), StretchBlt, StretchDIBits, SetDIBitsToDevice,
  GdiAlphaBlend -> real pixel ops on GdiObj surfaces. SelectObject/GetDC/GetWindowDC return real
  objects (crash-safe). SetTextColor/SetBkMode/SetBkColor added. CreateDIBSection tracks the
  window-sized composite DIB as g_gdi_mainsurf.
- Present path: gdi_present_surface() uploads a BGRA surface to a texture-backed FBO and
  glBlitFramebuffer -> window FBO 0 (flips top-down DIBs). Wired into MsgWait, swap_Present
  (IDXGISwapChain::Present), and available for the window DC.
REMAINING (two walls, both real):
1. PRESENT THREADING: the GL context is current only on the engine's render thread; MsgWait, the
   DXGI swap_Present, and the CPU thread that composites the GDI loading screen are DIFFERENT threads
   -> CGLGetCurrentContext()==0 there -> present always skips ("no GL ctx on this thread"). The engine
   never calls wglSwapBuffers. To show the CPU composite we need to present on the context-owning
   thread (or a shared GL context, or a non-GL Cocoa layer path). NOTE: this likely means the GL
   present_frontend never actually displayed either (explains earlier all-black .bmp captures).
2. The worker still aborts on "no map selected" (map-catalog root) after only ~7 render-loop
   iterations, before a full loading-screen composite exists.
So the rasteriser is done and correct, but a visible frame needs (1) the cross-thread present solved
and (2) getting past "no map selected" so a real composite is produced.

### Session 7 +++ — PRESENTATION ARCHITECTURE GAP identified (why nothing is ever visible)
Tried presenting the GDI composite / GL frontend FBO from MsgWait, IDXGISwapChain::Present, AND the
glDrawElements hook (render thread). ALL report CGLGetCurrentContext()==0 ("no GL ctx on this thread").
CONCLUSION: on the guardian/launch/loading path the engine renders through **D3D11/DXGI (which we stub
as no-ops), NOT OpenGL** — so there is NO live GL context on any thread we can present from, and nothing
ever reaches the visible window. This explains the all-black captures across every prior session: the
GL present_frontend path was never actually presenting (its own CGLGetCurrentContext check was failing).
The GL frontend path only lights up for the actual main MENU (menu_ui.rs), which we never reach.
To make ANYTHING visible on this path we need one of:
  (A) a real D3D11 -> Metal/GL bridge (the engine's swap chain actually presents) — large;
  (B) present the CPU GDI composite (g_gdi_mainsurf) to the NSWindow via a non-GL path
      (CoreGraphics/CALayer/Metal), independent of the engine's GL/D3D — medium, but the window is a
      GL NSOpenGLView and we never pump the Cocoa runloop, so it needs care;
  (C) reach the real GL menu (menu_ui.rs) — blocked by the same "no map selected"/catalog root.
STATUS: GDI software rasteriser is implemented and correct; the blocker to a visible frame is the
presentation path (no D3D bridge / CPU-composite can't reach the GL window) AND "no map selected".

### Session 7 ★★★ — LIVE PERSISTENT WINDOW: engine renders on screen (no Wine)
Scoped the render path: on the guardian/launch path the engine uses **D3D11 only** (no GL/window of its
own; 1 Draw / 1 Present / 1 CreateTexture2D), and composites the loading screen / client screens into a
**1280x720 GDI DIB** which it "presents" via the D3D swap chain. We now:
1. Implemented the full GDI software rasteriser (FillRect/BitBlt/StretchBlt/StretchDIBits/SetDIBitsToDevice/
   GdiAlphaBlend/CreateSolidBrush/CreatePen/SelectObject/GetDC...). CreateDIBSection tracks the window-sized
   composite as g_gdi_mainsurf. Confirmed real content (nonblack-sum ~2.8e9) -> dumped loadingscreen BMP:
   the Reclaimer dark-blue gradient loading screen with UI separator lines.
2. Create OUR OWN NSWindow+NSOpenGLContext on the MAIN thread (via i_MsgWaitForMultipleObjectsEx — the only
   legal place; NSWindow must be main-thread). Hand the GL context to a dedicated **present thread**
   (present_thread_fn) that uploads the DIB to a texture-backed FBO and glBlitFramebuffer -> window FBO 0,
   ~30fps. (gdi_present_surface, gdi_present_main.)
3. Keep it on screen: PELOAD_HOLD_WINDOW + PELOAD_PATCH "exe+4e1950:ebfe" (spin the engine_shell STOP fn)
   so the worker parks at the "no map selected" abort instead of exiting -> window persists.
Also: bcrypt sig verify (BCryptImportKeyPair/VerifySignature) -> variant valid; GDI text stubs
(CreateFontW/TextOutW/ExtTextOutW/GetTextExtentPoint32W/GdiFlush) added; CorExitProcess routed to i_ExitProcess.
RESULT (verified): worker YES-PERSISTENT, present thread running, **611 frames presented**, 0 ctx skips.
A live window shows the engine's loading screen rendered on macOS with no Wine.
COMMAND: cd "<MCC>" ; PELOAD_WINDOW=1 PELOAD_WAIT=6 PELOAD_WORKER_INHERIT_STDIO=1 \
  PELOAD_PATCH="exe+4e066d:909090909090,exe+4e1950:ebfe" PELOAD_LOCAL_RUNTIME="__unset__" \
  PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 arch -x86_64 shim/build/peload analysis/project-reclaimer-v0.9.13.exe
REMAINING: text glyphs (engine's own font_package_x3.bin not loaded -> text_font_loaded:false); and the
"no map selected" map-catalog root (to progress past the loading screen into the menu/game).

### Session 7 ★★★★ — WINDOW VISIBLE ON SCREEN (cocoa_pump was the missing piece)
Symptom: with the present thread working (611 frames) the user still saw NO window. Root cause:
cocoa_pump() (nextEventMatchingMask/sendEvent) existed but was NEVER called -> the NSWindow was created
but the macOS window server never mapped/displayed it ("[cocoa] created (visible)" is only our log).
FIX: call cocoa_pump() on the MAIN thread inside i_MsgWaitForMultipleObjectsEx (the engine's message loop
runs there) whenever PELOAD_PRESENT is set. Verified: user saw the window launch; `osascript System Events`
lists `peload` as a non-background (windowed) app. The loading screen is now visible on screen, no Wine.
Note: engine's loading-screen TEXT uses GDI DrawTextW/GetTextFaceW/GetTextMetricsW/SetTextCharacterExtra
(all still no-op stubs) -> no glyphs yet; and "no map selected" still gates progress past the loading screen.

### Session 7 — font_package_x3.bin fixed + full current stub list
WHY text_font_loaded:false: the engine opens ReclaimerCache/retail-fonts/<hash>/font_package_x3.bin
(the 3x/high-res text font) and it FAILS — Reclaimer's font cache build produced only the base
font_package.bin (x1), never the x2/x3 scaled variants. (Filename is built at runtime; "font_package_x3.bin"
is not a literal in the exe. MCC's maps/fonts only ships the ICON fonts here, no base text font.)
FIX: font_subst() in peload.c now falls back font_package_x{N}.bin -> font_package.bin when the scaled
file is absent. Verified text_font_loaded:true. (Loading-screen fallback text still uses GDI DrawTextW,
which is still a no-op stub, so that specific text won't draw until DrawTextW is implemented.)
CURRENT no-op STUBS still hit (29): TEXT: user32!DrawTextW, gdi32!GetTextMetricsW/GetTextFaceW/
SetTextCharacterExtra. NET: ws2_32!getaddrinfo/freeaddrinfo (DNS; unlock-lookup http also needs async
TCP). TIMING: winmm!timeGetTime, kernel32!CreateWaitableTimerExW. FILE(ANSI): kernel32!GetFileAttributesA/
GetFileAttributesExA/SetFilePointer/DeleteFileA/LoadLibraryExA/SetFileTime. WINDOW: user32!RedrawWindow/
IsIconic/EnumDisplaySettingsW/EnumWindows/GetFocus. LOW: kernel32!CreateJobObjectW/GetConsoleProcessList/
GetLocaleInfoEx/SetThreadPriority, (dynamic)!SetThreadDescription, winmm!timeBeginPeriod/timeEndPeriod/
waveOutGetNumDevs, XAudio2_9Redist!#ordinal, oleaut32!GetErrorInfo, version!GetFileVersionInfoW,
timezone!GetTimeZoneInformation.

## Session 8 — async HTTP networking fixed (unlock_lookup no longer errors)

Root cause of `unlock_lookup: "error sending request for url (http://148.251.153.44:8790/
v1/unlocks/lookup)"`: the client is Rust reqwest/hyper over **tokio + mio 1.2.3**, whose
Windows reactor learns socket readiness via `\Device\Afd\Mio` + `NtDeviceIoControlFile
(IOCTL_AFD_POLL 0x12024)` + IOCP (`GetQueuedCompletionStatusEx`). Several layers were
missing/wrong:

1. **Error-slot mismatch (the real root).** A non-blocking `connect()` returns EINPROGRESS;
   Windows reports WSAEWOULDBLOCK(10035). mio reads the code via `io::Error::last_os_error()`
   = **GetLastError** (`g_lasterr`), but we only set WSAGetLastError (`t_wsa_err`). mio saw a
   stale error, treated the connect as failed, and closed the socket with no registration.
   Fix: `ws2_err` and the connect in-progress branch now set BOTH slots (they are the same
   value on real Windows).
2. **getaddrinfo/freeaddrinfo/GetAddrInfoW/FreeAddrInfoW** implemented over the system
   resolver, converting BSD addrinfo/sockaddr to the Win32 ADDRINFO(A/W) 48-byte layout
   (AF_INET6 23<->30). Previously an empty stub -> no address.
3. **WSAIoctl**: `SIO_KEEPALIVE_VALS` (0x98000004, set on every hyper connection) now applies
   SO_KEEPALIVE instead of returning WSAEINVAL; `SIO_BASE_HANDLE` (0x48000022) + BSP variants
   return the socket handle so mio can get a base socket.
4. **AFD poll bridge**: `NtCreateFile("\Device\Afd\*")` returns a synthetic AfdDev handle;
   `CreateIoCompletionPort` binds it to the IOCP; `NtDeviceIoControlFile(IOCTL_AFD_POLL)`
   parses AFD_POLL_INFO (Handle=base socket, Events mask) and registers the fd on a dedicated
   `poll()` thread; on readiness it writes the satisfied AFD event bits + IO_STATUS_BLOCK and
   posts the IOCP completion with the original ApcContext overlapped. `NtCancelIoFileEx` and
   `SetFileCompletionNotificationModes` added. AFD event map: RECEIVE/ACCEPT<-POLLIN,
   SEND/CONNECT<-POLLOUT, ABORT/CONNECT_FAIL<-POLLERR, DISCONNECT<-POLLHUP.

Result: engine `unlock_lookup` goes from `local_error:"error sending request"` ->
`local_error:null, local_pending:true` (in flight; the headless self-test snapshots and
tears down the runtime before the response lands — same with sync or async connect, so it is
the diagnostic's teardown, not our plumbing). Proven end-to-end with `PELOAD_NETTEST=host:port
/path`, which drives the translated non-blocking path (connect->wait writable->send->wait
readable->recv) and fetches the full ~86 KB `/v3/discovery` list (curl ref: 86039 bytes).
Opt-in `PELOAD_SYNC_CONNECT` completes connect synchronously if ever needed; off by default.

## Session 8 (cont) — stubs closed + GDI text renders the loading screen

Closed the full runtime stub list (27 + deeper-path): networking (getaddrinfo family,
WSASend, AFD poll bridge, keepalive), timing (timeGetTime/timeBeginPeriod/CreateWaitable
TimerExW), file IO (DeleteFileA/GetFileAttributes[Ex]A/LoadLibraryExA/SetFilePointer/
SetFileTime/CreateDirectoryA), window (GetParent/GetFocus/RedrawWindow/EnumWindows/
EnumDisplaySettingsW/GetWindowThreadProcessId/IsIconic), fibers (ConvertThreadToFiber[Ex]/
ConvertFiberToThread), misc (GetLocaleInfoEx/GetTimeZoneInformation/GetErrorInfo/
RtlCaptureStackBackTrace/SetThreadPriority/SetThreadDescription/CreateJobObjectW/
SetInformationJobObject/GetConsoleProcessList/GetCurrentPackageId/CompareFileTime/
waveOutGetNumDevs/GetFileVersionInfo*/VerQueryValueW). Only XAudio2 #ordinal (audio) left.

Closing these let the **worker run far past the old "Unmapped shell callback" exit**: with
`PELOAD_LOCAL_RUNTIME=__unset__` (+ the 4e066d NOP / 4e1950 hold patches) it now resolves
the map catalog ("Native map catalog lookup verified: halo3\maps\guardian"), merges
130_epilogue.map with guardian's MP characters, reaches engine_initialized_flag:1,
game_mode Slayer, and the native swap-chain present path at ~15% load.

TEXT: implemented real GDI text via CoreText (DrawTextW/TextOutW/ExtTextOutW/GetTextMetricsW/
GetTextFaceW/GetTextExtentPoint32W/SetTextCharacterExtra + CreateFontW height tracking),
rasterizing runs/word-wrapped blocks (CTFramesetter) into the BGRA DIB and compositing with
premultiplied alpha. The engine draws its menu text into a DC whose bitmap is the 1x1 default
placeholder (its real surface was restored away); since the coords are full-screen (1280x720),
`gdi_text_target()` routes text with no real target onto the presented `g_gdi_mainsurf`.

Result: the window now shows the actual **Guardian/Slayer loading screen** (MAP/GUARDIAN +
blurb, GAME TYPE/SLAYER + blurb, progress bar + 15%) instead of black. The 3D scene itself is
still unbridged (D3D11 draw calls are no-ops — a real D3D11->Metal layer is the remaining
visible-3D blocker), but all 2D GDI UI/text now renders.

## Session 8 (cont) — D3D11 present bridge + loading-stall diagnosis

The engine's D3D "rendering" at the loading/UI stage is NOT 3D geometry: every draw is a
4-vertex tri-strip fullscreen quad (stride=0, no VB, no input layout) sampling a 1280x720
BGRA texture into the 4096x4096 back buffer — i.e. it composites its frame into a 720p
texture and blits it. So no HLSL->Metal translation is needed to SHOW the frame: we capture
that texture (implemented ctx_UpdateSubresource / ctx_CopyResource; Map already returns the
backing buffer) and present it (gdi_present_main prefers g_d3d_frame_bits, BGRA top-down).
Added pipeline-state tracking (IASetInputLayout/VertexBuffers/IndexBuffer/PrimitiveTopology,
PSSetShaderResources, OMSetRenderTargets, RSSetViewports) + PELOAD_TRACE_DRAW/TRACE_SWAP.
The window now shows the engine's OWN rendered loading screen (live progress), not the GDI hack.

Other fixes: real waitable timers (SetWaitableTimer arms timer_due_us; sync_wait fires by wall
clock) for frame pacing; valid swap_GetDesc (4096x4096/BGRA/2 buffers) + swap_GetFullscreenState
(was zeroed -> "GPU swap chain: 0x0"); privileged-port remap in sa_w2b/sa_b2w (loopback ports
<1024 -> +40000) so the native-UDP transport binds ("127.0.0.1:1000 result=0" now).

STILL OPEN — loading stalls at 15.45% (progress_source halo3_shell_stage_5), ready_presents:0.
Sample of the hung worker: main thread fine (idle in MsgWait/nanosleep); 4 job threads parked
in WaitForSingleObject; one thread BUSY-SPINS in guest code at halo3+0xd1ee3. The engine's
render/"presentation callback" runs ~43 frames (~80ms gaps) then stops "between frames" with
ready_presents never incrementing although swap_Present returns S_OK and the swap desc is now
valid. The engine is loading INTO a Guardian/Slayer MP match (peer_count 1, simulation_available
true). Next: RE halo3+0xd1ee3 to see what the spin polls (present-complete ack vs session/sim
readiness), or try the main-menu runtime path instead of loading straight into a match.

## Session 9 — angr/oxidizer decompilation + stage-5 stall RE

Set up Rust/PE decompilation (the user asked for sefcom/oxidizer; its full build fails on this
macOS — pinned pyvex builds VEX and dies in pycparser. Workaround: PyPI angr 10.0.3 wheels +
bitstring==4.1.4, in re-tools/ with decomp.py / refs.py; see CLAUDE.md + the rust-pe-decompile
skill). angr loads the 37 MB PE in ~3s and decompiles a region-limited function in seconds.

Decompiled the stall machinery (exe = reclaimer worker, 0x140000000; halo3 = 0x180000000):
- exe `sub_14054ae30` = per-frame update: pumps our PeekMessage/DispatchMessage, reads input
  ONLY when foreground (`g_14228b470 && GetForegroundWindow()==v63`, v63 = its main HWND passed
  in), and the present WATCHDOG fires "presentation callback stalled" when no present for
  1000ms (reads last-present ts `g_14228b490`). So presents simply STOP after ~40 frames.
- halo3 `sub_1800b19bc`/`sub_1800b1c90` = worker-thread loops that run until the engine STATE
  MACHINE reaches 1 (`g_181e439fc[state]==1` / `(&g_180000000)[0x1E452BC + 4*state]==1`). The
  loops also busy-wait each frame for a GPU-frame-ready flag `(&g_180000000)[0x21B0A74 + 24*v7]`
  (v7 = rotating buffer idx) or event `g_1821b7578` (event mode `g_18089de64`). Forcing those
  flags (PELOAD_POKE32 of 0x1821b0a74+24*i) did NOT unblock — those were one-shot startup
  callbacks; the active present loop's ready target differs.
- halo3 `sub_1800b1d28` = engine init (game globals / 91 init callbacks / init.txt), not a loop.
- exe main-menu fallback `sub_14012f400` runs only when `g_14228af60 && !g_14228aeec &&
  g_14228aeea`; else "Skipped main-menu-only fallback in disposable worker memory".

ROOT (still open): the Guardian/Slayer MATCH load stalls because the engine state machine won't
advance past stage 5 (needs the simulation/session/present handshake a stubbed GPU can't
satisfy), so hosting is never verified and the 45s probe gives up. The loading-screen Guardian
ARTWORK backdrop is missing for the same reason — it loads after 15%, which we never reach.
This is a *probe-engine-worker* (verifies hosting, then the orchestrator would launch the real
client); it is not itself the play client.

New loader hooks this session: PELOAD_POKE32=<absHex>:<v32> (write guest mem each present),
hook_log id=0xd15 (dispatch index + resolved fn ptr), stable swap_GetBuffer, valid swap_GetDesc.

## Session 9 (cont) — FIBERS: the real un-stub that moves the load

BREAKTHROUGH. The SIGUSR1 all-threads guest-stack dumper (dump_all_guest_stacks) showed the
busy load thread executing halo3+0x5ff2e0, which calls **SwitchToFiber**. Halo 3 streams/
decompresses its map assets through a fiber coroutine (sub_1805ff2e0 sets up in/out pointers on
the fiber ctx then SwitchToFiber(ctx->field_1848); the caller sub_18026610c loops until done).
SwitchToFiber/CreateFiber were STUBBED no-ops (the earlier stub audit only implemented the
fiber funcs that had fired in logs: ConvertThreadToFiber/Ex, ConvertFiberToThread — the create/
switch ones only fire this deep in the load). So the decompression worker never ran -> the loop
spun forever -> stuck at 15%.

Implemented real fibers with ucontext (CreateFiber/Ex, SwitchToFiber, DeleteFiber,
GetCurrentFiber, GetFiberData, IsThreadAFiber; 1MB guard-paged stacks). MUST also set
TEB->NtTib.FiberData (TEB+0x20, via gs:0x30 self-ptr) on switch/convert — GetFiberData reads it
(cut crashes 11->1). Also un-stubbed MessageBoxW (now LOGS the dialog text instead of swallowing
the engine's error) and GetWindow.

RESULT: the load now actually streams assets — guardian.map -> shared.map -> campaign.map
(was frozen at 15% forever). Remaining: an intermittent 0xC0000005 in halo3's decompression
bitstream reader (0x5FD849 / 0x5FEC1A) at asset boundaries: the caller passes a1=0 so
ctx->field_1810 (raw-input ptr) is intentionally null ("refill via callback" mode), but the
reader sometimes takes the raw-read branch and derefs null — a decompressor state/mode confusion
to chase next. Diagnostic: PELOAD trap via SIGUSR1 -> dump_all_guest_stacks.

## Session 10 — D3D11→GL live draw path WIRED + proven (PELOAD_D3DGL)

The DXBC→GLSL translator (session 9 "B" piece) is now executed live. `d3dgl_draw()` runs in
ctx_Draw/ctx_DrawIndexed/ctx_DrawInstanced/ctx_DrawIndexedInstanced (gated by `d3dgl_enabled()`):
links the bound VS+PS as **separable GL programs** under a program pipeline (so VS/PS uniform
namespaces can't collide), connects VS→PS varyings by **runtime-injected `layout(location=N)`**
(`d3dgl_inject` also adds `out gl_PerVertex{}` for a separable VS), binds the RTV as an FBO, uploads
+binds SRV textures and CBs (`glUniform4fv cbN[]`), and draws. Shaders load their GLSL in
`dev_crShader` by FNV-1a-64 of the DXBC (`d3dgl_load_glsl` ← ~/reclaimer_glsl/<fnv>.glsl).

**Translator → 100% compile+LINK** via the real runtime builder (`PELOAD_D3DGL_CHECK` runs
d3dgl_inject+d3dgl_prog over every ~/reclaimer_glsl file): VS 144/144, PS 1059/1059. Fixed 6
stragglers in dxbc2glsl.py: non-finite float immediates → `uintBitsToFloat(bits)` (GLSL has no
`nan`); cb array sized to the max register index actually used (was hardcoded [64]); SV_Depth
(reg 0xFFFFFFFF) → gl_FragDepth, never declared as a color output (was emitting
`layout(location=-1)` after an unsigned→int overflow); gl_FragDepth is scalar. Live:
`shaders translated=2794 miss=6` (99.8%; 6 misses = empty 256B placeholders).

**Proven end-to-end** without the game (`PELOAD_D3DGL_SELFTEST`): drives the real executor with
translator-convention shaders + a texture → ~/reclaimer_d3dgl_selftest.bmp. Synthetic gradient+grid
renders pixel-correct; `PELOAD_D3DGL_TEX=<bmp>` feeds a real dumped Guardian frame and the actual
loading screen renders through the executor (Y-mirrored only because the blit applies the correct
D3D→GL flip to an already-top-down capture).

**Race/crash hardening** (the explicit ask): race-free init (mutex + double-check + barrier before
publishing g_d3dgl_ready); **no AppKit off the main thread** — window only when `pthread_main_np()`,
else a windowless shared CGL context on the render thread (`d3dgl_make_offscreen`) displayed by the
present thread (`d3dgl_blit_present`, own VAO); texture-upload OOB guard (`o->size>=w*h*4`);
`i_ws2_bind` sets SO_REUSEADDR.

**Harness gotcha that caused false "0/8 stalls":** the orchestrator ExitProcess(0)'s ~3s after it
CreateProcessW's the worker; the worker keeps running and reaches gameplay ~9s later. Never gate the
wait loop on the orchestrator PID (`kill -0 $ORCH || break`) — poll the log instead. The forced-match
PROBE reaches "gameplay presentation ready" but issues no sustained draws; the ~1M draws are in
**game-client** (the path that exercises d3dgl live). stage-5/0.333 stall = the known ~50% early-load
race, not a regression.

Remaining (correctness): BC1-7 decoder ("A"; compressed scene textures are magenta now), translator
approximations (sincos/indexable-temp/relative-cb/int-uint), engine blend/depth/raster state,
VS vertex-texture SRVs. geometry=0 (indexed world path) is still gated off upstream — not a d3dgl issue.

## Session 10 (cont) — "geometry=0" ROOT CAUSE: missing SetPrivateDataInterface

geometry=0 was never real. halo3 submits tons of world geometry (3MB vertex buffers, classic
POSITION/NORMAL/TANGENT/BLENDWEIGHT VSs — confirmed via PELOAD_DRAW_CALLER: e.g. halo3+0x2a35d6,
vb0=3145728B stride=32). But NO input layout was ever bound (IASetInputLayout slot 17 = 0 runtime
calls; a full context-vtable slot census found no other slot carrying it). My census/d3dgl keyed
"geometry" on g_d3dst.il, so every real geometry draw was mislabeled "fullscreen" -> black.

angr chain on halo3.dll (base 0x180000000):
- The input-layout cache `sub_1802aacf0` (prior RE WRONGLY called this the "armor-skin registry")
  lazily builds each IL via `sub_1802ac24c` (= halo3+0x2ac24c, the PELOAD_SKINGUARD target), which
  calls `device->CreateInputLayout` (device vtable off 0x58 = slot 11).
- To build it, it needs the VS bytecode as an ID3DBlob (GetBufferPointer@0x18 / GetBufferSize@0x20),
  fetched from the shader via `GetPrivateData` (ID3D11DeviceChild slot 4, GUID 9FCF6ACD-C385-4940).
  halo3 stores that blob earlier via SetPrivateDataInterface (slot 6).
- OUR STUB's SetPrivateData/SetPrivateDataInterface were NO-OPS -> GetPrivateData returned null ->
  null blob (r8) -> sub_1802ac24c would fault at 0x2ac329 -> PELOAD_SKINGUARD (needed for round-start)
  made it "return 0 on null key" -> CreateInputLayout never called -> NO input layout EVER -> all
  world geometry drawn without a layout (invisible). The skinguard wasn't guarding armor skins; it
  was masking the broken input-layout creation.

FIX (peload.c): real per-object private-data storage (PDStore on D3DObj) wired into the
ID3D11DeviceChild vtable — child_GetPD / child_SetPD / child_SetPDI at g_d3dchild_vtbl slots 4/5/6
(res+view inherit). child_SetPDI stores the 8-byte interface pointer; GetPrivateData returns it.
Also dev_crIL now resolves D3D11_APPEND_ALIGNED_ELEMENT offsets (dxgi_vtx_bytes).
RESULT: IASetInputLayout 0 -> ~227k/run; census flipped **geometry 0 -> 5,349,148** / fullscreen
96,852. d3dgl now runs the real geometry path (~1830 translated geometry draws/frame). Remaining is
pure d3dgl rendering correctness (matrices/CBs, semantic attribute mapping, BC textures, depth/blend
state) to make it a correct visible image. New tooling (env-gated): PELOAD_IA_CENSUS,
PELOAD_SLOT_CENSUS, PELOAD_DRAW_CALLER, PELOAD_TRACE_PD, PELOAD_D3DGL_DEPTH.

---

## Session 11 — the BLACK image root cause: global constant buffers (cb0 view-proj) never bound

With the geometry path alive (Session 10), the d3dgl backbuffer still captured **100% black**
(`reclaimer_d3dgl_*.bmp`, max pixel = 0). Root-caused end to end:

**The gameplay VS collapses gl_Position to (0,0,0,0).** Example VS `vs342`
(fnv `01dd0e00aabf37a6`) declares `cb0[13], cb1[7], cb2[10], cb3[3], cb12[3]` and computes:
`r1 = position × cb12` (world) then `gl_Position.i = dot(r1, cb0[i])` — i.e. **cb0 is the
view-projection**. With cb0 unbound (all-zero uniform) every clip coord is `dot(r1,0)=0` →
degenerate → nothing rasterizes → black. cb1/cb2/cb3 only feed lighting/fog outputs (o7/o8/o9).

**What the engine actually binds (runtime trace, PELOAD_TRACE_CB with caller capture):**
the ONLY VS constant-buffer binds in the whole run are `VSSetConstantBuffers(start=12,num=1)`
(64B–3360B per-object world / skinning palette) and `start=13` (32B), ALL from halo3's
per-object renderable path (callers halo3+0x280934 / +0x29eaa1 / +0x2b5538 / +0x2ab9d2, and
+0x28a6fa for 13). **b0–b5 are NEVER bound** — not via VSSetConstantBuffers (slot 7), not via
VSSetConstantBuffers1 (slot 119 — never called), not via command lists (ExecuteCommandList slot
133 — slot census shows 96–159 empty). Per-draw ground truth (PELOAD_D3DGL_DRAWLOG, dedup by VS
fnv): **28 of 32 distinct gameplay VS shaders are MISSING referenced cbuffers**; the only 4
"complete" ones are fullscreen passes (n=4, no cb refs). All world geometry needs cb0.

**Where b0–b5 SHOULD come from:** halo3 has a material-shader system whose per-material vtables
live in `.rdata` 0x766f00–0x7681d8; each has a "bind constants" method (tiny 0x30-byte thunks at
RVA 0x84f70/0x89848/0x89f80/0x8a594/0x8b21c/0x8b4a0/0x920f4 that do `mov rax,[rcx]; call
[rax+0x38]` = VSSetConstantBuffers). These thunks are dispatched **indirectly** via those vtables
and are **never invoked under our shim** (no runtime bind ever originates from the 0x8xxxx region).
So under our loader halo3 runs only the simple per-object path (0x28xxxx, binds b12/b13) and the
material/global path (binds b0–b3 view-proj/lighting) is dead → black.

**Where the global data lives (PELOAD_CB_DUMP + CB registry):** constant buffers created with
`D3D11_BIND_CONSTANT_BUFFER` (desc+8 & 0x4) are now tracked in `g_cbreg[]`. halo3 recycles a set
of **low-index pool buffers as the per-frame globals**, rewritten every frame (upd==current tick):
`cb[10]` 64B = a view matrix `[-1 0 0 8.3][0 -1 0 -10][0 0 1 17.1][0 0 0 0]`; `cb[12]` 256B and
`cb[13]` 512B = scaled rotation+translation matrices. The per-object world/skin matrix (b12) is a
fresh large CB (e.g. 3360B, row0 `[-0.922 0.378 -0.081 10.732]`). The view-projection the shaders
want as cb0 is one of these recycled globals.

**Experiment in place:** `PELOAD_VP0..VP3=<cbreg index>` force-binds a chosen registry buffer into
unbound VS slots 0..3 in `d3dgl_draw`, to identify the view-proj empirically (sweep cb[10/12/13]).
New env tooling this session: PELOAD_TRACE_CB (now logs each unique stage/start/num + guest caller),
PELOAD_D3DGL_DRAWLOG (per-distinct-VS refs-vs-bound), PELOAD_CB_DUMP (CB registry + view-proj scan),
PELOAD_VP0..VP3 (force-bind experiment). Next: once the real cb0 (and cb1–3) are identified, either
wire them automatically in d3dgl, or revive halo3's material-bind path (find the gate that skips the
0x766xxx vtable dispatch).
