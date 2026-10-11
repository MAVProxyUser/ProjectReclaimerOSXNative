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
