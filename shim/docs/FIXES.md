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
- **Client 0.10+ is a native Mac app** (2026-11-06; shim/docs/native-client.md) — `Project Reclaimer.app` from the release's
  macOS zip goes to /Applications; it needs no loader. Player folder `~/Library/Application Support/ProjectReclaimer` (copy
  the loader's `prefix/drive_c/ReclaimerProfile` there by hand: the app only migrates `~/Documents/My Games`), with
  `mcc-folder.txt` = the macOS path of the MCC folder. The identity: a loader profile's DPAPI-labelled key is the raw seed
  (our CryptProtectData is a passthrough) and the app calls it "locked here: not made by Wine" -- WHY it needs converting;
  `python3 shim/tools/profile-native.py convert <old profile.json> <new profile.json>` checks the seed derives the profile's
  public key and writes the app's version-2 format (plain `seed`; `--wine <user>` = a Wine CryptProtectData blob instead,
  both verified: same community id). It refuses to overwrite. The old profile is left as it is (the switch back = keep
  using the loader with it). Our hooks / stats page don't run in the native app yet (a dylib loads with
  DYLD_INSERT_LIBRARIES: no hardened runtime).
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

- **Client 0.10.0 ("rcl-engine") runs** (2026-11-06) — the client is now a windows-gnu (MinGW-style, msvcrt.dll) build that
  RECOMPILES halo3.dll ("the build lifted", "the engine's code is 100% made", "linked recompiled engine, image at
  0x144e05000") instead of loading it, and runs halo3's own delay-load helper with its own LoadLibraryExA / GetProcAddress.
  It needed: synthetic system DLL images (below), DXGI factory slots up to IDXGIFactory7 (section 10), PropVariantToStringAlloc,
  Windows' fiber layout and TEB stack fields (section 2), GetRawInputDeviceList and window properties (section 7), and a
  refusal of foreign event handles in WASAPI (section 9). With them it reaches its menu scene (628 draws a frame) and joins a
  local 0.10.0 dedicated server into a match (1,550-1,650 draws). Every exe signature (patchscan) misses in this build and
  halo3.dll is no loaded module, so the stats page's halo3 readers, the overlay, lucache and the halo3 patches are inactive
  with it; the old clients are unaffected (0.9.23 menu checked).
- **Synthetic dxgi.dll / d3d11.dll / d3dcompiler_47.dll images** (2026-11-06) — peload.c `synth_dll`, `synth_is`, used by
  `i_GetModuleHandleW/A` and `i_LoadLibraryExW`. Why: client 0.10.0 resolves halo3's delay imports by asking
  GetModuleHandleW("dxgi.dll") (the exe imports dxgi, so on Windows it is loaded) and reading that image's EXPORT TABLE itself;
  the loader answered every unknown name with the exe's own image, which exports none of them: "[runtime] fatal: unhandled
  exception 0xc06d007f ... the delay-loaded dxgi.dll is missing" (then D3DCOMPILER_47.dll). How: per DLL a 0x2000-byte PE
  image (headers, a sorted export directory, per export a 16-byte `jmp [rip+0]` thunk to the loader's implementation),
  made on first use; GetProcAddress on it finds the thunks like on a guest DLL. i_D3DCompile's search for MCC's real compiler
  skips our own image (it would find itself) and, with no halo3.dll module (0.10.0), looks in MCC's root in the prefix.
  Old: `PELOAD_NO_SYNTH_DLL=1`. `PELOAD_TRACE_GMH=1` logs every GetModuleHandle* name.
- **PropVariantToStringAlloc** (2026-11-06) — peload.c `i_PropVariantToStringAlloc`. Why: a logging stub that returned S_OK
  with the string unset; client 0.10.0 reads the audio device's friendly name with it and crashed in ucrtbase (wcslen of
  garbage) right after the engine was linked. How: a CoTaskMem copy of VT_LPWSTR / VT_BSTR, VT_LPSTR widened, "" for VT_EMPTY,
  TYPE_E_TYPEMISMATCH otherwise.
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

- **Fibers in Windows' layout: TEB+0x20 holds the fiber object, its first field the parameter** (2026-11-06) — peload.c
  `Fiber` (param first), `fiber_set_cur`, ConvertThreadToFiber / CreateFiberEx / SwitchToFiber / ConvertFiberToThread. Why:
  the slot held the PARAMETER; GetCurrentFiber() is the slot and GetFiberData() its first field on Windows, both read inline.
  halo3 reads the slot as the current fiber (halo3+0x5ff97d: below 0x10000 -> ConvertThreadToFiber, else kept as the fiber to
  switch back to) and only got away with it because its main thread's parameter is 0; client 0.10.0's runtime matches the
  slot against the fibers it created: "[runtime] fatal: unknown guest fiber started" once the menu scene loaded. Old:
  `PELOAD_FIBER_TEB_PARAM=1`.
- **TEB NtTib.StackBase / StackLimit filled, and switched with fibers** (2026-11-06) — peload.c `thread_tramp`, main's TEB,
  `teb_set_stack`. Why: both were 0; client 0.10.0's runtime copies at most 256 bytes from a saved stack pointer up to
  StackBase (exe+0x3cf8fe5: gs:[0x30] -> +8), and with 0 it copied the full 256 past the top of a thread's stack ("exception
  0xc0000005 ... ucrtbase memcpy"). How: the pthread stack range at thread start (and for the main thread); CreateFiberEx /
  ConvertThreadToFiber record each fiber's range and every switch puts it in the TEB, as SwitchToFiber does. Old:
  `PELOAD_TEB_NOSTACK=1`.
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
- **Chained unwind info unwinds the parent's whole prologue** (2026-10-25) — `i_RtlVirtualUnwind`. Why: a function fragment
  with UNW_FLAG_CHAININFO (MSVC splits functions this way: 3,703 of halo3's 21,768 .pdata entries) recursed into its parent
  with the parent's START as pc, so the prologue offset was 0 and none of the parent's pushes / stack allocation were undone:
  wrong rsp and return address for an exception unwinding through such a frame (found while writing the sampler's stack
  walker; no crash was traced to it). How: the parent is unwound with the same pc (past its prologue), as Windows / Wine do.
  Old: `PELOAD_UNWIND_CHAIN_OLD=1`.
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

- **is_sync reads only live heap blocks** (2026-11-06) — peload.c `is_sync`. Why: any handle value >= 0x100000 was read as a
  SyncObj; client 0.10.0's runtime hands out handles like 0x400000c0, which faulted the read. How: `malloc_size` must say it
  is a heap block first. Old: `PELOAD_SYNC_UNCHECKED=1`.
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

- **GetRawInputDeviceList, SetPropW / GetPropW / RemovePropW** (2026-11-06) — wininput.c `wi_GetRawInputDeviceList`, peload.c
  `i_SetPropW` / `i_GetPropW` / `i_RemovePropW`. Why: logging stubs that client 0.10.0 calls (the device list returned 0 with
  the count unset; SetPropW "failed", GetPropW always NULL). How: the two raw devices WM_INPUT reports (mouse 0x4D53, keyboard
  0x4B42); a 256-entry (window, name or atom) -> handle table.
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

- **IAudioClient::SetEventHandle refuses handles that are not our events** (2026-11-06) — wasapi.c `wac_SetEventHandle`.
  Why: client 0.10.0's dedicated server registered 0x400000c0 (from its runtime's own handle numbering) for its engine's audio
  stream; Start and the buffer callback signalled it and the server's worker crashed 2 s after starting (wac_Start+0x119).
  How: E_INVALIDARG, as Windows answers an invalid handle (the game client's streams get real events and play; the trace
  `PELOAD_TRACE_WASAPI=1` logs every handle).
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

- **DXGI factory through IDXGIFactory7** (2026-11-06) — peload.c `g_dxgifactory_vtbl`, `dxgi_EnumAdapterByGpuPreference`,
  `dxgi_EnumAdapterByLuid`, `dxgi_EnumWarpAdapter`, `dxgi_CheckFeatureSupport`, `com_zero`. Why: client 0.10.0 QueryInterfaces
  the factory for IDXGIFactory6 (com_QI says yes to everything) and calls EnumAdapterByGpuPreference (slot 29) -- past the end
  of the 22-slot table, it called whatever pointer followed (NULL): crash at RIP 0 right after "[mods] ...". How: the table
  runs to slot 31; the new enumerations answer like EnumAdapters1, no adapter by LUID, no WARP adapter, no tearing support,
  creation flags 0.
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
- **CPU writes wait for queued kernels that read the resource live** (2026-10-25; found by review while hunting the
  HugeGrass "giant garbage triangle") — peload.c `cs_wait_write`, D3DObj `cs_readers` (counted up when a job is queued, down
  when it finishes), used by `ctxi_Map` (write maps), `ctxi_UpdateSubresource`, the copies' destinations, ClearUAV and
  d3dsubmit.c SQ_COMMIT. Why: before the CPU side writes a resource it waited (`cs_wait_res`) for queued kernels that WRITE it,
  not for those that READ it live (a buffer a pending kernel writes, textures, inputs past the 48 snapshots / 8 MB): Unmap's new
  block swapped in by SQ_COMMIT -- the old block recycled for the next Map --, an update, a copy or a clear could land while a
  reading kernel was still queued or running, which then read the next frame's data or a recycled block. How: such writes wait
  for the readers too, and each time it happens a `[cs-cpu] <write> of a resource ... that N queued kernel(s) still read` line
  counts it. Not the artifact's cause as far as measured: 0 such writes in a 180 s HugeGrass match.
  PELOAD_CS_READWAIT=0 = wait for writers only, as before (still counted).
- **Particle dispatches on disjoint slices of the shared pool run side by side** (2026-10-25; user: "start with cutting queued
  frames and the particle-chain wait") — peload.c `csq_range_predict` / `csq_range_after` / `csq_conflict` (CsQJob plo/phi),
  dxbc_cpu.c CsCtx `track` (u0 byte ranges a dispatch read / wrote) and CsProg `rr_*`; PELOAD_CS_DEPLOG=<n> prints `[csdep]` /
  `[csrange]` lines (dependencies, actual ranges, constant buffers). Why: halo3's particle update kernel runs once per particle
  system (one 64-thread group) and every dispatch writes the same 2.2 MB pool buffer, so the whole-resource rule chained
  ~200 dispatches of ~80 us a frame; in a 100-player HugeGrass fight the GL submission thread waited ~40% of its time for the
  chain's end while the compute workers idled 80%. Each dispatch touches only elements [cb3.x, cb3.y) of the pool. How: per
  program the rule is learned from its first 64 dispatches (tracked accesses; a constant-buffer dword pair {first, end} in the
  first 16 bytes of a CB survives if every access starts exactly at `first` and stays below `end`; the tightest survivor
  wins), then u0 conflicts compare slices; every dispatch stays tracked and an access outside its slice turns the rule off
  for that program for good (`[cs-range] ... outside its predicted ...`; the first, looser learning picked a coincidental pair
  that a later dispatch broke -- caught). In-match A/B (PELOAD_AB=csrange:20, Tour Guide's HugeGrass, ~98 players, ~8,000
  draws, tools/ab-phases.py): particle-heavy phases 24.2 -> 32.5, 27.9 -> 33.8, 31.3 -> 34.2 fps with the GL thread's compute
  waits 804 -> 68, 571 -> 112, 353 -> 124 ms per [submit] line; light phases unchanged. PELOAD_CS_RANGE=0 = whole-resource
  ordering, as before.
- **A finished job that makes one job ready runs it on the same worker** (`ab_csnext`, PELOAD_CS_WAKEALL=1 = wake a worker as
  before): saves a thread wake per hop of a dependency chain; A/B neutral in a 128-player match (167 vs 170 ms, same fps).
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

- **Release bundle: the native app's tooling** (2026-11-07; shim/tools/make-release.sh, README "Project Reclaimer 0.10+") —
  the tarball also carries shim/build/nhook.dylib (built by shim/native/build.sh), shim/native.sh, profile-native.py and
  the native / forensics docs; the README's new section says how to use them with the 0.10 app. nhook.dylib is stripped
  (`strip -S -x`, re-signed ad hoc: its -g symbol table named the build folder, i.e. the builder's home path) and the script
  refuses a bundle in which any file contains $HOME.
- **nhook: our tooling in the native 0.10 app** (2026-11-06; shim/native/nhook.c + stub.S, shim/native.sh, docs
  shim/docs/native-client.md "Inside the app") — WHY: the native app runs no loader, so none of the loader's halo3 hooks or
  readers exist there. A DYLD_INSERT_LIBRARIES dylib: interposed write()/writev() assemble the app's log lines (it writes a
  line in pieces) and catch the engine image address; the runtime's guest -> host code table gives each lifted halo3
  function's entry; an entry hook replaces its first instruction by a `b` to a stub (MAP_JIT: pthread_jit_write_protect_np)
  that calls handler(guest context); the engine thread's context (hook on halo3+0x9d22c) gives halo3's TLS block
  (ctx+0x2d8), and the players table reads as under the loader. NHOOK_CENSUS=1 counts every lifted function's calls (which
  halo3 code the app still runs). Switches: NHOOK_NO_HOOKS=1 (log tap and probe only), no DYLD_INSERT_LIBRARIES = the app
  untouched.
- **nhook stats page** (2026-11-06; shim/native/nstats.c, shim/src/h3maps.c) — WHY: the stats page is the feature used most,
  and the native app has none. The loader's page and JSON shape on 127.0.0.1:8765 from the native app's memory: players and
  objects through the engine thread's TLS block, map / tag names from the .map files the app opens (interposed open/openat),
  variant + traits (stats.c's code verbatim), heatmap (the loader's files), feed from the app's log lines and the players
  table. hitbox.c's .map-file code moved into shim/src/h3maps.c unchanged (HB_MAXOBJ: the loader's 8192 object slots, the
  native app's 16384) so both use it. Off: NHOOK_STATS_HTTP=0. shim/native.sh refuses a second instance of the app.
- **nhook: kills, Connected list, packets** (2026-11-07; shim/native/nstats.c, nnet.c, nhook.c) — WHY: the native page
  showed kills 0 and an empty Connected list / Packets window. Kills, deaths, assists, betrayals, suicides and score are
  halo3's statborg (game engine globals, TLS +0x20, + 0xfeb8 in halo3.dll; the app moves it and logs where:
  `[capacity] game_globals.statborg`): 32 i16 per player at +slot*0x40, stat 2 kills, 3 deaths, 4 suicides, 5 betrayals,
  6 assists, 0 score -- named through the game-results statistic each adjust feeds (halo3+0x1da7e0 callers; table
  halo3+0x792c80, re-tools/h3results.py). It is replicated, so a joined client reads the server's counts. The kill feed pairs
  a killer's +1 with a victim's deaths +1 within 1.5 s (suicides +1 = "killed themselves"), the weapon is the killer's weapon
  in hand (or the vehicle it drives): marked estimated. The tracker's own "X died" only when the statborg can't be read.
  Connected list = halo3's network session as under the 0.9 client (sess = *(halo3+0x1e250d0); the app moves peer / player
  records by the same shifts it logs: `[capacity] session.peers_shift / players_shift`); slots = the session's maximum.
  Packets = pktlog.c's wire layer on interposed sendto / recvfrom / sendmsg / recvmsg / send / recv / connect / close /
  getaddrinfo (test: shim/tools/nhook-selftest.sh); halo3's message names (the game layer) are not in it yet. display_event
  (halo3+0x11b0ec) is hooked and its first 200 events are logged raw (`[nhook] [event]`): the app enlarged the event struct
  (0x2c -> 0x40 bytes), its layout is to be read from a real match. Switches: NHOOK_PKTLOG=0, NHOOK_NO_EVHOOK=1,
  NHOOK_EVENT_LOG=<n>.
- **nhook: overlay, recording, captures, fps in the native app** (2026-11-07; shim/native/nrender.m, noverlay.c, shim/src/rec.m)
  — WHY: the native app draws with Metal (its own D3D11 in Rust); the loader drew all of these in its GL present path.
  -[MTLCommandBuffer presentDrawable:] is swizzled on the GPU's command buffer class; on the app's own command buffer
  (before the original runs, so after the frame's drawing) we add: a capture (do=capture, &overlay=1 after the overlay ->
  ~/reclaimer_cap_<pid>_<n>.bmp), the overlay's lines in one render pass, the recording's frame (the drawable scaled into
  the recorder's IOSurface buffer). The overlay is hitbox.c's line builder ported (noverlay.c: hitboxes, vehicles + weak
  spots, info labels, items, tracers; no visible-only, no zones) with halo3's observer camera (TLS +0x578, result +0x11c:
  position, +0x28 forward, +0x34 up, +0x40 horizontal fov) and the frame's aspect -- checked against Halo 3's lowered
  reticle: the camera looks 4.9 deg above the level aim, which projects 77 px below centre at 720p, where the reticle is.
  rec.m: the GL frame path is #ifndef REC_NO_GL, and rec_video_begin / rec_video_end let another renderer fill the pixel
  buffers (the loader is unchanged). Sound: AudioUnitSetProperty interposed, every output render callback wrapped
  (rec_audio_push). The CAMetalLayer becomes readable (framebufferOnly NO) only once a capture / recording asks.
  fps: the presents' times -> snapshot "perf" (fps, frame-interval median / 95% / max) and a [nhook] [perf] line every
  10 s. Switches: NHOOK_NO_RENDER=1 (no swizzle), NHOOK_NO_AUDIO_TAP=1, NHOOK_OV_DEBUG=1 (calibration crosses on
  scenery / machines / crates).
- **nhook: "not in a match" mid-game, overlay buttons dead** (2026-11-07; nstats.c nst_on_line) — WHY: in a 52-player
  online game the page fell to "not in a match" and the overlay stopped drawing the first time the user opened the in-game
  menu: `[menus] open` (logged for the scoreboard / settings menu too) cleared the match state, and the overlay draws only
  in a match. Now only the app's game state ends a match: `[switch] game <anything but Playing>`, `[switch] leaving the
  game`, `[host] stopping`.
- **Packets window: player names and reverse DNS** (2026-11-07; nnet.c, stats_page.html) — the 0.10 app talks to every
  player directly (55 peers in that game) and logs each one's address every few seconds (`[online] peer 0x<id> at
  <ip>:<port>`); the session's peer records hold the same ids, so a wire record gets "player". The page's "look up names"
  box (on by default since 2026-11-07, the user's request) adds ?rdns=1: each address's PTR name, asked once per address on a resolver thread (getnameinfo NI_NAMEREQD; 2048
  cached). Test: shim/tools/nhook-selftest.sh (127.0.0.1 -> localhost). The loader ignores rdns=1 (not ported there).
- **nhook: "no stub page near ..." -> stats page empty** (2026-11-07; nhook.c near_page) — WHY: in the user's launch both
  entry hooks failed, so the page never had the engine thread's context (0 players, 0 objects). A hook branches (`b`,
  +-128 MB) to a stub on a page of its own next to the app's generated code (a 28.5 MB MAP_JIT region); near_page tried
  only 1 MB steps out from that region's edges, and the free gaps around it are 16-352 KB and scattered: some layouts
  had none on a probe. Now it walks the region boundaries within +-120 MB (mach_vm_region; nothing is read) and
  allocates in the free gap nearest the code.
- **nhook: empty snapshots, join / leave flood** (2026-11-07; nhook.c on_main_loop / tls_block, nstats.c st_roster) — WHY:
  in a 72-player game ~1 snapshot in 7 had no players: halo3+0x9d22c (the hooked clock) runs on other engine threads too,
  and the page read whichever thread's TLS block called it last; every empty read made the whole lobby "leave" and the
  next "rejoin" (~160 feed lines a second, real events pushed out of the ring). The context is now kept only for the
  thread whose block holds halo3's "players" data array (its TLS pointer, re-learned after 2 s of failures), and joins /
  leaves are debounced (left = missing 3 s, joined = not seen for 30 s, none in a map's first 5 s, an empty read ignored).
  The fps line shows only what the native app measures. shim/native.sh serves the page from the checkout.
- **nhook overlay off on an ultrawide screen** (2026-11-07; noverlay.c nov_camera) — WHY: the user's hitboxes were "quite a
  bit off and run parallel with the player" at 3440x1440. The observer's field of view (70 deg) is the width of a 16:9 view
  and wider screens see more at the sides (Hor+); the projection spread it over the whole width, so its vertical field was
  0.74x the game's and everything off-centre was drawn 1.34x too far out (measured 1.37x against the game's own name markers
  in a live capture). Now tan(v/2) = tan(fov/2) / (16/9), tan(h/2) = tan(v/2) * aspect; identical at 16:9 (the reticle check).
- **nhook: overlay off and "not in a match" after the server's map change** (2026-11-07; nstats.c) — WHY: "after the rejoin
  all the hitboxes were off ... the checkboxes weren't working": when the server moves the session to its next map the app
  logs `[loading] map ...` but stays in its Playing state (no new `[switch] game Playing`); the loading line ended the match
  and nothing started the next one, and the overlay draws only in a match. The app's Playing state is tracked
  (g_app_playing) and the next map's match starts at the first snapshot whose players table is filled.
- **nhook: zones, /forensics, player IPs** (2026-11-07; noverlay.c nz_*, nstats.c, stats_page.html) — zones are hitbox.c's
  loader-side reader (kill / safe trigger volumes, soft ceilings, invisible walls) drawn as outlines through walls (the
  loader's line mode: no depth buffer in the native app), with the page's per-kind chips (do=zonetype). /forensics = the
  loader's forensic.c included unchanged, with native definitions of what it takes (session layout 1, the engine image
  as "halo3", the command line's --join-server); it archives events, the roster and per-second packet sums (no voice in
  the native app yet). The players table gets an IP column (the session peer's address from the app's `[online] peer`
  lines, + reverse DNS); the Packets panel sits below the players table. shim/native.sh test runs archive into
  ReclaimerMac/native/test-forensics.
- **/forensics Packets: the player at each address** (2026-11-07; forensic_page.html, nnet.c, nstats.c) — the archive's
  packets.jsonl lines carry the native app's "player" (the session peer at that address) and "rdns" (reverse DNS, now
  always asked for the archive: g_pk_want_rdns is per thread, the forensic writer's on); the page shows both beside each
  peer (demo mode: player aliased, rdns dropped). shim/native.sh serves forensic_page.html from the checkout too.
- **Overlay team filters in Infection** (2026-11-07; noverlay.c nov_friend, nstats.c st_is_zombie) — WHY: "the hitboxes all
  / enemies / friends flags seem wrong": in Infection with teams off halo3 gives every player a team of its own (69
  distinct in a 69-player game), so "friends" matched nobody and "enemies" everybody. The sides are zombie / human, told
  by the first trait field the variant's infected (or initial infected) set changes to a value a human doesn't hold (a
  human's active value = the base set's, else Halo's default -- infected "recharge 100%" is the default and tells nothing;
  shields None vs Normal does), read from each player's active traits (+0x1FC) by the stats snapshot. Every team filter uses
  it (hitboxes, vehicles by their occupants, tracers by their shooter, info). Team games: the team index as before.
  (The loader's hitbox.c has the same blind spot: not changed there.)
- **Native tracers: bullets** (2026-11-07; nhook.c hook_install, noverlay.c on_object_delete) — WHY: "I don't think the
  tracers work": only projectiles alive between frames were drawn; bullets live less than a tick and the loader drew them
  from an object_delete hook, which the native port skipped because that entry is a single `b`. hook_install now follows
  an entry branch to the lifted wrapper it leads to; the handler (engine thread, its own TLS read directly, checked)
  records each deleted projectile's start / impact / owner, drawn start -> impact for 0.5 s as in hitbox.c.
  NHOOK_NO_DELHOOK=1 = off. LIVE RESULT (same day): object_delete's code-table entry is not in the JIT region but in the
  app's own signed __TEXT (0x104faeb88, a `b` to the app's native object_delete at 0x104c2e0ac): the app replaced the
  function, so neither the entry nor its target can be patched the JIT way (signed code; no free page near the image
  either) -- bullets are still not drawn. hook_install now hooks an entry that is a `b` in place (displaced = nop, return =
  the branch target) for JIT-region branches; near_page collects up to 4096 gaps and logs when none works.
  SOLVED by the text hook below; VERIFIED in a live engine 2026-11-07 (local Guardian test match, the app's own test request
  {"campaign":{"hold_trigger":{"seconds":6}}}): 10-20 bullets at object_delete, the player's shots counted, tracer lines
  and impact marks in overlay captures. Repeatable: `bash shim/tools/native-fire-test.sh` (PASS/FAIL).
- **nhook: hooks on the app's OWN code** (2026-11-07; nhook.c text_hook, stub.S _nh_tp4) — WHY: object_delete (and 916 more
  halo3 functions: NHOOK_LIST_BRANCH_ENTRIES=1 lists them) is the app's own native code in its signed __TEXT; the lifted
  entry is one `b` there. The app is signed ad hoc with no hardened runtime (codesign flags 0x2), and patching its text works:
  the page is made a writable private copy (mach_vm_protect VM_PROT_COPY), the function's first four instructions (refused if
  any is PC-relative) become `ldr x16, #8; br x16; .quad stub`, the page goes back to r-x; the stub (anywhere: absolute jumps)
  saves the caller-saved registers, calls the handler with x0, runs the four instructions and jumps back. Proven in a test
  game: halo3+0x2af478 ran ~700k calls/s through it, 0xfbe58 ~60/s, 120 fps held, no crash (NHOOK_TEXTHOOK_TEST=<rva,...>
  counts calls in the [perf] line). object_delete is now hooked this way (its handler reads x0's guest context through
  rd()); bullets need a real game to show (no deletes in an idle offline match; --keys scripted input did nothing in test
  windows). NHOOK_NO_TEXT_HOOKS=1 = none.
- **nhook overlay: the frame's own camera, and hidden behind the world** (2026-11-07; nhook.c on_view_param + stub.S _nh_tpf,
  nrender.m nr_scene_depth / nv_fs, noverlay.c) — WHY: "the zones kinda follow you weirdly, especially kill zones below you,
  and the safe zone scales weird as you walk": they were drawn through the ground (no depth) with the observer camera.
  (1) halo3+0x2af478 (set a shader parameter; the app's own code, ~700k calls/s) gets a text hook whose stub filters in
  assembly for parameter 0x27 = the view constants (the loader's VS cb0): element 0 = view-projection rows 0-3, 4/5/6
  forward/left/up, 7 the camera position (found with NHOOK_CBPROBE=1). The overlay takes the latest "world camera" (rows 0, 1,
  3 vanish at its position) within 1 unit of the observer -- the player's view, exact and interpolated (the observer's 70.0
  deg vs the frame's 70.8 deg); the observer stays the fallback (NHOOK_OBSERVER_CAMERA=1 forces it; NHOOK_NO_VIEWHOOK=1 = no
  hook). (2) Row 2 of the view constants is (0, 0, 0, 1/128): Halo 3 is reverse-Z with no far plane, scene depth =
  (1/128) / distance. The app's depth textures are seen at -[MTLRenderPassDepthAttachmentDescriptor setTexture:]
  (swizzled); the frame's scene depth = the full-size one attached most in the frame (Depth32Float_Stencil8, shader-readable;
  do=capture&depth=1 dumps them: centre 12.5 units, the gun at 1.6 cm, the sky 558 units). Each overlay vertex carries its own
  depth (1/128 / w), the fragment shader hides a pixel whose depth, moved 0.3 units toward the camera, is behind the scene
  there. Zones are always hidden behind the world (as in the loader); the other features by their "visible only" setting
  (page panel vis=, do=visible = all; NHOOK_HB_VISIBLE=1 starts them on). NHOOK_NO_OCCLUSION=1 / NHOOK_NO_DEPTH=1 = off.
- **nhook: weapon / vehicle / power-up spawns and pickups** (2026-11-07; shim/native/nspawns.c) — the loader's stats.c
  st_spawns + hm_spawn + st_sentence copied (events through the native st_event_verb, 16384 object slots, NHOOK_SPAWN_LOG, "joined
  in progress" from the app's `[menus] joining the game in progress`): the feed's spawn / pickup lines, the snapshot's "items" /
  "deployed", the heatmap's spawn points (heatmaps/<map>.spawns, shared with the loader). Checked offline: Guardian's 22 weapons,
  the Overshield and Active camo. shim/native.sh test runs keep their heatmaps in ReclaimerMac/native/test-heatmaps
  (NHOOK_HEATMAP_DIR); they used to add their idle footprints and spawn points to the real files.
- **nhook kill feed: the nearest killer, a longer wait** (2026-11-07; nstats.c sb_tick) — WHY: in a 70-player Infection game
  57 of 68 deaths came out as "X was killed" with no killer: big games replicate a killer's kill count and the victim's death
  count more than 1.5 s apart, and the old pairing took the first waiting killer. HOW: a victim waits up to 4 s (was 1.5 s) for
  a killer's count, a killer's count waits up to 5 s (was 2 s), and a victim takes the waiting killer NEAREST to where it
  died (SbKiller.pos = the killer's position at its count, SbNow.cur = each slot's current position); a teammate's betrayal
  still goes first. Old behaviour: none kept (the pairing is an estimate either way; events are marked estimated).
- **nhook: shots / hits / accuracy** (2026-11-07; noverlay.c on_object_delete, nstats.c st_snapshot) — WHY: the players
  table's Shots / Hits / Accuracy columns stayed 0 in the native app (the loader counts them in its object_delete hook,
  hitbox.c g_hbshot). HOW: the app's object_delete text hook (above) records every deleted projectile's owner (+0xC0) and the
  biped whose bounding sphere (+0x1c centre, +0x28 radius, + 0.1) holds its end point (an ESTIMATED hit, like the loader's) in an
  8192-entry ring (one writer: the engine thread); the stats snapshot counts them per player by unit handle. Only while the
  hook is placed (NHOOK_NO_TEXTHOOK off).
- **nhook: page mutes = the app's own mute list** (2026-11-07; nnet.c voice_init / muted_list, nstats.c st_control) — WHY: the
  Connected list's mute buttons and Mute all / Unmute all did nothing in the native app (snapshot "mute": false). HOW: the
  app's scoreboard MUTE VOICE button calls set_muted(peer id, bool) (0.10.0 app 0x102154ef0; is_muted 0x10214db98), which
  pushes / removes the id in a static Mutex<Vec<u64>> (0x103f0a1e0: lock box, poison, cap +0x10, ptr +0x18, len +0x20; found
  from the button's strings with re-tools/machoref.py). do=mute / unmute&voice=<hex> and muteall / unmuteall call it from the
  stats thread (it takes the list's own lock); the session's "muted" flags are read from the Vec. Checked before use: the
  function's adrp / add must reach the list (another app build: "set_muted not where 0.10.0 has it", page mutes off). Checked
  in a test match: mute, mute, unmute, unmute -> the app's list held 1, 2, 1, 0.
- **nhook: who talks (page + forensic archive) from the app's voice report** (2026-11-07; nstats.c st_voice_report,
  forensic.c fx_voice_report / FXF_REPORT) — WHY: the talking dot never lit and the forensic "who talked when" lane stayed
  empty in the native app (the loader stamps them in a hook on the client's voice_recv; the native voice code is the app's own
  Rust). HOW: the app logs "[voice] active ... ; heard 0x<peer id>: N frames (C concealed) rms X dBFS peak Y dBFS, ..." every
  ~2 s; each sender heard above -60 dBFS (TALK_DBFS: open microphones send -120 dBFS silence or -90..-112 dBFS hiss) counts as
  talking for that report (the page's dot: 2.5 s, was 0.5 s per packet) and becomes one voice.bin record with the new flag
  FXF_REPORT = 64 (olen = frames, no bytes, channel 0xFF "not known"): placed at the start of the ~2 s interval when the
  sender also talked in the report before (continuous talk = one utterance), else at its end. The viewer's utterances carry
  "reported" and read "(timing from the app's voice report)", no play / packets buttons. No audio: the native app's voice
  packets are not seen. NHOOK_VOICE_LOG=1 logs every parsed sender. Offline: nhook-selftest.sh (parser) and
  forensic-oggtest.sh (two reports in a row = one 3.2 s utterance). The log tap's line buffer for nst_on_line grew 600 -> 4096
  (a report names every sender).
- **nhook: forensic voice AUDIO from the app's Opus decode** (2026-11-07; nhook.c voice_hook_install, stub.S _nh_opus_shim,
  nstats.c nh_on_opus) — WHY: the native archive had only "who talked when" (the voice report above); the loader keeps every
  voice packet (voice_recv hook) and builds utterance / speaker .ogg files from them. HOW: the app decodes voice with opus-rs
  (pure Rust, statically linked; its packet parser is the function using "Input packet empty", 0.10.0 app 0x1003f1e7c:
  x0 decoder, x1 packet, x2 length, 0 = a lost frame). Its one caller (0x102152578) is the voice mixer, which walks a hash map
  of senders once per 20 ms: x26 = the sender entry's end, the peer id at x26 - 0x278 (read there for the volume table); the
  packet is [kind: 1 Opus, 0 raw 16-bit PCM][data] and decode gets the part after the kind. A text hook on decode's entry runs
  a 3-instruction shim (x3 = x26, x4 = the caller's return address from the stub's save area) -> nh_on_opus, which ignores
  calls from anywhere else, reads the id with rd and hands forensic.c a record in the loader's layout ([channel 0xFF = not
  known][sequence = our per-sender count][0x01][Opus]) so voice.bin, the viewer and its .ogg files work unchanged (audio only
  with the archive's voice audio + one-party consent setting, as in the loader). While packets come, the voice reports write
  no FXF_REPORT records. Every patched / relied-on word is checked first (decode's 4-instruction prologue, the mixer's id load
  and its argument moves + bl): another app build gets "[nhook] voice packets: not hooked". NHOOK_NO_VOICEHOOK=1 = off.
  The [perf] line counts "voice packets". Installed and checked in local test matches (no remote talkers there: the packets
  themselves still need a real game).
- **Stats page: no dead controls in the native app** (2026-11-07; stats_page.html, the overlay-state update) — WHY: in the
  native app the Tools row still showed Teleport + its spot list, Map vote / Called votes, the map's "click to teleport" box
  and the Packets window's "halo3 message types": each answered {"ok":false} (nhook has no teleport / boundary patches,
  auto-vote or halo3 message table). HOW: when the snapshot says "native": true those are hidden; the loader's page is
  unchanged (it never sends "native"). Checked against a native test match: the five hidden, Capture frame / Mute all still
  there, no console errors.
- **nhook: the address role is real** (2026-11-07; nstats.c st_snapshot, nst_on_line) — WHY: the native snapshot always said
  addr_role "hidden", so a player's detail panel without an IP read "hidden (this page is reachable from other computers;
  PELOAD_STATS_IPS=1 shows it)" -- the native page listens on 127.0.0.1 only and has no such switch. HOW: "host" when the local
  session peer is the host, else "client" (in a session, or after a join), else "none"; "server_addr" = the address in the
  app's `[online] joining <ip>:<port>` line. Checked: a local test match says "host".
- **nhook: exact kills from halo3's game results** (2026-11-07; nstats.c rs_tick, sb_consume, results layout from the [c1] /
  [capacity] results.* lines) — WHY: the native kill feed was estimated (killer paired with victim from the scoreboard's
  counts, weapon = the killer's weapon in hand): display_event and chud_messaging_post never run in the app (a census across a
  death: halo3+0xfd418, the event handler that calls display_event, and 0x2d3f2c ran 0 times -- the app handles events in its
  own code; the old "display_event" entry hook is on a lifted function nothing calls; display_event is now text-hooked when
  its entry is a `b`, which it is not in 0.10.0). HOW: halo3's game results, which the app keeps live: per player 0x263 u16
  statistics at results + 0x6c98 + slot * 0x4c6 (9 kills, 11 deaths, 12 betrayals, 13 suicides; per damage type t from 234 +
  6t: damage-kills, -deaths, -betrayals, -suicides, -headshots -- names from halo3+0x792c80 / 0x793160, re-tools/h3results.py)
  and a killer x victim matrix at + 0x2cf98 (128 x 128 x 4 bytes, two u16 counts). Each snapshot: a matrix entry that went up
  names the pair; which side killed = whose kills / betrayals went up while the other's deaths did (else the orientation the
  matrix showed before), the killer's damage-kills that went up = the weapon, damage-headshots = a headshot; suicides from 13 +
  damage-suicides. Exact events carry no "estimated" flag; the scoreboard's pending entries they explain are dropped (it waits
  1.5 s while results count deaths), a death the scoreboard already reported is not repeated. A client that never gets the
  results keeps the scoreboard pairing. Checked live: a suicide (11, 13, 235, 237 and versus[0][0] 0 -> 0x00010001) -> one
  exact "killed themselves" each time; pairs offline: `bash shim/tools/results-feed-test.sh` (the real code, extracted: kill,
  either matrix orientation, double kill, a trade in one snapshot, betrayal, suicide, headshot, pending-entry handling, new
  match) -- kills between two players still need a real game (NHOOK_RESULTS_LOG=1 logs every change and decision).
- **nhook kill feed: history is not a kill (joining a game in progress)** (2026-11-07; nstats.c rs_tick, rs_cand,
  rs_confirm, sb_tick) — WHY: joining a 52-player public game in progress, the feed replayed ~200 of the match's old kills in
  its first seconds and showed some live kills twice; a later mid-game join replayed ~60 more through the scoreboard path. A
  client gets halo3's game results and the statborg in pieces after a join: results rows went 0 -> N over minutes (a slot's
  statistics long after its versus row, so the mirror half of a versus entry still found that killer's kills going up and
  printed the kill again), and the statborg came in one burst 0.8 s after loading in. HOW: (a) results: only the LOW half of
  each versus entry is read ([a][b] low = a killed b; [b][a] high is the same kill -- checked live on one kill: [12][49] low +1,
  [49][12] high +1), so each kill counts once; the kills / deaths deltas still decide the orientation where they tell. A
  change becomes a candidate (rs_cand) that is reported only once the scoreboard shows that victim's death (a pending
  SbVictim; rs_confirm, up to RS_WAIT 3 s either way) -- else it is history and dropped; a pair or a slot's suicides up by more
  than 1 in one snapshot, or more than RS_FLOOD (16) changes in one snapshot, is history outright. (b) the scoreboard (sb_tick):
  a slot's deaths up by more than 1 (kills by more than 6) in one snapshot, or more than SB_BURST (8) players dying in one
  snapshot, is the statborg filling in (taken as it is, no events); a single death waits up to 3 s for the death tracker to see
  that player's body die (or no live body) -- the statborg can come 0.1 s before the body (8 of ~250 deaths in one game) -- and
  a player who stays alive is history. g_rs_live stays set once the results have counted a death, so the next match's first
  deaths wait for them too. Checked live on a 35-50 player public server (Guardian, The Pit, Valhalla): mid-game joins -> first
  kill 2 s after loading in, ~250 kills and none replayed, every repeated kill line >= 11 s apart; a match joined at its start:
  249 kill lines for 249 scoreboard deaths. Offline: `bash shim/tools/results-feed-test.sh` (now with results before the
  scoreboard, a change the scoreboard never backs, a table filling in, a pair up by 2, scoreboard bursts, a player alive all
  along, the count before the body dies). NHOOK_RESULTS_LOG=1 logs every decision; NHOOK_NO_RESULTS_FEED=1 = the scoreboard
  pairing only.
- **nhook: chat in the feed and the forensic archive** (2026-11-07; nstats.c chat_log_tick + the "[chat] line" parser) — WHY:
  "Chat ... aren't showing on the timeline": a busy public game archived 0 chat lines. The app prints its chat lines
  (`[chat] line [GLOBAL|TEAM|NEARBY] <name> (0x<id>): <text>`, `[SERVER #rrggbb]`, `[SERVER]`, `[NOTICE]`) only while the byte
  at 0x103efcc9b is set: the dedicated server's `chat_log` option, a field of the chat state at 0x103efcc58 that a client never
  sets (`re-tools/machoref.py dataref`, new: it is read in two places, both this gate in the chat function 0x100d23d78). HOW:
  chat_log_tick (every snapshot) checks the gate's instructions (adrp x8 / ldrb w8, [x8, #0xc9b] at 0x100d2a6fc and
  0x100d2a720: another build is left alone with a log line) and sets the byte, again whenever it reads 0; lines that came while
  it was off are printed when it turns on (the app logs from its own last-logged index). The parser now records chat the way
  the loader's stats.c does: kind "chat", a = the sender, how = "" (everyone) / team / nearby / server / notice, text = the
  message, the sender id for the archive's sender_id ([SERVER] lines were kind "server" before, outside the Chat lane).
  NHOOK_NO_CHAT_LOG=1 = off. Checked live: the [SERVER] welcome and GLOBAL lines in /feed.json and events.jsonl (channel "all",
  sender ids).
- **Forensic page: the native voice blocks show, and a usable player** (2026-11-07; forensic_page.html) — WHY: "it leaves a
  play button in the bottom left but it isn't super useful ... the audio packets ... aren't showing on the timeline": native
  voice records carry channel 255 (not known) and the page coloured only channels 1-3, so every native utterance block was
  drawn without a background (33 invisible blocks in one session). HOW: a colour and a legend entry for "channel not known";
  the player bar is hidden until something plays, then has previous / next (in time, among what the filters show), "play on"
  (the next utterance when one ends: listening through a session), who / channel / time / "n of N" and a close button; a play
  head line runs across the timeline and the playing block is scrolled into view; each speaker gets a play button (their
  whole .ogg). Checked on a live session: the blocks show, a click plays, play on went 12 -> 18 of 33, close stops it.
- **nhook: aim vs hits, per player** (2026-11-07; noverlay.c nov_aim_shot / nov_aim_tick, nstats.c st_aim_tick / st_aim_json,
  stats_page.html "Aim" column, nrender.m calls nov_aim_tick every frame) — WHY: the user: "is there a way to determine if
  where the player is aiming is in sync with what is getting hit? ... a meter that tells how close to an accurate shot in a
  hitbox is being made ... a high deviation would indicate a trash player, or that lag was too high between players to land
  hits". HOW: (1) at each bullet's deletion (the object_delete text hook, engine thread) a straight shot (travelled <= 1.05 x
  its start-to-impact line + 0.3: no grenades or bounces) from a biped picks the enemy whose bounding-sphere centre lies
  nearest its path (within 12 degrees, in front of where it stopped, alive, on foot; teams / Infection sides as the overlay's
  filters), and copies that biped's pose (node matrices, region permutations) and shield + health into a 64-record seqlock
  ring. (2) the render thread (it owns h3maps.c's hitbox cache) poses the target's collision edges as the overlay does and,
  per (region, node), takes the centre and mean radius of the end points: the miss = the path's distance to the nearest such
  piece minus its radius, 0 = through a hitbox; the error = atan(miss / distance) in degrees. (3) the stats snapshot keeps the
  last 64 errors per player (median and 75th percentile shown), the share through a hitbox, and REGISTERED = the target's
  shield + health went down (or it died) within 0.6 s -- separately for on-target shots and visible misses. Measured as THIS
  client saw the target: a remote shooter's target sits where this client draws it; in a crowd other players' damage on the
  same target counts too (the page says so). The [perf] line counts "aimed / measured". NHOOK_NO_AIM=1 = off. Checked live on
  an 80-player Hugegrass server: 1,258 shots measured in 2 minutes; one player's 196 on-target shots were all registered and
  none of their 360 misses, others 33-76% of on-target shots registered.
- **nhook: the hook cost meter** (2026-11-07; shim/native/nhcost.h, nstats.c hc_json / hc_frame / hc_control, every hook's
  wrapper, nrender.m, stats_page.html "Hook cost" panel) — WHY: the user: "a *hook cost* meter for FPS when a hook is enabled
  it should calculate any performance loss that occurs as a factor of it being enabled"; and "a disabled hook should not cost
  anything". HOW: (1) self-timing: each hook's handler (bullets / object_delete, the aim meter's part of it, the camera hook's
  C handler, the voice decode hook, the socket hook, the stats snapshot) and each overlay part (objects + camera, hitboxes,
  vehicles, tracers, info, items, zones; the aim meter's render-thread measurement) adds its time from the CPU counter
  CNTVCT_EL0 (24 MHz, ~1 ns to read) and its calls to a slot; the snapshot gives per hook calls/s, us per call, ms per frame,
  share of the median frame and the fps it would give back IF its thread sets the frame rate (1000/(frame - its ms) -
  1000/frame). (2) the real cost: POST /control?do=hookab&h=<id>[&secs=36][&per=3] switches only that hook's handler
  (g_hc_off) or overlay feature (its g_nov_on bit; "overlay" = all of yours) off and on in turns, starting on; hc_frame,
  called from every presented frame, counts frames per phase after a 0.5 s settle; the result is the median of the on/off
  pair differences with their spread, and the state is put back (do=hookab&stop=1 ends a run; a run without frames is
  dropped after its length + 10 s). The camera hook's assembly filter (~700k calls a second) is neither timed nor switched
  off. Checked live on a 51-player Hugegrass server (22 fps, limited by the game's simulation): self-timed hitboxes 0.22 ms
  a frame, vehicles 1.3 ms, bullets 0.04 ms, the snapshot 2.3 ms per run on its own thread; measured on/off: overlay,
  hitboxes, vehicles, bullets and stats all within noise (spreads of 1-3 fps from the scene itself).
- **Stats page: an "Aimbot?" column** (2026-11-07; nstats.c st_aim_tick / st_aim_json open_* far_*, stats_page.html aimFlags)
  — WHY: the user asked for a column flagging possible aimbots (first idea: unregistered misses -- but a miss that does not
  register is a normal miss, so that number is not the signal). HOW: per player, from the aim meter's shots: the share
  through a hitbox, OPENING shots on target (the first aimed shot after 1 s without one: a person usually needs a correction),
  long-range shots (12+ units) and the head share (the topmost hitbox region). The page flags a player with 40+ aimed shots
  when at least two of {through-hitbox share, opening shots, head share} are above a floor (50 / 60 / 60%) and, with 5+
  comparable players, above 90% of the lobby; long range only shows in the reasons (on a big map nearly every shot is long
  range: it repeated the first signal). "⚠ suspect" with the reasons; "–" = nothing unusual. Not proof: one client's view,
  modded characters' "head" is their topmost hitbox (the lobby comparison cancels it when everyone is high). Checked: no
  flag in a live 51-player lobby; a made-up lock-on profile against that lobby is flagged, a normal one is not.
- **nhook: the stats snapshot can no longer slow the game** (2026-11-07; nhook.c rd / rd_sys / nh_thread / tls_block memo,
  nstats.c st_ocache_fill / oc_add / st_statborg memo, nnet.c, forensic.c) — WHY: the user: "does the stats snapshot lag our
  view? we don't want it to ever do that". Three ways it could: (1) it ran at normal priority, competing for the performance
  cores; (2) every read of game memory is a system call (mach_vm_read_overwrite) and a snapshot made 4,032 of them (profiled
  with NHOOK_RD_PROF=1: one per object in the biped / spawn loops, every object's type in the map check, tls_block's 5 reads
  per player, the scoreboard per player, the session per member); (3) a read above ~32 KB (the 156 KB game-results table)
  makes the kernel copy by marking the SOURCE pages copy-on-write -- the game's own pages -- so the game's next write to each
  one faults on the game's thread. HOW: (1) our background threads start at QOS_CLASS_UTILITY (snapshot, HTTP) or
  BACKGROUND (forensic writer, reverse DNS, reaper): the efficiency cores on Apple silicon. (2) before each run the snapshot
  copies, in 16 KB pieces, the engine's object memory (every object's data: one 3-6 MB block), the players array and the
  network session's player / peer tables; rd answers the snapshot thread's reads inside those copies from memory, and the
  engine TLS block and the scoreboard are looked up once per run: 4,032 system calls a run -> 348, 2.3 -> 1.4 ms. (3) rd
  never reads more than 16 KB per system call (rd_sys). The hook cost measurement now also reports the slowest frames (95th
  percentile and max frame time per phase). Checked live: two 60 s on/off runs of the snapshot in a 36-player Guardian game
  (~190 fps): p95 frame 13.5 / 13.2 and 12.0 / 11.8 ms, max 35.1 / 35.1 and 74.8 / 69.5 ms (on / off) -- no difference
  beyond noise. NHOOK_NO_OCACHE=1 = the old per-object reads.
- **Stats page: say why an address has no name** (2026-11-07; nnet.c pk_rdns / pk_peer_json, stats_page.html) — WHY: the user
  saw no reverse-DNS names on the main page in one game while the forensic page showed names: that game's server
  (207.102.103.168) simply has no PTR record, and the forensic sessions shown were from a server that has one; a client of a
  dedicated server never knows a player's address (it talks only to the server). HOW: a finished lookup without a name now
  says "no DNS name" in the Packets window (rdns_none); the IP column's title explains when players have an address.
- **Stats page: the Aim and Aimbot? cells say what they mean** (2026-11-07; stats_page.html aimCell / botCell) — WHY: the
  user: "'-' in aimbot field was unclear ... some say 100% next to them and I don't know why". HOW: Aim reads "1.2° off" and,
  under it, "N% of M hits counted" (of the shots through a hitbox, the share the server confirmed); Aimbot? reads "normal"
  once judged, "k/40 shots" before, "⚠ suspect" with reasons.
- **nhook: off means off** (2026-11-07; nhook.c on_view_param / nov_view_hook_want, noverlay.c nov_build_body / nov_idle /
  on_object_delete_body / nov_collect, nstats.c hc_set, stats_page.html Hook cost switches) — WHY: the user: "a disabled
  hook should not cost anything". HOW: the camera hook is no longer installed at launch: the render thread installs it the
  first time an overlay feature is on, and its handler returns at once while the overlay is off (its assembly filter stays
  once installed: a few instructions per call); the bullet hook's body-sphere pass runs only without the aim meter (hits come
  from the aim meter now, so one pass over the objects) and its tracer record only while tracers are on; the overlay reads
  players only for a feature that draws or filters by them (hitboxes, info, a vehicles / tracers team filter); the Hook cost
  panel switches any hook's handler off and on (POST do=hookset&h=<id>&on=0|1; overlay features keep their own buttons, the
  snapshot is not switchable). Checked: native-fire-test.sh PASS with the overlay on from the launch (the camera hook installs
  at the first frame).
- **nhook kill feed: a fresh launch's first kills wait for the exact results** (2026-11-07; nstats.c rs_tick g_rs_live,
  st_nice_case) — WHY: the first kills after a launch came from the scoreboard path (estimated, the weapon in capitals: "ASSAULT
  RIFLE") because it only waited for the results once they had counted a death. HOW: it waits whenever the results can be
  read; the scoreboard path's weapon names are in normal case ("Assault rifle").
- **nhook: hits = shots through an enemy's hitboxes** (2026-11-07; nstats.c st_aim_tick / StTrack.hits, stats_page.html) — WHY:
  a hit was "the bullet ended inside a body's bounding sphere", an estimate a practice target's own counter disagreed with
  (2 of 10). HOW: hits come from the aim meter's hitbox test (the path through an enemy's posed hitboxes, as this client saw
  it); NHOOK_NO_AIM=1 = the old estimate. Checked: shim/tools/native-aim-test.sh (new: a target 5 units ahead, the trigger held
  at it) -> 10 shots, 8 hits, all 10 measured.
- **nhook: medals in the feed and the player details** (2026-11-07; nstats.c rs_medals / g_medal, stats_page.html) — WHY: the
  native feed had no medals. HOW: the game results count each player's medals at statistic 63 + i for medal i in the order of
  the app's own medal table (0.10.0 0x103ed23c8: 112 entries of id, name, description, 48 bytes each; found from the app's
  stats names with re-tools/machoref.py dataref). Checked against live counts: melee kills -> 106 Beat Down, a first-kill sniper
  headshot -> 104 Sniper Kill, 140 First Strike, 147 Snapshot, a player's 149 Assist = their 17 assists. The feed says
  "<player>: <medal>" (kind "medal") for every medal except Kill / Assist (one per kill / assist) and Unused; a kill medal
  needs that player's kill on the scoreboard within 3 s (g_sb_kill_t), a player whose medal counts rose by more than 3 in one
  snapshot is the table filling in (both: history after a join). The player details list this match's medals. Offline:
  results-feed-test.sh (a backed Double Kill, an unbacked Sniper Kill dropped, an objective medal, 5 at once, Kill / Assist).
- **nhook overlay: no hitboxes on dead bodies** (2026-11-07; noverlay.c nov_build_body / nov_hitbox) — WHY: a body lying on
  the ground drew as red hitboxes: clutter. HOW: dead bipeds get no hitboxes; NHOOK_HB_DEAD=1 draws them grey.
- **nhook: the host tools on the native app** (2026-11-07; new shim/native/nhost.c, nstats.c st_control / snapshot overlay,
  stats_page.html) — WHY: the user asked to port the loader's host tools (teleport next to a player / to a point / by a
  delta / to the map's spots, boundaries off, invisible walls off, speed). HOW: the page's controls (do=tpnext, tpto, tpby,
  explore, bounds, invwalls, speed; host only: halo3's game_globals +0x11 is 5 or 1) queue one request; it runs on the
  engine thread after halo3's game tick runner (halo3+0xeefa8, a body hook; a dedicated server runs only that) or, as a
  fallback, at the entry of the app's per-frame function (0.10.0 0x1029ed000, a text hook; first words checked), inside
  halo3's game-state lock (0x12c948 / 0x12c9a0, recursion-aware). Teleport = halo3's own (0x259410(object, pos, forward, up,
  1); unit_exit_vehicle 0x3a3c4c first, scenario_location_from_point 0x9bc78 for "inside the structure" at +0.3 / +1.2, as
  the loader). halo3's network layer is not there natively (its simulation entity table, TLS block +0x240, is NULL; the
  app has its own netcode) but game_globals +0x11 still says host, so halo3's "mark for a network update" (0x1ec3c, called
  by teleport and disconnect-from-map) read that NULL table: signal 11 at 0x48. For the call halo3 is told it runs a local
  game (1), host (5) comes back right after (nhost.c ht_call; NHOOK_HOST_KEEPMODE=1 = no switch = the crash). Boundaries =
  text hooks on the app's own kill-volume test (the code-table branch of 0x1b8e00: answered "not in a kill volume, in a safe
  zone") and player soft-ceiling test (0x1bac00: none); invisible walls = a body hook after halo3's Havok polygon-shape
  builder (0x21b4d8) moving player-clip surfaces' vertices to z = -20000 (the loader's way); speed = the variant's base
  player speed trait (game_globals +0x1608 +0x134 +0x10). Requests take &slot=<player> (a dedicated server has no player
  of its own; without one it says so). Checked live: a hosted Guardian match (teleport by 2 units stays; boundaries off ->
  fell through the kill volume alive, back on -> dead; speed 200% walks 4.95 units/s vs 2.58 at 100% with the app's
  walk_to test request; 45 shapes built while walking, no invisible surfaces near Guardian's spawn) and our native
  dedicated server with a joined test client (the server moved the player 3 units, the client saw itself there; kill
  volume alive with boundaries off, dead once on). tpnext (next to another player) needs two players: not run live. Since checked
  live with two clients and invisible walls fixed (the next two entries).
- **nhook invisible walls off: the right surface index** (2026-11-07; nhost.c ht_shape) — WHY: with the switch off the
  player still stopped at a player-clip wall and the snapshot's walls_hidden stayed 0: the handler took the builder's 8th
  argument as the surface index. It is the 9th (`movsxd r14, [rbp+0x70]` in the x86 = entry rsp + 0x48), which the builder
  also stores at shape +0x28. HOW: read shape +0x28 after the original ran. Checked live with the new
  shim/tools/native-host-test.sh walls (a local Guardian match; the loader's 2026-10-18 test wall x -13.98..-8.10, y -19.35,
  z 21.31..32.67; the player put 1.8 units north of it, the app's walk_to test request walks south): walls on -> stopped at
  y -19.15 (the loader: -19.16), walls off -> walked through to -21.28 and fell down the drop the wall guards (4 surfaces
  hidden). No old-behaviour switch (the old index never found a wall).
- **nhook host tools: teleport next to a player, checked with two clients** (2026-11-07; shim/tools/native-host-test.sh
  tpnext, shim/native.sh) — our native dedicated server plus two test clients: the usual test profile and a second one with
  its OWN identity (ReclaimerMac/native/test-second: the first one's settings and first-run markers, no profile.json; the
  app made a new identity with a random name on its first start, the test renames it "Second"; services left alone like
  every test profile). do=tpnext&slot=<B>&who=<A> on the server's page: "moved player 0 next to player 1 (right)", 1 unit
  apart on the server, and A's own client and B's client showed A at the same place. The teleport sets the unit's facing,
  but a remote client keeps aiming where its own input says (the app's aim_at_nearest_player test request turned it).
  shim/native.sh: NATIVE_ALLOW_SECOND=1 no longer waits 30 s for the running instance (the server) to end first.
- **nhook kill feed: a medal follows its kill** (2026-11-07; nstats.c rs_tick, rs_medals) — WHY: the server's feed said
  "<player>: First Strike" just before "<player> killed <victim>", the kill it was for (same second, lower sequence).
  HOW: the kill candidates are confirmed (rs_confirm) before the medals are looked at, and a kill medal waits while a kill
  of that player still waits for the scoreboard's death (within its usual 3 s). Checked: results-feed-test.sh (a new case:
  a kill and its medal before the scoreboard's death -> both wait, then the kill line and the medal after it; fails on the
  old order) and live: the kill line, then First Strike, on the server's page and both clients'. No old-behaviour switch
  (an order only).
- **nhook: every game of a session is a match** (2026-11-07; nstats.c nst_on_line) — WHY: when our server started its next
  game on the same map, the page saw nothing: "Game over" three times, "Match started" once, and the per-match state ran
  on. The app logs no "[switch] game ..." line then, only its pregame lobby: "[shell] pregame lobby: waiting" after the game
  over and "... left" when the countdown ends (a client that joins a game in progress logs "game Playing" first and the
  lobby after; the server's engine logs the same lines). HOW: while the app is in a game, "waiting" ends the match and
  "left" starts the next one ("Match started"). Checked live: client B's feed: Game over, then Match started: Guardian
  again. Found with the new NHOOK_TAP_LOG=<file> (every line the hooks read, from every process that loads the dylib:
  what a server's worker logs, which nobody shows). No old-behaviour switch.
- **The map being played follows the engine's scenario at once** (2026-11-07; shim/src/h3maps.c hb_current_map: the loader
  and nhook) — WHY: a native client's first spawn line of a Guardian match was "36 weapons and equipment spawned: Phd
  bottompiece x3, Hard metal thin hum masterchief x3, ... Dervish head, Elite wipe" (the server's: "Gun lip garbage x3,
  Flood softform break small ..."), the right "22 weapons" line came right after: for a match's first seconds the map being
  played was still the loading screen's scenery (130_epilogue, a campaign cinematic) until the 2 s recheck, so the
  tracker classified and named Guardian's objects from the epilogue's tag table, then started over when the map changed.
  HOW: the engine's scenario name (game options, hb_scenario_name) is polled every 0.1 s and a change re-decides at once.
  Checked live: one "22 weapons and equipment spawned" line with Guardian's names on all three pages. Old:
  PELOAD_MAP_SLOWCHECK=1 (the 2 s check only).
- **nhook spawns: a dead player's dropped weapon is no spawn; a client's first weapons are one line** (2026-11-07;
  nspawns.c st_spawns) — WHY: on both test clients "Assault rifle spawned at (-3.0, -24.0)" followed every kill, at the
  victim's spot (a client of the native app gets the host's dropped weapon as a NEW object; the host keeps its object, so
  the server's page had no such line). And a client's match start came out as "Excavator spawned at ...", "Smg spawned at
  ...", ... then "17 weapons and equipment spawned": the host's objects reach a client over ~1 s of snapshots. HOW: a weapon
  appearing within 2 units of where a player died in the last 4 s is that drop: no event, no spawn point (Old:
  NHOOK_SPAWN_DROPS=1); from a match's first weapon spawn on they are gathered until 0.5 s pass without a new one (3 s at
  most) and go out together (Old: NHOOK_SPAWN_NOGATHER=1). shim/tools/spawn-test.sh compiles again (the loader's code had
  gained the heatmap lock and spawn points: stubs) and now runs the same fake game against the native copy too, with a drop
  case (fails without the rule).
- **nhook on a dedicated server: the probe waits for the engine** (2026-11-07; nhook.c probe_thread) — WHY: one start of
  our native server had no stats page at all: its engine process probed 2 s after the "image at" line, found the image
  half-filled ("image+0: no MZ", part of .text as in the file) and the runtime's code table empty ("code table at 0x0") ->
  "no code table: hooks off". HOW: after the 2 s it waits for the image's MZ header and a filled code table, up to 60 s
  (logged when it had to wait); the probe line names its pid. No old-behaviour switch.
- **nhook: who left, when they left** (2026-11-07; nstats.c st_roster, st_roster_notice, nst_on_line) — WHY: a player whose
  leaving ended the game (our server: the other player won) got "<player> left the game" only 8 s into the NEXT match, after
  "Game over", or never: halo3 keeps a player who quit in its players table until the game ends, and the roster started
  over with every map name (between games the map passes through the loading scenery). HOW: the app's own HUD notices
  "[engine] HUD notice: <name> joined the server" / "... left the server" (client and server, the moment it happens) report
  joins and leaves; a name they covered is left to them (the leaver still in the table is no new join). The players-table
  path stays for the rest, and its roster now starts over only when the app joins ANOTHER server: on the same server it goes
  on across maps (who does not come back once a map's first 5 s are over has left). Old: NHOOK_ROSTER_NOHUD=1 (no notices),
  NHOOK_ROSTER_PERMAP=1 (a new roster per map). A notice for "A player" (the app's word before it knows the name) is left to the table path. Checked live (two clients on
  our server, A quits mid-game): "<A> left the game" on B's page and the server's, before "Game over: Second won" (~20 s after
  the quit: the server's own disconnect timeout).
- **nhook teleport next to a player: a spot with a floor** (2026-11-07; nhost.c ht_floor, ht_do; do=floorprobe) — WHY: "right
  of" a player at a ledge was open air: a player put there by tpnext fell into Guardian's kill volume (three times in a row on
  our server). HOW: each candidate (right, left, behind, close behind) must also have a floor: halo3's collision ray
  (0x1fd748, called as halo3's own "put it on the ground below" 0x259c7c calls it: flags = the default at halo3+0x20ad508 |
  0x24000, vector (0, 0, -1.8) from 0.3 above the spot, no ignored objects; result +0x08 = the hit point) hits within 1.5
  below it; the first such spot wins, else the first open one as before. ("Solid just below" was tried first and is no test:
  open floors and thin walkways have open space under them.) Checked live with do=floorprobe&x=&y=&z= (host only; logs the
  ray and the open / solid test at 9 depths): no floor at the 3 known drops (the spot players fell from, the walls test's
  far side), floors at 16.55-21.40 everywhere players stood; shim/tools/native-host-test.sh tpnext puts B at that ledge
  and A must stand beside it 4 s later: PASS (A at z 17.44, alive). Old: NHOOK_HOST_NOFLOOR=1.
- **nhook forensic voice: the channel** (2026-11-07; nstats.c nh_on_opus_body, st_vch, snapshot session "voice_ch",
  stats_page.html; backlog N6) — WHY: the native archive wrote channel 0xFF ("not known") for every voice packet; the loader
  read the channel byte of the packet itself, which the native app's packets don't carry. HOW: the voice mixer (0x102152578)
  scales each sender by the channel's volume: a byte at the sender entry's end - 7 indexes the 3 floats at app 0x103f0a1d0,
  which 0x10214c0b4 stores from the settings' voice_global / voice_team / voice_proximity volumes in that order -- so the byte
  is the sender's channel, 1 global, 2 team, 3 proximity (the loader's numbering, forensic.c fx_chname). The archive's records
  carry it; the page's Connected list says "talking (team voice)"; the log names each sender's channel once (and on a change).
  Checked live with the new
  shim/tools/native-voice-test.sh (two test clients; A talks from a WAV with the app's --voice-wav / --voice-talk): --voice-talk
  all -> B: "talks on channel global"; nearby (the server put A beside B first) -> "proximity", and B's page voice_ch
  "proximity". Old: NHOOK_VOICE_NOCHAN=1 (always 0xFF).
- **nhook.dylib is linked to a temporary file and renamed into place** (2026-11-07; shim/native/build.sh) — WHY: a rebuild
  while a game runs must not change the file that game mapped (a file rewritten in place under a running process can get it
  killed for an invalid code signature when its pages are read again). HOW: link to build/nhook.dylib.new, mv over the old
  one (a new inode; running processes keep theirs).
- **nhook: hooks on a lifted function's body** (2026-11-07; nhook.c body_of / body_hook_after, stub.S _nh_ba) — WHY: entry
  hooks on the code table see only calls through the table; lifted halo3 code calls lifted halo3 code DIRECTLY at the
  body its wrapper calls, so the shape-builder hook saw 0 calls in a live test. HOW: the wrapper (stp / mov / stp, ldr
  w,[x0,#0x290], ...) passes x2 = the guest context, x3 = guest rsp, x4..x7 = rcx, rdx, r8, r9 to `bl body` and stores x2
  as rax; body_of finds that `bl`, checks the body starts with `stp x29, x30, [sp, #-16]!`, and one atomic `b <stub>`
  replaces it (MAP_JIT, pthread_jit_write_protect_np); the stub runs the original first, then the handler with the
  incoming registers, stack arguments 5-8 and the result, and returns every caller-saved register as the original left it.
- **nhook: text hooks stop the other threads while a page is patched** (2026-11-07; nhook.c text_hook_x, nh_world_stop /
  nh_world_go) — WHY: text hooks used to go in only at startup; the camera hook and the host tools now install while the
  game runs, and a 16 KB code page is not executable while it is written: a thread running anywhere in it faulted (signal
  10 at an address of the app's frame function). HOW: every other thread is suspended for those microseconds (task_threads
  + thread_suspend; no lock taken in between); a thread stopped inside the four patched instructions makes it resume and
  try again (up to 50 times). NHOOK_TEXT_NOSTOP=1 = old.
- **nhook: crash frames to halo3 functions** (2026-11-07; nhook.c NHOOK_CODETAB_DUMP / NHOOK_DUMP_ENTRY, new
  shim/tools/native-frames.py) — WHY: the app's crash report gives host addresses in its JIT region only. HOW:
  NHOOK_CODETAB_DUMP=<file> writes the code table ("halo3 rva, host entry"); native-frames.py <file> <log|addresses> names
  each frame halo3+<rva> + offset. The JIT layout is the same in every launch, only shifted: another process's addresses map
  with one known anchor (e.g. its [nhook] hook line). NHOOK_DUMP_ENTRY=<rva>[:<words>] logs a lifted function's first words.
- **nhook Packets: the dedicated server says "server"** (2026-11-07; nnet.c st_session g_st_host_id, pk_player) — WHY: the
  user wants the DNS name and the player's name by an address where they apply; on a dedicated server the one address is the
  server, which is no player, so it had nothing. HOW: the session's host peer id is kept; its address, when no player owns it,
  is labelled "server".
- **Forensic archive: an always-open microphone is split into what was said** (2026-11-07; forensic.c FXF_LEVEL /
  fx_voice_level / fx_segment, nstats.c st_voice_report) — WHY: a player whose microphone never closes sent packets of
  background noise the whole game: one 3.5-minute utterance on the timeline. Packet sizes could not tell (that noise came in
  normal-sized Opus packets, median 40 bytes vs 45-55 for speech). HOW: every app voice report ("heard 0x<id>: N frames ...
  rms X dBFS", ~2 s) is archived per sender as a FXF_LEVEL record (window start, length, rms x 100); the segmenter leaves a
  sender's packets inside windows quieter than -60 dBFS (the talking threshold) out of every utterance. Offline:
  forensic-oggtest.sh (6 s of packets with a quiet 2 s in the middle -> two utterances of 100 packets). The voice CHANNEL
  (global / team / proximity) is still not known natively: the app's voice mixer keeps per sender only a sequence-keyed queue
  of raw packets ([kind][data]; 0x10214dcbc pops it), so the channel would have to come from its network receive path.
- **nhook Packets: the game layer (halo3's message names), session messages** (2026-11-07; nnet.c pkg_install / pkg_hit /
  pk_control, nhook.c pkg_probe, stats_page.html) — WHY: backlog N9; the page hid the "game" switch for the native app.
  HOW: NHOOK_PKG_PROBE=1 showed halo3's message table (halo3+0x46b6e98) in the native app with every encode / decode a lifted
  function (player-acknowledge's are the app's own code; synchronous-client-ready has none) and the loader's five message
  senders / receivers lifted too; =2 counted them in a 59-player game: membership-update decoded ~150 times in 2 minutes, the
  gateway's receive twice, the channel send / receive / reply, the synchronous-* messages and game-results never -- the app
  carries in-game traffic in its own protocol. So the switch (POST do=pktgame&on=1) hooks each distinct encode / decode
  function on first use (code-table entry hooks, permanent; handlers idle while off: 75 of 77 hooked) and logs the SESSION
  messages by name (a function several types share is named by the first, "+", and listed in full in the counts), with no
  peer. Checked live: membership-update x64 received, parameters-update x10, peer-properties x5 sent in 70 s.
- **nhook on a native dedicated server** (2026-11-07; shim/tools/native-dedicated.sh, nhook.c probe_thread, nstats.c
  st_listen_thread) — WHY: backlog N12; and the host tools need a native host. HOW: native-dedicated.sh runs the app's own
  `--server FILE.toml` mode from an isolated folder (ReclaimerMac/native/test-dedicated: its own profile, cache, data) with a
  LOCAL-ONLY config made on first use (bind 127.0.0.1, hidden, no master, stats off, no UPnP, no auto-update; Guardian Slayer
  on UDP 49190, an RCON password in rcon-password.txt) and nhook injected; `rcon <command>` drives it. Two fixes on the way:
  nhook never installed its hooks when NHOOK_HALO3 was unset (probe_thread returned before hooks_start: native.sh always set
  it), and the stats page now binds only in the process that runs the engine (the server's supervisor and its re-exec carry
  the injection too and would have taken the port). Launched without nohup (a system binary strips DYLD_INSERT_LIBRARIES).
  Checked live: a test client joined 127.0.0.1:49190 (NATIVE_ALLOW_SECOND=1: the server is no client); the server's page
  (8767) says host with the player, the client's (8765) client + the server's address; an RCON `say` reached the client's
  feed as a server line; RCON `load construct Slayer` changed the map in the session: "Game over", "Match started:
  Construct", the client's map / heatmap (Construct's spawns) / zones followed (backlog N5).
- **nhook-selftest.sh: its own forensic / heatmap folders** (2026-11-07; shim/tools/nhook-selftest.sh) — WHY: the offline
  selftest starts nhook's stats server, whose archive opened an empty "menu" session in the player's real forensic archive on
  every run. HOW: PELOAD_FORENSIC_DIR and NHOOK_HEATMAP_DIR point into the test's temporary folder.
- **nhook archive: sessions not named after the loading screen** (2026-11-07; nstats.c, the fx_roster_tick call) — WHY: every
  mid-game join made a session "..._130_epilogue": the session opens 2 s after the host is known, while the loading screen's
  scenery (130_epilogue) is still the map hb_current_map reports (it rechecks every 2 s). HOW: the archive gets no map while
  joining, in a match's first 8 s, or when the map is 130_epilogue outside a match: such a session is named after the server;
  the roster lines carry the real map.
  NHOOK_NO_RESULTS_FEED=1 = the old estimate only. Also from the results: "Game over: <winners> won" (or "a tie") when
  games-completed (stat 1) goes up, winners = games-won (3) -- the loader's feed had it from the client's "Carnage report
  opened" line, the native feed had no end at all; checked live with the app's {"gameplay":"end_game"} request.
- **0.10 server list: shim/tools/servers10.py** (2026-11-07) — WHY: "there is no reason we should be making a .9 query": the
  0.9 client's `discover` (shim/tools/servers.sh) lists no 0.10 server, the 0.10 Windows exe's `discover` prints nothing under
  the loader, and the Mac app has no list command. HOW: 0.10 asks `/v3/discovery` on its masters and queries each server with
  ONE plain UDP datagram (a6 01 00 + 8-byte nonce, padded to 1229 bytes; reply a7 + nonce + JSON) -- no QUIC any more
  (server-status.md "Client 0.10+"); servers10.py does both with the Python standard library (one local port, answers read
  while sending so pings are real, a second try for silent servers). Found with NHOOK_UDP_HEX=<n> (nnet.c pk_hex: the first
  n bytes of the first 3 datagrams each way per peer, debug only) and the app's `--ui-script` pressing SERVER BROWSER in a
  test-profile run. A Network.framework QUIC prober (the 0.9 protocol) was tried first: 0.10 servers don't speak it.

- **nhook: the sparring aim in the native app** (2026-11-07; new shim/native/nbot.c, nstats.c snapshot / st_control,
  shim/tools/native-spar.sh; backlog N15, the loader's #17) — WHY: the user picked it to port ("wrap up the rest"). HOW: halo3's
  per-player control record (TLS block +0xC0, 4 x 0x110 bytes; +0xC0 unit, +0xC8 buttons, +0xD0 flags, +0xD4 yaw, +0xD8 pitch,
  +0xDC / +0xE0 throttle; static RE: halo3+0xf7354 runs, per local player, the update 0xf66ac then the reader 0xf7178 that copies
  the record into the unit's control). In the app both are its OWN code (their code-table entries are a `b` into its text: a
  body hook on 0xf66ac was refused), so a text hook at the entry of the app's 0xf7178 (ctx[1] = the local player index) writes
  record 0's yaw / pitch toward the nearest live enemy's head (the loader's aim point: the bounding sphere's centre + 0.55 r,
  from the observer camera) just before it is copied -- what the app's own aim request (aim_at_nearest_player, 0x102734f00)
  writes once. A thread (shim-bot, 30 Hz) decides; the hook only copies. Checked live: a moving practice target followed within
  0.5 deg; on our server (shim/tools/native-spar-test.sh) the SERVER's page has the partner's aim on the other player, median
  0.01 deg. The camera looks 4.9 deg above the record's pitch, so the page's "aim off by" is measured against the record. Test
  clients only, as in the loader (it takes over a player's view): the app must run a test profile (RECLAIMER_PROFILE_DIR; the
  app's test profile refuses public servers itself), in a local game or on a server at a loopback / private address. Switches:
  NHOOK_SPAR=1, the page's Sparring aim button (do=spar&on=), NHOOK_SPAR_LOG=1; the Hook cost panel's "bot" switch;
  NHOOK_NO_BOT=1 = none of it. shim/tools/native-spar.sh = the second test identity on our server with the aim on.
- **nhook: the walker in the native app; nav.c shared** (2026-11-07; nbot.c, shim/src/nav.c NAV_CORE / NAV_TOOL_PATH_FN /
  g_nav_mode_rt, noverlay.c g_nov_collect_mx / g_nov_units_only, stats_page.html "Walk"; backlog N16, the loader's #32) — WHY:
  the user picked it to port. HOW: nav.c's graph, danger map, A*, roam goals, builder and page path are compiled into nhook
  (NAV_CORE: without the loader's stick driver nav_step / nav_pad); the walker writes the record's yaw (at most ~0.2 rad a step,
  looking a little down) and throttle (forward, left: NHOOK_BOT_THROTTLE=0,1 moved a player facing +y toward -x) and, when
  stuck, the jump bit (record +0xC8 bit 1; bit 0 = crouch: NHOOK_BOT_BTNSCAN=1). With the aim on it walks the path in any
  direction relative to the view. Two changes from the loader's walker: no stuck detection while it strafes beside the enemy
  (side to side is no progress by design: it took that for stuck and backed off blind, off a ledge into Guardian's kill volume),
  and the back-off walks toward a path node already walked. LIMIT: only where this client's moves count -- a local game, or one
  it hosts. A joined client's movement reaches the server from the app's own input (its IOHID keyboards and game controllers),
  not from halo3's record: the throttle moved only the client's prediction and the server kept the player at its spawn (both
  pages, 2026-11-07); a key event sent to the app's window did not move it either. So the page offers the walker only then
  (walk_available), and the sparring partner on a server stands still. Checked live: roaming a local Guardian game, 5 goals and
  128 units in 80 s (native-spar-test.sh). Switches: NHOOK_NAV=roam|enemy|x,y,z|vehicle:<name>, the page's Walk list
  (do=walk&mode=), PELOAD_NAV_LOG=1 and nav.c's PELOAD_NAV_* knobs; NHOOK_NO_BOT=1 = none of it. The loader is unchanged.
- **nhook: the page's mic switch** (2026-11-07; nhook.c my_AudioUnitRender, nbot.c mic_json / g_mic_muted, nstats.c st_control
  do=mic; backlog N17, the loader's #22) — WHY: the user picked it to port. HOW: the app records its microphone with an AUHAL unit
  whose input callback pulls the samples with AudioUnitRender(..., bus 1, ...); nhook interposes AudioUnitRender and zeroes
  bus 1's buffers while muted, so the app's voice hears silence (nothing goes out with an open mic; silence with the talk key).
  The page's Mic button (the loader's: do=mic&on=) shows once the app has captured in the last 30 s, or while muted;
  NHOOK_MIC_MUTED=1 starts muted. NOT checked live: test clients open no microphone ("[voice] no capture (no microphone)"),
  and a test with the real microphone would capture the room -- it waits for the user's own game. Off: the Hook cost panel's
  "mic" switch (the interposer then passes everything through).
- **nhook snapshot: each player's aim** (2026-11-07; nstats.c st_snapshot "aim") — WHY: checking that the sparring partner's
  aim reaches the server needed the server's view of it; "facing" (+0x5c) is the body, which follows the way it walks. HOW:
  "aim": [yaw, pitch] in degrees from the unit's aim vector +0x1a0 (teleport.md). No old-behaviour switch (one more field).
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
- **Spawn points on the death heatmap** (2026-10-25; user: "add vehicle, weapon, and item spawn points to the death heatmap
  as checkboxes") — stats.c `hm_spawn` (HmSpawn, g_hm.sp; `heatmaps/<map>.spawns`, "RSP1"; loaded in `hm_switch`, saved in
  `hm_save`, `/heat.json` "spawns": [x, y, z, kind, name, times, last match]), `st_spawns` (records every spawn it confirms,
  burst weapons included), `st_feed_line` (g_sp_join), stats_page.html `drawSpawns` / `spawnShown` (checkboxes spv / spw / spi,
  localStorage mapSpawns, hover text), shim/tools/overlay-panel-mock.py (spawns + deaths in its /heat.json). How: the same kind
  and name within 1.5 units (3 up / down) is one point counted per spawn; kinds weapon / vehicle / equipment / power-up (the
  page's "items" = equipment + power-ups). After the client's "Joining the game in progress" line, the next 5 s of confirmed
  spawns are not recorded: they are the objects already lying around. Checked on the mock (markers, counts, scope, hover,
  remembered switches); live collection starts with the next launch. Old behaviour: leave the boxes unticked (the file is
  written either way; delete it to forget a map's points).
- **Spawn points live: dim when empty, items followed where they go** (2026-10-25; user: "make weapon vehicle and item spawns
  dim when not spawned, or taken, and IF the item is movable ... follow it accordingly with the icon ... IF a player got the
  overshield I want to know the x/y coordinates while it is active, or a vehicle if it is moving, where it is") — stats.c
  `st_spawns` (g_wt sp / sp0 / cur / state / holder), `hm_spawn` (returns the point), the snapshot's "items" ([spawn point,
  kind, state, x, y, z, carrier slot, name if no point]), stats_page.html `drawSpawns` / `spMark`, overlay-panel-mock.py
  (moving items, an Overshield holder). How: every snapshot each spawned / taken object's position is its own (on the
  ground) or its outermost holder's (parent chain: a player's unit, a vehicle; the carrier = the player whose unit is in
  it); state 0 = within 1.5 units of where it spawned. The page lights a spawn point only while an item of it is there
  (an older loader without "items": all lit), draws the others where they are (dashed line back to the point, not for
  vehicles), carried ones beside the carrier, and power-up holders (the players' "effects": red = Overshield, blue = Active
  camo, yellow = Custom, as st_wclass names them) with a ring and "<name> <s left> · x, y". Checked on the mock; live
  with the next launch. Old behaviour: none needed (untick the boxes).
- **Packet log: each halo3 message's peer** (2026-10-24; user: "proper" -- the per-peer answer over "the type only") —
  pktlog.c `pkc_install` / `pkc_hook` / `pk_ctx` / `pkc_peer`, `pk_game_ev` (now takes the entry rsp), `pkn_refresh` /
  `pkn_name` / `pk_peer_json`. Why: the message table's encode / decode functions get only (bitstream, size, data), so game
  rows had a direction but no peer. How: encode_message (halo3+0x5cd84) and decode_message (+0x5ce3c) have exactly five
  callers -- the gateway's connectionless send / receive (halo3+0x249dc / +0x24900; rdx -> the address, whose first 8 bytes
  are the session peer id) and a channel's send / receive / reply (+0x5f0b0 / +0x5f2f0 / +0x5efa8; rcx = the channel,
  +0x110 = the remote peer's id, found by searching the live channel object for the session's peer ids on both a client and
  our server). Entry hooks on the five (14-byte `jmp [rip+0]`, prologues checked, the dispatcher calls checked) note (rcx, rdx,
  entry rsp) per kind and thread; a message takes the note of the call whose dispatcher call is on its stack (the table
  function's entry rsp + 0x30 = the dispatcher's return address: both keep 0x28 bytes) when that call is still running (its
  entry rsp above that slot -- a note left by a returned call, nested ones included, is ignored). The record stores the
  id (fam PK_FAM_PEER); the readers name it from the session (player name, "host", "you", "peer N"; ids stay named after
  the session ends). JSON: peer, peer_id, peer_no, via (gateway / channel). The five hooks stay in once the game layer was
  on (each call notes three values). Old behaviour: `PELOAD_PKTLOG_NOPEER=1`. Debug: `PELOAD_PKTLOG_CALLERS=1` prints each
  type's first callers and peers.
- **Packet log window empty with 1-RTT hidden** (2026-10-24) — pktlog.c `pk_json` (skip1rtt=1, wire=0, game=0), stats page
  `pollPk`. Why: the page asked for the newest 600 records and filtered them itself; on a connected client those are all
  1-RTT datagrams (~160 a second), so with "1-RTT in the log" off the log stayed empty. How: the filters run in pk_json,
  which walks back from the newest record and returns the newest 600 that pass. Old behaviour: none (the page's filter
  still runs too).
- **Privacy: who sees your IP address** (2026-10-24) — README "Who sees your IP address": a joined client has one QUIC peer,
  the server (voice relayed); other players never get your address; the server, its admins, every server the browser queries
  and the community services do; hosting publishes it. Verified on the wire 2026-10-25 (shim/tools/privacy-check.sh, a
  hidden client joining a public 60-player BTB server by a join link): in the match UDP with the server only while six
  players talked; before it, status exchanges with 343 game servers in ~3 s even on a straight join link (the README said
  "while you browse": corrected), the master server, two more port-49175 hosts and api.github.com.
- **The status query decrypted: PELOAD_KEYLOG, PELOAD_TRACE_UDP_HEX, re-tools/quicparse.py** (2026-10-25; user: "what did it
  send, and what did it receive? ... fully parse, and label the fields") — pktlog.c `pk_keylog_install` / `pk_kl_log` /
  `pk_kl_will`, peload.c `udp_tracef`, pktlog.c `pk_on_frame` (PELOAD_FRAMECAP_HEX), shim/docs/server-status.md. Why: the
  status exchange is QUIC (quinn + rustls), so the packet log shows only packet types, and the client logs no keys (no
  SSLKEYLOGFILE). How: rustls asks its config's KeyLog -- the client's is rustls' NoKeyLog, a ZST whose vtable {drop, 0, 1,
  Debug::fmt, log = `ret`, will_log = `xor eax,eax; ret`} is found by its fmt (writes "NoKeyLog"; 0.9.23 exe+0x25ee408);
  PELOAD_KEYLOG=<file> points log / will_log at ms_abi functions of ours (data only) that write NSS key-log lines;
  PELOAD_TRACE_UDP_HEX=<n> hex-dumps datagrams; quicparse.py decrypts Initial (public salt), Handshake and 1-RTT packets and
  labels every header field, frame, TLS message (extensions, transport parameters, certificate) and stream. Result: the
  client sends `{"status":2}` (4-byte length + JSON) on a new connection per server, server name "reclaimer", no client
  certificate; the server answers name / host_name / map / mode / players / capacity / ready / password_required /
  version / variant / blocks_vpn with a self-signed rcgen certificate. Debug only, off without the variables.
  privacy-check.sh polls every second (a 5 s poll of /packets.json?max=4000 lost part of the start-up burst, which had made
  its handshakes look missing); README numbers corrected (343 servers in that recording is a lower bound; 383 listed).
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

- **Graphics quality set directly, not through the client's menu** (2026-10-25; user: "instead of navigating the menu system to
  apply the graphic setting it should just call the function directly in the http status") — stats.c `st_qual_install`
  (signature lists g_sig_quality_preset / g_sig_quality_apply; re-tools/patchscan.py point "quality"), `st_qual_now`, do=quality,
  the snapshot's overlay.quality; stats_page.html Quality (+ Auto). Why: the page clicked through Esc > Settings > Advanced
  graphics > preset arrows > Apply > Back > Resume (several seconds, the menu over the game, fragile). How: the client's
  apply-settings function (0.9.23 exe+0x91bb70) packs the one-view levels (Settings +0x1d3..+0x1d9) into a u64 it swaps into a
  global (exe+0x2b19e90) that its detour on halo3's detail function (exe+0x8662d0) reads whenever halo3 asks; the page now calls
  the client's own preset table (exe+0x181a0d0, preset -> the 7 levels; registers only, matched whole, identical 0.9.17-0.9.23)
  and swaps the result into that global (game-options.md "How the client applies it"). Refused before the client applied its
  settings once. Live HugeGrass match: low / ultra / medium / high / ultra each changed halo3's "Graphics detail for 1 view(s)"
  within 0.5 s. Live only: not saved to settings.json, the client's menu still shows its last Apply. PELOAD_QUALITY_MENU=1 = the
  menu clicks (also used when the globals aren't found).
- **Map: vehicles as their own outlines, spawn bays, names, no weapons on players** (2026-10-26; user: "vehicles, and
  vehicle spawn points weapons items don't have good visualizations ... the vehicle itself needs to look different from its
  spawn point, and they need to be tracked independently. we DO want to see when players are in the vehicles ... do NOT
  try to visualize weapons on players") — hitbox.c `hb_vshape`; stats.c vehicles snapshot, `st_vshape_*`, `/vshapes.json`,
  `deployed`; stats_page.html `drawVehicle`, `placeTags`, `drawSpawns`. Why: a live vehicle was a white square; one away
  from its spawn was drawn with the same orange square as a spawn point; turrets on a Warthog were listed as vehicles at
  their parent-relative position (near the map's origin); spawn and item markers had no names on the map; carried items
  were drawn beside their carrier. How: the snapshot lists only vehicles without a parent, with `id` (object handle), `def`,
  `yaw`, `dead`, and each seated player's `in` = the outermost vehicle (a gunner's turret -> its Warthog); per definition the
  loader makes a top-down outline from the collision model posed by the first live instance's node matrices (per region and
  node the convex hull, in the vehicle's frame) once per map, served as /vshapes.json. The page draws each vehicle as that
  outline, turned to its heading, at true size or at least 22 px long (14 in the 560 px panel), a white chevron at its front: grey when empty, the
  occupants' team colour when manned (seat dots, names in its label), dark and faded when destroyed; vehicle spawn points
  are dashed orange bays named "<vehicle> spawn" while empty; every weapon / item marker gets its name (a greedy placer:
  8 spots per tag, highest priority first, left out when there is no room); carried items are not drawn; deployed
  equipment (left out of the spawn tracker) is listed and drawn as a dashed cyan ring with its name. Checked on local
  Valhalla: Warthog, Mongoose, Banshee, Wraith and the machine-gun turret outlines are recognisable.

- **nhook on client 0.10.1: the app's addresses per build** (2026-11-08; shim/native/nhook.c app_build_pick / APP(), napp_builds.h,
  re-tools/machoport.py, shim/native.sh) — WHY: the app updated itself to 0.10.1 and every hook stayed off ("no code table:
  hooks off", no stats page at all): nhook used 0.10.0 constants for the guest image, the code table, the Opus decode / voice
  mixer, the per-frame function, the chat_log gate and byte, the mute list and set_muted. LOGIC: one row per app build in
  napp_builds.h, picked by the running app's LC_UUID (read from its loaded Mach-O header) + the app's dyld slide; a row
  re-tools/machoport.py wrote later is read from ~/Library/Application Support/ReclaimerMac/native/app-builds.txt (a prebuilt
  release dylib); a point 0 = not found in that build = that feature off. The run-time checks no longer compare 0.10.0's words:
  they decode them (the mixer's `bl` must reach the decode, its id load `sub x8, x26, #off` must have the row's offset, the chat
  gate's adrp + ldrb must reach the row's chat_log byte, set_muted's adrp + add the mute list). 0.10.1: image 0x104124000, code
  table 0x1040d4bc8 (22894 entries, unchanged), decode 0x1003fdc68, mixer call 0x102212338, id load 0x10221220c with the id at
  **x26 - 0x2a0** (0x278 in 0.10.0: the mixer's sender entry grew by 0x28; the channel byte stays at x26 - 7), frame function
  0x102ac23d8, chat gates 0x100d7ea98 / 0x100d7eabc, chat_log 0x10408cdf3, mute list 0x10409a728, set_muted 0x102214604. The
  halo3 side (code-table hooks, TLS fields, the capacity lines) needed nothing. shim/native.sh: when `machoport.py check` says the
  installed app has no row (exit 2), it ports it from re-tools/machoport-sigs.json, writes both tables and rebuilds the dylib, before
  launching. With hooks off (an unknown build) the stats page now starts anyway, so its Hooks check window says why
  (hooks_off_page). SWITCHES: NATIVE_NO_AUTOPORT=1 (no port at launch: the old "hooks off"), NHOOK_APP_BUILD=<version> (that row
  whatever the UUID), NATIVE_PORT_REF=<binary or sigs.json>. REMOVED with it: the 0.10.0 constants (APP_IMAGE, APP_CODETAB,
  APP_MUTED, APP_SETMUTE, HT_FRAME_FN, the voice / chat literals), the exact-word checks (incl. the mixer's bl encoding and
  #0x278), is_muted as a tracked address (never used at run time) -- native-client.md "What was removed".
- **The hooks check** (2026-11-08; shim/native/nhchk.h + nhchk.c, GET /hooks.json, the stats page's Hooks check window,
  shim/tools/native-hooks-test.sh) — WHY: the user, on 0.10.1: "can you add a hooks check? like all the stuff you patch I assume
  you can detect IF on a new version the hook loaded properly or function". LOGIC: a registry of every thing nhook depends on in
  the app -- the build's address row, the guest image, the code table, the halo3.dll image check, each entry / body / text hook,
  each interposed function, each swizzled Metal method, each fixed app address (chat_log gate, set_muted, voice mixer), the app
  log lines the parsers need, the memory readers (players table, statborg, game results, network session, observer camera) --
  with its feature, WHEN it is expected to fire (always / engine / match / shots / online / voice / chat / mic / overlay / bot /
  host / setup / never), its state (ok, on first use, off, FAILED + why) and its fires / last fire. Hooks are counted by a
  trampoline in front of the handler (hk_wrap: 48 counting thunks that pass x0-x7 on; not the Opus shim, which reads the stub's
  stack -- nh_on_opus counts it), so no handler changed; an install failure records the installer's own last log line (nlog keeps
  each thread's last line). Hooks installed on first use (the camera, sparring aim / walker, the host tools) get a preflight at
  start: the installer's shape checks without writing anything. Verdicts: ok / FAILED / silent (installed, its "when" happened,
  never fired) / waiting / off / not expected. [hooks] summary lines 90 s after the engine started and 60 s into each match;
  the snapshot's "hookcheck" counts ("hooks" is the cost meter's). The self test: a local Guardian test match that fires
  (hold_trigger), has the overlay on, the walker roaming and uses the host tools (boundaries, invisible walls, a teleport), then
  fails on anything FAILED or on an item that match must exercise that never fired. On 0.10.1 its first run passed 30 items and
  found the stub-page bug below. In the user's first 0.10.1 game: 33 ok, 0 failed (the observer camera's "when" is the overlay:
  nothing reads it with the overlay off -- it showed "silent" until that fix). shim/tools/hooks-watch.py prints verdict changes
  live. SWITCH: NHOOK_NO_HKWRAP=1 = hooks without the counting thunk (fires stay 0).
- **Stub slots: many hook stubs to a page** (2026-11-08; shim/native/nhook.c stub_alloc / stub_seal) — WHY: the hooks check's
  first run on 0.10.1: the host tools' game-tick body hook FAILED, "near_page: 0 free gaps of 16384 bytes within 120 MB of
  0x14aafa8c0": every entry / body hook took its own 16 KB page within branch range of the JIT region, and the free gaps there
  are few (three hooks took them all). That hook is what a dedicated server runs host requests from. LOGIC: a near page is
  mapped twice -- RX in the gap (what the hooks branch to) and RW anywhere (mach_vm_remap, shared) where stubs are written -- and
  stubs are 64-byte-aligned slots in it, so one gap holds every stub and a page with live stubs is never made non-executable
  (the reason for one page each). SWITCH: NHOOK_STUB_PAGE_EACH=1 = one page per stub (the old way).

- **The soundboard: a sound file into the game's microphone** (2026-11-08; shim/native/nmic.c, the stats page's Soundboard panel,
  GET /soundboard.json, POST /control?do=sb&play=|stop=|vol=, POST /soundboard/upload?name=) — WHY: the user: "add a soundboard in
  which I can select a wav and play it into the mic input path into game audio". The user's Mac (a Mac Studio) has NO input
  device: the app asks CoreAudio for the default input (kAudioHardwarePropertyDefaultInputDevice), gets 0 and stays "[voice] no
  capture (no microphone): listening only" -- there was no microphone path to play into. (The app's own --voice-wav reads the
  whole file at launch -- a named pipe was read to its end first -- and transmits continuously, silence too: no good for a
  soundboard; shim/tools/voice-fifo-probe.sh.) LOGIC: with no real input device nhook answers the default-input query with a
  VIRTUAL device ("Reclaimer soundboard", 48 kHz mono float) and answers every property query on it (name, UID, stream
  configuration, formats, rates, buffer sizes; a device property asked through the unit -- cpal's 'fsz#' -- too); the AUHAL unit the
  app points at it (kAudioOutputUnitProperty_CurrentDevice) is emulated (its settings kept, Initialize / Start / Stop / Dispose
  answered, a 10 ms clock thread calls the app's input callback) and its AudioUnitRender(bus 1) gets the soundboard's samples
  (silence when idle). The app logs "[voice] microphone Reclaimer soundboard at 48000 Hz" and treats it as its microphone, so its
  Talk mode decides when it transmits (Sensitivity: by itself while a clip plays; Hold / Toggle: with the talk key). With a real
  microphone the clip is mixed over it (the mic switch's AudioUnitRender interposer). Clips: ExtAudioFile decodes WAV / AIFF /
  CAF / MP3 / M4A / AAC / FLAC to 48 kHz mono (10 minutes at most), resampled to the app's client format (float or 16 / 32-bit int,
  any channels); files in ReclaimerMac/soundboard (NHOOK_SOUNDBOARD_DIR), added from the page (upload with /control's guard, 50 MB
  at most). Hooks check item "soundboard microphone". Live test shim/tools/native-soundboard-test.sh: A (virtual mic) uploads and
  plays a tone, B hears it at -24.6 dBFS, stop works. SWITCHES: NHOOK_VMIC=0 = no virtual microphone (the old "listening only"),
  NHOOK_VMIC=force = the virtual one beside a real one; diagnostics NHOOK_AUDIO_TRACE=1 (every CoreAudio device / AudioUnit call),
  NHOOK_MUTE_OUTPUT=1 (the app's sound zeroed: silent test clients).
- **The depth swizzle retained every depth texture** (2026-11-08; shim/native/nrender.m my_set_dtex) — WHY: found while chasing
  backlog N20: g_dseen was an NSMutableSet (it retains), filled with every depth texture the app ever attached, across map changes,
  never emptied; its 8 "full-size" slots never let one go either, so after a few map changes "visible only" / zones could read a
  dead map's depth. LOGIC: a weak NSHashTable; the 8 slots hold their texture strongly (g_dkeep, at most 8) and the stalest slot is
  reused once its texture has not been attached for 240 frames. (Not N20's cause: 13 textures in a two-client test, flat.)
  SWITCH: NHOOK_DEPTH_RETAIN=1 = the old set.
- **The slowdown watch** (2026-11-08; nstats.c slow_watch, backlog N20) — WHY: the user's first 0.10.1 session slid from ~220 to
  ~50 fps over a few minutes on one server and stayed there on the next map; not reproduced locally (not the map, not heat -- no
  thermal events on the Mac Studio --, not the depth set, no hook failed). LOGIC: with every [perf] line (10 s) a [perf+] line:
  this process's footprint and CPU (cores), nhook's render-thread time per frame (overlay, aim tick, camera hook), its engine-thread
  time per second (bullet hook + aim meter) and the snapshot's time; once per match, when the frame rate has been below half of
  the session's best for three windows, a [slow] line with the busiest threads of the last window by name (proc_pidinfo on our own
  pid). SWITCH: NHOOK_NO_SLOWWATCH=1.
- **native-aim-test from a fixed spot** (2026-11-08; shim/tools/native-aim-test.sh, nbot.c NHOOK_BOT_FACE, noverlay.c NHOOK_AIM_LOG;
  backlog N21) — WHY: on 0.10.1 one run gave 8 shots, all ~2 deg off the practice target, 0 hits. NHOOK_AIM_LOG=1 (each measured
  shot: start, direction, the nearest hitbox part, the miss) showed the meter right (shots through the target, "ON") from an open
  spawn: Guardian's random spawns put the target 5 units ahead on a ramp, against a wall or over the drop. LOGIC: the test teleports
  the player to the open ledge (-3.97, -23.39, 17.2; the host tools' tpto: a local game is its own host), holds the facing north
  (NHOOK_BOT_FACE=93.3: the sparring hook writes the yaw) and fires later (40 / 44 s); the log stays when it fails. 3 of 3 runs:
  8, 6, 10 hits of 10.

- **Walkers that fight on a server** (2026-11-08 night; nbot.c bot_on_abuild / the fighter, nav.c g_nav.pop, backlog N24) — WHY: the
  user: "make 3-4 walkers have a battle ... in a rumble pit match", "train your sparring partners well". A JOINED walker never moved
  on the server: a client sends its controls as packet "A" (net-authority.md section 1), built by halo3+0x50fb8 (the app's own code)
  from a control record; the app's input layer refills throttle and buttons from its devices every tick, and nbot.c wrote its values
  at the control READER (0xf7178), which runs after the packet was built -- only the aim leaked through (the input layer only nudges
  it). A virtual GameController pad (nvpad.m) did not help: the app reads controllers through IOHID and never polls GCController
  (0 calls; it imports only GameController's haptics). LOGIC: (1) a text hook BEFORE 0x50fb8 writes the walker's yaw / pitch
  (+0x10 / +0x14) and throttle (+0x18 / +0x1c) into the record it packs -- the server's path of a roaming walker 79.5 units vs the
  client's 76.2 in 40 s (shim/tools/native-vpad-test.sh). (2) Fire = the control record's bit 19 at +0xC8 (a scan: bits 19 and 21
  fire, 1 jumps): the local weapon fires and the packet's trigger bytes, which halo3+0x5452c reads from the local weapon's barrels,
  carry it -- 180 shots counted by the server for 182 on the client (a packet button bit of its own does nothing). (3) The fighter
  (NHOOK_BOT_FIGHT=1): line of sight by halo3's collision ray (0x1fd748, cast from the engine thread inside the control hook,
  ignoring both bodies), a sight cache over the 3 nearest enemies (a visible one beats a nearer hidden one), a reaction delay, a
  turn-rate limit and Ornstein-Uhlenbeck aim noise (NHOOK_BOT_SKILL), the real players' distance band, strafe timing and back-off
  on low shields (the model), fire only with a clear line and the aim on, grenades (left trigger bit) and melee, never off the
  walkable floor (a nav node within ~1 unit of every fighting step), no tracking through walls (out of sight beyond 8 units it
  looks where it walks); the walker's old "near the enemy: strafe" rule is off while fighting (on Guardian's stacked floors it
  strafed in place under an enemy). (4) A* prefers the lanes real players use (nav.c g_nav.pop, up to 40% cheaper) from the
  model's heat, recordings plus the stats page's footprint heatmaps. Battle 3 (4 walkers, 4 minutes, Guardian free-for-all): 13
  kills, each walker with a target in sight ~55% of the time. SWITCHES: NHOOK_BOT_NOPKT=1 (no packet hook: a joined walker is
  local only again), NHOOK_BOT_FIGHT=0 (the old spar aim + walker), NHOOK_BOT_FIREBIT=<hex>, the knobs NHOOK_BOT_SKILL / _DIST /
  _STRAFE / _SWITCH / _FIRE_EXTRA / _RETREAT / _GREN, diagnostics NHOOK_BOT_HOLDBTN / _PKTBTN / _BTNSCAN / _FACE, NHOOK_VPAD=1.
- **Learning from real players** (2026-11-08 night; shim/tools/match-recorder.py, observe-public.sh, walker-model.py,
  walker-compare.py, walker-train.py, walker-battle.sh, backlog N25) — WHY: the user: "join several games that don't require map
  downloads, and watch and sample how real players move around the map, and skirmish ... build a library / model ... have 3-4
  walkers spar while they learn to mimic actual players". LOGIC: observe-public.sh joins a busy mod-free public server with the
  user's own profile in a background window (the 0.10 app lets only a real profile into public servers; nothing is pressed; the
  walker code refuses public servers by itself; joins at least 3 minutes apart -- a first version rejoined each time its recorder
  crashed: 3 joins in 2 minutes, fixed); match-recorder.py polls the stats page 10 times a second: per player slot, team, alive,
  position, look angles, shield, health, weapon, vehicle, kills, deaths, shots, hits -- slots only, the names are dropped -- plus
  the feed's kills; walker-model.py turns recordings into the walkers' numbers (speeds, standing share, fight distances, strafe
  RMS and switch time, closing speed by shield, aim error, reaction, fire rate, hit share, per-map flow + heat); walker-compare.py
  puts walkers and players side by side; walker-train.py evolves the fighter's tactics over 4-walker battles (fixed skill:
  tactics, not aim, decide; fitness kills - 0.7 deaths + 2 x accuracy). First FFA data (Assembly, Snowbound): players run 2.1
  units/s and stand 40% of the time, fight at 5.8-15.7 units (median 12.6), strafe 0.9 units/s switching every 1.1 s, stop closing
  in on low shields, aim 4 deg off (median), react in ~1 s, fire 3.4 shots/s in fights, 48% of shots through a hitbox.

- **Far hooks: no stub page in branch range** (2026-11-08; nhook.c jit_far_hook, body_hook_after0's far path, stub.S _nh_ba's four
  displaced slots) — WHY: in a walker battle one client's engine_clock hook failed, "no stub page near 0x146ab4100": some launches
  have no free 16 KB gap at all within +-128 MB of the JIT code (the shared stub pages need one gap; that launch had none), and
  without that hook a client never finds its players -- the walker idled the whole battle, and the user's own game would lose its
  stats page the same way. LOGIC: when stub_alloc finds no slot, the entry's (or body's) first four instructions -- a lifted wrapper's
  prologue stp / mov / stp / ldr w,[x0,#0x290], none PC-relative -- are replaced by `ldr x16, #8 ; br x16 ; .quad stub` with the
  other threads stopped (none inside those 16 bytes) through pthread_jit_write_protect_np; the stub (anywhere: stub.S _nh_tp4 /
  _nh_tps for entry hooks, _nh_ba with four displaced slots for body hooks) runs them and returns to +16. Checked with
  NHOOK_FAR_TEST=1 (as if no gap were in range): engine_clock 398,732 calls, the players read, the host tools' game tick (7,993)
  and shape builder hooks fired. SWITCH: NHOOK_NO_FAR_HOOKS=1 = the old failure; NHOOK_FAR_TEST=1 forces the far path.
- **Walker grenades** (2026-11-08; nbot.c) — WHY: the fighter "threw" through nvpad.m's left trigger, which the app never reads.
  LOGIC: a 0.15 s pulse of the control record's bit 30 at the reader (the button scan: one press threw), like the trigger bit 19.
  SWITCH: NHOOK_BOT_GRENBIT=<hex>; NHOOK_BOT_GREN=0 = no grenades.

- **Walkers more like players; how human they are as a number; pickups** (2026-11-08 early morning; nbot.c, walker-compare.py,
  walker-model.py, walker-train.py) — WHY: walker-compare.py on the first training battles (1,478 walker engagements vs 39 of
  players so far): the walkers reacted in 0.42 s (players 1.08), aimed 2.8 deg off (4.1), barely pressed in with shields up (+0.02
  units/s; players +0.78) and backed off hard on low shields (-0.37; players about 0), fought closer (7.7 units; 12.6) and stood
  more (55%; 40%) -- "human-likeness" 55%. LOGIC: the fighter's reaction factor 0.7 -> 1.3 and aim noise x1.6 (skill 0.5 then
  matches the players; 1 is still better than them); inside the distance band it presses in (NHOOK_BOT_PRESS, 0.4 throttle), on
  low shields it mostly stops (NHOOK_BOT_BACKOFF, 0.15) -- both also training knobs. Human-likeness = 100 x exp(-mean |log(walker /
  player)|) over the comparable measures (walker-compare.py, the report). Infection recordings give lanes only (heat, flow),
  not fights or speeds. Pickups: out of a fight the walker walks to a better weapon (bot_wpn_rank: power weapons, BR, Carbine
  ... above the AR) lying within 12 units and holds the pick-up bit -- NHOOK_BOT_PICKBIT, found by shim/tools/native-pickup-scan.sh
  (a local match: on a weapon, each candidate bit held through do=botbtn&bits=&secs=, a test-identity control). SWITCHES: the
  knobs above; NHOOK_BOT_PICKBIT unset = no pickups.
- **The pickup scan, and what it found** (2026-11-08 06:30; shim/tools/native-pickup-scan.sh) — WHY: its first run tried every bit
  "on -1": the snapshot's items are [spawn point, kind, state, x, y, z, holder (-1 = on the floor), name only without a spawn
  point], so the last field was the holder, not a name, and no kind filter kept vehicles / equipment out. LOGIC: weapons only
  (kind 0) lying free (holder -1); the name from the spawn point's entry in /heat.json (spawns[i][4]) when the item has one; the
  player's position after the teleport is printed beside the weapon's (they matched within 0.2 units); PS_BITS / PS_SECS pick
  the bits and the match length; bits 32-63 too. RESULT: standing on a Mauler ("Excavator": halo3's tag name for it) with the
  assault rifle, NO bit of the control record's 64 (+0xC8; all but 1 jump, 19 / 21 fire, 30 grenade) held 1.5 s swaps it. Pickup
  is not a plain button bit there -- most likely a separate held-action state the app's input layer keeps. NHOOK_BOT_PICKBIT
  stays unset (walkers keep their loadout); backlog N24.
- **A dedicated server's hits** (2026-11-08 07:30; nstats.c, the players' "hits" / "accuracy") — WHY: every battle scoreboard of
  the night (both training runs, 17 battles) had hits 0 for every walker, so the fitness's accuracy term never counted. The
  hits come from the aim meter (noverlay.c nov_aim_shot queues each shot; nov_aim_tick resolves it against the target's posed
  hitboxes on the RENDER thread, every frame) and a dedicated server never presents a frame. LOGIC: a process that has
  presented no frame (g_nr_frames 0) reports the old estimate instead (hits_sphere: the projectile's end point inside a
  biped's bounding sphere), and noverlay.c's object_delete hook runs that sphere pass there too (it ran only with
  NHOOK_NO_AIM: the first showcase battle after the snapshot change alone still said 0). SWITCH: NHOOK_NO_AIM=1 = the
  estimate everywhere (as before).
- **Walkers react like the players** (2026-11-08 07:40; nbot.c, the fighter's reaction delay) — WHY: the showcase battle with
  the trained champion (four walkers, skill 0.6) measured a 0.49 s reaction against the recorded players' 1.08 s (walker-compare
  .py). A first try (the delay factor 1.3 -> 2.8) changed nothing (0.53 s): walker-model's reaction is the time from a target
  in reach within 30 deg of the view to the aim within 5 deg, and before its reaction time the fighter turned 1.5 rad/s -- the
  25 deg in ~0.3 s whatever the delay. LOGIC: the turn rate before the reaction time = NHOOK_BOT_TURN0, the delay = the
  model's reaction x (1.25 - skill) x NHOOK_BOT_REACT (default 1.6, was a fixed 1.3). Measured: 1.5 rad/s -> 0.51 s, 0.45 ->
  1.79 s (measured ~ 0.79 / rate), so the default is 0.73 rad/s (the players' 1.08 s).
  SWITCHES: NHOOK_BOT_TURN0=1.5 NHOOK_BOT_REACT=1.3 = before.
- **The engine image wait: 10 minutes, not 60 s** (2026-11-08 07:30; nhook.c probe_thread) — WHY: one battle's server engine
  process (while four clients started beside it) had no MZ at the engine image and code table 0 for 60 s, the old limit; the
  probe gave up, every hook stayed off for the process's life, its stats page showed no players and a wrong map, and the
  battle had no scoreboard (the hooks check said it: "code table FAILED", "halo3.dll image check FAILED 7173 of 65536").
  LOGIC: keep polling every 250 ms up to 10 minutes, a "still waiting" line at 60 s. SWITCH: none (waiting longer only delays
  hooks that would otherwise never come).
- **walker-train.py --champion** (2026-11-08 07:30) — WHY: selection carries the best two by their LAST three battles' mean,
  so the final best-tactics.json can name a set whose last three ended badly by luck while another had the better record
  (run 2: two sets tied at 1.63 in the last generation; over all their battles 2.97 vs 1.63). LOGIC: `--champion [--since <t>]`
  reads train.jsonl and writes the set with the best mean over ALL its battles (at least 3) to best-tactics.json ("by": "mean
  over all its battles"). Training itself is unchanged.
- **Lanes: the ways players move, carved into the map** (2026-11-08 11:00; shim/native/nlanes.c, stats_page.html, shim/tools/
  lanes-import.py) — WHY: the user asked for "a 'lane' option for the heat map, so you can select lanes, or deaths ... carving a
  visible rut the more they are used". The footprint heatmap counts TIME per 0.5-unit cell (a camper wears his spot most), so it
  shows where players stand, not the routes they take. LOGIC: every stats snapshot (~10/s) each live player's move since the last
  one (same body, <= 0.6 s, 0.02 .. 8 units, < 40 units/s -- respawns and teleporters are no lane) is walked through the grid
  (Amanatides & Woo) adding the length inside each cell in cells = "passes"; on foot and in vehicles apart, the on-foot
  direction summed. Per map in <heatmaps>/<map>.lanes (RLN1), saved every 30 s and at exit as a MERGE under an flock on
  <map>.lanes.lock (read, add what came since the last save, temporary file, rename) so lanes-import.py and a second game add
  up; this match's lanes in memory. GET /lanes.json?scope=all|match; the snapshot's lanes_live (match-recorder.py writes it into
  each recording, lanes-import.py skips those: never counted twice). lanes-import.py walks match-recorder.py's recordings the
  same way (the night's 25 recordings: Standoff 85 km on foot, Last Resort 66, Guardian 58 ...). The page: heatmap = deaths /
  lanes / lanes + deaths / neither (remembered), lanes on foot / vehicles; the passes smoothed (sigma >= 0.55 units), a rut from
  the map's typical crossed spot (median) to full shape at its busiest 0.5% (log scale), deeper overall as traffic grows (the
  busiest cells against 400 passes), lit from the top left as a relief, worn-earth floor, a dust wash below the typical spot;
  hover = "players crossed here about N times, mostly one way ↗ / both ways". A fixed scale (one pass a scratch, 400 a full rut)
  wore Standoff's whole valley into one hollow -- relative depth shows its lanes. Offline: shim/tools/lanes-test.sh (the walk
  adds up, the merge keeps both writers' cells and the import list), shim/tools/lanes-page-mock.py (the page on a real map's
  data, no game). SWITCH: NHOOK_LANES=0 = no lanes recorded (the page then has none); heatmap "deaths" = the old view.
- **The observer samples a mix of whole games** (2026-11-08 11:20; shim/tools/observe-public.sh) — WHY: the user: "sample a bunch
  of large games (that don't require downloads) make sure it is a mix of slayer, ctf, and infection game types. and sample the
  whole game". It left every server after a fixed SESSION (mid-match) and picked by FFA > Slayer, Infection last. LOGIC: each
  server gets a class (slayer / ctf / infection / other; campaign, forge, firefight, races and UNSC vs Flood vs Covenant never);
  a class's whole matches recorded this run lower its servers by OBS_CLASS_W (15) players, "other" by 25, so classes take
  turns while the bigger game wins inside a class; it stays until OBS_WHOLE (1) whole match is recorded there (a recording after
  the one it joined into, 1500+ samples) or MAXSESSION (2nd argument, 2700 s); OBS_MIN_PLAYERS (6), 4 if none has that many; a
  visit without a whole match skips that server for the run. SWITCH: the old picker is in git (dfad978).
- **match-recorder.py stops cleanly** (2026-11-08 11:20) — WHY: the observer killed it with SIGTERM mid-write, leaving gzip
  files cut off (readers had to catch EOFError; a cut file loses its last block). LOGIC: SIGTERM / SIGINT set a flag; the loop
  ends after the current sample and closes the file. observe-public.sh waits up to 6 s for it.
- **The engagement library: how players actually fight** (2026-11-08 11:40; shim/tools/engage-extract.py, engage-model.py) —
  WHY: the user: "a good dataset on how people *actually* play when engaging with someone else" and "don't just limit the logic
  to single player engagement ... they may get shot by someone else while fighting 1:1 and change focus". walker-model.py only
  measured "looking at an enemy within 10 deg". LOGIC: engage-extract.py cuts each recording into DUELS (two enemies within 45
  units, one attacking the other: horizontal aim within 8 deg while its shots counter rises, or 12 deg while the other is hurt;
  merged across 2-s pauses; from the awareness before to the outcome: kill event, death, third party or separation; 0.1-s rows
  of distance, each one's speed across / toward the other, aim errors, shields, fire, vertical speed) and MULTI-PLAYER records
  from every player's held target: choice (all enemies within 45 units with 8 features, which was taken), switch (a second
  attacker while fighting: turned to it or not, how fast), pressure (2+ attackers vs 1), join (a teammate's new target near me),
  skirmish (attack-graph components every 3 s). Infection sides from weapons (the Energy Blade). Per recording into
  tracks/engagements/*.eng.jsonl.gz (re-cut only when the recording is newer). engage-model.py reports by class and writes
  tracks/engage-model.json: distance bands by weapon class, movement numbers, retreats, time to kill, the target-choice rule as
  a conditional logit (top-1 64% on held-out Slayer choices vs 36% "nearest", 52% "nearest to the aim"), second-attacker odds
  (in view 33%, behind 7%, ~1.75 s), and a movement library (2-s pieces of the attackers' moves by weapon class / distance).
  First numbers (Slayer, mostly FFA BR): the first attacker wins 84% of decided duels; the defender was looking away 52% of the
  time and then won 7% (27% facing); BR fights start at 21.7 units median (14-31); outnumbered players die 41% vs 17%.
  shim/tools/engage-compare.py <walkers' recordings> = the same measures for a walker battle beside the players' (fight-
  likeness score; signed measures by their gap): last night's final battle 68% (attack distance 8 vs 14 units, outnumbered
  deaths 11% vs 53%, second-attacker turn 0.5 vs 1.3 s).
- **The fighter fights like the players: awareness, target choice, second attackers, weapon bands, real moves** (2026-11-08
  12:00; shim/native/nbot_engage.c, nbot.c) — WHY: the night's fighter took the NEAREST enemy (with a seen one first), knew every
  enemy around it, kept one distance band for every weapon and strafed side to side on a timer. LOGIC: engage-model.json
  (NHOOK_BOT_ENGAGE=<file>): an enemy is known in view (75 deg), when it attacked us (aimed within 12 deg while we were hurt),
  within 5 units, or our target of the last 3 s; a new target = the best w . features over the known enemies within 45 units
  (the players' fitted rule) +-1.5 for a clear / blocked line of sight; kept while alive and seen within 1 s; a NEW attacker
  while we fight for 1 s+ turns us with the players' odds (in view / behind) after their delay x 0.5..1.5, once per attacker;
  the band = the players' attack distances for the held weapon's class (20+ duels); moves = 2-s pieces of the players' fights
  (weapon class / distance band) as the throttle with a pull into the band. The A* lane preference (g_nav.pop) also takes the
  lanes (<map>.lanes on-foot passes, real heatmaps folder; NHOOK_BOT_LANES=<dir>, =0 = heat only): a node keeps the larger of
  heat and lanes. Offline: shim/tools/engage-load-test.sh (the C loader reads what Python wrote). Live battle test: after the
  sampling block (one game at a time). SWITCHES: NHOOK_BOT_ENGAGE=0 = the night's fighter; NHOOK_BOT_LANES=0.
- **Grenades from the players** (2026-11-08 12:00; match-recorder.py, engage-extract.py / engage-model.py, nbot.c) — WHY: the
  recordings had no grenade data, so the walker's grenades were a tactics guess (6..16 units, every NHOOK_BOT_GREN s); and the
  next throw's random delay was drawn EVERY tick, so the shortest draw won and throws came early. LOGIC: the recorder adds each
  player's grenades carried (row index 16; a drop = a throw); duels list each side's throws (time, distance); the model's
  "grenades" = throws per minute of fighting and the throw distances (25..75%); the walker uses that band and rate once 50+
  duels carry counts (NHOOK_BOT_GREN set = its own interval), the next throw time drawn once per throw.
- **The new fighter against the players, first battle** (2026-11-08 12:15; nbot_engage.c, nbot.c, engage-compare.py,
  engage-extract.py, engage-model.py) — WHY: a 5-minute 4-walker battle with the engagement fighter (champion tactics, distance
  x1) scored 62% fight-likeness against last night's 64% on the same player data. Better: closing in (+0.43 vs -0.08 u/s),
  outnumbered deaths (42% vs 11%; players 47-57%), turn delay (1.7 vs 0.5 s). Worse: the players' choice rule named the walker's
  target 72% of the time and the players' own 39% (argmax = too predictable); strafing 0.69 u/s / switch 1.54 s (players 0.91 /
  0.73: the tactics' strafe scale x0.76 shrank the replayed real moves); turning to a second attacker 3% (players 14%: after a
  switch the pre-reaction turn rate 0.73 rad/s needs ~3 s for 130 deg, the measure's window); aim 3.8 deg off (players 6.8);
  the band's push-back below its 25% kept walkers at its near edge while players close in at 1.3 u/s. LOGIC: the new target is
  DRAWN from the rule's probabilities (NHOOK_BOT_PICK=best = argmax); replayed moves scaled by NHOOK_BOT_LIBSCALE (1), not the
  strafe tactic; a deliberate second-attacker switch turns at full speed (its delay was the reaction); NHOOK_BOT_NOISE (x aim
  noise, 1); with replayed moves only a pull INTO the band. Tools: engage-compare.py compares with the players on the walkers'
  own maps when they have 300+ duels there (--all-maps = all: small Guardian with Assault Rifles vs Valhalla's Battle Rifles was
  no comparison); engage-extract.py --force takes a recording under 2 minutes old (a battle that just ended); engage-model.py's
  choice fit runs batched (padded sets + mask: minutes -> seconds) and writes the model atomically.
- **Radar, memory, own deaths and the goal draw** (2026-11-08 12:50; shim/native/nbot_goals.c, nbot.c, nbot_engage.c, nstats.c) —
  WHY: the user, watching four walkers: they "don't understand how the radar works", players return "to where you were just
  killed to seek out the person that just killed you", "seek out spawning power weapons, and items like overshield and camo" (a
  "route selection randomizer / solidifier"), walkers should know "their own death heat map" and "sometimes walkers just stop".
  The hunt walked to the NEAREST enemy's node, known through walls; an unreachable one kept the goal and the walker stood.
  LOGIC: radar = halo3's motion tracker rules for the walker's own resolved traits (player +0x1FC: +0x18 mode, +0x1A range; the
  snapshot publishes them for the walker's unit): enemies within the range (1 unit = 3.048 m) moving faster than 1.2 u/s or in a
  vehicle, all of them with "enhanced"; a contact is a KNOWN enemy (target choice) and remembered with its position. Goals: a
  weighted draw among known enemies (exp(-d/25)), after a respawn (25 s) the most recent killer's known position / the death spot
  / -- when that death was within 8 units of a power item's spawn point -- a point 10..25 units out along a lane leaving the
  item, drawn by traffic; power-ups and power weapons lying at their spawn points (nspawns.c g_wt); lane cells 10..40 units away
  by traffic (the busier half of <map>.lanes); held 6..15 s, dropped when reached, gone, held long enough or with no floor / no
  path -- and the next drawn at once (no more standing). Own deaths: position, killer (the last attacker within 3 s), FAILED when
  the life had no kill (a target died while fought): +3 path cost within 5 units per failed death, goal weights x (1 - 0.6 x
  failed deaths within 6 units, at least 0.15). The weights are first guesses until the players' numbers are in (engage-extract
  contact / respawn records). SWITCH: NHOOK_BOT_GOALS=0 = the old nearest-enemy hunt. Live test pending.
- **Radar and revenge in the players' data** (2026-11-08 12:50; engage-extract.py "contact" / "respawn" records) — WHY: the same
  requests, measured first. LOGIC: contact = an enemy within 15 units comes up more than 75 deg off a player's view, not attacking,
  while the player has no target: turned to face it within 2.5 s? (moving vs still); respawn = after each death with a known
  killer: back within 8 units of the death spot in 40 s (vs another death spot as far from the spawn), next target = the killer
  (vs 1 / enemies), approach speeds. Vectorized ([T, N] per player; the per-pair loop took minutes). First numbers, three big
  free-for-all Slayer matches (20,362 contacts, 1,934 respawns): within ~8 units players turn to an unseen MOVING enemy 26-32% vs a
  still one 20-21%; past 8.2 units 19% vs 17-18% -- the radar's effect and its 25 m range; turn 1.24 s. Revenge did NOT show
  there: next target = the killer 6% (chance 7%), back at the death spot 13% (baseline 10%), no faster approach to the killer.
- **Stats page: the players table above the event feed; our local server serves the page from the checkout** (2026-11-08 12:40;
  stats_page.html, native-dedicated.sh) — WHY: the user: "move all the players in the game, their k/d spread, their current
  weapon, kills, etc. ... above the event feed"; the dedicated server served the dylib's built-in copy, so page edits needed a
  server restart. LOGIC: the players panel moved before the feeds; native-dedicated.sh sets NHOOK_STATS_PAGE like native.sh.
- **walker-battle.sh: WB_IDS, WB_WATCH** (2026-11-08 12:10) — WB_IDS="1 2 3" picks the identities (leave test-v2 out when the
  user joins with NATIVE_REAL=1 NATIVE_ALLOW_SECOND=1 bash shim/native.sh --join-server 127.0.0.1:49190); WB_WATCH=1 opens the
  server's stats page (http://127.0.0.1:8767/) to watch. The battle-end cleanup follows the chosen identities.
- **engage-report.py** (2026-11-08) — the "How players fight" page from engage-model.py's report and engage-compare.py's outputs.
- **The players' behaviour drives the walker's radar look, revenge and power items** (2026-11-08 13:00; engage-extract.py "item"
  records, engage-model.py "behaviour", nbot_engage.c / nbot_goals.c) — WHY: the goal draw's weights were guesses. LOGIC: item =
  each power weapon / power-up spawn event: every player within 40 units -> approach speed toward it over 10 s vs a point as far
  another way, the first to come within 1.5 units (first numbers, 3 big Slayer matches, 69 spawns: someone reaches it in 94% of
  cases, median 4.7 s, the nearest at spawn only 25% of the time; heading in at 10-20 units 17% vs 11% baseline); behaviour =
  radar turn rates (moving / still, in / out of the 8.2-unit range) and delay, revenge rates vs chance / baseline, item heading-in
  shares. The walker: with no target, a new radar contact makes it look (1.5 s, walking on) with the players' odds (28%) after
  their delay (1.24 s x 0.5..1.5); at each respawn ONE draw: the killer with the players' excess over chance (at least 5%), else the
  death spot / the item's lane with theirs (at least 8%); a power item in range is the goal with twice the players' extra heading-
  in share at its distance per draw (x1.5 for power-ups, which walkers can take). engage-load-test.sh checks the parse.
- **No lanes from our local test server** (2026-11-08 13:00; nlanes.c ln_here) — WHY: the user's own client (the real profile)
  joined the walkers once and wore 3.9 km of walker moves into Guardian's real lanes. LOGIC: lanes are not recorded in a game on
  a 127.x server (NHOOK_LANES_LOCAL=1 = they are); guardian.lanes rebuilt from its 5 recordings (98.5 km).
- **The engine's motion tracker found** (2026-11-08 13:05; native-client.md "The motion tracker", nbot_goals.c rd_blips /
  rd_log) — WHY: the user: the walkers should "simply look at the radar's literal output, and check near players not every
  player". No string names it; found from the radar traits (player +0x214 / +0x216 reads) -> the "tracker shown" check
  halo3+0x2d349c -> its caller, the HUD draw, which passes the tracker state (S = [TLS +0xF0] + user * 0x28c8 + 0x244: range,
  10 history frames of up to 16 blips) to its draw. LOGIC: rd_blips reads user 0's newest frame; NHOOK_RADAR_LOG=1 logs it beside
  the truth to fit the sensor plane to the world; the walker keeps halo3's rules (rd_contact) until then.
- **Every public match recorded for the walkers** (2026-11-08 13:10; shim/native/ntracks.c, nstats.c, build.sh -lz) — WHY: the
  user: "any game played on a public server should be collecting stats for our walkers to hone themselves on in the future";
  only the observer's match-recorder.py (HTTP polling, a separate process) recorded. LOGIC: in a match on a server that is not
  127.x, every stats snapshot writes match-recorder.py's line format to tracks/<date>_<map>_<FNV tag>.jsonl.gz (header with the
  game engine and variant from st_variant, "source": "nhook", "you_plays": true -- "you" -1, so the local player counts as a real
  player), events from the feed ring with names mapped to slots (no names written), gzflush every 600 lines, closed on a new
  match / match end / exit. SWITCHES: NHOOK_TRACKS=0 off (observe-public.sh sets it: it runs match-recorder.py), NHOOK_TRACKS_DIR.
- **Weapon effectiveness** (2026-11-08 13:05; engage-extract.py "weapons" records, engage-model.py weapon_stats) — WHY: the user:
  "gather some stats about the most effective weapons too". LOGIC: per recording, seconds each weapon was held by live players on
  foot, kills credited by the game, kills by a player holding it, deaths of players holding it; the model: kills a minute held,
  K/D, duel win rate as attacker / defender, median time to kill (weapons held 3+ minutes). First table (non-Infection, 34
  matches): Mauler 5.9 kills/min, SMG 5.4, Energy Sword 5.0, Shotgun 4.4; best K/D Spartan Laser 2.80, Sniper 2.36, Carbine 1.90;
  sniper attackers win 92% of their duels; Assault Rifle / Battle Rifle ~1.0 K/D (the defaults); melee is the game's top kill
  credit (777), then the Assault Rifle (750) and Battle Rifle headshots (591).
- **Stats page: Live and Tools tabs; the event feed and chat beside the map** (2026-11-08 13:20; stats_page.html) — WHY: the
  user: "move the event and chat feed to where the tools are now. I think tools should just be its own tab like forensic is".
  LOGIC: header tabs Live / Tools / Forensic archive (the archive stays its own page); Live = game rules, the event feed + chat in
  the left column beside the map, Connected, the players table; Tools (#tools) = the in-game overlay panel (with recording, host
  and walker controls), hook cost, soundboard, hooks check, packets. Blocks moved whole (no ids changed); the map is redrawn when
  Live comes back. The observer leaves Infection out for now (observe-public.sh OBS_INFECTION=1 = back in; the user: 86 minutes
  of Infection biased the data).
- **observe-public.sh: the server going away ends the visit** (2026-11-08 13:40) — WHY: the server shut down mid-session ("the host
  left the game: back to the main menu"); the game sat in the main menu and the visit only ended at MAXSESSION (45 min): the 5-
  minute "no match" rule only covers a visit that never got into one. LOGIC: "back to the main menu" in the app log ends the visit
  and skips that server for the run ("[switch] leaving the game" is no sign: the app logs it on every join).
- **Medals in the event feed** (2026-11-08 13:50; stats_page.html) — WHY: the user: "can you add the medals feed to the event
  feed?" -- the snapshot's medal events (nstats.c rs_tick: the game results' medal counts going up, with the player's team) fell
  through to a plain "name: medal" text row, unstyled and with no switch. LOGIC: "<player in team colour> earned <medal badge>",
  a "medals" switch beside the others (remembered with them), the same medal for the same player within 0.5 s merged (x2 ...).
- **Per-map models, the tome, coverage-driven sampling** (2026-11-08 14:00; engage-model.py, engage-extract.py, nbot_engage.c,
  nbot.c, observe-public.sh, workboard.py, match-recorder.py) — WHY: the user: the archive "should probably also refine itself to
  per map stats too. both per map lanes and logic, as well as a larger aggregated everything general logic, and the walkers should
  be able to adapt learned logic for each map ... sample each map at least once ... keep track of all the maps you've played, and
  under what game types ... archived in our tome". LOGIC: engage-model.py builds one model per subset (make_model): the aggregate,
  each map with 150+ duels (tracks/maps/<map>.engage.json) and inside it each game type with 150+ ("by_class"); tome.json from
  engage-extract.py's new per-recording meta records (map, type, length, players, kills, start) -- per map x type recordings,
  minutes, players, duels, kills, first / last, plus the Halo 3 maps never sampled. The walker: eg_load_for(map, engine) = the
  aggregate, then the map's numbers, then its game type's, wherever present (a map's distance band from 20+ of its duels); reloaded
  on a map / game change; the map's movement pieces first (EgPiece.mine). NHOOK_BOT_NO_MAPMODEL=1 = aggregate only. The observer's
  pick adds +25 players for a map x type the tome has no minutes of, +12 under 10 (listing names -> maps by display name or the
  first 5 letters); OBS_DONE carries a phase's count across a restart. Game type is classified by the engine (match-recorder.py
  now writes "engine"; nhook's recorder's "mode" is it), then the variant's name, then the listing (a "CTF 30VS30" server's
  "BIGASSSLAYER" was filed as CTF). The work board shows the direction and the tome's coverage grid (maps x types, per-map model
  or aggregate, never sampled).
- **Teamwork: measured in team games, used by the walkers** (2026-11-08 14:30; engage-extract.py "team" / "aid" records,
  engage-model.py behaviour.teamwork, nbot_engage.c, nbot_goals.c, nbot.c) — WHY: the user: the radar learning should note "both
  enemy and friendly radar blips, and how they may be cooperative working together in the moment, and learn to also potentially
  cooperate with other team mates toward a common offense, or if one comes to your aid, ensure you are working together for your
  and his collective defense as a unit". LOGIC: per team-game recording the nearest teammate's distance (all / while fighting),
  the share of time with one within 8 / 15 / 25 units, focus fire (attackers whose target a teammate attacks too); per NEW attack
  on a player with teammates within 25 units: aided by fire (a teammate attacks that enemy within 3 s) or by moving in (1+ u/s
  toward the player for a second), how fast, and the 5-s death rate helped vs alone; the model's teamwork = spacing, within15,
  focus share, aid / join odds and delays. The walker, with no target held: a teammate within 25 units under fire (hurt in the last
  second while an enemy aims at it) -> aid with the players' odds after their delay (the attacker as target when known, else a
  goal to the teammate); a teammate shooting an enemy -> join with their odds and delay; once per (teammate, enemy) per 5 s; that
  teammate is the unit partner for 12 s. The goal draw adds the nearest teammate (weight 2 x within15, the unit partner 4),
  followed as it moves, reached at the players' spacing. Free-for-all has no teammates: nothing changes there.
- **What the game is learning** (2026-11-08 14:40; shim/tools/learnings.py, workboard.py) — WHY: the user: "add one more service that
  tells what the game is learning about everything from these stats? best lane to not get killed on a map, best weapon overall,
  best weapon per map, best tactics in each situation 1:1 1:3, distance shots with hitscan weapons, grenade tactics, jumping
  tactics, strafe tactics, team cohesion, best way to survive vs specific weapons ... add one more data display to
  http://127.0.0.1:8790". LOGIC: learnings.py reads the engagement library, lanes, deaths heatmaps and spawn points and writes
  plain-language findings with their sample sizes (under --min 30 cases = "thin") to tracks/learnings.json: the fundamentals (first
  shot, facing, shield lead, high ground), weapons (K/D and kills a minute held overall, the best per map), range (win rate by
  distance per weapon class), outnumbered and teamwork, movement in a fight (strafe / jump / push terciles), grenades, surviving a
  specific weapon (by distance, strafing, jumping, the attacker's firing pause of 1.2+ s as the reload stand-in), maps (the busy
  4-unit squares with the fewest / most deaths per 100 crossings, named by the nearest spawn; fight distance), radar / revenge /
  power items. The work board's "What the game is learning" panel shows it; a loop re-cuts new recordings, refits the models and
  the tome and rewrites the findings every 20 minutes while sampling runs. The board has tabs like the stats page (the user: "make the
  results a tab on the work board page ... just like we use tabs on the stats service"): Work (direction, jobs, tasks, data,
  coverage, observer) and Learnings (#learnings).
- **You vs the winners** (2026-11-08 15:00; ntracks.c, engage-extract.py, shim/tools/coach.py, workboard.py "You" tab) — WHY: the
  user: "theoretically if I played a few matches you could tell me what I do wrong vs what the winning population of halo that
  you've observed does eh?". LOGIC: nhook's recorder writes the local player's slot as "you" (its header's you_plays says it is a
  real player: engage-extract.py keeps it in -- an observer's "you" stays out -- and names it in the meta record's local_slot);
  engage-extract.py adds one "player" summary per player of a match (kills, deaths, time, shots, hits, team) and the player's slot
  to the contact / pressure / respawn / aid records (aid: the helper and the teammates near). coach.py: the WINNERS = the top
  quarter by (kills + 1) / (deaths + 1) of the player-matches of 5+ minutes outside Infection (you left out); per measure your
  pooled number, the winners', everyone's, a verdict and the winners' way where you are behind, biggest gaps first: K/D, duels won,
  shot first, caught looking away, turn-back time, aim error, strafing when shot / attacking, closing in, jumps and grenades a
  fight-minute, turning to radar blips, dying outnumbered, help given / got, fight distance by weapon class. --pretend <file>
  <slot> tries it on a recorded player. tracks/coach.json; the work board's You tab.
- **Whole matches: a recording from the match's start counts** (2026-11-08 15:05; match-recorder.py, shim/tools/observe-whole.py,
  observe-public.sh) — WHY: a visit's first recording never counted ("began when we joined, maybe mid-match"), but after a restart
  the observer usually joins as a match begins: a 22-minute Valhalla match from its start was thrown away and phase 1 crawled (2 of
  10). LOGIC: the recorder writes the game's match clock at a recording's start (header match_time_at_start, its log line "(match
  time N s)"); observe-whole.py counts a finished recording of 1500+ samples when it is not the visit's first or began under 90 s
  into its match (old logs without the clock: as before).
- **The walkers coach themselves** (2026-11-08 15:20; nbot.c, nbot_goals.c, shim/tools/walker-coach.py) — WHY: the user: "the
  walkers can use the coach on themselves eh!?". LOGIC: coach.py's report is made for every walker of a battle (its slot -> the
  test identity from the battle's samples; names never written) against the WINNERS of the same game class on the same map (40+
  player-matches), else that class on every map, else every match; each measure moves one knob half its gap in log space (x2 /
  x0.5 at most a round, 8+ samples): closing in -> NHOOK_BOT_RADSCALE (new: x the replayed closing / backing off), aim error ->
  NOISE, time to aim back -> REACT, strafing when shot / attacking -> LIBSCALE, jumps a fight-minute -> NHOOK_BOT_FJUMP (new: extra
  jumps while in sight of the target, Poisson), grenades a fight-minute -> NHOOK_BOT_GRENX (new: x the time between throws),
  turning to radar contacts -> NHOOK_BOT_RADARLOOK (new: x rd_turn), fight distance by weapon class -> DIST (same-map winners
  only: distances belong to their map). Measures with a better direction (aim error, aim-back time, radar turns) move a knob only
  while the walker is behind: the goal is to beat the players, not copy their misses. `walker-coach.py loop [--rounds N] [--secs S]
  [--n 2..4] [--map M] [--game G] [--teams]` = battle (walker-battle.sh with W<k>_ENV from the knobs, its own tracks folder) ->
  coach -> next battle; `round <tracks> <samples>` coaches one battle (--dry: no change), `show` prints the knobs and rounds.
  Per round: K/D, duels won, shot first and the distance to the winners (coach.py's gap averaged; 0 = their numbers) per walker in
  ReclaimerMac/battles/coach/state.json. New knobs default to the old behaviour (1, 1, 0, 1).
- **The walker knows its inventory and power-ups** (2026-11-08 15:30; nbot.c bot_inv, nbot_engage.c, nbot_goals.c, engage-model.py
  behaviour.powerups) — WHY: the user: "the walker should be aware of its own inventory, and power up status as well". LOGIC: every
  tick bot_inv reads the walker's own unit: the held and the other weapon (class + rank), grenades by type, an overshield (shield
  above 1.05) and active camo (+0x360 above 0.05); `[inv]` log lines on a change. The fighter: switches weapons (vpad Y, 0.15 s, 3 s
  apart) when the target is 30%+ outside the held weapon's players' band and well inside the other's; throws only with a grenade in
  hand; under camo holds fire beyond the band's near edge + 2 units unless hit in the last 2 s and walks in; reloads after a fight in
  which it fired (vpad RB tapped 0.4-1.0 s after; no ammo count is read yet); picks up on the vpad too (RB held 0.6 s,
  NHOOK_BOT_PICKPAD = another button). Target choice: an overshielded enemy costs half the players' logit (attackers win 54% against
  one vs 77%, behaviour.powerups, 35 duels) unless the walker has one; a camo'd enemy is not seen in view beyond 8 units (radar,
  attacks and close range still tell). Goal draw: fights weigh x2.5 with an overshield / camo (defenders with an overshield survive
  twice as often), an overshielded enemy half; no power-weapon goals with two power weapons in hand, no power-up goals with one
  running. NHOOK_BOT_NOINV=1 = blind to it (the old walker). behaviour(R, D) now gets the duels (it got none: powerups was empty).
- **Recordings carry camo and the second weapon** (2026-11-08 15:32; match-recorder.py, ntracks.c, nstats.c) — WHY: the walker's
  inventory logic should be learnt from players, and camo was never recorded. LOGIC: two columns more per player sample: 17 = the
  active camo amount, 18 = the other weapon carried ('' none; the stats page's weapons list minus the one in hand). Old readers
  index up to 16 and are unchanged.
- **One game against what we assumed** (2026-11-08 15:35; shim/tools/game-review.py) — WHY: the user: "coaching yourself based on
  the dataset you JUST collected for that game vs the assumptions you made about the map, and the larger aggregated data". LOGIC:
  for one cut recording, the same measures over the game, over the map's earlier matches (same class) and over every earlier
  match of that class: first-shot wins, caught looking away, high / low ground, wins and distance per weapon class, radar turns,
  dying outnumbered, help, items, back to the death spot, kills a minute, the winners' K/D line, kills a held-minute per weapon and
  coach.py's fight measures; a SURPRISE vs the map = shares 2+ standard errors of a game this size, other numbers 30%+ (15+ cases).
  tracks/reviews/<recording>.json (+ team_game); the work board's Walkers tab shows the latest.
- **The learn loop** (2026-11-08 15:38; shim/tools/learn-loop.py, observe-public.sh OBS_RANDOM, walker-coach.py --game-file /
  --ded-map) — WHY: the user: "start randomly joining and completing games ... in order to instruct the walker how to play the game
  in question. iterate with the goal of better vs human scoring". LOGIC: a turn = observe-public.sh with OBS_MATCHES=1 OBS_RANDOM=1
  (the server DRAWN with weights exp(score / 8) instead of the best score; no Infection / mods / downloads) -> engage-extract ->
  game-review -> engage-model (aggregate, per map, tome) -> on a stock map, walker-coach.py loop on that map (the server gets the
  display name: riverworld -> Valhalla; team games and CTF as Team Slayer 2v2, the walkers have no flag play) against THIS game's
  winners (top quarter of 8+ players, weight 2) blended with the map's / class's winners (weight 1) -> tracks/learn-loop.json. The
  observer only watches: walkers stay on our own server (bot_allowed); "better vs human scoring" = the walkers' distance to the human
  winners going down. walker-coach.py --dry no longer saves its round.
- **Work board: Walkers tab** (2026-11-08 15:40; workboard.py #walkers) — the latest game review (this game / the map / everything,
  surprises), the learn loop's turns, each walker's knobs and its coached rounds (distance to the winners, K/D, duels won); new job
  kinds learn-loop / walker-coach / game-review / coach.
- **You can play the observer's game** (2026-11-08 15:45; nvpad.m nh_takeover_install, nhook.c nh_takeover_fire, observe-public.sh,
  match-recorder.py --you-plays) — WHY: the user: "can you make it so that IF I choose to pickup the controls while you sample
  public games I can play? it seems to only capture the mouse and not the joystick right now". The observer runs the app with
  --background (the app's test mode: a 640 x 360 window below the others, never the frontmost app; without it the real profile's
  borderless full screen would take the display), and the GameController framework delivers a real controller's input only to
  the frontmost app. LOGIC: NHOOK_TAKEOVER=1 (the observer sets it) arms a local mouse-down monitor; the first click in the game's
  window turns on +[GCController shouldMonitorBackgroundEvents] (macOS 11.3+), activates the app, puts the window at the normal
  level and makes it 1600 x 900 (90% of the screen at most) when smaller; nh_takeover_fire turns the sound back on (the mute tap
  checks g_takeover) and writes NHOOK_TAKEOVER_FILE. observe-public.sh: with that file the visit lasts until the user quits the
  game (no leaving after a whole match, no time limit, no 5-minute no-match leave), the recorder restarts with --you-plays (header
  you_plays: the user's slot is a real player: coach.py's You tab). Not live-tested with a controller yet (the user's next
  take-over is the test).
- **The plan runs unattended** (2026-11-08 15:46; shim/tools/plan-run.sh) — sample-large (OBS_DONE from PLAN_DONE_LARGE) ->
  sample-small -> refit -> spar-ffa (walker-coach.py, 2 coached rounds, radar log) -> spar-teams (2v2) -> firefight (one walker on
  Courtyard; walkers together in Firefight need co-op joins, not done) -> learn (learn-loop.py); tracks/plan.txt; `plan-run.sh
  <step>` starts at a step. The observer's dylib is a frozen copy (NATIVE_DYLIB) refreshed when no game runs.
- **Kills by weapon** (2026-11-08 15:55; shim/tools/weapon-kills.py, workboard.py Learnings tab) — WHY: the user: "running kills
  counts to each weapon in playertable like arrangement. add vehicle guns too ... grenades should also be accounted for. scrape the
  history ... see the per game progression any time I go to the learnings tab". LOGIC: every public recording's kill events (the
  game's own feed: how = weapon / vehicle / gunner seat / grenade / melee; ", headshot" = a headshot of that weapon; betrayals
  apart; our local servers' recordings left out) summed per weapon per match -> tracks/weapon-kills.json (games oldest first, live
  = recorded in the last 2 minutes; per weapon its kind, kills per game, headshots, betrayals); a per-file cache (size + time)
  makes a rerun read only new / growing files, and the work board reruns it every minute (wk_loop). The table: class chips (all /
  slayer / ctf / infection / other), kind chips (hide a kind), sortable columns (kills, share, headshots, games, kills a game, best
  game, last game), a per-game line; a click opens the weapon's kills game by game (bars named by map, the live match amber);
  choices kept in localStorage. First count: 26 matches with kill events, 13,632 kills, 52 weapons.
- **Weapon switches and camo learnt from players** (2026-11-08 15:50; engage-extract.py inventory(), engage-model.py
  behaviour.inventory, shim/tools/inventory-extract-test.sh) — from recordings with columns 17 / 18: "wswitch" records (the held
  and the carried weapon trade places: classes, nearest enemy's distance, in a fight) and "camo" records (a stretch under camo:
  length, share of it firing, first shot's distance, kills, died in it); the model sums switches by class pair (n, median distance,
  in-fight share) and camo use. Offline test: a synthetic recording (one BR -> shotgun switch at 6 units in a fight, one 3 s camo
  stretch firing at its end; an old 17-column recording gives none): all PASS.
- **Finding the ammo field; the walker uses it** (2026-11-08 16:10; nstats.c NHOOK_WPNDUMP, shim/tools/ammo-scan.py, walker-coach.py,
  nbot.c bot_ammo_off / bot_ammo_of) — WHY: the walker's inventory without ammo reloads only after fights. No HaloScript getter
  gives a weapon's rounds (unit_get_total_grenade_count exists, nothing for ammo), so the field is found from data: NHOOK_WPNDUMP=1
  logs the local player's held weapon object (0x300 bytes) with its shots counter every snapshot; ammo-scan.py scores every 16-bit
  field (drops while the shots counter rises, jumps up on a reload with nothing fired; reserve = drops on a reload by about the
  magazine's gain) and writes ReclaimerMac/native/ammo.json {"loaded", "reserve"} when one field wins on 2+ weapons or 3+ reloads.
  walker-coach.py adds NHOOK_WPNDUMP=1 to the walkers while ammo.json is missing and scans their logs after the battle (the plan's
  first coached battle finds it). The walker (ammo.json or NHOOK_AMMO_OFF=<hex>): rounds loaded + the most seen per weapon
  definition; an empty magazine in a fight with a loaded other weapon = switch (vpad Y); out of a fight under 60% = reload (RB tap);
  after a fight only when not full. Unknown field = the old behaviour.
- **The take-over bridges the controller through the virtual pad** (2026-11-08 16:25; nvpad.m tk_bridge, nhook.c) — WHY: the
  user, after the first take-over: "I still can't control an observer, it's only mouse control". The click worked (the log: the
  user has the controls, background events on, window raised) but the app enumerates +[GCController controllers] once, early,
  and a --background app finds no physical controller then; it never reads one later. It does read nhook's virtual pad in
  --background clients (the walkers). LOGIC: with NHOOK_TAKEOVER the virtual pad is installed at start (at rest until the click;
  the user drives it, no bot does: bot_allowed() still refuses the real profile and public servers); the click attaches a
  valueChangedHandler (serial queue) to every physical controller (+controllers' original list without the virtual pad, connect
  notifications, a 2 s rescan) that copies sticks, triggers, A B X Y, bumpers, stick clicks, menu / options and the d-pad into it
  (nh_vpad_set). Log: "[takeover] your controller (<vendor>) drives the game through the virtual pad".
- **The walker's weapon names never matched natively** (2026-11-08 16:30; nbot_engage.c bot_tagkey, eg_wcls, nbot.c bot_wpn_rank,
  nbot_goals.c gl_power_tag) — WHY: found while adding weapon zones: natively hb_tag_name gives the tag's last path part in
  CAPITALS with spaces ("ASSAULT RIFLE", h3maps.c hb_tag_name_l), and every walker lookup was written for "assault_rifle": every
  weapon was class "other" (no per-class distance band from the players: the default band for everything), no pickup ranked
  better (rank 50 for all), no lying power weapon counted for the goal draw. LOGIC: bot_tagkey lowercases and turns spaces into
  underscores before each lookup. No switch: the old behaviour was a bug.
- **Weapon zones: the walker picks its weapon by where the target stands** (2026-11-08 16:35; weapon-kills.py -> tracks/maps/
  <map>.wzones.txt, nbot.c wz_load / wz_eff, shim/tools/wzones-test.sh) — WHY: the user: "walker should use its best weapon based on
  situation that can be derived from game observations of where kills happen with certain weapons. if opponent is in a certain zone
  and you have a certain high kill weapon for that zone and aren't using it switch to it and use it". LOGIC: weapon-kills.py counts
  every recorded match's kills (Infection left out) by the victim's 6-unit cell and the killer's weapon class (the walker's classes:
  precision, auto, sniper, shotgun, rocket, melee) and each class's share of the weapons carried on foot on that map (the samples),
  and writes them per map as text. The walker loads its map's file (with the engagement models); for the target's position the 3 x 3
  cells around it give each class's kill share there over its carried share; in a fight it switches (vpad Y) to the other weapon
  when that ratio is 1.5x the held one's and above 1.2 with 8+ kills around, and keeps a weapon the zone favours 1.5x even when the
  distance band would switch away (an empty magazine still switches first). Recounted every minute with the kills table.
  NHOOK_BOT_NOZONES=1 = off, NHOOK_WZONES_DIR = another folder. Offline test (the real C cut out of nbot.c): all PASS. First
  files: 9 maps, Valhalla from 2,470 kills.
- **Take-over at full resolution, and from the stats page** (2026-11-08 16:45; nvpad.m tk_take / nh_takeover_now, nstats.c
  do=takeover, nhook.c tk_size_watch) — WHY: the user: "relaunch the next match and ensure I can have control with the joystick and
  have full resolution available". LOGIC: the take-over window fills the screen's visible area (it was 1600 x 900 at most), stays
  resizable and titled, and allows native full screen (collection behavior FullScreenPrimary: the green button / Ctrl-Cmd-F);
  POST /control?do=takeover does what the click does (only with NHOOK_TAKEOVER: the observer); after a take-over the game's
  presented frame size is logged whenever it changes ("[takeover] the game draws W x H": does the game follow the window?).
- **Taking over = relaunching the game in normal mode** (2026-11-08 16:35; observe-public.sh launch(), OBS_PLAY_FIRST, nvpad.m
  tk_take, nhook.c, plan-run.sh) — WHY: the user: "still no control and the observer window scale resolution is garbage you broke
  my ability to control the game somehow". Measured: a --background game renders 640 x 360 whatever its window becomes (the
  filled-screen window showed a stretched 640 x 360 picture), and the bridge's virtual pad never got the controller's input (the
  physical controller was found, but no input reached the background app); the virtual pad installed at start in the user's
  observer game is gone too. LOGIC: a click in the observer's window only writes the take-over file; observe-public.sh then kills
  the background game and launches it in NORMAL mode on the same server (no --background, no NHOOK_TAKEOVER, sound on: the user's
  own settings -- borderless full screen at the display's resolution -- and the controller read by the app itself: the first
  normal launch logged "[menus] input device: controller"); the visit lasts until the user quits, recorded with --you-plays.
  OBS_PLAY_FIRST=1 = the first visit is the user's from the start. Another Project Reclaimer appearing (the user's own game) no
  longer ends the observer (exit 1 made plan-run.sh skip to the next step, whose own check refused too): its game leaves and it
  waits until that one quits; plan-run.sh waits before every step while a game runs. NHOOK_TAKEOVER_BRIDGE=1 = the old in-place
  take-over (window + bridge), kept for experiments. OBS_PLAY_FIRST is used once per process group ($TR/.play-first holds the group that used
  it): the plan's later observer steps and learn-loop turns inherit the variable and would each have opened a full-screen game
  waiting for the user.
- **Learnings: weapon zones and inventory** (2026-11-08 16:55; learnings.py) — "Weapon zones": per map up to 3 spots (non-overlapping,
  named by the nearest spawn) where one weapon class makes 25%+ of the kills on players within ~9 units and 2.5x+ its share of the
  weapons carried there (20+ kills; the walker's tracks/maps/<map>.wzones.txt), e.g. The Pit: shotguns 25% of 119 kills at 3% of the
  weapons carried (8.2x). "Inventory: switches and camo": the players' most common weapon switches (class -> class, the nearest
  enemy's median distance, the share in a fight) and their camo stretches, from engage-extract.py's wswitch / camo records.
- **The walker switches like the players** (2026-11-08 17:00; nbot_engage.c eg_parse behaviour.inventory.by_pair, nbot.c) — for a
  held -> other class pair with 20+ recorded player switches, the walker switches toward the shorter-range class (the players'
  distance band medians decide which is shorter) when its target is closer than the players' median nearest-enemy distance at
  that switch, toward the longer one when farther (first data: sniper -> precision 10 units, precision -> sniper 11: a small
  hysteresis, plus the 3 s between switches). Order: empty magazine, weapon zones, the players' switch, else the distance bands.
- **The bullet log and the spread ("bloom") analysis** (2026-11-08 17:20; noverlay.c nov_slog_*, shim/tools/bloom.py, bloom-test.sh,
  bloom-battle.sh, native-dedicated.sh DED_OPTIONS / NHOOK_ROLE) — WHY: the user: "what can you tell me about the 'random bloom' on
  both the AR, and the BR? ... do they seem random? any disadvantages or flaws under certain situations?". Nothing measured a
  bullet's direction before (tracers on screen, shots / hits counters). LOGIC: NHOOK_SHOTLOG=<file> (%p pid, %r NHOOK_ROLE: "server"
  on our dedicated server, else "client") logs every deleted bullet from the object_delete hook with its owner's aim vector (+0x1a0),
  velocity (+0x74) and held weapon at that moment, its start (+0x1e8), end (+0x1c) and distance travelled (+0x1b0), the estimated
  victim; a 16384-record ring written by a thread every 0.5 s (the engine thread only copies). bloom.py: each straight bullet's
  angle off the aim in the aim's right / up frame; per weapon median / p90 / max, the bias, the shape (KS against a uniform disc and
  a Gaussian), the error by the bullet's index in a string of fire, by burst position, by time since the last burst, by ground
  speed, airborne, distance, hit / miss; the correlation of consecutive bullets' errors. Offline test (a synthetic AR cone growing
  1 -> 4 deg over 15 rounds, a BR Gaussian with a hidden +0.5 deg up on every third round): all found. bloom-battle.sh: 4 walkers on
  our server with BR + AR starts (DED_OPTIONS '{"player.primary_weapon": 0, "player.secondary_weapon": 1}' -> a one-game playlist
  with those options), the server's log (authoritative) and the clients' analysed apart.
- **How random are the spawns** (2026-11-08 17:25; shim/tools/spawn-patterns.py) — WHY: the user: "pay attention to the randomness of
  the spawn patterns too ... patterns in either would be interesting". LOGIC: every respawn in the recorded public matches (a
  player alive again after a death; Infection left out) is matched to the map's spawn points (respawn positions merged within 2
  units, seen twice or more) and compared with every other point at that moment: log distance to the nearest live enemy, enemies
  within 15 units, log distance to the nearest teammate (team games), log distance to the player's own death spot, used in the
  last 10 s; a conditional logit per map and per class, its top-1 / top-3 hit rate against chance, how often the farthest point
  from enemies was chosen, the same point as the player's previous spawn, the spread of use (entropy over uniform).
  tracks/spawns.json. First run (16,110 respawns): free-for-all -- the model's first pick is the chosen point 26.5% (chance 2.5%),
  top 3 50% (7.5%), nearest enemy +3.85 (by far the strongest), own death +0.84, just used +0.92; team -- 37% (2%), 68% (6%),
  nearest teammate -0.85 (near them), just used +1.74 (teams spawn together); the farthest point from enemies only 4-32%; every
  point used (0.87-0.94 of uniform).
- **My analysis jobs no longer compete with the user's game** (2026-11-08 17:40; shim/tools/refresh-loop.sh) — WHY: the user: "something
  you did is making input feel choppy": the 20-minute learning refresh's model refit ran at ~90% of a core for minutes beside the
  user's full-screen game (that stretch: 49-52 fps, 255-342 ms frames). LOGIC: the refresh runs every step under taskpolicy -b + nice
  20 (efficiency cores, below everything) and WAITS while a Project Reclaimer runs without --background / --server (the user playing).
  Run the work board the same way (taskpolicy -b). The remaining once-a-second 150-300 ms frame in the user's full-screen game is NOT
  any hook: aim meter, stats snapshots / recorder and voice hook switched off live changed nothing; the hooks cost table and a 5 s
  sample show nhook cheap on the game's threads. (Corrected 19:20: the 640 x 360 background games did NOT stay flat -- their worst
  frames grew too, to 40-60 ms in the 60-90-player lobbies; see "Finding the full-screen stutter" below.)
- **The observer picks for coverage, not size** (2026-11-08 17:50; observe-public.sh pick) — WHY: the user: "you have way too many
  valhalla observations vs say sandtrap or snowbound": the bonus only rewarded a map x type with nothing recorded and never held back
  one with plenty, so the one big mod-free server (Valhalla / The Pit / Guardian rotation) won 6 of 9 joins. LOGIC: coverage +40 none,
  +25 under 10 min, +10 under 30 min, minus a point per 3 minutes already recorded (at most -30); players count up to 16 only; a server
  already visited in this run -10 a visit; a map x type under 10 minutes may be joined with 4+ players even in a strict (12+) phase.
  Live check: the big server (24/24) lost to Sandtrap Team Snipers (13 players, no Sandtrap Slayer recorded) and, for small games,
  Foundry (never sampled).
- **The walker melees like the players, and pulls the trigger on single-shot weapons** (2026-11-08 19:15; nbot.c mel_load /
  bot_pull_weapon / the fight's melee block and rush, shim/tools/melee-model.py -> tracks/melee.json) — WHY: the user, watching a
  walker hold a Gravity Hammer on an enemy at 0.6-1.3 units ("FIRING" in its log the whole time) without a swing: "clearly you never
  trained on melees sooooo many opportunities for the walker to melee". Melee is the second biggest killer in the recordings (3,865
  kills; the battle rifle 6,206). Two faults: the walker HELD the right trigger for the whole fight -- the hammer, the sword and
  single-shot weapons act on a pull, so it swung / fired once at most (the bullet log shows the battle rifle and carbine firing on
  while held: bursts every 0.25-0.35 s, so those stay held) -- and its melee button was down for ONE 33 ms step. LOGIC: melee-model.py
  measures every "Melee" / "Energy sword" / "Gravity hammer" kill in the public recordings (Infection out): killer -> victim distance
  at the kill, 1 s and 2 s before, the victim's shield 0.5 s before (beatdown = under 0.05), the killer's weapon class -- anchored at
  the victim's LAST LIVE SAMPLE: a kill event's own time comes from the game's results (polled) and lags the death by up to ~1-2 s
  (anchored at the event, only 419 of 3,836 melee kills had a live victim and every "1 s before" was shifted). 43 matches: melee at a
  median 0.65 units (p75 0.97, p90 1.23), the killer closing ~2.1 units in the last second (a median 2.9 apart 1 s before); 48% finish
  a victim whose shield was already down (closing in from 3.2 units, p75), mostly assault-rifle carriers (2,649 of 3,836); sword p75
  1.28, hammer p75 1.33, mostly against full shields. Offline test: shim/tools/melee-model-test.sh. The walker: B within the players' range + 0.25 (positions
  are a tick or two old; hammer / sword: their own range), held 0.12 s, at most every 0.6 s, logged ([melee]); a target whose shield is
  down within the rush distance -- or any target within 10 units with a sword / hammer in hand -- is walked straight at (throttle
  0.9, strafe x0.3; not with a rocket launcher); the trigger is pulsed 0.12 s down / 0.12 s up while firing a pull weapon (hammer,
  sword, sniper, beam rifle, magnum, rockets, brute shot, fuel rod). NHOOK_BOT_NOMELEE=1 = the old melee (1.8 units, one step, the
  trigger held); NHOOK_MELEE_FILE = another melee.json.
  THE BUTTONS (20:45): in four melee battles (melee-battle.sh, 4 walkers with Gravity hammers) no kill was ever a melee -- 333 presses
  held 0.3 s, a walker "swinging" 30 times at 0.33-0.6 units while its target's shield fell and came back -- and the hammers kept
  turning into floor weapons with pick-ups off. The app reads the virtual pad 26 times a second (nvpad.m read counters: B, Y, RB,
  the right trigger, the left stick 257 times in 10 s each, logged as [vpad] lines), so short presses were seen; but 10 of the
  weapon changes followed a B press: these profiles' layout is Bumper Jumper's ("LB jumps and RB melees ... B acts and reloads",
  the app's own description), not Halo 3's own (B melees). The walker pressed B to melee (= pick up) and held RB to pick up / tapped
  it to reload (= melee). Now melee = RB (NHOOK_BOT_MELEEPAD, hex nvpad bits), pick up and reload = B (NHOOK_BOT_PICKPAD).
  NHOOK_BOT_FIRE=0 now also stops the fighter's trigger (melee-battle.sh MELEE_ONLY=1: no shots, no grenades -- every kill a melee).
  STILL OPEN (20:55): a MELEE_ONLY battle with hammers (no shots, no grenades, melee on RB) -- 503 swings at a median 0.54 units,
  ZERO kills. Neither B nor RB nor the pulsed trigger has ever landed a walker melee (no "Melee" kill in any of the 22 walker
  battles; their 78 unnamed kills are grenades at 6-15 units). B does act (pick-ups), so the pad's buttons reach the app; melee
  does not reach the server, or is not in the control packet the walker's packet hook passes on (halo3+0x50fb8 packet "A": button
  bits 0-2, 11, 14 + 7 more at +0x2c). Next: a button scan on our local server -- one walker beside a standing one, each pad
  button and each control-record bit pressed alone for 0.3 s, the standing one's shield read from the server's stats.
  FROM BEHIND (21:10; melee-model.py, the user: "especially if the opponent's back is to it"): each melee kill's attacker angle off
  the victim's LOOK direction (the recordings' yaw = unit +0x1a0; behind = over 110 deg): 19% of melee kills come from behind (sword
  8%), half of those on a shield of half or more (assassinations); they close in from 3.3 units (p75, 1 s before). Opportunities:
  every 0.5 s, an enemy within 5 units looking away from a player (one per pair per 3 s), taken = that player's melee kill of it
  within 3 s: within 1.5 units 1,419 times, taken 20.4%; 1.5-3 units 5,096, 5.5%; 3-5 units 19,265, 3.0% -- players miss four of
  five backstabs at arm's length. melee.json walker.back_rush_from (3.31). NovObj.aim (noverlay.c): a unit's look direction for
  the walker. THE SCAN (shim/tools/melee-scan.py; do=botpad = virtual-pad buttons held for a time, do=botpkt = bits ORed into the
  packed control record -- packet "A" carries only action +8 bits 0-2, 11, 14 and +0x2c's 7 bits): on our server, A teleported
  1 unit beside B (tpnext), A's sparring aim on B (0.0 deg error at 1.0 unit), each input 0.3 s, B's shield from the server's
  stats: all 13 pad buttons x2, all 12 packet bits x2 -- no damage. BUT the positive control (--no-pads --bits 19: the fire
  bit) did no damage either: the measurement itself is broken (B's server-side shield / A's position or fire in that setup), so
  those negatives prove nothing yet. Being fixed next.
  21:40: the scan's own fault found halfway -- by hand (melee-scan.py --hold, then do=botpad / do=botbtn): the pad's right trigger
  held 3 s fired NOTHING (0 shots on A's own client), the control-record fire bit 19 held 3 s fired 31 shots on A's client while
  the SERVER counted 0 and B's shield stayed full: in this setup (sparring aim only, no fighter) a joined client's shots never
  reach the server, so no scan result there means anything yet (in walker battles -- fighter on, pad + bit together -- shots do
  land: 117 assault-rifle kills). Next: the same probe with the fighter's path. TURNED BACKS (same hour, the user: "especially if
  the opponent's back is to it"): the walker runs straight at a target looking away (its aim, NovObj.aim, over 110 deg off the
  line to the walker) within melee.json's back_rush_from (3.31 units) and presses melee there; it keeps shooting meanwhile (no
  walker melee lands yet). NHOOK_BOT_NOBACK=1 = off. melee-scan.py --hold / --debug / --rt-control / --no-pads.
- **A walker no longer stands still going for a weapon it cannot reach** (2026-11-08 20:20; nbot.c pickups) — WHY: in a melee test
  battle one walker stood the whole 3 minutes logging "going for BATTLE RIFLE (8.1 units)" (moved 0 units): a pickup target was only
  marked tried once reached. LOGIC: a target that brings no progress (0.5 units closer) in 6 s is given up and marked tried ("gave up
  on ..."). The tried list's check read past its 32 entries once 32 pickups had been tried (now bounded). NHOOK_BOT_NOPICK=1 = no
  pickups at all (melee-battle.sh: the walkers keep the start weapon -- they swapped the hammer for a shotgun / sniper rifle).
  Also: no reload tap while holding a sword / hammer (nothing to reload; the tap picked up the weapon on the floor: the test walkers
  swapped their hammers for plasma rifles / SMGs after fights). melee-battle.sh lists every kill (type, the killer's weapon, the
  distance at the victim's last live sample): (WRONG, corrected 22:30: our server names melee kills "Melee" -- the user's five were;
  "" are other kills) an unnamed kill within 4.5 units counts as melee. First battle (4 walkers, hammers, 3 min, before the reload fix): 95 melee presses at 0.7-1.2 units, 3 melee /
  hammer kills of 7 (one hammer kill from 4.2 units: its shockwave).
- **Coached up after the user's 17-4 win over two walkers** (2026-11-08 22:30; nbot_engage.c EgUnit / second attacker / target
  score, nbot.c the walker's unit, shim/tools/match-study.py) — WHY: the user: "the bots should study that ass whipping I just gave
  them! there are some things I notice they fail at. meles and just generally shooting first ... some times they stand around".
  match-study.py on that match (Guardian, 562 s; the user 17 kills / 4 deaths, 54% accuracy; the walkers 7/11 and 5/16, 39% / 31%):
  in fights (two enemies within 40 units, someone firing) the walkers fired FIRST as often as the user (28 / 28 / 26 of ~55) but,
  shot at first, replied after a median 4.4-5.5 s against the user's 1.1 s, and 14-22 times never; the user won 24 fights and lost
  7. Their logs: "second attacker ...: staying on the target" 19 times, "turning to it" 3 -- the players' odds (33% in view, 7% from
  behind, after ~1.75 s) and a target score where being behind (off aim -7.74 at 180 deg) outweighs having just shot us. One
  walker stood still 21% of its alive time in 14-17 s spells: "the record's unit ... is none of the units seen" ~20 times -- after a
  respawn the control record's unit stays stale and the walker idled. LOGIC: a second attacker hurting the walker while its current
  target is not shooting at it is turned to (the players' odds, at least 0.5, when both shoot), asked again every second while it
  keeps hitting, after half the players' turn delay (NHOOK_BOT_TURNX 0.5); a new target that shoots at the walker gets +6 on its
  score (NHOOK_BOT_ATTBONUS); NHOOK_BOT_HUMANLIKE=1 = the players' rules as before. The walker's body falls back to the player's own
  unit (players table, by the profile's nickname: nstats.c g_my_unit) when the record's unit is stale (NHOOK_BOT_RECUNIT=1 = old).
  Melee: the user's 5 melee kills are named "Melee" on our server (so "" is not melee -- corrected above); the walkers' close kills
  were all gunfire.
- **Levels apart, learned lifts / jumps, the stuck give-up, coaching between rounds** (2026-11-08 22:45; nbot.c, src/nav.c portals,
  shim/tools/nav-portals.py, coach-rounds.sh, match-study.py) — WHY: the user, after a round against three coached walkers: "they
  are way more aggressive now! ... one thing I think they have a hard time with is enemy radar on multi level maps. like if you see
  someone on radar and then in turn don't directly see them they are above or below you. the engagement struggled with that", "why
  was 'second' walker stationary so long with the smg and assault rifle?", "Can the coach process at the end of every round, and
  restart the walkers with the new training?". match-study.py on that round (the user 10 / 3, the walkers 12/9, 5/14, 5/12): replies
  when shot at first 1.8-2.5 s (were 4.4-5.5; the user 1.65), fired first 39-46 of ~80 fights. LEVELS APART (an enemy within 12
  units across, over 2 above / below, 2 s+): the walkers kept moving (1.3-1.7 u/s), never reached the other's level, won 3 / lost 6;
  the user stood (0.5 u/s) and won 3 from above, lost none. Now a known enemy hidden on another level is aimed at (up / down, 3
  rad/s) and, when it is BELOW, the walker holds its ground up to 8 s (NHOOK_BOT_NOLEVELS=1 = off). Guardian's floor graph has 423
  separate parts (the upper level, z ~22, apart from the main floor, z 15-20): nav-portals.py learns one-way links from every
  recorded player who crossed between parts within 4 s alive (lifts, cannons, jumps, steps): 161 on Guardian (2,122 crossings, 37
  recordings) -> maptop/<map>.portals.txt; src/nav.c loads them beside the graph and A* crosses one at the players' median time x
  2.2 units/s + 1 (PELOAD_NAV_NOPORTALS=1 = none). THE STATIONARY WALKER: 24 s at a ledge on the middle level (z ~19), hunting a
  hidden enemy 17 units off along a path dropping ~3 units: creeping, "stuck", backing off, replanning the same stretch -- in the
  goal draw four stucks in 10 s never dropped the goal; now they do ("stuck 4 times"). Its weapons (assault rifle, SMG) were what it
  held, not a choice. match-study.py adds the weapon held while standing still and the levels-apart episodes. coach-rounds.sh: beside
  a running battle, at every round end (our server's in_match true -> false) walker-coach.py round + match-study.py, then each
  walker's game restarted headless with its new knobs and the newest build (they rejoin in the lobby; the user's game and the
  server stay).
- **Break off when the shield pops** (2026-11-09 14:20; nbrain_beh.c beh_break, nbot.c g_ftg_*) — WHY: the last 15 death notes: the
  walker's shield 0 at death but its health a median 0.87 (once the shield pops a single headshot kills) and 9 of 15 with 2+ enemies
  aiming; the recharge behaviour waits for nobody in sight -- too late. LOGIC: the walker keeps its position every 0.5 s (4 s); with its
  shield under 0.15 and the fight's target in sight, 3-25 units away and healthier than it by 0.2+, it goes back to where it was ~2.5 s
  ago (2+ units off), still shooting, for up to 3 s or until its shield is over 0.6, bidding NHOOK_U_BREAK (70, over the fight's 60;
  0 = off). The fight's target vitality / distance / sight are kept per step (g_ftg_v / _d / _vis; the sight reset each step).
- **Finish the weak** (2026-11-09 14:15; nbot.c target choice) — WHY: in a 14-player Pit match the walker stood at 1 kill / 9 deaths
  with 5 ASSISTS -- it hit (the targets' shields fell, accuracy ~40%) but other players took the kills. LOGIC: after the choice rule, the
  weakest enemy in sight within NHOOK_BOT_FINISH_R (20, live) units with shield + health under NHOOK_BOT_FINISH_V (0.5, live) becomes the
  target ("[fight] ... is nearly dead (v): finishing it"); the enemy at arm's length still overrides it.
- **Let go of the trigger to switch; pulse the plasma pistol** (2026-11-09 14:05; nbot.c the trigger, bot_pull_weapon) — WHY: "[inv]
  switch presses (the learnt record bit): 0 of 32 took" -- 32 x "switching from PLASMA PISTOL to ASSAULT RIFLE" with the trigger held
  (the plasma pistol held = an overcharge held) while the earlier switches, 3 of 3 and 2 of 2, came with the trigger up. LOGIC: from a
  switch press until 0.15 s after it the trigger (and the reload click) stays up; PLASMA PISTOL joins the weapons whose trigger is pulsed.
- **A release watcher that ports nhook to a new client build without touching the walker's** (2026-11-09 12:25;
  shim/tools/release-watch.sh, native-hooks-test.sh HOOKS_TEST_BESIDE) — WHY: the user: "I see that .2 is out.. ensure you pull it and
  update everything, ensure all the hooks work, and continue testing. while you work the patches out stay on this current server
  since it is on .1". At 12:20 GitHub's latest release (what the app's own updater asks) was still v0.10.1, no v0.10.2 tag, and 273
  of 283 listed servers ran 0.10.1 (10 ran 0.10.0, none 0.10.2). LOGIC: release-watch.sh polls the latest release every 5 minutes;
  a new tag -> its macOS client zip checked against SHA256SUMS.txt into ReclaimerMac/native/<tag>/, unpacked there (never into
  /Applications: the walker runs from there and stays on its server's build), machoport.py port --apply (napp_builds.h +
  app-builds.txt), nhook rebuilt, and the hooks self test against that app beside the walker (HOOKS_TEST_BESIDE=1: no refusal, its
  own stats port 8781, output muted, NATIVE_APP = the unpacked app). Dry run on the kept 0.10.1 app beside the walker: PASS, 33 ok,
  0 failed, 10 not exercised in a local match.
- **The battle rifle is pulled, not held: accuracy 22.6% -> 61.8%** (2026-11-09 11:57-12:00; nbot.c the trigger, shim/tools/fire-ab.sh)
  — WHY: in a BR-start Valhalla match the walker's accuracy was 26% against the players' 34-59%. A held battle rifle measured 1.7 shots
  a second of trigger over a minute; the live A/B (fire-ab.sh: a knob at A then B for 90 s each, measured from the scoreboard and the
  [firegate] lines) settled it: pulled at NHOOK_BOT_PULL_PER 136 shots, 8.5 a second of trigger, accuracy 61.8%; held 159 shots, 9.7 a
  second, accuracy 22.6% -- the held burst rifle fires about as fast but sprays. LOGIC: the precision class (battle rifle / carbine /
  magnum) is a pull weapon: NHOOK_BOT_PULL_PRECISION (live, 1; 0 = held). The top players in that match: 52-56%.
  The pull period (fire-ab.sh on Guardian, 120 s each): 0.3 s -- 119 shots, accuracy 68.1%, 3.5 hits a second of trigger; 0.16 s --
  162 shots, 51.9%, 4.1 hits a second: the code's 0.24 s stays (the public walker's file and the Max aggression preset no longer set
  0.16). fire-ab.sh pauses the death tuner during a run and voids a window that spans a match change.
- **The hit test's pad grows with distance; the tuner ignores windows across a map change** (2026-11-09 12:05; nbot.c the fire gate,
  death-tuner.py) — WHY: a BR-start Valhalla match: the walker's accuracy 26% (on target 44%) against the others' 34-59% (60-76%),
  its aim line off the predicted target 17-46% of the steps -- the body column is ~0.6 deg wide at 25 units, while Halo 3's bullet
  magnetism pulls shots in from a little off; and the tuner had judged a Pit -> Valhalla window as the lead step's fault. Its
  server / client hit ratio (0.59 against the players' 0.68-0.81) points at too much lead at range. LOGIC: pad += tan(NHOOK_BOT_FIRE_PAD_DEG
  0.5 deg) x the distance; the tuner's lead and death windows that span a map change give no verdict (the step stays); the public
  walker's LEAD_X starts the search at 0.5.
- **"Ignored attacker": a death cause, live turn-to-attacker knobs, tuner moves** (2026-11-09 12:00; nbot.c why_note, nbot_engage.c the
  second-attacker rule, death-tuner.py) — WHY: on Valhalla 3 of 12 deaths were "slow start -- I never fired at it": a battle rifle
  14-26 units away, in sight, killing the walker while it stayed on another target (when the target shoots too, the walker turned to a
  second attacker only half the time, after the players' turn delay x 0.5). LOGIC: such a death (the killer in sight before the
  damage, not the target, never fired at) is "ignored attacker"; the odds to turn when the target shoots too (NHOOK_BOT_SECOND_P, 0.5)
  and the delay factor (NHOOK_BOT_TURNX, 0.5 -- it was read once at start) are live knobs on the Walker tab, and the tuner's moves for
  the cause raise the odds by 0.15 and shorten the delay by 0.1.
- **Lead horizontally with the body's velocity** (2026-11-09 11:55; nbot.c the head's path) — WHY: on Valhalla (battle rifles at 20-30
  units) the walker's aim line missed the target 29-36% of the steps and its accuracy fell to 25% (41% on target): the head node's
  frame-to-frame motion sways with the animation, and times a 0.15-0.25 s lead at that range it shook the aim. LOGIC: the predicted
  head moves with the engine's body velocity horizontally and the head node's own (smoothed, 0.3) velocity only vertically.
- **Cover searches were never asked on foot** (2026-11-09 11:45-11:50; nbot.c the on-foot line-of-sight scheduler, nbrain_beh.c
  beh_cover) — WHY: 0 of 89 cover searches found a spot; the new answers diagnostic said "asked 0" every time: the cover rays had gone
  into the vehicle gunner's scheduler (gun_seated -- its anchor text matched first), which never runs on foot. LOGIC: the cover rays
  take 2 slots of 3 in the fight's own scheduler; beh_cover keeps each spot's answer as it arrives (the line-of-sight cache forgets
  after 0.5 s, before the last of 6-8 spots is asked); "[cover] none ... (answers 1101..., asked N)". First live minutes: 2 of 4
  searches found a hidden spot 2.7-4.9 units away and went.
- **The walker's stats stay on 8765** (2026-11-09 11:40; shim/tools/stats-proxy.py) — WHY: the user: "game stats disappeared for some
  reason. http://127.0.0.1:8765" -- the relay QUIT when the user's own game started, and that game (opened from /Applications, no
  nhook) has no stats page, so 8765 stayed empty after it closed; then "the stats server should stay tied to the walker for now".
  LOGIC: --stay = never step aside; without it, a new game of the user's gets the port for 30 s (a game with nhook binds it at its
  start) and the relay serves again once the port is free -- it never ends by itself.
- **Cover when outnumbered; reload in a lull** (2026-11-09 11:35; nbrain_beh.c beh_cover, nbot.c g_naim_me / the lull reload) — WHY:
  the death review in a 37-player Pit match: 28 of the last 40 deaths "outnumbered" (2-4 enemies aiming at it) and 5 "empty" (in a
  brawl the walker is never out of a fight, and the reload only came out of one). LOGIC: with NHOOK_BOT_COVER_NAIM (2) or more
  enemies within 40 units aiming at it and the shield down 0.1+ in 1.5 s, the cover search starts at once; engaged, but the target out
  of sight 1 s+ and nobody aiming at it, a magazine under NHOOK_BOT_RELOAD_LULL (0.45) x full is reloaded ("reloading in a lull").
  Also: the head-path change had left the chest-height line and its comment twice (harmless); removed.
- **Weapon ranks and the grenade rate from the players' data** (2026-11-09 11:33; learnings.py WRANK / the grenade lesson, nbot.c
  bot_wpn_rank / ls_tick "_wrank") — WHY: two lessons the walker ignored: its "better weapon" order was a fixed list (which missed the
  HUD's "Energy Blade" -- only the tag name energy_sword was in it), and the coach's NHOOK_BOT_GRENX 0.237 had it throwing ~2 grenades
  a minute, 4x the players' rate, though attackers who throw win less (70% vs 78%) and a throw is time not shooting. LOGIC: learnings.py
  writes "_wrank" (weapons held 10+ minutes, by K/D x sqrt(kills a minute held), best first, tag-style keys) and NHOOK_BOT_GRENX =
  (win without / win with)^2 to walker-lessons.json; bot_wpn_rank = the place in "_wrank" (others after it, by the fixed list;
  NHOOK_BOT_WRANK_DATA=0 = the fixed list). The public walker's live file lost GRENX (the lesson owns it). Measured on the hit test +
  head lead before this: 65% of its shots on target since (55% before), the scoreboard's accuracy 52.4% (48%), 7th of 19 by accuracy
  in a 37-player Pit match (median 48%); the head share of hits 32% (55%: automatic weapons aim at the neck now).
- **The players' lessons drive the walker: walker-lessons.json under the live knobs; jump / strafe by role** (2026-11-09 11:28;
  shim/tools/learnings.py WL / USE, nbot.c ls_tick / ls_get / du_role, the fight jump and strafe) — WHY: the user: "i feel like the
  *lessons* in 'what the game is learning' are not applied to the walkers kernel". Many were, but through the models where nobody
  could see it (revenge odds, range bands, weapon zones, switch distances, death hot spots); others not at all (jumping and strafing
  are role-dependent in the data, the high-ground bid was fixed, the sword distance hand-set) and two were contradicted by the
  max-aggression preset (no recharge break-off: "pick fights with your shields up" -- 89% vs 51%). LOGIC: learnings.py writes the
  settings its lessons call for to tracks/walker-lessons.json (refreshed with the data every 20 minutes): NHOOK_U_RECHARGE (shields
  up), NHOOK_U_HOLD (65 + the high ground's edge), NHOOK_BOT_STRAFE_ATK / _DEF ((hardest / least strafing win rate)^2), NHOOK_BOT_FJUMP_ATK
  / _DEF (jump only in the role where jumpers win), NHOOK_BOT_CLOSE (0.4 + 2 x the push edge), NHOOK_BOT_KITE (the sword's
  best-surviving distance); the walker reads it as a layer between the launch values and the live knob file (a Walker tab value
  wins; NHOOK_BOT_LESSONS=0 off; [lessons] lines). The role in a duel (du_role: it fired first = attacking, the other aimed first =
  defending) scales the fight jump rate and the strafe. Every lesson on the Learnings tab gets a "walker" line: what it sets, how
  the walker already uses it, or "not yet" (weapon K/D ranks, the grenade opener, radar stealth, power-item timing). The public
  walker's live file lost U_RECHARGE 0 and CLOSE (the lessons own them); the Max aggression preset no longer sets them.
- **Lead along the head's path; a lead tuner on the server's accuracy** (2026-11-09 11:24; nbot.c the aim point, the hit test;
  shim/tools/death-tuner.py the lead track) — WHY: the user: "'fires on a cone around the head' classic inability to lead a shot, and
  predict the path of the head". LOGIC: the head node's velocity (smoothed from its positions every 0.06 s+, the body's velocity
  until then) x the lead time = NHOOK_BOT_LEAD_LAT (0.08 s: the host registers client bullets without lag compensation and the target
  as drawn is already behind) + distance / the weapon class's bullet speed (sniper 2000, precision 400, automatic 300, shotgun 250,
  rockets 40 units/s, x NHOOK_BOT_PROJ_SPEED) + one bot step, x NHOOK_BOT_LEAD_X; the aim point and the fire gate's hit test use the
  predicted head and body. death-tuner.py's lead track: every 250 server-counted shots (the scoreboard's hits / shots) LEAD_X moves
  0.25 in its direction; the next window puts it back and turns the direction if accuracy fell 3+ points.
- **Fire only when the aim line goes through the target; automatic weapons aim at the neck** (2026-11-09 11:20; nbot.c the fire gate
  ray_pd / g_bot_head_c, the aim point) — WHY: the user, from the scoreboard: "we miss way more shots than the competition" -- the
  walker 54% of its shots on target (accuracy 48%), the best players 64-84% (53-60%); its misses a median 1.1 deg off. The trigger was
  allowed inside a cone (body size + slack) around the aim point, and the aim point was the head -- plus the live knob file's
  NHOOK_BOT_HEAD_DZ 0.5 from an earlier tuning, half a head radius ABOVE it -- so every shot drifting upward missed everything.
  LOGIC: with the head known, the trigger is allowed only when the line from the camera along the record's aim passes within the
  head's radius x 1.1 + NHOOK_BOT_FIRE_PAD (0.03 units) of its centre, or within 0.22 + pad of the body column from the head down to
  0.7 radii under the centre (9 samples); NHOOK_BOT_FIRE_HITTEST=0 = the cone. Automatic weapons track the head node but aim
  NHOOK_BOT_HEAD_DZ_AUTO (-1.5) head radii under its centre (the neck); precision weapons the head centre (HEAD_DZ taken out of the
  public walker's file). [firegate]'s "aim line not through it" counts what the hit test holds back.
  A/B (fire-ab.sh, 4 x 60 s each side, a 36-player Last Resort match, mostly assault rifles): the neck (-1.5) -- 486 shots, accuracy
  54.3%, 5.1 hits a trigger second, 9 kills / 8 deaths; the upper chest (-3.0) -- 388 shots, 42.5%, 3.7, 4 / 10.
  Higher (-0.75) vs the neck (-1.5), same match: 48.0% vs 50.1%, 3.8 vs 3.7 hits a trigger second, 5 / 6 vs 8 / 6: the neck stays.
- **The brain revises itself: per map, every 3 minutes, and when a monitor trips; the death map is read again each time** (2026-11-09
  15:07-15:15; nbot_goals.c brain_review / rv_note / hm_reload_brain / gl_pen_zone_w, nbot_engage.c g_gl_nkill, nbot.c press / lull
  reload, shim/tools/brain-monitor.py, death-tuner.py MONITOR, workboard.py Brain tab) — WHY: a 58-player Pit match ended 63 kills /
  104 deaths, the most deaths in the game (the top killer 108 / 47), and the user: "I'd expect a brain loaded every 3 minutes to learn
  from its death heat map and frequency" / "there should be per map, and periodic, and other conditional settings that auto trigger
  new brains ... bad k/d spread, bad accuracy deviations, bad shots landed ... auto trigger relearning and reset the levers until they
  get tripped accordingly again". The loaded brain was 2.5 h old; its copy of the map's death file was read once per map (the body saves
  the match's deaths into it every 30 s, so a 58-minute match planned around earlier matches' deaths); its failed-death path penalties
  (+3 each, gl_pen_zone) never faded -- 60+ of them over most of The Pit. Of the 91 reviewed deaths: 73 in a straight duel with its own
  target, 73 closer than 70% of the distance players open fire from with its weapon, 82 with the shield gone, 23 on an EMPTY magazine.
  LOGIC: a REVISION r<N> ("[brain] review r<N> [<trigger>]", battles/follow/review-<nick>.jsonl) on a map change, on a new build of the
  brain ("new brain": it carries on from the last revision's number and caution when that is under 15 min old), every
  NHOOK_BOT_REVIEW_S (180, live; 0 = off), and at once when battles/live-knobs/<nick>.review gets a new "<seq> <reason>" (read every
  2 s). Each: the death map read again (the danger map rebuilt from it), kills (g_gl_nkill: each target that died while fought, once)
  and deaths in the window -> CAUTION = half the last + half of (1 - K/D capped at 1) + (deaths a minute - 1) / 3, capped at 1; it cuts
  the press into the target inside the weapon's band (x 1 - 0.6 caution) and raises the lull reload threshold (+ 0.25 caution); the
  path penalties halve (a monitor's relearn: zeroed) and each own death of the last 3 periods adds its zone again, 3 / 2 / 1 by age.
  brain-monitor.py (every 10 s, the walker's scoreboard; inside one match) trips: spread (kills - deaths over 5 min <= -6), kd (its
  last 10 deaths under half the lobby's median, and < 0.6), accuracy (its last 150 shots 0.08 under the lobby's median or 0.10 under
  its match), hits (hits landed a minute over 3 min under half its match rate), deaths (> 2.5 a minute), stale (no revision for two
  periods in a match); one trip per 90 s, a tripped trigger waits until its number recovers (hysteresis). A trip writes the .review
  signal; spread / kd / accuracy / hits also RESET THE LEVERS: death-tuner.py puts every setting it moves back to its last good set
  (its start, then after each kept step whose window beat the walker's K/D so far) -> monitor-<nick>.jsonl / .json, tuner "reset" /
  "good set". The work board's Brain tab: the loaded build and lesson, every revision, the triggers (armed / TRIPPED, value, limit),
  trips and resets, and ALARMS when the monitor stops or no revision came for two periods in a match. Old behaviour: NHOOK_BOT_REVIEW_S=0
  (no periodic revisions; map revisions remain), stop brain-monitor.py (no trips / resets).
- **Hot takeover: a new brain warms up beside the running one and swaps between two steps** (2026-11-09 15:25; nbot.c brain_tick /
  brain_warm_thread / bot_thread / nh_brain_warm / g_brain_warm, nav.c g_nav_quiet, nbot_goals.c brain_review) — WHY: the user: "brain
  updates should be seamless to the walker even if it is in the middle of an active firefight. it should never pause movement etc. the
  brain should always be ready for a hot takeover". The bot thread itself copied and dlopen'ed a new nbrain.dylib and stepped it at
  once: the new copy loaded its nav graph (91k nodes on Valhalla), the map's 11,658 tags, the models and the death map inside its first
  steps on the walker's own thread (legs and aim held their last values meanwhile), then started from nothing -- in the 15:14 swap a
  29-unit fight went from "FIRING, err 0.8 deg" to "hunting", 15 deg off. LOGIC: brain_tick only copies the new build and starts
  shim-brain-warm; that thread dlopens it and calls its nh_brain_warm (the step with g_brain_warm set: bot_log() silent, no why / review
  / switch-scan / gun-exit files, nav's stuck alarm and blocked-steps file off via g_nav_quiet, outputs thrown away) every 33 ms with
  the same inputs (io_in_pack only reads) while the running brain keeps driving -- until it has stepped NHOOK_BRAIN_WARM_STEPS (30) and
  its last 3 steps each took under 4 ms (everything loaded; its target, aim smoothing, velocity estimates and path caught up), at most
  NHOOK_BRAIN_WARM_MAX (20 s). The bot thread swaps the step pointer between two steps ("[brain] #N ready: warm (... steps in ... s,
  the slowest ... ms)", "#N took over at this step", and from the brain "took over (warm ...)"); its first own step makes the "new
  brain" revision. A build without nh_brain_warm is swapped in cold as before. Old behaviour: NHOOK_BRAIN_COLD=1. Needs the new body
  (nhook.dylib: a walker launch); brains are NHIO layout 1 as before. Open: an old brain's memory is never freed (dlclose would not
  free its mallocs; ~nav graph + models per swap).
- **The handoff: a warming brain adopts the running brain's target and goal** (2026-11-09 15:29; nbot.c hand_publish / hand_adopt /
  BrainHand, nbot_engage.c g_eg_adopt / g_eg_cur) — WHY: the first mid-match hot swap (15:27, Guardian) did not stall -- the 68 ms
  loading step ran on the warm-up thread and the walker walked straight through it -- but the new brain had picked its own target and
  goal while it warmed up: at the swap it turned 15-19 deg to another enemy and walked to another goal. LOGIC: the running brain (and
  the body's built-in one) writes its target (and since when) and its goal (kind, place, whom, until) after every step into a 4 KB
  shared-memory block of the process (shm "/nhbrain-<pid>", a seqlock); a warming brain copies them before every warm step
  (eg_choose takes the adopted target, g_gl the goal), so at the swap it carries on with them and decides for itself from the next
  step. A block of another layout (HAND_VER) is ignored. Live check of the adoption still to do (the walker was kicked at 15:29).
- **walker-public.sh keeps the last session's log** (2026-11-09 15:36) — WHY: the 15:29 kick ended the app cleanly (a normal exit),
  the loop relaunched once, native.sh truncated aiwalker.log and the kick left no trace; the rejoin then failed with "You were kicked
  from this server. You can rejoin in 29 minutes." and the loop stopped as it should. LOGIC: before each launch aiwalker.log ->
  aiwalker.prev.log; the kick check also matches "kicked".
- **The lead follows the ping; the reviews count the scoreboard's kills** (2026-11-09 16:50; nstats.c g_net_rtt_ms, nbot.c NhIO layout 2
  (my_kills / my_deaths / rtt_ms), the lead time, nbot_goals.c brain_review) — WHY: on a server 145 ms away the walker's accuracy fell
  to 5-12% against the lobby's 17-23% (it was above the lobby's median on a 44-64 ms server): the lead's latency part was a fixed 0.08 s
  tuned at 64 ms ("the host registers a client's bullets with no lag compensation"). And the three-minute reviews counted "a target that
  died while I fought it" as a kill -- 17 "kills" in that match where the scoreboard said 3 -- so the caution stayed 0 while it lost.
  LOGIC: the body reads the joined server's round trip from the app's own "[online] peer ... rtt <ms> ms" line (the peer at the joined
  address) and this match's kills / deaths from the game results (g_rs_cur at its slot); NhIO v2 hands them to the brain (a layout-1
  brain is refused by a v2 body and the other way round: rebuild both). Lead latency = rtt x NHOOK_BOT_LEAD_RTT (1.25 / 1000 -- 0.08 s
  at 64 ms, 0.18 s at 145 ms); a NHOOK_BOT_LEAD_LAT in the live file still wins. A review's kills = the scoreboard's since the window
  began (the old count when the body has none); the review line gives the ping.
- **A replaced brain lets go of its memory and its open map files** (2026-11-09 19:50; nbot.c nh_brain_free, bot_thread, proc_footprint_mb)
  — WHY: the user, playing 3 walkers: "it with 3 minutes left in this round got choppy... check the memory leak or whatever open file
  handles" / "we are all laggy now. close everything, fix the memory leak". Measured: no process grew (a walker 1.9 -> 0.7 GB, the user's
  game 3.6 -> 3.4 GB, 77-117 open files each); the user's game's WORST frame climbed 62 -> 102 ms over the round (the 0.10.1 app's own
  build-up, backlog N20) and a fresh server resets it. The real leak was a hot swap: the old brain's walking graph, lanes, links, its copy of
  the death map and -- in h3maps.c's g_hbm -- the game's map files its hitbox reader OPENED (an fd + tag tables per map per brain, never
  closed) stayed for the process's life: dozens of reloads in a long session piled them up. LOGIC: every brain exports nh_brain_free
  (frees g_nav + pop, the links, g_hm, g_bm_heat, g_gll; closes and frees g_hbm); the body calls the replaced brain's 5 s after the
  takeover (a brain two swaps back: at once); each takeover logs the process footprint, and "footprint X -> Y MB after the replaced brain
  let go".
- **Training on the player they fight: the rival** (2026-11-09 19:45; shim/tools/death-tuner.py --rival, shim/tools/rival-coach.py) —
  WHY: the user: "I want them to continue the every 3 minute train + brain reload vs me on this level. and aggressively try to get better
  than me by training on me vs the entire collection corpus + corrections in playing me". LOGIC: the death tuner's --rival <name>: a death
  whose killer (the why line's unit handle, matched on the walker's stats page) was that player weighs RIVAL_W (3) in the causes ("rival
  death" events). rival-coach.py, every 180 s of a battle: the battle's recording cut into fights (only that file), the rival's measures
  (coach.py: closing, aim error, time to aim back, strafing, jumps, grenades, radar turns, fight distance) blended 2 : 1 with the corpus
  winners' (walker-coach.py winners(), computed once at background priority, cached battles/coach/winners-<map>-<class>.json); each walker's
  steps (walker-coach.py steps()) written into its LIVE settings file (it plays with them within a second); battles/coach/rival.jsonl.
  No names in files ("rival").
- **Infection tactics: a zombie hunts and charges, a human groups up** (2026-11-09 18:40; nbot.c g_inf_me, inf_group_goal, the goal
  choice, the fight move; nbrain_beh.c cover / recharge / break-off) — WHY: the user asked for an Infection profile ("I know it's a
  completely new game tactic" -> "yes"): the walker played Infection with its normal fight -- as a zombie the caution of a bad window
  halved its charge (caution 0.98 on Sandtrap while it died ~3 a minute), as a human it fought alone. LOGIC: g_inf_me = its side from the
  sides the body hands the brain (NhIO v3): 1 zombie, 0 human, -1 not Infection ("[inf] I'm a ZOMBIE now ..." / "I'm a HUMAN ..."). A
  ZOMBIE: the nav goal is the nearest human, distance x (1 + 0.5 per other human within 6 units of it) -- the lonely ones first; in a
  fight r = 1 (straight in) with a NHOOK_INF_ZWEAVE (0.35) weave every 0.7 s, after the crowd / caution / shield rules (they no longer
  hold it back); no cover, recharge or break-off. A HUMAN out of a fight: to the biggest group of humans within 80 units (humans within
  NHOOK_INF_GROUP 8 units of each other) and, within NHOOK_INF_HOLD (4) of it, holds there (no goal, the old path dropped; the stuck
  grenade waits while it holds); swords and hammers are kited as before (NHOOK_BOT_KITE). NHOOK_INF=0 (live) = the normal fight.
- **Infection sides by a vote; the brain gets them** (2026-11-09 18:25; nstats.c st_is_zombie, the players' "zombie", "infection sides"
  line, nbot.c NhIO layout 3) — WHY: the user: "hitbox rendering for other team doesn't work on infection games. even though there is a
  *clear* other team, although they do switch mid game". In a custom variant (a 97-player "Infection - 300% Large": zombies colour 10, 50%
  gravity, no grenades, swords / hammers; humans a shotgun start, radar off; every player a team of its own) the check decided on the
  FIRST trait where the variant's zombie set differs from a human's, and one the players' resolved traits do not carry the same way made
  every zombie "human": everyone on "my side", the overlay's enemies filter drew nobody. And the brain never had the sides (the body's
  snapshot fills them): its bot_enemy fell back to team numbers -- everyone an enemy, its own side included. LOGIC: every trait where
  the zombie (or alpha zombie) set differs from a human's votes -- the resolved value equal to the zombie's: zombie, to a human's: human,
  else no vote; more zombie votes in either set -> zombie. Each player in stats.json says "zombie": true|false (Infection only); the log
  says "infection sides: N zombies, M humans" when the counts change. NhIO v3 hands the sides (128 units) to the brain. NHOOK_ZOMBIE_FIRST=1
  = the old first-field decision.
- **Lanes learned live: a minute into a new map, and at every revision** (2026-11-09 18:15; nbot.c bm_lanes_read / bm_lanes_mix /
  bm_lanes_refresh / bm_lanes_apply / g_bm_heat, nbot_goals.c brain_review) — WHY: the user: "lanes for the new map should come quick.
  learn them with each 3 minute brain update". The planner's lanes (g_nav.pop: A* prefers the routes players take; the goal draw's lane
  cells g_gll) were built once per map, from the real heatmaps folder only -- while the public walker's body saves the lanes of the
  match it is in (every player's moves it sees) into ITS folder (NHOOK_HEATMAP_DIR) every 30 s: a new map started with none and never
  got this match's. LOGIC: the lanes come from both folders (summed; the same file once); the model's heat part is kept apart
  (g_bm_heat); a revision -- and once 60 s into a map -- reads the files on a side thread, and the walker's next step mixes them in (the
  planner's arrays are only touched on its thread; a few ms): "[brain] lanes relearned for <map>: N lane cells, X km of players' moves".
- **No brain update without its lesson** (2026-11-09 18:05; shim/native/build.sh) — WHY: the user: "every update should describe its
  lesson" -- builds had gone out carrying the previous brain's lesson (the SWAT text on the island / unstick brains). LOGIC: build.sh
  hashes the brain's sources (native/*.c *.h, src/nav.c, src/h3maps.c) and native/brain-lesson.txt; when the code changed since the last
  brain build and the lesson did not (or it is empty), the BRAIN is not built (the body still is; the walker keeps its brain): "NO BRAIN
  BUILT -- ... write this update's lesson". A rebuild of unchanged code needs nothing. BRAIN_LESSON_SAME=1 builds anyway, marked
  "same_lesson" in brain-lessons.jsonl. State: shim/build/brain-state.txt. Tested: a one-line change refused, reverted -> built.
- **The Brain tab confirms every new brain; quiet windows are revisions too** (2026-11-09 18:00; nbot.c nh_brain_step, nbot_goals.c
  brain_review, shim/tools/brain-monitor.py brain_info, workboard.py "Brain loads") — WHY: the user: "how come the #brain log doesn't
  indicate a new brain loaded any more? it just says some stuff, but doesn't confirm new brain" -- the tab took the loaded build from
  the walker log's last 3 MB, which path-planning lines had filled (an empty "brain"), and a 3-minute window with no kill or death made
  no revision (the monitor forced them as "stale"). LOGIC: each brain, at its first own step, appends {t, built, warm, map, lesson} to
  battles/follow/brain-<nick>.jsonl; the tab's header says "Brain code built ... · loaded HH:MM (n ago), warmed up beside the running
  brain, then took over on <map>" and a "Brain loads" list shows the last 12; the monitor reads the same file. A periodic review in a
  match is a revision even when nothing happened (out of a match still none). The links events read "players' links relearned: ...".
  This build's lesson describes it (brain-lesson.txt was still the SWAT one).
- **A brain running from its temp copy finds the walking-graph tool again; no graph is not "stuck"** (2026-11-09 17:50; nbot.c
  nbot_tool_path, the unstick) — WHY: on a new map (hugetowers) the walker got no walking graph: "re-tools/h3nav.py not found" under
  $TMPDIR -- a brain runs from a copy there (hot swaps), and the tool path was taken from the brain's own file. And the new rule "no path
  anywhere for 45 s = stuck" would have had it grenade itself over and over on a map with no graph at all. LOGIC: when the path beside
  the dylib does not exist, the checkout comes from DYLD_INSERT_LIBRARIES (shim/native.sh injects <checkout>/shim/build/nhook.dylib),
  else NHOOK_STATS_PAGE; the no-path stuck rule needs a loaded graph (g_nav.ok). The next brain made the hugetowers graph at once.
- **Floor steps learned both ways; a stuck walker heads where players leave from** (2026-11-09 17:45; shim/tools/nav-portals.py STEPS,
  nbot.c the island escape) — WHY: the user: "the player position tells me the current walker ... can't even move from its spawn point,
  and clearly is not learning from teammate lanes". The recording showed it: Hugegrass's spawn room is closed, its exits are teleporters in
  the east wall (players walk to x ~612 and appear at (377, 22) or (177, 242)); the room's floor tiles differ by ~0.9 units, over the
  floor graph's 0.35 step, so the room was dozens of parts, and the walker's part beside the teleporter had a learned link IN, none OUT
  (links needed 3 sightings, one way). Its escape walked at its goal -- west, deeper into the room. LOGIC: a crossing between parts whose
  ends are under 3 units apart across and 1.5 up / down is floor: kept from one sighting, both ways (Hugegrass: 463 -> 3,684 links; every
  spawn-room part now reaches the field). The island escape walks to the nearest learned link's start within 60 units and 4 up / down
  (where players left from) before the goal ("walking straight at ... (where players left this spot from)").
- **Stuck too long: a grenade at its feet; the players' links learned live** (2026-11-09 17:25; nbot.c "STUCK TOO LONG", nbot_goals.c
  brain_review LIVE LINKS, shim/tools/brain-monitor.py links()) — WHY: the user: "seems to me like being *stuck* is because the map lanes
  are not being studied in real time. if stuck too long look straight down and throw grenades and intentionally stand on them" -- on
  Hugegrass the walker stayed alive 6+ minutes in a spawn spot no path led out of. LOGIC: alive, no fight for 15 s, and every position
  of the last NHOOK_BOT_UNSTICK_S (45 s, live; 0 = off) within 6 units of where the stretch began -> pitch straight down, legs still,
  the trigger off, a grenade (left trigger) every 1.5 s until it dies and respawns elsewhere ("[unstick]"); never with a teammate within
  10 units (friendly fire: a betrayal, and betrayal booting kicks) or without a grenade. LIVE LINKS: brain-monitor.py runs
  nav-portals.py <map> every --links-every (300 s) of a match (every recording of the map, the one being written included; background
  priority; "links" events in monitor-<nick>.jsonl), and the brain reads maptop/<map>.portals.txt again whenever it changes (checked
  every 5 s: "[brain] the players' links for this map changed") instead of only when the map loads.
  (17:35) The first version never fired on the Hugegrass island: the island escape hopped the walker back and forth over 6 units, and
  enemies seen far across the map kept "fighting" true. Now "no path found to anywhere for NHOOK_BOT_UNSTICK_S" (g_nav_ok_t, reset at
  each life's start) also counts as stuck, and only a fight within 40 units holds it off. After a quit and rejoin (the user: "it needs to
  quit and rejoin") the brain had 354 learned links (the monitor's relearn, from 178) and walked out of the east base down to the field.
- **One player in the whole window, beside a map of just that player** (2026-11-09 17:10; stats_page.html #pfs / openPfs / drawPlayerMap /
  trackSnap, shim/tools/stats-proxy.py /trail.json) — WHY: the user: the player details popup "full window, and it should side by side a
  version of the heatmap with JUST my player visualized". LOGIC: clicking a name opens the details over the whole window (the #det panel
  moves in; X, Esc or the name again closes it) beside a canvas of that player alone on the map picture: a heatmap of its positions (cells
  of 7 screen points, the page's colour ramp by time spent), its path (the last 30 s brighter), its death spots (white X), where it stood
  when its kill count rose (yellow), where it is now. The path: this match from the walker's own recording through the relay on 8765
  (/trail.json?slot=&map=: the newest tracks/*.jsonl.gz of that map, written in the last 90 s; positions every 0.5 s alive; a slot whose
  counters go back down changed hands and starts again; cached 10 s), else what the page has seen since it opened.
- **Islands of the floor graph: walk out instead of standing there** (2026-11-09 17:15; nbot.c the path planner) — WHY: on Hugegrass the
  walker spawned on a part of the graph joined to the rest by nothing it knew and stood at its base for 6 minutes, every goal "no path"
  (the player view above showed it: 148 units in 6 min). LOGIC: 3 failed plans within 8 s -> 4 s walking straight at the last goal,
  hopping, unless fighting ("[nav] no path 3 times in 8 s ... an island of the floor graph"). And the links were learned from this match's
  players: shim/tools/nav-portals.py hugeass_remastered_mcc (2,614 crossings between parts, 178 portals seen 3+ times) -> maptop/
  <map>.portals.txt, loaded by the next brain's nav (a hot takeover).
- **walker-public.sh --mods: a walker joins a mod server** (2026-11-09 17:00) — WHY: the user moved to a big mod server (2 mods, ~4 GB);
  when a server needs mods the PC lacks, the app opens its "required mods window" -- nobody clicks it in a headless walker -- and the loop
  stopped any join that took over 180 s, mid-download. LOGIC: --mods adds the app's own --download-mods to the join ("the host's game
  needs mods this PC lacks or has turned off: join with --download-mods") and lets a join take JOIN_WAIT (1800 s). First use: the app
  found both mods "Ready" and joined the game in progress in ~30 s.
- **The stats page names the server** (2026-11-09 17:00; shim/tools/stats-proxy.py server_info, stats_page.html #srv) — WHY: the user
  asked for the connected server's name at the top of the page, with its full description rather than only its address. LOGIC: the
  relay on 8765 sends the joined server the one status datagram every client sends to listed servers (a6 01 00 <nonce8> padded to
  1229 -> a7 <nonce8> {"info": ...}; servers10.py), at most once a minute, and adds server_name + server_info (map, mode, players, max,
  version) to the stats.json AND the /events snapshots it passes on (the page draws from the stream). The page's first line: "Server:
  <name> (<address> · <players>/<max> players · <mode>)"; the address alone without the relay, "Hosting this game" as host; the
  anonymised demo shows "An example server".
- **The SWAT profile: faster to the head, head-only trigger, in context** (2026-11-09 16:00; nbot.c g_noshield sites, nbot_goals.c
  rv_note "profile") — WHY: the user: "the swat gametype is headshots only btw... no shields... one shot. so you gonna have to kick up
  the accuracy, speed to target, etc. or walker is gonna get destroyed. the brain will need to adapt quickly in context cuz this is a
  fast paced game type". LOGIC: the shieldless check now needs only 5 s with 4+ live players and no shield (20 s otherwise); while it
  holds: the reaction factor x NHOOK_SWAT_REACT (0.5), the turn rate before the reaction x NHOOK_SWAT_TURN0 (1.5) and after x
  NHOOK_SWAT_TURN (1.3) (the turn is a clamp toward the aim point: faster, no overshoot), the aim noise x NHOOK_SWAT_NOISE (0.6),
  automatic weapons aim at the head centre (not the neck), the hit test passes only through the HEAD (NHOOK_SWAT_HEADONLY 1: the body
  column is skipped), the press into the target x NHOOK_SWAT_PRESS (0.5). All live knobs; everything returns the moment a shield shows.
  Each revision records the profile ("SWAT profile" in the line, "profile": "swat" | "normal" in review-<nick>.jsonl).
- **Games without shields (SWAT): the shield rules stand down** (2026-11-09 15:45; nbot.c g_noshield, fight movement, why_note,
  nbrain_beh.c beh_recharge / beh_break) — WHY: the walker moved to a new approved server playing SWAT (no shields, a headshot kills);
  every rule that reads its own shield as "how hurt am I" would see a permanent 0: back off inside every fight (the fight movement's
  shield < NHOOK_BOT_RETREAT), back off to recharge, break off, and every death "entered hurt" (the tuner stepping recharge settings for
  nothing). LOGIC: g_noshield = no live player in the object table has shown a shield above 0.05 for 20 s of this map (a shield seen
  turns it off at once; a new map starts it again); then those four rules stand down. Cover by a shield drop never fires there anyway.
  "[bot] no shields in this game" / "shields are back" lines. NHOOK_BOT_NOSHIELD=1 / 0 (live) forces it on / off.
- **The public walker's name comes from its profile** (2026-11-09 15:45; walkerrec.pubwalker_nick, workboard.py, death-tuner.py,
  brain-monitor.py, fire-ab.sh) — WHY: the new server's admin approved another name for it (the client allows 1-15 UTF-16 units, so the
  approved 19-character name was shortened with the user); every tool that followed the walker had its old name built in. LOGIC: the
  name = identity.nickname of the walker's own profile (native/public-walker/home, only that field read); --nick / WB_PUBWALKER still
  override. The walker's live settings file was copied to the new name; its why / tuner / review / monitor files start fresh under it.
  The nickname keeps the "AIWalker" prefix the bot's disclosure check needs (nbot.c: it plays only under a name that says so).
- **Before dry: switch at 20% of the magazine in a close fight; reload after each kill** (2026-11-09 15:07; nbot.c) — WHY: the same
  match: all 23 empty-magazine deaths came mid-duel at ~5 units -- in a brawl the next enemy comes right after a kill, so the
  after-the-fight and lull reloads never came, and the EMPTY switch at 0 rounds was too late. LOGIC: in a close fight (g_eng_close)
  with the held magazine at NHOOK_BOT_DRYSW (0.2, live; 0 = off) of its most or less (not a shotgun / sword, not a weapon of 4 rounds
  or fewer), the target above half its vitality and the other weapon loaded -> switch now; the rank / band switches then wait 4.5 s
  (no ping-pong). When its target dies (g_gl_nkill) with the magazine under NHOOK_BOT_RELOAD_KILL (0.6, live; 0 = off) of its most and
  nobody aiming at it -> reload. Old behaviour: NHOOK_BOT_DRYSW=0 NHOOK_BOT_RELOAD_KILL=0.
- **The You tab: Start / Stop sessions, only real matches of the user's; walkers' recordings marked** (2026-11-09 15:00; ntracks.c
  "walker", shim/tools/walkerrec.py, coach.py, engage-extract.py, workboard.py) — WHY: the user: "the you page should only be activated
  by a start and stop button on that page btw. and clear stats that are manufactured and only use real stats from started sessions".
  The tab showed "37 matches, K/D 0.36": the public walker's own recordings mark its slot "you" (you_plays), so coach.py took the
  walker's games for the user's; with no match it kept an EXAMPLE report (--pretend). The extractor also kept a walker's slot in the
  players' data as a real player. LOGIC: the recording header says "walker": true when NHOOK_SPAR / NHOOK_NAV / NHOOK_BOT_FIGHT drive
  the local player; walkerrec.walker_files() = those + every recording named in a walker log (battles/*.log "[tracks] recording ... ->
  <file>", for the recordings made before the field). coach.py: "you" = the user's slot in a match that STARTS inside a session
  (workboard/you-sessions.json; the tab's Start / Stop: POST /you?do=start|stop with an X-Workboard header) and is no walker's; no
  session or no match = a note, never a report; --pretend only prints. Stop re-reads the recordings (extract + coach, background
  priority). engage-extract.py leaves a walker's slot out like the observer's (meta "walker": true). The old coach.json was replaced.
- **The work board lists what really runs; the refit is ~4.7x faster** (2026-11-09 14:50-15:10; workboard.py KINDS / observer_age,
  engage-model.py fit_choice / main) — WHY: the user: "the jobs in the work page are still like hung" -- Running now listed only the
  refit (70 min at background priority, so the 20-minute learning refresh came every ~1.5-1.75 h), never the public walker, its tuner,
  the release watch or the refresh loop; the observer panel showed a run from the night before; direction and tasks were a day old.
  The target-choice fit was 76% of a refit (cProfile on a tenth of the data: 132 of 173 s): 400 full-batch gradient steps over ~316k
  choice sets padded to the largest, 28 fits a run, and the report (every class, all but Infection: their own fits) was built on every
  refresh and thrown away (stdout to /dev/null). LOGIC: the same objective (the sets' log-likelihood - 0.01 |w|^2) solved by Newton's
  method over the candidates laid end to end (np.*.reduceat segments; converged when a step < 1e-8, at most 60), the top-1 checks
  vectorised the same way; the report only with --json / --report. A tenth of the data: 173 -> 56 s, the same weights to 3 decimals;
  all of it (305k duels): ~70 min -> 14.7 min at background priority, weights within 0.05 of the old fit (more data since).
  ENGAGE_FIT_GD=1 = the old gradient ascent; --report = the old printed report. The board: Running now knows walker-public.sh,
  death-tuner.py, release-watch.sh, fire-ab.sh, refresh-loop.sh, stats-proxy.py, brain-monitor.py; the observer panel says "not
  running" when its log is 30+ min old.
- **walker-progress.py: the public walker match by match** (2026-11-09 14:47) — the walker's final kills / deaths / shots / hits per
  recorded match and its rank in that lobby (K/D, kills, accuracy among 50+ shot players): `python3 shim/tools/walker-progress.py
  [--hours N | --since HH:MM] [--server <tag>]`.
- **The work board learns again while the walker plays; its Walkers tab shows the public walker** (2026-11-09 11:18;
  shim/tools/refresh-loop.sh playing(), shim/tools/workboard.py pubwalker()) — WHY: the user: "are we no longer learning and updating
  the work board, etc?" The 20-minute refresh waits while the user plays, and it took the headless public walker for the user: it had
  waited since the walker joined (10:33); the Walkers tab showed only the local coached rounds and the learn loop, neither running
  since the night. LOGIC: a --headless game is not the user's; the Walkers tab's first panel: the public walker's kills / deaths /
  share on target / head share now (its stats page, 8771), the causes of its last 60 deaths, the death tuner's steps and verdicts,
  and the brain's current lesson.
- **Why did they beat me: a cause per death, and a tuner that moves settings by it** (2026-11-09 11:09-11:14; nbot.c du_tick /
  why_note, shim/tools/death-tuner.py, nwknobs.c "why" / "tune", stats_page.html Walker tab) — WHY: the user: "walker needs to focus
  hard on its K/D spread and tweak settings until it gets better, I feel like a per death why did the other player beat me
  calculation could be interesting, weighting future growth". LOGIC: every step the duels of this life (per enemy: in sight since,
  aiming at the walker since, the walker firing at it since and its shield + health then, its shield + health last seen) and the
  walker's damage episode (the first hit after 3 s without one, the shield before it). At a death: the likely killer (the target if
  it aimed at the walker, else the nearest enemy aiming at it) and ONE cause, the first that fits -- empty, outnumbered (2+
  aiming), unseen (hurt before ever in sight), point blank (shotgun / sword under 2.5 u), traded (killer left under 0.25),
  entered hurt (shield under 0.5 when the damage began), slow start (fired back 0.5+ s late or never), outranged (own weapon off
  its band, the killer's in), outshot -> "[why]" + battles/follow/why-<nick>.jsonl. death-tuner.py weighs the causes (each death's
  weight halves every 20 deaths), every 6 deaths steps ONE setting for the heaviest cause (each cause its own list: slow start ->
  reaction / first turn / see-near, outnumbered -> cover shield / retreat / press, outshot -> wobble / lead / fire slack, ...),
  and after the next 6 deaths puts it back if K/D (kills from the walker's stats page per death reviewed) fell 25%+ (that move then
  rests 30 deaths); NHOOK_TUNE=0 in the knob file pauses it (the tab's switch); steps -> battles/follow/tuner-<nick>.jsonl. The
  Walker tab shows the causes of the last 60 deaths, the last 10 with their detail, and the tuner's steps / verdicts. First live
  run (20-player Pit): 8 of the first 10 reviewed deaths "outnumbered" (2-4 enemies aiming).
- **Weapon choice as settings; the best weapon for the hunt** (2026-11-09 11:06; nbot.c the better-weapon rule / the fight switch) —
  WHY: the user: "select best weapon in inventory should be a setting too, and select best weapon for your hunt just the same".
  LOGIC: NHOOK_BOT_BEST_WEAPON (1) + NHOOK_BOT_BEST_RANKS (2): the rank rule; NHOOK_BOT_FIGHT_SWITCH (1): the range / zone / players'
  switch in a fight (the empty-magazine switch stays either way); NHOOK_BOT_HUNT_WEAPON (1) + _X (1.3): out of a fight with a goal,
  the zone model around the goal point (players' kills by weapon class over its carried share, 8+ kills there) puts the weapon that
  does 1.3x better there in hand before the meeting -- shotguns and swords too -- and keeps the one in hand that does 1.3x better
  (the rank rule does not switch it back). All live; the Walker tab's Weapons group. "[inv] for the hunt (...)" lines.
- **Aim column: the share on target and how far the misses went; the head share under it** (2026-11-09 11:13; nstats.c st_aim_json
  "miss" / "miss75", stats_page.html aimCell) — WHY: the user: "all aim stats are *always* 0.0 degrees off. why?" and "the 'hits
  counted' variable doesn't seem to update properly, it's always the same number". A shot through a hitbox is 0 off, so with over
  half on target the median of all shots is 0; and "counted" (damage within 0.6 s of an on-target shot) is met by anyone's damage
  in a crowd: 90-100% for every player, though it did update. LOGIC: the cell shows "N% on target · misses by X°" (the median over
  the shots that missed; a game started before this shows the share alone) and "N% of M hits on the head"; "counted" stays in the
  tooltip with its caveat; the column sorts by the share on target.
- **The stats page's Walker tab: the walkers' settings changed while they play** (2026-11-09 10:45-11:05; shim/native/nwknobs.c,
  nstats.c GET /walker.json + POST do=wknob, shim/src/stats_page.html #walker, nbot.c lk_tick; test shim/tools/wknobs-test.sh) — WHY:
  the user: "add a walker settings page to the status window as a tab please so we can adjust it on the fly". LOGIC: the walkers'
  live knob files (battles/live-knobs/<nickname>.json, which lk_tick re-reads within a second -- no restart, no rejoin) are listed
  from the user's REAL home (getpwuid: the public walker runs with a HOME of its own whose ReclaimerMac links back to the user's, so
  the folder is listed once and the public walker told apart by its profile's nickname); a change rewrites the file whole through a
  temporary file + rename; names must be NHOOK_[A-Z0-9_]+, values finite, ids pub/<nick> or test/<nick> (no paths). The tab groups
  37 knobs (shooting, aim, moving in a fight, cover, what it goes for) with the code's defaults, a ↺ per knob (back to the launch
  value) and a "Max aggression" preset + its undo. lk_tick now compares the file's change time to the NANOSECOND (two page writes in
  one second with the same size were missed). A game started before this answers 404 for /walker.json: the tab says to restart it.
  shim/tools/stats-proxy.py (the walker's page on 8765) passes Cache-Control on -- the browser kept an old page after an edit.
- **Fire whenever the aim is on an enemy in the weapon's reach; track the real head with every weapon** (2026-11-09 10:50-11:05;
  nbot.c the fire gate, the aim point, the trigger; nbrain_beh.c beh_follow) — WHY: the user: "it should always be shooting
  aggressively. crank that dial to max", then "it should ALWAYS be *allowed* to fire IF it can hit an enemy in range of the weapon in
  its hand", "that 32% accuracy can't help ... missing that head hitbox!", "you should not be firing at a static height, rather
  attempting to track heads with the aiming reticule just like any real human would". The public walker's [firegate]: fire allowed
  11-68% of the steps with a target within 40 units, the trigger down in only 31-51% of those; its head share 0 (automatic weapons
  aimed at a fixed chest height). LOGIC: the trigger no longer waits out the reaction delay (it only slows the first turn;
  NHOOK_BOT_FIRE_REACT=1 = the old wait); the reach is the held weapon's -- sniper 120, precision / rockets 60, automatic 40, shotgun
  12, sword / hammer 4 units -- x NHOOK_BOT_FIRE_RANGE (it was 40 for all); the aim follows the head hitbox (bot_head, the skeleton's
  head node) with every weapon (NHOOK_BOT_HEAD_ALL, 1; 0 = the old chest height for automatic weapons). New live knobs:
  NHOOK_BOT_HOSE (was launch-only), NHOOK_BOT_FIRE_LINGER (keep firing up to 0.6 s after the target ducks; 0), NHOOK_BOT_PULL_PER
  (single-shot trigger period, 0.24), NHOOK_BOT_NOWING (no following; was launch-only). [firegate] now gives "fire allowed" as a share
  of the steps the walker could SEE the target in reach. The first max-aggression preset (fire slack 6 deg, trigger held at every
  range, firing 0.4 s after the target ducked) put the trigger down 92-100% of the allowed steps but cost accuracy; the preset now
  keeps a 0.5 deg slack, bursts at range and no lingering: skill 1, fast turns (3 / 14 rad/s), single-shot period 0.16 s, sees
  within 4 units, pushes in, no back-off / recharge break-off, hunting (50) before small weapon upgrades (fetch base 35). Live, 15
  minutes on a 10-player Guardian: 24 kills / 25 deaths against 10 / 19 before, head share 0 -> 12%, accuracy 38% -> 43% (session).
- **Take cover when the shield is being stripped** (2026-11-09 11:00; nbrain_beh.c beh_cover, nbot.c the line-of-sight scheduler /
  g_cov_* / g_ftg_h) — WHY: the user: "the walker is aware of when its shield is getting decimated too eh? and like when it should try
  to take cover near by while continuing to shoot and defend itself aggressively?" -- it only had the break-off (shield under 0.15,
  back the way it came). LOGIC: the shield down NHOOK_BOT_COVER_DROP (0.35) within 1.5 s and under NHOOK_BOT_COVER_SH (0.5) with the
  target in sight -> up to 8 floor spots 3-6 units around (sideways first, never toward the attacker, its own floor part, within 1.5
  units of its level) tested from the attacker's eyes to where the walker's head would be, on the line-of-sight channel (2 ray
  slots of every 3 while searching; answers filed under 0xC0E00000 | generation << 4 | k). The first hidden spot: the legs go there
  bidding NHOOK_U_COVER (80) while the aim keeps tracking and shooting, until the shield is over NHOOK_BOT_COVER_UNTIL (0.9) or 6 s,
  or it is hit hard there again. None hidden -> fight on / the break-off. Live: searches ran in 2-10 unit fights; none of the 1-7
  spots was hidden in the first ones (close fights in the open) -- not judged yet. NHOOK_U_COVER=0 = off.
- **Press in with shields up (same level); strafe along a ledge instead of freezing** (2026-11-09 07:48-07:53; nbot.c the fight move,
  its floor check) — WHY: the walkers' battle recordings: closing speed 0.02-0.03 u/s in their engagements, the best players' 0.78 (0.85
  with shields up, 0.31 low; walker-model.py --top). The first press-in (+0.4 throttle with shield > 0.7 beyond the band's near edge) made
  a Guardian battle worse -- standing 51.5%, median 0.47 u/s: it pushed walkers toward enemies on a LOWER level and the floor check
  stopped them dead ("stopped" on 62-76% of fight steps): when the forward step and the strafe + forward step both left the floor it
  dropped the strafe too and never tried strafing alone. LOGIC: NHOOK_BOT_CLOSE (0.4, live; 0 = off) only toward a target within 1.5
  units of the walker's height, not when outranged or holding camo; at a ledge (all three steps fail) the strafe alone is tried, either
  side (|lat| or 0.6), before standing. The next Guardian battle: "stopped" 1% of fight steps, standing 28.1% near enemies (28.5-33% in
  the battles before the press-in), closing median 0.12 u/s.
- **[firegate]; the fire slack 1 -> 2 degrees** (2026-11-09 07:39-07:44; nbot.c the fire gate, the trigger) — WHY: walker-model.py over the
  walkers' own battle recordings against the top fifth of 128 public matches: reaction 0.32 vs 0.85 s and aim error 3.8 vs 4.1 deg (fine),
  but aimed at a target they fired 1.6 shots/s against the players' 3.85 and hit 19% against 47%. The new [firegate] line (every 20 s:
  of the steps with a target within 40 units, the share hidden / reacting / aim off / camo / head gate / fire allowed, and how often the
  trigger was really down) over a Guardian battle: hidden 23%, aim off 37%, allowed 33% -- the gate waited for the aim to sit within
  max(target size, 1.2) + NHOOK_BOT_FIRE_EXTRA (1) deg, ~1.5-2.5 deg at fight range, while the players shoot at ~4 deg off and let the
  aim assist / bullet magnetism pull the rounds in. LOGIC: NHOOK_BOT_FIRE_EXTRA defaults to 2 (live knob as before; 1 = old). The next
  Guardian battle: fire allowed 33% -> 56%, aim off 37% -> 25%; from the recording 1.0 -> 1.2 shots/s engaged, hit share 26% -> 20%
  (64-84 engagements; the battle's weapons -- hammers, shotguns, Excavators -- are not the public matches' mix): to be judged in public
  play (a live knob for the trainer).
- **Hunt one hidden target at a time** (2026-11-09 07:34; nbot_engage.c eg_pick) — WHY: on The Pit a walker logged "new target e41300ab"
  / "new target e4170175" alternating about once a second while both were hidden: the target draw (random by design -- exp(score) /
  sum, the players' own choice rule) ran on every step once the current target had not been seen for 1 s, and every flip changed the
  hunt goal and the path (a replan every ~0.5 s). LOGIC: a hidden current target is kept for NHOOK_BOT_HUNT_KEEP (5 s, live; 0 = old)
  after the last draw unless some enemy is in sight (vis_of == 1); the ENGAGE rule (a visible enemy within 10 units) is unchanged.
  The Pit (3 walkers, 3 min): "new target" 237 -> 36; the server's recording near enemies: median 1.65 u/s, standing 19.8%, across the
  enemy line 0.78 u/s -- the best players' 1.71 / 19.8% / 0.75 (fight-speed.py over 60 public matches).
- **Snapped back by the server = stuck** (2026-11-09 07:22-07:27; nbot.c the nav stuck test / the blocked step) — WHY: the 12.3 s still spell
  left on The Pit (and the 41.7 s one before) was at one spot, (-15.6, -3.3): the walker's log walked west to (-19.2, -4.2) and the next
  step stood at (-15.6, -3.3) again -- the server held its biped there (something blocks that way on the server only) and the client
  was corrected back, over and over; each return looked like a fresh detour to the no-progress test. LOGIC: a jump of over 1.5 units
  within 0.25 s while the legs walk a path (alive, on foot) is a snap-back: the stuck handler runs at once (no 1.5 s guard), logged
  "[nav] snapped back by the server to ...", and the blocked step is the path step LEAVING the snap spot (the path node nearest it up to
  pidx -> the next), not the client's far position. NHOOK_BOT_SNAPBACK=0 (live) = off. Only a jump BACK to a spot it stood on within
  the last 5 s (within 1 unit; a 50-entry 0.1 s position ring) counts: Valhalla's man cannons also move a walker 1.5+ units in 0.25 s.
  Valhalla (3 walkers, 3 min, the snap-back brain hot-loaded midway): the server's recording near enemies: median 1.51 u/s, standing
  13.8%, longest still spell 8.4 s; 7 snap-backs, 4 at one spot (86.2, -146.9). The Pit after that: 14 snap-backs detected, yet one
  walker held 43.7 s at (-15.6, -3.3) and 33.2 s at (-20.0, -7.7): the server held it whatever the first step, and each snap blocked one
  step, so the next plan went north again by a neighbouring step (it got free only when a plan went south). Since 07:29 a snap-back also
  adds NHOOK_NAV_BLOCKPEN to every path node the client walked before it (kb+1 .. pidx+2) for the session ("[nav] snap-back: N path
  nodes the server never let it walk cost +25"). The Pit with it: 6 snap-backs, the longest spells at (-15.6, -3.3) 3.2 s and 5.9 s
  (43.7 s before), standing 21.6% near enemies, median 1.44 u/s. 08:01: the position ring holds 12 s (120 entries; it was 5 s -- a later
  Pit battle held a walker 30 s at the same spot with only 3 snaps seen). OPEN: that hold began right after the walker TOOK the
  Excavator at its spawn (-16.9, -3.6) with act, and the first snap of the earlier battle came while fetching a weapon: the server may
  hold a walker's biped around a pickup / act press. Next: log act presses beside the snap-backs. 08:06, the next Pit battle with act
  timing on the snap-back line: EVERY snap-back came with the legs on "fetch a better weapon", one walker 6 times in 35 s at (17.7,
  14.2) toward one weapon (51.6 s held), act last pressed 12-47 s before -- not the act press; the fetch never gave up because the
  client kept getting closer. Two snap-backs within 20 s while fetching drop that weapon (g_fetch_abort -> the pickup code's tried list,
  "[fight] the X: the server put me back twice on the way -- left"). Why the server holds a fetching walker is still open.
  Lead (08:12): snap-backs gather on ONE test identity -- test-third (w2): 3-7 a battle against 0-2 for test-v2 / test-second over 6
  battles -- the only one whose settings.json has "frame_limit": 60 / "auto_frame_limit": true (60 fps against ~590 uncapped). The PUBLIC
  walker's profile (native/public-walker/home/.../ProjectReclaimer/settings.json) has the same 60 cap. One battle with test-third
  uncapped (373 fps): 2 snap-backs, still a 17.9 s hold at (-15.6, -3.3) -- not conclusive; its settings were put back
  (settings.json.bak-0830 = the original). Every snap-back is logged now, so the next public session shows whether it happens there.
- **No progress = stuck** (2026-11-09 07:15; nbot.c the nav stuck test; fight-speed.py --spells) — WHY: with the alarm fixed, a battle on
  The Pit: the server's recording (fight-speed.py --spells: per slot the standing share and the longest still spells) had one walker
  standing 68% of its time, 41.7 s at one spot and 17.9 s at another, while its own log said it walked (4% still): it hovered at
  (-16, -3.7), sliding back and forth along an obstacle -- always over 0.35 units in 1.4 s, so never "stuck" -- replanning paths of
  111-145 nodes. LOGIC: while the legs follow a path, the path left (plen - pidx) to the SAME goal must shrink by 2 nodes within
  NHOOK_BOT_NOPROG (4 s, live; 0 = off) -- a new plan 4+ nodes longer (a detour) starts the count over -- else the stuck handler runs
  (back off, jump, block the step, penalties, replan), logged "[nav] no progress for 4 s at ...". The Pit again (3 walkers, 3 min): the
  server's recording: median 1.39 u/s with an enemy within 15 units (0.78 before), standing 20.7% (41.4%; the best players 19.8%), longest
  still spell 12.3 s (41.7 s), 4,359 fight steps (575), 15 "stuck", 3 "no progress".
- **The stuck alarm fired on every step: its reset was inside a comment** (2026-11-09 07:05; nbot.c the nav stuck test) — WHY: a step
  trace (new NHOOK_BOT_STEPLOG=<s>: every 0.1 s for that many seconds of gameplay -- position, velocity, view / move yaw, throttle,
  legs) showed a walker moving in 83% of its 1.5 s windows while "stuck at" came 3 times in one 0.1 s step. aa7733b (01:07) appended
  "&& !(g_bot_fighting && tdist < 3)" plus a // comment to the test's line, and the statements that followed on that line -- tjump = now;
  hh = 0; tback = now; bstr = ... -- became part of the comment. From then on, once a walker made under 0.35 units in 1.4 s, every step
  was a "stuck": +NHOOK_BOT_STUCK_PEN on 10 path nodes, a blocked step (since 06:42), a replan from scratch (~10 a second), "4 in 10 s"
  always true (the wedge escape of 14:50 "fired constantly"), and the back-off / jump / short look-ahead never ran (they key on tjump /
  tback). 239-669 "stuck at" a 3-minute battle came from this. LOGIC: the statements on their own line again. A scan of native/*.c and
  src/nav.c for code after a mid-line // (a tail ending in ";" with an assignment) finds nothing else. The day's stuck measurements
  before 07:05 were taken with the broken alarm -- "jumping did not help", "the wedge escape made it worse", the off-lane cost and the
  lift penalty are unjudged; the blocked steps learned under it were moved to maptop/guardian.blocked.txt.bug-0730 (161 steps, mostly
  bogus: a step was blocked at every step of a slow stretch). Guard (07:58): shim/tools/comment-scan.py finds code after a mid-line //
  (a comment tail ending like a statement); shim/native/build.sh runs it on native/*.c + src/nav.c and prints a WARNING (non-fatal).
  Battle 8 (3 walkers, 3 min, Guardian): 50 "stuck at" (26 fetch, 22 hunt),
  2 "4 times", no traps; the server's recording (fight-speed.py --dir battles/tracks): median 1.07 u/s with an enemy within 15 units,
  standing 28.9% (battle 6: 0.84 / 38.7%); 4,291 fight steps against ~2,400 before (less wandering). fight-speed.py --dir / --all-groups
  measure the walkers' own battle recordings the same way as the players'.
- **The walker fights from the BEST players' model** (2026-11-09 06:55; nbot.c bm_load / bot_models_tick) — WHY: the walker's fight
  numbers (reaction, strafe switch, aim error, distance band, run speed) came from tracks/walker-model.json -- every player of 6 old
  recordings (reaction 1.08 s); the goal is the best players. LOGIC: walker-model.py --top --out tracks/walker-model-top.json (the top
  fifth by K/D of 128 recordings: reaction 0.85 s, strafe switch 0.74 s, aim error 4.1 deg, bands 8.0 / 15.1 / 25.8, run 1.95 u/s) is read
  first when it exists and NHOOK_BOT_MODEL_TOP (live, default 1) is on; 0 = the everyone model (re-read on a switch or a change of the
  file). The per-map lanes (walker-model.json "maps") still come from everyone's model. Not refit by refresh-loop.sh yet: rerun the
  --top command after new recordings. Also the fight GAIN (06:58; nbot.c before the fight's floor check): battle 5's [moves] said
  "moved 1.09 u/s, asked 1.19 u/s" -- timid, not blocked -- against the best players' 1.71: r and lat are scaled by
  NHOOK_BOT_FIGHT_GAIN (1.4, live; 1 = old), each kept within -1..1, before the floor check (not while kiting a sword / hammer).
- **The walker moves with authority: committed policy moves, the blend keeps its speed, the high ground sways, traps** (2026-11-09
  06:45-06:50; npolicy.c pol_move, nbot.c the policy blend / ARB_HOLD legs / the stuck handler, src/nav.c nav_trap; new
  shim/tools/fight-speed.py) — WHY: the user: the best player "moves more nimble ... with authority". fight-speed.py over 60 recorded
  public matches (an enemy within 15 units, speed over 0.5 s): the top fifth by K/D move at a median 1.71 u/s and stand (< 0.5 u/s)
  19.8% of the time, the others 1.51 u/s / 32.9%; the walkers' [moves] lines in local battles: 0.97-1.27 u/s. The learnt policy drew a
  fresh mixture component and fresh noise every 0.1 s -- a dither the engine's acceleration averages away -- and its 50/50 blend with the
  fight moves cancelled when the two disagreed. Holding the high ground stood still ("[still] 73% of 30 s ... hold 58%"; the user, who
  won 3-0 from above, moved ~0.5 u/s there). Learned blocked steps showed traps: one Guardian node had 5 of its 8 steps blocked.
  LOGIC: the component and its two normal draws are kept for NHOOK_BOT_POL_COMMIT (0.5 s, live; 0.1 = old), the means still follow the
  moment every 0.1 s; after the blend the move is scaled up to the faster of the two moves' speeds (NHOOK_BOT_POL_KEEPSPEED=0 = old);
  ARB_HOLD sways at NHOOK_BOT_HOLD_SWAY (0.3, live; 0 = old) throttle across the view, turning every 0.9-1.3 s or where the floor 1 unit
  to that side is not its own level (a graph node within 0.6 across, 0.5 up / down); a node with 3+ blocked steps is a trap (A* adds
  2 x NHOOK_NAV_BLOCKPEN to enter it), and a stuck inside one runs the escape for NHOOK_BOT_TRAP_ESC (2 s, live; 0 = off) -- only there.
  Battle 4 (3 walkers, 3 min, after all of it): stucks 308 -> 239 (17 trap escapes), [still] 19-24% -> 9% of the time alive; the fight
  speed stayed 1.25 u/s, so [moves] now also prints the speed the fight ASKED for ("asked N u/s": hypot(r, lat) after the floor check x
  the run speed) to tell a timid fighter from a blocked one. walker-model.py --top = the model of each match's top fifth by K/D only
  (128 recordings: reaction 0.85 s vs 0.94 for everyone, closing in 0.78 vs 0.40 u/s, strafe switch 0.74 vs 0.81 s, 3.85 vs 3.28
  shots/s, hit share 47% vs 41%) -- the default model (6 old files, reaction 1.08 s) is an average player's.
- **Fewer walker stucks: a reached goal leaves the draw, short look-ahead at a path's start, fetches by the path, lifts last, learned
  blocked steps** (2026-11-09 06:15-06:45; nbot.c the nav step / the FETCH branch / the stuck handler, src/nav.c nav_astar /
  nav_block_step / nav_blocked_load) — WHY: local 3-walker battles (test identities, our dedicated server) logged 360-670 "stuck at"
  in 3 minutes. The stuck line now names the legs, the path index and the goal, which showed: (1) the goal draw handed a reached hunt
  goal straight back ("path of 2 nodes ... reached the goal" in a loop, standing still); (2) 601 of 610 stucks were at path index 0:
  steering at the node 6 ahead cut the corner into the wall right at a path's start (cap: 2 nodes for the first steps, 1 for 3 s after
  a stuck, live NHOOK_BOT_LOOK_STUCK) -- 610 -> 358; (3) 412 of 666 came while fetching a weapon: within 3 units ACROSS the walker
  walked straight at it even on another level (an Excavator 1.6 units across, its path 27 nodes round) and pushed into the wall for a
  minute; (4) the rest at the first step of a fresh path, the replan from the spot choosing the same step again (0/141, 0/192, 0/164 at
  one spot); (5) one at a lift's foot, the next node 5.5 units up. LOGIC: (1) gl_drop(now, "reached"); (2) the look-ahead cap; (3)
  the straight walk needs |dz| < 1 and a path of at most 2 x distance + 4 nodes, else the path's way; the walk goes through the weapon
  at half speed (takes come walking over it, ~0.8 units); (4) every stuck marks the step being pushed along (the walker's node -> the
  path's next, one of its 8 neighbours) blocked: a per-node bit mask g_nav_blk, +g_nav_blkpen (25, live NHOOK_NAV_BLOCKPEN, 0 = old) on
  that step in A*; a step blocked twice in a session is appended to maptop/<map>.blocked.txt ("x y z k": the node's floor point and
  the direction, found by position again after a graph rebuild) and loaded with the graph (PELOAD_NAV_NOBLOCKED=1 = ignore the file);
  (5) a portal rising more than 2 units costs g_nav_liftpen more (40, live NHOOK_NAV_LIFTPEN, 0 = old; the loader leaves it 0).
- **Stucks only while walking a path; the wedge escape off by default; an off-lane path cost** (2026-11-09 15:10-15:30; nbot.c the nav
  stuck check / escape, src/nav.c nav_astar g_nav_offlane) — WHY: a local 4-walker battle (test identities, our dedicated server) after the
  kick: 300-460 "stuck at" and 100-150 "wedged" a walker in 5 minutes. Stucks were also counted while a fight held the legs (the walker
  strafes instead of walking the path), and "4 stucks in 10 s" was nearly always true, so the 3 s escape ran most of the time (its
  penalties piling up) -- it also overrode fight moves, live in the public game from 05:40 until the kick at 05:42. The stuck rate itself
  is old (402 in a 01:09 battle), spread over Guardian's upper level, the path's next node often 0.8 units up. LOGIC: the stuck check needs
  the legs on a path behaviour (not FIGHT / HOLD); the escape never overrides a fight's legs and runs only with NHOOK_BOT_WEDGE_PEN > 0
  (default 0 now); NHOOK_NAV_OFFLANE (env or live; 0 = off, the loader's) adds that much x the step cost on graph nodes no recorded player
  was seen on (g_nav.pop 0), so paths keep to the players' lanes.
- **A kicked walker stays out** (2026-11-09 15:00; shim/tools/walker-public.sh) — WHY: at 05:42 the walker was kicked from the
  owner-approved server ("You were kicked from this server. You can rejoin in 9 minutes.") -- the user's terms: every player can boot it,
  so anyone with an issue can get it out at once; a kick is that signal, and the patient rejoin loop would have come back after the
  cooldown. LOGIC: after each attempt the loop looks at the walker's log; a kick ends the loop ("the walker was kicked from ...: not
  rejoining"). The walker was stopped by hand this time; nothing rejoined.
- **Wedged: escape and fence off the spot** (2026-11-09 14:50; nbot.c the nav stuck handler, g_escape_*) — WHY: jumping on every stuck did
  not help (259 "stuck at" lines in the next 3000, 188 before): the walker sat WEDGED at (60.8, -146.9) / (60, -58) on Valhalla for minutes
  ("stuck 4 times ... a new goal" 144 times -- each new goal led back) and died there (0 kills / 7 deaths in a window). LOGIC: at 4 stucks
  within 10 s every graph node within 2.5 units (2 in height) costs NHOOK_BOT_WEDGE_PEN (60, live) more and an ESCAPE runs 3 s: a random
  compass direction at full throttle, a new one every 0.5 s, a jump every 0.6 s, overriding every other leg move ("[nav] wedged at ...").
- **Jump when stuck; route around stuck spots** (2026-11-09 14:45; nbot.c the nav stuck handler) — WHY: on Valhalla "[nav] stuck at ...:
  back off and replan" 188 times in 3000 log lines, ~50 at one spot ((61, -148)) -- the +4 node penalty never sent the path around it and
  the walker never jumped (Valhalla's rocks and lips free a player with a jump); a 4 kills / 26 deaths match. LOGIC: every stuck event
  also jumps (0.25 s) and adds NHOOK_BOT_STUCK_PEN (8, live; was a fixed 4) to the next 10 path nodes.
- **Policy A/B result; a general knob A/B** (2026-11-09 14:35; follow-study.py knob_ab, --ab N --ab-set "K=v,..." --ab-name NAME) —
  RESULT of --policy-ab 8 (14:00-14:35, The Pit / 9-15 players): policy ON (0.5) 5 kills / 14 deaths in 4 windows (K/D 0.36, 3.5 deaths a
  window), OFF 3 / 23 in 4 (0.13, 5.75 deaths a window) -- the learnt policy stays on. NEW: any set of live knobs alternated SET (B) /
  REMOVED (A: the brain's defaults) per window, the tally in battles/follow/ab-<name>.json; first use: the retreat rules (break off,
  out of crossfires, recharge) off vs on.
- **A clean policy A/B** (2026-11-09 14:00; follow-study.py --policy-ab N) — WHY: accuracy slid 66% -> 28% over windows that changed the
  opponents (a new 16-player match), the policy's weights (its 04:44 retrain) and a string of rules at once; the trainer's challengers
  nudge one knob among many, too noisy to tell whether the learnt policy helps. LOGIC: for N windows the study writes NHOOK_BOT_POLICY
  0 (OFF) and the trainer's incumbent weight (ON) alternately at each window start (after every other knob write), credits each window's
  kills / deaths to the phase that ran it and prints the tally (battles/follow/policy-ab.json).
- **Outranged from 1.2x the band** (2026-11-09 13:50; nbot.c the OUTRANGED block) — WHY: the death summary's mean killer distance rose
  to 16 units; a sniper killed the walker at 14 units, under the 1.5x threshold (~15 units for the assault rifle's band). LOGIC:
  NHOOK_BOT_OUTRANGE is now the factor (1.2, live; 0 = off) on the held weapon's band far edge (p75 x DIST).
- **Empty mid-fight: switch first** (2026-11-09 13:40; nbot.c the EMPTY block) — WHY: "[death] ... ASSAULT RIFLE 0 rounds" at 4 units
  against two enemies: the walker reloaded first (right while switching was unreliable); with the learnt switch button ("3 of 3 took")
  the Halo answer is the switch -- faster than a reload. LOGIC: an empty magazine with an enemy in sight within 20 units (g_eng_close),
  the switch button learnt and the other weapon not known empty -> the switch; else the reload (act tap + trigger click) as before.
- **Steadier aim with precision weapons** (2026-11-09 13:35; nbot.c the aim noise) — WHY: battle-rifle duels lost on the headshot race at
  8-14 units ("my shield 0.06 health 1.00"): the head is about +-0.3 deg at 14 units, the wander +-0.6 at level 4. LOGIC: with a precision
  weapon / sniper the OU wander x NHOOK_BOT_NOISE_PREC (0.7, live). (Also measured: the learnt switch button "3 of 3 took".)
- **The Spartan Laser's trigger stays down through the charge** (2026-11-09 13:25; nbot.c the trigger) — WHY: a death holding a Spartan
  Laser at 8 units; it fires only after a held charge and the aim-error gate released the trigger whenever the error spiked, restarting
  the charge. LOGIC: with a SPARTAN LASER in hand and the target in sight within 40 units the trigger is held (the aim keeps tracking).
- **The melee lunge from the players' numbers** (2026-11-09 13:20; nbot.c the melee press, shim/tools/melee-lunge.py) — WHY: the walker
  meleed within 1.22 units (1.45 in a race) and lost beatdown after beatdown; guessed ranges had fallen short (1.67 whiffed).
  melee-lunge.py measured 10 recorded melee kills: the killer's lunge (its fastest step in the last 0.6 s) began at a median 2.24 units
  (p25 1.78, p75 3.28), the kill at 1.45, the victim's shield then a median 0.57, the killer's full. LOGIC: within NHOOK_BOT_MELEE_LUNGE
  (2.0, live) units with the target's shield under NHOOK_BOT_MELEE_LUNGE_SH (0.7, live) the melee goes out (not with a sword / hammer of
  its own, never into a kited swinger).
- **Battery weapons have no magazine** (2026-11-09 13:15; nbot.c bot_ammo_of) — WHY: "DRY: PLASMA RIFLE still empty after 2 reloads"
  and a death "PLASMA RIFLE 0 rounds": the learnt magazine field (+0x23a) reads 0 for a plasma rifle all its life (a battery, no rounds),
  so it counted as empty -> reload clicks, DRY, the empty-gun rush. LOGIC: the field counts for a weapon definition only once it has read
  above 0 for it; before that the rounds are unknown (-1: no EMPTY / DRY handling).
- **RECHARGE: back off to refill the shield** (2026-11-09 13:10; nbrain_beh.c beh_recharge, nbot.c ARB_RECHARGE) — WHY: the death notes
  show a death every ~18 s (the best players about one a minute), most of them close fights lost on the last sliver of health, and the
  walker went from one fight straight into the next hunt with its shield broken. LOGIC: a new arbiter slot (a path to its point): with the
  shield under NHOOK_BOT_RECHARGE_SH (0.6) and no enemy in sight within 20 units (g_eng_close), 6 units away from the nearest enemy,
  for up to 6 s or until the shield is over 0.95, bidding NHOOK_U_RECHARGE (65, over the fight's 60 for a far / hidden target; a fight in
  sight is not bid against). NHOOK_U_RECHARGE=0 = off.
- **Kite swords and hammers sideways, never melee into them** (2026-11-09 13:00; nbot.c the KITE move, g_kite_t) — WHY: "[fight] a
  GRAVITY HAMMER at 4.0 units: kiting it", then 3.3, 1.8 units, a beatdown at 0.92 on its broken shield -- and the hammer swing killed
  the walker first: back-pedalling is slower than a charge, and a melee race with a sword / hammer is lost. LOGIC: kiting = full
  strafe (1.0), back at half (-0.5), shooting; no melee press within 0.5 s of a kiting step.
- **Arm's length first: the nearest threat is the target and is "seen"** (2026-11-09 12:50-12:55; nbot.c target choice, vis) — WHY: a
  0-kill / 9-death window on Guardian (14 players): every death at 0-6 units with a nearly full magazine, 3 melees in it; a quarter of the
  targets within 3 units read "hidden" (the melee and the trigger wait for "in sight"); a death fighting a target 9 units off while an
  enemy 1 unit away shot it. LOGIC: within NHOOK_BOT_SEE_NEAR (2.5, live) units and 1.5 in height a target counts as seen; after the
  choice rule an enemy within NHOOK_BOT_NEAR_THREAT (3, live) units and 2 in height, closer than half the chosen target, becomes the
  target ("[fight] ...: the one at arm's length first").
- **brain-timeline.py: every training window with the brain that played it** (2026-11-09 12:45; shim/tools/brain-timeline.py) — WHY: "each
  training session reload you should describe what the new brain is trying to show that it learned" -- the lesson of a brain and the
  windows it played live in two files. LOGIC: battles/follow/learned.jsonl (one line a window) joined with battles/follow/brain-lessons.jsonl
  (one line a build: its lesson) by the window's middle; per brain its windows and sums (kills / deaths / K/D / accuracy). First reading:
  before the lessons (03:31-03:40) 7 / 24 (0.29); 03:43-04:29 21 / 35 (0.60). --since HH:MM, --json.
- **Automatics aim at the chest; out of crossfires; the melee race at 1.45** (2026-11-09 12:35-12:40; nbot.c the aim point, the
  OUTNUMBERED block, live knobs) — WHY: since 08:00 every weapon aimed at the head hitbox -- right for a battle rifle / carbine / magnum
  / sniper, wrong for an assault rifle (no headshot bonus, the smallest part: fewer hits a second); most death notes had 2-4 enemies
  aiming at the walker at once; a melee race at 1.67 units fell short and the enemy's melee landed (0 u, shield 0.03, health 1.00).
  LOGIC: precision / sniper classes aim at the head hitbox (+ HEAD_DZ, honed), every other class at the centre + NHOOK_BOT_AIM_BODY
  (0.2, live) x the radius; with 2+ enemies within 20 units aiming within 12 deg and the walker's shield under NHOOK_BOT_CROWD_SH (0.5)
  it backs off at full speed with a full strafe while shooting (NHOOK_BOT_CROWD=0 off); NHOOK_BOT_MELEE_RACE 1.8 -> 1.45 (live knob).
- **The sight ray goes to the head; an attacker counts as seen; no close-range gun as the roaming weapon** (2026-11-09 12:25-12:30;
  nbot.c the LOS request / the shot-at override / the better-weapon switch) — WHY: a death note "fighting efef0095 at 2 u (in sight) ...
  aiming at me: efef0095 2 u = my target" came after the walker's own lines called that target "hidden, hunting" at 6.5, 4.1 and 3.3
  units -- no fire, no melee; over 30k log lines close targets read hidden 26% of the time within 3 units, 35% at 3-6 (the ray went to
  0.3 x the radius above the centre: waist-high cover blocked it while the head stood clear); another death: an 8-unit fight with a
  Mauler in hand (2 rounds), put there by the better-weapon rule while roaming. LOGIC: the current target's ray goes to its head
  hitbox (bot_head), other enemies' to 0.6 x their radius above the centre; a target aiming within 12 deg at the walker while it was hurt
  in the last 0.5 s counts as seen within 25 units ("[fight] ... is shooting me ...: seen"); out of a fight the better-weapon rule never
  puts a shotgun / melee-class weapon in hand (the range-band switch brings it out when an enemy is close).
- **The melee race; kite swords and hammers** (2026-11-09 12:15-12:20; nbot.c the melee press, the KITE block) — WHY: the first death
  notes: killed at 0-3 units with the walker's shield gone and its health 0.78-1.00 (lost beatdowns: once a shield is down the first
  melee wins, and the lunge reaches past the players' median landing distance the walker waited for, 0.97 + 0.25); killed at 1 unit
  with FULL shields by an enemy holding a melee weapon (a one-hit kill). LOGIC: within NHOOK_BOT_MELEE_RACE (1.8, live) units with the
  walker's or the target's shield under 0.15 the melee goes out at once (not with a sword / hammer of its own: their swing ranges
  stay); a target holding a melee-class weapon within NHOOK_BOT_KITE (5, live) units -> throttle full back, the strafe at least 0.8,
  shooting, after every rush / policy rule (the floor check still applies). NHOOK_BOT_MELEE_RACE=0 / NHOOK_BOT_KITE=0 = off.
- **Death notes** (2026-11-09 12:05; nbot.c DeathSnap / death_note) — WHY: the walker dies to enemies it is not dueling (battle-rifle
  headshots, snipers) and its target choice already turns to attackers when hurt (nbot_engage.c eg_choose): the next fix needs the
  facts. LOGIC: each step keeps its target (handle, distance, in sight) and the enemies within 40 units aiming within 12 deg of it (the
  likely killer: handle, distance, weapon class), its shield / health, weapon and rounds; at the alive -> dead change a [death] line,
  every 8 deaths a [deaths] summary (killer not my target, nobody aiming seen, killer 20+ units away, mean killer distance).
- **The fighting distance is copied only from close-range role models** (2026-11-09 12:00; follow-study.py train) — WHY: today's best
  player killed from 26 units with a sniper rifle and the trainer's challenger pulled the walker's distance halfway toward it (DIST 0.82
  -> 1.01) -- assault-rifle fights from sniper range. LOGIC: DIST follows the role model's kill distance only when its two most common kill
  weapons are no sniper / battle rifle / carbine / beam rifle / DMR / magnum; the running challenger's DIST was reset to the incumbent's.
  (Also live since 11:55: the learnt switch button took at once -- "switching from ASSAULT RIFLE to EXCAVATOR", then holding it.)
- **The switch button found (record bit 0); outranged = no charge; the policy learns being under fire** (2026-11-09 11:45-11:55; nbot.c
  swscan_tick / the OUTRANGED block / g_bot_outranged, nbrain_beh.c beh_fight, policy-train.py FEAT, npolicy.c) — WHY: the switch scan
  hit control-record bit 0 four times in seven presses (no other bit more than once) -- under the 75% bar, a press can fall in a
  reload / firing animation; today's best player sniped from 26 units (20 sniper kills, standing still 62%) while the walker's rifle
  pulled it across the open into the scope; most of its deaths come from enemies it is not dueling. LOGIC: a bit is adopted at 3+ hits
  and 55%+, also straight from the saved counts when a brain loads ("[swscan] LEARNED: ... bit 0"); every switch presses it 0.1 s.
  OUTRANGED: a target 1.5x beyond the held weapon's band (p75 x DIST) holding a sniper / precision weapon, the walker holding neither
  (nor a rocket) -> no closing in (r 0), a full strafe, no fire, and the fight bids NHOOK_U_FIGHT_OUTRANGED (35, under a weapon fetch's
  45): go get a long gun. NHOOK_BOT_OUTRANGE=0 = off. POLICY: two more inputs (29): the player's shield change over the last 0.3 s and
  the enemies within 25 units looking within 15 deg of it; npolicy.c builds both and runs a 27- or a 29-input policy.
- **The empty pull reloads; the first shot as fast as the best player** (2026-11-09 11:35-11:40; nbot.c g_bot_click_until / the EMPTY
  block, trainer.json) — WHY: "EMPTY BRUTE SHOT: reloading from 5 in reserve" twice and still empty; the user: "reload click noise is a
  signal in the game" -- Halo reloads an empty gun when its trigger is pulled; and "[the best player] ... engages faster": the walker
  fired ~1.0 s after acquiring a target (the average player's reaction 1.08 s x (1.25 - skill) x REACT 1.223), the best player here 0.7 s.
  LOGIC: on a 0 in the magazine with reserve rounds the fire bit goes out for 0.12 s every 1.5 s (the click) as well as the act-tap reload.
  The trainer's incumbent REACT 1.223 -> 0.9 (first shot ~0.73 s after acquiring; still a trained knob, trainer.json notes).
- **Reload first on an empty magazine; the switch scan judges by hit rate** (2026-11-09 11:25-11:30; nbot.c BotInv.reserve, the EMPTY
  block, swscan_tick) — WHY: "EMPTY BRUTE SHOT: switching to ASSAULT RIFLE" for a minute -- the walker stood in fights with an empty gun
  while switch pulses failed (packet pulses: 1 of 2 took), but every act-tap reload worked; and the scan's single "hits" (bits 0, 3, 35)
  were noise: the slot also changes from late packet pulses and pick-ups. LOGIC: the reserve = the int16 before the learnt magazine
  (+0x238 here: 117 / 64 / 160 seen); on a 0 in the magazine with reserve rounds the walker reloads at once (every 2.5 s until loaded),
  switches only with the reserve out, then any weapon on the floor (DRY). The scan's tries / hits per bit persist across brains
  (native/switch-bit.txt.scan); a bit is adopted at 3+ hits and a 75%+ hit rate, up to 6 presses of a bit in a row, a bit switching under
  half the time after 3 tries is dropped.
- **A mixture policy sampled, the aim lead off, the switch scan isolated** (2026-11-09 11:05-11:20; policy-train.py --mdn, npolicy.c,
  nbot.c swscan / bot_switch_press, follow-study.py LEADS) — WHY: the one-move policy predicts the AVERAGE of what the best players do
  -- a player strafes left OR right, the mean is a timid half-move: not "nimble"; the aim-lead honing's first try (0.04 s) registered
  0.20 hits a shot against 0.70 with no lead (125 / 35 shots) and the window went 0 kills / 4 deaths at 19% accuracy -- this client's
  hits register where it sees the target; the switch scan's first two "hits" (bits 0 and 3) were confounded by the empty-magazine
  handler's own switch pulses. LOGIC: policy-train.py --mdn K (default 5): the network outputs a K-Gaussian mixture over the next
  move (K logits, K x 2 means, K x 2 log sigmas) + the jump logit, trained by likelihood on the GPU (28 s, 10 epochs); npolicy.c samples
  a component, then a move from it, every 0.1 s (the last two moves are inputs: sampled strafes chain into the players' rhythms);
  either shape loads ("one mean move" / "a mixture of moves (sampled)"). The aim lead defaults to 0 and is honed only among 0 / 0.02 s.
  While a scan candidate is pressed and watched, bot_switch_press sends nothing, and a candidate waits 1 s after any other switch.
- **The head gate holds fire only for the kill shot** (2026-11-09 11:00; nbot.c the precision fire gate) — WHY: 5 of the walker's last
  12 deaths were battle-rifle headshots: it lost the precision duels; the always-on head gate (fire only within the head's angular size
  + 0.3 deg) is about the aim's own spread at 20+ units, so it halved the rate of fire while the target's shields were still up -- and
  with shields up a body hit strips them as fast as a head hit. LOGIC: the aim point stays the head hitbox; the head gate applies only
  while the target's shield is under NHOOK_BOT_HEADGATE_SH (0.1, live): the kill shot. NHOOK_BOT_HEADGATE_SH=2 = the old always-on gate.
- **The brain finds the weapon-switch button itself** (2026-11-09 10:50; nbot.c swscan_tick / bot_switch_press) — WHY: the packed
  packet's +0x2c bit 1 (seen with the user's switches) rarely switched when the walker sent it ("EMPTY BRUTE SHOT: switching" again and
  again; held 0.15 s it never did, 0.03 s now and then). Like fire -- its packet bit alone does nothing, the control record's bit 19
  fires -- the switch is likely a control-record button and the packet bit its consequence. LOGIC: out of a fight, with two weapons,
  one candidate bit of the record's button word (+0xC8) at a time is pressed 0.1 s (never jump 1, fire 19 / 21, grenade 30); 0.7 s later
  the unit's weapon slot (+0x262) is compared; a bit that changes it twice is adopted ("[swscan] LEARNED"), saved to
  native/switch-bit.txt and read back by later brains; from then every switch (empty magazine, better weapon, range band) presses that
  bit for 0.1 s instead of the packet pulse (bot_switch_press). NHOOK_BOT_SWSCAN=0 (live) = no scan. (A text replacement made the new
  helper call itself when no bit was learned -- brain #37, caught before its first switch; #38 fixed it within a minute.)
- **Every weapon ranked, no switch ping-pong, switch pulses counted** (2026-11-09 10:35-10:40; nbot.c bot_wpn_rank, the better-weapon
  switch, [inv] switch pulses) — WHY: "switching from SPIKE RIFLE / SMG to ASSAULT RIFLE" every 3 s: the Spiker, SMG, plasma rifle,
  needler, magnum and plasma pistol had no rank (unknown = 50, "worse than anything"), and in a fight the better-weapon switch and the
  range-band switch undid each other. LOGIC: those six ranked after the assault rifle (in that order); in a fight the better-weapon
  switch needs the held weapon OUT of its band as well as the other one in its band; 1 s after every new switch pulse the brain checks
  whether the weapon in hand changed and logs "[inv] switch pulses (P s): N of M took" every 2 minutes (NHOOK_BOT_SWPULSE live, set
  to 0.06 s while measuring).
- **The aim leads moving targets (host-side hit registration); the policy retrains itself and its weight is trained** (2026-11-09
  10:20-10:30; nbot.c the fighter's aim point, follow-study.py hone_lead / policy_retrain / TRAINED) — WHY: Halo 3 registers a client's
  bullets on the host with no lag compensation (non-host players lead by their ping), and a third of the walker's shots through a
  hitbox registered no damage (aim block reg 0.67); "keep crushing it and refining ... reloading the winning model". LOGIC: the aim
  point = the head hitbox + the target's velocity (units a second) x NHOOK_BOT_AIMLEAD (live, 0.06 s; z at half), honed each window
  among 0 / 0.04 / 0.08 / 0.12 / 0.16 s on REGISTERED hits a shot (reg x reg_n + miss_reg x miss_n over the aimed shots; untried first,
  25+ shots each, then the best, 1 in 4 random; aim-state.json lead_*). Every 6 windows (~18 min) follow-study.py starts policy-train.py
  on the GPU again with the newest matches (one at a time, battles/follow/policy-train.log); the brain reloads policy.json when it
  changes. NHOOK_BOT_POLICY joins the trained knobs (0.2-1.0; the incumbent starts at 0.5). NHOOK_BOT_AIMLEAD=0 = no lead.
- **No switch to an empty gun, range-aware upgrades, reachable fetches only, the policy's jumps drawn, [still]** (2026-11-09 10:00-10:15;
  nbot.c the switch press / the better-weapon switch / the pick-up scan and press / [still], npolicy.c) — WHY: "EMPTY SNIPER RIFLE:
  switching to ASSAULT RIFLE" every 1.5 s (the fight's band rule switched straight back to the long-range sniper without looking at its
  rounds); the better-weapon switch swapped a rifle for a shotgun mid-range; the recordings had the walker under 0.3 u/s 53% of its life
  and [still] (new: every 30 s, the still share by the legs' winner) put 65-79% of it on the weapon FETCH -- standing under a weapon on a
  ledge (more than 1.5 up: the press never began, nothing timed out); the policy made 0 jumps (a jump is rare in any 0.1 s sample, its
  probability never passed 0.5). LOGIC: a switch press goes out only if the other weapon is not known empty; in a fight the
  better-weapon switch needs the target inside the other weapon's band (bot_band_off <= 0.05); the scan fetches only weapons with a
  walking-graph node within 1 unit across, 1.3 below / 0.6 above them, and a walker within 0.6 across but 1.5+ apart in height leaves at
  once ("out of reach, left"); the policy's jump is drawn with its probability every 0.1 s. (Note: [still] reads 100% during the
  post-game screen -- every player is frozen there.)
- **Head tracking, no stands mid-fight, the better gun in hand, a learnt movement policy (GPU)** (2026-11-09 09:30-10:00; follow-study.py
  NOISE floor / lvl_knobs, trainer.json, nbrain_beh.c beh_fetch, nbot.c the pick-up look / the better-weapon switch / the policy hook,
  shim/native/npolicy.c, shim/tools/policy-train.py) — WHY: "[the best player] ... stays on the headshots no matter what the opponent
  does"; "this is a m4 mac so make sure the special GPU is being used"; "M4 has AI cores... use them for walker logic". FOUND / LOGIC:
  (1) AIM -- the fighter's aim wandered (an OU process) by the AVERAGE public player's median error, 4.06 deg x (1.25 - skill 0.5) x 1.6
  x NOISE 0.96 = about +-4.7 deg, while a head at 10 units is +-0.4: NOISE set to 0.15 (+-0.7 deg) in the trainer's incumbent and the
  live knobs, its floor 0.3 -> 0.05 (the bounds and the level rule). (2) With an enemy within 10 units the weapon fetch bids only
  NHOOK_U_FETCH_HOT (40, under the fight's 60) unless the weapon is within 2 units or the walker is dry -- it had stood on weapons
  pressing act for 2.5 s mid-fight ("moved 0.00 u/s" fight windows); at a weapon in a fight the legs take the fight's moves while act
  pulses. (3) Out of a fight the walker looks at the weapon it presses for (every failed take was a weapon on the floor; NHOOK_BOT_PICKLOOK
  =0 off); the better-weapon switch (2+ ranks) runs in fights too beyond 3 units -- confirmed: "switching from ASSAULT RIFLE to BATTLE
  RIFLE", then holding it (the one-packet switch impulse works). (4) POLICY -- policy-train.py: behaviour cloning on every recorded
  match (10 Hz): each fighting player sample (nearest enemy within 25 units) -> its next move in the enemy-line frame (toward / left,
  units a second) and its jumps, from 27 inputs (distance, height, shields / healths, enemy facing it, its view against the line, its
  last two moves, the enemy's last move, a second enemy, enemies within 10, weapon class); the top fifth of each match by (k+1)/(d+1)
  weigh 1, the rest 0.15; the walker's own slot left out; a 27-128-128-3 SiLU network trained with PyTorch on the M4 GPU (MPS) ->
  battles/follow/policy.json with the validation error against standing still / repeating the last move. npolicy.c (brain) reloads it
  on change, builds the same inputs from the walker's state at 10 Hz and runs it in microseconds on a CPU core (a Neural Engine call
  would cost more in dispatch than the network); the fight blends its move in by NHOOK_BOT_POLICY (live, 0 = off), its jumps at >= 0.5;
  the rush / arm's length / floor rules still apply on top. [policy] lines: loaded (validation numbers), moves per 30 s.
- **The walker learns its magazine field, never fights empty; one-packet button impulses; committed strafes; velocity is per second**
  (2026-11-09 08:30-09:20; nbot.c ammo_learn / bot_ammo_of / the EMPTY block / the floor check / [moves] / the pick-up press, build.sh +
  brain-lesson.txt, follow-study.py) — WHY: the user: "empty magazines sans reload are unacceptable btw. do better. find ammo, new weapon,
  or switch to alternate. reload click noise is a signal in the game. so clearly it goes somewhere"; "it is playing like garbage";
  "[the best player] simply moves more nimble, and engages faster, and stays on the headshots ... our walker does not move with that much
  *authority*"; "each training session reload you should describe what the new brain is trying to show that it learned". FOUND /
  LOGIC: (1) MAGAZINE -- the field ammo.json named (+0x1b4) is no count in this app; the brain now watches its own held weapon object
  (its handle; int16 fields +0x100..+0x3ff, 10 Hz) for one that steps down 1-6 while the trigger is held and jumps up on a reload:
  +0x23a (AR 32 -> 27 -> 13 ..., -2 = the reserve, 117 / 64 / 160), adopted after 15+ drops ("[ammo] LEARNED"), saved to
  ReclaimerMac/native/ammo-learned.json and read back by every later brain / launch. On 0 there: the other weapon if it has rounds
  (switch), else the reload tap at once (in fights too; reload no longer needs the virtual pad), and after 2 reloads still empty =
  DRY: any weapon on the floor counts as better (same kind refills); the grenade-and-beatdown rush only when DRY.
  NHOOK_BOT_AMMO_LEARN=0 (live) = off. (2) IMPULSES -- the packed record's +0x2c bits act like one-packet impulses (a held act
  "seats and then unseats", a held switch never switched, an act held 2.5 s on a weapon took nothing): switch and pick-up act go out
  for NHOOK_BOT_SWPULSE / NHOOK_BOT_ACTPULSE (0.03 s, ~one packet; act every 0.4 s at the weapon). First press pick-up: "TOOK the BATTLE
  RIFLE after 0.8 s". A taken weapon is matched by NAME (in hand it is another object handle than on the floor). Out of a fight the
  walker switches to the other weapon when it ranks 2+ better. (3) STRAFES -- the "never off a ledge" check flipped the strafe on
  23-60% of fight steps ([moves], new: asked / flipped / stopped, speed by position): a jitter in place = the recordings' "strafes 0.0
  u/s". Now the strafe keeps its side while that side has floor 1 unit off, else takes the open side and holds it 0.6 s, else stops;
  no check where the graph has no floor under the walker itself: flips 2%, 1.54 u/s in fights, 65% across the enemy line. (4)
  VELOCITY -- object +0x74 is units a SECOND (1.11 vs 1.04 u/s by position), not a tick: the grenade lead and the turret lead were
  x30 -- 30x too far ahead of a moving target (the lead honing had preferred the smallest value); fixed, the grenade honing restarted
  (nade-state.json keeps the old results under before_vel_fix). (5) LESSONS -- shim/native/brain-lesson.txt is compiled into each
  brain (build/brain_lesson.h): "[brain] lesson: ..." when it loads, appended to battles/follow/brain-lessons.jsonl; follow-study.py
  prints the lesson in play with each window. Also: the Mauler / Fuel Rod are known by their tag names (excavator / flak_cannon:
  shotgun / rocket classes, ranks after the shotgun).
- **No false "empty magazine" rushes; pick-ups walk onto the weapon** (2026-11-09 08:10; nbot.c bot_ammo_of, the pick-up press) — WHY:
  the walker ranked #21-#23 of ~24 with 30-40 deaths a match, killed mostly by battle-rifle headshots, snipers and shotguns while it
  held its spawn rifle. Its log: "empty magazine: a grenade at N units, then the beatdown" 111 times in one session -- the weapon
  object's rounds field (+0x1b4, ammo.json) read 0 for a rifle's whole life in the public game (129 now and then), so every fight
  began with a grenade and a punch rush at an armed player; and "at the weapon (1.3 units): holding act to take it" 8 times for one
  battle rifle in hand -- the press began 1.3 units off and the fetch ended with it, so the walker turned back before touching the
  weapon. LOGIC: the rounds field is trusted for a weapon definition only once it has shown NHOOK_BOT_AMMO_TRUST (3, live) different
  non-zero values (a magazine counting down); until then the rounds are unknown (-1: no empty rush, reloads after fights only). A
  fetch within NHOOK_BOT_PICK_R keeps its bid while act is held: the straight walk ends ON the weapon (full speed beyond 1 unit, 0.45
  to 0.3, then still), act held until the weapon is in one of the walker's slots ("TOOK") or NHOOK_BOT_PICK_HOLD (2.5, live) s pass
  ("could not take", tried; the log line gives the distance across / up and the arbiter's winner). NHOOK_BOT_AMMO_TRUST=0 = the old
  trust-at-once; NHOOK_BOT_PICK_HOLD=0 = press and leave as before.
- **The walker aims at the HEAD HITBOX; the brain's hitbox geometry works** (2026-11-09 08:00; nbot.c bot_head and the fighter's aim
  point, h3maps.c HB_CURMAP, nstats.c, follow-study.py hone_aim) — WHY: the same requests ("you can see all I can see for aim, tune,
  etc re: hitbox. don't make me a crutch"): the fighter aimed NHOOK_BOT_AIM_HEAD x the bounding radius over the bounding sphere's centre,
  a guess -- 8% of its 120 judged shots went through the head. LOGIC: bot_head poses the target's collision model with its node
  matrices (the aim meter's geometry: the region whose nodes sit highest = the head) and returns the head's centre and mean radius;
  the offset from the object's centre is kept 50 ms per target and added to the fresh centre; the fighter aims there plus
  NHOOK_BOT_HEAD_DZ head radii (live, 0; hone_aim now hones it among -0.5..+0.5 on head hits a shot, NHOOK_BOT_AIM_HEAD stays the
  fallback), and the precision head gate measures its error from there (width unchanged, 0.15 units). Live: mp_masterchief's head
  0.22-0.25 units above the centre, radius 0.07 -- the old point (0.55 x ~0.35) was at the neck. Brain only (its bot thread owns its
  own hb_geom cache; in the body the render / snapshot thread owns it). FOUND: in the brain build h3maps.c's hb_geom / vehicle-shape /
  name lookups called the brain's own renamed hb_current_map (empty: the brain never sees the map opens), so the brain had NO hitbox
  geometry ("geometry NOT found" for mp_elite / mp_masterchief); they now call HB_CURMAP, which the brain defines as the body's export
  (default hb_current_map: the loader and the body unchanged). NHOOK_BOT_HEADAIM=0 (live) = the old height.
- **The walker judges its own shots against the hitboxes, headless too; head-height aim, a head gate for precision weapons, the trainer
  scores accuracy and headshots** (2026-11-09 07:45; noverlay.c nov_aim_tick, nstats.c st_snap_thread, nbot.c the fighter's aim point
  and fire gate, follow-study.py hone_aim / the window score) — WHY: the user: "try to constantly bump up accuracy too. hitting the
  critical hitbox is key ... headshots being main goal", then "you can see all I can see for aim, tune, etc re: hitbox. don't make me
  a crutch in any way" and "the walker should be able to load the same dylib I do and have any function I had". The aim meter (every
  shot's path against the target's posed collision model: on = through a hitbox, head = through the topmost region) ran only in the
  Metal frame hook, and a --headless client never draws a frame, so the public walker had no measure of its own shots: the aim-height
  honing read them from the USER's client on 8765 (only while they played). LOGIC: the stats snapshot thread runs nov_aim_tick while the
  renderer has drawn nothing (g_nr_frames 0, i.e. headless: then nothing else touches hb_geom's cache), behind a try-lock so only one
  caller at a time; hone_aim reads the walker's OWN stats page (follow-study --port, the player marked "you"). The fighter aims at
  centre + NHOOK_BOT_AIM_HEAD x radius (live knob, was a fixed 0.55; honed among 0.45..0.9 by head hits a shot) and, holding a
  precision weapon or sniper, holds fire while the aim error exceeds the head's angular size (atan(0.15 / distance)) x
  NHOOK_BOT_HEADGATE (live, 1.0; trained 0.6..2.0) + 0.3 degrees. The trainer's window score = log((k + 1) / (d + 1)) + 0.5 x
  accuracy + the window's head hits a judged shot (from the walker's own aim block, 5+ shots) + 0.2 x headshot kills; HEADGATE is a
  trained knob, the aim HEIGHT is hone_aim's alone (two optimisers on one knob fought). First live numbers after the one rejoin: 27
  shots judged, 44% through a hitbox, 25% of those the head, median error 0.3 deg. The study's refit also waits while another model
  build runs (refresh-loop.sh builds the same models; two at once each took 10-20 min). NHOOK_NO_HEADLESS_AIM=1 = the old
  render-thread-only judging; NHOOK_BOT_HEADGATE=99 = no gate.
- **Weapon fetches that finish** (2026-11-09 07:25-07:30; nbot.c the pick-up scan and the step's FETCH branch) — WHY: "ALWAYS uses a
  garbage AR": the pick-up button ("pb") was set only with the virtual controller on, which the public walker has not, so no fetch had
  ever started; once they did, "gave up on BATTLE RIFLE (2.1 units): no closer in 6 s" -- the path ends at the walking graph's floor
  node and a weapon on a rack or ledge lies off it, while the press needed 0.9 units. LOGIC: the pick-up is the packet's act bit (pb 2
  whatever the controller); the press fires within NHOOK_BOT_PICK_R (1.3, live) units across and 1.5 up / down; the last 3 units of a
  FETCH are walked straight at the weapon (half speed inside 1 unit). Live: "at the weapon (1.3 units): holding act to take it" for a
  sniper rifle and a shotgun. NHOOK_BOT_PICK_R=0.9 = the old reach.
- **Training session: the trainer keeps what works and levels up the look; power weapons map-wide; the death map in the brain** (2026-11-09
  07:20; follow-study.py train / lvl_knobs, nbot.c the fighter's turn rate / the pick-up scan / nh_brain_step, nbrain_beh.c beh_fetch) —
  WHY: the user, away 7 hours: "I need walker to learn to hold his own ... the only thing that matters now is training in fluidly
  mimicking the best players in the games walker plays with ... the walker should do better every match. the ranking should steadily
  move up ... increase look sensitivity as you level up skills so walker can react like the best players too". Found on the way: no
  weapon fetch had EVER started in the public game (out of a fight nothing better lay within 12 units, and in a 24-player game it is
  nearly always in a fight -- the scan skipped fights and a fetch was cleared by one), and the reloadable brain had lost the death map
  (the body's copy; nav_danger_update had nothing to avoid). LOGIC: TRAINER -- an incumbent knob set (DIST, FJUMP, LIBSCALE, PRESS,
  REACT, RADSCALE, NOISE, GRENX) with its window scores (log((k + 1) / (d + 1)), windows the walker fought in); every 2 of its windows a
  challenger = halfway toward the role model's numbers + one knob nudged x e^N(0, 0.2), bounded; 2 windows later it is kept (level + 1)
  if its mean beats the incumbent's last 4 by 0.05, else the incumbent returns; battles/follow/trainer.json. LEVELS raise the look:
  NHOOK_BOT_TURN (the turn rate once reacting, a new live knob, 7 rad/s) +10% a level up to 14, NHOOK_BOT_TURN0 (before reacting, 0.73)
  +10% up to 2, NOISE x (1 - 0.04 a level, down to half). FETCH -- the scan runs in fights too; power weapons (rank <= 6) from
  NHOOK_BOT_FETCH_FAR (30) units, others 12, a better rank counting nearer; the bid = NHOOK_U_FETCH 45 + NHOOK_U_FETCH_GAIN 6 a rank
  better - 1 a unit beyond 10 (an AR -> a battle rifle close by ~81, over the fight's 60). DEATH MAP -- the brain loads the map being
  played's (hm_switch) itself: "death map for guardian: 544 deaths". All hot-loaded (brains #4, #5) into the walker in the game.
- **Brain stage 2: the behaviours in their own file** (2026-11-09 06:55; shim/native/nbrain_beh.c, nbot.c bot_step_body) — the same
  rewrite: board / follow / ball / fetch / fight / hold / hunt moved out of the 470-line step into functions that see one World
  (time, objects, self, camera, the fighter's view: engaged, fighter on, target distance) and only BID (side effects they own stay with
  them: the honk answer, the loose ball's act tap). The step builds the World, asks each, the arbiter chooses, the step walks the
  winner. No behaviour change (the same scores and rules); hot-loaded into the running walker.
- **Brain stage 1: the locomotion arbiter** (2026-11-09 06:45; nbot.c arb_* and the step's locomotion section) — WHY: the user: "I feel
  like native/nbot_* should be its own kernel or something. it looks like a pile of if then statements now" -> "yes" to the rewrite.
  Six blocks wrote the walker's path one after another (boarding, the wingman, the ball, the weapon fetch, the fight's moves, levels
  apart) and the last writer won -- the puppy (following beat fighting) and the AR forever (following overwrote the fetch). LOGIC:
  each behaviour BIDS for the legs (a score; a point for a path) and the highest bid moves the walker: board the leader's vehicle 95,
  follow beyond the leash 90, run with the ball 80, fetch the loose ball 75, hold the high ground 65, fight 60, hunt the ball carrier
  50, fetch a better weapon 45, escort the ball carrier 30, close up on the leader 20, hunt (the goal draw) 10 -- each a live knob
  (NHOOK_U_*). A path winner -> the pathfinder to its point (replanned on a new winner, a 1.5-unit move of the point, or every second);
  fight / hold -> their moves after the path; hunt -> the goal draw. "[arb] <winner> <score> > <others>" on a change. Aim and buttons
  keep their code (later stages: perception, aim / trigger channels). Hot-loaded into the public walker (brain #2): same process,
  still in the game, "[arb] fight 60 > hunt 10", "[arb] hold the high ground 65 > hunt 10".
- **The stats relay streams the live feed** (2026-11-09 06:35; shim/tools/stats-proxy.py) — WHY: the user: "why is it 'connecting...' and
  'not in match' I can't see anything from the walker stats page fluidly". The page's live data is Server-Sent Events (EventSource
  '/events'); the relay read each answer whole before passing it on, and a stream never ends. LOGIC: /events is passed on line by line
  as it comes (flushed); everything else as before. Checked in a browser through 8765: the match time advanced 2m 47s -> 2m 51s in 3 s.
- **The walker's brain reloads in a running game: code changes without a restart or a rejoin** (2026-11-09 06:20; nbot.c NhIO /
  brain_tick / nh_brain_step, nhook.c / nstats.c NH_BRAIN guards, build.sh nbrain.dylib, shim/tools/stats-proxy.py) — WHY: the user:
  "why did the walker quit? it should have just restarted itself. stop quitting games. make the engine able to restart or reimport the
  code change"; "I want those changes to be loaded into the running bot without a restart or rejoin". LOGIC: build.sh also builds the
  same C code with -DNH_BRAIN as shim/build/nbrain.dylib (-undefined dynamic_lookup): no start-up (the constructor), no interposing
  (INTERPOSE empty), and what only the body knows comes from the body's exports -- nhx_tls_block (the engine context its hooks
  pinned), nhx_hb_current_map / nhx_hb_tag_name (the maps it saw opened), nhx_bot_hook_install; the ObjC / stub symbols bind to the
  body's. The body's step thread looks at nbrain.dylib (beside nhook.dylib, or NHOOK_BRAIN) every 2 s; a changed one is copied to a
  fresh file in TMPDIR (dlopen caches by path), dlopen'ed, its layout (nh_brain_version, NHIO_VERSION 1) checked, and from the next step
  nh_brain_step(NhIO*) is the walker: the body packs its state (own / leader / human units, server, nickname, map, mode, radar traits,
  match start, test controls, line-of-sight answers ...), the brain applies it to its own copy, steps, and returns what the hooks write
  (aim, throttle, buttons, the act / melee / switch windows, the seat and exit times, line-of-sight requests -- the request counter last)
  for the body to apply. A brain starts its own memory (path, goal, models, live knobs) afresh; old brains stay loaded (their threads).
  NHOOK_NO_BRAIN=1 = the built-in walker. Each brain logs "[brain] code built <date time> is stepping" once. VERIFIED on our local
  server: a test walker on the new body loaded brain #1 and walked through it; the brain was rebuilt under it; "#2 loaded", the new
  build's line, 47 decisions from it within seconds -- same process (pid unchanged), still in the server's player list. ALSO:
  stats-proxy.py serves the walker's stats page (8771) on the user's usual 8765 while his game is closed (this Mac only; it steps
  aside when a game of his starts).
- **A leash, not a puppy; the aim point on the head; weapons fetched within the leash** (2026-11-09 06:00; nbot.c WINGMAN / the sparring
  aim, follow-study.py hone_aim) — WHY: the user: "now the walker is super needy following me like a puppy but not engaging in anything
  aggressively. I feel like it sucks at hitting the head hitbox. and ALWAYS uses a garbage AR instead of finding something better". The
  user's client had measured the walker's shots: 120 aimed, median error 0.11 deg, 47.5% through a hitbox, only 28% of those the head
  -- it aimed at 0.55 x the bounding radius over the centre, the neck. Following at all costs beyond 5 units held its fight's moves
  back nearly always, and the wingman's goal (the leader) overwrote the fighter's weapon fetch every step. LOGIC: live knobs
  NHOOK_BOT_LEASH (15): within it the walker fights freely, beyond it the leader's path beats the fight; NHOOK_BOT_WING_START (8) /
  _WING_STOP (4): out of a fight it closes up beyond / until; a better weapon within the leash (bot_wpn_rank, the old 12-unit fetch)
  is fetched before following. NHOOK_BOT_AIM_HEAD (0.55) = the aim point's height x the radius; follow-study.py hones it over 0.55 /
  0.65 / 0.75 / 0.85 by head hits a shot -- the change each window in the watching client's aim analysis of the walker (n, on, head)
  -- 20+ shots each first, then the best (1 in 4 random), saved in battles/follow/aim-state.json.
- **Grenade practice: twice the throws, the arc honed after the lead, the honing kept across restarts** (2026-11-09 05:50; follow-study.py
  hone, battles/live-knobs, battles/follow/nade-state.json) — WHY: the user: "I really want the walker to practice grenade lays here too".
  LOGIC: NHOOK_BOT_GRENX 0.42 -> 0.2 in the live knobs (x the players' time between throws: about twice the throws). Honing in two
  stages: the lead per unit (NHOOK_BOT_NADE_TPU 0.06..0.14) until every value has 12+ throws, then the arc per unit (NHOOK_BOT_NADE_ARC
  0.015 / 0.025 / 0.035) with the best lead held; each window's throws credited to the setting that ran; the next = untried first,
  else the best hit rate (1 in 4 random). The state is saved after each window and read at the study's start (a restart of the study
  had dropped the counts: seeded with the night's 0.06 11/19, 0.08 16/34, 0.10 12/46).
- **The walker copies the best player's movement, step by step; its rank leads every report** (2026-11-09 05:40; follow-study.py
  role_model / copy_role_model) — WHY: the user: "the biggest thing to study is the mannerisms in movement and fluidity in covering lanes,
  and engaging, and not hesitating. keep crushing it and refining and studying and reloading the winning model. tell me the rank of the
  model vs the crowd every time you refresh it". LOGIC: per player over the match: the share of alive time under 0.3 u/s (standing),
  the sideways speed to its look while an enemy is within 20 units (strafing), and HESITATION = the seconds from an enemy within 40
  units coming within 15 deg of its look to its next shot (within 3 s). Each window the walker's live knobs move at most 25% toward
  the role model: NHOOK_BOT_REACT x (its hesitation / the walker's), NHOOK_BOT_LIBSCALE x (its strafing / the walker's), NHOOK_BOT_PRESS
  +0.1 when the walker stands more than it (-0.05 when less), from the walker's launch knobs (walker-coach.py show, --id). The report
  opens with "RANK vs the crowd: #r of N" and prints both movements side by side. First dry run: the match's best stood still 47% of
  its time and fired 0.8 s after an enemy came in front; the walker 35% and 0.7 s, #15 of 25.
- **Kills / deaths on the stats page are this match's** (2026-11-09 05:25; nstats.c the players JSON) — WHY: the user: "the kills for
  game in play are wrong ... they don't match what is on my screen". On a public Slayer server the statborg's kills (stat 2) ran ~2.2x
  the score (94 vs 44 for the top player) -- the same in two clients (the user's, joined a match earlier, and the walker's, joined this
  one), so it is the server's own count, not this match's. LOGIC: "kills" / "deaths" = the game results' kills (stat 9) / deaths (11)
  -- they start again with each match (rs_tick) -- whenever the results have anything for the slot; else the statborg's as before.
  The statborg's are kept as "kills_total" / "deaths_total". NHOOK_SB_KILLS=1 = the old (statborg) numbers. Takes effect at a game's
  next launch (the JSON is the running hook's).
- **The walker copies the match's best player, not its leader** (2026-11-09 05:15; shim/tools/follow-study.py role_model / copy_role_model)
  — WHY: the user: "the walker should study what everyone else does for lanes, weapon choices, and tactics, not me ... it should only aim
  to protect me, and get better in its ranking vs the humans. right now <the top player> is destroying everyone. study him. be like
  him". LOGIC: each window, over the whole current match (the newest recording of the walker's view): every player's kills / deaths,
  kill distances, weapons killed with and held, speed, jumps a minute while an enemy is within 20 units, the 6-unit cells it kills
  from; the walker's rank by (kills + 1) / (deaths + 1); the ROLE MODEL = the best by that with 5+ kills (names from the walker's stats
  page, printed only). Copied through the live knobs (no restart): NHOOK_BOT_DIST = its median kill distance / everyone's (0.5..2.5),
  NHOOK_BOT_FJUMP = its jumps a minute near enemies (up to 12). Its weapons and spots are reported (steering the walker toward them is
  code: next). The report drops the leader's own numbers (the walker learns from the best, not from its leader) and shows protection
  instead: distance to the leader and how many of its attackers the walker went after. First dry run: the role model 69 kills / 16
  deaths, fights at 16 units (everyone 17.8), ~10 jumps a minute near enemies, battle-rifle headshots and melee; the walker #24 of 27.
- **The wingman defends its leader, follows at all costs, and learns from the live game every 3 minutes** (2026-11-09 05:00; nbot_engage.c
  eg_choose, nbot.c WINGMAN / g_fol_go, follow-study.py --refit + the sample-time fix, walker-epochs.py the same fix) — WHY: the user:
  "he should be trying to find me at all costs to be my wingman and help ME every chance he can all paths should be plotted to me";
  "also auto target anyone shooting me"; "walker should be learning every dirty trick from the tome of data and every 3 minutes
  learning from the current gamers and hone and restart the model". LOGIC: LEADER'S ATTACKERS -- when the leader (NHOOK_BOT_FOLLOW)
  loses shield / health, an enemy aiming at it within 12 deg counts as attacking the walker (known, the attacker bonus, the quick
  switch) and gets NHOOK_BOT_LEADATT (+4) more ("[wing] <id> is shooting my leader: on it"); the leader's own target (it aims within
  6 deg, under 40 units) +NHOOK_BOT_LEADTGT (5); anyone within 8 units of it +NHOOK_BOT_LEADNEAR (3) -- all live knobs. AT ALL COSTS --
  beyond 5 units (was 12 in a fight) the leader is the path's goal and the fight's moves wait (it still aims and fires) until it is
  back beside it. LIVE LEARNING -- follow-study.py --refit: each window engage-extract.py cuts the walker's recordings of this game
  (every current player's fights) into tracks/engagements and engage-model.py refits the models + tome (taskpolicy -b, nice 20, one at
  a time); the walker re-reads the changed files itself. BUG FIXED: a recording's sample t counts from the RECORDER's start, its header
  start is the file's first sample -- follow-study.py and walker-epochs.py added t to the start and missed every window (shadowing
  "None", the Sandtrap epochs scored 0.00): the wall time is now start - first t + t.
- **A walker with a HOME of its own could not build a new map's walking graph** (2026-11-09 04:45; shim/native.sh) — WHY: the user: "AIwalker
  test is just standing around now... doing nothing". The public game moved to Last Resort (zanzibar), which had no walking graph;
  the walker's background builder (re-tools/h3nav.py, spawned with the walker's environment) died "No module named 'numpy'": the
  public walker runs with HOME = its own folder, and Python's user packages live under the user's real home. With no graph it could
  aim but not walk. (Built by hand with the real home; a restart then loaded it.) LOGIC: when $HOME is not the user's real home (read
  from the user database: ~$(id -un)), native.sh exports PYTHONUSERBASE=<real home>/Library/Python/<python3's version> unless set.
  Still open: once that graph appeared, the running walker's step thread spun at the map-setup line (nav_ensure / nav_danger_update /
  bm_pop, inlined) and never stepped again -- a restart loaded the same graph; a sample (sample <pid>) of the next freeze names the loop.
- **Live knobs and models (no reconnect), the walker shadows a named player, grenade honing, kills in the player panel** (2026-11-09
  04:30; nbot.c lk_* / bot_models_tick / nade_note / nade_check / the WINGMAN leader, nbot_engage.c eg_knob, nhost.c + nstats.c
  g_follow_unit, shim/tools/follow-study.py, walker-public.sh, stats_page.html) — WHY: the user, the walker struggling in a public FFA:
  "have it try to shadow me and be my near by wingman. and update every 3 minutes what it learns. it should always find me"; "fix the
  walker code so it can do a hot launch of its learning model and not lose connection"; "ensure the walker can reset its model
  without leaving again!"; "this is the first place to try to hone the predictive nades"; "if we lost the slot... patiently wait to
  rejoin"; "when you select a player... show their kills and deaths here too". LOGIC: LIVE KNOBS -- <battles>/live-knobs/<nickname>.json
  (NHOOK_KNOB_FILE) is re-read within a second of a change and wins over the launch environment for the coached knobs (REACT, TURN0,
  NOISE, FJUMP, GRENX, LIBSCALE, RADSCALE, SKILL, DIST, STRAFE, SWITCH, FIRE_EXTRA, RETREAT, GREN, BACKOFF, PRESS, every eg_knob:
  ATTBONUS, BALLBONUS ...) and the new grenade-lead knobs NHOOK_BOT_NADE_T0 / _TPU / _ARC / _ARCMAX (were 0.4 s + 0.1 s a unit, 0.025 rad
  a unit up to 0.4); a knob taken out returns to its launch value. MODELS -- walker-model.json, engage-model.json (+ tracks/maps/),
  melee.json re-read when their files change (checked every 10 s). Verified in the public game: "[knobs] live from ..." and "melee.json
  changed: re-read" with the walker still in it. (New CODE still needs a relaunch.) FOLLOW -- NHOOK_BOT_FOLLOW=<player name> (read at
  launch from the profile, never stored): that player is never an enemy (any game type), is the wingman's leader before anyone,
  followed beyond 5 units until within 3, and even mid-fight once 12+ units away. GRENADES -- each throw notes its target; 1.6 s later
  "[nade] ... hit / missed / KILLED / target gone" (shield + health drop > 0.3 = hit); follow-study.py credits each window's throws to
  the lead per unit then running and steps NHOOK_BOT_NADE_TPU over 0.06..0.14 (untried first, then the best hit rate, 1 in 4 random)
  through the live knobs file. follow-study.py: every 3 minutes from the walker's own recording -- how close it shadowed (median, share
  within 6 units, longest apart), the leader's and the walker's kills / deaths / accuracy / kill distances / weapons, lessons where
  they differ. walker-public.sh: the disclosed walker on one owner-approved server, relaunched patiently (180 s to get in, a minute
  out = rejoin, 30 s between tries). Stats page: the player panel shows Kills (+ assists) and K/D beside Deaths.
- **A walker on an owner-approved public server, under a name that discloses it** (2026-11-09 04:00; nbot.c bot_owner_ok / bot_allowed,
  camp_say_thread) — WHY: the user asked the host of a public server whether a walker could join; the host agreed for that server. The
  user asked for a non-developer identity renamed "AIWalkerTest" for disclosure. (Earlier the same night a walker on another owner's
  server had been refused while its owner had not agreed.) LOGIC: NHOOK_BOT_ALLOW_SERVER=<ip:port> unlocks the walker on exactly that
  joined address and only while the profile's nickname starts "AIWalker" ("[bot] owner-approved server ...: the walker plays / not
  this one"); a test profile never plays anything public (the app refuses public servers to one), so the walker runs as a REGULAR
  profile in a home of its own (ReclaimerMac/native/public-walker/home via HOME= and NATIVE_REAL=1: a copy of a test identity, never
  the user's -- every copy of the user's identity carries the developer flag), with ReclaimerMac linked in for its maps and models.
  The console chat lines (camping, "coming!") go only to OUR server now (they used our server's RCON wherever the walker was). The
  chat announcement asked for is not possible yet: the app's player-chat send is not found (--ui-script drives menus; --request-at
  answers only inside a match). No server address or host name is kept in the sources.
- **Five-minute variant epochs; the public server behind carrier-grade NAT** (2026-11-09 03:45; shim/tools/walker-epochs.py,
  walker-restart.sh WR_ENV, ded-public-config.py [master] off, native-dedicated.sh MCCF) — WHY: the user: "walkers should read the game
  corpus every 5 minutes and retrain and understand where their K/D, shots fired vs landed, melee kills, powerup kills, power weapon
  kills, and heat maps per level come into play in being better. the better walker variant will win out every 5 minutes in a weighted
  vote for the best model"; "upnp won't help I'm on starlink behind cgnat". LOGIC: walker-epochs.py keeps a population of 4 knob sets
  (A = the coached knobs, B..D log-normal mutations); every 300 s it scores the running one from the recordings -- log((k+1)/(d+1)) +
  0.8 accuracy + 0.3 (melee + power-weapon + power-up kills) + 0.02 ball seconds - 0.5 share of deaths in the map's hot cells (the
  top tenth of 3-unit cells by deaths over the whole corpus of that map, tracks/ + battles/tracks) -- as a running mean, then a vote:
  votes exp(mean / 0.5), an untried variant as the best + 0.5; every 3rd epoch the worst tried variant becomes a mutation of the best;
  a new pick restarts the walker with its knobs (walker-restart.sh WR_ENV, after the coached ones). battles/epochs/{population.json,
  epochs.jsonl}. NAT: with this machine's master on, the server registered only with it ("registered with the server list at
  127.0.0.1:49175") -- behind carrier-grade NAT nobody reaches that master and it kept the listing to itself; with [master] off the game
  socket registers with the project's public masters (seen: "registered with the server list at <public master> as <public addr>:<NAT
  port>"), which list it and introduce players through the NAT. UPnP / PCP get no answer there. BUG FIXED: DED_PUBLIC changed HOME before
  the launcher read the MCC folder, so the server's nhook found no halo3.dll and its stats page (8767) never came up -- the folder is
  read first now; the running match is recorded from the walker's own page (8766) meanwhile. DECLINED the same hour: the walker in the
  LARGEST public game on another owner's server, renamed and announcing itself -- that server's owner and players never agreed to an
  automated client that reads positions from memory (the platform's own bots need the owner's allow_bots and take no part in the
  game); our listed server (named for it, a welcome line naming the walker) and shadow mode stay the ways.
- **Oddball play, a public walker server, out of any driver's seat** (2026-11-09 03:20; nbot.c ob_scan / the ball moves / driver's-seat
  exit tries / gun_near_veh, nbot_engage.c carrier bonus, shim/tools/ded-public-config.py, native-dedicated.sh DED_CONFIG / DED_PUBLIC,
  walker-battle.sh one walker) — WHY: the user: "start a big team battle match with one walker, and me, and make it public, set the size
  to 127 ... sand trap, and fiesta oddball. the walkers will have to learn to run with the oddball and melee only and obtain the oddball
  from other players with whatever weapon given. vehicles enabled ... call the public game 'walker texas rangers only!', and set the
  description to experimental walker test"; "make sure the game settings are as such that they auto start too ... is walkers game even
  in the list? does it matter that I'm behind nat?". Seen first: a walker in a Wraith's DRIVER's seat could not get out (packet act
  taps and holds both failed; turret seats leave on act), and it got in because the act guard measured to the vehicle's centre (a
  Wraith is wide). LOGIC: ODDBALL -- every 0.3 s the ball (a weapon whose tag name has "ball" / "skull"; two balls: the nearest):
  loose -> the pathfinder to it, act tapped on it (not beside a vehicle); mine -> 12 units away from the nearest enemy (replanned every
  2 s), no shots, melee within 1.8 units, the fight's moves only in melee reach; an enemy's -> its carrier is always known (the game
  marks it) and gets +8 on the target score (NHOOK_BOT_BALLBONUS), the walker goes to it when not fighting; a teammate's -> stays
  within 5 units. A loose ball within 8 units beats the fight's moves. NHOOK_BOT_NOBALL=1 off. DRIVER'S SEAT EXIT: tries go round --
  the packet's act tap, its hold, then control-record bits (+0xC8) 2..31 bar the known ones, 0.5 s each; the bit after which the seat
  ends is logged and kept in battles/exit-bit.txt, tried first from then on. ACT GUARD: within 2 units of a vehicle's HULL (centre
  distance minus its radius). PUBLIC SERVER: ded-public-config.py writes <slug>.toml + a one-game playlist (here Fiesta Oddball =
  Oddball with rounds.teams 1 and random primary / secondary, map weapons / vehicles / power-ups / grenades at their defaults, on
  Sandtrap, max_players 127 -- the server's cap -- min_players 1 so it starts with the first player); host_name = the line under the
  name; listed (hidden false), UPnP on, bind "::"; stats reporting OFF (a walker's games must not count); RCON on 127.0.0.1 only; every
  joiner reads a welcome line naming the automated player. The app counts any RECLAIMER_PROFILE_DIR profile as a TEST profile and then
  leaves the masters and the router alone ("[net] test profile: master at 148.251.153.44 left alone", no UPnP: the first start was
  not listed), so DED_PUBLIC=1 runs the server without one, with a home of its own (test-dedicated/public-home, never the user's
  profile). The walkers stay test profiles and join on 127.0.0.1 (allowed: a private address). walker-battle.sh takes 1 walker.
- **No driving; the honk is the walkers' to detect; restarts that keep the teams** (2026-11-09 02:50; nbot.c gun_seated / honk_scan /
  WINGMAN, shim/tools/walker-restart.sh, hold-teams.py) — WHY: the user: "walkers should not attempt to drive vehicles until they study
  the player driving corpus from historic games" (a walker's boarding tap landed in a Wraith its teammate had just left: the driver's
  seat), "honk detector should run on the walkers not me" (02:30 put it in the player's input logger). LOGIC: seat 0 of a vehicle with
  no parent = the driver's: out at once (tap / hold in turn), whoever's vehicle it is -- a driving study first (the recordings have a
  vehicle column). The player's game writes nothing now; honk_scan (in every walker, NHOOK_HONK_SCAN=0 off) keeps a copy of a driving
  teammate's unit datum and vehicle datum (0x380 bytes each) and logs each byte that leaves its value and comes back within 1.5 s,
  skipping bytes that pulse 10+ times a minute -> "[honkscan] unit|vehicle +0xNNN pulsed for N ms (wall t)", to match against the
  player's own [input] bit-5 presses and name the honk field (then honk_age reads it; battles/honk.txt stays a manual trigger).
  WINGMAN follows beyond 6 units until within 4 (it flapped at 6) and logs at most every 10 s. TEAMS: our server's RCON `team` answers
  "This game has no teams." in these playlists even mid-match, so hold-teams.py cannot hold them; the auto-balance deals them anew at
  every coach restart (all walkers at once) -- the gunner / wingman is whichever walker is on the human's team. walker-restart.sh
  restarts walkers ONE at a time (each rejoins the smaller team, the one it left), each back in the server's list before the next.
- **The gunner stays on, any of three vehicles, honks, the loyal wingman** (2026-11-09 02:30; nbot.c gun_* / bot_on_abuild / inp_log /
  the step's seated test and WINGMAN block, nstats.c g_human_unit) — WHY: the user, Third on his turret: "third has got to stop jumping
  out of the turret, and increase look sensitivity for aiming while in a vehicle. he should treat the wraith the same way if I jump
  in, or mongoose. make the other walkers train on either wraith or mongoose or warthog too. cohesion should be the goal for the
  moment. always be a loyal wingman. and smoke the nearest, or easiest target you can"; "they have to stop falling out the turret
  though. and IF they do they MUST get back in, especially if the team mate honks". Seen: Third boarded, fired at 52 / 47 / 29 units,
  came off; its boarding pulse (0.4 s of each second) went on pressing act once seated (= get out); after a later burst it fought on
  foot while seated (the seat field alone missed the turret); Second sat in a Banshee and Fourth on a chaingun, 0.8 s act holds never
  got them out. LOGIC: SEATED = a seat index or a parent (logged at each change with the unit / record / player handles); while
  seated the act bit only leaves inside a deliberate exit window (bot_on_abuild: g_bot_seat_t / g_gun_exit_until); boarding is a
  0.15 s TAP with 1.2 s to see it seated; exits alternate a 0.25 s tap and a 1.2 s hold. Vehicles: Warthog, Wraith (their turret),
  Mongoose (no turret: its passenger seat, 0.8 behind it); the aim in a seat turns 12 rad/s (was 4); the target is the EASIEST: distance
  x 4 out of sight x (0.5 + 0.25 x (shield + health)). HONK: the player's game (NHOOK_INPUT_LOG) writes the wall time of each melee
  press while seated as a driver (+0x24E 0 with a parent) to battles/honk.txt (NHOOK_HONK_FILE); a walker going to a teammate's vehicle
  answers a new honk with "<name>: coming!" (RCON say) and drops weapon detours while boarding. WINGMAN: no fight in sight -> within 6
  units of its leader (the nearest human teammate -- nstats publishes the units of players whose names are no walker's -- else the
  teammate walker with the lowest unit handle), the pathfinder there, replanned every second (NHOOK_BOT_NOWING=1 off). Walkers do
  not DRIVE yet: they ride with whoever does.
- **The gunner tells the turret from the passenger seat** (2026-11-09 02:05; nbot.c gun_seated / gun_board) — WHY: the user, Third
  in his Warthog for the first time: "third needs to understand turret vs passenger seat. he's in the wrong spot". Its act pulse went
  off anywhere within 1.5 units of a point 1.3 behind the turret -- a circle that reaches the passenger door -- and it took a seat in
  the Warthog itself as "mine". LOGIC: the Warthog object holds the driver and the passenger; the gunner sits on the TURRET (the
  vehicle parented to it). In the passenger seat with the turret free it gets out (act 0.8 s) and boards again; after 4 such misses in
  a minute it stays a passenger. Boarding in stages along the Warthog's forward (object +0x5c): beside / in front of the turret ->
  round the side (1.8 out, 1 back), behind -> 1.6 straight back, lined up there (0.9+ back, under 0.8 off the line) -> in to 0.6 back
  (until it drifts under 0.15 back or 1.2 off); act only in that last stage within 0.7 of the point and behind the turret.
- **The gunner: a walker mans its teammate's Warthog turret, and gets out of any other seat** (2026-11-09 01:50; nbot.c gun_seated /
  gun_board / gun_near_veh / gun_find, shim/tools/hold-teams.py) — WHY: the user: "every time my guy is in a warthog, it must come find
  me and get into the turret gunner. I want it to train as my gunner as I drive it around after the other walkers"; then "third got
  into a banshee on its own... now it is stuck. have it get out, and ONLY get in *MY* warthog" and "one of the walkers tried to board
  me, but jumped into the passenger seat". The act bit (+0x2c bit 0) enters vehicles: a pick-up hold beside a Banshee seated Third in
  it, another put a walker in the user's passenger seat (FFA), and a seated walker went idle ("in a vehicle"). LOGIC: (1) seated
  anywhere but a TEAMMATE's Warthog or its turret -> hold act 0.8 s (again every 3 s) to get out; (2) the pick-up hold and the
  reload tap never press act within 3.5 units of a vehicle; (3) a teammate seated in a Warthog whose turret (the vehicle parented to
  it) nobody mans -> the pathfinder to a point 1.3 units behind the turret (the Warthog's forward, object +0x5c), straight in the last
  4 units facing the turret, act pulsed 0.4 s a second within 1.5 units -- from behind the gunner seat's entry is the nearest; its
  fight still aims and fires on the way but no longer steers; (4) on the turret (or in that Warthog's passenger seat): the nearest
  enemy (one out of sight counts 4x as far), line of sight from the camera ignoring the turret, the chest led by velocity x d/300, the
  view turned at most 4 rad/s, the trigger (bit 19) held within max(2.5 deg, the target's size) and 60 units; out again when the
  teammate leaves the Warthog. Teams: Halo 3 never seats an enemy in a gunner seat, so the match is Team Slayer (DED_TEAMS=1) and
  hold-teams.py keeps them (every 5 s, RCON `team`: Third red with the human, Second / Fourth / Fifth blue -- coach restarts let the
  auto-balance shuffle the walkers; the human's name is never printed). NHOOK_BOT_GUNNER=0 = the old (seated = idle, pick-ups anywhere).
- **Walkers say when they decide to camp** (2026-11-09 01:30; nbot.c bot_camp_tick / camp_say_thread) — WHY: the user: "can you have
  the walkers send a chat message when they *decide* to camp, and wait for a user. just send a chat message that says i'm camping...".
  The walkers had no camping decision of their own (nothing named camp in the code): their standing still was the levels-apart hold
  of the high ground (a choice) or being stuck / fighting at arm's length (not). LOGIC: camping = the throttle the walker CHOSE is
  under 0.1 for 2 s with no fight in sight and a path driving it (a stuck walker still pushes its throttle, so it never counts) ->
  "[camp] decided to camp (<why>)" in its log and, at most once every 30 s, "<nickname>: i'm camping..." in everyone's chat as a
  [Server] line: our server's RCON say, run as `<the app> --rcon 127.0.0.1:49190 <password> say <text>` on its own thread with a bare
  environment (no DYLD_*), the password from test-dedicated/rcon-password.txt. Real player chat from the walker needs the app's chat
  send path (not found yet). Checked: the same command in the same environment put "[SERVER] walkers: camping announcements on" in
  the user's game. NHOOK_BOT_CAMPCHAT=0 = off; NHOOK_CHAT_RCON=<host:port>, NHOOK_CHAT_RCON_PWFILE=<file>.
- **Melee is bit 5; walkers stop climbing onto the enemy at arm's length** (2026-11-09 01:15; nbot.c bot_meleebits, the fight move's
  ARM'S LENGTH block, the stuck test, g_bot_empty) — WHY: the user, Pit FFA: "if i stand still they'll try to walk over me and get stuck
  jumping not shooting or meleeing", "it's not actually meleeing though", "fourth and third are standing around ... all the logic locked
  up". Live: Second stood 0.36 units across and 0.4-1.1 above the user (on his head, bouncing), logging "melee at 0.46 units" every
  step with nothing landing, "firing" a shotgun with 0 rounds; the round's walker kills under a unit were all gunshots, the user's two
  melee kills the only "Melee" ones. LOGIC: (1) MELEE = control word bit 5 (0x20): the user's two beatdowns came 0.2 s after his bit 5
  presses (round clock 232.3 / 381.3 s vs his input log 211.0 / 360.0 s -- one 21.3 s offset for both), his other two bit-5 presses
  were missed swings; the walker had sent bit 0. It goes into the control record (+0xC8, where fire's bit 19 works for a joined
  walker) and the packed record (mask 0x4827). NHOOK_BOT_MELEEBITS=1 = the old bit 0. (2) ARM'S LENGTH: the beatdown / back / empty
  rush drove on at full throttle inside a unit; now within 1 unit across it does not close further (circles), and on top of the enemy
  (0.5+ above, under 0.8 across) it steps off. NHOOK_BOT_NOARM=1 = old. (3) The stuck test (back off, jump, replan, four times = drop
  the goal) no longer counts a walker fighting within 3 units -- standing at arm's length is no progress by design; it was what made
  them "stand around" after a point-blank fight. (4) EMPTY needs 2+ rounds seen in the weapon (was 8: a shotgun holds 6, so an empty
  shotgun never counted as empty and kept "firing").
- **Every player dot on the stats map is visible on any background** (2026-11-09 01:10; shim/src/stats_page.html drawMap) — WHY: the user,
  in a 4-player FFA on The Pit: "I can only see 2 walkers ... I see second, and fourth, but not third". Third was in the match and in
  the snapshot; in FFA every player has its own team colour, and its blue (#4070f0) 3.5 px dot vanished on the dark map picture and on
  the heatmap's blue-green blobs it stood on. LOGIC: each dot is 4.5 px (6 selected) with a 3.5 px black ring under it and a light edge
  (white 2 px = you, yellow = selected, translucent white 1 px = everyone else); the labels keep 6 px clear of a dot. No switch (drawing
  only).
- **The walker presses melee / act / switch in the packed record** (2026-11-09 01:00; nbot.c bot_meleebits / bot_actbit / bot_switchbit,
  bot_on_abuild; shadow-study.py, match-study.py) — WHY: a headless walker's buttons never reach the server (the virtual pad is not read
  for buttons in a headless / background client), so it could not melee, pick up or switch weapons ("claiming you can't change weapons
  on headless walkers is weird"); the user's own play, labelled in context by NHOOK_INPUT_LOG, named the bits. LOGIC: with more of the
  user's play (jump 27 times after +8 bit 1, a grenade throw 47 times after 0x21100800), +8 bit 0 is the one packed bit that came with
  play and left no trace in the unit: MELEE's best candidate; the extra byte (+0x2c) bit 0 came with 10 pick-ups (the act button: pick
  up / reload), its bit 1 with the weapon switches. The walker ORs them into the packed record before each control packet: +8 bit 0 while
  it melees (g_bot_melee_until), +0x2c bit 0 for 0.6 s with a pick-up hold and 0.1 s with a reload tap (g_bot_act_until), +0x2c bit 1
  for 0.15 s at a weapon switch (g_bot_switch_until). Switches: NHOOK_BOT_MELEEBITS (hex, default 1; 0 = off), NHOOK_BOT_ACTBIT /
  NHOOK_BOT_SWITCHBIT (hex, default 1 / 2; 0 = off). Unconfirmed until a walker lands a beatdown / picks up / changes its held weapon on
  the server's view (the next rounds' study). shadow-study.py counts the player's melee on +8 bit 0 (bit 11 is part of the throw);
  match-study.py names a public recording's players "player N" (it said "walker N"), --human you = the recording's own slot.
- **Shadow mode, the input map from a real player, melee bits, the memory watch** (2026-11-09 00:15; nbot.c shadow_log / inp_context /
  bot_meleebits, shim/tools/shadow-study.py, mem-watch.py, play-vs-walkers.sh) — WHY: the user asked for a walker in the biggest public
  game ("I need it to learn in a REAL scenario now"); declined -- an automated player with memory-read targeting against people who
  never agreed to face one -- and offered shadow mode instead (the user: "option 1 please"). Also: "I'm sending walking, jumping,
  change grenade, you know the entire dang controller map. the server should see it all in context", "over time the choppiness came.
  I wonder what's causing that memory leak". LOGIC: NHOOK_SHADOW=1 runs the walker's whole brain in the player's own game on any
  server and publishes nothing (the end of bot_step_body returns before g_bot_out is set; bot_write and the packet hook write only
  from it; the virtual pad is skipped) -- verified: the packet hook's one-time "builder's record" line never appears; every 0.1 s
  its decision (target + position, fire, melee, empty, throttle, aim) and the player's own (throttle, aim, the record's buttons) ->
  tracks/shadow/<time>_<map>.jsonl; the snapshot's "shadow" = its target live. shadow-study.py: same target (player's look within 10
  deg of it), fire agreement by distance, melee, movement direction, aim gap. NHOOK_INPUT_LOG now labels bits IN CONTEXT: each bit
  that switches on is checked 0.6 s later against the player's unit (found by handle in the object array): grenades fewer = throw,
  +0x298 changed = grenade switch, +0x262 = weapon switch, +0x268 / +0x26c = pick-up, risen 0.3+ = jump -> an [input-map] line per bit
  every 60 s. First two rounds of the user's play: 0x80040 (fire) 121 times, the extra byte bit 0 78, 0x21100800 (bit 11 -- a
  packet bit -- with 20 / 24 / 29) 33, bit 1 (jump) 19, bit 5 17, bit 0 7, extra bit 1 6. MELEE BITS: while a walker melees,
  0x21100800 goes into its control record and bit 11 into the packed record (NHOOK_BOT_MELEEBITS, =0 off) -- the best candidate,
  unconfirmed until the input map names it. CORRECTED 00:35: the input map, in context, put 15 grenade throws after 0x21100800 -- it is
  the GRENADE THROW, not melee -- so NHOOK_BOT_MELEEBITS is off by default (no walker restart had it yet); melee leaves no trace in the
  unit and hides among the "nothing seen" bits. Shadow fixes the same hour: the shadow walker judged its fire by the PLAYER's view (it
  never fired) -- it now turns a virtual view of its own; the player's buttons were read from the control record at random moments
  (always 0) -- now ORed from every packed record between samples (the packet hook runs in shadow mode for that). AMMO: the found "rounds loaded" field (+0x1b4) reads 0 for a fresh rifle in a player's
  game and 129 in the walkers' -- not a magazine: "empty" now needs 8+ rounds seen in that weapon before. mem-watch.py: every minute,
  each client's footprint (top: phys_footprint, no scanning), worst frame, kills / deaths -> battles/mem-watch.csv; --report fits MB
  per hour per client. A Magnum secondary alone in the playlist options gave nobody a Magnum: primary + secondary set together now
  (as the bloom battle's) -- and a full second slot would stop the walkers' only way to get power weapons (the game's own pick-up
  into an empty slot) until their pick-up button works.
- **Engage, lead the grenade, burst at range, follow the best walker, a fourth walker** (2026-11-08 22:45; nbot.c, nbot_engage.c,
  shim/tools/fire-rhythm.py, walker-scores.py, walker-coach.py follow, coach-rounds.sh v2, walker-battle.sh test-fifth) — WHY: the user:
  "walkers should never just walk past an enemy without predictive grenade lay some shots, or a beat down! engage!" (a walker went
  "click click click" right past him: an empty magazine), "it is shanking shots due to the random bloom on the AR, spreading out at
  range", "pay attention to which walker is doing the best too, and train the other walkers to be more like the good walker", "can we
  add one more headless walker". LOGIC: ENGAGE -- a visible enemy within 10 units is taken at once unless the current target is itself
  visible within 10 (NHOOK_BOT_NOENGAGE=1 off). PREDICTIVE GRENADE -- the throw is aimed where the target will be: its velocity x the
  flight (0.4 s + distance / 10), +0.025 rad a unit for the arc (NHOOK_BOT_NOLEAD=1 off). EMPTY MAGAZINE (the ammo field: ammo.json
  "loaded" is known now) with the enemy in sight -> a grenade at once (3-16 units) and the rush for the beatdown (NHOOK_BOT_NOEMPTY=1
  off). BURSTS -- fire-rhythm.py over the public recordings, aiming at an enemy with an automatic weapon: the top-quarter players fire
  9.0 shots/s within 8 units, 3.1 at 8-15, 2.0 at 15-25, 1.4 beyond; the walkers 5.2, 5.7, 1.6 (spraying at mid range, timid up
  close); beyond 8 units the walker fires 0.25 s bursts with pauses keeping the players' share of their close-range rate (34 / 22 /
  15%) (NHOOK_BOT_HOSE=1 = held). (The 10 Hz shots counter is too coarse for burst lengths: rates only.) FOLLOW THE BEST --
  walker-coach.py follow <best> <others> moves each other walker's knobs half way (log space) toward the best one's; coach-rounds.sh
  picks the best from the finished round's final scores ((kills + 1) / (deaths + 1); walker-scores.py --recording). COACH-ROUNDS v2: a
  round ends when the battle recorder starts a new file (the in-match flag stayed on through a short lobby and the first version
  missed a round); each walker is coached on THAT recording alone (a folder with just it: the battle folder's older battles had other
  slots); slots -> walkers from the server's players list written every poll; COACH_LAST=<recording>. First pass (the user's 44 / 19
  round): Third 32 / 34 the best walker; the others moved toward it. FOURTH WALKER -- test-fifth ("Fifth", stats 8770): ~0.35 core and
  ~2.2 GB a headless walker; a new identity needs the small --background window once (headless never makes one: "no identity yet").
  ALSO: NHOOK_INPUT_LOG=1 logs a REAL player's packed buttons (+8, +0x2c) and the unit's weapon / grenade bytes when they change
  (the user meleeing / reloading / switching names the bits the walkers' dead buttons should set); do=botwslot (writes the unit's
  +0x260 before each packet: the weapon-switch experiment -- NEGATIVE, 22:44: with two weapons (Mauler or Brute Shot + the assault
  rifle), +0x260 = 1 or 0 for 2 s never changed the server's view of the held weapon, 4 tries; the unit then read +0x260 = 2, +0x262
  = 1 (the current slot), +0x268 / +0x26c the two weapons: the switch is not that byte -- NHOOK_INPUT_LOG on the user's Y press next).
- **Walker battles run headless** (2026-11-08 22:15; walker-battle.sh) — WHY: the user: "running all 3 clients visible lagged the
  screen hard, can you drive the client without the display?". LOGIC: the walkers' games start with the app's own --headless ("no
  window, the null renderer"); the walker needs no frames (aim, line of sight and moves come from the game's state). Checked: two
  headless walkers moved ~67 units in 2 minutes and fought (71 shots, a kill). WB_VISIBLE=1 = the small --background windows.
- **Every bullet was logged twice** (2026-11-08 19:15; shim/tools/bloom.py load) — WHY: the bloom battle's log had each bullet twice
  ~1 ms apart (same shooter, start, end and length; the aim sampled again): the app deletes most projectiles twice and the
  object_delete hook logs both. Consecutive bullets then looked perfectly correlated. LOGIC: the first report of a (shooter, start,
  end) within 0.1 s is kept -- and at the source (noverlay.c on_object_delete_body): a projectile handle among the last 256 projectile
  deletes is skipped, so the shots counters, the aim meter (accuracy), tracers and the bullet log count each bullet once (they all counted it
  twice; accuracy, hits over shots, was roughly unchanged). NHOOK_DEL_DUPS=1 = the old double count.
- **Finding the full-screen stutter: stall meters, the A/B runner, NHOOK_NO_TAP** (2026-11-08 19:20; nhcost.h SM_*, nrender.m,
  nhook.c tap / my_AudioUnitRender, nmic.c vmic_clock, nnet.c pk_wire, noverlay.c on_object_delete, nstats.c [stall] line;
  shim/tools/hitch-ab.sh, hitch-summary.py) — WHY: the user: the full-screen game "choppy as heck ... that we NEVER EVER EVER had";
  their 2-hour game through nhook (16:34-18:51) had a frame over 100 ms in 73% of the app's 5-second frame windows (worst-frame
  median 152 ms), growing with time: 18 ms at the start, 36 / 62 / 109 / 175 ms at the start of later matches, more within each
  match as players came and went. The cost meter sums each hook's time, so one 150 ms call a second hides as ~1.5 ms a second.
  LOGIC: every place nhook runs inside an app thread (present: nhook's part and the app's own present call apart; the depth
  attachment setter; the log tap: the wait for its lock and the time holding it; sockets; AudioUnitRender; the virtual mic's call
  into the app's input callback; object_delete) and the snapshot keep their LONGEST single call per 10 s: the [stall] line beside
  [perf+], with the depth-texture table's size and the depth passes. NHOOK_NO_TAP=1: the app's log lines are read only until the
  engine's image line (no match state / chat / voice report / peers from the log after it). hitch-ab.sh <server> <secs> <arm>...:
  the app in normal mode (the user's settings: full screen) on one public server per arm -- none (the app alone) / nhook /
  nhook:ENV=..; waits for a free slot; HITCH_SAMPLE=1 samples the app for 3 s once the stutter shows. hitch-summary.py: per log the
  app's own "[capture] frame ... max" windows (printed with or without nhook). First runs (The Pit, 6 min each): the app alone
  worst-frame median 22.0 ms (2% of windows over 100 ms), with nhook 21.4 ms (0%) -- the same early in a session: it builds up.
  Long runs (hitch-summary.py --curve, worst-frame median per 2 minutes): nhook 22 27 25 24 28 25 30 33 40, nhook with NHOOK_VMIC=0
  14 17 21 27, the app ALONE 14 14 14 16 21 28 32 39 -- the build-up is the 0.10.1 app's own (backlog N20, first seen in the user's
  first 0.10.1 session, before the virtual mic existed). Every stall meter stayed under 0.1 ms; the snapshot switched off and on in
  10 s phases mid-run: worst frame 33.0 / 31.5 ms (no difference). The ~30 ms frame every ~10 s (the 5 s windows alternate ~14 / ~30
  ms) is the app's too: the app alone shows it on Valhalla. Not a fix -- a finding; nhook keeps the meters ([stall], every 10 s).

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

- **Engine-thread levers for 120-player games, opt-in until measured in a match** (2026-10-25; user: "can we do anything about
  pulling 11 fps or less on Big team games like HugeGrass?", "don't compromise quality", "fix as many of those [boundary
  crossings] as possible") — Measured first (123 players, 315 vehicles, M4 Max): halo3's engine thread 98% busy, everything
  else idle-ish; the simulation keeps real time, so frames get what is left; `sample` of that thread: halo3 65%, the client
  8%, game-state copies 8%, Rosetta runtime routines ~9% (lookups at call returns: halo3's own polymorphic calls), the
  client's file opens ~2%, QueryPerformanceCounter ~1%, XAudio2 lock waits ~1%. shim/tools/xcallbench.c: a call from
  JIT-translated code costs the same into loader text as into generated code (1.2 ns: crossing into the loader is free);
  the ms_abi -> SysV switch adds ~3 ns (fastcall.c already covers the hot imports); rdtsc under Rosetta = 1 tick per ns,
  6.5 ns, a constant offset from mach_absolute_time (10.9 ns). Added, each OFF by default:
  `PELOAD_FASTCALL=...,clock` (fastcall.c fc_QueryPerformanceCounter: rdtsc minus the start-up offset, a leaf; checked rate,
  else off; not in "all" yet), `PELOAD_XA_MATSKIP=1` (xaudio.c xv_SetOutputMatrix: an unchanged matrix returns before the
  mixer's lock), `PELOAD_GUEST_QOS=1` / `PELOAD_AB=qos:<secs>` (peload.c guest_qos_tick from hb_frame_hook: the engine
  thread at QOS_CLASS_USER_INTERACTIVE like the loader's own threads; it ran at the default class), `PELOAD_MAINFS_LOG=1`
  (files opened on the engine thread, counted per path every 10 s: what to cache). Next: run them in a big match (A/B
  where there is a switch: qos, cow, fastcall) before any becomes a default. shim/tools/callcost.sh does not build any more
  (the heap block it cuts out of peload.c now needs the heap tracker): xcallbench.c measures the call costs on its own.

- **The client's metadata queries on the engine thread: 23 -> 1.1 us each** (2026-10-25; found with PELOAD_MAINFS_LOG) —
  peload.c `i_CreateFileW` / `i_CreateFileW_body` (split so the log can time it), `metaopen_fd` / `metaopen_keep`, hitbox.c
  `hb_note_map`. Why: the client calls std::fs::metadata on halo3/maps/005_intro.map 3,100-9,500 times a second on halo3's
  engine thread (exe+0x1fe7916; CreateFileW access 0, OPEN_EXISTING, BACKUP_SEMANTICS), and each cost ~23 us: open()+close()
  of that path ~8.5 us under Rosetta, and hb_note_map re-opened the file to check its header and shifted its 32 x 1 KB path
  list on every call -- 7-22% of the engine thread. How: (1) hb_note_map only for opens that can read data (an access-0 query
  can't load a map; it also kept a campaign map at the top of "maps the game opened"), and returns at once for the newest
  path (PELOAD_NOTEMAP_ALL=1 = every open, as before); (2) metadata opens (OPEN_EXISTING, no data access, not
  delete-on-close) of a path asked for within the last second get a dup() of one descriptor kept per path (16 paths): a real
  handle on the same file, fstat / GetFileInformationByHandle stay live; the kept descriptor is reopened after 1 s
  (PELOAD_FS_METACACHE=0 = always open). Measured in a local match: 23.4 -> 12.5 (1) -> 1.1 us (2) per query, path
  translation included; the game unchanged (spawns, items, match time = wall time). Both ON by default.
- **HugeGrass +47% more: particle compute jobs from a ready list instead of a picker under one lock** (2026-10-25) — peload.c
  `csq_worker`, `csq_adddep` / `csq_ready_push` / `csq_ready_pop`, `ctxi_Dispatch`, `ab_csq`. Why: with the readback stall
  gone, `sample` of a HugeGrass client (shim/tools/sample-threads.py) showed the GL thread ~25% waiting for g_csq_mx in
  Dispatch and ~50% for compute results, and the 8 compute workers ~15-20% computing, ~35-40% waiting for that mutex: every
  finished job broadcast to all idle workers, and each ran csq_pick under the lock -- every queued job checked against every
  earlier one (O(n^2), ~100 particle dispatches a frame). How: a job's dependencies (the same conflict rule, against every
  unfinished earlier job) are counted once when it is queued; a finished job counts its dependents down and moves those at
  zero to a FIFO ready list; a worker takes the head; one wake-up per job made ready. Workers re-read the mode after every
  wake-up (PELOAD_AB=csq switches it between batches -- the first build let workers sleep on the wrong list after a switch:
  no compute ran and the client never left its loading screen). Measured: HugeGrass in-match A/B (PELOAD_AB=csq:20, 93-95
  players, ~240 vehicles; the readback fix on in both): 52.9 vs 35.9 fps over 8 / 9 phases, 7 of 8 pairs ahead.
  PELOAD_CS_OLDQUEUE=1 = csq_pick and broadcasts, as before. Tool: shim/tools/sample-threads.py (per-thread waits from a
  macOS `sample` file).
- **HugeGrass +42%: the client's per-frame luminance readback no longer stalls the render pipeline** (2026-10-25; user: "can
  we do anything about pulling 11 fps or less on Big team games like HugeGrass?") — d3dsubmit.c `ctx_Map` (a blocking READ
  map waits for the queued writes into that resource only), peload.c `rb_async` / `rb_complete` (called from
  `d3dgl_gpu_copy_impl`, `ctxi_Map`, the CPU-side copies), glh_client.c `glh_rb_request_at` / `glh_rb_ready_at`. Why:
  `sample` of a 120-player HugeGrass client: halo3's render thread 76% waiting in sub_drain <- ctx_Map, the GL submission
  thread 34% asleep in a synchronous glReadPixels. Every frame the client (exe+0xbbbc6a) copies the scene's 1x1 RGBA16F
  average-luminance texture into one of a ring of staging textures and Map(READ)s one copied frames earlier -- built not to
  wait -- but the read map drained the whole queue and the copy read the pixels back synchronously (the GL thread waited for
  the helper to run everything and the GPU to draw it): CPU and GPU took turns once a frame. How: the read map waits only
  until that resource's queued writes ran (normally long ago), and with the GL helper the copy reads into a pixel buffer and
  requests a mailbox readback (a ticket, 64 staging textures of <= 16 KB at 4 MB + 16 KB x n) -- the CPU copy is filled when
  it is read. Compute-owned buffers still drain. Measured: HugeGrass in-match A/B (PELOAD_AB=rb:20, 75 players, 263
  vehicles): 31.6 vs 22.2 fps over five comparable pairs (+42%); a 7,000-8,000-draw view 16.7 vs 16.0 (GL-bound there);
  local match: no drains left, luminance values change as before (auto exposure intact). PELOAD_D3DGL_SYNCRB=1 = synchronous
  readback, PELOAD_D3DGL_SUBMIT_DRAINREAD=1 = full drain (old behaviour: both).
- **Copy-on-write game-state copies were declined after the first ~10** (2026-10-25; found in a 118-player HugeGrass match:
  plain memmove of halo3+0x13f23f's 16.4 MB game state, 7 GB/s, 14% of the engine thread) — peload.c `cow_private_range`,
  `crt_big_memmove` (PELOAD_MEMCPY_LOG now says why a big copy isn't copy-on-write). Why: the safety check declines
  shared memory (a copy-on-write of a shared file view would cut the sharing with the client's worker) by the kernel's
  share mode, and anonymous memory that a copy-on-write copy has touched reports SM_TRUESHARED: from then on every copy
  was "not private" and done by memmove -- the default-on copy-on-write (2026-10-18, +4% BTB A/B, which saw it work through
  the 8-entry result cache) had not been happening. How: only FILE-backed shared mappings (external pager: MapViewOfFile
  views) are declined; PELOAD_COW_STRICT=1 = any sharing declines, as before. Local match: 0 -> ~4,900 copies per 5 s done
  copy-on-write. HugeGrass in-match A/B (PELOAD_AB=cow:20, 118 players): 14.8 vs 15.0 fps, neutral -- the copies' cost
  moves into page faults when the game rewrites the state.
- **Test runs are silent: PELOAD_WASAPI_MUTE** (2026-10-25; user: "can you mute the game audio while you test?") — wasapi.c
  `wa_aq_cb`, shim/tools/quiet-run.sh, shim/dedicated.sh. Why: test clients muted the game's XAudio2 sound
  (PELOAD_AUDIO=0, or PELOAD_AUDIO_MUTE=1 when a test needs the mixer), but the client's own WASAPI sound -- menu music,
  other players' voice chat on a public server -- still played on the Mac. How: PELOAD_WASAPI_MUTE=1 plays every WASAPI
  stream as silence (consumed as usual; a clip recording still gets it); quiet-run.sh always sets it and PELOAD_AUDIO_MUTE=1,
  dedicated.sh sets it for its workers. Unset (play.sh) = sound as before.
- **PELOAD_RB_CHECK=1: the asynchronous luminance readback checked byte for byte** (2026-10-25; hunting the HugeGrass "washed
  out" report) — peload.c `rb_async` / `rb_complete` (D3DObj `rbchk`). Each asynchronous readback is also read synchronously at
  the copy and the two are compared when the CPU copy is filled (`[rbcheck]` lines). HugeGrass, 100 s: 6,000 matched, 0 differed
  (log luminance -3.6 .. -12.9): the client's auto exposure gets what it got before the change.
- **PELOAD_AB=new** (2026-10-25; user: "benchmarks must be vs something meaningful: join the largest game you can, then
  back down to smaller ones") — peload.c `ab_new`, d3dsubmit.c sub_switch feature 13: flips every engine-thread change of the
  day together (metadata cache, data-opens-only map notes, the QPC leaf, the matrix skip, the engine thread's QoS) so one
  public match measures before vs after phase by phase.

- **Frame latency measured: PELOAD_LAG_LOG=1** (2026-10-25; user: "the graphic UI feels sluggish and floaty") — d3dsubmit.c
  `sub_present` / SQ_PRESENT (`g_lag_req` -> `g_lag_t`), glh_client.c `glh_pq_publish`, peload.c `pq_present_wait_glh`. Every
  5 s: `[lag] present -> GPU done` (halo3's render thread presenting a frame -> the helper's fence for that frame's blit; hidden
  windows too) and `[lag] present -> window` (-> the present thread showing it; frames in flight render->GL and GL->GPU). The
  timestamps are keyed by d3dgl_present's frame number: the first version keyed them by the submission thread's own present
  count, which only starts when that thread switches on, and paired frames ~110 apart (it read "2.8 s"). HugeGrass, 17 players,
  hidden: 70-95 ms at 26-40 fps -- about 2.5-3 frames queued between the game and the GPU.
- **HugeGrass +45%: the GL helper no longer waits for the GPU at every readback** (2026-10-25; found with `sample` of the
  helper during a latency run: 88.6% of its main thread in iokit_user_client_trap <- GLDBufferRec::restoreData <-
  gleSynchronizeBufferOnCurrentDevices <- glMapBufferRange) — glhelper.c `rb_poll` / `rb_finish` (GLH_X_READBACK), glh_shm.h
  `rb_sync` (GLH_VERSION 7), glh_client.c `ab_rbdefer` (d3dsubmit.c A/B feature 19). Why: the helper mapped a readback's pixel
  buffer as soon as the command arrived -- right after the ReadPixels into it -- and the map waits for the GPU to finish
  everything queued before it. The overlay's depth readback asked two frames late, so it never mattered; the client's
  per-frame luminance readback (this morning's rb_async) asks at once, so every frame the helper idled until the GPU caught
  up (its "busy 101%" was mostly that wait). How: a fence after the ReadPixels; the buffer is mapped once the fence has
  signalled (polled beside the frame and query fences, in ticket order). In-match A/B (PELOAD_AB=rbdefer:20, HugeGrass, 12
  phases): 32.9-35.6 vs 23.1-24.3 fps, input -> screen 108 vs 146 ms (GPU done 82 vs 118). PELOAD_GLH_RB_SYNC=1 = map at once.
- **The window showed at most ~27 new frames a second with the GL helper (the default since 2026-10-17)** (2026-10-25; user: "the
  graphic UI feels sluggish and floaty"; found with the new window-fps column of the per-second log and PELOAD_PRESENT_PROF=1)
  — peload.c `pq_started` (used by present_thread_fn), `ab_presleep` (d3dsubmit.c A/B feature 18). Why: the present thread
  sleeps 33 ms per loop until frames flow through the present queue ("frame-driven once the queue runs"), and it looked only at
  the in-process queue's counter g_pq_seq -- which never moves in helper mode, where frames come through the helper's
  pq_seq. So after every frame it slept 33 ms: menu engine 320 fps, window 26 new frames/s; local Valhalla 194 fps, window 27;
  the title showed the engine's rate, not what the player saw. How: pq_started() also reads the helper's counter.
  Measured with the in-match A/B (PELOAD_AB=presleep:20) and the latency probe: Valhalla 194 fps -- window 27 -> 90-110 new
  frames a second (every refresh of the 60 Hz display gets a new frame), input -> screen 66 -> 60 ms; HugeGrass (6,200 draws)
  -- engine 18.4-22.2 -> 24.2-26.0 fps in comparable phases (+25-35%: the helper's frames were consumed late), input -> screen
  147 -> 131 ms, GPU done -> window +21 -> +1 ms. PELOAD_PRESENT_SLEEP=1 = the old sleep. PELOAD_PRESENT_PROF=1 prints
  `[presentprof]` every 5 s: new frames / presents per second, time waiting for a frame / drawing / flushing. The same bug
  was found and fixed independently in PR #2 (willfell/present-sleep-glh, merged into main 2026-11-06: `g_glh_on` counted as
  the queue running); on merging main the present thread kept `pq_started()` (it waits for the helper's first frame and has
  the switch). That PR's measurement (M4 Pro, 5120x1440 at 120 Hz, client v0.9.23, PELOAD_MAX_FPS=60, Guardian): before,
  engine 60.0 / shown 27.0 fps, every gap between shown frames 25-42 ms, `sample` had shim-present in that nanosleep 86 of 90
  times; after, engine 60.0 / shown 59.6 fps, 93% of gaps 12-21 ms. Menu after: engine 58-60, shown 58.7-60.1.
- **Match loads 22% faster and the first minutes after joining stop stuttering: halo3's file hashing runs on its own thread**
  (2026-10-25; found with shim/tools/hitch-sample.sh -- `sample` taken during a 0-fps second after joining a ~100-player
  HugeGrass game: halo3's engine thread ~24% in CC_SHA1_Update) — peload.c `bha_queue` / `bha_worker` (thread shim-hash) /
  `bha_drain` (BCryptFinishHash and BCryptDestroyHash wait for it), BHash `pend`; PELOAD_BCRYPT_LOG=1 `[bcrypt]` (bytes per 5 s,
  time in the calls, top callers), PELOAD_BCRYPT_CHECK=1 (each digest also computed synchronously and compared). Why: halo3's
  streaming file reader (halo3+0x205ebc) feeds every chunk it reads into a running SHA-1 while its flag (+0xb2) is set -- the
  whole of a file it streams, the HugeGrass mod's 2.1 GB map included: 400-1000 MB every 5 s during loading and the first
  minutes of a match, 0.25-0.95 s of the calling thread's time per 5 s at Rosetta's ~770 MB/s (natively 3.4 GB/s, but Rosetta
  exposes no x86 SHA instructions: SSE4.2, AES and PCLMULQDQ only). How: a chunk >= 4 KB is copied into a queue that the worker
  hashes in order and the call returns (at most 64 MB queued). Check: 9,800 digests matched, 0 differed. Measured on the
  127-slot HugeGrass server: time in the hash calls 250-945 -> 27-42 ms per 5 s, match loaded in 62.3 -> 48.8 s, seconds with
  a frame over 100 ms in the first / second minute 24 / 30 -> 1 / 0. PELOAD_BCRYPT_SYNC=1 = hash in the call, as before.
- **Late camera update: the newest mouse movement applied to each frame's camera at its upload** (2026-10-25; user: "is there
  anything novel we can do to improve hugegrass? 239ms of input lag is huge") — new shim/src/latch.c (`latch_src` called by
  d3dgl_bind_ubo / d3dgl_ubo_batch, `latch_overlay_vp` at the overlay's camera capture, `latch_received` in wi_raw_mouse; D3DObj
  `latch_f`; d3dsubmit.c A/B feature 22 `latch`); play.sh sets PELOAD_LATCH=1. Why: in big games 40-110 ms pass between halo3
  building a frame and its constant buffers going to the GPU; the turn the player made meanwhile shows a frame or more later.
  How: the idea of VR late latching / Reflex 2's Frame Warp on the camera matrix (so HUD and first-person weapon stay): a
  learner fits degrees per mouse count and how old a frame's input is at upload (HugeGrass: -0.0217 deg/count, 50-60 ms,
  R^2 0.82-0.87), a predictive angle follows the mouse with an offset pulled toward the engine's camera, and every constant
  buffer holding the world camera (hb_is_world_camera at its bound offset) is rotated about the camera position in its GPU
  copy only; none in a vehicle or without a live local player; clamped to 12 degrees. Two first designs failed and are
  described in the file (snapshots of the counts the game read: R^2 0.25-0.6; a fixed-window correction: the view reversed
  direction 11.7 times a second); a third bug -- the overlay's capture took the frame's camera before the patched upload, so
  the probe never saw the patch -- was found by an A/B that showed nothing. In-match A/B (PELOAD_AB=latch:20, ~97-player
  HugeGrass, ~42 fps): input -> screen 120 -> 84 ms; yaw direction reversals 0.8 -> 1.2-1.3 a second (a little extra
  wobble). Valhalla ~230 fps: 52-62 -> 42-49 ms. Unpatched matrices facing the camera's way (PELOAD_LATCH_SKYCHECK=1) were
  none camera matrices: the sky shares the world camera. Mouse only. PELOAD_LATCH=0 = off (no learning either). Stats page:
  a "late camera" checkbox beside the frame-rate line (POST do=latch&on=0|1: applied vs learning only, to feel the difference in
  a running game) with what it learned; snapshot "latch" (latch_json). TURNED OFF the same day (play.sh no longer sets it, the
  page toggle removed, no learning unless PELOAD_LATCH=1): it is mouse only, and the user plays with a controller.
- **Input -> screen latency measured end to end, and a per-second performance log** (2026-10-25; user: "the window adds its own
  display delay on top of these numbers -- THIS is what I need you to calculate, cuz that feel is all that will matter to the
  player"; "make sure you have a running log of the FPS ... compared to objects on the screen") — new shim/src/latency.c
  (hooks lat_cam in hitbox.c hb_capture_view, lat_present in d3dsubmit.c sub_present, lat_gpu in glh_pq_publish, lat_win in
  present_thread_fn after the window flush, lat_input in wininput.c wi_raw_mouse and XInputGetState; perf_tick on the stats
  thread). How: every engine frame gets its camera's forward vector and the times halo3 presented it, the GPU finished it and
  the window got it; a look input (mouse delta, or the right stick leaving its dead zone) after 250 ms of none, with the camera
  still, is followed to the first frame whose camera turned, and each stage is timed from the input. The display step is a
  model: the next refresh after the window flush (CVDisplayLink) + one refresh for the window server's composite.
  PELOAD_LATPROBE=<secs> (test clients) sends a mouse delta through the raw-input path every <secs>. Output: `[latency]` per
  input, `[perf]` every 10 s, the snapshot's "perf", and ~/Library/Application Support/ReclaimerMac/perf/<date>_<time>_<pid>.csv
  every second (engine and window fps, frame-time median / 95% / worst, draws, objects by type and in the camera's view,
  players, map, quality, stage lags, input -> screen; 30 newest kept). shim/tools/latency-summary.py <log> = the stage medians;
  shim/tools/titleshot.sh = a picture of the client's window with its title bar (fps / draws / frame time), via
  shim/tools/winid.c + `screencapture -l`. First numbers, HugeGrass hangar view, 22 fps, 6,400 draws, 60 Hz display, 76
  probed inputs: input -> halo3 presents the turned frame 53 ms, -> GPU done 124 (+71), -> window 129, -> screen ~154 ms
  (10% 128, 90% 189). PELOAD_LATENCY=0 / PELOAD_PERF_LOG=0 = off. GPU-done times are the helper's own (glhelper.c pq_poll writes
  each frame's number and fence time into glh_shm.h done_fno / done_t, GLH_VERSION 8; at first the loader stamped them when it
  next looked, up to a frame late, which made the window step negative). The stats page's header has a second line with the
  same numbers (snapshot "perf"). After the day's fixes: local Valhalla 207 fps 48 ms (input -> present 18, GPU +5, window +1,
  screen +24); The Pit FFA, 41 players, ~104 fps: 54 ms; HugeGrass hangar ~36 fps: ~104 ms.
- **Integer vertex formats read as their values, not as 4 floats; the false "format 12 not translated" warning** (2026-10-25;
  user: "seems like something to fix") — peload.c `d3dgl_attrib_fmt` (returns 1 for an integer format, using
  `d3dgl_attrib_ifmt`'s size and type, read as float(n)), the attribute loop (warns once per VS / format only when an integer
  format really feeds a FLOAT shader input: `vertex input <sem> ... feeds a float input of VS <hash>`). Why: integer formats
  fell through to "4 floats" (16 bytes, garbage values) and logged "vertex attribute format 12 not translated" before the
  loop switched integer VS inputs to glVertexAttribIPointer -- which is what every R16G16B16A16_UINT input in HugeGrass is
  (no mismatch line in a match), so it was a false alarm there; a float input fed an integer format now gets the values.
  PELOAD_D3DGL_INTFMT_FLOAT4=1 = 4 floats, as before.
- **One frame on the GPU instead of two: ~35 ms less latency** (2026-10-25; user: "the graphic UI feels sluggish and floaty")
  — glh_client.c `glh_pq_publish` (`g_gpu_frames`, `ab_gpuf`; d3dsubmit.c A/B feature 17). Why: the GL thread could run 2
  presented frames ahead of the frames the GPU had finished, on top of the render thread's 1 frame of lead over the GL thread:
  ~2.5-3 frames between the game presenting a frame and the GPU finishing it (70-90 ms at 26-40 fps on HugeGrass). How: 1 frame
  (the GPU draws one while the next is encoded). In-match A/B, HugeGrass, 18 players, phases of 20 s (PELOAD_AB=gpuf:20):
  present -> GPU done 70-90 ms with 2, 36-50 ms with 1; fps in adjacent comparable phases 36.1 / 38.2 (2) vs 37.9 (1).
  PELOAD_GPU_FRAMES=2 = as before (1..4).
- **PELOAD_GLH_GPUPROF=1** — glhelper.c `gp_switch` / `gp_poll`: GPU time (GL_TIME_ELAPSED queries) per framebuffer the helper
  draws into, the present copy and readbacks, reported every 5 s: where the GPU's time goes when a game is GPU-bound.- **halo3's local-user lookups answered once per frame** (2026-10-25) — peload.c `lu_memo` / `lucache_install` (stubs at
  halo3+0xfbb94 and 0xfbc04), `hb_frame_hook`, `ab_lucache` (d3dsubmit.c A/B feature 23). Why: the engine thread's call stacks
  (`PELOAD_SAMPLE_STACK`, public Hugegrass, ~126 players) put ~9% of it under each of two questions halo3 asks for EVERY player,
  twice a frame (0xff054 -> 0xfbda8, a player's status, from the render preparation and the players update), with answers that
  are the same for all of them: (1) 0xfbb94 "how many local users": it calls 0x10e0b4(i) per user slot and 0x10e0b4 builds the
  whole user list again per call; the Reclaimer client raises the slot count from 8 to 64 after load (`mov edi,8` -> 64) and
  replaces 0x10e0b4 with a version that also fills and frees two heap vectors: ~16,000 list builds a frame. (2) 0xfbc04 "which
  player is local": it walks the player list asking 0x10e060(player) (client-hooked) until the local player answers, so its
  cost depends on where our player sits in the server's list -- late (a client that joined a full server) = ~32,000 hook calls
  a frame; that is why the same 126-player server ran at ~27 ms a frame in one match and 76-97 ms in others. How: each entry
  jumps (`jmp [rip+0]`, no register) to a stub that returns the answer computed earlier in the same engine frame (the main-loop
  frame hook advances one shared frame number) and otherwise runs the original -- the client-patched body as it is in memory --
  and caches it. Hit path writes only eax and flags, miss path also ecx (each original writes both; halo3 is whole-program
  optimised, section 17); the functions' bytes are checked at load; installed only with the frame hook. `PELOAD_LUCACHE_CHECK=1`
  reruns each original at every frame start and compares: local Valhalla 1,200 frames, 0 differences (answers 64 and the local
  player's handle). Result so far: the profile's ~9% under 0xfbb94 drops to 0.0% (stacks), but four visible in-match A/Bs on
  public Hugegrass (20 s phases, engine thread CPU ms per frame) showed no measurable difference (e.g. 32-39 ms on and off with
  our player in slot 53) -- the lookups' cost varies with the match; kept on since it is verified correct. Old:
  `PELOAD_NO_LUCACHE=1`; in-match A/B `PELOAD_AB=lucache:<secs>`.
- **A/B log: the engine thread's CPU time per frame** (2026-10-25) — d3dsubmit.c `sub_switch`, peload.c `eng_cpu_s`. Why: big
  Hugegrass scenes are often limited by the GL / GPU side (5,000-9,000 draws), which hides an engine-thread saving from the
  frame rate. How: each `[ab]` phase also prints halo3's engine thread's user + system CPU time divided by the frames
  presented ("engine X ms/frame", totals per side).
- **GLSL varying scanners search one line, not the rest of the shader** (2026-10-25) — peload.c `d3dgl_line_find` (used by
  `d3dgl_rename_varyings`, `d3dgl_dedup_varyings`, `d3dgl_iface_mask`, `d3dgl_relocate`). Why: each line's `strstr` ran to the
  END of the source and the hit was then only used if it fell inside the line: O(lines x length) per pass, several passes per
  shader pair; it was most of the pre-linker's loader-side time (`[sample]` stacks: d3dgl_prelink_tick -> d3dgl_monoprog ->
  d3dgl_iface_mask -> strstr, ~10% of halo3's engine thread at the menu) and part of every first-use link in a match. How:
  memmem within the line (same results: every caller only used matches inside the line). Menu, 730 pairs prelinked, GL helper:
  18.2 -> 15.2 ms per pair (in-process GL 30.7 -> 22.9). Old: `PELOAD_D3DGL_SCAN_OLD=1`.

## 20. Scripts and RE tools

- **Sampler call stacks, a dump with every function, no lost threads** (2026-10-25) — peload.c `sampler_thread`, `smp_walk`,
  `smp_epilog`, `smp_chain_root`; shim/tools/sample-dump.py. Why: (1) the in-process sampler (`PELOAD_SAMPLE=<us>`) only saw
  the instruction pointer, so halo3's flat profile (top 45 functions ~26% of the engine thread) said nothing about which
  engine systems cost the time; (2) `PELOAD_SAMPLE_DUMP` was written after the 10 s report, which clears each thread's top 8
  entries as it prints them -- the dump lacked exactly the hottest functions; (3) its thread table had 96 records and never
  freed one: a menu-runtime client fills it with short-lived threads before halo3's match threads start, and those were then
  never sampled; (4) a fragment with chained unwind info was keyed by its own start (halo3+0x449693 instead of 0x449650).
  How: `PELOAD_SAMPLE_STACK=<thread name part>` (1 = halo3's engine thread guest-0x18000a120) walks that thread's stack at each
  sample while it is suspended: guest frames by their .pdata unwind codes (chained -> parent; an epilogue on the top frame
  decoded like Windows does), host code on top by the rbp chain to the first guest return address (its frames are kept: a
  loader function under halo3 shows by name; a frameless leaf's caller from [rsp] when a call instruction ends there); reads
  only between the sample's rsp and the thread's pthread stack top, a fault ends the walk (`t_dump_jb`), no dladdr while
  suspended. Distinct stacks are counted (16,384 per 10 s) and dumped as `S<tab>thread<tab>samples<tab>outer;...;inner`;
  `sample-dump.py <dump> [--dll halo3.dll] [--tree] [--focus halo3+0x..]` prints self, inclusive and the call tree (--dll names
  leaf functions without .pdata by their start). The dump is written before the report; records of threads not listed for 200
  passes are reused (256 records); names and stack ranges are re-read every 10 s. Each interval starts with a `T` line (seconds
  up, seconds since gameplay started or -1): `--match [SECS]` keeps gameplay only. Old keys: `PELOAD_SAMPLE_NOCHAIN=1`.

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

- **shim/tools/cowtest.c** (2026-10-25) — is the loader's copy-on-write guest copy (mach_vm_copy) a real copy under Rosetta
  when used like halo3's game-state copies (16 MB, the source written after each copy, the copy read on another thread,
  copies of copies in a ring)? 1,500 + 900 rounds: 0 mismatches.
- **shim/tools/aotbench/** (2026-10-25) — Rosetta ahead-of-time vs run-time translation of the same x86-64 code, plus the
  dylib route and a detour patch into it (PERF-PLAN.md "Not the bottleneck": equal within noise; not pursued).
  **shim/tools/sample-binaries.py** — a thread's samples by the image they end in (translated guest code "Rosetta JIT",
  "Rosetta Runtime Routines", the loader, system libraries). **shim/tools/ab-phases.py** — fps and [submit] waits per
  PELOAD_AB phase. **shim/tools/hitch-sample.sh** — `sample` a client when a second drops below N fps.

- **re-tools/tlsref.py** (2026-11-07) — who uses a halo3 TLS block field: halo3's idiom loads the field offset into a
  register (`mov edx, 0xc0` ; `mov rax, [rax+rcx*8]` ; `mov rsi, [rdx+rax]`), which a plain `[reg + 0xc0]` search misses;
  `--then` keeps only hits that go on to touch given offsets. Found the player control record's users (halo3+0xf3f48..0xf9520).

- **re-tools/machoport.py** (2026-11-08) — nhook's app addresses into a new native app build, the arm64 Mach-O counterpart of
  sigport.py / patchscan.py: each point (POINTS) found again by its function's shape (b / bl / adrp / adrp-relative add, ldr,
  str and literal immediates masked; strict, then loose with conditional-branch offsets masked too), else a window of masked
  instructions unique in the new __text; data points through their users' instructions; `call` (the bl to a ported function with
  given words before it) and `infunc` / `imm` (an instruction inside a ported function, a field decoded from it: the mixer's
  sender id offset). Every point then gets a meaning check in the new build, and the log text the parsers need is looked up.
  `sigs` stores the reference build's needed functions as raw words (re-tools/machoport-sigs.json, 11 functions, 177 KB) so `port`
  needs no 0.10.0 binary (the app replaces itself on update). `selftest` reproduces 0.10.0 from the binary and from the sigs;
  porting 0.10.1 from either gives the same 12 addresses. **shim/tools/native-test-all.sh** -- every live native test in a row
  (hooks, fire, aim, walls, tpnext, voice, spar, soundboard; ~25 min), the check after an app update.
  **shim/tools/native-soundboard-test.sh** -- the soundboard end to end (two test clients, our server, muted output).
  **shim/tools/voice-fifo-probe.sh** -- whether the app's --voice-wav streams (it does not: it reads the file to its end first).

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
