# Reclaimer-Mac shim — status

Goal: run Project Reclaimer's **dedicated server** on Apple Silicon (M4 Max)
**without Wine**, by loading the Windows PE binaries directly into a
macOS/Rosetta process and providing the Windows APIs they call. Graphics are
out of scope for the server (the engine DLL delay-loads d3d11/dxgi/xaudio and
a headless server never calls them).

## Proven so far (2026-10-10)

A from-scratch Win64 PE loader (`src/peload.c`, ~260 lines, builds as an
x86_64 Mach-O, runs under Rosetta) does the following on the **real
`halo3.dll`** shipped in MCC:

- maps the 72 MB image, applies **45,792** DIR64 base relocations
- binds all **375** imports to logging stubs; decodes the 7 delay-import DLLs
- installs a minimal TEB/PEB via the `%gs` overlay (works under Rosetta —
  verified separately that `gs:[0x30]`/`[0x58]`/`[0x60]` are writable and
  libc survives)
- runs the CRT init chain (`_initterm`, `InitializeSListHead`, …) and
  **`DllMain` returns `TRUE`**

That is real Halo 3 engine code executing natively-translated on the Mac,
no Wine, no wineserver, no prefix.

On the **dedicated-server exe** the loader relocates (22,919 fixups), binds
302 imports, runs the TLS callback, enters `mainCRTStartup`, and faults at a
precise, expected spot: `__p___argv` / `_get_initial_narrow_environment`
return NULL from our stubs, so the Rust runtime dereferences a null `argv`.

## MILESTONE 11: audio init survives; engine reaches the NATIVE-TRANSPORT gate (2026-10-11)

With shaders compiling (M9) and the engine's self-patch landing (M10), the
native client worker now runs all the way through graphics + audio init:

```
In-game menu renderer: Apple M4 Max
Persistent menu created in native client PID …
Native frame cap disabled; client frame-rate setting controls presentation.
Native graphics quality hooks installed.
Menu music ready: 8 of 8 songs, 0.02s
Standalone player preferences use native defaults …
Menu music output unavailable: No music playback device
Initialized standalone Rust window/native graphics contract; no MCC executable launched.
Error: Native transport instruction/import mismatch; refused   <-- current blocker
```

**Audio fix (WASAPI COM).** Reclaimer's audio crate `cpal-0.15.3` does
`CoCreateInstance::<IMMDeviceEnumerator>(&MMDeviceEnumerator, …).unwrap()` in
`get_enumerator()` (wasapi/device.rs:851). Our `CoCreateInstance` was a no-op
auto-stub → cpal got an `Err` → `rust_panic`. Implemented a minimal **but real**
WASAPI COM surface in `peload.c`: an `IMMDeviceEnumerator` vtable whose
`EnumAudioEndpoints` returns an **empty** `IMMDeviceCollection` (count 0) and
whose `GetDefaultAudioEndpoint` returns `E_NOTFOUND`, plus
`CoInitializeEx`/`CoUninitialize`/`CoTaskMemAlloc`/`CoTaskMemFree`/
`PropVariantClear`. cpal now reports "no output device" gracefully (silent
menu) instead of panicking. COM objects are `{ void **vtbl; }` with ms_abi
methods (`this` in rcx, 32-bit HRESULT return).

**Current blocker — the `--native-udp` integrity gate.** The experimental
native client runs a locked flag chain: `--client-window` ⇒ `--guardian-slayer`
⇒ `--native-udp` ⇒ (`--startup --shell-callbacks --skip-render-warmup`), so the
window path can't drop native-udp. Before wiring its loopback UDP transport the
worker runs an **in-memory integrity check** that reads halo3.dll's code/IAT
**directly** (no shimmed API calls between window-init and the refusal) and
compares against the "supported build". Sibling string: `XAudio2 import slots
differ from the supported build; refused`. Related strings cluster:
`Cannot protect native transport worker memory` / `Cannot finalize native
transport worker patch` / `Invalid native transport patch size` / `ws2_32.dll`
/ `Winsock module missing` / `bind`.

halo3.dll imports **WS2_32.dll** (IAT at RVA `0x6fa6b8`, ~29 funcs, mostly by
ordinal: 1,2,3,4,7,8,9,10,11,13,14,15,16,17,18,19,20,21,22,23,52,57,111,115,
116,151 + `WSAIoctl`,`getaddrinfo`,`freeaddrinfo`). Our loader binds those IAT
slots to stub pointers; the check appears to validate the slots / the
instruction bytes they point at against the real Winsock, which we fundamentally
can't reproduce (we're not Windows). See `docs/native-transport-gate.md` for the
disassembly and options.

NOTE: even past this gate, a *visible* window still needs work — our
`CreateWindowExW` is a stub (fake HWND) and GL renders to an **offscreen CGL
context**, so nothing is shown on the Mac screen yet. Making a real Cocoa/NSWindow
with the CGL context bound to it is a separate task (see docs).

## MILESTONE 10: engine self-patches its own .text (VirtualProtect + FlushInstructionCache) (2026-10-11)

The engine installs **inline hooks into halo3.dll's own `.text`** (the
"private match experiment patch": 5-byte detours at `0x1802d27bc`, `0x2d2a36`,
`0x2d2cb1`, `0x279cfe`). Two shim bugs blocked it:

1. **SIGBUS on the patch write.** `i_VirtualProtect` passed the raw, *unaligned*
   target address straight to `mprotect`, which requires a **page-aligned base**
   → `EINVAL`, which we ignored and reported success. The page stayed `r-x`, so
   the engine's inline `movb/movl` write to `.text` faulted (`EXC_BAD_ACCESS
   code=2`, i.e. a write to a non-writable page). Fix: page-align base + round
   length (RWX actually *succeeds* in this Rosetta process — `r1=0(ok)` — no
   fallback needed; RW fallback kept for safety).
2. **"Cannot finalize patch" after the write landed.** `FlushInstructionCache`
   was an auto-stub (no-op). Under Rosetta the x86 code is JIT-translated to
   arm64 and **cached**, so writing new bytes to a code page leaves a **stale
   translation**; the engine's finalize/verify ran the old code. Fix: implement
   `FlushInstructionCache` → `sys_icache_invalidate(base,size)`, the signal
   Rosetta honors to drop and re-translate.

After both: all four hook sites patch + finalize, and the engine proceeds to
audio init (M11).

Key Rosetta insight: self-modifying guest code needs the page **writable** for
the store and `sys_icache_invalidate` afterward; RWX is permitted here.

## MILESTONE 9: GLSL #version rewrite on the REAL call path — menu shaders compile (2026-10-11)

macOS Core profile rejects `#version <150`; Reclaimer ships egui/glow shaders
at `#version 130/140`. We already had `i_glShaderSource`/`i_glCompileShader`
wrappers that bump the version, special-cased by **name** in every GL resolver
— but lldb proved the engine reached the *real* `glShaderSource` with an
un-rewritten `#version 140`. The engine requests the function under a name our
`strcmp`s didn't catch, so it fell through to the generic `make_gl_thunk`
(ABI-only passthrough, no rewrite).

Fix: intercept by **resolved pointer** inside `make_gl_thunk` — compare the
`dlsym` result against the real `glShaderSource`/`glShaderSourceARB`/
`glCreateShaderProgramv` and return the version-rewriting wrapper instead of a
raw thunk. Now every upload path is covered regardless of the alias used. Log
shows `[glShaderSource] rewritten` ×N and the engine advances past shader
compile to `In-game menu renderer: Apple M4 Max` and `Persistent menu created`.

## MILESTONE 8: ABI-thunk fix — crash gone; engine reaches SHADER COMPILE

Root-caused and fixed the glow crash, and the engine now runs all the way to
real GLSL shader compilation. Only one precise blocker remains.

**The crash was a Win64↔System V ABI mismatch.** macOS GL functions are SysV
(args in rdi,rsi,rdx,rcx…; rsi/rdi are *volatile*); the engine calls them
Win64 (rcx,rdx,r8,r9; rsi/rdi *nonvolatile*). Calling a raw dlsym'd GL function
directly passed args in the wrong registers AND let it clobber rsi/rdi — which
held glow's live `Context` → garbage `self` → crash. Fix: `make_gl_thunk`, a
generated Win64→SysV thunk (shuffles the 6 integer arg registers, preserves
rsi/rdi) wrapping every GL pointer handed to the engine. glGetString also gets
an ms_abi wrapper that normalizes macOS's "4.1 Metal - 90.5" to "4.1.0".

With that, the worker runs past glow into the renderer and the Apple GLSL
compiler reports:

> Character shader: ERROR: 0:1: version '130' is not supported

**The one remaining blocker is GLSL `#version 130`.** macOS Core profile only
accepts `#version 150+`; the engine ships 130 shaders (the exe embeds
#version 100/120/130/140/300es variants and picks 130). Fix = rewrite the
version directive (130→150, same length) at shader upload. I added wrappers
(i_glShaderSource / i_glCompileShader) and a DYLD-interpose dylib (glfix.c),
but neither fires: the shader-upload call reaches the real macOS GL without
passing through any of our resolution paths (GetProcAddress, wglGetProcAddress,
IAT bind, or make_gl_thunk — all instrumented, all show zero for the shader
functions) and __interpose doesn't catch pointer-based calls. Pinning the
actual upload site needs an lldb breakpoint on the real glShaderSource /
glCreateShaderProgramv to see who calls it and rewrite there.

Also added: a reaped-child exit-code cache (GetExitCodeProcess no longer
fabricates garbage), and worker spawn now execs peload directly (not via
/usr/bin/arch, which stripped DYLD_* and is why env/DYLD didn't reach the
worker). Both --version green.

## MILESTONE 7: real threading/sync; engine reaches egui/glow menu GL setup

Made the multi-threaded engine deterministic and drove it into the menu
renderer. Did NOT yet reach a visible startup screen — crashes in glow.

Fixes this pass:
- **Real synchronization** (was no-ops): CRITICAL_SECTION→recursive
  pthread_mutex, SRWLOCK→pthread_rwlock, CONDITION_VARIABLE→pthread_cond,
  Events/Mutexes/Semaphores→a pthread-backed SyncObj, real WaitForSingle/
  MultipleObjects. This removed the run-to-run nondeterminism.
- **Process exit-code bug**: WaitForSingleObject + GetExitCodeProcess both
  waitpid'd, double-reaping and fabricating a garbage status (0xC000002F).
  Added a reaped-child cache; the worker's true status now shows (SIGSEGV).
- **Crash diagnostics**: a SIGSEGV/BUS/ILL handler prints the faulting RIP as
  module+offset and a manual Win64 stack scan (backtrace() can't unwind the
  guest ABI).
- GetProcAddress/wglGetProcAddress route gl*/wgl* to OpenGL.framework; a
  glGetString wrapper normalizes macOS's "4.1 Metal - 90.5" to "4.1.0".

Where it crashes (precisely isolated): the worker reaches the egui/glow menu
GL init, reads GL_VERSION/RENDERER/VENDOR fine, then calls
`glGetString(GL_SHADING_LANGUAGE_VERSION)` **through a glow `Context` whose
`self` pointer is garbage** (disassembled: `mov rax,[rdx+0x9c8]; call rax`,
rdx = Context, is a bad low/heap address that varies per run). So glow's
Context (or the engine's reference to it) is uninitialized/corrupted by this
point. Confirmed NOT a GL-version issue (identical at 4.1 and 4.6).

Honest next steps toward the screen: (a) run the worker under lldb staged with
the scratch cwd + RECLAIMER_LOCAL_RUNTIME=menu:0 to single-step where the glow
Context pointer is clobbered; (b) stand up a real NSWindow + NSOpenGLContext
drawable (needed for on-screen output regardless). ~288 shims; both --version
green.

## MILESTONE 6: the C++ throw decoded — it was ours, now fixed; engine runs GL

Answered the "error or control flow?" question by **decoding the MSVC C++
throw** in `RaiseException` (code 0xE06D7363): read its `ThrowInfo` RTTI
(imagebase-relative RVAs) to get the thrown type and message. Result:

> C++ type: **`rust_panic`** — glow-0.16 `native.rs:59`: *"Reading GL_VERSION
> failed. Make sure there is a valid GL context currently active."*

So it was **neither an engine error nor control flow** — it was a `glow`
(Rust OpenGL wrapper) panic because our shim wasn't resolving `glGetString`.
`GetProcAddress`/`wglGetProcAddress` now route `gl*`/`wgl*` to macOS
OpenGL.framework (`dlsym`), and a `glGetString` wrapper makes our CGL context
current on the calling thread first. glow now reads real GL state:

> GL_VERSION "4.1 Metal - 90.5", GL_RENDERER "Apple M4 Max", GL_VENDOR "Apple"

The engine runs **past** the panic into its GL render setup. The C++-throw
decoder stays in as a diagnostic; full Win64 SEH *dispatch* (catching a real
throw) is still unimplemented and only needed if the engine later throws for
control flow — this one was a bug we removed, which beats catching it.

Current frontier: multi-threaded GL — the engine issues GL from more than one
thread and CGL contexts are per-thread, so GL work needs the context current
per GL-calling thread (and, for on-screen output, a real drawable: an
offscreen FBO or a hidden `NSOpenGLContext`/`NSWindow`).

## MILESTONE 5: engine runs through graphics init; OpenGL context created

Drove the engine worker from "loaded" all the way through its native startup
and graphics init — it now creates a real OpenGL context and executes its
render setup. The march (each a real shim, build with `./build.sh`):

- native-startup handshake: parse `CreateProcessW`'s `lpEnvironment` block so
  `RECLAIMER_LOCAL_RUNTIME=menu:0` / `RECLAIMER_PARENT_CONTROL` reach the
  worker — clears "Invalid local runtime configuration";
- `AddVectoredExceptionHandler`/`SetUnhandledExceptionFilter`/`SetErrorMode`
  succeed — clears "Could not install worker fault handler";
- `RECLAIMER_MOUSE_SENSITIVITY` default + locale/codepage shims;
- windowing: `RegisterClassExW`, `CreateWindowExW`, `MonitorFromWindow` +
  `GetMonitorInfoW` (1920×1080), `SetWindowPos`, `RegisterRawInputDevices`,
  layered-window/DC — the engine prints **"Graphics quality: one view High…"**;
- **OpenGL bridge: `wgl*` → CGL and `opengl32!gl*` → macOS OpenGL.framework
  (`dlsym`). `wglCreateContext` creates a real `CGLContextObj`** — "[wgl] CGL
  context created".

Current frontier: the engine throws a **C++ exception (`RaiseException`
0xE06D7363)**, which needs real **Win64 SEH dispatch** — walking `.pdata`/
`.xdata` unwind info and running the MSVC `__CxxFrameHandler` personality to
reach the `catch`. That's the substantial remaining subsystem. Beyond it:
giving the GL context a real drawable (offscreen FBO or a hidden `NSWindow`)
so the menu actually presents, and eventually D3D11→Metal for in-game.

~250 shims. Build: `./build.sh` (links `-framework OpenGL`). Both `--version`
green. Debug env: `PELOAD_WORKER_INHERIT_STDIO`, `PELOAD_TRACE_FS/_ALL/_VA/_ENV`,
`PELOAD_NO_THREADS`.

## MILESTONE 4: both named frontiers fixed; engine (halo3.dll) initializing

Fixed the two concrete issues from Milestone 3, then pushed into the engine:

**1. Dir-enumeration loop — FIXED.** It was delete-on-close semantics: the
client cleans the scratch by deleting entries and re-scanning. Fixes:
refcounted last-handle-wins delete (files *and* dirs), honoring the
delete-flag *clear* (`FileDispositionInfoEx` flag 0x1), and — the real
trigger — mapping the Windows `NUL` device to `/dev/null` (the client opened
`\\.\NUL` for worker stdio and aborted when it failed).

**2. Worker process via `CreateProcessW` — IMPLEMENTED.** The engine runs in a
child process: `project-reclaimer probe-engine-worker "<halo3.dll>"
--guardian-slayer --client-window --startup --shell-callbacks --native-udp`.
`CreateProcessW` now `posix_spawn`s the worker **under the loader** (re-exec
`arch -x86_64 peload <exe> <args…>`), wiring child stdio from `STARTUPINFOW`,
the working directory from `lpCurrentDirectory`, and real process handles for
`GetExitCodeProcess`/`WaitForSingleObject`. Needed `DuplicateHandle`,
`SetEnvironmentVariableW`, `GetCurrentDirectoryW` (real cwd), command-line
re-quoting, and an absolute loader path (child `chdir`s).

**Then — the engine itself starts.** The worker:
- loads the genuine CRT, runs `probe-engine-worker`, and `LoadLibraryExW`s the
  real **`halo3.dll`, mapped at its preferred base `0x180000000`** (CRT guests
  are kept off that address so Reclaimer's hardcoded engine pokes line up);
- resolves `halo3.dll!CreateGameEngine` and reads the engine config via
  `ReadProcessMemory`/`WriteProcessMemory` (in-process memcpy — the
  shell-callback patch mechanism);
- **reserves its 64 MB "near the game"** — `VirtualQuery` (backed by
  `mach_vm_region`) now lets the engine's free-memory scan succeed.

Current engine frontier: `Error: Invalid local runtime configuration` — a
Reclaimer handshake validating the native-startup role after memory setup
(remaining stubs: `CreateIoCompletionPort`, `GetCurrentPackageId`). Beyond it:
the shell-callback install, then rendering (window + GL/D3D→Metal).

190 shims. Both `--version` still green. Debug env: `PELOAD_WORKER_INHERIT_STDIO`
(worker shares terminal), `PELOAD_TRACE_FS` / `_ALL` / `_VA`, `PELOAD_NO_THREADS`.

## MILESTONE 3: client reaches engine-worker launch; FS bridge complete

Built the full filesystem/handle bridge (the "it's a day, not weeks" piece —
correct). The client now runs end-to-end through userland and reaches the
point of launching the native engine:

- **MCC fully discovered & validated**: opens the real `halo3.dll` and the
  entire Halo 3 map set (campaign + every MP map + `.mapinfo`) through the
  path-translating FS shim.
- **Private runtime scratch**: creates `mcc-startup-<uuid>`, builds the
  `halo3/maps/info` mirror tree (auto-`mkdir -p`), and logs
  *"Private runtime scratch (removed after worker exit)"* — i.e. it's ready
  to hand the scratch to the engine worker.

Added this turn (~215 shims total): `GetFileInformationByHandleEx` directory
enumeration, `NtCreateFile`/`NtOpenFile` (incl. handle-relative opens),
`RemoveDirectoryW` (recursive), `SetFileInformationByHandle` (delete),
`CopyFileExW`/`CopyFileW`/`CreateHardLinkW`, a handle→path map, delete-on-close
(file-only, Windows refcount semantics), real `GetLastError` + NT-status
translation, `FindNextFileW` end-of-enumeration error, drive/volume queries,
`FormatMessageW`, a full-call tracer (`PELOAD_TRACE_ALL`) and a
`PELOAD_NO_THREADS` diagnostic.

Two concrete frontiers remain for the client:
1. **Dir-enumeration semantics loop (immediate):** the client's recursive walk
   of the scratch loops because our enumeration omits `.`/`..` and
   `FindFirstFileW` treats a trailing path component as a pattern. Fix:
   Windows-accurate `FindFirstFileW`/`read_dir` semantics.
2. **Worker process (architecture):** the engine (`halo3.dll` + D3D11 +
   graphics) runs in a child process the client spawns via `CreateProcessW`
   with the `--startup`/`--shell-callbacks` native-worker flags. Implementing
   `CreateProcessW` means launching that worker *under the loader* (re-exec
   peload on the worker exe). Then the worker needs Win64 SEH and the
   graphics bridge — the genuinely multi-week part.

## MILESTONE 2: the graphical CLIENT runs its full userland init, Wine-free

`project-reclaimer-v0.9.13.exe` (the GUI client) now boots through essentially
all of its pre-engine initialization under the loader, no Wine:

- `profile create TestPlayer` **works end-to-end** — generates a real community
  identity (public key via OS RNG), seals the seed with a DPAPI passthrough,
  and writes a valid 565-byte `profile.json`.
- A normal launch runs: logging, cache setup, **MCC auto-discovery** (reads the
  real `halo3.dll`, `campaign.map`, `guardian.map`, `shared.map` from the
  prefix via the FS shim), profile load, Ctrl-C handler, **real threads**
  (`CreateThread`→pthread, each with its own TEB/TLS), env handling, and into
  native-engine startup prep (creating `mcc-startup-<uuid>` scratch dirs).

This required a real support layer, all genuine-MS-CRT-backed, ~160 shims:
- **Filesystem**: Windows→Unix path translation (C:\ → prefix/drive_c, Z:\ → /),
  `CreateFileW` (all 5 dispositions), `ReadFile`/`NtReadFile`,
  `WriteFile`/`NtWriteFile` (fd-routed), `GetFileAttributes[Ex]W`,
  `FindFirstFileExW`/`FindNextFileW` (real `opendir`/`readdir` + glob),
  `GetFileSizeEx`, `SetFilePointerEx`, file locking (no-op), `MoveFileExW`.
- **Steam/MCC discovery**: `RegGetValueW` (SteamPath), a generated
  `libraryfolders.vdf` + `appmanifest_976730.acf` in the prefix.
- **Identity**: `BCryptGenRandom`/`ProcessPrng`/`RtlGenRandom`,
  `CryptProtectData`/`CryptUnprotectData` (passthrough), `LocalAlloc/Free`.
- **Threads/sync**: `CreateThread` (pthread + per-thread TEB), events,
  semaphores, `WaitForSingleObject`, `SetConsoleCtrlHandler`.
- **Misc**: real `GetLastError` (errno→Win32), `GetEnvironmentVariableW`
  (host env), `GetLocalTime`, `CompareStringOrdinal`, temp-path, console.

Current client frontier: `remove_dir_all` of the scratch dir needs real
`GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` enumeration; then the
client spawns/loads the native MCC engine worker — which needs Win64 SEH
(`RtlLookupFunctionEntry`/`RtlVirtualUnwind`) and, for actual rendering, a
window + an OpenGL/D3D→Metal bridge. Those remain the big phases.

Both binaries load the genuine MS CRT from `shim/crt/` (project-relative,
`$RECLAIMER_CRT_DIR` overridable). FS trace: `PELOAD_TRACE_FS=1`.

## MILESTONE: the Rust dedicated server runs, Wine-free (2026-10-10)

The genuine Microsoft CRT is now loaded as guest PEs and the dedicated
server's CLI executes:

```
$ arch -x86_64 build/peload project-reclaimer-dedicated-v0.9.13.exe --version
project-reclaimer 0.9.13
```

`--help` and subcommand parsing (`discovery-bootstrap --help`) all work. The
Rust binary reaches `main`, runs its clap parser, and writes correct output —
on an M4 Max under Rosetta, no Wine, no wineserver, no prefix.

How it fits together now:
- `peload` maps the exe, sets up TEB/PEB/TLS via the `%gs` overlay.
- It loads genuine `ucrtbase.dll` + `vcruntime140.dll` + `msvcp140.dll`
  (extracted from the MS VC redist, zero Wine markers) as guest PEs, binds
  their imports, runs their DllMains.
- The exe's CRT imports resolve by name against those guests' real export
  tables (forwarders followed); `kernel32`/`ntdll`/core calls resolve to ~90
  native ms_abi shims (heap→malloc, file/console I/O→fd, `NtWriteFile`→write,
  locks→no-op single-thread, FLS/TLS, `RtlPcToFileHeader`, time, argv via
  `GetCommandLineW`).
- Loading genuine ucrtbase collapsed the entire hand-stub march: the
  unresolved surface dropped to a handful of real OS calls.

Next frontier: the `local-server` hosting path, which will `LoadLibraryExW`
the real `halo3.dll` + MCC engine (our loader already maps that DLL's DllMain
successfully). That pulls in full Win64 SEH/stack-unwinding
(`RtlLookupFunctionEntry`/`RtlVirtualUnwind`/`.pdata`) — the known hard part.

## Boot march (dedicated exe, with real bootstrap impls)

`peload.c` now implements ~45 bootstrap functions (argv/argc/environ, a real
`_initterm` that calls the init table, time, a malloc-backed Win32 heap,
mmap-backed VirtualAlloc/Protect, TLS, the `__p__*` stdio/locale accessors,
and `GetProcAddress` that hands back live stubs). Each function added walks
the Rust server's startup a few calls further — an empirical, repeatable
march:

| after implementing            | boot reaches                               |
|-------------------------------|--------------------------------------------|
| (NULL stubs)                  | `__p___argv` → null argv deref             |
| argv/environ/`_initterm`      | `__p__commode` (stdio globals)             |
| `__p__*` stdio/locale         | `SetUnhandledExceptionFilter`, `_set_new_mode` |
| …                             | marches on, one function at a time         |

This confirms the surface is finite and mechanical, but hand-implementing it
one accessor at a time has diminishing per-step value.

## Next step (bounded, clear)

The boot march above proves option 1 works but is tedious. **Decision: do
option 2 next** — load the real Microsoft ucrt as a guest PE:

- recursively map `ucrtbase.dll` + `vcruntime140.dll` + `msvcp140.dll` (all
  present in the MCC folder / VC redist depot 228987) with the same loader
- they import essentially only kernel32, so this turns ~100 CRT functions
  (every `__p__*`, stdio, heap, locale accessor) into *real* code at once
- we then own only kernel32 / ntdll / ws2_32 / winmm / bcrypt + a few user32
  stubs — a far smaller, non-accessor surface

This requires the loader to (a) load a dependency DLL by name, (b) run its
DllMain, (c) resolve the main exe's CRT imports against the loaded DLL's
export table instead of our stubs. That export-table resolver is the one
new piece of loader machinery; everything else is already here.

After CRT: kernel32 heap+sync+TLS, ntdll, then ws2_32→BSD sockets and the
tokio AFD/IOCP poll → kqueue bridge. Then `halo3.dll`'s exported engine
entry points. SEH unwinding is the known hard part and comes when engine C++
starts throwing.

## Files
- `src/peload.c` — the loader / diagnostic
- `docs/security-rcon.md` — RCON + wire-protocol review
- build: `clang -arch x86_64 -O1 -o build/peload src/peload.c`
- run:   `arch -x86_64 build/peload <path-to.pe>`
