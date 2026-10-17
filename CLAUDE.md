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
`simulation_ready:true`, "gameplay presentation ready". Read the worker's stdout for state
(see memory [[reclaimer-skinguard-simstart]]). `C:\ReclaimerProfile\Logs\client.log` stays empty: the client opens it
(OPEN_ALWAYS, GENERIC_WRITE) but never writes to it under the loader (fd offset stays 0; cause unknown, harmless).
Rust append-mode opens (FILE_APPEND_DATA without GENERIC_WRITE, 0x120114, e.g. the dedicated supervisor's
server.log) used to open read-only so every write failed and WriteFile dumped it to stderr; since 2026-10-16 they
are O_APPEND (PELOAD_APPEND_RO = old), and writes to *.log files are also copied to stderr (PELOAD_NO_LOG_TEE).
`PELOAD_STDERR_TEE=<file>` / `PELOAD_STDOUT_TEE=<file>` (`%p` = pid) copy a process's stdio into a file, inherited by
every child: the way to read processes whose stdio another process owns (dedicated server workers).
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
resizable and full-screen capable. `play.sh` sets PELOAD_RECLAIMER_PATCHES=all (signature patches, peload.c
g_sigpatches); its `watchdog` patch only matters for RECLAIMER_DIRECT=1: the worker's engine hang watchdog arms
only when RECLAIMER_LOCAL_RUNTIME is unset or "<role>:<secs>" with secs > 0 (menu mode passes "menu:0"). Key A/B switches: `PELOAD_AUDIO=0`, `PELOAD_INVERT_PITCH=0`,
`PELOAD_D3DGL_NOPQ/NOSTATECACHE/NORING/NOUBO/NOSRGB/NOCMP/NOYFLIP/NODEPTH`, `PELOAD_NOCSCPU`.
Diagnostics: `PELOAD_FPS_LOG`, `PELOAD_D3DGL_PROF`, `PELOAD_SAMPLE=<us>`, `PELOAD_TRACE_FAULT`,
`PELOAD_D3DGL_SHOTWIN=<n>` (+`_FROM/_TO` seconds after gameplay start; capture names carry the engine
frame `_f<N>`; `PELOAD_D3DGL_ALT5` phase-tagged A/B), `PELOAD_INPUT_SCRIPT=walk|fire|<segments>`,
`PELOAD_KBM_TEST`, `PELOAD_AUDIO_DUMP=<raw>`. The GLSL translator runs in-process every launch;
`~/reclaimer_glsl_v5/` is written for inspection only (`PELOAD_D3DGL_GLSLCACHE=1` reads it back).
Hang debugging: `kill -USR1 <worker pid>` dumps every guest thread (rip + stack-scanned return addrs);
`PELOAD_PEEK=<hexaddr>:<hexlen>[,...]` adds hex dumps of guest memory to it; `PELOAD_DUMP_DEPTH=<n>` shows n frames
per thread (default 16). `PELOAD_TRACE_LE=<code>`
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
target.pgid and play.sh's leftover-kill skips it. It runs windowless: `PELOAD_HIDE_WINDOW=1` (Accessory app, the
NSWindow is never ordered in, the present thread idles; PELOAD_AUTOCLICK still works because it posts to the
guest wndproc) + `PELOAD_MAX_FPS` (Present sleeps; default 30 when hidden). TARGET_SHOW=1 = the old minimized window.
Headless `dedicated` server WORKS (2026-10-16, v0.9.18): `bash shim/dedicated.sh` (start/status/stop; first use runs
`dedicated init`, links game/halo3 to MCC and writes a LOCAL-ONLY config: hidden, master off, upnp off, 127.0.0.1,
Valhalla Slayer UDP 49190). Tree: supervisor -> `local-server` -> `probe-engine-worker ... --allow-empty-match
--real-client` (stdout NUL, stderr = pipe to the supervisor, so read them via PELOAD_STDERR_TEE, which dedicated.sh
sets: /tmp/reclaimer_dedicated_<pid>.log). The server sits in its lobby until min_players (1) join -- "Private native
host descriptor verified; waiting for the native game client" is client_bridge.rs (real-MCC private bridge) and
benign, every host prints it. A join takes ~20 s; the first join right after a server start can fail ("Public
admission rejected: aborted by peer ... during the handshake" / client "Public join timed out"): retry. The
supervisor never fills data/<server>/server.log (it reads the worker pipes; not chased). Shim fixes found on the way: CreateDirectoryW now returns real errors (183/3; it
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
Live stats (shim/src/stats.c; user docs in README "Live game stats over HTTP"): `PELOAD_STATS_HTTP=<port>` (play.sh:
8765 by default, `=1` = 8765, `=0` off). A loader thread snapshots halo3 memory every 100 ms into JSON; only the process
that loads halo3.dll binds. Page = shim/src/stats_page.html, #embed-ed at build time; play.sh sets PELOAD_STATS_PAGE to
the source file in a checkout, so page edits need only a browser refresh. Sources (all CONFIRMED by static RE, docs in
shim/docs): players table TLS +0x10 (player +0x28 unit, +0x58 name, +0x1FC active traits), objects TLS +0x38, game
variant = game_globals (TLS +0x48) + 0x1608 (halo3-variant-traits.md), local player = player mapping TLS +0x110,
output user -> player handle at +0xB8 (section 6; camera-in-biped / view-ray guess only as fallback), network session
`*(u64*)(halo3+0x1e250d0)` for the Connected list (works in the lobby; reclaimer-voice-mute.md). Deaths: any biped
alive -> dead. Shots: the object_delete hook (hb_on_delete, sim thread) logs every projectile's owner (+0xC0) and the
biped whose bounding sphere holds its end point (estimated hit) into g_hbshot (8192 ring). Voice: set_muted(id,bool)
(found by signature, run from the halo3 frame hook via st_voice_jobs) = the client's own mute list; talking = an entry
hook on the client's voice_recv (installed at exe load, st_voice_install) stamping each sender. Controls: POST
/control?do=... + header X-Reclaimer-Control (CSRF guard; PELOAD_STATS_READONLY=1 disables) flip the same switches as
the F-keys (g_hb_toggle bits; F9 = visible only). F8 / do=capture (hb_capture_tick, render-thread Present):
~/reclaimer_cap_<pid>_<n>.bmp (the game backbuffer -- the overlay is drawn later, at the window blit, so it is NOT in
it), _depth.bmp/.f32 (the camera draw's DSV, rows top-first), .txt (camera, view-proj, depth stats).
Heatmap per map: footprint cells (0.5 u) + deaths, ~/Library/Application Support/ReclaimerMac/heatmaps/<map>.bin (RHM2),
drawn over a top-down picture of the map: re-tools/h3bsp.py decodes the collision BSP + instanced geometry from the
.map's resource section (sbsp +0x38C -> zone structure_bsp_tag_resources -> raw-deflate page; shim/docs/
h3-map-geometry.md) into maptop/<map>.png + .json (world bounds); shim/tools/maptop.sh makes all 27 (needs python3 +
numpy + Pillow); served as /maptop/<map>.png|json. (re-tools/h3map.py's string-id decoding is wrong for these builds;
h3bsp.sid_name is right.)
Overlay "visible only" (F9 / PELOAD_HB_VISIBLE): hb_vis_tick blits the camera draw's depth (D32F_S8) down to 480x270
and reads it back through a 3-PBO ring (2 frames late, no stall); hb_vis keeps an object when the scene at its centre /
top / bottom is not nearer than its front (centre moved toward the camera by radius + 0.3). Label text is
PELOAD_HB_TEXT_PX window pixels tall (default 7; the page's Text -/+).
Frame rate (2026-10-17, FKK BTB, 60 players, `PELOAD_D3DGL_PROF`): 3,000-4,700 draws/frame, ~9 us of loader + driver
per draw. The biggest phase is "texs" (texture/sampler binds: 0.4-1.2 s per 2 s) -- NOT uploads (texture re-uploads
7-430 ms, buffer textures ~15 ms per 2 s; `[prof] textures` / `[prof] buffer textures` lines) and not compute waits
(none): it is Apple's GL driver revalidating on each bind, translated under Rosetta. Real fix = PERF-PLAN.md option 1
(native GL helper process). Done meanwhile: getenv cache (cgetenv: the loader's 553 getenv calls on literal keys hit a
-- removed again: peload_cgetenv already caches PELOAD_* lookups), and a skip for re-uploads
when only mips below the BC-decoded base level changed (dpart/dmin per resource; PELOAD_D3DGL_NOMIPSKIP; 0 hits in BTB
so far: halo3 rewrites whole textures). The client's Advanced graphics settings (Shadows, LOD, draw distance) cut draws.
Mods/Workshop (user guide: shim/docs/mods.md; `shim/tools/servers.sh` = busiest servers + their mods from `discover`,
whose JSON has per-server mods[{title, maps[{stem,digest}], size, shared}]): the client's own `workshop-download
--subscribe <PublishedFileId>` helper needs Steam (its command line is in /tmp/reclaimer_play.log); DepotDownloader
3.4 does `-app 976730 -pubfile <id>` (+ -filelist / -manifest-only) with the remembered login (-username ...
-remember-password; saved tokens expire -> the user signs in again in a terminal, never us); the client reads
steamapps\workshop\content\976730\<id>\ (ModInfo.json; "Program Files (x86)/Steam/steamapps" in the prefix is a
symlink to "Program Files/Steam/steamapps") and checks each file against the server's SHA-256 manifest. Ultimate
Forge 2.0 (2933266434) ships its own halo3/maps/shared.map + campaign.map besides the Forge-world maps.
Rock-wall seams (tabled by the user, display-only): `PELOAD_D3DGL_DFCENSUS` (draws per depth func), `PELOAD_D3DGL_INVARIANT`
(invariant gl_Position, opt-in); the main depth buffer is DXGI 19 (D32F_S8, reverse-Z) and keeps full float precision.
Unfocused/hidden clients are held at 60 fps by the engine; PELOAD_MAX_FPS caps lower (default 30 when hidden).
Betrayal popup (backlog 15): halo3's native GUI screen c_gui_screen_boot_betrayer, posted by halo3+0xff5fc when
halo3+0xfcc20 is true (player dead: +0x28 unit NONE; betrayer handle at player +0x1F0; variant betrayal booting on).
`PELOAD_TEST_BOOT_PROMPT=1|<local player name>` names a betrayer whenever the local player is dead (test hook).
Test server: C:\ReclaimerTestServer\betrayal.toml (Team Slayer, friendly fire + betrayal booting, min_players 1,
anti-cheat off -- with it on, a 2nd local client is refused). RCON `team` moves do NOT kill the player; a client killed
with kill -9 lingers ~45 s server-side (rejoining sooner times out). Native popups do render text (the "Team Slayer /
Kill the enemy team" intro toast is legible), so the boot screen's empty text is specific to that screen.
Glitch hunting: `PELOAD_D3DGL_COVQ` (+`_COVQ_FRAC`, `_HUGEPROBE=<t>`) per-draw sample counts,
`PELOAD_D3DGL_WHITEDUMP` (draw flight recorder on auto-exposure spikes), `_LUMLOG`, `_FSLOG`,
`_FRAMESTAT`, `_CBSCAN`, `_PSCLAMP=<c>`, `_SKIPVSH=<hash,..>`, `_DUMPHASH=<hash,..>` (GLSL+DXBC to ~).

Overlay (2026-10-16, shim/src/hitbox.c; field layout in shim/docs/halo3-objects.md). Features, each a launch
variable + function key (wininput.c flips bits of g_hb_toggle; keys are consumed, not sent to the game):
F2 `PELOAD_HITBOXES=1|all|spheres` hitboxes, F3 `PELOAD_HB_TRACERS` projectile trails + bullet lines, F4
`PELOAD_HB_INFO` shield/health/weapon/grenades/camo/vehicle labels, F5 `PELOAD_HB_ITEMS` weapon/equipment labels,
F6 / `PELOAD_HB_TEAMS=all|enemies|friends` team filter (enemies red/yellow head, teammates blue). Objects come from the
"object" data array (TLS index halo3+0xa39f9c -> TLS block +0x38). Real hitboxes = the object's COLLISION MODEL read
from the .map file the game opened (hb_note_map in CreateFileW; bipd/vehi +0x34 model -> hlmt +0x10 coll ->
regions/permutations/bsps; all permutations cached, each object picks its own via its region permutation bytes at
datum+0x13A -- Elite helmets), each edge posed by the node matrices (datum+0x136). Bullets live < 1 tick, so an
entry hook on halo3 object_delete (halo3+0x342ed8, hbdel_install, prologue-checked; PELOAD_NO_DELHOOK) records
projectile datums at deletion (start +0x1e8, impact +0x1c). Text = a stroke font in the same GL_LINES batch, tag
names from the map file. Drawn in the present-queue blit (pq_present_wait -- NOT d3dgl_blit_fullscreen_i, which
only runs without the present queue). Camera = VS cb0 rows 0-3 + row 7 of the first draw passing
hb_is_world_camera. Diagnostics: `PELOAD_HITBOXES_LOG` (counts, sphere fallback reasons, permutation bytes),
`PELOAD_HB_DELDUMP=<n>` (raw projectile datums), `PELOAD_HB_SELFDUMP=<file>` (own unit + player record every
0.25 s for offline field hunting), `PELOAD_OBJDUMP`. Finding fields: halo3's HaloScript getters are in the release
build (re-tools/hsfuncs.py prints a script function's table entry -> its evaluate function -> the datum offset).
Test helpers: `PELOAD_INPUT_FACE=1` steers scripted pad segments with a zero right stick at the nearest other biped
(`PELOAD_INPUT_FACE_POINT=x,y,z` at a fixed point).
NEVER find engine structures by sweeping process memory -- it kernel-panicked the Mac (memory [[no-memory-sweeps]]).
Test runs: `bash shim/tools/quiet-run.sh "<env>" <secs>` (one instance, visible but input-less; QR_MINIMIZE=1
minimizes, QR_HIDE=1 = no window for log-only runs). The old "minimized client stalls loading" was the thread-id
mutex bug (55811fa), not minimizing. Process matches must be anchored (`pgrep -f '^[^ ]*build/peload'`):
an unanchored pattern also matches shells whose command text mentions build/peload.

Thread ids (2026-10-16): GetCurrentThreadId MUST be unique per thread (mach thread port; CreateThread's id and
GetThreadId match it). It returned 0x1000 everywhere, which silently disabled every msvcp140 std::mutex (its owner
check skips the lock when the ids match) -> data races + a lock left held -> intermittent hang at match start.
`PELOAD_TID_CONST=1` = old behavior. Loader-created threads are named (shim-*, guest-<start fn>) so [CRASH] and
SIGUSR1 dumps identify them. `PELOAD_TRACE_STAGES` counts draws with GS/HS/DS/stream-out bound (Halo 3 uses none)
and indirect draws (none either).

Water (2026-10-16): Halo 3 water = instanced patches (VS 95d9d62eca234cc3 "water_tessellated", per-instance corner
data in VB slots 1/3, barycentrics in slot 2) + PS 7ed7117500fe75b1/23caa222baa5cb9f. It never drew because the pair
failed to LINK: varying locations come from semantics (TEXCOORD10/11 -> 14/15) and Apple's GL refuses user varying
locations >= 14 (MAX_VERTEX_OUTPUT_COMPONENTS 64). d3dgl_relocate compacts a pair's varyings when any location
>= PELOAD_D3DGL_VARYING_SLOTS (14). Link failures are only printed with PELOAD_TRACE_D3DGL -- check that first when
something "doesn't render". `PELOAD_TRACE_HASH=<vs/ps hash,..>` logs those draws' state, input layout, buffers,
CPU-projected instance corners and samples passed (`PELOAD_TRACE_HASH_NODEPTH=1` draws them without depth test).
Shader hashes by name: dump with PELOAD_DUMP_SHADERS and grep the DXBC RDEF strings (e.g. "WaterPS").

Client audio (2026-10-16): Reclaimer's own menu music / voice playback use cpal (WASAPI). shim/src/wasapi.c is a real
render endpoint (IMMDeviceEnumerator/IMMDevice/IMMEndpoint/IPropertyStore/IAudioClient/IAudioRenderClient/IAudioClock)
with one AudioQueue per stream; no capture device. `PELOAD_NO_WASAPI` = old no-device stub, `PELOAD_TRACE_WASAPI` logs
(incl. per-interface AddRef/Release). Without a mic the voice system retries every ~5 s and leaves each playback stream
with 1 reference: once only that is left (svc refs 0) the AudioQueue is closed (PELOAD_WASAPI_KEEP_IDLE = keep).
Teleport (PELOAD_ALLOW_TELEPORT; stats page "Teleport next to them" = POST /control?do=tpnext&slot=N, F7 = opposite
side): shim/docs/teleport.md. halo3+0x259410 (object, pos*, fwd*, up*, 1) run from hb_engine_jobs in an entry hook on
main_loop_body (halo3+0xb2250, engine main thread, before the frame's ticks -- where the Reclaimer client teleports)
inside the game-state lock (halo3+0x12c948 / 0x12c9a0), host only (game_globals+0x11 == 5, or 1 local; a client's body
is snapped back by the host), unit_exit_vehicle (halo3+0x3a3c4c) first, destination checked with
scenario_location_from_point (halo3+0x9bc78) at +0.3 and +1.2, |coord| < 30000 (past 32768 the host deletes the unit).
The engine marks the object for network update itself. The old hook halo3+0x9d22c is a ms timer called from 421
places (many mid-tick) -- that is why teleports crashed. Test: target.sh as host with TARGET_STATS_HTTP=8766
TARGET_ENV=PELOAD_ALLOW_TELEPORT=1, a quiet-run joiner (visible window: a hidden joiner crashed once in Apple's GL
driver on the present thread), POST tpnext on 8766, compare positions on 8766 and 8765.
Tested 2026-10-17: a client-side teleport (PELOAD_TELEPORT_CLIENT_TEST=1) shows on the client for <1 s and the host
never accepts it (server-authoritative, no bug). Client requests are forwarded to the host's loader instead
(PELOAD_TELEPORT_HOST=<host stats addr>; POST tpnext&slot=<target>&who=<requester>): the host moves the requester,
both views agree within 0.5 s.
WASAPI crash fixed 2026-10-17: "Experimental startup exception 0xC000001D at unmapped module" (rbx/rdi in
caulk::alloc::make_audio_buffer_resource) = an AudioQueue callback re-enqueueing a buffer while another thread
disposed the queue (the client's 5 s voice retries abandon streams; wasapi.c closes their queues). Now a closing flag
under the stream lock, the enqueue inside the lock, dispose outside it (wa_close_queue). Tip: identify a system address
from a crash report with a tiny x86_64 dladdr helper -- the shared cache is at the same address in every process.

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
