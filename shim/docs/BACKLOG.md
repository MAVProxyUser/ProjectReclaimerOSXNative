# Backlog — requests from the user, and their status

Kept current as work lands. "Done" items name the commit that delivered them. Open items are in the
order they will be worked; frame rate is deliberately last (user, 2026-10-16).

## Open

| # | Item | Status / notes |
|---|---|---|
| 1 | **Hitbox overlay: Elite helmets, team colours, friend/enemy filter** | Elite heads use the default helmet's collision shape (permutation 0); must follow each player's helmet. Colour by team relative to the local player, with a key to show enemies only / friends only. |
| 2 | **Overlay features: player info, projectile tracers, item labels** (user request) | F3 tracers, F4 shield/health/weapon/grenades/camo/vehicle per player, F5 weapon/equipment labels; launch variables PELOAD_HB_TRACERS / _INFO / _ITEMS. Fields confirmed from halo3's HaloScript getters (shim/docs/halo3-objects.md). Still to confirm: aim direction, crouch, player names, scores, objective state. |
| 3 | **Menu music / voice chat output** | The client logs "Menu music output unavailable: No music playback device" and "Voice audio unavailable": the client's own audio output (not XAudio2) finds no device under the loader. Game audio works, so this should be a missing/stubbed device API. |
| 4 | **Second test player without a window** | target.sh shows a (minimized) game window; the user wants test opponents to run as a backend with no GUI. |
| 5 | **45 s startup watchdog — find the gate** | The signature patch `watchdog` skips the deadline check. On Windows something clears that deadline in time; find which step does not complete under the loader. |
| 6 | **Seams at polygon joins on outer map geometry** (e.g. Valhalla rock walls) | Quick check whether the cracks can be found programmatically (e.g. T-junctions or depth gaps in a capture); low priority. |
| 7 | **Headless `dedicated` server** | Loads the map, then idles at "waiting for the native game client". Possibly expected until a client joins: test a join against it. |
| 8 | **Release bundle** | New release named for the client version it supports (e.g. "for Reclaimer 0.9.18"); loader + scripts, no game files. User approved 2026-10-16. |
| 9 | **Unexplained crashes** (watch) | (a) v0.9.18 SIGTRAP during a direct Guardian match; (b) one startup SIGSEGV with guest code on a thread without TLS. Not seen again since the thread-id fix (55811fa); loader threads are now named for the next [CRASH] line. |
| 10 | **Minimized window stalls loading** (watch) | A client minimized during loading never reaches gameplay. Test runs no longer minimize. |
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
- Hitbox overlay: bounding spheres (f73252e), then the real hitboxes — each object's collision model posed on
  its live node matrices (76a80c8); F2 toggles it in-game (c1f12f8). Verified on Valhalla vehicles, a Guardian
  Spartan and a 60-player BTB.
- Title fps no longer claims more than the screen can show (loading screens spin at ~1000 presents/s) — f73252e
- Intermittent hang at match start: GetCurrentThreadId returned 0x1000 on every thread, which disabled every
  msvcp140 std::mutex — 55811fa
- Water renders: the water shader pair never linked (varying locations >= 14 on Apple GL) — 434db9b (user confirmed)
