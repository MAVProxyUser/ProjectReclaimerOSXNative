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
