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

## Next step (bounded, clear)

The wall is the C-runtime bootstrap, ~20 functions. Two options:

1. **Implement them** — `__p___argv`/`__p___argc`/`_get_initial_narrow_environment`
   return real argv/argc/environ; `_initterm[_e]` actually walk and call the
   init table; real `GetSystemTimeAsFileTime`/`QueryPerformanceCounter`.
2. **Load the real Microsoft ucrt as a guest PE** (preferred) — recursively
   map `ucrtbase.dll` + `vcruntime140.dll` + `msvcp140.dll` (all present in
   the MCC folder / depot 228987). They import essentially only kernel32, so
   this collapses ~100 CRT functions into real code and leaves us owning just
   kernel32/ntdll/ws2_32/winmm/bcrypt + a handful of user32 stubs.

After CRT: kernel32 heap+sync+TLS, ntdll, then ws2_32→BSD sockets and the
tokio AFD/IOCP poll → kqueue bridge. Then `halo3.dll`'s exported engine
entry points. SEH unwinding is the known hard part and comes when engine C++
starts throwing.

## Files
- `src/peload.c` — the loader / diagnostic
- `docs/security-rcon.md` — RCON + wire-protocol review
- build: `clang -arch x86_64 -O1 -o build/peload src/peload.c`
- run:   `arch -x86_64 build/peload <path-to.pe>`
