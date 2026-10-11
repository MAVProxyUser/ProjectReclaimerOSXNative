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
