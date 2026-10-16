# HaloReclaimerMac — project notes for Claude

Goal: run Project Reclaimer (fan Halo 3/MCC client; Windows-only Rust exes that load MCC's
`halo3.dll`) on Apple Silicon **without Wine**, via a from-scratch Win64 PE loader
(`shim/src/peload.c`) under Rosetta. Success = see the menu render OR launch the game.

## Build & run the loader
```bash
bash shim/build.sh            # clang -arch x86_64 ... -o shim/build/peload
# full-client / match path (loads Guardian/Slayer, starts the sim):
PELOAD_SKINGUARD=1 PELOAD_PATCH="exe+4e066d:909090909090" PELOAD_LOCAL_RUNTIME="__unset__" \
  PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_WORKER_INHERIT_STDIO=1 \
  arch -x86_64 shim/build/peload analysis/project-reclaimer-v0.9.13.exe
```
`PELOAD_SKINGUARD=1` installs the armor-skin null-key guard (entry detour on halo3+0x2ac24c);
without it round-start faults at RVA 0x2AC329. With it: map loads 100%, player spawns,
`simulation_ready:true`, "gameplay presentation ready". Read the worker's stdout for state —
the engine's `C:\ReclaimerProfile\Logs\client.log` file stays empty, so always run with
`PELOAD_WORKER_INHERIT_STDIO=1` and parse stdout (see memory [[reclaimer-skinguard-simstart]]).
`PELOAD_WORKER_INHERIT_STDIO=1` makes the spawned `probe-engine-worker` child log to our
terminal (it's a separate process; the orchestrator spawns it via CreateProcessW).
Useful env: `PELOAD_TRACE_NET/_D3D/_DRAW/_TEXT/_GDI/_SWAP/_MSG`, `PELOAD_SHOT` (dumps the
presented surface to `~/reclaimer_gdi_*.bmp`), `PELOAD_POKE=<rva>:<u8>` (continuously writes a
byte into guest memory from the present thread). Patches: `PELOAD_PATCH=<mod>+<rva>:<hexbytes>`.
Kill leftover workers between runs: `pkill -9 -f 'build/peload'`.

## Play (current best path — 2026-10-16)
```bash
bash shim/build.sh && bash shim/play.sh      # game-client on the D3D11->GL renderer; log: /tmp/reclaimer_play.log
```
Guardian/Slayer renders with correct colors, depth, shadows, skinned meshes (first-person weapon),
GPU particles (CPU compute interpreter, `shim/src/dxbc_cpu.c`), the loading screen, game audio
(XAudio2 on CoreAudio, `shim/src/xaudio.c`), controller (XInput on GameController.framework, pitch
inverted by default) and keyboard/mouse (Raw Input from NSEvents, `shim/src/wininput.c`; click the
window to capture, F1 releases). The window title shows engine fps / draws per frame; the window is
resizable and full-screen capable. `play.sh` adds `exe+65ddf9:eb` (skips the client's 45 s startup
watchdog that otherwise parks the input thread). Key A/B switches: `PELOAD_AUDIO=0`, `PELOAD_INVERT_PITCH=0`,
`PELOAD_D3DGL_NOPQ/NOSTATECACHE/NORING/NOUBO/NOSRGB/NOCMP/NOYFLIP/NODEPTH`, `PELOAD_NOCSCPU`.
Diagnostics: `PELOAD_FPS_LOG`, `PELOAD_D3DGL_PROF`, `PELOAD_SAMPLE=<us>`, `PELOAD_TRACE_FAULT`,
`PELOAD_D3DGL_SHOTWIN=<n>` (+`_FROM/_TO` seconds after gameplay start; capture names carry the engine
frame `_f<N>`; `PELOAD_D3DGL_ALT5` phase-tagged A/B), `PELOAD_INPUT_SCRIPT=walk|fire|<segments>`,
`PELOAD_KBM_TEST`, `PELOAD_AUDIO_DUMP=<raw>`. The GLSL translator runs in-process every launch;
`~/reclaimer_glsl_v5/` is written for inspection only (`PELOAD_D3DGL_GLSLCACHE=1` reads it back).
Hang debugging: `kill -USR1 <worker pid>` dumps every guest thread (rip + stack-scanned return addrs);
`PELOAD_PEEK=<hexaddr>:<hexlen>[,...]` adds hex dumps of guest memory to it. `PELOAD_TRACE_LE=<code>`
logs GetLastError calls returning <code> (with the guest caller and the last failed path).

Online join (2026-10-16): `RECLAIMER_JOIN=host:port bash shim/play.sh` passes the client its own join
link `reclaimer://join?server=host:port` as argv[1] (what its URL handler / Discord does). The client finds
MCC itself, boots the menu, joins the lobby, prepares the server's next playlist map and loads in. Proven
on btb1.coyot.ee:49178 (Epitaph, ~85 fps). `RECLAIMER_JOIN_LINK` env is only read by the Discord thread.
Two shim bugs blocked it: FindFirstFile handles were never reused (after 64 scans every read_dir failed
with a stale error -> "Cannot prepare playlist map: os error 80"), and XAudio2 FlushSourceBuffers kept the
head buffer of a STOPPED voice, so its OnBufferEnd never came and the engine's pre-load "drain every voice"
wait (halo3+0x1d17be, busy byte at voice+0x38) spun forever.

GL call ABI: guest code (Reclaimer's egui/WGL UI) calls GL through make_gl_thunk's Win64->SysV stub, which
must preserve xmm6-xmm15 (Win64 callee-saved, SysV scratch) as well as rdi/rsi. It didn't until 2026-10-16:
egui's painter keeps pixels_per_point in xmm6 across GL calls and got 1280.0 back, so all its clip rects
collapsed (empty Server Browser list, missing lobby names/vote list). `PELOAD_GL_NOXMMSAVE` = old behavior.
Float-arg GL calls go through typed ms_abi wrappers (GLMIX) because Win64 passes floats positionally.

play.sh starts in Reclaimer's MENU runtime by default (2026-10-16); `RECLAIMER_DIRECT=1` = the old direct
Guardian match (PELOAD_LOCAL_RUNTIME=__unset__), which never creates Reclaimer's in-game UI windows, so
its scoreboard has bars but no names/scores. Menu Host Game -> Start Game works (its old "hang" was the
XAudio2 voice-drain bug). Scoreboard text needed a foreground fix: ShowWindow/CreateWindow only make a
window foreground under Windows' activation rules (not WS_CHILD / WS_EX_NOACTIVATE, not SW_SHOWNOACTIVATE
/ SW_SHOWNA ...); the menu runtime's ReclaimerIngameMenu overlay (child, NOACTIVATE) used to become
"foreground", so GetForegroundWindow != game window and keys went to the overlay ("Scoreboard shown"
never fired). `PELOAD_FG_LASTSHOWN` = old behavior. Self-test: `PELOAD_KBM_TEST_HOLD=30:6` (Tab) +
`PELOAD_KBM_TEST_HOLD_GAMEPLAY=1`/`PELOAD_KBM_TEST_GAMEPLAY=1`; KBM tests never capture the real cursor
(`PELOAD_NO_SYS_CURSOR` does the same for any run).

Second player for testing (2026-10-16): `shim/target.sh` = an input-less client (PELOAD_NO_PAD, NO_KBM,
NO_SYS_CURSOR, NO_FOCUS, AUDIO=0, own RECLAIMER_PROFILE_DIR=C:\ReclaimerProfileTarget, nickname "Target")
that hosts a hidden Online game via `PELOAD_AUTOCLICK` ("x,y;w:x,y,n;t:text;k:hex;s:secs" scripted UI steps
after the menu is up). Game port 11774; join with RECLAIMER_JOIN=127.0.0.1:11774. A LOCAL-mode host is NOT
joinable by address (only its 127.0.0.3 transport; "Public join timed out"), and `local-join` needs a
local-server ticket ("Invalid client ticket"). Its pgid lives in ~/Library/Application Support/ReclaimerMac/
target.pgid and play.sh's leftover-kill skips it.
Headless `dedicated` server (dedicated init/check/run, C:\ReclaimerTestServer test folder with game/halo3 ->
MCC symlink): config/check/online setup work, but its local-server worker idles at "Private native host
descriptor verified; waiting for the native game client" (halo3 main loop never runs) -> server stays
ready:false. Not solved. Shim fixes found on the way: CreateDirectoryW now returns real errors (183/3; it
always claimed success, so create_dir_all never made parents; PELOAD_MKDIR_LAX = old), WSADuplicateSocketW +
WSASocketW(FROM_PROTOCOL_INFO) for Rust try_clone (was a stub -> WSAEAFNOSUPPORT 10047), overlapped ReadFile
on pipes/sockets uses read() + async completion (pread -> ESPIPE made parents stop reading children's
stdout), GetOverlappedResult(wait) waits. PELOAD_WORKER_INHERIT_STDIO breaks parents that pipe child stdio
(the dedicated supervisor saw EOF and restarted servers in a loop).

Perf (2026-10-16, M4, 1280x720 window, 1920x1080 internal): gameplay median ~116 fps, p10 ~80,
min ~50 (3-min soak). Draw-call bound: ~3.6 us per glDraw* in GL-on-Metal x ~1200-2300 draws/frame;
the next big step would be a GL submission thread. CPU particle compute runs on a worker thread
(`PELOAD_CS_SYNC` = inline). Window flips are NOT vsynced by default (`PELOAD_VSYNC=1` opts in; one soak
froze with it while the display slept); the present thread idles while the window is occluded or the display
sleeps (`PELOAD_PRESENT_HIDDEN=1` keeps presenting -- needed for SHOTWIN captures in unattended test runs).
First-use shader stalls: every linked VS/PS pair is recorded (by bytecode hash) in
`~/Library/Application Support/ReclaimerMac/glpairs_v1.txt` and pre-built during the next loading
screen (`PELOAD_D3DGL_NOPRELINK`; `PELOAD_D3DGL_PROGTIME` logs build costs).
Correctness switches added 2026-10-16: `PELOAD_D3DGL_NOINTIN` (integer vertex attributes as floats --
the old cause of one-frame white flashes / exploded skinned geometry), `PELOAD_D3DGL_NOQUERY`
(occlusion queries), `PELOAD_D3DGL_NOCANON` (identical-bytecode shader sharing).
Glitch hunting: `PELOAD_D3DGL_COVQ` (+`_COVQ_FRAC`, `_HUGEPROBE=<t>`) per-draw sample counts,
`PELOAD_D3DGL_WHITEDUMP` (draw flight recorder on auto-exposure spikes), `_LUMLOG`, `_FSLOG`,
`_FRAMESTAT`, `_CBSCAN`, `_PSCLAMP=<c>`, `_SKIPVSH=<hash,..>`, `_DUMPHASH=<hash,..>` (GLSL+DXBC to ~).

## Render Guardian via the game client (GL-on-Metal, no MCC/Steam)
`game-client` runs the engine in a standalone native window ("Reclaimer GL (OpenGL on Metal)") and
actually renders Guardian gameplay. Needs the XAudio2 bypass + skinguard + two crash guards:
```bash
XA="exe+460294:909090909090,exe+4602cb:909090909090,exe+460302:909090909090,exe+460335:9090"
PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 PELOAD_PATCH="$XA" PELOAD_LOCAL_RUNTIME="__unset__" \
  PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_WORKER_INHERIT_STDIO=1 arch -x86_64 shim/build/peload \
  analysis/project-reclaimer-v0.9.13.exe \
  game-client --mcc-path "C:\Program Files\Steam\steamapps\common\Halo The Master Chief Collection"
```
- `PELOAD_VTGUARD` — detour at halo3+0x172bce: skips a refcounted-resource drop when its vtable is NULL
  (stubbed-GPU resource) instead of faulting at RVA 0x172BD4. See [[reclaimer-remote-and-gameclient]].
- `PELOAD_ALLOC_GUARD` — VirtualAlloc trailing guard page for general/high allocs (addr hint NULL or
  >=0x200000000 only; never the 2GB-scan window) so the GL-on-Metal SIMD texture/buffer copies can
  overread a few bytes past a buffer end without faulting.
- Both are opt-in (off by default → canonical probe/sim path unaffected). With them, Guardian gameplay
  runs sustained (tick 2152 / ~36s, 0 crashes) vs the old crash at ~6s.
- Networking works against the live internet: `discover` lists real servers (243/255 answered),
  `server-status <host:port>` does a full UDP handshake. Needs the TCP net fixes in [[reclaimer-client-discovery]].

## Reverse-engineering the Rust worker (angr — the user asked for oxidizer; this is its engine)
`oxidizer` (https://github.com/sefcom/oxidizer) is a Rust decompiler built on angr. Its full
`uv sync` FAILS on this macOS (pinned pyvex builds VEX from source → pycparser "Unmatched '}'").
Workaround that WORKS: PyPI angr wheels (no native build) via `re-tools/`.

```bash
re-tools/setup.sh                          # one-time: uv venv + angr==10.0.3, bitstring==4.1.4
source re-tools/angrenv/bin/activate
# decompile a function BY VMA (ImageBase 0x140000000 for the exe, 0x180000000 for halo3.dll):
python re-tools/decomp.py analysis/project-reclaimer-v0.9.13.exe 0x14054ae30 0x14054a000 0x14054f000
# helpers (no angr needed): find function bounds / string refs / data-xrefs:
python re-tools/refs.py analysis/project-reclaimer-v0.9.13.exe func   0x553132
python re-tools/refs.py analysis/project-reclaimer-v0.9.13.exe strref "presentation callback stalled"
python re-tools/refs.py analysis/project-reclaimer-v0.9.13.exe globref 0x228b490
```
angr loads the 37 MB PE in ~3 s and decompiles one function in a few seconds when CFGFast is
region-limited (pass [rstart,rend] around the target). Standard angr gives C-like pseudocode
with recovered structs + global names (`g_<vma>`); oxidizer's Rust-specific layer (Option/
Result/match) needs its native `rustylib` built for macOS, which isn't done. `.pdata` gives
exact function [start,end). Rust string literals are concatenated, so a literal often has no
direct `lea` ref — search a distinctive substring that IS referenced directly.

## RE findings index
- `shim/docs/RE-findings-menu-reentrancy.md` — the running RE log (sessions 1–8).
- Memory files under `~/.claude/projects/.../memory/` — net/AFD, GDI text, goal.
- SOLVED: map load (was 15% stall) now reaches 100% via the ucontext fiber fix; round-start
  (was 0xC0000005 @ RVA 0x2AC329) now passes via `PELOAD_SKINGUARD`. Sim starts:
  `simulation_ready:true`, player unit spawns, "gameplay presentation ready".
- SOLVED: audio per-tick log spam (readback-matched Set/GetVolume + Set/GetChannelVolumes on the
  XAudio2 voice vtable, slots 12-15) and the "thin-strip" garbage window (present-capture now only
  latches window-aspect ~16:9 surfaces, so 1024² atlases/shadow maps can't hijack the frame).
- RENDER PATH FOUND: `project-reclaimer.exe --help` lists subcommands. The default forced-match run
  (above) is a disposable hosting PROBE that never draws 3D (geometry=0). The commands that actually
  render a local match (no probe/hosting-verification): `local-server` (headless Guardian host),
  `local-join` (loads Halo 3 + joins the local server = the RENDERING client), `tool-match` (needs
  H3EK --tools-path), `game-client` (default). e.g.:
    peload <exe> local-server --mcc-path "C:\Program Files\Steam\steamapps\common\Halo The Master Chief Collection" --seconds N
  local-server spawns the worker with `--real-client` (the render mode).
- XAUDIO2 CHECK CRACKED -> local-server HOSTS Guardian. The XAudio2 integrity check is exe
  sub_140460250 (verifies 4 halo3 callback slots then installs our XAudio2 shims). Bypass by NOP'ing
  its 4 mismatch branches. Run a working host:
  ```bash
  XA="exe+4e066d:909090909090,exe+460294:909090909090,exe+4602cb:909090909090,exe+460302:909090909090,exe+460335:9090"
  PELOAD_SKINGUARD=1 PELOAD_PATCH="$XA" arch -x86_64 shim/build/peload \
    analysis/project-reclaimer-v0.9.13.exe local-server \
    --mcc-path "C:\Program Files\Steam\steamapps\common\Halo The Master Chief Collection" --seconds 60
  ```
  -> "Constructed native Guardian/Slayer variant 'Custom Slayer'" + "Local server browser listening
  on 127.0.0.1:<port>". Headless host (CPU rasterizer, no GPU). The CLIENT `local-join` renders.
- CLIENT DISCOVERY MOSTLY SOLVED — see memory [[reclaimer-client-discovery]]. `local-join` discovery
  is: enumerate `local-servers/<uuid>.json` (`{"port","pid"}`, 25B) -> GetFileInformationByHandle
  freshness -> OpenProcess(pid) liveness -> non-blocking TCP connect to the port -> select(w,e).
  `local-join --instance <uuid>` (uuid len 32/36) targets `<uuid>.json`. Fixed shim bugs that broke
  every step (all verified non-regressing vs the canonical SKINGUARD sim path):
  * GetFileInformationByHandle zeroed FILETIMEs (1601 = always stale) -> now real stat timestamps.
  * OpenProcess returned -1 (GetExitCodeProcess(-1)->0 = "exited") -> now pid-encoded handle
    (0x8000+pid, kill(pid,0) liveness) so a live non-child pid reports STILL_ACTIVE; added GetProcessId.
  * TCP listen/accept were stubbed ("not needed for UDP") -> implemented (ws2 ordinals 1,13).
  * `PELOAD_KEEP_ADV` keeps local-servers/*.json alive (transient otherwise) for a stable client window.
  New debug: `PELOAD_TRACE_IPC` (OpenProcess/DuplicateHandle), `[FindFirst]` under TRACE_FS,
  `PELOAD_CAP_ADV` also dumps advertisement WriteFile bytes.
- Current blockers (on the render path):
  1. **Private host doesn't publish locally** — runtime main `sub_14065c7d0` splits on `rdi` at
     exe+0x65d4ca (`cmp rdi,2; je`): rdi==2 (what `local-server` is) -> private-descriptor path
     `sub_1404f95d0` ("Private native host descriptor verified; waiting for the native game client")
     which writes NO `local-servers/<uuid>.json`; rdi!=2 -> browser path `sub_14059cfe0` which DOES
     advertise but also `connect(148.251.153.44:8790)` = remote master server (internet path, wrong for
     same-PC, and its local accept didn't deliver). Next: RE how `sub_1404f95d0` is meant to expose the
     private host to local-join (local advertisement the shim drops? other same-PC IPC?).
  2. **D3D11→GL translation** — once local-join joins, it issues real 3D geometry draws (unlike the
     probe); translating them to GL/Metal is the last step to pixels. Groundwork: D3DObj retains
     shader bytecode; PELOAD_DUMP_SHADERS/TRACE_SHADER/DRAW_CENSUS/TRACE_UDP/TRACE_NAMED.
