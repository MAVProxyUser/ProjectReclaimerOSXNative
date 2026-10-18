# Fix catalogue

Every fix made in this repository, so none gets lost: the bug or gap, where it was fixed, why, and the logic of the
fix. Sources: the commit messages (`git log`), CLAUDE.md, shim/docs/* (BACKLOG Done, PERF-PLAN, patch-addresses,
RE-findings-menu-reentrancy, workshop-helper, teleport, ...), checked against the code.

**Rule:** every future fix gets an entry here **in the same commit** as the fix, in the same format, in the right
section. When a fix replaces an earlier one, mark the earlier entry "superseded by ..." instead of deleting it.

**Entry format:** `- **name** — where. Why: symptom; cause. How: the logic. Old: switch that restores the old behaviour
(or Switch: an opt-in). (MM-DD commit)`.

- Dates are October 2026 (`10-16` = 2026-10-16). Commit **`da262fb`** (10-14) bundles three days of work (10-12 .. 10-14,
  RE-log sessions 7-16: ~3,000 lines of peload.c); entries from those days cite it.
- `exe+0x...` = client exe RVA (ImageBase 0x140000000) of the build named; `halo3+0x...` = MCC halo3.dll RVA
  (SHA-256 b209d845..., the only build available). Fixed RVAs are valid for one build only (patch-addresses.md).
- "Old:" switches are environment variables read by the loader (`PELOAD_X=1` unless a value is given).
- Diagnostics-only switches (traces, dumps, censuses) are not listed unless they were part of a fix.
- Where a detail could not be confirmed it says so (UNCONFIRMED).

Sections: 0 Install, setup and usage · 1 PE loading and the API surface · 2 Processes, threads, exceptions · 3 Memory · 4 Files, pipes, console ·
5 Sync objects and timers · 6 Networking · 7 Windows, input, focus · 8 GDI and layered windows · 9 Audio ·
10 D3D11 / DXGI objects · 11 D3D11->GL: shaders and the translator · 12 D3D11->GL: textures · 13 D3D11->GL: buffers,
state, draws, queries · 14 GL context, present, GL thunks, GL helper · 15 CPU particle compute · 16 Client exe patches
and hooks · 17 halo3.dll patches and detours · 18 Overlay, stats, forensics, walker, recording · 19 Performance ·
20 Scripts and RE tools · 21 Superseded workarounds · 22 Known open (not fixed).

---

## 0. Install, setup and usage

The short version (README.md has the full guide: requirements, USB copy, controls, the stats page).

### Install
```bash
softwareupdate --install-rosetta --agree-to-license     # Rosetta 2 (the game code is x86-64)
xcode-select --install                                  # clang, only to build from source
git clone <this repository> ~/ReclaimerMac && cd ~/ReclaimerMac
bash shim/build.sh                                      # -> shim/build/peload (x86-64) + shim/build/glhelper (arm64)
```
Building from source also needs `ucrtbase.dll`, `vcruntime140.dll` and `msvcp140.dll` in `shim/crt/` (the release bundle
has them). A release bundle instead: unpack it and `xattr -dr com.apple.quarantine ProjectReclaimerOSXNative-*-macos`.

### Setup (once)
```bash
./reclaimer-mac.sh download                 # MCC's Halo 3 files with DepotDownloader, your own Steam login (QR code)
bash shim/tools/client-update.sh            # the newest Reclaimer client -> client/, SHA-256 checked against the release
bash shim/tools/client-update.sh v0.9.18    # an older build beside it (to compare; servers want the newest)
bash shim/tools/maptop.sh                   # optional: map pictures for the stats page heatmap (python3, numpy, Pillow)
```
The first launch of a new client version builds its Forge asset library (~2.5 minutes, "BUILDING ASSET LIBRARY").

### Usage: startup examples
```bash
bash shim/play.sh                                         # menu (server browser, host, settings)
RECLAIMER_JOIN=69.9.183.16:38164 bash shim/play.sh        # straight into a server (its join link)
bash shim/tools/servers.sh                                # the busiest servers, their addresses and mods
RECLAIMER_EXE=client/project-reclaimer-client-v0.9.18.exe bash shim/play.sh   # a specific client build
RECLAIMER_DIRECT=1 bash shim/play.sh                      # skip the menu: a local Guardian match
PELOAD_INVERT_PITCH=0 PELOAD_MIC=0 bash shim/play.sh      # normal look, silent microphone
PELOAD_HB_ZONES=1 PELOAD_HITBOXES=1 bash shim/play.sh     # overlay: map boundaries + hitboxes (also stats page buttons)

bash shim/dedicated.sh                                    # a local dedicated server (Valhalla Slayer, UDP 49190)
RECLAIMER_JOIN=127.0.0.1:49190 bash shim/play.sh          # join it (first join after a start can time out: retry)
DED_CONFIG=mapchange.toml bash shim/dedicated.sh          # three 1-minute games (map-change tests)
bash shim/dedicated.sh status; bash shim/dedicated.sh stop

bash shim/target.sh                                       # an input-less second player hosting a game ...
RECLAIMER_JOIN=127.0.0.1:11774 bash shim/play.sh          # ... to join and shoot at

bash shim/tools/quiet-run.sh "PELOAD_D3DOBJ_LOG=1" 600 "reclaimer://join?server=127.0.0.1:49190"   # unattended test client

bash shim/tools/make-usb.sh /Volumes/<stick>              # copy everything to a USB stick; run again to UPDATE it (only changes)
bash /Volumes/<stick>/ReclaimerMac/install.sh             # on the other Mac (again after each update: only changes)
```
Requirements: an Apple Silicon Mac (M1 or newer) with Rosetta 2 and macOS 13 or newer (the binaries are built for 13.0;
tested on macOS 26).
While a game runs: live stats at http://127.0.0.1:8765/ (players, heatmap, overlay switches, recording) and the forensic
archive at http://127.0.0.1:8765/forensics. Logs: `/tmp/reclaimer_play.log` (play.sh), `/tmp/quiet-run.log`,
`/tmp/reclaimer_dedicated*.log`. Several client builds can sit in `client/` side by side: the launchers take the newest unless
`RECLAIMER_EXE` (play.sh, dedicated.sh, target.sh) or `QR_EXE` (quiet-run.sh) names one, and the loader picks each build's
patch variants by signature (section 16; shim/docs/patch-addresses.md).
---

## 1. PE loading and the API surface

- **Static TLS block** — peload.c TLS setup (TEB+0x58, gs:0x58 overlay). Why: the dedicated exe faulted (rcx = 0) at its
  first `__declspec(thread)` access. How: build the static TLS block from the image's TLS template, TLS index 0, publish
  the array at TEB+0x58 and the gs:0x58 overlay. (10-10 adeb8cb)
- **Multi-module static TLS** — `register_tls`, `thread_tramp`. Why: only the exe's TLS directory was set up;
  halo3.dll's `_tls_index` stayed 0, so halo3 read the exe's block (singleton accessor halo3+0xa505c faulted at
  halo3+0xa50ae, address 0x2). How: every module with a TLS directory gets its own index from the counter TlsAlloc
  also uses, its `_tls_index` is written, and each new thread gets a block for every registered index. (10-12 ce6e4d0)
- **TLS of DLLs loaded later** — Why: v0.9.18 loads MCC's Waves DLLs; WavesLibDLL read NULL from
  ThreadLocalStoragePointer on an existing thread. How: a DLL's static TLS registered after start is given to every
  existing thread. (10-16 a574219)
- **One ws2_32 resolution table** — `g_ws2ord`, `WS2_HANDLE`, `bind_imports`, `i_GetProcAddress`. Why: the client's
  native-transport gate compares halo3's WS2_32 IAT slot (bind, ordinal 2) with GetProcAddress(ws2_32, 2); each path
  made its own stub -> "Native transport instruction/import mismatch; refused". How: every ws2_32 function resolves
  through one ordinal/name table; GetModuleHandle/LoadLibrary("ws2_32") return a distinct WS2_HANDLE. (10-11 2bfe516)
- **Stable kernel32 pointers for the client's import check** — `resolve_impl`. Why: the Forge library builder checks
  that halo3's CreateFileW/CreateFileA/CloseHandle/GetFileSizeEx/CreateIoCompletionPort/ReadFileEx/ReadFile imports equal
  GetProcAddress(kernel32, name) (exe+0x4b9480, v0.9.13); missing names got a fresh stub per call -> "halo3.dll's
  CreateFileA import is not kernel32's". How: CreateFileA, ReadFileEx, CreateIoCompletionPort added to resolve_impl,
  so both paths return one pointer. (10-12 e6e2b02)
- **Delay imports bound** — import binder. Why: no `__delayLoadHelper2`, so halo3's dxgi/d3d11 delay slots were NULL.
  How: delay-import tables are bound up front like normal imports. (10-11 2a8f65e)
- **Guest-to-guest imports** — Why: imports of one guest DLL from another (l360/q10 -> WavesLibDLL) bound to stubs.
  How: they bind to the real exports. (10-16 a574219)
- **GetModuleFileNameW/A, GetModuleHandleExW, GetModuleHandleW for guest DLLs** — Why: WaveShell-Xbox.dll looked for
  L360.dll next to the exe under a `\\?\` path and boost::filesystem threw "invalid name". How: a guest DLL's own path
  in plain `C:\` form; GetModuleHandleExW(FROM_ADDRESS) returns the containing image; GetModuleHandleW finds loaded
  guest DLLs by name; module table 64 entries. (10-16 a574219)
- **GetModuleFileNameW returns the exe path** — Why: it returned the command line, so Rust `current_exe()` was wrong.
  How: the exe path in its Z:\ form (computed after the drive mapping, so it round-trips). (10-11 2a8f65e)
- **GL functions via GetProcAddress / wglGetProcAddress** — Why: glow panicked "Reading GL_VERSION failed" (decoded
  from the C++ throw 0xE06D7363 'rust_panic'): glGetString was not bound. How: gl*/wgl* names resolve through dlsym
  into OpenGL.framework; a glGetString wrapper makes the CGL context current on the calling thread and normalises the
  version string. (10-11 46468b3, 7af88c1)
- **Missing GL functions resolve to NULL** — `gl_or_missing`. Why: a named no-op stub for a function macOS lacks made
  glow call it and read garbage ("glDeleteProgram not loaded" panic). How: NULL (GL loaders null-check for
  capabilities); real wrappers kept for the functions the loader instruments. (10-11 6767c8c)
- **Stub and thunk pools permanently RWX** — `make_stub`, `make_gl_thunk`. Why: the pools were toggled RW<->RX for every
  new stub, faulting a stub that another thread was executing (random multi-thread crashes). How: permanent RWX (works
  under Rosetta), creation under a mutex, icache invalidated after writing; GL thunk pool 2048 -> 8192. (10-11 6767c8c)
- **Trace stubs safe** — `PELOAD_TRACE_ALL` / `PELOAD_CALLCOUNT` stubs. Why: toggling the pool faulted when halo3 bound
  late; CRT float calls through a trace stub got xmm0-3 clobbered. How: pool permanently RWX, xmm0-3 preserved, the
  exe's Winsock imports traced but never halo3's (the client checks those slots). (10-17 7d12a64)
- **Early API bring-up (stubs that broke the client)** — Why: the first ~300 shims were generic logging stubs returning
  0. How: real implementations, in waves: argv/environ/_initterm/heap/VirtualAlloc/TLS accessors (10-10 7e16949);
  genuine MS CRT DLLs (ucrtbase/vcruntime140/msvcp140) loaded as guest PEs with export forwarding (10-10 9c1d6ec);
  filesystem, registry (generated libraryfolders.vdf / appmanifest), BCryptGenRandom/ProcessPrng, CryptProtectData,
  CreateThread, events (10-10 4764a3e); handle layer, NtCreateFile/NtOpenFile, CopyFileExW, FormatMessageW (10-11
  090e7ce). Each step was driven by the next crash or error in the logs.
- **Stub sweep before the map catalog** — Why: the worker exited at "Unmapped shell callback" through ~29 no-op stubs.
  How: getaddrinfo family, WSASend, timeGetTime/timeBeginPeriod/CreateWaitableTimerExW, DeleteFileA,
  GetFileAttributes[Ex]A, LoadLibraryExA, SetFilePointer, SetFileTime, CreateDirectoryA, GetParent/GetFocus/
  RedrawWindow/EnumWindows/EnumDisplaySettingsW/GetWindowThreadProcessId/IsIconic, ConvertThreadToFiber(Ex)/
  ConvertFiberToThread, GetLocaleInfoEx, GetTimeZoneInformation, GetErrorInfo, RtlCaptureStackBackTrace,
  SetThreadPriority/Description, job objects, GetConsoleProcessList, GetCurrentPackageId, CompareFileTime,
  waveOutGetNumDevs, GetFileVersionInfo*/VerQueryValueW (RE log session 8). (10-12..14 da262fb)
- **BCrypt** — Why: anti-cheat hashing and the game-variant signature failed (launch inputs game_variant_valid false).
  How: SHA/MD5 via CommonCrypto (10-11 2a8f65e); BCryptImportKeyPair/ImportKey return a key handle (was NULL),
  BCryptVerifySignature STATUS_SUCCESS, BCryptDestroyKey (10-12..14 da262fb).
- **v0.9.18 small APIs** — Why: bink2w64's CRT startup crashed on a garbage GetStartupInfoA stub; other names were
  missing. How: GetStartupInfoA filled in, HeapSetInformation, LoadLibraryA, GetCurrentThreadStackLimits,
  Get{Windows,System}DirectoryA, SetHandleCount, GetKeyboardLayout, MulDiv, GetThreadUILanguage, RemoveDirectoryA.
  (10-16 a574219)
- **Stubs found in a log sweep** — `wi_MapVirtualKeyExW` (wininput.c), LCMapStringEx, RoGetActivationFactory. Why: a
  sweep of every `[stub]` line after NtDelayExecution turned out to be one. How: MapVirtualKey(Ex)(A/W) from the Mac key
  table (US layout), LCMapStringEx case mapping, RoGetActivationFactory -> REGDB_E_CLASSNOTREG, RoInitialize/
  RoUninitialize, VirtualUnlock. (10-18 ccf348f)
- **DPI and monitor APIs** — Why: GetDpiForWindow / GetDeviceCaps / IsProcessDPIAware / AdjustWindowRectExForDpi and
  shcore's GetDpiForMonitor were unresolved (0-returning stub); EnumDisplayMonitors never called its callback. How: a
  96-DPI 1920x1080 display; the callback is called. (10-16 5666cce)

## 2. Processes, threads, exceptions

- **NUL device** — path translation. Why: the client opened `\\.\NUL` for the worker's stdio; the open failed (the
  real trigger of a directory-enumeration loop). How: mapped to /dev/null. (10-11 c04b6a2)
- **Worker spawn under the loader** — `i_CreateProcessW`. Why: the engine worker could not start. How: posix_spawn of
  the loader with the child exe and args, STARTUPINFOW stdio wired, lpCurrentDirectory, real process handles,
  command-line re-quoting. (10-11 c04b6a2)
- **Child environment block** — `i_CreateProcessW`. Why: lpEnvironment was ignored, so RECLAIMER_LOCAL_RUNTIME /
  RECLAIMER_PARENT_CONTROL never reached the worker ("Invalid local runtime configuration"). How: parse the UTF-16
  block into the child's environment. (10-11 4956a43)
- **Spawn the loader directly, not /usr/bin/arch** — Why: arch stripped DYLD_* and the environment from the worker.
  How: exec the absolute loader path. (10-11 7dbbfce)
- **Exit-code double reap** — `GetExitCodeProcess`, `WaitForSingleObject` on a process. Why: both waitpid()ed, so the
  second got garbage (0xC000002F) instead of the real status. How: a reaped-child exit-code cache. (10-11 7af88c1,
  7dbbfce)
- **TSD slot 0x60 at thread exit** — thread trampoline. Why: macOS's %gs TSD slot 0x60 (Windows PEB slot) has a pthread
  destructor that free()d the static PEB at thread exit -> malloc abort. How: zero only gs:0x60 before a worker thread
  returns (0x58/0x30 stay so late thread-local destructors still find TLS). (10-11 7f5b6ac)
- **Per-thread TlsAlloc slots** — TlsGetValue/TlsSetValue. Why: one global `g_tls[256]` shared by every thread -> timing-dependent
  corruption. How: values in the TEB TlsSlots (TEB+0x1480) per thread; only the index counter is global.
  (10-11 17a3745)
- **Waitable thread handles** — `i_CreateThread`. Why: the handle was a raw pthread_t; WaitForSingleObject returned
  "signaled" at once, so halo3's parallel map load raced its own workers (non-deterministic corruption). How: a thread
  handle is a manual-reset SyncObj (K_THREAD) signaled with the exit code; GetThreadId/GetExitCodeThread read it.
  (10-11 84d68c7)
- **_beginthreadex / _beginthread** — Why: unimplemented, so halo3's ucrtbase threads never started. How: routed
  through the waitable CreateThread. (10-11 9d7df20)
- **Per-thread last error** — `g_lasterr` is `__thread`. Why: a benign ENOENT probe on one thread polluted another
  thread's GetLastError. (10-11 04aabf4)
- **GetEnvironmentVariableW miss** — Why: unset variables came back as Some("") in Rust (winit/eframe mis-detection).
  How: return 0 with ERROR_ENVVAR_NOT_FOUND. (10-11 2a8f65e)
- **Environment block and system directories** — GetEnvironmentStringsW/A, FreeEnvironmentStrings*,
  GetSystemDirectoryW, GetWindowsDirectoryW. Why: an empty `{0,0}` block (Rust saw no variables); Command::spawn's
  resolve_exe called the unregistered directory functions. How: a real UTF-16 NAME=VALUE block from `environ`;
  C:\Windows\System32 and C:\Windows. (10-14 e4fb654)
- **OpenProcess / GetProcessId** — Why: OpenProcess returned (void*)-1 and GetExitCodeProcess(-1) said "exited" (a live
  local server looked restarted); GetProcessId was a stub. How: a pid-encoded handle 0x8000+pid after `kill(pid,0)`
  (STILL_ACTIVE for a live non-child); GetProcessId decodes it. (10-12..14 da262fb)
- **GetCurrentThreadId unique per thread** — Why: it returned 0x1000 everywhere. msvcp140's std::mutex records the
  owner id and skips the lock when it matches, so every std::mutex in halo3 had no mutual exclusion; threads then
  unlocked SRW locks they did not own, leaving one held -> intermittent hang at match start in `_Mtx_lock`
  (halo3+0x21c2398 mutex). Also the "minimized client stalls loading" report. How: the mach thread port, also used by
  CreateThread's id and GetThreadId. Old: `PELOAD_TID_CONST=1`. (10-16 55811fa)
- **CreateThread reads the thread id before detaching** — Why: reading it after the detach raced the thread's exit and
  reaping. (10-18 410a6ee)
- **Guest threads detached and reaped** — `i_CreateThread`, thread reaper. Why: ~34 guest threads start and return per
  map load; they were joinable and never joined (each kept its stack), and their TEB / TLS array / TLS blocks were
  never freed; the 1024-entry TLS-array table filled after ~25 map loads. How: detached; a returned thread's TEB, TLS
  array and blocks are freed 30 s later and its array leaves `g_tarrs`. Old: `PELOAD_THREAD_KEEP=1`. (10-18 fdf5717)
- **GetSystemInfo reports the real machine** — `i_GetSystemInfo`. Why: it said ONE processor and allocation granularity
  0; the client sizes ~10 thread pools from the count, and halo3's spin-then-block locks (halo3+0x620170, count s16 at
  halo3+0xa3a48c) spin only when it is > 1 and wake one waiter at a time. How: hw.logicalcpu (or `PELOAD_NCPU`) and
  Windows' 64 KB granularity; in-match A/B +1.7%. Old: `PELOAD_SYSINFO=old`. (opt-in 10-18 353a833, default 29299e3)
- **NtDelayExecution** — `i_NtDelayExecution`. Why: GetProcAddress hands a stub to any unknown name, so the client's
  `wine_get_version` probe succeeded and it took its Wine path, sleeping with ntdll NtDelayExecution -- which was a
  no-op stub. Its voice worker (mcc-voice, exe+0x66f390 v0.9.18) spun ~210k loops/s (server: a voice-control flood of
  15-24k packets/s to clients; client: GetAsyncKeyState ~500k/s, a whole core, hammering a lock the main thread
  takes); the frame limiter (exe+0xa398e0) never slept. How: relative/absolute 100 ns intervals, alertable like SleepEx.
  128-player A/B: +11.8% median fps. Old: `PELOAD_NTDELAY_STUB=1`. (10-18 b02cc4b, 16a34d4)
- **TerminateProcess of a child** — `i_TerminateProcess`. Why: it ignored its handle and `_exit()`ed the loader (the
  client's workshop-download drop guard calls it). How: a 0x8000+pid handle kills that child and its process group when
  the loader started it in its own. Old: `PELOAD_TERMINATE_SELF=1`. (10-17 6810207)
- **CorExitProcess** — routed to `i_ExitProcess` (was a stub). (10-12..14 da262fb)
- **MessageBoxW logs its text** — Why: the stub swallowed the engine's error dialogs. (10-12..14 da262fb)
- **MiniDumpWriteDump reports the crash** — Why: a silent stub; the client's crash handler calls it just before it
  fast-fails, so the real exception was lost. How: logs the exception code, faulting module+offset, guest return
  addresses and the C++ exception type; `[CRASH]` names the thread. (10-16 5217a41)
- **Fibers** — CreateFiber(Ex), SwitchToFiber, DeleteFiber, GetCurrentFiber, GetFiberData, IsThreadAFiber. Why: map load
  stuck at 15%: halo3 streams/decompresses map assets in a fiber coroutine (halo3+0x5ff2e0 calls SwitchToFiber) and the
  create/switch functions were no-op stubs, so the loop spun forever. How: real ucontext fibers with 1 MB guard-paged
  stacks; TEB->NtTib.FiberData (TEB+0x20) set on switch/convert; `_XOPEN_SOURCE 700` + `_DARWIN_C_SOURCE` defined at
  the top of peload.c so `ucontext_t` has inline machine context (else getcontext overflowed it and corrupted the
  fiber parameter: the intermittent faults in halo3's decompressor at halo3+0x5fd849 / 0x5fec1a). Map load 15% -> 100%.
  (10-12..14 da262fb)
- **Windows SEH dispatch** — RtlVirtualUnwind (UNWIND_INFO interpreter with chained info), RtlLookupFunctionEntry
  (.pdata), RtlUnwindEx, RaiseException dispatch, RtlRestoreContext, `seh_capture`/`seh_restore`. Why: Rust panics and
  C++ throws could not unwind; caught panics std::terminate()d their thread (SIGILL). (10-11 7f5b6ac)
- **Hardware faults to the guest's handlers** — `crashsig`. Why: the engine's probe/recovery handlers never ran. How:
  SIGSEGV/ILL/BUS/FPE/TRAP become Windows exceptions run through the guest's vectored handlers and .pdata handlers;
  EXCEPTION_CONTINUE_EXECUTION resumes through the signal ucontext. (10-11 7f5b6ac)
- **RaiseException context rebased onto the guest frame** — `i_RaiseException`. Why: the context was captured inside
  the shim (no guest .pdata), so the unwinder found zero frames and a caught Rust panic fell through as 0xC0000005.
  How: RIP = our return address, RSP just above it, so RtlVirtualUnwind walks the real guest frames to
  `__CxxFrameHandler3` and catch_unwind. (10-11 90f2361)
- **RtlUnwindEx bounded** — Why: a lost unwind target walked into garbage and faulted inside the loop. How: RSP bounded
  to a sane window, stop once the establisher frame reaches/passes the target. (10-11 00d8c68)
- **Unwind across the guest stack switch** — `RtlUnwindEx`. Why: `__CxxFrameHandler3` runs on the host stack, so the
  context it passes has a host RSP that cannot unwind to the guest-stack target. How: unwind from the guest-stack
  exception context stashed by i_RaiseException. (10-11 ce8de8f)
- **Faults on loader threads not handed to the game** — `crashsig`. Why: every hardware fault went to the game's
  exception handlers, also faults on loader threads (present, GL submission, compute, stats) that have no TEB; the
  game's Rust handler then faulted on gs:[0x58] = 0 (`eprintln`, exe+0x1d6455d v0.9.18) and the first fault was never
  reported. How: only threads with a TEB (`g_guest_thr`) dispatch to the game; a loader-thread fault prints
  "[CRASH] fault on loader thread '<name>'" with the loader function (dladdr). Old: `PELOAD_SEH_ALL_THREADS=1`;
  test: `PELOAD_TEST_LOADER_FAULT=<secs>`. (10-17 3e5b7b0)
- **Named threads** — loader threads are `shim-*`, guest threads `guest-<start fn>`, so crash reports and SIGUSR1 dumps
  say which thread (10-16 55811fa); names looked up before suspending (10-17 5c44158).
- **Child processes get only their stdio** — map-picture / walking-graph generators, the Workshop helper, the GL helper.
  Why: a generator inherited the stats listening socket and kept port 8765 bound after its game was killed; the GL
  helper inherited stray fds. How: POSIX_SPAWN_CLOEXEC_DEFAULT / only stdio + its pipe; the stats listener is
  FD_CLOEXEC. (10-17 8035d4e, 10-18 0261ef3)

- **Child working directory on older macOS** — `posix_spawn_file_actions_addchdir` exists only from macOS 26; before it the
  `_np` call (10.15+) sets a CreateProcessW child's working directory. (10-18)

## 3. Memory

- **VirtualProtect page-aligned** — `i_VirtualProtect`. Why: the raw unaligned address went to mprotect (EINVAL,
  ignored), the page stayed r-x and the engine's inline hook writes into halo3 .text SIGBUSed. How: align base, round
  the length (RWX works under Rosetta; RW fallback). (10-11 b478e58)
- **FlushInstructionCache** — Why: a no-op, so Rosetta kept running the stale translation of patched code ("Cannot
  finalize patch"). How: `sys_icache_invalidate`. (10-11 b478e58)
- **Read/WriteProcessMemory in-process** — Why: the client reads/patches halo3 through them. How: in-process memcpy
  (10-11 c04b6a2); WriteProcessMemory no longer downgrades the page to r-x afterwards (it faulted the
  `*lpNumberOfBytesWritten` store when that shared the page), keeps RWX + icache invalidate (10-11 2a8f65e).
- **VirtualQuery / GlobalMemoryStatus** — Why: the engine reserves 64 MB "near the game" by scanning regions. How:
  VirtualQuery over mach_vm_region. (10-11 c04b6a2)
- **HeapSize** — Why: returned 0 (callers size copies/reallocs by it). How: `malloc_size`. (10-11 edd9a3e)
- **Interlocked SList atomic** — Push/Pop/FlushEntrySList, QueryDepthSList. Why: halo3's lock-free memory pools; the
  non-atomic implementation raced -> corrupted lists, random heap crashes. How: serialised under a mutex (the list
  head layout is ours). (10-11 f45b8f3)
- **File mapping** — CreateFileMapping{W,A}, OpenFileMappingW, MapViewOfFile(Ex), UnmapViewOfFile over mmap. Why: the
  Forge asset-library builder maps every .map file (stub returned 0). How: file-backed or anonymous mmap;
  FILE_MAP_READ maps PROT_READ (RW gave EACCES). A first attempt (10-11 bb35912) made the corruption of that time worse
  and was reverted (4135438). (10-12 e6e2b02)
- **Trailing guard page for big allocations** — `PELOAD_ALLOC_GUARD` (switch; set by the launchers). Why: a GL-on-Metal
  texture/buffer copy's SIMD loads read ~16 bytes past a guest buffer that ended flush on an unmapped page -> render
  thread fault ~6 s into gameplay. How: VirtualAlloc adds one readable page, only for hint NULL or >= 0x200000000 --
  never in the [4 GB, 8 GB) window halo3 scans for "64 MB free within 2 GB" (padding there broke the scan); keyed on
  the hint, not the result, to avoid shifting placement. (10-12..14 da262fb)
- **VirtualFree(MEM_RELEASE) frees the whole region** — `i_VirtualFree`. Why: it unmapped 4 KB. How: VirtualAlloc
  records base -> size; MEM_RELEASE unmaps the recorded region, MEM_DECOMMIT with size 0 the whole region; strips the
  file-view marker. Old: `PELOAD_VA_OLDFREE=1`. (10-18 ddfcab0, fc77ce6)
- **UnmapViewOfFile unmaps** — Why: a no-op (views leaked). Old: `PELOAD_KEEP_VIEWS=1`. (10-18 fc77ce6)
- **HEAP_GUARD realloc over-read** (diagnostic allocator `PELOAD_HEAP_GUARD`) — copied the new size instead of
  min(old, new). (10-11 3da9e9c)

## 4. Files, pipes, console

- **FindNextFileW end of enumeration** — Why: no ERROR_NO_MORE_FILES -> spurious fatal "System error 0". (10-11 090e7ce)
- **Delete-on-close semantics** — Why: a recursive directory walk looped. How: refcounted last-handle-wins
  delete-on-close for files and directories, FileDispositionInfoEx CLEAR honoured. (10-11 090e7ce, c04b6a2)
- **No blanket directory auto-create** — Why: auto-creating any missing directory-intent open (acc == 0 opens are often
  queries) made bogus folders and hung the parent's init. How: reverted; the worker's content folders (Maps, Game
  Modes, Images, Mods, ...) are pre-created instead, and RECLAIMER_CONTENT_DIR / RECLAIMER_DISCOVERY_DIR point at real
  directories. (10-11 cc80693, 04aabf4)
- **FindFirstFileA / FindNextFileA** — Why: stubbed, so halo3's map catalog was empty ('guardian' never resolved). How:
  real dirent enumeration with WIN32_FIND_DATAA. (10-11 fabd4b7)
- **Empty-path open -> staged playlist** — CreateFileW. Why: fatal "System error 2 (os error 2)": `File::open("")`
  cloned from an empty global String, the playlist path. How: stage `C:\ReclaimerContent\default.playlist.json` (empty
  rotation) and redirect empty-path opens to it (/dev/null if missing). Old: `PELOAD_NO_EMPTY_FIX=1`. (10-11 2a8f65e)
- **Font substitution** — `font_subst`. Why: missing Windows UI fonts (bahnschrift.ttf, ...). How: redirect to staged
  TTFs in $drive_c/ReclaimerFonts (10-11 04aabf4, 1af5f2f); font_package_x{N}.bin falls back to font_package.bin when
  the scaled package is absent (text_font_loaded was false) (da262fb).
- **Rename through SetFileInformationByHandle** — Why: FileRenameInfo (10) / FileRenameInfoEx (65) were dropped, so
  `<uuid>.partial -> <uuid>.json` advertisements never appeared. How: implemented (rename). (10-12..14 da262fb)
- **GetFileInformationByHandle timestamps** — Why: FILETIMEs zeroed (1601), so every local-server advertisement looked
  400 years stale. How: from stat (100 ns ticks + 116444736000000000), volume serial, file index from st_ino.
  (10-12..14 da262fb)
- **GetFullPathNameW contract** — Why: a passthrough that never set lpFilePart, didn't clear the error, ignored the
  "buffer too small -> required size" rule. (10-14 e4fb654)
- **Named pipes** — CreateNamedPipeW/A, ConnectNamedPipe, CreateFileW/NtCreateFile on `\\.\pipe\` / `\Device\NamedPipe\`.
  Why: the dedicated server's game-server start failed (`[NtCreateFile] PIPE/ errno=2`). How: a UNIX socket at
  /tmp/rnp_<fnv(name)>; ReadFile/WriteFile -> recv/send. (10-14 0acedbd)
- **Rust anon_pipe (NtCreateNamedPipeFile)** — Why: the dedicated server's "Cannot start '<game>': os error 2": Rust's
  Stdio::piped() builds pipes with NtOpenFile("\??\PIPE\") + NtCreateNamedPipeFile + NtOpenFile(root = server end, ""),
  none implemented. How: the pipe device opens to a sentinel; NtCreateNamedPipeFile makes a bidirectional socketpair
  and returns one end (0x1000+fd) with its mate in `g_pipe_other`; NtOpenFile relative to a pipe end returns the mate.
  (10-14 873ed26)
- **ReadConsoleW/A block on stdin** — Why: a stub returning FALSE; the dedicated game-server host read its console,
  saw an error and exited -> "stopped (exit code: 1); restarting" loop. How: blocking read of the real stdin fd.
  (10-14 2055ad0)
- **FindFirstFile handle reuse** — FindFirstFileW/A. Why: slots of the 64-entry find table were never reused, so after
  64 scans every read_dir failed without setting the last error -> Rust reported a stale "os error 80" and joining a
  server failed with "Cannot prepare playlist map". How: closed slots reused, 256 entries, under a lock.
  (10-16 36c3c98)
- **CreateDirectoryW real errors** — Why: it always claimed success, so Rust's create_dir_all never created parent
  folders. How: ERROR_ALREADY_EXISTS (183) / ERROR_PATH_NOT_FOUND (3). Old: `PELOAD_MKDIR_LAX=1`. (10-16 f4eb1e6)
- **Overlapped ReadFile on pipes and sockets** — Why: pread() at OVERLAPPED.Offset failed with ESPIPE, so parents
  stopped reading their children's stdout. How: read() and an asynchronous completion; GetOverlappedResult(wait)
  waits. (10-16 f4eb1e6)
- **Append-mode opens can write** — CreateFileW. Why: Rust `append(true)` opens (FILE_APPEND_DATA without GENERIC_WRITE,
  0x120114) were opened read-only, every write failed and WriteFile dumped it to stderr. How: FILE_APPEND_DATA /
  FILE_WRITE_DATA / GENERIC_ALL grant write; append-only = O_APPEND. `*.log` writes are still copied to stderr.
  Old: `PELOAD_APPEND_RO=1` / `PELOAD_NO_LOG_TEE=1`. (10-16 de84a5b)
- **Robust std-handle WriteFile** — `i_WriteFile`. Why: a failed std-handle write made Rust panic ("failed printing to
  stderr"). How: never fails a std-handle write; falls back to fd 2. (10-12..14 da262fb)
- **ReadFileEx on pipes/sockets + alertable SleepEx** — `i_ReadFileEx`, `i_SleepEx`. Why: ReadFileEx pread()ed at the
  OVERLAPPED offset (ESPIPE on the Workshop helper's socketpair, the reader thread ended), and an alertable SleepEx
  after an inline completion waited forever (Rust's pipe reads hung). How: sequential read() (EOF -> 0 bytes /
  ERROR_BROKEN_PIPE); SleepEx returns at once when a completion is pending. Old: `PELOAD_READEX_PREAD=1`,
  `PELOAD_SLEEPEX_PLAIN=1`. (10-17 6810207)
- **WriteConsoleW writes UTF-8** — Why: it copied per character with '?' for non-ASCII (player names). How: one UTF-8
  write. Old: `PELOAD_CONSOLE_ASCII=1`. (10-17 04e35ce)
- **`starting` crash-recovery marker** — shim/play.sh. Why: the client writes ReclaimerProfile\starting at launch and
  deletes it only at the menu; a surviving marker makes the next launch a "disposable worker" that skips the menu, so
  every stalled run perpetuated the loop and the menu was never shown. How: play.sh deletes it before launch.
  (10-16 71b968e)
- **~20 s pre-loading stall** — Why (diagnosed with `PELOAD_TRACE_DEL`): the client's log rotation walked 707k empty
  directories left in ReclaimerProfile/Logs "by an old bug" (which bug is not recorded: UNCONFIRMED). (10-16 9c65850)

- **MoveFileA / MoveFileW / MoveFileExA** — Why: halo3.dll calls MoveFileA mid-match (seen in a 128-player Hugegrass game
  on client 0.9.20); it was a logging stub. How: MoveFileW fails with ERROR_ALREADY_EXISTS when the destination exists
  (no replace), else MoveFileExW's rename; the A versions take the bytes as they are. (10-18)

- **Per-launch scratch folders removed** — `scratch_sweep`, `scratch_note_owner`. Why: the client makes
  C:\ReclaimerCache\Temp\mcc-startup-<uuid> for every game worker ("Private runtime scratch (removed after worker exit)")
  and runs the worker inside it, but under the loader none was ever removed: 1,322 folders / 1.2 GB after a week (backlog
  66). How: CreateProcessW records the worker's pid for a folder it starts a process in
  (~/Library/Application Support/ReclaimerMac/scratch-owners/<name>); every loader start sweeps (20 s in, own thread) the
  mcc-startup-* folders older than 2 minutes whose owner is gone and that no running process has as its working directory
  (proc_pidinfo), removing them with nftw (no symlinks followed). `PELOAD_NO_SCRATCH_SWEEP=1` = off. (10-18)

## 5. Sync objects and timers

- **Real synchronisation primitives** — CRITICAL_SECTION (recursive mutex), SRWLOCK (rwlock), CONDITION_VARIABLE,
  events/mutexes/semaphores (SyncObj), WaitForSingle/MultipleObjects. Why: no-ops -> multi-threaded nondeterminism.
  (10-11 7af88c1)
- **WaitOnAddress / WakeByAddressSingle / All** — Why: no-ops; Rust std::sync (Mutex/RwLock/Condvar/Once/park) uses them
  on Windows 8+, so Rust locks never waited. How: a bucketed condvar futex. (10-11 d326650)
- **Sleep / SleepEx / WaitFor*ObjectsEx** — Why: generic stubs returning "signaled" at once (races, broken backoff).
  How: the Ex variants route to the real waiters; Sleep sleeps. (10-11 9756497)
- **MsgWaitForMultipleObjectsEx returns WAIT_TIMEOUT** — Why: the engine's present loop exited and tore down after one
  frame. (10-11 3da9e9c) It also pumps Cocoa on the main thread (`cocoa_pump`), without which the NSWindow was never
  mapped on screen (da262fb).
- **CloseHandle on sync/thread handles** — Why: thread handles became heap pointers; CloseHandle cast them to fds and
  indexed `g_fddir[]` out of bounds. How: recognised and left alone; fd path bounded to [0, MAXFD). (10-11 84d68c7)
- **I/O completion ports** — CreateIoCompletionPort, PostQueuedCompletionStatus, GetQueuedCompletionStatus(Ex),
  GetOverlappedResult, overlapped ReadFile posting a packet. Why: tokio/mio's driver panicked "failed to wake I/O
  driver" (err 203). (10-12 e6e2b02) Also SetFileCompletionNotificationModes, NtCancelIoFileEx (da262fb).
- **Waitable timers** — SetWaitableTimer arms a due time that `sync_wait` fires by wall clock (frame pacing; were stubs).
  (10-12..14 da262fb)
- **Closed waitable timers freed** — Why: Rust's sleeps create and close a timer ~37 times a second; CloseHandle kept
  every sync object (map-change heap growth). How: a closed timer is freed 30 s later (other sync objects stay: thread
  handles are written by their thread at exit). Old: `PELOAD_KEEP_TIMERS=1`. (10-18 80381f4)
- **mach_absolute_time units** — `mt_us`. Why: under Rosetta the timebase is 1/1 (nanoseconds), not 24 MHz ticks; the
  submission thread's idle spin was 12 us instead of 500 (41x off). (10-17 7d12a64)

- **DuplicateHandle per handle kind** — `i_DuplicateHandle`, peload.c. Why: every handle >= 0x1000 was dup()ed as an fd (handle - 0x1000), so events, timers, thread handles and pipes (heap pointers) gave EBADF and the call returned FALSE; the pseudo-handles came back unchanged. How: current thread (-2) -> its own handle (CreateThread's object, kept in `t_self_thread`, or one made for it); current process (-1) -> 0x8000+pid; sync objects -> the same object (a duplicated timer is marked never-free); pipes -> the same object + a reference (`PipeObj.refs`, `pipe_close` drops one); fds -> dup(); DUPLICATE_CLOSE_SOURCE honoured. CloseHandle no longer close()s process handles (0x8000+pid fell in the fd range). Old: PELOAD_DUPHANDLE_OLD. (10-18, backlog 64)

## 6. Networking (WinSock, AFD, loopback)

- **Real Winsock over BSD sockets** — socket/bind/sendto/recvfrom/select (via poll)/setsockopt/ioctlsocket/WSAStartup/
  WSAGetLastError, sockaddr and AF_INET6 Win<->BSD conversion (`sa_w2b`/`sa_b2w`). Why: no-op stubs; the native
  loopback UDP transport had no I/O. (10-11 04aabf4)
- **127/8 collapse** — `sa_w2b`. Why: macOS routes only 127.0.0.1; the native-UDP transport binds unique 127.x.x.x peer
  IPs -> EADDRNOTAVAIL ("Native UDP experiment bind ... result=-1"). How: 127.x.x.x -> 127.0.0.1 (peers stay distinct by
  port). Old: `PELOAD_NO_LOOPBACK_FIX=1`. (10-12 9dcfe99)
- **Privileged loopback ports** — `sa_w2b`/`sa_b2w`. Why: the transport binds 127.0.0.1:1000/1002/1003 (< 1024, refused
  for a user process). How: loopback ports < 1024 shifted by +40000 and back. (10-12..14 da262fb)
- **Per-peer loopback IP identity** — UDP recv path. Why: the private bridge identifies peers by source IP (host
  127.0.0.3, client transport 127.0.1.1, client native 127.0.2.1); after the collapse every packet came from 127.0.0.1,
  the host never adopted the client's port (MCH1/MCV1 hole-punch), kept replying to port 0, and the join never
  verified. How: record per fd the guest destination IP last sent to (before the collapse) and present a 127.0.0.1
  source as that IP. Old: `PELOAD_NO_PEERIP=1`. (10-14 328ab74)
- **Global peer-IP fallback** — Why: channels the host only receives on had no recorded IP, so the bridge stalled at
  channel_state 3 and dropped. How: the most recent distinct guest peer IP seen on any fd is the fallback. Old:
  `PELOAD_NO_PEERIP_GLOBAL=1`. (10-14 9d7b087)
- **Alias-aware collapse** — Why: with lo0 aliases for the engine's loopback IPs, the real addresses work like Windows
  (no hole-punch restore needed). How: loopback IPs that getifaddrs reports as local are not collapsed (set cached
  once; `PELOAD_NO_IFCACHE` re-reads). (10-14 0380fa0)
- **Async HTTP: AFD poll bridge** — NtCreateFile("\Device\Afd*"), NtDeviceIoControlFile(IOCTL_AFD_POLL 0x12024) + IOCP.
  Why: Reclaimer's reqwest/hyper over tokio + mio learns socket readiness through AFD; master-server lookups failed
  ("error sending request"). How: a poll() thread registers fd + mask, writes the satisfied AFD event bits and the
  IO_STATUS_BLOCK and posts the completion with the original ApcContext. (10-12..14 da262fb)
- **Socket errors in both error slots** — `ws2_err`. Why: mio reads socket errors through GetLastError, not
  WSAGetLastError; a non-blocking connect's WSAEWOULDBLOCK (10035) was misread and mio abandoned the socket (the real
  root of the "error sending request"). How: set both slots to the WSA code. (10-12..14 da262fb)
- **DNS and WSAIoctl** — getaddrinfo/freeaddrinfo/GetAddrInfoW/FreeAddrInfoW over the system resolver (Win32 48-byte
  ADDRINFO, AF_INET6 23<->30); SIO_KEEPALIVE_VALS -> SO_KEEPALIVE; SIO_BASE_HANDLE (+BSP variants) -> the socket.
  (10-12..14 da262fb)
- **TCP listen / accept** — ws2 ordinals 13 / 1 (`i_ws2_listen`, `i_ws2_accept`). Why: stubbed "not needed for UDP"; the
  local server's browser listener could never accept. How: real; accepted sockets inherit O_NONBLOCK from the listener
  (Windows semantics, BSD doesn't); bind sets SO_REUSEADDR (a quickly restarted worker can rebind). (10-12..14 da262fb)
- **SO_SNDTIMEO / SO_RCVTIMEO** — `i_ws2_setsockopt`. Why: Winsock passes a 4-byte DWORD of milliseconds, BSD expects a
  struct timeval -> EINVAL; local-join's probe caller (sub_14058cfe0, v0.9.13) took its failure path and never sent the
  server query ("No local server found" for every host). How: ms -> timeval. (10-14 16fef84)
- **WSASendMsg / WSARecvMsg** — `i_ws2_WSASendMsg/RecvMsg`. Why: unimplemented (a stub returning 0: fake success, nothing
  on the wire); the private bridge uses them, and the client reads its game UDP through the WSAIoctl extension
  pointers. How: over sendto/recvfrom with WSABUF gather/scatter and IOCP completion; their pointers returned from
  WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, WSAID_WSARECVMSG/WSASENDMSG). (10-14 fdfea70)
- **Socket duplication** — WSADuplicateSocketW + WSASocketW(FROM_PROTOCOL_INFO). Why: Rust's `try_clone` hit a stub ->
  WSAEAFNOSUPPORT (10047), online setup failed. (10-16 f4eb1e6)
- **Experiment switches from the local-join work** (off by default, kept): `PELOAD_SYNC_CONNECT` (connect completes
  synchronously: Rust's decode_error_kind has no case for 10035), `PELOAD_KEEP_ADV` (keep local-servers/*.json),
  `PELOAD_ADVERTISE` (write an advertisement; emits a valid RFC-4122 v4 UUID since 10-14 e3a6f59 -- the first form had
  version nibble 'c'), `PELOAD_LOOPBACK_ONLY` (refuse non-127 connects, bf8aece), `PELOAD_UDP_ROLE=host|client`
  (role-asymmetric private-port remap, 45b0c85). (10-12..14)

## 7. Windows, input, focus (Cocoa, Raw Input, XInput)

- **Window-creation protocol** — RegisterClassExW keeps the WndProc; CreateWindowExW makes a real per-window object and
  sends WM_NCCREATE/WM_CREATE with a CREATESTRUCTW; per-HWND SetWindowLongPtrW/GetWindowLongPtrW; RegisterWindowMessage
  returns a unique id per name. Why: winit panicked (`initdata.window.unwrap()`) and asserted on message ids.
  (10-11 2580c61)
- **NSWindow on the real main thread, in an autorelease pool** — Why: AppKit off the main thread and "autorelease pool
  page corrupted". How: window + NSOpenGLContext pre-created on the main thread in main(), wrapped in a pool and
  retained. (10-11 548d4ce) The D3D render context never touches AppKit off the main thread (a windowless CGL context
  on the render thread instead). (da262fb)
- **Real window size** — GetClientRect / GetWindowRect / GetSystemMetrics. Why: they said 1920x1080 while the drawable
  was 1280x720, so the engine read back past the drawable -> GL driver OOB crash (libGLImage glgConvertTo_32). How: the
  real size. (10-11 6cce548)
- **Foreground window follows Windows' activation rules** — ShowWindow, CreateWindowEx, SetForegroundWindow. Why: any
  shown window became "foreground"; the menu runtime's ReclaimerIngameMenu overlay (WS_CHILD, WS_EX_NOACTIVATE,
  SW_SHOWNOACTIVATE) took it, so GetForegroundWindow != the game window and keys went to the overlay (Tab scoreboard
  never shown). How: only activating shows/creates make a window foreground. Old: `PELOAD_FG_LASTSHOWN=1`.
  (10-16 5d425e5; supersedes the "last shown top-level window" rule of 71b968e, which had fixed XInput being polled only
  for the foreground game window)
- **XInput on GameController.framework** — XInputGetState/SetState/GetCapabilities/Enable. Why: stubs, no controller.
  How: extendedGamepad sticks/triggers/buttons, background events on; pitch inverted by default
  (`PELOAD_INVERT_PITCH=0` = normal). (10-16 71b968e)
- **A message ring and Raw Input** — wininput.c. Why: the "message queue" was 32 one-shot activation messages; no
  keyboard/mouse. How: a real ring behind PeekMessageW/GetMessageW (PeekMessage pumps Cocoa when empty); NSEvents ->
  RAWMOUSE/RAWKEYBOARD WM_INPUT (set-1 scan codes) plus WM_KEY*/WM_CHAR/WM_MOUSE*; GetRawInputData,
  Get(Async)KeyState/GetKeyboardState from a key table; ClipCursor captures, ShowCursor hides. Old: `PELOAD_NO_KBM=1`.
  (10-16 c33cca1)
- **Mouse-moved events** — Why: AppKit delivers mouse-moved events (with deltas) only to windows that ask, so mouse look
  did nothing. How: `setAcceptsMouseMovedEvents:YES` on the windows. (10-16 b8eae29)
- **NSRect-returning messages through objc_msgSend_stret** — Why: plain objc_msgSend for `bounds` on x86_64 crashed the
  first window resize. How: _stret; the drawable is updated on size change under the CGL lock. (10-16 b1b9379)
- **Test runs don't steal focus** — `PELOAD_NO_FOCUS`. Why: background test clients activated their window and took the
  user's keyboard. How: orderFrontRegardless without activateIgnoringOtherApps. (10-11 bb35912)
- **Pads per process** — `PELOAD_NO_PAD`. Why: GameController delivers pad input to every process, so a second test
  client also moved with the user's controller. How: that instance sees no controllers. (10-16 f4eb1e6)
- **Input-less test clients swallow keys** — Why: with `PELOAD_NO_KBM` key events went to AppKit, which beeped. How:
  consumed. (10-16 2445619)
- **Self-tests never grab the real cursor** — Why: KBM tests captured/hid the user's macOS cursor. How: tests don't;
  `PELOAD_NO_SYS_CURSOR` does the same for any run. (10-16 5d425e5)
- **Title bar and frame clicks stay with macOS** — wininput.c. Why: once a game ran, a click on the title bar captured
  the mouse instead of dragging the window. How: while not captured, a press outside the content area and its release
  are left to macOS and never reach the game. Old: `PELOAD_TITLEBAR_CLICKS=1`. (10-17 be9dabb)
- **GCController arrays autoreleased** — XInputGetState. Why: `[GCController controllers]` on game threads whose
  autorelease pool never drains: ~950 Objective-C objects a second leaked. How: inside an autorelease push/pop.
  (10-18 80381f4)
- **Scripted typing like a keyboard** — `PELOAD_AUTOCLICK` "t:" steps. Why: bare WM_CHARs never reached the chat box
  (chat commands didn't send). How: key down, WM_CHAR, key up. (10-18 309e2fb)
- **Per-window ShowWindow logging** — Why: the menu runtime re-hides two overlay windows every frame: 50k log lines in
  40 s. (10-16 6ec488d)

## 8. GDI and layered windows

- **GDI DIB surfaces** — CreateDIBSection/CreateCompatibleDC/SelectObject/GetObjectW/BitBlt. Why: "Cannot allocate menu
  surface" (CreateDIBSection returned NULL). (10-11 027162d)
- **Valid GDI objects** — Why: SelectObject returned (void*)1 and GetDC/GetWindowDC a bare 0xDC01, which the engine
  dereferenced -> 0xC0000005 at address 1 in the loading-screen compositing. How: real GdiObj/GdiDC objects.
  (10-12..14 da262fb)
- **GDI software rasteriser** — FillRect, BitBlt (SRCCOPY/BLACKNESS/WHITENESS), StretchBlt, StretchDIBits,
  SetDIBitsToDevice, GdiAlphaBlend/AlphaBlend, CreateSolidBrush/CreatePen, SetTextColor/SetBkMode/SetBkColor. Why: all
  no-ops; client screens produced no pixels. (10-12..14 da262fb)
- **GDI text with CoreText** — DrawTextW, TextOutW, ExtTextOutW, GetTextMetricsW, GetTextFaceW, GetTextExtentPoint32W,
  SetTextCharacterExtra; CreateFontW/Indirect keep height and face. Why: no glyphs on the loading screen. How: CTLine
  runs / CTFramesetter for DT_WORDBREAK, composited premultiplied into the DIB; text drawn into a DC whose bitmap is
  the 1x1 placeholder goes to the presented main surface (`gdi_text_target`); face-aware fonts (Segoe UI / Bahnschrift
  -> the real font if installed, else Helvetica Neue / Avenir Next Condensed; `PELOAD_FONT*` overrides).
  (10-12..14 da262fb)
- **Present capture latches window-aspect surfaces only** — Why: a 1024x1024 UI atlas DIB or a bound shadow map hijacked
  the presented frame ("thin strip" garbage). How: only ~16:9 surfaces at least 1152 wide are latched. (10-12..14 da262fb)
- **StretchDIBits goes where the text goes** — Why: the loading screen's art blit went to a DC bitmap without pixels
  while its text went to the main surface, so the art never showed and status lines piled up. How: same destination
  as text; ETO_OPAQUE background fill in ExtTextOutW; SetBkColor tracked (default white). (10-16 e9a6373)
- **FillRect clears the text surface** — Why: the client clears its map-loading screen with a full-size FillRect every
  frame, which went to the empty bitmap, so each frame's text piled up ("CAPTURE THE FLAG" doubled, progress line
  garbled; backlog 25). How: FillRect fills the text's surface. Old: `PELOAD_GDI_FILL_OLD=1`. (10-17 6e33013)
- **Layered windows drawn** — UpdateLayeredWindow, `lay_draw`. Why: a no-op; when the game stops presenting for > 1 s
  the client shows its screens as layered windows, so its loading screen vanished during heavy loads. How: each layered
  window keeps bitmap, position, alpha and ShowWindow state and is drawn over the frame at the window blit.
  Old: `PELOAD_NO_LAYERED=1`. (10-17 70ac312)
- **Layered windows honour SetWindowPos / SetLayeredWindowAttributes** — hide/show/move and alpha. Old:
  `PELOAD_LAYERED_NOSWP=1`. (10-17 6e33013)
- **Layered-window z order** — Why: drawn in slot order, a loading splash could cover the lobby and its map vote. How:
  new windows on top; SetWindowPos HWND_TOP/BOTTOM/insert-after (without SWP_NOZORDER) reorder; drawn bottom to top.
  Old: `PELOAD_LAYERED_NOZ=1`. (10-18 7c3278c)

## 9. Audio (XAudio2, WASAPI)

- **WASAPI enumerator** — CoCreateInstance(MMDeviceEnumerator). Why: a no-op stub; cpal's `get_enumerator().unwrap()`
  panicked (device.rs:851). How: a real but empty IMMDeviceEnumerator + CoInitializeEx/CoTaskMem*/PropVariantClear.
  (10-11 9a68be7)
- **Minimal XAudio2** — XAudio2Create (bound by ordinal 1 in both import tables) returns an IXAudio2 + generic voice;
  CreateMasteringVoice etc. succeed silently. Why: a stub; the engine's audio init needs the objects. (10-11 027162d)
- **WavesFX table and XAPO effects** — `waves_prefill` writes no-op WavesFX_* pointers into halo3's table
  (halo3+0x46bb780..; WavesFX_Initialize returns a dummy context); xaudio2_9redist ordinals 2/3/4 (CreateAudioReverb/
  VolumeMeter/CreateFX) return a silent IXAPO with a safe vtable. Why: halo3 called NULL function pointers. (10-11 7f5b6ac)
- **Voice volume read-back** — Set/GetVolume (slots 12/13), Set/GetChannelVolumes (14/15). Why: the getters returned 0,
  the worker's per-tick audio-settings apply (exe+0x481560, v0.9.13) compared with its target and re-applied + logged
  "Game audio volume applied" every tick. How: Get returns what Set stored. (10-12..14 da262fb)
- **Real game audio** — xaudio.c: XAudio2 2.9 + X3DAudio on CoreAudio. Why: a silent shared-voice stub. How: per-voice
  objects with full vtables, PCM/float sources resampled to 48 kHz, submix, downmix, master -> AudioQueue; the mixer
  runs callbacks outside the audio lock (a callback waiting on a game lock could otherwise deadlock a Submit); xaudio2.h
  structures `#pragma pack(1)` (pSends@4, 12-byte sends, BUFFER.pContext@36, SamplesPlayed@12). Off: `PELOAD_AUDIO=0`.
  (10-16 5afa6f0, 789ad68)
- **FlushSourceBuffers on a stopped voice** — `xs_FlushSourceBuffers`. Why: it kept the head buffer of a STOPPED voice,
  which the mixer never advances, so its OnBufferEnd never came; the engine's pre-load "drain every voice" wait
  (halo3+0x1d17be, busy byte voice+0x38) spun forever -> joins and Host Game hung at "starting". How: a stopped voice
  flushes and reports every buffer. (10-16 36c3c98)
- **WASAPI render endpoint** — wasapi.c: IMMDeviceEnumerator/IMMDevice/IMMEndpoint/IPropertyStore/IAudioClient/
  IAudioRenderClient/IAudioClock, one AudioQueue per stream. Why: "no devices", so no menu music or voice playback.
  Old: `PELOAD_NO_WASAPI=1`. (10-16 81416fd) `PELOAD_AUDIO=0` also mutes it (10-16 6ec488d).
- **Abandoned playback streams closed** — Why: without a mic the client's voice chat retries every ~5 s, each try leaves
  a stream with one IAudioClient reference -> its AudioQueue (called back every 10 ms) lived forever (136 in a 10-minute
  BTB, ~720/hour). How: a stream whose render client and clock are released with only that reference left is idle:
  queue stopped and disposed; Start reopens. Old: `PELOAD_WASAPI_KEEP_IDLE=1`. (10-17 66b4562)
- **caulk crash: callback vs dispose race** — `wa_close_queue`. Why: "exception 0xC000001D at unmapped module" (SIGILL in
  caulk::alloc::make_audio_buffer_resource): an AudioQueue callback re-enqueued a buffer while another thread disposed
  the queue. How: a closing flag; the enqueue inside the stream lock; dispose outside it. (10-17 2f24545)
- **Queue lifecycle under one lock** — Why: two Releases could dispose the same AudioQueue (the input-less target
  crashed in caulk's allocator). How: the queue is taken out of the stream and closed in one locked step; all queue
  create/start/stop/dispose under one process-wide lock. (10-17 b02529c)
- **Pooled AudioQueues** — Why: the voice retries' create/dispose churn kept tripping caulk's allocator trap, now in the
  user's own game. How: idle streams hand their stopped queue + buffers to a pool; the next same-format stream reuses
  it; callbacks reach their stream via WaAq.s under `g_wa_cbmx`. Old: `PELOAD_WASAPI_NOPOOL=1`. (10-17 b2c71ee)
- **A microphone for the client** — WASAPI capture endpoint (AudioQueueNewInput -> IAudioCaptureClient). Why: without a
  capture device the client's voice chat rebuilt a playback stream every 5 s (the caulk crash trigger). How: the Mac mic
  is the capture endpoint, so voice chat starts once. `PELOAD_MIC=tone`, `PELOAD_MIC_MUTED=1`. (10-17 6810207)
- **Silent microphone when there is none** — Why: a mic-less client (PELOAD_MIC=0 then) still crashed 9 ms after a voice
  stream opened. How: no mic / a mic that can't open (permission) = a SILENT capture device, so voice starts once.
  `PELOAD_MIC=none` = no device (old). (10-17 0c5285c)
- **Dropped capture streams closed** — Why: the client opens a capture stream per map load and drops the old one with a
  reference held; each kept its tone thread / input queue (18 after 18 loads). How: closed when idle like playback
  streams, reopened on Start. (10-18 8090714) Reopen tries the real microphone again; the idle close checks its flag
  under the lock. (10-18 410a6ee)

## 10. D3D11 / DXGI objects

- **Minimal DXGI factory** — CreateDXGIFactory1 returns a real COM object (EnumAdapters: no adapters). Why: the engine's
  DXGI probe dereferenced NULL. (10-11 027162d)
- **Null D3D11 device** — D3D11CreateDevice returns a device + context; every Create* a typed COM object; views keep their
  resource (GetResource never NULL); Map returns a real sized buffer; DXGI CreateSwapChain/Present/GetBuffer. Why: halo3
  crashed at its first D3D use (and writes into NULL maps corrupted the heap). (10-11 daa4fa6)
- **Resource sizing heap overflows** — Why: random free() aborts and BTreeMap/glow panics. How: texture buffers sized
  from the real DXGI footprint (BC block sizes, per-format bpp) instead of Width*16; Map's RowPitch matches; `res_GetDesc`
  wrote 64 bytes into a 24-byte D3D11_BUFFER_DESC -> exact per-type sizes (1D 32, 3D 36: 9d7df20, f45b8f3);
  view_GetDesc/CheckCounterInfo shrunk; DepthPitch = one 2D slice (was the whole buffer, slice N written at N*size).
  (10-11 816165d)
- **Context Get* out-params zeroed** — Why: the engine used uninitialised pointers. (10-11 d698353)
- **D3DReflect / D3DGetInputSignatureBlob** — Why: unimplemented, `*ppReflector` left uninitialised -> garbage pointer,
  heap corruption. How: a reflection reporting zero resources (10-11 454f140). Superseded by the real reflector
  (section 11, 90b5320).
- **IDXGISwapChain::GetDevice** — Why: slot 7 returned S_OK with a NULL out pointer; ui_composition took its error path
  and leaked an app-state RefCell borrow -> "RefCell already borrowed" panic on the next UI draw. (10-11 a800d1b)
- **Per-IID QueryInterface, IDXGIDevice, IDXGIAdapter** — `dev_QI`. Why: the shared QI returned the device for
  IDXGIDevice (wrong vtable) -> the same leaked borrow (menu re-entrancy panic 5/5 runs). How: ID3D11Device/Device1,
  ID3D11DeviceContext, IDXGIDevice/Device1 (a real `g_dxgidevice` with GetAdapter) routed per IID. (10-11 9476f8c)
- **GetAdapter / adapter description** — Why: GetAdapter returned garbage (void), adapter GetDesc was empty. How: S_OK, a
  real Description ("Reclaimer GL (OpenGL on Metal)") and DedicatedVideoMemory. (10-11 a435cae)
- **DXGI_ADAPTER_DESC is 304 bytes** — Why: adapter_GetDesc memset 320, a 16-byte overflow that clobbered the caller's
  saved rbx -> NULL &str in the "Graphics card:" log formatter -> 0xC0000005 at exe+0x43A3. How: exact 0x130; QI for
  IDXGIAdapter1-4 returns E_NOINTERFACE (the engine's "IDXGIAdapter3 unavailable" fallback). (10-11 21c5f89)
- **Private data on device children** — `child_GetPD`/`child_SetPD`/`child_SetPDI` (PDStore). Why: halo3 stashes each VS
  blob on the shader with SetPrivateDataInterface and reads it back for CreateInputLayout; the no-op stubs made the
  input-layout builder (halo3+0x2ac24c) get a NULL blob, so NO input layout was ever created and all world geometry was
  invisible ("geometry=0" was this, not a gate). How: real per-object private data. (10-12..14 da262fb)
- **D3D11_APPEND_ALIGNED_ELEMENT** — `dev_crIL`. Why: 0xFFFFFFFF offsets were taken literally. How: resolved from the
  previous element's size (`dxgi_vtx_bytes`). (10-12..14 da262fb)
- **UpdateSubresource honours pDstBox; CopySubresourceRegion implemented** — Why: the game crashed every run ~2 s after
  "gameplay presentation ready" in `_platform_memmove` from halo3+0x28fa5e: UpdateSubresource ignored the box and read
  a full resource height from a box-sized source; CopySubresourceRegion (12k calls/frame) was a no-op. How: copy
  (Bottom-Top) rows of (Right-Left)*bpp (buffers: byte offsets), box-aware copies. (10-12..14 da262fb)
- **Valid swap-chain description** — swap_GetDesc / GetFullscreenState. Why: zeroed ("GPU swap chain: 0x0").
  (10-12..14 da262fb)
- **Swap chain / back buffer 1920x1080** — Why: a 4096x4096 placeholder; in menu mode the client composites its UI into
  the back buffer with a back-buffer-sized viewport, so the menu showed shrunk into the top-left 47% x 26%. How:
  1920x1080 (`PELOAD_SWAP_SIZE`). (10-16 b95677e)
- **ID3D11Query results** — Why: GetData returned S_OK without writing a result. How: occlusion / occlusion-predicate on
  GL SAMPLES_PASSED; event, timestamp and disjoint answered on the CPU. Old: `PELOAD_D3DGL_NOQUERY=1`. (10-16 1a80367)
- **D3DCompile** — returns a valid ID3DBlob with a placeholder DXBC (halo3's video-display shader compile). (10-11 7f5b6ac)
- **D3D objects reference counted and freed** — obj_AddRef/obj_Release/obj_QI, `d3dobj_stage`, `d3dobj_reap`. Why:
  Release was a no-op: every map load leaked 4,000-7,000 textures (~7 GB of CPU copies), plus ~120 MB/hour in play.
  How: COM refcounts (GetResource/GetBuffer/QI hand out references; a view holds its resource); release to 0 queues the
  object; staged on the game thread until nothing bound uses it, reaped on the GL side a frame after it passed that
  point and when no bound GL state, queued compute job or pending query uses it; GL objects deleted, pointer-keyed
  caches (FBOs, CB registry, camera latch, compute texture cache) cleaned, struct quarantined (4096). A live-object
  registry makes stray pointers harmless; GetBuffer AddRefs (the back-buffer underflow). Old: `PELOAD_D3D_FREE=0` /
  `PELOAD_D3D_NOFREE=1`. (10-17 8ec3407)
- **Two use-after-free holes in freeing** — Why: map-change corruption in a public BTB (player models over everything,
  white splotches). How: queued CPU particle jobs' views and samplers are checked before a free (only resources were);
  frees no longer rely on the game's and the GL side's Present counts staying in step (also the submission queue
  position). Freeing and COW were off 10-17 21:15 .. 10-18 00:35, back on after a clean 40-minute BTB soak.
  (10-17 8035d4e, 10-18 cef684c)
- **Duplicate shaders freed** — Why: every map load re-created byte-identical VS/PS that were never freed (8,335 / 70 MB of
  bytecode after two map changes). How: duplicates (`o->canon`) share the canon's bytecode and GLSL (no second
  translation) and go through the normal free pipeline; canon shaders stay (programs key on them); compile-failure
  paths no longer free a GLSL a duplicate may share. Old: `PELOAD_D3D_KEEPDUPS=1`. (10-18 ecceb6d)
- **Input layouts freed** — plain data, programs key on VS/PS only; ~25 a load were kept. Old: `PELOAD_D3D_KEEPIL=1`.
  (10-18 df329f9)

## 11. D3D11->GL: shaders and the DXBC->GLSL translator

The translator: shim/src/dxbc2glsl.c (in-process, `dg_*`), with re-tools/dxbc2glsl.py kept byte-identical as the test
oracle. Fixes below apply to both unless noted.

- **GLSL #version bump for the client's own GL** — `i_glShaderSource` / `i_glCreateShaderProgramv`, intercepted by
  resolved POINTER inside `make_gl_thunk`. Why: egui/glow's #version 130/140 shaders fail on macOS Core profile (150+);
  the engine requested the functions under names that skipped the name-based special cases. How: rewrite the version
  to 150. (10-11 2bafa13; a DYLD interpose attempt, glfix.c 7dbbfce, did not catch it)
- **Opcode table off by one** — Why: the table lacked D3D10_SB_OPCODE_CUT = 9, so every opcode >= 9 decoded as its
  neighbour (dp4 as else, mov as movc, mad as min): every matrix transform became garbage, gl_Position collapsed, all
  RTs black -- while still "100% compiling". How: the authoritative SM4/5 enum; the no-operand set fixed; CUSTOMDATA (53)
  length read from its second dword (else the stream desyncs); sample_l LOD scalar `(x).x`. (10-12..14 da262fb)
- **Compile/link fixes for the first full corpus** — non-finite float immediates -> `uintBitsToFloat(bits)` (no GLSL
  nan); cb arrays sized to the highest register used (not 64); SV_Depth (register 0xFFFFFFFF) -> gl_FragDepth, scalar,
  never declared as an output (killed a `layout(location=-1)`). VS 144/144, PS 1059/1059 linked. (10-12..14 da262fb)
- **Monolithic programs** — `d3dgl_draw`. Why: Apple's GL-on-Metal can't build a Metal pipeline from a separable program
  pipeline (GL_PROGRAM_SEPARABLE + UseProgramStages): "buildPipelineState failed", m_disable_code:1000 and every draw
  fell to software vertex processing. How: one VS+PS program per pair (`g_mono`, keyed by (vs, ps)), glUseProgram.
  Old: `PELOAD_D3DGL_SEP=1`. (10-14 da262fb)
- **Semantic varying matching** — `d3dgl_inject`, `d3dgl_rename_varyings`, `d3dgl_augment`. Why: a monolithic link matches
  varyings by NAME, but VS outputs were oN and PS inputs vM by register. How: each inter-stage varying gets
  `layout(location=L)` from its D3D semantic (shared semantic->location table) and the name `xfer<L>`; each pair's
  interface is padded with dummy declarations so a strict link validates; VS/PS cbuffer uniforms renamed per stage
  (vXcb/pXcb). Links 2 -> 40, executed draws 27k -> 148k. (10-14 da262fb)
- **Python out of the runtime path** — `d3dgl_load_glsl` -> `dg_translate`. Why: translating on a cache miss by popen of
  dxbc2glsl.py launched hundreds of python3 processes and starved engine init (it never reached the sim from an empty
  cache); first, untranslated shaders had rendered black because only an offline pre-pass existed (a34c526). How: a C
  port compiled into the loader, parity 2826/2826 byte-identical over the 2833-shader corpus. Off:
  `PELOAD_D3DGL_NOXLAT=1`. (10-14 a34c526, 125fbf7)
- **Duplicate varying declarations** — `d3dgl_dedup_varyings`. Why: ~20% of world draws skipped (noprog 20,352/100k):
  58 "xferN may not be redeclared": an input register used in the body but absent from the signature got
  location=register, colliding with a semantic-mapped input. How: drop duplicate `layout(location=L) in/out vec4 xferL;`
  declarations (keep the first). noprog -> 484. (10-14 b5f107c)
- **Untranslated opcodes** — sample_b (2,905 uses: texture(t, uv, bias)), discard (alpha test), round_ni/pi/ne
  (floor/ceil/roundEven), ineg, ult. Why: silently dropped ("// UNTRANSLATED"): no diffuse fetch, no alpha cut-outs.
  (10-15 1ce6690)
- **Write masks, resource swizzles, sincos** — Why: a write mask took the first N lanes instead of the mask lanes (`mov
  o0.zw, l(0,0,.5,.5)` gave w = 0: HUD/fullscreen passes vanished); sample ops ignored the resource swizzle; sincos wrote
  sin into the cos output. GLSL cache versioned (v2). (10-16 90b5320)
- **Real shader reflection** — `D3DReflect` over the DXBC RDEF chunk. Why: the stub reported zero constant buffers, so
  halo3's constant registry (halo3+0x2af4b8 -> flush -> VS/PSSetConstantBuffers; one D3DReflect call site 0x2af4ef) never
  bound cb0 (ViewVS), the vertex-compression constants or any lighting cbuffer -- the "material bind path never runs".
  How: a real ID3D11ShaderReflection. Replaced the camera/decompression workarounds (section 21). `PELOAD_REFLECT=0/1`
  overrides. (10-16 90b5320)
- **Translator correctness sweep** — resource dimensions -> sampler3D/Cube/2DArray coordinates; aoffimmi offsets;
  integer `ld` addresses + mip; PS SV_Position -> gl_FragCoord (an unbound varying in 1,841 of 2,426 pixel shaders); VS
  epilogue flips clip y and maps z to GL's range (GL row y == D3D row y everywhere; present flips once); bit-exact
  compares/masks and branch tests; null destinations no longer clobber r0; imul writes destLO; relative cbuffer indexing
  (light loops, bone palettes), indexable temps, immediate constant buffer; SM5 derivatives, rcp, f16<->f32, bitfield
  ops, gather4. Old switches: `PELOAD_D3DGL_NOYFLIP`. (10-16 a6b7628)
- **GLSL cache write-only by default** — Why: a stale ~/reclaimer_glsl cache hid translator changes. How: translate every
  launch; the cache is written for inspection (`PELOAD_D3DGL_GLSLCACHE=1` reads it). (10-16 a6b7628; now v5)
- **Constant buffers at their declared size** — Why: register-indexed palettes were declared 4096 vec4 and hit GL's
  4096-component limit, so every skinned mesh (first-person arms/weapon, bipeds) and light-loop materials failed to link
  (27% of draws skipped). How: declared at dcl_constantbuffer size. Old: `PELOAD_D3DGL_NODCLSZ=1`. (10-16 338a146)
- **_sat modifier** — Why: opcode bit 13 was ignored in 7,210 instructions / 1,852 shaders (lighting, fog, blend weights
  out of range). How: clamp to [0,1]. (10-16 aaf41bb)
- **Integer NEG** — Why: integer operands' NEG modifier was float negation. How: two's complement (18 opcodes). (10-16 f6c958e)
- **Structured / raw buffers in shaders** — dcl_resource_structured/raw -> usamplerBuffer, ld_structured/ld_raw ->
  dword texelFetch. Why: particle vertex shaders read particle state at t16/t17. (10-16 f6c958e)
- **Shadow compares** — sample_c / sample_c_lz -> sampler2DShadow (array/cube variants); comparison samplers get
  GL_COMPARE_REF_TO_TEXTURE with the D3D ComparisonFunc. Why: a fixed "depth >= ref" single tap ignored the game's
  direction and hardware PCF (366 shaders). Old: `PELOAD_D3DGL_NOCMP=1`. (10-16 7968a99)
- **SV_VertexID / SV_InstanceID are integer bits** — Why: exposed as float(gl_VertexID), so shaders deriving a fullscreen
  triangle from `id & 1` / `id >> 1` (loading screen, vertex-pulled post passes) produced nothing and particle VS indexed
  state with garbage; 10-20 s of black window during map load. How: `intBitsToFloat(ivec4(gl_VertexID))`. (10-16 07c239f)
- **Integer vertex attributes** — Why: *_UINT/_SINT attributes (BLENDINDICES as R8G8B8A8_UINT) went through
  glVertexAttribPointer (3 -> 3.0f) while the VS reads integer bits (0x40400000): skinned draws indexed their bone
  palette out of bounds -> one-frame white flashes (Inf pixels through bloom/auto-exposure) and exploded geometry ~1
  every 2-3 s. How: those inputs are uvec4/ivec4 `viN` read with uintBitsToFloat/intBitsToFloat; the shim uses
  glVertexAttribIPointer. 5 white + 1 geometry glitch -> 0 + 0 over 433 frames. Old: `PELOAD_D3DGL_NOINTIN=1`.
  (10-16 1a80367)
- **Varying locations above Apple's limit** — `d3dgl_relocate`. Why: Halo 3's water pair never linked: TEXCOORD10/11 map to
  locations 14/15 and Apple's GL refuses user varyings >= 14 (64 VS output components); water drew nothing. How: pairs
  with a location >= `PELOAD_D3DGL_VARYING_SLOTS` (14) get their varyings renumbered 0..n-1 on both sides.
  (10-16 434db9b)

- **umax / umin** — `dxbc2glsl.c` + `re-tools/dxbc2glsl.py`. Why: named in the opcode table but never translated
  ("// UNTRANSLATED: umax" in 484 of 4,180 shaders); the deferred shadow pass a42824308404ea6d takes umax of the shadow
  map's textureSize, so a stale register became the shadow texel size and the pass darkened lit pixels 0.051 -> 0.004
  (backlog 61). How: ivec4(max/min(uvec4(a), uvec4(b))). The CPU compute interpreter already had them. (10-18)

## 12. D3D11->GL: textures

- **TYPELESS / SRGB 8-bit formats** — `d3dgl_tex_upload_fmt`. Why: the loading-screen composite is format 90
  (B8G8R8A8_TYPELESS), unsupported -> 1x1 placeholder -> black window. How: 27 as RGBA, 90/92/93 as BGRA. (10-14 5081b49)
- **Block-compressed upload** — `d3dgl_bc_glfmt`, `d3dgl_ensure_tex`. Why: BC1-7 textures were rejected (magenta/black).
  How: glCompressedTexImage2D with the GL format per DXGI format, placeholder fallback if the driver rejects it.
  Off: `PELOAD_NO_BC=1`. (10-14 a34c526)
- **Texture-upload bounds guard** — Why: uploads could read past a too-small CPU buffer. How: glTexImage2D real pixels
  only when the object holds w*h*4 bytes, else 1x1. (10-12..14 da262fb)
- **CreateTexture2D copies pInitialData** — Why: never copied, so every map texture stayed zero (all samples 0 -> black).
  How: copy the initial data, BC-aware (tight ceil(w/4)*ceil(h/4)*block bytes, not SysMemPitch*rows, which under-copied).
  Off: `PELOAD_NO_TEXINIT=1`. (10-15 bbd83c7, bdb0916)
- **UpdateSubresource on BC textures** — Why: a per-pixel bpp = rowpitch/w path scrambled compressed data. How: 4-row
  block rows with block-row pitch. (10-15 bdb0916, 97f068d)
- **Subresource (mip) indices honoured** — `tex_sub_off`. Why: halo3 streams each mip with CopySubresourceRegion(dstSub,
  srcSub) and the shim ignored the indices, so every mip copy landed at offset 0 ("25% filled" textures). How: byte
  offset/size of each subresource; BC box path block-aware. (10-15 97f068d)
- **Wrap mode REPEAT** — Why: textures were CLAMP_TO_EDGE, but halo3's terrain/detail textures tile 8-48x, so tiled UVs
  sampled the edge texel -> dark/black terrain. How: GL_REPEAT (D3D11's default sampler is WRAP); render targets stay
  clamped. Old: `PELOAD_D3DGL_CLAMP=1`. (10-15 1b18f9d; later real sampler objects, 90b5320)
- **BC decode to RGBA8 + mipmaps + anisotropy** — Why: Apple GL-on-Metal returned BLACK when sampling a compressed
  texture with a mipmap min-filter, so tiled terrain could not be mipmapped (moire). How: BC1-5 decoded in the loader,
  glGenerateMipmap, trilinear + anisotropic (16x, c4bec6d); BC5 normal maps reconstructed; complete-mip selection
  (`PELOAD_D3DGL_MIPTHRESH`). (10-15 6112645) **Superseded** for normal BC formats by the compressed mip chain below.
- **SRV slots 0-31 packed into GL units** — Why: halo3 binds material textures to high sparse slots (t13, t14, t16, t17)
  but only slots < 8 were stored. How: 32 slots, each declared sampler packed into the next free GL unit (Apple has ~16
  fragment units). (10-14 1c0ba01)
- **Exact subresource layout, real texture targets, format table** — Why: init data, Map, UpdateSubresource and copies
  assumed one layout; cube maps, arrays and 3D textures were uploaded as 2D; unknown formats became a 1x1 magenta
  placeholder. How: slice-major layout (sub = mip + slice * mips) everywhere; 3D/1D get initial data; cube/cube-array/
  2D-array/3D uploaded to their GL targets and bound by the shader's sampler type; a general DXGI->GL table
  (float/half/snorm/R8/RG8/A8...); BC4/BC5 with D3D semantics; SNORM normal maps as RGBA8_SNORM. (10-16 a6b7628)
- **sRGB per D3D view** — Why: halo3 renders its final frame into an R8G8B8A8_TYPELESS target through an _SRGB RTV and
  presents through a UNORM view; the loader stored linear values and showed them raw -> mid-tones far too dark. How:
  8-bit RGBA/BGRA targets/textures stored SRGB8_ALPHA8; GL_FRAMEBUFFER_SRGB on per draw/clear when the bound RTV is
  _SRGB; sampling decodes only through _SRGB SRVs (EXT_texture_sRGB_decode on sampler variants); copies, readbacks and
  the present blit move raw bytes; RTV array-dimension enums fixed. Old: `PELOAD_D3DGL_NOSRGB=1`. (10-16 a5c79b9)
- **Compressed BC with the asset's own mip chain** — Why: the RGBA8 decode cost 4-8x the GPU memory (GL helper 5.0-5.3 GB
  on a 100-player Workshop map) and 1.5 s of GL-thread upload time per window. How: upload compressed with the asset's
  mips down to 4x4, GL_TEXTURE_MAX_LEVEL capped at the last level (without the cap the chain is incomplete and samples
  black -- why the decode was added); sRGB storage only for _SRGB formats (the decode path's rule on compressed storage
  gave white bloom blobs: `PELOAD_D3DGL_BCSRGB=1`). Helper 3.0 GB, upload 0.02 s. Old: `PELOAD_D3DGL_BCDECODE=1`;
  A/B `PELOAD_AB=bcdec`. (10-18 8c8e83e)
- **BC chain bounded by MipLevels; TYPELESS BC decoded** — Why: levels past the texture's MipLevels were zero-filled
  memory, so minified 1-level textures went black/transparent; TYPELESS BC (70/73/76/97) can't pick sRGB storage per view
  on compressed storage. How: the chain stops at MipLevels; TYPELESS BC back on the decode path. (10-18 410a6ee)

- **Decoded BC UNORM in plain RGBA8 storage** — `bc_srgb_storage` (d3dgl_sub_pixels = arrays / cubes / 3D, and the 2D
  decode path). Why: decoded BC1-3 were ALL kept in sRGB storage (SRGB8_ALPHA8), UNORM views reading it with the sRGB
  decode skipped -- but the skip sits on the SAMPLER object whenever one is bound, and texelFetch ignores sampler objects.
  Halo 3's static-lighting shaders texelFetch their SH lightmap arrays (t13 x8 / t14 x2 slices, BC3_UNORM, decoded as
  (a+b)*2-2 times alpha times a cb4 scale), so the coefficients came back sRGB-decoded (0.5 -> 0.21: about -1.2 instead
  of 0) and interiors went black while the sky, emissives, the HUD and the scope's night vision were fine (backlog 61,
  Hugegrass; found with the do=pixwho probe: albedo 0.31, then the lighting PS 7bcaa3c3c92f37d7 wrote 0-0.016). How:
  sRGB storage only for the _SRGB (72/75/78) and TYPELESS (70/73/76) formats -- a UNORM resource can only have UNORM
  views. `PELOAD_D3DGL_BCUNORM_SRGB=1` = old. (10-18)

## 13. D3D11->GL: buffers, state, draws, queries

- **MSAA sample-count clamp** — Why: GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT (0x8cd6) -> "Incomplete menu framebuffer". How:
  clamp sample counts consistently across renderbuffers/textures. (10-11 548d4ce)
- **MRT tracking, clears, dirty uploads** — OMSetRenderTargets keeps rtvs[8] + DSV; ClearRenderTargetView /
  ClearDepthStencilView (slots 50/53) hooked so RTs clear per frame; VB/IB upload only when Map dirtied them (3 MB VBs
  were re-uploaded per draw); DXGI->GL vertex attribute format table. (10-12..14 da262fb)
- **Input assembler** — Why: draws dropped StartIndex/BaseVertex/StartVertex/StartInstance; layouts were bound by element
  ORDER, not semantic; per-instance data had no divisor; strip cut indices weren't restarted. How: semantic matching via
  the VS ISGN, glVertexAttribDivisor, DrawElementsBaseVertex, StartInstance pointer offset, PRIMITIVE_RESTART.
  (10-16 90b5320)
- **Copies of GPU-rendered targets** — CopyResource / CopySubresourceRegion. Why: they memcpy'd stale CPU shadows of
  render targets. How: FBO blits; glReadPixels into the CPU shadow only for small (readback) destinations. (10-16 90b5320)
- **Pipeline state applied** — Why: blend / depth-stencil / rasterizer / sampler descriptions were discarded and their
  setters unhooked. How: parsed and applied (blend func/eq/color mask/factor, cull + front face, scissor, viewport
  origin, GL sampler objects with a no-mip variant for single-level textures, RT textures MAX_LEVEL 0). A/B:
  `PELOAD_D3DGL_NOBLEND/NOCULL/NOSCISSOR/NOSAMPLER`. (10-16 90b5320)
- **Depth and render-target precision** — real depth-stencil textures, D3D depth/stencil state, ClearDSV, depth-only
  (shadow) passes with a null PS; an FBO per (RTVs, DSV) with draw buffers and independent blend; render targets at their
  real precision (RGBA16F ...). A/B: `PELOAD_D3DGL_NODEPTH`, `_RT8`. (10-16 a6b7628)
- **No render target = depth-only pass** — Why: such draws were drawn into the back buffer. How: colour skipped.
  Old: `PELOAD_D3DGL_NULLRT_BB=1`. (10-16 d69fd1b)
- **Viewport depth range, depth clip, depth bias** — MinDepth/MaxDepth, DepthClipEnable, depth bias applied. A/B:
  `PELOAD_D3DGL_NODEPTHRANGE`. Views keep their descriptor (format, dimension, slices). (10-16 338a146)
- **No GL_DEPTH_CLAMP for DepthClipEnable = FALSE** — Why: on GL-on-Metal it produced screen-covering polygons (4/86 frames
  vs 0/86 in a phase-tagged A/B). How: clipping stays on. Old: `PELOAD_D3DGL_DEPTHCLAMP=1`. (10-16 f6c958e)
- **NULL-pixel-shader passes drawn with colour masked** — Why: D3D11 still runs depth/stencil with no PS; ~6-8 such draws a
  frame were skipped. Old: `PELOAD_D3DGL_SKIPNULLPS=1`. (10-16 3eebb13)
- **Streaming ring never orphans mid-draw** — `d3dgl_ring_reserve`. Why: a wrap in the middle of a draw's uploads
  orphaned data the draw still used. How: wrap between draws; an oversized put takes the synchronous path. Old:
  `PELOAD_D3DGL_RINGWRAP_OLD=1`. (10-16 1a80367)
- **Buffer-backed SRVs** — buffer SRVs bound as GL texture buffers (R32UI copy, re-uploaded per version); VS-stage
  resources bound (units 16..31, VS SRV slots widened to 32). (10-16 f6c958e)
- **Submission thread: no per-frame drains** — d3dsubmit.c. Why: readbacks and writes drained the whole queue every frame.
  How: Map(READ, DO_NOT_WAIT) of a staging texture answers DXGI_ERROR_WAS_STILL_DRAWING while a queued copy into it
  hasn't run; Map(WRITE) renames the block (like WRITE_DISCARD, copying the newest contents) when no queued write
  targets it. Old: `PELOAD_D3DGL_SUBMIT_DRAINREAD/_DRAINWRITE=1`. (10-17 7101435)
- **Submission thread: fresher occlusion results** — Why: staler results made the game draw ~5% more. How: queries
  harvested every 128 commands, 1 frame of lead by default. (10-17 7d12a64)

- **GPU copies from MRT-only render targets and copy destinations** — `gpu_copy_src_ok` / `d3dgl_gpu_copy`, `ctxi_CopySubresourceRegion`, peload.c. Why: a GPU-side copy needed the source's own FBO; a target only ever drawn as part of an MRT framebuffer, or a texture whose content came from a GPU copy (`copydst`), has none, so copies from it fell back to its CPU copy -- zeros (halo3 reads one pixel of such a 1920x1080 RGBA8 texture back every frame). How: such a source gets its own FBO on demand (`d3dgl_fbo_for`, which keeps the existing storage) and takes the GPU copy + small-readback path. Old: PELOAD_D3DGL_NOMRTCOPY. Diagnostic: PELOAD_TRACE_SMALLRB (copies / maps of <= 4x4 textures with their bytes). (10-18)

## 14. GL context, present, GL thunks, GL helper

- **Win64 -> SysV GL thunk** — `make_gl_thunk`. Why: macOS GL is SysV (rsi/rdi volatile), the guest calls Win64 (rsi/rdi
  callee-saved): a GL call clobbered rsi holding glow's Context -> crash. How: wrap every GL pointer handed to the guest
  with an argument shuffle that preserves rsi/rdi. (10-11 7dbbfce)
- **Thunk forwards 12 arguments** — Why: only 6 were forwarded, so args 7+ were garbage (glReadPixels' destination,
  glTexSubImage2D's pixels): the GL driver scribbled over the heap -- the root cause of the random glow
  "glDeleteProgram not loaded" / BTreeMap / sig6 crashes. (10-11 3da9e9c)
- **Typed wrappers for float-argument GL calls (GLMIX)** — Why: the generic thunk only remaps integer registers; Win64
  passes floats positionally, so glUniform{1-4}f, glTexParameterf, glVertexAttrib*f, glClearBufferfi got wrong values
  (egui's menu). How: typed ms_abi wrappers. (10-16 5666cce)
- **Thunk preserves xmm6-xmm15** — Why: callee-saved in Win64, scratch in SysV; egui's painter keeps pixels_per_point in
  xmm6 across GL calls and got 1280.0 back from a texture upload: every clip rect collapsed (empty Server Browser list,
  lobby names, map vote list). How: each thunk is `mov r11,target; jmp stub`; one shared stub forwards 12 args and
  saves/restores rdi/rsi and xmm6-15. Old: `PELOAD_GL_NOXMMSAVE=1`. (10-16 f125b76; supersedes the u_screen_size
  workaround of 5666cce)
- **Present thread draws the right frame** — Why: (a) the D3D present blit sampled `1.0 - uv.y`, turning the already
  right-side-up loading screen upside down (`PELOAD_BLIT_FLIP=1` = old flip); (b) MsgWait / swap Present / the GDI
  compositor run on threads without a current GL context, so nothing presented; a dedicated present thread owns the
  window context. (10-12..14 da262fb)
- **Present latch can't zoom into a corner** — Why: a small UI draw's viewport magnified a corner of the frame. How: keep
  the largest viewport seen, snap to the full texture when it covers >= 85%. (10-12..14 da262fb)
- **D3D render context init race-free** — mutex + double-check + barrier before publishing `g_d3dgl_ready` (two render
  threads could both create the context). (10-12..14 da262fb)
- **One GL context, any thread** — Why: the context belonged to the first thread; halo3 issues the end-of-frame
  CopyResource(back buffer <- frame) + Present + clears from another thread, which were silently dropped. How: one
  context made current by whoever calls, serialised by a recursive mutex. (10-16 90b5320)
- **Shared render/window contexts** — Why: CGLCreateContext(share) returned kCGLBadMatch (10009) because the render
  context used a different pixel format than the window's, so the window never saw the render thread's textures. How:
  build the render context from the window context's own pixel format (10-14 f2e8d97); opt-in while the D3D output was
  still black (4f28ecf, `PELOAD_D3DGL_SHAREPF`); on by default once the game frame rendered. Old:
  `PELOAD_D3DGL_NOSHAREPF=1`. (10-16 d69fd1b)
- **Present the region halo3 composited** — Why: the window showed the whole 4096^2 surface. How: on the end-of-frame
  CopyResource into the back buffer take the source target's last viewport (1920x1080) and sample that region; glFinish
  after that copy (later replaced by fences). (10-16 d69fd1b)
- **Fenced present queue** — Why: the present thread re-blitted the back buffer on a fixed 33 ms timer (window capped at
  ~30 fps) and the render thread glFinish()ed every frame. How: each engine Present copies the frame into one of three
  window-sized textures, drops a fence and signals; the present thread glWaitSync()s and shows that frame.
  Old: `PELOAD_D3DGL_NOPQ=1`. (10-16 0b2e4d6)
- **Vsync opt-in; no presenting while invisible** — Why: a vsynced soak froze ~105 s ("presentation callback stalled")
  while the display slept: a vsynced flush blocks while there is no vblank. How: vsync only with `PELOAD_VSYNC=1`; the
  present thread idles while NSWindow occlusionState isn't Visible (`PELOAD_PRESENT_HIDDEN=1` keeps presenting).
  (10-16 2222e76 -> 9d769ad)
- **Resizable / full-screen window** — the content-view size updates the NSOpenGLContext drawable (under the CGL lock),
  the frame is letterboxed. (10-16 b1b9379)
- **Honest title fps** — Why: loading screens spin at ~1000 presents/s and the title claimed it. How: capped at the
  screen rate, engine rate beside it. (10-16 f73252e)
- **GL helper: generated encoders only** — shim/tools/glh_gen.py. Why: an unencoded pointer argument crashed the native
  helper. How: the generator refuses any pointer argument without a size rule. (10-17 24f64f8)
- **GL helper: shared-memory wake** — Why: a wake pipe's end was closed under the helper (the game's CloseHandle once
  closed a low fd). How: os_sync wait/wake in shared memory; the remaining pipe write end sits at fd >= 4000.
  (10-17 24f64f8, CLAUDE.md)
- **GL helper: fewer round trips** — a program link asks for all uniform locations and block indices in one round trip
  (map loads ~26k -> ~600 round trips per 2 s); occlusion results polled in order; GetError -> 0 and
  CheckFramebufferStatus -> complete without a round trip (`PELOAD_GLH_SYNCERR/_SYNCFBO` = synchronous). (10-17 8ec3407,
  24f64f8)
- **GL helper: death is reported** — the loader exits with a clear message if the helper dies (instead of hanging).
  (10-17 24f64f8)
- **GL helper encoder lock** — Why: the recursive mutex took the slow path on every GL call. How: os_unfair_lock.
  (10-17 69f3fc5)
- **GL helper: depth readback froze** — Why: two alternating readback tickets, one never collected, so "visible only"
  and zones used a stale depth. How: one ticket in flight (`glh_rb_request`). (10-17 335a1fb)
- **GL helper: loading screen flipped / shrunk between games** — Why: when no helper frame came within 50 ms (the engine
  stalls while loading) the present thread fell back to the in-process blit, which has no frame of its own in helper
  mode. How: re-present the helper's last IOSurface with the layered loading screens on top (`glh_pq_acquire` stale).
  Old: `PELOAD_GLH_PQ_FALLBACK=1`. (10-17 335a1fb)

- **GL helper wake / wait on macOS before 14.4** — Why: `os_sync_wait_on_address` / `os_sync_wake_by_address_any` (the
  ring's cross-process sleep) exist only from macOS 14.4, and the binaries were built for macOS 26 (the build machine), so
  neither would start on an older Mac. How: build.sh targets macOS 13 (`MACOS_MIN`); glh_shm.h `glh_addr_wait_ms` /
  `glh_addr_wake` use the os_sync calls when available, else the kernel's `__ulock_wait` / `__ulock_wake` with
  `UL_COMPARE_AND_WAIT_SHARED` (what libc++'s atomic wait uses, macOS 10.12+; checked across two processes natively and
  under Rosetta). (10-18)

## 15. CPU particle compute (dxbc_cpu.c)

- **Compute on the CPU** — Why: macOS GL 4.1 has no compute shaders/SSBOs; halo3's particle kernels never ran (no muzzle
  flashes, explosions). How: an SM5 compute interpreter over CPU copies of the RWStructuredBuffers; written UAVs bump
  their version; ClearUnorderedAccessView{Uint,Float}, CSSetSamplers, CSSetConstantBuffers1 hooked. Off:
  `PELOAD_NOCSCPU=1`. (10-16 f6c958e)
- **Texture cache locked** — with chunked (optionally GCD-parallel) dispatch. (10-16 a5d64b1)
- **Per-program immediate constant buffer** — Why: kernels used the translator's shared `g_icb`, clobbered by later
  translations. (10-16 f5ae033)
- **4-component immediates** — Why: `l(a,b,c,d)` was broadcast from lane x (`and r2.yz, r2.zzwz, l(0,65535,65535,0)` gave
  0), so the kernels unpacked their half-float fields to zero (found by the GPU A/B, PELOAD_CS_GPUCHECK). How: per-lane
  immediates. Old: `PELOAD_CS_IMMBCAST=1`. (10-17 be9dabb)
- **Compute programs trimmed and shared** — `cs_compile`. Why: each kernel reserved a fixed 8192-instruction array (~9 MB)
  and every map load recompiled byte-identical kernels (+18-27 MB a minute over map changes). How: trimmed to the
  program, compiled once per bytecode on the canon; duplicates run it and are freed when released. (10-18 80381f4) No
  duplicate freeing under `PELOAD_AB=csgpu` (10-18 410a6ee).
- Performance work (async workers, SIMT, input snapshots): section 19.

## 16. Client exe patches and hooks

Where every point is in every release, with signatures: patch-addresses.md (sections 2-3, 6, 8.6; v0.9.20 table in 3).
No fixed exe RVA survives a release; the loader finds patch sites by signature (`g_sigpatches`, `sig_find`: a unique
match in the executable sections, original bytes checked) and reports what it can't find as `[sigpatch]`.

- **UI re-entrancy patch** (`ui-reentrancy`, always on) — `reentrancy_fix` / `g_sigpatches`. v0.9.13 exe+0x553add and
  +0x553af1; v0.9.18 0x65703d / 0x657051; v0.9.20 0x6e0d3f. Why: ui_composition's once-per-second cache cleanup
  write-borrows the app-state thread_local RefCell while the outer borrow from the first call is still held -> "RefCell
  already borrowed" panic (thread/local.rs:757) -> ud2. How: NOP the borrow-conflict panic branch (`jne rel32`) and the
  drop-dispatch branch (`jne rel8`), so it takes the no-drop path that only resets the borrow flag (the 8 cached
  sub-objects are all NULL that early). Old: `PELOAD_NO_REENTRANCY_FIX=1`. (10-12 6c076a1; signature 10-16 a574219)
  The panic's main cause, a leaked borrow after failed D3D/DXGI queries, was fixed in section 10 (a800d1b, 9476f8c).
- **XAudio2 integrity-check bypass** (`xaudio`, PELOAD_RECLAIMER_PATCHES) — the client compares halo3's four XAudio2
  callback slots (halo3+0xa39e10/18/20/28) with the retail callbacks (halo3+0x60c416/0x60c422/0x60c40a/0x60c385) before
  installing its headless-audio replacements; with the loader's XAudio2 in the slots it refused ("XAudio2 import slots
  differ from the supported build; refused") and local-server / the real client exited. How: NOP the four mismatch
  `jne`s so the install always runs. v0.9.13 sub_140460250, exe+0x460294/0x4602cb/0x460302 (6 bytes) + 0x460335 (2),
  first as a manual PELOAD_PATCH (10-13, in da262fb); signatures for v0.9.13/v0.9.18 (0x53d134/16b/1a2/1d5 in FUNC
  0x53d0f0) 10-16 a574219; v0.9.20 moved the check into a table-driven routine (exe+0x5c2de0: `cmp [rsp+0x30],reg ; jne`
  after `add reg,r13`), sites 0x5c2e50/7f/b6/eb, new signature variants (10-18 6113b66).
- **Engine hang watchdog skip** (`watchdog`, PELOAD_RECLAIMER_PATCHES, needed only with RECLAIMER_DIRECT=1) — `je` -> `jmp`.
  v0.9.13 exe+0x65ddf9 (10-16 338a146), v0.9.18 0x7963f6 in FUNC 0x793f40, v0.9.20 0x82baf1 (deadline in rbx: second
  signature, 6113b66). Why: the worker's watchdog ("still running after N seconds", 15/45/240 s) gave up on the engine
  thread and the client stopped pumping input. Finding (10-16 40cd6a3): it arms only for time-limited runs --
  RECLAIMER_LOCAL_RUNTIME unset (the direct-match probe) or "<role>:<secs>" with secs > 0 (parsed at exe+0x6a09e0); the
  menu runtime passes "menu:0" and never arms it, so play.sh's menu mode doesn't need the patch.
- **Patch slots** — `g_sigpatches` "slot" field. Why: a new build compiles a check to a different shape; reporting every
  unmatched variant was noise and a missing one was easy to miss. How: a build needs one variant per slot; only slots
  with no matching variant are reported. (10-18 6113b66)
- **Guardian-refusal NOP** (v0.9.13 only, manual `PELOAD_PATCH=exe+4e066d:909090909090`) — forced the Guardian
  map-resolution result hub (`jno` to "Guardian configuration refused") to succeed on the old direct-probe path; with
  `exe+4e1950:ebfe` (spin the engine_shell STOP function) it held the window open. Debug/workaround patches of 10-12/13,
  obsolete with the menu path; the per-release sites are in patch-addresses.md section 3.
- **set_muted validated by meaning** — `st_voice_install`. Why: the set_muted signature also matches a Forge
  `Mutex<Vec>` setter in v0.8.16 .. v0.9.1; the stats page would have called it for a mute and read the Forge list as
  the mute list. How: a hit is accepted only if an is_muted-shaped function locks the same byte; the old-shape signature
  added for builds up to v0.9.1. (10-17 8c6b6ff)
- **Entry hooks compare their prologue** — voice_recv (`st_on_voice`, v0.9.18 exe+0x684080), push_history (`st_on_chat`,
  exe+0xb89930): 19 bytes compared before relocating, refused if different. Note (patch-addresses rule 6): a `??` inside
  the compared bytes becomes 0 and fails silently; copy real bytes from the image. (10-17 e400dae, d495340)
- **cast_vote on a guest thread** — `st_bv_*` (v0.9.18 exe+0x8f9fe0, Ballot cache exe+0x27d1ce0; v0.9.20 0x9b3910 /
  0x29f7838, size read from the code). Why: clicking a ballot card never worked on a joined client (its loading splash
  covers the lobby). How: call the client's own cast_vote(&round, choice) on a worker made with our CreateThread (it
  needs a TEB); confirmed by the next broadcast's count. (10-18 ddfcab0)
- **max_players getter in both prologue shapes** — `st_slots_install`. Why: v0.9.20's getter also pushes rdi, the first
  signature missed it. How: a second signature that finds the clone call by what follows it (`test rax,rax`); max
  players at Arc+0x233 (was +0x22d). (10-18 8968d57, 6113b66)
- **Moderation table found from code, read safely** — Why: the v0.9.18 fixed RVA (exe+0x27cf790) doesn't survive a
  release. How: the first `lock cmpxchg byte [rip+d]` before the code that formats "You are banned from this server.
  Reason: "; the table is accepted only with its lock-cmpxchg partner, read without the lock and validated twice.
  (10-18 f4720fc, 6113b66)
- **Workshop-download helper replaced** — `i_CreateProcessW`. Why: the client's `workshop-download --subscribe|--update
  <id>` helper needs Steam ("Steam Workshop is unavailable"), and with `PELOAD_WORKER_INHERIT_STDIO=1` its JSON went to the
  loader's log so the parent read EOF. How: run shim/tools/workshop-helper.sh (DepotDownloader) on the client's pipes in
  its own process group, always wiring STARTUPINFO stdin/stdout. Old: `PELOAD_NO_WORKSHOP_HELPER=1`. (10-17 6810207)
- **Runtime-mode override** — `PELOAD_LOCAL_RUNTIME=<value>|__unset__` rewrites RECLAIMER_LOCAL_RUNTIME in a spawned
  worker's environment (exe fn 0x599920 v0.9.13 reads it: menu:0 -> stage 0, unset -> stage 2). The direct-match path
  uses `__unset__`. (10-12..14 da262fb)
- **Do not hook**: halo3+0x118a1c game_engine_player_killed (the client detours it itself and checks its 15-byte
  prologue) and halo3+0x1b8e00 (a boundary function whose entry the client checks: "Native function contract mismatch").

## 17. halo3.dll patches and detours

All at fixed RVAs of the one MCC halo3.dll available (patch-addresses.md section 8.1-8.4, re-finding after an MCC
update: section 9). Hooks marked "checked" compare the prologue first and refuse on a mismatch.

- **Input-layout null guard** (`PELOAD_SKINGUARD`, set by every launcher) — `skinguard_install`, entry detour on
  halo3+0x2ac24c (14 bytes relocated, NOT checked): `test r8,r8 ; jz -> xor eax,eax ; ret`. Why: round start faulted at
  RVA 0x2AC329 (`mov rax,[r14]`, r14 = 0). First misread as an armor-skin key registry; it is the input-layout builder
  (called by the cache halo3+0x2aacf0, CreateInputLayout with the VS blob from GetPrivateData). The NULL came from the
  missing SetPrivateDataInterface, fixed later (section 10), so the guard may now be unnecessary (untested). The parent
  already tolerates a 0 result. (10-12..14 da262fb; name corrected 10-18 09368ef)
- **NULL-vtable resource release guards** (`PELOAD_VTGUARD`, set by the launchers) — `vtguard_install`. Why: a stub-GPU
  resource with a NULL vtable reached a refcount release: 0xC0000005 at RVA 0x172BD4 ~6 s into gameplay; later the same
  in a shared_ptr release (RVA 0x172FA0) when a player joined a match in progress. How: mid-function detours at
  halo3+0x172bce (sweep FUNC 0x172ad0) and +0x172f9a (FUNC 0x172e88): NULL vtable -> jump to the entry cleanup (skip
  0x172bee / 0x172fb7; leaks that stub). (10-12..14 da262fb; second site 10-16 d858a8b)
- **object_delete entry hook** (checked) — halo3+0x342ed8 (`hbdel_install` -> `hb_on_delete`): bullets live < 1 tick, so
  shots are recorded at deletion. Off: `PELOAD_NO_DELHOOK=1`. (10-16 8448a06)
- **Frame safe point: main_loop_body entry hook** (checked) — halo3+0xb2250 (`hb_frame_hook`). Why: the first teleports
  ran from a hook on halo3+0x9d22c, which is `system_milliseconds` (421 call sites, many mid-tick: physics, object
  walks), without the game-state lock and on network clients -> crashes; a QPC-gated job (10-16 793d725/3f4b315)
  crashed too (thread lacked TLS +0x240). How: engine jobs (teleport, boundaries, voice mutes) run at the top of each
  main-loop iteration, before the tick loop, inside the game-state lock halo3+0x12c948 / 0x12c9a0. (10-17 2834479)
- **Teleport made safe** — halo3+0x259410 (prologue checked), unit_exit_vehicle 0x3a3c4c, scenario_location_from_point
  0x9bc78. Why: the first F7 teleport crashed in the physics update: the point reflected through the map centre lay
  under Valhalla's far floor. How: snap to the nearest weapon/equipment spawn (10-16 2445619); later: host only
  (game_globals+0x11 == 5, or 1 local), leave the vehicle first, destination checked inside the map structure at +0.3
  and +1.2, |coord| < 30000 (past 32768 the host deletes the unit) (10-17 2834479); `tpto&slot=` works on a host with no
  camera (dedicated worker) (10-18 b02cc4b). Docs: teleport.md.
- **display_event entry hook** (checked) — halo3+0x11b0ec (`st_events_install`), exact kills. Off: `PELOAD_NO_EVHOOK=1`.
  (10-17 6810207)
- **chud_messaging_post through `entry_hook_tail`** — halo3+0x2d3f2c. Why: its prologue is rip-relative (not relocatable
  by the plain entry hook). How: a custom replay tail re-runs `sub rsp,0x28 ; cmp ecx,4 ; jge`, loads the TLS index from
  halo3+0xa39f9c and resumes at +0x10; r9 carries the caller's return address. (10-17 6810207)
- **Map-boundary switch** — halo3+0x37b816 `0F 85 54 01 00 00` -> `E9 55 01 00 00 90` and +0x37b734 `74` -> `EB` (boundary
  tests of FUNC 0x37b6c8: alive outside the safe zone), halo3+0x219c1f `75 06` -> `EB 46` (biped ground test FUNC
  0x2199c4: slip surfaces, BSP flag 0x40, count as ground). Why: the entry patch of halo3+0x1b8e00 is refused by the
  client. How: written/restored by the simulation thread at the frame safe point, all or nothing after comparing the
  expected bytes (stats `do=bounds`). (10-17 7087fbc, e24ef55, 34bd9a5) A boundaries-only job no longer falls through to
  the F7 map-reflection teleport (e24ef55).
- **Decompression-constant capture** (`PELOAD_D3DGL_CAPCB1`, superseded) — trace detour on halo3+0x28e8e0. (10-15 31162a5)
- **WavesFX table prefill** — halo3+0x46bb780.. (section 9). (10-11 7f5b6ac)
- **Game-changing switches (features, not fixes; listed so their writes are known)** — `PELOAD_H3=name=value` writes
  HaloScript globals every 0.5 s (10-16 e998a40); stats `do=speed` writes the variant's base speed trait
  (game_globals+0x1608+0x134+0x10, host) (10-17 6810207); `PELOAD_TEST_BOOT_PROMPT` writes player +0x1F0 (10-17 26e4054);
  `PELOAD_AB=ncpu` pokes the CPU count at halo3+0xa3a48c (10-18 da5b4a7); generic `PELOAD_PATCH` / `PELOAD_HOOK` /
  `PELOAD_POKE` / `PELOAD_BP` debug tools (10-11 fabd4b7, a800d1b).

## 18. Overlay, stats, forensics, walker, recording (the loader's own tooling)

- **No memory sweeps** — `PELOAD_OBJSCAN` (removed). Why: walking every VM region of the ~6 GB process with
  mach_vm_read_overwrite to find halo3's object array kernel-panicked the Mac. How: removed entirely; engine structures
  are found through halo3.dll's own code/data only (rule in hitbox.c / CLAUDE.md). (10-16 0ff8ddf)
- **Overlay camera** — `hb_is_world_camera`. Why: full-screen passes carry a view-only projection and were picked as the
  camera. How: the first draw whose VS cb0 row 7 (camera position) sits at the clip-space origin of rows 0-3.
  (10-16 f73252e)
- **Map paths always recorded** — `hb_note_map` in CreateFileW. Why: turning the overlay on mid-match found no collision
  models (the .map was opened before). (10-16 c1f12f8)
- **Per-object collision permutations** — Why: every object drew the same permutation (Elite heads ignored the helmet).
  How: each object draws the permutations its region bytes (datum+0x13A) select. (10-16 635bc46)
- **hb_tag_name thread-safe** — for the stats thread. (10-16 4621a04)
- **Label text a fixed pixel height** — Why: sized from a stale 1280x720, ~4x too large. How: `PELOAD_HB_TEXT_PX` (7 px
  default), page Text -/+. (10-17 694b38e)
- **Mute acts on press** — stats page. Why: clicks were lost to the 10 Hz row redraws. (10-17 694b38e)
- **Seated players' positions** — Why: the object origin (+0x50) is vehicle-relative while seated, so seated players
  were placed wrong. How: the world-space bounding centre (+0x1c). (10-17 9e619d0)
- **Variant weapon/vehicle sets** — halo3 keeps "use the map's own" as -1 in 16-bit fields the client's option table
  doesn't list; shown as "Map default". (10-17 2e6791c)
- **Exact local player** — Why: "(you)" and the friend/enemy split guessed from the camera, wrong in third person,
  vehicles and while dead. How: halo3's player mapping (TLS +0x110, output user -> player handle at +0xB8); camera guess
  only as a fallback. (10-17 6d99f5b)
- **Overlay F-keys retired** — Why: the loader consumed F2-F10, so the client's F5/F6 vote keys never reached it. How:
  every switch is a stats page button; keys only with `PELOAD_HB_FKEYS=1`. (10-17 335a1fb)
- **Sparring aim only in the input-less client** — Why: the user switched it on in their own client and it took over their
  controller. How: available only with `PELOAD_NO_PAD` (shim/target.sh). (10-17 b2c71ee)
- **Vehicles switch independent; weak spots for vehicles only** — the Vehicles button draws vehicle hitboxes + weak spots
  on its own, Hitboxes = players' (10-18 29299e3); weak-spot colouring applied only to vehicles (10-18 353a833).
- **No sphere stand-ins** — the bounding-sphere fallback and `PELOAD_HITBOXES=all|spheres` were removed: objects without
  a collision model get nothing (user). (10-18 912250c)
- **Autosave .map files are not "the map being played"** — `hb_note_map` keeps only real cache maps ("daeh" v13).
  (10-17 8ec3407)
- **The map being played after a map change** — `hb_current_map`. Why: it was the newest .map the game opened; around a
  change the client opens maps it borrows skins/tags from, so the heatmap was filed under the wrong map, the walker
  loaded the wrong graph, tag names and hitbox geometry came from the wrong map (backlog 44). How: the recent map whose
  tag table matches the most live objects' definition datums, rechecked every 2 s; per-definition caches reset when it
  changes; 32 recent paths (was 8), 48 open map files (was 6) (10-18 cef684c); then the engine's own scenario path
  (game_options +0x44) picks the file, 256 live definitions sampled, ties keep the current map (10-18 ab7f719).
- **Zones never drawn without occlusion** — `hbz_gpu`. Why: a frame that met the depth readback mid-update drew every zone
  surface unoccluded (push-back ceilings flashed over the view). How: always the last complete depth copy and its
  camera; none = no zones. (10-18 3eb0680)
- **Zones at the screen edges while turning** — Why: zone pixels outside the 2-frame-old depth frame's view were drawn.
  How: hidden. (10-18 cf5f7ae)
- **Depth readback only when needed** — runs only while an enabled feature has visible-only on, or zones are on (off means
  off). (10-18 e1411a9)
- **Deaths of leaving players** — a death within 3 s of that player leaving isn't shown (leaving removes the body)
  (10-17 d495340); estimated deaths wait 2 s for the engine's exact event (10-17 6810207).
- **Stats port on every launch** — Why: a map generator inherited the listening socket and kept 8765 bound after its game
  was killed ("cannot reach the loader"). How: port retried (10-18 1f9123b); listener FD_CLOEXEC, generators spawned with
  only their stdio, collection starts whether or not the page can be served (0261ef3); no fallback port: the game waits
  for 8765, a leftover generator holding it is killed (0faafe7).
- **Stats control CSRF guard** — POST /control needs the `X-Reclaimer-Control` header (other sites' pages can't send it);
  `PELOAD_STATS_READONLY=1` disables control. (10-17 cfca0e7) Player addresses hidden when the page is reachable from
  other computers unless `PELOAD_STATS_IPS=1` (10-18 f4720fc).
- **Slots and roles** — no slot count in the menu (its local session is not a game) (10-18 d157084); addr_role "client"
  also from the session when the join link isn't on the command line (10-18 1ddbf41).
- **Heatmap frame fits the data** — Why: anything > 20 units outside the picture discarded all data bounds (truncated
  heat on big maps). How: picture + 0.5..99.5% of footprint cells + every death/player within a margin. (10-18 1ddbf41)
  The heatmap only switches maps when there is data (menus open campaign maps for scenery). (10-17 9e619d0)
- **Forensic voice payload** — Why: the payload is a frame-count byte (always 1) + one Opus packet; readers decoded from the
  count byte (half the packets invalid, garbled playback). How: skip it. (10-18 4edd201)
- **Forensic sessions per server** — Why: the menu's background scenes opened a session each. How: sessions follow the
  host peer id; map changes stay in the server's session. (10-18 e9c3b31)
- **Forensic session delete** — Why: a session folder holding anything unlink() refuses (a subfolder) lost its
  session.json first, vanished from the list and stayed on disk; a loader older than delete=all refused Clear all ("could
  not be deleted completely"). How: fx_rm_tree removes everything inside (folders too, 8 deep; symlinks removed, never
  followed) and session.json goes last, so a folder that can't be emptied stays listed and is named; the page's Clear all
  deletes what is still listed one by one after delete=all; Select all / Select none. (10-18)
- **Forensic page: unreadable session** — Why: a session folder without voice.bin (index.json answers
  {"ok":false,...}) threw in the page (X.utterances not iterable) and left the previous session's timeline up. How: the
  summary shows the loader's error and the timeline / list are cleared. Delete selected / Clear all (delete=all) added
  beside it. (10-18)
- **Forensic voice-control flood cap** — Why: 1,053,203 control packets in 44 s from our local dedicated server (the
  NtDelayExecution bug). How: at most one control packet per sender per 50 ms kept; the rest counted. (10-18 bfc4650)
- **Spawn events** — security cameras are vehicle objects: left out (10-18 bfc4650); an object counts as a spawn only
  after two parentless snapshots (one caught before its attachment is no spawn) (10-18 1ebdaa4).
- **Walker** — nav.c / re-tools/h3nav.py: pitch-sign detection needs two readings and locks; back off + sidestep before
  jumping when stuck (10-17 ab42bef); graphs and map pictures made only once gameplay started (the menu borrows other
  maps); hotspot danger relative per map (a fixed scale made 59% of Valhalla "hot") (10-18 d1dbce9); vehicle mode: nearest
  in 3D (it chose a Hornet 120 units overhead), an out-of-reach vehicle skipped for 60 s (it re-planned to it forever),
  look only within 8 units of height (10-18 aa26f27); a vehicle it got stuck on 4 times skipped (10-18 fdf5717).
- **Graph builder can't swamp the Mac** — Why: Forge World's 16 giant outer-shell triangles made billions of samples; the
  builder grew past 6 GB and pushed a 105-player game into 37 GB of swap (3 fps). How: shells left out, a sample budget
  picks coarser cells, per-chunk binning, nice 10, killed past `PELOAD_NAV_TOOL_MB` (10-18 f4720fc); the budget counts
  real samples (long triangles cut), kill limit 4096 MB (10-18 ad4ca59).
- **Recording time scale** — rec.m. Why: the default 600 time scale rounded two frames onto one timestamp. How: 90000.
  (10-18 4edd201) `PELOAD_REC_DIR` keeps test recordings out of ~/Movies (cf2f4f5).

## 19. Performance

Numbers are from the commit messages; PERF-PLAN.md has the measurements. Compare only inside one match
(`PELOAD_AB=<feature>:<secs>`); scenes vary too much between runs.

- **Cached environment lookups** — every `PELOAD_*` getenv cached (`peload_cgetenv`; per-draw env scans) (10-16 90b5320);
  literal-key `GENV1` cache (10-17 694b38e, 7101435); duplicate cache removed (2f24545); HeapAlloc no longer calls getenv
  per allocation (10-17 6defccb).
- **Upload only the referenced range** — every draw re-uploaded whole multi-MB dynamic VBs; now BufferSubData of the
  range (any Map invalidates). 8 -> ~21 fps. (10-16 90b5320)
- **Streaming ring, uniform blocks, bind caches** — dynamic (NO_OVERWRITE) VB/IB/CB data copied into a loader GL buffer
  through UNSYNCHRONIZED maps, the draw rebased by the scanned min vertex (glBufferSubData into in-flight buffers
  synchronised ~19 us each); CBs as std140 uniform blocks bound by range (not glProgramUniform4fv copies); per-unit
  texture/sampler bind cache. 18 -> 85 fps. Old: `PELOAD_D3DGL_NORING[_VB|_IB|_CB]`, `_NOUBO`, `_NOBINDCACHE`.
  (10-16 338a146)
- **Program bind cache** — `gs_useprog` / `gs_pipeline`. Why: Apple's GL revalidates even a redundant glUseProgram
  (2.09 us vs 0.60 for a draw). 3.81 -> 2.94 us per draw. (10-16 4afd0a9)
- **GL state shadow** — per-draw blend/raster/depth/viewport/scissor/caps and the shared VAO's attribute arrays: only
  changes sent. Old: `PELOAD_D3DGL_NOSTATECACHE=1`. (10-16 0b2e4d6)
- **First-use shader hitches** — Why: a new VS/PS pair cost ~37 ms (translate 6 + compile 21 + link 10); a grenade
  introduces ~13 pairs: explosions froze ~0.4 s. How: per-shader mono GLSL built once, compiled shader objects cached per
  interface-padding mask, identical-bytecode shaders share programs (`o->canon`, `PELOAD_D3DGL_NOCANON`), every linked
  pair recorded in ~/Library/Application Support/ReclaimerMac/glpairs_v1.txt and pre-built during the next loading
  screen (`PELOAD_D3DGL_NOPRELINK`); program table 1024 -> 4096. (10-16 ea5d28b)
- **Particle compute 2x + asynchronous** — swizzles precomputed (38 -> 19 ns/instruction), textures decoded once per
  dispatch, dispatches queued on a worker (CBs and read-only buffers snapshotted, written UAVs pending, the render thread
  waits only where it reads them). Median 77 -> 116 fps. Old: `PELOAD_CS_SYNC=1`. (10-16 f5ae033)
- **SIMT particle compute, shared snapshots** — one instruction across 64 lanes (active lanes only), dependency-ordered
  workers (`PELOAD_CS_WORKERS`; play.sh: performance cores - 2), read-only inputs copied once per resource version (was
  ~0.5 GB/s). 235 -> 87 us per dispatch. Old: `PELOAD_CS_SCALAR=1`, `PELOAD_CS_NOSNAPSHARE=1`; `PELOAD_CS_CHECK` compares.
  (10-17 7101435, 24f64f8)
- **GL submission thread** — d3dsubmit.c, `PELOAD_D3DGL_SUBMIT=1/0/auto`. The render thread records D3D11 calls, a GL thread
  executes them; +10-15% with 60-100 players, -7% alone, so `auto` = on at 40+ session players (play.sh).
  (10-17 7101435, 51593d4)
- **Big guest copies on the host's memmove** — vcruntime memcpy/memmove/memset >= 2 KB. Old: `PELOAD_GUEST_MEMCPY=1`.
  (10-17 7101435)
- **Mip-streaming upload skip** — partially streamed high mips aren't re-uploaded. Old: `PELOAD_D3DGL_NOMIPSKIP=1`.
  (10-17 694b38e)
- **Fewer texture-bind calls** — active texture unit cached, a slot whose texture is unchanged only rebinds its sampler.
  Old: `PELOAD_D3DGL_NOSPLITBIND=1`. (10-17 aeca05a) FBO bind cache (`PELOAD_D3DGL_NOFBOCACHE`). (10-17 7101435)
- **Native GL helper** — glh_client.c / glhelper.c: GL calls of the render context encoded into a 256 MB shared-memory
  ring and replayed by a native arm64 process (Apple's driver ~1.5x faster natively). BTB 13-25 -> median 49 fps; direct
  Guardian 122 -> 204. Old: `PELOAD_GL_HELPER=0` (play.sh default 1). (10-17 8ec3407, 24f64f8)
- **Copy-on-write game-state copies** — host memmove stub. Why: halo3+0x13f23f copies its ~14 MB game state ~5 times a
  frame. How: copies >= 1 MB between private memory with the same offset modulo 16 KB go COW with mach_vm_copy. +5-7%
  (Guardian), +4% (BTB). Old: `PELOAD_MEMCPY_COW=0`. (10-17 3dbbd5a; off 8035d4e; on again 10-18 cef684c)
- **GetCurrentThreadId cached per thread** — ~900k calls a second on the engine's main thread in big games. (10-17 ab42bef)
- **Import fast paths** — fastcall.c, `PELOAD_FASTCALL=all|tid,heap,keys` (play.sh: all). Why: an ms_abi import that calls
  any SysV function, reads a `__thread`, or gets a SysV `.cold.N` part saves xmm6-15 on every call (~4 ns under Rosetta).
  How: GetCurrentThreadId from the TEB (+0x1FF0), GetAsyncKeyState table leaf, a per-thread small-block heap cache
  (<= 1 KB, malloc's 16-byte classes, 2 GB arena, TEB +0x1FF8); the originals are noinline. +1.3% median (128 players).
  (10-18 4ddbac5, a35803f)
- **Compressed BC textures** (section 12), **NtDelayExecution** (+11.8%, section 2), **real CPU count** (+1.7%, section 2).
- **Measured and left opt-in (neutral or worse)** — GL state cache `PELOAD_D3DGL_GLCACHE=1` (31 -> 18 calls/draw, no fps
  change) and batched CB maps `PELOAD_D3DGL_NOCBBATCH` (10-17 3a5346d); parallel big copies `PELOAD_PAR_MEMCPY` (COW beats
  it by 7.7%, 10-18 353a833, cf3d728); GPU particle compute as transform feedback `PELOAD_CS_GPU=1` (correct, slower on
  Apple GL, 10-17 be9dabb); Apple's multithreaded GL engine in the helper `PELOAD_GLH_MP` (b2cf132); invariant
  gl_Position for the rock-wall seams `PELOAD_D3DGL_INVARIANT=1` (not confirmed by eye, 10-16 4621a04); the voice-thread poll
  damper `PELOAD_POLL_DAMP=1` (it slept inside the key poll exe+0x7385e0, which holds a lock the main thread takes:
  sluggish sticks; default off since 10-17 37be8a7, obsolete since NtDelayExecution).

## 20. Scripts and RE tools

- **build.sh builds to a temp file and renames** — safe while a game is running. (10-16 fe9e948)
- **play.sh** — checks the client exe's SHA-256 (patches were offsets into one build) (10-16 15e3edd); matches only the
  loader binary itself when killing leftovers (an unanchored pattern also matched shells mentioning build/peload)
  (10-16 f73252e); leaves a running target / dedicated server alone (10-16 f4eb1e6, de84a5b); its stdin feeder no longer
  holds the caller's stderr and is killed when the game exits (it held a caller's `| tail` for 24 h) (10-17 0465d03);
  frees the stats port first (10-18 0faafe7); uses the newest client/project-reclaimer-client-v*.exe (servers refuse an
  old client within hours of a release) (10-18 37c15de).
- **quiet-run.sh** — no longer minimizes by default (`QR_MINIMIZE=1`), anchored process match (10-16 f73252e); frees the
  port (0faafe7); serves the source pages (d157084).
- **target.sh** — waits after typing the map name (55811fa) and 30 s (was 12) after a match before Start Game (the click
  came before the lobby was ready) (10-16 4621a04); no WASAPI for the target while the caulk crash was open (10-17 b02529c).
- **workshop-helper.sh progress** — Why: DepotDownloader prints a percentage only when a file finishes and verifies
  existing files silently; the client gives up after 45 s without progress ("Steam Workshop download timed out" at 863 MB
  of 8.9 GB). How: every 2 s the larger of the percentage and the bytes on disk, at most 99%, +1 byte heartbeat.
  (10-18 3eb0680)
- **Workshop helper item id from the dedicated server** — Why: the dedicated server runs `workshop-download --mcc-path
  <path> <id>` (the id last, no flag before it); the substitution found no id ("item -") and the server reported "Steam
  Workshop is unavailable". How: a trailing all-digit token not preceded by --mcc-path is the item id. (10-18)
- **make-usb.sh / usb-install.sh** — Why: the stick from 10-16 had no GL helper (play.sh's default since 10-17: the game
  could not render on the other Mac), no Workshop mods, no map tools; an update re-checked or recopied everything and
  kept old client exes beside new ones; 1.2 GB of the client's per-launch scratch folders (ReclaimerCache/Temp) were copied.
  How: incremental (rsync size + mtime; each part lists the files it copied), the app folder staged exactly (--delete: only
  the newest exe, peload + glhelper), Workshop content, maptop / heatmaps / glpairs, Steam's library files; Temp excluded
  and removed from the stick; the installer copies only changes, keeps --delete inside the folders the stick owns,
  recreates "Program Files (x86)/Steam/steamapps" as a RELATIVE link (it was absolute, with this Mac's user name), checks
  macOS >= 13. (10-18)
- **re-tools/setup.sh** — oxidizer's `uv sync` fails on this macOS (pinned pyvex builds VEX, pycparser "Unmatched '}'");
  PyPI angr 10.0.3 wheels + bitstring 4.1.4 instead. (10-14 da262fb)
- **re-tools/dxbc2glsl.py** — kept byte-identical with the C translator as the oracle (the opcode-table, opcode and
  correctness fixes of section 11 are in both).
- **re-tools/h3bsp.py** — string ids: namespace at bit 19 (`sid_name`; it resolved the Warthog's "main" section as
  "bulletwater_6") (10-17 6defccb); the map picture covers the safe zones (Sandtrap's play area reached past the
  spawn/item box) (10-17 4865e4e); placements > 5 units outside the safe zones left out (The Pit's unreachable Warthog)
  (83a53fc); Forge-canvas maps frame their geometry (Forge World was a 26 x 153 strip) (10-18 1ddbf41).
- **re-tools/h3nav.py** — floor heights below z = 0 were clamped to 0 (Sandtrap had no real floor nodes) (10-17 ab42bef);
  memory budget (section 18).
- **re-tools/h3map.py** — its string-id decoding is wrong for these builds (namespace-0 continuations); use
  `h3bsp.sid_name`. NOT fixed (CLAUDE.md).

## 21. Superseded workarounds (kept for history; most switches still exist)

- **Separable GL programs** -> monolithic programs (da262fb). `PELOAD_D3DGL_SEP=1`.
- **Camera / vertex-decompression reconstruction** -> real shader reflection (90b5320). Before halo3 bound its own
  cbuffers: `PELOAD_D3DGL_SYNVP` synthesised cb0 from a pool CB (rejecting near-identity views, 10-14 eb91d35),
  `PELOAD_D3DGL_CAM` look-at camera (10-14 a6dfd0d), `PELOAD_D3DGL_DECOMP` uniform decompression scale + camera latch
  (10-15 c2dc7c0), `PELOAD_D3DGL_CAPCB1` per-section capture at halo3+0x28e8e0 (10-15 31162a5), `PELOAD_D3DGL_AUTOFRAME`
  + per-draw decompress gate (10-15 0bc2ce3); diagnostics RANGEDUMP / SKIPWIDE / SKIPDEGEN (62c1f07, de4be19, e57f057;
  a FIFO capture order was tried and reverted).
- **D3DReflect zero-resource stub** (454f140) -> real RDEF reflection (90b5320).
- **BC -> RGBA8 decode for every BC texture** (6112645) -> compressed with the mip chain (8c8e83e); TYPELESS BC still
  decoded (410a6ee).
- **egui u_screen_size patch in glUniform2f** (5666cce, `PELOAD_GL_NOFIXSCREEN`) -> xmm6-15 saved by the thunk (f125b76),
  patch removed.
- **glfix.c DYLD interpose** for glShaderSource (7dbbfce) -> pointer intercept in make_gl_thunk (2bafa13).
- **Foreground = last shown top-level window** (71b968e) -> Windows' activation rules (5d425e5), `PELOAD_FG_LASTSHOWN`.
- **Vsync by default** (2222e76, `PELOAD_NOVSYNC`) -> opt-in `PELOAD_VSYNC=1` (9d769ad).
- **Render-context sharing opt-in** (4f28ecf) -> on by default (d69fd1b).
- **Fixed-RVA PELOAD_PATCH lists for v0.9.13** (XAudio2, watchdog) -> signature patches (a574219).
- **"Startup watchdog parks input"** (338a146) -> understood as the direct-path engine hang watchdog (40cd6a3).
- **First file-mapping attempt** (bb35912) reverted (4135438) -> e6e2b02.
- **Teleport from halo3+0x9d22c / a QPC gate** (793d725, 3f4b315) -> main_loop_body hook (2834479).
- **Disposing WASAPI queues** -> pooled (b2c71ee); **target.sh without WASAPI** (b02529c) -> silent microphone (0c5285c).
- **Poll damper** (7d12a64) -> NtDelayExecution (b02cc4b).
- **Bounding-sphere overlay** (f73252e) -> collision-model hitboxes (76a80c8); sphere fallback removed (912250c).
- **Object freeing / COW toggled off** (8035d4e) -> on again after the UAF fixes and a clean soak (cef684c).
- **Input-layout guard (`PELOAD_SKINGUARD`)** -> its NULL came from the missing SetPrivateDataInterface (fixed); still
  set by every launcher, removal untested.

## 22. Known open (not fixed)

- **halo3 patches after an MCC update** — skinguard, vtguard, capcb1, `waves_prefill` and the unchecked calls write or jump
  at fixed RVAs without a check (patch-addresses.md section 9).
- **client.log stays empty** under the loader (opened, never written; cause unknown). The dedicated supervisor never fills
  data/<server>/server.log (it reads the worker pipes; not chased).
- **`PELOAD_WORKER_INHERIT_STDIO=1` breaks parents that pipe child stdio** (the dedicated supervisor saw EOF and
  restarted servers in a loop) -- don't use it there; `PELOAD_STDERR_TEE` instead.
- **CreateProcessW applies the child's environment with setenv() in the loader itself** (SteamAppId stays set after the
  first Workshop helper; GUESSED harmless; workshop-helper.md section 6).
- **Runtime-compiled shaders are no-ops** — D3DCompile returns a placeholder DXBC without code for halo3's 5 HLSL shaders
  (video playback CopyToScreen / YUVToRGB + VS, a night-vision-style luminance CS); backlog 67. `PELOAD_REFUSE_EMPTY_SHADER=1`
  refuses such blobs with E_INVALIDARG as D3D11 would.
- **First join right after a dedicated server starts** can fail its handshake; a retry works.
- Backlog 60 (client crash in object_get_origin on a deleted object during Forge placement sync, seen once), 61 (Hugegrass
  renders nearly black wherever its own lighting matters -- emissive surfaces, the sky and the HUD are fine; not a regression:
  dark with client 0.9.18 too and with every 10-17/18 change reverted; stock maps such as Valhalla render normally with
  0.9.20), 56 (Forge World ground black, probably the same), rock-wall seams (tabled by the user).
