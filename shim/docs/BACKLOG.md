# Backlog — requests from the user, and their status

Kept current as work lands. "Done" items name the commit that delivered them. Open items are in the
order they will be worked; frame rate is deliberately last (user, 2026-10-16).

## Open

| # | Item | Status / notes |
|---|---|---|
| 13 | **Stats page: mute a player** (user, 2026-10-17) | The client has per-player "MUTE VOICE" in its scoreboard/player details; wiring it to the web page needs its mute toggle found and called on the right thread. |
| 14 | **Join Workshop-only servers (e.g. CHAOS 128P)** (user, 2026-10-17) | Mods there are Workshop-only ("Direct sharing is disabled"). DepotDownloader -pubfile (Ultimate Forge 2.0 = 2933266434, Permafrost Beta = 3663541185, Hugegrass = 2984061723) into steamapps/workshop/content/976730/<id>; needs the user's Steam account name for the remembered login and their OK per download. |
| 6 | **Seams at polygon joins on outer map geometry** (e.g. Valhalla rock walls) | Checked (2026-10-16): the world is drawn reverse-Z into a 32-bit float depth buffer (DXGI 19), ~835 depth-writing GEQUAL draws + ~625 GEQUAL no-write passes over them per frame (PELOAD_D3DGL_DFCENSUS). Two suspects: (a) a later pass computing a position 1 ulp off fails GEQUAL along edges -- `PELOAD_D3DGL_INVARIANT=1` declares gl_Position invariant (no fps loss seen below the 60 fps unfocused limit); needs a look by the user at the rock walls with and without it; (b) GL 4.1 has no clip control, so the D3D [0,1] -> GL [-1,1] depth mapping costs ~5000x precision on far geometry (z-fighting at distance; not fixable on this GL). |
| 7b | **Teleport (F7, PELOAD_ALLOW_TELEPORT) -- experimental, crashes** | Uses halo3's own teleport (halo3+0x259410). Run from an arbitrary QueryPerformanceCounter call it crashed in the physics update (halo3+0x23300E, mid-tick); run from an entry hook on the main loop's frame-timing function (halo3+0x9d22c) it crashed at halo3+0x1ECA6 reading TLS block +0x240 (null on that thread). Thread dumps show halo3 spreading game work over job-system worker threads, so the safe point is the simulation job's own tick -- not found yet. Destination snapping to a map item spawn works (computed correctly on Guardian and Valhalla). |
| 9 | **Unexplained crashes** (watch) | (a) v0.9.18 SIGTRAP during a direct Guardian match; (b) one startup SIGSEGV with guest code on a thread without TLS. Not seen again since the thread-id fix (55811fa); loader threads are now named for the next [CRASH] line. |
| 11 | **Frame rate** (last) | shim/docs/PERF-PLAN.md. Done: redundant glUseProgram cache (4afd0a9). Small wins left; big step: native arm64 GL helper process (up to ~2x). |

Closed by the user 2026-10-16: teleporter audio desync (not seen since the XAudio2 and thread-id fixes), class-selection input
(fine; only scripted tests lacked input), controller in menu-joined matches (caused by a second window), v0.9.13 checks
(old client no longer supported).

## Done (2026-10-16)

- Join public servers (remote join proven, then real BTB on v0.9.18) — 36c3c98, a574219
- Server Browser list, lobby names, map vote render (GL thunk xmm6-15) — f125b76
- Scoreboard text (foreground/activation rules); play.sh defaults to the menu — 5d425e5
- Second player to shoot at: shim/target.sh (input-less client hosting hidden Valhalla) — f4eb1e6, d858a8b
- Bigger window (largest 16:9 that fits the screen) — 1367a68
- Reclaimer client v0.9.18 support (signature patches, Bink/Waves DLL support) — a574219
- Game audio after joining a match in progress (user heard full audio in FKK BTB)
- Release "Project Reclaimer 0.9.18 for Mac" (tag reclaimer-0.9.18; releases now named for the client version;
  shim/tools/make-release.sh) and the USB stick installer (shim/tools/make-usb.sh + install.sh)
- Menu music and voice playback: a real WASAPI render endpoint on CoreAudio for the client's cpal audio
  (shim/src/wasapi.c); user confirmed menu music plays
- Overlay features (user: "these features all rock"): F3 projectile/bullet tracers, F4 player info, F5 item labels
  (8448a06); per-player collision permutations for Elite helmets, enemy/teammate colours, F6 team filter (635bc46);
  object layout documented in shim/docs/halo3-objects.md (d1054ea). Tested in a 70-player FKK BTB: every player drawn
  as a hitbox, no sphere fallbacks.
- Hitbox overlay: bounding spheres (f73252e), then the real hitboxes — each object's collision model posed on
  its live node matrices (76a80c8); F2 toggles it in-game (c1f12f8). Verified on Valhalla vehicles, a Guardian
  Spartan and a 60-player BTB.
- Title fps no longer claims more than the screen can show (loading screens spin at ~1000 presents/s) — f73252e
- Intermittent hang at match start: GetCurrentThreadId returned 0x1000 on every thread, which disabled every
  msvcp140 std::mutex — 55811fa
- Stats page: death heatmap (per map, kept across matches, over a "where players walk" footprint) and click-a-player
  details (deaths, lives, distance, top speed, projectiles fired, weapons, camo, vehicle seat, facing)
- Live game stats over HTTP (user request): PELOAD_STATS_HTTP=<port> -- /, /stats.json, /events (shim/src/stats.c);
  verified in a Valhalla match (names, teams, you, weapons, grenades, 18 vehicles, projectiles)
- 45 s startup watchdog, the gate found: it is the worker's "engine hang watchdog" (exe+0x793f40), armed only for
  time-limited runs -- RECLAIMER_LOCAL_RUNTIME unset (the direct-match probe: 15/45/240 s, "still running after N
  seconds") or "<role>:<seconds>" with seconds > 0. The menu runtime passes "menu:0" and never arms it; nothing on
  Windows "clears" it. Only RECLAIMER_DIRECT=1 needs the `watchdog` patch (a menu-mode client ran >45 s in a match
  without it)
- Headless dedicated server: works on the Mac (v0.9.18) -- shim/dedicated.sh starts it with a local-only config; a
  client joined, the match started (`ready: true, players: 1`). The old "waiting for the native game client" line is
  benign (real-MCC private bridge); the server waits in its lobby for min_players. The first join right after a server
  start can time out (handshake aborted); a retry works. Worker logs via the new PELOAD_STDERR_TEE/STDOUT_TEE
- Second test player without a window: shim/target.sh runs windowless (PELOAD_HIDE_WINDOW: no window, no Dock icon)
  with a 30 fps cap (PELOAD_MAX_FPS) and its menu music muted; quiet-run.sh QR_HIDE=1 does the same for test clients
- Minimized window stalled loading: not the minimizing -- the GetCurrentThreadId mutex bug (55811fa); a minimized test
  client now loads in 12.8 s
- Water renders: the water shader pair never linked (varying locations >= 14 on Apple GL) — 434db9b (user confirmed)
