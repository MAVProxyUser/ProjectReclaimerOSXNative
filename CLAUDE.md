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
`PELOAD_SKINGUARD=1` installs the null-key guard on halo3's input-layout builder (entry detour on halo3+0x2ac24c; first misread as an armor-skin registry);
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
/control?do=... + header X-Reclaimer-Control (CSRF guard; PELOAD_STATS_READONLY=1 disables) flip the overlay switches
(g_hb_toggle bits). do=capture (the page's Capture frame; hb_capture_tick, render-thread Present):
~/reclaimer_cap_<pid>_<n>.bmp (the game backbuffer -- the overlay is drawn later, at the window blit, so it is NOT in
it), _depth.bmp/.f32 (the camera draw's DSV, rows top-first), .txt (camera, view-proj, depth stats).
Event feed + chat (2026-10-17, stats.c st_event ring, GET /feed.json?since=<seq>, snapshot feed_seq; PELOAD_FEED_LOG=1
prints events): the client's log lines are tapped in NtWriteFile (Rust's std writes stdio there, NOT WriteFile) +
WriteFile/WriteConsoleW (st_feed_log: "Community roster: X joined/left the server", votes, shuffles, match start/end).
Chat = entry hook on the client's TextChat::push_history (exe+0xb89930 in 0.9.18, by signature, st_chat_install): every
chat / [SERVER] / notice line, UTF-8 Rust Strings in a 0x40-byte Line (shim/docs/halo3-event-feed.md section 1). Kills
are ESTIMATED from the death tracker + object_delete shots (g_lasthit; weapon in hand / seat object / grenade) until a
halo3 game-engine event hook exists (same doc). The roster notices go to halo3 chud_messaging_post (halo3+0x2d3f2c;
its prologue is NOT position-independent).
Forensic archive (2026-10-18, shim/src/forensic.c + forensic_page.html at /forensics; shim/docs/forensics.md): every voice packet
voice_recv gets (st_on_voice now takes payload r8 / len r9; before the client's mute/channel checks), every st_event (chat with
sender peer id via t_st_chat_id) and roster changes (fx_roster_tick in st_snapshot) -> ~/Library/Application Support/ReclaimerMac/
forensics/<time>_<map>/{voice.bin,roster.jsonl,events.jsonl}; hooks only queue (g_fx_on is the off check), shim-forensic writes 1/s.
The SERVER relays voice and filters it (exe+0xb25d30: team = same team, proximity = alive within 10 world units; exe+0x684da5 in voice_recv): a client gets only what it is sent, no client-side filter precedes the hook; as host the hook sees everything (forensics.md). Opus bytes ONLY with voice audio on, which needs the page's one-party-consent box (settings.json); utt/<n>.ogg + speaker/<id>.ogg
are Ogg Opus built on the fly. PELOAD_FORENSIC=0 off. Offline test: bash shim/tools/forensic-oggtest.sh (ffmpeg + python3).
Engine events (2026-10-17, st_events_install): entry hook on display_event halo3+0x11b0ec (event* in rcx, 0x2c bytes:
type/index/cause/effect handles+teams, damage byte +0x24 = reporting type | special<<6, +0x25 bit0 = kill info) gives
exact kills; chud_messaging_post is hooked through entry_hook_tail (a custom replay tail for its rip-relative prologue;
ret_r9 passes the caller's return address: halo3+0x11b860 = an event line, exe = the client's roster line). A CLIENT
only receives kills involving its team: all in 2-team games, only its own in FFA and Infection (every player has its
own team number there). Estimated deaths wait 2 s for an engine event before going out. PELOAD_FEED_RAW=1 prints every
raw event; PELOAD_NO_EVHOOK off. Damage type 0 with a killer = an unnamed (modded) weapon.
Microphone (2026-10-17, wasapi.c): the Mac mic is the client's WASAPI capture endpoint (AudioQueueNewInput ->
IAudioCaptureClient); with a mic the client's voice chat starts once instead of rebuilding a playback stream every 5 s
(the caulk crash trigger). PELOAD_MIC=0 a SILENT mic (also the fallback when the real one can't open), =none no device (the
crash-prone old 0), =tone a test tone (quiet-run uses 0 unless QR_MIC=tone; target.sh 0),
PELOAD_MIC_MUTED=1; stats do=mic&on=. The client sends voice only with its talk key (Hold/Toggle to talk).
Player slots (2026-10-18, backlog 53; stats.c st_slots_install / st_max_players): the joined server's max players = u8 at Arc+off in the client's Mutex<Option<Arc<HostedSession>>> (signature on its max_players() getter; 0.9.18 exe+0x27dd668, +0x22d; Arc+0x10 == 0x8000000000000000 = we host -> halo3 session sess+0x419c if sess+0x4194 bit 0); snapshot "slots".
Spawns (2026-10-18, backlog 47; stats.c st_spawns): events kind weapon / vehicle / powerup with "pos" (object +0x50), "<x> spawned
at (x, y)"; an object counts after two parentless snapshots; vehicles without attached turrets, the deployed auto turret and
security cameras; power-ups = objects\multi\powerups\powerup_red/blue/yellow (Overshield / Active camo / Custom), pickups from the
player's pickup ticks +0x218/+0x21C/+0x220 (unverified live); feed filters + forensic lanes; PELOAD_SPAWN_LOG=1; offline
shim/tools/spawn-test.sh. Live (local The Pit): 41 weapons in one summary, Warthogs, turrets, Overshield, Active camo.
Auto-vote (2026-10-18, stats.c st_av_*): PELOAD_AUTOVOTE=yes|no presses F5/F6 (client keybindings vote_yes/vote_no, WM_KEYDOWN ->
exe+0x3bec80 -> answer_vote exe+0x87f840; menu runtime only; one answer per vote) after "... called a vote to ..."; the lobby map
ballot has no key: PELOAD_AUTOVOTE_MAP=1|2|3|none clicks cards at (640, 268/348/428/484) -- did NOT register on a joined client
(the loading splash covers the lobby during the 5 s ballot); do=autovote. Chat text typed by "t:" steps did not send (open
question). Zones never draw without a complete depth copy (a frame meeting the readback mid-update drew everything: the
push-back flash). Workshop helper: Progress every 2 s from bytes on disk (the client's 45 s stall limit, workshop-helper.md).
Clip recording (2026-10-18, backlog 42; shim/docs/recording.md): stats page Record row (do=rec&secs=&mode=av|audio, do=recstop,
GET /recordings.json + /recordings/<file>, snapshot "rec") -> ~/Movies/Reclaimer/<map>_<time>.mp4 (H.264 + AAC) or .m4a. shim/src/rec.m
(ObjC, MRR, built beside peload.c; rec.h): the present paths' REC_FRAME() blits the window back buffer's picture rect into
CVOpenGLTextureCache/IOSurface pixel buffers -> AVAssetWriter (hardware H.264, no B-frames: they broke fragmented finishes); xa_aq_cb /
wa_aq_cb tap rec_audio_push into a 48 kHz timeline ring. Off = g_rec_video / g_rec_audio flag tests. Offline test: shim/tools/rec-test.sh.
Stats page (2026-10-17, backlog 29-31): Kills (st_kill_at counts every kill event per match by name; betrayals/suicides
excluded) + K/D beside Deaths; every column sortable (team sort / full sort, remembered in localStorage); the map opens in the
whole window (click it or "full window"; X / Esc closes; the canvas is resized and redrawn); a "vehicles" map layer; overlay
switch HBF_VEH = the Vehicles button (do=vehicles; PELOAD_HB_VEHICLES=0 starts off; bit 6 of g_hb_toggle).
Overlay keys F2-F10 are OFF since 2026-10-17 (PELOAD_HB_FKEYS=1 brings them back): the stats page has every switch. Zones are
drawn by hbz_gpu (translucent surfaces + edges, static VBO per map, per-pixel occlusion against the hb_vis depth readback as a
texture; kinds g_hbz_show bits 1 kill, 2 safe, 4 acceleration, 8 soft kill, 16 slip -- page chips, do=zonetype&t=). Helper-mode
present: a frame that doesn't come within 50 ms re-presents the last IOSurface (no in-process fallback; PELOAD_GLH_PQ_FALLBACK=1 old).
Zones overlay (page button / PELOAD_HB_ZONES, hitbox.c hbz_*): kill/safe trigger volumes (scnr +0x258/+0x484/+0x490) and soft
ceilings (sddt) read from the map being played (hb_current_map: halo3/maps or a Workshop folder). Pixels outside the
depth readback's view (2 frames old: the strip turning into view, behind its camera) are hidden, not drawn through walls
(2026-10-18). PELOAD_HB_ZONES_XRAY=1 = the old lines through walls (hbz_draw, hb_seg_clip). Host speed control: stats do=speed&v=<tl_speed index> writes the variant's base player speed trait.
The map being played (2026-10-18, backlog 44): hb_current_map (hitbox.c) = the recently opened map (halo3/maps or a Workshop folder)
whose tag table matches the most live objects' definition datums (index -> salt), rechecked every 2 s -- NOT the newest .map
opened (the client opens maps it borrows skins / tags from around a change). Heatmap, map picture, walker graph, zones, tag
names and hitbox geometry all follow it; g_hb_mapgen resets the per-definition caches. 32 recent paths, 48 open map files.
Layered windows (2026-10-17): UpdateLayeredWindow stores each window's bitmap/position/alpha and ShowWindow its
visibility; lay_draw draws them over the frame at the window blit. The client falls back to layered windows whenever
the game stops presenting for >1 s ("Client screens now shown as layered windows"); before this its loading screen
vanished during heavy loads (backlog 25). PELOAD_NO_LAYERED = old no-op.
Patch points across releases: shim/docs/patch-addresses.md (all 34 client releases surveyed statically, downloads in
~/Library/Application Support/ReclaimerMac/releases/<tag>/; never run them). No fixed exe RVA survives a release; a
unique signature hit can still be the wrong function -- validate by meaning (st_voice_install refuses a set_muted hit
whose lock byte no is_muted function takes). Speed-step demo for the developers: shim/tools/speedstep-demo.sh (local
dedicated server only).
Workshop helper (2026-10-17): CreateProcessW runs shim/tools/workshop-helper.sh (DepotDownloader, account name from
STEAM_USER or ~/Library/Application Support/ReclaimerMac/steam_user, never a password) in place of the client's
`workshop-download --subscribe|--update <id>`, on the client's pipes, own process group (shim/docs/workshop-helper.md;
PELOAD_NO_WORKSHOP_HELPER = old). Shim fixes for it: ReadFileEx on sockets/pipes reads sequentially (pread -> ESPIPE;
PELOAD_READEX_PREAD = old), TerminateProcess(child handle) kills the child (+ its group) instead of exiting the loader
(PELOAD_TERMINATE_SELF = old). PELOAD_TRACE_PIPEIO=1 logs pipe/socket reads. The client finds Workshop mods by the id in
each folder's ModInfo.json (any folder name), and a mod folder with a file missing is a verify error, not a download.
Heatmap per map: footprint cells (0.5 u) + deaths, ~/Library/Application Support/ReclaimerMac/heatmaps/<map>.bin (RHM2),
drawn over a top-down picture of the map: re-tools/h3bsp.py decodes the collision BSP + instanced geometry from the
.map's resource section (sbsp +0x38C -> zone structure_bsp_tag_resources -> raw-deflate page; shim/docs/
h3-map-geometry.md) into maptop/<map>.png + .json (world bounds); shim/tools/maptop.sh makes all 27 (needs python3 +
numpy + Pillow); served as /maptop/<map>.png|json. (re-tools/h3map.py's string-id decoding is wrong for these builds;
h3bsp.sid_name is right.)
Overlay "visible only": hb_vis_tick blits the camera draw's depth (D32F_S8) down to 480x270
and reads it back through a 3-PBO ring (2 frames late, no stall); hb_vis keeps an object when the scene at its centre /
top / bottom is not nearer than its front (centre moved toward the camera by radius + 0.3).
Overlay panel (2026-10-18, backlog 45): team filter, visible only and label px are PER FEATURE (hitbox.c g_hbs[HBF_*], -1 = n/a:
items/zones no team, zones always per pixel, px only info/items (+ zones' labels in XRAY mode)); vehicles' team = occupants' (bipeds
whose +0x10 parent is it; empty = "all" only), tracers' = the shooter's (+0xC0 owner). POST /control?do=feat&f=<a,b|all>[&on=][&team=]
[&vis=][&px=4..60] (stats.c st_feat -> hb_feat_set); snapshot overlay.features (null = n/a). The old globals (PELOAD_HB_TEAMS /
_VISIBLE / _TEXT_PX, do=teams / visible / textup|textdown, F6 / F9) are "apply to all" (hbs_sync notices a change). The readback runs
only while an ENABLED feature has vis on, or zones are on. Offline: shim/tools/overlay-feat-test.sh (the real C, extracted),
shim/tools/overlay-panel-mock.py (the page against a fake stats server; never port 8765).
Frame rate (overnight 2026-10-17; details + numbers in shim/docs/PERF-PLAN.md "Overnight"): 100-player Infection
15-24 -> 37-48 fps. Particle compute (dxbc_cpu.c) is a SIMT executor (64 lanes, active lanes only; PELOAD_CS_SCALAR = old,
PELOAD_CS_CHECK compares both byte for byte) on PELOAD_CS_WORKERS (4) dependency-ordered workers with read-only inputs
snapshotted once per resource version (CsSnap). GL submission thread = shim/src/d3dsubmit.c (PELOAD_D3DGL_SUBMIT=1/0/auto;
play.sh: auto = on at 40+ session players; switchable at run time at a Present: stats control do=submit&on=, in-game A/B
PELOAD_AB=submit|memcpy|glcache|cbbatch:<secs> logs [ab] per phase -- the way to compare anything in a live game, since
scenes vary too much between runs). Under it: Map(READ,DO_NOT_WAIT) never drains (WAS_STILL_DRAWING), Map(WRITE) renames
blocks, queries are harvested every 128 commands, 1 frame of lead. Diagnostics: PELOAD_SAMPLE (per-thread module/function
breakdown, callers of memmove/memcpy, generated-code pools named), PELOAD_GLCOUNT (GL calls per 2 s), PELOAD_D3DGL_PROF
[submit] line (GL-thread waits for work/compute/GL lock, compute input bytes), PELOAD_CALLCOUNT=<thread name> (Win32 calls
per thread), PELOAD_TRACE_WOA=<thread>. tools/glbench.sh: Apple's GL driver is ~1.5x faster native than under Rosetta --
the native GL helper (PERF-PLAN option 1) is the next big step for BTB-sized games, where the GL thread (~6.3 us/draw)
is the limit; the GPU is 15-27% busy. Not worth it (A/B neutral): the GL state cache (PELOAD_D3DGL_GLCACHE=1, 31 -> 18
calls/draw), batched CB maps. mach_absolute_time is NANOSECONDS under Rosetta (timebase 1/1): convert with mt_us().
Busy-poll damper (obsolete since the NtDelayExecution fix, 2026-10-18: mcc-voice now polls ~260/s): the client's mcc-voice thread polled GetAsyncKeyState ~500k/s; PELOAD_POLL_DAMP=1 (OPT-IN: the
key poll holds a lock the main thread also takes -- sleeping there made sticks sluggish and movement choppy).
Big guest memcpy/memset (>= 2 KB) run on the host's (PELOAD_GUEST_MEMCPY=1 = old). Trace stubs (PELOAD_TRACE_ALL,
CALLCOUNT) are RWX-permanent and keep xmm0-3; never trace halo3's ws2 imports (the client checks them: "Native transport
instruction/import mismatch"). The texture-bind notes: texs = Apple's driver revalidating on binds; the client's Advanced
graphics settings (Shadows, LOD, draw distance) cut draws.
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
play.sh's stdin feeder is `sleep 86400.<pid>` (killed when the game exits; it used to hold a caller's `| tail` for 24 h).
Native GL helper (2026-10-17, PELOAD_GL_HELPER=1: play.sh's default, the code's default without the env is off; spawned with
only stdio + its pipe; shim/src/glh_client.c + glhelper.c + glh_shm.h, generated glh_gen.inc /
glh_dec.inc from shim/tools/glh_gen.py, which build.sh reruns): peload.c redefines dlsym (after the D3DGL globals) so every GL name
resolves to a router that encodes the call into a shared-memory ring while the render context (g_d3dgl_cgl, a dummy in helper mode) is
current, else calls the real function. The helper (native arm64, its own GL context) replays them. Names come from pools it keeps filled;
Get*/link-time calls are synchronous round trips; GetError -> 0 and CheckFramebufferStatus -> complete without a round trip
(PELOAD_GLH_SYNCERR/_SYNCFBO); occlusion queries come back through a result table; pq_publish -> GLH_X_PQ_BLIT into 3 global IOSurfaces,
the present thread samples them as rectangle textures. Every pointer argument needs a size rule in glh_gen.py (it refuses to generate
otherwise: an unencoded pointer crashed the helper). Debug: PELOAD_GLH_TRACE=<n> (first n commands), PELOAD_GLH_STATS=1, crash reports
in /Library/Logs/DiagnosticReports/glhelper-*. The depth readback (visible only, zones) goes through glh_rb_request (one ticket in flight). The helper's write end of the wake pipe sits at fd >= 4000
(the game's CloseHandle once closed a low one).
D3D object lifetime (2026-10-17, backlog 27): refcounted D3DObjs, freed via d3dobj_stage/d3dobj_reap; since 2026-10-18 05:00 also DUPLICATE VS/PS (o->canon set: bc/glsl shared with the canon, o->bcshared, keep=0; destroy deletes shc_id GL shaders + glprog; PELOAD_D3D_KEEPDUPS=1 = old) -- the [d3dobj] line (PELOAD_D3DOBJ_LOG) counts what stays held (canon shaders, input layouts). Never free a canon shader (programs key on it) nor a glsl on compile failure (duplicates share it) -- ON by default again since 2026-10-18 00:35 (PELOAD_D3D_FREE=0 / PELOAD_D3D_NOFREE=1 = never free; it was off 2026-10-17 21:15 after map-change corruption in a public BTB, before two use-after-free fixes); copy-on-write (PELOAD_MEMCPY_COW, =0 off) on again too: a 40-minute BTB soak with both (85 players, 3 maps, 21.8 GB freed) was clean. Map-change test server: DED_CONFIG=mapchange.toml bash shim/dedicated.sh (3 x 1-minute games).
NtDelayExecution (2026-10-18, backlog 51): the client believes it runs under Wine (GetProcAddress(ntdll,"wine_get_version") returns a stub) and then sleeps with ntdll NtDelayExecution -- it was missing (no-op stub), so its voice worker spun ~210k loops/s (server: a voice control flood; client: mcc-voice ate a core polling GetAsyncKeyState ~500k/s) and its frame limiter / runtime pacing (exe+0xa398e0) never slept. Implemented (PELOAD_NTDELAY_STUB=1 = old; in-match A/B PELOAD_AB=ntdelay, 128 players: +11.8% median fps). GetProcAddress hands a logging stub to ANY unknown name: grep logs for "[stub] (dynamic)!" when a client feature misbehaves. Stubs the client called (2026-10-18 sweep of all logs): NtDelayExecution, MapVirtualKeyExW (key <-> scan code <-> character; wininput.c, from the Mac key table), LCMapStringEx (case mapping), RoGetActivationFactory (now REGDB_E_CLASSNOTREG) -- implemented; FindWindowW (0 = no window, fine), XAudio2 ordinals #5/#6 (X3DAudio: only stubbed with PELOAD_AUDIO=0).
Big games (2026-10-18, PERF-PLAN "80-100 player games"): the limit is halo3's MAIN thread (guest-0x18000a120, 99% under
Rosetta: halo3 code 55-60%, client hooks 8-10%, host memmove ~20% = its ~14 MB game-state copies ~5 a frame, malloc), not the
GL helper (0 ms waited in the slowest stretches). BTB: Valhalla median 63 fps / p10 39, Highground 50 / 28. COW in-match A/B
+4%. PELOAD_PAR_MEMCPY=<helpers> copies big (>= 2 MB) non-overlapping guest copies in 1 MB chunks on helper threads
(shim-copy; Rosetta bench 5 x 14 MB 4.0 -> 2.0 ms; PELOAD_PAR_CHECK=1 verifies: 6,145 copies, 0 mismatches; PELOAD_AB=parcopy) --
BTB A/B: COW (default) beats it by 7.7%, so it stays opt-in.
Import fast paths (2026-10-18, backlog 11/41; shim/src/fastcall.c, PERF-PLAN "Import fast paths"): PELOAD_FASTCALL=all|tid,heap,keys In-match A/B (128 players, stationary, 32 pairs): +1.3% median -- opt-in.
(OPT-IN until an in-match A/B: PELOAD_AB=fastcall:<secs>, run-time safe). An ms_abi import that calls ANY SysV function -- or reads
a __thread variable, or gets a SysV ".cold.N" part from Apple clang's hot/cold splitting -- saves xmm6-15 on EVERY call (~3 ns
under Rosetta; a plain call is ~1 ns), so fast paths keep per-thread state in the TEB (+0x1FF0 tid, +0x1FF8 heap cache) and are
noinline. heap = per-thread small-block cache (<= 1 KB, malloc's 16-byte classes) from a 2 GB arena: alloc+free 27-76 -> ~5 ns;
tid 4 -> 2 ns; GetAsyncKeyState 4.5 -> 1.3 ns. Expected ~1.5-3% of the main thread. Offline bench + checks: bash shim/tools/callcost.sh.
GetSystemInfo said ONE processor + allocation granularity 0 (the client sizes ~10 thread pools from the count; halo3's
locks at halo3+0x620170.. spin before blocking only when it is > 1 and wake at most that many waiters):
Since 2026-10-18 it reports Windows' values by default (hw.logicalcpu or PELOAD_NCPU; in-match A/B of halo3's count via
PELOAD_AB=ncpu: +1.7%); PELOAD_SYSINFO=old = 1 processor / granularity 0.
Processes and cores (README "How it runs"): the LOADER (x86-64 under Rosetta: client exe + halo3.dll, Win32, D3D11->GL encode,
audio, input; can never be native, the game code is x86-64) and the GL HELPER (native arm64, Apple's GL driver, 256 MB command
ring + 3 IOSurfaces). Loader threads: halo3 main (the limit), halo3 render, shim-gl-submit (40+ players), shim-compute x N
(play.sh: P-cores minus 2, 4..8; code default 4), shim-present, shim-copy (opt-in), shim-stats-*, shim-forensic, shim-nav.
Diagnostics: PELOAD_GETENV_STATS (real environment lookups per site: every PELOAD_* is cached by peload_cgetenv, the rest is
~20-260/s), PELOAD_HEAP_STATS=<thread> (HeapAlloc sizes), PELOAD_CALLCOUNT=guest-0x18000a120 (Win32 calls of the main thread:
~3.5 M/s in a BTB: GetCurrentThreadId (now cached per thread), GetProcessHeap, memset, HeapAlloc/Free, memcpy).
Walking with the map (2026-10-17, backlog 32): PELOAD_NAV=roam|enemy|x,y,z|vehicle:<name> (vehicle: walk up to the nearest vehicle whose tag name contains <name>, stop beside it and look at it -- for overlay checks) drives pad 0 along A* paths of a floor graph
(re-tools/h3nav.py -> maptop/<map>.nav, made in the background on first play; shim/src/nav.c, own thread shim-nav;
PELOAD_NAV_LOG). Use it for unattended test movement instead of PELOAD_INPUT_SCRIPT (needs QR_PAD=1). With the sparring partner
(TARGET_ENV="PELOAD_SPAR=1 PELOAD_NAV=enemy") it also walks: nav's left stick + jump, the aim keeps the right stick; within
PELOAD_NAV_NEAR (4) units it strafes side to side. The stats page map draws the walker's path and goal (snapshot "nav").
Death hotspots (nav_danger_update): danger is RELATIVE per map since 2026-10-18 (0.5 at the PELOAD_NAV_HOT quantile, default 0.9, of
the per-node death sums: ~10% of the floor; a fixed scale had made 59% of Valhalla "hot" from 2,393 stored deaths); with
PELOAD_NAV_LOG each path line also gives its hotspot nodes vs the same A* without avoidance (public BTB Standoff, 28 plans: 109 vs
352). The walking graph / map picture are only MADE once gameplay started (the menu borrows other maps). Vehicle weak spots
(backlog 31/36): re-tools/h3dmg.py <map> prints each vehicle's damage sections / seats (--table <maps...> = every vehicle, the table in
halo3-objects.md: 178 definitions in stock + Workshop maps, 60 with weak spots); the overlay colors lethal sections yellow,
driver-exposing ones orange, gun/steering-disabling ones (< half the body, response flag bits 4, 8-15) magenta (hitbox.c hbg_weak,
PELOAD_HB_WEAK_SHOW mask 1/2/4, default 7; Wraith back hatch = 75 of 750 kills it). Live-checked on Avalanche. String ids:
re-tools/h3bsp.sid_name (19-bit index, namespace at bit 19) -- h3map.H3Map.sid mis-resolves namespace-0 continuations.
Map pictures (2026-10-17): hb_note_map only keeps real cache maps ("daeh" v13; the engine's autosaves are also .map files); the stats
server makes a missing maptop picture in the background the first time a map is played (st_maptop_ensure: arch -arm64 python3
re-tools/h3bsp.py; PELOAD_MAPTOP_TOOL, PELOAD_NO_MAPTOP); maptop.sh also covers Workshop maps (and takes map files as arguments:
workshop-helper.sh runs it on a finished DepotDownloader item); h3bsp.py reads editing-kit (H3EK) maps,
whose play tag is empty: the layout tables are in the zone tag (+0x1C ext, +0x34 pages, +0x58 segments) and page offsets are file offsets.
Focus and fps (2026-10-17): the engine never sees the Mac window's focus -- the loader always reports its game window as the
foreground/active window -- so there is no unfocused 60 fps cap; the "60 fps (screen max; engine N)" title only labels the screen
rate. Hidden vs shown measured in ONE match (PELOAD_AB=hidewin:30, orderOut/orderFrontRegardless, never activates): ~100 vs ~100
fps. Compare runs only inside one match: two runs of the same local server differed 110 vs 142 by spawn/scene alone.
PELOAD_MAX_FPS caps a client (default 30 when PELOAD_HIDE_WINDOW).
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
variable + a stats page button (bits of g_hb_toggle; the old F2-F10 keys only with PELOAD_HB_FKEYS=1):
`PELOAD_HITBOXES=1` hitboxes (NEVER a stand-in sphere / ring shape: the user hates it -- objects without a collision model get nothing), `PELOAD_HB_TRACERS` projectile trails + bullet lines,
`PELOAD_HB_INFO` shield/health/"<weapon in hand> / <other weapon>"/grenades/camo/vehicle labels, `PELOAD_HB_ITEMS` weapon/equipment
labels, `PELOAD_HB_TEAMS=all|enemies|friends` team filter (enemies red/yellow head, teammates blue), `PELOAD_HB_VEHICLES`
(weak spots yellow/orange/magenta, chips do=weaktype&t=1|2|4). Objects come from the
"object" data array (TLS index halo3+0xa39f9c -> TLS block +0x38). Real hitboxes = the object's COLLISION MODEL read
from the .map file the game opened (hb_note_map in CreateFileW; bipd/vehi +0x34 model -> hlmt +0x10 coll ->
regions/permutations/bsps; all permutations cached, each object picks its own via its region permutation bytes at
datum+0x13A -- Elite helmets), each edge posed by the node matrices (datum+0x136). Bullets live < 1 tick, so an
entry hook on halo3 object_delete (halo3+0x342ed8, hbdel_install, prologue-checked; PELOAD_NO_DELHOOK) records
projectile datums at deletion (start +0x1e8, impact +0x1c). Text = a stroke font in the same GL_LINES batch, tag
names from the map file. Drawn in the present-queue blit (pq_present_wait -- NOT d3dgl_blit_fullscreen_i, which
only runs without the present queue). Camera = VS cb0 rows 0-3 + row 7 of the first draw passing
hb_is_world_camera. Diagnostics: `PELOAD_HITBOXES_LOG` (counts, why an object has no hitboxes, permutation bytes),
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

GPU particles (2026-10-17, OFF, PELOAD_CS_GPU=1; PERF-PLAN "GPU particle compute"): own-element kernels as transform-feedback
vertex shaders (cstf_* in peload.c, dxbc2glsl g_dg_tf_dw), spawn kernels on the CPU with a write log; correct
(PELOAD_CS_GPUCHECK=<n>) but slower on Apple's GL. Apple GL-on-Metal: no glCopyBufferSubData (silently nothing), no ordering of a
TF write before a read in the same render pass (own FBO needed), draws need a complete FBO even with RASTERIZER_DISCARD.
PELOAD_AB=csgpu|hidewin:<secs> A/B them. CPU interpreter: 4-component immediates were broadcast from lane x until 2026-10-17
(PELOAD_CS_IMMBCAST=1 = old). Stats page: play.sh / quiet-run open it in the browser unless a tab reconnects within 5 s
(PELOAD_STATS_OPEN, _WAIT; an open tab reloads itself for a new game via the snapshot's "inst").
Faults on loader threads (2026-10-17, backlog 19): only threads with a TEB (main + CreateThread, __thread g_guest_thr) hand a
hardware fault to the game's exception handlers; a loader thread's fault is reported as "[CRASH] fault on loader thread '<name>'"
with the loader function (dladdr). Before, the game's handler (Rust) ran there and faulted on gs:[0x58]=0 (exe+0x1d6455d,
eprintln) or the crash handler faulted, hiding the real fault. PELOAD_SEH_ALL_THREADS=1 = old; PELOAD_TEST_LOADER_FAULT=<secs> tests it.
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
Sparring partner (2026-10-17): `TARGET_ENV="PELOAD_SPAR=1" TARGET_STATS_HTTP=8766 bash shim/target.sh` -- hitbox.c hb_spar_tick
(every Present) picks the nearest live enemy, XInputGetState pad 0 reports only a right stick toward its head (aim only);
stats page "Sparring aim" toggle (do=spar&on=). hb_collect has a units-only mode for it (the overlay switches otherwise
decide what it gathers). Map boundaries tested live: host with PELOAD_PATCH=halo3+37b816:E95501000090,halo3+37b734:EB
(the entry patch of halo3+1b8e00 is refused: the client checks that function's entry bytes), host-only teleport to a
point POST /control?do=tpto&x=&y=&z=[&slot=] (shim/docs/map-boundaries.md); stats page buttons "Boundaries on/off"
(do=bounds, patches written at run time at the frame safe point) and per-map spots (do=explore&spot=, g_st_spots: Valhalla east cliff). Speed-step test: the host adopts client
positions within 2.5 units with no speed check (shim/docs/net-authority.md).
Teleport (PELOAD_ALLOW_TELEPORT; stats page "Teleport next to them" = POST /control?do=tpnext&slot=N, the Teleport button = opposite
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
never accepts it (server-authoritative, no bug). Same against our local Reclaimer DEDICATED server (what public BTB
servers run; anti-cheat at its default, one client): POST tpby&dx=10 moved the client's own view to x+10 for ~1.5 s,
the server's stats (DED_ENV=PELOAD_STATS_HTTP=8767) never moved, the client snapped back -- nothing to report. Client requests are forwarded to the host's loader instead
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
- `shim/docs/easyanticheat-eos.md` — static analysis of MCC's Easy Anti-Cheat (EOS) service: upload-only telemetry
  (2 events), crash-dump commands, kernel-driver load via the world-writable `Global\EasyAntiCheat_EOSBin` section.
  Reclaimer never loads MCC-Win64-Shipping.exe / EOSSDK, so none of it runs under the loader.
- `shim/docs/patch-addresses.md` — every patch/hook/call/data address in halo3.dll and the client, with signatures
  (section 8) and how to re-find them after an MCC update (section 9).
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
