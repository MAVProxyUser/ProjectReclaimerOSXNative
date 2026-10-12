# Reclaimer menu re-entrancy — RE findings (session 5)

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
- Tracer caveat: single-step re-arm / int3 DON'T work under Rosetta; the detour
  (PELOAD_HOOK) is the working mechanism. Hook only sites whose first >=13 bytes are
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
