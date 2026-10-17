# Backlog — requests from the user, and their status

Kept current as work lands. "Done" items name the commit that delivered them. Open items are in the
order they are being worked (2026-10-17: frame rate is now active, at the user's request).

## Open

| # | Item | Status / notes |
|---|---|---|
| 14 | **Join Workshop-only servers (e.g. CHAOS 128P)** (user, 2026-10-17) | Download path works (2026-10-17): DepotDownloader (brew cask) signed in by the user, `-pubfile 2933266434 -filelist` fetched the desert_sandbox subset (1.73 GB: map + the mod's own shared.map/campaign.map) into steamapps/workshop/content/976730/2933266434. The join itself is unverified: CHAOS (65.59.89.203:11774) did not answer that night. Guide: shim/docs/mods.md; busiest servers + their mods: shim/tools/servers.sh |
| 17 | **Sparring partner: auto-targeting only** (user, 2026-10-17) | The windowless second player (shim/target.sh) aims at the nearest enemy's head, with an on/off toggle on its stats page; tested by the user against it on a private server. After the frame-rate work. |
| 11 | **Frame rate** (active) | BTB (60 players): 15-25 fps, 3,000-4,700 draws/frame at ~9 us each; the biggest phase is texture/sampler binds in Apple's GL driver (not uploads, not compute; CLAUDE.md "Frame rate"). Done: getenv cache, mip-streaming upload skip, profiler counters. Next: PERF-PLAN.md option 1 (native GL helper process, up to ~2x); meanwhile the client's Advanced graphics settings cut draws. |

Tabled by the user 2026-10-17: rock-wall seams (backlog 6) -- a minor display-only glitch (seen over rocks during a
rocket blast); the F8 captures of the rendered frame never show it, and depth/precision/invariance were ruled out.

Closed by the user 2026-10-17: betrayal popup (backlog 15, findings kept in CLAUDE.md), unexplained crashes (backlog 9,
not seen in a long time).

Closed by the user 2026-10-16: teleporter audio desync (not seen since the XAudio2 and thread-id fixes), class-selection input
(fine; only scripted tests lacked input), controller in menu-joined matches (caused by a second window), v0.9.13 checks
(old client no longer supported).

## Done (2026-10-16)

- Every game option, exact (2026-10-17): shim/docs/option-map.json from the client's option table (639 keys, offsets,
  bits, choices); the stats page decodes every option of the running mode from the raw variant. Checked live on
  Slayer (all rounds/respawn/map/powerup/points values and the powerup trait sets match the game); halo3's -1 in the
  16-bit weapon/vehicle set fields shows as "Map default"
- Teleport from either end (2026-10-17): a client-side move is rejected by the host (tested on a listen host and on our
  local Reclaimer dedicated server -- the software public BTB servers run: shown ~1.5 s, never accepted, snapped back;
  nothing to report); a client's request is forwarded to the host's loader (PELOAD_TELEPORT_HOST), which moves the requester.
  Audio crash fixed on the way (AudioQueue callback vs dispose race, caulk SIGILL)
- Teleport next to a selected player (2026-10-17): host only, from an entry hook on main_loop_body (halo3+0xb2250,
  engine main thread, between ticks) inside halo3's game-state lock; leaves a vehicle first; destination beside the
  other player (right/left/behind/close behind) accepted only inside the map structure. Tested host + joiner on
  Valhalla: the host moved from (81.6,-147.4) to 1 unit beside the joiner, the joiner's stats showed the same position
  within 1 s; no crash. A client's request is refused (the host would snap it back). shim/docs/teleport.md. The old
  crashes: halo3+0x9d22c is a ms timer called mid-tick, not the top of the main loop
- Stats page: X, Y, Z per player, names + coordinates on the map, coordinates under the mouse; every game option of
  the running mode decoded from the raw variant with the client's option table (shim/docs/option-map.json)
- Stats page (2026-10-17, later): heatmap over a top-down picture of the map made from the .map file (re-tools/h3bsp.py,
  shim/tools/maptop.sh, all 27 MP maps); overlay "visible only" (F9; user: "worked perfectly") and Text -/+ (labels 7 px by default, were ~4x
  larger); speaker mute icons + Mute all, mute acting on press (clicks were lost to 10 Hz redraws), big Mute button in
  the player details; exact local player from halo3's player mapping; burst / rocket-impact captures removed (user)
- Stats page: lobby/session player list (works in the lobby), voice Mute / Mute all through the client's own mute list,
  talking highlight, details panel under the heatmap (2026-10-17, e400dae) -- backlog 13
- Game options for every mode documented from the client's option table (shim/docs/game-options.md)
- Stats page: game rules and traits (2026-10-17) -- the running variant (game_globals+0x1608: rounds, respawn, map,
  powerups, score/points, every trait set incl. the mode's role sets) and each player's active traits (player +0x1FC)
  with powerup / respawn-trait countdowns; layout found statically, shim/docs/halo3-variant-traits.md
- Stats page (2026-10-17): on by default in play.sh (http://127.0.0.1:8765/); In-game overlay buttons for F2-F8
  (POST /control); shots fired per player from the object_delete hook (every projectile, not just the slow ones) with
  estimated hits and accuracy; F8 frame + depth + camera capture for reporting glitches at a spot

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
- Audio stream leak: with no microphone the client's voice chat retries every ~5 s and each try left a playback
  stream (and its AudioQueue, ticking every 10 ms) alive with one leftover reference -- 720 an hour. An abandoned
  stream (only that reference left, render client and clock released) now closes its queue; Start reopens it
  (PELOAD_WASAPI_KEEP_IDLE = old). Busy-match test: 13 streams, 10 closed, live ones (menu music) untouched
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
