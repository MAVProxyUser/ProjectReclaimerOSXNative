# Backlog — requests from the user, and their status

Kept current as work lands. "Done" items name the commit that delivered them. Open items are in the
order they will be worked; frame rate is deliberately last (user, 2026-10-16).

## Open

| # | Item | Status / notes |
|---|---|---|
| 5 | **45 s startup watchdog — find the gate** | The signature patch `watchdog` skips the deadline check. On Windows something clears that deadline in time; find which step does not complete under the loader. |
| 6 | **Seams at polygon joins on outer map geometry** (e.g. Valhalla rock walls) | Quick check whether the cracks can be found programmatically (e.g. T-junctions or depth gaps in a capture); low priority. |
| 7 | **Headless `dedicated` server** | Loads the map, then idles at "waiting for the native game client". Possibly expected until a client joins: test a join against it. |
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
- Second test player without a window: shim/target.sh runs windowless (PELOAD_HIDE_WINDOW: no window, no Dock icon)
  with a 30 fps cap (PELOAD_MAX_FPS) and its menu music muted; quiet-run.sh QR_HIDE=1 does the same for test clients
- Minimized window stalled loading: not the minimizing -- the GetCurrentThreadId mutex bug (55811fa); a minimized test
  client now loads in 12.8 s
- Water renders: the water shader pair never linked (varying locations >= 14 on Apple GL) — 434db9b (user confirmed)
