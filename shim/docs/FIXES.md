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

- **Play by double-click: Reclaimer.app** (2026-10-19) — in the release bundle's top folder, made by shim/build.sh in a
  checkout and by install.sh on a USB install; `bash shim/tools/make-app.sh --applications` puts one in ~/Applications.
  A downloaded bundle needs `xattr -dr com.apple.quarantine <bundle folder>` first (the app is signed ad hoc). Details:
  section 20 "Reclaimer.app".
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

DEPOTS="1064225 1064226" ./reclaimer-mac.sh download      # Halo: Reach files for Reach Renaissance (13 GB)
bash shim/reach.sh 20_sword_slayer slayer_054 10          # Reach Renaissance: a local Reach match, no menus (shim/docs/reach-renaissance.md)
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

- **Game profiles: one game's work can't change another's** (2026-10-21) — shim/src/games.c, shim/docs/games.md. Why: three fan clients
  (Reclaimer / halo3, Reach Renaissance / haloreach, Gravemind / halo2) share the loader; Reclaimer's exe signature patches and
  stats hooks were searched for in EVERY exe (a chance byte match would patch another game), halo3.dll's hooks were keyed on the
  DLL name only. How: `g_game` from the exe name (else the first engine DLL; `PELOAD_GAME=h3|reach|h2|other`), printed as
  `[game] ...`; Reclaimer-only: `reentrancy_fix` (g_sigpatches), `st_voice_install` (stats exe hooks), the halo3.dll hooks
  (h3 or an unplaced exe), the menu grading shader case; Halo 2 defaults: the real HLSL compiler (`PELOAD_REAL_D3DCOMPILE` on), one
  waveOut device (h3 / reach keep 0). Windows-behaviour fixes stay shared (games.md "The rule"). `shim/tools/game-matrix.sh` runs
  all installed games (h3 local-server join + chat hook, reach Sword Base frames, h2 offline Lockout frame) for a PASS / FAIL each.
- **Project Gravemind (Halo 2 on MCC's halo2.dll) groundwork, 2026-10-20, branch h2-gravemind.** Gravemind (h2gravemind.exe,
  a Zig/C build that loads MCC's halo2.dll like Reclaimer loads halo3.dll) needed the functions below. Each was a logging stub
  (return 0 = "success" with nothing written, or a NULL handle); the old behaviour is the stub unless a switch is named.
  - **PEB contents** — `peb_fill` (peload.c, before the entry point). Why: PEB->ProcessParameters was NULL; Gravemind's runtime
    read ProcessParameters->Flags at start (exe+0x42da54) and faulted. How: a real RTL_USER_PROCESS_PARAMETERS (std handles 1/2/3 =
    GetStdHandle's, current directory, image path, command line, environment snapshot) + PEB ImageBaseAddress, processor count,
    OS version 10.0.19045. Old: `PELOAD_PEB_EMPTY=1`.
  - **Guest DLL dependencies first** — `load_guest`. Why: halo2.dll delay-imports mss64.dll (Miles) from its own folder; the
    imports were bound while mss64 wasn't loaded yet, so every AIL_* call was a stub. How: before binding, every normal and delay
    import that is a file in the DLL's folder is loaded (recursively), as Windows does. Old: `PELOAD_NO_DEPLOAD=1`.
  - **Ordinal-only imports** — `ord_name` in uni_resolve: d2d1 #1, oleaut32 #2/#4/#6/#7 (BSTR), xinput1_4 #2-#5 get their names.
  - **Resources** — FindResourceW / FindResourceExW / LoadResource / LockResource / SizeofResource / FreeResource over the PE
    resource tree (win32x.c `rsrc_find`; Gravemind keeps fonts / pictures in RCDATA).
  - **Known folders** — SHGetKnownFolderPath (Documents, AppData, Saved Games ... on USERPROFILE / LOCALAPPDATA / APPDATA, CoTaskMem
    string, KF_FLAG_CREATE; old `PELOAD_KNOWNFOLDER_STUB=1`) and SHGetFolderPathW/A on top of it (CSIDL ids).
  - **CommandLineToArgvW** (Windows' quoting / backslash rules), **GetPrivateProfile\*** / WritePrivateProfileStringW (INI files,
    encoding kept), **CreateFile2**, **GetStringTypeW**, **K32GetModuleInformation** (Gravemind finds halo2.dll's base this way:
    with the stub it read RVA 0x6a8f30 as an address), GetFileSize, GetFullPathNameA, GetEnvironmentVariableA,
    EnumDisplaySettingsA (DEVMODEA layout), LoadIconA / LoadCursorA, GetDesktopWindow (+ its GetWindowRect = the monitor),
    LocaleNameToLCID, HeapDestroy, AppPolicyGetThreadInitializationType, SysAllocString & co., FlsGetValue2 (= FlsGetValue),
    AdjustWindowRect, D3D11CreateDeviceAndSwapChain (= CreateDevice + CreateSwapChain), GetConsoleWindow / AttachConsole,
    CoInitialize, SetThreadAffinityMask, shlwapi PathAppendW, CreateSemaphoreExW. Old for win32x.c's: `PELOAD_WIN32X_STUB=1`.
  - **Registry, ANSI side** — RegOpenKeyExA / RegQueryValueExA / RegGetValueA through the W versions; RegEnumKey(Ex)A/W and
    RegEnumValueA/W answer ERROR_NO_MORE_ITEMS. Why: the stubs said ERROR_SUCCESS forever and MCC's D3DCompiler_47.dll enumerated
    keys in an endless loop on halo2's engine thread.
  - **msvcrt legacy names** for MCC's real D3DCompiler_47.dll (a guest DLL; 14 of its 115 msvcrt imports aren't in the guest UCRT):
    _vsnprintf / _vsnwprintf / sprintf_s / swprintf_s / _snwprintf_s / sscanf_s through the guest ucrtbase's __stdio_common_*
    (legacy option bits; variadic ones via __builtin_ms_va_list), _lock / _unlock (recursive mutexes), __dllonexit / _onexit,
    _XcptFilter, _amsg_exit, std::terminate, type_info's destructor.
  - **Clean failures instead of S_OK with nothing** — MFTEnumEx, MFCreateWaveFormatExFromMFMediaType,
    MFInitMediaTypeFromWaveFormatEx, GetAdaptersAddresses (ERROR_NO_DATA: its LAN game list stays empty), waveIn (no devices).
- **Media Foundation and loopback capture fail cleanly** (client 0.9.22) — `i_MFStartup`, `i_MFCreateSinkWriterFromURL`,
  `i_MFCreateMediaType` / `Attributes` / `Sample` / `MemoryBuffer`, `i_ActivateAudioInterfaceAsync`,
  `i_CoCreateFreeThreadedMarshaler`. Why: 0.9.22's Theater EXPORT MP4 / RECORD VIDEO import them (an import-table diff
  0.9.20 -> 0.9.22 found these 9 plus CreateEventW and atan2, both already resolved); as logging stubs they returned 0 =
  S_OK with no object behind it, a call through a null pointer as soon as an export started. How: E_NOTIMPL with the
  out-pointer cleared (MFShutdown S_OK); MFStartup fails first. Film export is not available under the loader (README
  "Known gaps"; the stats page's Record row records clips). No old-behaviour switch (the stubs could only crash). (10-20)

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
  How: delay-import tables are bound up front like normal imports. (10-11 2a8f65e) Except halo3's bink2w64.dll since
  2026-10-24 (next entry).
- **halo3's Bink delay imports left to its own delay loader: mods' movies** (2026-10-24) — `bind_delay_imports` (now given the
  module's name), `PELOAD_BINK_SELFTEST`. Why: every launch logged "Mods' movies are unavailable: Bink import slots differ from
  the supported build; refused" (client 0.9.20 .. 0.9.23, client and dedicated server). The client checks that halo3's 12
  bink2w64.dll delay slots (halo3+0xa39e38 .. +0xa39e90) still hold their own delay thunks (the table at exe+0x2296938 in
  0.9.23, slot -> expected thunk, e.g. BinkClose halo3+0xa39e88 -> halo3+0x620c2a) -- the state of an untouched DLL on
  Windows -- before it wraps BinkClose (exe+0x71c850) and patches halo3's movie code (15 bytes at halo3+0x18cf04) to play a
  mod's `bink/*.bik`; a headless server checks them before pointing every slot at `xor eax,eax; ret` ("Movies are skipped:
  Bink is never loaded"). Our up-front binding had replaced the thunks, and since the client loads MCC's bink2w64.dll only
  later (LoadLibraryExW of MCC\Binaries\Win64\bink2w64.dll), with logging stubs that return 0. How: for halo3.dll (Halo 3
  profile) the bink2w64.dll delay descriptor is skipped: the thunks call halo3's own __delayLoadHelper2 -> our LoadLibraryExA
  (which returns the already loaded guest bink2w64.dll by name) + GetProcAddress (its export). Checked (0.9.23): no refusal;
  `PELOAD_BINK_SELFTEST=1` calls BinkOpen on a missing file through halo3's slot on the engine thread -> NULL, and the slot then
  points into bink2w64.dll (PASS); the BinkClose slot holds the client's wrapper. No mod movie was available to watch one play
  (no .bik on disk; MCC's own movies aren't in the depots we have). The same check shape exists for XAudio2 (its four slots
  hold halo3+0x60c416 .. thunks on disk), which is why the `xaudio` signature patches exist. Old: `PELOAD_BINK_EAGER=1`.
- **halo3's XAudio2 delay imports left lazy too: the xaudio patches retired** (2026-10-24) — `bind_delay_imports`, `sig_enabled`.
  Why: the client's XAudio2 check (exe+0x600600 in 0.9.23) is the Bink check's twin -- halo3's four XAudio2_9Redist.dll delay
  slots (halo3+0xa39e10..+0xa39e28, ordinals #5 #2 #6 #1) must still hold their delay thunks (halo3+0x60c416 / 0x60c422 /
  0x60c40a / 0x60c385); the loader bound them up front, the client refused, and four signature patches NOP'd its mismatch
  branches -- four points to port in every release (they moved in 0.9.20). How: the slots stay lazy; the check passes, the
  client installs its wrappers as on Windows; a thunk that runs before that resolves through halo3's delay helper ->
  LoadLibraryExA("XAudio2_9Redist.dll") -> XA_HANDLE -> GetProcAddress(ordinal) -> xa_resolve (our CoreAudio XAudio2). The
  xaudio patches apply only with `PELOAD_XA_EAGER=1` (or named in PELOAD_RECLAIMER_PATCHES); patchscan / port-verify report
  them as optional. Checked (0.9.23, A/B in the same local match, audio mixed but muted, PELOAD_AUDIO_MUTE + AUDIO_DUMP): old
  (eager + patches) vs new (lazy, no xaudio patches): the client's audio setup lines identical ("voice settings wrapped ...",
  "repeated voice settings are skipped"), no refusal, 53 s of 81 non-silent in both, RMS 0.0194 vs 0.0192, no NaN; the
  headless dedicated server takes its "audio off" path. Old: `PELOAD_XA_EAGER=1`.
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
- **EnumDisplaySettingsW wrote DEVMODEA offsets** — `i_EnumDisplaySettingsW`. Why (backlog 69): the client's Settings >
  Graphics said "Your display: 0 x 0". All 5 client callers take ENUM_CURRENT_SETTINGS of MonitorFromWindow's device and read
  dmPelsWidth/Height at +0xAC/+0xB0; the shim wrote width/height at +108/+112 (the ANSI struct's offsets -- the wide one has
  two 32-WCHAR names), and it answered TRUE for every mode number (an enumeration loop would never end). The DXGI output
  guess in the backlog was wrong: nothing calls EnumOutputs. How: the DEVMODEW fields (dmFields POSITION | BITSPERPEL |
  PELSWIDTH | PELSHEIGHT | DISPLAYFLAGS | DISPLAYFREQUENCY; bpp @168, width @172, height @176, frequency @184) of the same
  1920x1080 60 Hz monitor GetMonitorInfoW / EnumDisplayMonitors describe (MON_W / MON_H, one definition now); mode 0,
  ENUM_CURRENT and ENUM_REGISTRY only. Checked: the page reads "Your display: 1920 x 1080"; a Borderless-fullscreen profile
  still renders at 1920x1080 (scene and back buffer). `PELOAD_DEVMODE_OLD=1` = the old fill. (10-18)
- **msvcrt.dll for windows-gnu programs** — `mv_resolve`, `uni_resolve`. Why: Reach Renaissance's five programs are Rust built
  for x86_64-pc-windows-gnu, whose C runtime is msvcrt.dll (34 imports), not ucrtbase / vcruntime140; those imports bound by
  name to native functions or logging stubs, so mingw's startup (`__getmainargs`, `__set_app_type`, `_initterm` ...) could
  not run. How: msvcrt imports bind to the genuine ucrtbase / vcruntime140 exports of the same name (24 of 34); the 10
  msvcrt-only names are built on ucrtbase: `__getmainargs` (`_configure_narrow_argv` + `__p___argc` / `__p___argv` /
  `__p__environ`), `__set_app_type` (`_set_app_type`), `_amsg_exit`, `_onexit` (`_crt_atexit`), the data imports `__initenv` /
  `_commode` / `_fmode`, and the FILE* calls (`__iob_func` hands out a 3-entry _iobuf array; `fwrite` / `fprintf` / `vfprintf`
  translate it to ucrtbase's streams via `__acrt_iob_func` / `__stdio_common_vfprintf`). kernel32's `__C_specific_handler`
  (windows-gnu imports it there; Windows forwards it to ntdll) binds to vcruntime140's. Checked: rr-master.exe and
  host-probe.exe run to their own argument / build checks. `PELOAD_NO_MSVCRT=1` = old. (10-20, reach-port)
- **LoadLibraryW, SetDllDirectoryW** — `i_LoadLibraryW`, `i_SetDllDirectoryW`, `i_LoadLibraryExW`. Why: Reach Renaissance's
  host-probe loads the engine with SetDllDirectoryW(<MCC>\haloreach) + LoadLibraryW("haloreach.dll"); both were logging stubs
  (MSVC builds only ever imported LoadLibraryExW). How: LoadLibraryW = LoadLibraryExW(name, 0, 0); SetDllDirectoryW remembers the
  folder, and a bare DLL name that is no file in the current folder is loaded from there. (10-20, reach-port)

## 2. Processes, threads, exceptions

- **CREATE_SUSPENDED / ResumeThread** (2026-10-20) — `i_CreateThread`, `thread_tramp`, win32x.c `i_ResumeThread`. Why: the flag was
  ignored (the thread ran at once) and ResumeThread was a stub; a creator that fills in a thread's data before resuming it raced
  it. How: the thread object's count is the suspend count; the trampoline waits until it is 0; ResumeThread returns the previous
  count. SuspendThread of a running thread fails (-1). GetThreadPriority 0, GetLogicalProcessorInformation (one core entry per
  logical CPU + a package). Old: `PELOAD_IGNORE_SUSPENDED=1`.
- **SIGUSR1 dump names every guest DLL** (2026-10-20) — `dump_attr`. It named everything at 0x180000000 "halo3" (halo2.dll and
  haloreach.dll load there too) and left other DLLs unnamed; now exe / <dll>+0x<off> from each module's SizeOfImage.
- **SIGUSR1 dump survives unreadable stacks** (2026-10-24) — `dump_all_guest_stacks`, `crashsig`. Why: `kill -USR1` on a
  0.9.23 worker killed it: the stack scan read past a thread's stack into unmapped memory (`hook_safe` only checks the address
  range). How: the scan stops at the thread's own stack top (`pthread_get_stackaddr_np`), and each thread's scan runs under a
  `sigsetjmp`: a SIGSEGV / SIGBUS during it jumps back ("stack scan stopped: unreadable memory") instead of crashing the game.
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
- **Guest thread stacks of the requested size** — `i_CreateThread`, `g_stack_reserve`. Why: every guest thread got macOS's
  512 KB default whatever it asked for (Windows: the requested dwStackSize, at least the exe's SizeOfStackReserve -- Reclaimer
  1 MB, Reach Renaissance 2 MB; Rust asks 2 MB for each std thread). An overflow wasn't caught the Windows way either: MSVC's
  `__chkstk` probes pages only below TEB.StackLimit (gs:0x10), which is a macOS TSD slot under the loader, so a big frame
  stepped over pthread's one guard page into the memory below. In Reach Renaissance's host-probe that was the loader's
  shim-sweep thread: it aborted (libsystem_c `__abort`) ~20 s into every match with haloreach frames on its stack. How:
  max(requested, exe reserve, 1 MB) rounded to 64 KB, guard 256 KB. Checked: Sword Base Slayer runs its full 60 / 150 s.
  `PELOAD_THREAD_STACK=<bytes>` raises the minimum; old: `PELOAD_THREAD_STACK_OLD=1`. (10-20, reach-port)
- **ExitThread, FreeLibraryAndExitThread** — `i_ExitThread`, `thread_tramp`. Why: neither existed; haloreach.dll ends a worker
  with FreeLibraryAndExitThread when a session ends, the logging stub returned and the thread ran into the int3 padding after
  the noreturn call (SIGTRAP at exit). How: thread_tramp `_setjmp`s before the start function; ExitThread `_longjmp`s back
  with the code, so the thread finishes as if it had returned (handle signaled, TEB / TLS reaped later); nothing on the guest
  stack is unwound (as on Windows). A thread we did not start leaves through pthread_exit. (10-20, reach-port)
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
- **VirtualAlloc honours PAGE_EXECUTE_\*** — `i_VirtualAlloc`. Why: every allocation was mapped read/write only; Reach
  Renaissance's host-probe allocates its jump stubs right after haloreach.dll with PAGE_EXECUTE_READWRITE and runs them, and
  Rosetta refuses to execute a page without PROT_EXEC (SIGBUS at the stub's first byte, haloreach.dll+0x4ee0350). halo3 /
  the Reclaimer client only ever made code executable with VirtualProtect, which already did. How: protections 0x10 / 0x20 /
  0x40 / 0x80 add PROT_EXEC (pages stay readable + writable as before). Old: `PELOAD_VA_NOEXEC=1`. (10-20, reach-port)
- **HEAP_GUARD realloc over-read** (diagnostic allocator `PELOAD_HEAP_GUARD`) — copied the new size instead of
  min(old, new). (10-11 3da9e9c)

## 4. Files, pipes, console

- **FindFirstFileA translates Windows paths** (2026-10-21) — `i_FindFirstFileA`. Why: Project Gravemind had no game sound at all.
  FindFirstFileA only turned backslashes into slashes, so an absolute `C:\...\halo2\*.flt` searched a folder named "C:" and found
  nothing. MCC's Miles Sound System finds its DSP plugin (halo2\mss64dsp.flt) this way: no plugin, so its filters never
  registered, halo2.dll's voice setup (`halo2+0x779000`: AIL_allocate_sample_handle, then AIL_find_filter for "Parametric EQ",
  "BandPass", "Gain Block" and "LowPass") failed, and halo2 never started a sound -- Miles opened waveOut, prepared its buffers and
  wrote nothing. How: paths with a drive letter or `\\` go through win2unix like FindFirstFileW's (relative ones -- halo3's `maps\*`
  -- stay byte for byte, non-ASCII kept). Found with PELOAD_BP in mss64 (section 20). Old: `PELOAD_FINDA_RAW=1`.
- **win.ini answers its defaults** (2026-10-21) — win32x.c `i_GetProfileStringA/W`, `i_GetProfileIntA/W`. Why: the stub returned 0
  and left the buffer untouched (Miles reads `[MSS] Reset` and compared stack garbage). How: no win.ini, so the default.

- **FindFirstFileW's find data** (2026-10-20) — `i_FindFirstFileW`. Why: FindFirstFileW was mapped to FindFirstFileExW itself, whose
  WIN32_FIND_DATAW is the THIRD argument; a FindFirstFileW caller passes it second, so 592 bytes went wherever r8 pointed (halo2.dll:
  a memset to 0xfffffffe70b0c4ba). Rust's read_dir uses FindFirstFileExW, so Reclaimer never hit it. How: a two-argument wrapper.
- **File APIs that defeated the client's own checks** (security review 2026-10-19) — peload.c `i_RemoveDirectoryW`,
  `i_DeleteFileW`, `i_GetDiskFreeSpaceExW`, `i_SHFileOperationW` (new). Why: RemoveDirectoryW deleted a NON-empty folder's
  whole tree and always succeeded (a 10-11 bring-up workaround from when folder listing was broken); DeleteFileW always
  succeeded; GetDiskFreeSpaceExW said 200 GB free everywhere (the client's "Not enough disk space" check before a mod
  download never fired); SHFileOperationW was an unknown-import stub returning success, so deleting a mod in Settings > Mods
  (FO_DELETE + FOF_ALLOWUNDO: to the Recycle Bin) said done and deleted nothing. How: rmdir only, ERROR_DIR_NOT_EMPTY (145) /
  2 / 267 / 5 as Windows ("[RemoveDirectoryW] not empty, kept" logged); unlink errors as 2 / 3 / 5; statfs of the folder's
  volume (or its nearest existing parent); FO_DELETE moves each path to the macOS Trash (NSFileManager trashItemAtURL, never
  a permanent delete), other operations error 120. Checked: clients and the dedicated server on the new build join and
  play with no refused delete. Old: `PELOAD_RMDIR_RECURSIVE=1`, `PELOAD_DELETE_LAX=1`, `PELOAD_DISKFREE_FAKE=1`,
  `PELOAD_SHFILEOP_STUB=1`. Still open (section 22): symlinks are never reported as reparse points, GetFinalPathNameByHandleW
  returns the unresolved path, `..` / other drive letters are not confined to the prefix. (10-19)
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

- **Recursive mutexes** (2026-10-20) — `sync_wait`, `i_CreateMutexW`, `i_ReleaseMutex`. Why: a Windows mutex may be taken again by its
  owner; the loader's was a plain lock, and MCC's Miles Sound System (halo2's mss64.dll) takes its mutex in AIL_open_digital_driver
  and again in AIL_set_preference -- halo2's engine thread blocked on itself. How: depth in the object's maxc, ReleaseMutex frees
  it at depth 0 and only for the owner (ERROR_NOT_OWNER otherwise). Old: `PELOAD_MUTEX_OLD=1`.
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

- **WinHTTP on libcurl** (2026-10-20) — shim/src/winhttp.c (build.sh links -lcurl, the system library). Why: every WinHttp* was a stub;
  Project Gravemind's beta check (raw.githubusercontent.com/BriRoz1/h2gravemind-beta/main/beta.txt) failed and it refused to start
  ("beta: status file not reachable; not allowed to start"). How: synchronous WinHTTP -- Open / SetTimeouts / Connect /
  OpenRequest / AddRequestHeaders / SendRequest keep the request, ReceiveResponse runs curl_easy_perform (redirects followed),
  QueryHeaders (status, standard and custom headers, numbers), QueryDataAvailable / ReadData, CrackUrl. Checked: the status file
  arrives (200, "status=open"). Old: `PELOAD_WINHTTP_STUB=1`; trace `PELOAD_TRACE_WINHTTP=1`.
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
- **TLS: Schannel and certificate checks on macOS's LibreSSL** — schannel.c (`sch_*`). Why: Reach Renaissance's programs
  speak HTTPS to their master server (server list, join leases, rendezvous) through Rust's native-tls -> schannel crate, i.e.
  secur32's SSPI (AcquireCredentialsHandleA, InitializeSecurityContextW, QueryContextAttributesW, EncryptMessage /
  DecryptMessage, ApplyControlToken, DeleteSecurityContext, FreeContextBuffer) and crypt32's CertGetCertificateChain +
  CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL); all were logging stubs, every connection failed ("0 of 2 masters
  responded"). How: LibreSSL 3.3 from /usr/lib (libssl.48 / libcrypto.46, in every macOS's shared cache, TLS 1.3) on memory
  BIOs, which is SSPI's model: records go in one at a time, so a partial record is SEC_E_INCOMPLETE_MESSAGE (MISSING) and the
  bytes after the handshake's last record or the decrypted record are SECBUFFER_EXTRA; EncryptMessage lays one record over
  header | data | trailer (StreamSizes 5 / 64 / 16384; AEAD suites only); the first ISC call's EMPTY output buffer becomes
  the TOKEN (the crate relies on that); SNI + ALPN from the request; DecryptMessage hands back HEADER | DATA | TRAILER | EXTRA
  in place, close_notify = SEC_I_CONTEXT_EXPIRED. The certificate is checked by LibreSSL (/etc/ssl/cert.pem, or
  `PELOAD_TLS_CAFILE`; the SNI name as the expected host) and that verdict is what the chain / policy calls report
  (CERT_E_UNTRUSTEDROOT / _CN_NO_MATCH / _EXPIRED), so the crate's own check accepts or refuses as on Windows. Also
  CertDuplicate/FreeCertificateContext, CertDuplicate/FreeCertificateChain, CertOpen/Close/DuplicateStore (native-tls clones
  its empty extra-roots store), CertAddCertificateContextToStore, CertEnumCertificatesInStore. Checked: `rr-browser list` and
  `serve` reach the public master over TLS 1.3 with the certificate verified; leases and joins work. `PELOAD_TLS_TRACE=1` logs
  handshakes; old: `PELOAD_NO_SCHANNEL=1`. (10-20, reach-port)
- **(diagnostic) UDP trace shows each datagram's first bytes; `PELOAD_UDP_BLOCK`** — `udp_tracef`, `udp_blocked`
  (`i_ws2_sendto` / `i_ws2_recvfrom`). Why: a Reach Renaissance join through the master's relay stalled at JOINING 9%. Telling
  the loader apart from the host needed a relayed join on demand and readable traces. How: `PELOAD_TRACE_UDP` lines end with
  the first 16 bytes as text (`|RR-RDV1.{"t":"he|`, `|RR-RLY1...|`). `PELOAD_UDP_BLOCK=<port>[,<port>..]` drops datagrams to
  and from those remote ports (a send still reports success; traced as TX-BLOCKED / RX-BLOCKED), as if the NATs between two
  peers never opened. Result: the relay works under the loader, and the failed public join was a host that answered neither
  path (reach-renaissance.md "Joining"). Off unless set; nothing else changes. (10-20, reach-port)

## 7. Windows, input, focus (Cocoa, Raw Input, XInput)

- **F1 releases the mouse in every game but Reclaimer** (2026-10-21) — wininput.c `wi_translate` (F1), `wi_raw_mouse`. Why: F1 only
  worked for re-centring games (Reach Renaissance); Project Gravemind holds the mouse with ClipCursor and reads Raw Input, so F1
  went to the game and the pointer could not be got back. How: F1 applies whenever the pointer is captured and the game is not h3
  (Reclaimer has its own F1, games.c); while released, Raw Input mouse motion is not sent (the game would keep turning). A click in
  the window captures again. Old: `PELOAD_NO_F1_RELEASE=1` (no F1 release at all).
- **PELOAD_FG_TEST_ACTIVATE=<secs>** (2026-10-21, tests) — cocoa_pump: an input-less test run gets the user's activation
  (fg_user_activate) <secs> after start, without focus -- so a test client behaves like a played one (Gravemind's sound resume and
  pad poll both require its window in front).

- **A click (or switching to the window) takes the foreground back from a hidden window** (2026-10-21) — `fg_user_activate`
  (peload.c; called from wininput.c's mouse-down and cocoa_pump's key-window check), WWin `vis` (CreateWindowExW WS_VISIBLE,
  ShowWindow, SetWindowPos SWP_SHOW/HIDEWINDOW), `g_user_hwnd` (the last activatable top-level window shown). Why: Project
  Gravemind had no keyboard, mouse or pad at all, clicking included -- halo2.dll creates a hidden 1920x1080 window and calls
  SetForegroundWindow on it, the loader kept that as the foreground window for good, and Gravemind reads input only while
  GetForegroundWindow() is its main window (exe+0x9b350 against the window it registers for raw input at exe+0x9cb20; the pad poll
  exe+0x6d620 likewise; seen with PELOAD_TRACE_FG). On Windows the user's click activates the window clicked. How: a click in the
  Mac window's content, or the Mac window becoming the key window, makes the last shown activatable window the foreground window
  when the current one is hidden; a shown foreground window is never replaced (Reclaimer and Reach show their main window and
  never have a hidden one in front). Logged as `[fg] ... is hidden -> ... takes it`. Old: `PELOAD_FG_NO_USERACT=1`.

- **PELOAD_TRACE_FG** (2026-10-21, diagnostic) — `i_GetForegroundWindow` logs each code site that asks (module+RVA, once) and the
  window it got, beside the last shown / last created windows. Why: Project Gravemind reads its pads only while
  GetForegroundWindow() is its own window (exe+0x6d620, then XInputGetState at exe+0x6d63f), and no XInputGetState call showed up in
  its runs; shim/gravemind.sh turns it on with PELOAD_TRACE_XINPUT and PELOAD_TRACE_D2D (all in /tmp/gravemind_play.log).

- **Clipboard on the Mac pasteboard** — `i_OpenClipboard` / `i_EmptyClipboard` / `i_SetClipboardData` / `i_CloseClipboard`
  / `i_GetClipboardData` / `i_IsClipboardFormatAvailable` (+ Count / Enum / sequence number / RegisterClipboardFormatW) and
  `i_GlobalAlloc` / `Lock` / `Unlock` / `Size` / `Free`. Why: Settings > Online services > Player ID > COPY (and every
  other copy / paste in the client's menus) failed: "Could not copy to clipboard: OSError(203)", the menu's "The clipboard
  is unavailable"; OpenClipboard was a logging stub returning 0 and no Global* function existed. How: the general
  NSPasteboard as plain text. Text set between EmptyClipboard and CloseClipboard (CF_UNICODETEXT preferred, CF_TEXT /
  CF_OEMTEXT as Windows-1252) is written at CloseClipboard with CRLF -> LF; GetClipboardData of a text format reads the
  pasteboard into a fresh block (UTF-16 / UTF-8) that the "system" frees at the next read or EmptyClipboard; while the
  clipboard is open and changed, the pending set answers the format queries; other formats stay in-process until the
  next EmptyClipboard; the opening thread owns it (another thread gets ERROR_ACCESS_DENIED, a non-owner
  ERROR_CLIPBOARD_NOT_OPEN). Global* blocks: 16-byte header {magic, lock count, size} before the data, the handle = the
  data pointer. halo3.dll imports the same functions. Old: `PELOAD_NO_CLIPBOARD=1`. `PELOAD_PASTEBOARD=<name>` = a private
  named pasteboard (quiet-run.sh, target.sh and the offline test shim/tools/clipboard-test.sh use one, so test clients
  never overwrite your clipboard); `PELOAD_TRACE_CLIPBOARD=1` logs the calls. Checked in game (0.9.22): COPY shows
  "Player ID copied" and the 64-character ID is on the pasteboard. (10-20)

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
- **Game window from a PeekMessageW / GetMessageW pump** — `present_window_from_pump`. Why: under `PELOAD_PRESENT` the macOS
  window (and the present thread) was only made in MsgWaitForMultipleObjectsEx, the Reclaimer worker's wait; Reach
  Renaissance's host-probe pumps its window with PeekMessageW, so its frames rendered offscreen and nothing showed. How: the
  first PeekMessageW / GetMessageW on the main thread makes the window too (idempotent). (10-20, reach-port)
- **Mouse look by re-centring the cursor** — wininput.c `wi_SetCursorPos`, `wi_cursor_captured_move`, `wi_raw_mouse`. Why:
  Reach Renaissance's host-probe looks around with GetCursorPos (distance from the centre) + SetCursorPos (back to the
  centre) every frame while ClipCursor holds the pointer; the captured pointer never moved here (motion went to Raw Input
  only), so the look delta was always 0. How: once a guest calls SetCursorPos while the pointer is captured, captured motion
  moves the virtual cursor too, clamped to the ClipCursor rectangle; a guest that never does (the Reclaimer client, Raw
  Input) is unchanged. Checked with `PELOAD_KBM_TEST=1 PELOAD_KBM_TEST_DELAY=16`: the view turns with the sweep and W walks.
  Old: `PELOAD_CURSOR_FROZEN=1`. (10-20, reach-port)
- **CallWindowProcW** — `i_CallWindowProcW`. Why: a subclassed window procedure (host-probe hooks its game window for the UI
  cursor) forwards what it doesn't handle to the previous one; the logging stub returned 0, so every forwarded message was
  swallowed. How: calls the previous procedure (a guest function or our DefWindowProcW, both ms_abi). (10-20, reach-port)
- **F1 releases the pointer from a game without a release key** — wininput.c `g_wi_user_released`, `wi_sys_hold`. Why: in a
  Reach Renaissance match the pointer could not be freed (the user reported F1 did nothing): F1 was only ever the Reclaimer
  client's own key, and host-probe holds the mouse with ClipCursor, re-clipping it every frame. How: for a re-centring guest
  (`g_wi_recenter`; the Reclaimer client keeps its own F1) F1 gives the Mac its pointer back -- shown, moving freely; the
  guest still thinks it holds it and gets no look motion -- until a click inside the window captures it again (that click
  does not reach the game); the guest letting go itself clears it. Old: `PELOAD_NO_F1_RELEASE=1`. (10-20, reach-port)
- **A message queue for GetMessageW loops** — `i_GetMessageW`, `i_PeekMessageW`, `i_SetTimer`, `i_PostMessageW` .... Why: Reach
  Renaissance's menu-overlay sits in GetMessageW (its window, WebView2 callbacks and its worker threads' results arrive as
  messages); GetMessageW returned 0 (= WM_QUIT) whenever nothing was queued, SetTimer never fired, PostMessageW /
  PostThreadMessageW / PostQuitMessage / SendMessageW were logging stubs. How: posted messages share the input queue (any
  thread); due timers come out of PeekMessageW as WM_TIMER and DispatchMessageW calls a TIMERPROC; SendMessageW calls the window
  procedure; GetMessageW waits (the main thread keeps pumping macOS events) and returns 0 for WM_QUIT. Old: `PELOAD_MSGQ_OLD=1`.
  (10-20, reach-port)
- **WebView2 on WKWebView** — webview2.m (`wv2_*`), peload.c `WV2_HANDLE`. Why: Reach Renaissance's menus are HTML / JS pages in
  Microsoft Edge WebView2; there is no Edge runtime here (the genuine WebView2Loader.dll returned 0x80070002 and the menus quit).
  How: LoadLibrary("WebView2Loader.dll") returns a pseudo-module whose CreateCoreWebView2EnvironmentWithOptions builds our
  environment -> controller -> ICoreWebView2 (+ settings, event args) on a WKWebView in a borderless, transparent, floating
  NSWindow that stands for the guest's window (its rect from SetWindowPos / MoveWindow, shown / hidden with ShowWindow,
  click-through with WS_EX_TRANSPARENT). Navigate (a `file://///%3F/C:/...` URL becomes the prefix path, read access to all of
  C:), ExecuteScript, AddScriptToExecuteOnDocumentCreated, PostWebMessageAsString / Json, add_WebMessageReceived,
  NavigationStarting / Completed / ContentLoading / DOMContentLoaded; the page's `window.chrome.webview` (postMessage,
  addEventListener("message") ...) is a document-start script on WKScriptMessageHandler. The page gets AppKit's events directly
  (keys, mouse, Gamepad API). Shared buffers are refused (QueryInterface for Environment12 / ICoreWebView2_17): the menus fall
  back. Unimplemented methods log "[wv2] <interface> slot N". `PELOAD_WV2_TRACE=1` logs calls, `PELOAD_WV2_SNAPSHOT=<s>` saves
  the page as PNGs, `PELOAD_WV2_INSPECT=1` enables Safari's inspector. Old: `PELOAD_NO_WV2=1`. (10-20, reach-port) Follow-up (the
  user: the menus stayed on top of every other app): the window floats only while one of our processes is frontmost (the menus,
  or a game whose window is published, xwin.c) and is put away while another app is; showing it never takes activation from
  another app, and SetForegroundWindow on it activates the menus only from our own processes.
- **Windows of other loader processes** — xwin.c. Why: menu-overlay follows the game's window (another process: --follow "host
  probe") by title and keeps itself over its client area; every loader process only knew its own windows. How: a process with a
  game window publishes title, client rect (Win32 screen coordinates), shown and minimised to ~/Library/Application Support/
  ReclaimerMac/windows/<pid>.win when they change; other processes see it as a pseudo-handle (0x7F000000 | pid) in EnumWindows,
  FindWindowW, GetWindowTextW, GetWindowRect / GetClientRect / ClientToScreen, IsWindow / IsWindowVisible / IsIconic,
  GetWindowThreadProcessId and SetForegroundWindow (activates that app); SetWinEventHook callbacks fire for its location
  changes, show / hide, minimise and destroy (polled every 50 ms). Windows keep their titles (GetWindowTextW, SetWindowTextW).
  `PELOAD_TRACE_XWIN=1` logs hooks and events; old: `PELOAD_NO_XWIN=1`. (10-20, reach-port)
- **WM_MOVE / WM_SIZE for WebView2 windows; their real rect** — `win_sized`, `i_GetWindowRect`, `i_GetClientRect`. Why: the menus
  moved and resized their window over the game but the page stayed 640x360 in its corner (the user's screenshot): menu-overlay
  sizes its WebView2 controller on WM_SIZE, which SetWindowPos never sent; GetWindowRect / GetClientRect also reported the fixed
  1280x720. How: SetWindowPos / MoveWindow track the window's rect and send WM_MOVE / WM_SIZE when it changed; GetWindowRect /
  GetClientRect report it -- both only for windows hosting a WebView2 (the Reclaimer client's windows are unchanged); GWL_STYLE /
  GWL_EXSTYLE are stored. Old: `PELOAD_NO_WMSIZE=1`. (10-20, reach-port)

## 8. GDI and layered windows

- **Direct2D 1.1, DirectWrite and WIC** (2026-10-21) — shim/src/d2d.c (+ wic.c, dwrite.c, which it includes; d2d_slots.h = every
  interface's IID and slot names, generated by shim/tools/com_slots.py from Wine's IDL). Why: Project Gravemind's launcher and
  in-game screens are all Direct2D on the D3D11 back buffer -- ID2D1Factory1 -> CreateDevice(IDXGIDevice) -> CreateDeviceContext ->
  CreateBitmapFromDxgiSurface(back buffer) + SetTarget, then BeginDraw / shapes / DrawTextLayout / DrawBitmap / EndDraw per frame;
  DirectWrite 5 with its fonts (Michroma, Halo 2 Handel, Halo 2 Conduit) in RCDATA, loaded through IDWriteInMemoryFontFileLoader ->
  IDWriteFontSetBuilder1 -> CreateFontCollectionFromFontSet; its pictures through WIC. D2D1CreateFactory failed (E_NOTIMPL), it
  logged "ui: Direct2D init failed 0x80004001" and crashed. How: COM objects (a class = IIDs + implemented slots; every other slot
  logs `[d2d] <interface>::<method> (slot N) not implemented` once and answers E_NOTIMPL). A device context draws onto a CPU canvas
  the size of its target (CGBitmapContext, premultiplied BGRA, y down): shapes, geometries (paths with lines / beziers / quadratic
  beziers / SVG-style arcs, groups, transformed), strokes (caps, joins, dash styles), solid / linear / radial / bitmap brushes,
  clips (axis-aligned; rotated ones clip on the CPU), layers (content bounds, geometric mask, opacity), text. The canvas goes onto
  the target texture -- premultiplied source-over, only the box drawn into (`d2_flush`) -- at EndDraw / Flush and before the two
  GPU operations: Clear (a GL clear of the clip box) and DrawBitmap of a texture (a GL quad: a video frame never comes back to the
  CPU). A texture is read back (GL ReadPixels) only when a CPU path needs it. CPU target bitmaps (CreateBitmap + TARGET) are drawn
  directly. COM methods that return a struct (GetSize, GetPixelSize, GetPixelFormat, GetColor, the brush getters) take MSVC's
  hidden return pointer as their 2nd argument. DirectWrite: lines from a CTTypesetter (word / character wrapping, hard breaks),
  alignment, trimming with the ellipsis sign, line spacing, per-range font / size / weight / style / underline / strikethrough /
  drawing effect, GetMetrics / GetLineMetrics / GetClusterMetrics / DetermineMinWidth / the HitTests; fonts: in-memory faces by
  family name (nearest weight, a slant when there is no italic face), the system collection = files staged in C:\ReclaimerFonts
  matched by their own family names (genuine bahnschrift.ttf / segoeui.ttf; a variable font's weight axis follows the weight) +
  macOS fonts + mapped substitutes (Bahnschrift -> Avenir Next Condensed, Segoe UI -> Helvetica Neue, Consolas -> Menlo).
  WIC: ImageIO decoders (whole first frame), format converters (32bpp BGRA / PBGRA / BGR / RGBA / PRGBA), scaler, bitmaps + locks,
  memory / file streams; ON FOR h2 ONLY -- halo3.dll references CLSID_WICImagingFactory2 and has only ever been told "not
  registered" (`PELOAD_WIC=1` any game, `=0` off). Old: `PELOAD_NO_D2D=1` (the failing factories). `PELOAD_TRACE_D2D` logs
  creations, composites and operations per EndDraw; `PELOAD_DWRITE_FONT=<family>` forces one font. Offline test:
  `bash shim/tools/d2d-test.sh`. Speed (2026-10-21, Gravemind's launcher 15 -> 7 ms a frame): a CPU bitmap of 256x256 pixels or more
  is drawn on the GPU like a texture bitmap, from a GL copy refreshed only when its pixels changed (its 2560x1280 menu video was
  scaled by CoreGraphics every frame; `PELOAD_D2D_CPU_BITMAPS=1` = the canvas as before); the canvas survives SetTarget (Gravemind
  sets NULL and the back buffer every frame). Diagnostics: with PELOAD_TRACE_D2D the first EndDraws print the dirty box, clips,
  target and canvas; `PELOAD_D2D_DUMP=<n>` writes the canvas of the first frame with >= n operations to ~/d2d_canvas_<pid>.bmp;
  `PELOAD_TRACE_D2D_OPS=1` logs the first operations' bounds / transform / clip and SetTransform's callers. IDWriteTextLayout1 (Gravemind's in-game text asks for it): pair kerning, character spacing (as
  extra advance; the minimum advance width is not applied). Not done (logged when called): effects, command lists, glyph runs, custom text renderers
  (IDWriteTextLayout::Draw), HWND / DC / WIC render targets, compatible render targets, opacity masks, gradient meshes.

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
- **GetGlyphOutlineW** — `i_GetGlyphOutlineW`. Why: Reach Renaissance's host-probe draws the engine's UI text itself
  (CreateFontW + GetTextMetricsW + GetGlyphOutlineW per character into a glyph atlas); the logging stub returned 0, the atlas
  never got a view, and screens such as the loadout picker showed no text (the user's report). How: CoreText with the font
  GetTextMetricsW reports (`ct_font_for`): GGO_METRICS, GGO_BITMAP and GGO_GRAY2/4/8_BITMAP (DWORD rows; 4 / 16 / 64 levels),
  GGO_GLYPH_INDEX, surrogate pairs; GLYPHMETRICS = black box, origin (left, top above the baseline), advance; an empty glyph
  is 1x1 with no bitmap, as on Windows; MAT2 ignored (identity); outlines (GGO_NATIVE / BEZIER) = GDI_ERROR. Checked: the
  loadout picker shows "Select Loadout", the loadout names and the game type text. Old: `PELOAD_GLYPH_STUB=1`. (10-20, reach-port)

## 9. Audio (XAudio2, WASAPI)

- **DirectSound 8 on CoreAudio, for Halo 2** (2026-10-21) — shim/src/dsound.c (`DS_HANDLE`: LoadLibrary("dsound.dll") / GetProcAddress
  in peload.c). Why: Project Gravemind's sound ran ~0.2 s behind the picture. MCC's Miles Sound System uses DirectSound when it can
  load it and waveOut otherwise; on waveOut it keeps five 50 ms buffers, four always queued (the `[winmm] ... 4 queued` lines), so
  a sound was heard ~200 ms after it was mixed. How: DirectSoundCreate(8) / DirectSoundEnumerateA/W (no capture devices);
  IDirectSound8 (secondary + primary buffers, software-mixing caps, stereo speaker config) and IDirectSoundBuffer8 (a ring:
  Lock hands out pointers into it; Play / Stop / cursors / status / format / volume and pan as AudioQueue parameters). A playing
  buffer has an AudioQueue pulling 10 ms at a time from its ring at the play cursor (3 in flight); the write cursor is one step
  ahead. Miles now mixes into a looping 49152-byte buffer a short way ahead of the play cursor. PELOAD_AUDIO=0: a silent device
  moving the cursors by the clock. Per game: on for h2 only -- halo3.dll and haloreach.dll also look for DirectSound and have
  only ever been told there is none (`PELOAD_DSOUND=1` any game, `=0` off: Miles goes back to waveOut). `PELOAD_TRACE_DSOUND`:
  buffers created / started + a 2 s line per buffer (bytes played, peak). Offline test: `bash shim/tools/dsound-test.sh`.
- **waveOut diagnostics** (2026-10-21) — winmm.c: `PELOAD_TRACE_WINMM=1` logs every waveOut call (open, prepare, write, reset,
  close, WOM_DONE dispatch), `=stats` a line per stream every 2 s (writes, bytes, peak sample, headers played, queued; shim/gravemind.sh
  sets it), `PELOAD_WINMM_DUMP=<file>` appends the PCM written. Told "Miles writes nothing" apart from "CoreAudio plays nothing"
  (it was the former: section 4, FindFirstFileA).

- **waveOut on CoreAudio** (2026-10-20) — shim/src/winmm.c. Why: MCC's Miles Sound System for Halo 2 plays through winmm waveOut; every
  wave function was a stub. How: one AudioQueue per waveOut (PCM 8-32 bit, float, EXTENSIBLE), WAVEHDRs copied into queue buffers
  and returned with WHDR_DONE; callbacks NULL / EVENT / FUNCTION (WOM_DONE on a dispatcher thread made with our CreateThread);
  Reset / Pause / Restart / GetPosition / GetDevCaps / GetID. PELOAD_AUDIO=0 (test runs) or a queue that won't open = a silent
  device that returns headers at the real playback rate. waveOutGetNumDevs stays 0 (halo3.dll / haloreach.dll ask it first and so
  keep XAudio2 only); Miles opens WAVE_MAPPER directly. Old: `PELOAD_WINMM_STUB=1`; trace `PELOAD_TRACE_WINMM=1`.
- **No DirectSound** (2026-10-20) — `i_LoadLibraryExW`: dsound.dll is "not found" (126) instead of the placeholder module, whose
  GetProcAddress gave Miles a DirectSoundCreate stub (S_OK, no object -> crash at mss64+0x2a780); Miles then uses waveOut.
  Old: `PELOAD_DSOUND_PLACEHOLDER=1`.
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
- **LoadLibrary of XAudio2 returns our XAudio2** — `XA_HANDLE`, `i_LoadLibraryExW`, `i_GetProcAddress`. Why: haloreach.dll
  loads XAudio2 by name and takes its exports with GetProcAddress (halo3 only reached it through delay imports, which bind to
  our CoreAudio XAudio2). The genuine xaudio2_9redist.dll in MCC\Binaries\Win64 loaded as a guest and recursed into the
  engine until the stack ran out. How: a LoadLibrary of any `xaudio2_*` name returns a distinct handle, and GetProcAddress
  on it resolves through `xa_resolve` (the import binder's table). Old: `PELOAD_XA_REALDLL=1`. (10-20, reach-port) Follow-up:
  a name `xa_resolve` doesn't know (X3DAudio with `PELOAD_AUDIO=0`) gets a logging stub; it fell through to export_lookup on the
  handle, which is no image (a fault reading 0x773303c in a windowless test host).

## 10. D3D11 / DXGI objects

- **A 2D texture's IDXGISurface is a real interface** (2026-10-21) — `obj_QI` -> `dxsurf_qi`, `DxSurf` / `g_dxsurf_vtbl` (peload.c),
  `swap_GetBuffer`, D3DObj `dxsurf`. Why: QueryInterface answered every IID with the texture itself, so IDXGISurface::GetDesc
  (slot 8) ran ID3D11Resource::SetEvictionPriority and the caller read its own uninitialised memory as the size. Project
  Gravemind lays its launcher out from the back buffer's DXGI_SURFACE_DESC: it drew everything at 1/1080 scale, 40 million pixels
  to the side (an empty launcher). How: for IDXGISurface / 1 / 2 a second vtable of the same object (one per texture, freed with
  it; AddRef / Release are the texture's): GetDesc (size, format, 1 sample), GetDevice, Get/SetPrivateData (none), Map / Unmap
  (the CPU copy), IDXGISurface2::GetResource; QueryInterface from it for anything else gives the texture. GetBuffer with a surface
  IID returns it too; Direct2D accepts either. Old: `PELOAD_DXGI_SURFACE_OLD=1`.

- **Halo 2's back buffer has its swap chain's size** (2026-10-21) — `dxgi_CreateSwapChain`, `swap_size`. Why: every back buffer was
  1920x1080 (Reclaimer's UI is laid out for it); Gravemind asks for 1280x720 and lays its Direct2D screens out for the target's
  size, only the viewport's 1280x720 corner would show. How: for h2 (games.c) the CreateSwapChain desc's size (320x200..4096x4096)
  becomes the back buffer's. `PELOAD_SWAP_DESC_SIZE=0` = 1920x1080 for h2 too, `=1` for any game; `PELOAD_SWAP_SIZE` still wins.
- **The present latch is one function** (2026-10-21) — `d3dgl_latch_present`, split out of the draw path unchanged. Why: a frame
  drawn only by Direct2D (Gravemind's launcher: no D3D draw into the back buffer) never became the presented texture. How: d2d.c's
  composites, clears and texture quads into the back buffer latch it like a draw does.

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
- **Private-data interfaces are referenced like Windows does** (client 0.9.23, 2026-10-24) — `d3d_SetPDI` / `d3d_GetPD`
  (device, swap chain: the `g_spdi` table), `child_SetPDI` / `child_SetPD` / `child_GetPD` (PDStore entries now flag interface
  data), `d3dobj_stage` (releases them). Why: 0.9.23's menu drew 1 draw a frame (its render job thread was stuck). Its new constant-buffer
  upload filter (src/engine_constant_uploads.rs: it patches the context's Map / Unmap and keeps shadow copies of DISCARD-mapped
  dynamic constant buffers <= 64 KB) attaches a "resource watcher" COM object to each buffer with SetPrivateDataInterface
  ({9FCF6ACD-C385-4940-...}) and then Releases its own reference. Our SetPrivateDataInterface didn't AddRef, so that Release
  freed the watcher, whose cleanup took the filter's mutex -- which the filter's Map wrapper was holding: the render job
  thread deadlocked on itself (WaitOnAddress, every other thread waiting on it). How: SetPrivateDataInterface AddRefs the
  new interface and releases the one it replaces (NULL clears), SetPrivateData over interface data releases it, GetPrivateData
  of interface data returns an AddRef'd pointer (halo3 Releases what it gets: without that AddRef, halo3+0x2A2293 crashed),
  and an object's interfaces are released when it is destroyed (collected under the pending-free lock, released after it).
  The first try (releasing in obj_Release at refcount 0) crashed in halo3; the missing GetPrivateData reference was the
  cause. With it: menu ~620 draws a frame, loaded in ~20 s; 0.9.22 unchanged (620 draws). The filter itself stays on (it is the client's
  own optimisation; `RECLAIMER_SEND_ALL_UPLOADS=1` is its off switch). Old: `PELOAD_PDI_NOREF=1` (no references: the
  deadlock again).
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

- **GetType / GetDesc lied about every texture: halo3's auto-exposure never ran (dark interiors, backlog 61)** — `res_GetType`,
  `res_GetDesc`, `dev_CreateBuffer`. Why: Hugegrass's vehicle bay (and every dark interior) rendered almost black at a fixed
  exposure of ~0.17. halo3's auto-exposure (update halo3+0x287288) reads the frame's 1x1 average-luminance target back on the
  CPU: halo3+0x2a0d00 asks the resource's type and accepts only TEXTURE2D / TEXTURE3D, and the staging copies it reads come
  from the luminance target's GetDesc (halo3+0x2a1486). GetType returned our internal kind (2 = Texture2D), which is D3D's
  TEXTURE1D, so the readback quietly gave up every frame; and GetDesc reported Format 28 (RGBA8), one mip, one slice and no
  usage / bind / CPU flags for EVERY texture, so the staging copies were RGBA8 and the half-float result (decoded by
  halo3+0x1dc05c) never arrived. How: GetType maps kinds to D3D11_RESOURCE_DIMENSION; GetDesc reports the stored
  description (format, mips, array size / depth, usage, bind, CPU access, misc; buffers too, whose usage and CPU access
  CreateBuffer now keeps). Checked: the readback carries the log luminance (half 0xC52F = -5.2 in the menu); the bay's
  exposure 0.17 -> 7.4 (the room, the Shadow, the structures visible); Guardian's interior at 2.4, nothing blown out; the
  client's menu grading (which checks the back buffer's format) still runs. `PELOAD_GETTYPE_OLD=1` / `PELOAD_GETDESC_OLD=1`
  = old. Found with `PELOAD_TRACE_MAPREAD=1` (every READ map with its guest caller and first bytes) and
  `PELOAD_CS_GPUINPUT_LOG=1` (a CPU compute kernel reading a GPU render target: none). (10-18)
- **OMSetRenderTargetsAndUnorderedAccessViews** — `ctx_OMSetRenderTargetsAndUAVs` (context vtable 34). Why: the do-nothing
  default; haloreach.dll binds many of its passes' targets with it (halo3 never did), so ~500 draws a frame went to the
  previous targets or none and the frame never reached the back buffer (window black, `[presentsrc] tex=0`). How: render
  targets + depth as OMSetRenderTargets (NumRTVs 0xFFFFFFFF keeps them); pixel-shader UAVs are not bound (noted once with
  `PELOAD_TRACE_D3DGL`). Checked: Sword Base renders with HUD and first-person weapon, ~2,500 draws a frame, none skipped.
  Old: `PELOAD_OM_UAV_OLD=1`. (10-20, reach-port)

## 11. D3D11->GL: shaders and the DXBC->GLSL translator

- **System-value semantics case-insensitive** (2026-10-21) — dxbc2glsl.c `dg_out_is_pos` and the input / output signature loops. Why:
  halo2.dll's shaders name the position "SV_POSITION" (halo3's say "SV_Position"); with strcmp the vertex shader wrote its position
  into a plain varying and never set gl_Position, so nothing rasterized -- Project Gravemind's black frame (every render target
  empty, 0 samples passed even with PELOAD_D3DGL_SOLIDPS). How: strcasecmp / strncasecmp for SV_Position and SV_Depth (as
  SV_VertexID / SV_InstanceID already were). Halo 3 and Reach unaffected (same names, other case).
- **Diagnostics by present count** (2026-10-21) — `PELOAD_D3DGL_DUMPFRAME_N=<present #>` (the frame draw list) and `PELOAD_RTDUMP_N=<present #>`
  (every render target of a frame, the stats page's rtdump) for games without Reclaimer's "gameplay presentation ready" line or a
  stats page; `PELOAD_TRACE_HASH` also prints slot 0's first four vertices; `PELOAD_TRACE_SWAP` logs CreateSwapChain;
  `PELOAD_FRAMELOG` now records clears, render-target binds and GetBuffer.
- **Real HLSL compiler, opt-in** (2026-10-20) — `i_D3DCompile`, `PELOAD_REAL_D3DCOMPILE=1`: MCC's own D3DCompiler_47.dll (loaded as a guest
  DLL now that its msvcrt imports exist) compiles the source to real DXBC for the translator. Off by default (Reclaimer's video
  shaders are only looked at as blobs; its menu grading shader keeps its own path).
- **halo3's video shaders compiled for real** (2026-10-24) — `i_D3DCompile`. Why: halo3 compiles four HLSL shaders at start
  (two CopyToScreen PS, YUVToRGB PS, the quad VS: what its Bink movies are drawn with) and got 256-byte code-less placeholders,
  so a movie would have drawn nothing; and its D3DReflect / CreateInputLayout calls on them had nothing to read. How: for
  Halo 3's vs_ / ps_ targets MCC's own D3DCompiler_47.dll compiles them (as for Halo 2); MCC keeps it in its root folder, which
  no bare-name LoadLibrary finds for halo3, so it is looked for beside halo3.dll and one folder up. The menu grading compute
  shader keeps its own recognised path. Checked: all four compile and translate (350-640 bytes of GLSL), ~90 ms at start with
  the DLL load, menu unchanged (619 draws, 18.6 s). Old: `PELOAD_REAL_D3DCOMPILE=0`.
- **The translator trusted shader bytecode from map files** (security review 2026-10-19; harnesses with AddressSanitizer
  proved 1-3) — dxbc2glsl.c, dxbc_cpu.c, peload.c. Why: halo3 hands every shader of every map (Workshop and server maps too)
  to `dg_translate` at map load; on Windows the D3D11 runtime validates and checksums DXBC first, here nothing did. Found and
  fixed: (1) HEAP OVERFLOW writing an immediate constant buffer as text (a row prints up to 122 chars, the buffer allowed
  80: four integer rows sufficed) -> sized 128/row, stops at the end; (2) a signature register >= 2^31 became a NEGATIVE
  index (`sscanf %d`) and `has_v[d]=1` wrote below a stack array -> `d>=0` checks, signature registers >= 64 dropped, names
  only identifier characters; (3) control flow nested past 64 overflowed `cf[64]` -> refused; (4) relative indices nested
  without limit (stack exhaustion) -> one level, operands read only within their instruction; (5) compute numthreads
  unchecked (a 1024x1024 group ran for an hour, 65536x65536 divided by 0) -> D3D11's limits, Dispatch with a zero
  dimension runs nothing; (6) a temp index from the bytecode sized the declarations (r2147483646 = 2^31 lines) -> r0..r4095;
  (7) the translator's shared tables (ICB, signatures, sysin, tf mode) raced between the creating thread, the dispatcher and
  the GL thread -> one mutex (`dg_translate_tf` for the transform-feedback form); (8) a CB block size parsed from text
  trusted (32 GB calloc per draw) -> <= 4096, calloc checked; (9) chunk counts up to 2^32 looped -> <= 64; (10) indexable
  temps capped at D3D's 4096. Checked: all 7,600 dumped stock shaders (~/reclaimer_shaders) translate to byte-identical GLSL
  (the same 7 fail before and after). Open: GPU-side relative indices are not clamped (undefined reads in GLSL; D3D reads
  0); the text passes over big shaders are quadratic. (10-19)
- **Texel offsets (aoffimmi) reach the GLSL** (2026-10-19) — dxbc2glsl.c `dg_decode_program`. Why: since the first translator
  (10-16) the flag was set while decoding the extended opcode tokens and RESET on the next line, so every `sample` /
  `sample_l` / `sample_b` / `sample_d` / `ld` with an immediate offset sampled its centre texel: the final composite
  (scene + bloom + colour grading, 4 pixel shaders) and 4 post-process shaders take 16 bloom / blur taps at offsets
  (-2..1, -2..1) that all collapsed to one -- bloom and blur blockier than Halo 3 draws them. (The 10-16 "translator
  correctness sweep" entry below lists aoffimmi offsets as fixed; they never were.) How: the flag is cleared before the
  extended tokens. 8 of the 7,600 dumped stock shaders change (90 textureOffset taps); all 8 compile in a GL 4.1 core
  context. Visual check pending. Old: `PELOAD_D3DGL_NOAOFF=1`. (10-19)

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
  launch; the cache is written for inspection (`PELOAD_D3DGL_GLSLCACHE=1` reads it). (10-16 a6b7628; now v5) The writing is
  opt-in since 10-21 (next entry).
- **GLSL files only on request** — `d3dgl_load_glsl`. Why: every launch wrote each translated shader to
  ~/reclaimer_glsl_v5/<hash>.glsl "for inspection" -- 11,018 files, 127 MB in the user's home folder (Halo 3 + Reach + Halo 2
  + mods), read by nothing unless `PELOAD_D3DGL_GLSLCACHE=1`; the user asked for it to be off. How: written only with
  `PELOAD_D3DGL_GLSLDUMP=1` (= the old behaviour) or `PELOAD_D3DGL_GLSLCACHE=1` (which reads them back and keeps adding new
  ones); translation itself is unchanged. (10-21)
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

- **The client's menu-scene grading compute shader runs** (backlog 67) — `rtgrade_parse` (D3DCompile), `rtgrade_dispatch`
  (ctxi_Dispatch). Why: D3DCompile is a stub, so the one shader the CLIENT compiles (src/menu_scene.rs: the menu's back
  buffer -> luminance -> blue tones, Halo 3's own menu look) was a no-op and the main menu showed the plain sunset colours.
  The CPU compute path can't run it (a 1920x1080 typed-UAV texture every menu frame, from a GPU-only render target). How:
  D3DCompile recognises its source and reads the client's constants by structure (y = dot(rgb, A,B,C); y*(D,E,F) + G*y^4;
  x H) into a DXBC with one marker chunk "RCLG"; a Dispatch of it is one fullscreen GL draw into u0's texture (made a render
  target, so the client's one-pixel readback and later sampling take the GL copy), texelFetch of t0 (stored bytes of an sRGB
  store read through a UNORM view), every GL cache reset afterwards. Checked: the menu turns blue-moonlit as on Windows,
  menu 380-385 engine fps (390 before). `PELOAD_NO_RTGRADE=1` = the old placeholder. (10-18)

## 12. D3D11->GL: textures

- **Big texture arrays went up in one command the GL helper dropped (Forge World black)** — `d3dgl_upload_layered`. Why
  (backlog 56): the native GL helper drops any command over half its 256 MB ring; Forge World's lightmap array (2048x2048x8
  BC3, decoded to RGBA8) is 128 MB in one glTexImage3D ("[glh] command 116 of 134217816 bytes is too large: dropped"), so the
  helper's texture stayed empty, Apple's driver sampled it as zero ("unit 0 GLD_TEXTURE_INDEX_2D_ARRAY is unloadable ...
  using zero texture") and the map's static lighting came out black -- floors, walls, the terrain. How: layered textures over
  16 MB are allocated with a NULL glTexImage3D and filled one slice per glTexSubImage3D (missing slices: zeros, as before).
  Checked: the spawn room's floor pixel's lighting draw writes 0.15 instead of 0, the room is lit. `PELOAD_LAYERED_ONECMD=1` =
  the old single command. Also new: the helper checks every texture upload with its own glGetError (the loader's answers
  0 without a round trip) and logs the first 40 failures (`[glhelper] <call> error 0x...`; `PELOAD_GLH_NOTEXERR=1` = off). (10-18)

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

- **Retina screens: the picture filled the bottom-left quarter** — `cocoa_make_gl_window`, `cocoa_pump`. Why (M1 MacBook,
  2026-10-18): the window's GL surface is at the screen's pixel resolution (2x on a Retina screen), but its size was taken
  from the view's bounds in POINTS, so every window blit set a viewport a quarter of the surface. This Mac's 1x external
  monitor never showed it. How: the surface asks for full resolution explicitly (setWantsBestResolutionOpenGLSurface:YES,
  AppKit's default here anyway), and its size is the bounds converted to backing pixels (convertRectToBacking:), rechecked
  every pump (a window moved to another screen follows); g_win_scale = pixels per point (`[cocoa] window ... (scale N)`).
  Overlay label sizes stay in points. `PELOAD_RETINA=0` = a points-sized surface the compositor scales up. (10-18)

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

- **Register indices from the bytecode checked unsigned** (security review 2026-10-19) — dxbc_cpu.c `cs_src` / `cs_dst` /
  `cs_relidx` / `csg_src` / `csg_dst` / `csg_relidx`, program setup. Why: `(int)idx < ntemps` let an index >= 2^31 through, and
  `r[idx]` then read or WROTE 32 GB+ past the register file; dcl_temps / dcl_indexable_temp counts could go negative or wrap.
  Shader bytecode comes from the .map files -- a Workshop or server-shared map's too -- and unlike Windows (the D3D11 runtime
  checksums and validates DXBC) nothing checked it. How: unsigned compares, `dcl_temps` <= 4096 and indexable temps <= 65536
  refused when parsed, constant-buffer / immediate-constant-buffer index math in 64 bits. Valid shaders run as before. (10-19)
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

Where every point is in every release, with signatures: patch-addresses.md (sections 2-3, 6, 8.6; v0.9.20 and v0.9.22
tables in 3).
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
- **Moderation table: players Vec found by its code** (client 0.9.22) — `st_addr_install` / `st_addr_refresh`. Why: 0.9.22
  added 0xc8 bytes of fields to the table before the players Vec (the new VPN / ASN block lists, GUESSED), so lock+0xa8 --
  taken as the Vec's len since 0.9.18 -- was another field and the stats page's player addresses (shown while hosting)
  would have been read from the wrong place; the Option tag was read at len-0xa0, also moved. How: the lock is found as
  before; the Vec's len is the most common target of `imul r64,[rip+d],0x68` (len x the 0x68-byte entry) within 0x400
  bytes after the lock, the Option tag lock+8. Same result for 0.9.18 / 0.9.20 (lock+0xa8), 0.9.22 lock+0x170; the launch
  line names it ("players at lock+0x..."). Checked live: our 0.9.22 dedicated server's stats listed a joined client's
  address. No old-behaviour switch (the old offsets are wrong for 0.9.22 and identical for older builds). (10-20)
- **play.sh knows client 0.9.22** — `KNOWN` list (SHA-256 517ee179...). Why: the 0.9.22 port didn't add it, so every launch
  warned "not a client build this loader was tested with". (10-21)
- **Port to client v0.9.22** (v0.9.21 skipped: current for a day) — every signature and hook found unchanged (sites in
  patch-addresses.md section 3); the Ballot cache grew to 0x3f8 (three 0x140-byte choices, were 0x128): the vote fields
  are still its last 0x20 bytes, where st_bv_tick reads them; the moderation table moved (entry above). Checked in game
  against our dedicated server on 0.9.22: menu, join, match, chat hook, spawns, slots, Quality control. (10-20)
- **Port to client v0.9.23** (2026-10-24) — the watchdog needed a new row: 0.9.23 sets a flag byte (`mov byte [rbp+X],1`)
  between the deadline test's `je` and the `Instant::now` call and reloads the deadline from the frame again (`mov rcx,[rbp+X] ;
  cmp rax,rcx`), so neither older row matched; new `g_sigpatches` row (exe+0x87ace5, `74` -> `EB`). Every other point moved
  with its signature (patch-addresses.md section 3 v0.9.23 table; shim/docs/porting.md section 3 walks through it). The live
  check found the private-data deadlock (section 10). Checked in game against our dedicated server on 0.9.23: menu, join,
  match, chat hook (an RCON `say` line in the feed), server-side player addresses, slots (port-verify.sh: 18 PASS).
- **stats.c signatures are per-point lists** (2026-10-24) — `g_sig_set_muted`, `g_sig_voice_recv`, `g_sig_push_history`,
  `g_sig_max_players`, `g_sig_cast_vote`, `g_sig_ballot`, `sig_next`. Why: porting a stats hook meant editing C inside its
  install function (one string each, fixed offsets after it), and nothing could add one mechanically. How: one 0-terminated
  list per point, tried in order; the first signature that matches once AND passes the point's meaning check is used, else
  the next. re-tools/patchscan.py reads the lists by name; re-tools/sigport.py --apply appends to them. set_muted's lock is
  now its first `lock cmpxchg byte [rip+d],cl` within 0x40 bytes (was: the disp32 at +0x20, right only for the v0.9.2+
  prologue), and the is_muted pairing accepts any `mov dl,1 ; xor eax,eax ; lock cmpxchg [rip+d],dl` on that lock (was: one
  13-byte is_muted prologue, so the pre-0.9.2 `56 57 48 83 EC 28` is_muted never paired). Checked on every release 0.8.1 ..
  0.9.23: each real set_muted pairs, the 0.8.16 .. 0.9.1 Forge lookalikes (only ever locked with cl) don't. Same results on
  0.9.22 / 0.9.23 as before.
- **Moderation table: at least 3 reading sites** (2026-10-24) — `st_addr_install`. Why: re-tools/patchscan.py over every
  release found a "players Vec" in 0.8.9 and 0.8.11, before the table existed: a single stray `imul r64,[rip+d],0x68`
  after the first lock. How: the most common target is accepted only with >= 3 sites (the real Vec: 6-7 in 0.9.2 .. 0.9.23).
- **The loader logs where it hooked** (2026-10-24) — `st_voice_install` / `st_chat_install` print the set_muted, voice_recv
  and push_history addresses on their `[stats]` lines, so shim/tools/port-verify.sh compares every point's address between
  the loader (C, at load) and re-tools/patchscan.py (Python, on the file).
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
- **object_get_origin empty-slot guard** (default on, bytes checked; `PELOAD_NO_ORIGINGUARD=1` = off) — `originguard_install`.
  Why (backlog 60): a client crashed once in a ~100-player Forge game, 0xC0000005 at RVA 0x3451E8 reading [0+0x10]:
  object_get_origin (halo3+0x3451b4) was asked for an object deleted under the client's Forge placement sync. How: the two
  table-slot loads (the object @0x3451e3, an attached object's parent @0x345213) go through a trampoline that tests the
  loaded pointer; NULL -> `*out = (0,0,0)` and the function's own return @0x345232; the normal path is the original bytes.
  The jumps are `jmp [rip+0]` + address and touch NO register. The first version (e425a5c, then opt-in) jumped through r11
  (`mov r11,imm64 ; jmp r11`, like the older guards) and crashed 2 of 2 loads into Hugegrass at RVA 0x34534F: halo3 is
  built with whole-program optimisation, and halo3+0x57ab3 keeps the object handle in r11 across object_get_origin (it
  knows that leaf never writes r11), then calls object_get_orientation with it -- which got the jump address's low half
  (0x51f1). Checked: the older guards' sites don't need it (skinguard's function and vtguard's sweeps call imports /
  virtual functions, so no caller can keep r11 across them, and r11 is not read near the sites). Offline test:
  `bash shim/tools/originguard-test.sh` (the real code on halo3's own bytes: same origin for a live object, (0,0,0) for an
  empty object / parent slot, r11 kept, refused on other bytes; it fails on the r11 version). (10-18)
- **Invisible walls off switch** (feature; written at run time, bytes checked) — `invwalls_prepare` (peload.c) +
  `hb_invwalls_set` (hitbox.c). Why (backlog 18, user: buttons for games you host / our local servers): player clip
  stopped explorers, and no branch in the vector tests controls it (map-boundaries.md 2c). How: Havok's polygon-shape
  builder halo3+0x21b4d8 (called by the structure / instance shape collections' getChildShape 0x21c0f8 / 0x21cf6c) gets
  an exit patch at 0x21b759 (15 bytes -> `jmp [rip+0]`): a surface flagged invisible gets every vertex at z = -20000. The
  trampoline uses only registers that function writes anyway and no register for its jumps. Written / removed at the
  frame safe point under the game-state lock (like the boundaries). Stats `do=invwalls&off=`, page button, launch
  `PELOAD_INVWALLS_OFF=1`, `PELOAD_INVWALLS_LOG=1`. Verified live: walls on, a biped stops at the wall; off (also switched
  at run time), it walks through. (10-18)
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

- **"Run 2.4x" removed** (2026-10-19, revert 606a38c) — it crashed the client as soon as it was used; the user asked for it
  to be deleted. The page's Speed 2x / speed menu (the host's movement-speed trait) is unchanged. (10-19)
- **Stats server: headers only, Host, Origin; no open CORS** (security review 2026-10-19) — stats.c `st_conn`, `st_hdr`,
  `st_host_ok`, `st_origin_ok`. Why: (1) the CSRF guard `X-Reclaimer-Control` was searched for anywhere in the request, body
  included, so ANY website could POST a text/plain body holding "\r\nX-Reclaimer-Control: 1" (no preflight) and run every
  control (mic on, recording, forensic delete / voice archive, captures filling the disk, teleport / speed when hosting);
  (2) no Host check, so a DNS-rebinding page could read /feed.json, /stats.json (player IPs while hosting), forensic voice
  .ogg and recordings and POST controls; (3) `Access-Control-Allow-Origin: *` on /stats.json, /heat.json, /events let every
  site read the live data. How: the request is read up to the empty line and headers are matched only there, at a line
  start; Host must be an IP literal or localhost with our port (421 otherwise; PELOAD_STATS_HOSTS=<names> adds names, e.g.
  this Mac's .local name for LAN use); a POST with an Origin must come from http://<Host>; ACAO only with PELOAD_STATS_CORS=1;
  5 s receive timeout, 128 connections at most, 431 for headers past 4 KB. Checked live: the page, curl and the loader's
  own forwarding work; a foreign Host, a body-borne guard and a foreign Origin are refused. Old: `PELOAD_STATS_ANYHOST=1`
  (Host), `PELOAD_STATS_CORS=1` (ACAO). (10-19)
- **Map-file and saved-file reads bounded** (security review 2026-10-19) — hitbox.c tag names (`noff` offsets from the .map
  outside the names table -> ""), collision models (node index outside the 128 pose matrices skipped; HB_MAXSEG 262144 edges
  per definition, checked reallocs in size_t), soft ceilings (HBZ_MAXTRI 1M), stats.c (NaN / inf positions -> `null`: they
  made /stats.json unparseable; spot ids / labels escaped; a held object's index checked before the read; heatmap death
  names NUL-terminated), forensic.c `fx_jstr` (a trailing `\` or short `\u` no longer reads past the end). A crafted
  Workshop / server map could crash the game through the stats thread or leak heap bytes into the page. (10-19)
- **Map tools: resource limits** (security review 2026-10-19) — stats.c `st_tool_wait` (h3bsp / h3inv / h3spots spawns),
  re-tools/h3map.py (`_cstr`, count checks, BLOCK_MAX_BYTES), h3bsp.py (PAGE_MAX, inflate capped at the page's size). Why:
  they run by themselves on every map played (Workshop and server-shared maps too) with no limits: a deflate bomb or crafted
  counts could take all memory (an 8 GB M1 swaps). How: nice 10, killed past PELOAD_TOOL_MB (default PELOAD_NAV_TOOL_MB, else
  3072) resident or PELOAD_TOOL_SECS (600); names at most 256 bytes; implausible tag / name / string-id counts refused; one
  block read <= 256 MB; a page inflates to exactly its declared size (<= 256 MB). Outputs on stock + Workshop maps unchanged
  (picture, height grid, invisible walls, walking graph byte-identical). (10-19)
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
- **"Match started" names the map being played** (2026-10-24) — stats.c `st_feed_line` / `st_snapshot`, hitbox.c
  `g_hb_map_recheck`. Why: the feed's "Match started: <map>" (on the client's "Loading screen complete" line) copied the map
  the last snapshot saw, and hb_current_map decides that only every 2 s: right after a load it was still a map the client
  merely opened (0.9.23 on Valhalla: "Match started: 130_epilogue"). How: that line sets a recheck flag (the next
  hb_current_map call decides again) and a pending flag; the next snapshot (<= 100 ms) takes the pending flag before its map
  lookup, so the lookup it posts with is the fresh one; the event keeps the loading screen's time. Checked live (0.9.23,
  Valhalla): "Match started: riverworld" = the snapshot's map; port-verify.sh checks it ("feed map").
- **Packet log: the type of every datagram and connection** (2026-10-24; user: "shim the QUIC datagrams and add a packet
  type log, not content, just type to and from and any flags ... to the forensic viewer and as a log window in the http
  screen; if it bogs stuff down, then don't") — shim/src/pktlog.c (new; included after forensic.c), call sites in peload.c
  `i_ws2_sendto` / `recvfrom` / `WSASendMsg` / `WSARecvMsg` (datagrams), `connect` / `send` / `recv` / `WSASend` /
  `closesocket` (TCP), `getaddrinfo` / `GetAddrInfoW` (host names), `hb_frame_hook` (`pk_game_tick`); stats.c GET
  /packets.json + POST do=pktgame; forensic.c FXI_PACKETS -> packets.jsonl; the stats page's Packets window and the
  forensic viewer's Packets panel. How: the wire layer reads only what the first bytes show without decryption (QUIC long
  header type and version, short header = 1-RTT with the spin / greased flags; plaintext tags; TCP connect / close with
  the bytes between) into a 16384-record ring (64-byte records, one atomic sequence number, readers check it before and
  after copying); the game layer (opt-in) swaps halo3's message-table encode / decode pointers (halo3+0x46b6e98, 46
  types; +0x20 encode = send, +0x28 decode = receive, checked on ping) for thunks that log the type and jump on (data
  only; installed / removed / re-wrapped at the frame safe point; the client patches code inside some encoders -- its
  table at exe+0x269625c, bit widths -- never the pointers). Cost measured: 26 ns per datagram record
  (`PELOAD_PKTLOG_BENCH=1`). Checked live (0.9.23 client joined to our dedicated server): client ~85 rx / 78 tx 1-RTT
  datagrams a second, the server the mirror image with the client's port; the game layer logged the join (connect-request
  rx, connect-establish, join-request tx, peer-establish, time-synchronize, view-establishment, membership-update /
  parameters-update rx ...). Off: `PELOAD_PKTLOG=0`; the game layer starts with `PELOAD_PKTLOG_GAME=1` or the page switch.
- **PELOAD_FRAMECAP and the client's datagram format** (2026-10-24; user: "pin them down" -- the 43 bytes 0.9.23 removed) —
  pktlog.c `pk_framecap_install` / `pk_on_frame` (debug only): an entry hook on the client's frame sender (0.9.23 exe+0x8bb5a0, by
  signature; the hook compares the image's own first 19 bytes since the frame size differs between builds) logs the first n
  frames of each kind before the client compacts and encrypts them. Result (shim/docs/client-transport.md): 0.9.22 sent
  `[class][message id 4][fragment index][fragment count]` + the whole 36-byte internal header (kind tag 4, session value 16,
  sender / recipient endpoint ids 8 + 8) = 43 bytes per datagram; 0.9.23 sends `[kind code | flags][class]` and adds the ids
  (16) or the fragment fields (6) only when needed. Measured with the packet log: ~31 / ~36 bytes less per datagram on average.
- **Privacy: who sees your IP address** (2026-10-24) — README "Who sees your IP address": a joined client has one QUIC peer,
  the server (voice relayed); other players never get your address; the server, its admins, every server the browser queries
  and the community services do; hosting publishes it.
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

- **Host controls from a client of our local server** — `st_forward` (stats.c), play.sh, dedicated.sh. Why: the stats
  page's speed / boundaries / map-click teleport are host actions; a client joined to our local dedicated server got
  "unknown or disabled action" (the user, trying 2x speed). How: when this client isn't the host and knows the host
  loader's stats address (PELOAD_TELEPORT_HOST: play.sh sets it for 127.0.0.1 joins when dedicated.sh's page answers on
  8767, which dedicated.sh now always serves), the request goes to the host; "Boundaries off" also patches this client's
  own prediction. The page shows the controls (and the new "Speed 2x" toggle) only for loaders that forward. (10-18)

- **Stats control query length** — Why: `qs[128]` cut longer requests (a do=uisteps step list stopped after ~120 bytes, so the
  last steps never ran). How: path 1024, query 768. (10-18)

- **Teleport for a player on our local server (a guest of the host)** — stats.c `do=teleport` / `do=explore`, hitbox.c
  `hb_engine_jobs_locked`, the page. Why (user, 2026-10-18: "I'm not host so I can't teleport!"): a client of our dedicated
  server got the Boundaries button (forwarded) but the Teleport button ran locally (the game refuses a guest), the explore
  spots were host-only, and the map's click-to-teleport said "only the host can teleport". How: a guest's Teleport is
  forwarded as `do=teleport&who=<slot>`, an explore spot as boundaries off (forwarded + its own movement patched) + `tpto&slot=`;
  the host moves a forwarded requester for the far side too (not only "next to them"), without a host camera (a dedicated
  server has none: the player's own facing); the page enables Teleport, the spot list and click-to-teleport for guests
  (`tp_forward`). Two bugs on the way: a boundaries request marks the next engine job "boundaries only", which swallowed a
  teleport sent right behind it (the host clears the mark for teleport requests; a guest's explore also waits 300 ms between
  the two); and the client's map detection (hb_current_map) only weighs the 32 most recently opened map files -- the client
  opens many after a match starts, Valhalla fell out and Standoff won the definition vote (88 of 88 match both), so the page
  had no Valhalla spots: with the engine's scenario name known and no recent file matching, halo3/maps/<scenario>.map
  itself is used. Checked live as a guest of our dedicated server: Teleport (far side), the east-cliff spot (boundaries off,
  standing on the cliff, walking around), click-to-teleport. (10-18)
- **Forge canvases' map pictures were a speck; stale pictures are remade** — re-tools/h3bsp.py `TOOL_VERSION`, stats.c
  `st_maptop_ensure` (`ST_MAPTOP_VER`), the `/maptop/` server, the page's `pollHeat` / `loadMapTop`. Why (user, 2026-10-19:
  "alpine heatbox sandbox does not render"; the heatmap showed a grey speck, the player clamped to a corner): Ultimate
  Forge's sandboxes (Alpine, Desert, Jungle, Savannah, Tundra, Island, Mombasa, Flood) are flat canvases whose 4-9 placed
  objects sit at the origin, so the picture framed ~36 x 36 units of a 1155 x 1155 canvas. The Forge-canvas framing rule
  (10-18 03:25) fixed h3bsp.py, but the loader only makes a picture that is MISSING, so these 8, made at 02:41, stayed. How:
  h3bsp.py writes `"tool_version"` (2); the loader makes a picture with an older or no version again in the background
  (keeping the old one if it can't: no numpy / no tool; `PELOAD_MAPTOP_KEEP=1` = never remake), and the page fetches the
  picture again (cache-busted) when the state goes making -> ready. The 8 were remade here, the 39 current ones stamped 2.
  Also: `/maptop/` served only lower-case names, so HugeGrass Rebalance (`fixedHugeGrassTrapasRemix`) never had a picture
  on the page (now A-Z and `-` too; `..` still refused). (10-19)
- **Spots past the boundaries for every map, in a Teleport list** — re-tools/h3spots.py, stats.c `st_dyn_load` /
  `st_spot_find`, the page's `#tpsel`. Why (user: "several known good locations for standing on the other side of what used
  to be boundaries ... for all maps", "a drop down list next to the teleport button"): only two hand-picked Valhalla spots
  existed. How: h3spots.py builds h3nav's floor field (all collision: structure, instances, player clip, placed scenery;
  slip terrain counts as floor) and keeps the floors in a kill volume or outside every safe zone with 1.0 of headroom,
  groups them into flat regions (neighbours within 0.5), and gives each region of >= 12 square units its most interior cell
  (>= 1 unit from any edge), up to 8 per map, 18 apart, named by compass direction from the play area's centre + high /
  low ground. maptop/<map>.spots.json, made for all 40 multiplayer + Workshop maps here (0 on enclosed maps: Chill Out,
  Fortress, Midship, Flood Sandbox) and in the background the first time a new map is played. The page lists them after the
  hand-picked spots; Teleport goes to the picked entry ("far side of the map" = the old button). (10-18)
- **Every spot checked in game** — shim/tools/spots-verify.sh, stats.c `st_dyn_load`. Why: a spot from the map file alone can
  be outside the map's structure (the host refuses the teleport), in a place that kills (Sandtrap's and Sandbox's Guardians
  zap players in the dunes) or on a surface you slide off. How: per map our local dedicated server plays it (Slayer), one
  minimized input-less client joins, boundaries go off (server + the client's own movement), and the server teleports the
  player to each spot; 5 s and 10 s later it must be alive, within 3 units of the spot, no more than 2 lower, and still.
  Passing spots get `"verified": true`, the rest move to `"rejected"` with the reason, the file gets `"checked"`. The loader
  lists only the spots array (a checked file's rejected entries come after it) and labels an unchecked spot "(not checked
  yet)" (a map first played after this, whose spots were just made in the background). Result 2026-10-18/19, 41 maps: 146
  spots verified, 18 rejected (8 refused outside the structure: Construct 3, Avalanche 3, Edge 1, Sandtrap 1; 10 died:
  Sandbox 5, Sandtrap 2, Forge Halo 2, Snowbound 1); Chill Out, Citadel, Heretic and Flood Sandbox have none (enclosed). The
  script's server names: the client's own map table (Highground is "High Ground"); a server that doesn't start is skipped at
  once. A first join right after a server start timed out 10 times in ~38 (the second always worked: section 22). (10-19)
- **Several spots on one big floor; checked spots kept** — re-tools/h3spots.py. Why (user: "several known good locations"
  per map): a region gave one spot, so the Forge worlds' single huge floor past the boundaries gave 1, and 18 maps had
  under 4. How: after a region's most interior cell, farthest-point picks among its cells at least a quarter as far from
  the edge, MIN_SPACING apart (every region's best first); `--keep` keeps a checked file's verified spots, never proposes
  its rejected places again and drops "checked" when it added spots (spots-verify.sh then checks the map again). Result after
  the re-check: 243 spots verified on 40 maps (25 rejected); `--spacing` / `--min-area` (2026-10-19 later) for small maps
  (Ghost Town, Orbital, Foundry regenerated with 9 / 6, then checked: 5 / 4 / 2 verified -- 249 in all). (10-19)
- **Input-less clients answer the REQUIRED MODS screen** — stats.c `st_am_required` / `st_am_tick`, `PELOAD_AUTO_MODS=1`
  (quiet-run.sh, target.sh). Why (user, 2026-10-19, a test client stuck on "HugeGrass Rebalance: Another version -- update
  it from Steam Workshop": "that screen should be hooked to use depot downloader on its own"): the screen IS hooked (its
  Update & join / Download & join starts `workshop-download`, which the loader runs as workshop-helper.sh, section 16), but
  a window without input can't press it. How: 1.5 s after the client's "Joining server failed: Required mod ..." line, Enter
  is posted to the game window (wi_steps_async, as auto-vote's keys); 3 presses at most per 10 minutes. The client's own
  `auto_download_mods` covers only mods a server hands out itself, not Workshop ones. Off: unset / `PELOAD_AUTO_MODS=0`.
  Not yet seen live (the stale HugeGrass copy was updated by hand: the server's own copy had updated itself to 1.0.75, the
  client's was 1.0.3; DepotDownloader fetched 36 MB). (10-19)

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

- **PELOAD_BP=<dll>+<rva>** (2026-10-21) — `bp_install_mod` (load_guest, before the DLL's DllMain), `bp_plant`: the software
  breakpoints also inside guest DLLs (`mss64+e700`, the DLL's name without extension, exact), up to 8 in all; a hit names its module
  (`[bp] HIT mss64+0xe700 #1 ... rcx= rdx=`). Exe RVAs as before. Found the missing Miles plugin (section 4).
- **shim/tools/d2d-test.sh** (2026-10-21) — the loader's Direct2D / DirectWrite / WIC through their COM vtables, as a game calls
  them (`PELOAD_D2D_SELFTEST=<out.bmp>`, d2d.c `d2d_selftest`): a CPU target (shapes, dashes, gradients, an arc path, clip, opacity
  layer, text, a wrapped layout's metrics, an in-memory font collection, a WIC-decoded PNG) and a texture target (GL clear, canvas
  composites, a texture quad under canvas content, the present latch) in-process and through the GL helper, offscreen; pixel checks,
  PASS / FAIL, pictures in /tmp. No game, no window. **shim/tools/com_slots.py** regenerates shim/src/d2d_slots.h from Wine's IDL.
- **shim/gravemind.sh** (2026-10-21) — Project Gravemind on the loader: installs the newest `client/h2gravemind*.zip` into C:\Gravemind,
  refuses beside a running game, passes Gravemind's own options through (`--map-id 44` offline Lockout, `--host`, `--connect`);
  `GM_TEST=1` = an input-less test run with its own profile (C:\ReclaimerProfileGMTest: test runs never use the player's profile or
  name). Logs `[xinput] XInputGetState called` (`PELOAD_TRACE_XINPUT`) to tell whether Gravemind polls controllers. Guide: shim/docs/gravemind.md.
- **shim/tools/game-matrix.sh** (2026-10-21) — see the game-profiles entry in section 1.
- **Reclaimer.app: double-click to play** (2026-10-19; user: "a .app I can click ... feels more macish than launching
  scripts") — shim/tools/make-app.sh, shim/tools/make-icon.py (shim/assets/Reclaimer.icns, an original icon), peload.c
  `cocoa_app_icon`, install.sh `make_app`. How: a bundle whose program is a shell script holding the install folder's path:
  Homebrew on PATH (Finder-started apps have only /usr/bin:/bin..., DepotDownloader would be missing), a notification instead
  of a second launch while the game runs (play.sh would close the running game), dialogs for a moved folder / missing
  Rosetta, output to /tmp/reclaimer_launcher.log; LSUIElement (no Dock icon of its own) and PELOAD_APP_ICON gives the game
  window's Dock icon the app's icon. install.sh (full and --update) makes it in the install folder and ~/Applications;
  make-usb.sh carries the script and the icon. Not committed: the app itself (.gitignore; it holds a machine's path). (10-19)
  Fixed the same day (user: "the app did not seem to launch"): /tmp/reclaimer_launcher.log said "/bin/bash: shim/play.sh:
  Operation not permitted" -- macOS privacy protection (TCC) silently refuses an app whose PROGRAM is a shell script access to
  a protected folder (this checkout is on the Desktop; Documents / Downloads / USB volumes are protected too). Now the
  program is a tiny native launcher (shim/src/launcher.c, universal, built by build.sh) that runs Contents/Resources/
  launch.sh, the bundle is signed ad hoc, and Info.plist carries the Desktop / Documents / Downloads / removable-volume
  usage strings, so macOS can ask once instead. Checked: double-click (open) starts the menu from the Desktop checkout; a
  second click while it runs only notifies (game processes 2 -> 2).
  Then (user: "create the .app and sign it", "a new release including the .app"): shim/build.sh makes the app after every
  build (NO_APP=1 skips); make-app.sh --portable writes no folder path (launch.sh uses the folder the app sits in whenever
  it holds shim/play.sh, else the folder written in), RECLAIMER_SIGN_IDENTITY signs with a real identity (ad hoc otherwise,
  and if that identity fails), the signature is verified; make-release.sh puts a portable signed app at the top of the
  bundle, adds the launcher, the icon, machine.sh / workshop-helper.sh / client-update.sh / make-app.sh and the map tools
  the loader runs (h3nav, h3inv, h3spots, h3dmg), and refuses to package uncommitted shim/ or re-tools/ changes.
- **clipboard-test.sh** (shim/tools) — the loader's clipboard + GlobalAlloc code extracted from peload.c by marker, built for
  x86_64 and run under Rosetta against a private named pasteboard (your clipboard is never touched): Global* blocks,
  owner / not-open rules, copy with CRLF and non-ASCII, paste as UTF-16 and UTF-8, registered formats, the format list
  after EmptyClipboard, the sequence number, PELOAD_NO_CLIPBOARD. (10-20)
- **Megalo lab** (backlog 72; shim/tools/megalo-lab/megalo_fuzz.py, shim/docs/megalo.md) — Why (user, 2026-10-19: "isolate
  it for us to test the things it supports ... make it crash, corrupt memory"): the client's Megalo compiler is reachable
  without MCC through its own `megalo-check` / `mode-compile` / `mode-check` subcommands (~0.07 s each under the loader).
  How: `corpus` extracts the example scripts / presets the client carries (to Application Support, not git), `run` runs one
  with a verdict (OK / ERR / PANIC / CRASH / HANG), `fuzz` / `json` mutate sources / presets (numbers to extremes, kind
  swaps, API swaps, wrong arity, deep nesting, long expressions, strings, capacity) and keep anything that panics,
  crashes, hangs or is refused in a new way. First 3,000 mutants: nothing but clean refusals. (10-19)

- **play.sh: the local dedicated server is asked for up to 15 s** — Why (user, 2026-10-19: "I can't use teleport or boundary
  disable"; the Speed 2x toggle was gone too): a client started 20 s after its server came up asked once, with a 1 s
  timeout, before the server answered "dedicated", so PELOAD_TELEPORT_HOST was never set and every host control on its
  page stayed off for the whole game. How: up to 15 tries a second apart. (10-19)

- **workshop-helper.sh size / disk checks; servers.sh strips control characters** (security review 2026-10-19). Why: a
  server only has to name a Workshop item for the client to ask for it (an input-less test client even presses Download:
  PELOAD_AUTO_MODS); servers.sh printed server names / mod titles raw, so a JSON "\u001b" became a real escape sequence in
  the terminal (title / clipboard OSC). How: refuse items over RECLAIMER_WORKSHOP_MAX_GB (default 32) or when the volume
  lacks the missing bytes + 1 GB; strip C0/C1 controls and bidi overrides from every printed field. (10-19)
- **build.sh builds to a temp file and renames** — safe while a game is running. (10-16 fe9e948)
- **play.sh** — checks the client exe's SHA-256 (patches were offsets into one build) (10-16 15e3edd); matches only the
  loader binary itself when killing leftovers (an unanchored pattern also matched shells mentioning build/peload)
  (10-16 f73252e); leaves a running target / dedicated server alone (10-16 f4eb1e6, de84a5b); its stdin feeder no longer
  holds the caller's stderr and is killed when the game exits (it held a caller's `| tail` for 24 h) (10-17 0465d03);
  frees the stats port first (10-18 0faafe7); uses the newest client/project-reclaimer-client-v*.exe (servers refuse an
  old client within hours of a release) (10-18 37c15de).
- **quiet-run.sh** — no longer minimizes by default (`QR_MINIMIZE=1`), anchored process match (10-16 f73252e); frees the
  port (0faafe7); serves the source pages (d157084).
- **quiet-run.sh writes its process group** (2026-10-24) — /tmp/quiet-run.pgid (`QR_PGID_FILE`) while the game runs, so a
  caller done early (port-verify.sh) stops exactly that group, never another game.
- **Porting toolkit** (2026-10-24; guide: shim/docs/porting.md) — the scripts that port the loader to a new client release
  and check the result, all in the repo (they were in a scratch folder once and got lost). shim/tools/fetch-releases.sh (the
  corpus: every release exe + SHA256SUMS.txt, verified), shim/tools/client-survey.sh (the loader's `--version` lines on
  two builds, import diff, `dedicated` subcommand), re-tools/patchscan.py (every point in any number of builds with the
  loader's own checks, the signatures read from the C sources; `--diff`, `--md`, `--json`, `--rev <commit>`; documented old
  misses shown with their anchor-found places from patch-addresses.md section 3), re-tools/sigport.py (ports a point to a
  new build: anchors -> candidates -> instruction-shape alignment -> wildcarded signature -> the meaning check; `--corpus`,
  `--apply`, `--selftest` replays every historical change), shim/tools/port-verify.sh (static + loader + live PASS / FAIL /
  SKIP per point against our dedicated server with one hidden client; the chat check sends an RCON `say`), re-tools/pelib.py
  (the shared PE reader: mapped like the loader, .pdata functions, call index, batched string references, file offsets),
  re-tools/fanchors.py / sigscan.py / ripref.py / callers.py / fcalls.py / imulscan.py / peimps.py, shim/tools/winshot.sh.
- **target.sh** — waits after typing the map name (55811fa) and 30 s (was 12) after a match before Start Game (the click
  came before the lobby was ready) (10-16 4621a04); no WASAPI for the target while the caulk crash was open (10-17 b02529c).
- **workshop-helper.sh progress** — Why: DepotDownloader prints a percentage only when a file finishes and verifies
  existing files silently; the client gives up after 45 s without progress ("Steam Workshop download timed out" at 863 MB
  of 8.9 GB). How: every 2 s the larger of the percentage and the bytes on disk, at most 99%, +1 byte heartbeat.
  (10-18 3eb0680)
- **Workshop helper item id from the dedicated server** — Why: the dedicated server runs `workshop-download --mcc-path
  <path> <id>` (the id last, no flag before it); the substitution found no id ("item -") and the server reported "Steam
  Workshop is unavailable". How: a trailing all-digit token not preceded by --mcc-path is the item id. (10-18)
- **usb-install.sh options for a small disk** — Why: the other Mac had 20.46 GB free for a ~46 GB install. Tested against a
  stripped MCC copy (links): the client refuses to start without the campaign maps ("requires BOTH Halo 3 Campaign and Halo
  3 Multiplayer") and its engine worker exits without MCC-WindowsNoEditor.pak ("MCC Conduit font archive unavailable");
  the menu boots without MCC/Content/Movies. How: --no-movies (~40 GB), --no-workshop (~29), --minimal (~25),
  --keep-on-stick (~15: the pak, movies and mods linked to the stick), --dry-run. (10-18)
- **usb-install.sh free space** — Why: it compared the copy with `df`'s free space only, which leaves out purgeable space
  (iCloud caches, local snapshots) that Finder counts and macOS frees on demand: a Mac showing 69.7 GB free in Finder was
  refused for a ~46 GB copy. How: the larger of df and NSURLVolumeAvailableCapacityForImportantUsageKey (via osascript JXA,
  no python needed), both printed; `--skip-space-check` goes on anyway. (10-18)
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
- **`PELOAD_BSPWATCH=s|e|v|sev` — who reads a collision BSP** (RE tool, debug only) — `bspwatch_install` / `bw_trap` /
  `bw_fault` / `bw_thread` (peload.c). A breakpoint on halo3+0x2566b7 keeps the collision BSP with the most surfaces
  (`PELOAD_BSPWATCH_HITS`, 3000 hits), then the pages of the chosen arrays (surfaces / edges / vertices) are made
  unreadable: each fault records the instruction and [rsp] (the caller, for the frameless accessors), opens that one
  page, and a thread closes open pages every 20 ms; `[bspwatch]` lists the readers every 5 s. It found Havok's
  polygon-shape builder (backlog 18). Lessons: open only the faulting page (one hot reader hid every other one), and
  treat any fault in the watched range as the tool's (another thread can fault before the page reopens). (10-18)

- **make-usb.sh / install.sh `--update`** — only the loader, GL helper, scripts, docs and client exe (no MCC files, Workshop
  mods, caches or map pictures; install.sh skips its space check). Why (user, 2026-10-18): "a --update flag that only copies
  the binaries and not the maps". Since 10-19 also the teleport spots (maptop/*.spots.json, ~30 KB, checked in game here;
  making them needs numpy, which the M1 lacks) and re-tools/h3spots.py. (10-18, 10-19)
- **Machine profile: decisions per Mac** (`shim/tools/machine.sh`, sourced by play.sh) — Why (user, 2026-10-18: "detect m1 vs m4
  and make code decisions accordingly"; M1 menu 78 fps vs ~390 on the M4 Max): chip, performance / efficiency cores, GPU
  cores (ioreg gpu-core-count; tier base <= 10, pro <= 20, max), memory. Decisions: particle workers = performance cores - 2
  in 2..8 (it was 4..8: 4 on an M1's 4 performance cores, competing with the main thread); a base-tier GPU's profile that
  never chose a preset (no "quality" in settings.json) gets the client's own Medium preset (decals High, the rest Medium;
  backup settings.json.before-medium; python3 only when it won't open the Command Line Tools dialog), never over a chosen
  one (RECLAIMER_KEEP_QUALITY=1 = never); at 8 GB a note and the background walking-graph tool capped at 1 GB
  (PELOAD_NAV_TOOL_MB; up to 4 GB otherwise -- the M1 has 8 GB). One `[machine]` line on the terminal and at the top of
  /tmp/reclaimer_play.log. The display scale is the loader's (section 14), read live. Checked: this M4 Max -> 8 workers,
  no change; a stand-in M1 (fake sysctl / ioreg) on a profile copy -> 2 workers, Medium written once, kept on the next run;
  the client reads it ("Graphics quality: one view Medium"). (10-18)
  Added 10-19: a base-tier GPU gets a 60 fps frame cap by default (PELOAD_MAX_FPS=60 unless set; =0 uncaps) -- frames past the
  panel's 60 Hz are never shown and only heat a fanless MacBook Air into throttling; and PELOAD_MONITOR=<w>x<h> (peload.c
  mon_wh, any Mac, opt-in) sets the monitor size the loader reports (1920x1080 otherwise), which the client uses as its
  "native" render resolution -- 1600x900 or 1280x720 lowers the internal render resolution on a weak GPU.
- **shim/reach.sh; gameplay start for Reach Renaissance** — Why: a way to run a Reach match without Reach Renaissance's menus,
  and the loader's test tools (`PELOAD_INPUT_SCRIPT`, `PELOAD_KBM_TEST`, `PELOAD_D3DGL_SHOTWIN_FROM/_TO`, the walker) wait for
  gameplay start, which was read only from the Reclaimer client's "gameplay presentation ready" log line. How: reach.sh runs
  host-probe with the launcher's match template (shim/docs/reach-renaissance.md); `GAMEPLAY_MARK` also takes host-probe's
  "load fraction 1.000"; `PELOAD_KBM_TEST_DELAY=<s>` (default 3) starts the keyboard/mouse self-test later (Reach shows
  its loadout picker first). (10-20, reach-port)
- **reach.sh servers / join** — Why: joining a game without the menus. How: `servers` runs `rr-browser list`; `join <address>`
  starts `rr-browser serve` on a free local port, takes a lease (`/api/lease?address=`, what the menus do), and runs host-probe
  as a guest with the launcher's join-args.txt arguments (0.1.17) -- exactly those forms: the `\\?\C:\...` paths for
  `--strings` / `--extra-maps` (in plain `C:\` form host-probe skipped the Elite armor pack the host has, and the join stalled)
  and the player id from player_id.txt WITHOUT its CRLF (with a CR the host never got the guest's state:
  "external_launch_individual_state_timeout"). Checked against a local `rr-master` + a windowless host: the guest loads the
  host's Slayer match with both players on the scoreboard; then a public Firefight game on Beachhead (joined through the public
  master's rendezvous, played with its players). `RR_NAME=<name>` sets the in-game name. (10-20, reach-port)

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
- ~~**halo3's 4 Bink video shaders stay no-ops**~~ -- superseded 2026-10-24: the client's mod-movie path works (section 1,
  "halo3's Bink delay imports") and the four shaders are compiled for real (section 11, "Real HLSL compiler").
  `PELOAD_REFUSE_EMPTY_SHADER=1` refuses code-less blobs with E_INVALIDARG as D3D11 would.
- **Loader path handling is not a sandbox** (security review 2026-10-19): `win2unix` does not confine `..` to the drive,
  maps Z:\ to / and other letters to drive_x; GetFinalPathNameByHandleW returns the unresolved path (Rust canonicalize is
  not canonical, so the client's "escapes its package" checks pass for symlinks); FindFirstFile / GetFileInformationByHandleEx
  never report reparse points (the dedicated installer's "contains a filesystem link" check never fires; remove_dir_all
  follows directory symlinks). Not reachable through server mod file names (the client whitelists them), but a symlink in
  a Workshop item or our own links (usb-install's workshop folder, dedicated.sh's game/halo3) matter. BCryptVerifySignature
  always succeeds (what halo3 verifies with it is not traced). Fixing needs care: the client canonicalizes both sides of
  some checks, and our "Program Files (x86)/Steam/steamapps" link must keep working.
- **First join right after a dedicated server starts** can fail its handshake ("Public join timed out"; 10 of ~38 in the
  10-19 spot checks); a retry works.
- Rock-wall seams (tabled by the user, display-only). (61 Hugegrass dark and 56 Forge World black are fixed: sections 10, 12.)
